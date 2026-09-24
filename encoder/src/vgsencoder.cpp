#include "vgsencoder/vgsencoder.h"

#include "vgscodec.h"
#include "vgssign.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace vgsenc {
namespace {

// Everything the container calls an extra: a payload stored beside the frames, described
// by the signed table so a reader can check it without trusting the bytes around it.
vgs::ExtraInput makeExtra(uint32_t type, uint32_t format, const uint8_t *data,
                          size_t size) {
  return {type, format, vgs::Bytes(data, data + size)};
}

bool readFile(const std::string &path, vgs::Bytes &into, std::string &error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot open " + path;
    return false;
  }
  into.assign(std::istreambuf_iterator<char>(file), {});
  if (file.bad()) {
    error = "cannot read " + path;
    return false;
  }
  return true;
}

// The file's own name, without directory or extension: the default for the fields a
// capture would otherwise carry empty.
std::string baseName(const std::string &path) {
  const size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string extensionOf(const std::string &path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return {};
  std::string ext = path.substr(dot + 1);
  for (char &c : ext)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

} // namespace

struct Encoder::State {
  std::string inputPath;
  const uint8_t *inputData = nullptr;
  size_t inputSize = 0;

  vgs::EncodeOptions options;
  Coding coding = Coding::Compressed;

  ProgressFn progress;
  std::atomic<bool> cancelled{false};

  std::array<uint8_t, 16> uuid{};
  uint64_t outputSize = 0;
  std::string error;

  // Wraps the caller's callback in what the codec expects, and folds cancellation in so
  // that one place decides a run should stop.
  vgs::Progress codecProgress(int from, int to) {
    return [this, from, to](int done, int total) {
      if (cancelled.load())
        return false;
      if (!progress)
        return true;
      const int percent = total > 0 ? from + (to - from) * done / total : from;
      return progress(percent, "coding chunks");
    };
  }

  bool report(int percent, const char *stage) {
    if (cancelled.load())
      return false;
    return !progress || progress(percent, stage);
  }
};

Encoder::Encoder() : state(new State) {}
Encoder::~Encoder() = default;
Encoder::Encoder(Encoder &&) noexcept = default;
Encoder &Encoder::operator=(Encoder &&) noexcept = default;

uint32_t Encoder::formatVersion() { return vgs::Version; }

bool Encoder::setInputFile(const std::string &path) {
  // Opened here only to fail early: a path that cannot be read is worth reporting when
  // it is given, not after the caller has filled in ten other fields.
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    state->error = "cannot open " + path;
    return false;
  }
  state->inputPath = path;
  state->inputData = nullptr;
  state->inputSize = 0;
  state->error.clear();
  return true;
}

void Encoder::setInputMemory(const uint8_t *data, size_t size) {
  state->inputPath.clear();
  state->inputData = data;
  state->inputSize = size;
}

void Encoder::setCoding(Coding coding) {
  state->coding = coding;
  state->options.compression =
      coding == Coding::Plain ? vgs::Compression::None : vgs::Compression::Auto;
}

void Encoder::setSphericalHarmonicDegree(uint32_t degree) {
  state->options.shDegree = degree > 3 ? 3 : degree;
}

void Encoder::setStartTick(uint64_t tick) { state->options.startTick = tick; }

void Encoder::setPlaybackMode(PlaybackMode mode) {
  state->options.playbackMode = vgs::PlaybackMode(uint32_t(mode));
}

void Encoder::setMotionType(MotionType type) {
  state->options.motionType = vgs::MotionType(uint32_t(type));
}

void Encoder::setMovingSpeed(float speed) { state->options.movingSpeed = speed; }

void Encoder::setPageRows(uint32_t rows) { state->options.pageRows = rows; }

void Encoder::setId(std::string v) { state->options.metadata.id = std::move(v); }
void Encoder::setTitle(std::string v) { state->options.metadata.title = std::move(v); }
void Encoder::setAuthor(std::string v) { state->options.metadata.author = std::move(v); }
void Encoder::setProjectName(std::string v) {
  state->options.metadata.projectName = std::move(v);
}
void Encoder::setTakeName(std::string v) {
  state->options.metadata.takeName = std::move(v);
}
void Encoder::setCaptureStudio(std::string v) {
  state->options.metadata.captureStudio = std::move(v);
}
void Encoder::setCopyright(std::string v) {
  state->options.metadata.copyright = std::move(v);
}
void Encoder::setSoftwareName(std::string v) {
  state->options.metadata.softwareName = std::move(v);
}
void Encoder::setSoftwareVersion(std::string v) {
  state->options.metadata.softwareVersion = std::move(v);
}
void Encoder::addTag(std::string v) {
  state->options.metadata.tags.push_back(std::move(v));
}
void Encoder::clearTags() { state->options.metadata.tags.clear(); }

void Encoder::setAudio(const uint8_t *data, size_t size, AudioFormat format) {
  state->options.extras.push_back(
      makeExtra(vgs::AudioExtra, uint32_t(format), data, size));
}

