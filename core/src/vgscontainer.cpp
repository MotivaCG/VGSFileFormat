// Reading VFGS captures: the container half both the decoder and the encoder need.
//
// Everything here is a reader. The writer is in vgsencode.cpp, which a decoder build does
// not compile, so a library built for playback cannot produce a capture and carries no
// authoring code.

#include "vgsinternal.h"
#include "vgspublickey.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace vgs {
using namespace detail;
const char *attributeName(uint32_t id) {
  static const char *shared[] = {"scale_lut",
                                 "sh_static_codebooks",
                                 "sh_temporal_codebooks",
                                 "sh0_trajectories",
                                 "sh0_base_lut",
                                 "opacity_trajectories",
                                 "rotation_initial",
                                 "rotation_delta_lut",
                                 "rotation_delta_indices",
                                 "position_trajectories",
                                 "mesh_extent_lut"};
  static const char *group[] = {"scale_indices",
                                "position_samples",
                                "sh_static_indices",
                                "sh_temporal_indices",
                                "lifetimes",
                                "rotation_samples",
                                "sh0_rq_indices",
                                "sh0_base_indices",
                                "opacity_rq_indices",
                                "rotation_base",
                                "rotation_rq_indices",
                                "rotation_rank_boundaries",
                                "position_base",
                                "position_rq_coefficients",
                                "position_rank_boundaries"};
  if (id >= 1 && id <= 11)
    return shared[id - 1];
  if (id >= 32 && id <= 46)
    return group[id - 32];
  return "unknown";
}
uint32_t crc32(const uint8_t *p, size_t n) {
  static const auto table = []() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      auto c = i;
      for (int k = 0; k < 8; ++k)
        c = (c >> 1) ^ (0xedb88320u & uint32_t(-int32_t(c & 1)));
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = ~0u;
  for (size_t i = 0; i < n; ++i)
    c = table[(c ^ p[i]) & 255] ^ (c >> 8);
  return ~c;
}
namespace {
// An upper bound for the structural region, so a malformed prefix cannot make a reader
// fetch or allocate something absurd before anything has been authenticated. Comfortably
// above a real file: a million chunks of table plus the largest metadata block.
constexpr uint64_t MaxStructuralSize = 128ull * 1024 * 1024;

// Checks the signature block before anything past the fixed header is believed. Every
// failure reports the same thing: which check failed is of no use to a caller and of
// some use to whoever is trying to get past them.
void authenticate(const uint8_t *data, size_t size, Header &h) {
  // The sizes come from bytes nobody has vouched for yet, so they are bounded here
  // rather than trusted: everything below indexes with them.
  if (h.signedSize < FixedHeaderSize || h.signedSize > MaxStructuralSize ||
      h.signedSize < h.headerSize ||
      h.signedSize - h.headerSize > MaxMetadataSize ||
      h.signedSize + SignatureBlockSize > size)
    throw Error(InvalidCapture);
  R r(data + h.signedSize, SignatureBlockSize);
  SignatureInfo info;
  info.algorithm = r.u32();
  info.keyId = r.u32();
  const uint32_t length = r.u32();
  const uint32_t reserved = r.u32();
  r.need(SignatureSize);
  for (auto &b : info.signature)
    b = r.p[r.at++];
  const PublicKey *key = trustedKey(info.keyId);
  if (info.algorithm != Ed25519Signature || length != SignatureSize || reserved || !key)
    throw Error(InvalidCapture);
  if (!verify(data, size_t(h.signedSize), info.signature, *key))
    throw Error(InvalidCapture);
  h.signature = info;
}
} // namespace

uint64_t structuralSize(const uint8_t *data, size_t size) {
  R r(data, size);
  if (r.u32() != Magic)
    throw Error("not a VFGS file");
  if (r.u32() != Version)
    throw Error("unsupported VGS version");
  const uint64_t headerSize = r.u64();
  const uint64_t signedSize = r.u64();
  if (headerSize < FixedHeaderSize || signedSize < headerSize ||
      signedSize - headerSize > MaxMetadataSize || signedSize > MaxStructuralSize)
    throw Error("unsupported or malformed VGS header");
  return add(signedSize, SignatureBlockSize);
}

