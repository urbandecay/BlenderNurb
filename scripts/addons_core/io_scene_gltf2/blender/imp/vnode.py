# SPDX-FileCopyrightText: 2018-2021 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import bpy
from mathutils import Vector, Quaternion
from ...io.imp.gltf2_io_binary import BinaryData
from ..com.gltf2_blender_math import scale_rot_swap_matrix


def compute_vnodes(gltf):
    """Computes the tree of virtual nodes.
    Copies the glTF nodes into a tree of VNodes, then performs a series of
    passes to transform it into a form that we can import into Blender.
    """
    init_vnodes(gltf)
    fixup_multitype_nodes(gltf)
    correct_cameras_and_lights(gltf)


class VNode:
    """A "virtual" node.
    These are what eventually get turned into nodes
    in the Blender scene.
    """
    # Types
    Object = 0
    DummyRoot = 1
    Inst = 2

    def __init__(self):
        self.name = None
        self.default_name = 'Node'  # fallback when no name
        self.children = []
        self.parent = None
        self.world_matrix = None
        self.type = VNode.Object
        self.base_trs = (
            Vector((0, 0, 0)),
            Quaternion((1, 0, 0, 0)),
            Vector((1, 1, 1)),
        )
        # Additional rotations before/after the base TRS.
        # Allows per-vnode axis adjustment. See local_rotation.
        self.rotation_after = Quaternion((1, 0, 0, 0))
        self.rotation_before = Quaternion((1, 0, 0, 0))

        # Indices of the glTF node where the mesh, etc. came from.
        # (They can get moved around.)
        self.mesh_node_idx = None
        self.camera_node_idx = None
        self.light_node_idx = None

        # Store in which glTF scene(s) this node is used.
        self.scenes = []

    def trs(self):
        # (final TRS) = (rotation after) (base TRS) (rotation before)
        t, r, s = self.base_trs
        m = scale_rot_swap_matrix(self.rotation_before)
        return (
            self.rotation_after @ t,
            self.rotation_after @ r @ self.rotation_before,
            m @ s,
        )

    def base_locs_to_final_locs(self, base_locs):
        ra = self.rotation_after
        return [ra @ loc for loc in base_locs]

    def base_rots_to_final_rots(self, base_rots):
        ra, rb = self.rotation_after, self.rotation_before
        return [ra @ rot @ rb for rot in base_rots]

    def base_scales_to_final_scales(self, base_scales):
        m = scale_rot_swap_matrix(self.rotation_before)
        return [m @ scale for scale in base_scales]


def local_rotation(gltf, vnode_id, rot):
    """Appends a local rotation to vnode's world transform:
    (new world transform) = (old world transform) @ (rot)
    without changing the world transform of vnode's children.

    For correctness, rot must be a signed permutation of the axes
    (eg. (X Y Z)->(X -Z Y)) OR vnode's scale must always be uniform.
    """
    gltf.vnodes[vnode_id].rotation_before @= rot

    # Append the inverse rotation after children's TRS to cancel it out.
    rot_inv = rot.conjugated()
    for child in gltf.vnodes[vnode_id].children:
        gltf.vnodes[child].rotation_after = \
            rot_inv @ gltf.vnodes[child].rotation_after


