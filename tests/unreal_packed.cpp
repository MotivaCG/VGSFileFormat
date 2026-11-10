// The Unreal plugin's packed playback, checked without Unreal.
//
//   vgs_unreal_packed <capture.vgs>
//
// Plays the same instants two ways - evaluated on the CPU by vgsbridge (vgsb_acquire), and
// packed (vgsb_acquire_packed) then evaluated by unreal_packed_reference.h, the C++ twin of
// the plugin's compute shader - and compares every live splat: the same splats alive, in
// the same order, with the same position, rotation, scale, colour, opacity and harmonics
// to within the half-float rounding both paths share. Then it times what each costs the
// CPU per frame, which is the reason the packed path exists.

#include "unreal_packed_reference.h"
#include "vgsbridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &what) {
  if (!condition)
    throw std::runtime_error(what);
}

struct Errors {
  double position = 0, rotation = 0, scale = 0, dc = 0, opacity = 0, sh = 0;
  void merge(const Errors &o) {
    position = std::max(position, o.position);
    rotation = std::max(rotation, o.rotation);
    scale = std::max(scale, o.scale);
    dc = std::max(dc, o.dc);
    opacity = std::max(opacity, o.opacity);
    sh = std::max(sh, o.sh);
  }
};

double relative(float a, float b) {
  return std::fabs(double(a) - double(b)) / std::max(1e-4, std::max(std::fabs(double(a)), std::fabs(double(b))));
}

Errors compare(const vgsb_frame &f, const vgsb_packed_frame &p, double extent) {
  const vgsb_packed_chunk &c = *p.chunk;
  vgspacked::Instant at;
  at.frame = p.frame;
  at.alpha = p.alpha;
  at.normalized = p.normalized;
  at.basis = p.basis;

  Errors e;
  uint64_t live = 0;
  for (uint32_t i = 0; i < uint32_t(c.splats); ++i) {
    const vgspacked::Record r = vgspacked::evaluate(c, at, i);
    if (!r.alive)
      continue;
    require(live < f.count, "packed has more live splats than the evaluated frame");
    const uint64_t j = live++;
    const float *pos = f.positions + j * 3, *rot = f.rotations + j * 4, *scl = f.scales + j * 3,
                *rad = f.radiance + j * 4;
    e.position = std::max(e.position, std::max({std::fabs(double(r.position.x - pos[0])), std::fabs(double(r.position.y - pos[1])),
                                                std::fabs(double(r.position.z - pos[2]))}) / extent);
    // The bridge's rotations are wxyz; a quaternion and its negation are the same turn.
    const double dot = double(r.rotation.x) * rot[1] + double(r.rotation.y) * rot[2] + double(r.rotation.z) * rot[3] +
                       double(r.rotation.w) * rot[0];
    e.rotation = std::max(e.rotation, 1.0 - std::min(1.0, std::fabs(dot)));
    e.scale = std::max({e.scale, relative(r.scale.x, scl[0]), relative(r.scale.y, scl[1]), relative(r.scale.z, scl[2])});
    e.dc = std::max({e.dc, std::fabs(double(r.dc.x - rad[0])), std::fabs(double(r.dc.y - rad[1])), std::fabs(double(r.dc.z - rad[2]))});
    e.opacity = std::max(e.opacity, std::fabs(double(r.opacity - rad[3])));
    require(f.sh_coefficients == c.sh_coefficients, "the two paths carry different harmonics");
    for (int k = 0; k < c.sh_coefficients; ++k) {
      const float *s = f.sh + (size_t(k) * f.count + j) * 3;
      e.sh = std::max({e.sh, std::fabs(double(r.sh[k].x - s[0])), std::fabs(double(r.sh[k].y - s[1])),
                       std::fabs(double(r.sh[k].z - s[2]))});
    }
  }
  require(live == f.count, "packed has " + std::to_string(live) + " live splats, the evaluated frame " + std::to_string(f.count));
  return e;
}

double millisSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::printf("usage: vgs_unreal_packed <capture.vgs>\n");
    return 2;
  }
  try {
    vgsb_player *evaluated = vgsb_open(argv[1], 1, 4, 2);
    require(evaluated != nullptr, std::string("vgsb_open: ") + vgsb_last_error());
    vgsb_player *packed = vgsb_open_packed(argv[1], 1, 2);
    require(packed != nullptr, std::string("vgsb_open_packed: ") + vgsb_last_error());
    vgsb_info info{};
    vgsb_get_info(packed, &info);
    double extent = 0;
    for (int k = 0; k < 3; ++k)
      extent = std::max(extent, info.bounds[3 + k] - info.bounds[k]);
    extent = std::max(extent, 1e-6);

    // Instants spread over the capture, each on its frame grid, a few inside one chunk and
    // some on either side of where chunks meet.
    std::vector<double> times;
    const double step = 1.0 / info.frame_rate;
    for (int k = 0; k < 12; ++k)
      times.push_back(std::floor(info.duration * k / 12.0 * info.frame_rate) * step);
    for (int k = 1; k <= 8; ++k)
      times.push_back(k * step);

    Errors worst;
    vgsb_frame f{};
    vgsb_packed_frame p{};
    for (double t : times) {
      vgsb_schedule(evaluated, &t, 1);
      vgsb_schedule(packed, &t, 1);
      require(vgsb_acquire(evaluated, t, -1, &f) == VGSB_OK, std::string("vgsb_acquire: ") + vgsb_last_error());
      require(vgsb_acquire_packed(packed, t, -1, &p) == VGSB_OK, std::string("vgsb_acquire_packed: ") + vgsb_last_error());
      const Errors e = compare(f, p, extent);
      std::printf("  %7.3f s  chunk %3llu  frame %2u  alpha %.3f  %8llu live  pos %.1e  rot %.1e  scale %.1e  dc %.1e  opacity %.1e  sh %.1e\n",
                  t, (unsigned long long)p.chunk->chunk_index, p.frame, p.alpha, (unsigned long long)f.count,
                  e.position, e.rotation, e.scale, e.dc, e.opacity, e.sh);
      worst.merge(e);
    }
    vgsb_release(evaluated);
    vgsb_release(packed);

    // Both paths round through half floats the same way; what is left is float32 arithmetic
    // done in a different order.
    require(worst.position < 1e-5, "positions differ");
    require(worst.rotation < 1e-5, "rotations differ");
    require(worst.scale < 1e-5, "scales differ");
    require(worst.dc < 1e-4, "colours differ");
    require(worst.opacity < 1e-5, "opacities differ");
    require(worst.sh < 1e-4, "harmonics differ");

    // What a frame costs the CPU each way, playing on: evaluated is everything, packed only
    // the basis (and, once a chunk, the packing on a lane, which this does not see).
    const int frames = int(std::min(90.0, info.duration * info.frame_rate));
    auto play = [&](bool usePacked) {
      std::vector<double> schedule;
      const auto start = std::chrono::steady_clock::now();
      for (int k = 0; k < frames; ++k) {
        schedule.clear();
        for (int a = 0; a < 45; ++a)
          schedule.push_back(std::min(info.duration, (k + a) * step));
        if (usePacked) {
          vgsb_schedule(packed, schedule.data(), int(schedule.size()));
          require(vgsb_acquire_packed(packed, schedule[0], -1, &p) == VGSB_OK, "packed playback");
        } else {
          vgsb_schedule(evaluated, schedule.data(), int(schedule.size()));
          require(vgsb_acquire(evaluated, schedule[0], -1, &f) == VGSB_OK, "evaluated playback");
        }
      }
      return millisSince(start) / frames;
    };
    const double evaluatedMs = play(false), packedMs = play(true);
    std::printf("OK: %zu instants agree. Playing %d frames as fast as they come: evaluated %.2f ms a frame, packed %.2f ms (%.1f MB a chunk to upload)\n",
                times.size(), frames, evaluatedMs, packedMs, double(p.chunk->bytes) / 1e6);
    vgsb_close(evaluated);
    vgsb_close(packed);
    return 0;
  } catch (const std::exception &e) {
    std::printf("FAILED: %s\n", e.what());
    return 1;
  }
}
