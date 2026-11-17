"""Run in a full UE editor with -ExecutePythonScript, not the Python commandlet.

VGS_SGS_QA_DIR points to the existing signed SGS fixtures and reports.
Checks default lighting/caster, billboard geometry, and global No Shadows on mesh proxies
with actor ticking disabled. Temporary QA project only; does not save project settings.
"""
import json
import os
import time
from pathlib import Path
import unreal

root = Path(os.environ["VGS_SGS_QA_DIR"])
tools = unreal.AssetToolsHelpers.get_asset_tools()
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
options = unreal.SGSImportOptions()
options.set_editor_property("estimate_normals", False)
options.set_editor_property("make_shadow_proxy", True)
task = unreal.AssetImportTask()
task.filename = str(root / "fixtures/mesh.sgs")
task.destination_path = "/Game/SGSControlsQA"
task.destination_name = "mesh"
task.automated = True
task.replace_existing = True
task.save = True
task.options = options
task.factory = unreal.SGSSplatFactory()
tools.import_asset_tasks([task])
asset = unreal.load_asset("/Game/SGSControlsQA/mesh")
assert asset and asset.get_editor_property("shadow_proxy")
actor = actors.spawn_actor_from_object(asset, unreal.Vector())
component = actor.get_editor_property("splat_component")
assert component.get_editor_property("lighting") == unreal.SGSLighting.SHADED
sprite = actor.get_component_by_class(unreal.BillboardComponent)
assert abs(sprite.get_relative_transform().scale3d.x - 0.125) < 1e-5
assert sprite.get_relative_transform().translation.z == 40.0
assert not sprite.get_editor_property("use_in_editor_scaling")
component.set_editor_property("proxy_collision", True)
component.set_component_tick_enabled(False)


def proxy():
    result = [c for c in actor.get_components_by_class(unreal.StaticMeshComponent)
              if c.get_editor_property("static_mesh") == asset.get_editor_property("shadow_proxy")]
    assert len(result) == 1, result
    return result[0]


steps = [(2, False), (3, False), (0, False), (1, False), (2, False), (3, False)]
settings = unreal.get_default_object(unreal.load_class(None, "/Script/SGSSplatRenderer.SGSSplatSettings"))
method_type = type(settings.get_editor_property("shadow_method"))
methods = {0: method_type.SPLAT_SHADOW_MAP,
           1: method_type.MIXED,
           2: method_type.UNREAL_SHADOW_MAPS,
           3: method_type.NONE}
index = 0
last = time.monotonic()
handle = None
results = []


def tick(delta):
    global index, last
    if time.monotonic() - last < 0.25:
        return
    try:
        if index > 0:
            method, expected = steps[index - 1]
            actual = proxy().get_editor_property("cast_shadow")
            assert actual == expected, (method, actual, expected)
            assert proxy().get_collision_profile_name() == "BlockAll"
            assert not proxy().is_visible()
            assert not proxy().get_editor_property("visible_in_ray_tracing")
            results.append({"method": method, "proxy_casts": actual, "collision": True})
        if index == len(steps):
            unreal.unregister_slate_post_tick_callback(handle)
            (root / "controls.json").write_text(json.dumps({"default_shaded": True, "small_raised_icon": True, "project_caster": results}, indent=2))
            unreal.SystemLibrary.quit_editor()
            return
        settings.set_editor_property("shadow_method", methods[steps[index][0]])
        index += 1
        last = time.monotonic()
    except Exception:
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.SystemLibrary.quit_editor()
        raise


handle = unreal.register_slate_post_tick_callback(tick)
# Keep the editor running while the asynchronous assertions exercise console sinks.
unreal.EditorPythonScripting.set_keep_python_script_alive(True)
