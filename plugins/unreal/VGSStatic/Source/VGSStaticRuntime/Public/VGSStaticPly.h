#pragma once

/*
 * A Gaussian splat PLY, read and turned into what the VGSStatic asset keeps: the renderer's
 * float records in cluster order, one packed normal per splat, and the clusters.
 *
 * Plain C++ with nothing of Unreal in it, as VGSSplatLayout.h (which it uses for the
 * records): the editor module imports with it, and tests/unreal_static.cpp compiles it on
 * its own and checks it, so the bytes can be verified without building the engine.
 *
 * The PLY is the one 3D Gaussian splatting training writes (and SMNForge, and most tools):
 * one `vertex` element with x, y, z; nx, ny, nz (optional, often all 0); f_dc_0..2;
 * f_rest_0.. (0, 9, 24 or 45 of them, all of red, then green, then blue); opacity (a logit);
 * scale_0..2 (logarithms); rot_0..3 (w, x, y, z, not necessarily unit). Binary little
 * endian or ASCII; any numeric property type. The axes are the file's own: the component
 * turns them into Unreal's as VGS turns captures (VGSSplatLayout.h, captureToUnreal).
 *
 * Header comments are kept. SMNForge writes two that say where a splat comes from:
 *   comment capture_rig SMNV1 | THE4DSCANNER   the studio rig, in whose frame the splats are;
 *   comment normals SMNForge                   nx, ny, nz are its outward normals.
 */

#include "VGSSplatLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace vgsstatic {

/* The splats of a PLY, as vgsbridge hands a frame over (VGSSplatLayout.h, FrameView):
 * rotations wxyz unit length, scales and opacity activated, SH one plane per coefficient
 * past DC. */
struct Splats {
  size_t count = 0;
  int shCoefficients = 0; // 0, 3, 8 or 15
  std::vector<float> positions, rotations, scales, radiance, sh;
  std::vector<float> normals; // 3 per splat, as in the file; empty when it has none
  size_t splatsWithNormals = 0;
  size_t dropped = 0; // records with a non-finite position, scale or rotation
  std::vector<std::string> comments; // the header's, without "comment "

  /* The rest of the comment that starts with `key`, or "" (comment capture_rig SMNV1). */
  std::string comment(const std::string &key) const {
    for (const std::string &c : comments)
      if (c.compare(0, key.size(), key) == 0 && (c.size() == key.size() || c[key.size()] == ' ')) {
        size_t at = key.size();
        while (at < c.size() && c[at] == ' ') ++at;
        return c.substr(at);
      }
    return std::string();
  }

  vgsunreal::FrameView view() const {
    vgsunreal::FrameView v;
    v.count = count;
    v.shCoefficients = shCoefficients;
    v.positions = positions.data();
    v.rotations = rotations.data();
    v.scales = scales.data();
    v.radiance = radiance.data();
    v.sh = shCoefficients ? sh.data() : nullptr;
    return v;
  }
};

namespace detail {

enum class Type { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64, Invalid };

inline Type typeOf(const std::string &name) {
  if (name == "char" || name == "int8") return Type::Int8;
  if (name == "uchar" || name == "uint8") return Type::UInt8;
  if (name == "short" || name == "int16") return Type::Int16;
  if (name == "ushort" || name == "uint16") return Type::UInt16;
  if (name == "int" || name == "int32") return Type::Int32;
  if (name == "uint" || name == "uint32") return Type::UInt32;
  if (name == "float" || name == "float32") return Type::Float32;
  if (name == "double" || name == "float64") return Type::Float64;
  return Type::Invalid;
}

inline size_t sizeOf(Type t) {
  switch (t) {
  case Type::Int8: case Type::UInt8: return 1;
  case Type::Int16: case Type::UInt16: return 2;
  case Type::Int32: case Type::UInt32: case Type::Float32: return 4;
  case Type::Float64: return 8;
  default: return 0;
  }
}

inline double readBinary(const uint8_t *p, Type t) {
  switch (t) {
  case Type::Int8: return double(int8_t(*p));
  case Type::UInt8: return double(*p);
  case Type::Int16: { int16_t v; std::memcpy(&v, p, 2); return v; }
  case Type::UInt16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
  case Type::Int32: { int32_t v; std::memcpy(&v, p, 4); return v; }
  case Type::UInt32: { uint32_t v; std::memcpy(&v, p, 4); return v; }
  case Type::Float32: { float v; std::memcpy(&v, p, 4); return v; }
  case Type::Float64: { double v; std::memcpy(&v, p, 8); return v; }
  default: return 0.0;
  }
}

struct Property {
  std::string name;
  Type type = Type::Invalid;
  size_t offset = 0; // in a binary record
};

struct Element {
  std::string name;
  size_t count = 0;
  std::vector<Property> properties;
  size_t stride = 0;
  bool hasList = false;
};

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

} // namespace detail

