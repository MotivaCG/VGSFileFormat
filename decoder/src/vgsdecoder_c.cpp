#include "vgsdecoder/vgsdecoder_c.h"

#include "vgsdecoder/vgsdecoder.h"

#include <new>
#include <string>
#include <vector>

// The C surface is a wrapper, not a second implementation. It holds a C++ capture and
// the buffers whose lifetime C callers expect the library to own, and it stops every
// exception at the boundary: a failure becomes a status and a message.

namespace {

// One message per thread, so two threads failing at once do not overwrite each other's
// explanation. It outlives the call that produced it and nothing else.
thread_local std::string lastError;

/** A Source that calls back into C. */
class CallbackSource : public vgsdec::Source {
public:
  CallbackSource(vgs_read_fn fn, void *user, uint64_t total)
      : fn(fn), user(user), total(total) {}
  bool read(uint64_t offset, size_t size, uint8_t *into) override {
    return fn && fn(offset, size, into, user) != 0;
  }
  uint64_t size() const override { return total; }

private:
  vgs_read_fn fn = nullptr;
  void *user = nullptr;
  uint64_t total = 0;
};

struct Wrapper {
  explicit Wrapper(vgsdec::Capture &&c) : capture(std::move(c)) {}
  vgsdec::Capture capture;
  std::unique_ptr<CallbackSource> source; // owned when the capture is streamed
  std::string uuidText;
  std::string json;
  std::vector<uint8_t> extraBytes;
};

Wrapper *self(vgs_capture *handle) { return reinterpret_cast<Wrapper *>(handle); }
const Wrapper *self(const vgs_capture *handle) {
  return reinterpret_cast<const Wrapper *>(handle);
}

void fill(vgs_frame *out, const vgsdec::Frame &frame) {
  if (!out)
    return;
  out->seconds = frame.seconds;
  out->splat_count = frame.splatCount;
  out->positions = frame.positions;
  out->rotations = frame.rotations;
  out->scales = frame.scales;
  out->opacities = frame.opacities;
  out->colors = frame.colors;
  out->spherical_harmonics = frame.sphericalHarmonics;
  out->sh_coefficients = frame.shCoefficients;
  out->active = frame.active;
  out->chunk_index = frame.chunkIndex;
}

} // namespace

