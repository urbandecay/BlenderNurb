/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "usd_writer_transform.hh"
#include "usd_hierarchy_iterator.hh"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/usd/usdGeom/xform.h>

#include "BKE_object.hh"

#include "DNA_object_types.h"

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_rotation.h"
#include "BLI_vector.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"io.usd"};

namespace io::usd {

USDTransformWriter::USDTransformWriter(const USDExporterContext &ctx) : USDAbstractWriter(ctx) {}

pxr::UsdGeomXformable USDTransformWriter::create_xformable() const
{
  pxr::UsdGeomXform xform;

  /* If prim exists, cast to #UsdGeomXform
   * (Solves merge transform and shape issue for animated exports). */
  pxr::UsdPrim existing_prim = usd_export_context_.stage->GetPrimAtPath(
      usd_export_context_.usd_path);
  if (existing_prim.IsValid() && existing_prim.IsA<pxr::UsdGeomXform>()) {
    xform = pxr::UsdGeomXform(existing_prim);
  }
  else {
    xform = pxr::UsdGeomXform::Define(usd_export_context_.stage, usd_export_context_.usd_path);
  }

  return pxr::UsdGeomXformable(xform.GetPrim());
}

bool USDTransformWriter::should_apply_world_xform(const HierarchyContext &context) const
{
  /* Every object transform is written in world space, so coordinate conversion must be applied
   * to each regular object instead of relying on a converted root transform to propagate through
   * parents. Instance and prototype subgraphs retain their local hierarchy. */
  if (!usd_export_context_.export_params.root_prim_path.empty()) {
    return false;
  }
  const bool preserve_instance_hierarchy = context.duplicator != nullptr || context.is_duplisource;
  if (preserve_instance_hierarchy && context.export_parent != nullptr) {
    return false;
  }
  return usd_export_context_.export_params.convert_orientation ||
         usd_export_context_.export_params.convert_scene_units != SceneUnits::Meters;
}

void USDTransformWriter::do_write(HierarchyContext &context)
{
  if (context.is_point_proto || context.is_point_instance) {
    return;
  }

  pxr::UsdGeomXformable xform = create_xformable();

  if (!xform) {
    CLOG_ERROR(&LOG, "USDTransformWriter: couldn't create xformable");
    return;
  }

  /* Preserve local transforms inside instance/prototype subgraphs. Regular objects use world
   * transforms and reset the USD stack, so the namespace hierarchy cannot apply object-parent
   * transforms again. */
  const bool preserve_instance_hierarchy = context.duplicator != nullptr || context.is_duplisource;
  float4x4 transform_matrix = context.matrix_world;
  if (preserve_instance_hierarchy) {
    transform_matrix = context.parent_matrix_inv_world * context.matrix_world;
  }
  xform.SetResetXformStack(!preserve_instance_hierarchy);

  if (should_apply_world_xform(context)) {
    if (usd_export_context_.export_params.convert_orientation) {
      float3x3 mrot;
      float4x4 mat = float4x4::identity();
      mat3_from_axis_conversion(IO_AXIS_Y,
                                IO_AXIS_Z,
                                usd_export_context_.export_params.forward_axis,
                                usd_export_context_.export_params.up_axis,
                                mrot.ptr());
      mat.view<3, 3>() = math::transpose(mrot);
      transform_matrix = mat * transform_matrix;
    }

    if (usd_export_context_.export_params.convert_scene_units != SceneUnits::Meters) {
      const float3 scale = float3(1.0 / get_meters_per_unit(usd_export_context_.export_params));
      const float4x4 mat_scale = math::from_scale<float4x4>(scale);
      transform_matrix = mat_scale * transform_matrix;
    }
  }

  /* USD Xforms are by default the identity transform; only write if necessary when static. */
  if (is_animated_ || !math::is_equal(transform_matrix, float4x4::identity(), 0.000000001f))
  {
    set_xform_ops(transform_matrix, xform);
  }

  if (usd_export_context_.export_params.use_instancing && context.is_instance()) {
    mark_as_instance(context, xform.GetPrim());
  }

  if (context.object) {
    auto prim = xform.GetPrim();
    add_to_prim_map(prim.GetPath(), &context.object->id);
    write_id_properties(prim, context.object->id, get_export_time_code());
  }
}

bool USDTransformWriter::check_is_animated(const HierarchyContext &context) const
{
  if (context.duplicator != nullptr) {
    /* This object is being duplicated, so could be emitted by a particle system and thus
     * influenced by forces. TODO(Sybren): Make this more strict. Probably better to get from the
     * depsgraph whether this object instance has a time source. */
    return true;
  }
  if (check_has_physics(context)) {
    return true;
  }
  return BKE_object_moves_in_time(context.object);
}

void USDTransformWriter::set_xform_ops(const float4x4 &parent_relative_matrix,
                                       const pxr::UsdGeomXformable &xf)
{
  if (!xf) {
    return;
  }

  XformOpMode xfOpMode = usd_export_context_.export_params.xform_op_mode;

  if (xformOps_.is_empty()) {
    switch (xfOpMode) {
      case XformOpMode::TRS:
        xformOps_.append(xf.AddTranslateOp());
        xformOps_.append(xf.AddRotateXYZOp());
        xformOps_.append(xf.AddScaleOp());
        break;
      case XformOpMode::TOS:
        xformOps_.append(xf.AddTranslateOp());
        xformOps_.append(xf.AddOrientOp());
        xformOps_.append(xf.AddScaleOp());
        break;
      case XformOpMode::MAT:
        xformOps_.append(xf.AddTransformOp());
        break;
      default:
        CLOG_WARN(&LOG, "Unknown XformOp type");
        xformOps_.append(xf.AddTransformOp());
        break;
    }
  }

  if (xformOps_.is_empty()) {
    /* Shouldn't happen. */
    return;
  }

  pxr::UsdTimeCode time_code = get_export_time_code();

  if (xformOps_.size() == 1) {
    pxr::GfMatrix4d mat_val(parent_relative_matrix.ptr());
    usd_value_writer_.SetAttribute(xformOps_[0].GetAttr(), mat_val, time_code);
  }
  else if (xformOps_.size() == 3) {
    float3 loc;
    math::Quaternion rot;
    float3 scale;

    math::to_loc_rot_scale<true>(parent_relative_matrix, loc, rot, scale);

    if (xfOpMode == XformOpMode::TRS) {
      pxr::GfVec3d loc_val(loc.x, loc.y, loc.z);
      usd_value_writer_.SetAttribute(xformOps_[0].GetAttr(), loc_val, time_code);

      const math::EulerXYZ eul = math::to_euler(rot);
      pxr::GfVec3f rot_val(eul.x().degree(), eul.y().degree(), eul.z().degree());
      usd_value_writer_.SetAttribute(xformOps_[1].GetAttr(), rot_val, time_code);

      pxr::GfVec3f scale_val(scale.x, scale.y, scale.z);
      usd_value_writer_.SetAttribute(xformOps_[2].GetAttr(), scale_val, time_code);
    }
    else if (xfOpMode == XformOpMode::TOS) {
      pxr::GfVec3d loc_val(loc.x, loc.y, loc.z);
      usd_value_writer_.SetAttribute(xformOps_[0].GetAttr(), loc_val, time_code);

      pxr::GfQuatf quat_val(rot.w, rot.x, rot.y, rot.z);
      usd_value_writer_.SetAttribute(xformOps_[1].GetAttr(), quat_val, time_code);

      pxr::GfVec3f scale_val(scale.x, scale.y, scale.z);
      usd_value_writer_.SetAttribute(xformOps_[2].GetAttr(), scale_val, time_code);
    }
  }
}

}  // namespace io::usd
}  // namespace blender
