# The .mgs format (Mint GS)

Naming, 2026-09-18: the implemented lossless format is **MGS**, extension `.mgs`,
magic `MGS1` (`0x3153474D` little-endian), version **4**. It was previously called
VGS. The payload layout and compression are unchanged. Old version-4 files can be
migrated by replacing their first four bytes `VGS1` with `MGS1` and renaming the
extension; renaming the extension alone is insufficient. The current reader accepts
`MGS1` only. **VGS** / `.vgs` / magic **`VFGS`** is reserved for the future format in
the design notes, not supported by this decoder.

A `.mgs` is a Gracia `.mint` recoded losslessly. Decoding one gives back the original
`.mint` byte for byte, so the Gracia runtime plays it unchanged: the viewer rebuilds each
chunk and hands the SDK a file-like object it reads as a local `.mint`.

On the captures measured so far it is **80-83% of the `.mint`**, against 92-94% for gzip.

This document specifies the bitstream. The container it encodes is specified in
the source format document; a `.mgs` decoder needs that document too, because the
plan in section 4 is derived from the `.mint` metadata. the design notes
is a separate proposal for a format that would not be tied to `.mint` at all.

Implementation: [mgs/mgscodec.h](mgs/mgscodec.h) and `mgs/mgscodec.cpp` (plain C++17),
`mgs/wasm/` (the WebAssembly decoder the viewer loads), `mgs/web/` (its JavaScript),
`mgs/tools/mgstool.cpp` (command line). The converter writes one through
*Despill and export mint...*, output format *MGS capture (.mgs)*.

## 1. Conventions

- All integers are little-endian. The decoder refuses to run on a big-endian platform.
- `u8`, `u32`, `u64`: fixed-size unsigned integers.
- `varint`: LEB128, seven bits per byte, low group first, high bit means "more follows".
- Offsets called *chunk offsets* are relative to the first byte of the chunk's payload
  region inside the `.mint`, not to the file.
- Sizes are bytes unless stated otherwise.

## 2. Why the format looks like this

Three properties drove every decision, and each is measured in
the design notes section 3:

1. **The runtime reads whole chunks**, one range request each, so chunks are the unit of
   independence: a reader fetches and decodes exactly one.
2. **A phone must keep up.** A chunk splits into items that decode in parallel workers,
   and the table at the front of a chunk says where each one lands without parsing `.mint`.
3. **Nothing may be lost.** Anything the encoder does not recognise, including padding,
   goes to a residue stream, so an unexpected layout costs size, never correctness.

## 3. File layout

```
u32   magic          "MGS1" (0x3153474D)
u32   version        4
u64   mintSize       size of the .mint this rebuilds
u64   metadataSize
u8[]  metadata       .mint bytes [0, metadataSize): its whole metadata region
u64   tailSize
u8[]  tail           .mint bytes after the last chunk
u32   chunkCount
      per chunk:
u64     mintStart    where the chunk's bytes start in the .mint
u64     mintSize     how many bytes it covers
u64     mgsSize      the size of its payload here
u8[]  payloads       the chunks, in order, sizes as listed above
```

The header is fixed-size once `chunkCount` is known, so a reader gets everything it needs
to seek from one small range request at the start of the file. Chunk payloads follow in
order; a chunk's payload offset is the end of the header plus the sizes before it.

The encoder requires the `.mint` chunks to be stored contiguously and in order, which is
what Gracia writes. Metadata and tail cover everything outside them, so every byte of the
original file is accounted for.

## 4. A chunk payload

```
varint rawCount                  spans copied verbatim (the rank boundaries)
       per span: varint offset, varint size          (chunk offsets)
u8[]   rawBytes                  the spans' contents, concatenated
--- the bytes up to here are the chunk's "head" ---
varint itemCount
       per item: varint offset, varint size, varint payloadSize
varint residueSpanCount
       per span: varint offset, varint size
varint residueSize
u8[]   itemPayloads              concatenated, sizes as listed
u8[]   residue                   one order-0 byte stream (section 7.5)
```

Every byte of the chunk is covered exactly once by the raw spans, the items and the residue
spans, so a decoder can allocate the chunk, fill those three, and be done.

**The head** is everything before `itemCount`. It is the only part a planner needs: the raw
spans hold the rank boundary arrays, and section 5 needs those to know how many residual
terms each splat has. The viewer sends the head to each worker with every item.

## 5. The item plan

