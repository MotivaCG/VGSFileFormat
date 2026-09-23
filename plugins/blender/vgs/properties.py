# SPDX-License-Identifier: GPL-3.0-or-later

"""Settings: one group per point cloud (PointCloud.vgs), and the add-on's preferences."""

import bpy
from bpy.props import (BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty,
                       StringProperty)
from bpy.types import AddonPreferences, PropertyGroup


# What happens past either end of the capture. Shared with the import operator.
LOOP_MODES = (
    ('CAPTURE', "From Capture", "As the capture's author set it: once, loop or ping-pong"),
    ('NONE', "No Loop", "Hold the last instant once the capture has played"),
    ('LOOP', "Loop", "Start over from the other end"),
    ('PING_PONG', "Ping-Pong", "Play back the other way, and so on"),
)


def _refresh(self, context):
    from . import playback
    playback.refresh(context)


def _get_start_frame(self):
    return int(round(self.phase * self.last_frame))


def _set_start_frame(self, value):
    # Only the phase is stored, so the two fields can never disagree.
    if self.last_frame > 0:
        self.phase = min(max(value / self.last_frame, 0.0), 1.0)


def _path_changed(self, context):
    from . import playback
    playback.reload(self.id_data)
    playback.refresh(context)


class VGSCaptureSettings(PropertyGroup):
    filepath: StringProperty(
        name="Capture",
        description="The .vgs or .pgs capture this point cloud plays",
        subtype='FILE_PATH',
        update=_path_changed,
    )
    use_sh: BoolProperty(
        name="Spherical Harmonics",
        description="Decode view-dependent colour. Off decodes faster and uses less memory",
        default=True,
        update=_refresh,
    )
    no_sh_while_playing: BoolProperty(
        name="No Harmonics While Playing",
        description=(
            "Play base colour only, which decodes and copies about three times faster. "
            "Paused, scrubbed or rendered frames always carry the harmonics"),
        default=True,
        update=_refresh,
    )
    viewport_density: FloatProperty(
        name="Viewport Density",
        description=(
            "How many splats the viewport draws, for a lighter scene: 1 draws all of them, "
            "0.01 one in a hundred, and the fraction falls faster than the slider as it goes "
            "down. Renders always draw every splat. The splats kept are raised in opacity so "
            "a thinned capture stays solid"),
        default=1.0,
        min=0.01,
        max=1.0,
        subtype='FACTOR',
        update=_refresh,
    )
    phase: FloatProperty(
        name="Phase",
        description=(
            "Where in the capture the scene's first frame falls: 0 its first instant, 1 its "
            "last. The same thing as Start Frame, as a fraction"),
        default=0.0,
        min=0.0,
        max=1.0,
        subtype='FACTOR',
        precision=3,
        update=_refresh,
    )
    start_frame: IntProperty(
        name="Start Frame",
        description=(
            "The capture's frame shown on the scene's first frame, counting from 0. The same "
            "thing as Phase, in frames"),
        min=0,
        get=_get_start_frame,
        # No update of its own: setting it sets Phase, whose update refreshes.
        set=_set_start_frame,
    )
    # The capture's last frame, for turning Phase into Start Frame. Kept here rather than
    # asked of the capture so the field works before the file is opened.
    last_frame: IntProperty(options={'HIDDEN'}, default=0)
    speed: FloatProperty(
        name="Speed",
        description=(
            "Capture seconds played per scene second. Negative plays it backwards from its "
            "phase"),
        default=1.0,
        soft_min=-4.0,
        soft_max=4.0,
        update=_refresh,
    )
    loop_mode: EnumProperty(
        name="Loop",
        description="What plays once the capture reaches its end",
        items=LOOP_MODES,
        default='CAPTURE',
        update=_refresh,
    )


class VGSPreferences(AddonPreferences):
    bl_idname = __package__

    slots: IntProperty(
        name="Frames Decoded Ahead",
        description=(
            "Frames each capture keeps decoded, the one on screen included. More smooths "
            "playback and costs memory: about 240 bytes per splat per frame with spherical "
            "harmonics, 56 without. Applies to captures opened after the change"),
        default=4,
        min=2,
        max=32,
    )
    threads: IntProperty(
        name="Decoder Threads",
        description=(
            "Cores one capture may use to decode a chunk. 0 uses half of them. With several "
            "captures playing, keep captures times this near the number of cores"),
        default=0,
        min=0,
        max=64,
    )
    playback_wait_ms: IntProperty(
        name="Playback Wait (ms)",
        description=(
            "During playback, how long to wait for a frame that is not decoded yet before "
            "showing the previous one. Scrubbing and rendering always wait"),
        default=250,
        min=0,
        max=5000,
    )
    strip_on_save: BoolProperty(
        name="Keep Splats Out of .blend Files",
        description=(
            "Save captures as a reference to their file rather than the decoded splats of "
            "the current frame, which are tens of megabytes and are decoded again on load"),
        default=True,
    )

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "slots")
        layout.prop(self, "threads")
        layout.prop(self, "playback_wait_ms")
        layout.prop(self, "strip_on_save")


_classes = (VGSCaptureSettings, VGSPreferences)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.PointCloud.vgs = PointerProperty(type=VGSCaptureSettings)


def unregister():
    del bpy.types.PointCloud.vgs
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
