// The Unreal plugin's splat layouts, checked without Unreal.
//
// plugins/unreal/VGS/Source/VGSRuntime/Private/VGSSplatLayout.h writes the buffers Epic's
// Gaussian Splat renderer (UE 5.9) reads. This reads them back the way its shaders do -
// the functions below are transcriptions of GaussianSplatShared.h, GaussianSplatShading.usf
// and SphericalGaussianCommon.ush - and checks three things:
//
//   1. the float layout returns every value bit for bit;
//   2. the quantized layout returns every value within half a quantization step;
//   3. leaving the capture in its own axes and putting the turn to Unreal's in
//      LocalToWorld draws the same splats as converting every splat the way Epic's
//      importer converts an SPZ file (right handed, Y up): the same centre, the same
//      covariance, the same view direction for the harmonics.
//
// With a .vgs path as its argument it also packs a real frame through vgsbridge, reports
// the errors and the time each layout takes, and fails on the same bounds.

#include "VGSSplatLayout.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(VGS_UNREAL_LAYOUT_WITH_BRIDGE)
#include "vgsbridge.h"
#endif

using namespace vgsunreal;

namespace {

void require(bool condition, const std::string &what) {
  if (!condition)
    throw std::runtime_error(what);
}

// ---- the renderer's reads ---------------------------------------------------------------

struct Splat {
  float position[3], scale[3], rotation[4] /* xyzw */, color[3], opacity;
  std::vector<float> sh; // 3 per coefficient past DC
};

uint32_t word(const uint8_t *p) {
  uint32_t w;
  std::memcpy(&w, p, 4);
  return w;
}
float real(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}

Splat readFloat(const uint8_t *record, int coefficients) {
  Splat s;
  for (int c = 0; c < 3; ++c) {
    s.position[c] = real(record + FloatPositionOffset + 4 * c);
    s.scale[c] = real(record + FloatScaleOffset + 4 * c);
    s.color[c] = real(record + FloatColorOpacityOffset + 4 * c);
  }
  for (int c = 0; c < 4; ++c)
    s.rotation[c] = real(record + FloatRotationOffset + 4 * c);
  s.opacity = real(record + FloatColorOpacityOffset + 12);
  for (int k = 0; k < coefficients * 3; ++k)
    s.sh.push_back(real(record + FloatBaseStride + 4 * k));
  return s;
}

Splat readQuantized(const uint8_t *record, int coefficients, const Quantization &q) {
  const uint32_t x = word(record), y = word(record + 4), z = word(record + 8), w = word(record + 12);
  const uint32_t m21 = (1u << 21) - 1, m11 = (1u << 11) - 1, m10 = (1u << 10) - 1;
  Splat s;
  // Unpack_21_21_21
  const float p[3] = {float((y >> 10) & m21), float(((x >> 21) | (y << 11)) & m21), float(x & m21)};
  // Unpack_11_11_10_Exp
  const float l[3] = {float(z >> 21), float((z >> 10) & m11), float(z & m10)};
  for (int c = 0; c < 3; ++c) {
    s.position[c] = p[c] * q.positionScale[c] + q.positionBias[c];
    s.scale[c] = std::exp(l[c] * q.scaleScale[c] + q.scaleBias[c]);
  }
  // UnpackQuat
  s.rotation[0] = float(w >> 21) * (2.0f / 2047.0f) - 1.0f;
  s.rotation[1] = float((w >> 10) & m11) * (2.0f / 2047.0f) - 1.0f;
  s.rotation[2] = float(w & m10) * (2.0f / 1023.0f) - 1.0f;
  s.rotation[3] = std::sqrt(std::max(1.0f - (s.rotation[0] * s.rotation[0] + s.rotation[1] * s.rotation[1] +
                                             s.rotation[2] * s.rotation[2]),
                                     0.0f));
  // Unpack_8_8_8_8
  const auto bytes = [](uint32_t v, float out[4]) {
    out[0] = float(v >> 24);
    out[1] = float((v >> 16) & 0xff);
    out[2] = float((v >> 8) & 0xff);
    out[3] = float(v & 0xff);
  };
  float c4[4];
  bytes(word(record + QuantizedColorOpacityOffset), c4);
  for (int c = 0; c < 3; ++c)
    s.color[c] = c4[c] * q.colorScale[c] + q.colorBias[c];
  s.opacity = c4[3] * q.colorScale[3] + q.colorBias[3];
  for (int k = 0; k < coefficients; ++k) {
    bytes(word(record + QuantizedBaseStride + 4 * k), c4);
    for (int c = 0; c < 3; ++c)
      s.sh.push_back(c4[c] * q.shScale[c] + q.shBias[c]);
  }
  return s;
}

// ---- 3x3 algebra, row-vector convention as in the shaders -------------------------------

struct M3 {
  double m[3][3] = {};
};

M3 mul(const M3 &a, const M3 &b) {
  M3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k)
        r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}
