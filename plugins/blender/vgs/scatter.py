# SPDX-License-Identifier: GPL-3.0-or-later

"""A capture scattered over points: variants of it, instanced with Geometry Nodes.

A scatter is a handful of variants of one capture - point clouds of their own, each at its
own phase and speed, played like any other capture - in a collection kept out of the view
layer, and a Geometry Nodes modifier on the points object that instances one of them on
every point. What a frame costs grows with the variants, not the copies: each is decoded
once, and every copy of it is an instance, drawn without copying a splat.

Everything about a scatter is set on its modifier. The instancing inputs - seed, turn,
scale - are used by the nodes directly; the ones that shape the variants - how many, their
phases and speeds - are watched by a depsgraph handler, which rebuilds the variants to
match. The variants also follow the capture's own settings, so changing the capture
changes its scatter.
"""

import math
import random

import bpy
from bpy.app.handlers import persistent
from bpy.props import FloatProperty, IntProperty
from bpy.types import Operator

from . import playback

MODIFIER_NAME = "VGS Scatter"
# On the points object: the collection its variants live in, the capture they copy, and
# what they were last built from.
_COLLECTION_KEY = "vgs_scatter_collection"
_CAPTURE_KEY = "vgs_scatter_capture"
_STATE_KEY = "vgs_scatter_state"

# The modifier's inputs, by name. The first five shape the variants; the rest the copies.
VARIANTS = "Variants"
PHASE_SPREAD = "Phase Spread"
SPEED = "Speed"
SPEED_VARIATION = "Speed Variation"
SEED = "Seed"
ROTATION = "Random Z Rotation"
INHERIT_SCALE = "Inherit Object Scale"
FOLLOW_NORMAL = "Follow Normal"
SHOW_ORIGINAL = "Show Original Geometry"

# What the Distribution section says above its button: this is one way to lay copies out,
# offered as a place to start.
NOTE = ("Tip: this button builds a quick example scatter with Geometry Nodes. "
        "Use it as a starting point and roll your own distribution. Any points work, "
        "so feel free to make them with your own nodes or modifiers above it. "
        "All the settings live on the modifier.")


# ---- the modifier's inputs ----------------------------------------------------------------


def _input_ids(modifier):
    group = modifier.node_group
    if group is None:
        return {}
    return {item.name: item.identifier for item in group.interface.items_tree
            if item.item_type == 'SOCKET' and item.in_out == 'INPUT'}


def get_input(modifier, name, default=None):
    identifier = _input_ids(modifier).get(name)
    if identifier is None:
        return default
    socket = getattr(modifier.properties.inputs, identifier, None)
    return default if socket is None else socket.value


def set_input(modifier, name, value):
    identifier = _input_ids(modifier).get(name)
    if identifier is not None:
        getattr(modifier.properties.inputs, identifier).value = value


# ---- the node group -----------------------------------------------------------------------


def _socket(tree, name, kind, default, description, **limits):
    socket = tree.interface.new_socket(name, in_out='INPUT', socket_type=kind)
    for key, value in limits.items():
        setattr(socket, key, value)
    socket.default_value = default
    socket.description = description
    return socket


