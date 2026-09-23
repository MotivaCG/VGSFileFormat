#ifndef VGSDECODER_C_H
#define VGSDECODER_C_H

// The C interface to the VFGS decoder, for callers that are not C++: a plugin ABI, a
// language binding, a host that will not link a C++ runtime. It is a thin wrapper over
// vgsdecoder.h and has the same shape - open a capture, ask it things, seek, read frames.
//
//     vgs_capture *c = vgs_open_file("boxing.vgs");
//     if (!c) { fprintf(stderr, "%s\n", vgs_last_error()); return 1; }
//     vgs_frame frame;
//     if (vgs_set_time(c, 1.5, 1, &frame) == VGS_DEC_OK)
//       draw(frame.positions, frame.splat_count);
//     vgs_close(c);
//
// Opening a capture authenticates it. A NULL return means the file is not one of ours or
// has been altered, and vgs_last_error() then reads exactly "invalid 4dgs capture".
//
// Pointers inside vgs_frame belong to the capture and are replaced by the next
// vgs_set_time on it; copy what you need to keep. Nothing here throws.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


// A shared build exports this interface and nothing else. The C++ API in vgsdecoder.h is
// compiled into the library but deliberately not exported: it passes std::string and
// std::vector across the boundary, which only works when both sides were built by the
// same compiler with the same runtime. That is a fair assumption for a static library
// and a poor one for a DLL, so a DLL offers the stable half.
#if defined(VGS_DECODER_SHARED)
#  if defined(_WIN32)
#    if defined(VGS_DECODER_BUILD)
#      define VGS_DECODER_API __declspec(dllexport)
#    else
#      define VGS_DECODER_API __declspec(dllimport)
#    endif
#  else
#    define VGS_DECODER_API __attribute__((visibility("default")))
#  endif
#else
#  define VGS_DECODER_API
#endif

typedef struct vgs_capture vgs_capture;

enum vgs_decoder_status { VGS_DEC_OK = 0, VGS_DEC_ERROR = 1 };

/** One instant. Arrays hold splat_count entries of the stated width. */
typedef struct vgs_frame {
  double seconds;
  uint64_t splat_count;
  const float *positions; /* 3 per splat, xyz */
  const float *rotations; /* 4 per splat, xyzw */
  const float *scales;    /* 3 per splat */
  const float *opacities; /* 1 per splat */
  const float *colors;    /* 3 per splat, RGB */
  const float *spherical_harmonics; /* 3 per coefficient per splat, or NULL */
  int sh_coefficients;
  const uint8_t *active;  /* 1 per splat, non-zero where the record is alive */
  size_t chunk_index;
} vgs_frame;

/** One chunk of the timeline. */
typedef struct vgs_chunk_info {
  uint64_t start_tick, intervals, splats, offset, size;
  float bounds[6];
  double start_seconds, end_seconds;
} vgs_chunk_info;

/** Reads `size` bytes at `offset` into `into`; return 0 on failure. */
typedef int (*vgs_read_fn)(uint64_t offset, size_t size, uint8_t *into, void *user);

/** NULL on failure; see vgs_last_error. */
VGS_DECODER_API vgs_capture *vgs_open_file(const char *path);
/** The bytes are borrowed and must outlive the capture. */
VGS_DECODER_API vgs_capture *vgs_open_memory(const uint8_t *data, size_t size);
/** Streams through a callback. `total` is the file's size in bytes. */
VGS_DECODER_API vgs_capture *vgs_open_stream(vgs_read_fn, void *user, uint64_t total);
VGS_DECODER_API void vgs_close(vgs_capture *);

/** Why the last call failed, for the calling thread. Never NULL. */
VGS_DECODER_API const char *vgs_last_error(void);
/** The format version this build reads. */
VGS_DECODER_API uint32_t vgs_decoder_format_version(void);
/** True when these bytes begin like a capture this build can read. Says nothing about
 *  whether the file is genuine. */
VGS_DECODER_API int vgs_looks_like_capture(const uint8_t *data, size_t size);

/* ---- what the capture says about itself. Strings are UTF-8 and belong to it. ---- */

VGS_DECODER_API const char *vgs_id(const vgs_capture *);
VGS_DECODER_API const char *vgs_title(const vgs_capture *);
VGS_DECODER_API const char *vgs_author(const vgs_capture *);
VGS_DECODER_API const char *vgs_project(const vgs_capture *);
VGS_DECODER_API const char *vgs_take(const vgs_capture *);
VGS_DECODER_API const char *vgs_studio(const vgs_capture *);
VGS_DECODER_API const char *vgs_copyright(const vgs_capture *);
VGS_DECODER_API const char *vgs_software(const vgs_capture *);
VGS_DECODER_API const char *vgs_software_version(const vgs_capture *);
VGS_DECODER_API size_t vgs_tag_count(const vgs_capture *);
VGS_DECODER_API const char *vgs_tag(const vgs_capture *, size_t index);

