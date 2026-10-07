#include "framepack_reference.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>

using vgsbdetail::FrameBuffers;

static void equal(const FrameBuffers &a, const FrameBuffers &b) {
  if (a.count != b.count || a.shCoefficients != b.shCoefficients)
    throw std::runtime_error("frame metadata differs");
  const auto bytes = [](const std::vector<float> &x, const std::vector<float> &y, size_t n) {
    if (x.size() < n || y.size() < n || (n && std::memcmp(x.data(), y.data(), n * sizeof(float))))
      throw std::runtime_error("frame bytes differ");
  };
  const size_t n = size_t(a.count);
  bytes(a.positions, b.positions, n * 3);
  bytes(a.rotations, b.rotations, n * 4);
  bytes(a.scales, b.scales, n * 3);
  bytes(a.radiance, b.radiance, n * 4);
  bytes(a.sh, b.sh, n * 3 * size_t(a.shCoefficients));
}

static void synthetic() {
  std::mt19937 random(315);
  FrameBuffers before, after;
  size_t cases = 0;
  for (size_t n : {4096u, 0u, 1u, 127u, 128u, 129u, 511u, 4096u, 2u}) {
    std::vector<float> p(n * 3), q(n * 4), scale(n * 3), dc(n * 3), opacity(n), sh(n * 45);
    std::vector<uint8_t> active(n);
    for (auto *v : {&p, &q, &scale, &dc, &opacity, &sh})
      for (float &f : *v) f = float(int(random() % 2001) - 1000) / 1000.0f;
    if (n) {
      std::fill_n(q.data(), 4, 0.0f);
      scale[0] = std::numeric_limits<float>::quiet_NaN();
      opacity[0] = 1.0f;
    }
    if (n > 1) { q[4] = std::numeric_limits<float>::quiet_NaN(); opacity[1] = 0; }
    vgsdec::Frame frame;
    frame.splatCount = n;
    frame.positions = p.data(); frame.rotations = q.data(); frame.scales = scale.data();
    frame.colors = dc.data(); frame.opacities = opacity.data();
    for (int coefficients : {15, 8, 3, 0}) {
      frame.shCoefficients = coefficients;
      frame.sphericalHarmonics = coefficients ? sh.data() : nullptr;
      for (int mask = 0; mask < 4; ++mask) {
        for (uint8_t &v : active) v = mask == 1 ? 0 : mask == 2 ? 1 : uint8_t(random() % 3 == 0);
        frame.active = mask ? active.data() : nullptr;
        frame.activeCount = mask ? size_t(std::count_if(active.begin(), active.end(), [](uint8_t v) { return v != 0; })) : n;
        for (float density : {1.0f, 0.7f, 0.25f, 0.01f}) for (bool include : {true, false, true}) {
          legacy::packFrame(frame, before, include, density);
          vgsbdetail::packFrame(frame, after, include, density);
          equal(before, after); ++cases;
        }
      }
    }
  }
  std::printf("framepack: %zu synthetic cases passed (including reused buffers and tile boundaries)\n", cases);
}

static void benchmark(const char *path) {
  auto capture = vgsdec::Capture::openFile(path);
  FrameBuffers before, after;
  std::printf("chunk,alpha,sh,splats,old_ms,new_ms\n");
  for (size_t chunk = 0; chunk < capture.chunkCount(); ++chunk) {
    const auto info = capture.chunk(chunk);
    for (double alpha : {0.0, 0.5, 0.999}) {
      const auto &frame = capture.setTime(info.startSeconds + alpha * (info.endSeconds - info.startSeconds), true);
      // Exact comparisons also cover density changes; timed path keeps all live splats.
      for (float density : {1.0f, 0.25f, 0.01f}) for (bool include : {true, false}) {
        legacy::packFrame(frame, before, include, density);
        vgsbdetail::packFrame(frame, after, include, density);
        equal(before, after);
      }
      for (bool include : {false, true}) {
        for (int warm = 0; warm < 3; ++warm) {
          legacy::packFrame(frame, before, include, 1);
          vgsbdetail::packFrame(frame, after, include, 1);
        }
        double elapsed[2] = {};
        using Pack = void (*)(const vgsdec::Frame &, FrameBuffers &, bool, float);
        const Pack functions[] = {legacy::packFrame, vgsbdetail::packFrame};
        for (int round = 0; round < 8; ++round) {
          for (int step = 0; step < 2; ++step) {
            const int which = (round + step) % 2;
            const auto start = std::chrono::steady_clock::now();
            functions[which](frame, which ? after : before, include, 1);
            elapsed[which] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
          }
          equal(before, after);
        }
        std::printf("%zu,%.3f,%d,%llu,%.6f,%.6f\n", chunk, alpha, int(include),
                    static_cast<unsigned long long>(after.count), elapsed[0] / 8, elapsed[1] / 8);
      }
    }
  }
}

int main(int argc, char **argv) {
  try {
    if (argc > 1) benchmark(argv[1]);
    else synthetic();
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "framepack: %s\n", e.what());
    return 1;
  }
}