Header readHeader(const uint8_t *data, size_t size) {
  R r(data, size);
  if (r.u32() != Magic)
    throw Error("not a VFGS file");
  if (r.u32() != Version)
    throw Error("unsupported VGS version");
  Header h;
  h.headerSize = r.u64();
  h.signedSize = r.u64();
  h.fileSize = r.u64();
  h.timeNumerator = r.u32();
  h.timeDenominator = r.u32();
  h.frameCount = r.u64();
  h.durationTicks = r.u64();
  h.maxSplatsPerFrame = r.u64();
  h.maxChunkSplatRecords = r.u64();
  for (auto &x : h.bounds)
    x = r.f64();
  h.shDegree = r.u32();
  h.shBasis = r.u32();
  h.coordinates = r.u32();
  h.encoding = r.u32();
  auto np = r.u32(), nc = r.u32();
  h.pageRows = r.u32();
  auto ne = r.u32(), nl = r.u32();
  h.startTick = r.u64();
  const uint32_t metadataBytes = r.u32();
  h.createdMillis = r.u64();
  // Before anything is checked for sense, the structure is checked for provenance: an
  // edited header must answer "invalid 4dgs capture" whether or not the edit also broke
  // an invariant, and a reader has no business explaining which field looked wrong in a
  // capture it cannot vouch for.
  authenticate(data, size, h);
  // A tick is timeNumerator / timeDenominator seconds: 1/30 here, but 1/24, 1/25,
  // 1/50, 1/60 or 1001/30000 are all expressible.
  const double rate = h.timeNumerator ? double(h.timeDenominator) / h.timeNumerator : 0;
  if (h.encoding || !np || np > 64 || !nc || nc > 1000000 || ne > 64 ||
      !nl || nl > 64 ||
      h.headerSize != tablesSize(np, nl, ne, nc) ||
      metadataBytes > MaxMetadataSize ||
      h.signedSize != add(h.headerSize, metadataBytes) ||
      add(h.signedSize, SignatureBlockSize) > h.fileSize || !h.timeNumerator || !h.timeDenominator ||
      rate < 1 || rate > 1000 || h.shDegree > 3 || h.shBasis != 1 ||
      h.coordinates != 1 || !h.frameCount || h.frameCount != h.durationTicks ||
      h.maxSplatsPerFrame > h.maxChunkSplatRecords || h.pageRows < 1024 ||
      h.pageRows > 1048576 || h.startTick > UINT64_MAX - h.durationTicks)
    throw Error("unsupported or malformed VGS header");
  for (int i = 0; i < 3; ++i)
    if (h.bounds[i] > h.bounds[i + 3])
      throw Error("invalid VGS bounds");
  r.need(h.headerSize - r.at);
  std::set<uint32_t> seen;
  for (uint32_t i = 0; i < np; ++i) {
    Policy p;
    p.attribute = r.u32();
    p.codec = r.u32();
    p.model = r.u32();
    p.family = r.u32();
    p.flags = r.u32();
    const bool optional = (p.flags & OptionalAttribute) != 0;
    // An unknown attribute is only acceptable when its policy says a reader may skip
    // it; anything else would mean silently dropping data a frame needs.
    if (r.u32() || p.flags > OptionalAttribute ||
        (std::string(attributeName(p.attribute)) == "unknown" &&
         (!optional || p.attribute < VendorAttributeBase)) ||
        !seen.insert(p.attribute).second || p.codec > Rans || p.family > 9 ||
        p.model > (p.family == 9   ? 3u
                   : p.family == 0 ? 0u
                                   : 1u) ||
        (p.codec == Raw && p.model))
      throw Error("invalid VGS policy");
    h.policies.push_back(p);
  }
  std::set<uint32_t> layerIds;
  for (uint32_t i = 0; i < nl; ++i) {
    Layer l;
    l.id = r.u32();
    l.kind = r.u32();
    l.dependsOn = r.u32();
    l.flags = r.u32();
    const bool base = h.layers.empty();
    if (l.flags || (base && (l.id || l.kind != BaseLayer || l.dependsOn)) ||
        (!base && (l.id <= h.layers.back().id || !layerIds.count(l.dependsOn))) ||
        (l.kind > TemporalShLayer && l.kind < VendorLayerBase) ||
        (h.shDegree == 0 && l.kind != BaseLayer))
      throw Error("invalid VGS layer");
    layerIds.insert(l.id);
    h.layers.push_back(l);
  }
  uint64_t at = add(h.signedSize, SignatureBlockSize);
  for (uint32_t i = 0; i < ne; ++i) {
    Extra e;
    e.type = r.u32();
    e.format = r.u32();
    e.offset = r.u64();
    e.size = r.u64();
    e.digest = r.digest();
    const bool known = e.type == MetadataExtra || e.type == ThumbnailExtra ||
                       e.type == AudioExtra || e.type == MetadataExtra2;
    if (!e.type || !e.size || e.size > Limit || e.offset != aligned(at) ||
        ((e.type == MetadataExtra || e.type == MetadataExtra2) &&
         e.format != JsonUtf8) ||
        (e.type == ThumbnailExtra && (!e.format || e.format > Webp)) ||
        (e.type == AudioExtra && (!e.format || e.format > Wav)) ||
        (!known && e.type < VendorExtraBase))
      throw Error("invalid VGS extra");
    at = add(e.offset, e.size);
    if (at > h.fileSize)
      throw Error("VGS extra outside file");
    h.extras.push_back(e);
  }
  uint64_t tick = 0, maxRecords = 0;
  for (uint32_t i = 0; i < nc; ++i) {
    ChunkEntry c;
    c.startTick = r.u64();
    c.intervals = r.u64();
    c.splats = r.u64();
    c.offset = r.u64();
    c.size = r.u64();
    c.directoryDigest = r.digest();
    for (auto &v : c.bounds)
      v = r.f32();
    for (int k = 0; k < 3; ++k)
      if (!(c.bounds[k] <= c.bounds[k + 3]))
        throw Error("invalid VGS chunk bounds");
    if (c.startTick != tick || c.offset != aligned(at) ||
        c.size < 16 || !c.intervals || c.intervals > 255 ||
        c.splats > h.maxChunkSplatRecords)
      throw Error("invalid VGS chunk index");
    at = add(c.offset, c.size);
    tick = add(tick, c.intervals);
    maxRecords = std::max(maxRecords, c.splats);
    h.chunks.push_back(c);
  }
  if (at != h.fileSize || tick != h.durationTicks ||
      maxRecords != h.maxChunkSplatRecords)
    throw Error("inconsistent VGS totals");
  if (r.at != h.headerSize)
    throw Error("inconsistent VGS header size");
  h.metadata = readMetadata(r);
  if (r.at != h.signedSize)
    throw Error("inconsistent VGS metadata size");
  return h;
}
ChunkDirectory readChunkDirectory(const Header &h, size_t index,
                                  const uint8_t *data, size_t size) {
  if (index >= h.chunks.size())
    throw Error("VGS chunk index outside file");
  auto c = h.chunks[index];
  R r(data, size);
  if (r.u32() != ChunkMagic)
    throw Error("invalid VGS chunk magic");
  auto ng = r.u32(), np = r.u32(), ds = r.u32();
  if (!ng || ng > 1024 || !np || np > 1000000 || ds < 16 || ds > c.size ||
      ds > size || uint64_t(ng) * 128 + uint64_t(np) * 112 > ds - 16)
    throw Error("invalid VGS directory size");
  // The table this digest came from was signed, so a directory that does not match it
  // is not a damaged file, it is a different one.
  if (digest(data, ds) != c.directoryDigest)
    throw Error(InvalidCapture);
  r.n = ds;
  ChunkDirectory d;
  d.size = ds;
  uint64_t splats = 0;
  for (uint32_t i = 0; i < ng; ++i) {
    auto g = readGroup(r);
    if (g.intervals != c.intervals || g.type != (i ? 1u : 0u))
      throw Error("invalid VGS group order");
    splats = add(splats, g.splats);
    d.groups.push_back(g);
  }
  if (splats != c.splats)
    throw Error("VGS splat count mismatch");
  uint64_t at = ds;
  uint32_t lastLayer = 0;
  std::map<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>>
      coverage;
  for (uint32_t i = 0; i < np; ++i) {
    Page p;
    p.attribute = r.u32();
    p.group = r.u32();
    p.layer = r.u32();
    p.crc = r.u32();
    p.firstRow = r.u64();
    p.totalRows = r.u64();
    p.offset = r.u64();
    p.size = r.u64();
    p.decodedSize = r.u64();
    p.spec = readSpec(r);
    const auto &pol = policy(h, p.attribute);
    const bool known = std::string(attributeName(p.attribute)) != "unknown";
    if (p.group >= ng || (known && p.layer != layer(p.attribute)) ||
        !declaresLayer(h, p.layer) || p.layer < lastLayer || !p.size ||
        p.size > Limit ||
        p.decodedSize != mgs::attributeSize(p.spec) ||
        p.offset != aligned(at) || p.spec.family != pol.family ||
        p.firstRow > p.totalRows ||
        p.spec.rows > p.totalRows - p.firstRow ||
        (pol.codec == Raw && p.size != p.decodedSize) ||
        ((p.attribute < 32) != (p.group == 0)))
      throw Error("invalid VGS page");
    auto key = std::make_pair(p.group, p.attribute);
    auto &cov = coverage[key];
    if (p.firstRow != cov.first || (cov.second && cov.second != p.totalRows))
      throw Error("VGS page coverage mismatch");
    cov.first = add(cov.first, p.spec.rows);
    cov.second = p.totalRows;
    at = add(p.offset, p.size);
    lastLayer = p.layer;
    d.pages.push_back(std::move(p));
  }
  for (auto kv : coverage)
    if (kv.second.first != kv.second.second)
      throw Error("incomplete VGS attribute");
  if (at != c.size || r.at != ds)
    throw Error("VGS chunk size mismatch");
  return d;
}
Bytes decodePage(const Header &h, const Page &p, const uint8_t *data,
                 size_t size) {
  if (size != p.size || crc32(data, size) != p.crc)
    throw Error("VGS page checksum mismatch");
  const auto &pol = policy(h, p.attribute);
  if (p.spec.family != pol.family ||
      p.decodedSize != mgs::attributeSize(p.spec))
    throw Error("invalid VGS page descriptor");
  if (pol.codec == Raw) {
    if (size != p.decodedSize)
      throw Error("invalid raw VGS page");
    return Bytes(data, data + size);
  }
  return mgs::decodeAttribute(p.spec, int(pol.model), data, size);
}
DecodedChunk decodeChunk(const Header &h, size_t index, const uint8_t *data,
                         size_t size, uint32_t mask,
                         const std::function<bool(const Page &)> &keep) {
  if (index >= h.chunks.size() || size != h.chunks[index].size || mask > 7)
    throw Error("invalid VGS chunk input");
  auto d = readChunkDirectory(h, index, data, size);
  DecodedChunk out;
  out.groups = std::move(d.groups);
  for (const auto &p : d.pages)
    if ((mask & (1u << p.layer)) && (!keep || keep(p)))
      out.pages.push_back(
          {p, decodePage(h, p, data + p.offset, size_t(p.size))});
  return out;
}
// Replaces a chunk's stored box with the extent its live splats actually occupy,
Bytes readExtra(const Extra &e, const uint8_t *data, size_t size) {
  if (e.offset > size || e.size > size - e.offset)
    throw Error("VGS extra outside file");
  return Bytes(data + e.offset, data + e.offset + e.size);
}
} // namespace vgs
