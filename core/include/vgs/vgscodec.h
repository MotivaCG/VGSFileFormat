#ifndef VGS_CODEC_H
#define VGS_CODEC_H

#include "mgscodec.h"
#include "vgscrypto.h"
#include <array>
#include <string>

namespace vgs {
using Bytes = mgs::Bytes;
using Error = mgs::Error;
using Spec = mgs::AttributeSpec;
constexpr uint32_t Magic = 0x53474656; // VFGS
// Version 2 signs the structure and carries capture metadata in the header. Nothing
// reads a version 1 file: the layout changed where it mattered and the format had not
// shipped, so there is no fallback path to keep honest.
constexpr uint32_t Version = 2;
constexpr uint32_t FixedHeaderSize = 176;
// Signature block: algorithm, key id, length, reserved, then the signature itself. It
// sits immediately after the signed region, so [0, signedSize) is what was signed and
// [signedSize, signedSize + SignatureBlockSize) is the proof - no overlap, nothing to
// agree about between two implementations.
constexpr uint32_t SignatureBlockSize = 80;
enum SignatureAlgorithm : uint32_t { Ed25519Signature = 1 };

// What a capture says about itself. Held in the header rather than in an extra so it
// is covered by the signature and arrives with the first range request: a catalogue
// can read provenance without fetching a single splat.
//
// Strings are UTF-8, each at most MaxMetadataString bytes; the whole block is bounded
// so a malformed file cannot ask a reader for an unreasonable allocation.
constexpr uint32_t MaxMetadataString = 1024;
constexpr uint32_t MaxMetadataTags = 64;
constexpr uint32_t MaxMetadataSize = 64 * 1024;
struct Metadata {
  // Raw 16 bytes, not text: an identifier that survives being copied between systems
  // without a formatting convention to argue about. Derived when a capture is written,
  // so it names that export exactly.
  std::array<uint8_t, 16> uuid{};
  // The catalogue identifier, written by whoever files the capture rather than derived
  // from it. Free text on purpose: it belongs to a system outside this format.
  std::string id;
  std::string title, author, projectName, takeName, captureStudio, copyright,
      softwareName, softwareVersion;
  std::vector<std::string> tags;
};

// The proof that the structure above came from the authoring pipeline. keyId names the
// key so that several can be trusted at once and one can be retired without a format
// change; algorithm is here for the same reason.
struct SignatureInfo {
  uint32_t algorithm = 0, keyId = 0;
  Signature signature{};
};

// Optional payloads the header addresses: capture metadata, preview images and vendor
// data. They sit between the header and the first chunk, so a player skips them with one
// range request and a thumbnail reader fetches only its own bytes. A file with none costs
// nothing. Unknown types are ignored by readers, not rejected.
enum ExtraType : uint32_t {
  MetadataExtra = 1,   // format 1: one UTF-8 JSON object
  ThumbnailExtra = 2,  // format 1 PNG, 2 JPEG, 3 WebP
  // The capture's own sound, so a clip travels as one file rather than as a
  // capture plus an audio track a player has to be told about separately.
  AudioExtra = 3,      // format 1 MP3, 2 AAC, 3 Opus, 4 WAV
  // A second metadata object, kept apart from the first on purpose: one belongs
  // to whoever produced the capture, the other to whoever uses it, and neither
  // has to parse or preserve the other's.
  MetadataExtra2 = 4,  // format 1: one UTF-8 JSON object
  VendorExtraBase = 0x8000
};
enum MetadataFormat : uint32_t { JsonUtf8 = 1 };
enum ImageFormat : uint32_t { Png = 1, Jpeg = 2, Webp = 3 };
enum AudioFormat : uint32_t { Mp3 = 1, Aac = 2, Opus = 3, Wav = 4 };
struct Extra {
  uint32_t type = 0, format = 0;
  uint64_t offset = 0, size = 0;
  // Of the extra's bytes, which live outside the signed region. The table entry is
  // signed, so checking this when the extra is actually read extends the signature to
  // a thumbnail or an audio track without fetching either to open the file.
  Digest digest{};
};
struct ExtraInput {
  uint32_t type = 0, format = 0;
  Bytes bytes;
};

enum Attribute : uint32_t {
  ScaleLut = 1,
  ShStaticBook,
  ShTemporalBook,
  Sh0Trajectories,
  Sh0Lut,
  OpacityTrajectories,
  RotationInitial,
  RotationDeltaLut,
  RotationDeltas,
  PositionTrajectories,
  MeshExtentLut,
  ScaleIndices = 32,
  PositionSamples,
  ShStaticIndices,
  ShTemporalIndices,
  Lifetimes,
  RotationSamples,
  Sh0Terms,
  Sh0Base,
  OpacityTerms,
  RotationBase,
  RotationTerms,
  RotationRanks,
  PositionBase,
  PositionTerms,
  PositionRanks
};
enum Codec : uint32_t { Raw = 0, Rans = 1 };
// An optional attribute may be skipped by a reader that does not know its ID, which is
// how this format grows without breaking readers already in the field. Attributes a
// frame cannot be reconstructed without are never optional.
enum PolicyFlags : uint32_t { OptionalAttribute = 1 };
constexpr uint32_t VendorAttributeBase = 0x8000;
struct Policy {
  uint32_t attribute = 0, codec = Raw, model = 0, family = 0, flags = 0;
};

// Layers are contiguous, independently downloadable parts of every chunk. The table
// names them, so a reader knows what it may skip and what a layer needs, instead of
// hard-coding three meanings.
enum LayerKind : uint32_t {
  BaseLayer = 0,        // everything a frame needs at base colour
  StaticShLayer = 1,    // higher-order SH that does not change within a chunk
  TemporalShLayer = 2,  // higher-order SH that changes per sample
  VendorLayerBase = 0x8000
};
struct Layer {
  uint32_t id = 0, kind = 0, dependsOn = 0, flags = 0;
};
struct ChunkEntry {
  uint64_t startTick = 0, intervals = 0, splats = 0, offset = 0, size = 0;
  // Of the chunk's own directory, which is stored with the chunk rather than here.
  // This is what carries the signature out to every chunk's page table without
  // downloading any of them up front.
  Digest directoryDigest{};
  // A conservative box holding every position in the chunk: cull or pick detail
  // without decoding anything. minXYZ then maxXYZ.
  std::array<float, 6> bounds{};
};
struct Header {
  // headerSize ends the tables; signedSize also covers the metadata block and is where
  // the signature block starts. Data begins after that, 16-byte aligned.
  uint64_t headerSize = 0, signedSize = 0, fileSize = 0;
  // One tick lasts timeNumerator / timeDenominator seconds.
  uint32_t timeNumerator = 1, timeDenominator = 30;
  uint64_t frameCount = 0, durationTicks = 0, maxSplatsPerFrame = 0,
           maxChunkSplatRecords = 0;
  // Where this capture starts on its own timeline, for syncing with other media.
  uint64_t startTick = 0;
  // When the file was written: milliseconds since the Unix epoch, UTC, so date and time
  // are one number with no timezone to misread. Inside the signed region, so it is the
  // capture's own statement about itself rather than a filesystem timestamp that any
  // copy would destroy. Also an input to the identifier, which is what separates two
  // exports of the same take.
  uint64_t createdMillis = 0;
  std::array<double, 6> bounds{};
  // shDegree 0..3; 0 means base colour only, with no higher-order SH layers.
  uint32_t shDegree = 3, shBasis = 1, coordinates = 1, pageRows = 65536;
  // How the attribute data is built: 0 is the scheme described here, the one
  // every file written so far uses. It is held apart from the per-attribute
  // codec on purpose — a different value would mean the arrays themselves are
  // formed differently (absolutes rather than deltas, say), so a reader that
  // does not know a value must refuse the file rather than misread it.
  uint32_t encoding = 0;
  std::vector<Policy> policies;
  std::vector<Layer> layers;
  std::vector<Extra> extras;
  std::vector<ChunkEntry> chunks;
  Metadata metadata;
  SignatureInfo signature;
};
struct Group {
  uint32_t type = 0,
           flags =
               0; // 0 dictionary, 1 splats; bit 0/1 direct position/rotation
  uint64_t splats = 0, intervals = 0, meshSamples = 0;
  std::array<uint64_t, 6>
      counts{}; // static SH, temporal SH, SH0, opacity, rotation, position
  double positionMin = 0, positionMax = 0, trajectoryMin = 0, trajectoryMax = 0;
};
struct Page {
  uint32_t attribute = 0, group = 0, layer = 0;
  uint64_t firstRow = 0, totalRows = 0, offset = 0, size = 0, decodedSize = 0;
  uint32_t crc = 0;
  Spec spec;
};
struct ChunkDirectory {
  uint64_t size = 0;
  std::vector<Group> groups;
  std::vector<Page> pages;
};
struct DecodedPage {
  Page descriptor;
  Bytes bytes;
};
struct DecodedChunk {
  std::vector<Group> groups;
  std::vector<DecodedPage> pages;
};
enum class Compression { Auto, None };
// How a file gets signed. The codec never holds key material: it is compiled into the
// WebAssembly decoder the browser downloads, so a private key reaching this library
// would be a private key reaching every viewer. The authoring side passes one of these
// in (see crypto/vgssign.h); a build without it cannot sign, which is the point.
struct Signer {
  uint32_t keyId = 0;
  std::function<Signature(const uint8_t *data, size_t size)> sign;
};

struct EncodeOptions {
  std::vector<ExtraInput> extras;
  Metadata metadata;
  Signer signer;
  uint64_t startTick = 0;
  uint32_t pageRows = 65536;
  // Highest spherical harmonic degree to keep, 0..3. The source carries degree 3;
  // writing less drops whole planes of three coefficients, which is where a third
  // of the file lives. Degree 2 keeps three planes (one coefficient slot spare),
  // degree 1 keeps one, degree 0 writes no higher-order layers at all.
  uint32_t shDegree = 3;
  bool verify = true;
  Compression compression = Compression::Auto;
};
using Progress = std::function<bool(int done, int total)>;

// Input is format-6 .mint. Output carries logical attributes only, no original
// metadata, padding or opaque residue. Unsupported source layouts are rejected.
Bytes encodeMint(const uint8_t *mint, size_t size, const EncodeOptions & = {},
                 const Progress & = {});
// What a reader says when a capture is not ours, or has been altered since it was
// made. Every signature failure reports the same thing on purpose: telling a caller
// which check failed only helps whoever is trying to get past them.
constexpr const char *InvalidCapture = "invalid 4dgs capture";

// How many bytes from the start a reader needs before it can decide whether a file is
// genuine: the signed region plus its signature block. Readable from the first
// FixedHeaderSize bytes alone, so a streaming reader makes one small request, then one
// exact request, and never fetches payload to validate. Throws on a malformed prefix.
uint64_t structuralSize(const uint8_t *data, size_t size);

// Parses and authenticates. The signature is checked against the trusted keys before
// any table is trusted, and anything wrong with it - missing, truncated, unknown
// algorithm or key, altered bytes - throws Error(InvalidCapture). The data must hold
// at least structuralSize() bytes; the payload need not be present at all.
Header readHeader(const uint8_t *data, size_t size);
// The bytes of one extra, given the whole file.
Bytes readExtra(const Extra &, const uint8_t *data, size_t size);
ChunkDirectory readChunkDirectory(const Header &, size_t chunk,
                                  const uint8_t *data, size_t size);
Bytes decodePage(const Header &, const Page &, const uint8_t *payload,
                 size_t size);
DecodedChunk decodeChunk(const Header &, size_t chunk, const uint8_t *data,
                         size_t size, uint32_t layerMask = 7);
void verifyMint(const uint8_t *vgs, size_t vgsSize, const uint8_t *mint,
                size_t mintSize);
uint32_t crc32(const uint8_t *, size_t);
const char *attributeName(uint32_t);
} // namespace vgs
#endif
