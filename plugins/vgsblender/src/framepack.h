#ifndef VGSBLENDER_FRAMEPACK_H
#define VGSBLENDER_FRAMEPACK_H

#include "vgsdecoder/vgsdecoder.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Private to the player shared by Blender and Houdini; not part of the C ABI.
namespace vgsbdetail {

constexpr float C0 = 0.28209479177387814f;
constexpr float SmallestScale = 1e-6f;

struct FrameBuffers {
  uint64_t count = 0;
  int shCoefficients = 0;
  // Keep the initialized high-water mark as slots are reused. Only count and
  // shCoefficients describe the published prefix; shrinking/regrowing a vector
  // would zero floats that the next frame immediately overwrites.
  std::vector<float> positions, rotations, scales, radiance, sh;
};

inline bool keeps(size_t record, uint64_t threshold) {
  uint32_t x = uint32_t(record) * 0x9E3779B1u;
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  x *= 0xC2B2AE35u;
  x ^= x >> 16;
  return x < threshold;
}

inline void packFrame(const vgsdec::Frame &frame, FrameBuffers &slot, bool includeSh,
                      float density) {
  const size_t total = size_t(frame.splatCount);
  const bool thinned = density < 1.0f;
  const uint64_t threshold = uint64_t(double(density) * 4294967296.0);
  const float exponent = thinned ? 1.0f / density : 1.0f;
  const auto wanted = [&](size_t i) {
    return (!frame.active || frame.active[i]) && (!thinned || keeps(i, threshold));
  };
  // The decoder already counted active records. Only density filtering needs
  // another count, because it selects a subset of the live records.
  size_t live = frame.active ? size_t(frame.activeCount) : total;
  if (thinned) {
    live = 0;
    for (size_t i = 0; i < total; ++i)
      live += wanted(i) ? 1 : 0;
  }
  const int coefficients =
      (includeSh && frame.sphericalHarmonics) ? frame.shCoefficients : 0;
  const auto grow = [](std::vector<float> &values, size_t size) {
    if (values.size() < size)
      values.resize(size);
  };
  grow(slot.positions, live * 3);
  grow(slot.rotations, live * 4);
  grow(slot.scales, live * 3);
  grow(slot.radiance, live * 4);
  grow(slot.sh, live * 3 * size_t(coefficients));

  size_t j = 0;
  for (size_t i = 0; i < total; ++i) {
    if (!wanted(i))
      continue;
    for (int c = 0; c < 3; ++c)
      slot.positions[j * 3 + c] = frame.positions[i * 3 + c];

    // Preserve the original floating-point operations and quaternion convention.
    const float *q = frame.rotations + i * 4;
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    float *r = &slot.rotations[j * 4];
    if (length > 0) {
      r[0] = q[3] / length;
      r[1] = q[0] / length;
      r[2] = q[1] / length;
      r[3] = q[2] / length;
    } else {
      r[0] = 1;
      r[1] = r[2] = r[3] = 0;
    }
    for (int c = 0; c < 3; ++c)
      slot.scales[j * 3 + c] = std::max(frame.scales[i * 3 + c], SmallestScale);
    for (int c = 0; c < 3; ++c)
      slot.radiance[j * 4 + c] = (frame.colors[i * 3 + c] - 0.5f) / C0;
    const float opacity = frame.opacities[i];
    slot.radiance[j * 4 + 3] =
        thinned ? 1.0f - std::pow(std::max(0.0f, 1.0f - opacity), exponent) : opacity;

    // Per-splat coefficients become one output plane per coefficient.
    for (int k = 0; k < coefficients; ++k)
      for (int c = 0; c < 3; ++c)
        slot.sh[(size_t(k) * live + j) * 3 + size_t(c)] =
            frame.sphericalHarmonics[(i * size_t(coefficients) + size_t(k)) * 3 + size_t(c)];
    ++j;
  }
  slot.count = live;
  slot.shCoefficients = coefficients;
}

} // namespace vgsbdetail
#endif
