// An urgent read completes the download while read-ahead stands aside. No later
// background request exists to notify a player that all bytes have arrived.
import assert from 'node:assert/strict';
import { BufferedSource } from '../decoder/wasm/js/vgssource.mjs';

const data = Uint8Array.from({ length: 64 }, (_, i) => i);
let finishRead, reads = 0;
const source = new BufferedSource({
  size: async () => data.length,
  read: () => { reads++; return new Promise(resolve => { finishRead = resolve; }); }
});
const progress = [];
source.onProgress = fraction => progress.push(fraction);
await source.size();
const urgent = source.read(0, data.length);
source.start();
finishRead(data);
assert.deepEqual(await urgent, data);
await source.running;
assert.equal(reads, 1, 'read-ahead downloaded bytes already delivered by playback');
assert.equal(source.filledFraction, 1);
assert.equal(progress.at(-1), 1, 'completion never reached the player');
source.stop();
console.log('Urgent read completion reports 100% without a duplicate download');