/** 16 raw bytes. */
VGS_DECODER_API const uint8_t *vgs_uuid(const vgs_capture *);
/** 8-4-4-4-12 hexadecimal, NUL terminated. */
VGS_DECODER_API const char *vgs_uuid_text(const vgs_capture *);
VGS_DECODER_API uint64_t vgs_created_millis(const vgs_capture *);
VGS_DECODER_API uint32_t vgs_signature_algorithm(const vgs_capture *);
VGS_DECODER_API uint32_t vgs_signature_key_id(const vgs_capture *);
VGS_DECODER_API uint64_t vgs_signed_bytes(const vgs_capture *);
VGS_DECODER_API uint32_t vgs_version(const vgs_capture *);
/** 1 when the pages are stored without entropy coding, which is what .pgs means. */
VGS_DECODER_API int vgs_is_plain(const vgs_capture *);

/* ---- the timeline ---- */

/** The last time that can be asked for, in seconds. */
VGS_DECODER_API double vgs_duration(const vgs_capture *);
VGS_DECODER_API uint64_t vgs_frame_count(const vgs_capture *);
VGS_DECODER_API double vgs_frame_rate(const vgs_capture *);
VGS_DECODER_API double vgs_start_seconds(const vgs_capture *);
/* How the capture is meant to be played: once (holding the last frame), in a loop, or
 * there and back. A player starts it this way unless its user chooses otherwise. */
enum vgs_playback_mode { VGS_PLAYBACK_ONCE = 0, VGS_PLAYBACK_LOOP = 1, VGS_PLAYBACK_PING_PONG = 2 };
VGS_DECODER_API int vgs_playback_mode(const vgs_capture *);
VGS_DECODER_API uint32_t vgs_sh_degree(const vgs_capture *);
VGS_DECODER_API uint64_t vgs_max_splats_per_frame(const vgs_capture *);
VGS_DECODER_API uint64_t vgs_file_size(const vgs_capture *);
/** 6 doubles: minXYZ then maxXYZ. */
VGS_DECODER_API const double *vgs_bounds(const vgs_capture *);

VGS_DECODER_API size_t vgs_chunk_count(const vgs_capture *);
VGS_DECODER_API int vgs_get_chunk(const vgs_capture *, size_t index, vgs_chunk_info *out);
/** The chunk covering a time, or vgs_chunk_count() when there is none. */
VGS_DECODER_API size_t vgs_chunk_at(const vgs_capture *, double seconds);

/* ---- playback ---- */

/**
 * Decodes the instant at `seconds`, clamped to [0, vgs_duration()], and fills `out`.
 * Pass include_sh as 0 to skip the colour detail layers, which is most of the work.
 */
VGS_DECODER_API int vgs_set_time(vgs_capture *, double seconds, int include_sh, vgs_frame *out);
VGS_DECODER_API double vgs_time(const vgs_capture *);
/** The instant last decoded, without decoding anything. */
VGS_DECODER_API int vgs_current_frame(const vgs_capture *, vgs_frame *out);
/** Drops the cached chunk. */
VGS_DECODER_API void vgs_release_cache(vgs_capture *);

/* ---- payloads carried alongside ----
 *
 * A capture can carry a sound track, a thumbnail and two independent blocks of JSON.
 * Each has its own pair of calls, and each checks the bytes against the signed table
 * before handing them over. The bytes belong to the capture and stay valid until the
 * next call of the same kind on it.
 */

enum vgs_audio_kind { VGS_AUDIO_NONE = 0, VGS_AUDIO_MP3 = 1, VGS_AUDIO_AAC = 2, VGS_AUDIO_OPUS = 3, VGS_AUDIO_WAV = 4 };
enum vgs_image_kind { VGS_IMAGE_NONE = 0, VGS_IMAGE_PNG = 1, VGS_IMAGE_JPEG = 2, VGS_IMAGE_WEBP = 3 };

VGS_DECODER_API int vgs_has_audio(const vgs_capture *);
/** One of vgs_audio_kind. */
VGS_DECODER_API int vgs_audio_format(const vgs_capture *);
/** The sound track as delivered. Returns VGS_DEC_OK, or VGS_DEC_ERROR; see vgs_last_error. */
VGS_DECODER_API int vgs_audio(vgs_capture *, const uint8_t **data, size_t *size);

VGS_DECODER_API int vgs_has_thumbnail(const vgs_capture *);
/** One of vgs_image_kind. */
VGS_DECODER_API int vgs_thumbnail_format(const vgs_capture *);
VGS_DECODER_API int vgs_thumbnail(vgs_capture *, const uint8_t **data, size_t *size);

VGS_DECODER_API int vgs_has_metadata_json(const vgs_capture *);
/** The free-form JSON block as NUL terminated UTF-8, or "" when there is none. */
VGS_DECODER_API const char *vgs_metadata_json(vgs_capture *);

VGS_DECODER_API int vgs_has_metadata_json2(const vgs_capture *);
VGS_DECODER_API const char *vgs_metadata_json2(vgs_capture *);

#ifdef __cplusplus
}
#endif
#endif
