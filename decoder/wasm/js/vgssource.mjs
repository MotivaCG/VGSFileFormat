// Where a capture's bytes come from.
//
// A source answers two questions: how long is the file, and give me this range. That is
// all the decoder needs, so anything that can answer them works - an HTTP server with
// range support, a File the user picked, an array already in memory, a cache of your own.
//
// Nothing here knows the format. Deciding what to fetch and when is the caller's business
// and stays on this side; the decoder only ever sees bytes it asked for.

/**
 * A capture served over HTTP, read with range requests.
 *
 * The length comes from the first range response rather than a separate HEAD, so opening
 * a capture is two requests: the fixed header, then the structural region.
 */
export class HttpSource {
  /**
   * @param {string} url
   * @param {object} [options]
   * @param {RequestInit} [options.fetchOptions] passed to every fetch, for credentials
   *   or an abort signal
   */
  constructor(url, { fetchOptions = {} } = {}) {
    this.url = url;
    this.fetchOptions = fetchOptions;
    this.length = 0;
  }

  async size() {
    // Two bytes rather than one. A range whose start and end are the same byte is a
    // corner some servers get wrong - Vite's dev server answers `bytes=0-0` with 206 and
    // the whole file - and asking for one byte more costs nothing and avoids it.
    if (!this.length) await this.read(0, 2);
    return this.length;
  }

  /**
   * @param {number} offset
   * @param {number} length
   * @returns {Promise<Uint8Array>} exactly `length` bytes
   */
  async read(offset, length) {
    if (length <= 0) return new Uint8Array(0);
    const end = offset + length - 1;
    const response = await fetch(this.url, {
      ...this.fetchOptions,
      headers: { ...(this.fetchOptions.headers || {}), Range: `bytes=${offset}-${end}` },
    });
    if (!response.ok) throw new Error(`${this.url}: HTTP ${response.status}`);

    // A server that ignores Range answers 200 with the whole file. Reading the requested
    // slice out of it is correct but ruinous on a 300 MB capture, so it is worth saying
    // so rather than silently downloading everything per chunk.
    if (response.status !== 206) {
      throw new Error(`${this.url}: the server does not support range requests`);
    }

    const contentRange = response.headers.get('Content-Range');
    let from = offset;
    if (contentRange) {
      const parsed = /^bytes (\d+)-(\d+)\/(\d+|\*)$/.exec(contentRange.trim());
      if (!parsed) throw new Error(`${this.url}: unreadable Content-Range "${contentRange}"`);
      from = Number(parsed[1]);
      const total = Number(parsed[3]);
      if (Number.isFinite(total)) this.length = total;
      if (from > offset || Number(parsed[2]) < end) {
        throw new Error(`${this.url}: asked for bytes ${offset}-${end}, served ${contentRange}`);
      }
    }

    const bytes = new Uint8Array(await response.arrayBuffer());
    // A server is allowed to widen a range, and some do - one widely used development
    // server answers a single-byte range with the whole file. Take the part that was
    // asked for rather than refusing: the alternative is a capture that plays
    // everywhere except where it is being built.
    if (from < offset || bytes.length > length) {
      const at = offset - from;
      if (bytes.length < at + length) {
        throw new Error(`${this.url}: asked for ${length} bytes, got ${bytes.length}`);
      }
      return bytes.subarray(at, at + length);
    }
    if (bytes.length !== length) {
      throw new Error(`${this.url}: asked for ${length} bytes, got ${bytes.length}`);
    }
    return bytes;
  }
}

/** A capture already in memory. */
export class BytesSource {
  /** @param {Uint8Array|ArrayBuffer} bytes */
  constructor(bytes) {
    this.bytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  }

  async size() {
    return this.bytes.length;
  }

  async read(offset, length) {
    if (offset + length > this.bytes.length) {
      throw new Error('read past the end of the capture');
    }
    return this.bytes.subarray(offset, offset + length);
  }
}

/** A capture in a File or Blob, as picked from a file input or dropped on the page. */
export class BlobSource {
  /** @param {Blob} blob */
  constructor(blob) {
    this.blob = blob;
  }

  async size() {
    return this.blob.size;
  }

  async read(offset, length) {
    const slice = this.blob.slice(offset, offset + length);
    return new Uint8Array(await slice.arrayBuffer());
  }
}

