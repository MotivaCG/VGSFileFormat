# SPDX-License-Identifier: GPL-3.0-or-later
# Additional permission for the VGS native library: see LICENSE.md.

"""Self-illuminated or shaded: how a capture answers the scene's lights.

A Gaussian splat point cloud with no material draws its colour as emission (Blender's
default: Attribute "radiance" into Emission), so lights do nothing to it. Shaded gives it
a material that mixes that emission with a Principled BSDF fed the same colour, roughness
1 and IOR 1 so it is diffuse only, mostly shaded with a little self-illumination left in.

The material is shared by every capture of the same file, as any Blender material can be;
making it single user, the usual way, gives a capture its own. Going back to
Self-Illumination wires the emission straight to the output again and leaves the added
nodes where they are, so going to Shaded once more brings back what was set on them. The
mode is read from the material's wiring rather than stored, so captures sharing a
material can never show different modes.
"""

import os

import bpy

SHADING_MODES = (
    ('SELF', "Self-Illumination",
     "The capture's colour as recorded, untouched by the scene's lights"),
    ('SHADED', "Shaded",
     "Lit by the scene's lights, with a little self-illumination left in. Adds a material "
     "shared by every capture of the same file"),
)

MIX_NAME = "VGS Mix"
BSDF_NAME = "VGS Shaded"
RADIANCE_NAME = "VGS Radiance"
# How much of the mix is shaded when it is first made: the rest is self-illumination,
# which keeps shadowed sides from going black.
DEFAULT_SHADED_AMOUNT = 0.9

# On the material: the capture it was made for, and which node and socket gave the
# emission before the mix took its place.
_FILE_KEY = "vgs_filepath"
_SELF_NODE_KEY = "vgs_self_node"
_SELF_SOCKET_KEY = "vgs_self_socket"


def _file_key(pointcloud):
    path = pointcloud.vgs.filepath
    return os.path.normcase(os.path.normpath(bpy.path.abspath(path))) if path else ""


def material(pointcloud):
    """The point cloud's first material, or None."""
    for mat in pointcloud.materials:
        if mat is not None:
            return mat
    return None


def _shared_material(pointcloud):
    key = _file_key(pointcloud)
    if not key:
        return None
    for mat in bpy.data.materials:
        if mat.get(_FILE_KEY) == key and mat.library is None and mat.node_tree is not None:
            return mat
    return None


def _output(tree):
    output = tree.get_output_node('ALL')
    if output is None:
        output = tree.nodes.new("ShaderNodeOutputMaterial")
        output.location = (200, 100)
        output.is_active_output = True
    return output


def _surface_link(tree):
    surface = _output(tree).inputs["Surface"]
    return surface.links[0] if surface.is_linked else None


def _radiance(tree):
    for node in tree.nodes:
        if node.bl_idname == "ShaderNodeAttribute" and node.attribute_name == "radiance":
            return node
    node = tree.nodes.new("ShaderNodeAttribute")
    node.name = RADIANCE_NAME
    node.attribute_name = "radiance"
    node.location = (-300, 100)
    return node


def _new_material(pointcloud):
    """Blender's own material for Gaussian splats, which is self-illuminated."""
    mat = bpy.data.materials.new("VGS " + os.path.splitext(
        os.path.basename(pointcloud.vgs.filepath))[0])
    if mat.node_tree is None:
        mat.use_nodes = True
    tree = mat.node_tree
    tree.nodes.clear()
    attribute = _radiance(tree)
    emission = tree.nodes.new("ShaderNodeEmission")
    emission.location = (-50, 100)
    output = _output(tree)
    tree.links.new(attribute.outputs["Color"], emission.inputs["Color"])
    tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
    mat[_FILE_KEY] = _file_key(pointcloud)
    return mat


def _self_socket(mat):
    """The output the emission came from before the mix replaced it, if it still exists."""
    node = mat.node_tree.nodes.get(mat.get(_SELF_NODE_KEY, ""))
    if node is None or node.name == MIX_NAME:
        return None
    return next((s for s in node.outputs if s.identifier == mat.get(_SELF_SOCKET_KEY)), None)


def _mix(mat):
    """The shaded nodes, made if missing, with the emission on the mix's first input."""
    tree = mat.node_tree
    output = _output(tree)
    mix = tree.nodes.get(MIX_NAME)
    if mix is None:
        mix = tree.nodes.new("ShaderNodeMixShader")
        mix.name = mix.label = MIX_NAME
        mix.location = (output.location.x, output.location.y - 220)
        mix.inputs["Fac"].default_value = DEFAULT_SHADED_AMOUNT
    bsdf = tree.nodes.get(BSDF_NAME)
    if bsdf is None:
        bsdf = tree.nodes.new("ShaderNodeBsdfPrincipled")
        bsdf.name = bsdf.label = BSDF_NAME
        bsdf.location = (mix.location.x - 300, mix.location.y - 150)
        bsdf.inputs["Roughness"].default_value = 1.0
        bsdf.inputs["IOR"].default_value = 1.0
        tree.links.new(_radiance(tree).outputs["Color"], bsdf.inputs["Base Color"])
    if not mix.inputs[2].is_linked:
        tree.links.new(bsdf.outputs["BSDF"], mix.inputs[2])
    if not mix.inputs[1].is_linked:
        source = _self_socket(mat)
        if source is not None:
            tree.links.new(source, mix.inputs[1])
    return mix


def mix_node(pointcloud):
    """The mix of a shaded capture's material, for its slider, or None."""
    mat = material(pointcloud)
    if mat is None or mat.node_tree is None:
        return None
    link = _surface_link(mat.node_tree)
    return link.from_node if link is not None and link.from_node.name == MIX_NAME else None


def get_mode(pointcloud):
    return 'SHADED' if mix_node(pointcloud) is not None else 'SELF'


def set_mode(pointcloud, mode):
    mat = material(pointcloud)
    if mode == 'SHADED':
        if mat is None:
            mat = _shared_material(pointcloud) or _new_material(pointcloud)
            if len(pointcloud.materials):
                pointcloud.materials[0] = mat
            else:
                pointcloud.materials.append(mat)
        if mat.node_tree is None:
            mat.use_nodes = True
        tree = mat.node_tree
        link = _surface_link(tree)
        if link is not None and link.from_node.name == MIX_NAME:
            return
        if link is not None:
            # Whatever drew the capture until now is its self-illumination.
            mat[_SELF_NODE_KEY] = link.from_node.name
            mat[_SELF_SOCKET_KEY] = link.from_socket.identifier
        mix = _mix(mat)
        tree.links.new(mix.outputs["Shader"], _output(tree).inputs["Surface"])
    else:
        mix = mix_node(pointcloud)
        if mix is None:
            return
        tree = mat.node_tree
        source = _self_socket(mat)
        if source is None and mix.inputs[1].is_linked:
            source = mix.inputs[1].links[0].from_socket
        output = _output(tree)
        if source is not None:
            tree.links.new(source, output.inputs["Surface"])
        else:
            tree.links.remove(output.inputs["Surface"].links[0])
