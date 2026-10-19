/* SPDX-FileCopyrightText: 2021 Tangent Animation. All rights reserved.
 * SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapted from the Blender Alembic importer implementation. */

#include "usd_reader_xform.hh"

#include "BKE_constraint.h"
#include "BKE_lib_id.hh"
#include "BKE_object.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_string.h"

#include "DNA_cachefile_types.h"
#include "DNA_constraint_types.h"
#include "DNA_object_types.h"

#include <pxr/base/gf/matrix4f.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformCache.h>

#include <string>

namespace blender::io::usd {

static bool usd_transform_hierarchy_is_constant(pxr::UsdPrim prim)
{
  while (prim && !prim.IsPseudoRoot()) {
    const pxr::UsdGeomXformable xformable(prim);
    if (xformable && xformable.TransformMightBeTimeVarying()) {
      return false;
    }
    prim = prim.GetParent();
  }
  return true;
}

void USDXformReader::create_object(Main *bmain)
{
  object_ = BKE_object_add_only_object(bmain, OB_EMPTY, name_.c_str());
  object_->empty_drawsize = 0.1f;
  object_->data = nullptr;
}

void USDXformReader::read_object_data(Main * /*bmain*/, const pxr::UsdTimeCode time)
{
  bool is_constant;
  float4x4 transform_from_usd;

  read_matrix(transform_from_usd, time, settings_->scene_scale, &is_constant);

  if (!is_constant && settings_->get_cache_file) {
    bConstraint *con = BKE_constraint_add_for_object(
        object_, nullptr, CONSTRAINT_TYPE_TRANSFORM_CACHE);
    bTransformCacheConstraint *data = static_cast<bTransformCacheConstraint *>(con->data);

    pxr::SdfPath object_path = use_parent_xform_ ? prim_.GetParent().GetPath() : this->prim_path();

    STRNCPY(data->object_path, object_path.GetAsString().c_str());

    data->cache_file = settings_->get_cache_file();
    id_us_plus(&data->cache_file->id);
  }

  BKE_object_apply_mat4(object_, transform_from_usd.ptr(), true);

  /* Make sure to collect custom attributes */
  set_props(use_parent_xform(), time);
}

pxr::SdfPath USDXformReader::object_prim_path() const
{
  return get_xformable().GetPrim().GetPath();
}

void USDXformReader::read_matrix(float4x4 &r_mat /* world matrix */,
                                 const pxr::UsdTimeCode time,
                                 const float scale,
                                 bool *r_is_constant) const
{
  BLI_assert(r_is_constant);

  *r_is_constant = true;
  r_mat = float4x4::identity();

  std::optional<XformResult> xf_result = get_world_usd_xform(time);
  if (!xf_result) {
    return;
  }

  std::get<0>(*xf_result).Get(r_mat.ptr());
  *r_is_constant = std::get<1>(*xf_result);

  /* Transform the full USD world matrix into Blender coordinates. Every imported object is
   * independent, so root-only conversion would leave descendant transforms in source units. */
  if (!is_in_proto()) {
    if (scale != 1.0f) {
      const float4x4 mat_scale = math::from_scale<float4x4>(float3(scale));
      r_mat = mat_scale * r_mat;
    }

    if (settings_->do_convert_mat) {
      r_mat = settings_->conversion_mat * r_mat;
    }
  }
}

bool USDXformReader::prim_has_xform_ops() const
{
  const pxr::UsdGeomXformable xformable(prim_);

  if (!xformable) {
    /* This might happen if the prim is a Scope. */
    return false;
  }

  bool reset_xform_stack = false;

  return !xformable.GetOrderedXformOps(&reset_xform_stack).empty();
}

std::optional<XformResult> USDXformReader::get_world_usd_xform(const pxr::UsdTimeCode time) const
{
  /* GetLocalToWorldTransform honors USD resetXformStack behavior and evaluates all ancestor
   * transforms directly from the source stage. */
  pxr::UsdGeomXformCache xform_cache(time);
  const pxr::GfMatrix4d world_xform = xform_cache.GetLocalToWorldTransform(prim_);
  const bool is_constant = usd_transform_hierarchy_is_constant(prim_);

  /* The USD bind transform is a matrix of doubles,
   * but we cast it to GfMatrix4f because Blender expects
   * a matrix of floats. */
  return XformResult(pxr::GfMatrix4f(world_xform), is_constant);
}

pxr::UsdGeomXformable USDXformReader::get_xformable() const
{
  pxr::UsdPrim prim = use_parent_xform_ ? prim_.GetParent() : prim_;
  return pxr::UsdGeomXformable(prim);
}
}  // namespace blender::io::usd
