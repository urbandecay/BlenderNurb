# SPDX-FileCopyrightText: 2018-2021 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import bpy
import numpy as np
from ...io.imp.user_extensions import import_user_extensions
from ...io.imp.gltf2_io_binary import BinaryData
from ...io.com.constants import DataType, ComponentType
from ...blender.com.conversion import get_attribute_type
from ..com.extras import set_extras
from ..com.gltf2_blender_utils import fast_structured_np_unique
from .material import BlenderMaterial
from .draco_compression_extension import decode_primitive


class BlenderMesh():
    """Blender Mesh."""
    def __new__(cls, *args, **kwargs):
        raise RuntimeError("%s should not be instantiated" % cls)

    @staticmethod
    def create(gltf, mesh_idx):
        """Mesh creation."""
        return create_mesh(gltf, mesh_idx)

    @staticmethod
    def create_pointcloud(gltf, mesh_idx):
        """Point Cloud creation."""
        return create_pointcloud(gltf, mesh_idx)


# Maximum number of TEXCOORD_n/COLOR_n sets to import
UV_MAX = 8
COLOR_MAX = 8


def create_mesh(gltf, mesh_idx):
    pymesh = gltf.data.meshes[mesh_idx]

    import_user_extensions('gather_import_mesh_before_hook', gltf, pymesh)

    name = pymesh.name or 'Mesh_%d' % mesh_idx
    mesh = bpy.data.meshes.new(name)

    # Temporarily parent the mesh to an object.
    # This is used to construct shape keys.
    tmp_ob = None
    try:
        tmp_ob = bpy.data.objects.new('##gltf-import:tmp-object##', mesh)
        do_primitives(gltf, mesh_idx, mesh, tmp_ob)
        set_extras(mesh, gltf.data.meshes[mesh_idx].extras, exclude=['targetNames'])

    finally:
        if tmp_ob:
            bpy.data.objects.remove(tmp_ob)

    import_user_extensions('gather_import_mesh_after_hook', gltf, pymesh, mesh)

    return mesh


def create_pointcloud(gltf, mesh_idx):
    pypc = gltf.data.meshes[mesh_idx]
    import_user_extensions('gather_import_pointcloud_before_hook', gltf, pypc)

    name = pypc.name or 'PointCloud_%d' % mesh_idx
    pointcloud = bpy.data.pointclouds.new(name)

    # Point clouds do not have shape keys.
    do_primitives_pointcloud(gltf, mesh_idx, pointcloud)
    set_extras(pointcloud, gltf.data.meshes[mesh_idx].extras)

    import_user_extensions('gather_import_pointcloud_after_hook', gltf, pypc, pointcloud)
    return pointcloud


