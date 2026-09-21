// vgsdump - writes one instant of a capture as a Gaussian splat .ply.
//
//   vgsdump boxing.vgs 1.5 frame.ply
//
// The smallest thing a consumer can do with the decoder: open, seek, read the arrays.
// vgsexport does the same for a whole timeline; the conversion itself is in plywriter.h,
// shared by both.

#include "plywriter.h"
#include "vgsdecoder/vgsdecoder.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: vgsdump <capture.vgs|capture.pgs> <seconds> <out.ply>\n");
    return 2;
  }

  try {
    vgsdec::Capture capture = vgsdec::Capture::openFile(argv[1]);
    const vgsdec::Frame &frame = capture.setTime(std::atof(argv[2]));

    std::string error;
    const long long written = plyexample::writePly(argv[3], frame, &error);
    if (written < 0) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }

    std::printf("%s  %lld splats at %.3f s (of %llu records in chunk %zu)\n", argv[3],
                written, frame.seconds,
                static_cast<unsigned long long>(frame.splatCount), frame.chunkIndex);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
