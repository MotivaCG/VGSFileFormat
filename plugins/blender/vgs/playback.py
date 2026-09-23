# SPDX-License-Identifier: GPL-3.0-or-later

"""Which instant each capture shows, and writing it into its point cloud.

Every point cloud with a capture path gets a native player, keyed by the point cloud's
session id, so linked duplicates share one. On a frame change the add-on first tells
every player which instants are coming - so all of them decode in parallel, each on its
own thread - and only then waits for each one's current frame and copies it in.
"""

import math
from contextlib import contextmanager

import bpy
from bpy.app.handlers import persistent

from . import native

# How far ahead of the playhead chunks are decompressed, in seconds. A chunk covers about
# a second and takes a fraction of one to decode, so this is enough to have the next one
# ready before playback arrives.
_LOOKAHEAD_SECONDS = 1.5

# Waits that are not playback: scrubbing and rendering want the exact frame. A bound
# rather than forever, so a fault in the library cannot hang Blender.
_EXACT_WAIT_MS = 60000.0


class _Entry:
    __slots__ = ("player", "path")

    def __init__(self, player, path):
        self.player = player
        self.path = path


_entries = {}   # PointCloud.session_uid -> _Entry
_errors = {}    # PointCloud.session_uid -> (path, message), shown in the panel
_suspended = 0

# Set from render_init to render_complete or render_cancel. bpy.app.is_job_running says
# the same for a render started from the interface, but not for one run from the command
# line - `blender -b file.blend -a`, how farms render - and the render pipeline changes
# the frame itself, which would otherwise decode it for the viewport.
_rendering = False


def preferences():
    addon = bpy.context.preferences.addons.get(__package__)
    return addon.preferences if addon else None


@contextmanager
def suspended():
    """No refreshes while settings are being filled in; refresh once afterwards."""
    global _suspended
    _suspended += 1
    try:
        yield
    finally:
        _suspended -= 1


def capture_path(pointcloud):
    return bpy.path.abspath(pointcloud.vgs.filepath, library=pointcloud.library)


def status(pointcloud):
    """(info, error) for the panel: the open capture's info, or why it is not open."""
    uid = pointcloud.session_uid
    entry = _entries.get(uid)
    failure = _errors.get(uid)
    return (entry.player.info if entry else None, failure[1] if failure else None)


def close(uid):
    entry = _entries.pop(uid, None)
    if entry is not None:
        entry.player.close()


def close_all():
    for uid in list(_entries):
        close(uid)
    _errors.clear()


def reload(pointcloud):
    uid = pointcloud.session_uid
    close(uid)
    _errors.pop(uid, None)


def _player_for(pointcloud, include_sh, density):
    uid = pointcloud.session_uid
    path = capture_path(pointcloud)
    entry = _entries.get(uid)
    if entry is not None and entry.path != path:
        close(uid)
        entry = None

    if entry is None:
        # A capture that failed to open is not retried every frame, only once its path
        # changes or somebody asks for a reload.
        failure = _errors.get(uid)
        if failure is not None and failure[0] == path:
            return None
        prefs = preferences()
        try:
            player = native.Player(path, include_sh,
                                   prefs.slots if prefs else 4,
                                   prefs.threads if prefs else 0)
        except (native.NativeError, OSError) as error:
            _errors[uid] = (path, str(error))
            return None
        _errors.pop(uid, None)
        entry = _Entry(player, path)
        _entries[uid] = entry

    entry.player.set_include_sh(include_sh)
    entry.player.set_density(density)
    last = last_frame(entry.player.info)
    if pointcloud.vgs.last_frame != last and not pointcloud.library:
        pointcloud.vgs.last_frame = last
    return entry.player


def last_frame(info):
    """The index of a capture's last frame: 0 is the first."""
    return int(round(info.duration * info.frame_rate)) if info.frame_rate > 0 else 0


# The viewport density slider runs from 0.01 to 1 and the fraction of splats drawn from
# 0.01 to 1 too, but not in step: the slider is mapped onto [cbrt(0.01), 1] and cubed.
# Thinning only pays off from about a quarter of the splats down, and a linear slider put
# all of that in its last stretch; cubed, the lower half of the slider covers it.
_SLIDER_LOW = 0.01
_CUBE_LOW = _SLIDER_LOW ** (1.0 / 3.0)


def splat_fraction(slider):
    t = (slider - _SLIDER_LOW) / (1.0 - _SLIDER_LOW)
    return (_CUBE_LOW + max(0.0, min(1.0, t)) * (1.0 - _CUBE_LOW)) ** 3


def scene_fps(scene):
    return scene.render.fps / scene.render.fps_base


# The capture's own playback mode, 0 once, 1 loop, 2 ping-pong, as the loop modes here.
_CAPTURE_MODES = ('NONE', 'LOOP', 'PING_PONG')


