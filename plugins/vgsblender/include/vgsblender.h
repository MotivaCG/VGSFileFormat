#ifndef VGSBLENDER_H
#define VGSBLENDER_H

/*
 * Playing VFGS captures inside a host that pulls frames: Blender, through its add-on.
 *
 * The decoder hands over one instant at a time and belongs to one thread. A host like
 * Blender asks for a frame on its main thread when the playhead moves and wants it at
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
 * What comes out is already shaped for Blender's Gaussian splat point clouds, so the
 * add-on only copies arrays into attributes:
 *
 *   - records that are not alive at the instant are left out, and `count` is what is
 *     left: Blender quantises attributes against their range over every point, and a dead
 *     record with a zero scale would stretch that range until nothing else resolved;
 *   - rotations are wxyz, the order of Blender's quaternion attribute;
 *   - `radiance` is the spherical harmonic DC term and the activated opacity, which is
 *     exactly the "radiance:base" attribute;
 *   - `sh` is one plane of 3 * count floats per coefficient, so plane k is the
 *     "radiance:sh_k" attribute as it stands.
 *
 * Scales and opacities are activated, as Blender stores them.
 *
 * Several players run side by side: one per capture in the scene, each with its own
 * lanes. Every function is safe to call from any one thread at a time per player; the
 * add-on calls them all from Blender's main thread, or from the render thread while
 * rendering.
 *
 * The Houdini plugin compiles this same player into its SOP, with VGSBLENDER_STATIC
 * defined, and reshapes each frame into Houdini's GSplat attributes as it copies it.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(VGSBLENDER_STATIC)
/* Compiled into another module, as the Houdini plugin does: nothing to export. */
#  define VGSB_API
#elif defined(_WIN32)
#  ifdef VGSBLENDER_BUILD
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
#define VGSB_API_VERSION 3

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

#ifdef __cplusplus
}
#endif
#endif
