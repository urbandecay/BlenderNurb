# SPDX-FileCopyrightText: 2018-2021 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import bpy
from mathutils import Matrix, Quaternion, Vector

from ...io.com import gltf2_io
from ...io.com import gltf2_io_extensions
from ...io.exp.user_extensions import export_user_extensions
from ..com.extras import generate_extras, BLACK_LIST
from ..com.blender_default import LIGHTS
from ..com import gltf2_blender_math
from . import cameras as gltf2_blender_gather_cameras
from . import obj_data as gltf2_blender_gather_mesh
from . import lights as gltf2_blender_gather_lights
from .tree import VExportNode

# In this file, 'mesh' refers to glTF2 Mesh
# blender_data or data refers to any object.data
# blender_mesh is used when we specifically refer to Mesh data


def gather_node(vnode, export_settings):

    blender_object = vnode.blender_object

    # Hook to check if we should export mesh or not (force it to None)

    class GltfHookNodeMesh:
        def __init__(self):
            self.export_mesh = True

    gltf_hook_node_mesh = GltfHookNodeMesh()

    export_user_extensions('gather_node_mesh_hook', export_settings, gltf_hook_node_mesh, blender_object)
    if gltf_hook_node_mesh.export_mesh is True:
        mesh = __gather_mesh(vnode, blender_object, export_settings)
    else:
        mesh = None

    node = gltf2_io.Node(
        camera=__gather_camera(vnode, export_settings),
        children=__gather_children(vnode, export_settings),
        extensions=__gather_extensions(vnode, export_settings),
        extras=__gather_extras(blender_object, export_settings),
        matrix=__gather_matrix(blender_object, export_settings),
        mesh=mesh,
        name=__gather_name(blender_object, export_settings),
        rotation=None,
        scale=None,
        translation=None,
        weights=__gather_weights(blender_object, export_settings)
    )

    node.translation, node.rotation, node.scale = __gather_trans_rot_scale(vnode, export_settings)

    export_user_extensions('gather_node_hook', export_settings, node, blender_object)

    vnode.node = node

    return node


def __gather_camera(vnode, export_settings):
    if not vnode.blender_object:
        return
    if vnode.blender_type == VExportNode.COLLECTION:
        return None
    if vnode.blender_object.type != 'CAMERA':
        return None

    cam = gltf2_blender_gather_cameras.gather_camera(vnode.blender_object.data, export_settings)

    if len(export_settings['current_paths']) > 0:
        export_settings['KHR_animation_pointer']['cameras'][id(vnode.blender_object.data)] = {}
        export_settings['KHR_animation_pointer']['cameras'][id(
            vnode.blender_object.data)]['paths'] = export_settings['current_paths'].copy()
        export_settings['KHR_animation_pointer']['cameras'][id(vnode.blender_object.data)]['glTF_camera'] = cam

    export_settings['current_paths'] = {}

    return cam


def __gather_children(vnode, export_settings):
    children = []

    vtree = export_settings['vtree']

    for child_uuid in vnode.children:
        node = gather_node(vtree.nodes[child_uuid], export_settings)
        if node is not None:
            children.append(node)

    return children


def __gather_extensions(vnode, export_settings):
    blender_object = vnode.blender_object
    extensions = {}

    blender_lamp = None
    if vnode.blender_type == VExportNode.COLLECTION:
        return None

    if export_settings["gltf_lights"] and vnode.blender_type == VExportNode.INSTANCE and vnode.data is not None:
        if vnode.data.id_type in LIGHTS:
            blender_lamp = vnode.data
    elif export_settings["gltf_lights"] and blender_object is not None and (blender_object.type == "LAMP" or blender_object.type == "LIGHT"):
        blender_lamp = blender_object.data

    if blender_lamp is not None:
        world_matrix = blender_object.matrix_world.copy().freeze()
        light = gltf2_blender_gather_lights.gather_lights_punctual(
            blender_lamp,
            world_matrix,
            export_settings
        )
        if light is not None:
            light_extension = gltf2_io_extensions.ChildOfRootExtension(
                name="KHR_lights_punctual",
                path=["lights"],
                extension=light
            )
            extensions["KHR_lights_punctual"] = gltf2_io_extensions.Extension(
                name="KHR_lights_punctual",
                extension={
                    "light": light_extension
                }
            )
            if len(export_settings['current_paths']) > 0:
                export_settings['KHR_animation_pointer']['lights'][id(blender_lamp)] = {}
                export_settings['KHR_animation_pointer']['lights'][id(
                    blender_lamp)]['paths'] = export_settings['current_paths'].copy()
                export_settings['KHR_animation_pointer']['lights'][id(blender_lamp)]['glTF_light'] = light_extension

            export_settings['current_paths'] = {}

    return extensions if extensions else None


