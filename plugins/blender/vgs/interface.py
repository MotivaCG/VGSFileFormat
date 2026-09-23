# SPDX-License-Identifier: GPL-3.0-or-later

"""The capture panel in the point cloud's data properties, and File > Import."""

import bpy
from bpy.types import Panel

from . import playback
from .operators import VGS_OT_fit_scene, VGS_OT_import, VGS_OT_reload


class DATA_PT_vgs_capture(Panel):
    bl_label = "VGS"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        return getattr(context, "pointcloud", None) is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        pointcloud = context.pointcloud
        settings = pointcloud.vgs

        layout.prop(settings, "filepath")
        if not settings.filepath:
            return

        col = layout.column()
        col.prop(settings, "frame_start")
        col.prop(settings, "speed")
        col.prop(settings, "loop_mode")
        col.prop(settings, "use_sh")
        sub = col.column()
        sub.active = settings.use_sh
        sub.prop(settings, "no_sh_while_playing")

        row = layout.row(align=True)
        row.operator(VGS_OT_fit_scene.bl_idname, icon='TIME')
        row.operator(VGS_OT_reload.bl_idname, text="", icon='FILE_REFRESH')

        info, error = playback.status(pointcloud)
        if error:
            box = layout.box()
            box.label(text=error, icon='ERROR')
        if info is None:
            return

        header, body = layout.panel("vgs_capture_info", default_closed=True)
        header.label(text="Capture")
        if body:
            col = body.column(align=True)
            for label, value in (
                ("Title", info.title),
                ("Author", info.author),
                ("Project", info.project),
                ("Take", info.take),
                ("Studio", info.studio),
                ("Copyright", info.copyright),
            ):
                if value:
                    col.label(text="{}: {}".format(label, value))
            col.separator()
            col.label(text="Duration: {:.2f} s, {:d} frames at {:.3g} fps".format(
                info.duration, info.frame_count, info.frame_rate))
            col.label(text="Splats: up to {:,}".format(info.max_splats))
            col.label(text="Spherical harmonics: degree {:d}".format(info.sh_degree))


def _menu_import(self, context):
    self.layout.operator(VGS_OT_import.bl_idname, text="VGS (.vgs, .pgs)")


def register():
    bpy.utils.register_class(DATA_PT_vgs_capture)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    bpy.utils.unregister_class(DATA_PT_vgs_capture)
