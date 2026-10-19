/* SPDX-FileCopyrightText: 2023 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <pxr/usd/usd/prim.h>

namespace blender {

struct Key;
struct Mesh;
struct Object;

namespace io::usd {

/**
 * Return the shape key on the given mesh object.
 *
 * \param obj: The mesh object
 * \return The shape key on the given object's mesh data, or
 *         null if the object isn't a mesh.
 */
const Key *get_mesh_shape_key(const Object *obj);

/**
 * Query whether the given object is a mesh with relative
 * shape keys.
 *
 * \param obj: The mesh object
 * \return True if the object is a mesh with shape keys, false otherwise
 */
bool is_mesh_with_shape_keys(const Object *obj);

/**
 * Convert shape keys on the given object to USD blend shapes. The blend-shapes
 * will be added to the stage as children of the given USD mesh prim. The blend-shape
 * names and targets will also be set as properties on the primitive.
 *
 * \param stage: The stage
 * \param obj: The mesh object whose shape keys will be converted to blend shapes
 * \param mesh_prim: The USD mesh that will be assigned the blend shape targets
 * \param allow_unicode: Whether to allow unicode encoded characters in the blend shape name
 */
void create_blend_shapes(pxr::UsdStageRefPtr stage,
                         const Object *obj,
                         const pxr::UsdPrim &mesh_prim,
                         bool allow_unicode);

/**
 * If the given object is a mesh with shape keys, return a copy of the object's pre-modified mesh
 * with its verts in the shape key basis positions. The returned mesh must be freed by the caller.
 *
 * \param obj: The mesh object with shape keys
 * \return A new mesh corresponding to the shape key basis shape, or null if the object
 *         isn't a mesh or has no shape keys.
 */
Mesh *get_shape_key_basis_mesh(Object *obj);

}  // namespace io::usd
}  // namespace blender
