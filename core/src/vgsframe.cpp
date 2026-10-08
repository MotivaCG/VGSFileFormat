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

// buildRanks' arithmetic for one splat at a time. Evaluating a frame used to build the
// whole table for every group on every frame - two arrays the size of the group, filled
// on one thread, for values that never change within a chunk. This answers the same
// question from the boundaries alone, so any thread can ask it for any range of splats.
class RankLookup {
public:
  RankLookup() = default;
  RankLookup(const char *boundaryBytes, int boundaryCount, uint64_t splats)
      : count(boundaryCount) {
    if (boundaryCount <= 0 || boundaryCount > kMaxBoundaries)
      throw Error("invalid rank boundaries");
    for (int j = 0; j < count; ++j)
      boundaries[j] = readU32(boundaryBytes + 4 * j);
    for (int j = 1; j < count; ++j) {
      starts[j] = boundaries[j - 1];
      rankStart[j] = rankStart[j - 1] + uint64_t(j) * (boundaries[j - 1] - starts[j - 1]);
    }
    // buildRanks throws for a splat that lies past every boundary. If any does, the last
    // one does, so checking it here is the same check, made once and before any thread
    // starts.
    if (splats) {
      bool past = true;
      for (int j = 0; j < count; ++j)
        past = past && boundaries[j] <= splats - 1;
      if (past)
        throw Error("invalid rank boundaries");
    }
  }

  // The splat's number of terms, 1-based, and the index of its first term.
  void find(uint64_t id, uint32_t *rank, uint64_t *offset) const {
    int below = 0;
    while (below < count && boundaries[below] <= id)
      ++below;
    *rank = uint32_t(below + 1);
    *offset = rankStart[below] + uint64_t(below + 1) * (id - starts[below]);
  }

private:
  static constexpr int kMaxBoundaries = 8;
  int count = 0;
  uint64_t boundaries[kMaxBoundaries] = {}, starts[kMaxBoundaries] = {},
           rankStart[kMaxBoundaries] = {};
};

// Below this many splats a piece is not worth handing to another thread.
constexpr uint64_t kSplatsPerPiece = 16384;

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
constexpr double sphericalHarmonicC0() { return 0.28209479177387814; }

// ------------------------------------------------------------ motion helpers

// The real spherical harmonic basis every 3DGS renderer evaluates, bands 1 to 3 in the
// order the coefficients are stored. Rotations are derived from these same functions, so
// they agree with the evaluation by construction rather than by matching a convention.
void shBasis(double x, double y, double z, double *b) {
  b[0] = -0.4886025119029199 * y;
  b[1] = 0.4886025119029199 * z;
  b[2] = -0.4886025119029199 * x;
  b[3] = 1.0925484305920792 * x * y;
  b[4] = -1.0925484305920792 * y * z;
  b[5] = 0.31539156525252005 * (2 * z * z - x * x - y * y);
  b[6] = -1.0925484305920792 * x * z;
  b[7] = 0.5462742152960396 * (x * x - y * y);
  b[8] = -0.5900435899266435 * y * (3 * x * x - y * y);
  b[9] = 2.890611442640554 * x * y * z;
  b[10] = -0.4570457994644658 * y * (4 * z * z - x * x - y * y);
  b[11] = 0.3731763325901154 * z * (2 * z * z - 3 * x * x - 3 * y * y);
  b[12] = -0.4570457994644658 * x * (4 * z * z - x * x - y * y);
  b[13] = 1.445305721320277 * z * (x * x - y * y);
  b[14] = -0.5900435899266435 * x * (x * x - 3 * y * y);
}
constexpr int kShBandFirst[3] = {0, 3, 8}, kShBandSize[3] = {3, 5, 7};
constexpr int kShDirections = 64;