def init_vnodes(gltf):
    # Map of all VNodes. The keys are arbitrary IDs.
    # Nodes coming from glTF use the index into gltf.data.nodes for an ID.
    gltf.vnodes = {}

    for i, pynode in enumerate(gltf.data.nodes or []):
        vnode = VNode()
        gltf.vnodes[i] = vnode
        vnode.name = pynode.name
        vnode.default_name = 'Node_%d' % i
        vnode.children = list(pynode.children or [])
        vnode.base_trs = get_node_trs(gltf, pynode)
        if pynode.mesh is not None:
            # Check if there is gpu_instancing extension
            if pynode.extensions and "EXT_mesh_gpu_instancing" in pynode.extensions.keys():
                manage_gpu_instancing(gltf, vnode, i, pynode.extensions['EXT_mesh_gpu_instancing'], pynode.mesh)
            else:
                vnode.mesh_node_idx = i
        if pynode.camera is not None:
            vnode.camera_node_idx = i
        if 'KHR_lights_punctual' in (pynode.extensions or {}):
            vnode.light_node_idx = i

    for id in gltf.vnodes:
        for child in gltf.vnodes[id].children:
            assert gltf.vnodes[child].parent is None
            gltf.vnodes[child].parent = id

    # Map the node/scene

    # Create a recursive function to find all the nodes in a scene
    # add assign the nodes to the scene(s)
    def add_nodes_to_scene(idx_scene, node):
        gltf.vnodes[node].scenes.append(idx_scene)
        for child in gltf.vnodes[node].children:
            add_nodes_to_scene(idx_scene, child)

    for idx_scene, scene in enumerate(gltf.data.scenes or []):
        for node in scene.nodes or []:
            add_nodes_to_scene(idx_scene, node)

    # Create a map of all scene / blender collections
    gltf.blender_collections = {}
    gltf.active_collection = bpy.context.collection
    gltf.blender_scenes = {}

    # Create needed scenes
    for idx_scene, scene in enumerate(gltf.data.scenes or []):
        # Create a new scene for all not default scenes
        if idx_scene != (gltf.data.scene or 0):
            new_scene = bpy.data.scenes.new(name=scene.name or "Scene %d" % idx_scene)
            gltf.blender_scenes[idx_scene] = new_scene
        else:
            gltf.blender_scenes[idx_scene] = bpy.context.scene

    # If we have only 1 scene, we can use the active collection
    # If we have multiple scenes, we create a collection for each scene (as child of active collection)
    # And if some nodes are orphan, we create a collection for them too
    if len(gltf.data.scenes or []) == 1:
        gltf.blender_collections[gltf.data.scene or 0] = bpy.context.collection
    elif len(gltf.data.scenes or []) > 1:
        for idx_scene, scene in enumerate(gltf.data.scenes or []):
            if gltf.import_settings['import_scene_as_collection'] is True:
                # Create a new collection for the scene
                collection = bpy.data.collections.new(gltf.data.scenes[idx_scene].name or "Scene %d" % idx_scene)
                # Link the collection to the active collection or the collection of the scene
                # Collection on current scene
                gltf.active_collection.children.link(collection)
                # Add the collection to the map
                gltf.blender_collections[idx_scene] = collection
            else:
                if idx_scene == gltf.data.scene:
                    gltf.blender_collections[idx_scene] = gltf.active_collection
                # No collection creation, so no linking
                # Link between glTF scene and blender scene is already done

    # Check if we have orphan nodes
    orphan_nodes = [node for node in gltf.vnodes if len(gltf.vnodes[node].scenes) == 0]
    if len(orphan_nodes) > 0:
        # Create a new collection for the orphan nodes
        orphan_collection = bpy.data.collections.new("Orphan Nodes")
        # Link the collection to the active collection
        gltf.active_collection.children.link(orphan_collection)
        # Add the collection to the map
        gltf.blender_collections[None] = orphan_collection

    # Inserting a root node will simplify things.
    roots = [id for id in gltf.vnodes if gltf.vnodes[id].parent is None]
    gltf.vnodes['root'] = VNode()
    gltf.vnodes['root'].type = VNode.DummyRoot
    gltf.vnodes['root'].default_name = 'Root'
    gltf.vnodes['root'].children = roots
    for root in roots:
        gltf.vnodes[root].parent = 'root'


