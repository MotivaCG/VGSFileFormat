#include "vgsencoder/vgsencoder_c.h"

#include "vgsencoder/vgsencoder.h"

#include <new>
#include <string>
#include <vector>

// The C surface is a wrapper, not a second implementation: it holds a C++ encoder and
// the buffers whose lifetime C callers expect the library to own. Nothing may escape as
// an exception, so every entry point is wrapped; a failure becomes a status and a
// message, which is what the C++ side already reports anyway.

namespace {

struct Wrapper {
  vgsenc::Encoder encoder;
  std::vector<uint8_t> bytes;  // kept so vgs_encoder_encode can hand out a pointer
  std::string uuidText;
  std::string error;           // a copy, so the returned pointer outlives the call
};

Wrapper *self(vgs_encoder *handle) { return reinterpret_cast<Wrapper *>(handle); }
const Wrapper *self(const vgs_encoder *handle) {
  return reinterpret_cast<const Wrapper *>(handle);
}

// Every setter takes a possibly null pointer from a caller we cannot see; treating null
// as the empty string is kinder than a crash and matches what a caller means by it.
std::string text(const char *value) { return value ? std::string(value) : std::string(); }

int statusOf(Wrapper &w) {
  w.error = w.encoder.lastError();
  return w.error == "cancelled" ? VGS_CANCELLED : VGS_ERROR;
}

} // namespace

