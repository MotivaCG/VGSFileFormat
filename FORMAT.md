# VFGS version 2: the `.vgs` and `.pgs` container

A capture is a 4D Gaussian splat recording: a timeline of frames, each a few hundred
thousand splats, laid out so a player fetches and decodes one chunk of time at a time
rather than the whole file.

This document describes the bytes: what is in a capture and where. What to call to read
one is in the decoder's own README, and how the projects here are built is in the
repository's.

Two extensions share one container. `.vgs` codes its attribute pages with static rANS;
`.pgs` stores them as they are. Nothing else differs, and a reader does not need to know
which it has: the header declares the codec per attribute, so both are read by the same
code. `.pgs` exists for cases where CPU time matters more than bytes.

The container holds logical attributes, not the source's bytes. It preserves the exact
packed attribute arrays and scalar reconstruction parameters of a format-6 `.mint`,
including every f16 bit pattern, and drops that container's metadata, padding and opaque
residue. It is therefore **not** a `.mint` and must not be handed to the Gracia runtime.
Recreating a `.mint` byte for byte is what MGS does, which is a different format.

## What a reader gets

- Everything the capture says about itself - title, author, project, take, studio,
  copyright, tags, an identifier and when it was written - before any frame is fetched.
- An Ed25519 signature over that whole region, so whether a file is genuine is decided
  from about a kilobyte, with none of the payload downloaded.
- Global spherical harmonic degree, timing, source bounds, the largest number of active
  splats in any frame and the largest chunk record count.
- A chunk table: where each span of time is in the file and what it covers, so a player
  can seek without reading anything else.
- Base colour, static spherical harmonics and temporal spherical harmonics as three
  independently downloadable contiguous layers, so a reader that evaluates colour detail
  on the GPU, or not at all, can skip fetching it.
- A digest on every chunk directory and every stored page, all of them reachable from the
  signature, so corruption and tampering are caught where they happen rather than
  producing wrong frames.

## Spherical harmonic degree

The header's `shDegree` says how much of the higher-order SH the file kept, and the encoder
takes it as an option rather than always writing what the source had. The index arrays are
planes of three coefficients, one 32-bit word per splat each, so a degree keeps whole
planes: five at degree 3, three at degree 2, one at degree 1, none at degree 0, where both
SH layers are absent. Degree 2 needs eight coefficients and therefore leaves the ninth slot
of its third plane unused; readers evaluate whole bands only, so that slot is never read.

The plane count also appears in each SH index page's `width`, which is what sizes the page;
a file written before the degree could be chosen leaves it at zero and means five. On
`boxing.mint` the degrees measure 44.85, 38.40, 31.99 and 28.78 MB — degree 2 costs 14.4%
less than degree 3, and it is a third of the shader's work in the viewer as well.

## Import profile and current limits

Version 1 accepts the observed SH degree 3 format-6 profile, at whatever frame rate the
source was captured: the timebase is taken from the source, each chunk's duration over
its interval count, and written as the exact fraction that gives it back (1/30, 1/25,
1001/30000). Every chunk must agree on it. Chunks have 1..255 intervals, one dictionary
group followed by splat groups. Unknown nonzero header fields,
nonempty auxiliary top-level records, unknown blocks, missing supported attributes and
unsupported sampling/SH layouts fail explicitly. The source order and rank grouping are
retained. Bounds are measured, not copied: the encoder decodes each chunk it has just written and takes the extent of its live splats over the chunk's own sample grid, which is exact because positions move linearly between samples.
Positions are in source local space; rotations use the conventions below.

Spatial page bounds/culling, Morton reorder, local dictionary dependencies, temporal
rotation checkpoints, lower SH degrees and GPU motion evaluation remain roadmap work.
Pages are numerical work units, not spatial tiles. Each splat group depends on the
chunk's shared dictionaries; no chunk depends on another chunk. The temporal SH entry
array stays in one page to retain its sample-major predictor in v1.

### The encoding field

