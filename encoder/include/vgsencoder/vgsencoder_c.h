#ifndef VGSENCODER_C_H
#define VGSENCODER_C_H

// The C interface to the VFGS encoder, for callers that are not C++: a plugin ABI, a
// language binding, a host that will not link a C++ runtime. It is a thin wrapper over
// vgsencoder.h and has the same shape - make an encoder, set fields, run it.
//
//     vgs_encoder *e = vgs_encoder_create();
//     vgs_encoder_set_input_file(e, "boxing.mint");
//     vgs_encoder_set_coding(e, VGS_CODING_COMPRESSED);
//     if (vgs_encoder_write(e, "boxing.vgs") != VGS_OK)
//       fprintf(stderr, "%s\n", vgs_encoder_last_error(e));
//     vgs_encoder_destroy(e);
//
// Strings are UTF-8 and are copied on the way in. Pointers returned by this interface
// belong to the encoder and stay valid until the next call on it or until it is
// destroyed. Nothing here throws, and no C++ exception crosses the boundary.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


// A shared build exports this interface and nothing else. The C++ API in vgsencoder.h is
// compiled into the library but deliberately not exported: it passes std::string and
// std::vector across the boundary, which only works when both sides were built by the
// same compiler with the same runtime. That is a fair assumption for a static library
// and a poor one for a DLL, so a DLL offers the stable half.
#if defined(VGS_ENCODER_SHARED)
#  if defined(_WIN32)
#    if defined(VGS_ENCODER_BUILD)
#      define VGS_ENCODER_API __declspec(dllexport)
#    else
#      define VGS_ENCODER_API __declspec(dllimport)
#    endif
#  else
#    define VGS_ENCODER_API __attribute__((visibility("default")))
#  endif
#else
#  define VGS_ENCODER_API
#endif

typedef struct vgs_encoder vgs_encoder;

enum vgs_encoder_status {
  VGS_OK = 0,
  VGS_ERROR = 1,     /* see vgs_encoder_last_error */
  VGS_CANCELLED = 2  /* the progress callback asked to stop */
};

enum vgs_coding {
  VGS_CODING_COMPRESSED = 0, /* .vgs */
  VGS_CODING_PLAIN = 1       /* .pgs */
};

enum vgs_audio_format { VGS_AUDIO_MP3 = 1, VGS_AUDIO_AAC = 2, VGS_AUDIO_OPUS = 3, VGS_AUDIO_WAV = 4 };
enum vgs_image_format { VGS_IMAGE_PNG = 1, VGS_IMAGE_JPEG = 2, VGS_IMAGE_WEBP = 3 };

/** Return 0 to cancel the encode, non-zero to continue. */
typedef int (*vgs_progress_fn)(int percent, const char *stage, void *user);

VGS_ENCODER_API vgs_encoder *vgs_encoder_create(void);
VGS_ENCODER_API void vgs_encoder_destroy(vgs_encoder *);

/** The format version this build writes. */
VGS_ENCODER_API uint32_t vgs_format_version(void);

VGS_ENCODER_API int vgs_encoder_set_input_file(vgs_encoder *, const char *path);
/** The bytes are borrowed and must outlive the encode. */
VGS_ENCODER_API void vgs_encoder_set_input_memory(vgs_encoder *, const uint8_t *data, size_t size);

VGS_ENCODER_API void vgs_encoder_set_coding(vgs_encoder *, int coding);
VGS_ENCODER_API void vgs_encoder_set_sh_degree(vgs_encoder *, uint32_t degree);
VGS_ENCODER_API void vgs_encoder_set_start_tick(vgs_encoder *, uint64_t tick);
/* How players should run the capture by default: once (holding the last frame), in a
 * loop, or there and back. Loop unless set; other values are ignored. */
enum vgs_playback_mode_setting {
  VGS_ENCODE_PLAYBACK_ONCE = 0,
  VGS_ENCODE_PLAYBACK_LOOP = 1,
  VGS_ENCODE_PLAYBACK_PING_PONG = 2
};
VGS_ENCODER_API void vgs_encoder_set_playback_mode(vgs_encoder *, int mode);
VGS_ENCODER_API void vgs_encoder_set_page_rows(vgs_encoder *, uint32_t rows);

VGS_ENCODER_API void vgs_encoder_set_id(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_set_title(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_set_author(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_set_project(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_set_take(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_set_studio(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_set_copyright(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_set_software(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_set_software_version(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_add_tag(vgs_encoder *, const char *);
VGS_ENCODER_API void vgs_encoder_clear_tags(vgs_encoder *);

VGS_ENCODER_API int vgs_encoder_set_audio_file(vgs_encoder *, const char *path);
VGS_ENCODER_API void vgs_encoder_set_audio(vgs_encoder *, const uint8_t *data, size_t size, int format);
VGS_ENCODER_API int vgs_encoder_set_thumbnail_file(vgs_encoder *, const char *path);
VGS_ENCODER_API void vgs_encoder_set_thumbnail(vgs_encoder *, const uint8_t *data, size_t size, int format);
VGS_ENCODER_API int vgs_encoder_set_metadata_json(vgs_encoder *, const char *utf8_json);
VGS_ENCODER_API int vgs_encoder_set_metadata_json2(vgs_encoder *, const char *utf8_json);

VGS_ENCODER_API void vgs_encoder_set_progress(vgs_encoder *, vgs_progress_fn, void *user);
/** Safe to call from another thread while an encode runs. */
VGS_ENCODER_API void vgs_encoder_cancel(vgs_encoder *);

/** Encodes and writes the file. Returns one of vgs_encoder_status. */
VGS_ENCODER_API int vgs_encoder_write(vgs_encoder *, const char *path);

/**
 * Encodes into memory. On success *data points at the bytes and *size holds their
 * count; they belong to the encoder and stay valid until the next call on it.
 */
VGS_ENCODER_API int vgs_encoder_encode(vgs_encoder *, const uint8_t **data, size_t *size);

/** The derived identifier of the capture last written: 16 bytes, never NULL. */
VGS_ENCODER_API const uint8_t *vgs_encoder_uuid(const vgs_encoder *);
/** The same as 8-4-4-4-12 hexadecimal, NUL terminated. */
VGS_ENCODER_API const char *vgs_encoder_uuid_text(const vgs_encoder *);
VGS_ENCODER_API uint64_t vgs_encoder_output_size(const vgs_encoder *);
/** Why the last call failed. Empty after a successful one, never NULL. */
VGS_ENCODER_API const char *vgs_encoder_last_error(const vgs_encoder *);

#ifdef __cplusplus
}
#endif
#endif
