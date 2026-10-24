# VGS Decoder for the web

The decoder compiled to WebAssembly, with the JavaScript that drives it.
VGS 5 uses per-attribute FSE/Pcodec/Raw selection; older captures must be exported
again. Pcodec is linked into the module, so no Rust runtime download is needed.
`VGS_WASM_SIMD128=ON` (default) enables portable SIMD128 in Pcodec. Set it OFF for
a scalar module; neither variant requires AVX2. Building from source requires
Rust/Cargo 1.87+ and the `wasm32-unknown-unknown` target. Dependencies are vendored
and built offline; `THIRD_PARTY_NOTICES.txt` installs with the decoder.

    vgsdecoder.js    the module, with the .wasm embedded in it
    vgs.mjs          the API you use
    vgssource.mjs    where the bytes come from, and the background download

Terms are in the licence beside the library, one directory up.

## Build configurations

A WASM build defaults to `Release` when no configuration is selected. `Release`
uses O3/LTO; `MinSizeRel` uses Oz/LTO. Both strip internal symbol names and debug
metadata, omit source maps, and minify the generated Emscripten glue. The public
JavaScript API is preserved. This raises the effort of inspection; it does not
encrypt the module or make it impossible to reverse engineer.

`Debug` uses O0, full DWARF, internal names and runtime assertions.
`RelWithDebInfo` uses O2 with the same debug information and assertions. Neither
configuration applies the stripping/minification used by Release. For Ninja:

```sh
emcmake cmake -S . -B build_wasm_debug -DVGS_BUILD_WASM=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build_wasm_debug
emcmake cmake -S . -B build_wasm_symbols -DVGS_BUILD_WASM=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_wasm_symbols
```

`-DVGS_WASM_DEBUG_INFO=ON` also preserves full debug information and internal
names in an otherwise optimized configuration. It defaults to OFF; Debug and
RelWithDebInfo retain debug information regardless of that option. Debug data
is embedded in the module, so no separate source-map file is required. The
packaging script explicitly rebuilds Release with this option OFF.

With `VGS_BUILD_TESTS=ON`, `ctest --test-dir BUILD_DIR` checks the actual embedded
WASM: release artifacts must omit debug/name sections, while debug artifacts
must retain names and DWARF. For a multi-config generator, also pass `-C CONFIG`.
`tests/wasm_browser.cjs` optionally compares three decoded frames across builds
using a current-format capture and an installed Edge/Playwright-core setup.

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

The header is getters: `duration`, `frameCount`, `frameRate` (the capture's own, not
always 30), `shDegree`, `bounds`, and `playbackMode`, one of the exported `PlaybackMode`
values: how the capture's author means it to play. `motionType` and `movingSpeed` are
reserved for later and read `MotionType.inPlace` and 0 for now. `antialiased` says the
capture was trained with anti-aliasing and is drawn with the matching opacity compensation
(in PlayCanvas, `app.scene.gsplat.antiAlias = true`); without it its needle splats show as
solid lines. `renderHints` holds every such `RenderHint` bit.

`hasMotion` says whether the capture moves as a whole. `setTime` and `positionsAt` return
it already moved, harmonics included. In packed mode, `motionAt(seconds)` gives
`{ translation, rotation, scale, matrix }` for the held chunk, interpolated the way
`instantAt` places the instant: use `matrix` as the model matrix (or put the motion on the
splat entity's transform) and evaluate the harmonics in the capture's own space. A
module older than motion refuses such a capture with `invalid VGS policy`.

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
the GPU but sorts splats by depth on the CPU - WebGL, which has no compute shaders to sort
with. On its own it decodes only the position attributes, about a third of a chunk's base
layer; after a `setTime` on the same chunk it uses what that decoded. Keep one capture for
positions and prepare it at that level, so its chunks never decode more:

```js
import { Detail } from './vgs.mjs';
await sorterCapture.prepare(next, 2, { detail: Detail.positions });
const positions = await sorterCapture.positionsAt(now);
```

`Detail` is `positions`, `base` or `full`, each holding everything the one before does.
`prepare`'s older `{ sphericalHarmonics: false }` still means `base`.

## Payloads

`audio()`, `thumbnail()`, `metadataJson()` and `metadataJson2()`, each with a matching
`has…` and, where it applies, a format. `audioBlob()` and `thumbnailBlob()` return a
`Blob` with its media type set, ready for an `<audio>` element or `URL.createObjectURL`.
All of them are checked against the capture's signature before they come back.

## Serving it

`vgsdecoder.js` has the `.wasm` embedded as base64. It costs about a third in transfer
size and buys a module that loads from any server without that server having been told
what `application/wasm` is.
