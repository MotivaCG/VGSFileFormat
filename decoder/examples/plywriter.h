#ifndef VGS_EXAMPLE_PLYWRITER_H
#define VGS_EXAMPLE_PLYWRITER_H

// Writing one decoded instant as a Gaussian splat .ply, shared by the examples.
//
// This is not part of the library: it is here to show what the frame arrays mean. Every
// field below is a straight copy from the Frame the decoder handed over, rearranged into
// the layout the usual 3DGS tools expect - which is the only reason any arithmetic
// appears at all.
//
// Two conventions differ between the decoder and a .ply, and both are undone here:
//
//   - the decoder activates opacity and scale, and turns the DC term into RGB, because
//     that is what a renderer wants; a .ply stores the raw values, so they are put back;
//   - the decoder groups the rest coefficients per coefficient and writes rotation xyzw,
//     while a .ply is channel-major and wxyz.
//
// Records that are not alive at this instant are dropped, because a .ply has no notion of
// a record that is not there.

#include "vgsdecoder/vgsdecoder.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace plyexample {

inline float inverseSigmoid(float v) {
  const float clamped = v <= 1e-6f ? 1e-6f : (v >= 1 - 1e-6f ? 1 - 1e-6f : v);
  return std::log(clamped / (1 - clamped));
}

/** The constant that turns a spherical harmonic DC term into a colour, and back. */
constexpr float C0 = 0.28209479177387814f;

/**
 * Writes `frame` to `path`. Returns the number of splats written, or -1 on failure with
 * a reason in `error`.
 */
inline long long writePly(const std::string &path, const vgsdec::Frame &frame,
                          std::string *error = nullptr) {
  std::vector<uint64_t> live;
  live.reserve(static_cast<size_t>(frame.splatCount));
  for (uint64_t i = 0; i < frame.splatCount; ++i)
    if (!frame.active || frame.active[i])
      live.push_back(i);

  std::FILE *out = std::fopen(path.c_str(), "wb");
  if (!out) {
    if (error)
      *error = "cannot create " + path;
    return -1;
  }

  const int shCoefficients = frame.sphericalHarmonics ? frame.shCoefficients : 0;
  std::fprintf(out, "ply\nformat binary_little_endian 1.0\n");
  std::fprintf(out, "element vertex %zu\n", live.size());
  for (const char *p : {"x", "y", "z", "nx", "ny", "nz"})
    std::fprintf(out, "property float %s\n", p);
  std::fprintf(out, "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n");
  for (int i = 0; i < shCoefficients * 3; ++i)
    std::fprintf(out, "property float f_rest_%d\n", i);
  std::fprintf(out, "property float opacity\n");
  for (const char *p : {"scale_0", "scale_1", "scale_2"})
    std::fprintf(out, "property float %s\n", p);
  for (const char *p : {"rot_0", "rot_1", "rot_2", "rot_3"})
    std::fprintf(out, "property float %s\n", p);
  std::fprintf(out, "end_header\n");

  std::vector<float> row;
  for (uint64_t index : live) {
    const size_t i = static_cast<size_t>(index);
    row.clear();
    row.insert(row.end(), {frame.positions[i * 3], frame.positions[i * 3 + 1],
                           frame.positions[i * 3 + 2], 0.f, 0.f, 0.f});
    for (int c = 0; c < 3; ++c)
      row.push_back((frame.colors[i * 3 + c] - 0.5f) / C0);
    for (int c = 0; c < 3; ++c)
      for (int k = 0; k < shCoefficients; ++k)
        row.push_back(frame.sphericalHarmonics[(i * size_t(shCoefficients) + size_t(k)) * 3 +
                                               size_t(c)]);
    row.push_back(inverseSigmoid(frame.opacities[i]));
    for (int c = 0; c < 3; ++c)
      row.push_back(std::log(frame.scales[i * 3 + c]));
    row.push_back(frame.rotations[i * 4 + 3]);
    for (int c = 0; c < 3; ++c)
      row.push_back(frame.rotations[i * 4 + c]);
    std::fwrite(row.data(), sizeof(float), row.size(), out);
  }

  const bool ok = std::ferror(out) == 0;
  std::fclose(out);
  if (!ok) {
    if (error)
      *error = "cannot write " + path;
    return -1;
  }
  return static_cast<long long>(live.size());
}

} // namespace plyexample
#endif