def do_primitives_pointcloud(gltf, mesh_idx, pointcloud):
    pypc = gltf.data.meshes[mesh_idx]

    point_locs = np.empty(dtype=np.float32, shape=(0, 3))  # coordinate for each point
    attributes = {}
    attribute_type = {}
    attribute_component_type = {}
    attribute_data_type = {}

    for prim in pypc.primitives:
        if 'POSITION' not in prim.attributes:
            continue

        if prim.extensions is not None and 'KHR_draco_mesh_compression' in prim.extensions:
            gltf.log.info('Draco Decoder: Decode primitive {}'.format(pypc.name or '[unnamed]'))
            decode_primitive(gltf, prim)

        if prim.mode != 0:
            gltf.log.warning(
                'Point Cloud import: Primitive mode is not POINTS, skipping primitive in mesh {}'.format(
                    pypc.name or '[unnamed]'))
            continue

        if prim.indices is not None:
            indices = BinaryData.decode_accessor(gltf, prim.indices)
            indices = indices.reshape(len(indices))
        else:
            num_points = gltf.data.accessors[prim.attributes['POSITION']].count
            indices = np.arange(0, num_points, dtype=np.uint32)

        # We'll add one vert to the arrays for each index used in indices
        unique_indices, inv_indices = np.unique(indices, return_inverse=True)

        vs = BinaryData.decode_accessor(gltf, prim.attributes['POSITION'], cache=True)
        point_locs = np.concatenate((point_locs, vs[unique_indices]))

        # Custom Attributes for this primitive
        custom_attrs = [k for k in prim.attributes if k.startswith('_') or k.startswith('KHR_')]
        for attr in custom_attrs:
            if attr not in attributes:  # This attribute is not yet known
                # So set it up
                attribute_type[attr] = gltf.data.accessors[prim.attributes[attr]].type
                attribute_component_type[attr] = gltf.data.accessors[prim.attributes[attr]].component_type
                # Initialize with empty data for all previous primitives
                attributes[attr] = np.zeros(
                    dtype=ComponentType.to_numpy_dtype(attribute_component_type[attr]),
                    shape=(len(point_locs) - len(vs[unique_indices]), DataType.num_elements(attribute_type[attr]))
                )
                attribute_data_type[attr] = get_attribute_type(
                    gltf.data.accessors[prim.attributes[attr]].component_type,
                    gltf.data.accessors[prim.attributes[attr]].type
                )
        for idx, attr in enumerate(attributes.keys()):
            if attr in prim.attributes:
                attr_data = BinaryData.decode_accessor(gltf, prim.attributes[attr], cache=True)
                attributes[attr] = np.concatenate((attributes[attr], attr_data[unique_indices]))
            else:
                # Setup default data for attribute not defined in this primitive
                attr_data = np.zeros(
                    (len(unique_indices), DataType.num_elements(attribute_type[attr])),
                    dtype=ComponentType.to_numpy_dtype(attribute_component_type[attr])
                )
                attributes[attr] = np.concatenate((attributes[attr], attr_data))

    pointcloud.resize(point_locs.shape[0])  # Add points to the point cloud

    # Setup positions
    gltf.locs_batch_gltf_to_blender(point_locs)
    position_attribute = attribute_ensure(pointcloud.attributes, 'position', 'FLOAT_VECTOR', 'POINT')
    position_attribute.data.foreach_set('vector', squish(point_locs, np.float32))

    # Custom Attributes

    for attr in attributes:
        blender_attribute_data_type = attribute_data_type[attr]

        if blender_attribute_data_type is None:
            continue

        blender_attribute = pointcloud.attributes.new(attr, blender_attribute_data_type, 'POINT')
        if DataType.num_elements(attribute_type[attr]) == 1:
            blender_attribute.data.foreach_set('value', attributes[attr].flatten())
        elif DataType.num_elements(gltf.data.accessors[prim.attributes[attr]].type) > 1:
            if blender_attribute_data_type in ["BYTE_COLOR", "FLOAT_COLOR"]:
                blender_attribute.data.foreach_set('color', attributes[attr].flatten())
            else:
                blender_attribute.data.foreach_set('vector', attributes[attr].flatten())


