// Writing VFGS captures: importing a MINT source, coding its pages, and laying out the
// container. This is the authoring half of the codec and is compiled only into the
// encoder library, never into the decoder or the WebAssembly module.

#include "vgsinternal.h"
#include "vgsframe.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace vgs {
using namespace detail;
namespace {
void writeSpec(W &w, const Spec &s) {
  w.u32(s.kind);
  w.u32(s.family);
  w.u32(s.width);
  w.u32(uint32_t(s.fields.size()));
  w.u64(s.rows);
  w.u64(s.samples);
  w.u64(s.intervals);
  w.u64(s.entries);
  for (auto f : s.fields)
    w.u32(uint32_t(f));
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  for (auto rank : s.ranks) {
    if (runs.empty() || runs.back().first != rank)
      runs.push_back({rank, 1});
    else
      ++runs.back().second;
  }
  w.u32(uint32_t(runs.size()));
  for (auto run : runs) {
    w.u32(run.first);
    w.u32(run.second);
  }
}
void writeGroup(W &w, const Group &g) {
  w.u32(g.type);
  w.u32(g.flags);
  w.u64(g.splats);
  w.u64(g.intervals);
  w.u64(g.meshSamples);
  for (auto n : g.counts)
    w.u64(n);
  w.f64(g.positionMin);
  w.f64(g.positionMax);
  w.f64(g.trajectoryMin);
  w.f64(g.trajectoryMax);
  w.u64(0);
  w.u64(0);
}
W directory(const ChunkDirectory &d) {
  W w;
  w.u32(ChunkMagic);
  w.u32(uint32_t(d.groups.size()));
  w.u32(uint32_t(d.pages.size()));
  w.u32(0);
  for (const auto &g : d.groups)
    writeGroup(w, g);
  for (const auto &p : d.pages) {
    w.u32(p.attribute);
    w.u32(p.group);
    w.u32(p.layer);
    w.u32(p.crc);
    w.u64(p.firstRow);
    w.u64(p.totalRows);
    w.u64(p.offset);
    w.u64(p.size);
    w.u64(p.decodedSize);
    writeSpec(w, p.spec);
  }
  if (w.b.size() > UINT32_MAX)
    throw Error("VGS directory too large");
  auto n = uint32_t(w.b.size());
  for (int i = 0; i < 4; ++i)
    w.b[12 + i] = uint8_t(n >> (8 * i));
  return w;
}
void pad(W &w) {
  while (w.b.size() % Alignment)
    w.b.push_back(0);
}

// The metadata block, in a fixed order with no field names on the wire. A reader of
// this version knows the order; there is nothing to look up and nothing to parse
// before the signature has been checked.
void writeMetadata(W &w, const Metadata &m) {
  w.raw(m.uuid.data(), m.uuid.size());
  w.text(m.id);
  w.text(m.title);
  w.text(m.author);
  w.text(m.projectName);
  w.text(m.takeName);
  w.text(m.captureStudio);
  w.text(m.copyright);
  w.text(m.softwareName);
  w.text(m.softwareVersion);
  if (m.tags.size() > MaxMetadataTags)
    throw Error("too many VGS metadata tags");
  w.u32(uint32_t(m.tags.size()));
  for (const auto &tag : m.tags)
    w.text(tag);
}

// An identifier derived from the capture rather than drawn at random: the metadata it
// carries, the identity of its contents - which is what the chunk digests already are -
// and the moment it was written.
//
// Deriving it from the metadata alone would be worse than useless: a studio that always
// writes the same author and studio would stamp every capture with the same identifier.
// The digests separate two different takes; the timestamp names this export exactly, so
// two writes of the same take are two identifiable files rather than one identity.
// The catalogue id is the field for the other question - which asset this is - and is
// left to a human, so it is an input here and not a substitute.
//
// The uuid field itself is excluded, or it would be an input to its own value. Version 8
// (RFC 9562) is the honest label: a custom derivation, not a random or a SHA-1 name-based
// identifier.
std::array<uint8_t, 16> deriveUuid(const Header &h) {
  W w;
  Metadata anonymous = h.metadata;
  anonymous.uuid = {};
  writeMetadata(w, anonymous);
  w.u64(h.frameCount);
  w.u64(h.durationTicks);
  w.u64(h.maxSplatsPerFrame);
  w.u32(h.shDegree);
  w.u32(h.encoding);
  for (const auto &c : h.chunks) {
    w.u64(c.startTick);
    w.u64(c.splats);
    w.digest(c.directoryDigest);
  }
  w.u64(h.createdMillis);
  w.u32(uint32_t(h.playbackMode));
  w.u32(uint32_t(h.motionType));
  w.f32(h.movingSpeed);
  const Digest d = digest(w.b.data(), w.b.size());
  std::array<uint8_t, 16> uuid{};
  std::copy(d.begin(), d.end(), uuid.begin());
  uuid[6] = uint8_t((uuid[6] & 0x0f) | 0x80);  // version 8
  uuid[8] = uint8_t((uuid[8] & 0x3f) | 0x80);  // RFC 4122 variant
  return uuid;
}

uint64_t metadataSize(const Metadata &m) {
  W w;
  writeMetadata(w, m);
  return w.b.size();
}

// Everything the signature covers: the fixed header, every table, and the metadata.
// Byte zero to signedSize, contiguous, so both implementations sign and verify the
// same range without a canonicalisation step to disagree about.
W headerBytes(const Header &h) {
  W w;
  w.u32(Magic);
  w.u32(Version);
  w.u64(h.headerSize);
  w.u64(h.signedSize);
  w.u64(h.fileSize);
  w.u32(h.timeNumerator);
  w.u32(h.timeDenominator);
  w.u64(h.frameCount);
  w.u64(h.durationTicks);
  w.u64(h.maxSplatsPerFrame);
  w.u64(h.maxChunkSplatRecords);
  for (auto v : h.bounds)
    w.f64(v);
  w.u32(h.shDegree);
  w.u32(h.shBasis);
  w.u32(h.coordinates);
  w.u32(h.encoding);
  w.u32(uint32_t(h.policies.size()));
  w.u32(uint32_t(h.chunks.size()));
  w.u32(h.pageRows);
  w.u32(uint32_t(h.extras.size()));
  w.u32(uint32_t(h.layers.size()));
  w.u64(h.startTick);
  w.u32(uint32_t(h.signedSize - h.headerSize));
  w.u64(h.createdMillis);
  w.u32(uint32_t(h.playbackMode));
  w.u32(uint32_t(h.motionType));
  w.f32(h.movingSpeed);
  w.u32(0);
  for (const auto &p : h.policies) {
    w.u32(p.attribute);
    w.u32(p.codec);
    w.u32(p.model);
    w.u32(p.family);
    w.u32(p.flags);
    w.u32(0);
  }
  for (const auto &l : h.layers) {
    w.u32(l.id);
    w.u32(l.kind);
    w.u32(l.dependsOn);
    w.u32(l.flags);
  }
  for (const auto &e : h.extras) {
    w.u32(e.type);
    w.u32(e.format);
    w.u64(e.offset);
    w.u64(e.size);
    w.digest(e.digest);
  }
  for (const auto &c : h.chunks) {
    w.u64(c.startTick);
    w.u64(c.intervals);
    w.u64(c.splats);
    w.u64(c.offset);
    w.u64(c.size);
    w.digest(c.directoryDigest);
    for (auto v : c.bounds)
      w.f32(v);
  }
  writeMetadata(w, h.metadata);
  return w;
}

// The signature block, written after the signed region and never part of it.
W signatureBytes(const SignatureInfo &s) {
  W w;
  w.u32(s.algorithm);
  w.u32(s.keyId);
  w.u32(uint32_t(SignatureSize));
  w.u32(0);
  w.raw(s.signature.data(), s.signature.size());
  return w;
}

struct SourceArray {
  uint32_t attribute, group;
  uint64_t offset, size;
  Spec spec;
  // Set when the array is not a window onto the source: the SH codebooks are
  // rebuilt when a lower degree drops coefficients out of the middle of them.
  Bytes data;
};

// Planes of three SH coefficients kept for each degree; a plane is one 32-bit
// index word per splat. Degree 2 needs eight coefficients, so it keeps three
// planes and leaves the ninth slot unused.
inline uint32_t shPlanes(uint32_t degree) {
  return degree >= 3 ? 5 : degree == 2 ? 3 : degree;
}
struct SourceChunk {
  ChunkEntry entry;
  std::vector<Group> groups;
  std::vector<SourceArray> arrays;
};
// The chunk's motion samples, validated, or nothing when it does not move.
std::vector<Motion> motionOf(const SourceChunk &c, const uint8_t *mint) {
  for (const auto &a : c.arrays)
    if (a.attribute == MotionSamples) {
      const uint64_t samples = c.entry.intervals + 1;
      if (a.group != 0 || !isMotionSamplesSpec(a.spec, a.spec.rows, samples))
        throw Error("invalid motion samples");
      const uint8_t *base = a.data.empty() ? mint + a.offset : a.data.data();
      return unpackMotionSamples(base, size_t(a.size), samples);
    }
  return {};
}
struct Source {
  Header header;
  std::vector<SourceChunk> chunks;
  std::function<SourceChunk(size_t)> provider;
};
const uint32_t SharedSlots[] = {8,   112, 128, 168, 184, 208,
                                232, 248, 264, 304, 320};
const uint32_t GroupSlots[] = {56,  136, 200, 216, 232, 248, 264, 280,
                               296, 312, 328, 344, 360, 376, 392};
// The container's timebase for a source whose frames last `interval` seconds, as the
// exact fraction a reader turns back into that interval: 1/25 for 25 Hz, 1/30 for 30,
// 1001/30000 for 29.97. The smallest numerator that makes the denominator a whole
// number wins, NTSC's 1001 tried first so 29.97 is not approximated by something odd.
void setTimebase(Header &h, double interval) {
  if (!(interval > 0))
    throw Error("invalid source frame interval");
  const double rate = 1.0 / interval;
  if (rate < 1 || rate > 1000)
    throw Error("source frame rate outside 1 to 1000 Hz");
  const auto fits = [&](uint32_t numerator) {
    const double denominator = std::round(rate * numerator);
    if (denominator < 1 || denominator > double(UINT32_MAX))
      return false;
    const double back = double(numerator) / denominator;
    if (std::abs(back - interval) > 1e-6 * interval)
      return false;
    h.timeNumerator = numerator;
    h.timeDenominator = uint32_t(denominator);
    return true;
  };
  if (fits(1) || fits(1001))
    return;
  for (uint32_t numerator = 2; numerator <= 1000; ++numerator)
    if (fits(numerator))
      return;
  throw Error("source frame rate is not a fraction the container can hold");
}

Source importMint(const uint8_t *data, size_t size, uint32_t shDegree = 3) {
  if (shDegree > 3)
    throw Error("SH degree out of range");
  auto u64 = [&](uint64_t at) {
    if (at > size || size - at < 8)
      throw Error("truncated source .mint");
    R r(data + at, 8);
    return r.u64();
  };
  auto f64 = [&](uint64_t at) {
    if (at > size || size - at < 8)
      throw Error("truncated source .mint");
    R r(data + at, 8);
    return r.f64();
  };
  auto span = [&](uint64_t at, uint64_t n) {
    if (at > size || n > size - at)
      throw Error("source array outside file");
  };
  if (u64(0) != 6)
    throw Error("VGS importer requires format-6 .mint");
  uint64_t cursor = 24, index = 0, base = u64(16), records = u64(8);
  if (records > size / 24)
    throw Error("invalid source record count");
  for (uint64_t i = 0; i < records; ++i) {
    auto type = u64(cursor);
    if (type == 1) {
      if (index)
        throw Error("duplicate source index");
      index = u64(cursor + 16);
      cursor += 24;
    } else if (type == 2) {
      if (u64(cursor + 24))
        throw Error("VGS cannot import nonempty auxiliary .mint records");
      cursor += 32;
    } else
      throw Error("unknown source record type");
  }
  if (!index)
    throw Error("source index missing");
  Source src;
  src.header.shDegree = shDegree;
  // The source's frame interval, taken from its first chunk: duration / intervals.
  double sourceInterval = 0;
  auto nc = u64(index);
  if (!nc || nc > size / 24)
    throw Error("invalid source chunk count");
  for (int j = 0; j < 6; ++j)
    src.header.bounds[j] = f64(index + 8 + 8 * j);
  for (uint64_t ci = 0; ci < nc; ++ci) {
    const auto e = index + 56 + ci * 24;
    SourceChunk c;
    c.entry.startTick = src.header.durationTicks;
    auto duration = f64(e);
    auto nb = u64(e + 8);
    cursor = u64(e + 16);
    if (!nb || nb > 1024)
      throw Error("invalid source group count");
    auto known = mgs::sourceAttributes(data, size, size_t(ci));
    std::map<uint64_t, mgs::SourceAttribute> typed;
    for (auto &a : known)
      typed.emplace(a.offset, std::move(a));
    uint32_t dictionaries = 0;
    std::vector<uint64_t> active;
    for (uint64_t bi = 0; bi < nb; ++bi) {
      const uint64_t type = u64(cursor), start = add(base, u64(cursor + 8)),
                     blockSize = u64(cursor + 16), hs = u64(cursor + 24),
                     h = cursor + 32;
      span(start, blockSize);
      if (hs != (type == 3 ? 336 : 416) || (type != 1 && type != 3))
        throw Error("unsupported source block layout");
      span(h, hs);
      Group g;
      g.type = type == 3 ? 0 : 1;
      if (type == 3) {
        ++dictionaries;
        const uint32_t slots[] = {144, 152, 160, 200, 224, 296};
        for (int j = 0; j < 6; ++j)
          g.counts[j] = u64(h + slots[j]);
        g.trajectoryMin = f64(h + 280);
        g.trajectoryMax = f64(h + 288);
      } else {
        g.splats = u64(h + 16);
        g.intervals = u64(h + 8);
        g.meshSamples = u64(h + 408);
        g.positionMin = f64(h + 24);
        g.positionMax = f64(h + 32);
        if (!g.intervals || g.intervals > 255 || g.splats > Limit / 2)
          throw Error("unsupported source splat group");
        if (c.entry.intervals && c.entry.intervals != g.intervals)
          throw Error("mixed sample counts");
        c.entry.intervals = g.intervals;
        c.entry.splats = add(c.entry.splats, g.splats);
        active.resize(size_t(g.intervals), 0);
        g.flags = (u64(h + 144) ? 1 : 0) | (u64(h + 256) ? 2 : 0);
        auto lifeStart = add(start, u64(h + 232));
        auto lifeBytes = u64(h + 240);
        if (lifeBytes < mul(g.splats, 2))
          throw Error("missing source lifetimes");
        span(lifeStart, lifeBytes);
        for (uint64_t n = 0; n < g.splats; ++n) {
          auto a = data[lifeStart + n * 2], b = data[lifeStart + n * 2 + 1];
          if (a > b || b > g.intervals)
            throw Error("invalid source lifetime");
          for (uint32_t t = a; t < b; ++t)
            ++active[t];
        }
      }
      const uint32_t count = type == 3 ? 11 : 15;
      const auto *slots = type == 3 ? SharedSlots : GroupSlots;
      for (uint32_t ai = 0; ai < count; ++ai) {
        auto reserved = u64(h + slots[ai] + 8);
        if (!reserved)
          continue;
        auto off = u64(h + slots[ai]);
        if (off > blockSize || reserved > blockSize - off)
          throw Error("source array outside block");
        auto absolute = add(start, off);
        SourceArray a{};
        a.attribute = (type == 3 ? 1 : 32) + ai;
        a.group = uint32_t(bi);
        a.offset = absolute;
        auto it = typed.find(absolute);
        if (it != typed.end()) {
          a.spec = it->second.spec;
          a.size = it->second.size;
        } else {
          uint64_t logical = 0;
          switch (a.attribute) {
          case ScaleLut:
          case RotationDeltaLut:
          case MeshExtentLut:
            logical = 1024;
            break;
          case Sh0Lut:
            logical = 512;
            break;
          case RotationRanks:
            logical = 20;
            break;
          case PositionRanks:
            logical = 16;
            break;
          default:
            throw Error("unsupported source attribute: " +
                        std::string(attributeName(a.attribute)));
          }
          a.size = logical;
          a.spec.rows = logical;
        }
        if (a.size > reserved)
          throw Error("truncated logical source attribute");
        // Writing fewer SH planes than the source carries: the index arrays keep
        // their first planes, which the page builder reads plane by plane anyway,
        // and the codebooks are rebuilt because the coefficients they drop sit in
        // the middle of every sample.
        const uint32_t planes = shPlanes(shDegree);
        if (planes < 5 &&
            (a.attribute == ShStaticIndices || a.attribute == ShTemporalIndices ||
             a.attribute == ShStaticBook || a.attribute == ShTemporalBook)) {
          if (!planes)
            continue;                     // degree 0 writes no higher-order SH
          if (a.attribute == ShStaticIndices || a.attribute == ShTemporalIndices) {
            a.spec.width = planes;
          } else {
            // Rows of three halves, fifteen coefficient blocks per sample. The
            // static book is one such sample and says so by carrying no entry
            // count; the temporal one repeats the block once per sample and puts
            // its size in `entries`, which is also its model's stride.
            const uint64_t block = a.spec.entries ? a.spec.entries : a.spec.rows;
            if (!block || block % 15 || a.spec.rows % block)
              throw Error("unexpected SH codebook layout");
            const uint64_t samples = a.spec.rows / block;
            const uint64_t kept = block / 15 * 3 * planes;
            a.data.resize(size_t(samples * kept * 6));
            for (uint64_t si = 0; si < samples; ++si)
              std::memcpy(a.data.data() + size_t(si * kept * 6),
                          data + a.offset + size_t(si * block * 6),
                          size_t(kept * 6));
            a.spec.rows = samples * kept;
            if (a.spec.entries)
              a.spec.entries = kept;
            a.size = a.data.size();
          }
        }
        c.arrays.push_back(std::move(a));
      }
      // Unnamed header fields are unsupported unless zero: never silently drop
      // a new semantic feature.
      std::set<uint32_t> used;
      for (uint32_t j = 0; j < count; ++j) {
        used.insert(slots[j]);
        used.insert(slots[j] + 8);
      }
      if (type == 3) {
        for (uint32_t x : {144u, 152u, 160u, 200u, 224u, 280u, 288u, 296u})
          used.insert(x);
      } else {
        for (uint32_t x : {8u, 16u, 24u, 32u, 408u})
          used.insert(x);
      }
      for (uint32_t j = 0; j < hs; j += 8)
        if (!used.count(j) && u64(h + j))
          throw Error("unsupported nonzero source metadata field");
      c.groups.push_back(g);
      cursor = add(h, hs);
    }
    if (dictionaries != 1 || !c.entry.intervals || c.groups.front().type != 0)
      throw Error("unsupported source chunk composition");
    // Every chunk has to keep the same frame interval: the container has one timebase.
    const double interval = duration / double(c.entry.intervals);
    if (!(interval > 0))
      throw Error("invalid source chunk duration");
    if (!sourceInterval)
      sourceInterval = interval;
    else if (std::abs(interval - sourceInterval) > 1e-6 * sourceInterval) {
      // Said in the terms of whoever picked the file: chunks counted from one, rates
      // and times as a person writes them.
      char what[256];
      std::snprintf(what, sizeof what,
                    "the source changes frame rate: chunk %llu of %llu runs at %g Hz "
                    "(%llu frames in %g s), the ones before at %g Hz; a capture has to "
                    "keep one frame rate",
                    static_cast<unsigned long long>(ci + 1),
                    static_cast<unsigned long long>(nc), 1.0 / interval,
                    static_cast<unsigned long long>(c.entry.intervals), duration,
                    1.0 / sourceInterval);
      throw Error(what);
    }
    c.groups[0].intervals = c.entry.intervals;
    // Current import profile is degree 3 and requires both higher-order layers.
    for (uint32_t gi = 0; gi < c.groups.size(); ++gi) {
      auto has = [&](uint32_t id) {
        return std::any_of(c.arrays.begin(), c.arrays.end(),
                           [&](const SourceArray &a) {
                             return a.group == gi && a.attribute == id;
                           });
      };
      const bool wantsSh = shPlanes(shDegree) != 0;
      if (c.groups[gi].type == 0) {
        if (wantsSh && (!has(ShStaticBook) || !has(ShTemporalBook)))
          throw Error("source SH profile unsupported");
      } else if ((wantsSh && (!has(ShStaticIndices) || !has(ShTemporalIndices))) ||
                 !has(Lifetimes) || !has(ScaleIndices) || !has(Sh0Terms) ||
                 !has(Sh0Base) || !has(OpacityTerms) ||
                 (c.groups[gi].flags & 1
                      ? !has(PositionSamples)
                      : (!has(PositionBase) || !has(PositionTerms) ||
                         !has(PositionRanks))) ||
                 (c.groups[gi].flags & 2
                      ? !has(RotationSamples)
                      : (!has(RotationBase) || !has(RotationTerms) ||
                         !has(RotationRanks))))
        throw Error("incomplete source splat group");
    }
    src.header.durationTicks = add(src.header.durationTicks, c.entry.intervals);
    src.header.maxChunkSplatRecords =
        std::max(src.header.maxChunkSplatRecords, c.entry.splats);
    for (auto n : active)
      src.header.maxSplatsPerFrame = std::max(src.header.maxSplatsPerFrame, n);
    src.chunks.push_back(std::move(c));
  }
  src.header.frameCount = src.header.durationTicks;
  setTimebase(src.header, sourceInterval);
  return src;
}
// Split trajectories on their boundaries. Temporal SH can split into contiguous
// blocks of complete sample-major entry rows: every page resets the predictor,
// and firstRow places its bytes back in the original array without a gather.
struct InputPage {
  Page page;
  Bytes data;
};
std::vector<InputPage> pages(const SourceChunk &c, const uint8_t *mint,
                             uint32_t pageRows, bool splitTemporalSh = false) {
  std::vector<InputPage> out;
  for (const auto &a : c.arrays) {
    const auto &s = a.spec;
    const uint64_t stride = s.family == 2 ? s.entries
                            : s.family == 1 || s.family == 7 ? s.samples
                            : s.family == 4                ? s.intervals
                                                           : 1;
    const uint64_t step =
        std::max<uint64_t>(stride, (pageRows / stride) * stride);
    if (s.family == 2 && !splitTemporalSh) {
      InputPage p;
      p.page.attribute = a.attribute;
      p.page.group = a.group;
      p.page.layer = layer(a.attribute);
      p.page.totalRows = s.rows;
      p.page.spec = s;
      const uint8_t *base = a.data.empty() ? mint + a.offset : a.data.data();
      p.data.assign(base, base + a.size);
      p.page.decodedSize = a.size;
      out.push_back(std::move(p));
      continue;
    }
    uint64_t row = 0, rankIndex = 0;
    while (row < s.rows) {
      InputPage p;
      p.page.attribute = a.attribute;
      p.page.group = a.group;
      p.page.layer = layer(a.attribute);
      p.page.firstRow = row;
      p.page.totalRows = s.rows;
      p.page.spec = s;
      auto &ps = p.page.spec;
      uint64_t n = std::min(step, s.rows - row);
      if (!s.ranks.empty()) {
        ps.ranks.clear();
        n = 0;
        while (rankIndex < s.ranks.size() &&
               (n < pageRows || ps.ranks.empty())) {
          auto rank = s.ranks[size_t(rankIndex++)];
          ps.ranks.push_back(rank);
          n += rank;
        }
      }
      ps.rows = n;
      p.page.decodedSize = mgs::attributeSize(ps);
      p.data.resize(size_t(p.page.decodedSize));
      const uint8_t *base = a.data.empty() ? mint + a.offset : a.data.data();
      if (s.kind == 3) {
        // Plane p of the source is a run of one word per splat; a capture written
        // at a lower degree simply stops after the planes it keeps.
        for (uint32_t plane = 0; plane < ps.width; ++plane)
          std::memcpy(p.data.data() + plane * n * 4,
                      base + (plane * s.rows + row) * 4, size_t(n * 4));
      } else {
        auto rowBytes = a.size / s.rows;
        std::memcpy(p.data.data(), base + row * rowBytes, p.data.size());
      }
      out.push_back(std::move(p));
      row += n;
    }
  }
  return out;
}
} // namespace
// measured by decoding the chunk that was just written. Only the base layer is
// decoded and spherical harmonics are skipped: this needs positions, nothing else.
// A chunk whose box cannot be measured keeps the conservative one it came in with.
static void tighten(Header &h, size_t ci, const uint8_t *chunk, size_t size) {
  auto &entry = h.chunks[ci];
  const uint64_t intervals = entry.intervals;
  float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
  bool any = false;
  double pad = 0;
  try {
    const auto decoded = decodeChunk(h, ci, chunk, size, 1);
    const FrameDecoder decoder(decoded,
                               double(h.timeNumerator) / h.timeDenominator);
    // With motion, positions are no longer linear between samples: rotation follows an
    // arc and scale multiplies a moving point. What leaves the box spanned by the samples
    // is bounded by f''/8 for f(t) = s(t) R(t) p(t), with p linear, R a slerp through
    // angle theta and s linear, so the samples are measured and the box grown by that.
    std::vector<double> previousLocal;
    Motion previous;
    double radius = 0, step = 0;
    struct Pair { double theta, scaleStep, scale; };
    std::vector<Pair> pairs;
    for (uint64_t s = 0; s <= intervals; ++s) {
      // The last sample is approached from just inside the chunk: one is the start
      // of the next chunk, not an instant this one ever shows.
      const double nt =
          intervals ? std::min(double(s) / double(intervals), 1.0 - 1e-9) : 0.0;
      const Frame frame = decoder.evaluate(nt, false);
      Motion motion;
      double forward[12] = {};
      if (decoder.hasMotion()) {
        motion = decoder.motion(nt);
        motion.matrix(forward);
      }
      std::vector<double> local;
      if (decoder.hasMotion())
        local.assign(size_t(frame.count) * 3, 0.0);
      for (uint64_t i = 0; i < frame.count; ++i) {
        if (decoder.hasMotion()) {
          // Back to the capture's own space: R^T (world - t) / s.
          double d[3];
          for (int k = 0; k < 3; ++k)
            d[k] = frame.position[size_t(i) * 3 + size_t(k)] - forward[k * 4 + 3];
          for (int k = 0; k < 3; ++k)
            local[size_t(i) * 3 + size_t(k)] =
                (forward[k] * d[0] + forward[4 + k] * d[1] + forward[8 + k] * d[2]) /
                (motion.scale * motion.scale);
        }
        if (!frame.active[size_t(i)])
          continue;
        for (int k = 0; k < 3; ++k) {
          const float v = frame.position[size_t(i) * 3 + size_t(k)];
          lo[k] = any ? std::min(lo[k], v) : v;
          hi[k] = any ? std::max(hi[k], v) : v;
        }
        if (decoder.hasMotion()) {
          const double *p = &local[size_t(i) * 3];
          radius = std::max(radius, std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
          if (!previousLocal.empty()) {
            const double *q = &previousLocal[size_t(i) * 3];
            step = std::max(step, std::sqrt((p[0] - q[0]) * (p[0] - q[0]) +
                                            (p[1] - q[1]) * (p[1] - q[1]) +
                                            (p[2] - q[2]) * (p[2] - q[2])));
          }
        }
        any = true;
      }
      if (decoder.hasMotion()) {
        if (s) {
          double dot = 0;
          for (int k = 0; k < 4; ++k)
            dot += previous.rotation[k] * motion.rotation[k];
          pairs.push_back({2 * std::acos(std::min(1.0, std::abs(dot))),
                           std::abs(motion.scale - previous.scale),
                           std::max(motion.scale, previous.scale)});
        }
        previous = motion;
        previousLocal = std::move(local);
      }
    }
    for (const auto &p : pairs) {
      const double second = p.scale * p.theta * p.theta + 2 * p.scaleStep * p.theta;
      const double first = p.scale * p.theta + p.scaleStep;
      pad = std::max(pad, (second * radius + 2 * first * step) / 8);
    }
  } catch (const Error &) {
    return;
  }
  if (!any)
    return;
  for (int k = 0; k < 3; ++k) {
    entry.bounds[size_t(k)] = float(lo[k] - pad);
    entry.bounds[size_t(k) + 3] = float(hi[k] + pad);
  }
}

static Bytes encodeSource(Source src, const uint8_t *mint, const EncodeOptions &options,
                          const Progress &progress, const WriteSink &sink = {}) {
  if (options.pageRows < 1024 || options.pageRows > 1048576)
    throw Error("VGS pageRows outside 1024..1048576");
  Header h = src.header;
  h.pageRows = options.pageRows;
  auto report = [&](int done) {
    if (progress && !progress(done, int(src.chunks.size() * 2)))
      throw Error("cancelled");
  };
  report(0);
  SourceChunk provided;
  auto getChunk = [&](size_t index) -> const SourceChunk & {
    if (!src.provider) return src.chunks[index];
    provided = src.provider(index); return provided;
  };
  // First pass measures complete candidate cost over every page in the file.
  struct Costs {
    uint32_t family = 0;
    uint64_t raw = 0;
    std::vector<uint64_t> models;
  };
  std::map<uint32_t, Costs> costs;
  size_t movingChunks = 0;
  for (size_t ci = 0; ci < src.chunks.size(); ++ci) {
    const auto &sourceChunk = getChunk(ci);
    // Motion is checked here, before anything is written, rather than discovered by a
    // reader: a sample that does not validate, or a chunk without samples in a capture
    // that has them, would make every reader refuse the file.
    if (!motionOf(sourceChunk, mint).empty())
      ++movingChunks;
    if (src.provider) {
      h.maxChunkSplatRecords = std::max(h.maxChunkSplatRecords, sourceChunk.entry.splats);
      h.maxSplatsPerFrame = std::max(h.maxSplatsPerFrame, sourceChunk.entry.splats);
    }
    for (const auto &p : pages(sourceChunk, mint, options.pageRows, options.splitTemporalShPages)) {
      auto &t = costs[p.page.attribute];
      int count = mgs::attributeModelCount(p.page.spec);
      if (t.models.empty()) {
        t.family = p.page.spec.family;
        t.models.resize(count, 0);
      } else if (t.family != p.page.spec.family ||
                 t.models.size() != size_t(count))
        throw Error("mixed attribute schema");
      t.raw = add(t.raw, p.data.size());
      if (options.compression == Compression::None)
        std::fill(t.models.begin(), t.models.end(), UINT64_MAX);
      for (int model = 0; model < count; ++model)
        if (t.models[model] != UINT64_MAX) {
          try {
            t.models[model] =
                add(t.models[model],
                    mgs::encodeAttribute(p.page.spec, model, p.data.data(),
                                         p.data.size(), options.entropySearch)
                        .size());
          } catch (const Error &) {
            if (!model)
              throw;
            t.models[model] = UINT64_MAX;
          }
        }
    }
    report(int(ci + 1));
  }
  if (movingChunks && movingChunks != src.chunks.size())
    throw Error("motion samples must be in every chunk or in none");
  for (const auto &kv : costs) {
    Policy p;
    p.attribute = kv.first;
    p.family = kv.second.family;
    uint64_t best = kv.second.raw;
    for (uint32_t m = 0; m < kv.second.models.size(); ++m)
      if (kv.second.models[m] < best) {
        best = kv.second.models[m];
        p.codec = Rans;
        p.model = m;
      }
    h.policies.push_back(p);
  }
  h.chunks.resize(src.chunks.size());
  h.startTick = options.startTick;
  if (uint32_t(options.playbackMode) > MaxPlaybackMode)
    throw Error("invalid VGS playback mode");
  h.playbackMode = options.playbackMode;
  if (uint32_t(options.motionType) > MaxMotionType)
    throw Error("invalid VGS motion type");
  if (!std::isfinite(options.movingSpeed))
    throw Error("invalid VGS moving speed");
  h.motionType = options.motionType;
  h.movingSpeed = options.movingSpeed;
  h.layers.push_back({0, BaseLayer, 0, 0});
  if (h.shDegree) {
    h.layers.push_back({1, StaticShLayer, 0, 0});
    h.layers.push_back({2, TemporalShLayer, 0, 0});
  }
  for (const auto &e : options.extras) {
    if (!e.type || e.bytes.empty())
      throw Error("empty VGS extra");
    // The digest goes in the signed table, so an extra's bytes are covered without the
    // reader having to fetch them to open the file.
    h.extras.push_back({e.type, e.format, 0, e.bytes.size(),
                        digest(e.bytes.data(), e.bytes.size())});
  }
  h.metadata = options.metadata;
  for (const auto *field :
       {&h.metadata.id, &h.metadata.title, &h.metadata.author, &h.metadata.projectName,
        &h.metadata.takeName, &h.metadata.captureStudio, &h.metadata.copyright,
        &h.metadata.softwareName, &h.metadata.softwareVersion})
    if (!validUtf8(*field))
      throw Error("VGS metadata string is not valid UTF-8");
  for (const auto &tag : h.metadata.tags)
    if (!validUtf8(tag))
      throw Error("VGS metadata string is not valid UTF-8");
  h.headerSize = tablesSize(h.policies.size(), h.layers.size(), h.extras.size(),
                            h.chunks.size());
  h.signedSize = add(h.headerSize, metadataSize(h.metadata));
  if (h.signedSize - h.headerSize > MaxMetadataSize)
    throw Error("VGS metadata block too large");
  W out;
  // The structural region and its signature block are reserved up front and written
  // once everything they describe is known.
  out.b.resize(size_t(add(h.signedSize, SignatureBlockSize)));
  for (size_t i = 0; i < options.extras.size(); ++i) {
    pad(out);
    h.extras[i].offset = out.b.size();
    out.bytes(options.extras[i].bytes);
  }
  uint64_t outputBase = 0;
  if (sink) { sink(0,out.b.data(),out.b.size()); outputBase = out.b.size(); out.b.clear(); }
  for (size_t ci = 0; ci < src.chunks.size(); ++ci) {
    const auto &sourceChunk = getChunk(ci);
    auto inputs = pages(sourceChunk, mint, options.pageRows, options.splitTemporalShPages);
    std::stable_sort(inputs.begin(), inputs.end(),
                     [](const InputPage &a, const InputPage &b) {
                       return a.page.layer < b.page.layer;
                     });
    ChunkDirectory d;
    d.groups = sourceChunk.groups;
    std::vector<Bytes> payloads;
    for (auto &input : inputs) {
      auto p = input.page;
      const auto &pol = policy(h, p.attribute);
      Bytes payload =
          pol.codec == Raw
              ? input.data
              : mgs::encodeAttribute(p.spec, int(pol.model), input.data.data(),
                                     input.data.size(), options.entropySearch);
      p.size = payload.size();
      p.crc = crc32(payload.data(), payload.size());
      if (options.verify &&
          decodePage(h, p, payload.data(), payload.size()) != input.data)
        throw Error("VGS page verification failed");
      d.pages.push_back(std::move(p));
      payloads.push_back(std::move(payload));
    }
    auto dir = directory(d);
    uint64_t at = aligned(dir.b.size());
    for (auto &p : d.pages) {
      p.offset = at;
      at = aligned(add(at, p.size));
    }
    dir = directory(d);
    auto c = sourceChunk.entry;
    // A starting box that certainly contains the chunk: the position quantisation
    // range, which is a scalar and so the same on every axis. The real extent is
    // measured below, once the chunk can be decoded; this stands in until then and
    // remains the answer for a chunk with nothing alive in it.
    float lo = 0, hi = 0;
    bool first = true;
    for (const auto &g : sourceChunk.groups) {
      if (g.type != 1)
        continue;
      lo = first ? float(g.positionMin) : std::min(lo, float(g.positionMin));
      hi = first ? float(g.positionMax) : std::max(hi, float(g.positionMax));
      first = false;
    }
    for (int k = 0; k < 3; ++k) {
      c.bounds[k] = lo;
      c.bounds[k + 3] = hi;
    }
    // A moving chunk's quantisation cube moves with it. Its circumscribed sphere holds it
    // under any rotation, and between two samples the centre moves linearly and the scale
    // stays between theirs, so the boxes of those spheres at the samples hold the chunk.
    if (const auto motion = motionOf(sourceChunk, mint); !motion.empty()) {
      const double radius = std::sqrt(3.0) * std::max(std::abs(double(lo)), std::abs(double(hi)));
      for (int k = 0; k < 3; ++k) {
        double a = 1e300, b = -1e300;
        for (const auto &m : motion) {
          a = std::min(a, m.translation[k] - radius * m.scale);
          b = std::max(b, m.translation[k] + radius * m.scale);
        }
        c.bounds[k] = float(a);
        c.bounds[k + 3] = float(b);
      }
    }
    while ((outputBase + out.b.size()) % 16) out.b.push_back(0);
    c.offset = outputBase + out.b.size();
    c.directoryDigest = digest(dir.b.data(), dir.b.size());
    out.bytes(dir.b);
    for (size_t pi = 0; pi < payloads.size(); ++pi) {
      while (outputBase + out.b.size() - c.offset < d.pages[pi].offset)
        out.b.push_back(0);
      out.bytes(payloads[pi]);
    }
    c.size = outputBase + out.b.size() - c.offset;
    h.chunks[ci] = c;
    // What a player does with this box is fit things to it: culling, and the shadow
    // map of a directional light. The quantisation cube is several times the size of
    // the capture, which spreads that shadow map over mostly empty space until the
    // shadow stops being visible at all. So the chunk is decoded back and its own
    // sample grid measured: positions move linearly between samples, so the extremes
    // are all at sample points and this is exact, not an estimate. Only live splats
    // count - a dead one's stored position is not on screen.
    tighten(h, ci, out.b.data() + size_t(c.offset - outputBase), size_t(c.size));
    if (sink) { sink(outputBase,out.b.data(),out.b.size()); outputBase += out.b.size(); out.b.clear(); }
    report(int(src.chunks.size() + ci + 1));
  }
  // The capture's own box, and the only one a player has before it fetches a chunk:
  // the union of the boxes just measured, so it is the exact extent over every frame
  // rather than whatever the source declared. A capture with no live splat anywhere
  // keeps the source's box, since there is nothing to measure.
  {
    bool any = false;
    std::array<double, 6> box{};
    for (const auto &c : h.chunks) {
      for (int k = 0; k < 3; ++k) {
        box[size_t(k)] = any ? std::min(box[size_t(k)], double(c.bounds[size_t(k)]))
                             : double(c.bounds[size_t(k)]);
        box[size_t(k) + 3] = any ? std::max(box[size_t(k) + 3], double(c.bounds[size_t(k) + 3]))
                                 : double(c.bounds[size_t(k) + 3]);
      }
      any = true;
    }
    if (any)
      h.bounds = box;
  }
  h.fileSize = outputBase + out.b.size();
  if (!h.createdMillis)
    h.createdMillis = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
  if (h.metadata.uuid == std::array<uint8_t, 16>{})
    h.metadata.uuid = deriveUuid(h);
  if (add(h.headerSize, metadataSize(h.metadata)) != h.signedSize)
    throw Error("VGS metadata size changed after layout");
  auto hb = headerBytes(h);
  if (hb.b.size() != h.signedSize)
    throw Error("VGS header size mismatch");
  if (!sink) std::copy(hb.b.begin(), hb.b.end(), out.b.begin());
  // Signed last, over the bytes as they will be read: the signature covers the whole
  // structural region and nothing else, so verifying is one range request and one call.
  if (!options.signer.sign)
    throw Error("VGS encoding needs a signer");
  h.signature.algorithm = Ed25519Signature;
  h.signature.keyId = options.signer.keyId;
  h.signature.signature = options.signer.sign(hb.b.data(), size_t(h.signedSize));
  auto sb = signatureBytes(h.signature);
  if (sink) { sink(0,hb.b.data(),hb.b.size()); sink(h.signedSize,sb.b.data(),sb.b.size()); }
  else std::copy(sb.b.begin(), sb.b.end(), out.b.begin() + size_t(h.signedSize));
  return std::move(out.b);
}

Bytes encodeMint(const uint8_t *mint, size_t size, const EncodeOptions &options,
                 const Progress &progress) {
  return encodeSource(importMint(mint, size, options.shDegree), mint, options, progress);
}

struct MintLogicalSource::State {
  const uint8_t *bytes;
  Source source;
};
MintLogicalSource::MintLogicalSource(const uint8_t *bytes,size_t size,uint32_t degree)
    : state(new State{bytes,importMint(bytes,size,degree)}) {
  for (const auto &chunk : state->source.chunks) state->source.header.chunks.push_back(chunk.entry);
}
MintLogicalSource::~MintLogicalSource() = default;
const Header &MintLogicalSource::header() const { return state->source.header; }
DecodedChunk MintLogicalSource::chunk(size_t index) const {
  if (index>=state->source.chunks.size()) throw Error("source chunk out of range");
  const auto &source=state->source.chunks[index];DecodedChunk result;result.groups=source.groups;
  for (const auto &array : source.arrays) {
    DecodedPage page;page.descriptor.attribute=array.attribute;page.descriptor.group=array.group;
    page.descriptor.layer=detail::layer(array.attribute);page.descriptor.spec=array.spec;
    page.descriptor.totalRows=array.spec.rows;page.descriptor.decodedSize=array.size;
    if (!array.data.empty()) page.bytes=array.data;
    else page.bytes.assign(state->bytes+array.offset,state->bytes+array.offset+array.size);
    result.pages.push_back(std::move(page));
  }
  return result;
}

void encodeSequence(const Header &header, const ChunkProvider &provider,
                    const WriteSink &sink, const EncodeOptions &options,
                    const Progress &progress) {
  if (header.encoding != 0 || header.chunks.empty() || !provider || !sink)
    throw Error("invalid edited sequence header");
  Source src; src.header = header;
  src.header.policies.clear(); src.header.layers.clear(); src.header.extras.clear();
  src.chunks.resize(header.chunks.size());
  src.provider = [&](size_t index) {
    auto chunk = provider(index);
    SourceChunk c; c.entry = header.chunks[index]; c.groups = std::move(chunk.groups);
    c.entry.splats = 0;
    for (const auto &group : c.groups) if (group.type == 1) c.entry.splats += group.splats;
    for (auto &page : chunk.pages) {
      if (page.descriptor.firstRow || page.bytes.size() != mgs::attributeSize(page.descriptor.spec))
        throw Error("edited input requires complete attribute arrays");
      c.arrays.push_back({page.descriptor.attribute,page.descriptor.group,0,
                         page.bytes.size(),page.descriptor.spec,std::move(page.bytes)});
    }
    return c;
  };
  encodeSource(std::move(src),nullptr,options,progress,sink);
}

#ifdef VGS_TEMPORAL_EXPERIMENT
Bytes encodeLogical(const Header &header, const std::vector<DecodedChunk> &chunks,
                    const EncodeOptions &options, const Progress &progress) {
  if (header.chunks.size() != chunks.size() || chunks.empty() || header.encoding > 1)
    throw Error("invalid native sequence header");
  Source src;
  src.header = header;
  src.header.policies.clear();
  src.header.layers.clear();
  src.header.extras.clear();
  for (size_t i = 0; i < chunks.size(); ++i) {
    SourceChunk c;
    c.entry = header.chunks[i];
    c.groups = chunks[i].groups;
    for (const auto &p : chunks[i].pages) {
      if (p.descriptor.firstRow || p.bytes.size() != mgs::attributeSize(p.descriptor.spec))
        throw Error("native input requires complete attribute arrays");
      c.arrays.push_back({p.descriptor.attribute, p.descriptor.group, 0,
                         p.bytes.size(), p.descriptor.spec, p.bytes});
    }
    src.chunks.push_back(std::move(c));
  }
  return encodeSource(std::move(src), nullptr, options, progress);
}
#endif

void verifyMint(const uint8_t *coded, size_t size, const uint8_t *mint,
                size_t mintSize) {
  const auto h = readHeader(coded, size);
  if (h.fileSize != size)
    throw Error("VGS file length mismatch");
  auto src = importMint(mint, mintSize, h.shDegree);
  if (h.chunks.size() != src.chunks.size() ||
      h.frameCount != src.header.frameCount ||
      h.maxSplatsPerFrame != src.header.maxSplatsPerFrame)
    throw Error("VGS source metadata mismatch");
  // The stored box is measured from the decoded splats, so it does not have to equal
  // the one the source declared - it is usually a little tighter, and quantisation can
  // push a face out by a fraction of a millimetre. What it must not be is somewhere
  // else: a box that escapes the source's by a tenth of its own size means the wrong
  // capture, not rounding.
  for (int k = 0; k < 3; ++k) {
    const double slack =
        0.1 * (src.header.bounds[size_t(k) + 3] - src.header.bounds[size_t(k)]) + 1e-3;
    if (h.bounds[size_t(k)] < src.header.bounds[size_t(k)] - slack ||
        h.bounds[size_t(k) + 3] > src.header.bounds[size_t(k) + 3] + slack)
      throw Error("VGS bounds do not match the source");
  }
  for (size_t ci = 0; ci < h.chunks.size(); ++ci) {
    const auto &e = h.chunks[ci];
    auto d = readChunkDirectory(h, ci, coded + e.offset, size_t(e.size));
    const bool splitTemporalSh = std::any_of(d.pages.begin(), d.pages.end(), [](const Page& p) {
      return p.spec.family == 2 && p.spec.rows != p.totalRows;
    });
    auto expected = pages(src.chunks[ci], mint, h.pageRows, splitTemporalSh);
    if (d.pages.size() != expected.size())
      throw Error("VGS page count mismatch");
    W ga, gb;
    for (const auto &g : d.groups)
      writeGroup(ga, g);
    for (const auto &g : src.chunks[ci].groups)
      writeGroup(gb, g);
    if (ga.b != gb.b)
      throw Error("VGS group metadata mismatch");
    for (const auto &p : d.pages) {
      auto it = std::find_if(
          expected.begin(), expected.end(), [&](const InputPage &q) {
            return q.page.attribute == p.attribute && q.page.group == p.group &&
                   q.page.firstRow == p.firstRow;
          });
      if (it == expected.end())
        throw Error("VGS unexpected page");
      W sa, sb;
      writeSpec(sa, p.spec);
      writeSpec(sb, it->page.spec);
      if (sa.b != sb.b || decodePage(h, p, coded + e.offset + p.offset,
                                     size_t(p.size)) != it->data)
        throw Error("VGS logical attribute mismatch");
    }
  }
}
} // namespace vgs