Both sides derive the same ordered list of items from the `.mint` metadata plus the head.
Nothing about the list is stored, apart from the offsets and sizes in the table, which a
decoder must check against its own plan.

For each chunk, with `T` = intervals of the first splat group and `S = T + 1` samples:

**Raw items**, in block order, for each type-1 group: `rotation_rank_boundaries`, then
`position_rank_boundaries`, each as its whole array.

**Coded items**, from the type-3 shared block first (skipped unless the chunk has both a
shared block and at least one group, and `0 < T < 65536`):

| Array | Logical size | Kind | Family | Rows per column |
|---|---|---|---|---|
| `sh_static_codebooks` | `15·3·2·ns` | f16 ×3 | plain | `15·ns` |
| `sh_temporal_codebooks` | `S·15·3·2·nt` | f16 ×3 | entry (`E = 15·nt`) | `S·15·nt` |
| `sh0_trajectories` | `nc·S·3·2` | f16 ×3 | time | `nc·S` |
| `opacity_trajectories` | `no·S·2` | f16 ×1 | time | `no·S` |
| `rotation_initial` | `nr·4·2` | f16 ×4 | plain | `nr` |
| `rotation_delta_indices` | `nr·T·4` | u8 ×4 | runs (`T`) | `nr·T` |
| `position_trajectories` | `np·S·8` | packed 8 `[1,21,21,21]` | time | `np·S` |

then, for each type-1 group with `n` splats and `intervals == T`:

| Array | Logical size | Kind | Family | Rows |
|---|---|---|---|---|
| `lifetimes` | `2n` | u8 ×2 | lifetimes | `n` |
| `scale_indices` | `3n` | u8 ×3 | channels | `n` |
| `sh0_base_indices` | `3n` | u8 ×3 | channels | `n` |
| `opacity_rq_indices` | `8n` | packed 8 `[12,12,12,12,12,4]` | plain | `n` |
| `sh0_rq_indices` | `8n` | packed 8 `[12,12,12,12,12,4]` | plain | `n` |
| `sh_static_indices` | `5·4n` | planes `[10,10,10,2]` | plain | `n` |
| `sh_temporal_indices` | `5·4n` | planes `[10,10,10,2]` | plain | `n` |
| `rotation_base` | `4n` | packed 4 `[1,9,10,10,2]` | quatBase | `n` |
| `rotation_samples` | `4nS` | packed 4 `[1,9,10,10,2]` | quatTime | `nS` |
| `position_base` | `8n` | packed 8 `[1,21,21,21]` | plain | `n` |
| `position_samples` | `8nS` | packed 8 `[1,21,21,21]` | time | `nS` |
| `rotation_rq_indices` | `used·2` | rotTerms | terms | `used` |
| `position_rq_coefficients` | `used·4` | posTerms | posTerms | `used` |

An array is coded only if it exists, its logical size is non-zero, the array is at least
that large, and it lies inside the chunk. Anything skipped, and every array's padding
beyond its logical size, ends up in the residue. `ns`, `nt`, `nc`, `no`, `nr`, `np` are the
dictionary entry counts from the shared block header.

**Rank layout.** `rotation_rq_indices` and `position_rq_coefficients` store a variable
number of terms per splat, grouped by count. Reading `R` boundaries (`5` for rotation, `4`
for position) as u32 from the head: splats `[0, b[0])` have 1 term, `[b[0], b[1])` have 2,
and so on; `used` is the total. Boundaries must be non-decreasing and end at `n`, or the
array falls back to the residue. Before allocating by `n`, the plan checks that `n · width`
fits in the array, since every splat has at least one term.

## 6. Columns

Each item is split into integer columns, and every column is coded on its own.

- **f16 ×C**: `C` half floats per row. Each is mapped through a **value rank**: the 65,536
  bit patterns sorted by numeric value, NaNs last, ties broken by pattern. Close values get
  close integers, so a delta between neighbours is small. The mapping is a bijection, so
  the original bits come back exactly, including both zeros and every NaN.
- **u8 ×C**: `C` bytes per row.
- **packed w `[widths]`**: one `w`-byte word per row, split into bit fields from the low bit
  up. The widths sum to `8w` bits.
- **planes `[10,10,10,2]`**: five consecutive arrays of `n` u32 words, each split into four
  fields: 20 columns, column `4p + f` being field `f` of plane `p`.