def __gather_extras(blender_object, export_settings):
    if export_settings['gltf_extras']:
        return generate_extras(blender_object)
    return None


def __gather_matrix(blender_object, export_settings):
    # return blender_object.matrix_local
    return []


def __gather_mesh(vnode, blender_object, export_settings):
    if vnode.blender_type == VExportNode.COLLECTION:
        return None
    if blender_object and blender_object.type in ['CURVE', 'SURFACE', 'FONT']:
        return __gather_mesh_from_blender_nonmesh(blender_object, export_settings)
    if blender_object is None and type(vnode.data).__name__ not in ["Mesh", "PointCloud"]:
        return None  # TODO
    if blender_object is None:
        # GN instance
        blender_data = vnode.data
        # Keep materials from the tmp data, but if no material, keep from object
        materials = tuple(mat for mat in blender_data.materials)
        __keep_material_info(materials, False, export_settings)
        if len(materials) == 1 and materials[0] is None:
            materials = tuple(ms.material for ms in vnode.original_object.material_slots)
            __keep_material_info(materials, True, export_settings)

        modifiers = None

        if blender_data is None:
            return None

    else:
        if blender_object.type not in ["MESH", "POINTCLOUD"]:
            return None
        # For duplis instancer, when show is off -> export as empty
        if vnode.force_as_empty is True:
            return None
        # Be sure that object is valid (no NaN for example)
        if blender_object.type == "MESH":
            res = blender_object.data.validate()
            if res is True:
                export_settings['log'].warning("Mesh " + blender_object.data.name +
                                               " is not valid, and may be exported wrongly")

        modifiers = blender_object.modifiers
        if len(modifiers) == 0:
            modifiers = None

        if export_settings['gltf_apply']:
            if modifiers is None:  # If no modifier, use original data, it will instance all shared data in a single glTF mesh
                blender_data = blender_object.data
                # Keep materials from object, as no modifiers are applied, so no risk that
                # modifiers changed them
                materials = tuple(ms.material for ms in blender_object.material_slots)
                __keep_material_info(materials, True, export_settings)
            else:
                depsgraph = bpy.context.evaluated_depsgraph_get()
                blender_data_owner = blender_object.evaluated_get(depsgraph)
                blender_data = blender_data_owner.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
                # Seems now (from 4.2) the custom properties are Statically Typed
                # so no need to copy them in that case, because overwriting them will crash
                if len(blender_data.keys()) == 0:
                    # Copy custom properties
                    for prop in [p for p in blender_object.data.keys() if (
                            (p not in BLACK_LIST) or p.startswith("gltf"))]:
                        blender_data[prop] = blender_object.data[prop]
                else:
                    # But we need to remove some properties that are not needed
                    for prop in [p for p in blender_object.data.keys() if (
                            p in BLACK_LIST and not p.startswith("gltf"))]:
                        del blender_data[prop]
                # Store the fact that this evaluated data has been created by the
                # exporter, and is not a GN instance data
                blender_data['gltf2_mesh_applied'] = True

                # Keep materials from the newly created tmp data, but if no materials, keep from object
                materials = tuple(mat for mat in blender_data.materials)
                # We need to store link between the original material and the tmp data material,
                # Because of animation pointer NLA that will still have the original
                __keep_material_info(materials, False, export_settings)
                if len(materials) == 1 and materials[0] is None:
                    materials = tuple(ms.material for ms in blender_object.material_slots)
                    __keep_material_info(materials, True, export_settings)

        else:
            blender_data = blender_object.data
            # Keep materials from object, as no modifiers are applied, so no risk that
            # modifiers changed them
            materials = tuple(ms.material for ms in blender_object.material_slots)
            __keep_material_info(materials, True, export_settings)

    result = gltf2_blender_gather_mesh.gather_mesh(blender_data,
                                                   blender_object,
                                                   modifiers,
                                                   materials,
                                                   None,
                                                   export_settings)

    if export_settings['gltf_apply'] and modifiers is not None:
        blender_data_owner.to_mesh_clear()
    return result


def __keep_material_info(materials, originals, export_settings):
    for m in [m for m in materials if m is not None]:
        if 'material_identifiers' not in export_settings.keys():
            export_settings['material_identifiers'] = {}
        if originals is True:
            export_settings['material_identifiers'][id(m)] = m
        else:
            export_settings['material_identifiers'][id(m)] = m.original