bool Encoder::setAudioFile(const std::string &path) {
  static const struct {
    const char *ext;
    AudioFormat format;
  } known[] = {{"mp3", AudioFormat::Mp3},
               {"aac", AudioFormat::Aac},
               {"m4a", AudioFormat::Aac},
               {"opus", AudioFormat::Opus},
               {"wav", AudioFormat::Wav}};
  const std::string ext = extensionOf(path);
  for (const auto &entry : known)
    if (ext == entry.ext) {
      vgs::Bytes bytes;
      if (!readFile(path, bytes, state->error))
        return false;
      setAudio(bytes.data(), bytes.size(), entry.format);
      return true;
    }
  // The container stores audio as delivered and declares what it is; it never sniffs,
  // so an unknown extension is a question for the caller rather than a guess here.
  state->error = "audio must be .mp3, .aac, .m4a, .opus or .wav";
  return false;
}

void Encoder::setThumbnail(const uint8_t *data, size_t size, ImageFormat format) {
  state->options.extras.push_back(
      makeExtra(vgs::ThumbnailExtra, uint32_t(format), data, size));
}

bool Encoder::setThumbnailFile(const std::string &path) {
  static const struct {
    const char *ext;
    ImageFormat format;
  } known[] = {{"png", ImageFormat::Png},
               {"jpg", ImageFormat::Jpeg},
               {"jpeg", ImageFormat::Jpeg},
               {"webp", ImageFormat::Webp}};
  const std::string ext = extensionOf(path);
  for (const auto &entry : known)
    if (ext == entry.ext) {
      vgs::Bytes bytes;
      if (!readFile(path, bytes, state->error))
        return false;
      setThumbnail(bytes.data(), bytes.size(), entry.format);
      return true;
    }
  state->error = "thumbnail must be .png, .jpg or .webp";
  return false;
}

bool Encoder::setMetadataJson(const std::string &utf8Json) {
  state->options.extras.push_back(
      makeExtra(vgs::MetadataExtra, vgs::JsonUtf8,
                reinterpret_cast<const uint8_t *>(utf8Json.data()), utf8Json.size()));
  return true;
}

bool Encoder::setMetadataJson2(const std::string &utf8Json) {
  state->options.extras.push_back(
      makeExtra(vgs::MetadataExtra2, vgs::JsonUtf8,
                reinterpret_cast<const uint8_t *>(utf8Json.data()), utf8Json.size()));
  return true;
}

void Encoder::setProgressCallback(ProgressFn fn) { state->progress = std::move(fn); }

void Encoder::cancel() { state->cancelled.store(true); }

bool Encoder::encode(std::vector<uint8_t> &out) {
  State &s = *state;
  s.error.clear();
  s.cancelled.store(false);
  s.outputSize = 0;
  s.uuid = {};

  try {
    vgs::Bytes owned;
    const uint8_t *data = s.inputData;
    size_t size = s.inputSize;
    if (!s.inputPath.empty()) {
      if (!s.report(0, "reading input"))
        throw vgs::Error("cancelled");
      if (!readFile(s.inputPath, owned, s.error))
        return false;
      data = owned.data();
      size = owned.size();
    }
    if (!data) {
      s.error = "no input set";
      return false;
    }

    // The fields a capture should never carry empty, defaulted from the source's own
    // name. Done here rather than in the setters, so that setting an input after a title
    // does not overwrite what the caller chose.
    vgs::EncodeOptions options = s.options;
    const std::string name =
        s.inputPath.empty() ? std::string("capture") : baseName(s.inputPath);
    if (options.metadata.title.empty())
      options.metadata.title = name;
    if (options.metadata.id.empty())
      options.metadata.id = name;
    // The one field with no setter: the identifier is derived from the metadata and the
    // contents, so it cannot be dictated, only read back afterwards.
    options.metadata.uuid = {};
    options.signer = vgs::authoringSigner();

    out = vgs::encodeMint(data, size, options, s.codecProgress(0, 95));

    if (!s.report(100, "done"))
      throw vgs::Error("cancelled");

    const vgs::Header header = vgs::readHeader(out.data(), out.size());
    s.uuid = header.metadata.uuid;
    s.outputSize = out.size();
    return true;
  } catch (const std::exception &e) {
    s.error = s.cancelled.load() ? "cancelled" : e.what();
    out.clear();
    return false;
  }
}

bool Encoder::write(const std::string &path) {
  std::vector<uint8_t> bytes;
  if (!encode(bytes))
    return false;

  // Written beside the destination and renamed over it, so an interrupted run leaves the
  // previous file intact rather than half a capture with the right name.
  const std::string temporary = path + ".part";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      state->error = "cannot create " + temporary;
      return false;
    }
    file.write(reinterpret_cast<const char *>(bytes.data()),
               std::streamsize(bytes.size()));
    file.close();
    if (!file) {
      std::remove(temporary.c_str());
      state->error = "cannot write " + temporary;
      return false;
    }
  }
  std::remove(path.c_str());
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    state->error = "cannot replace " + path;
    return false;
  }
  return true;
}

const std::array<uint8_t, 16> &Encoder::uuid() const { return state->uuid; }

std::string Encoder::uuidText() const {
  static const char *digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < state->uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out.push_back('-');
    out.push_back(digits[state->uuid[i] >> 4]);
    out.push_back(digits[state->uuid[i] & 15]);
  }
  return out;
}

uint64_t Encoder::outputSize() const { return state->outputSize; }

const std::string &Encoder::lastError() const { return state->error; }

} // namespace vgsenc
