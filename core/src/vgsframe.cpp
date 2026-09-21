#include "vgsframe.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace vgs {
namespace {
uint16_t readU16(const char *p);
uint64_t readU64(const char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= uint64_t(uint8_t(p[i])) << (i * 8);
  return v;
}
uint32_t readU32(const char *p) {
  return uint32_t(readU16(p)) | (uint32_t(readU16(p + 2)) << 16);
}
uint16_t readU16(const char *p) {
  return uint8_t(p[0]) | (uint16_t(uint8_t(p[1])) << 8);
}

double readF64(const char *p) {
  uint64_t bits = readU64(p);
  double v;
  std::memcpy(&v, &bits, 8);
  return v;
}

float readF32(const char *p) {
  uint32_t bits = readU32(p);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

float halfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1Fu;
  const uint32_t mantissa = h & 0x3FFu;

  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // Subnormal half: renormalise into a float exponent.
      uint32_t e = 0, m = mantissa;
      while (!(m & 0x400u)) {
        m <<= 1;
        ++e;
      }
      m &= 0x3FFu;
      bits = sign | ((127 - 15 - e + 1) << 23) | (m << 13);
    }
  } else if (exponent == 0x1Fu) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
  }

  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

uint16_t floatToHalf(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;

  if (exponent >= 0x1F)
    return uint16_t(sign | 0x7C00u); // overflow to infinity
  if (exponent <= 0) {
    if (exponent < -10)
      return uint16_t(sign); // underflow to zero
    mantissa |= 0x800000u;
    const uint32_t shift = uint32_t(14 - exponent);
    const uint32_t rounded = (mantissa + (1u << (shift - 1))) >> shift;
    return uint16_t(sign | rounded);
  }
  // Round to nearest, ties away from zero. The GPU that the format was measured
  // against truncates instead, which shifts rotations by about 4e-4; that is
  // far below anything an export or a color edit cares about. See
  // MINT_FORMAT.md 6.6.
  const uint32_t rounded = (mantissa + 0x1000u) >> 13;
  if (rounded & 0x400u)
    return uint16_t(sign | (uint32_t(exponent + 1) << 10));
  return uint16_t(sign | (uint32_t(exponent) << 10) | rounded);
}

float readHalf(const char *p) { return halfToFloat(readU16(p)); }

// ------------------------------------------------------------ format helpers

// 21 bits per axis, sharing one scalar range. See MINT_FORMAT.md 6.4.
void unpackPosition(uint64_t word, double lo, double hi, float *xyz) {
  const double scale = (hi - lo) / 2097151.0;
  xyz[0] = float(lo + double((word >> 43) & 0x1FFFFFull) * scale);
  xyz[1] = float(lo + double((word >> 22) & 0x1FFFFFull) * scale);
  xyz[2] = float(lo + double((word >> 1) & 0x1FFFFFull) * scale);
}

// Smallest-three variant, stored wxyz, returned xyzw. See MINT_FORMAT.md 6.6.
void unpackQuaternion(uint32_t word, float *xyzw) {
  static const float kMax[3] = {1023.0f, 1023.0f, 511.0f};
  static const int kShift[3] = {20, 10, 1};
  static const uint32_t kMask[3] = {1023u, 1023u, 511u};
  const float kInvRoot2 = 0.70710678118654752f;

  float kept[3];
  for (int j = 0; j < 3; ++j) {
    const uint32_t q = (word >> kShift[j]) & kMask[j];
    kept[j] = (2.0f * float(q) / kMax[j] - 1.0f) * kInvRoot2;
  }

  const float sumSquares =
      kept[0] * kept[0] + kept[1] * kept[1] + kept[2] * kept[2];
  const float omitted = std::sqrt(std::max(0.0f, 1.0f - sumSquares));
  const int largest = int(word >> 30);

  float wxyz[4];
  for (int c = 0, j = 0; c < 4; ++c)
    wxyz[c] = (c == largest) ? omitted : kept[j++];

  const float sign = 1.0f - 2.0f * float(word & 1u);
  xyzw[0] = wxyz[1] * sign;
  xyzw[1] = wxyz[2] * sign;
  xyzw[2] = wxyz[3] * sign;
  xyzw[3] = wxyz[0] * sign;
}

// Splats are sorted by term count and the boundaries are cumulative, so a
// splat's terms are found by arithmetic rather than by a stored offset.
// See 6.5.
struct RankTable {
  std::vector<uint32_t> rank;   // 1-based number of terms
  std::vector<uint64_t> offset; // index of the splat's first term
  uint64_t totalTerms = 0;
};