/* Reads a Gaussian splat PLY from memory. Returns an empty string, or what is wrong. */
inline std::string readPly(const uint8_t *data, size_t size, Splats &out) {
  using namespace detail;
  out = Splats();
  // ---- header
  const char *text = reinterpret_cast<const char *>(data);
  const std::string headEnd = "end_header";
  size_t end = 0;
  {
    const size_t limit = std::min<size_t>(size, 1u << 20);
    const std::string head(text, limit);
    const size_t at = head.find(headEnd);
    if (head.compare(0, 3, "ply") != 0 || at == std::string::npos)
      return "not a PLY file";
    end = at + headEnd.size();
    while (end < size && text[end] != '\n') ++end; // the rest of the line
    if (end < size) ++end;
  }
  std::istringstream header(std::string(text, end));
  std::string line, format;
  std::vector<Element> elements;
  while (std::getline(header, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream words(line);
    std::string keyword;
    words >> keyword;
    if (keyword == "format") {
      words >> format;
    } else if (keyword == "comment") {
      out.comments.push_back(line.size() > 8 ? line.substr(8) : std::string());
    } else if (keyword == "element") {
      Element e;
      words >> e.name >> e.count;
      elements.push_back(e);
    } else if (keyword == "property") {
      if (elements.empty()) return "a property before any element";
      std::string type, name;
      words >> type;
      if (type == "list") {
        elements.back().hasList = true;
        continue;
      }
      words >> name;
      Property p;
      p.name = name;
      p.type = typeOf(type);
      if (p.type == Type::Invalid) return "unknown property type " + type;
      p.offset = elements.back().stride;
      elements.back().stride += sizeOf(p.type);
      elements.back().properties.push_back(p);
    }
  }
  const bool binary = format == "binary_little_endian";
  if (!binary && format != "ascii")
    return "PLY format " + format + " is not supported (binary little endian or ASCII)";

  // ---- the vertex element, after whatever comes before it
  size_t at = end;
  const Element *vertex = nullptr;
  for (const Element &e : elements) {
    if (e.name == "vertex") {
      vertex = &e;
      break;
    }
    if (e.hasList) return "element " + e.name + " before the splats has lists";
    if (binary) at += e.count * e.stride;
    else return "ASCII PLY with elements before the splats is not supported";
  }
  if (!vertex) return "no vertex element";
  if (vertex->hasList) return "the vertex element has lists: not a Gaussian splat PLY";
  if (vertex->count == 0) return "no splats";

  auto find = [&](const std::string &name) -> int {
    for (size_t i = 0; i < vertex->properties.size(); ++i)
      if (vertex->properties[i].name == name) return int(i);
    return -1;
  };
  const char *required[] = {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                            "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"};
  int index[14];
  for (int i = 0; i < 14; ++i) {
    index[i] = find(required[i]);
    if (index[i] < 0) {
      if (i >= 3 && find("red") >= 0)
        return "a coloured point cloud, not Gaussian splats (no " + std::string(required[i]) + ")";
      return std::string("not a Gaussian splat PLY: no ") + required[i];
    }
  }
  int restCount = 0;
  while (find("f_rest_" + std::to_string(restCount)) >= 0) ++restCount;
  const int coefficients = restCount / 3;
  if (restCount % 3 != 0 || vgsunreal::shDegree(coefficients) < 0)
    return "f_rest_* has " + std::to_string(restCount) + " values: not a spherical harmonics degree of 1 to 3";
  std::vector<int> rest(restCount);
  for (int i = 0; i < restCount; ++i) rest[i] = find("f_rest_" + std::to_string(i));
  const int nx = find("nx"), ny = find("ny"), nz = find("nz");
  const bool fileNormals = nx >= 0 && ny >= 0 && nz >= 0;

  const size_t n = vertex->count;
  const size_t P = vertex->properties.size();
  if (binary && at + n * vertex->stride > size) return "the file ends before its splats do";

  // One record's values, as doubles, whichever the encoding.
  std::vector<double> values(P);
  const char *cursor = text + at;
  const char *textEnd = text + size;
  auto readRecord = [&](size_t i) -> bool {
    if (binary) {
      const uint8_t *record = data + at + i * vertex->stride;
      for (size_t k = 0; k < P; ++k)
        values[k] = readBinary(record + vertex->properties[k].offset, vertex->properties[k].type);
      return true;
    }
    for (size_t k = 0; k < P; ++k) {
      while (cursor < textEnd && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) ++cursor;
      if (cursor >= textEnd) return false;
      char *next = nullptr;
      values[k] = std::strtod(cursor, &next);
      if (next == cursor) return false;
      cursor = next;
    }
    return true;
  };

  out.shCoefficients = coefficients;
  out.positions.reserve(n * 3);
  out.rotations.reserve(n * 4);
  out.scales.reserve(n * 3);
  out.radiance.reserve(n * 4);
  std::vector<float> sh; // splat-major while reading: n * coefficients * 3
  sh.reserve(n * size_t(coefficients) * 3);
  if (fileNormals) out.normals.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) {
    if (!readRecord(i)) return "the file ends before its splats do";
    const auto v = [&](int k) { return float(values[size_t(k)]); };
    float p[3] = {v(index[0]), v(index[1]), v(index[2])};
    float s[3] = {std::exp(v(index[7])), std::exp(v(index[8])), std::exp(v(index[9]))};
    float q[4] = {v(index[10]), v(index[11]), v(index[12]), v(index[13])}; // w x y z
    const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    bool finite = ql > 0.0f && std::isfinite(ql);
    for (int c = 0; c < 3; ++c) finite = finite && std::isfinite(p[c]) && std::isfinite(s[c]);
    if (!finite) {
      ++out.dropped;
      continue;
    }
    for (float &c : q) c /= ql;
    out.positions.insert(out.positions.end(), p, p + 3);
    out.scales.insert(out.scales.end(), s, s + 3);
    out.rotations.insert(out.rotations.end(), q, q + 4);
    out.radiance.push_back(v(index[3]));
    out.radiance.push_back(v(index[4]));
    out.radiance.push_back(v(index[5]));
    out.radiance.push_back(sigmoid(v(index[6])));
    for (int k = 0; k < coefficients; ++k)
      for (int c = 0; c < 3; ++c)
        sh.push_back(v(rest[size_t(c * coefficients + k)]));
    if (fileNormals) {
      const float m[3] = {v(nx), v(ny), v(nz)};
      const float ml = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
      const bool valid = ml > 1e-6f && std::isfinite(ml);
      for (int c = 0; c < 3; ++c) out.normals.push_back(valid ? m[c] / ml : 0.0f);
      out.splatsWithNormals += valid ? 1 : 0;
    }
  }
  out.count = out.positions.size() / 3;
  if (out.count == 0) return "no splat with a finite position, scale and rotation";
  if (out.splatsWithNormals == 0) out.normals.clear(); // nx, ny, nz all 0: no normals

  // Planes of coefficients, as the layout reads them.
  if (coefficients) {
    out.sh.resize(out.count * size_t(coefficients) * 3);
    for (size_t i = 0; i < out.count; ++i)
      for (int k = 0; k < coefficients; ++k)
        for (int c = 0; c < 3; ++c)
          out.sh[(size_t(k) * out.count + i) * 3 + c] = sh[(i * size_t(coefficients) + size_t(k)) * 3 + c];
  }
  return std::string();
}

