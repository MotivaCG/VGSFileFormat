<div align="center">

<img src="logo.png" alt="The4DScanner" width="130">

### VGS Decoder

**Reads 4D Gaussian splat captures: `.vgs` and `.pgs`**

</div>

---

By Víctor M. Feliz, The4DScanner | ScanMeNow. Free to use, including commercially, and to redistribute as part of end-user
products (including viewers and plugins); see [LICENSE.md](LICENSE.md).
The decoder is proprietary: it may not be repackaged as a standalone SDK.

A capture is a timeline of frames, each a few hundred thousand splats, stored so a player
fetches and decodes one chunk of time at a time rather than the whole file. Every capture
carries what it is - title, author, project, take, studio, copyright, tags - and signed metadata and payloads. A decoder can verify the initial signed information
using a trusted verification key after fetching only the initial bytes, without
downloading all frame data. Successful signature verification establishes integrity
and correspondence to that trusted key; it does not independently establish who
created the underlying capture or whether its contents are truthful.

This package reads captures. It cannot write one.

## What is here

    LICENSE.md                          the decoder licence
    DISTRIBUTION.md                     integration and distribution guide
    include/vgsdecoder/vgsdecoder.h     the C++ API
    include/vgsdecoder/vgsdecoder_c.h   the same thing in C
    lib/                                the library, and CMake package files
    bin/                                vgsinfo, vgsplay, vgsdump, vgsexport, vgspagecost
    examples/                           the source of those five, with a CMakeLists.txt

## C++

```cpp
#include "vgsdecoder/vgsdecoder.h"

vgsdec::Capture capture = vgsdec::Capture::openFile("boxing.vgs");
// Opening verifies the signed capture information using trusted decoder keys.
// This checks integrity and key authenticity, not the truth of the metadata.

printf("%s by %s, %.2f s\n", capture.metadata().title.c_str(),
       capture.metadata().author.c_str(), capture.duration());

// Frames at the capture's own rate: 30 for most, but 25 or 29.97 are as valid.
for (double t = 0; t < capture.duration(); t += 1.0 / capture.frameRate()) {
  const vgsdec::Frame &frame = capture.setTime(t);
  draw(frame.positions, frame.rotations, frame.scales, frame.colors, frame.splatCount);
}
```

Besides its metadata, a capture says how its author means it to play:
`capture.playbackMode()` is `Once` (hold the last frame), `Loop` or `PingPong`. Start it
that way unless your user chooses otherwise. `motionType()` and `movingSpeed()` are
reserved for later and read `InPlace` and 0 for now.

`antialiased()` says how to draw it: the capture was trained with anti-aliasing (every
capture converted from a Gracia `.mint` is). If your renderer widens a splat smaller than
a pixel to a pixel, lower its opacity by the same factor - `sqrt(det(cov) / det(cov +
0.3 I))` on the screen-space covariance, PlayCanvas' gsplat `antiAlias` - or the capture's
needle splats show as solid lines. `renderHints()` holds all such bits; ignore the ones
you do not know.

A capture can also move as a whole - `hasMotion()` says so. Frames from `setTime` and
`positionsAt` are already moved, spherical harmonics included, so a player that draws
them needs nothing more. In `Output::Packed` the buffers hold the capture in its own
space: `motionAt(seconds)` gives the motion at an instant, interpolated exactly as
`instantAt` places it, with a column-major `matrix` to use as the model matrix; multiply
splat rotations by its `rotation`, scales by its `scale`, and evaluate the harmonics with
the view direction rotated back. A decoder older than motion refuses such a capture
rather than play it in place. The C API has `vgs_has_motion` and `vgs_motion_at`.

`setTime` gives plain arrays - positions, rotations, scales, opacities, colours, spherical
harmonics - all indexed the same way, so element `i` of each describes the same splat. The
pointers belong to the capture and are replaced by the next `setTime`.

Three ways in: `openFile`, `openMemory`, and `openStream` with a `Source` you implement
for a socket or a CDN. A streamed capture can verify its initial signed information from the first
kilobytes using a trusted key. This allows early rejection of unsupported or
untrusted data; payload integrity is checked as the relevant data is accessed.

### Staying in real time

Two costs decide whether a host keeps up, and they are separate questions.

As an illustrative measurement, evaluating a frame has taken about 16 ms for
a quarter of a million splats, and roughly twice that with spherical harmonics.
Preparing a chunk has taken around 200 ms, often once per second of playback.
These figures are not performance guarantees: CPU, thread settings, capture
complexity and decoder build matter. The hardware and benchmark configuration
for these measurements are not specified here. A chunk prepared synchronously
can stall playback; `prepare` spreads the work instead:

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

This is the recommended way to render several captures at once and can be
important for tight headset frame budgets. It asks more of you: the shader has to know how the attributes are
encoded, which the default `Output::Floats` keeps inside the library. Contact the Licensor if you need the packed attribute layout and a reference
shader implementation. Availability and terms for additional documentation
should be confirmed separately; the decoder licence does not grant access to
undistributed source code or confidential internal specifications.

Payloads carried alongside the frames each have their own getter: `audio()`,
`thumbnail()`, `metadataJson()`, `metadataJson2()`, each with a matching `has…()` and,
where it applies, a format. All four are checked against the signature before they come
back.

For certain invalid or unauthenticated captures - for example, altered bytes,
a broken or missing signature, or an unknown key - the decoder throws `vgsdec::Error` whose message is exactly `invalid 4dgs capture`. This is the documented error text for this failure category in the supplied
implementations; other failures may have different messages. Callers should
handle the error type rather than rely only on matching its text.

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

## Format support and compatibility

The package accepts `.vgs` and `.pgs` captures. Their detailed differences and
per-version compatibility guarantees are not specified in this distribution
guide; refer to the decoder release notes or ask the Licensor before relying on
a particular producer/decoder version combination.

Applications should handle unsupported versions and features as errors, rather
than assuming that future captures can always be read by older decoders.

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

`vgsexport` is the bridge to everything that does not read VGS: the 3DGS tools, the DCC
importers and the training code all read per-frame `.ply`. Expect it to be large - a
capture is a few hundred megabytes precisely because it does not store frames
independently, and writing them back out separately undoes that.


## Redistributing VGS Decoder

The supplied native library or WebAssembly module may be embedded in or shipped
alongside a commercial or free End Product, including a viewer, game, plugin,
website or importer whose main purpose is reading VGS captures. You may not
redistribute the decoder as a standalone third-party SDK, library or substitute
for the Licensor's developer distribution.

Include the notice required by [LICENSE.md](LICENSE.md) somewhere reasonably
accessible in the End Product's credits, acknowledgements, documentation or
legal notices:

> Includes VGS Decoder. Copyright © 2026 Víctor M. Feliz.

You may distribute End Products in binary form without publishing your own
source code. Modification of the supplied native decoder library or WebAssembly
module is not permitted under the standard licence; the example source code
under `examples/` is expressly modifiable. See [LICENSE.md](LICENSE.md) for
the full terms, limitations and exceptions.

VGS Encoder is separate software and is not licensed with this package.