// Fibonacci directions, and per band the pseudo-inverse (A^T A)^-1 A^T of the basis
// sampled at them. Neither depends on the rotation, so both are built once.
struct ShFit {
  double directions[kShDirections][3];
  std::vector<double> pseudoInverse[3]; // size x kShDirections, row-major
  ShFit() {
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < kShDirections; ++i) {
      const double z = 1.0 - 2.0 * (i + 0.5) / kShDirections;
      const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
      directions[i][0] = r * std::cos(golden * i);
      directions[i][1] = r * std::sin(golden * i);
      directions[i][2] = z;
    }
    for (int band = 0; band < 3; ++band) {
      const int n = kShBandSize[band], first = kShBandFirst[band];
      std::vector<double> a(size_t(kShDirections * n));
      for (int i = 0; i < kShDirections; ++i) {
        double b[15];
        shBasis(directions[i][0], directions[i][1], directions[i][2], b);
        for (int j = 0; j < n; ++j)
          a[size_t(i * n + j)] = b[first + j];
      }
      // Normal matrix, inverted by Gauss-Jordan with partial pivoting: at most 7x7 and
      // well conditioned on 64 spread directions.
      std::vector<double> m(size_t(n * 2 * n), 0.0);
      for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c)
          for (int i = 0; i < kShDirections; ++i)
            m[size_t(r * 2 * n + c)] += a[size_t(i * n + r)] * a[size_t(i * n + c)];
        m[size_t(r * 2 * n + n + r)] = 1.0;
      }
      for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
          if (std::abs(m[size_t(r * 2 * n + col)]) > std::abs(m[size_t(pivot * 2 * n + col)]))
            pivot = r;
        for (int c = 0; c < 2 * n; ++c)
          std::swap(m[size_t(col * 2 * n + c)], m[size_t(pivot * 2 * n + c)]);
        const double d = m[size_t(col * 2 * n + col)];
        for (int c = 0; c < 2 * n; ++c)
          m[size_t(col * 2 * n + c)] /= d;
        for (int r = 0; r < n; ++r)
          if (r != col) {
            const double f = m[size_t(r * 2 * n + col)];
            for (int c = 0; c < 2 * n; ++c)
              m[size_t(r * 2 * n + c)] -= f * m[size_t(col * 2 * n + c)];
          }
      }
      auto &p = pseudoInverse[band];
      p.assign(size_t(n * kShDirections), 0.0);
      for (int r = 0; r < n; ++r)
        for (int i = 0; i < kShDirections; ++i)
          for (int k = 0; k < n; ++k)
            p[size_t(r * kShDirections + i)] += m[size_t(r * 2 * n + n + k)] * a[size_t(i * n + k)];
    }
  }
};
const ShFit &shFit() {
  static const ShFit fit;
  return fit;
}
void rotationMatrix(const double q[4], double r[9]) {
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - z * w);     r[2] = 2 * (x * z + y * w);
  r[3] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - x * w);
  r[6] = 2 * (x * z - y * w);     r[7] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}
} // namespace