RankTable buildRanks(const char *boundaryBytes, int boundaryCount,
                     uint64_t splats) {
  std::vector<uint64_t> boundaries(boundaryCount);
  for (int j = 0; j < boundaryCount; ++j)
    boundaries[j] = readU32(boundaryBytes + 4 * j);

  std::vector<uint64_t> starts(boundaryCount);
  starts[0] = 0;
  for (int j = 1; j < boundaryCount; ++j)
    starts[j] = boundaries[j - 1];

  std::vector<uint64_t> rankStart(boundaryCount);
  rankStart[0] = 0;
  for (int j = 1; j < boundaryCount; ++j)
    rankStart[j] =
        rankStart[j - 1] + uint64_t(j) * (boundaries[j - 1] - starts[j - 1]);

  RankTable table;
  table.rank.resize(int(splats));
  table.offset.resize(int(splats));
  for (uint64_t id = 0; id < splats; ++id) {
    int below = 0;
    while (below < boundaryCount && boundaries[below] <= id)
      ++below;
    const int r = below + 1;
    const int j = r - 1;
    if (j >= boundaryCount)
      throw Error("invalid rank boundaries");
    table.rank[int(id)] = uint32_t(r);
    table.offset[int(id)] = rankStart[j] + uint64_t(r) * (id - starts[j]);
    table.totalTerms =
        std::max(table.totalTerms, table.offset[int(id)] + uint64_t(r));
  }
  return table;
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
constexpr double sphericalHarmonicC0() { return 0.28209479177387814; }
} // namespace
void FrameDecoder::buildBasis(const Block &shared, uint64_t frame, float alpha,
                              bool needPosition, bool needRotation,
                              SharedBasis *out) const {
  const uint64_t samples = shared.samples();
  const uint64_t intervals = shared.intervals;

  // SH0: [entries][samples][3] f16, interpolated with alpha.
  {
    const uint64_t n = shared.sh0Entries;
    const char *base = shared.array("sh0_trajectories");
    out->sh0.resize(int(n * 3));
    for (uint64_t i = 0; i < n; ++i) {
      const char *row = base + (i * samples) * 6;
      for (int c = 0; c < 3; ++c) {
        const float a = readHalf(row + frame * 6 + uint64_t(c) * 2);
        const float b = readHalf(row + (frame + 1) * 6 + uint64_t(c) * 2);
        out->sh0[int(i * 3 + uint64_t(c))] = a + (b - a) * alpha;
      }
    }
  }

  // Opacity: [entries][samples] f16.
  {
    const uint64_t n = shared.opacityEntries;
    const char *base = shared.array("opacity_trajectories");
    out->opacity.resize(int(n));
    for (uint64_t i = 0; i < n; ++i) {
      const char *row = base + (i * samples) * 2;
      const float a = readHalf(row + frame * 2);
      const float b = readHalf(row + (frame + 1) * 2);
      out->opacity[int(i)] = a + (b - a) * alpha;
    }
  }

  if (needPosition) {
    const uint64_t n = shared.positionEntries;
    const char *base = shared.array("position_trajectories");
    out->position.resize(int(n * 3));
    for (uint64_t i = 0; i < n; ++i) {
      const char *row = base + (i * samples) * 8;
      float a[3], b[3];
      unpackPosition(readU64(row + frame * 8), shared.trajectoryMin,
                     shared.trajectoryMax, a);
      unpackPosition(readU64(row + (frame + 1) * 8), shared.trajectoryMin,
                     shared.trajectoryMax, b);
      for (int c = 0; c < 3; ++c)
        out->position[int(i * 3 + uint64_t(c))] = a[c] + (b[c] - a[c]) * alpha;
    }
  }

  if (needRotation) {
    const uint64_t n = shared.rotationEntries;
    const char *initial = shared.array("rotation_initial");
    const char *lut = shared.array("rotation_delta_lut");
    const uint8_t *indices = reinterpret_cast<const uint8_t *>(
        shared.array("rotation_delta_indices"));

    float deltaLut[256];
    for (int i = 0; i < 256; ++i)
      deltaLut[i] = readF32(lut + 4 * i);

    out->rotation.resize(int(n * 4));
    for (uint64_t i = 0; i < n; ++i) {
      // The accumulator stays float32; only the stored samples are rounded
      // back to half. Rounding each step instead is visibly different.
      float acc[4];
      for (int c = 0; c < 4; ++c)
        acc[c] = readHalf(initial + (i * 4 + uint64_t(c)) * 2);

      const uint8_t *row = indices + i * intervals * 4;
      for (uint64_t step = 0; step < frame; ++step)
        for (int c = 0; c < 4; ++c)
          acc[c] += deltaLut[row[step * 4 + uint64_t(c)]];

      float q0[4], q1[4];
      for (int c = 0; c < 4; ++c) {
        q0[c] = halfToFloat(floatToHalf(acc[c]));
        q1[c] = halfToFloat(
            floatToHalf(acc[c] + deltaLut[row[frame * 4 + uint64_t(c)]]));
      }
      // Stored wxyz, emitted xyzw.
      static const int kOrder[4] = {1, 2, 3, 0};
      for (int c = 0; c < 4; ++c) {
        const int s = kOrder[c];
        const float v = q0[s] + (q1[s] - q0[s]) * alpha;
        out->rotation[int(i * 4 + uint64_t(c))] = halfToFloat(floatToHalf(v));
      }
    }
  }
}

