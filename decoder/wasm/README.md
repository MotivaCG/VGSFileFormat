# VGS Decoder for the web

The decoder compiled to WebAssembly, with the JavaScript that drives it.

    vgsdecoder.js    the module, with the .wasm embedded in it
    vgs.mjs          the API you use
    vgssource.mjs    where the bytes come from, and the background download

Terms are in the licence beside the library, one directory up.

## Reading a capture

```js
import { VgsCapture } from './vgs.mjs';

const capture = await VgsCapture.open('boxing.vgs');
// Opening it authenticated it. If this line runs, the capture is genuine.
console.log(capture.metadata.title, capture.duration);

const frame = await capture.setTime(1.5);
draw(frame.positions, frame.rotations, frame.scales, frame.colors, frame.splatCount);

capture.close();
```

The parsing, the signature check and the decoding all happen inside the module, compiled
from the same C++ as the native library. This file fetches bytes and hands them over; it
does not know the format, so there is no second implementation of it to keep in step.

Anything wrong with a capture throws `VgsError` whose message is exactly
`invalid 4dgs capture`, the same string the native library uses.

## Where the bytes come from

`VgsCapture.open` takes a URL, a `Uint8Array`, a `Blob`, or anything of your own with
`read(offset, length)` and `size()`. A URL needs a server that supports range requests.

By default the capture is also downloaded in the background, ahead of playback, and
`capture.downloadedFraction` reports how far that has got. This is what keeps playback
smooth: fetching a chunk when playback reaches it stutters at every chunk boundary
however fast the decoder is. Pass `{ readAhead: false }` if something else is already
doing it.

## Staying smooth

Two calls, once a frame:

```js
const frame = await capture.setTime(now);
await capture.prepare(now + 1.0, 4);   // with the time left in the frame
```

`prepare` decodes the next chunk a few milliseconds at a time. Without it, the frame that
arrives at a new chunk pays for decoding all of it at once and is dropped.

Run this in a worker. Decoding does not yield, so on the main thread it competes with
whatever draws.

## The frame arrays

They are views into the module's memory. The next `setTime` replaces them, and a call
that grows the module's memory detaches them outright, so pass `{ copy: true }` for
anything you hold across an `await`.

`positionsAt(seconds)` gives positions alone, for a renderer that evaluates the rest on
the GPU but sorts splats by depth on the CPU. It shares the decoded chunk with `setTime`,
so asking for both at one instant decodes once.

## Payloads

`audio()`, `thumbnail()`, `metadataJson()` and `metadataJson2()`, each with a matching
`has…` and, where it applies, a format. `audioBlob()` and `thumbnailBlob()` return a
`Blob` with its media type set, ready for an `<audio>` element or `URL.createObjectURL`.
All of them are checked against the capture's signature before they come back.

## Serving it

`vgsdecoder.js` has the `.wasm` embedded as base64. It costs about a third in transfer
size and buys a module that loads from any server without that server having been told
what `application/wasm` is.
