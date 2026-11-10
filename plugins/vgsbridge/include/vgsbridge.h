#ifndef VGSBRIDGE_H
#define VGSBRIDGE_H

/*
 * vgsbridge: playing VGS captures inside a host that pulls frames.
 *
 * It is the native component a host integration loads to play captures, and the boundary
 * between the closed VGS code and the integration's own: everything that reads the format
 * is in here, behind this small C interface, and the integration only calls it and copies
 * what it hands back. An integration that must be free software, such as a plugin for a
 * GPL host, stays entirely its own source while using this separately supplied library
 * under the VGS Decoder licence (see LICENSE.md beside this folder).
 *
 * The decoder hands over one instant at a time and belongs to one thread. A host
 * typically asks for a frame on its main thread when the playhead moves and wants it at
 * once; decoding it there would cost the whole frame budget, and a chunk boundary would
 * stall playback for a fifth of a second. So each player decodes on threads of its own:
 * two lanes, one for the even chunks and one for the odd, so that while one evaluates the
 * frames the host says it will want next into a small ring of slots, the other has the
 * next chunk decompressed before playback reaches it. Two chunks are held decoded.
 *
 *     vgsb_player *p = vgsb_open("boxing.vgs", 1, 4, 0);
 *     double next[] = {t, t + dt, t + 2 * dt, t + 3 * dt};
 *     vgsb_schedule(p, next, 4);             // what playback will ask for, soonest first
 *     vgsb_frame frame;
 *     if (vgsb_acquire(p, t, 250, &frame) == VGSB_OK) {
 *       upload(frame.positions, frame.rotations, ...);
 *       vgsb_release(p);                     // the slot can be refilled now
 *     }
 *
 * Frames come out in a plain, host-independent layout of flat float arrays, which an
 * integration copies into its own attributes or reshapes as it copies:
 *
 *   - records that are not alive at the instant are left out, and `count` is what is
 *     left, so a dead record with a zero scale never stretches the range of a host that
 *     quantises attributes over every point;
 *   - rotations are unit quaternions in wxyz order;
 *   - `radiance` is the spherical harmonic DC term followed by the activated opacity;
 *   - `sh` is one plane of 3 * count floats per coefficient, plane k holding coefficient k
 *     of every record.
 *
 * Scales and opacities are activated, not stored as logarithms and logits.
 *
 * Several players run side by side: one per capture in the scene, each with its own
 * lanes. Every function is safe to call from any one thread at a time per player, such as
 * a host's main thread, or its render thread while rendering.
 *
 * Current integrations: the Blender add-on loads the shared library through this C
 * interface, and so does the Unreal plugin, which plays packed (below) and evaluates on the
 * GPU; the Houdini plugin compiles the same player into its SOP with VGSBRIDGE_STATIC
 * defined.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(VGSBRIDGE_STATIC)
/* Compiled into another module, as the Houdini plugin does: nothing to export. */
#  define VGSB_API
#elif defined(_WIN32)
#  ifdef VGSBRIDGE_BUILD
#    define VGSB_API __declspec(dllexport)
#  else
#    define VGSB_API __declspec(dllimport)
#  endif
#else
#  define VGSB_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever a struct below or a signature changes, so a stale add-on refuses to load
 * a newer library rather than read its structs wrongly. */
#define VGSB_API_VERSION 5

typedef struct vgsb_player vgsb_player;

enum vgsb_status { VGSB_OK = 0, VGSB_TIMEOUT = 1, VGSB_ERROR = -1 };

/* What a capture says about itself. Strings are UTF-8 and live as long as the player. */
typedef struct vgsb_info {
  double duration;      /* the last time that can be asked for, in seconds */
  double frame_rate;    /* of the capture's own timebase */
  double start_seconds; /* where it starts on an external timeline */
  double bounds[6];     /* minXYZ then maxXYZ over every frame */
  uint64_t frame_count;
  uint64_t max_splats;
  uint32_t sh_degree;
  int32_t sh_coefficients; /* 0, 3, 8 or 15: what a frame carries with harmonics on */
  int32_t playback_mode;   /* how its author means it to play: 0 once, 1 loop, 2 ping-pong */
  const char *title, *author, *project, *take, *studio, *copyright, *id, *uuid;
} vgsb_info;

/* One decoded instant. The arrays belong to the player and stay put until vgsb_release,
 * the next vgsb_acquire or vgsb_close on it. */
typedef struct vgsb_frame {
  double seconds;
  uint64_t count;          /* live splats; every array is sized by this */
  int32_t sh_coefficients; /* planes in `sh`, 0 when decoded without harmonics */
  int32_t reserved;
  const float *positions;  /* 3 per splat, xyz */
  const float *rotations;  /* 4 per splat, wxyz, normalised */
  const float *scales;     /* 3 per splat, activated */
  const float *radiance;   /* 4 per splat: DC rgb, activated opacity */
  const float *sh;         /* sh_coefficients planes of 3 * count floats */
  const float *normals;    /* 3 per splat, outward and unit length; NULL when the capture
                            * carries none */
} vgsb_frame;

