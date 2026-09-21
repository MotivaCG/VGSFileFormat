// The VFGS decoder as a WebAssembly module.
//
// Everything expensive is here: parsing the container, checking the signature, decoding
// chunks, evaluating a frame. JavaScript fetches bytes and decides what to fetch; it never
// looks inside a capture. That division is deliberate - a reader written in JavaScript
// would be a second implementation of the format to keep in step with this one, and the
// part of it that matters most, deciding whether a file is genuine, would be the part
// easiest to get subtly wrong.
//
// The interface is push-based because the web is. A browser cannot answer a synchronous
// read from inside WebAssembly, so instead of the decoder pulling bytes through a Source,
// JavaScript hands ranges in before asking for anything that needs them:
//
//     vgs_head_reserve(n)           a buffer to write the head into
//     vgs_structural_size()         how much of the head is actually needed
//     vgs_open(fileSize)            parses and authenticates; nothing else is believed
//     vgs_prime_reserve(offset, n)  a buffer for a range you are about to need
//     vgs_set_time(seconds)         decode the instant; frame pointers follow
//
// The reserve calls hand back the decoder's own buffer rather than taking bytes, so a
// fetched range is written into its final place once. Nothing is copied between the
// response and the decoder, and nothing between the decoder and the frame arrays a
// caller reads back.
//
// The library underneath is the same vgsdec::Capture a native program uses, driven through
// a Source that serves the ranges JavaScript has primed. There is no second code path for
// the browser, and no renderer's conventions anywhere in this file: what comes out is the
// frame arrays as they are, and a caller packs them however its renderer wants.

#include "vgsdecoder/vgsdecoder.h"

#include <emscripten/emscripten.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// Ranges JavaScript has handed over. Two are enough in practice - the structural region,
// which stays for the life of the capture, and whatever chunk is being decoded - so this
// keeps the head and one working range rather than growing a cache the caller cannot see
// the size of. A caller that wants more buffering does it on its own side, where it
// already decides what to fetch.
struct Range {
  uint64_t offset = 0;
  std::unique_ptr<uint8_t[]> bytes;
  size_t length = 0, capacity = 0;

  /**
   * A buffer of `size` bytes for the caller to overwrite completely.
   *
   * Deliberately not a std::vector: resizing one zeroes what it is about to hand over, and
   * the caller then writes over every byte of it. On a capture with 25 MB chunks that is a
   * wasted pass over 25 MB per chunk. The buffer also only ever grows, so after the first
   * chunk there is no allocation either.
   */
  uint8_t *reserve(size_t size) {
    if (size > capacity) {
      bytes.reset(new uint8_t[size]);
      capacity = size;
    }
    length = size;
    return bytes.get();
  }

  bool covers(uint64_t at, size_t size) const {
    return at >= offset && at - offset <= length && size <= length - (at - offset);
  }
};

class PrimedSource : public vgsdec::Source {
public:
  Range head, working;
  uint64_t total = 0;

  bool read(uint64_t offset, size_t size, uint8_t *into) override {
    if (const uint8_t *at = map(offset, size)) {
      std::memcpy(into, at, size);
      return true;
    }
    return false;
  }

  // The bytes are already in the module's heap, so the decoder is handed a pointer to
  // them. That is the whole reason this class exists: a chunk is tens of megabytes and
  // copying it again on the way into the decoder would be the most expensive thing the
  // module does.
  const uint8_t *map(uint64_t offset, size_t size) override {
    for (const Range *range : {&head, &working})
      if (range->covers(offset, size))
        return range->bytes.get() + (offset - range->offset);
    return nullptr;
  }

  uint64_t size() const override { return total; }
};

PrimedSource source;
std::unique_ptr<vgsdec::Capture> capture;
std::string error;
std::string text; // whatever string getter was called last

// A single staging buffer, so JavaScript writes bytes straight into the module's heap
// instead of allocating a copy per range on its side.
std::vector<uint8_t> staging;

const float *positions = nullptr; // what vgs_positions_at last produced

int fail(const std::exception &e) {
  error = e.what();
  return -1;
}

const char *hand(const std::string &value) {
  text = value;
  return text.c_str();
}

} // namespace

