// vgsencode - turns a MINT capture into a signed .vgs or .pgs.
//
//   vgsencode boxing.mint boxing.vgs --sh 2 --project "Summer shoot" --tag boxing
//
// It is the encoder API and nothing else: everything this does is a setter and a call to
// write(), so it doubles as a worked example of the library.

#include "vgsencoder/vgsencoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: vgsencode <input.mint> <output.vgs|output.pgs> [options]\n"
               "\n"
               "  --plain                store pages without entropy coding (.pgs)\n"
               "  --sh <0..3>            highest spherical harmonic degree to keep\n"
               "  --page-rows <n>        rows per page (1024..1048576)\n"
               "  --start-tick <n>       where the capture starts on an external timeline\n"
               "  --playback <mode>      how players run it: once, loop or pingpong (default loop)\n"
               "\n"
               "  --id <s>               catalogue identifier   (default: input name)\n"
               "  --title <s>            title                  (default: input name)\n"
               "  --author <s>           author\n"
               "  --project <s>          project name\n"
               "  --take <s>             take name\n"
               "  --studio <s>           capture studio\n"
               "  --copyright <s>        copyright notice\n"
               "  --software <s>         producing software\n"
               "  --software-version <s> its version\n"
               "  --tag <s>              a tag; repeat for more\n"
               "\n"
               "  --audio <file>         .mp3, .aac, .m4a, .opus or .wav\n"
               "  --thumbnail <file>     .png, .jpg or .webp\n"
               "  --metadata <file>      free-form UTF-8 JSON\n"
               "  --metadata2 <file>     a second, independent JSON block\n"
               "  --quiet                no progress output\n"
               "\n"
               "The identifier has no option: it is derived from the metadata and the\n"
               "contents, and printed when the file is written.\n");
  return 2;
}

bool readTextFile(const char *path, std::string &into) {
  std::FILE *file = std::fopen(path, "rb");
  if (!file)
    return false;
  char buffer[65536];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0)
    into.append(buffer, got);
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

// One line that rewrites itself, so a long encode says where it is without filling the
// scrollback. Only whole percents are drawn; the callback fires far more often than that.
struct Reporter {
  int last = -1;
  bool operator()(int percent, const char *stage) {
    if (percent != last) {
      last = percent;
      std::fprintf(stderr, "\r%3d%%  %-16s", percent, stage);
      std::fflush(stderr);
    }
    return true;
  }
};

} // namespace

int main(int argc, char **argv) {
  if (argc < 3 || argv[1][0] == '-')
    return usage();

  vgsenc::Encoder encoder;
  const std::string input = argv[1];
  const std::string output = argv[2];
  if (!encoder.setInputFile(input)) {
    std::fprintf(stderr, "%s\n", encoder.lastError().c_str());
    return 1;
  }

  // A .pgs asked for by name is a .pgs, so the extension and the coding cannot disagree
  // without someone having said so explicitly.
  if (output.size() > 4 && output.compare(output.size() - 4, 4, ".pgs") == 0)
    encoder.setCoding(vgsenc::Coding::Plain);

  encoder.setSoftwareName("vgsencode");
  bool quiet = false;

  for (int i = 3; i < argc; ++i) {
    const std::string flag = argv[i];
    auto value = [&](const char **out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", flag.c_str());
        std::exit(2);
      }
      *out = argv[++i];
    };
    const char *v = nullptr;

    if (flag == "--plain")
      encoder.setCoding(vgsenc::Coding::Plain);
    else if (flag == "--quiet")
      quiet = true;
    else if (flag == "--sh") {
      value(&v);
      encoder.setSphericalHarmonicDegree(uint32_t(std::atoi(v)));
    } else if (flag == "--page-rows") {
      value(&v);
      encoder.setPageRows(uint32_t(std::atoi(v)));
    } else if (flag == "--start-tick") {
      value(&v);
      encoder.setStartTick(uint64_t(std::atoll(v)));
    } else if (flag == "--playback") {
      value(&v);
      const std::string mode = v;
      if (mode == "once")
        encoder.setPlaybackMode(vgsenc::PlaybackMode::Once);
      else if (mode == "loop")
        encoder.setPlaybackMode(vgsenc::PlaybackMode::Loop);
      else if (mode == "pingpong" || mode == "ping-pong")
        encoder.setPlaybackMode(vgsenc::PlaybackMode::PingPong);
      else {
        std::fprintf(stderr, "--playback is once, loop or pingpong, not %s\n", v);
        return 2;
      }
    } else if (flag == "--id") {
      value(&v);
      encoder.setId(v);
    } else if (flag == "--title") {
      value(&v);
      encoder.setTitle(v);
    } else if (flag == "--author") {
      value(&v);
      encoder.setAuthor(v);
    } else if (flag == "--project") {
      value(&v);
      encoder.setProjectName(v);
    } else if (flag == "--take") {
      value(&v);
      encoder.setTakeName(v);
    } else if (flag == "--studio") {
      value(&v);
      encoder.setCaptureStudio(v);
    } else if (flag == "--copyright") {
      value(&v);
      encoder.setCopyright(v);
    } else if (flag == "--software") {
      value(&v);
      encoder.setSoftwareName(v);
    } else if (flag == "--software-version") {
      value(&v);
      encoder.setSoftwareVersion(v);
    } else if (flag == "--tag") {
      value(&v);
      encoder.addTag(v);
    } else if (flag == "--audio") {
      value(&v);
      if (!encoder.setAudioFile(v)) {
        std::fprintf(stderr, "%s\n", encoder.lastError().c_str());
        return 1;
      }
    } else if (flag == "--thumbnail") {
      value(&v);
      if (!encoder.setThumbnailFile(v)) {
        std::fprintf(stderr, "%s\n", encoder.lastError().c_str());
        return 1;
      }
    } else if (flag == "--metadata" || flag == "--metadata2") {
      value(&v);
      std::string json;
      if (!readTextFile(v, json)) {
        std::fprintf(stderr, "cannot read %s\n", v);
        return 1;
      }
      if (flag == "--metadata")
        encoder.setMetadataJson(json);
      else
        encoder.setMetadataJson2(json);
    } else {
      std::fprintf(stderr, "unknown option %s\n", flag.c_str());
      return usage();
    }
  }

  if (!quiet) {
    Reporter reporter;
    encoder.setProgressCallback(reporter);
  }

  if (!encoder.write(output)) {
    std::fprintf(stderr, "\n%s\n", encoder.lastError().c_str());
    return 1;
  }

  if (!quiet)
    std::fprintf(stderr, "\r%-24s\r", "");
  std::printf("%s  %llu bytes  %s\n", output.c_str(),
              static_cast<unsigned long long>(encoder.outputSize()),
              encoder.uuidText().c_str());
  return 0;
}