VGSB_API int vgsb_api_version(void);

/* Why the last call on this thread failed. */
VGSB_API const char *vgsb_last_error(void);

/*
 * Opens and authenticates a capture and starts its decoding lanes. Null on failure.
 *
 * `slots` is how many decoded frames are held at once, the one being shown included;
 * 2 to 64. `threads` is how many cores a chunk decode may use, 0 for half of them.
 */
VGSB_API vgsb_player *vgsb_open(const char *path_utf8, int include_sh, int slots, int threads);
VGSB_API void vgsb_close(vgsb_player *);

VGSB_API int vgsb_get_info(const vgsb_player *, vgsb_info *out);

/*
 * The times the host expects to ask for, soonest first. The first `slots` are decoded
 * ahead into slots; the rest only have their chunks decompressed ahead of time, which is
 * what keeps a chunk boundary from stalling playback. Replaces the previous schedule.
 */
VGSB_API void vgsb_schedule(vgsb_player *, const double *seconds, int count);

/*
 * The frame at `seconds`, waiting up to `timeout_ms` for it (negative waits as long as it
 * takes). VGSB_TIMEOUT leaves the frame untouched; the request stands, so the frame keeps
 * being decoded and a later call gets it.
 */
VGSB_API int vgsb_acquire(vgsb_player *, double seconds, double timeout_ms, vgsb_frame *out);

/*
 * As vgsb_acquire, but when `seconds` is not ready by the timeout, settles for the ready
 * frame nearest it among those past `after` - the frame on screen, excluded - on the way
 * to `seconds`, in either direction of play; `out->seconds` says which. VGSB_TIMEOUT only
 * when there is none of those either.
 *
 * For a machine that cannot decode every frame in time, which with several captures
 * playing is any machine sooner or later. Asking for the exact frame, such a capture
 * decodes each frame just after the playhead has left it, and shows almost none; settling
 * for a late one shows every frame it manages to decode, at a lower rate.
 */
VGSB_API int vgsb_acquire_between(vgsb_player *, double seconds, double after, double timeout_ms,
                                  vgsb_frame *out);

/* Done with the acquired frame: its slot may be refilled. */
VGSB_API void vgsb_release(vgsb_player *);

/* Whether frames carry spherical harmonics. Changing it discards what is decoded ahead. */
VGSB_API void vgsb_set_include_sh(vgsb_player *, int include_sh);

/*
 * The fraction of splats frames carry, from 0.01 to 1, for a lighter viewport. 1, the
 * default, is every splat.
 *
 * Which splats stay is decided by a hash of each record's index, so the choice holds
 * still from frame to frame within a chunk rather than shimmering; it changes at chunk
 * boundaries, where the records themselves change. The ones that stay have their opacity
 * raised to 1 - (1 - opacity)^(1 / density), which keeps the total opacity where splats
 * overlap about what it was, so a thinned capture does not turn translucent. Changing it
 * discards what is decoded ahead.
 */
VGSB_API void vgsb_set_density(vgsb_player *, float density);

/*
 * How many decoded chunks each lane keeps besides the one it is on, and a ceiling on what
 * they may take together, in bytes (0 for none). 0 chunks, the default, keeps only the
 * chunk being played, which is all that playing one timeline needs. A host asking for
 * instants all over the capture at once - copies of it at different times - keeps them
 * all, or it would decode a chunk again on every frame.
 */
VGSB_API void vgsb_set_cache(vgsb_player *, int chunks, uint64_t max_bytes);

/*
 * Where players decode. By default each player's lanes split their work over threads of
 * their own, `threads` of them per lane, made for each batch and joined after it. That is
 * right for one capture in a host that leaves the cores alone; with several captures, or
 * in a host that keeps a worker on every core as game engines do, those threads fight
 * the host's for the same cores and every capture slows down.
 *
 * A host with a task system of its own hands it over here, and from then on players run
 * that work on it instead: every capture in the scene shares the host's workers, and the
 * host decides who gets the cores. `threads` in vgsb_open is then how many pieces a batch
 * is cut into, not how many threads run them.
 *
 * `run` must call `task(context, i)` once for every i from 0 to count - 1, on any threads
 * it likes, the calling one included, and return when all have finished. Tasks do not
 * throw and do not wait on each other, so a scheduler that runs them inline is fine. The
 * calling thread is one of the bridge's lanes, never a thread of the host's.
 *
 * Applies to players opened after the call; open ones keep what they had. NULL goes back
 * to the bridge's own threads. `run` and `host` must stay valid while any player opened
 * with them is open.
 */
typedef void (*vgsb_task)(void *context, size_t index);
typedef void (*vgsb_parallel_for)(void *host, size_t count, vgsb_task task, void *context);
VGSB_API void vgsb_set_parallel_for(vgsb_parallel_for run, void *host);

