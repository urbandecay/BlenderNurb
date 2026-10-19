# SPDX-FileCopyrightText: 2018-2022 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import bpy
from ....io.com import gltf2_io
from ...com.extras import generate_extras
from ..tree import VExportNode
from .sampled.object.channels import gather_object_sampled_channels
from .sampled.shapekeys.channels import gather_sk_sampled_channels
from .sampled.data.channels import gather_data_sampled_channels
from .anim_utils import link_samplers, add_slide_data


def gather_scene_animations(export_settings):

    # Even if we don't have any animation,
    # We are going to bake.
    # Here are some cases where there are no action in bpy.data.actions, but we still have to bake:
    # - GN instances, that can be animated
    # - Everything that can be animated by drivers (animation pointer, but not only)

    total_channels = []
    animations = []

    start_frame = bpy.context.scene.frame_start
    end_frame = bpy.context.scene.frame_end

    # The following options has no impact:
    # - We force sampling & baking
    # - Export_frame_range --> Because this is the case for SCENE mode, because we bake all scene frame range
    # - CROP or SLIDE --> Scene don't have negative frames

    # This mode will bake all objects like there are in the scene
    vtree = export_settings['vtree']
    for obj_uuid in vtree.get_all_objects():

        # Do not manage not exported objects
        if vtree.nodes[obj_uuid].node is None:
            continue

        if export_settings['vtree'].nodes[obj_uuid].blender_type == VExportNode.COLLECTION:
            continue

        # blender_object can be None for GN instances
        blender_object = export_settings['vtree'].nodes[obj_uuid].blender_object

        export_settings['ranges'][obj_uuid] = {}
        export_settings['ranges'][obj_uuid][obj_uuid] = {'start': start_frame, 'end': end_frame}
        if export_settings['gltf_anim_slide_to_zero'] is True and start_frame > 0:
            add_slide_data(start_frame, obj_uuid, obj_uuid, export_settings)

        # Perform baking animation export

        if blender_object is not None:
            channels, _ = gather_object_sampled_channels(obj_uuid, obj_uuid, None, export_settings)
            if channels is not None:
                total_channels.extend(channels)

            if export_settings['gltf_morph_anim'] and blender_object.type == "MESH" \
                    and blender_object.data is not None \
                    and blender_object.data.shape_keys is not None:

                channels = gather_sk_sampled_channels(obj_uuid, obj_uuid, None, export_settings)
                if channels is not None:
                    total_channels.extend(channels)
        else:
            # This is GN instances
            # No action / slot for GN instances
            # Setting slot_identifier to None, always
            channels, _ = gather_object_sampled_channels(obj_uuid, obj_uuid, None, export_settings)
            if channels is not None:
                total_channels.extend(channels)
        if export_settings['gltf_anim_scene_split_object'] is True:
            if len(total_channels) > 0:
                animation = gltf2_io.Animation(
                    channels=total_channels,
                    extensions=None,
                    extras=__gather_extras(blender_object, export_settings),
                    name=blender_object.name if blender_object else "GN Instance",
                    samplers=[]
                )
                link_samplers(animation, export_settings)
                animations.append(animation)

            total_channels = []

    if export_settings['gltf_export_anim_pointer'] is True:
        # Export now KHR_animation_pointer for materials
        for mat in export_settings['KHR_animation_pointer']['materials'].keys():
            if len(export_settings['KHR_animation_pointer']['materials'][mat]['paths']) == 0:
                continue

            if export_settings['gltf_animation_mode'] == "NLA_TRACKS" and export_settings['gltf_apply'] is True:
                blender_material = export_settings['material_identifiers'][mat]
            else:
                blender_material = [m for m in bpy.data.materials if id(m) == mat][0]

            export_settings['ranges'][id(blender_material)] = {}
            export_settings['ranges'][id(blender_material)][id(blender_material)] = {
                'start': start_frame, 'end': end_frame}

            if export_settings['gltf_anim_slide_to_zero'] is True and start_frame > 0:
                add_slide_data(start_frame, mat, mat, export_settings)

            # Setting slot_identifier to None, always
            channels = gather_data_sampled_channels('materials', mat, mat, None, None, export_settings)
            if channels is not None:
                total_channels.extend(channels)

        if export_settings['gltf_anim_scene_split_object'] is True:
            if len(total_channels) > 0:
                animation = gltf2_io.Animation(
                    channels=total_channels,
                    extensions=None,
                    extras=__gather_extras(blender_material, export_settings),
                    name=blender_material.name,
                    samplers=[]
                )
                link_samplers(animation, export_settings)
                animations.append(animation)

            total_channels = []

        # Export now KHR_animation_pointer for lights
        for light in export_settings['KHR_animation_pointer']['lights'].keys():
            if len(export_settings['KHR_animation_pointer']['lights'][light]['paths']) == 0:
                continue

            blender_light = [alight for alight in bpy.data.lights if id(alight) == light][0]

            export_settings['ranges'][id(blender_light)] = {}
            export_settings['ranges'][id(blender_light)][id(blender_light)] = {'start': start_frame, 'end': end_frame}

            if export_settings['gltf_anim_slide_to_zero'] is True and start_frame > 0:
                add_slide_data(start_frame, light, light, export_settings)

            # Setting slot_identifier to None, always
            channels = gather_data_sampled_channels('lights', light, light, None, None, export_settings)
            if channels is not None:
                total_channels.extend(channels)

            if export_settings['gltf_anim_scene_split_object'] is True:
                if len(total_channels) > 0:
                    animation = gltf2_io.Animation(
                        channels=total_channels,
                        extensions=None,
                        extras=__gather_extras(blender_light, export_settings),
                        name=blender_light.name,
                        samplers=[]
                    )
                    link_samplers(animation, export_settings)
                    animations.append(animation)

                total_channels = []

        # Export now KHR_animation_pointer for cameras
        for cam in export_settings['KHR_animation_pointer']['cameras'].keys():
            if len(export_settings['KHR_animation_pointer']['cameras'][cam]['paths']) == 0:
                continue

            blender_camera = [camera for camera in bpy.data.cameras if id(camera) == cam][0]

            export_settings['ranges'][id(blender_camera)] = {}
            export_settings['ranges'][id(blender_camera)][id(blender_camera)] = {'start': start_frame, 'end': end_frame}

            if export_settings['gltf_anim_slide_to_zero'] is True and start_frame > 0:
                add_slide_data(start_frame, cam, cam, export_settings)

            # Setting slot_identifier to None, always
            channels = gather_data_sampled_channels('cameras', cam, cam, None, None, export_settings)
            if channels is not None:
                total_channels.extend(channels)

            if export_settings['gltf_anim_scene_split_object'] is True:
                if len(total_channels) > 0:
                    animation = gltf2_io.Animation(
                        channels=total_channels,
                        extensions=None,
                        extras=__gather_extras(blender_camera, export_settings),
                        name=blender_camera.name,
                        samplers=[]
                    )
                    link_samplers(animation, export_settings)
                    animations.append(animation)

                total_channels = []

    if export_settings['gltf_anim_scene_split_object'] is False:
        if len(total_channels) > 0:
            animation = gltf2_io.Animation(
                channels=total_channels,
                extensions=None,
                extras=__gather_extras(bpy.context.scene, export_settings),
                name=bpy.context.scene.name,
                samplers=[]
            )
            link_samplers(animation, export_settings)
            animations.append(animation)

    return animations


def __gather_extras(blender_asset, export_settings):
    if export_settings['gltf_extras']:
        return generate_extras(blender_asset)
    return None
