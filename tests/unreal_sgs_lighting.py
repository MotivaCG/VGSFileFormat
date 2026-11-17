"""GPU acceptance scene: default SGS must respond to light intensity and color.

Run in the isolated QA project using -ExecutePythonScript -RenderOffscreen (D3D12).
VGS_SGS_QA_DIR holds the signed fixtures; VGS_SGS_RENDER_DIR is the screenshot folder.
"""
import os
from pathlib import Path
import time
import unreal

root = Path(os.environ["VGS_SGS_QA_DIR"])
images = Path(os.environ["VGS_SGS_RENDER_DIR"])
images.mkdir(parents=True, exist_ok=True)
unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
options = unreal.SGSImportOptions()
options.set_editor_property("estimate_normals", False)
options.set_editor_property("make_shadow_proxy", False)
task = unreal.AssetImportTask()
task.filename = str(root / "fixtures/plain.sgs")
task.destination_path = "/Game/SGSLightingQA"
task.destination_name = "sphere"
task.automated = True
task.replace_existing = True
task.save = True
task.options = options
task.factory = unreal.SGSSplatFactory()
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
asset = unreal.load_asset("/Game/SGSLightingQA/sphere")
actor = actors.spawn_actor_from_object(asset, unreal.Vector())
component = actor.get_editor_property("splat_component")
assert component.get_editor_property("lighting") == unreal.SGSLighting.SHADED
component.set_editor_property("self_illumination_amount", 0.0)
component.set_editor_property("point_scale_factor", 2.0)
light_actor = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 150), unreal.Rotator(-20, 180, 0))
light = light_actor.get_component_by_class(unreal.DirectionalLightComponent)
light.set_mobility(unreal.ComponentMobility.MOVABLE)
light.set_intensity(0.0)
location = unreal.Vector(200, 0, 20)
rotation = unreal.MathLibrary.find_look_at_rotation(location, unreal.Vector())
camera = actors.spawn_actor_from_class(unreal.CameraActor, location, rotation)
camera.get_component_by_class(unreal.CameraComponent).set_field_of_view(50.0)
unreal.EditorLevelLibrary.pilot_level_actor(camera)
unreal.EditorLevelLibrary.editor_set_game_view(True)
for cmd in ("r.EyeAdaptationQuality 0", "r.DefaultFeature.AutoExposure 0", "r.AntiAliasingMethod 0",
            "r.DynamicGlobalIlluminationMethod 0", "r.ReflectionMethod 0", "ShowFlag.BillboardSprites 0",
            f"r.SGSSplat.Shadow.Method {os.environ.get('VGS_SGS_SHADOW_METHOD', '3')}"):
    unreal.SystemLibrary.execute_console_command(None, cmd)

# The optional mix run also captures scene-linear EXR, for a numeric HDR lerp check.
mix_test = os.environ.get("VGS_SGS_TEST_MIX") == "1"
if mix_test:
    unreal.SystemLibrary.execute_console_command(None, "ShowFlag.Tonemapper 0")
    cases = [("mix0_red", 0.0, (1, 0, 0), 8),
             ("mix05_red", 0.5, (1, 0, 0), 8),
             ("mix0999_red", 0.999, (1, 0, 0), 8),
             ("mix1_red", 1.0, (1, 0, 0), 8),
             ("mix1_blue", 1.0, (0, 0, 1), 8)]
else:
    cases = [("dark", 0.0, (1, 0, 0), 0),
             ("red", 0.0, (1, 0, 0), 8),
             ("blue", 0.0, (0, 0, 1), 8)]
index = 0
last_change = time.monotonic()
handle = None
jobs = []
configured = False
pending = False


def tick(delta):
    global index, last_change, configured, pending
    try:
        if pending:
            if time.monotonic() - last_change < 2 or not all(job.is_task_done() for job in jobs):
                return
            pending = False
            if index == len(cases):
                unreal.unregister_slate_post_tick_callback(handle)
                unreal.SystemLibrary.quit_editor()
                return
            configured = False
        if not configured:
            name, amount, color, intensity = cases[index]
            component.set_editor_property("self_illumination_amount", amount)
            light.set_light_color(unreal.LinearColor(*color, 1))
            light.set_intensity(intensity)
            configured = True
            last_change = time.monotonic()
        if time.monotonic() - last_change < 4 or not all(job.is_task_done() for job in jobs):
            return
        if index < len(cases):
            name = cases[index][0]
            jobs.append(unreal.AutomationLibrary.take_high_res_screenshot(960, 540, str(images / f"{name}.png")))
            if mix_test:
                jobs.append(unreal.AutomationLibrary.take_high_res_screenshot(960, 540, str(images / f"{name}.exr"), capture_hdr=True, delay=0.5))
            index += 1
            pending = True
            last_change = time.monotonic()
    except Exception:
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.SystemLibrary.quit_editor()
        raise


unreal.EditorPythonScripting.set_keep_python_script_alive(True)
handle = unreal.register_slate_post_tick_callback(tick)