M3 transpose(const M3 &a) {
  M3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      r.m[i][j] = a.m[j][i];
  return r;
}
double determinant(const M3 &a) {
  const auto &m = a.m;
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}
M3 inverse(const M3 &a) {
  const auto &m = a.m;
  const double d = determinant(a);
  M3 r;
  r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
  r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
  r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
  r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
  r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
  r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
  r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
  r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
  r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
  return r;
}
void rowTimes(const double v[3], const M3 &a, double out[3]) {
  for (int j = 0; j < 3; ++j)
    out[j] = v[0] * a.m[0][j] + v[1] * a.m[1][j] + v[2] * a.m[2][j];
}

// QuaternionToFloat3x3 in SphericalGaussianCommon.ush, q = xyzw.
M3 quaternionToFloat3x3(const double q[4]) {
  const double x2 = q[0] + q[0], y2 = q[1] + q[1], z2 = q[2] + q[2];
  const double xx = q[0] * x2, xy = q[0] * y2, xz = q[0] * z2;
  const double yy = q[1] * y2, yz = q[1] * z2, zz = q[2] * z2;
  const double wx = q[3] * x2, wy = q[3] * y2, wz = q[3] * z2;
  M3 r;
  const double v[3][3] = {{1 - (yy + zz), xy + wz, xz - wy}, {xy - wz, 1 - (xx + zz), yz + wx}, {xz + wy, yz - wx, 1 - (xx + yy)}};
  std::memcpy(r.m, v, sizeof(v));
  return r;
}

// ComputeCovariance3D in GaussianSplatShading.usf: rows of R scaled, then LocalToWorld.
M3 covariance(const double q[4], const double scale[3], const M3 &localToWorld) {
  M3 local = quaternionToFloat3x3(q);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      local.m[i][j] *= scale[i];
  const M3 m = mul(local, localToWorld);
  return mul(transpose(m), m);
}

M3 identity() {
  M3 r;
  r.m[0][0] = r.m[1][1] = r.m[2][2] = 1;
  return r;
}

M3 basis(UpAxis up, float unitScale) {
  float rows[3][3];
  captureToUnreal(up, unitScale, rows);
  M3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      r.m[i][j] = rows[i][j];
  return r;
}

double largest(const M3 &a) {
  double v = 0;
  for (auto &row : a.m)
    for (double x : row)
      v = std::max(v, std::abs(x));
  return v;
}

// ---- 1 and 2: the layouts ---------------------------------------------------------------

struct Synthetic {
  std::vector<float> positions, rotations, scales, radiance, sh;
  FrameView view;
};