// ---- normals --------------------------------------------------------------------------

/* The renderer's normal word (VGSPacked.usf PackNormal, VGSSplatShading.usf VGSUnpackNormal):
 * octahedral, 16 bits a component, each biased by 1 so that 0 means "no normal". */
inline uint32_t packNormal(const float n[3]) {
  const float l1 = std::max(std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]), 1e-20f);
  float px = n[0] / l1, py = n[1] / l1;
  if (n[2] < 0.0f) {
    const float ox = (1.0f - std::fabs(py)) * (px >= 0.0f ? 1.0f : -1.0f);
    const float oy = (1.0f - std::fabs(px)) * (py >= 0.0f ? 1.0f : -1.0f);
    px = ox;
    py = oy;
  }
  const auto q = [](float v) {
    const float u = std::min(std::max(v * 0.5f + 0.5f, 0.0f), 1.0f);
    return uint32_t(u * 65534.0f + 0.5f) + 1u;
  };
  return q(px) | (q(py) << 16);
}

inline void unpackNormal(uint32_t w, float n[3]) {
  float px = (float(w & 65535u) - 1.0f) / 65534.0f * 2.0f - 1.0f;
  float py = (float(w >> 16) - 1.0f) / 65534.0f * 2.0f - 1.0f;
  const float z = 1.0f - std::fabs(px) - std::fabs(py);
  if (z < 0.0f) {
    const float ox = (1.0f - std::fabs(py)) * (px >= 0.0f ? 1.0f : -1.0f);
    const float oy = (1.0f - std::fabs(px)) * (py >= 0.0f ? 1.0f : -1.0f);
    px = ox;
    py = oy;
  }
  const float l = std::sqrt(px * px + py * py + z * z);
  n[0] = px / l;
  n[1] = py / l;
  n[2] = z / l;
}

