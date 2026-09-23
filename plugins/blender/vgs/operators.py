# SPDX-License-Identifier: GPL-3.0-or-later

"""Import a capture, reload one, and fit the scene's timeline to one."""

import math
import os

import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, StringProperty
from bpy.types import FileHandler, Operator, OperatorFileListElement
from bpy_extras.io_utils import ImportHelper

from . import native, playback
from .properties import LOOP_MODES

# Rotations about X that bring a capture's up axis to Blender's Z.
_UP_AXIS_ROTATION = {
    'Z': 0.0,
    'Y': math.pi / 2,
    'NEG_Y': -math.pi / 2,
}


def fit_scene(scene, pointcloud, info, set_fps):
    """Frame range, and optionally frame rate, to play `info` once from the scene's start."""
    settings = pointcloud.vgs
    if set_fps and info.frame_rate > 0:
        fps = max(1, round(info.frame_rate))
        scene.render.fps = fps
        scene.render.fps_base = fps / info.frame_rate
    speed = abs(settings.speed) or 1.0
    frames = int(round(info.duration * playback.scene_fps(scene) / speed))
    scene.frame_end = scene.frame_start + max(frames, 0)


class VGS_OT_import(Operator, ImportHelper):
    """Import 4D Gaussian splat captures as point clouds that play with the timeline"""
    bl_idname = "import_scene.vgs"
    bl_label = "Import VGS Capture"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".vgs"
    filter_glob: StringProperty(default="*.vgs;*.pgs", options={'HIDDEN'})
    files: CollectionProperty(type=OperatorFileListElement, options={'HIDDEN', 'SKIP_SAVE'})
    directory: StringProperty(subtype='DIR_PATH', options={'HIDDEN', 'SKIP_SAVE'})

    use_sh: BoolProperty(
        name="Spherical Harmonics",
        description="Decode view-dependent colour. Off decodes faster and uses less memory",
        default=True,
    )
    loop_mode: EnumProperty(
        name="Loop",
        description="What plays once the capture reaches its end",
        items=LOOP_MODES,
        default='CAPTURE',
    )
    up_axis: EnumProperty(
        name="Up Axis",
        description="Which axis of the capture points up",
        items=(
            ('Y', "Y Up", "Rotate +90° about X: how captures are written"),
            ('Z', "Z Up", "Use the capture's coordinates as they are"),
            ('NEG_Y', "Y Down", "Rotate -90° about X, for OpenCV / COLMAP conventions"),
        ),
        default='Y',
    )
    fit_scene: BoolProperty(
        name="Fit Scene",
        description="Set the scene's frame range and frame rate to the first capture",
        default=True,
    )

    def invoke(self, context, event):
        return self.invoke_popup(context)

    def _paths(self):
        if self.directory and self.files:
            return [os.path.join(self.directory, f.name) for f in self.files if f.name]
        return [self.filepath]

    def execute(self, context):
        created = []
        first = None
        with playback.suspended():
            for path in self._paths():
                try:
                    probe = native.Player(path, False, 2, 1)
                except (native.NativeError, OSError) as error:
                    self.report({'ERROR'}, "{}: {}".format(os.path.basename(path), error))
                    continue
                info = probe.info
                probe.close()

                name = info.title or os.path.splitext(os.path.basename(path))[0]
                pointcloud = bpy.data.pointclouds.new(name)
                pointcloud.type = 'GAUSSIAN_SPLAT'
                settings = pointcloud.vgs
                settings.filepath = path
                settings.use_sh = self.use_sh
                settings.last_frame = playback.last_frame(info)
                settings.loop_mode = self.loop_mode

                obj = bpy.data.objects.new(name, pointcloud)
                obj.rotation_euler.x = _UP_AXIS_ROTATION[self.up_axis]
                context.collection.objects.link(obj)
                created.append(obj)
                if first is None:
                    first = (pointcloud, info)

            if first is not None and self.fit_scene:
                fit_scene(context.scene, first[0], first[1], set_fps=True)

        if not created:
            return {'CANCELLED'}

        for obj in context.view_layer.objects.selected:
            obj.select_set(False)
        for obj in created:
            obj.select_set(True)
        context.view_layer.objects.active = created[-1]

        playback.refresh(context)
        return {'FINISHED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop(self, "use_sh")
        layout.prop(self, "loop_mode")
        layout.prop(self, "up_axis")
        layout.prop(self, "fit_scene")


class VGS_FH_import(FileHandler):
    bl_idname = "VGS_FH_import"
    bl_label = "VGS Capture"
    bl_import_operator = VGS_OT_import.bl_idname
    bl_file_extensions = ".vgs;.pgs"

    @classmethod
    def poll_drop(cls, context):
        return context.area is not None and context.area.type == 'VIEW_3D'


def _active_capture(context):
    pointcloud = getattr(context, "pointcloud", None)
    if pointcloud is None and context.object is not None and context.object.type == 'POINTCLOUD':
        pointcloud = context.object.data
    if pointcloud is not None and pointcloud.vgs.filepath:
        return pointcloud
    return None


class VGS_OT_reload(Operator):
    """Close the capture and open it again from disk"""
    bl_idname = "vgs.reload"
    bl_label = "Reload Capture"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return _active_capture(context) is not None

    def execute(self, context):
        playback.reload(_active_capture(context))
        playback.refresh(context)
        return {'FINISHED'}


class VGS_OT_fit_scene(Operator):
    """Set the scene's frame range to play this capture once from its start frame"""
    bl_idname = "vgs.fit_scene"
    bl_label = "Fit Scene to Capture"
    bl_options = {'REGISTER', 'UNDO'}

    set_fps: BoolProperty(
        name="Frame Rate",
        description="Also set the scene's frame rate to the capture's",
        default=True,
    )

    @classmethod
    def poll(cls, context):
        pointcloud = _active_capture(context)
        return pointcloud is not None and playback.status(pointcloud)[0] is not None

    def execute(self, context):
        pointcloud = _active_capture(context)
        info = playback.status(pointcloud)[0]
        fit_scene(context.scene, pointcloud, info, self.set_fps)
        playback.refresh(context)
        return {'FINISHED'}


_classes = (VGS_OT_import, VGS_FH_import, VGS_OT_reload, VGS_OT_fit_scene)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