`encoding` at offset 124 says how the attribute data is built, and every file written so
far says 0: the scheme this document describes. It is deliberately separate from the
per-attribute codec in the policy table, which only says how a page's bytes are packed
after the fact. A different encoding would mean the arrays themselves are formed
differently — absolute values where there are deltas now, say — so a reader that does not
know a value must refuse the file rather than decode it into something plausible and
wrong. Readers validate it as zero today.

## Compression and raw mode

`Compression::Auto` measures raw storage and every supported model over **all pages**
of each attribute type in the file. The smallest aggregate payload wins, once per type.
Policies never change between groups or chunks. rANS is the only entropy codec in v1;
raw is a genuine no-entropy-code path. No TurboPFor, Rust, pcodec or new dependency is used.
`Compression::None` assigns raw to every type and skips all entropy trials.

Raw data retains its binary numerical packing (including quantized values, VQ indices
and f16 fields already present in the source). It is not expanded float32 frame data.
The JavaScript reader returns raw payload views and never loads WASM/workers for an
all-raw capture. Checksums and directory parsing still consume CPU time; direct GPU
upload requires shaders that understand these attribute representations and any upload
alignment required by the graphics API.

Extra payloads, chunk payloads and page payloads each start on a **16-byte boundary**, with
zero padding in between, so a reader can hand a page to a GPU upload without first copying it
into an aligned buffer. A chunk's stored size ends at its last page and carries no trailing
padding. Alignment costs about 1.4 KB in a 45 MB capture.

Pages target 65,536 rows by default (configurable from 1,024 to 1,048,576). Temporal
pages start/end on complete trajectory boundaries. Term pages end on complete splats.
SH index pages gather the corresponding row range from each of five planes. There is
no entropy state shared between pages. File payloads are not implicitly padded/aligned.

## Wire conventions

All integers and IEEE doubles are little-endian. Offsets and sizes are bytes. No native
struct layout is written. Decoders reject unknown versions, required flags, codecs and
unsafe sizes. Current implementation caps one decoded page at 1 GiB; the browser also
caps headers/directories at 64 MiB. Production applications should use lower memory
budgets appropriate to the device. CRC32 is IEEE CRC-32, not cryptographic authentication.

### Authenticity

A capture is signed. Everything a reader needs in order to know what the file is and
where its data lives - the fixed header, the policy, layer, extra and chunk tables, and
the metadata block - is one contiguous region from byte zero, and an Ed25519 signature
over exactly that region follows it. The payload is not signed: captures run to hundreds
of megabytes and are streamed, so a reader that had to hash the whole file before
playing anything would not be a streaming reader.

    [fixed header][policies][layers][extras][chunk table][metadata]   <- signed region
    [signature block]                                                  <- 80 bytes
    [extras payloads][chunk 0][chunk 1]...

The signed range is `[0, signedSize)` and the signature block starts at `signedSize`.
There is no canonicalisation step and nothing to agree about between implementations:
both sides sign and verify a byte range.

The signature reaches past its own region through digests that sit inside it. Each chunk
table entry carries a digest of that chunk's directory, and each extra table entry a
digest of that extra's bytes, so a directory or a thumbnail is authenticated when it is
read rather than when the file is opened. A digest is SHA-512 truncated to its first 16
bytes; SHA-512 rather than a second hash because Ed25519 needs it anyway.

**A reader opens a capture with two reads.** The first `FixedHeaderSize` bytes give
`signedSize`; `[0, signedSize + 80)` is then read and verified. Nothing else is fetched
to decide whether a file is genuine: on a 194 MB capture that is about 1.2 kB.

**Every failure answers the same way.** A missing, truncated, malformed or wrong
signature, an unknown algorithm, an unknown key id, or a file that cannot be read as far
as its signature, all report exactly `invalid 4dgs capture` - no detail about which check
failed, which would only help whoever is trying to get past them. A reader that cannot
authenticate a capture does not decode it.

**Keys rotate without a format change.** The signature block names the algorithm and the
key id, and a reader keeps a table of trusted keys, so a new pair is a new id and an
extra entry: files already in the field keep verifying until nothing signed with the old
key is left. The authoring key lives only in the program that writes captures; a reader,
native or web, holds the public half.

