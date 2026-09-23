# SPDX-License-Identifier: GPL-3.0-or-later

"""The capture panel in the point cloud's data properties, and File > Import."""

import os

import bpy
import bpy.utils.previews
from bpy.types import Panel

from . import playback
from .operators import VGS_OT_fit_scene, VGS_OT_import, VGS_OT_reload


# The logo, loaded once as a preview icon: the only way a panel can show an image.
_previews = None

# How large it is drawn, in multiples of an icon's height: about five lines of the panel.
_LOGO_SCALE = 5.0


def _logo_icon():
    return _previews["logo"].icon_id if _previews is not None else 0


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

        icon = _logo_icon()
        if icon:
            row = layout.row()
            row.alignment = 'CENTER'
            row.template_icon(icon_value=icon, scale=_LOGO_SCALE)

        layout.prop(settings, "filepath")
        if not settings.filepath:
            return

        col = layout.column()
        sub = col.column(align=True)
        sub.prop(settings, "phase")
        sub.prop(settings, "start_frame")
        col.prop(settings, "speed")
        col.prop(settings, "loop_mode")
        col.prop(settings, "viewport_density")
        # A heading on the left and short checkbox labels, as Blender's own panels do: long
        # labels in a split layout are cut off in a narrow editor.
        col = layout.column(heading="Harmonics")
        col.prop(settings, "use_sh", text="Enabled")
        sub = col.column()
        sub.active = settings.use_sh
        sub.prop(settings, "no_sh_while_playing", text="Off While Playing")

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
            modes = ("Once", "Loop", "Ping-Pong")
            if 0 <= info.playback_mode < len(modes):
                col.label(text="Plays: {}".format(modes[info.playback_mode]))


def _menu_import(self, context):
    self.layout.operator(VGS_OT_import.bl_idname, text="VGS (.vgs, .pgs)")


def register():
    global _previews
    _previews = bpy.utils.previews.new()
    _previews.load("logo", os.path.join(os.path.dirname(__file__), "logo.png"), 'IMAGE')
    bpy.utils.register_class(DATA_PT_vgs_capture)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)


def unregister():
    global _previews
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    bpy.utils.unregister_class(DATA_PT_vgs_capture)
    if _previews is not None:
        bpy.utils.previews.remove(_previews)
        _previews = None