def _node_group(name, collection, settings):
    """Collection Info -> Instance on Points, a variant, a turn and a scale per point."""
    tree = bpy.data.node_groups.new(name, 'GeometryNodeTree')
    tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    _socket(tree, VARIANTS, 'NodeSocketInt', settings[VARIANTS],
            "How many different timelines the copies share. Each point takes one at random, "
            "or the one its vgs_variant attribute names", min_value=1, max_value=256)
    _socket(tree, PHASE_SPREAD, 'NodeSocketFloat', settings[PHASE_SPREAD],
            "How much of the capture the variants' starting points cover: 1 all of it, 0 "
            "all at the capture's phase", subtype='FACTOR', min_value=0.0, max_value=1.0)
    _socket(tree, SPEED, 'NodeSocketFloat', settings[SPEED],
            "The variants' speed. Negative plays them backwards")
    _socket(tree, SPEED_VARIATION, 'NodeSocketFloat', settings[SPEED_VARIATION],
            "How much the variants' speeds differ, as a fraction of Speed: 0.2 plays them "
            "between 0.8 and 1.2 times as fast", subtype='FACTOR', min_value=0.0, max_value=1.0)
    _socket(tree, SEED, 'NodeSocketInt', settings[SEED],
            "Changes which variant each point gets, the variants' speeds and the turns",
            min_value=0)
    _socket(tree, ROTATION, 'NodeSocketFloat', settings[ROTATION],
            "Turn each copy about the vertical axis by a random angle up to this either way, "
            "different for every copy. 0 leaves them unturned; a point with a rotation "
            "attribute keeps its own", subtype='ANGLE', min_value=0.0, max_value=2.0 * math.pi)
    _socket(tree, INHERIT_SCALE, 'NodeSocketBool', False,
            "Scale the copies with the object whose points they sit on. Off, they keep the "
            "capture's own size however that object is scaled; their places still follow it")
    _socket(tree, FOLLOW_NORMAL, 'NodeSocketBool', False,
            "Stand each copy along the surface's normal instead of straight up. The random "
            "turn stays about the copy's own vertical axis. Points from Distribute Points on "
            "Faces carry no normal: store its Normal output as a 'normal' attribute")
    _socket(tree, SHOW_ORIGINAL, 'NodeSocketBool', True,
            "Keep the object's own geometry alongside the copies")
    tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    nodes, links = tree.nodes, tree.links

    group_in = nodes.new("NodeGroupInput")
    group_in.location = (-1400, 0)
    group_out = nodes.new("NodeGroupOutput")
    group_out.location = (800, 0)

    info = nodes.new("GeometryNodeCollectionInfo")
    info.location = (-500, 150)
    info.inputs["Collection"].default_value = collection
    info.inputs["Separate Children"].default_value = True
    # The variants keep their own transforms, which carry the capture's up-axis turn.
    info.inputs["Reset Children"].default_value = False

    # Which variant: the point's vgs_variant if it has one, a random one otherwise.
    variant = nodes.new("GeometryNodeInputNamedAttribute")
    variant.location = (-700, 450)
    variant.data_type = 'INT'
    variant.inputs["Name"].default_value = "vgs_variant"
    last = nodes.new("ShaderNodeMath")
    last.location = (-1000, 300)
    last.operation = 'SUBTRACT'
    links.new(group_in.outputs[VARIANTS], last.inputs[0])
    last.inputs[1].default_value = 1.0
    chance = nodes.new("FunctionNodeRandomValue")
    chance.location = (-700, 300)
    chance.data_type = 'INT'
    chance.inputs["Min"].default_value = 0
    links.new(last.outputs[0], chance.inputs["Max"])
    links.new(group_in.outputs[SEED], chance.inputs["Seed"])
    pick = nodes.new("GeometryNodeSwitch")
    pick.location = (-450, 350)
    pick.input_type = 'INT'
    links.new(variant.outputs["Exists"], pick.inputs[0])
    links.new(chance.outputs["Value"], pick.inputs[1])
    links.new(variant.outputs["Attribute"], pick.inputs[2])

    # Rotation: the point's own where it has one; otherwise a random turn about the
    # vertical axis of up to Random Z Rotation either way, different for every point.
    negative = nodes.new("ShaderNodeMath")
    negative.location = (-1100, -250)
    negative.operation = 'MULTIPLY'
    links.new(group_in.outputs[ROTATION], negative.inputs[0])
    negative.inputs[1].default_value = -1.0
    angle = nodes.new("FunctionNodeRandomValue")
    angle.location = (-900, -250)
    angle.data_type = 'FLOAT'
    links.new(negative.outputs[0], angle.inputs["Min"])
    links.new(group_in.outputs[ROTATION], angle.inputs["Max"])
    links.new(group_in.outputs[SEED], angle.inputs["Seed"])
    about_z = nodes.new("ShaderNodeCombineXYZ")
    about_z.location = (-700, -250)
    links.new(angle.outputs["Value"], about_z.inputs["Z"])
    euler = nodes.new("FunctionNodeEulerToRotation")
    euler.location = (-500, -250)
    links.new(about_z.outputs["Vector"], euler.inputs["Euler"])
    # Follow Normal: the turn first, about the copy's own vertical axis, then that axis
    # stood along the normal. Aligning takes the shortest rotation from Z to the normal,
    # which leaves a turn about Z as it was, so each copy still spins on itself on a slope.
    # The normal is the point's 'normal' attribute if it has one, the mesh's otherwise.
    normal_attribute = nodes.new("GeometryNodeInputNamedAttribute")
    normal_attribute.location = (-900, -500)
    normal_attribute.data_type = 'FLOAT_VECTOR'
    normal_attribute.inputs["Name"].default_value = "normal"
    mesh_normal = nodes.new("GeometryNodeInputNormal")
    mesh_normal.location = (-900, -650)
    normal = nodes.new("GeometryNodeSwitch")
    normal.location = (-700, -550)
    normal.input_type = 'VECTOR'
    links.new(normal_attribute.outputs["Exists"], normal.inputs[0])
    links.new(mesh_normal.outputs["Normal"], normal.inputs[1])
    links.new(normal_attribute.outputs["Attribute"], normal.inputs[2])
    align = nodes.new("FunctionNodeAlignRotationToVector")
    align.location = (-500, -500)
    align.axis = 'Z'
    links.new(euler.outputs["Rotation"], align.inputs["Rotation"])
    links.new(normal.outputs[0], align.inputs["Vector"])
    follow = nodes.new("GeometryNodeSwitch")
    follow.location = (-350, -300)
    follow.input_type = 'ROTATION'
    links.new(group_in.outputs[FOLLOW_NORMAL], follow.inputs[0])
    links.new(euler.outputs["Rotation"], follow.inputs[1])
    links.new(align.outputs["Rotation"], follow.inputs[2])

    rotation = nodes.new("GeometryNodeInputNamedAttribute")
    rotation.location = (-350, -450)
    rotation.data_type = 'QUATERNION'
    rotation.inputs["Name"].default_value = "rotation"
    turn = nodes.new("GeometryNodeSwitch")
    turn.location = (-150, -300)
    turn.input_type = 'ROTATION'
    links.new(rotation.outputs["Exists"], turn.inputs[0])
    links.new(follow.outputs[0], turn.inputs[1])
    links.new(rotation.outputs["Attribute"], turn.inputs[2])

    # Scale from the points, where they have one.
    scale = nodes.new("GeometryNodeInputNamedAttribute")
    scale.location = (-500, -550)
    scale.data_type = 'FLOAT_VECTOR'
    scale.inputs["Name"].default_value = "scale"
    size = nodes.new("GeometryNodeSwitch")
    size.location = (-250, -550)
    size.input_type = 'VECTOR'
    size.inputs[1].default_value = (1.0, 1.0, 1.0)
    links.new(scale.outputs["Exists"], size.inputs[0])
    links.new(scale.outputs["Attribute"], size.inputs[2])

    instance = nodes.new("GeometryNodeInstanceOnPoints")
    instance.location = (0, 0)
    instance.inputs["Pick Instance"].default_value = True
    links.new(group_in.outputs[0], instance.inputs["Points"])
    links.new(info.outputs[0], instance.inputs["Instance"])
    links.new(pick.outputs[0], instance.inputs["Instance Index"])
    links.new(turn.outputs[0], instance.inputs["Rotation"])
    links.new(size.outputs[0], instance.inputs["Scale"])

    # The copies live in the points object's space, so its scale would size them too. Each
    # is scaled back by the inverse of that scale, about its own position and in the
    # object's space: exact even for a non-uniform scale and a turned copy, and the
    # positions keep following the scaled surface.
    self_object = nodes.new("GeometryNodeSelfObject")
    self_object.location = (-250, -750)
    self_info = nodes.new("GeometryNodeObjectInfo")
    self_info.location = (-50, -750)
    self_info.transform_space = 'ORIGINAL'
    links.new(self_object.outputs[0], self_info.inputs["Object"])
    inverse = nodes.new("ShaderNodeVectorMath")
    inverse.location = (150, -750)
    inverse.operation = 'DIVIDE'
    inverse.inputs[0].default_value = (1.0, 1.0, 1.0)
    links.new(self_info.outputs["Scale"], inverse.inputs[1])
    keep = nodes.new("GeometryNodeSwitch")
    keep.location = (350, -600)
    keep.input_type = 'VECTOR'
    keep.inputs[2].default_value = (1.0, 1.0, 1.0)
    links.new(group_in.outputs[INHERIT_SCALE], keep.inputs[0])
    links.new(inverse.outputs["Vector"], keep.inputs[1])
    position = nodes.new("GeometryNodeInputPosition")
    position.location = (350, -750)
    unscale = nodes.new("GeometryNodeScaleInstances")
    unscale.location = (550, 0)
    unscale.inputs["Local Space"].default_value = False
    links.new(instance.outputs[0], unscale.inputs["Instances"])
    links.new(keep.outputs[0], unscale.inputs["Scale"])
    links.new(position.outputs["Position"], unscale.inputs["Center"])

    # The inputs that shape the variants are read by the add-on, not by these nodes, and
    # an input the nodes do not reach is shown greyed out on the modifier. Each copy
    # carries them instead, as instance attributes: they count as used, and a copy says
    # what it was made with.
    current = unscale.outputs[0]
    for index, (name, attribute) in enumerate((
            (PHASE_SPREAD, "vgs_phase_spread"),
            (SPEED, "vgs_speed"),
            (SPEED_VARIATION, "vgs_speed_variation"))):
        store = nodes.new("GeometryNodeStoreNamedAttribute")
        store.location = (750 + 200 * index, 0)
        store.data_type = 'FLOAT'
        store.domain = 'INSTANCE'
        store.inputs["Name"].default_value = attribute
        links.new(current, store.inputs["Geometry"])
        links.new(group_in.outputs[name], store.inputs["Value"])
        current = store.outputs["Geometry"]

    # The object's own geometry, shown alongside the copies unless asked not to.
    original = nodes.new("GeometryNodeSwitch")
    original.location = (1150, -250)
    original.input_type = 'GEOMETRY'
    links.new(group_in.outputs[SHOW_ORIGINAL], original.inputs[0])
    links.new(group_in.outputs[0], original.inputs[2])
    join = nodes.new("GeometryNodeJoinGeometry")
    join.location = (1400, 0)
    links.new(current, join.inputs[0])
    links.new(original.outputs[0], join.inputs[0])
    group_out.location = (1600, 0)
    links.new(join.outputs[0], group_out.inputs[0])
    return tree