extern "C" {

VGS_DECODER_API const char *vgs_last_error(void) { return lastError.c_str(); }

VGS_DECODER_API uint32_t vgs_decoder_format_version(void) { return vgsdec::Capture::formatVersion(); }

VGS_DECODER_API int vgs_looks_like_capture(const uint8_t *data, size_t size) {
  return vgsdec::Capture::looksLikeCapture(data, size) ? 1 : 0;
}

VGS_DECODER_API vgs_capture *vgs_open_file(const char *path) {
  lastError.clear();
  try {
    return reinterpret_cast<vgs_capture *>(
        new Wrapper(vgsdec::Capture::openFile(path ? path : "")));
  } catch (const std::exception &e) {
    lastError = e.what();
  } catch (...) {
    lastError = "unknown failure";
  }
  return nullptr;
}

VGS_DECODER_API vgs_capture *vgs_open_memory(const uint8_t *data, size_t size) {
  lastError.clear();
  try {
    return reinterpret_cast<vgs_capture *>(
        new Wrapper(vgsdec::Capture::openMemory(data, size)));
  } catch (const std::exception &e) {
    lastError = e.what();
  } catch (...) {
    lastError = "unknown failure";
  }
  return nullptr;
}

VGS_DECODER_API vgs_capture *vgs_open_stream(vgs_read_fn fn, void *user, uint64_t total) {
  lastError.clear();
  try {
    std::unique_ptr<CallbackSource> source(new CallbackSource(fn, user, total));
    std::unique_ptr<Wrapper> wrapper(
        new Wrapper(vgsdec::Capture::openStream(*source)));
    // The source outlives the capture because the wrapper now owns both.
    wrapper->source = std::move(source);
    return reinterpret_cast<vgs_capture *>(wrapper.release());
  } catch (const std::exception &e) {
    lastError = e.what();
  } catch (...) {
    lastError = "unknown failure";
  }
  return nullptr;
}

VGS_DECODER_API void vgs_close(vgs_capture *handle) { delete self(handle); }

VGS_DECODER_API const char *vgs_id(const vgs_capture *h) { return self(h)->capture.metadata().id.c_str(); }
VGS_DECODER_API const char *vgs_title(const vgs_capture *h) {
  return self(h)->capture.metadata().title.c_str();
}
VGS_DECODER_API const char *vgs_author(const vgs_capture *h) {
  return self(h)->capture.metadata().author.c_str();
}
VGS_DECODER_API const char *vgs_project(const vgs_capture *h) {
  return self(h)->capture.metadata().projectName.c_str();
}
VGS_DECODER_API const char *vgs_take(const vgs_capture *h) {
  return self(h)->capture.metadata().takeName.c_str();
}
VGS_DECODER_API const char *vgs_studio(const vgs_capture *h) {
  return self(h)->capture.metadata().captureStudio.c_str();
}
VGS_DECODER_API const char *vgs_copyright(const vgs_capture *h) {
  return self(h)->capture.metadata().copyright.c_str();
}
VGS_DECODER_API const char *vgs_software(const vgs_capture *h) {
  return self(h)->capture.metadata().softwareName.c_str();
}
VGS_DECODER_API const char *vgs_software_version(const vgs_capture *h) {
  return self(h)->capture.metadata().softwareVersion.c_str();
}

VGS_DECODER_API size_t vgs_tag_count(const vgs_capture *h) {
  return self(h)->capture.metadata().tags.size();
}

VGS_DECODER_API const char *vgs_tag(const vgs_capture *h, size_t index) {
  const auto &tags = self(h)->capture.metadata().tags;
  return index < tags.size() ? tags[index].c_str() : "";
}

VGS_DECODER_API const uint8_t *vgs_uuid(const vgs_capture *h) { return self(h)->capture.uuid(); }

VGS_DECODER_API const char *vgs_uuid_text(const vgs_capture *h) {
  Wrapper &w = *const_cast<Wrapper *>(self(h));
  w.uuidText = w.capture.uuidText();
  return w.uuidText.c_str();
}

VGS_DECODER_API uint64_t vgs_created_millis(const vgs_capture *h) {
  return self(h)->capture.createdMillis();
}
VGS_DECODER_API uint32_t vgs_signature_algorithm(const vgs_capture *h) {
  return self(h)->capture.signature().algorithm;
}
VGS_DECODER_API uint32_t vgs_signature_key_id(const vgs_capture *h) {
  return self(h)->capture.signature().keyId;
}
VGS_DECODER_API uint64_t vgs_signed_bytes(const vgs_capture *h) {
  return self(h)->capture.signature().signedBytes;
}
VGS_DECODER_API uint32_t vgs_version(const vgs_capture *h) { return self(h)->capture.version(); }
VGS_DECODER_API int vgs_is_plain(const vgs_capture *h) { return self(h)->capture.isPlain() ? 1 : 0; }

VGS_DECODER_API double vgs_duration(const vgs_capture *h) { return self(h)->capture.duration(); }
VGS_DECODER_API uint64_t vgs_frame_count(const vgs_capture *h) { return self(h)->capture.frameCount(); }
VGS_DECODER_API double vgs_frame_rate(const vgs_capture *h) { return self(h)->capture.frameRate(); }
VGS_DECODER_API double vgs_start_seconds(const vgs_capture *h) { return self(h)->capture.startSeconds(); }
VGS_DECODER_API int vgs_playback_mode(const vgs_capture *h) { return int(self(h)->capture.playbackMode()); }
VGS_DECODER_API int vgs_motion_type(const vgs_capture *h) { return int(self(h)->capture.motionType()); }
VGS_DECODER_API float vgs_moving_speed(const vgs_capture *h) { return self(h)->capture.movingSpeed(); }
VGS_DECODER_API uint32_t vgs_sh_degree(const vgs_capture *h) { return self(h)->capture.shDegree(); }
VGS_DECODER_API uint64_t vgs_max_splats_per_frame(const vgs_capture *h) {
  return self(h)->capture.maxSplatsPerFrame();
}
VGS_DECODER_API uint64_t vgs_file_size(const vgs_capture *h) { return self(h)->capture.fileSize(); }
VGS_DECODER_API const double *vgs_bounds(const vgs_capture *h) { return self(h)->capture.bounds(); }
VGS_DECODER_API size_t vgs_chunk_count(const vgs_capture *h) { return self(h)->capture.chunkCount(); }

VGS_DECODER_API int vgs_get_chunk(const vgs_capture *h, size_t index, vgs_chunk_info *out) {
  if (!out)
    return VGS_DEC_ERROR;
  try {
    const vgsdec::ChunkInfo &info = self(h)->capture.chunk(index);
    out->start_tick = info.startTick;
    out->intervals = info.intervals;
    out->splats = info.splats;
    out->offset = info.offset;
    out->size = info.size;
    for (int i = 0; i < 6; ++i)
      out->bounds[i] = info.bounds[i];
    out->start_seconds = info.startSeconds;
    out->end_seconds = info.endSeconds;
    return VGS_DEC_OK;
  } catch (const std::exception &e) {
    lastError = e.what();
  }
  return VGS_DEC_ERROR;
}

VGS_DECODER_API size_t vgs_chunk_at(const vgs_capture *h, double seconds) {
  return self(h)->capture.chunkAt(seconds);
}

VGS_DECODER_API int vgs_set_time(vgs_capture *h, double seconds, int include_sh, vgs_frame *out) {
  lastError.clear();
  try {
    fill(out, self(h)->capture.setTime(seconds, include_sh != 0));
    return VGS_DEC_OK;
  } catch (const std::exception &e) {
    lastError = e.what();
  } catch (...) {
    lastError = "unknown failure";
  }
  return VGS_DEC_ERROR;
}

VGS_DECODER_API double vgs_time(const vgs_capture *h) { return self(h)->capture.time(); }

VGS_DECODER_API int vgs_current_frame(const vgs_capture *h, vgs_frame *out) {
  if (!out)
    return VGS_DEC_ERROR;
  fill(out, self(h)->capture.frame());
  return VGS_DEC_OK;
}

VGS_DECODER_API void vgs_release_cache(vgs_capture *h) { self(h)->capture.releaseCache(); }

} // extern "C"

