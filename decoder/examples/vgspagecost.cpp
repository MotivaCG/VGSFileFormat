// vgspagecost - where a capture's bytes and decoding time go, and what each level of
// detail costs to decode and to evaluate.
//
//   vgspagecost boxing.vgs
//   vgspagecost boxing.vgs --rounds 5 --threads 4
//
// What it is for. A player has three things it can ask a chunk for (vgsdec::Detail):
// positions alone, a frame at base colour, or a full frame with spherical harmonics. How
// much each costs depends on the capture - how much of it moves, how the encoder split
// its attributes, whether it is compressed - so the only honest answer is to measure the
// capture you have. This does, on this machine, and prints four things:
//
//   1. Bytes, by attribute: what every chunk's pages hold, stored and decoded, summed
//      over the capture. Marked "<- positions" are the pages Detail::Positions decodes.
//   2. Decoding a chunk, by detail level: the milliseconds prepare() takes to get one
//      chunk ready, averaged over every chunk, and what the decoded chunk then holds.
//   3. Evaluating a frame: positionsAt(), setTime() without and with harmonics, on a
//      chunk already decoded, so none of it is decoding.
//   4. What that means for a player that sorts on the CPU: decoding one chunk per chunk
//      duration plus positions every frame, as a share of one core. This is the number a
//      WebGL viewer lives or dies by - it has no compute shaders to sort with, so it
//      sorts on the CPU and needs positions there every frame.
//
// How to read it.
//
//   - Everything runs on one thread unless --threads says otherwise, which is how a
//     player that gives each capture its own thread runs it (see "using more than one
//     core" in vgsdecoder.h).
//   - The file is read into memory first, so disk speed is not in any of the numbers.
//   - This is native code. The same decoder compiled to WebAssembly ran about 1.3x
//     slower in Chrome on the machine it was written on, and a phone core is two to five
//     times slower than a desktop one. Section 4 is the one to scale by those factors: a
//     share of one core that is comfortable here and over 100% scaled is a device that
//     cannot keep up.
//   - The first round warms caches and allocators like any other; averages over several
//     rounds (--rounds) are steadier than one.
//
// It only uses the public API: Capture::pages() for section 1, prepare() with each
// Detail for section 2, and the ordinary playback calls for section 3.

#include "vgsdecoder/vgsdecoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double millisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double megabytes(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

const char *detailName(vgsdec::Detail detail) {
  switch (detail) {
  case vgsdec::Detail::Positions: return "Positions";
  case vgsdec::Detail::Base: return "Base";
  case vgsdec::Detail::Full: return "Full";
  }
  return "?";
}

struct AttributeTotals {
  uint32_t layer = 0;
  uint64_t stored = 0, decoded = 0;
  bool positions = false;
};

struct DetailCost {
  double totalMs = 0, worstMs = 0;
  uint64_t heldBytes = 0;
  int samples = 0;
};