/*
 * ---- packed playback ------------------------------------------------------------------
 *
 * Evaluating a frame - turning a chunk's compressed attributes into the position,
 * rotation, scale, colour and harmonics of every splat at one instant - is most of what
 * playing costs on the CPU: about 22 ms of a 26 ms frame for a quarter of a million
 * splats, and the memory traffic of writing every splat out. A host that draws on a GPU
 * with compute can do that part in a shader instead, where it costs next to nothing, and
 * then the CPU only decompresses each chunk once, whatever the frame rate and however
 * many captures play.
 *
 * A player opened with vgsb_open_packed hands over chunks instead of frames: the arrays
 * the shader reads, built once per chunk on the lanes, and per frame only where the
 * instant falls in its chunk and the chunk's shared tables evaluated at it (the basis, a
 * few hundred kilobytes at most). How a shader reads them is the layout of the VGS
 * editor's packed viewport and the web viewer; the Unreal plugin's VGSPacked.usf is one.
 */
typedef struct vgsb_packed_chunk {
  /* Different for every chunk this library packs, in any player: a host that keeps the
   * chunk it uploaded last uploads again only when this changes. */
  uint64_t serial;
  uint64_t chunk_index;
  uint64_t splats;          /* every record of the chunk, alive or not at a given instant */
  uint64_t samples, intervals;
  int32_t sh_planes;        /* 0, 1, 3 or 5 words of three coefficients' indices */
  int32_t sh_coefficients;  /* 0, 3, 8 or 15 */
  int32_t group_count;      /* up to 8 */
  int32_t basis_texels;     /* RGBA texels in a frame's basis */
  int32_t basis_offset[4];  /* where each table starts in it: position, colour, opacity, rotation */
  uint64_t entries_static, entries_temporal, sh0_stride, opacity_stride;
  float group_a[32];        /* per group: end index, flags, position min, max */
  float group_b[32];        /* per group: first per-sample position and rotation word */

  const uint32_t *splat_a, *splat_b, *splat_c; /* 4 words per splat each */
  const uint32_t *sh_static, *sh_temporal;     /* sh_planes * splats words, plane-major */
  const uint32_t *position_terms;   /* pairs: dictionary entry, float weight bits */
  const uint32_t *rotation_terms;   /* dictionary entries */
  const uint32_t *position_samples; /* pairs: one packed 64-bit position per sample */
  const uint32_t *rotation_samples; /* one packed quaternion per sample */
  const float *books;               /* RGBA per codebook entry: static, then temporal */
  const float *luts;                /* 256 pairs: scale, base colour */
  uint64_t sh_words, position_term_words, rotation_term_words, position_sample_words,
      rotation_sample_words, book_floats, lut_floats;
  uint64_t bytes;                   /* all of the above together, what uploading it costs */
  /* One word per splat when the capture carries normals, else NULL: the format's two code
   * bytes in the low 16 bits and bit 16 set, or 0 for a splat that has none. The code is in
   * the splat's own axes ordered by its scale indices (splat_a's fourth word), as FORMAT.md
   * (Normals) and vgs::decodeNormal read it; turn the result by the splat's rotation at the
   * instant. Added after `bytes`, which counts it, so the fields before keep their places. */
  const uint32_t *normals;
  uint64_t normal_words;
} vgsb_packed_chunk;

typedef struct vgsb_packed_frame {
  double seconds;
  const vgsb_packed_chunk *chunk;
  uint32_t frame;   /* the sample the instant starts from */
  float alpha;      /* how far it is towards the next one, 0 to 1 */
  float normalized; /* the instant as a fraction of the chunk, for per-sample positions */
  int32_t reserved;
  const float *basis; /* chunk->basis_texels RGBA texels */
} vgsb_packed_frame;

/*
 * Opens a capture to play packed. The same lanes and threads as vgsb_open, and the same
 * vgsb_schedule; frames come from vgsb_acquire_packed, and vgsb_acquire is refused.
 * vgsb_set_density does nothing here: a shader thins splats itself.
 */
VGSB_API vgsb_player *vgsb_open_packed(const char *path_utf8, int include_sh, int threads);

/*
 * The instant at `seconds`, waiting up to `timeout_ms` (negative: as long as it takes)
 * for its chunk to be packed. The chunk and the basis stay valid until the next
 * vgsb_acquire_packed, vgsb_release or vgsb_close on the player; to keep the chunk longer,
 * as a renderer that uploads it later on another thread does, retain it.
 */
VGSB_API int vgsb_acquire_packed(vgsb_player *, double seconds, double timeout_ms,
                                 vgsb_packed_frame *out);

/* Keep a chunk past what vgsb_acquire_packed promises, and let it go. Any thread; a
 * retained chunk outlives the player that packed it. */
VGSB_API void vgsb_retain_chunk(const vgsb_packed_chunk *);
VGSB_API void vgsb_release_chunk(const vgsb_packed_chunk *);

#ifdef __cplusplus
}
#endif
#endif
