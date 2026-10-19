# SPDX-FileCopyrightText: 2018-2021 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import bpy

from ...io.com import gltf2_io
from ...io.exp.user_extensions import export_user_extensions
from ..com.extras import generate_extras
from .cache import cached
from . import nodes as gltf2_blender_gather_nodes
from . import tree as gltf2_blender_gather_tree
from .animation.sampled.object.keyframes import get_cache_data
from .animation.animations import gather_animations


def gather_gltf2(export_settings):
    """
    Gather glTF properties from the current state of blender.

    :return: list of scene graphs to be added to the glTF export
    """
    scenes = []
    animations = []  # unfortunately animations in gltf2 are just as 'root' as scenes.
    active_scene = None
    store_user_scene = bpy.context.scene
    if export_settings['gltf_collection'] == "" and export_settings['gltf_active_scene'] is False:
        # If no collection export and no active scene export, we need to export all scenes
        scenes_to_export = bpy.data.scenes
    elif export_settings['gltf_collection'] == "" and export_settings['gltf_active_scene'] is True:
        # If no collection export and active scene export, we need to export only the active scene
        scenes_to_export = [
            scene for scene in bpy.data.scenes if scene.name == store_user_scene.name]
    elif export_settings['gltf_collection'] != "":
        # If collection export, we need to export only the collection, so keeping only the active scene
        scenes_to_export = [
            scene for scene in bpy.data.scenes if scene.name == store_user_scene.name]
    else:
        # This should never happen
        raise Exception("Unknown export settings")

    for blender_scene in scenes_to_export:
        scenes.append(__gather_scene(blender_scene, export_settings))
        if export_settings['gltf_animations']:
            # resetting object cache
            get_cache_data.reset_cache()
            animations += gather_animations(export_settings)
        if bpy.context.scene.name == store_user_scene.name:
            active_scene = len(scenes) - 1

    # restore user scene
    bpy.context.window.scene = store_user_scene
    return active_scene, scenes, animations


@cached
def __gather_scene(blender_scene, export_settings):
    scene = gltf2_io.Scene(
        extensions=None,
        extras=__gather_extras(blender_scene, export_settings),
        name=__gather_name(blender_scene, export_settings),
        nodes=[]
    )

    # Initialize some data needed for animation pointer
    export_settings['KHR_animation_pointer'] = {}
    export_settings['KHR_animation_pointer']['materials'] = {}
    export_settings['KHR_animation_pointer']['lights'] = {}
    export_settings['KHR_animation_pointer']['cameras'] = {}

    vtree = gltf2_blender_gather_tree.VExportTree(export_settings)
    vtree.construct(blender_scene)

    export_user_extensions('vtree_before_filter_hook', export_settings, vtree)

    # Now, we can filter tree if needed
    vtree.filter()

    if export_settings['gltf_flatten_obj_hierarchy'] is True:
        vtree.break_obj_hierarchy()

    # Now we filtered the tree, in case of Collection Export,
    # We need to calculate the collection center,
    # In order to set the scene center to the collection center
    # Using object center barycenter for now (another option could be to use bounding box center)
    if export_settings['gltf_collection'] and export_settings['gltf_at_collection_center']:
        vtree.calculate_collection_center()

    vtree.variants_reset_to_original()

    export_user_extensions('vtree_after_filter_hook', export_settings, vtree)

    export_settings['vtree'] = vtree

    for root in [vtree.nodes[uuid] for uuid in vtree.roots]:
        node = gltf2_blender_gather_nodes.gather_node(root, export_settings)
        if node is not None:
            scene.nodes.append(node)

    export_user_extensions('gather_scene_hook', export_settings, scene, blender_scene)

    return scene


def __gather_extras(blender_object, export_settings):
    if export_settings['gltf_extras']:
        # If case of collection export, use custom properties of the collection instead of the scene
        # So Collection custom properties are exported as glTF Scene extras
        if export_settings['gltf_collection']:
            return generate_extras(bpy.data.collections[export_settings['gltf_collection']])
        return generate_extras(blender_object)
    return None


def __gather_name(blender_scene, export_settings):
    if export_settings['gltf_collection']:
        return export_settings['gltf_collection']
    return blender_scene.name
