// Reading VFGS captures (.vgs and .pgs) from JavaScript.
//
//     import { VgsCapture } from './vgs.mjs';
//
//     const capture = await VgsCapture.open('boxing.vgs');
//     // Opening it authenticated it. If this line runs, the capture is genuine.
//     console.log(capture.metadata.title, capture.duration);
//
//     const frame = await capture.setTime(1.5);
//     draw(frame.positions, frame.rotations, frame.scales, frame.colors, frame.splatCount);
//
//     capture.close();
//
// The parsing, the signature check and the decoding all happen in WebAssembly, compiled
// from the same C++ the native decoder is built from. This file fetches bytes, hands them
// over, and turns what comes back into typed arrays. It does not know the format, and
// there is deliberately no second implementation of it here to keep in step.
//
// Nothing renderer-specific appears either: a frame is the arrays as the decoder produced
// them, and packing them into textures or buffers is the caller's business.
//
// The arrays are views into the module's memory. They are replaced by the next setTime,
// and a call that grows the module's memory detaches them outright, so anything held
// across an await should be a copy: pass `{ copy: true }`.

import createVgsDecoder from './vgsdecoder.js';
import { BufferedSource, BytesSource, toSource } from './vgssource.mjs';

/** What a reader says about a capture it cannot vouch for. */
export const INVALID_CAPTURE = 'invalid 4dgs capture';

const FIXED_HEADER_SIZE = 176;

// Field and attribute numbers, matching the enums in vgswasm.cpp.
const Field = {
  id: 0, title: 1, author: 2, projectName: 3, takeName: 4,
  captureStudio: 5, copyright: 6, softwareName: 7, softwareVersion: 8, uuid: 9,
};
const Number_ = {
  duration: 0, frameCount: 1, frameRate: 2, startSeconds: 3, shDegree: 4,
  maxSplatsPerFrame: 5, fileSize: 6, createdMillis: 7, chunkCount: 8,
  signatureKeyId: 9, signatureAlgorithm: 10, signedBytes: 11, version: 12, isPlain: 13,
};
const Chunk = { offset: 0, size: 1, startSeconds: 2, endSeconds: 3, splats: 4 };
const Attribute = {
  positions: 0, rotations: 1, scales: 2, opacities: 3, colors: 4,
  sphericalHarmonics: 5, active: 6,
};

/**
 * How much of the work the decoder does.
 *
 * `floats` evaluates every splat into arrays you can use directly. `packed` decompresses
 * the chunk and stops, handing over buffers to upload and where the frame falls between
 * two of the chunk's samples; the interpolation and activation happen in your shader.
 *
 * `packed` is for a renderer that will do that work on the GPU. Taking it and then
 * evaluating in JavaScript gains nothing and rewrites what `floats` already does.
 */
export const Output = { floats: 0, packed: 1 };

// The layout array written by vgs_chunk_layout, whose shape is described in vgswasm.cpp.
const LAYOUT_HEADER = 4;
const GROUP_STRIDE = 8;
const BUFFER_STRIDE = 10;

/** What a sound track is. The container stores it as delivered and never transcodes. */
export const AudioFormat = { none: 0, mp3: 1, aac: 2, opus: 3, wav: 4 };
export const ImageFormat = { none: 0, png: 1, jpeg: 2, webp: 3 };

// Payload numbers, matching PayloadKind in vgswasm.cpp.
const Payload = { audio: 0, thumbnail: 1, metadataJson: 2, metadataJson2: 3 };

/** The media type to give a Blob made from a payload, so it can be played or shown. */
const MEDIA_TYPES = {
  [Payload.audio]: ['', 'audio/mpeg', 'audio/aac', 'audio/ogg; codecs=opus', 'audio/wav'],
  [Payload.thumbnail]: ['', 'image/png', 'image/jpeg', 'image/webp'],
};

/** Anything that goes wrong. `message` is exactly INVALID_CAPTURE when a file is not ours. */
export class VgsError extends Error {
  constructor(message) {
    super(message);
    this.name = 'VgsError';
  }
}