def loop_mode(settings, info):
    """The loop mode in effect: the one chosen, or the capture's own for 'From Capture'."""
    if settings.loop_mode != 'CAPTURE':
        return settings.loop_mode
    mode = info.playback_mode
    return _CAPTURE_MODES[mode] if 0 <= mode < len(_CAPTURE_MODES) else 'LOOP'


def capture_seconds(pointcloud, info, scene, frame):
    """The capture's time at a scene frame, after phase, speed and looping.

    The scene's first frame shows the capture at its phase, and from there it runs at its
    speed, backwards when that is negative.
    """
    settings = pointcloud.vgs
    duration = info.duration
    elapsed = (frame - scene.frame_start) / scene_fps(scene) * settings.speed
    seconds = settings.phase * duration + elapsed
    mode = loop_mode(settings, info)
    if duration > 0:
        if mode == 'LOOP':
            # One frame interval past the last instant, so the loop does not show the last
            # and the first frame back to back as if they were one. Python's modulo is
            # never negative, which is what makes this work backwards too.
            period = duration + (1.0 / info.frame_rate if info.frame_rate > 0 else 0.0)
            seconds %= period
        elif mode == 'PING_PONG':
            # There and back in twice the duration: each end is shown once per turn.
            seconds %= 2.0 * duration
            if seconds > duration:
                seconds = 2.0 * duration - seconds
    return min(max(seconds, 0.0), duration)


def _playing():
    wm = bpy.context.window_manager
    if wm is None:
        return False
    return any(window.screen and window.screen.is_animation_playing for window in wm.windows)


def _upcoming_frames(scene, count, playing, rendering):
    """The scene frames that will be asked for next, soonest first."""
    frame = scene.frame_current
    if not playing and not rendering:
        # Scrubbing: whichever way the user steps next.
        return [frame + 1, frame - 1]
    if scene.use_preview_range:
        first, last = scene.frame_preview_start, scene.frame_preview_end
    else:
        first, last = scene.frame_start, scene.frame_end
    step = max(1, scene.frame_step) if rendering else 1
    frames = []
    for _ in range(count):
        frame += step
        if frame > last:
            if rendering:
                break
            frame = first
        frames.append(frame)
    return frames


def _pointclouds_in(scene):
    found = {}
    for obj in scene.objects:
        if obj.type != 'POINTCLOUD':
            continue
        pointcloud = obj.data
        if pointcloud is not None and pointcloud.vgs.filepath:
            found[pointcloud.session_uid] = pointcloud
    return list(found.values())


def _ensure_attribute(attributes, name, data_type):
    attribute = attributes.get(name)
    if attribute is not None and (attribute.data_type != data_type or attribute.domain != 'POINT'):
        attributes.remove(attribute)
        attribute = None
    if attribute is None:
        attribute = attributes.new(name, data_type, 'POINT')
    return attribute


def write_frame(pointcloud, frame):
    """Copies a decoded frame into the point cloud's Gaussian splat attributes."""
    n = frame.count
    if len(pointcloud.points) != n:
        pointcloud.resize(n)
    attributes = pointcloud.attributes

    # Look every attribute up before writing any: adding one can reallocate the others.
    coefficients = len(frame.sh)
    _ensure_attribute(attributes, "rotation", 'QUATERNION')
    _ensure_attribute(attributes, "scale", 'FLOAT_VECTOR')
    _ensure_attribute(attributes, "radiance:base", 'FLOAT4')
    for k in range(coefficients):
        _ensure_attribute(attributes, "radiance:sh_{:d}".format(k), 'FLOAT_VECTOR')
    # Blender reads sh_0, sh_1... until one is missing, so leftovers from a frame decoded
    # with more harmonics have to go.
    k = coefficients
    while (stale := attributes.get("radiance:sh_{:d}".format(k))) is not None:
        attributes.remove(stale)
        k += 1

    if n:
        attributes["position"].data.foreach_set("vector", frame.positions)
        attributes["rotation"].data.foreach_set("value", frame.rotations)
        attributes["scale"].data.foreach_set("vector", frame.scales)
        attributes["radiance:base"].data.foreach_set("vector", frame.radiance)
        for k, plane in enumerate(frame.sh):
            attributes["radiance:sh_{:d}".format(k)].data.foreach_set("vector", plane)

    pointcloud.update_tag()