# ---- the variants -------------------------------------------------------------------------


def _layer_collection(layer, collection):
    if layer.collection == collection:
        return layer
    for child in layer.children:
        found = _layer_collection(child, collection)
        if found is not None:
            return found
    return None


def _plain(value):
    return tuple(value) if hasattr(value, "__len__") and not isinstance(value, str) else value


def _capture_state(capture):
    """What of the capture its variants copy, as a comparable value."""
    settings = capture.data.vgs
    values = [(prop.identifier, _plain(getattr(settings, prop.identifier)))
              for prop in settings.bl_rna.properties
              if prop.identifier not in ("rna_type", "speed", "start_frame")
              and not prop.is_readonly]
    transform = (capture.rotation_mode, tuple(capture.rotation_euler),
                 tuple(capture.rotation_quaternion), tuple(capture.scale))
    return (values, transform)


def _state(modifier, capture):
    shape = tuple(get_input(modifier, name) for name in
                  (VARIANTS, PHASE_SPREAD, SPEED, SPEED_VARIATION, SEED))
    return repr((shape, _capture_state(capture)))


def _copy_settings(source, target):
    for prop in source.bl_rna.properties:
        name = prop.identifier
        if name == "rna_type" or prop.is_readonly or name == "start_frame":
            continue
        setattr(target, name, getattr(source, name))