/**
 * Downloads a capture in the background, in front of where playback is.
 *
 * This is what makes playback smooth, and it is worth being clear about why, because it is
 * not the decoding. Decoding a chunk takes about 150 ms; fetching one over a network takes
 * far longer and is far less predictable. A player that fetches a chunk when it reaches it
 * stutters at every chunk boundary no matter how fast the decoder is. A player that
 * already has the bytes does not.
 *
 * So this reserves a buffer for the whole capture and fills it from the start, one chunk
 * at a time, in the same units playback consumes - fewer round trips than a stream of
 * small reads, and the read-ahead advances in steps that mean something. A read for bytes
 * already filled is answered from memory with no request at all.
 *
 * Reads that playback is waiting on always win: the background fill stands aside while one
 * is in flight, so seeking never queues behind a read-ahead that was going somewhere else.
 *
 * A capture too large for the budget, or a buffer the browser will not allocate, simply
 * turns this off: every read then goes to the underlying source, which still works.
 */
export class BufferedSource {
  /**
   * @param {{read: Function, size: Function}} inner the source to read through
   * @param {object} [options]
   * @param {number} [options.budgetBytes] largest capture to hold in memory, default 768 MB
   * @param {number} [options.blockSize] fill unit when the chunk table is not known yet
   */
  constructor(inner, { budgetBytes = 768 * 1024 * 1024, blockSize = 8 * 1024 * 1024 } = {}) {
    this.inner = inner;
    this.budgetBytes = budgetBytes;
    this.blockSize = blockSize;

    this.buffer = null;
    /** Byte ranges held, merged and in file order. */
    this.spans = [];
    /** Bytes held contiguously from the start: what a progress bar means. */
    this.filled = 0;
    this.total = 0;
    this.blocks = null; // byte boundaries to fetch in, from the capture's chunk table
    this.urgent = new Set();
    /** The fill's request in flight, so a read for the same bytes can join it. */
    this.pending = null;
    this.stopped = false;
    this.running = null;
    this.onProgress = null;
    /**
     * What the background fill is actually getting, in bytes per second, smoothed.
     * A player needs it to answer the only question that matters after a stall: can
     * playback reach the end without stopping again. Zero until the first block lands.
     */
    this.bytesPerSecond = 0;
  }

  async size() {
    if (!this.total) this.total = await this.inner.size();
    return this.total;
  }

  async read(offset, length) {
    if (length <= 0) return new Uint8Array(0);
    const end = offset + length;

    // Already downloaded: no request, no copy.
    if (this.#holds(offset, end)) return this.buffer.subarray(offset, end);

    // The fill is bringing in exactly these bytes right now. Waiting for it is cheaper
    // than asking for them a second time, and it is the usual case at the start, where
    // playback and the fill both want the opening chunk.
    const inFlight = this.pending;
    if (inFlight && offset >= inFlight.start && end <= inFlight.end) {
      await inFlight.task;
      if (this.#holds(offset, end)) return this.buffer.subarray(offset, end);
    }

    // Playback is waiting on this one, so the fill gets out of the way until it is done.
    const span = { start: offset, end };
    this.urgent.add(span);
    try {
      const bytes = await this.inner.read(offset, length);
      // Keep it. A read playback made is a byte of the file like any other, and the
      // whole point of the fill is that the capture is downloaded once: without this it
      // fetches the opening chunks a second time, behind the playback that just read
      // them, which is the most expensive moment of the whole session to waste.
      this.#keep(offset, end, bytes);
      return bytes;
    } finally {
      this.urgent.delete(span);
    }
  }

  /**
   * The units to fill in, normally a capture's chunk table. Reading a chunk per request is
   * what the format is laid out for and what playback consumes.
   *
   * @param {Array<{offset: number, size: number}>} blocks in file order
   */
  useBlocks(blocks) {
    this.blocks = blocks && blocks.length ? blocks : null;
  }

  /** Starts filling, if the capture fits the budget. Safe to call more than once. */
  start() {
    if (this.running || this.stopped || !this.total) return;
    if (this.total > this.budgetBytes) return;
    try {
      this.buffer = new Uint8Array(this.total);
    } catch {
      // Not enough memory for the whole capture: every read goes to the source instead,
      // which is slower but correct, and better than failing to open the file.
      this.buffer = null;
      return;
    }
    this.running = this.#fill();
  }

  /** How much of the capture is downloaded, 0 to 1. For a progress bar. */
  get filledFraction() {
    return this.total ? this.filled / this.total : 0;
  }

  /** Stops the background fill and releases the buffer. */
  stop() {
    this.stopped = true;
    this.buffer = null;
    this.spans = [];
    this.filled = 0;
    this.bytesPerSecond = 0;
  }

  #holds(start, end) {
    if (!this.buffer) return false;
    for (const span of this.spans) {
      if (span.start > start) break;
      if (span.end >= end) return true;
    }
    return false;
  }

  /** The first byte at or after `from` that nobody has brought in yet. */
  #firstGap(from) {
    let at = from;
    for (const span of this.spans) {
      if (span.end <= at) continue;
      if (span.start > at) break;
      at = span.end;
    }
    return at;
  }