def manage_gpu_instancing(gltf, vnode, i, ext, mesh_id):
    attrs = ext.get('attributes', {})

    trans_list = BinaryData.get_data_from_accessor(gltf, attrs.get('TRANSLATION', None)) \
        if attrs.get('TRANSLATION', None) is not None else None

    rot_list = BinaryData.get_data_from_accessor(gltf, attrs.get('ROTATION', None)) \
        if attrs.get('ROTATION', None) is not None else None

    scale_list = BinaryData.get_data_from_accessor(gltf, attrs.get('SCALE', None)) \
        if attrs.get('SCALE', None) is not None else None

    # Retrieve the first available attribute to get the number of children
    val = next((elem for elem in [
        trans_list,
        rot_list,
        scale_list,
    ] if elem is not None), None)

    # Wwe can't have only custom properties
    if not val:
        return

    length = len(val)

    if trans_list is None:
        trans_list = [None] * length
    if rot_list is None:
        rot_list = [None] * length
    if scale_list is None:
        scale_list = [None] * length

    assert len(trans_list) == len(rot_list) == len(scale_list)

    for inst in range(length):
        inst_id = '%d' % i + "." + '%d' % inst
        inst_vnode = VNode()
        inst_vnode.type = VNode.Inst
        gltf.vnodes[inst_id] = inst_vnode
        inst_vnode.name = None
        inst_vnode.default_name = 'Node_' + inst_id
        inst_vnode.children = []
        inst_vnode.base_trs = get_inst_trs(gltf, trans_list[inst], rot_list[inst], scale_list[inst])
        inst_vnode.mesh_idx = mesh_id
        # Do not set scenes here, this will be handle later by recursive add_nodes_to_scene

        vnode.children.append(inst_id)


def get_inst_trs(gltf, trans, rot, scale):
    t = gltf.loc_gltf_to_blender(trans or [0, 0, 0])
    r = gltf.quaternion_gltf_to_blender(rot or [0, 0, 0, 1])
    s = gltf.scale_gltf_to_blender(scale or [1, 1, 1])
    return t, r, s


def get_node_trs(gltf, pynode):
    if pynode.matrix is not None:
        m = gltf.matrix_gltf_to_blender(pynode.matrix)
        return m.decompose()

    t = gltf.loc_gltf_to_blender(pynode.translation or [0, 0, 0])
    r = gltf.quaternion_gltf_to_blender(pynode.rotation or [0, 0, 0, 1])
    s = gltf.scale_gltf_to_blender(pynode.scale or [1, 1, 1])
    return t, r, s


def fixup_multitype_nodes(gltf):
    """
    Blender objects hold one data block. Move additional mesh, camera, or
    light data onto child nodes when a glTF node contains more than one.
    """
    ids = list(gltf.vnodes.keys())
    for id in ids:
        vnode = gltf.vnodes[id]

        needs_move = False

        if vnode.mesh_node_idx is not None:
            if needs_move:
                new_id = str(id) + '.mesh'
                gltf.vnodes[new_id] = VNode()
                gltf.vnodes[new_id].mesh_node_idx = vnode.mesh_node_idx
                gltf.vnodes[new_id].parent = id
                gltf.vnodes[new_id].scenes = vnode.scenes
                vnode.children.append(new_id)
                vnode.mesh_node_idx = None
            needs_move = True

        if vnode.camera_node_idx is not None:
            if needs_move:
                new_id = str(id) + '.camera'
                gltf.vnodes[new_id] = VNode()
                gltf.vnodes[new_id].camera_node_idx = vnode.camera_node_idx
                gltf.vnodes[new_id].parent = id
                gltf.vnodes[new_id].scenes = vnode.scenes
                vnode.children.append(new_id)
                vnode.camera_node_idx = None
            needs_move = True

        if vnode.light_node_idx is not None:
            if needs_move:
                new_id = str(id) + '.light'
                gltf.vnodes[new_id] = VNode()
                gltf.vnodes[new_id].light_node_idx = vnode.light_node_idx
                gltf.vnodes[new_id].parent = id
                gltf.vnodes[new_id].scenes = vnode.scenes
                vnode.children.append(new_id)
                vnode.light_node_idx = None
            needs_move = True


def correct_cameras_and_lights(gltf):
    """
    Depending on the coordinate change, lights and cameras might need to be
    rotated to match Blender conventions for which axes they point along.
    """
    if gltf.camera_correction is None:
        return

    for id, vnode in gltf.vnodes.items():
        needs_correction = \
            vnode.camera_node_idx is not None or \
            vnode.light_node_idx is not None

        if needs_correction:
            local_rotation(gltf, id, gltf.camera_correction)
