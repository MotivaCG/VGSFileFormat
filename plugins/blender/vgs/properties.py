# SPDX-License-Identifier: GPL-3.0-or-later

"""Settings: one group per point cloud (PointCloud.vgs), and the add-on's preferences."""

import bpy
from bpy.props import (BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty,
                       StringProperty)
from bpy.types import AddonPreferences, PropertyGroup


# What happens past either end of the capture. Shared with the import operator.
LOOP_MODES = (
    ('NONE', "No Loop", "Hold the last instant once the capture has played"),
    ('LOOP', "Loop", "Start over from the other end"),
    ('PING_PONG', "Ping-Pong", "Play back the other way, and so on"),
)


def _refresh(self, context):
    from . import playback
    playback.refresh(context)


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
    frame_start: IntProperty(
        name="Start Frame",
        description="Scene frame at which the capture's first instant is shown",
        default=1,
        update=_refresh,
    )
    speed: FloatProperty(
        name="Speed",
        description=(
            "Capture seconds played per scene second. Negative plays it backwards, starting "
            "from its last instant"),
        default=1.0,
        soft_min=-4.0,
        soft_max=4.0,
        update=_refresh,
    )
    loop_mode: EnumProperty(
        name="Loop",
        description="What plays once the capture reaches its end",
        items=LOOP_MODES,
        default='LOOP',
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