  #keep(start, end, bytes) {
    if (!this.buffer || this.stopped) return;
    this.buffer.set(bytes, start);
    let at = 0;
    while (at < this.spans.length && this.spans[at].end < start) at++;
    let last = at;
    while (last < this.spans.length && this.spans[last].start <= end) last++;
    this.spans.splice(at, last - at, {
      start: Math.min(start, this.spans[at]?.start ?? start),
      end: Math.max(end, last > at ? this.spans[last - 1].end : end),
    });
    this.filled = this.spans.length && this.spans[0].start === 0 ? this.spans[0].end : 0;
  }

  async #fill() {
    while (!this.stopped && this.buffer && this.filled < this.total) {
      // Wait out anything playback is waiting on. A yield rather than a lock: the urgent
      // read is a promise nobody here holds, and a tick of delay costs nothing against a
      // fill that runs for the length of a download.
      while (this.urgent.size && !this.stopped) {
        await new Promise((resolve) => setTimeout(resolve, 4));
      }
      if (this.stopped || !this.buffer) return;

      const block = this.#nextBlock();
      if (!block) return;
      const end = block.offset + block.size;
      const started = Date.now();
      const task = this.inner.read(block.offset, block.size);
      // Announced before it is awaited, so a read that wants these bytes joins it
      // instead of racing it.
      this.pending = { start: block.offset, end, task };
      try {
        const bytes = await task;
        if (this.stopped || !this.buffer) return;
        this.#keep(block.offset, end, bytes);
        const rate = block.size / Math.max(0.001, (Date.now() - started) / 1000);
        // Weighted towards the slower reading of the two, because a player that
        // overestimates the link stalls again and one that underestimates it only waits
        // a little longer.
        this.bytesPerSecond = this.bytesPerSecond
          ? Math.min(rate, this.bytesPerSecond * 0.75 + rate * 0.25) : rate;
      } catch {
        // A failed read-ahead is not a failure: the range will be fetched again, as an
        // urgent read, if playback ever reaches it.
        return;
      } finally {
        this.pending = null;
      }
      this.onProgress?.(this.filledFraction);
    }
  }

  #nextBlock() {
    // Start where the gap is, not at the watermark: playback has usually brought in the
    // chunk it is on, and fetching it again is the one thing this is here to avoid.
    const from = this.#firstGap(this.filled);
    if (from >= this.total) return null;
    let end = this.total;
    if (this.blocks) {
      for (const block of this.blocks) {
        const stop = Math.min(block.offset + block.size, this.total);
        if (stop <= from) continue;
        // The watermark only ever means "everything up to here is held", so a gap before
        // the first chunk - the header and the tables - is filled rather than skipped
        // over, which would leave the watermark claiming bytes that are not there.
        end = block.offset > from ? block.offset : stop;
        break;
      }
    } else {
      end = Math.min(from + this.blockSize, this.total);
    }
    // Stop short of anything already held, so a block is never partly re-fetched.
    for (const span of this.spans) {
      if (span.start > from && span.start < end) { end = span.start; break; }
    }
    return end > from ? { offset: from, size: end - from } : null;
  }
}

/** Wraps whatever was handed in: a URL, some bytes, a Blob, or an already made source. */
export function toSource(what, options) {
  if (typeof what === 'string' || what instanceof URL) {
    return new HttpSource(String(what), options);
  }
  if (what instanceof Uint8Array || what instanceof ArrayBuffer) return new BytesSource(what);
  if (typeof Blob !== 'undefined' && what instanceof Blob) return new BlobSource(what);
  if (what && typeof what.read === 'function' && typeof what.size === 'function') return what;
  throw new TypeError('expected a URL, bytes, a Blob, or a source with read() and size()');
}