def update_scene(scene, playing=None, rendering=None):
    """Shows every capture in `scene` at the scene's current frame.

    `playing` overrides asking the screens, for the moment playback stops: the screens may
    not say so yet, and that frame is the one that should get its harmonics back.
    `rendering` overrides asking whether a render job runs, for the render handlers, which
    run at its edges.
    """
    if scene is None:
        return
    if rendering is None:
        rendering = _rendering or bpy.app.is_job_running('RENDER')
    if rendering:
        playing = False
    elif playing is None:
        playing = _playing()
    prefs = preferences()
    frame = scene.frame_current + scene.frame_subframe
    lookahead = max(prefs.slots if prefs else 4,
                    int(math.ceil(_LOOKAHEAD_SECONDS * scene_fps(scene))))
    upcoming = _upcoming_frames(scene, lookahead, playing, rendering)

    # Schedule everything first, so every capture decodes at once...
    jobs = []
    for pointcloud in _pointclouds_in(scene):
        # Harmonics are most of what a frame costs to decode and to copy, and view-dependent
        # colour is hard to judge on a moving picture; paused, scrubbed or rendered, they
        # come back.
        settings = pointcloud.vgs
        include_sh = settings.use_sh and not (playing and settings.no_sh_while_playing)
        # Renders draw every splat whatever the viewport is set to.
        density = 1.0 if rendering else splat_fraction(settings.viewport_density)
        player = _player_for(pointcloud, include_sh, density)
        if player is None:
            continue
        info = player.info
        now = capture_seconds(pointcloud, info, scene, frame)
        player.schedule([now] + [capture_seconds(pointcloud, info, scene, f) for f in upcoming])
        jobs.append((pointcloud, player, now))

    # ...then collect them one by one.
    wait = float(prefs.playback_wait_ms if prefs else 250) if playing else _EXACT_WAIT_MS
    for pointcloud, player, now in jobs:
        uid = pointcloud.session_uid
        try:
            decoded = player.acquire(now, wait)
        except native.NativeError as error:
            _errors[uid] = (capture_path(pointcloud), str(error))
            close(uid)
            continue
        if decoded is None:
            # Not ready in time during playback: keep showing the frame before.
            continue
        try:
            write_frame(pointcloud, decoded)
        except (RuntimeError, TypeError, ValueError) as error:
            _errors[uid] = (capture_path(pointcloud), str(error))
        finally:
            player.release()

    _close_unused()


def _close_unused():
    alive = {pc.session_uid for pc in bpy.data.pointclouds if pc.users and pc.vgs.filepath}
    for uid in list(_entries):
        if uid not in alive:
            close(uid)


def refresh(context=None):
    if _suspended:
        return
    context = context or bpy.context
    update_scene(context.scene)


# ---- handlers ------------------------------------------------------------------------


@persistent
def _on_frame_change(scene, depsgraph=None):
    update_scene(scene)


@persistent
def _on_playback_stop(scene, depsgraph=None):
    update_scene(scene, playing=False)


# Renders draw every splat and every harmonic whatever the viewport is set to. The
# viewport shares the same data, so it gets its own settings back once the whole job is
# over, not after every frame of an animation.
@persistent
def _on_render_init(scene, depsgraph=None):
    global _rendering
    _rendering = True


@persistent
def _on_render_pre(scene, depsgraph=None):
    # A still render may not change the frame, so this is what brings the full splats in.
    update_scene(scene, rendering=True)


@persistent
def _on_render_end(scene, depsgraph=None):
    global _rendering
    _rendering = False
    update_scene(scene, rendering=False)


@persistent
def _on_load_pre(*_args):
    close_all()


@persistent
def _on_load_post(*_args):
    update_scene(bpy.context.scene)


@persistent
def _on_undo(*_args):
    # Undo brings back the point cloud as it was stored, which may be another instant, or
    # empty when splats are kept out of the file.
    update_scene(bpy.context.scene)


@persistent
def _on_save_pre(*_args):
    prefs = preferences()
    if prefs is not None and not prefs.strip_on_save:
        return
    for pointcloud in bpy.data.pointclouds:
        if pointcloud.vgs.filepath and not pointcloud.library and len(pointcloud.points):
            pointcloud.resize(0)


@persistent
def _on_save_post(*_args):
    prefs = preferences()
    if prefs is None or prefs.strip_on_save:
        update_scene(bpy.context.scene)


_handlers = (
    (bpy.app.handlers.frame_change_pre, _on_frame_change),
    (bpy.app.handlers.animation_playback_post, _on_playback_stop),
    (bpy.app.handlers.render_init, _on_render_init),
    (bpy.app.handlers.render_pre, _on_render_pre),
    (bpy.app.handlers.render_complete, _on_render_end),
    (bpy.app.handlers.render_cancel, _on_render_end),
    (bpy.app.handlers.load_pre, _on_load_pre),
    (bpy.app.handlers.load_post, _on_load_post),
    (bpy.app.handlers.undo_post, _on_undo),
    (bpy.app.handlers.redo_post, _on_undo),
    (bpy.app.handlers.save_pre, _on_save_pre),
    (bpy.app.handlers.save_post, _on_save_post),
)


def register():
    for handlers, function in _handlers:
        if function not in handlers:
            handlers.append(function)


def unregister():
    for handlers, function in _handlers:
        if function in handlers:
            handlers.remove(function)
    close_all()
