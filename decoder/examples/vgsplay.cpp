// vgsplay - plays a capture and reports what decoding it costs.
//
//   vgsplay boxing.vgs
//
// Both delivery modes, each with and without spherical harmonics. There is no window:
// this is the decode side, which is the part a host has to budget for.
//
//   evaluated   the library decompresses the chunk and evaluates every splat into float
//               arrays, ready to use
//   packed      the library decompresses the chunk and stops, handing over buffers to
//               upload and where the frame falls between two samples
//
// The second is not "the GPU path" in the sense of this doing anything on a GPU - it
// touches no graphics API at all. It is the same decompression with the evaluation left
// out, for a renderer that will do that in its own vertex shader. The packed rows below
// are therefore not a faster way of doing the same work: they are the cost of doing less
// of it, and what is missing has to happen somewhere.
//
// Measured: reading the capture, getting a chunk ready, evaluating a frame, and for the
// packed rows the copy an upload begins with. Not measured: the driver's transfer, the
// shader that does the evaluation, and the draw call.

#include "vgsdecoder/vgsdecoder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point from) {
  return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

double percentile(std::vector<double> &sorted, double fraction) {
  if (sorted.empty())
    return 0;
  return sorted[std::min(sorted.size() - 1, size_t(double(sorted.size()) * fraction))];
}

struct Result {
  double median = 0, p99 = 0;
  double chunkMedian = 0, chunkWorst = 0;
  double firstFrame = 0;
  double uploadMillis = 0, uploadMegabytes = 0;
  double prepareMillis = 0;
  size_t chunkFrames = 0;
  uint64_t frames = 0, splats = 0;
};

/**
 * Somewhere to copy a chunk's buffers, so that the copy an upload begins with is paid
 * for rather than assumed away. It is reused between chunks: a renderer would keep its
 * GPU buffers too, rather than allocating one per chunk.
 */
std::vector<uint8_t> uploadTarget;

double simulateUpload(const vgsdec::ChunkData &data) {
  if (uploadTarget.size() < data.totalBytes)
    uploadTarget.resize(size_t(data.totalBytes));
  const auto at = Clock::now();
  size_t written = 0;
  for (size_t i = 0; i < data.bufferCount; ++i) {
    const vgsdec::Buffer &buffer = data.buffers[i];
    std::memcpy(uploadTarget.data() + written, buffer.data, size_t(buffer.size));
    written += size_t(buffer.size);
  }
  return millisSince(at);
}

/** How the pages of a chunk are decompressed, which is the part that parallelises. */
enum class Cores {
  One,      // everything on this thread
  Library,  // the decoder makes threads for each batch
  Host      // the decoder is handed a pool and never makes a thread
};

/**
 * A pool of the kind an application already has, so the third run exercises the path an
 * engine would use rather than a second copy of the library's own.
 *
 * Deliberately plain: threads that wait for work, take one item each, and signal when the
 * batch is done. The point is not that this is a good pool - the one in your engine
 * will be better - but that the decoder cannot tell the difference, and creates no
 * thread of its own when one is installed.
 */
class Pool {
public:
  explicit Pool(unsigned count) {
    for (unsigned i = 0; i < count; ++i)
      workers.emplace_back([this] { serve(); });
  }

  ~Pool() {
    {
      std::lock_guard<std::mutex> lock(guard);
      done = true;
    }
    wake.notify_all();
    for (std::thread &worker : workers)
      worker.join();
  }

  void run(size_t count, const std::function<void(size_t)> &body) {
    {
      std::lock_guard<std::mutex> lock(guard);
      job = &body;
      next = 0;
      remaining = count;
      total = count;
    }
    wake.notify_all();
    std::unique_lock<std::mutex> lock(guard);
    finished.wait(lock, [this] { return remaining == 0; });
    job = nullptr;
  }

private:
  void serve() {
    for (;;) {
      size_t index = 0;
      {
        std::unique_lock<std::mutex> lock(guard);
        wake.wait(lock, [this] { return done || (job && next < total); });
        if (done)
          return;
        index = next++;
      }
      (*job)(index);
      {
        std::lock_guard<std::mutex> lock(guard);
        --remaining;
      }
      finished.notify_all();
    }
  }