- **rotTerms**: one u16 per term, one column.
- **posTerms**: one u32 per term: the low 16 bits are a trajectory index, the high 16 a half
  float weight mapped through the value rank. Two columns.

A decoder writes columns into the item's own bytes as they arrive, so only the columns a
model needs as context stay alive. The first column written into a word assigns it and
later ones accumulate, which is why the output is never zero-filled first.

## 7. Models

Each item payload starts with `u8 model`, an index into its family's list. The encoder
tries every model of the family and keeps the smallest output; a model that does not apply
(for example sorted terms that are not sorted) is skipped. Model 0 is always "each column
order-0", so it always applies.

| Family | Models |
|---|---|
| plain | 0 order-0 |
| time | 0 order-0, 1 temporal delta |
| entry | 0 order-0, 1 delta against the same entry one sample back |
| channels | 0 order-0, 1 channel given the previous channel |
| runs | 0 order-0, 1 value given the same component one interval back |
| lifetimes | 0 order-0, 1 start, then duration given start |
| quatBase | 0 order-0, 1 components given which one was dropped |
| quatTime | 0 order-0, 1 quaternion samples over time |
| terms | 0 order-0, 1 first term plus gaps |
| posTerms | `2·weights + indices`: indices 0 order-0 / 1 first plus gaps; weights 0 order-0 / 1 given the term slot |

### 7.1 Temporal models

Values live in runs: `S` consecutive rows are one trajectory (`time`), or the rows `E` apart
are the same codebook entry at successive samples (`entry`).

- `time`: row `i` is stored as the value itself when `i mod S == 0`, otherwise as
  `zigzag(v[i] - v[i-1])`. The context is `min(i mod S, 2)`.
- `entry`: rows `i < E` hold values, the rest `zigzag(v[i] - v[i-E])`. No context.
- Both then **split into two streams**: one with the absolute values, one with the deltas,
  both derivable from the row index. A single stream made the small deltas pay the raw low
  bits the wide absolutes needed (measured: `position_samples` 0.22 → 0.09 MB).

### 7.2 Quaternion samples (`quatTime`)

Columns are `[sign, a, b, c, largest]` of the smallest-three encoding. `largest` and `sign`
are coded with the previous sample's value as context (context 4 and 2 at the start of a
run). From them both sides derive, per row:

```
cv = 0                       first sample of the run
   = 1                       the dropped component changed
   = 2                       same dropped component, same sign
   = 3                       same dropped component, sign flipped
```

`a`, `b` and `c` are deltas from the previous sample when `cv >= 2` and absolute otherwise,
coded with `cv` as context and split as in 7.1 (absolute when `cv < 2`).

### 7.3 Residual terms

Terms are stored ascending inside a splat, so "first + gaps" costs nothing to enable. The
first term of each splat is coded with the splat's rank as context; the gaps likewise. A
gap below zero means the terms are not sorted and the model does not apply.

For `posTerms` the index and weight columns choose their models independently: the weights
can use the term slot (0-3) as context.

### 7.4 Symbol streams

Every column ends up as one or more symbol streams:

```
varint A            alphabet size (max symbol + 1), at most 65536
u8     bits         12 to 16; M = 1 << bits
varint nctx         number of contexts, derived by the encoder from the data
       per context: u8 mode
           0: empty
           1: sparse: varint count, then per symbol varint gap-since-last, varint freq-1
           2: dense: A varints, one frequency each
u8     states       interleaved rANS states (4, or 1 for streams under 4096 symbols)
varint length ×states
u8[]   bytes ×states
```

Frequencies sum to `M` in every non-empty context. `bits` is `clamp(bitlen(A-1) + 3, 12, 16)`:
tables stay in cache for small alphabets and stay precise for big ones.

**Decoding.** State `s` holds symbol `i` where `s = i mod states`, decoded in order of `i`,
so a context may still be the symbol before it. Per symbol, with context `c`:

```
slot = x & (M-1)
sym  = slots[c][slot]                        slots[c] maps a slot to its symbol
x    = freq[c][sym] * (x >> bits) + slot - cum[c][sym]
while x < 2^23:  x = (x << 8) | nextByte()
```

Each state starts by reading four bytes as its initial `x`. A valid stream is consumed
exactly, so running out of bytes means the data is corrupt.

**Wide values** (anything that can exceed the alphabet) are coded as `u8 lo`, then the
symbols `v >> lo`, then the low `lo` bits of every value packed LSB-first with three bytes
of padding. The encoder tries `lo = max(0, bitlen(max) - 16/12/8)` and keeps the smallest.