### Signature block: 80 bytes, at signedSize

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u32 | algorithm = 1: Ed25519 |
| 4 | u32 | keyId: which key pair signed this |
| 8 | u32 | signatureSize = 64 |
| 12 | u32 | reserved = 0 |
| 16 | u8 ? 64 | detached Ed25519 signature over `[0, signedSize)` |

### Metadata block

Fixed field order, no names on the wire, UTF-8 throughout, and inside the signed region,
so what a capture says about itself arrives with the first read and cannot be edited
afterwards. Strings are `u32` length then bytes, bounded at 1024 bytes each and 64 kB for
the whole block; a reader rejects a malformed UTF-8 sequence rather than replacing it.

| Order | Type | Field |
|---:|---|---|
| 1 | u8 ? 16 | uuid, raw bytes rather than text |
| 2 | string | id, the catalogue identifier, written by hand |
| 3 | string | title |
| 4 | string | author |
| 5 | string | projectName |
| 6 | string | takeName |
| 7 | string | captureStudio |
| 8 | string | copyright |
| 9 | string | softwareName |
| 10 | string | softwareVersion |
| 11 | u32 + strings | tags, at most 64 |

**Two identifiers, two questions.** `uuid` answers *which file is this*: it is derived
when the capture is written, from the metadata, the chunk digests and the instant of
writing, so two exports of the same take are two identifiable files. `id` answers *which
asset is this*: free text belonging to a catalogue outside this format, written by a
person, and left alone by every tool here. The authoring pipeline defaults it to the
source file's name, which is at least something searchable.

The derived identifier is marked version 8 (RFC 9562), the label for a custom
derivation: it is neither random nor the SHA-1 name-based scheme of version 5.

The two JSON metadata extras stay as they were, for whatever a project wants to carry
that these fields do not describe.

### Fixed header: 184 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | magic `VFGS` = `0x53474656` |
| 4 | u32 | version = 2 |
| 8 | u64 | headerSize: the fixed header and the four tables |
| 16 | u64 | signedSize: headerSize plus the metadata block; where the signature starts |
| 24 | u64 | fileSize |
| 32 | u32, u32 | timeNumerator, timeDenominator: one tick lasts num/den seconds |
| 40 | u64 | frameCount (playback intervals; closing samples are not extra frames) |
| 48 | u64 | durationTicks |
| 56 | u64 | maxSplatsPerFrame |
| 64 | u64 | maxChunkSplatRecords |
| 72 | f64 ? 6 | measured bounds over every frame, minXYZ then maxXYZ |
| 120 | u32 | shDegree, 0 to 3 |
| 124 | u32 | shBasis = 1: real SH, INRIA/3DGS ordering and constants |
| 128 | u32 | coordinates = 1: original .mint local coordinates |
| 132 | u32 | encoding = 0: how the attribute data itself is built (see below) |
| 136 | u32 | policyCount |
| 140 | u32 | chunkCount |
| 144 | u32 | target pageRows |
| 148 | u32 | extraCount |
| 152 | u32 | layerCount |
| 156 | u64 | startTick: where this capture begins on its own timeline |
| 164 | u32 | metadataSize |
| 168 | u64 | createdMillis: when the file was written, milliseconds since the Unix epoch, UTC |
| 176 | u32 | playbackMode: 0 once, 1 loop, 2 ping-pong (see below) |
| 180 | u32 | reserved = 0 |

`headerSize = 184 + 24 * policyCount + 16 * layerCount + 40 * extraCount + 80 * chunkCount`,
and `signedSize = headerSize + metadataSize`. The header block holds the policies, then
the layer table, then the extras table, then the chunk index, then the metadata.

Version 1 files are not readable by this version and are not meant to be: the format had
not shipped, so the layout changed where it needed to rather than growing a compatibility
path nobody would exercise.

**When it was written.** `createdMillis` is inside the signed region, so it is the
capture's own statement rather than a filesystem timestamp that copying, uploading or
re-downloading would destroy. UTC, so it means the same thing wherever it is read, and an
input to the derived identifier, which is what separates two exports of one take.