  std::vector<std::thread> workers;
  std::mutex guard;
  std::condition_variable wake, finished;
  const std::function<void(size_t)> *job = nullptr;
  size_t next = 0, remaining = 0, total = 0;
  bool done = false;
};

Result play(vgsdec::Capture &capture, bool packed, bool includeSh, double fps,
            double budget, Cores cores, unsigned width, Pool *pool) {
  capture.setOutput(packed ? vgsdec::Output::Packed : vgsdec::Output::Floats);
  capture.setThreadCount(cores == Cores::One ? 1 : width);
  if (cores == Cores::Host && pool)
    capture.setParallelFor([pool](size_t n, const std::function<void(size_t)> &body) {
      pool->run(n, body);
    });
  else
    capture.setParallelFor({});
  capture.releaseCache();

  Result result;
  std::vector<double> frames, chunkFrames;
  size_t lastChunk = size_t(-1);
  bool first = true;

  for (double t = 0; t <= capture.duration(); t += 1.0 / fps) {
    const auto before = Clock::now();
    size_t chunkIndex = 0;
    double upload = 0;

    if (packed) {
      const vgsdec::Instant at = capture.instantAt(t);
      chunkIndex = at.chunkIndex;
      // The chunk has to be there. prepare() below keeps it that way; when it has not
      // finished, this frame pays for the rest, exactly as the evaluated path does.
      while (!capture.isChunkCached(chunkIndex))
        capture.prepare(chunkIndex, 1000, includeSh);

      const vgsdec::ChunkData &data = capture.chunkData(chunkIndex);
      if (chunkIndex != lastChunk) {
        upload = simulateUpload(data);
        result.uploadMillis += upload;
        result.uploadMegabytes += double(data.totalBytes) / 1048576.0;
      }
      result.splats += data.groupCount ? data.groups[0].splats : 0;
    } else {
      const vgsdec::Frame &frame = capture.setTime(t, includeSh);
      chunkIndex = frame.chunkIndex;
      result.splats += frame.activeCount;
    }

    const double took = millisSince(before);
    if (first) {
      result.firstFrame = took;
      first = false;
    } else if (chunkIndex != lastChunk) {
      chunkFrames.push_back(took);
    } else {
      frames.push_back(took);
    }
    lastChunk = chunkIndex;
    ++result.frames;

    // What a renderer does after drawing, with the time it has left: getting the next
    // chunk ready, spread over the frames before it is needed rather than landing on the
    // one that arrives at it.
    const auto preparing = Clock::now();
    const size_t next = capture.chunkAt(t + 1.0);
    if (next < capture.chunkCount())
      capture.prepare(next, budget, includeSh);
    result.prepareMillis += millisSince(preparing);
  }

  std::sort(frames.begin(), frames.end());
  std::sort(chunkFrames.begin(), chunkFrames.end());
  result.median = percentile(frames, 0.5);
  result.p99 = percentile(frames, 0.99);
  result.chunkMedian = percentile(chunkFrames, 0.5);
  result.chunkWorst = chunkFrames.empty() ? 0 : chunkFrames.back();
  result.chunkFrames = chunkFrames.size();
  return result;
}

/**
 * What a frame really costs: what it spends producing what is drawn, plus its share of
 * getting the next chunk ready.
 *
 * Both terms matter. A chunk costs the same to get ready in either mode, and prepare()
 * spreads that over the frames before it is needed; reporting only the first term is how
 * a mode that spreads a large cost comes to look free.
 */
double perFramePrepare(const Result &r) {
  return r.frames ? r.prepareMillis / double(r.frames) : 0;
}