def sync(points):
    """Rebuilds `points`' variants to match its modifier and its capture, if they differ."""
    modifier = points.modifiers.get(MODIFIER_NAME)
    collection = bpy.data.collections.get(points.get(_COLLECTION_KEY, ""))
    capture = bpy.data.objects.get(points.get(_CAPTURE_KEY, ""))
    if modifier is None or collection is None or capture is None or capture.data is None:
        return False
    state = _state(modifier, capture)
    if points.get(_STATE_KEY) == state:
        return False

    variants = max(1, int(get_input(modifier, VARIANTS, 8)))
    spread = float(get_input(modifier, PHASE_SPREAD, 1.0))
    speed = float(get_input(modifier, SPEED, 1.0))
    variation = float(get_input(modifier, SPEED_VARIATION, 0.0))
    seed = int(get_input(modifier, SEED, 0))
    source = capture.data.vgs

    with playback.suspended():
        existing = sorted(collection.objects, key=lambda o: o.get("vgs_variant_index", 0))
        for obj in existing[variants:]:
            data = obj.data
            bpy.data.objects.remove(obj)
            if data is not None and data.users == 0:
                bpy.data.pointclouds.remove(data)
        objects = existing[:variants]
        for k in range(len(objects), variants):
            pointcloud = bpy.data.pointclouds.new("{} {:d}".format(capture.data.name, k))
            pointcloud.type = 'GAUSSIAN_SPLAT'
            obj = bpy.data.objects.new("{} {:d}".format(capture.name, k), pointcloud)
            obj["vgs_variant_index"] = k
            collection.objects.link(obj)
            objects.append(obj)

        for k, obj in enumerate(objects):
            settings = obj.data.vgs
            _copy_settings(source, settings)
            phase = source.phase + spread * k / variants
            settings.phase = phase - math.floor(phase)
            chance = random.Random(seed * 1000003 + k).random()
            settings.speed = speed * (1.0 + variation * (2.0 * chance - 1.0))
            obj.rotation_mode = capture.rotation_mode
            obj.rotation_euler = capture.rotation_euler
            obj.rotation_quaternion = capture.rotation_quaternion
            obj.scale = capture.scale
            obj.location = (0.0, 0.0, 0.0)

    points[_STATE_KEY] = state
    return True