void FrameDecoder::decodeGroupColor(const Block &group, const Block &shared,
                                    const SharedBasis &basis,
                                    std::vector<float> *colorDc) const {
  const uint64_t n = group.splats;
  const uint64_t stride = shared.sh0Entries / 5; // 1024 rows per stage
  const char *lutBytes = shared.array("sh0_base_lut");
  const char *baseIndices = group.array("sh0_base_indices");
  const char *words = group.array("sh0_rq_indices");

  float lut[256];
  for (int i = 0; i < 256; ++i)
    lut[i] = readHalf(lutBytes + 2 * i);

  colorDc->resize(int(n * 3));
  const float c0 = float(sphericalHarmonicC0());
  for (uint64_t i = 0; i < n; ++i) {
    const uint8_t *idx = reinterpret_cast<const uint8_t *>(baseIndices + i * 3);
    float sh0[3] = {lut[idx[0]], lut[idx[1]], lut[idx[2]]};

    const uint64_t word = readU64(words + i * 8);
    for (int k = 0; k < 5; ++k) {
      const uint64_t stage = (word >> (12 * k)) & 0xFFFull;
      const float *row = basis.sh0.data() + (uint64_t(k) * stride + stage) * 3;
      for (int c = 0; c < 3; ++c)
        sh0[c] += row[c];
    }
    for (int c = 0; c < 3; ++c)
      (*colorDc)[int(i * 3 + uint64_t(c))] = 0.5f + c0 * sh0[c];
  }
}

void FrameDecoder::evaluatePositions(double normalized,
                                     std::vector<float> *out) const {
  if (!std::isfinite(normalized) || normalized < 0 || normalized >= 1)
    throw Error("time must be in [0,1)");
  const Block *shared = &blocks.front();
  const uint64_t intervals = shared->intervals, samples = intervals + 1;
  const float r = float(normalized), sampleTime = r * float(intervals);
  const uint64_t frame =
      std::min<uint64_t>(uint64_t(std::floor(sampleTime)), intervals - 1);
  const float alpha = sampleTime - float(frame);

  bool needDictionary = false;
  uint64_t total = 0;
  for (size_t gi = 1; gi < blocks.size(); ++gi) {
    needDictionary |= !blocks[gi].positionPerSample;
    total += blocks[gi].splats;
  }

  // Only the position dictionary is interpolated here. Skipping the SH0, opacity and
  // rotation tables is most of what makes this cheaper than a whole frame.
  std::vector<float> dictionary;
  if (needDictionary) {
    const uint64_t n = shared->positionEntries;
    const char *base = shared->array("position_trajectories");
    dictionary.resize(size_t(n * 3));
    for (uint64_t i = 0; i < n; ++i) {
      const char *row = base + (i * samples) * 8;
      float a[3], b[3];
      unpackPosition(readU64(row + frame * 8), shared->trajectoryMin,
                     shared->trajectoryMax, a);
      unpackPosition(readU64(row + (frame + 1) * 8), shared->trajectoryMin,
                     shared->trajectoryMax, b);
      for (int c = 0; c < 3; ++c)
        dictionary[size_t(i * 3 + uint64_t(c))] = a[c] + (b[c] - a[c]) * alpha;
    }
  }

  out->resize(size_t(total * 3));
  uint64_t written = 0;
  for (size_t gi = 1; gi < blocks.size(); ++gi) {
    const Block &group = blocks[gi];
    const uint64_t n = group.splats;
    if (group.positionPerSample) {
      const char *base = group.array("position_samples");
      for (uint64_t i = 0; i < n; ++i) {
        const char *row = base + (i * samples) * 8;
        float a[3], b[3];
        unpackPosition(readU64(row + frame * 8), group.positionMin,
                       group.positionMax, a);
        unpackPosition(readU64(row + (frame + 1) * 8), group.positionMin,
                       group.positionMax, b);
        for (int c = 0; c < 3; ++c)
          (*out)[size_t((written + i) * 3 + uint64_t(c))] =
              a[c] + (b[c] - a[c]) * r;
      }
    } else {
      const char *baseBytes = group.array("position_base");
      const RankTable ranks =
          buildRanks(group.array("position_rank_boundaries"), 4, n);
      const char *terms = group.array("position_rq_coefficients");
      for (uint64_t i = 0; i < n; ++i) {
        float p[3];
        unpackPosition(readU64(baseBytes + i * 8), group.positionMin,
                       group.positionMax, p);
        const uint32_t rank = ranks.rank[int(i)];
        const uint64_t first = ranks.offset[int(i)];
        for (uint32_t k = 0; k < rank; ++k) {
          const uint32_t packed = readU32(terms + (first + k) * 4);
          const float weight = halfToFloat(uint16_t(packed >> 16));
          const float *row = dictionary.data() + uint64_t(packed & 0xFFFFu) * 3;
          for (int c = 0; c < 3; ++c)
            p[c] += weight * row[c];
        }
        for (int c = 0; c < 3; ++c)
          (*out)[size_t((written + i) * 3 + uint64_t(c))] = p[c];
      }
    }
    written += n;
  }
}