bool Motion::isIdentity() const {
  return translation[0] == 0 && translation[1] == 0 && translation[2] == 0 &&
         rotation[0] == 0 && rotation[1] == 0 && rotation[2] == 0 &&
         std::abs(rotation[3]) == 1 && scale == 1;
}
void Motion::matrix(double out[12]) const {
  double r[9];
  rotationMatrix(rotation, r);
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col)
      out[row * 4 + col] = r[row * 3 + col] * scale;
    out[row * 4 + 3] = translation[row];
  }
}
Spec motionSamplesSpec(uint64_t samples) {
  Spec s;
  s.kind = 2; // packed words
  s.family = 0;
  s.width = 8;
  s.fields = {16, 16, 16, 16};
  s.rows = samples * 8;
  return s;
}
bool isMotionSamplesSpec(const Spec &s, uint64_t totalRows, uint64_t samples) {
  const std::vector<int> fields = {16, 16, 16, 16};
  return s.kind == 2 && s.family == 0 && s.width == 8 && s.fields == fields &&
         s.ranks.empty() && samples && samples <= 256 && totalRows == samples * 8;
}
bool declaresMotion(const Header &h) {
  for (const auto &p : h.policies)
    if (p.attribute == MotionSamples)
      return true;
  return false;
}
Bytes packMotionSamples(const std::vector<Motion> &samples) {
  Bytes out(samples.size() * MotionSampleBytes);
  size_t at = 0;
  auto put = [&](double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; ++i)
      out[at++] = uint8_t(bits >> (8 * i));
  };
  for (const auto &m : samples) {
    for (double v : m.translation)
      put(v);
    for (double v : m.rotation)
      put(v);
    put(m.scale);
  }
  return out;
}
std::vector<Motion> unpackMotionSamples(const uint8_t *data, size_t size, uint64_t samples) {
  if (!samples || samples > 256 || size != samples * MotionSampleBytes)
    throw Error("invalid motion samples");
  std::vector<Motion> out(static_cast<size_t>(samples));
  const char *p = reinterpret_cast<const char *>(data);
  for (auto &m : out) {
    for (double &v : m.translation) {
      v = readF64(p);
      p += 8;
    }
    for (double &v : m.rotation) {
      v = readF64(p);
      p += 8;
    }
    m.scale = readF64(p);
    p += 8;
    double norm = 0;
    for (double v : m.rotation)
      norm += v * v;
    norm = std::sqrt(norm);
    for (double v : m.translation)
      if (!std::isfinite(v) || std::abs(v) > 1e9)
        throw Error("invalid motion translation");
    if (!std::isfinite(norm) || std::abs(norm - 1) > 1e-6)
      throw Error("invalid motion rotation");
    if (!std::isfinite(m.scale) || !(m.scale > 0) || m.scale > 1e6)
      throw Error("invalid motion scale");
    for (double &v : m.rotation)
      v /= norm;
  }
  return out;
}
Motion interpolateMotion(const Motion &a, const Motion &b, double t) {
  Motion m;
  for (int k = 0; k < 3; ++k)
    m.translation[k] = a.translation[k] + (b.translation[k] - a.translation[k]) * t;
  m.scale = a.scale + (b.scale - a.scale) * t;
  double dot = 0;
  for (int k = 0; k < 4; ++k)
    dot += a.rotation[k] * b.rotation[k];
  const double sign = dot < 0 ? -1 : 1;
  dot = std::abs(dot);
  double wa = 1 - t, wb = t;
  // Nearly parallel: the sine below vanishes, and linear interpolation then
  // normalisation is the same rotation to within rounding.
  if (dot < 0.9999995) {
    const double angle = std::acos(std::min(1.0, dot)), s = std::sin(angle);
    wa = std::sin((1 - t) * angle) / s;
    wb = std::sin(t * angle) / s;
  }
  double norm = 0;
  for (int k = 0; k < 4; ++k) {
    m.rotation[k] = wa * a.rotation[k] + wb * sign * b.rotation[k];
    norm += m.rotation[k] * m.rotation[k];
  }
  norm = std::sqrt(norm);
  for (double &v : m.rotation)
    v /= norm;
  return m;
}
void shRotationMatrices(const double rotation[4], double band1[9], double band2[25],
                        double band3[49]) {
  // A function f(d) moved by R becomes f(R^T d): sample that at fixed directions and
  // project back onto the basis. The rotated band lies in the band's span, so this is
  // exact to rounding.
  const auto &fit = shFit();
  double r[9];
  rotationMatrix(rotation, r);
  double rotated[kShDirections][15];
  for (int i = 0; i < kShDirections; ++i) {
    const double *d = fit.directions[i];
    const double x = r[0] * d[0] + r[3] * d[1] + r[6] * d[2];
    const double y = r[1] * d[0] + r[4] * d[1] + r[7] * d[2];
    const double z = r[2] * d[0] + r[5] * d[1] + r[8] * d[2];
    shBasis(x, y, z, rotated[i]);
  }
  double *out[3] = {band1, band2, band3};
  for (int band = 0; band < 3; ++band) {
    const int n = kShBandSize[band], first = kShBandFirst[band];
    const auto &p = fit.pseudoInverse[band];
    for (int row = 0; row < n; ++row)
      for (int col = 0; col < n; ++col) {
        double v = 0;
        for (int i = 0; i < kShDirections; ++i)
          v += p[size_t(row * kShDirections + i)] * rotated[i][first + col];
        out[band][row * n + col] = v;
      }
  }
}
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
      const RankLookup ranks(group.array("position_rank_boundaries"), 4, n);
      const char *terms = group.array("position_rq_coefficients");
      for (uint64_t i = 0; i < n; ++i) {
        float p[3];
        unpackPosition(readU64(baseBytes + i * 8), group.positionMin,
                       group.positionMax, p);
        uint32_t rank = 0;
        uint64_t first = 0;
        ranks.find(i, &rank, &first);
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
  if (!motionSamples.empty()) {
    double m[12];
    motion(normalized).matrix(m);
    for (uint64_t i = 0; i < total; ++i) {
      float *p = out->data() + i * 3;
      const double x = p[0], y = p[1], z = p[2];
      for (int row = 0; row < 3; ++row)
        p[row] = float(m[row * 4] * x + m[row * 4 + 1] * y + m[row * 4 + 2] * z + m[row * 4 + 3]);
    }
  }
}

