/* SPDX-FileCopyrightText: 2023 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_blend_shape_utils.hh"
#include "usd_utils.hh"

#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShape.h>

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"

#include "BKE_key.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "DNA_object_types.h"

#include "BLI_assert.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"

#include <numeric>
#include <vector>

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"io.usd"};

namespace io::usd {

const Key *get_mesh_shape_key(const Object *obj)
{
  BLI_assert(obj);

  if (!obj->data || obj->type != OB_MESH) {
    return nullptr;
  }

  const Mesh *mesh = id_cast<const Mesh *>(obj->data);

  return mesh->key;
}

bool is_mesh_with_shape_keys(const Object *obj)
{
  const Key *key = get_mesh_shape_key(obj);
  return key && key->totkey > 0 && key->type == KEY_RELATIVE;
}

void create_blend_shapes(pxr::UsdStageRefPtr stage,
                         const Object *obj,
                         const pxr::UsdPrim &mesh_prim,
                         bool allow_unicode)
{
  const Key *key = get_mesh_shape_key(obj);

  if (!(key && mesh_prim)) {
    return;
  }

  pxr::UsdSkelBindingAPI binding_api = pxr::UsdSkelBindingAPI::Apply(mesh_prim);

  if (!binding_api) {
    CLOG_WARN(&LOG,
              "Couldn't apply UsdSkelBindingAPI to mesh prim %s",
              mesh_prim.GetPath().GetAsString().c_str());
    return;
  }

  pxr::VtTokenArray blendshape_names;
  std::vector<pxr::SdfPath> blendshape_paths;

  /* Get the basis, which we'll use to calculate offsets. */
  KeyBlock *basis_key = static_cast<KeyBlock *>(key->block.first);

  if (!basis_key) {
    return;
  }

  for (KeyBlock &kb : key->block) {
    if (&kb == basis_key) {
      /* Skip the basis. */
      continue;
    }

    pxr::TfToken name(make_safe_name(kb.name, allow_unicode));
    blendshape_names.push_back(name);

    pxr::SdfPath path = mesh_prim.GetPath().AppendChild(name);
    blendshape_paths.push_back(path);

    pxr::UsdSkelBlendShape blendshape = pxr::UsdSkelBlendShape::Define(stage, path);

    pxr::UsdAttribute offsets_attr = blendshape.CreateOffsetsAttr();

    /* Some applications, like Houdini, don't render blend shapes unless the point
     * indices are set, so we always create this attribute, even when every index
     * is included. */
    pxr::UsdAttribute point_indices_attr = blendshape.CreatePointIndicesAttr();

    pxr::VtVec3fArray offsets(kb.totelem);
    pxr::VtIntArray indices(kb.totelem);
    std::iota(indices.begin(), indices.end(), 0);

    const float (*fp)[3] = static_cast<float (*)[3]>(kb.data);

    const float (*basis_fp)[3] = static_cast<float (*)[3]>(basis_key->data);

    for (int i = 0; i < kb.totelem; ++i) {
      /* Subtract the key positions from the
       * basis positions to get the offsets. */
      sub_v3_v3v3(offsets[i].data(), fp[i], basis_fp[i]);
    }

    offsets_attr.Set(offsets);
    point_indices_attr.Set(indices);
  }

  /* Set the blend-shape names and targets on the shape. */
  pxr::UsdAttribute blendshape_attr = binding_api.CreateBlendShapesAttr();
  blendshape_attr.Set(blendshape_names);
  binding_api.CreateBlendShapeTargetsRel().SetTargets(blendshape_paths);
}

Mesh *get_shape_key_basis_mesh(Object *obj)
{
  if (!obj || !obj->data || obj->type != OB_MESH) {
    return nullptr;
  }

  /* If we're exporting blend shapes, we export the unmodified mesh with
   * the verts in the basis key positions. */
  const Mesh *mesh = BKE_object_get_pre_modified_mesh(obj);

  if (!mesh || !mesh->key || !mesh->key->block.first) {
    return nullptr;
  }

  const KeyBlock *basis = reinterpret_cast<KeyBlock *>(mesh->key->block.first);

  if (mesh->verts_num != basis->totelem) {
    CLOG_WARN(&LOG, "Vertex and shape key element count mismatch for mesh %s", obj->id.name + 2);
    return nullptr;
  }

  /* Make a copy of the mesh so we can update the verts to the basis shape. */
  Mesh *temp_mesh = BKE_mesh_copy_for_eval(*mesh);

  /* Update the verts. */
  BKE_keyblock_convert_to_mesh(basis, temp_mesh->vert_positions_for_write());

  return temp_mesh;
}

}  // namespace io::usd
}  // namespace blender
