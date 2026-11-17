"""Run with UnrealEditor-Cmd -run=pythonscript -AllowCommandletRendering -script=<this file>.

Rebuilds the small Light Function assets shipped by VGS and SGS. It uses
the public material graph API; the runtime plugin never compiles material graphs.
"""
import unreal

CODE = "if (Enabled < 0.5f) return 1.0f; return VGSSampleDeepShadow(ShadowAtlas, ShadowAtlasSampler, CaptureMask, CaptureMaskSampler, WorldPosition, GridOrigin, GridX, GridY, GridZ, GridExtent, GridResolution, DepthBias, SelfShadow, Penumbra);"


def include_path(plugin):
    filename = "SGSSplatDeepShadow.ush" if plugin == "SGS" else "VGSSplatDeepShadow.ush"
    return f"/Plugin/{plugin}/Private/SplatRenderer/{filename}"


def build(plugin):
    path = f"/{plugin}/Materials"
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = unreal.load_asset(f"{path}/M_SplatShadowLightFunction")
    if material is None:
        material = tools.create_asset("M_SplatShadowLightFunction", path, unreal.Material, unreal.MaterialFactoryNew())
    else:
        # Constructor-loaded material expressions can be rooted in UE 5.9. Update the
        # two custom nodes in place rather than marking rooted expressions as garbage.
        nodes = [node for node in unreal.MaterialEditingLibrary.get_material_expressions(material)
                 if isinstance(node, unreal.MaterialExpressionCustom)]
        if len(nodes) != 2:
            raise RuntimeError(f"Unexpected graph in {material.get_path_name()}")
        for node in nodes:
            node.set_editor_property("inputs", [item for item in node.get_editor_property("inputs")
                                               if str(item.get_editor_property("input_name")) != "ScreenUV"])
            node.set_editor_property("code", CODE)
            node.set_editor_property("include_file_paths", [include_path(plugin)])
        unreal.MaterialEditingLibrary.recompile_material(material)
        unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
        unreal.log(f"Updated {material.get_path_name()}")
        return
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_LIGHT_FUNCTION)
    material.set_editor_property("force_compatible_with_light_function_atlas", False)
    edit = unreal.MaterialEditingLibrary
    a = build_slot(material, plugin, "A")
    b = build_slot(material, plugin, "B")
    product = edit.create_material_expression(material, unreal.MaterialExpressionMultiply)
    edit.connect_material_expressions(a, "", product, "A")
    edit.connect_material_expressions(b, "", product, "B")
    edit.connect_material_property(product, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    edit.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
    unreal.log(f"Created {material.get_path_name()}")


def build_slot(material, plugin, slot):
    edit = unreal.MaterialEditingLibrary
    custom = edit.create_material_expression(material, unreal.MaterialExpressionCustom, 500, 0)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    custom.set_editor_property("include_file_paths", [include_path(plugin)])
    inputs = []

    def connect(name, expression, output=""):
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        inputs.append(item)
        custom.set_editor_property("inputs", inputs)
        if not edit.connect_material_expressions(expression, output, custom, name):
            raise RuntimeError(f"Could not connect {name}")
        # Keep the connected expression inputs when appending the next one.
        inputs[:] = custom.get_editor_property("inputs")

    for name in ("ShadowAtlas", "CaptureMask"):
        node = edit.create_material_expression(material, unreal.MaterialExpressionTextureObjectParameter)
        node.set_editor_property("parameter_name", name + slot)
        node.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture"))
        connect(name, node)
    connect("WorldPosition", edit.create_material_expression(material, unreal.MaterialExpressionWorldPosition))
    for name in ("GridOrigin", "GridX", "GridY", "GridZ", "GridExtent"):
        node = edit.create_material_expression(material, unreal.MaterialExpressionVectorParameter)
        node.set_editor_property("parameter_name", name + slot)
        node.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 0))
        connect(name, node)
    for name, value in (("Enabled", 0), ("GridResolution", 512), ("DepthBias", 3), ("SelfShadow", 0), ("Penumbra", 0)):
        node = edit.create_material_expression(material, unreal.MaterialExpressionScalarParameter)
        node.set_editor_property("parameter_name", name + slot)
        node.set_editor_property("default_value", value)
        connect(name, node)
    custom.set_editor_property("code", CODE)
    return custom


for name in ("VGS", "SGS"):
    build(name)
