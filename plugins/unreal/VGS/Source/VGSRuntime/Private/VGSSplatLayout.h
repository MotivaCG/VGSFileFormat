#pragma once

/*
 * How a frame from vgsbridge becomes the splat buffer Epic's Gaussian Splat renderer reads.
 *
 * Plain C++ on purpose, with nothing of Unreal in it: the plugin compiles it into its
 * module, and tests/unreal_layout.cpp compiles it on its own and checks it against a copy
 * of the renderer's unpacking, so the bytes can be verified without building the engine.
 *
 * The renderer (Engine/Plugins/GaussianSplat, UE 5.9) takes one ByteAddressBuffer per
 * object in one of two layouts, chosen per object, and its shading pass reads them as
 * GaussianSplatShading.usf does:
 *
 *   float      56 bytes: position xyz, scale xyz, rotation xyzw, SH DC rgb, opacity,
 *              then 12 bytes (rgb) per further SH coefficient;
 *   quantized  20 bytes: position 21:21:21 in two words, log scale 11:11:10, rotation
 *              11:11:10 (w rebuilt, kept non-negative), DC rgb + opacity 8:8:8:8, then
 *              4 bytes (8:8:8, low byte unused) per further SH coefficient. Every field is
 *              q * scale + bias, with the scales and biases handed to the renderer beside
 *              the buffer (FGaussianSplatRenderProxy).
 *
 * Nothing is converted per splat. The renderer applies the object's LocalToWorld to the
 * positions and to the covariance it builds from rotation and scale, and its WorldToLocal
 * to the view direction it evaluates the harmonics with. So the capture's own space stays
 * the object's local space, and the turn from the capture's axes and metres to Unreal's
 * is one matrix (captureToUnreal below) at the front of LocalToWorld. The harmonics are
 * then evaluated in the capture's space, which is the space they were fitted in.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace vgsunreal {

/* A frame as vgsbridge hands it over (vgsbridge.h, vgsb_frame): flat float arrays,
 * rotations wxyz, radiance = SH DC rgb + activated opacity, sh one plane per coefficient
 * after the DC term. */
struct FrameView {
  size_t count = 0;
  int shCoefficients = 0; // 0, 3, 8 or 15
  const float *positions = nullptr;
  const float *rotations = nullptr;
  const float *scales = nullptr;
  const float *radiance = nullptr;
  const float *sh = nullptr;
};

// Mirrors FGaussianSplatShared in GaussianSplatShared.h.
constexpr uint32_t FloatPositionOffset = 0;
constexpr uint32_t FloatScaleOffset = 12;
constexpr uint32_t FloatRotationOffset = 24;
constexpr uint32_t FloatColorOpacityOffset = 40;
constexpr uint32_t FloatBaseStride = 56;
constexpr uint32_t QuantizedColorOpacityOffset = 16;
constexpr uint32_t QuantizedBaseStride = 20;
constexpr float QuantizedMinLinearScale = 1.0e-8f;

/* The renderer's SH degree for a number of coefficients past the DC term, -1 for a count
 * it cannot draw. */
inline int shDegree(int coefficients) {
  switch (coefficients) {
  case 0: return 0;
  case 3: return 1;
  case 8: return 2;
  case 15: return 3;
  default: return -1;
  }
}

inline uint32_t floatStride(int coefficients) {
  return FloatBaseStride + 12u * uint32_t(coefficients);
}
inline uint32_t quantizedStride(int coefficients) {
  return QuantizedBaseStride + 4u * uint32_t(coefficients);
}

// ---- axes -----------------------------------------------------------------------------

enum class UpAxis {
  /* How captures are written: right handed, X right, Y up, Z towards the viewer. Turned
   * the way Epic's importer turns SPZ files (EGaussianSplatCoordinateSpace::RH_RUB):
   * Unreal (X, Y, Z) = (-z, x, y). */
  Y = 0,
  /* Right handed with Z up, as Blender: Unreal (X, Y, Z) = (x, -y, z), the usual mirror of
   * Y between the two. */
  Z = 1,
};

/* The capture-to-Unreal matrix in Unreal's row-vector convention (v' = v * M): row i is
 * where the capture's axis i lands, in centimetres when unitScale is 100 (captures are in
 * metres). Its determinant is negative: a capture is right handed and Unreal is not. */
inline void captureToUnreal(UpAxis up, float unitScale, float rows[3][3]) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      rows[r][c] = 0.0f;
  if (up == UpAxis::Z) {
    rows[0][0] = unitScale;
    rows[1][1] = -unitScale;
    rows[2][2] = unitScale;
  } else {
    rows[0][1] = unitScale;  // capture +X (right) -> Unreal +Y (right)
    rows[1][2] = unitScale;  // capture +Y (up)    -> Unreal +Z (up)
    rows[2][0] = -unitScale; // capture +Z (back)  -> Unreal -X (forward is +X)
  }
}

