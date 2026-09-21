// The read-ahead's byte bookkeeping, on its own: no network, no wasm.
import { BufferedSource } from '../decoder/wasm/js/vgssource.mjs';

const TOTAL = 1000;
let checks = 0, failures = 0;
const check = (ok, what) => { checks++; if (!ok) { failures++; console.log('FAIL ', what); } else console.log('ok    ', what); };

class Counting {
  constructor() { this.reads = []; }
  async size() { return TOTAL; }
  async read(offset, length) {
    this.reads.push([offset, offset + length]);
    const bytes = new Uint8Array(length);
    for (let i = 0; i < length; i++) bytes[i] = (offset + i) & 255;
    return bytes;
  }
  get bytes() { return this.reads.reduce((n, [a, b]) => n + b - a, 0); }
}

const blocks = Array.from({ length: 10 }, (_, i) => ({ offset: i * 100, size: 100 }));

// A player reads chunks 0..2 urgently, then the fill runs. It must not fetch them again.
{
  const inner = new Counting();
  const source = new BufferedSource(inner);
  await source.size();
  source.useBlocks(blocks);
  source.start();
  for (let i = 0; i < 3; i++) await source.read(i * 100, 100);
  await source.running;
  check(inner.bytes === TOTAL, `the capture is read once: ${inner.bytes} of ${TOTAL} bytes`);
  check(source.filled === TOTAL, 'the watermark reaches the end');
  const back = await source.read(150, 50);
  check(back[0] === 150 && back[49] === 199, 'a read comes back from memory with the right bytes');
  check(inner.bytes === TOTAL, 'and cost no request');
}

// A seek to the middle, then the fill. The gap before it is filled, the seek is not refetched.
{
  const inner = new Counting();
  const source = new BufferedSource(inner);
  await source.size();
  source.useBlocks(blocks);
  source.start();
  await source.read(500, 100);
  await source.running;
  check(inner.bytes === TOTAL, `one pass over the file: ${inner.bytes} bytes`);
  check(source.filled === TOTAL, 'and the watermark closes over the seek');
}

// Every byte is the byte it should be, whichever route it came by.
{
  const inner = new Counting();
  const source = new BufferedSource(inner);
  await source.size();
  source.useBlocks(blocks);
  source.start();
  await source.read(320, 40);
  await source.read(700, 100);
  await source.running;
  let wrong = 0;
  for (let at = 0; at < TOTAL; at += 37) {
    const n = Math.min(37, TOTAL - at);
    const got = await source.read(at, n);
    for (let i = 0; i < n; i++) if (got[i] !== ((at + i) & 255)) wrong++;
  }
  check(wrong === 0, 'every byte of the assembled capture is correct');
}

// No chunk table: it still fills, in blocks of its own.
{
  const inner = new Counting();
  const source = new BufferedSource(inner, { blockSize: 256 });
  await source.size();
  source.start();
  await source.running;
  check(source.filled === TOTAL && inner.bytes === TOTAL, 'it fills without a chunk table too');
}

// Too large for the budget: no buffer, reads still work.
{
  const inner = new Counting();
  const source = new BufferedSource(inner, { budgetBytes: 10 });
  await source.size();
  source.start();
  check(source.buffer === null, 'a capture over the budget is not held');
  const got = await source.read(0, 4);
  check(got[3] === 3, 'and reads go straight to the source');
}

console.log(`\n${checks} check(s), ${failures} failure(s)`);
process.exit(failures ? 1 : 0);