export class VgsCapture {
  /**
   * Opens a capture and checks that it is genuine.
   *
   * @param {string|URL|Uint8Array|Blob|{read,size}} what a URL, bytes, a Blob, or a
   *   source of your own with `read(offset, length)` and `size()`
   * @param {object} [options]
   * @param {RequestInit} [options.fetchOptions] for an HTTP source
   * @param {boolean} [options.readAhead=true] download the capture in the background,
   *   ahead of playback. This is what keeps playback smooth: fetching a chunk when
   *   playback reaches it stutters at every chunk boundary however fast the decoder is.
   *   Turn it off only when something else is already doing it.
   * @param {number} [options.budgetBytes] largest capture to hold in memory, default 768 MB
   * @returns {Promise<VgsCapture>}
   * @throws {VgsError} with message INVALID_CAPTURE when the file is not one of ours or
   *   has been altered since it was made
   */
  static async open(what, options = {}) {
    let source = toSource(what, options);
    // Bytes already in memory need no read-ahead; anything else gets one unless the
    // caller says otherwise.
    const wantsReadAhead = options.readAhead !== false && !(source instanceof BytesSource);
    if (wantsReadAhead) source = new BufferedSource(source, options);
    // Each capture gets its own module instance: the decoder keeps one open capture and
    // one decoded chunk, so sharing an instance between two captures would mean they
    // evicted each other's work.
    const module = await createVgsDecoder();
    const capture = new VgsCapture(module, source);
    try {
      await capture.#open();
      // The chunk table is the right unit to download in - it is what the format is laid
      // out for and what playback consumes - so the read-ahead only starts once it is
      // known, which is after the capture has authenticated.
      if (wantsReadAhead) {
        source.useBlocks(capture.chunks);
        source.start();
      }
    } catch (error) {
      capture.close();
      throw error;
    }
    return capture;
  }

  #module;
  #source;
  #metadata = null;
  #chunks = null;
  #frame = null;
  #closed = false;

  constructor(module, source) {
    this.#module = module;
    this.#source = source;
  }

  async #open() {
    const m = this.#module;
    const total = await this.#source.size();

    // The fixed header says how much the structural region is. Two small reads decide
    // whether the file is genuine; none of the payload is touched to find out.
    let head = await this.#source.read(0, Math.min(FIXED_HEADER_SIZE, total));
    this.#write(m._vgs_head_reserve(head.length), head);

    const structural = m._vgs_structural_size();
    if (structural < 0) this.#fail();
    if (structural > total) throw new VgsError(INVALID_CAPTURE);

    head = await this.#source.read(0, structural);
    this.#write(m._vgs_head_reserve(structural), head);

    if (m._vgs_open(total) !== 0) this.#fail();

