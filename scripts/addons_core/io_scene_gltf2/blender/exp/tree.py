# SPDX-FileCopyrightText: 2021 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import bpy
import re
import uuid
from mathutils import Quaternion, Matrix, Vector
from ...io.exp.user_extensions import export_user_extensions
from ..com.blender_default import BLENDER_GLTF_SPECIAL_COLLECTION


def _natural_sort_key(name):
    """Mirror Blender's BLI_strcasecmp_natural: case-insensitive natural sort.
    Splits name into text/number chunks so that numeric parts sort numerically.
    E.g. Object2 < Object10, matching the Outliner's display order.
    """
    return [int(c) if c.isdigit() else c.lower() for c in re.split(r'(\d+)', name)]


def _sort_by_name(iterable):
    """Sort Blender objects/collections using natural sort to match
    the Outliner's default alphabetical display order."""
    return sorted(iterable, key=lambda x: _natural_sort_key(x.name))


class VExportNode:

    OBJECT = 1
    LIGHT = 2
    CAMERA = 3
    COLLECTION = 4
    INSTANCE = 5  # For instances of GN

    # Children type
    # Is used to split instance children into 2 categories:
    CHILDREN_REAL = 90
    CHILDREN_IS_IN_COLLECTION = 91

    def __init__(self):
        self.children = []
        self.children_type = {}  # Used to track child relationships for instances
        self.blender_type = None
        self.matrix_world = None

        self.blender_object = None
        self.default_hide_viewport = False
        self.force_as_empty = False  # Used for instancer display

        # glTF
        self.node = None

        # For mesh instance data of GN instances
        self.data = None
        self.materials = None

        self.is_instancer = False

    def add_child(self, uuid):
        self.children.append(uuid)

    def set_blender_data(self, blender_object):
        self.blender_object = blender_object

    def recursive_display(self, tree, mode):
        if mode == "simple":
            for c in self.children:
                print(
                    tree.nodes[c].uuid,
                    self.blender_object.name if self.blender_object is not None else "GN" +
                    self.data.name,
                    "-->",
                    tree.nodes[c].blender_object.name if tree.nodes[c].blender_object else "GN" +
                    tree.nodes[c].data.name)
                tree.nodes[c].recursive_display(tree, mode)


