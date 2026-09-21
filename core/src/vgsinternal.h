#ifndef VGSINTERNAL_H
#define VGSINTERNAL_H

// Byte-level internals of the VFGS container, shared by the reader and the writer.
//
// This header is private to the codec: it is not installed and no consumer includes it.
// It exists because the container is read in one translation unit and written in another,
// so that a decoder build can leave the writer out entirely. What lives here is what both
// halves need - the reader and writer cursors, the overflow-checked arithmetic, and the
// table entries whose layout both sides must agree on to the byte.

#include "mgscodec.h"
#include "vgscodec.h"
#include <cstring>
#include <string>

namespace vgs {
namespace detail {
constexpr uint32_t ChunkMagic = 0x4b484356; // VCHK
constexpr uint64_t Limit = uint64_t(1) << 30;
inline uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a)
    throw Error("VGS integer overflow");
  return a + b;
}
inline uint64_t mul(uint64_t a, uint64_t b) {
  if (a && b > UINT64_MAX / a)
    throw Error("VGS integer overflow");
  return a * b;
}
struct W {
  Bytes b;
  void u32(uint32_t x) {
    for (int i = 0; i < 4; ++i)
      b.push_back(uint8_t(x >> (8 * i)));
  }
  void u64(uint64_t x) {
    u32(uint32_t(x));
    u32(uint32_t(x >> 32));
  }
  void f64(double x) {
    uint64_t u;
    std::memcpy(&u, &x, 8);
    u64(u);
  }
  void f32(float x) {
    uint32_t u;
    std::memcpy(&u, &x, 4);
    u32(u);
  }
  void bytes(const Bytes &x) { b.insert(b.end(), x.begin(), x.end()); }
  void raw(const uint8_t *p, size_t n) { b.insert(b.end(), p, p + n); }
  void digest(const Digest &d) { b.insert(b.end(), d.begin(), d.end()); }
  // UTF-8, length prefixed. The bound belongs to the format, so writing past it is a
  // programming error here rather than something a reader has to cope with.
  void text(const std::string &x) {
    if (x.size() > MaxMetadataString)
      throw Error("VGS metadata string too long");
    u32(uint32_t(x.size()));
    b.insert(b.end(), x.begin(), x.end());
  }
};
// Well-formed UTF-8, checked on the way in. Metadata arrives from outside and ends up
// in a catalogue, a window title or a JavaScript string; malformed sequences are
// rejected here rather than turned into replacement characters three systems later.
inline bool validUtf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto lead = uint8_t(s[i]);
    size_t extra = 0;
    uint32_t point = 0;
    if (lead < 0x80) { extra = 0; point = lead; }
    else if ((lead & 0xe0) == 0xc0) { extra = 1; point = lead & 0x1fu; }
    else if ((lead & 0xf0) == 0xe0) { extra = 2; point = lead & 0x0fu; }
    else if ((lead & 0xf8) == 0xf0) { extra = 3; point = lead & 0x07u; }
    else return false;                       // continuation byte or 5-byte lead
    if (s.size() - i <= extra)
      return false;                          // truncated sequence
    for (size_t k = 1; k <= extra; ++k) {
      const auto next = uint8_t(s[i + k]);
      if ((next & 0xc0) != 0x80)
        return false;
      point = (point << 6) | (next & 0x3fu);
    }
    // Overlong encodings, surrogate halves and anything past U+10FFFF are all invalid,
    // and all three have been used to smuggle characters past naive validators.
    if ((extra == 1 && point < 0x80) || (extra == 2 && point < 0x800) ||
        (extra == 3 && point < 0x10000) || point > 0x10ffff ||
        (point >= 0xd800 && point <= 0xdfff))
      return false;
    i += extra + 1;
  }
  return true;
}
struct R {
  const uint8_t *p;
  size_t n, at = 0;
  R(const uint8_t *p_, size_t n_) : p(p_), n(n_) {}
  void need(uint64_t count) const {
    if (count > n - at)
      throw Error("truncated VGS data");
  }
  uint32_t u32() {
    need(4);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= uint32_t(p[at++]) << (8 * i);
    return v;
  }
  float f32() {
    const uint32_t u = u32();
    float x;
    std::memcpy(&x, &u, 4);
    if (!std::isfinite(x))
      throw Error("nonfinite VGS value");
    return x;
  }
  uint64_t u64() {
    uint64_t lo = u32();
    return lo | (uint64_t(u32()) << 32);
  }
  double f64() {
    auto v = u64();
    double d;
    std::memcpy(&d, &v, 8);
    if (!std::isfinite(d))
      throw Error("non-finite VGS metadata");
    return d;
  }
  Digest digest() {
    need(DigestSize);
    Digest d{};
    for (auto &x : d)
      x = p[at++];
    return d;
  }
  std::string text() {
    const uint32_t count = u32();
    if (count > MaxMetadataString)
      throw Error("VGS metadata string too long");
    need(count);
    std::string s(reinterpret_cast<const char *>(p + at), count);
    at += count;
    if (!validUtf8(s))
      throw Error("VGS metadata string is not valid UTF-8");
    return s;
  }
};
inline const Policy &policy(const Header &h, uint32_t id) {
  for (const auto &p : h.policies)
    if (p.attribute == id)
      return p;
  throw Error("missing VGS codec policy");
}
inline bool declaresLayer(const Header &h, uint32_t id) {
  for (const auto &l : h.layers)
    if (l.id == id)
      return true;
  return false;
}
inline uint32_t layer(uint32_t id) {
  return id == ShStaticBook || id == ShStaticIndices       ? 1
         : id == ShTemporalBook || id == ShTemporalIndices ? 2
                                                           : 0;
}
inline Spec readSpec(R &r) {
  Spec s;
  s.kind = r.u32();
  s.family = r.u32();
  s.width = r.u32();
  auto nf = r.u32();
  if (!nf || nf > 6)
    throw Error("invalid VGS field count");
  s.rows = r.u64();
  s.samples = r.u64();
  s.intervals = r.u64();
  s.entries = r.u64();
  s.fields.clear();
  for (uint32_t i = 0; i < nf; ++i)
    s.fields.push_back(int(r.u32()));
  auto runs = r.u32();
  if (runs > 5)
    throw Error("invalid VGS rank runs");
  for (uint32_t i = 0; i < runs; ++i) {
    auto rank = r.u32(), count = r.u32();
    if (rank < 1 || rank > 5 || !count || count > Limit / 4 ||
        s.ranks.size() + count > std::min<uint64_t>(s.rows, Limit / 4))
      throw Error("invalid VGS ranks");
    if (!s.ranks.empty() && rank <= s.ranks.back())
      throw Error("VGS ranks must increase");
    s.ranks.insert(s.ranks.end(), count, uint8_t(rank));
  }
  mgs::attributeSize(s);
  return s;
}
inline Group readGroup(R &r) {
  Group g;
  g.type = r.u32();
  g.flags = r.u32();
  g.splats = r.u64();
  g.intervals = r.u64();
  g.meshSamples = r.u64();
  for (auto &n : g.counts)
    n = r.u64();
  g.positionMin = r.f64();
  g.positionMax = r.f64();
  g.trajectoryMin = r.f64();
  g.trajectoryMax = r.f64();
  if (r.u64() || r.u64() || g.type > 1 || g.flags > 3 || g.intervals < 1 ||
      g.intervals > 255 || g.splats > Limit)
    throw Error("invalid VGS group");
  return g;
}
// Page and chunk payloads start on 16-byte boundaries so a reader can hand a page
// straight to a GPU upload without copying it to an aligned buffer first.
constexpr uint64_t Alignment = 16;
inline uint64_t aligned(uint64_t v) { return (v + Alignment - 1) / Alignment * Alignment; }

inline Metadata readMetadata(R &r) {
  Metadata m;
  r.need(m.uuid.size());
  for (auto &b : m.uuid)
    b = r.p[r.at++];
  m.id = r.text();
  m.title = r.text();
  m.author = r.text();
  m.projectName = r.text();
  m.takeName = r.text();
  m.captureStudio = r.text();
  m.copyright = r.text();
  m.softwareName = r.text();
  m.softwareVersion = r.text();
  const uint32_t tags = r.u32();
  if (tags > MaxMetadataTags)
    throw Error("too many VGS metadata tags");
  m.tags.reserve(tags);
  for (uint32_t i = 0; i < tags; ++i)
    m.tags.push_back(r.text());
  return m;
}

inline uint64_t tablesSize(size_t policies, size_t layers, size_t extras, size_t chunks) {
  return FixedHeaderSize + uint64_t(policies) * 24 + uint64_t(layers) * 16 +
         uint64_t(extras) * 40 + uint64_t(chunks) * 80;
}
} // namespace detail
} // namespace vgs
#endif
