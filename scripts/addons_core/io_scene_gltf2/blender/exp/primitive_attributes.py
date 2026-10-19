# SPDX-FileCopyrightText: 2018-2021 The glTF-Blender-IO authors
#
# SPDX-License-Identifier: Apache-2.0

import numpy as np

from ...io.com import gltf2_io, constants as gltf2_io_constants
from ...io.exp import binary_data as gltf2_io_binary_data
from ...io.exp.user_extensions import export_user_extensions
from .accessors import array_to_accessor


def gather_primitive_attributes(blender_primitive, export_settings):
    """
    Gathers the attributes, such as POSITION, NORMAL, TANGENT, and all custom attributes from a blender primitive

    :return: a dictionary of attributes
    """
    attributes = {}

    # Loop over each attribute extracted from the Blender mesh.
    for attribute in blender_primitive["attributes"]:
        if attribute.startswith("MORPH_"):
            continue  # Target for morphs will be managed later
        attributes.update(__gather_attribute(blender_primitive, attribute, export_settings))

    return attributes


def __gather_attribute(blender_primitive, attribute, export_settings):
    data = blender_primitive["attributes"][attribute]

    include_max_and_mins = {
        "POSITION": True
    }

    if (attribute.startswith("_") or attribute.startswith("COLOR_") or attribute.startswith("KHR_")
        ) and blender_primitive["attributes"][attribute]['component_type'] == gltf2_io_constants.ComponentType.UnsignedShort:
        # Byte Color vertex color, need to normalize

        data['data'] = np.clip(data['data'], 0, 1)
        data['data'] *= 65535
        data['data'] += 0.5  # bias for rounding
        data['data'] = data['data'].astype(np.uint16)

        export_user_extensions('gather_attribute_change', export_settings, attribute, data, True)

        return {
            attribute: gltf2_io.Accessor(
                buffer_view=gltf2_io_binary_data.BinaryData(
                    data['data'].tobytes(),
                    gltf2_io_constants.BufferViewTarget.ARRAY_BUFFER),
                byte_offset=None,
                component_type=data['component_type'],
                count=len(
                    data['data']),
                extensions=None,
                extras=None,
                max=None,
                min=None,
                name=None,
                normalized=True,
                sparse=None,
                type=data['data_type'],
            )}

    elif attribute.startswith("COLOR_") and blender_primitive["attributes"][attribute]['component_type'] == gltf2_io_constants.ComponentType.UnsignedByte:
        # We are in special case where we fake a COLOR_0 attribute with UNSIGNED_BYTE
        # We need to normalize it

        export_user_extensions('gather_attribute_change', export_settings, attribute, data, True)

        return {
            attribute: gltf2_io.Accessor(
                buffer_view=gltf2_io_binary_data.BinaryData(
                    data['data'].tobytes(),
                    gltf2_io_constants.BufferViewTarget.ARRAY_BUFFER),
                byte_offset=None,
                component_type=data['component_type'],
                count=len(
                    data['data']),
                extensions=None,
                extras=None,
                max=None,
                min=None,
                name=None,
                normalized=True,
                sparse=None,
                type=data['data_type'],
            )}

    else:

        export_user_extensions('gather_attribute_change', export_settings, attribute, data, False)

        return {
            attribute: array_to_accessor(
                attribute,
                data['data'],
                export_settings,
                component_type=data['component_type'],
                data_type=data['data_type'],
                include_max_and_min=include_max_and_mins.get(attribute, False),
                normalized=data.get('normalized')
            )
        }