Frame FrameDecoder::evaluate(double normalized, bool includeSh) const {
  Frame result;
  evaluateInto(normalized, includeSh, &result);
  return result;
}

void FrameDecoder::evaluateInto(double normalized, bool includeSh, Frame *out) const {
  if (!std::isfinite(normalized) || normalized < 0 || normalized >= 1)
    throw Error("time must be in [0,1)");
  const Block *shared = &blocks.front();
  std::vector<const Block *> groups;
  for (size_t i = 1; i < blocks.size(); ++i)
    groups.push_back(&blocks[i]);
  const uint64_t intervals = shared->intervals, samples = intervals + 1;
  const float r = float(normalized), sampleTime = r * float(intervals);
  const uint64_t frame =
      std::min<uint64_t>(uint64_t(std::floor(sampleTime)), intervals - 1);
  const float alpha = sampleTime - float(frame);
  bool needPosition = false, needRotation = false;
  for (const Block *group : groups) {
    needPosition |= !group->positionPerSample;
    needRotation |= !group->rotationPerSample;
  }

  SharedBasis basis;
  buildBasis(*shared, frame, alpha, needPosition, needRotation, &basis);

  uint64_t total = 0;
  for (const Block *group : groups)
    total += group->splats;

  out->seconds = normalized * double(intervals) * secondsPerTick;
  out->chunkIndex = 0;
  out->sampleIndex = int(frame);
  out->sampleAlpha = alpha;
  out->count = total;
  out->position.resize(int(total * 3));
  out->rotation.resize(int(total * 4));
  out->scale.resize(int(total * 3));
  out->opacity.resize(int(total));
  out->colorDc.resize(int(total * 3));
  out->active.resize(int(total));

  // A capture written at a lower SH degree carries fewer coefficients: three at
  // degree 1, eight at degree 2, fifteen at degree 3. The planes it stores hold
  // three coefficients each, so degree 2 has one spare slot that is not a whole
  // band and is not evaluated.
  // The degree is implied by the data rather than passed in: the index array holds
  // one 32-bit word per plane per splat, so its size names the plane count.
  uint64_t shPlanes = 0;
  for (const Block *group : groups) {
    const auto it = group->arrays.find("sh_static_indices");
    if (it == group->arrays.end() || !group->splats)
      continue;
    shPlanes = it->second.size() / (group->splats * 4);
    break;
  }
  if (shPlanes != 1 && shPlanes != 3 && shPlanes != 5)
    shPlanes = 0;
  const uint64_t shCoefficients =
      shPlanes == 5 ? 15 : shPlanes == 3 ? 8 : shPlanes * 3;
  out->shCoefficients = int(includeSh ? shCoefficients : 0);
  if (includeSh && shCoefficients)
    out->shRest.resize(size_t(total * shCoefficients * 3));
  else
    out->shRest.clear();

  float scaleLut[256];
  {
    const char *bytes = shared->array("scale_lut");
    for (int i = 0; i < 256; ++i)
      scaleLut[i] = readF32(bytes + 4 * i);
  }

  uint64_t written = 0;
  for (const Block *group : groups) {
    const uint64_t n = group->splats;

    // --- positions -------------------------------------------------------
    if (group->positionPerSample) {
      const char *base = group->array("position_samples");
      for (uint64_t i = 0; i < n; ++i) {
        const char *row = base + (i * samples) * 8;
        float a[3], b[3];
        unpackPosition(readU64(row + frame * 8), group->positionMin,
                       group->positionMax, a);
        unpackPosition(readU64(row + (frame + 1) * 8), group->positionMin,
                       group->positionMax, b);
        // This build interpolates per-sample positions with normalized
        // chunk time, not with the fraction inside the interval. It looks
        // like a defect, but it is what the runtime does. See 6.4.
        for (int c = 0; c < 3; ++c)
          out->position[int((written + i) * 3 + uint64_t(c))] =
              a[c] + (b[c] - a[c]) * r;
      }
    } else {
      const char *baseBytes = group->array("position_base");
      const RankTable ranks =
          buildRanks(group->array("position_rank_boundaries"), 4, n);
      const char *terms = group->array("position_rq_coefficients");
      for (uint64_t i = 0; i < n; ++i) {
        float p[3];
        unpackPosition(readU64(baseBytes + i * 8), group->positionMin,
                       group->positionMax, p);
        const uint32_t rank = ranks.rank[int(i)];
        const uint64_t first = ranks.offset[int(i)];
        for (uint32_t k = 0; k < rank; ++k) {
          const uint32_t packed = readU32(terms + (first + k) * 4);
          const uint32_t index = packed & 0xFFFFu;
          // The high half is the weight's raw binary16 bits, not a
          // normalised integer: real weights run well outside [-1,1].
          const float weight = halfToFloat(uint16_t(packed >> 16));
          const float *row = basis.position.data() + uint64_t(index) * 3;
          for (int c = 0; c < 3; ++c)
            p[c] += weight * row[c];
        }
        for (int c = 0; c < 3; ++c)
          out->position[int((written + i) * 3 + uint64_t(c))] = p[c];
      }
    }

    // --- rotations -------------------------------------------------------
    if (group->rotationPerSample) {
      const char *base = group->array("rotation_samples");
      for (uint64_t i = 0; i < n; ++i) {
        const char *row = base + (i * samples) * 4;
        float q0[4], q1[4];
        unpackQuaternion(readU32(row + frame * 4), q0);
        unpackQuaternion(readU32(row + (frame + 1) * 4), q1);
        float dot = 0.0f;
        for (int c = 0; c < 4; ++c)
          dot += q0[c] * q1[c];
        const float sign = (dot < 0.0f) ? -1.0f : 1.0f;
        float q[4], norm = 0.0f;
        for (int c = 0; c < 4; ++c) {
          q[c] = q0[c] + (q1[c] * sign - q0[c]) * alpha;
          norm += q[c] * q[c];
        }
        norm = std::max(std::sqrt(norm), 1e-20f);
        for (int c = 0; c < 4; ++c)
          out->rotation[int((written + i) * 4 + uint64_t(c))] = q[c] / norm;
      }
    } else {
      const char *baseBytes = group->array("rotation_base");
      const RankTable ranks =
          buildRanks(group->array("rotation_rank_boundaries"), 5, n);
      const char *terms = group->array("rotation_rq_indices");
      for (uint64_t i = 0; i < n; ++i) {
        float base[4];
        unpackQuaternion(readU32(baseBytes + i * 4), base);

        float residual[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const uint32_t rank = ranks.rank[int(i)];
        const uint64_t first = ranks.offset[int(i)];
        for (uint32_t k = 0; k < rank; ++k) {
          const uint16_t index = readU16(terms + (first + k) * 2);
          const float *row = basis.rotation.data() + uint64_t(index) * 4;
          for (int c = 0; c < 4; ++c)
            residual[c] += row[c];
        }

        float normSq = 0.0f;
        for (int c = 0; c < 4; ++c)
          normSq += residual[c] * residual[c];

        float q[4];
        if (normSq < 1e-12f) {
          for (int c = 0; c < 4; ++c)
            q[c] = base[c];
        } else {
          const float inv = 1.0f / std::sqrt(normSq);
          for (int c = 0; c < 4; ++c)
            residual[c] *= inv;
          // normalize(residual * base), quaternions in xyzw.
          const float *v = residual;
          const float w = residual[3];
          const float *bv = base;
          const float bw = base[3];
          q[0] = w * bv[0] + bw * v[0] + (v[1] * bv[2] - v[2] * bv[1]);
          q[1] = w * bv[1] + bw * v[1] + (v[2] * bv[0] - v[0] * bv[2]);
          q[2] = w * bv[2] + bw * v[2] + (v[0] * bv[1] - v[1] * bv[0]);
          q[3] = w * bw - (v[0] * bv[0] + v[1] * bv[1] + v[2] * bv[2]);
        }
        float norm = 0.0f;
        for (int c = 0; c < 4; ++c)
          norm += q[c] * q[c];
        norm = std::max(std::sqrt(norm), 1e-20f);
        for (int c = 0; c < 4; ++c)
          out->rotation[int((written + i) * 4 + uint64_t(c))] = q[c] / norm;
      }
    }

    // --- scale, opacity, lifetime, color ---------------------------------
    {
      const uint8_t *scaleIdx =
          reinterpret_cast<const uint8_t *>(group->array("scale_indices"));
      const char *opacityWords = group->array("opacity_rq_indices");
      const uint8_t *lifetimes =
          reinterpret_cast<const uint8_t *>(group->array("lifetimes"));
      const uint64_t opacityStride = shared->opacityEntries / 5;

      for (uint64_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c)
          out->scale[int((written + i) * 3 + uint64_t(c))] =
              scaleLut[scaleIdx[i * 3 + uint64_t(c)]];

        const uint64_t word = readU64(opacityWords + i * 8);
        float opacity = 0.0f;
        for (int k = 0; k < 5; ++k) {
          const uint64_t stage = (word >> (12 * k)) & 0xFFFull;
          opacity += basis.opacity[int(uint64_t(k) * opacityStride + stage)];
        }
        out->opacity[int(written + i)] = clamp01(opacity);

        const uint8_t begin = lifetimes[i * 2];
        const uint8_t end = lifetimes[i * 2 + 1];
        out->active[int(written + i)] =
            (frame >= begin && frame + 1 <= end) ? uint8_t(1) : uint8_t(0);
      }
    }

    {
      std::vector<float> color;
      decodeGroupColor(*group, *shared, basis, &color);
      std::memcpy(out->colorDc.data() + written * 3, color.data(),
                  size_t(n * 3) * sizeof(float));
    }

    // --- higher-order spherical harmonics --------------------------------
    if (includeSh && shCoefficients) {
      const char *staticBytes = shared->array("sh_static_codebooks");
      const char *temporalBytes = shared->array("sh_temporal_codebooks");
      const char *staticIdx = group->array("sh_static_indices");
      const char *temporalIdx = group->array("sh_temporal_indices");
      const uint64_t ns = shared->shStaticEntries;
      const uint64_t nt = shared->shTemporalEntries;
      // The temporal codebook is read at `frame` with no interpolation to the
      // next sample, unlike every other temporal table. See 6.8.
      const char *temporalFrame =
          temporalBytes + frame * int64_t(shPlanes) * 3 * nt * 3 * 2;
      static const int kShift[3] = {20, 10, 0};

      for (uint64_t g = 0; g < shPlanes; ++g) {
        // The index arrays are plane-major: all N words of group 0, then
        // all N of group 1, and so on.
        const char *sPlane = staticIdx + g * n * 4;
        const char *tPlane = temporalIdx + g * n * 4;
        for (uint64_t i = 0; i < n; ++i) {
          const uint32_t sWord = readU32(sPlane + i * 4);
          const uint32_t tWord = readU32(tPlane + i * 4);
          for (int k = 0; k < 3; ++k) {
            const uint32_t si = (sWord >> kShift[k]) & 0x3FFu;
            const uint32_t ti = (tWord >> kShift[k]) & 0x3FFu;
            const char *sEntry =
                staticBytes + (((g * 3 + uint64_t(k)) * ns) + si) * 3 * 2;
            const char *tEntry =
                temporalFrame + (((g * 3 + uint64_t(k)) * nt) + ti) * 3 * 2;
            const uint64_t coefficient = g * 3 + uint64_t(k);
            if (coefficient >= shCoefficients)
              break;
            for (int c = 0; c < 3; ++c) {
              out->shRest[int((written + i) * shCoefficients * 3 +
                              coefficient * 3 + uint64_t(c))] =
                  readHalf(sEntry + c * 2) + readHalf(tEntry + c * 2);
            }
          }
        }
      }
    }

    written += n;
  }


}