**How it plays.** `playbackMode` is how the capture's author means it to run: `0` plays
it once and holds the last frame, `1` loops it, `2` plays it there and back. Encoders
write `1` unless told otherwise. It is a default, not a rule: every player starts a
capture this way, and may still let its user choose another. Inside the signed region,
so it stays what the author set. A reader refuses any other value, and a non-zero
reserved field.

**Timebase.** A tick lasts `timeNumerator / timeDenominator` seconds, so 24, 25, 30, 50,
60 and 30000/1001 are all expressible; readers accept 1 to 1000 ticks per second. Frames
are ticks. `startTick` lets a capture carry its position on an external timeline, for
syncing with audio or other takes; it is 0 unless an encoder sets it.

**Chunk length.** Each chunk declares its own interval count, 1 to 255, so an encoder
chooses the length and may vary it (a shorter chunk at a cut, for instance). A chunk is the
unit of random access: reaching any frame costs decoding its chunk and nothing before it.

Shorter is not better here. Most of a chunk is paid once per splat, not per sample:
measured on `boxing`, 32.11 MB of its 44.82 MB are SH indices, residual terms, bases,
scales and lifetimes, and only 12.72 MB are the dictionaries that scale with the 32
intervals. Re-cutting that capture into shorter chunks would cost +72% at 16 intervals,
+215% at 8 and +501% at 4. Around one second per chunk is the sensible default: 30
intervals at 30 Hz, which is also what the MINT profile produces (32).

**Spherical harmonics.** `shDegree` 0 to 3 describes the whole capture. Degree 0 means
base colour only, and such a file declares no SH layers. The MINT import profile writes
degree 3.
For degree d, RGB has `(d+1)^2` coefficients per channel including DC; DC is stored
separately. Header degree describes the complete capture, not the currently loaded layer.
`maxSplatsPerFrame` counts splats whose lifetime satisfies `frame >= start && frame+1 <= end`.
`maxChunkSplatRecords` counts all splat records in the largest chunk, including those not
simultaneously alive. Consumers must not use the frame maximum to size chunk storage.

Each **policy** is six u32: attribute ID, codec (0 raw / 1 rANS), model, model family,
flags and a zero. Flag bit 0 marks the attribute **optional**: a reader that cannot name
the attribute may skip its pages and still reconstruct a frame. An attribute ID this
specification does not define is accepted only when it is both optional and at or above
0x8000 (the vendor range); anything else is refused rather than silently dropped. This is
how the format grows without breaking readers already in the field.

Each **layer** is four u32: id, kind, dependsOn, flags(0). Layer 0 is always the base
layer and depends on nothing; later layers have increasing ids and depend on a layer
already declared. Kinds are 0 base, 1 static higher-order SH, 2 temporal higher-order SH,
and 0x8000 and above for vendors. The table exists so a reader knows what it may skip and
what a layer needs, instead of three meanings fixed in code.
Each **extra** is four fields: u32 type, u32 format, u64 absolute offset, u64 size.
Each **chunk index entry** is: five u64 (startTick, intervals, splats, absolute offset,
stored size), a 16-byte digest of the chunk's directory, then six f32: a box (minXYZ,
maxXYZ) holding every live position in the chunk. The box lets a reader cull a chunk, pick a
level of detail, or fit a shadow map to the capture without decoding anything, and the header
box is the union of these. Files written before this was measured carry the position
quantization range instead, which is the same on all three axes and several times the size of
the capture: it contains the positions, so it is safe, but a reader that fits anything to it
should intersect it with the header box. Chunk time intervals are contiguous. Policies
come first in the header block, then the extras table, then the chunk index.

### Extras: metadata, thumbnails and vendor payloads

Extras are optional payloads the header addresses. Their bytes sit **between the header and
the first chunk**, so a player skips them with one range request and a thumbnail reader
fetches only its own bytes. A file with none costs nothing beyond `extraCount = 0`.

