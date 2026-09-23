<div align="center">

<img src="logo.png" alt="The4DScanner" width="130">

### VGS Decoder

**Reads 4D Gaussian splat captures: `.vgs` and `.pgs`**

</div>

---

By Víctor M. Feliz, The4DScanner | ScanMeNow. Free to use, including commercially; see
[LICENSE.md](LICENSE.md).

A capture is a timeline of frames, each a few hundred thousand splats, stored so a player
fetches and decodes one chunk of time at a time rather than the whole file. Every capture
carries what it is - title, author, project, take, studio, copyright, tags - and a
signature over all of it, so a reader knows whether a file is genuine after about a
kilobyte, before any of the frames have been downloaded.

This package reads captures. It cannot write one.

## What is here

    LICENSE.md                          the terms
    include/vgsdecoder/vgsdecoder.h     the C++ API
    include/vgsdecoder/vgsdecoder_c.h   the same thing in C
    lib/                                the library, and CMake package files
    bin/                                vgsinfo, vgsplay, vgsdump, vgsexport, vgspagecost
    examples/                           the source of those five, with a CMakeLists.txt

## C++

```cpp
#include "vgsdecoder/vgsdecoder.h"

vgsdec::Capture capture = vgsdec::Capture::openFile("boxing.vgs");
// Opening it authenticated it. If this line runs, the capture is genuine.

printf("%s by %s, %.2f s\n", capture.metadata().title.c_str(),
       capture.metadata().author.c_str(), capture.duration());

for (double t = 0; t < capture.duration(); t += 1.0 / 30) {
  const vgsdec::Frame &frame = capture.setTime(t);
  draw(frame.positions, frame.rotations, frame.scales, frame.colors, frame.splatCount);
}
```

`setTime` gives plain arrays - positions, rotations, scales, opacities, colours, spherical
harmonics - all indexed the same way, so element `i` of each describes the same splat. The
pointers belong to the capture and are replaced by the next `setTime`.

Three ways in: `openFile`, `openMemory`, and `openStream` with a `Source` you implement
for a socket or a CDN. A streamed capture authenticates from its first kilobytes, so you
know whether a file is worth downloading before downloading it.

### Staying in real time

Two costs decide whether a host keeps up, and they are separate questions.

Evaluating a frame happens every frame and costs about 16 ms for a quarter of a million
splats, twice that with spherical harmonics. Getting a chunk ready happens about once a
second and costs around 200 ms; paid by the frame that arrives at the chunk, it is a
dropped frame every second. `prepare` spreads it instead:

```cpp
// once a frame, after drawing, with the time left over
capture.prepare(capture.chunkAt(now + 1.0), 8);
```

A `Capture` belongs to one thread. Run it on its own and let the renderer draw the last
frame that was ready rather than wait for the next one.

`setThreadCount` decompresses several pages of a chunk at once and evaluates a frame in
as many ranges of splats, and `setParallelFor` sends that work to your own task system
instead of letting the decoder make threads. Both are off by default. Use them when you have fewer captures than cores; several captures are
already parallel on their own, one `Capture` each on its own thread.

### Letting your shader do the work

`setOutput(vgsdec::Output::Packed)` stops after decompressing. `chunkData` then gives the
buffers to upload and the values each group is reconstructed against, and `instantAt`
gives the two samples the current time falls between and the blend factor. Your vertex
shader does the interpolation and activation, and the per-frame cost on the CPU stops
depending on the splat count or on how many captures are playing.

This is how to draw several captures at once, and the only way to fit one inside a
headset's frame. It asks more of you: the shader has to know how the attributes are
encoded, which the default `Output::Floats` keeps inside the library. Ask if you need it.

Payloads carried alongside the frames each have their own getter: `audio()`,
`thumbnail()`, `metadataJson()`, `metadataJson2()`, each with a matching `has…()` and,
where it applies, a format. All four are checked against the signature before they come
back.

Anything wrong with a capture - altered bytes, a broken or missing signature, an unknown
key - throws `vgsdec::Error` whose message is exactly `invalid 4dgs capture`. That string
is the same in every implementation, including the JavaScript one, so a user-facing
message can be written against it.

### Building against it

```cmake
find_package(VGSDecoder REQUIRED PATHS <this directory>)
target_link_libraries(player PRIVATE VGS::Decoder)
```

The samples in `examples` build on their own against this install and are
the quickest way to check that everything is in place:

```bash
cd examples
cmake -S . -B build && cmake --build build --config Release
```

### C

`vgsdecoder_c.h` is the same API without C++ types, for a plugin ABI, a language binding,
or a host built with a different compiler. If this package contains a DLL rather than a
static library, that C interface is what it exports, and the only thing it exports.

## The web

The WebAssembly build of this decoder, with its JavaScript API, installs into `web/` and
documents itself there. It is present only if this package was assembled with it.

## The tools

| | |
|---|---|
| `vgsinfo capture.vgs` | what the capture says about itself |
| `vgsplay capture.vgs` | walks the whole timeline and reports what decoding it cost |
| `vgsdump capture.vgs 1.5 frame.ply` | one instant as a Gaussian splat `.ply` |
| `vgsexport capture.vgs out/` | the whole capture as a numbered `.ply` sequence |
| `vgspagecost capture.vgs` | where the bytes and decoding time go, per attribute and per detail level, and what a CPU-sorting player spends |

`vgsexport` is the bridge to everything that does not read VFGS: the 3DGS tools, the DCC
importers and the training code all read per-frame `.ply`. Expect it to be large - a
capture is a few hundred megabytes precisely because it does not store frames
independently, and writing them back out separately undoes that.