// ---- what the asset keeps ---------------------------------------------------------------

/* The renderer's cluster record (VGSSplatCluster.h, a copy of Epic's FGaussianSplatCluster):
 * three float4s. Level 0, error 0 and no parent: one level of detail, culled by its box. */
struct Cluster {
  float boundsMin[3];
  uint32_t splatOffset;
  float boundsMax[3];
  uint32_t splatCount;
  uint32_t level = 0;
  float error = 0.0f;
  uint32_t parentIndex = ~0u;
  uint32_t padding = 0;
};
static_assert(sizeof(Cluster) == 48, "the renderer reads three float4s per cluster");

/* Epic's cluster bounds: a splat reaches sqrt(2) * 2 of its largest scale from its centre
 * (GaussianSplatCommon::SplatBoundsScale). */
constexpr float SplatBoundsScale = 1.41421356f * 2.0f;

struct Built {
  uint32_t count = 0;
  int shCoefficients = 0;
  uint32_t stride = 0;              // float layout
  std::vector<uint8_t> records;     // count * stride, in cluster order
  std::vector<uint32_t> normals;    // count words, 0 for none; empty when no splat has one
  std::vector<Cluster> clusters;
  float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0}; // splat centres
  std::vector<uint32_t> order;      // record i is the PLY's splat order[i]
};

namespace detail {
// 21 bits of v spread to every third bit.
inline uint64_t spread3(uint64_t v) {
  v &= 0x1FFFFF;
  v = (v | v << 32) & 0x1F00000000FFFFull;
  v = (v | v << 16) & 0x1F0000FF0000FFull;
  v = (v | v << 8) & 0x100F00F00F00F00Full;
  v = (v | v << 4) & 0x10C30C30C30C30C3ull;
  v = (v | v << 2) & 0x1249249249249249ull;
  return v;
}
} // namespace detail

/* Orders the splats along a Morton curve (so neighbours in the buffer are neighbours in
 * space, as Epic's importer orders them), cuts them into clusters of `perCluster`, packs
 * the float records and, given normals (3 per splat, 0 for none; or null), their words. */
