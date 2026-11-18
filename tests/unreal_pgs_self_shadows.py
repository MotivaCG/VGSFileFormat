"""Visual self-shadow regression on a real PGS asset in an isolated UE test project.
VGS_PGS_SELF_ASSET selects an imported asset; VGS_PGS_RENDER_DIR receives comparisons.
Checks the reported circular acne with identical light, exposure, camera and shadow bias.
"""
import os
from pathlib import Path
import time
import unreal

images = Path(os.environ["VGS_PGS_RENDER_DIR"])
images.mkdir(parents=True, exist_ok=True)
unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
asset = unreal.load_asset(os.environ.get('VGS_PGS_SELF_ASSET', '/Game/andreavictor_normals'))
actor = actors.spawn_actor_from_object(asset, unreal.Vector())
component = actor.get_editor_property("splat_component")
assert component.get_editor_property("lighting") == unreal.PGSLighting.SHADED
component.set_editor_property("self_illumination_amount", 0.0)
component.set_editor_property("point_scale_factor", 1.0)
light_actor = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 150), unreal.Rotator(-25, 0, 0))
light = light_actor.get_component_by_class(unreal.DirectionalLightComponent)
light.set_mobility(unreal.ComponentMobility.MOVABLE)
light.set_intensity(0.0)
origin, extent = actor.get_actor_bounds(False)
location = origin + unreal.Vector(-260, 0, 30)
rotation = unreal.MathLibrary.find_look_at_rotation(location, origin + unreal.Vector(0, 0, 35))
camera = actors.spawn_actor_from_class(unreal.CameraActor, location, rotation)
camera.get_component_by_class(unreal.CameraComponent).set_field_of_view(50.0)
unreal.EditorLevelLibrary.pilot_level_actor(camera)
unreal.EditorLevelLibrary.editor_set_game_view(True)
for cmd in ("r.EyeAdaptationQuality 0", "r.DefaultFeature.AutoExposure 0", "r.AntiAliasingMethod 0",
            "r.DynamicGlobalIlluminationMethod 0", "r.ReflectionMethod 0", "ShowFlag.BillboardSprites 0",
            f"r.PGSSplat.Shadow.Method {os.environ.get('VGS_PGS_SHADOW_METHOD', '3')}"):
    unreal.SystemLibrary.execute_console_command(None, cmd)

cases = [("self_off", 0, 3), ("self_on", 1, 3), ("self_bias8", 1, 8), ("side_off", 0, 3), ("side_on", 1, 3)]
index = 0
phase = 0
last = time.monotonic()
job = None
handle = None
def tick(delta):
    global index, phase, last, job
    if time.monotonic()-last < 4: return
    if phase == 0:
        name, enabled, bias = cases[index]
        light_actor.set_actor_rotation(unreal.Rotator(-25, 65 if name.startswith("side") else 0, 0), False)
        light.set_light_color(unreal.LinearColor(1,1,1,1))
        light.set_intensity(5)
        unreal.SystemLibrary.execute_console_command(None, 'r.PGSSplat.Shadow.Method 0')
        unreal.SystemLibrary.execute_console_command(None, f'r.PGSSplat.Shadow.Self {enabled}')
        unreal.SystemLibrary.execute_console_command(None, f'r.PGSSplat.Shadow.Bias {bias}')
        phase=1;last=time.monotonic()
    elif phase == 1:
        job=unreal.AutomationLibrary.take_high_res_screenshot(960,540,str(images/(cases[index][0]+'.png')))
        phase=2;last=time.monotonic()
    elif job.is_task_done():
        index+=1
        if index==len(cases):
            unreal.unregister_slate_post_tick_callback(handle)
            unreal.SystemLibrary.quit_editor()
        else:phase=0
unreal.EditorPythonScripting.set_keep_python_script_alive(True)
handle=unreal.register_slate_post_tick_callback(tick)