def __gather_mesh_from_blender_nonmesh(blender_object, export_settings):
    """Handles curves, surfaces, text, etc."""
    needs_to_mesh_clear = False
    try:
        # Convert to a mesh
        try:
            if export_settings['gltf_apply']:
                depsgraph = bpy.context.evaluated_depsgraph_get()
                blender_data_owner = blender_object.evaluated_get(depsgraph)
                blender_mesh = blender_data_owner.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
                # TODO: do we need preserve_all_data_layers?

            else:
                blender_data_owner = blender_object
                blender_mesh = blender_data_owner.to_mesh()

            # In some cases (for example curve with single vertice), no blender_mesh is created (without crash)
            if blender_mesh is None:
                return None

        except Exception:
            return None

        needs_to_mesh_clear = True

        materials = tuple([ms.material for ms in blender_object.material_slots if ms.material is not None])
        modifiers = None
        result = gltf2_blender_gather_mesh.gather_mesh(blender_mesh,
                                                       blender_object,
                                                       modifiers,
                                                       materials,
                                                       blender_object.data,
                                                       export_settings)

    finally:
        if needs_to_mesh_clear:
            blender_data_owner.to_mesh_clear()

    return result


def __gather_name(blender_object, export_settings):

    new_name = blender_object.name if blender_object else "GN Instance"

    class GltfHookName:
        def __init__(self, name):
            self.name = name
    gltf_hook_name = GltfHookName(new_name)

    export_user_extensions('gather_node_name_hook', export_settings, gltf_hook_name, blender_object)
    return gltf_hook_name.name


def __gather_trans_rot_scale(vnode, export_settings):
    if vnode.parent_uuid is None:
        # No parent, so matrix is world matrix, except if we export a collection
        if export_settings['gltf_collection'] and export_settings['gltf_at_collection_center']:
            # If collection, we need to take into account the collection offset
            trans, rot, sca = vnode.matrix_world.decompose()
            trans -= export_settings['gltf_collection_center']
        else:
            # No parent, so matrix is world matrix
            trans, rot, sca = vnode.matrix_world.decompose()
    else:
        # calculate local matrix
        trans, rot, sca = (
            export_settings['vtree'].nodes[vnode.parent_uuid].matrix_world.inverted_safe() @ vnode.matrix_world).decompose()

    # make sure the rotation is normalized
    rot.normalize()

    trans = __convert_swizzle_location(trans, export_settings)
    rot = __convert_swizzle_rotation(rot, export_settings)
    sca = __convert_swizzle_scale(sca, export_settings)

    translation, rotation, scale = (None, None, None)
    trans[0], trans[1], trans[2] = gltf2_blender_math.round_if_near(
        trans[0], 0.0), gltf2_blender_math.round_if_near(
        trans[1], 0.0), gltf2_blender_math.round_if_near(
            trans[2], 0.0)
    rot[0], rot[1], rot[2], rot[3] = gltf2_blender_math.round_if_near(
        rot[0], 1.0), gltf2_blender_math.round_if_near(
        rot[1], 0.0), gltf2_blender_math.round_if_near(
            rot[2], 0.0), gltf2_blender_math.round_if_near(
                rot[3], 0.0)
    sca[0], sca[1], sca[2] = gltf2_blender_math.round_if_near(
        sca[0], 1.0), gltf2_blender_math.round_if_near(
        sca[1], 1.0), gltf2_blender_math.round_if_near(
            sca[2], 1.0)
    if trans[0] != 0.0 or trans[1] != 0.0 or trans[2] != 0.0:
        translation = [trans[0], trans[1], trans[2]]
    if rot[0] != 1.0 or rot[1] != 0.0 or rot[2] != 0.0 or rot[3] != 0.0:
        rotation = [rot[1], rot[2], rot[3], rot[0]]
    if sca[0] != 1.0 or sca[1] != 1.0 or sca[2] != 1.0:
        scale = [sca[0], sca[1], sca[2]]
    return translation, rotation, scale


def __gather_weights(blender_object, export_settings):
    return None


def __convert_swizzle_location(loc, export_settings):
    """Convert a location from Blender coordinate system to glTF coordinate system."""
    if export_settings['gltf_yup']:
        return Vector((loc[0], loc[2], -loc[1]))
    else:
        return Vector((loc[0], loc[1], loc[2]))


def __convert_swizzle_rotation(rot, export_settings):
    """
    Convert a quaternion rotation from Blender coordinate system to glTF coordinate system.

    'w' is still at first position.
    """
    if export_settings['gltf_yup']:
        return Quaternion((rot[0], rot[1], rot[3], -rot[2]))
    else:
        return Quaternion((rot[0], rot[1], rot[2], rot[3]))


def __convert_swizzle_scale(scale, export_settings):
    """Convert a scale from Blender coordinate system to glTF coordinate system."""
    if export_settings['gltf_yup']:
        return Vector((scale[0], scale[2], scale[1]))
    else:
        return Vector((scale[0], scale[1], scale[2]))