def do_primitives(gltf, mesh_idx, mesh, ob):
    """Put all primitive data into the mesh."""
    pymesh = gltf.data.meshes[mesh_idx]

    # Scan the primitives to find out what we need to create

    has_normals = False
    num_uvs = 0
    num_cols = 0
    attributes = {}
    attribute_data = []
    attribute_type = {}
    attribute_component_type = {}

    for prim in pymesh.primitives:
        if 'POSITION' not in prim.attributes:
            continue

        if gltf.import_settings['import_shading'] == "NORMALS":
            if 'NORMAL' in prim.attributes:
                has_normals = True

        i = 0
        while i < UV_MAX and ('TEXCOORD_%d' % i) in prim.attributes:
            i += 1
        num_uvs = max(i, num_uvs)

        i = 0
        while i < COLOR_MAX and ('COLOR_%d' % i) in prim.attributes:
            i += 1
        num_cols = max(i, num_cols)

        custom_attrs = [k for k in prim.attributes if k.startswith('_') or k.startswith('KHR_')]
        for attr in custom_attrs:
            if attr not in attributes:
                attribute_type[attr] = gltf.data.accessors[prim.attributes[attr]].type
                attribute_component_type[attr] = gltf.data.accessors[prim.attributes[attr]].component_type
                attribute_data.append(
                    np.empty(
                        dtype=ComponentType.to_numpy_dtype(attribute_component_type[attr]),
                        shape=(0, DataType.num_elements(attribute_type[attr])))
                )
        # Make sure all attributes are in the dict, even those not in the first primitive(s)
        # And make sure the order of attributes is the same for all primitives
        attributes.update(dict.fromkeys(custom_attrs))

    num_shapekeys = sum(sk_name is not None for sk_name in pymesh.shapekey_names)

    # -------------
    # We'll process all the primitives gathering arrays to feed into the
    # various foreach_set function that create the mesh data.

    num_faces = 0  # total number of faces
    vert_locs = np.empty(dtype=np.float32, shape=(0, 3))  # coordinate for each vert
    vert_normals = np.empty(dtype=np.float32, shape=(0, 3))  # normal for each vert
    edge_vidxs = np.array([], dtype=np.uint32)  # vertex_index for each loose edge
    loop_vidxs = np.array([], dtype=np.uint32)  # vertex_index for each loop
    loop_uvs = [
        np.empty(dtype=np.float32, shape=(0, 2))  # UV for each loop for each layer
        for _ in range(num_uvs)
    ]
    loop_cols = [
        np.empty(dtype=np.float32, shape=(0, 4))  # color for each loop for each layer
        for _ in range(num_cols)
    ]
    sk_vert_locs = [
        np.empty(dtype=np.float32, shape=(0, 3))  # coordinate for each vert for each shapekey
        for _ in range(num_shapekeys)
    ]

    # We need to detect if some non-tri primitives have some VC.
    # (Because, in that case, we will need to create vertex domain VC, instead of corner domain VC)
    has_non_tri_vcs = []
    vc_domains = []
    for i in range(num_cols):
        has_non_tri_vcs.append(any([prim.mode not in [None, 4, 5, 6] and any(
            ['COLOR_' + str(i) in attr for attr in prim.attributes]) for prim in pymesh.primitives]))
        vc_domains.append('POINT' if has_non_tri_vcs[i] else 'CORNER')

    for prim in pymesh.primitives:
        prim.num_faces = 0

        if 'POSITION' not in prim.attributes:
            continue

        vert_index_base = len(vert_locs)

        if prim.extensions is not None and 'KHR_draco_mesh_compression' in prim.extensions:

            gltf.log.info('Draco Decoder: Decode primitive {}'.format(pymesh.name or '[unnamed]'))
            decode_primitive(gltf, prim)

        import_user_extensions('gather_import_decode_primitive', gltf, pymesh, prim)

        if prim.indices is not None:
            indices = BinaryData.decode_accessor(gltf, prim.indices)
            indices = indices.reshape(len(indices))
        else:
            num_verts = gltf.data.accessors[prim.attributes['POSITION']].count
            indices = np.arange(0, num_verts, dtype=np.uint32)

        mode = 4 if prim.mode is None else prim.mode
        points, edges, tris = points_edges_tris(mode, indices)
        if points is not None:
            indices = points
        elif edges is not None:
            indices = edges
        else:
            indices = tris

        # We'll add one vert to the arrays for each index used in indices
        unique_indices, inv_indices = np.unique(indices, return_inverse=True)

        vs = BinaryData.decode_accessor(gltf, prim.attributes['POSITION'], cache=True)
        vert_locs = np.concatenate((vert_locs, vs[unique_indices]))

        if has_normals:
            if 'NORMAL' in prim.attributes:
                ns = BinaryData.decode_accessor(gltf, prim.attributes['NORMAL'], cache=True)
                ns = ns[unique_indices]
            else:
                ns = np.zeros((len(unique_indices), 3), dtype=np.float32)
            vert_normals = np.concatenate((vert_normals, ns))

        sk_i = 0
        for sk, sk_name in enumerate(pymesh.shapekey_names):
            if sk_name is None:
                continue
            if prim.targets and 'POSITION' in prim.targets[sk]:
                morph_vs = BinaryData.decode_accessor(gltf, prim.targets[sk]['POSITION'], cache=True)
                morph_vs = morph_vs[unique_indices]
            else:
                morph_vs = np.zeros((len(unique_indices), 3), dtype=np.float32)
            sk_vert_locs[sk_i] = np.concatenate((sk_vert_locs[sk_i], morph_vs))
            sk_i += 1

        # inv_indices are the indices into the verts just for this prim;
        # calculate indices into the overall verts array
        prim_vidxs = inv_indices.astype(np.uint32, copy=False)
        prim_vidxs += vert_index_base  # offset for verts from previous prims

        if edges is not None:
            edge_vidxs = np.concatenate((edge_vidxs, prim_vidxs))

        if tris is not None:
            prim.num_faces = len(indices) // 3
            num_faces += prim.num_faces

            loop_vidxs = np.concatenate((loop_vidxs, prim_vidxs))

            # UV only if we have a face
            for uv_i in range(num_uvs):
                if ('TEXCOORD_%d' % uv_i) in prim.attributes:
                    uvs = BinaryData.decode_accessor(gltf, prim.attributes['TEXCOORD_%d' % uv_i], cache=True)
                    uvs = uvs[indices]
                else:
                    uvs = np.zeros((len(indices), 2), dtype=np.float32)
                loop_uvs[uv_i] = np.concatenate((loop_uvs[uv_i], uvs))

        # We can have VC for points, lines, and tris
        for col_i in range(num_cols):
            if ('COLOR_%d' % col_i) in prim.attributes:
                cols = BinaryData.decode_accessor(gltf, prim.attributes['COLOR_%d' % col_i], cache=True)
                if vc_domains[col_i] == 'CORNER':
                    cols = cols[indices]
                else:
                    cols = cols[unique_indices]
                if cols.shape[1] == 3:
                    cols = colors_rgb_to_rgba(cols)
            else:
                if vc_domains[col_i] == 'CORNER':
                    cols = np.ones((len(indices), 4), dtype=np.float32)
                else:
                    cols = np.ones((len(unique_indices), 4), dtype=np.float32)
            loop_cols[col_i] = np.concatenate((loop_cols[col_i], cols))

        for idx, attr in enumerate(attributes):
            if attr in prim.attributes:
                attr_data = BinaryData.decode_accessor(gltf, prim.attributes[attr], cache=True)
                attribute_data[idx] = np.concatenate((attribute_data[idx], attr_data[unique_indices]))
            else:
                attr_data = np.zeros(
                    (len(unique_indices), DataType.num_elements(attribute_type[attr])),
                    dtype=ComponentType.to_numpy_dtype(attribute_component_type[attr])
                )
                attribute_data[idx] = np.concatenate((attribute_data[idx], attr_data))

    # Accessors are cached in case they are shared between primitives; clear
    # the cache now that all prims are done.
    gltf.decode_accessor_cache = {}

    if gltf.import_settings['merge_vertices']:
        vert_locs, vert_normals, sk_vert_locs, loop_vidxs, edge_vidxs, attribute_data = \
            merge_duplicate_verts(
                vert_locs, vert_normals, sk_vert_locs, loop_vidxs, edge_vidxs, attribute_data
            )

    # ---------------
    # Convert all the arrays glTF -> Blender

    # Change from relative to absolute positions for morph locs
    for sk_locs in sk_vert_locs:
        sk_locs += vert_locs

    gltf.locs_batch_gltf_to_blender(vert_locs)
    gltf.normals_batch_gltf_to_blender(vert_normals)
    for sk_locs in sk_vert_locs:
        gltf.locs_batch_gltf_to_blender(sk_locs)

    for uvs in loop_uvs:
        uvs_gltf_to_blender(uvs)

    # ---------------
    # Start creating things

    mesh.vertices.add(len(vert_locs))
    position_attribute = attribute_ensure(mesh.attributes, 'position', 'FLOAT_VECTOR', 'POINT')
    position_attribute.data.foreach_set('vector', squish(vert_locs, np.float32))

    mesh.loops.add(len(loop_vidxs))
    corner_vert_attribute = attribute_ensure(mesh.attributes, '.corner_vert', 'INT', 'CORNER')
    corner_vert_attribute.data.foreach_set('value', squish(loop_vidxs, np.intc))

    mesh.edges.add(len(edge_vidxs) // 2)
    edge_verts_attribute = attribute_ensure(mesh.attributes, '.edge_verts', 'INT32_2D', 'EDGE')
    edge_verts_attribute.data.foreach_set('value', squish(edge_vidxs, np.intc))

    mesh.polygons.add(num_faces)

    # All polys are tris
    loop_starts = np.arange(0, 3 * num_faces, step=3)
    mesh.polygons.foreach_set('loop_start', loop_starts)

    for uv_i in range(num_uvs):
        name = 'UVMap' if uv_i == 0 else 'UVMap.%03d' % uv_i
        layer = mesh.uv_layers.new(name=name)

        if layer is None:
            gltf.log.warning("WARNING: UV map is ignored because the maximum number of UV layers has been reached.")
            break

        layer.uv.foreach_set('vector', squish(loop_uvs[uv_i], np.float32))

    for col_i in range(num_cols):
        name = 'Color' if col_i == 0 else 'Color.%03d' % col_i
        layer = mesh.color_attributes.new(name, 'BYTE_COLOR', vc_domains[col_i])

        layer.data.foreach_set('color', squish(loop_cols[col_i], np.float32))

    # Make sure the first Vertex Color Attribute is the rendered one
    if num_cols > 0:
        mesh.color_attributes.render_color_index = 0

    # Shapekeys
    if num_shapekeys:
        ob.shape_key_add(name='Basis')
        mesh.shape_keys.name = mesh.name

        sk_i = 0
        for sk_name in pymesh.shapekey_names:
            if sk_name is None:
                continue

            key_block = ob.shape_key_add(name=sk_name)
            key_block.value = 0.0  # Starting Blender 5.0, SK are created with weight 1.0, so setting to 0.0, as 0.0 is the default glTF SK weight
            key_block.points.foreach_set('co', squish(sk_vert_locs[sk_i], np.float32))

            sk_i += 1

    # ----
    # Assign materials to faces
    has_materials = any(prim.material is not None for prim in pymesh.primitives)
    # Even if no primitive have material, we need to create slots if some primitives have some variant
    if has_materials is False:
        has_materials = any(prim.extensions is not None and 'KHR_materials_variants' in prim.extensions.keys()
                            for prim in pymesh.primitives)

    # Even if there is no material, some primitives can have some Vertex Color
    # (and we need to create a slot for them, with a default material + vertex color)
    if has_materials is False:
        has_materials = any(prim.attributes.get('COLOR_0') is not None for prim in pymesh.primitives)

    default_materials = {}  # Store VC => index
    we_can_merge_slots = gltf.import_settings['import_merge_material_slots']

    if has_materials is True:
        bl_material_index_dtype = np.intc
        material_indices = np.empty(num_faces, dtype=bl_material_index_dtype)
        empty_material_slot_index = None
        f = 0

        for idx_prim, prim in enumerate(pymesh.primitives):

            has_variant = prim.extensions is not None and 'KHR_materials_variants' in prim.extensions.keys() \
                and 'mappings' in prim.extensions['KHR_materials_variants'].keys()

            if prim.material is not None:
                # Get the material
                pymaterial = gltf.data.materials[prim.material]
                vertex_color = 'COLOR_0' if ('COLOR_0' in prim.attributes) else None
                if vertex_color not in pymaterial.blender_material:
                    BlenderMaterial.create(gltf, prim.material, vertex_color)
                material_name = pymaterial.blender_material[vertex_color]

                # Put material in slot (if not there)
                # So we are going to use the existing slot if exists, or create a new one
                if we_can_merge_slots is True and not has_variant:
                    if material_name not in mesh.materials:
                        mesh.materials.append(bpy.data.materials[material_name])
                    material_index = mesh.materials.find(material_name)
                else:
                    # In case of variant, do not merge slots // or if user does not want to merge slots
                    # So we are going to create a new slot
                    mesh.materials.append(bpy.data.materials[material_name])
                    material_index = len(mesh.materials) - 1
            else:
                # Check if the primitive has a vertex color
                vertex_color = 'COLOR_0' if ('COLOR_0' in prim.attributes) else None
                if vertex_color is not None:
                    if we_can_merge_slots is True and not has_variant:
                        # Check if we already have a slot for default material + this vertec color
                        if vertex_color in default_materials.keys():
                            material_index = default_materials[vertex_color]
                        else:
                            # Create a new slot for default material + this vertec color
                            name = BlenderMaterial.create(gltf, None, vertex_color)
                            mesh.materials.append(bpy.data.materials[name])
                            material_index = len(mesh.materials) - 1
                            default_materials[vertex_color] = material_index
                    else:
                        # In case of variant, do not merge slots // or if user does not want to merge slots
                        # So we are going to create a new slot if not exists already
                        # Else, create a new slot, but using the existing material
                        if vertex_color in default_materials.keys():
                            material_index = default_materials[vertex_color]
                            mesh.materials.append(bpy.data.materials[mesh.materials[material_index].name])
                            material_index = len(mesh.materials) - 1
                        else:
                            name = BlenderMaterial.create(gltf, None, vertex_color)
                            mesh.materials.append(bpy.data.materials[name])
                            material_index = len(mesh.materials) - 1
                            default_materials[vertex_color] = material_index
                else:
                    if we_can_merge_slots is True and not has_variant:
                        # Create an empty slot if not exists already, or use the existing one
                        if empty_material_slot_index is None:
                            mesh.materials.append(None)
                            empty_material_slot_index = len(mesh.materials) - 1
                        material_index = empty_material_slot_index
                    else:
                        # In case of variant, do not merge slots // or if user does not want to merge slots
                        # So we are going to create a new slot
                        mesh.materials.append(None)
                        material_index = len(mesh.materials) - 1

            material_indices[f:f + prim.num_faces].fill(material_index)

            f += prim.num_faces

            # Manage variants
            if has_variant:

                # Store default material
                default_mat = mesh.gltf2_variant_default_materials.add()
                default_mat.material_slot_index = material_index
                default_mat.default_material = bpy.data.materials[material_name] if prim.material is not None else None

                for mapping in prim.extensions['KHR_materials_variants']['mappings']:
                    # Store, for each variant, the material link to this primitive

                    variant_primitive = mesh.gltf2_variant_mesh_data.add()
                    variant_primitive.material_slot_index = material_index
                    if 'material' not in mapping.keys():
                        # Default material
                        variant_primitive.material = None
                    else:
                        vertex_color = 'COLOR_0' if 'COLOR_0' in prim.attributes else None
                        if str(mapping['material']) + str(vertex_color) not in gltf.variant_mapping.keys():
                            BlenderMaterial.create(gltf, mapping['material'], vertex_color)
                        variant_primitive.material = gltf.variant_mapping[str(mapping['material']) + str(vertex_color)]

                    for variant in mapping['variants']:
                        vari = variant_primitive.variants.add()
                        vari.variant.variant_idx = variant

        material_index_attribute = attribute_ensure(mesh.attributes, 'material_index', 'INT', 'FACE')
        material_index_attribute.data.foreach_set('value', material_indices)

    # Custom Attributes
    for idx, attr in enumerate(attributes):

        blender_attribute_data_type = get_attribute_type(
            attribute_component_type[attr],
            attribute_type[attr]
        )

        if blender_attribute_data_type is None:
            continue

        blender_attribute = mesh.attributes.new(attr, blender_attribute_data_type, 'POINT')
        if DataType.num_elements(attribute_type[attr]) == 1:
            blender_attribute.data.foreach_set('value', attribute_data[idx].flatten())
        elif DataType.num_elements(attribute_type[attr]) > 1:
            if blender_attribute_data_type in ["BYTE_COLOR", "FLOAT_COLOR"]:
                blender_attribute.data.foreach_set('color', attribute_data[idx].flatten())
            else:
                blender_attribute.data.foreach_set('vector', attribute_data[idx].flatten())

    # ----
    # Normals

    # Set polys smooth/flat
    set_poly_smoothing(gltf, pymesh, mesh, vert_normals, loop_vidxs)
    has_loose_edges = len(edge_vidxs) != 0  # need to calc_loose_edges for them to show up
    # Make sure edges are calculated (new for Blender 5.1+)
    mesh.update(calc_edges=True, calc_edges_loose=has_loose_edges)
    mesh.validate()

    if has_normals:
        mesh.normals_split_custom_set_from_vertices(vert_normals)


def points_edges_tris(mode, indices):
    points = None
    edges = None
    tris = None

    if mode == 0:
        # POINTS
        points = indices

    elif mode == 1:
        # LINES
        #   1   3
        #  /   /
        # 0   2
        edges = indices

    elif mode == 2:
        # LINE LOOP
        #   1---2
        #  /     \
        # 0-------3
        # in:  0123
        # out: 01122330
        edges = np.empty(2 * len(indices), dtype=np.uint32)
        edges[[0, -1]] = indices[[0, 0]]  # 0______0
        edges[1:-1] = np.repeat(indices[1:], 2)  # 01122330

    elif mode == 3:
        # LINE STRIP
        #   1---2
        #  /     \
        # 0       3
        # in:  0123
        # out: 011223
        edges = np.empty(2 * len(indices) - 2, dtype=np.uint32)
        edges[[0, -1]] = indices[[0, -1]]  # 0____3
        edges[1:-1] = np.repeat(indices[1:-1], 2)  # 011223

    elif mode == 4:
        # TRIANGLES
        #   2     3
        #  / \   / \
        # 0---1 4---5
        tris = indices

    elif mode == 5:
        # TRIANGLE STRIP
        # 0---2---4
        #  \ / \ /
        #   1---3
        # in: 01234
        # out: 012132234
        # out (viewed as triplets): 012, 132, 234
        tris = np.empty((len(indices) - 2) * 3, dtype=np.uint32)
        # 012__
        first_indices = indices[:-2]
        # _123_
        second_indices = indices[1:-1]
        # __234
        third_indices = indices[2:]

        # Each triplet starts with the first index
        # 0__, 1__, 2__ <- 012__
        tris[0::3] = first_indices

        # Even triplets end with the next two indices in order
        # _1_, ___, _3_ <- _1_3_ <- _123_
        # 01_, 1__, 23_
        tris[1::6] = second_indices[0::2]
        # __2, ___, __4 <- __2_4 <- __234
        # 012, 1__, 234
        tris[2::6] = third_indices[0::2]

        # Odd triplets end with the next two indices in reverse order
        # ___, _3_, ___ <- ___3_ <- __234
        # 012, 13_, 234
        tris[4::6] = third_indices[1::2]
        # ___, __2, ___ <- __2__ <- _123_
        # 012, 132, 234
        tris[5::6] = second_indices[1::2]

    elif mode == 6:
        # TRIANGLE FAN
        #   3---2
        #  / \ / \
        # 4---0---1
        # in: 01234
        # out: 012023034
        # out (viewed as triplets): 012, 023, 034
        # Start filled with the first index
        # 000, 000, 000
        tris = np.full((len(indices) - 2) * 3, indices[0], dtype=np.uint32)
        # _1_, _2_, _3_ <- _123_
        # 010, 020, 030
        tris[1::3] = indices[1:-1]
        # __2, __3, __4 <- __234
        # 012, 023, 034
        tris[2::3] = indices[2:]

    else:
        raise Exception('primitive mode unimplemented: %d' % mode)

    return points, edges, tris


def squish(array, dtype=None):
    """Squish nD array into a C-contiguous (required for faster access with the buffer protocol in foreach_set) 1D array
    (required by foreach_set). Optionally converting the array to a different dtype."""
    return np.ascontiguousarray(array, dtype=dtype).reshape(array.size)


def colors_rgb_to_rgba(rgb):
    rgba = np.ones((len(rgb), 4), dtype=np.float32)
    rgba[:, :3] = rgb
    return rgba


def uvs_gltf_to_blender(uvs):
    # u,v -> u,1-v
    uvs[:, 1] *= -1
    uvs[:, 1] += 1


def set_poly_smoothing(gltf, pymesh, mesh, vert_normals, loop_vidxs):
    num_polys = len(mesh.polygons)

    if gltf.import_settings['import_shading'] == "FLAT":
        # Polys are smooth by default, setting to flat
        mesh.shade_flat()
        return

    if gltf.import_settings['import_shading'] == "SMOOTH":
        poly_sharps = np.full(num_polys, False)
        f = 0
        for prim in pymesh.primitives:
            if 'NORMAL' not in prim.attributes:
                # Primitives with no NORMALs should use flat shading
                poly_sharps[f:f + prim.num_faces].fill(True)
            f += prim.num_faces
        sharp_face_attribute = attribute_ensure(mesh.attributes, 'sharp_face', 'BOOLEAN', 'FACE')
        sharp_face_attribute.data.foreach_set('value', poly_sharps)
        return

    assert gltf.import_settings['import_shading'] == "NORMALS"

    # Try to guess which polys should be flat based on the fact that all the
    # loop normals for a flat poly are = the poly's normal.

    poly_sharps = np.empty(num_polys, dtype=bool)

    poly_normals = np.empty(num_polys * 3, dtype=np.float32)
    mesh.polygon_normals.foreach_get('vector', poly_normals)
    poly_normals = poly_normals.reshape(num_polys, 3)

    f = 0
    for prim in pymesh.primitives:
        if 'NORMAL' not in prim.attributes:
            # Primitives with no NORMALs should use flat shading
            poly_sharps[f:f + prim.num_faces].fill(True)
            f += prim.num_faces
            continue

        # Check the normals at the three corners against the poly normal.
        # Two normals are equal iff their dot product is 1.

        poly_ns = poly_normals[f:f + prim.num_faces]

        # Dot product against the first vertex normal in the tri
        vert_ns = vert_normals[loop_vidxs[3 * f:3 * (f + prim.num_faces):3]]
        dot_prods = np.sum(vert_ns * poly_ns, axis=1)  # dot product
        smooth = (dot_prods <= 0.9999999)

        # Same for the second vertex, etc.
        vert_ns = vert_normals[loop_vidxs[3 * f + 1:3 * (f + prim.num_faces):3]]
        dot_prods = np.sum(vert_ns * poly_ns, axis=1)
        np.logical_or(smooth, dot_prods <= 0.9999999, out=smooth)

        vert_ns = vert_normals[loop_vidxs[3 * f + 2:3 * (f + prim.num_faces):3]]
        dot_prods = np.sum(vert_ns * poly_ns, axis=1)
        np.logical_or(smooth, dot_prods <= 0.9999999, out=smooth)

        np.logical_not(smooth, out=poly_sharps[f:f + prim.num_faces])

        f += prim.num_faces

    sharp_face_attribute = attribute_ensure(mesh.attributes, 'sharp_face', 'BOOLEAN', 'FACE')
    sharp_face_attribute.data.foreach_set('value', poly_sharps)


def merge_duplicate_verts(vert_locs, vert_normals, sk_vert_locs, loop_vidxs, edge_vidxs, attribute_data):
    """Weld vertices with matching positions, normals, and morph targets."""
    if len(vert_normals) != 0:
        vert_normals *= 50000
        vert_normals[:] = np.trunc(vert_normals)
        vert_normals *= (1 / 50000)

    dot_fields = [('x', np.float32), ('y', np.float32), ('z', np.float32)]
    if len(vert_normals) != 0:
        dot_fields += [('nx', np.float32), ('ny', np.float32), ('nz', np.float32)]
    for i, _ in enumerate(sk_vert_locs):
        dot_fields += [('sk%dx' % i, np.float32), ('sk%dy' % i, np.float32), ('sk%dz' % i, np.float32)]

    dots = np.empty(len(vert_locs), dtype=np.dtype(dot_fields))
    dots['x'] = vert_locs[:, 0]
    dots['y'] = vert_locs[:, 1]
    dots['z'] = vert_locs[:, 2]
    if len(vert_normals) != 0:
        dots['nx'] = vert_normals[:, 0]
        dots['ny'] = vert_normals[:, 1]
        dots['nz'] = vert_normals[:, 2]
    for i, locs in enumerate(sk_vert_locs):
        dots['sk%dx' % i] = locs[:, 0]
        dots['sk%dy' % i] = locs[:, 1]
        dots['sk%dz' % i] = locs[:, 2]

    unique_dots, unique_ind, inv_indices = fast_structured_np_unique(dots, return_index=True, return_inverse=True)
    loop_vidxs = inv_indices[loop_vidxs]
    edge_vidxs = inv_indices[edge_vidxs]

    # Custom attributes are not part of the weld key; retain one matching value.
    for idx, data in enumerate(attribute_data):
        attribute_data[idx] = data[unique_ind]

    vert_locs = np.empty((len(unique_dots), 3), dtype=np.float32)
    vert_locs[:, 0] = unique_dots['x']
    vert_locs[:, 1] = unique_dots['y']
    vert_locs[:, 2] = unique_dots['z']
    if len(vert_normals) != 0:
        vert_normals = np.empty((len(unique_dots), 3), dtype=np.float32)
        vert_normals[:, 0] = unique_dots['nx']
        vert_normals[:, 1] = unique_dots['ny']
        vert_normals[:, 2] = unique_dots['nz']
    for i in range(len(sk_vert_locs)):
        sk_vert_locs[i] = np.empty((len(unique_dots), 3), dtype=np.float32)
        sk_vert_locs[i][:, 0] = unique_dots['sk%dx' % i]
        sk_vert_locs[i][:, 1] = unique_dots['sk%dy' % i]
        sk_vert_locs[i][:, 2] = unique_dots['sk%dz' % i]

    return vert_locs, vert_normals, sk_vert_locs, loop_vidxs, edge_vidxs, attribute_data