class VExportTree:
    def __init__(self, export_settings):
        self.nodes = {}
        self.roots = []

        self.export_settings = export_settings

        self.tree_troncated = False

        self.axis_basis_change = Matrix.Identity(4)
        if self.export_settings['gltf_yup']:
            self.axis_basis_change = Matrix(
                ((1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (0.0, -1.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)))

    def add_node(self, node):
        self.nodes[node.uuid] = node

    def add_children(self, uuid_parent, uuid_child):
        self.nodes[uuid_parent].add_child(uuid_child)

    def construct(self, blender_scene):
        bpy.context.window.scene = blender_scene

        # Make sure the active object is in object mode
        if bpy.context.active_object and bpy.context.active_object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')

        depsgraph = bpy.context.evaluated_depsgraph_get()

        if self.export_settings['gltf_hierarchy_full_collections'] is False:
            scene_eval = blender_scene.evaluated_get(depsgraph=depsgraph)
            for blender_object in _sort_by_name([obj.original for obj in scene_eval.objects]):
                self.recursive_node_traverse(blender_object, None, Matrix.Identity(4))
        else:
            if self.export_settings['gltf_collection']:
                # Collection exporter
                self.recursive_node_traverse(
                    bpy.data.collections[self.export_settings['gltf_collection']],
                    None,
                    Matrix.Identity(4),
                    is_collection=True)
            else:
                # Scene / classic export
                self.recursive_node_traverse(
                    blender_scene.collection,
                    None,
                    Matrix.Identity(4),
                    is_collection=True)

    def recursive_node_traverse(
            self,
            blender_object,
            parent_uuid,
            parent_coll_matrix_world,
            dupli_world_matrix=None,
            data=None,
            original_object=None,
            is_collection=False,
            is_children_in_collection=False):
        node = VExportNode()
        node.uuid = str(uuid.uuid4())
        node.parent_uuid = parent_uuid
        node.set_blender_data(blender_object)
        if blender_object is None:
            node.data = data
            node.original_object = original_object

        # add to parent if needed
        if parent_uuid is not None:
            self.add_children(parent_uuid, node.uuid)

            # 2 cases where we will need to store the fact that children are in collection or a real children
            #       1. GN instance
            #       2. Old Dupli vertices feature
            # For any other case, children are real children
            if original_object is not None or (self.nodes[parent_uuid].blender_type !=
                                               VExportNode.COLLECTION and
                                               self.nodes[parent_uuid].blender_object is not None and
                                               self.nodes[parent_uuid].blender_object.is_instancer is True):
                self.nodes[parent_uuid].children_type[node.uuid] = VExportNode.CHILDREN_IS_IN_COLLECTION if is_children_in_collection is True else VExportNode.CHILDREN_REAL
            else:
                # We are in a regular case where children are real children
                self.nodes[parent_uuid].children_type[node.uuid] = VExportNode.CHILDREN_REAL
        else:
            self.roots.append(node.uuid)

        # Set blender type
        if blender_object is None:  # GN instance
            node.blender_type = VExportNode.INSTANCE
        elif is_collection is True:
            node.blender_type = VExportNode.COLLECTION
        elif blender_object.type == "CAMERA":
            node.blender_type = VExportNode.CAMERA
        elif blender_object.type == "LIGHT":
            node.blender_type = VExportNode.LIGHT
        else:
            node.blender_type = VExportNode.OBJECT
            node.default_hide_viewport = blender_object.hide_viewport

        # World Matrix

        # Store World Matrix for objects
        if dupli_world_matrix is not None:
            node.matrix_world = dupli_world_matrix
        elif node.blender_type in [VExportNode.OBJECT, VExportNode.COLLECTION, VExportNode.CAMERA, VExportNode.LIGHT]:
            # Objects in exported collections use the collection's transform context.
            if is_collection:
                node.matrix_world = parent_coll_matrix_world.copy()
            else:
                node.matrix_world = parent_coll_matrix_world @ blender_object.matrix_world.copy()

            if node.blender_type == VExportNode.CAMERA and self.export_settings['gltf_cameras']:
                if self.export_settings['gltf_yup']:
                    correction = Quaternion((2**0.5 / 2, -2**0.5 / 2, 0.0, 0.0))
                else:
                    correction = Matrix.Identity(4).to_quaternion()
                node.matrix_world @= correction.to_matrix().to_4x4()
            elif node.blender_type == VExportNode.LIGHT and self.export_settings['gltf_lights']:
                if self.export_settings['gltf_yup']:
                    correction = Quaternion((2**0.5 / 2, -2**0.5 / 2, 0.0, 0.0))
                else:
                    correction = Matrix.Identity(4).to_quaternion()
                node.matrix_world @= correction.to_matrix().to_4x4()

        # Force empty ?
        # For duplis, if instancer is not display, we should create an empty
        if blender_object and is_collection is False and blender_object.is_instancer is True and blender_object.show_instancer_for_render is False:
            node.force_as_empty = True

        # Storing this node
        self.add_node(node)

        ###### Manage children ######

        # GN instance have no children
        if blender_object is None:
            return

        if is_collection is True:  # Only for gltf_hierarchy_full_collections == True
            # Manage children objects
            collection_objects = set(blender_object.objects)
            for child in _sort_by_name(collection_objects):
                # Collection.objects includes objects in nested collections, not only direct children.

                self.recursive_node_traverse(child, node.uuid, parent_coll_matrix_world)
            # Manage children collections
            for child in _sort_by_name(blender_object.children):
                self.recursive_node_traverse(
                    child,
                    node.uuid,
                    node.matrix_world,
                    is_collection=True)

        # Duplis
        if is_collection is False and blender_object.is_instancer is True:
            depsgraph = bpy.context.evaluated_depsgraph_get()
            for (
                dupl,
                mat) in [
                (dup.object.original,
                 dup.matrix_world.copy()) for dup in depsgraph.object_instances if
                    dup.parent and id(dup.parent.original) == id(blender_object)
                # Not sure why, but when duplis is on object where there is also a GN that instance some data,
                # this returns the object itself, so we avoid an infinite loop by checking it
                # This is only to avoid infinite loop, as the GN will take precedence over dupli
                and id(dup.object.original) != id(blender_object)
            ]:
                self.recursive_node_traverse(
                    dupl,
                    node.uuid,
                    parent_coll_matrix_world,
                    dupli_world_matrix=mat)

        # Geometry Nodes instances
        # Geometry Nodes instances are exported from their evaluated instances.
        if self.export_settings['gltf_gn_mesh'] is True and node.blender_type == VExportNode.OBJECT:
            # Do not force export as empty
            # Because GN graph can have both geometry and instances
            depsgraph = bpy.context.evaluated_depsgraph_get()
            eval = blender_object.evaluated_get(depsgraph)
            for inst in depsgraph.object_instances:  # use only as iterator
                if inst.parent == eval:
                    if not inst.is_instance:
                        continue
                    if type(inst.object.data).__name__ == "Mesh" and len(inst.object.data.vertices) == 0:
                        continue  # This is nested instances, and this mesh has no vertices, so is an instancier for other instances
                    node.is_instancer = True
                    self.recursive_node_traverse(
                        None,
                        node.uuid,
                        parent_coll_matrix_world,
                        dupli_world_matrix=inst.matrix_world.copy(),
                        data=inst.object.data,
                        original_object=blender_object,
                        is_children_in_collection=True)

    def get_all_objects(self):
        return [n.uuid for n in self.nodes.values()]

    def get_all_node_of_type(self, node_type):
        return [n.uuid for n in self.nodes.values() if n.blender_type == node_type]

    def display(self, mode):
        if mode == "simple":
            for n in self.roots:
                print(
                    self.nodes[n].uuid,
                    "Root",
                    self.nodes[n].blender_object.name if self.nodes[n].blender_object else "GN instance")
                self.nodes[n].recursive_display(self, mode)

    def filter_tag(self):
        roots = self.roots.copy()
        for r in roots:
            self.recursive_filter_tag(r, None)

    def filter_perform(self):
        roots = self.roots.copy()
        for r in roots:
            self.recursive_filter(r, None)  # Root, so no parent

    def filter(self):
        self.filter_tag()
        export_user_extensions('gather_tree_filter_tag_hook', self.export_settings, self)
        self.filter_perform()
        self.remove_empty_collections()  # Used only when exporting full collection hierarchy
        self.remove_filtered_nodes()

    def recursive_filter_tag(self, uuid, parent_keep_tag):
        # parent_keep_tag tracks inherited visibility for nested objects and instances.
        # some properties (selection, visibility, renderability)
        # are defined at collection level, and we need to use these values
        # for all objects nested under the collection.
        # But some properties (camera, lamp ...) are not defined at collection level
        if parent_keep_tag is None:
            self.nodes[uuid].keep_tag = self.node_filter_not_inheritable_is_kept(
                uuid) and self.node_filter_inheritable_is_kept(uuid)
        elif parent_keep_tag is True:
            self.nodes[uuid].keep_tag = self.node_filter_not_inheritable_is_kept(uuid)
        elif parent_keep_tag is False:
            self.nodes[uuid].keep_tag = False
        else:
            self.export_settings['log'].error("This should not happen")

        for child in self.nodes[uuid].children:
            if self.nodes[uuid].is_instancer:
                # We need to split children into 2 categories: real children, and objects inside the collection
                if self.nodes[uuid].children_type[child] == VExportNode.CHILDREN_IS_IN_COLLECTION:
                    self.recursive_filter_tag(child, self.nodes[uuid].keep_tag)
                else:
                    self.recursive_filter_tag(child, parent_keep_tag)
            else:
                self.recursive_filter_tag(child, parent_keep_tag)

    def recursive_filter(self, uuid, parent_kept_uuid):
        children = self.nodes[uuid].children.copy()

        new_parent_kept_uuid = None
        if self.nodes[uuid].keep_tag is False:
            new_parent_kept_uuid = parent_kept_uuid
            # Need to modify tree
            if self.nodes[uuid].parent_uuid is not None:
                self.nodes[self.nodes[uuid].parent_uuid].children.remove(uuid)

        else:
            new_parent_kept_uuid = uuid

            # If parent_uuid is not parent_kept_uuid, we need to modify children list of parent_kept_uuid
            if parent_kept_uuid != self.nodes[uuid].parent_uuid and parent_kept_uuid is not None:
                self.tree_troncated = True
                self.nodes[parent_kept_uuid].children.append(uuid)

            # If parent_kept_uuid is None, and parent_uuid was not, add to root list
            if self.nodes[uuid].parent_uuid is not None and parent_kept_uuid is None:
                self.tree_troncated = True
                self.roots.append(uuid)

            # Modify parent uuid
            self.nodes[uuid].parent_uuid = parent_kept_uuid

        for child in children:
            self.recursive_filter(child, new_parent_kept_uuid)

    def node_filter_not_inheritable_is_kept(self, uuid):
        # Export Camera or not
        if self.nodes[uuid].blender_type == VExportNode.CAMERA:
            if self.export_settings['gltf_cameras'] is False:
                return False

        # Export Lamp or not
        if self.nodes[uuid].blender_type == VExportNode.LIGHT:
            if self.export_settings['gltf_lights'] is False:
                return False

        return True

    def node_filter_inheritable_is_kept(self, uuid):

        if self.nodes[uuid].blender_object is None:
            # geometry node instances
            return True

        if self.nodes[uuid].blender_type == VExportNode.COLLECTION:
            # Collections, can't be filtered => we always keep them
            return True

        if self.export_settings['gltf_selected'] and self.nodes[uuid].blender_object.select_get() is False:
            return False

        if self.export_settings['gltf_visible']:
            # The eye in outliner (object)
            if self.nodes[uuid].blender_object.visible_get() is False:
                return False

            # The screen in outliner (object)
            if self.nodes[uuid].blender_object.hide_viewport is True:
                return False

            # The screen in outliner (collections)
            if all([c.hide_viewport for c in self.nodes[uuid].blender_object.users_collection]):
                return False

        # The camera in outliner (object)
        if self.export_settings['gltf_renderable']:
            if self.nodes[uuid].blender_object.hide_render is True:
                return False

            # The camera in outliner (collections)
            if all([c.hide_render for c in self.nodes[uuid].blender_object.users_collection]):
                return False

        # If we are given a collection, use all objects from it
        if self.export_settings['gltf_collection']:
            local_collection = bpy.data.collections.get((self.export_settings['gltf_collection'], None))
            if not local_collection:
                return False
            found = any(x == self.nodes[uuid].blender_object for x in local_collection.all_objects)
            if not found:
                return False
        else:
            if self.export_settings['gltf_active_collection'] and not self.export_settings['gltf_active_collection_with_nested']:
                found = any(x == self.nodes[uuid].blender_object for x in bpy.context.collection.objects)
                if not found:
                    return False

            if self.export_settings['gltf_active_collection'] and self.export_settings['gltf_active_collection_with_nested']:
                found = any(x == self.nodes[uuid].blender_object for x in bpy.context.collection.all_objects)
                if not found:
                    return False

        if BLENDER_GLTF_SPECIAL_COLLECTION in bpy.data.collections and self.nodes[uuid].blender_object.name in \
                bpy.data.collections[BLENDER_GLTF_SPECIAL_COLLECTION].objects:
            return False

        return True

    def remove_filtered_nodes(self):
        self.nodes = {k: n for (k, n) in self.nodes.items() if n.keep_tag is True}

    def remove_empty_collections(self):
        def recursive_remove_empty_collections(uuid):
            if self.nodes[uuid].blender_type == VExportNode.COLLECTION:
                if len(self.nodes[uuid].children) == 0:
                    if self.nodes[uuid].parent_uuid is not None:
                        self.nodes[self.nodes[uuid].parent_uuid].children.remove(uuid)
                    else:
                        self.roots.remove(uuid)
                    self.nodes[uuid].keep_tag = False
                else:
                    for c in self.nodes[uuid].children:
                        recursive_remove_empty_collections(c)

        roots = self.roots.copy()
        for r in roots:
            recursive_remove_empty_collections(r)

    def variants_reset_to_original(self):
        # Only if Variants are displayed and exported
        if bpy.context.preferences.addons['io_scene_gltf2'].preferences.KHR_materials_variants_ui is False:
            return
        objects = [self.nodes[o].blender_object for o in self.get_all_node_of_type(VExportNode.OBJECT) if self.nodes[o].blender_object.type == "MESH"
                   and self.nodes[o].blender_object.data.gltf2_variant_default_materials is not None]
        for obj in objects:
            # loop on material slots ( primitives )
            for mat_slot_idx, s in enumerate(obj.material_slots):
                # Check if there is a default material for this slot
                for i in obj.data.gltf2_variant_default_materials:
                    if i.material_slot_index == mat_slot_idx:
                        s.material = i.default_material
                        break

            # If not found, keep current material as default

    def break_obj_hierarchy(self):
        # Can be useful when matrix is not decomposable
        # TODO: if we get real collection one day, we probably need to adapt this code
        for obj in self.get_all_objects():
            if self.nodes[obj].parent_uuid is not None:
                self.nodes[self.nodes[obj].parent_uuid].children.remove(obj)
                self.nodes[obj].parent_uuid = None
                self.roots.append(obj)

    def calculate_collection_center(self):
        # Because we already filtered the tree, we can use all objects
        # to calculate the center of the scene
        # Are taken into account all objects that are direct root in the exported collection
        centers = []

        for node in [
            n for n in self.nodes.values() if n.parent_uuid is None and n.blender_type in [
                VExportNode.OBJECT,
                VExportNode.LIGHT,
                VExportNode.CAMERA]]:
            if node.matrix_world is not None:
                centers.append(node.matrix_world.translation)

        if len(centers) == 0:
            self.export_settings['gltf_collection_center'] = Vector((0.0, 0.0, 0.0))
            return

        self.export_settings['gltf_collection_center'] = sum(centers, Vector()) / len(centers)