def remove_scatter(points):
    """Takes a scatter off `points`: its modifier, node group, variants and collection."""
    modifier = points.modifiers.get(MODIFIER_NAME)
    if modifier is not None:
        group = modifier.node_group
        points.modifiers.remove(modifier)
        if group is not None and group.users == 0:
            bpy.data.node_groups.remove(group)
    collection = bpy.data.collections.get(points.get(_COLLECTION_KEY, ""))
    if collection is not None:
        for obj in list(collection.objects):
            data = obj.data
            bpy.data.objects.remove(obj)
            if data is not None and data.users == 0:
                bpy.data.pointclouds.remove(data)
        bpy.data.collections.remove(collection)
    for key in (_COLLECTION_KEY, _CAPTURE_KEY, _STATE_KEY):
        if key in points:
            del points[key]


def scatter(context, capture, points, variants=8, phase_spread=1.0, speed_variation=0.0,
            seed=0, rotation=2.0 * math.pi, speed=None):
    """Scatters `capture` over `points`, replacing a scatter already there."""
    remove_scatter(points)
    name = "{} on {}".format(capture.name, points.name)
    collection = bpy.data.collections.new(name)
    context.scene.collection.children.link(collection)
    layer = _layer_collection(context.view_layer.layer_collection, collection)
    if layer is not None:
        # Out of the view layer: the variants are drawn through their instances only.
        layer.exclude = True
    points[_COLLECTION_KEY] = collection.name
    points[_CAPTURE_KEY] = capture.name

    settings = {
        VARIANTS: variants,
        PHASE_SPREAD: phase_spread,
        SPEED: capture.data.vgs.speed if speed is None else speed,
        SPEED_VARIATION: speed_variation,
        SEED: seed,
        ROTATION: rotation,
    }
    modifier = points.modifiers.new(MODIFIER_NAME, 'NODES')
    # A new modifier takes the group's defaults, which are this scatter's settings.
    modifier.node_group = _node_group(name, collection, settings)
    sync(points)
    playback.refresh(context)
    return collection