extern "C" {

VGS_ENCODER_API vgs_encoder *vgs_encoder_create(void) {
  return reinterpret_cast<vgs_encoder *>(new (std::nothrow) Wrapper);
}

VGS_ENCODER_API void vgs_encoder_destroy(vgs_encoder *handle) { delete self(handle); }

VGS_ENCODER_API uint32_t vgs_format_version(void) { return vgsenc::Encoder::formatVersion(); }

VGS_ENCODER_API int vgs_encoder_set_input_file(vgs_encoder *handle, const char *path) {
  Wrapper &w = *self(handle);
  if (w.encoder.setInputFile(text(path)))
    return VGS_OK;
  return statusOf(w);
}

VGS_ENCODER_API void vgs_encoder_set_input_memory(vgs_encoder *handle, const uint8_t *data, size_t size) {
  self(handle)->encoder.setInputMemory(data, size);
}

VGS_ENCODER_API void vgs_encoder_set_coding(vgs_encoder *handle, int coding) {
  self(handle)->encoder.setCoding(coding == VGS_CODING_PLAIN ? vgsenc::Coding::Plain
                                                             : vgsenc::Coding::Compressed);
}

VGS_ENCODER_API void vgs_encoder_set_sh_degree(vgs_encoder *handle, uint32_t degree) {
  self(handle)->encoder.setSphericalHarmonicDegree(degree);
}

VGS_ENCODER_API void vgs_encoder_set_start_tick(vgs_encoder *handle, uint64_t tick) {
  self(handle)->encoder.setStartTick(tick);
}

VGS_ENCODER_API void vgs_encoder_set_page_rows(vgs_encoder *handle, uint32_t rows) {
  self(handle)->encoder.setPageRows(rows);
}

VGS_ENCODER_API void vgs_encoder_set_id(vgs_encoder *h, const char *v) { self(h)->encoder.setId(text(v)); }
VGS_ENCODER_API void vgs_encoder_set_title(vgs_encoder *h, const char *v) { self(h)->encoder.setTitle(text(v)); }
VGS_ENCODER_API void vgs_encoder_set_author(vgs_encoder *h, const char *v) { self(h)->encoder.setAuthor(text(v)); }
VGS_ENCODER_API void vgs_encoder_set_project(vgs_encoder *h, const char *v) {
  self(h)->encoder.setProjectName(text(v));
}
VGS_ENCODER_API void vgs_encoder_set_take(vgs_encoder *h, const char *v) {
  self(h)->encoder.setTakeName(text(v));
}
VGS_ENCODER_API void vgs_encoder_set_studio(vgs_encoder *h, const char *v) {
  self(h)->encoder.setCaptureStudio(text(v));
}
VGS_ENCODER_API void vgs_encoder_set_copyright(vgs_encoder *h, const char *v) {
  self(h)->encoder.setCopyright(text(v));
}
VGS_ENCODER_API void vgs_encoder_set_software(vgs_encoder *h, const char *v) {
  self(h)->encoder.setSoftwareName(text(v));
}
VGS_ENCODER_API void vgs_encoder_set_software_version(vgs_encoder *h, const char *v) {
  self(h)->encoder.setSoftwareVersion(text(v));
}
VGS_ENCODER_API void vgs_encoder_add_tag(vgs_encoder *h, const char *v) { self(h)->encoder.addTag(text(v)); }
VGS_ENCODER_API void vgs_encoder_clear_tags(vgs_encoder *h) { self(h)->encoder.clearTags(); }

VGS_ENCODER_API int vgs_encoder_set_audio_file(vgs_encoder *handle, const char *path) {
  Wrapper &w = *self(handle);
  return w.encoder.setAudioFile(text(path)) ? VGS_OK : statusOf(w);
}

VGS_ENCODER_API void vgs_encoder_set_audio(vgs_encoder *handle, const uint8_t *data, size_t size,
                           int format) {
  self(handle)->encoder.setAudio(data, size, vgsenc::AudioFormat(format));
}

VGS_ENCODER_API int vgs_encoder_set_thumbnail_file(vgs_encoder *handle, const char *path) {
  Wrapper &w = *self(handle);
  return w.encoder.setThumbnailFile(text(path)) ? VGS_OK : statusOf(w);
}

VGS_ENCODER_API void vgs_encoder_set_thumbnail(vgs_encoder *handle, const uint8_t *data, size_t size,
                               int format) {
  self(handle)->encoder.setThumbnail(data, size, vgsenc::ImageFormat(format));
}

VGS_ENCODER_API int vgs_encoder_set_metadata_json(vgs_encoder *handle, const char *json) {
  Wrapper &w = *self(handle);
  return w.encoder.setMetadataJson(text(json)) ? VGS_OK : statusOf(w);
}

VGS_ENCODER_API int vgs_encoder_set_metadata_json2(vgs_encoder *handle, const char *json) {
  Wrapper &w = *self(handle);
  return w.encoder.setMetadataJson2(text(json)) ? VGS_OK : statusOf(w);
}

VGS_ENCODER_API void vgs_encoder_set_progress(vgs_encoder *handle, vgs_progress_fn fn, void *user) {
  if (!fn) {
    self(handle)->encoder.setProgressCallback({});
    return;
  }
  self(handle)->encoder.setProgressCallback(
      [fn, user](int percent, const char *stage) { return fn(percent, stage, user) != 0; });
}

VGS_ENCODER_API void vgs_encoder_cancel(vgs_encoder *handle) { self(handle)->encoder.cancel(); }

VGS_ENCODER_API int vgs_encoder_write(vgs_encoder *handle, const char *path) {
  Wrapper &w = *self(handle);
  try {
    if (w.encoder.write(text(path))) {
      w.error.clear();
      return VGS_OK;
    }
  } catch (const std::exception &e) {
    w.error = e.what();
    return VGS_ERROR;
  } catch (...) {
    w.error = "unknown failure";
    return VGS_ERROR;
  }
  return statusOf(w);
}

VGS_ENCODER_API int vgs_encoder_encode(vgs_encoder *handle, const uint8_t **data, size_t *size) {
  Wrapper &w = *self(handle);
  try {
    if (w.encoder.encode(w.bytes)) {
      w.error.clear();
      if (data)
        *data = w.bytes.data();
      if (size)
        *size = w.bytes.size();
      return VGS_OK;
    }
  } catch (const std::exception &e) {
    w.error = e.what();
    return VGS_ERROR;
  } catch (...) {
    w.error = "unknown failure";
    return VGS_ERROR;
  }
  if (data)
    *data = nullptr;
  if (size)
    *size = 0;
  return statusOf(w);
}

VGS_ENCODER_API const uint8_t *vgs_encoder_uuid(const vgs_encoder *handle) {
  return self(handle)->encoder.uuid().data();
}

VGS_ENCODER_API const char *vgs_encoder_uuid_text(const vgs_encoder *handle) {
  Wrapper &w = *const_cast<Wrapper *>(self(handle));
  w.uuidText = w.encoder.uuidText();
  return w.uuidText.c_str();
}

VGS_ENCODER_API uint64_t vgs_encoder_output_size(const vgs_encoder *handle) {
  return self(handle)->encoder.outputSize();
}

VGS_ENCODER_API const char *vgs_encoder_last_error(const vgs_encoder *handle) {
  const Wrapper &w = *self(handle);
  // The C++ object is the authority; w.error only exists so that a message survives the
  // call that produced it.
  return w.encoder.lastError().empty() ? w.error.c_str()
                                       : w.encoder.lastError().c_str();
}

} // extern "C"
