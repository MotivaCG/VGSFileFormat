// Independent container-level source comparison, including repaginated attributes.
#include "vgscodec.h"
#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
vgs::Bytes read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw vgs::Error("cannot open source verification input");
  vgs::Bytes result((std::istreambuf_iterator<char>(file)), {});
  if (file.bad()) throw vgs::Error("cannot read source verification input");
  return result;
}
}
int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: verify_source input.mint file.vgs [file.vgs ...]\n"); return 2; }
  try {
    const auto source = read(argv[1]);
    for (int i = 2; i < argc; ++i) {
      const auto capture = read(argv[i]);
      vgs::verifyMint(capture.data(), capture.size(), source.data(), source.size());
      std::printf("exact source attributes: %s\n", argv[i]);
    }
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