| type | Meaning | format |
|---:|---|---|
| 1 | capture metadata | 1: a single UTF-8 JSON object |
| 2 | thumbnail | 1 PNG, 2 JPEG, 3 WebP |
| 3 | audio | 1 MP3, 2 AAC, 3 Opus, 4 WAV |
| 4 | second metadata block | 1: a single UTF-8 JSON object |
| >= 0x8000 | vendor payload | defined by that vendor |

Audio is carried as delivered and is not transcoded: a clip then travels as one file that
already knows what it sounds like, and a player fetches the track with a single range
request without touching a byte of splat data. The two metadata blocks are separate on
purpose — block 1 belongs to whoever produced the capture, block 2 to whoever uses it, and
neither has to parse or preserve the other's.

Types 5..0x7FFF are reserved for future revisions of this specification; a version 1 reader
rejects them rather than guessing. Vendor payloads must be ignorable: a reader that does not
know a vendor type skips it and still plays the capture. Metadata keys `creator`, `source`,
`take`, `capturedAt`, `license` and `units` are reserved by this specification; anything
else belongs under a vendor-prefixed key. Extras carry no checksum: they are not needed to
reconstruct any frame.

### Chunk directory

Four u32: magic `VCHK` (`0x4b484356`), groupCount, pageCount, directorySize.
Next come group records, then variable-length page records. Directory checksum covers
all directory bytes. Page payloads immediately follow, sorted by layer (0, 1, 2).

Each **group** occupies 128 bytes:

- u32 type (0 dictionary / 1 splats), u32 flags (bit 0 direct positions, bit 1 direct rotations).
- u64 splats, intervals, meshSamples (depth-proxy quota).
- u64 ? 6 dictionary counts: static SH entries, temporal SH entries, SH0 trajectories,
  opacity trajectories, rotation trajectories, position trajectories.
- f64 positionMin, positionMax, trajectoryMin, trajectoryMax.
- u64 ? 2 reserved zeros.

Dictionary fields unused by a splat group and splat fields unused by a dictionary group
are zero. Dictionary group is group 0; splat groups follow in source order.

Each **page** starts with:

- u32 attribute ID, group index, layer ID, stored-payload CRC32.
- u64 firstRow, totalRows, payload offset relative to chunk start, stored size, decoded size.
- Numerical descriptor below.

Pages cover each logical array exactly, without gaps or overlaps. `firstRow` indexes
logical rows, which are terms for term arrays, samples for temporal arrays, and splats
for ordinary splat arrays. SH planes are reassembled using firstRow in **each** plane,
not by concatenating page byte buffers. Layers are 0 base (including DC), 1 all static
higher-order SH, 2 all temporal higher-order SH. Layers 1/2 require base for rendering.

### Numerical descriptor and entropy payload

- u32 kind, family, packed width, fieldCount.
- u64 rows, samples, intervals, entries.
- u32 ? fieldCount bit widths (zero placeholders for scalar columns).
- u32 rankRunCount; per run: u32 rank, u32 count. Ranks strictly increase; reconstruct
  one rank per splat for term models. Non-term arrays have no rank runs.