void row(const char *label, const Result &r, double captureFps) {
  const double prepare = perFramePrepare(r);
  const double total = r.median + prepare;
  const double perSecond = total > 0.0005 ? 1000.0 / total : 0;
  // Against the capture's own rate, because decoding faster than it was shot buys
  // nothing: what a host wants to know is how much of a frame it gets to keep.
  const double margin = captureFps > 0 ? perSecond / captureFps : 0;
  std::printf("  %-20s %9.2f %9.2f %9.2f %10.0f %8.1fx  %8.2f\n", label, r.median, prepare,
              total, perSecond, margin, r.chunkWorst);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argv[1][0] == '-') {
    std::fprintf(stderr, "usage: vgsplay <capture.vgs|capture.pgs> [--budget <ms>]\n");
    return 2;
  }

  double budget = 8;
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == "--budget" && i + 1 < argc) {
      budget = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    }
  }

  try {
    vgsdec::Capture capture = vgsdec::Capture::openFile(argv[1]);
    const double fps = capture.frameRate() > 0 ? capture.frameRate() : 30.0;

    std::printf("%s\n%.3f s, %zu chunks, SH degree %u, up to %llu splats per frame, %.0f fps\n\n",
                capture.metadata().title.c_str(), capture.duration(), capture.chunkCount(),
                capture.shDegree(),
                static_cast<unsigned long long>(capture.maxSplatsPerFrame()), fps);

    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    const unsigned width = std::min(4u, cores);
    Pool pool(width);

    // Each run plays the whole capture, so eight of them take a while. The table is
    // printed a row at a time as they finish, with the row being measured shown first,
    // rather than leaving the screen blank until the end.
    //
    // The parallel runs decompress the pages of a chunk several at a time, in both modes:
    // once on threads the decoder makes, once on a pool it is handed. What differs
    // between those two is who owns the threads, not what is done on them. The evaluated
    // pair shows where this stops helping - only the decompression is spread over cores.
    struct Run {
      const char *label;
      bool packed, includeSh;
      Cores cores;
      const char *before; // a line to print first, or nothing
    };

    // The width goes in the label rather than being left to the legend: a row saying
    // "own" alone does not say how many of anything, and these four are the rows a reader
    // compares against the ones above.
    char own[32], hosted[32];
    std::snprintf(own, sizeof own, "own x%u", width);
    std::snprintf(hosted, sizeof hosted, "pool x%u", width);
    // Spherical harmonics are what a capture normally carries, so rows say nothing when
    // they are included and NoSH when they are not. Marking the exception rather than the
    // rule keeps the labels short enough to line up.
    char evaluatedOwn[48], evaluatedPool[48], packedOwn[48], packedPool[48];
    std::snprintf(evaluatedOwn, sizeof evaluatedOwn, "evaluated, %s", own);
    std::snprintf(evaluatedPool, sizeof evaluatedPool, "evaluated, %s", hosted);
    std::snprintf(packedOwn, sizeof packedOwn, "packed, %s", own);
    std::snprintf(packedPool, sizeof packedPool, "packed, %s", hosted);

    char heading[128];
    std::snprintf(heading, sizeof heading,
                  "\n  the same, decompressing %u pages of a chunk at once\n", width);

    const Run runs[] = {
        {"evaluated", false, true, Cores::One, nullptr},
        {"evaluated NoSH", false, false, Cores::One, nullptr},
        {"packed", true, true, Cores::One, nullptr},
        {"packed NoSH", true, false, Cores::One, nullptr},
        {evaluatedOwn, false, true, Cores::Library, heading},
        {evaluatedPool, false, true, Cores::Host, nullptr},
        {packedOwn, true, true, Cores::Library, nullptr},
        {packedPool, true, true, Cores::Host, nullptr},
    };
    const size_t runCount = sizeof runs / sizeof runs[0];

    std::printf("  %-20s %9s %9s %9s %10s %9s  %8s\n", "", "evaluate", "prepare", "total",
                "frames/s", "margin", "worst");
    std::printf("  %-20s %9s %9s %9s %10s %9s  %8s\n", "", "ms", "ms", "ms", "decoded", "",
                "ms");
    std::printf("  -------------------------------------------------------------------------------\n");

    std::vector<Result> results;
    results.reserve(runCount);
    for (size_t i = 0; i < runCount; ++i) {
      const Run &run = runs[i];
      if (run.before)
        std::printf("%s", run.before);

      // Progress goes to the error stream and the table to the output stream, so the two
      // never interleave: a terminal overwrites the progress line in place, and a
      // redirected run gets a clean table with the progress somewhere else.
      std::fprintf(stderr, "  [%zu/%zu] %-18s playing %.1f s ...\r", i + 1, runCount,
                   run.label, capture.duration());
      std::fflush(stderr);

      results.push_back(play(capture, run.packed, run.includeSh, fps, budget, run.cores,
                             run.cores == Cores::One ? 1 : width, &pool));

      std::fprintf(stderr, "%*s\r", 62, "");
      std::fflush(stderr);
      row(run.label, results.back(), fps);
      std::fflush(stdout);
    }

    const Result &evaluated = results[0];
    const Result &packed = results[2];
    const Result &packedSh = results[3];

    std::printf("\n  rows\n");
    std::printf("    evaluated   the library decompresses the chunk and turns it into\n"
                "                float arrays of every splat, ready to use\n");
    std::printf("    packed      the library decompresses the chunk and stops\n"
                "                your shader evaluates it, and that is not measured here\n");
    std::printf("    NoSH        spherical harmonics left out. Rows without this mark\n"
                "                evaluate them, which is what a capture normally carries\n");
    std::printf("\n    The last four decompress the pages of a chunk %u at a time rather\n"
                "    than one after another, on a machine reporting %u cores. Pages are\n"
                "    independent, so that is the part which divides; evaluating a frame\n"
                "    does not, and stays on the calling thread - which is why the evaluate\n"
                "    column barely moves while prepare halves.\n",
                width, cores);
    std::printf("\n    own         the decoder makes %u threads of its own for each batch\n"
                "                and joins them at the end of it. One call and nothing\n"
                "                else to arrange\n", width);
    std::printf("    pool        the decoder is handed a parallel-for and makes no thread\n"
                "                at all: the same %u-way work goes to whatever task system\n"
                "                the application already runs on\n", width);
    std::printf("\n    The two cost the same, as the rows show. What differs is who owns\n"
                "    the threads, and that decides which of them you want.\n");

    std::printf("\n  choosing\n");
    std::printf("    A few characters on a machine with cores to spare is what these last\n"
                "    four rows are for: nothing else is using those cores, so spending\n"
                "    them on one capture is free speed, and %u-way took prepare from\n"
                "    %.2f ms to %.2f.\n",
                width, results[2].prepareMillis / double(results[2].frames ? results[2].frames : 1),
                results[6].prepareMillis / double(results[6].frames ? results[6].frames : 1));
    std::printf("\n    A crowd is the opposite case. Captures are already independent of\n"
                "    each other, so give each one its own Capture on its own thread and\n"
                "    they fill the machine on their own, sharing nothing and waiting for\n"
                "    nothing. Splitting each of them again as well only nests one\n"
                "    parallel-for inside another: more scheduling, no more work done, and\n"
                "    a pool that joins while it waits can stop dead. Aim for captures\n"
                "    times threads per capture to come out near the core count, and when\n"
                "    you divide by character leave the thread count at 1.\n");

    std::printf("\n  columns\n");
    std::printf("    evaluate    what one frame spends producing what is drawn\n");
    std::printf("    prepare     that frame's share of getting the next chunk ready,\n"
                "                which is the chunk's cost spread over the frames before\n"
                "                it is needed; the same work in every row\n");
    std::printf("    total       evaluate + prepare: what a frame really costs\n");
    std::printf("    frames/s    1000 / total\n");
    std::printf("    margin      frames/s against the capture's own %.0f. Below 1.0x this\n"
                "                cannot keep up; above it, that is what is left for the\n"
                "                rest of the application\n", fps);
    std::printf("    worst       the slowest frame that crossed into a new chunk\n");

    std::printf("\n  Opening a capture costs %.0f ms evaluated, %.0f ms packed, once.\n",
                evaluated.firstFrame, packed.firstFrame);
    std::printf("  The packed rows also copy %.1f MB per chunk, taking %.1f ms, which is\n"
                "  what an upload starts with.\n",
                capture.chunkCount() ? packedSh.uploadMegabytes / double(capture.chunkCount()) : 0.0,
                capture.chunkCount() ? packedSh.uploadMillis / double(capture.chunkCount()) : 0.0);

    const double evaluatedTotal = evaluated.median + perFramePrepare(evaluated);
    const double packedTotal = packed.median + perFramePrepare(packed);
    std::printf("\n  A headset frame at 90 Hz is %.1f ms: evaluated needs %.1f, packed %.1f.\n",
                1000.0 / 90.0, evaluatedTotal, packedTotal);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