### 7.5 Residue

Every byte no item covers, in chunk order, as one order-0 symbol stream of bytes. It holds
the small luts, padding, and anything the plan skipped.

## 8. Decoding

1. Read the header (section 3). A reader that has only the first bytes can ask for more.
2. For a chunk, fetch its payload range and read the table (section 4).
3. Copy the raw spans into the chunk.
4. Plan the chunk from the metadata and the head (section 5), and check every item's
   offset and size against the table.
5. Decode items, in any order or in parallel, each into its own `size` bytes, and place
   them at `offset`.
6. Decode the residue and scatter it over the residue spans.

The C++ API mirrors these steps: `readHeader`, `readChunkTable`, `ItemDecoder::plan`,
`ItemDecoder::decodeItem`, `decodeResidue`. `decodeChunk` and `decodeFile` are
single-threaded convenience wrappers, and `encodeFile` does the whole conversion.

### 8.1 In the browser

`mgs/web/mgsreader.mjs` exposes `readMgsHeader`, `readChunkTable`, `MgsDecoder` (a pool of
workers) and `MgsFile`, which presents a remote `.mgs` as the `.mint` it encodes: `size`
and `slice(a, b).arrayBuffer()`, which is all the Gracia SDK asks of a local file. Chunks
are range-fetched on demand; if a server ignores `Range` and sends the whole file, that is
kept and used. Compressed payloads are cached within a memory budget and the last couple of
decoded chunks are kept, because the runtime re-reads a chunk every time it re-enters its
four-chunk window.

Each worker (`mgsworker.mjs`) holds one WebAssembly instance built by `mgs/wasm/build.bat`,
which embeds the module in a single ES file so no server has to know the `.wasm` media type.
Its C entry points are `mgs_init` (metadata), `mgs_plan` (chunk head), `mgs_item_size`,
`mgs_decode_item`, `mgs_decode_residue` and `mgs_error`.

### 8.2 Untrusted input

The decoder runs on downloaded bytes, so every length, offset, span and table is bounds
checked, sizes are computed with saturating multiplication, and failures throw
`mgs::Error`. A corrupt file produces an error, not a wrong `.mint` and not a read outside
the buffer. It is not hardened beyond that: the WebAssembly sandbox is the second line.

## 9. Measurements

Decode times are one chunk of `boxing.mint` (56.18 MB), native single thread on the
development machine, and Chrome with a pool of workers.

| Capture | `.mint` | `.mgs` | Share | gzip -6 |
|---|---|---|---|---|
| boxing | 56.18 MB | 45.13 MB | 80.3% | 92.5% |
| mario | 41.34 MB | 33.57 MB | 81.2% | 93.5% |
| boxingfull | 279.09 MB | 225.49 MB | 80.8% | 94.0% |
| tiki | 447.59 MB | 370.77 MB | 82.8% | 93.5% |

| Decoder | 1 thread | 4 workers |
|---|---|---|
| Native C++ | 216 ms | — |
| Chrome, WebAssembly | 281 ms | 97 ms |
| Android phone (Chrome, version 2 of the format) | 743 ms | 234 ms |

Encoding a capture takes 0.9 s (boxing) to 10.6 s (tiki).

## 10. Version history

The version in the header is exact: a decoder refuses anything else rather than risk
misreading it. Every change so far was a re-encode of the same captures.

| Version | Change |
|---|---|
| 1 | First format: header, per-chunk item payloads, models, rANS. |
| 2 | Chunk table inside each payload, so a reader never parses `.mint`. |
| 3 | Absolute values and deltas in separate streams (7.1). |
| 4 | Four interleaved rANS states; decoder no longer zero-fills its output. |

## 11. Verifying a change

1. `mgstool encode capture.mint out.mgs` re-decodes what it wrote, chunk by chunk and then
   whole, and compares it with the input. It fails loudly on the first differing byte.
2. Run it on all four captures above: they cover one and several chunks, both per-sample
   encoding modes, and a short final chunk.
3. In the browser, the bench page decodes every chunk in workers and checks a hash of each
   rebuilt chunk against the original `.mint`.
4. In the viewer, play a `.mgs` scene and the `.mint` scene it came from and compare
   screenshots at the same times: they must differ only by render noise (two renders of the
   same `.mint` differ at about 70 dB PSNR).
