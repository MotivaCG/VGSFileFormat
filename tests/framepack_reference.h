#pragma once
// Frozen pre-optimization host conversion. Keep independent of packFrame so changes
// to selection, float arithmetic, ordering and SH layout are checked against it.
#include "../plugins/vgsbridge/src/framepack.h"
namespace legacy {
constexpr float C0 = 0.28209479177387814f;
constexpr float SmallestScale = 1e-6f;
bool keeps(size_t record, uint64_t threshold) {
  uint32_t x = uint32_t(record) * 0x9E3779B1u;
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  x *= 0xC2B2AE35u;
  x ^= x >> 16;
  return x < threshold;
}

void packFrame(const vgsdec::Frame &frame, vgsbdetail::FrameBuffers &slot, bool includeSh, float density) {
  const size_t total = size_t(frame.splatCount);

  // Thinning is one more test in the loop that already drops dead records, and everything
  // after it - the copy into the host, its packing, the upload, the sort, the drawing -
  // then has that much less to do.
  const bool thinned = density < 1.0f;
  const uint64_t threshold = uint64_t(double(density) * 4294967296.0);
  const float exponent = thinned ? 1.0f / density : 1.0f;
  const auto wanted = [&](size_t i) {
    return (!frame.active || frame.active[i]) && (!thinned || keeps(i, threshold));
  };

  size_t live = 0;
  for (size_t i = 0; i < total; ++i)
    live += wanted(i) ? 1 : 0;

  const int coefficients =
      (includeSh && frame.sphericalHarmonics) ? frame.shCoefficients : 0;
  slot.positions.resize(live * 3);
  slot.rotations.resize(live * 4);
  slot.scales.resize(live * 3);
  slot.radiance.resize(live * 4);
  slot.sh.resize(live * 3 * size_t(coefficients));

  size_t j = 0;
  for (size_t i = 0; i < total; ++i) {
    if (!wanted(i))
      continue;

    for (int c = 0; c < 3; ++c)
      slot.positions[j * 3 + c] = frame.positions[i * 3 + c];

    // xyzw in, wxyz out, normalised the way Blender's own importer does.
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
    // What a thinned-out neighbour would have covered, the ones that stay cover instead.
    const float opacity = frame.opacities[i];
    slot.radiance[j * 4 + 3] =
        thinned ? 1.0f - std::pow(std::max(0.0f, 1.0f - opacity), exponent) : opacity;

    // Per splat per coefficient in, one plane per coefficient out.
    for (int k = 0; k < coefficients; ++k)
      for (int c = 0; c < 3; ++c)
        slot.sh[(size_t(k) * live + j) * 3 + size_t(c)] =
            frame.sphericalHarmonics[(i * size_t(coefficients) + size_t(k)) * 3 + size_t(c)];
    ++j;
  }

  slot.count = live;
  slot.shCoefficients = coefficients;
}

}