namespace {

// One shape for all four: fetch, check, keep the bytes alive, report.
int handOver(Wrapper &w, std::vector<uint8_t> bytes, const uint8_t **data, size_t *size) {
  w.extraBytes = std::move(bytes);
  if (data)
    *data = w.extraBytes.empty() ? nullptr : w.extraBytes.data();
  if (size)
    *size = w.extraBytes.size();
  return VGS_DEC_OK;
}

} // namespace

extern "C" {

VGS_DECODER_API int vgs_has_audio(const vgs_capture *h) {
  return self(h)->capture.hasAudio() ? 1 : 0;
}

VGS_DECODER_API int vgs_audio_format(const vgs_capture *h) {
  return int(self(h)->capture.audioFormat());
}

VGS_DECODER_API int vgs_audio(vgs_capture *h, const uint8_t **data, size_t *size) {
  lastError.clear();
  Wrapper &w = *self(h);
  try {
    return handOver(w, w.capture.audio(), data, size);
  } catch (const std::exception &e) {
    lastError = e.what();
  }
  if (data) *data = nullptr;
  if (size) *size = 0;
  return VGS_DEC_ERROR;
}

VGS_DECODER_API int vgs_has_thumbnail(const vgs_capture *h) {
  return self(h)->capture.hasThumbnail() ? 1 : 0;
}

VGS_DECODER_API int vgs_thumbnail_format(const vgs_capture *h) {
  return int(self(h)->capture.thumbnailFormat());
}

VGS_DECODER_API int vgs_thumbnail(vgs_capture *h, const uint8_t **data, size_t *size) {
  lastError.clear();
  Wrapper &w = *self(h);
  try {
    return handOver(w, w.capture.thumbnail(), data, size);
  } catch (const std::exception &e) {
    lastError = e.what();
  }
  if (data) *data = nullptr;
  if (size) *size = 0;
  return VGS_DEC_ERROR;
}

VGS_DECODER_API int vgs_has_metadata_json(const vgs_capture *h) {
  return self(h)->capture.hasMetadataJson() ? 1 : 0;
}

VGS_DECODER_API const char *vgs_metadata_json(vgs_capture *h) {
  lastError.clear();
  Wrapper &w = *self(h);
  try {
    w.json = w.capture.metadataJson();
  } catch (const std::exception &e) {
    lastError = e.what();
    w.json.clear();
  }
  return w.json.c_str();
}

VGS_DECODER_API int vgs_has_metadata_json2(const vgs_capture *h) {
  return self(h)->capture.hasMetadataJson2() ? 1 : 0;
}

VGS_DECODER_API const char *vgs_metadata_json2(vgs_capture *h) {
  lastError.clear();
  Wrapper &w = *self(h);
  try {
    w.json = w.capture.metadataJson2();
  } catch (const std::exception &e) {
    lastError = e.what();
    w.json.clear();
  }
  return w.json.c_str();
}

} // extern "C"