Synthetic makeFrame(std::mt19937 &random, size_t n, int coefficients) {
  std::uniform_real_distribution<float> unit(-1.0f, 1.0f), logScale(-9.0f, 0.0f), open(0.0f, 1.0f);
  Synthetic f;
  f.positions.resize(n * 3);
  f.rotations.resize(n * 4);
  f.scales.resize(n * 3);
  f.radiance.resize(n * 4);
  f.sh.resize(n * 3 * size_t(coefficients));
  for (float &v : f.positions) v = unit(random) * 2.5f;
  for (float &v : f.scales) v = std::exp(logScale(random));
  for (size_t i = 0; i < n; ++i) {
    float *q = &f.rotations[i * 4];
    float length = 0;
    for (int c = 0; c < 4; ++c) { q[c] = unit(random); length += q[c] * q[c]; }
    length = std::sqrt(length);
    for (int c = 0; c < 4; ++c) q[c] /= length;
    for (int c = 0; c < 3; ++c) f.radiance[i * 4 + c] = unit(random) * 1.8f;
    f.radiance[i * 4 + 3] = open(random);
  }
  for (float &v : f.sh) v = unit(random) * 0.4f;
  f.view.count = n;
  f.view.shCoefficients = coefficients;
  f.view.positions = f.positions.data();
  f.view.rotations = f.rotations.data();
  f.view.scales = f.scales.data();
  f.view.radiance = f.radiance.data();
  f.view.sh = coefficients ? f.sh.data() : nullptr;
  return f;
}

struct Errors {
  double position = 0, logScale = 0, color = 0, opacity = 0, sh = 0, rotation = 0;
};

/* Packs `frame` both ways, reads it back as the renderer does, and fails on anything
 * beyond half a quantization step (rotations: on the angle between them). */
Errors checkLayouts(const FrameView &frame, double *floatMs = nullptr, double *quantizedMs = nullptr) {
  const int k = frame.sh ? frame.shCoefficients : 0;
  const size_t n = frame.count;

  std::vector<uint8_t> floats(n * floatStride(k) + 1, 0xCD);
  auto t0 = std::chrono::steady_clock::now();
  packFloat(frame, 0, n, floats.data());
  auto t1 = std::chrono::steady_clock::now();
  require(floats.back() == 0xCD, "packFloat wrote past its records");

  std::vector<uint8_t> quantized(n * quantizedStride(k) + 1, 0xCD);
  auto t2 = std::chrono::steady_clock::now();
  const Quantization q = quantization(measure(frame, 0, n));
  packQuantized(frame, q, 0, n, quantized.data());
  auto t3 = std::chrono::steady_clock::now();
  require(quantized.back() == 0xCD, "packQuantized wrote past its records");
  if (floatMs) *floatMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  if (quantizedMs) *quantizedMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

  // Measuring in pieces and merging must give the ranges of the whole.
  if (n > 2) {
    Ranges pieces = measure(frame, 0, n / 3);
    pieces.merge(measure(frame, n / 3, n));
    const Quantization merged = quantization(pieces);
    require(std::memcmp(&merged, &q, sizeof(q)) == 0, "merged ranges differ from the whole");
  }

  Errors e;
  const float eps = 1e-6f;
  for (size_t i = 0; i < n; ++i) {
    const Splat a = readFloat(floats.data() + i * floatStride(k), k);
    const float *r = frame.rotations + i * 4;
    for (int c = 0; c < 3; ++c) {
      require(a.position[c] == frame.positions[i * 3 + c], "float position");
      require(a.scale[c] == frame.scales[i * 3 + c], "float scale");
      require(a.color[c] == frame.radiance[i * 4 + c], "float color");
    }
    require(a.opacity == frame.radiance[i * 4 + 3], "float opacity");
    require(a.rotation[0] == r[1] && a.rotation[1] == r[2] && a.rotation[2] == r[3] && a.rotation[3] == r[0],
            "float rotation is not xyzw");
    for (int j = 0; j < k; ++j)
      for (int c = 0; c < 3; ++c)
        require(a.sh[j * 3 + c] == frame.sh[(size_t(j) * n + i) * 3 + c], "float harmonics");

    const Splat b = readQuantized(quantized.data() + i * quantizedStride(k), k, q);
    const auto within = [&](double error, float scale, const char *what) {
      require(error <= 0.5 * scale + eps * (1 + scale), std::string("quantized ") + what + " beyond half a step");
    };
    for (int c = 0; c < 3; ++c) {
      const double dp = std::abs(b.position[c] - frame.positions[i * 3 + c]);
      within(dp, q.positionScale[c], "position");
      e.position = std::max(e.position, dp);
      const double ds = std::abs(std::log(b.scale[c]) - logScale(frame.scales[i * 3 + c]));
      within(ds, q.scaleScale[c], "scale");
      e.logScale = std::max(e.logScale, ds);
      const double dc = std::abs(b.color[c] - frame.radiance[i * 4 + c]);
      within(dc, q.colorScale[c], "color");
      e.color = std::max(e.color, dc);
    }
    const double dop = std::abs(b.opacity - frame.radiance[i * 4 + 3]);
    within(dop, q.colorScale[3], "opacity");
    e.opacity = std::max(e.opacity, dop);
    for (int j = 0; j < k; ++j)
      for (int c = 0; c < 3; ++c) {
        const double dh = std::abs(b.sh[j * 3 + c] - frame.sh[(size_t(j) * n + i) * 3 + c]);
        within(dh, q.shScale[c], "harmonics");
        e.sh = std::max(e.sh, dh);
      }
    // q and -q are the same rotation; the angle between them is what the eye sees.
    double length = 0, dot = 0;
    const double original[4] = {r[1], r[2], r[3], r[0]};
    for (int c = 0; c < 4; ++c) length += original[c] * original[c];
    for (int c = 0; c < 4; ++c) dot += b.rotation[c] * original[c] / std::sqrt(length);
    const double angle = 2.0 * std::acos(std::min(1.0, std::abs(dot)));
    require(angle < 0.1, "quantized rotation turned by " + std::to_string(angle) + " rad");
    e.rotation = std::max(e.rotation, angle);
  }
  return e;
}