// One chunk from nothing to ready at `detail`, as a player's prepare() loop would do it
// with no other work in between. The budget is large so the loop is one call; the cost
// is the same either way, since prepare() only stops between pages.
double decodeChunk(vgsdec::Capture &capture, size_t chunk, vgsdec::Detail detail) {
  capture.releaseCache();
  const Clock::time_point start = Clock::now();
  while (!capture.prepare(chunk, 1.0e9, detail)) {
  }
  return millisecondsSince(start);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: vgspagecost <capture.vgs|capture.pgs> [--rounds N] [--threads N]\n");
    return 2;
  }
  int rounds = 3;
  unsigned threads = 1;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--rounds") == 0)
      rounds = std::max(1, std::atoi(argv[i + 1]));
    else if (std::strcmp(argv[i], "--threads") == 0)
      threads = unsigned(std::max(1, std::atoi(argv[i + 1])));
    else {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    }
  }

  // Into memory first: the numbers are about decoding, not about the disk.
  std::ifstream file(argv[1], std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());

  try {
    vgsdec::Capture capture = vgsdec::Capture::openMemory(bytes.data(), bytes.size());
    capture.setThreadCount(threads);
    const size_t chunks = capture.chunkCount();
    double averageChunkSeconds = 0;
    for (size_t c = 0; c < chunks; ++c)
      averageChunkSeconds += capture.chunk(c).endSeconds - capture.chunk(c).startSeconds;
    averageChunkSeconds /= double(chunks);

    std::printf("%s: %zu chunks of %.2f s on average, %llu splats at most, SH degree %u, %s,"
                " %u thread(s)\n\n",
                argv[1], chunks, averageChunkSeconds,
                static_cast<unsigned long long>(capture.maxSplatsPerFrame()), capture.shDegree(),
                capture.isPlain() ? "plain" : "compressed", threads);

    // ---- 1. bytes, by attribute --------------------------------------------------------
    std::map<std::string, AttributeTotals> attributes;
    uint64_t layerStored[3] = {}, layerDecoded[3] = {}, positionStored = 0, positionDecoded = 0;
    for (size_t c = 0; c < chunks; ++c)
      for (const vgsdec::PageInfo &page : capture.pages(c)) {
        AttributeTotals &a = attributes[page.attribute];
        a.layer = page.layer;
        a.stored += page.storedSize;
        a.decoded += page.decodedSize;
        a.positions = page.usedByPositions;
        if (page.layer < 3) {
          layerStored[page.layer] += page.storedSize;
          layerDecoded[page.layer] += page.decodedSize;
        }
        if (page.usedByPositions) {
          positionStored += page.storedSize;
          positionDecoded += page.decodedSize;
        }
      }

    std::printf("1. Bytes, by attribute, over the whole capture\n\n");
    std::printf("   layer  %-28s %10s %10s\n", "attribute", "stored MB", "decoded MB");
    for (const auto &[name, a] : attributes)
      std::printf("   %5u  %-28s %10.1f %10.1f%s\n", a.layer, name.c_str(), megabytes(a.stored),
                  megabytes(a.decoded), a.positions ? "   <- positions" : "");
    const char *layerNames[3] = {"base", "static SH", "temporal SH"};
    std::printf("\n");
    for (int l = 0; l < 3; ++l)
      if (layerStored[l])
        std::printf("   %-12s layer %10.1f stored %10.1f decoded MB\n", layerNames[l],
                    megabytes(layerStored[l]), megabytes(layerDecoded[l]));
    std::printf("   %-12s       %10.1f stored %10.1f decoded MB  (%.0f%% of the base layer's "
                "decoded bytes)\n\n",
                "positions", megabytes(positionStored), megabytes(positionDecoded),
                layerDecoded[0] ? 100.0 * double(positionDecoded) / double(layerDecoded[0]) : 0.0);

    // ---- 2. decoding a chunk, by detail level ---------------------------------------
    const vgsdec::Detail levels[3] = {vgsdec::Detail::Positions, vgsdec::Detail::Base,
                                      vgsdec::Detail::Full};
    DetailCost cost[3];
    for (int round = 0; round < rounds; ++round)
      for (size_t c = 0; c < chunks; ++c)
        for (int d = 0; d < 3; ++d) {
          const double ms = decodeChunk(capture, c, levels[d]);
          cost[d].totalMs += ms;
          cost[d].worstMs = std::max(cost[d].worstMs, ms);
          cost[d].heldBytes += capture.cachedBytes();
          ++cost[d].samples;
        }

    std::printf("2. Decoding one chunk (prepare), averaged over %zu chunks x %d round(s)\n\n",
                chunks, rounds);
    std::printf("   %-10s %10s %10s %12s %14s\n", "detail", "ms", "worst ms", "held MB",
                "vs Base");
    const double baseMs = cost[1].totalMs / cost[1].samples;
    for (int d = 0; d < 3; ++d) {
      const double ms = cost[d].totalMs / cost[d].samples;
      std::printf("   %-10s %10.1f %10.1f %12.1f %13.0f%%\n", detailName(levels[d]), ms,
                  cost[d].worstMs, megabytes(cost[d].heldBytes / uint64_t(cost[d].samples)),
                  100.0 * ms / baseMs);
    }
    std::printf("\n");

    // ---- 3. evaluating a frame ---------------------------------------------------------
    // On the middle chunk, decoded in full first, so every call below is evaluation only.
    const size_t middle = chunks / 2;
    const vgsdec::ChunkInfo &info = capture.chunk(middle);
    capture.releaseCache();
    while (!capture.prepare(middle, 1.0e9, vgsdec::Detail::Full)) {
    }
    const int evaluations = 30;
    auto timeEach = [&](auto &&call) {
      double total = 0;
      for (int i = 0; i < evaluations; ++i) {
        // Spread over the chunk, never quite reaching its end, which is the next chunk.
        const double t = info.startSeconds +
                         (info.endSeconds - info.startSeconds) * (double(i) + 0.5) / evaluations;
        const Clock::time_point start = Clock::now();
        call(t);
        total += millisecondsSince(start);
      }
      return total / evaluations;
    };
    const double positionsMs = timeEach([&](double t) { capture.positionsAt(t); });
    const double baseFrameMs = timeEach([&](double t) { capture.setTime(t, false); });
    const double fullFrameMs = timeEach([&](double t) { capture.setTime(t, true); });

    std::printf("3. Evaluating one instant on a decoded chunk, averaged over %d instants\n\n",
                evaluations);
    std::printf("   positionsAt              %8.2f ms\n", positionsMs);
    std::printf("   setTime, base colour     %8.2f ms\n", baseFrameMs);
    std::printf("   setTime, with SH         %8.2f ms\n\n", fullFrameMs);

    // ---- 4. what a CPU-sorting player spends -------------------------------------------
    // Per second of playback at the capture's own rate: one chunk decode per chunk
    // duration, and positions once per source frame.
    const double fps = capture.frameRate();
    const double perFrame = positionsMs * fps;
    std::printf("4. A player that sorts on the CPU, per second of playback at %.0f fps\n\n", fps);
    std::printf("   %-34s %10s %10s %12s\n", "the sorter's decoder prepares", "decode", "positions",
                "one core");
    for (int d = 0; d < 2; ++d) {
      const double decodePerSecond = (cost[d].totalMs / cost[d].samples) / averageChunkSeconds;
      std::printf("   %-34s %8.0f ms %8.0f ms %11.0f%%\n", detailName(levels[d]), decodePerSecond,
                  perFrame, (decodePerSecond + perFrame) / 10.0);
    }
    std::printf("\n   Scale the last column by how much slower the target is than this machine:"
                "\n   about 1.3x for WebAssembly, and 2-5x more for a phone. Over 100%% is a"
                "\n   device that cannot hold the capture's frame rate on one thread.\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
