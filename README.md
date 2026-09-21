<div align="center">

<img src="docs/logo.png" alt="The4DScanner" width="150">

## VGSFileFormat

**The VGS container: `.vgs` and `.pgs`**

</div>

---

The format, the encoder that writes it, the decoder that reads it, and the tools that
exercise both.

A capture is a 4D Gaussian splat recording: a timeline of frames, each a few hundred
thousand splats, stored so that a player can fetch and decode one chunk of time at a
request. Every file carries what it is (title, author, project, take, studio, copyright,
tags) and an Ed25519 signature over its whole structural region, so a reader knows a file
is genuine after about a kilobyte, before any of the payload has been fetched.

[FORMAT.md](FORMAT.md) describes the bytes. This file describes the projects and how to
build them.

## The three projects

    core/        the codec: container, entropy coding, frame evaluation, crypto
    encoder/     libvgsencoder  + vgsencode                   writes captures
    decoder/     libvgsdecoder  + vgsinfo, vgsplay, vgsdump, vgsexport, vgspagecost, WebAssembly
    tests/       the conformance and separation tests
    cmake/       package files for find_package
    logo.png     the mark, full size; docs/logo.png is a copy for documents
    logo.ico     the same as a Windows icon, compiled into the tools

`core/` builds nothing on its own. It hands the other two a list of sources each, and the
lists are not the same:

| | encoder | decoder |
|---|---|---|
| container reader, entropy codec, frame evaluation | yes | yes |
| SHA-512, Ed25519 verification, public keys | yes | yes |
| container **writer**, MINT import | yes | **no** |
| Ed25519 **signing**, private key | yes | **no** |

That asymmetry is the point. The decoder is the half that ships - to a customer, to a
browser - and it cannot write a capture, because the code to do so is not in it, and could
not sign one if it could, because the private key is not in it either. `vgskeys.h` refuses
to compile unless `VGS_AUTHORING` is defined, which only the encoder target does, so the
separation is a build error rather than a convention. `tests/keyleak` checks the built
artefacts for the key bytes and fails if it finds them.

What this does **not** do is hide the format from someone who has the decoder. Anything
that can read a file teaches a determined reader how; the signature does not prevent
reading, it prevents forging. Protection here means nobody else can produce a capture that
a viewer of ours will accept - and that rests entirely on the private key.

## Building

There is one CMake project and **two build directories**. Not two projects, and nothing to
do with source control: a native library and a WebAssembly module come from two different
toolchains, and one configured build tree can only hold one of them.

    build_win64/   the static libraries and the tools, built by MSVC
    build_wasm/    the WebAssembly decoder, built by Emscripten

Each installs into its own `INSTALL/` inside itself, so the two never overwrite each other.

### The short way

```powershell
.\wasm_package.ps1
```

That builds both, installs both, and assembles `dist\vgsdecoder-<version>\` ready to hand
out - native library, headers, tools, sample source and the WebAssembly module in one
folder - plus `dist\vgsencoder-<version>\` for us. It also refuses to finish if it finds
the signing key in anything it is about to package. Use this unless you are iterating on
one of the two.

`.\wasm_package.ps1 -Shared` builds DLLs instead. `.\wasm_package.ps1 -SkipWasm` skips Emscripten.
`-EmsdkRoot <path>` points it at an emsdk somewhere other than `D:\Dependencias\emsdk`.

### Native, by hand

```powershell
cmake -S . -B build_win64 -G "Visual Studio 17 2022" -A x64
cmake --build build_win64 --config Release
cmake --install build_win64 --config Release
```

On Linux and macOS the generator and the config flag go away:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build
```

### WebAssembly, by hand

```powershell
emcmake cmake -S . -B build_wasm -G Ninja -DVGS_BUILD_WASM=ON
cmake --build build_wasm
cmake --install build_wasm
```

`emcmake` has to be on PATH, which an unpacked emsdk is not by default. If `emsdk_env` has
never been run - it needs an activated SDK, which a plain checkout does not have - set the
toolchain up from the folder itself:

```powershell
$env:EMSDK_ROOT = 'D:\Dependencias\emsdk'
$env:EM_CONFIG  = "$env:EMSDK_ROOT\.emscripten"
$node   = (Get-ChildItem "$env:EMSDK_ROOT\node\*"   -Directory | Select-Object -First 1).FullName + '\bin'
$python = (Get-ChildItem "$env:EMSDK_ROOT\python\*" -Directory | Select-Object -First 1).FullName
$ninja  = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = "$env:EMSDK_ROOT\upstream\emscripten;$node;$python;$ninja;$env:PATH"
```

Ninja is on that list because the Visual Studio generator cannot drive Emscripten; the
copy that ships inside Visual Studio does the job.

`VGS_BUILD_WASM=ON` skips the encoder entirely - a browser has no business holding the
signing key - and builds `decoder/wasm/` instead of the static library. See
[decoder/wasm/README.md](decoder/wasm/README.md) for the JavaScript side.

### Options

| option | default | |
|---|---|---|
| `VGS_BUILD_TOOLS` | `ON` | the command line encoder and the decoder samples |
| `VGS_BUILD_TESTS` | `ON` | the test programs, run with `ctest` |
| `VGS_BUILD_WASM` | `OFF` | build the WebAssembly decoder instead of the libraries |
| `VGS_BUILD_SHARED` | `OFF` | build DLLs (`.so`) exporting the C interface, instead of static libraries |
| `VGS_CACHE_BEHIND` | per target | decoded chunks kept behind the one being played |
| `VGS_CACHE_AHEAD` | per target | decoded chunks kept ahead of it |
| `VGS_CACHE_MAX_BYTES` | per target | ceiling on the decoded chunks held |
| `CMAKE_INSTALL_PREFIX` | `<build dir>/INSTALL` | where `--install` puts things |

The three cache options set where a capture starts; `setCachePolicy` overrides them at
runtime. Both builds keep one decoded chunk either side of the one being played; the
WebAssembly build adds a 192 MB ceiling, because a browser does not get to decide what
machine it runs on.

## What an install gives you

    INSTALL/
      bin/      vgsencode, vgsinfo, vgsplay, vgsdump, vgsexport, vgspagecost
      include/  vgsencoder/vgsencoder.h, vgsencoder_c.h
                vgsdecoder/vgsdecoder.h, vgsdecoder_c.h
      lib/      vgsencoder.lib, vgsdecoder.lib     (.a on Linux)
                cmake/VGSEncoder, cmake/VGSDecoder
      share/    vgsdecoder/examples: the source of the four samples, with a
                CMakeLists.txt that builds them against this very install

The codec's own headers are not installed. A consumer sees the encoder and decoder APIs
and nothing else, so none of the container's internals end up in somebody else's build.

Each library comes with a package file, so a consuming project needs three lines:

```cmake
find_package(VGSDecoder REQUIRED PATHS ../VGSFileFormat/build_win64/INSTALL)
add_executable(player main.cpp)
target_link_libraries(player PRIVATE VGS::Decoder)
```

The samples' source ships too, so a consumer gets working code for every part of the API
and can build it on the spot:

```bash
cd INSTALL/share/vgsdecoder/examples
cmake -S . -B build && cmake --build build --config Release
```

That directory refers to nothing outside the install, which makes it the shortest check
that an install is complete.

### Static or shared

Both libraries also have a flat C interface (`vgsencoder_c.h`, `vgsdecoder_c.h`) for
callers that will not link a C++ runtime: a plugin ABI, a language binding, a host
application built with a different compiler.

`-DVGS_BUILD_SHARED=ON` builds DLLs instead of static libraries. A DLL exports that C
interface and nothing else - 44 functions, all named `vgs_*`, with none of the C++ API,
the codec or the crypto reachable from outside. That is deliberate: the C++ API passes
`std::string` and `std::vector` across the boundary, which is fine for a static library
both sides compiled the same way, and a source of silent corruption across a DLL.

Choose between them on integration, not on secrecy. Neither hides the format: both carry
the whole decoder in machine code, and a static `.lib` is in fact the more talkative of
the two, since it keeps every internal symbol name for the linker to read. Take the static
library when the consumer builds with your compiler and runtime; take the DLL when they do
not, and have them use the C interface.

## Using it

Writing a capture is a handful of setters and a call:

```cpp
#include "vgsencoder/vgsencoder.h"

vgsenc::Encoder encoder;
encoder.setInputFile("boxing.mint");
encoder.setCoding(vgsenc::Coding::Compressed);   // Plain writes a .pgs
encoder.setSphericalHarmonicDegree(2);
encoder.setAuthor("SMN|The4DSCanner");
encoder.setCaptureStudio("Valladolid");
encoder.setProgressCallback([](int percent, const char *stage) {
  printf("%3d%% %s\n", percent, stage);
  return true;                                   // false cancels
});
if (!encoder.write("boxing.vgs"))
  fprintf(stderr, "%s\n", encoder.lastError().c_str());
printf("identifier: %s\n", encoder.uuidText().c_str());
```

There is a setter for every metadata field except the identifier, which is derived from
the metadata and the contents when the file is written, so two exports of the same take
agree and two different takes never collide.

Reading one is an open and a seek:

```cpp
#include "vgsdecoder/vgsdecoder.h"

vgsdec::Capture capture = vgsdec::Capture::openFile("boxing.vgs");
// If this line runs, the capture authenticated.

for (double t = 0; t < capture.duration(); t += 1.0 / 30) {
  const vgsdec::Frame &frame = capture.setTime(t);
  draw(frame.positions, frame.rotations, frame.scales, frame.colors, frame.splatCount);
}
```

`setTime` evaluates every splat on the CPU. `setOutput(Output::Packed)` stops after
decompressing instead, handing over the chunk's buffers and where the frame falls between
two of its samples, for a renderer that evaluates in its own shader: the per-frame cost
then stops depending on the splat count. `prepare` spreads a chunk's decompression over
the frames before it is needed, and `setThreadCount` or `setParallelFor` spread it over
cores. `vgsplay` measures all of it.

Anything wrong with a capture's authenticity - altered header, metadata, tables or chunk
directories, a broken or missing signature, an unknown key - throws
`vgsdec::Error("invalid 4dgs capture")`. That string is the same in every implementation,
including the JavaScript one, on purpose.

## The tools

| | |
|---|---|
| `vgsencode in.mint out.vgs [options]` | writes a capture; `--plain` for `.pgs`, `--sh 0..3`, and a flag per metadata field |
| `vgsinfo capture.vgs` | prints what the capture says about itself |
| `vgsplay capture.vgs` | walks the whole timeline and reports what decoding it cost |
| `vgsdump capture.vgs 1.5 frame.ply` | one instant as a Gaussian splat `.ply` |
| `vgsexport capture.vgs out/` | the whole capture as a numbered `.ply` sequence |
| `vgspagecost capture.vgs` | where the bytes and decoding time go, per attribute and per detail level, and what a CPU-sorting player spends |

`vgsexport` is the bridge to everything that does not read VGS - the 3DGS tools, the DCC
importers, the training code all read per-frame `.ply`. Expect it to be large: a capture is
a few hundred megabytes precisely because it does not store frames independently.

Run `ctest --test-dir build_win64 -C Release` for the test suite.

## Decoding less: `Detail`

`prepare(chunk, budget, Detail)` says how much of a chunk to decode: `Positions`, `Base` or
`Full`, each holding everything the one before it does. `Positions` is for a renderer that
evaluates on the GPU but sorts on the CPU - WebGL, with no compute shaders to sort with -
and so needs positions every frame and nothing else; `positionsAt` asks for it on its own.
On `boxing_despill.vgs` it decodes a chunk in about a third of the time the base layer
takes, and the decoded chunk holds 10 MB rather than 25. `vgspagecost` measures it on any
capture, and prints what a CPU-sorting player then spends per second of playback.

The earlier `prepare(chunk, budget, bool)` still compiles and means what it did (true is
`Full`, false `Base`), and so does the JavaScript `{ sphericalHarmonics }` option.

## Third-party code

One file is not ours: `core/crypto/tweetnacl.c`, the reference Ed25519 and SHA-512
implementation, vendored unchanged and in the public domain. Its header records where it
came from and who wrote it.

Public domain means there is nothing to comply with and nothing for a customer to carry,
so the package that ships says nothing about it - a notice somebody does not owe is noise
in a document they are reading to get started. The fact lives here and in the file itself,
which is where anyone auditing a binary would look.

The JavaScript has no third-party code. Verifying a signature in a browser would otherwise
mean bundling an MIT-licensed Ed25519 library and carrying its notice into every consumer
bundle; doing the check inside the WebAssembly module removes that entirely.