void layouts() {
  std::mt19937 random(2026);
  size_t cases = 0;
  for (size_t n : {0u, 1u, 2u, 7u, 1000u, 4097u})
    for (int k : {0, 3, 8, 15}) {
      Synthetic f = makeFrame(random, n, k);
      checkLayouts(f.view);
      ++cases;
    }
  // A frame where every value is the same: zero ranges must still read back exactly.
  Synthetic flat = makeFrame(random, 64, 3);
  for (size_t i = 1; i < 64; ++i) {
    std::memcpy(&flat.positions[i * 3], &flat.positions[0], 12);
    std::memcpy(&flat.scales[i * 3], &flat.scales[0], 12);
    std::memcpy(&flat.radiance[i * 4], &flat.radiance[0], 16);
  }
  checkLayouts(flat.view);
  require(shDegree(0) == 0 && shDegree(3) == 1 && shDegree(8) == 2 && shDegree(15) == 3 && shDegree(5) == -1,
          "shDegree");
  require(floatStride(15) == 236 && quantizedStride(15) == 80, "strides");
  std::printf("layouts: %zu frames read back as the renderer reads them\n", cases + 1);
}

// ---- intensity ----------------------------------------------------------------------------

// CalculateColor's last steps in GaussianSplatShading.usf: + 0.5, then sRGB to linear.
double shaderLinear(double e) {
  const double s = std::max(e + 0.5, 6.10352e-5);
  return s > 0.04045 ? std::pow(s * (1.0 / 1.055) + 0.0521327, 2.4) : s * (1.0 / 12.92);
}

