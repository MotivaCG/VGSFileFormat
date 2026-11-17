"""UE commandlet acceptance of SGS assets. Uses an isolated content-only QA project.

VGS_SGS_QA_DIR names the fixture/report folder; VGS_SGS_QA_PHASE is import/reload/negative.
Fixtures are made by sgseditor's sgs_unreal_fixture with the real signed exporter.
"""
import json
import os
from pathlib import Path
import unreal

root = Path(os.environ["VGS_SGS_QA_DIR"])
phase = os.environ.get("VGS_SGS_QA_PHASE", "import")
folder = "/Game/SGSImportQA"
tools = unreal.AssetToolsHelpers.get_asset_tools()


def import_asset(name, proxy):
    options = unreal.SGSImportOptions()
    options.set_editor_property("estimate_normals", False)
    options.set_editor_property("make_shadow_proxy", proxy)
    task = unreal.AssetImportTask()
    task.filename = str(root / "fixtures" / f"{name}.sgs")
    task.destination_path = folder
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    task.options = options
    task.factory = unreal.SGSSplatFactory()
    tools.import_asset_tasks([task])
    return unreal.load_asset(f"{folder}/{name}") if task.imported_object_paths else None


def inspect(name):
    asset = unreal.load_asset(f"{folder}/{name}")
    assert asset and asset.get_class().get_name() == "SGSSplat", name
    assert asset.get_editor_property("num_splats") == 128, name
    assert asset.get_editor_property("sh_degree") == 1, name
    normals = asset.get_editor_property("normals")
    assert normals == (unreal.SGSNormals.NONE if name == "no_normals" else unreal.SGSNormals.FILE), (name, normals)
    mesh = asset.get_editor_property("shadow_proxy")
    assert bool(mesh) == (name == "mesh"), name
    assert asset.get_editor_property("has_embedded_proxy") == (name == "mesh"), name
    if mesh:
        assert "2 triangles" in asset.get_editor_property("shadow_proxy_note")
    return asset


if phase == "negative":
    assert import_asset("unsupported", True) is None
    assert import_asset("corrupt", True) is None
    (root / "negative.json").write_text(json.dumps({"rejected_unsupported_proxy": True, "rejected_corrupt_sgs": True}))
elif phase == "import":
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    for i, name in enumerate(("plain", "mesh", "no_normals")):
        asset = import_asset(name, name == "mesh")
        assert asset is not None, name
        inspect(name)
        actor = actors.spawn_actor_from_object(asset, unreal.Vector(0, i * 120, 0))
        assert isinstance(actor, unreal.SGSSplatActor), name
        assert actor.get_editor_property("splat_component").get_editor_property("splat") == asset
        sprite = actor.get_component_by_class(unreal.BillboardComponent).get_editor_property("sprite")
        assert sprite.get_path_name() == "/SGS/Icons/S_SGSCapture.S_SGSCapture", sprite
    # Replace/reimport the same source without producing a second asset.
    assert import_asset("plain", False) is not None
    inspect("plain")
    static_only = os.environ.get("VGS_SGS_QA_STATIC_ONLY") == "1"
    if not static_only:
        actor = actors.spawn_actor_from_class(unreal.VGSCaptureActor, unreal.Vector(0, -120, 0))
        sprite = actor.get_component_by_class(unreal.BillboardComponent).get_editor_property("sprite")
        assert sprite.get_path_name() == "/VGS/Icons/S_VGSCapture.S_VGSCapture", sprite
        # Keep the empty VGS actor out of the saved SGS-only map.
        actors.destroy_actor(actor)
    assert unreal.EditorLoadingAndSavingUtils.save_map(unreal.EditorLevelLibrary.get_editor_world(), f"{folder}/Imported")
    (root / ("import_static_only.json" if static_only else "import.json")).write_text(json.dumps({"assets": ["plain", "mesh", "no_normals"], "drag_drop_factory": True, "actor_icons": True, "reimport": True, "animated_vgs_disabled": static_only}))
elif phase == "reload":
    assert not list((root / "fixtures").glob("*.sgs")), "source files must be absent for this check"
    assert unreal.EditorLoadingAndSavingUtils.load_map(f"{folder}/Imported")
    for name in ("plain", "mesh", "no_normals"):
        inspect(name)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    placed = [a for a in actors if isinstance(a, unreal.SGSSplatActor)]
    assert len(placed) == 3
    for actor in placed:
        assert actor.get_editor_property("splat_component").get_editor_property("splat") is not None
    (root / "reload.json").write_text(json.dumps({"source_sgs_absent": True, "assets_reloaded": 3, "level_actor_references": 3}))
else:
    raise RuntimeError(phase)
unreal.log(f"SGS acceptance {phase} passed")