const char *FrameDecoder::Block::array(const char *name) const {
  auto it = arrays.find(name);
  if (it == arrays.end())
    throw Error(std::string("missing attribute: ") + name);
  return reinterpret_cast<const char *>(it->second.data());
}
FrameDecoder::FrameDecoder(const DecodedChunk &chunk, double tick)
    : secondsPerTick(tick) {
  if (chunk.groups.size() < 2)
    throw Error("no splat groups");
  for (const auto &g : chunk.groups) {
    Block b;
    b.splats = g.splats;
    b.intervals = g.intervals;
    b.positionPerSample = (g.flags & 1) != 0;
    b.rotationPerSample = (g.flags & 2) != 0;
    b.shStaticEntries = g.counts[0];
    b.shTemporalEntries = g.counts[1];
    b.sh0Entries = g.counts[2];
    b.opacityEntries = g.counts[3];
    b.rotationEntries = g.counts[4];
    b.positionEntries = g.counts[5];
    b.positionMin = g.positionMin;
    b.positionMax = g.positionMax;
    b.trajectoryMin = g.trajectoryMin;
    b.trajectoryMax = g.trajectoryMax;
    if (!b.intervals || b.intervals > 255)
      throw Error("invalid interval count");
    blocks.push_back(std::move(b));
  }
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> covered;
  for (const auto &item : chunk.pages) {
    const auto &p = item.descriptor;
    if (p.group >= blocks.size() || !p.spec.rows ||
        item.bytes.size() != mgs::attributeSize(p.spec) ||
        p.firstRow > p.totalRows || p.spec.rows > p.totalRows - p.firstRow)
      throw Error("invalid decoded page");
    auto &end = covered[{p.group, p.attribute}];
    if (end != p.firstRow)
      throw Error("non-contiguous decoded pages");
    end += p.spec.rows;
    const auto stride = item.bytes.size() / p.spec.rows;
    if (p.totalRows > 1073741824 / stride)
      throw Error("attribute too large");
    auto &dest = blocks[p.group].arrays[attributeName(p.attribute)];
    dest.resize(size_t(p.totalRows * stride));
    if (p.spec.kind == 3) {
      // SH index planes: five at degree 3, three at degree 2, one at degree 1.
      // A capture from before the degree could be chosen leaves the count at zero.
      const size_t planes = p.spec.width ? size_t(p.spec.width) : 5;
      for (size_t plane = 0; plane < planes; ++plane)
        std::memcpy(dest.data() + (plane * p.totalRows + p.firstRow) * 4,
                    item.bytes.data() + plane * p.spec.rows * 4,
                    size_t(p.spec.rows * 4));
    } else
      std::memcpy(dest.data() + p.firstRow * stride, item.bytes.data(),
                  item.bytes.size());
  }
  for (const auto &item : chunk.pages)
    if (covered[{item.descriptor.group, item.descriptor.attribute}] !=
        item.descriptor.totalRows)
      throw Error("incomplete attribute");
  // Validate all shapes and dictionary indices before any pointer-based
  // evaluation.
  auto require = [](const Block &b, const char *name, uint64_t bytes) {
    auto it = b.arrays.find(name);
    if (it == b.arrays.end() || it->second.size() != bytes)
      throw Error(std::string("invalid attribute shape: ") + name);
  };
  const auto &shared = blocks[0];
  const auto samples = shared.samples();
  for (auto n :
       {shared.shStaticEntries, shared.shTemporalEntries, shared.sh0Entries,
        shared.opacityEntries, shared.rotationEntries, shared.positionEntries})
    if (n > 65536)
      throw Error("dictionary too large");
  if (!shared.sh0Entries || shared.sh0Entries % 5 || !shared.opacityEntries ||
      shared.opacityEntries % 5)
    throw Error("invalid staged dictionary");
  require(shared, "scale_lut", 1024);
  require(shared, "sh0_base_lut", 512);
  require(shared, "sh0_trajectories", shared.sh0Entries * samples * 6);
  require(shared, "opacity_trajectories", shared.opacityEntries * samples * 2);
  auto checkRq = [&](const Block &b, const char *name, uint64_t stride) {
    const char *p = b.array(name);
    for (uint64_t i = 0; i < b.splats; ++i) {
      auto w = readU64(p + i * 8);
      for (int k = 0; k < 5; ++k)
        if (((w >> (12 * k)) & 4095) >= stride)
          throw Error("RQ index out of range");
    }
  };
  auto checkRanks = [&](const Block &b, const char *name, int count) {
    require(b, name, uint64_t(count) * 4);
    auto p = b.array(name);
    uint64_t prev = 0;
    for (int i = 0; i < count; ++i) {
      auto n = readU32(p + i * 4);
      if (n < prev || n > b.splats)
        throw Error("invalid rank boundary");
      prev = n;
    }
    if (prev != b.splats)
      throw Error("incomplete rank boundaries");
    return buildRanks(p, count, b.splats);
  };
  bool position = false, rotation = false;
  for (size_t gi = 1; gi < blocks.size(); ++gi) {
    const auto &b = blocks[gi];
    auto n = b.splats;
    if (n > 10000000 || b.intervals != shared.intervals)
      throw Error("invalid splat group");
    require(b, "scale_indices", n * 3);
    require(b, "lifetimes", n * 2);
    require(b, "sh0_base_indices", n * 3);
    require(b, "sh0_rq_indices", n * 8);
    require(b, "opacity_rq_indices", n * 8);
    checkRq(b, "sh0_rq_indices", shared.sh0Entries / 5);
    checkRq(b, "opacity_rq_indices", shared.opacityEntries / 5);
    if (b.positionPerSample)
      require(b, "position_samples", n * samples * 8);
    else {
      position = true;
      require(b, "position_base", n * 8);
      auto ranks = checkRanks(b, "position_rank_boundaries", 4);
      require(b, "position_rq_coefficients", ranks.totalTerms * 4);
      auto p = b.array("position_rq_coefficients");
      for (uint64_t i = 0; i < ranks.totalTerms; ++i)
        if ((readU32(p + i * 4) & 65535) >= shared.positionEntries)
          throw Error("position index out of range");
    }
    if (b.rotationPerSample)
      require(b, "rotation_samples", n * samples * 4);
    else {
      rotation = true;
      require(b, "rotation_base", n * 4);
      auto ranks = checkRanks(b, "rotation_rank_boundaries", 5);
      require(b, "rotation_rq_indices", ranks.totalTerms * 2);
      auto p = b.array("rotation_rq_indices");
      for (uint64_t i = 0; i < ranks.totalTerms; ++i)
        if (readU16(p + i * 2) >= shared.rotationEntries)
          throw Error("rotation index out of range");
    }
    // The planes the capture kept: its SH degree, read off the data rather than
    // assumed, since every SH array is sized by it.
    uint64_t planes = 0;
    if (b.arrays.count("sh_static_indices") && n)
      planes = b.arrays.at("sh_static_indices").size() / (n * 4);
    if (b.arrays.count("sh_static_indices") &&
        (planes != 1 && planes != 3 && planes != 5))
      throw Error("invalid SH plane count");
    for (auto pair :
         {std::make_pair("sh_static_indices", shared.shStaticEntries),
          std::make_pair("sh_temporal_indices", shared.shTemporalEntries)})
      if (b.arrays.count(pair.first)) {
        require(b, pair.first, n * 4 * planes);
        auto p = b.array(pair.first);
        for (uint64_t i = 0; i < n * planes; ++i) {
          auto w = readU32(p + i * 4);
          for (int shift : {0, 10, 20})
            if (((w >> shift) & 1023) >= pair.second)
              throw Error("SH index out of range");
        }
      }
  }
  if (position)
    require(shared, "position_trajectories",
            shared.positionEntries * samples * 8);
  if (rotation) {
    require(shared, "rotation_initial", shared.rotationEntries * 8);
    require(shared, "rotation_delta_lut", 1024);
    require(shared, "rotation_delta_indices",
            shared.rotationEntries * shared.intervals * 4);
  }
  // The codebooks carry a block per kept plane, the same count the index arrays do.
  uint64_t bookPlanes = 5;
  for (const auto &b : blocks) {
    const auto it = b.arrays.find("sh_static_indices");
    if (it != b.arrays.end() && b.splats) {
      bookPlanes = it->second.size() / (b.splats * 4);
      break;
    }
  }
  if (shared.arrays.count("sh_static_codebooks"))
    require(shared, "sh_static_codebooks",
            bookPlanes * 3 * shared.shStaticEntries * 3 * 2);
  if (shared.arrays.count("sh_temporal_codebooks"))
    require(shared, "sh_temporal_codebooks",
            samples * bookPlanes * 3 * shared.shTemporalEntries * 3 * 2);
}
} // namespace vgs