# ---- keeping scatters in step -------------------------------------------------------------

_syncing = False


@persistent
def _on_depsgraph_update(scene, depsgraph=None):
    """Rebuilds the variants of any scatter whose modifier or capture has changed."""
    global _syncing
    if _syncing or scene is None:
        return
    _syncing = True
    try:
        changed = False
        for obj in scene.objects:
            if _COLLECTION_KEY in obj and obj.modifiers.get(MODIFIER_NAME) is not None:
                changed = sync(obj) or changed
        if changed:
            playback.refresh()
    finally:
        _syncing = False


# ---- operators ----------------------------------------------------------------------------


def _points_object(context, capture):
    for obj in context.selected_objects:
        if obj is not capture and obj.type in {'MESH', 'POINTCLOUD', 'CURVES'}:
            return obj
    return None


class VGS_OT_scatter(Operator):
    """Put a copy of this capture on every point of another selected object, each at one
    of a few timelines: variants of the capture, instanced with Geometry Nodes"""
    bl_idname = "vgs.scatter"
    bl_label = "Scatter on Points"
    bl_options = {'REGISTER', 'UNDO'}

    variants: IntProperty(
        name="Variants",
        description=(
            "How many different timelines the copies share. Each point takes one at random, "
            "or the one its vgs_variant attribute names. What a frame costs grows with this, "
            "not with the number of copies"),
        default=8, min=1, soft_max=32, max=256,
    )
    phase_spread: FloatProperty(
        name="Phase Spread",
        description=(
            "How much of the capture the variants' starting points cover: 1 spreads them over "
            "all of it, 0 starts them all at the capture's phase"),
        default=1.0, min=0.0, max=1.0, subtype='FACTOR',
    )
    speed_variation: FloatProperty(
        name="Speed Variation",
        description=(
            "How much the variants' speeds differ, as a fraction of their speed: 0.2 plays "
            "them between 0.8 and 1.2 times as fast"),
        default=0.0, min=0.0, max=1.0, subtype='FACTOR',
    )
    rotation: FloatProperty(
        name="Random Z Rotation",
        description=(
            "Turn each copy about the vertical axis by a random angle up to this either way, "
            "different for every copy. 0 leaves them unturned"),
        default=2.0 * math.pi, min=0.0, max=2.0 * math.pi, subtype='ANGLE',
    )
    seed: IntProperty(
        name="Seed",
        description="Changes which variant each point gets, the variants' speeds and the turns",
        default=0, min=0,
    )

    @classmethod
    def poll(cls, context):
        obj = context.object
        return (obj is not None and obj.type == 'POINTCLOUD' and obj.data is not None
                and bool(obj.data.vgs.filepath))

    def invoke(self, context, event):
        if _points_object(context, context.object) is None:
            self.report({'ERROR'}, "Select the object whose points get the copies too, "
                                   "with this capture active")
            return {'CANCELLED'}
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        capture = context.object
        points = _points_object(context, capture)
        if points is None:
            self.report({'ERROR'}, "Select the object whose points get the copies too, "
                                   "with this capture active")
            return {'CANCELLED'}
        scatter(context, capture, points, self.variants, self.phase_spread,
                self.speed_variation, self.seed, self.rotation)
        self.report({'INFO'}, "{:d} variants of {} on {}".format(
            self.variants, capture.name, points.name))
        return {'FINISHED'}


class VGS_OT_remove_scatter(Operator):
    """Take the VGS scatter off the active object: its modifier and its variants"""
    bl_idname = "vgs.remove_scatter"
    bl_label = "Remove VGS Scatter"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.modifiers.get(MODIFIER_NAME) is not None

    def execute(self, context):
        remove_scatter(context.object)
        return {'FINISHED'}


_classes = (VGS_OT_scatter, VGS_OT_remove_scatter)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    if _on_depsgraph_update not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph_update)


def unregister():
    if _on_depsgraph_update in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph_update)
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