extern "C" {

// ---- errors and staging ---------------------------------------------------------

EMSCRIPTEN_KEEPALIVE const char *vgs_error() { return error.c_str(); }

/** Where vgs_extra leaves what it fetched. */
EMSCRIPTEN_KEEPALIVE const uint8_t *vgs_staging() { return staging.data(); }

// ---- opening --------------------------------------------------------------------

/**
 * A buffer of `size` bytes at the start of the file for the caller to fill. Called twice:
 * once for the fixed header, then again for the structural region it turns out to need.
 * Growing it keeps what was already written, so the second call only has to fetch the
 * rest.
 */
EMSCRIPTEN_KEEPALIVE uint8_t *vgs_head_reserve(int size) {
  try {
    if (size < 0)
      return nullptr;
    capture.reset();
    source.head.offset = 0;
    return source.head.reserve(size_t(size));
  } catch (const std::exception &e) {
    fail(e);
    return nullptr;
  }
}

/**
 * How many bytes from the start authenticating this file will need, from whatever is in
 * the head buffer. Negative when those bytes are not a readable header.
 */
EMSCRIPTEN_KEEPALIVE double vgs_structural_size() {
  try {
    return double(
        vgsdec::Capture::structuralSize(source.head.bytes.get(), source.head.length));
  } catch (const std::exception &e) {
    return fail(e);
  }
}

/**
 * Parses and authenticates from the structural region. `total` is the file's full size,
 * which the caller knows from the response. Returns 0, or -1 with vgs_error() reading
 * exactly "invalid 4dgs capture" when the file is not one of ours.
 */
EMSCRIPTEN_KEEPALIVE int vgs_open(double total) {
  try {
    capture.reset();
    source.working.length = 0;
    if (!(total >= 0))
      throw vgsdec::Error("invalid capture size");
    source.total = uint64_t(total);
    capture.reset(new vgsdec::Capture(vgsdec::Capture::openStream(source)));
    error.clear();
    return 0;
  } catch (const std::exception &e) {
    capture.reset();
    return fail(e);
  }
}

EMSCRIPTEN_KEEPALIVE void vgs_close() {
  capture.reset();
  source.head = Range();
  source.working = Range();
  source.total = 0;
  staging.clear();
  staging.shrink_to_fit();
  positions = nullptr;
}

/**
 * A buffer for a range the decoder is about to need, replacing the previous one. The
 * caller writes the bytes for [offset, offset + size) into it; nothing copies them again.
 */
EMSCRIPTEN_KEEPALIVE uint8_t *vgs_prime_reserve(double offset, int size) {
  try {
    if (size < 0 || !(offset >= 0))
      throw vgsdec::Error("invalid range");
    source.working.offset = uint64_t(offset);
    return source.working.reserve(size_t(size));
  } catch (const std::exception &e) {
    fail(e);
    return nullptr;
  }
}

// ---- what the capture says about itself -----------------------------------------
//
// The strings come back one at a time through a single buffer, so a caller reads one,
// copies it out, then asks for the next. Field numbers rather than names keeps the call
// a plain integer across the boundary.

enum Field {
  FieldId = 0,
  FieldTitle,
  FieldAuthor,
  FieldProject,
  FieldTake,
  FieldStudio,
  FieldCopyright,
  FieldSoftware,
  FieldSoftwareVersion,
  FieldUuid
};

EMSCRIPTEN_KEEPALIVE const char *vgs_string(int field) {
  if (!capture)
    return "";
  const vgsdec::Metadata &m = capture->metadata();
  switch (field) {
  case FieldId: return hand(m.id);
  case FieldTitle: return hand(m.title);
  case FieldAuthor: return hand(m.author);
  case FieldProject: return hand(m.projectName);
  case FieldTake: return hand(m.takeName);
  case FieldStudio: return hand(m.captureStudio);
  case FieldCopyright: return hand(m.copyright);
  case FieldSoftware: return hand(m.softwareName);
  case FieldSoftwareVersion: return hand(m.softwareVersion);
  case FieldUuid: return hand(capture->uuidText());
  default: return "";
  }
}

EMSCRIPTEN_KEEPALIVE int vgs_tag_count() {
  return capture ? int(capture->metadata().tags.size()) : 0;
}

EMSCRIPTEN_KEEPALIVE const char *vgs_tag(int index) {
  if (!capture)
    return "";
  const auto &tags = capture->metadata().tags;
  return index >= 0 && size_t(index) < tags.size() ? hand(tags[size_t(index)]) : "";
}

// Numbers come back as doubles: every quantity in a capture fits exactly in one, and it
// saves the caller reassembling 64-bit values from pairs of 32-bit ones.
enum Number {
  NumberDuration = 0,
  NumberFrameCount,
  NumberFrameRate,
  NumberStartSeconds,
  NumberShDegree,
  NumberMaxSplatsPerFrame,
  NumberFileSize,
  NumberCreatedMillis,
  NumberChunkCount,
  NumberSignatureKeyId,
  NumberSignatureAlgorithm,
  NumberSignedBytes,
  NumberVersion,
  NumberIsPlain
};

EMSCRIPTEN_KEEPALIVE double vgs_number(int which) {
  if (!capture)
    return 0;
  switch (which) {
  case NumberDuration: return capture->duration();
  case NumberFrameCount: return double(capture->frameCount());
  case NumberFrameRate: return capture->frameRate();
  case NumberStartSeconds: return capture->startSeconds();
  case NumberShDegree: return capture->shDegree();
  case NumberMaxSplatsPerFrame: return double(capture->maxSplatsPerFrame());
  case NumberFileSize: return double(capture->fileSize());
  case NumberCreatedMillis: return double(capture->createdMillis());
  case NumberChunkCount: return double(capture->chunkCount());
  case NumberSignatureKeyId: return capture->signature().keyId;
  case NumberSignatureAlgorithm: return capture->signature().algorithm;
  case NumberSignedBytes: return double(capture->signature().signedBytes);
  case NumberVersion: return capture->version();
  case NumberIsPlain: return capture->isPlain() ? 1 : 0;
  default: return 0;
  }
}

/** The capture's own box over every frame, six doubles: minXYZ then maxXYZ. */
EMSCRIPTEN_KEEPALIVE const double *vgs_bounds() {
  return capture ? capture->bounds() : nullptr;
}

// ---- the chunk table, which is what a fetch policy runs on ----------------------

enum ChunkField {
  ChunkOffset = 0,
  ChunkSize,
  ChunkStartSeconds,
  ChunkEndSeconds,
  ChunkSplats
};

EMSCRIPTEN_KEEPALIVE double vgs_chunk(int index, int field) {
  try {
    if (!capture)
      throw vgsdec::Error("no capture open");
    const vgsdec::ChunkInfo &info = capture->chunk(size_t(index));
    switch (field) {
    case ChunkOffset: return double(info.offset);
    case ChunkSize: return double(info.size);
    case ChunkStartSeconds: return info.startSeconds;
    case ChunkEndSeconds: return info.endSeconds;
    case ChunkSplats: return double(info.splats);
    default: return -1;
    }
  } catch (const std::exception &e) {
    return fail(e);
  }
}

/** Which chunk holds a time, or -1 when none does. */
EMSCRIPTEN_KEEPALIVE int vgs_chunk_at(double seconds) {
  if (!capture)
    return -1;
  const size_t index = capture->chunkAt(seconds);
  return index < capture->chunkCount() ? int(index) : -1;
}

/** The chunk's bounding box, six floats, without decoding it. */
EMSCRIPTEN_KEEPALIVE const float *vgs_chunk_bounds(int index) {
  try {
    return capture ? capture->chunk(size_t(index)).bounds : nullptr;
  } catch (const std::exception &e) {
    fail(e);
    return nullptr;
  }
}

// ---- playback -------------------------------------------------------------------

/**
 * Decodes the instant at `seconds`. The range holding that chunk must have been primed;
 * it has not been if this returns -1 and vgs_error() says the read failed. Returns the
 * number of splat records in the frame.
 */
EMSCRIPTEN_KEEPALIVE int vgs_set_time(double seconds, int includeSh) {
  try {
    if (!capture)
      throw vgsdec::Error("no capture open");
    error.clear();
    return int(capture->setTime(seconds, includeSh != 0).splatCount);
  } catch (const std::exception &e) {
    return fail(e);
  }
}

/**
 * Decodes a chunk a little at a time, so nothing blocks for long. Returns 1 when the chunk
 * is ready, 0 when there is more to do, -1 on failure. The chunk's range must have been
 * primed before the first call; it is copied in then, so the caller is free to prime
 * something else afterwards.
 */
EMSCRIPTEN_KEEPALIVE int vgs_prepare(int chunkIndex, double budgetMilliseconds,
                                     int includeSh) {
  try {
    if (!capture || chunkIndex < 0)
      throw vgsdec::Error("no capture open");
    error.clear();
    return capture->prepare(size_t(chunkIndex), budgetMilliseconds, includeSh != 0) ? 1 : 0;
  } catch (const std::exception &e) {
    return fail(e);
  }
}

/** How far the chunk being prepared has got, 0 to 1. */
EMSCRIPTEN_KEEPALIVE double vgs_prepared_fraction() {
  return capture ? capture->preparedFraction() : 0;
}

enum Attribute {
  AttributePositions = 0,
  AttributeRotations,
  AttributeScales,
  AttributeOpacities,
  AttributeColors,
  AttributeSphericalHarmonics,
  AttributeActive
};

/**
 * A pointer into the module's heap for one of the frame's arrays, valid until the next
 * vgs_set_time. The caller wraps it in a typed array; nothing is copied.
 */
EMSCRIPTEN_KEEPALIVE const void *vgs_frame(int attribute) {
  if (!capture)
    return nullptr;
  const vgsdec::Frame &frame = capture->frame();
  switch (attribute) {
  case AttributePositions: return frame.positions;
  case AttributeRotations: return frame.rotations;
  case AttributeScales: return frame.scales;
  case AttributeOpacities: return frame.opacities;
  case AttributeColors: return frame.colors;
  case AttributeSphericalHarmonics: return frame.sphericalHarmonics;
  case AttributeActive: return frame.active;
  default: return nullptr;
  }
}

/** Coefficients per splat in the spherical harmonic array: 0, 3, 8 or 15. */
EMSCRIPTEN_KEEPALIVE int vgs_frame_sh_coefficients() {
  return capture ? capture->frame().shCoefficients : 0;
}

/**
 * Positions alone for an instant, for a caller that evaluates everything else on the GPU
 * but sorts splats by depth on the CPU. Returns the splat count; the pointer follows from
 * vgs_frame_positions_data. Shares the decoded chunk with vgs_set_time, so calling both
 * for the same instant decodes once.
 */
EMSCRIPTEN_KEEPALIVE int vgs_positions_at(double seconds) {
  try {
    if (!capture)
      throw vgsdec::Error("no capture open");
    error.clear();
    uint64_t count = 0;
    positions = capture->positionsAt(seconds, &count);
    return int(count);
  } catch (const std::exception &e) {
    positions = nullptr;
    return fail(e);
  }
}

EMSCRIPTEN_KEEPALIVE const void *vgs_positions_data() { return positions; }

EMSCRIPTEN_KEEPALIVE int vgs_frame_chunk() {
  return capture ? int(capture->frame().chunkIndex) : -1;
}

// ---- what playback keeps in memory ----------------------------------------------
//
// Decoded chunks, not the ranges that were primed: a range is only needed until the chunk
// is decoded, while the decoded form is what a player steps back into. The rule is the one
// on vgsdec::CachePolicy - the two limits are the shape, their sum plus one is the budget,
// and nothing is evicted while the total fits.
//
// A caller that keeps more than one chunk has to ask vgs_is_chunk_cached before deciding
// to fetch, or it will download chunks the decoder already holds.

EMSCRIPTEN_KEEPALIVE void vgs_set_cache_policy(int behind, int ahead, double maxBytes) {
  if (!capture)
    return;
  vgsdec::CachePolicy policy;
  policy.behind = behind > 0 ? size_t(behind) : 0;
  policy.ahead = ahead > 0 ? size_t(ahead) : 0;
  policy.maxBytes = maxBytes > 0 ? uint64_t(maxBytes) : 0;
  capture->setCachePolicy(policy);
}

/** 1 when this chunk is decoded right now, so the caller knows not to fetch it again. */
EMSCRIPTEN_KEEPALIVE int vgs_is_chunk_cached(int index) {
  return capture && index >= 0 && capture->isChunkCached(size_t(index)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int vgs_cached_chunk_count() {
  return capture ? int(capture->cachedChunkCount()) : 0;
}

/** Roughly how much the decoded chunks are holding. */
EMSCRIPTEN_KEEPALIVE double vgs_cached_bytes() {
  return capture ? double(capture->cachedBytes()) : 0;
}

/** Drops every decoded chunk, for a caller that has seeked away and wants the memory. */
EMSCRIPTEN_KEEPALIVE void vgs_release_cache() {
  if (capture)
    capture->releaseCache();
}

// ---- handing a chunk to a shader ------------------------------------------------
//
// In packed mode the decoder stops after decompressing and the caller's shader does the
// evaluation. Describing a chunk means a lot of small numbers, and crossing into
// WebAssembly for each of them would be the most frequent call in a player for no reason,
// so the whole description is written into one array of doubles and read back in one go.
//
// Doubles throughout: every quantity here, pointers included, is exact in one, and a
// single typed array on the other side is simpler than four of different widths.
//
//   [0] sampleCount   [1] groupCount   [2] bufferCount   [3] totalBytes
//   then groupCount groups of GroupStride:
//     type, flags, splats, intervals, positionMin, positionMax, trajectoryMin,
//     trajectoryMax
//   then bufferCount buffers of BufferStride:
//     attribute, group, layer, firstRow, rows, totalRows, kind, width, address, size
//
// `address` is a byte offset into the module's heap, so the caller wraps it directly.

namespace {
constexpr int LayoutHeader = 4;
constexpr int GroupStride = 14;
constexpr int BufferStride = 10;
std::vector<double> layout;
double instant[4] = {};
} // namespace

/** Floats (0) or packed (1). Changing it drops what is decoded, which was built for the
 *  other one. */
EMSCRIPTEN_KEEPALIVE int vgs_set_output(int packed) {
  try {
    if (!capture)
      throw vgsdec::Error("no capture open");
    capture->setOutput(packed ? vgsdec::Output::Packed : vgsdec::Output::Floats);
    return 0;
  } catch (const std::exception &e) {
    return fail(e);
  }
}

EMSCRIPTEN_KEEPALIVE int vgs_output() {
  return capture && capture->output() == vgsdec::Output::Packed ? 1 : 0;
}

/**
 * Everything about a prepared chunk, in one array. Null when that chunk is not prepared
 * or the capture is not in packed mode. Valid until the chunk is evicted or this is
 * called for another chunk.
 */
EMSCRIPTEN_KEEPALIVE const double *vgs_chunk_layout(int chunkIndex) {
  try {
    if (!capture || chunkIndex < 0)
      throw vgsdec::Error("no capture open");
    const vgsdec::ChunkData &data = capture->chunkData(size_t(chunkIndex));

    layout.assign(size_t(LayoutHeader + GroupStride * int(data.groupCount) +
                         BufferStride * int(data.bufferCount)),
                  0.0);
    layout[0] = double(data.sampleCount);
    layout[1] = double(data.groupCount);
    layout[2] = double(data.bufferCount);
    layout[3] = double(data.totalBytes);

    size_t at = LayoutHeader;
    for (size_t i = 0; i < data.groupCount; ++i, at += GroupStride) {
      const vgsdec::GroupData &group = data.groups[i];
      layout[at + 0] = group.type;
      layout[at + 1] = group.flags;
      layout[at + 2] = double(group.splats);
      layout[at + 3] = double(group.intervals);
      layout[at + 4] = group.positionMin;
      layout[at + 5] = group.positionMax;
      layout[at + 6] = group.trajectoryMin;
      layout[at + 7] = group.trajectoryMax;
      for (size_t k = 0; k < 6; ++k)
        layout[at + 8 + k] = double(group.counts[k]);
    }
    for (size_t i = 0; i < data.bufferCount; ++i, at += BufferStride) {
      const vgsdec::Buffer &buffer = data.buffers[i];
      layout[at + 0] = buffer.attribute;
      layout[at + 1] = buffer.group;
      layout[at + 2] = buffer.layer;
      layout[at + 3] = double(buffer.firstRow);
      layout[at + 4] = double(buffer.rows);
      layout[at + 5] = double(buffer.totalRows);
      layout[at + 6] = buffer.kind;
      layout[at + 7] = buffer.width;
      layout[at + 8] = double(reinterpret_cast<uintptr_t>(buffer.data));
      layout[at + 9] = double(buffer.size);
    }
    error.clear();
    return layout.data();
  } catch (const std::exception &e) {
    fail(e);
    return nullptr;
  }
}

/** Where a time falls: four doubles, chunkIndex, sampleA, sampleB, alpha. */
EMSCRIPTEN_KEEPALIVE const double *vgs_instant(double seconds) {
  try {
    if (!capture)
      throw vgsdec::Error("no capture open");
    const vgsdec::Instant at = capture->instantAt(seconds);
    instant[0] = double(at.chunkIndex);
    instant[1] = at.sampleA;
    instant[2] = at.sampleB;
    instant[3] = at.alpha;
    error.clear();
    return instant;
  } catch (const std::exception &e) {
    fail(e);
    return nullptr;
  }
}

// ---- payloads carried alongside -------------------------------------------------
//
// Same four payloads the C++ API names, with one addition: the caller has to fetch and
// prime the range first, because a browser cannot answer a synchronous read. So each one
// says where it is, and then hands the bytes over once they are within reach.

enum PayloadKind { PayloadAudio = 0, PayloadThumbnail, PayloadMetadataJson, PayloadMetadataJson2 };

namespace {
vgsdec::Capture::Payload payloadOf(int kind) {
  switch (kind) {
  case PayloadThumbnail: return vgsdec::Capture::Payload::Thumbnail;
  case PayloadMetadataJson: return vgsdec::Capture::Payload::MetadataJson;
  case PayloadMetadataJson2: return vgsdec::Capture::Payload::MetadataJson2;
  default: return vgsdec::Capture::Payload::Audio;
  }
}
} // namespace

/** 1 when the capture carries this payload. */
EMSCRIPTEN_KEEPALIVE int vgs_has_payload(int kind) {
  if (!capture)
    return 0;
  switch (kind) {
  case PayloadAudio: return capture->hasAudio() ? 1 : 0;
  case PayloadThumbnail: return capture->hasThumbnail() ? 1 : 0;
  case PayloadMetadataJson: return capture->hasMetadataJson() ? 1 : 0;
  case PayloadMetadataJson2: return capture->hasMetadataJson2() ? 1 : 0;
  default: return 0;
  }
}

/** Its declared format: an audio or image kind, 0 for the JSON blocks. */
EMSCRIPTEN_KEEPALIVE int vgs_payload_format(int kind) {
  if (!capture)
    return 0;
  if (kind == PayloadAudio)
    return int(capture->audioFormat());
  if (kind == PayloadThumbnail)
    return int(capture->thumbnailFormat());
  return 0;
}

/** Where it sits in the file, so the caller can fetch that range and prime it. */
EMSCRIPTEN_KEEPALIVE double vgs_payload_offset(int kind) {
  return capture ? double(capture->locate(payloadOf(kind)).offset) : 0;
}

EMSCRIPTEN_KEEPALIVE double vgs_payload_size(int kind) {
  return capture ? double(capture->locate(payloadOf(kind)).size) : 0;
}

/**
 * Checks the payload against the signed table and leaves it in the staging buffer, whose
 * address vgs_staging() then returns. Its range must have been primed. Returns the size,
 * or -1.
 */
EMSCRIPTEN_KEEPALIVE int vgs_payload(int kind) {
  try {
    if (!capture)
      throw vgsdec::Error("no capture open");
    switch (kind) {
    case PayloadAudio: staging = capture->audio(); break;
    case PayloadThumbnail: staging = capture->thumbnail(); break;
    case PayloadMetadataJson: {
      const std::string json = capture->metadataJson();
      staging.assign(json.begin(), json.end());
      break;
    }
    default: {
      const std::string json = capture->metadataJson2();
      staging.assign(json.begin(), json.end());
      break;
    }
    }
    return int(staging.size());
  } catch (const std::exception &e) {
    return fail(e);
  }
}

} // extern "C"
