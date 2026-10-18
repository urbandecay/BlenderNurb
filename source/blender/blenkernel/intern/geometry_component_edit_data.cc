/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_geometry_nodes_gizmos_transforms.hh"
#include "BKE_geometry_set.hh"

namespace blender::bke {

GeometryComponentEditData::GeometryComponentEditData() : GeometryComponent(Type::Edit) {}

GeometryComponentPtr GeometryComponentEditData::copy() const
{
  GeometryComponentEditData *new_component = new GeometryComponentEditData();
  if (gizmo_edit_hints_) {
    new_component->gizmo_edit_hints_ = std::make_unique<GizmoEditHints>(*gizmo_edit_hints_);
  }
  if (curves_edit_hints_) {
    new_component->curves_edit_hints_ = std::make_unique<CurvesEditHints>(*curves_edit_hints_);
  }
  return GeometryComponentPtr(new_component);
}

bool GeometryComponentEditData::owns_direct_data() const
{
  return true;
}

void GeometryComponentEditData::ensure_owns_direct_data()
{
  /* Nothing to do. */
}

void GeometryComponentEditData::clear()
{
  BLI_assert(this->is_mutable() || this->is_expired());
  curves_edit_hints_.reset();
  gizmo_edit_hints_.reset();
}

static ImplicitSharingPtrAndData save_shared_attribute(const GAttributeReader &attribute)
{
  if (attribute.sharing_info && attribute.varray.is_span()) {
    const void *data = attribute.varray.get_internal_span().data();
    attribute.sharing_info->add_user();
    return {ImplicitSharingPtr(attribute.sharing_info), data};
  }
  auto *data = new ImplicitSharedValue<GArray<>>(attribute.varray.type(), attribute.varray.size());
  attribute.varray.materialize(data->data.data());
  return {ImplicitSharingPtr<>(data), data->data.data()};
}

static void remember_deformed_curve_positions_if_necessary(
    const Curves *curves_id, GeometryComponentEditData &edit_component)
{
  if (!edit_component.curves_edit_hints_) {
    return;
  }
  if (curves_id == nullptr) {
    return;
  }
  CurvesEditHints &edit_hints = *edit_component.curves_edit_hints_;
  if (edit_hints.positions().has_value()) {
    return;
  }
  const CurvesGeometry &curves = curves_id->geometry.wrap();
  const int points_num = curves.points_num();
  if (points_num != edit_hints.curves_id_orig.geometry.point_num) {
    return;
  }
  edit_hints.positions_data = save_shared_attribute(curves.attributes().lookup("position"));
}

void GeometryComponentEditData::remember_deformed_positions_if_necessary(GeometrySet &geometry)
{
  /* This component should be created at the start of object evaluation if it's necessary. */
  if (!geometry.has<GeometryComponentEditData>()) {
    return;
  }
  GeometryComponentEditData &edit_component =
      geometry.get_component_for_write<GeometryComponentEditData>();
  remember_deformed_curve_positions_if_necessary(geometry.get_curves(), edit_component);
}

}  // namespace blender::bke