void intensity() {
  std::mt19937 random(11);
  std::uniform_real_distribution<double> unit(-1.0, 1.0);
  require(gainFor(1.0f).identity() && gainFor(0.0f).identity() && gainFor(-3.0f).identity(),
          "an intensity of 1, or none, must leave the colours alone");

  // The drawn colour, linear, is k times brighter wherever both are above sRGB's toe.
  size_t checked = 0;
  double worst = 0;
  for (float k : {0.25f, 0.5f, 2.0f, 16.0f, 1000.0f, 30000.0f}) {
    const Gain g = gainFor(k);
    for (int trial = 0; trial < 5000; ++trial) {
      // DC and three further coefficients, with basis values as a view direction gives them.
      const double dc = unit(random) * 1.8;
      double e = SHC0 * dc, scaled = SHC0 * (dc * g.m + g.dcOffset);
      for (int i = 0; i < 3; ++i) {
        const double coefficient = unit(random) * 0.4, basis = unit(random) * 0.5;
        e += basis * coefficient;
        scaled += basis * coefficient * g.m;
      }
      if (e + 0.5 <= 0.04045 || scaled + 0.5 <= 0.04045)
        continue;
      const double relative = std::abs(shaderLinear(scaled) - k * shaderLinear(e)) / (k * shaderLinear(e));
      require(relative < 2e-4, "intensity " + std::to_string(k) + " is off by " + std::to_string(relative));
      worst = std::max(worst, relative);
      ++checked;
    }
  }
  require(checked > 20000, "too few colours above the toe to say anything");

  // Both layouts carry the gain: float in the values, quantized in its scales and biases.
  Synthetic f = makeFrame(random, 3000, 8);
  const Gain g = gainFor(5000.0f);
  const int k = f.view.shCoefficients;
  std::vector<uint8_t> floats(f.view.count * floatStride(k));
  packFloat(f.view, 0, f.view.count, floats.data(), g);
  std::vector<uint8_t> quantized(f.view.count * quantizedStride(k));
  Quantization q = quantization(measure(f.view, 0, f.view.count));
  packQuantized(f.view, q, 0, f.view.count, quantized.data());
  applyGain(q, g);
  for (size_t i = 0; i < f.view.count; ++i) {
    const Splat a = readFloat(floats.data() + i * floatStride(k), k);
    const Splat b = readQuantized(quantized.data() + i * quantizedStride(k), k, q);
    for (int c = 0; c < 3; ++c) {
      const float want = f.radiance[i * 4 + c] * g.m + g.dcOffset;
      require(a.color[c] == want, "float colour does not carry the gain");
      require(std::abs(b.color[c] - want) <= 0.5f * q.colorScale[c] * 1.0001f + 1e-5f, "quantized colour does not carry the gain");
    }
    require(a.opacity == f.radiance[i * 4 + 3], "the gain touched opacity");
    for (int j = 0; j < k; ++j)
      for (int c = 0; c < 3; ++c) {
        const float want = f.sh[(size_t(j) * f.view.count + i) * 3 + c] * g.m;
        require(a.sh[j * 3 + c] == want, "float harmonics do not carry the gain");
        require(std::abs(b.sh[j * 3 + c] - want) <= 0.5f * q.shScale[c] * 1.0001f + 1e-5f, "quantized harmonics do not carry the gain");
      }
  }
  std::printf("intensity: %zu colours k times brighter in linear light (worst %.1e)\n", checked, worst);
}

// ---- 3: the axes ------------------------------------------------------------------------

void axes() {
  std::mt19937 random(7);
  std::uniform_real_distribution<double> unit(-1.0, 1.0), logScale(-6.0, 0.0);
  const float unitScale = 100.0f;
  const M3 ours = basis(UpAxis::Y, unitScale);
  require(determinant(ours) < 0, "the Y up basis must mirror: captures are right handed");
  require(determinant(basis(UpAxis::Z, 1.0f)) < 0, "the Z up basis must mirror too");

  double worst = 0;
  for (int trial = 0; trial < 2000; ++trial) {
    double p[3], s[3], q[4] /* wxyz, as vgsbridge hands it */, length = 0;
    for (double &v : p) v = unit(random) * 2;
    for (double &v : s) v = std::exp(logScale(random));
    for (double &v : q) { v = unit(random); length += v * v; }
    for (double &v : q) v /= std::sqrt(length);

    // Epic's import of a right handed, Y up file (GaussianSplatFactory.cpp, RH_RUB):
    // the splat is turned into Unreal's axes and drawn with an identity LocalToWorld.
    const double epicPosition[3] = {-p[2] * unitScale, p[0] * unitScale, p[1] * unitScale};
    const double epicScale[3] = {s[2] * unitScale, s[0] * unitScale, s[1] * unitScale};
    // FQuat4f(-Z, X, Y, -W) of the file's quaternion, here from wxyz.
    const double epicRotation[4] = {-q[3], q[1], q[2], -q[0]};
    const M3 epicCovariance = covariance(epicRotation, epicScale, identity());

    // Ours: the splat as the capture has it, the turn in LocalToWorld.
    const double ourRotation[4] = {q[1], q[2], q[3], q[0]};
    const M3 ourCovariance = covariance(ourRotation, s, ours);
    double ourPosition[3];
    rowTimes(p, ours, ourPosition);

    for (int c = 0; c < 3; ++c)
      require(std::abs(ourPosition[c] - epicPosition[c]) < 1e-9, "centres differ");
    M3 difference;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        difference.m[i][j] = ourCovariance.m[i][j] - epicCovariance.m[i][j];
    const double relative = largest(difference) / largest(epicCovariance);
    require(relative < 1e-9, "covariances differ by " + std::to_string(relative));
    worst = std::max(worst, relative);

    // The view direction the harmonics are evaluated with. Epic's local space is Unreal's
    // and CalculateColor turns it back to the file's: (y, z, -x). Ours is WorldToLocal.
    double d[3], len = 0;
    for (double &v : d) { v = unit(random); len += v * v; }
    for (double &v : d) v /= std::sqrt(len);
    const double epicDirection[3] = {d[1], d[2], -d[0]};
    double ourDirection[3];
    rowTimes(d, inverse(ours), ourDirection);
    len = std::sqrt(ourDirection[0] * ourDirection[0] + ourDirection[1] * ourDirection[1] +
                    ourDirection[2] * ourDirection[2]);
    for (int c = 0; c < 3; ++c)
      require(std::abs(ourDirection[c] / len - epicDirection[c]) < 1e-12, "harmonic directions differ");
  }
  std::printf("axes: 2000 splats drawn alike by Epic's per-splat turn and ours (worst %.1e)\n", worst);
}