Frame FrameDecoder::evaluate(double normalized, bool includeSh) const {
  Frame result;
  evaluateInto(normalized, includeSh, &result);
  return result;
}

void FrameDecoder::evaluateInto(double normalized, bool includeSh, Frame *out,
                                size_t pieces, const Parallel &parallel) const {
  if (held == Contents::Positions)
    throw Error("this chunk was decoded for positions only");
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
  const bool evaluateSh = includeSh && shCoefficients;
  out->shCoefficients = int(includeSh ? shCoefficients : 0);
  if (evaluateSh)
    out->shRest.resize(size_t(total * shCoefficients * 3));
  else
    out->shRest.clear();

  // ---- what every splat reads, looked up once ------------------------------------
  //
  // Everything that can throw happens here, on the calling thread, so that the ranges
  // below are plain arithmetic whichever thread runs them.

  float scaleLut[256];
  {
    const char *bytes = shared->array("scale_lut");
    for (int i = 0; i < 256; ++i)
      scaleLut[i] = readF32(bytes + 4 * i);
  }

  float sh0Lut[256];
  {
    const char *bytes = shared->array("sh0_base_lut");
    for (int i = 0; i < 256; ++i)
      sh0Lut[i] = readHalf(bytes + 2 * i);
  }
  const uint64_t sh0Stride = shared->sh0Entries / 5; // 1024 rows per stage
  const uint64_t opacityStride = shared->opacityEntries / 5;
  const float c0 = float(sphericalHarmonicC0());

  const char *shStaticBytes = nullptr, *shTemporalFrame = nullptr;
  const uint64_t ns = shared->shStaticEntries, nt = shared->shTemporalEntries;
  if (evaluateSh) {
    shStaticBytes = shared->array("sh_static_codebooks");
    // The temporal codebook is read at `frame` with no interpolation to the
    // next sample, unlike every other temporal table. See 6.8.
    shTemporalFrame = shared->array("sh_temporal_codebooks") +
                      frame * int64_t(shPlanes) * 3 * nt * 3 * 2;
  }

  struct GroupPlan {
    const Block *block = nullptr;
    uint64_t first = 0; // where its splats start in the frame
    const char *positions = nullptr, *positionTerms = nullptr;
    const char *rotations = nullptr, *rotationTerms = nullptr;
    RankLookup positionRanks, rotationRanks;
    const uint8_t *scaleIndices = nullptr, *lifetimes = nullptr;
    const char *opacityWords = nullptr, *sh0Indices = nullptr, *sh0Words = nullptr;
    const uint8_t *opacityScales = nullptr; // optional: a per-splat factor, or none
    const char *shStaticIndices = nullptr, *shTemporalIndices = nullptr;
  };
  std::vector<GroupPlan> plans;
  plans.reserve(groups.size());
  {
    uint64_t first = 0;
    for (const Block *group : groups) {
      GroupPlan plan;
      plan.block = group;
      plan.first = first;
      const uint64_t n = group->splats;
      if (group->positionPerSample) {
        plan.positions = group->array("position_samples");
      } else {
        plan.positions = group->array("position_base");
        plan.positionRanks = RankLookup(group->array("position_rank_boundaries"), 4, n);
        plan.positionTerms = group->array("position_rq_coefficients");
      }
      if (group->rotationPerSample) {
        plan.rotations = group->array("rotation_samples");
      } else {
        plan.rotations = group->array("rotation_base");
        plan.rotationRanks = RankLookup(group->array("rotation_rank_boundaries"), 5, n);
        plan.rotationTerms = group->array("rotation_rq_indices");
      }
      plan.scaleIndices = reinterpret_cast<const uint8_t *>(group->array("scale_indices"));
      plan.opacityWords = group->array("opacity_rq_indices");
      if (group->arrays.count("opacity_scales"))
        plan.opacityScales = reinterpret_cast<const uint8_t *>(group->array("opacity_scales"));
      plan.lifetimes = reinterpret_cast<const uint8_t *>(group->array("lifetimes"));
      plan.sh0Indices = group->array("sh0_base_indices");
      plan.sh0Words = group->array("sh0_rq_indices");
      if (evaluateSh) {
        plan.shStaticIndices = group->array("sh_static_indices");
        plan.shTemporalIndices = group->array("sh_temporal_indices");
      }
      plans.push_back(plan);
      first += n;
    }
  }

  // ---- the chunk's motion at this instant, applied to every splat below -------------
  const bool moving = !motionSamples.empty();
  double motionMatrix[12] = {};
  double motionRotation[4] = {0, 0, 0, 1};
  float motionScale = 1;
  bool rotateSh = false;
  float shBand[3][49] = {};
  if (moving) {
    const Motion m = motion(normalized);
    m.matrix(motionMatrix);
    for (int k = 0; k < 4; ++k)
      motionRotation[k] = m.rotation[k];
    motionScale = float(m.scale);
    rotateSh = evaluateSh && !(m.rotation[0] == 0 && m.rotation[1] == 0 && m.rotation[2] == 0);
    if (rotateSh) {
      double band[3][49];
      shRotationMatrices(m.rotation, band[0], band[1], band[2]);
      for (int b = 0; b < 3; ++b)
        for (int k = 0; k < 49; ++k)
          shBand[b][k] = float(band[b][k]);
    }
  }

  // ---- one range of one group's splats ----------------------------------------------
  //
  // Every splat depends on the shared tables above and on nothing another splat writes,
  // so any split of the frame gives exactly the same result.

  const auto evaluateGroupRange = [&](const GroupPlan &plan, uint64_t begin, uint64_t end) {
    const Block *group = plan.block;
    const uint64_t n = group->splats;
    const uint64_t written = plan.first;

    // --- positions -------------------------------------------------------
    if (group->positionPerSample) {
      for (uint64_t i = begin; i < end; ++i) {
        const char *row = plan.positions + (i * samples) * 8;
        float a[3], b[3];
        unpackPosition(readU64(row + frame * 8), group->positionMin,
                       group->positionMax, a);
        unpackPosition(readU64(row + (frame + 1) * 8), group->positionMin,
                       group->positionMax, b);
        // This build interpolates per-sample positions with normalized
        // chunk time, not with the fraction inside the interval. It looks
        // like a defect, but it is what the runtime does. See 6.4.
        for (int c = 0; c < 3; ++c)
          out->position[size_t((written + i) * 3 + uint64_t(c))] =
              a[c] + (b[c] - a[c]) * r;
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        float p[3];
        unpackPosition(readU64(plan.positions + i * 8), group->positionMin,
                       group->positionMax, p);
        uint32_t rank = 0;
        uint64_t first = 0;
        plan.positionRanks.find(i, &rank, &first);
        for (uint32_t k = 0; k < rank; ++k) {
          const uint32_t packed = readU32(plan.positionTerms + (first + k) * 4);
          const uint32_t index = packed & 0xFFFFu;
          // The high half is the weight's raw binary16 bits, not a
          // normalised integer: real weights run well outside [-1,1].
          const float weight = halfToFloat(uint16_t(packed >> 16));
          const float *row = basis.position.data() + uint64_t(index) * 3;
          for (int c = 0; c < 3; ++c)
            p[c] += weight * row[c];
        }
        for (int c = 0; c < 3; ++c)
          out->position[size_t((written + i) * 3 + uint64_t(c))] = p[c];
      }
    }

    // --- rotations -------------------------------------------------------
    if (group->rotationPerSample) {
      for (uint64_t i = begin; i < end; ++i) {
        const char *row = plan.rotations + (i * samples) * 4;
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
          out->rotation[size_t((written + i) * 4 + uint64_t(c))] = q[c] / norm;
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        float base[4];
        unpackQuaternion(readU32(plan.rotations + i * 4), base);

        float residual[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        uint32_t rank = 0;
        uint64_t first = 0;
        plan.rotationRanks.find(i, &rank, &first);
        for (uint32_t k = 0; k < rank; ++k) {
          const uint16_t index = readU16(plan.rotationTerms + (first + k) * 2);
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
          out->rotation[size_t((written + i) * 4 + uint64_t(c))] = q[c] / norm;
      }
    }

    // --- scale, opacity, lifetime ------------------------------------------
    for (uint64_t i = begin; i < end; ++i) {
      for (int c = 0; c < 3; ++c)
        out->scale[size_t((written + i) * 3 + uint64_t(c))] =
            scaleLut[plan.scaleIndices[i * 3 + uint64_t(c)]];

      const uint64_t word = readU64(plan.opacityWords + i * 8);
      float opacity = 0.0f;
      for (int k = 0; k < 5; ++k) {
        const uint64_t stage = (word >> (12 * k)) & 0xFFFull;
        opacity += basis.opacity[size_t(uint64_t(k) * opacityStride + stage)];
      }
      opacity = clamp01(opacity);
      if (plan.opacityScales)
        opacity *= opacityScale(plan.opacityScales[i]);
      out->opacity[size_t(written + i)] = opacity;

      const uint8_t begins = plan.lifetimes[i * 2];
      const uint8_t ends = plan.lifetimes[i * 2 + 1];
      out->active[size_t(written + i)] =
          (frame >= begins && frame + 1 <= ends) ? uint8_t(1) : uint8_t(0);
    }

    // --- colour ------------------------------------------------------------
    for (uint64_t i = begin; i < end; ++i) {
      const uint8_t *idx = reinterpret_cast<const uint8_t *>(plan.sh0Indices + i * 3);
      float sh0[3] = {sh0Lut[idx[0]], sh0Lut[idx[1]], sh0Lut[idx[2]]};

      const uint64_t word = readU64(plan.sh0Words + i * 8);
      for (int k = 0; k < 5; ++k) {
        const uint64_t stage = (word >> (12 * k)) & 0xFFFull;
        const float *row = basis.sh0.data() + (uint64_t(k) * sh0Stride + stage) * 3;
        for (int c = 0; c < 3; ++c)
          sh0[c] += row[c];
      }
      for (int c = 0; c < 3; ++c)
        out->colorDc[size_t((written + i) * 3 + uint64_t(c))] = 0.5f + c0 * sh0[c];
    }

    // --- higher-order spherical harmonics --------------------------------
    if (evaluateSh) {
      static const int kShift[3] = {20, 10, 0};
      for (uint64_t g = 0; g < shPlanes; ++g) {
        // The index arrays are plane-major: all N words of group 0, then
        // all N of group 1, and so on.
        const char *sPlane = plan.shStaticIndices + g * n * 4;
        const char *tPlane = plan.shTemporalIndices + g * n * 4;
        for (uint64_t i = begin; i < end; ++i) {
          const uint32_t sWord = readU32(sPlane + i * 4);
          const uint32_t tWord = readU32(tPlane + i * 4);
          for (int k = 0; k < 3; ++k) {
            const uint32_t si = (sWord >> kShift[k]) & 0x3FFu;
            const uint32_t ti = (tWord >> kShift[k]) & 0x3FFu;
            const char *sEntry =
                shStaticBytes + (((g * 3 + uint64_t(k)) * ns) + si) * 3 * 2;
            const char *tEntry =
                shTemporalFrame + (((g * 3 + uint64_t(k)) * nt) + ti) * 3 * 2;
            const uint64_t coefficient = g * 3 + uint64_t(k);
            if (coefficient >= shCoefficients)
              break;
            for (int c = 0; c < 3; ++c) {
              out->shRest[size_t((written + i) * shCoefficients * 3 +
                                 coefficient * 3 + uint64_t(c))] =
                  readHalf(sEntry + c * 2) + readHalf(tEntry + c * 2);
            }
          }
        }
      }
    }

    // --- motion ------------------------------------------------------------
    if (moving) {
      const double *m = motionMatrix, *turn = motionRotation;
      for (uint64_t i = begin; i < end; ++i) {
        const size_t record = size_t(written + i);
        float *p = out->position.data() + record * 3;
        const double x = p[0], y = p[1], z = p[2];
        for (int row = 0; row < 3; ++row)
          p[row] = float(m[row * 4] * x + m[row * 4 + 1] * y + m[row * 4 + 2] * z + m[row * 4 + 3]);
        // World orientation is the motion's rotation applied after the splat's own.
        float *q = out->rotation.data() + record * 4;
        const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
        const double nx = turn[3] * qx + qw * turn[0] + (turn[1] * qz - turn[2] * qy);
        const double ny = turn[3] * qy + qw * turn[1] + (turn[2] * qx - turn[0] * qz);
        const double nz = turn[3] * qz + qw * turn[2] + (turn[0] * qy - turn[1] * qx);
        const double nw = turn[3] * qw - (turn[0] * qx + turn[1] * qy + turn[2] * qz);
        const double norm = std::max(std::sqrt(nx * nx + ny * ny + nz * nz + nw * nw), 1e-20);
        q[0] = float(nx / norm);
        q[1] = float(ny / norm);
        q[2] = float(nz / norm);
        q[3] = float(nw / norm);
        float *s = out->scale.data() + record * 3;
        for (int c = 0; c < 3; ++c)
          s[c] *= motionScale;
      }
      if (rotateSh) {
        for (uint64_t i = begin; i < end; ++i) {
          float *coefficients = out->shRest.data() + size_t(written + i) * shCoefficients * 3;
          for (int band = 0; band < 3; ++band) {
            const int first = band == 0 ? 0 : band == 1 ? 3 : 8, size = 3 + 2 * band;
            if (uint64_t(first + size) > shCoefficients)
              break;
            float source[7][3];
            for (int k = 0; k < size; ++k)
              for (int c = 0; c < 3; ++c)
                source[k][c] = coefficients[(first + k) * 3 + c];
            for (int row = 0; row < size; ++row)
              for (int c = 0; c < 3; ++c) {
                float v = 0;
                for (int k = 0; k < size; ++k)
                  v += shBand[band][row * size + k] * source[k][c];
                coefficients[(first + row) * 3 + c] = v;
              }
          }
        }
      }
    }
  };

  // A range of the whole frame, which may cross from one group into the next.
  const auto evaluateRange = [&](uint64_t begin, uint64_t end) {
    for (const GroupPlan &plan : plans) {
      const uint64_t groupEnd = plan.first + plan.block->splats;
      const uint64_t from = std::max(begin, plan.first), to = std::min(end, groupEnd);
      if (from < to)
        evaluateGroupRange(plan, from - plan.first, to - plan.first);
    }
  };

  const size_t useful = size_t(std::max<uint64_t>(1, total / kSplatsPerPiece));
  const size_t split = parallel ? std::min(pieces, useful) : 1;
  if (split <= 1) {
    evaluateRange(0, total);
    return;
  }
  parallel(split, [&](size_t piece) {
    evaluateRange(total * piece / split, total * (piece + 1) / split);
  });
}

const char *FrameDecoder::Block::array(const char *name) const {
  auto it = arrays.find(name);
  if (it == arrays.end())
    throw Error(std::string("missing attribute: ") + name);
  return reinterpret_cast<const char *>(it->second.data());
}
bool FrameDecoder::usedByPositions(uint32_t attribute) {
  // By name, because that is how evaluatePositions looks its arrays up: every array it
  // reads is one of the format's position_ attributes, and it reads nothing else.
  // Motion moves the positions too, so a positions-only reader needs it as well.
  if (attribute == MotionSamples)
    return true;
  const char *name = attributeName(attribute);
  return name && std::strncmp(name, "position_", 9) == 0;
}

Motion FrameDecoder::motion(double normalized) const {
  if (motionSamples.empty())
    return {};
  if (!std::isfinite(normalized) || normalized < 0 || normalized >= 1)
    throw Error("time must be in [0,1)");
  // The same sample and fraction every other attribute is evaluated with, in double.
  const uint64_t intervals = blocks.front().intervals;
  const float sampleTime = float(normalized) * float(intervals);
  const uint64_t frame =
      std::min<uint64_t>(uint64_t(std::floor(sampleTime)), intervals - 1);
  const double alpha = double(sampleTime - float(frame));
  return interpolateMotion(motionSamples[size_t(frame)], motionSamples[size_t(frame + 1)],
                           alpha);
}

FrameDecoder::FrameDecoder(const DecodedChunk &chunk, double tick, Contents contents)
    : secondsPerTick(tick), held(contents) {
  // Positions only: every check below that is about another attribute is skipped, and
  // the position ones stay exactly as strict, since those arrays are still indexed by
  // pointer arithmetic.
  const bool positionsOnly = contents == Contents::Positions;
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
    // An optional attribute this reader cannot name: the policy allowed skipping it.
    if (std::strcmp(attributeName(p.attribute), "unknown") == 0)
      continue;
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
    if (std::strcmp(attributeName(item.descriptor.attribute), "unknown") != 0 &&
        covered[{item.descriptor.group, item.descriptor.attribute}] !=
            item.descriptor.totalRows)
      throw Error("incomplete attribute");
  // Validate all shapes and dictionary indices before any pointer-based
  // evaluation.
  auto require = [positionsOnly](const Block &b, const char *name, uint64_t bytes) {
    if (positionsOnly && std::strncmp(name, "position_", 9) != 0)
      return;
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
    if (b.arrays.count("opacity_scales"))
      require(b, "opacity_scales", n);
    if (!positionsOnly) {
      checkRq(b, "sh0_rq_indices", shared.sh0Entries / 5);
      checkRq(b, "opacity_rq_indices", shared.opacityEntries / 5);
    }
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
    if (positionsOnly) {
      // Rotations are not held; nothing below reads them.
    } else if (b.rotationPerSample)
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
  const auto motion = shared.arrays.find(attributeName(MotionSamples));
  if (motion != shared.arrays.end()) {
    motionSamples = unpackMotionSamples(motion->second.data(), motion->second.size(), samples);
    // Nothing to apply: evaluate as the static capture it is, at no cost.
    if (std::all_of(motionSamples.begin(), motionSamples.end(),
                    [](const Motion &m) { return m.isIdentity(); }))
      motionSamples.clear();
  }
}
} // namespace vgs
