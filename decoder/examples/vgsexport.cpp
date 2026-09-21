// vgsexport - writes a whole capture out as a numbered sequence of .ply files.
//
//   vgsexport boxing.vgs out/                    every frame, at the capture's own rate
//   vgsexport boxing.vgs out/ --fps 24           resampled to 24 per second
//   vgsexport boxing.vgs out/ --from 2 --to 5    just that span
//   vgsexport boxing.vgs out/ --prefix shot_ --no-sh
//
// This is the bridge to everything that does not read VFGS: a per-frame .ply sequence is
// what the 3DGS tools, the DCC importers and the training code all understand. It is also
// the honest end-to-end test of the decoder, because every frame of every chunk goes
// through setTime and comes out as numbers somebody else can check.
//
// Be ready for the size. A capture is a few hundred megabytes precisely because it does
// not store frames independently; written back out as separate .ply files, the same take
// is routinely tens of gigabytes.

#include "plywriter.h"
#include "vgsdecoder/vgsdecoder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: vgsexport <capture.vgs|capture.pgs> <directory> [options]\n"
               "\n"
               "  --fps <n>       frames per second to write (default: the capture's)\n"
               "  --from <s>      first time to write, in seconds (default: 0)\n"
               "  --to <s>        last time to write (default: the capture's duration)\n"
               "  --prefix <s>    file name prefix (default: frame_)\n"
               "  --digits <n>    digits in the frame number (default: 5)\n"
               "  --no-sh         drop the spherical harmonic detail layers\n");
  return 2;
}

// The directory is taken as given, with a separator added if it lacks one: making one is
// the caller's business, and silently creating paths is not what an example should teach.
std::string join(const std::string &directory, const std::string &name) {
  if (directory.empty())
    return name;
  const char last = directory[directory.size() - 1];
  return last == '/' || last == '\\' ? directory + name : directory + "/" + name;
}

std::string numbered(const std::string &prefix, int index, int digits) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%0*d", digits, index);
  return prefix + buffer + ".ply";
}

} // namespace

int main(int argc, char **argv) {
  double fps = 0, from = 0, to = -1;
  std::string prefix = "frame_";
  int digits = 5;
  bool includeSh = true;

  // The capture and the directory are picked out of whatever is not an option, so they
  // can go before the options, after them, or on either side. Insisting they come first
  // means `--from 0 --to 2 out/` reads the flag as the directory and then complains about
  // a number, which tells the caller nothing about what it actually got wrong.
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag.size() < 2 || flag[0] != '-') {
      positional.push_back(flag);
      continue;
    }
    auto value = [&]() -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", flag.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (flag == "--fps")
      fps = std::atof(value());
    else if (flag == "--from")
      from = std::atof(value());
    else if (flag == "--to")
      to = std::atof(value());
    else if (flag == "--prefix")
      prefix = value();
    else if (flag == "--digits")
      digits = std::atoi(value());
    else if (flag == "--no-sh")
      includeSh = false;
    else {
      std::fprintf(stderr, "unknown option %s\n", flag.c_str());
      return usage();
    }
  }

  if (positional.size() != 2) {
    std::fprintf(stderr,
                 positional.size() < 2 ? "need a capture and a directory\n"
                                       : "expected a capture and a directory, got %zu\n",
                 positional.size());
    return usage();
  }
  const std::string capturePath = positional[0];
  const std::string directory = positional[1];

  if (digits < 1 || digits > 12) {
    std::fprintf(stderr, "--digits must be between 1 and 12\n");
    return 2;
  }

  try {
    vgsdec::Capture capture = vgsdec::Capture::openFile(capturePath);
    if (fps <= 0)
      fps = capture.frameRate() > 0 ? capture.frameRate() : 30.0;
    if (to < 0)
      to = capture.duration();
    if (from < 0)
      from = 0;
    if (to < from) {
      std::fprintf(stderr, "--to is before --from\n");
      return 2;
    }

    // Counted rather than accumulated, so a long export does not drift a frame off the
    // end of the timeline through repeated addition.
    const int count = int((to - from) * fps + 0.5) + 1;
    std::printf("%s  %.3f s  %d frames at %.3f fps -> %s\n",
                capture.metadata().title.c_str(), to - from, count, fps,
                directory.c_str());

    const auto start = std::chrono::steady_clock::now();
    unsigned long long splats = 0;

    for (int i = 0; i < count; ++i) {
      const double seconds = from + i / fps;
      const vgsdec::Frame &frame = capture.setTime(seconds, includeSh);

      const std::string path = join(directory, numbered(prefix, i, digits));
      std::string error;
      const long long written = plyexample::writePly(path, frame, &error);
      if (written < 0) {
        std::fprintf(stderr, "\n%s\n", error.c_str());
        return 1;
      }
      splats += static_cast<unsigned long long>(written);

      std::fprintf(stderr, "\r  %d/%d  %.2f s  %lld splats   ", i + 1, count, seconds,
                   written);
      std::fflush(stderr);
    }

    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(stderr, "\r%-48s\r", "");
    std::printf("%d files, %llu splats, %.1f s (%.1f frames/s)\n", count, splats, took,
                took > 0 ? count / took : 0.0);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "\n%s\n", e.what());
    return 1;
  }
}