// ---- a real capture ---------------------------------------------------------------------

#if defined(VGS_UNREAL_LAYOUT_WITH_BRIDGE)
void capture(const char *path) {
  require(vgsb_api_version() == VGSB_API_VERSION, "vgsbridge API version");
  vgsb_player *player = vgsb_open(path, 1, 2, 0);
  require(player != nullptr, std::string("vgsb_open: ") + vgsb_last_error());
  vgsb_info info{};
  vgsb_get_info(player, &info);
  const double times[] = {0.0, info.duration * 0.5};
  for (double t : times) {
    vgsb_frame frame{};
    require(vgsb_acquire(player, t, -1, &frame) == VGSB_OK, std::string("vgsb_acquire: ") + vgsb_last_error());
    require(shDegree(frame.sh_coefficients) >= 0, "the renderer cannot draw this many harmonics");
    FrameView view;
    view.count = size_t(frame.count);
    view.shCoefficients = frame.sh_coefficients;
    view.positions = frame.positions;
    view.rotations = frame.rotations;
    view.scales = frame.scales;
    view.radiance = frame.radiance;
    view.sh = frame.sh;
    double floatMs = 0, quantizedMs = 0;
    const Errors e = checkLayouts(view, &floatMs, &quantizedMs);
    std::printf("capture t=%.3f s: %llu splats, %d coefficients\n"
                "  float     %6.1f MB, packed in %.1f ms on one core\n"
                "  quantized %6.1f MB, packed in %.1f ms on one core\n"
                "  worst quantized error: position %.2e m, scale x%.4f, colour %.4f, opacity %.4f, "
                "harmonics %.4f, rotation %.4f rad\n",
                frame.seconds, (unsigned long long)frame.count, frame.sh_coefficients,
                double(view.count) * floatStride(view.shCoefficients) / 1e6, floatMs,
                double(view.count) * quantizedStride(view.shCoefficients) / 1e6, quantizedMs, e.position,
                std::exp(e.logScale), e.color, e.opacity, e.sh, e.rotation);
    vgsb_release(player);
  }
  vgsb_close(player);
}
#endif

} // namespace

int main(int argc, char **argv) {
  try {
    layouts();
    intensity();
    axes();
    if (argc > 1) {
#if defined(VGS_UNREAL_LAYOUT_WITH_BRIDGE)
      capture(argv[1]);
#else
      std::fprintf(stderr, "built without vgsbridge: the capture argument is ignored\n");
#endif
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAILED: %s\n", error.what());
    return 1;
  }
  std::puts("unreal_layout: OK");
  return 0;
}
