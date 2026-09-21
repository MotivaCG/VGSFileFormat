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
    if (!this.length) await this.read(0, 1);
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
    if (contentRange) {
      const total = Number(contentRange.split('/')[1]);
      if (Number.isFinite(total)) this.length = total;
    }

    const bytes = new Uint8Array(await response.arrayBuffer());
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
    this.filled = 0; // bytes held from the start, a single watermark
    this.total = 0;
    this.blocks = null; // byte boundaries to fetch in, from the capture's chunk table
    this.urgent = 0;
    this.stopped = false;
    this.running = null;
    this.onProgress = null;
  }

  async size() {
    if (!this.total) this.total = await this.inner.size();
    return this.total;
  }

  async read(offset, length) {
    if (length <= 0) return new Uint8Array(0);

    // Already downloaded: no request, no copy.
    if (this.buffer && offset + length <= this.filled) {
      return this.buffer.subarray(offset, offset + length);
    }

    // Playback is waiting on this one, so the fill gets out of the way until it is done.
    this.urgent += 1;
    try {
      return await this.inner.read(offset, length);
    } finally {
      this.urgent -= 1;
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
    this.filled = 0;
  }

  async #fill() {
    while (!this.stopped && this.buffer && this.filled < this.total) {
      // Wait out anything playback is waiting on. A yield rather than a lock: the urgent
      // read is a promise nobody here holds, and a tick of delay costs nothing against a
      // fill that runs for the length of a download.
      while (this.urgent > 0 && !this.stopped) {
        await new Promise((resolve) => setTimeout(resolve, 4));
      }
      if (this.stopped || !this.buffer) return;

      const { offset, size } = this.#nextBlock();
      try {
        const bytes = await this.inner.read(offset, size);
        if (this.stopped || !this.buffer) return;
        this.buffer.set(bytes, offset);
        this.filled = offset + size;
      } catch {
        // A failed read-ahead is not a failure: the range will be fetched again, as an
        // urgent read, if playback ever reaches it.
        return;
      }
      this.onProgress?.(this.filledFraction);
    }
  }

  #nextBlock() {
    const from = this.filled;
    if (this.blocks) {
      for (const block of this.blocks) {
        const end = Math.min(block.offset + block.size, this.total);
        if (end <= from) continue;
        // The watermark only ever means "everything up to here is held", so a gap before
        // the first chunk - the header and the tables - is filled rather than skipped
        // over, which would leave the watermark claiming bytes that are not there.
        if (block.offset > from) return { offset: from, size: block.offset - from };
        return { offset: from, size: end - from };
      }
    }
    return { offset: from, size: Math.min(this.blockSize, this.total - from) };
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