inline void build(const Splats &s, const float *normals, Built &out, uint32_t perCluster = 64) {
  out = Built();
  out.count = uint32_t(s.count);
  out.shCoefficients = s.shCoefficients;
  out.stride = vgsunreal::floatStride(s.shCoefficients);
  if (s.count == 0) return;

  float lo[3], hi[3];
  for (int c = 0; c < 3; ++c) lo[c] = hi[c] = s.positions[size_t(c)];
  for (size_t i = 0; i < s.count; ++i)
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min(lo[c], s.positions[i * 3 + c]);
      hi[c] = std::max(hi[c], s.positions[i * 3 + c]);
    }
  std::memcpy(out.boundsMin, lo, sizeof(lo));
  std::memcpy(out.boundsMax, hi, sizeof(hi));

  std::vector<uint64_t> key(s.count);
  for (size_t i = 0; i < s.count; ++i) {
    uint64_t k = 0;
    for (int c = 0; c < 3; ++c) {
      const float range = hi[c] - lo[c];
      const float u = range > 0.0f ? (s.positions[i * 3 + c] - lo[c]) / range : 0.0f;
      k |= detail::spread3(uint64_t(std::min(std::max(u, 0.0f), 1.0f) * 2097151.0f)) << (2 - c);
    }
    key[i] = k;
  }
  out.order.resize(s.count);
  std::iota(out.order.begin(), out.order.end(), 0u);
  std::stable_sort(out.order.begin(), out.order.end(), [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });

  // The splats in that order, then packed as VGS packs a frame.
  Splats sorted;
  sorted.count = s.count;
  sorted.shCoefficients = s.shCoefficients;
  sorted.positions.resize(s.count * 3);
  sorted.scales.resize(s.count * 3);
  sorted.rotations.resize(s.count * 4);
  sorted.radiance.resize(s.count * 4);
  sorted.sh.resize(s.sh.size());
  for (size_t i = 0; i < s.count; ++i) {
    const size_t from = out.order[i];
    std::memcpy(&sorted.positions[i * 3], &s.positions[from * 3], 12);
    std::memcpy(&sorted.scales[i * 3], &s.scales[from * 3], 12);
    std::memcpy(&sorted.rotations[i * 4], &s.rotations[from * 4], 16);
    std::memcpy(&sorted.radiance[i * 4], &s.radiance[from * 4], 16);
    for (int k = 0; k < s.shCoefficients; ++k)
      std::memcpy(&sorted.sh[(size_t(k) * s.count + i) * 3], &s.sh[(size_t(k) * s.count + from) * 3], 12);
  }
  out.records.resize(s.count * out.stride);
  vgsunreal::packFloat(sorted.view(), 0, s.count, out.records.data());

  if (normals) {
    out.normals.resize(s.count);
    bool any = false;
    for (size_t i = 0; i < s.count; ++i) {
      const float *n = normals + size_t(out.order[i]) * 3;
      const bool set = n[0] != 0.0f || n[1] != 0.0f || n[2] != 0.0f;
      out.normals[i] = set ? packNormal(n) : 0u;
      any = any || set;
    }
    if (!any) out.normals.clear();
  }

  perCluster = std::max(perCluster, 1u);
  for (uint32_t first = 0; first < out.count; first += perCluster) {
    Cluster c;
    c.splatOffset = first;
    c.splatCount = std::min(perCluster, out.count - first);
    for (int a = 0; a < 3; ++a) {
      c.boundsMin[a] = std::numeric_limits<float>::max();
      c.boundsMax[a] = -std::numeric_limits<float>::max();
    }
    for (uint32_t i = first; i < first + c.splatCount; ++i) {
      const float *sc = &sorted.scales[size_t(i) * 3];
      const float extent = SplatBoundsScale * std::max(std::max(std::fabs(sc[0]), std::fabs(sc[1])), std::fabs(sc[2]));
      for (int a = 0; a < 3; ++a) {
        c.boundsMin[a] = std::min(c.boundsMin[a], sorted.positions[size_t(i) * 3 + a] - extent);
        c.boundsMax[a] = std::max(c.boundsMax[a], sorted.positions[size_t(i) * 3 + a] + extent);
      }
    }
    out.clusters.push_back(c);
  }
}

/* Float records back into splats, for packing them in another layout (quantized). */
inline void fromFloatRecords(const uint8_t *records, size_t count, int shCoefficients, Splats &out) {
  out = Splats();
  out.count = count;
  out.shCoefficients = shCoefficients;
  const size_t stride = vgsunreal::floatStride(shCoefficients);
  out.positions.resize(count * 3);
  out.scales.resize(count * 3);
  out.rotations.resize(count * 4);
  out.radiance.resize(count * 4);
  out.sh.resize(count * size_t(shCoefficients) * 3);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t *r = records + i * stride;
    float q[4]; // xyzw in the record
    std::memcpy(&out.positions[i * 3], r + vgsunreal::FloatPositionOffset, 12);
    std::memcpy(&out.scales[i * 3], r + vgsunreal::FloatScaleOffset, 12);
    std::memcpy(q, r + vgsunreal::FloatRotationOffset, 16);
    out.rotations[i * 4 + 0] = q[3];
    out.rotations[i * 4 + 1] = q[0];
    out.rotations[i * 4 + 2] = q[1];
    out.rotations[i * 4 + 3] = q[2];
    std::memcpy(&out.radiance[i * 4], r + vgsunreal::FloatColorOpacityOffset, 16);
    for (int k = 0; k < shCoefficients; ++k)
      std::memcpy(&out.sh[(size_t(k) * count + i) * 3], r + vgsunreal::FloatBaseStride + size_t(k) * 12, 12);
  }
}

} // namespace vgsstatic
