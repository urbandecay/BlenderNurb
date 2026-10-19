# SPDX-FileCopyrightText: 2018-2021 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import bpy
from mathutils import Matrix
from ...io.imp.user_extensions import import_user_extensions
from ..com.extras import set_extras
from .mesh import BlenderMesh
from .camera import BlenderCamera
from .light import BlenderLight
from .vnode import VNode


class BlenderNode():
    """Blender Node."""
    def __new__(cls, *args, **kwargs):
        raise RuntimeError("%s should not be instantiated" % cls)

    @staticmethod
    def create_vnode(gltf, vnode_id):
        """Create VNode and all its descendants."""
        vnode = gltf.vnodes[vnode_id]

        gltf.display_current_node += 1
        if bpy.app.debug_value == 101:
            gltf.log.critical("Node %d of %d (id %s)", gltf.display_current_node, len(gltf.vnodes), vnode_id)

        if vnode.type in [VNode.Object, VNode.Inst]:
            gltf_node = gltf.data.nodes[vnode_id] if isinstance(vnode_id, int) else None
            import_user_extensions('gather_import_node_before_hook', gltf, vnode, gltf_node)
            obj = BlenderNode.create_object(gltf, vnode_id)
            import_user_extensions('gather_import_node_after_hook', gltf, vnode, gltf_node, obj)
        elif vnode.type == VNode.DummyRoot:
            # Don't actually create this
            vnode.blender_object = None
            vnode.world_matrix = Matrix.Identity(4)

        for child in vnode.children:
            BlenderNode.create_vnode(gltf, child)

    @staticmethod
    def create_object(gltf, vnode_id):
        vnode = gltf.vnodes[vnode_id]

        if vnode.mesh_node_idx is not None:
            obj = BlenderNode.create_mesh_object(gltf, vnode)

        elif vnode.type == VNode.Inst and vnode.mesh_idx is not None:
            obj = BlenderNode.create_mesh_object(gltf, vnode)

        elif vnode.camera_node_idx is not None:
            pynode = gltf.data.nodes[vnode.camera_node_idx]
            cam = BlenderCamera.create(gltf, vnode, pynode.camera)
            name = vnode.name or cam.name
            obj = bpy.data.objects.new(name, cam)

            # Since we create the actual Blender object after the create call, we call the hook here
            import_user_extensions('gather_import_camera_after_hook', gltf, vnode, obj, cam)

        elif vnode.light_node_idx is not None:
            pynode = gltf.data.nodes[vnode.light_node_idx]
            light = BlenderLight.create(gltf, vnode, pynode.extensions['KHR_lights_punctual']['light'])
            name = vnode.name or light.name
            obj = bpy.data.objects.new(name, light)

            # Since we create the actual Blender object after the create call, we call the hook here
            import_user_extensions('gather_import_light_after_hook', gltf, vnode, obj, light)

        else:
            # Transform-only glTF nodes do not have a corresponding Blender object.
            obj = None

        vnode.blender_object = obj

        # Compose the VNode path into a world-space transform. Transform-only
        # nodes still contribute to their descendants without creating Empty
        # objects in Blender.
        trans, rot, scale = vnode.trs()
        local_matrix = Matrix.LocRotScale(trans, rot, scale)
        parent_vnode = gltf.vnodes.get(vnode.parent)
        if parent_vnode is None:
            world_matrix = local_matrix
        else:
            world_matrix = parent_vnode.world_matrix @ local_matrix
        vnode.world_matrix = world_matrix

        if obj is None:
            return None

        # Set extras (if came from a glTF node)
        if isinstance(vnode_id, int):
            pynode = gltf.data.nodes[vnode_id]
            set_extras(obj, pynode.extras)

        obj.rotation_mode = 'QUATERNION'
        obj.matrix_world = world_matrix

        # Store Rest matrix of object
        # Can't use directly matrix_world because not refreshed yet
        if hasattr(obj, 'gltf2_animation_rest'):
            obj.gltf2_animation_rest = obj.matrix_world.copy()

        # Add to scene, by linking to the collections
        if len(vnode.scenes) == 0:
            # Add to the orphan collection
            gltf.blender_collections[None].objects.link(obj)
        else:
            for c in vnode.scenes:
                if gltf.import_settings['import_scene_as_collection'] is True:
                    gltf.blender_collections[c].objects.link(obj)
                else:
                    if len(gltf.data.scenes or []) == 1:
                        # Assign to the scene collection of the scene
                        gltf.blender_scenes[c].collection.objects.link(obj)
                    else:
                        if c == gltf.data.scene:
                            # Assign to the active collection
                            gltf.blender_collections[c].objects.link(obj)
                        else:
                            # Assign to the scene collection of the scene
                            gltf.blender_scenes[c].collection.objects.link(obj)

        return obj

    @staticmethod
    def create_mesh_object(gltf, vnode):
        if vnode.type != VNode.Inst:
            # Regular case
            pynode = gltf.data.nodes[vnode.mesh_node_idx]
        else:
            class DummyPyNode:
                pass
            pynode = DummyPyNode()
            pynode.mesh = vnode.mesh_idx
            pynode.weights = None

        if not (0 <= pynode.mesh < len(gltf.data.meshes)):
            # Avoid traceback for invalid gltf file: invalid reference to meshes array
            # No Blender object is created for this invalid node.
            gltf.log.warning("Skipping glTF node with invalid mesh index %s", pynode.mesh)
            return None
        pymesh = gltf.data.meshes[pynode.mesh]

        # Detect if the mesh will be a Point Cloud or not
        is_point_cloud = False
        if gltf.import_settings.get('import_point_as_pointcloud', False):
            if all([prim.mode == 0 for prim in pymesh.primitives]):  # All POINTS
                is_point_cloud = True
        else:
            is_point_cloud = False

        # Key to cache the Blender mesh by.
        # Same cache key = instances of the same Blender mesh.
        cache_key = None
        if not pymesh.shapekey_names:
            cache_key = ()
        else:
            # Unlike glTF, all instances of a Blender mesh share shapekeys.
            # So two instances that might have different morph weights need
            # different cache keys.
            if pynode.weight_animation is False:
                cache_key = (tuple(pynode.weights or []),)
            else:
                cache_key = None  # don't use the cache at all

        if cache_key is not None and cache_key in pymesh.blender_name:
            mesh = bpy.data.meshes[pymesh.blender_name[cache_key]
                                   ] if is_point_cloud is False else bpy.data.pointclouds[pymesh.blender_name[cache_key]]
        else:
            if not is_point_cloud:
                gltf.log.info("Blender create Mesh node {}".format(pymesh.name or pynode.mesh))
                mesh = BlenderMesh.create(gltf, pynode.mesh)
            else:
                gltf.log.info("Blender create Point Cloud node {}".format(pymesh.name or pynode.mesh))
                mesh = BlenderMesh.create_pointcloud(gltf, pynode.mesh)

            if cache_key is not None:
                pymesh.blender_name[cache_key] = mesh.name

        name = vnode.name or mesh.name
        obj = bpy.data.objects.new(name, mesh)

        if pymesh.shapekey_names:
            BlenderNode.set_morph_weights(gltf, pynode, obj)

        return obj

    @staticmethod
    def set_morph_weights(gltf, pynode, obj):
        pymesh = gltf.data.meshes[pynode.mesh]
        weights = pynode.weights or pymesh.weights or []
        for i, weight in enumerate(weights):
            if pymesh.shapekey_names[i] is not None:
                kb = obj.data.shape_keys.key_blocks[pymesh.shapekey_names[i]]
                # extend range if needed
                if weight < kb.slider_min:
                    kb.slider_min = weight
                if weight > kb.slider_max:
                    kb.slider_max = weight
                kb.value = weight

                # Store default weight
                if hasattr(obj, 'gltf2_animation_weight_rest'):
                    w = obj.gltf2_animation_weight_rest.add()
                    w.val = weight