    this.#metadata = Object.freeze({
      ...Object.fromEntries(
        Object.entries(Field)
          .filter(([name]) => name !== 'uuid')
          .map(([name, id]) => [name, this.#text(m._vgs_string(id))]),
      ),
      tags: Object.freeze(
        Array.from({ length: m._vgs_tag_count() }, (_, i) => this.#text(m._vgs_tag(i))),
      ),
    });

    // The chunk table is read out once: a fetch policy runs on it every frame, and
    // crossing into WebAssembly for each field would be the most frequent call in the
    // whole player for no reason.
    const count = m._vgs_number(Number_.chunkCount);
    this.#chunks = Object.freeze(
      Array.from({ length: count }, (_, i) =>
        Object.freeze({
          index: i,
          offset: m._vgs_chunk(i, Chunk.offset),
          size: m._vgs_chunk(i, Chunk.size),
          startSeconds: m._vgs_chunk(i, Chunk.startSeconds),
          endSeconds: m._vgs_chunk(i, Chunk.endSeconds),
          splats: m._vgs_chunk(i, Chunk.splats),
          bounds: this.#boundsOf(i),
        }),
      ),
    );
  }

  // Reserving a buffer can grow the module's memory, and growing it replaces the
  // ArrayBuffer every heap view is built on. So the pointer is taken first and the view
  // read afterwards; the obvious one-liner detaches the array it is writing through.
  #write(pointer, bytes) {
    if (!pointer && bytes.length) this.#fail();
    this.#module.HEAPU8.set(bytes, pointer);
  }

  #fail() {
    const message = this.#text(this.#module._vgs_error()) || 'VGS failure';
    throw new VgsError(message);
  }

  #text(pointer) {
    return pointer ? this.#module.UTF8ToString(pointer) : '';
  }

  #boundsOf(index) {
    const pointer = this.#module._vgs_chunk_bounds(index);
    if (!pointer) return null;
    return Float32Array.from(
      this.#module.HEAPF32.subarray(pointer >>> 2, (pointer >>> 2) + 6),
    );
  }

  // ---- what the capture says about itself ----------------------------------------

  /** @returns {{id,title,author,projectName,takeName,captureStudio,copyright,softwareName,softwareVersion,tags}} */
  get metadata() { return this.#metadata; }
  /** The identifier as 8-4-4-4-12 hexadecimal. */
  get uuid() { return this.#text(this.#module._vgs_string(Field.uuid)); }
  /** When the capture was written. */
  get created() { return new Date(this.#module._vgs_number(Number_.createdMillis)); }
  get signature() {
    const m = this.#module;
    return Object.freeze({
      algorithm: m._vgs_number(Number_.signatureAlgorithm),
      keyId: m._vgs_number(Number_.signatureKeyId),
      signedBytes: m._vgs_number(Number_.signedBytes),
    });
  }
  get version() { return this.#module._vgs_number(Number_.version); }
  /** True when the pages are stored without entropy coding, which is what .pgs means. */
  get isPlain() { return this.#module._vgs_number(Number_.isPlain) === 1; }

  // ---- the timeline ----------------------------------------------------------------

  /** The last time that can be asked for, in seconds. */
  get duration() { return this.#module._vgs_number(Number_.duration); }
  get frameCount() { return this.#module._vgs_number(Number_.frameCount); }
  get frameRate() { return this.#module._vgs_number(Number_.frameRate); }
  get startSeconds() { return this.#module._vgs_number(Number_.startSeconds); }
  get shDegree() { return this.#module._vgs_number(Number_.shDegree); }
  get maxSplatsPerFrame() { return this.#module._vgs_number(Number_.maxSplatsPerFrame); }
  get fileSize() { return this.#module._vgs_number(Number_.fileSize); }

  /** The capture's own box over every frame: [minX, minY, minZ, maxX, maxY, maxZ]. */
  get bounds() {
    const pointer = this.#module._vgs_bounds();
    if (!pointer) return null;
    return Array.from(this.#module.HEAPF64.subarray(pointer >>> 3, (pointer >>> 3) + 6));
  }

  /** Every chunk: where it is in the file, what it spans, and its box. */
  get chunks() { return this.#chunks; }

  /** The chunk holding a time, or null. */
  chunkAt(seconds) {
    const index = this.#module._vgs_chunk_at(seconds);
    return index < 0 ? null : this.#chunks[index];
  }

  /** Which chunk is decoded right now, or null. Nothing is fetched to answer this. */
  get loadedChunk() {
    if (!this.#frame) return null;
    const index = this.#module._vgs_frame_chunk();
    return index < 0 ? null : this.#chunks[index];
  }

  // ---- playback ----------------------------------------------------------------------

  /**
   * Fetches whatever the instant at `seconds` needs and decodes it.
   *
   * Consecutive times inside one chunk cost one evaluation; crossing into a new chunk
   * costs a fetch and a decode. A player that wants neither to happen mid-frame calls
   * `prefetch` ahead of time.
   *
   * @param {number} seconds clamped to [0, duration]
   * @param {object} [options]
   * @param {boolean} [options.sphericalHarmonics=true] false skips the colour detail
   *   layers, which is most of the decoding work
   * @param {boolean} [options.copy=false] return copies rather than views into the
   *   module's memory, which the next call replaces
   * @returns {Promise<{seconds:number,splatCount:number,positions:Float32Array,rotations:Float32Array,scales:Float32Array,opacities:Float32Array,colors:Float32Array,sphericalHarmonics:(Float32Array|null),shCoefficients:number,active:Uint8Array,chunkIndex:number}>}
   */
  async setTime(seconds, { sphericalHarmonics = true, copy = false } = {}) {
    this.#check();
    await this.prefetch(seconds);

    const m = this.#module;
    const splatCount = m._vgs_set_time(seconds, sphericalHarmonics ? 1 : 0);
    if (splatCount < 0) this.#fail();

    const shCoefficients = m._vgs_frame_sh_coefficients();
    const take = (attribute, perSplat, Type) => {
      const pointer = m._vgs_frame(attribute);
      if (!pointer || !perSplat) return null;
      const heap = Type === Uint8Array ? m.HEAPU8 : m.HEAPF32;
      const shift = Type === Uint8Array ? 0 : 2;
      const start = pointer >>> shift;
      const view = heap.subarray(start, start + splatCount * perSplat);
      return copy ? Type.from(view) : view;
    };

    this.#frame = {
      seconds,
      splatCount,
      positions: take(Attribute.positions, 3, Float32Array),
      rotations: take(Attribute.rotations, 4, Float32Array),
      scales: take(Attribute.scales, 3, Float32Array),
      opacities: take(Attribute.opacities, 1, Float32Array),
      colors: take(Attribute.colors, 3, Float32Array),
      sphericalHarmonics: take(Attribute.sphericalHarmonics, shCoefficients * 3, Float32Array),
      shCoefficients,
      active: take(Attribute.active, 1, Uint8Array),
      chunkIndex: m._vgs_frame_chunk(),
    };
    return this.#frame;
  }

  /**
   * Positions alone for an instant, for a renderer that evaluates everything else on the
   * GPU but sorts splats by depth on the CPU. Shares the decoded chunk with setTime, so
   * asking for both at one instant decodes once.
   *
   * @returns {Promise<Float32Array>} 3 floats per splat
   */
  async positionsAt(seconds, { copy = false } = {}) {
    this.#check();
    await this.prefetch(seconds);

    const m = this.#module;
    const count = m._vgs_positions_at(seconds);
    if (count < 0) this.#fail();
    const pointer = m._vgs_positions_data();
    const view = m.HEAPF32.subarray(pointer >>> 2, (pointer >>> 2) + count * 3);
    return copy ? Float32Array.from(view) : view;
  }

  /**
   * Makes sure the chunk holding `seconds` can be decoded without waiting on the network.
   * Call it ahead of where playback is and setTime never blocks.
   *
   * Nothing is fetched when the decoder already holds that chunk decoded, so a player can
   * call this freely - running it over the next few seconds each frame costs a lookup, not
   * a download.
   */
  async prefetch(seconds) {
    this.#check();
    const m = this.#module;
    const index = m._vgs_chunk_at(seconds);
    if (index < 0) throw new VgsError('no chunk at that time');
    // Already decoded, or already sitting in the decoder's input buffer.
    if (m._vgs_is_chunk_cached(index) === 1 || this.#primed === index) return;

    const chunk = this.#chunks[index];
    const bytes = await this.#source.read(chunk.offset, chunk.size);
    // The decoder hands back its own buffer, so the fetched range is written straight
    // into the place the decode will read it from.
    this.#write(m._vgs_prime_reserve(chunk.offset, chunk.size), bytes);
    this.#primed = index;
  }

  #primed = -1;

  /**
   * Decodes the chunk holding `seconds` a little at a time, so nothing blocks for long.
   *
   * This is what keeps a player smooth, and it is worth being precise about why. Fetching
   * a chunk early is not enough on its own: decoding one costs around 150 ms and a frame
   * at 30 fps has 33, so a player that decodes when it arrives at a chunk drops frames at
   * every boundary however early the bytes arrived. Spread across the second of playback
   * before it, the same work never shows.
   *
   *     // once a frame, after drawing, with whatever time is left in the budget
   *     await capture.prepare(now + 1.0, 4);
   *
   * Fetches the bytes if they are not here yet, so one call covers both halves of getting
   * ready. Returns true when that chunk is ready and setTime on it will not block.
   *
   * @param {number} seconds a time slightly ahead of where playback is
   * @param {number} [budgetMilliseconds=4] how long this call may spend decoding
   * @param {object} [options]
   * @param {boolean} [options.sphericalHarmonics=true] must match what setTime will ask
   *   for, or the work is done twice
   * @returns {Promise<boolean>}
   */
  async prepare(seconds, budgetMilliseconds = 4, { sphericalHarmonics = true } = {}) {
    this.#check();
    const m = this.#module;
    const index = m._vgs_chunk_at(seconds);
    if (index < 0) return false;
    if (m._vgs_is_chunk_cached(index) === 1) return true;

    // Only the first step reads the range; after that the decoder holds its own copy, so
    // the window is free to move on.
    if (m._vgs_prepared_fraction() === 0) await this.prefetch(seconds);

    const done = m._vgs_prepare(index, budgetMilliseconds, sphericalHarmonics ? 1 : 0);
    if (done < 0) this.#fail();
    return done === 1;
  }

  /** How far the chunk being prepared has got, 0 to 1. */
  get preparedFraction() {
    return this.#module._vgs_prepared_fraction();
  }

  // ---- handing a chunk to a shader --------------------------------------------------

  /**
   * Chooses between evaluating on the CPU and handing the data to your shader. Changing
   * it drops whatever is decoded, because the two keep different things.
   *
   * @param {number} output one of Output
   */
  setOutput(output) {
    this.#check();
    if (this.#module._vgs_set_output(output === Output.packed ? 1 : 0) !== 0) this.#fail();
    this.#primed = -1;
  }

  get output() {
    return this.#module._vgs_output() === 1 ? Output.packed : Output.floats;
  }

  /**
   * Everything a shader needs about a prepared chunk: the buffers to upload and the
   * values its groups are reconstructed against.
   *
   * Only in packed mode, and only once `prepare` has finished with that chunk. The byte
   * arrays are views into the module's memory and stay valid until the chunk is evicted,
   * which `isChunkCached` reports; copy them, or upload them, before then.
   *
   * @param {number} index from `chunkAt`
   * @returns {{sampleCount:number,totalBytes:number,groups:Array,buffers:Array}}
   */
  chunkLayout(index) {
    this.#check();
    const m = this.#module;
    const pointer = m._vgs_chunk_layout(index);
    if (!pointer) this.#fail();

    // One read of the whole description rather than a call per field: crossing into
    // WebAssembly is cheap, but not cheap enough to do it a few hundred times a chunk.
    const base = pointer >>> 3;
    const header = m.HEAPF64.subarray(base, base + LAYOUT_HEADER);
    const [sampleCount, groupCount, bufferCount, totalBytes] = header;

    const groups = [];
    let at = base + LAYOUT_HEADER;
    for (let i = 0; i < groupCount; ++i, at += GROUP_STRIDE) {
      const g = m.HEAPF64.subarray(at, at + GROUP_STRIDE);
      groups.push({
        type: g[0], flags: g[1], splats: g[2], intervals: g[3],
        positionMin: g[4], positionMax: g[5],
        trajectoryMin: g[6], trajectoryMax: g[7],
      });
    }

    const buffers = [];
    for (let i = 0; i < bufferCount; ++i, at += BUFFER_STRIDE) {
      const b = m.HEAPF64.subarray(at, at + BUFFER_STRIDE);
      const address = b[8], size = b[9];
      buffers.push({
        attribute: b[0], group: b[1], layer: b[2],
        firstRow: b[3], rows: b[4], totalRows: b[5],
        kind: b[6], width: b[7],
        bytes: m.HEAPU8.subarray(address, address + size),
      });
    }

    return { sampleCount, totalBytes, groups, buffers };
  }

  /**
   * Where a time falls between the samples of its chunk. Costs nothing: no decoding, no
   * allocation, no fetch. In packed mode this is the whole of a frame's work on this
   * side, and what you hand the shader as uniforms.
   *
   * @returns {{chunkIndex:number, sampleA:number, sampleB:number, alpha:number}}
   */
  instantAt(seconds) {
    this.#check();
    const m = this.#module;
    const pointer = m._vgs_instant(seconds);
    if (!pointer) this.#fail();
    const at = m.HEAPF64.subarray(pointer >>> 3, (pointer >>> 3) + 4);
    return { chunkIndex: at[0], sampleA: at[1], sampleB: at[2], alpha: at[3] };
  }

  // ---- what playback keeps in memory --------------------------------------------------

  /**
   * How many decoded chunks to keep either side of the one being played.
   *
   * The two limits are the shape of the window; the budget is their sum plus the chunk
   * being played. Nothing is evicted while the total held fits, so near the end of a
   * timeline, where there is nothing ahead to keep, what is behind is not thrown away to
   * honour an allowance that cannot be used.
   *
   * Decoding is the expensive part, so keeping a chunk either side is what lets a player
   * nudge back a second without paying for it again. On a phone, set `maxBytes` as well:
   * a chunk of a large capture decodes to tens of megabytes.
   *
   *     capture.setCachePolicy({ behind: 1, ahead: 2, maxBytes: 256 * 1024 * 1024 });
   *
   * @param {{behind?: number, ahead?: number, maxBytes?: number}} policy
   */
  setCachePolicy({ behind = 1, ahead = 1, maxBytes = 0 } = {}) {
    this.#check();
    this.#module._vgs_set_cache_policy(behind, ahead, maxBytes);
  }

  /**
   * How much of the capture is downloaded, 0 to 1, when the read-ahead is on; 1 otherwise.
   * This is the number to draw as a buffer bar.
   */
  get downloadedFraction() {
    return this.#source instanceof BufferedSource ? this.#source.filledFraction : 1;
  }

  /** Called with downloadedFraction as the background download advances. */
  set onDownloadProgress(fn) {
    if (this.#source instanceof BufferedSource) this.#source.onProgress = fn;
  }

  /** Whether a chunk is decoded right now, so a player knows what a seek would cost. */
  isChunkCached(index) {
    return this.#module._vgs_is_chunk_cached(index) === 1;
  }

  get cachedChunkCount() { return this.#module._vgs_cached_chunk_count(); }
  /** Roughly how much the decoded chunks are holding, in bytes. */
  get cachedBytes() { return this.#module._vgs_cached_bytes(); }

  /** Drops the decoded chunk, for a player that has seeked away and wants the memory. */
  releaseCache() {
    this.#module._vgs_release_cache();
  }

  // ---- payloads carried alongside -----------------------------------------------------
  //
  // A capture can carry a sound track, a thumbnail and two independent blocks of JSON.
  // Each has its own getter, each fetches only what it needs, and each is checked against
  // the signed table before it comes back.

  get hasAudio() { return this.#module._vgs_has_payload(Payload.audio) === 1; }
  /** One of AudioFormat. */
  get audioFormat() { return this.#module._vgs_payload_format(Payload.audio); }

  /**
   * The sound track as it was delivered.
   * @returns {Promise<Uint8Array|null>} null when the capture carries none
   */
  async audio() { return this.#payload(Payload.audio); }

  /**
   * The same as a Blob with its media type set, ready for an <audio> element:
   *
   *     const track = await capture.audioBlob();
   *     if (track) audioElement.src = URL.createObjectURL(track);
   */
  async audioBlob() { return this.#blob(Payload.audio); }

  get hasThumbnail() { return this.#module._vgs_has_payload(Payload.thumbnail) === 1; }
  /** One of ImageFormat. */
  get thumbnailFormat() { return this.#module._vgs_payload_format(Payload.thumbnail); }
  async thumbnail() { return this.#payload(Payload.thumbnail); }
  /** The thumbnail as a Blob, ready for URL.createObjectURL. */
  async thumbnailBlob() { return this.#blob(Payload.thumbnail); }

  get hasMetadataJson() { return this.#module._vgs_has_payload(Payload.metadataJson) === 1; }
  /**
   * The free-form JSON block, already parsed.
   * @returns {Promise<any|null>} null when the capture carries none
   */
  async metadataJson() { return this.#json(Payload.metadataJson); }

  get hasMetadataJson2() { return this.#module._vgs_has_payload(Payload.metadataJson2) === 1; }
  /** The second block. One belongs to whoever produced the capture, the other to
   *  whoever uses it, and neither has to parse the other's. */
  async metadataJson2() { return this.#json(Payload.metadataJson2); }

  async #payload(kind) {
    this.#check();
    const m = this.#module;
    if (m._vgs_has_payload(kind) !== 1) return null;

    // The module knows where it is, from the signed table. This side fetches that range
    // and primes it, exactly as it does for a chunk; the check against the table's digest
    // then happens inside, which is the point - the bytes are trusted only once the
    // signature has vouched for them.
    const offset = m._vgs_payload_offset(kind);
    const size = m._vgs_payload_size(kind);
    const bytes = await this.#source.read(offset, size);
    this.#write(m._vgs_prime_reserve(offset, size), bytes);
    // Priming replaced whatever chunk was there, so the next setTime fetches again.
    this.#primed = -1;

    const length = m._vgs_payload(kind);
    if (length < 0) this.#fail();
    const pointer = m._vgs_staging();
    return Uint8Array.from(m.HEAPU8.subarray(pointer, pointer + length));
  }

  async #json(kind) {
    const bytes = await this.#payload(kind);
    if (!bytes) return null;
    try {
      return JSON.parse(new TextDecoder().decode(bytes));
    } catch (error) {
      throw new VgsError(`the capture's JSON block is not valid JSON: ${error.message}`);
    }
  }

  async #blob(kind) {
    const bytes = await this.#payload(kind);
    if (!bytes) return null;
    const type = MEDIA_TYPES[kind]?.[this.#module._vgs_payload_format(kind)] || '';
    return new Blob([bytes], { type });
  }

  /** Releases the module and everything it holds. The capture cannot be used after this. */
  close() {
    if (this.#closed) return;
    this.#closed = true;
    if (this.#source instanceof BufferedSource) this.#source.stop();
    try {
      this.#module._vgs_close();
    } catch {
      // Closing a module that never opened a capture is not worth reporting.
    }
    this.#frame = null;
    this.#primed = -1;
  }

  #check() {
    if (this.#closed) throw new VgsError('the capture is closed');
  }
}
