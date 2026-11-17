"""Import the existing project logos unchanged as editor billboard textures.

Run in UnrealEditor-Cmd with -run=pythonscript -script=<this file>.
The PNG originals are copied from editor/assets/gracia; no artwork is generated.
"""
from pathlib import Path
import unreal

art = Path(__file__).resolve().parent.parent / "art"
for plugin, name in (("VGS", "S_VGSCapture"), ("SGS", "S_SGSCapture")):
    task = unreal.AssetImportTask()
    task.filename = str(art / (name + ".png"))
    task.destination_path = f"/{plugin}/Icons"
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.load_asset(f"/{plugin}/Icons/{name}")
    if not texture:
        raise RuntimeError(f"Failed to import {plugin} icon")
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("never_stream", True)
    unreal.EditorAssetLibrary.save_loaded_asset(texture, only_if_is_dirty=False)
