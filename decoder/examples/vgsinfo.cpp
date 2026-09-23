// vgsinfo - opens a capture and prints what it says about itself.
//
//   vgsinfo boxing.vgs
//
// Reaching the first line of output means the file authenticated: opening it is what
// checks the signature, so nothing here is printed about a capture we cannot vouch for.

#include "vgsdecoder/vgsdecoder.h"

#include <cstdio>
#include <ctime>

namespace {

void printField(const char *label, const std::string &value) {
  if (!value.empty())
    std::printf("%-18s %s\n", label, value.c_str());
}

// UTC, because a capture that travels between machines should not carry an instant whose
// meaning depends on where it is read.
void printCreated(uint64_t millis) {
  if (!millis)
    return;
  const std::time_t seconds = std::time_t(millis / 1000);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char stamp[32] = {};
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &utc);
  std::printf("%-18s %s UTC\n", "created", stamp);
}

const char *audioFormatName(vgsdec::Capture::AudioFormat format) {
  switch (format) {
  case vgsdec::Capture::AudioFormat::Mp3: return "MP3";
  case vgsdec::Capture::AudioFormat::Aac: return "AAC";
  case vgsdec::Capture::AudioFormat::Opus: return "Opus";
  case vgsdec::Capture::AudioFormat::Wav: return "WAV";
  default: return "unknown";
  }
}

const char *imageFormatName(vgsdec::Capture::ImageFormat format) {
  switch (format) {
  case vgsdec::Capture::ImageFormat::Png: return "PNG";
  case vgsdec::Capture::ImageFormat::Jpeg: return "JPEG";
  case vgsdec::Capture::ImageFormat::Webp: return "WebP";
  default: return "unknown";
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: vgsinfo <capture.vgs|capture.pgs>\n");
    return 2;
  }

  try {
    vgsdec::Capture capture = vgsdec::Capture::openFile(argv[1]);

    std::printf("%-18s Ed25519, key %u, verified over %llu bytes\n", "signature",
                capture.signature().keyId,
                static_cast<unsigned long long>(capture.signature().signedBytes));
    std::printf("%-18s %s\n", "uuid", capture.uuidText().c_str());
    printCreated(capture.createdMillis());

    const vgsdec::Metadata &m = capture.metadata();
    printField("id", m.id);
    printField("title", m.title);
    printField("author", m.author);
    printField("project", m.projectName);
    printField("take", m.takeName);
    printField("studio", m.captureStudio);
    printField("copyright", m.copyright);
    printField("software", m.softwareName);
    printField("software version", m.softwareVersion);
    if (!m.tags.empty()) {
      std::printf("%-18s", "tags");
      for (const auto &tag : m.tags)
        std::printf(" %s", tag.c_str());
      std::printf("\n");
    }

    std::printf("\n%-18s VFGS v%u, %s pages, SH degree %u\n", "format",
                capture.version(), capture.isPlain() ? "plain" : "compressed",
                capture.shDegree());
    std::printf("%-18s %.3f s, %llu frames at %.3f fps, %zu chunks\n", "timeline",
                capture.duration(),
                static_cast<unsigned long long>(capture.frameCount()),
                capture.frameRate(), capture.chunkCount());
    const char *modes[] = {"once", "loop", "ping-pong"};
    std::printf("%-18s %s\n", "playback", modes[int(capture.playbackMode())]);
    std::printf("%-18s %llu bytes, up to %llu splats per frame\n", "size",
                static_cast<unsigned long long>(capture.fileSize()),
                static_cast<unsigned long long>(capture.maxSplatsPerFrame()));
    const double *b = capture.bounds();
    std::printf("%-18s %.3f %.3f %.3f  to  %.3f %.3f %.3f\n", "bounds", b[0], b[1],
                b[2], b[3], b[4], b[5]);

    // Each payload has its own getter, so asking what a capture carries reads like a
    // list of what a capture can carry.
    if (capture.hasAudio())
      std::printf("%-18s %s, %zu bytes\n", "audio",
                  audioFormatName(capture.audioFormat()), capture.audio().size());
    if (capture.hasThumbnail())
      std::printf("%-18s %s, %zu bytes\n", "thumbnail",
                  imageFormatName(capture.thumbnailFormat()), capture.thumbnail().size());
    if (capture.hasMetadataJson())
      std::printf("%-18s %zu bytes\n", "metadata json", capture.metadataJson().size());
    if (capture.hasMetadataJson2())
      std::printf("%-18s %zu bytes\n", "metadata json 2", capture.metadataJson2().size());

    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
