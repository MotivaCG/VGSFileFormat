// Does anything we ship contain the signing key?
//
// The separation between the two halves is arranged in core/CMakeLists.txt: the encoder
// compiles the authoring sources, the decoder does not, and vgskeys.h refuses to compile
// without VGS_AUTHORING. All of that is a convention until something checks the bytes.
//
// This reads the key out of the header and looks for it in whatever files it is given.
// Decoder artefacts must not contain it; encoder artefacts must, because a test that
// cannot find the key in the encoder is not proving anything about the decoder - it is
// looking for the wrong bytes.
//
//   keyleak --absent  vgsdecoder.lib vgsinfo.exe vgsdecoder.js
//   keyleak --present vgsencoder.lib vgsencode.exe

#define VGS_AUTHORING 1
#include "vgskeys.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool contains(const std::vector<uint8_t> &haystack, const uint8_t *needle, size_t size) {
  if (haystack.size() < size)
    return false;
  for (size_t i = 0; i + size <= haystack.size(); ++i)
    if (std::memcmp(haystack.data() + i, needle, size) == 0)
      return true;
  return false;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: keyleak --absent|--present <file>...\n");
    return 2;
  }

  const bool expectPresent = std::strcmp(argv[1], "--present") == 0;
  const auto &seed = vgs::AuthoringPrivateSeed;
  int failures = 0, checked = 0;

  for (int i = 2; i < argc; ++i) {
    std::ifstream file(argv[i], std::ios::binary);
    if (!file) {
      // A build that did not produce this artefact is not a pass. Saying so is the point:
      // a leak test that silently checks nothing is worse than no test.
      std::fprintf(stderr, "FAIL  cannot open %s\n", argv[i]);
      ++failures;
      continue;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    const bool found = contains(bytes, seed.data(), seed.size());
    ++checked;

    if (found == expectPresent) {
      std::printf("ok    %s: key %s, as expected\n", argv[i], found ? "present" : "absent");
    } else if (found) {
      std::printf("FAIL  %s: THE SIGNING KEY IS IN THIS FILE\n", argv[i]);
      ++failures;
    } else {
      std::printf("FAIL  %s: the key is not here, so this test proves nothing; the build "
                  "changed or the key did\n",
                  argv[i]);
      ++failures;
    }
  }

  std::printf("\n%d file(s) checked, %d failure(s)\n", checked, failures);
  return failures ? 1 : 0;
}