// ---- float layout ---------------------------------------------------------------------

/* Writes records [begin, end) into `buffer`, which holds count * floatStride records. */
inline void packFloat(const FrameView &frame, size_t begin, size_t end, uint8_t *buffer) {
  const int coefficients = frame.sh ? frame.shCoefficients : 0;
  const size_t stride = floatStride(coefficients);
  for (size_t i = begin; i < end; ++i) {
    uint8_t *record = buffer + i * stride;
    const float *r = frame.rotations + i * 4; // wxyz
    const float rotation[4] = {r[1], r[2], r[3], r[0]};
    std::memcpy(record + FloatPositionOffset, frame.positions + i * 3, 12);
    std::memcpy(record + FloatScaleOffset, frame.scales + i * 3, 12);
    std::memcpy(record + FloatRotationOffset, rotation, 16);
    std::memcpy(record + FloatColorOpacityOffset, frame.radiance + i * 4, 16);
    float *sh = reinterpret_cast<float *>(record + FloatBaseStride);
    for (int k = 0; k < coefficients; ++k) {
      const float *plane = frame.sh + (size_t(k) * frame.count + i) * 3;
      float value[3] = {plane[0], plane[1], plane[2]};
      std::memcpy(sh + k * 3, value, 12);
    }
  }
}

// ---- quantized layout -----------------------------------------------------------------

/* Value ranges over a span of records, gathered in parallel and merged. */
struct Ranges {
  float positionMin[3], positionMax[3];
  float logScaleMin[3], logScaleMax[3];
  float colorMin[4], colorMax[4]; // DC rgb, opacity
  float shMin[3], shMax[3];

  static Ranges empty() {
    Ranges r;
    const float big = std::numeric_limits<float>::max();
    for (int c = 0; c < 3; ++c) {
      r.positionMin[c] = r.logScaleMin[c] = r.shMin[c] = big;
      r.positionMax[c] = r.logScaleMax[c] = r.shMax[c] = -big;
    }
    for (int c = 0; c < 4; ++c) {
      r.colorMin[c] = big;
      r.colorMax[c] = -big;
    }
    return r;
  }

  void merge(const Ranges &o) {
    for (int c = 0; c < 3; ++c) {
      positionMin[c] = std::min(positionMin[c], o.positionMin[c]);
      positionMax[c] = std::max(positionMax[c], o.positionMax[c]);
      logScaleMin[c] = std::min(logScaleMin[c], o.logScaleMin[c]);
      logScaleMax[c] = std::max(logScaleMax[c], o.logScaleMax[c]);
      shMin[c] = std::min(shMin[c], o.shMin[c]);
      shMax[c] = std::max(shMax[c], o.shMax[c]);
    }
    for (int c = 0; c < 4; ++c) {
      colorMin[c] = std::min(colorMin[c], o.colorMin[c]);
      colorMax[c] = std::max(colorMax[c], o.colorMax[c]);
    }
  }
};

inline float logScale(float linear) { return std::log(std::max(linear, QuantizedMinLinearScale)); }

inline Ranges measure(const FrameView &frame, size_t begin, size_t end) {
  Ranges r = Ranges::empty();
  const int coefficients = frame.sh ? frame.shCoefficients : 0;
  for (size_t i = begin; i < end; ++i) {
    for (int c = 0; c < 3; ++c) {
      const float p = frame.positions[i * 3 + c];
      r.positionMin[c] = std::min(r.positionMin[c], p);
      r.positionMax[c] = std::max(r.positionMax[c], p);
      const float s = logScale(frame.scales[i * 3 + c]);
      r.logScaleMin[c] = std::min(r.logScaleMin[c], s);
      r.logScaleMax[c] = std::max(r.logScaleMax[c], s);
    }
    for (int c = 0; c < 4; ++c) {
      const float v = frame.radiance[i * 4 + c];
      r.colorMin[c] = std::min(r.colorMin[c], v);
      r.colorMax[c] = std::max(r.colorMax[c], v);
    }
    for (int k = 0; k < coefficients; ++k) {
      const float *plane = frame.sh + (size_t(k) * frame.count + i) * 3;
      for (int c = 0; c < 3; ++c) {
        r.shMin[c] = std::min(r.shMin[c], plane[c]);
        r.shMax[c] = std::max(r.shMax[c], plane[c]);
      }
    }
  }
  return r;
}

/* What the renderer multiplies and adds to each quantized field (value = q * scale +
 * bias): the proxy's PositionScale/Bias, ScaleScale/Bias, ColorScale/Bias, SHScale/Bias. */
struct Quantization {
  float positionScale[3], positionBias[3];
  float scaleScale[3], scaleBias[3];
  float colorScale[4], colorBias[4];
  float shScale[3], shBias[3];
};