Kinds: 0 f16 interleaved columns; 1 u8 interleaved columns; 2 packed words; 3 five SH
planes; 4 u16 rotation terms; 5 u32 position terms (u16 dictionary index + f16 weight).
Families 0..9: plain, time, entry, channels, runs, lifetimes, quaternion base, quaternion
time, rotation terms, position terms. Models, field extraction, f16 rank mapping and
rANS streams follow [sections 6 and 7 of the numerical coding document](docs/NUMERICAL_CODING.md#6-columns). A VGS payload
has **no model prefix**: that value is fixed in the global policy. This reuses numerical
coding only; no MGS header, chunk table, original offsets or .mint metadata is embedded.
Raw pages contain exactly the logical array bytes, without the numerical transform.

Attribute IDs and shapes are named in `core/include/vgs/vgscodec.h` and `attributeName()`. IDs 1..11
are shared dictionaries/LUTs; IDs 32..46 are group attributes. Their interpretation,
quantized field widths, quaternion ordering and reconstruction arithmetic are defined
by the source format, whose semantics this import profile retains unchanged. The
normative statement of them here is `core/src/vgsframe.cpp`, which is the code every
reader runs; a reader does not parse a .mint and does not need that format's document.

## Reading and writing one

Writing is a handful of setters and a call. There is no setter for the identifier: it is
derived from the metadata and the contents when the file is written, so two exports of the
same take agree and two different takes never collide.

```cpp
#include "vgsencoder/vgsencoder.h"

vgsenc::Encoder encoder;
encoder.setInputFile("boxing.mint");
encoder.setCoding(vgsenc::Coding::Compressed);   // Plain writes a .pgs
encoder.setSphericalHarmonicDegree(2);
encoder.setAuthor("SMN|The4DSCanner");
if (!encoder.write("boxing.vgs"))
  fprintf(stderr, "%s\n", encoder.lastError().c_str());
```

Reading is an open and a seek. Opening authenticates: if the object exists, the structure,
metadata and chunk table came from the authoring pipeline unaltered.

```cpp
#include "vgsdecoder/vgsdecoder.h"

vgsdec::Capture capture = vgsdec::Capture::openFile("boxing.vgs");
const vgsdec::Frame &frame = capture.setTime(1.5);
```

From the command line:

```sh
vgsencode capture.mint capture.vgs --sh 2 --author "SMN|The4DSCanner"
vgsencode capture.mint capture.pgs --plain
vgsinfo capture.vgs
vgsexport capture.vgs frames/        # the whole timeline as a .ply sequence
```

In a browser, the same decoder compiled to WebAssembly:

```js
import { VgsCapture } from './vgs.mjs';

const capture = await VgsCapture.open('boxing.vgs');
const frame = await capture.setTime(1.5);
```

Nothing about the format is implemented twice. The browser runs the same C++ as the native
library, so there is no second parser to keep in step and, in particular, no second
implementation of the part that decides whether a file is genuine.

## Playing one in real time

Two costs decide whether a host keeps up, and they are different questions.

**Evaluating a frame** happens every frame. For a quarter of a million splats it takes
about 16 ms without spherical harmonics and about twice that with them - so a renderer
that evaluates colour detail on the GPU, which is most of them, spends about half a frame
at 30 fps and has the rest for everything else.

**Decoding a chunk** happens once per chunk, roughly once a second, and takes about
200 ms. Paid on the frame that arrives at the chunk, it is a dropped frame every second no
matter how early the bytes were downloaded. `Capture::prepare` decodes it a few
milliseconds at a time instead, so a player spreads it across the second before:

```cpp
// once a frame, after drawing, with the time left over
capture.prepare(capture.chunkAt(now + 1.0), 8);
```

Measured on a 7-second capture of 247k splats, that turns the chunk-boundary frames from
202 ms into 16 ms - ordinary frames. A budget below about 7 ms per frame does not finish
in time and the boundary is a dropped frame anyway.

A `Capture` belongs to one thread. Run it on its own and let the renderer draw the last
frame that was ready, rather than wait for the next one; the display then runs at its own
rate whatever the decoder is doing, which is what makes playback look smooth.

## Verification

The suite covers the format rather than the code paths that happen to exist: exact source
attributes after a round trip, raw mode, layer filtering, truncation at every boundary,
digest failures, cancellation, and all 65,536 f16 bit patterns.

Three of them are about the guarantees rather than the bytes. A tamper suite alters the
header, the metadata, each table and a chunk directory in turn and requires every one to
be refused with exactly `invalid 4dgs capture`. A leak test scans the built decoder
library, the tools and the WebAssembly module for the signing key and fails if it finds
it. And the WebAssembly decoder is run against the native one over the same capture, so
the two cannot drift apart unnoticed.

```sh
ctest --test-dir build_win64 -C Release --output-on-failure
```

Tests that need a capture are skipped unless one is supplied: configure with
`-DVGS_TEST_MINT=/path/to/capture.mint`.