inline Quantization quantization(const Ranges &r) {
  // A field with no finite range (no records, or no harmonics) gets a zero scale, which
  // packs it as 0 and reads it back as its bias.
  const auto step = [](float lo, float hi, float levels, float &scale, float &bias) {
    if (std::isfinite(lo) && std::isfinite(hi) && hi >= lo) {
      scale = (hi - lo) / levels;
      bias = lo;
    } else {
      scale = 0.0f;
      bias = 0.0f;
    }
  };
  Quantization q;
  for (int c = 0; c < 3; ++c) {
    step(r.positionMin[c], r.positionMax[c], float((1u << 21) - 1), q.positionScale[c], q.positionBias[c]);
    step(r.logScaleMin[c], r.logScaleMax[c], float((1u << (c == 2 ? 10 : 11)) - 1), q.scaleScale[c], q.scaleBias[c]);
    step(r.shMin[c], r.shMax[c], 255.0f, q.shScale[c], q.shBias[c]);
  }
  for (int c = 0; c < 4; ++c)
    step(r.colorMin[c], r.colorMax[c], 255.0f, q.colorScale[c], q.colorBias[c]);
  return q;
}

// Mirrors FGaussianSplatShared::Quantize.
inline uint32_t quantize(float value, float scale, float bias, uint32_t maxValue) {
  if (scale == 0.0f)
    return 0;
  const float q = std::round((value - bias) / scale);
  if (!(q > 0.0f)) // also NaN
    return 0;
  return q >= float(maxValue) ? maxValue : uint32_t(q);
}

/* Writes records [begin, end) into `buffer`, which holds count * quantizedStride records. */
inline void packQuantized(const FrameView &frame, const Quantization &q, size_t begin, size_t end,
                          uint8_t *buffer) {
  const int coefficients = frame.sh ? frame.shCoefficients : 0;
  const size_t stride = quantizedStride(coefficients);
  constexpr uint32_t mask21 = (1u << 21) - 1, mask11 = (1u << 11) - 1, mask10 = (1u << 10) - 1;
  constexpr float rotationScale11 = 2.0f / 2047.0f, rotationScale10 = 2.0f / 1023.0f;
  for (size_t i = begin; i < end; ++i) {
    uint32_t words[5];
    const float *p = frame.positions + i * 3;
    const uint32_t px = quantize(p[0], q.positionScale[0], q.positionBias[0], mask21);
    const uint32_t py = quantize(p[1], q.positionScale[1], q.positionBias[1], mask21);
    const uint32_t pz = quantize(p[2], q.positionScale[2], q.positionBias[2], mask21);
    words[0] = pz | (py << 21);
    words[1] = (py >> 11) | (px << 10);

    const float *s = frame.scales + i * 3;
    words[2] = (quantize(logScale(s[0]), q.scaleScale[0], q.scaleBias[0], mask11) << 21) |
               (quantize(logScale(s[1]), q.scaleScale[1], q.scaleBias[1], mask11) << 10) |
               quantize(logScale(s[2]), q.scaleScale[2], q.scaleBias[2], mask10);

    const float *r = frame.rotations + i * 4; // wxyz
    float x = r[1], y = r[2], z = r[3], w = r[0];
    if (w < 0.0f) {
      x = -x; y = -y; z = -z; w = -w;
    }
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length > 0.0f) {
      x /= length; y /= length; z /= length;
    } else {
      x = y = z = 0.0f;
    }
    words[3] = (quantize(x, rotationScale11, -1.0f, mask11) << 21) |
               (quantize(y, rotationScale11, -1.0f, mask11) << 10) |
               quantize(z, rotationScale10, -1.0f, mask10);

    const float *c = frame.radiance + i * 4;
    words[4] = (quantize(c[0], q.colorScale[0], q.colorBias[0], 255) << 24) |
               (quantize(c[1], q.colorScale[1], q.colorBias[1], 255) << 16) |
               (quantize(c[2], q.colorScale[2], q.colorBias[2], 255) << 8) |
               quantize(c[3], q.colorScale[3], q.colorBias[3], 255);

    uint8_t *record = buffer + i * stride;
    std::memcpy(record, words, sizeof(words));
    for (int k = 0; k < coefficients; ++k) {
      const float *plane = frame.sh + (size_t(k) * frame.count + i) * 3;
      const uint32_t packed = (quantize(plane[0], q.shScale[0], q.shBias[0], 255) << 24) |
                              (quantize(plane[1], q.shScale[1], q.shBias[1], 255) << 16) |
                              (quantize(plane[2], q.shScale[2], q.shBias[2], 255) << 8);
      std::memcpy(record + QuantizedBaseStride + size_t(k) * 4, &packed, 4);
    }
  }
}

} // namespace vgsunreal
