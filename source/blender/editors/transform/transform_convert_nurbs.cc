/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <algorithm>
#include <cmath>
#include <limits>

#include "DNA_curve_types.h"
#include "DNA_scene_types.h"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLI_time.h"

#include "BKE_curve.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_debug.hh"
#include "BKE_nurbs_transform.hh"
#include "BKE_report.hh"
#include "BKE_unit.hh"

#include "ED_nurbs.hh"

#include "RNA_access.hh"

#include "WM_types.hh"

#include "transform_convert.hh"
#include "transform_snap.hh"
#include "transform_snap_object.hh"
#include "transform_translate_precision.hh"

namespace blender::ed::transform {

namespace nurbs = bke::nurbs;

struct NurbsTransformData {
  nurbs::ReadSnapshot original;
  nurbs::ReadSnapshot current;
  Vector<int64_t> vertices;
  Array<float3> proxies;
  Array<double3> centers;
  double3x3 world_to_local, local_to_world;
  double3x3 linear = double3x3::identity();
  int applied_mode = TFM_DUMMY;
  bool reset_requested = false;
  double3 offset = double3(0);
  double3 applied_offset = double3(0);
  double position_error_bound = 0;
  double boundary_error_bound = 0;
  double chart_error_bound = 0;
  int reparameterized_faces = 0;
  bool changed = false;
  wmOperator *op = nullptr;
  std::optional<TranslationPrecision> replay;
  std::optional<TranslationPrecision> final_value;
  std::optional<double3> exact_space;

  explicit NurbsTransformData(nurbs::ReadSnapshot source)
      : original(source), current(std::move(source))
  {
  }
};

static void create_trans_nurbs(bContext * /*C*/, TransInfo *t)
{
  auto fail = [&](const char *message) {
    if (t->mode != TFM_DUMMY) {
      BKE_report(t->reports, RPT_ERROR, message);
    }
  };
  if (!ELEM(t->mode, TFM_TRANSLATION, TFM_ROTATION, TFM_RESIZE, TFM_DUMMY)) {
    fail("This native NURBS transform needs a geometry adapter");
    return;
  }
  if (t->data_container_len != 1) {
    fail("Native NURBS transforms currently require a single edit object");
    return;
  }
  if (t->flag & T_PROP_EDIT_ALL) {
    fail("Native NURBS proportional editing requires a fit-point deformation rule");
    return;
  }
  TransDataContainer &tc = t->data_container[0];
  Curve &curve = *id_cast<Curve *>(tc.obedit->data);
  if (curve.editnurb == nullptr || curve.editnurb->nurbs_edit == nullptr) {
    fail("Native NURBS editable source is unavailable");
    return;
  }
  auto source = curve.editnurb->nurbs_edit->read_snapshot();
  const auto index = nurbs::TopologyIndex::build(source.geometry());
  if (!index || source.selection().vertices.is_empty()) {
    return;
  }
  bool invertible = true;
  const double3x3 inverse = t->mode == TFM_DUMMY ?
                                double3x3::identity() :
                                math::invert(double3x3(float3x3(tc.obedit->object_to_world())),
                                             invertible);
  if (!invertible) {
    fail("Native NURBS transforms cannot invert this object's scale");
    return;
  }
  for (const int64_t id : source.selection().vertices) {
    const auto *vertex = index->vertex(id);
    if (vertex == nullptr) {
      fail("Native NURBS selection references an unavailable vertex");
      return;
    }
    for (const double value : vertex->position) {
      if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
        fail("Native NURBS selection exceeds the viewport transform coordinate range");
        return;
      }
    }
  }
  auto *data = MEM_new<NurbsTransformData>(__func__, std::move(source));
  tc.custom.type.data = data;
  tc.custom.type.free_cb = [](TransInfo *, TransDataContainer *, TransCustomData *custom) {
    MEM_delete(static_cast<NurbsTransformData *>(custom->data));
    custom->data = nullptr;
  };
  for (const int64_t id : data->original.selection().vertices) {
    data->vertices.append(id);
  }
  std::sort(data->vertices.begin(), data->vertices.end());
  data->world_to_local = inverse;
  data->local_to_world = double3x3(float3x3(tc.obedit->object_to_world()));
  data->proxies.reinitialize(data->vertices.size());
  data->centers.reinitialize(data->vertices.size());
  std::optional<Vector<nurbs::SelectionTransformGroup>> islands;
  if (t->around == V3D_AROUND_LOCAL_ORIGINS) {
    std::string error;
    islands = nurbs::selection_transform_groups(data->original.geometry(),
                                                data->original.selection(),
                                                nurbs::SelectionPivot::Individual,
                                                &error);
    if (!islands) {
      fail(error.c_str());
      return;
    }
  }
  tc.data_len = data->vertices.size();
  tc.data = MEM_new_array_zeroed<TransData>(tc.data_len, __func__);
  for (int i = 0; i < tc.data_len; i++) {
    TransData &td = tc.data[i];
    data->proxies[i] = float3(0);
    td.loc = data->proxies[i];
    td.flag = TD_SELECTED;
    td.factor = 1;
    const auto &vertex = *index->vertex(data->vertices[i]);
    data->centers[i] = double3(vertex.position);
    for (int axis = 0; axis < 3; axis++) {
      td.center[axis] = float(vertex.position[axis]);
    }
    if (islands) {
      for (const auto &island : *islands) {
        if (island.vertices.contains(data->vertices[i])) {
          copy_v3_v3(td.center, float3(island.center));
          data->centers[i] = island.center;
          break;
        }
      }
    }
    copy_m3_m3(td.mtx, tc.mat3);
    copy_m3_m3(td.smtx, tc.imat3);
    unit_m3(td.axismtx);
  }
}

static bool restore_nurbs(TransInfo *t, TransDataContainer &tc, NurbsTransformData &data)
{
  if (!data.changed) {
    return true;
  }
  Curve &curve = *id_cast<Curve *>(tc.obedit->data);
  std::string error;
  if (!ed::nurbs::edit_commit(
          *t->bmain, curve, data.original.copy_for_edit(), data.current, &error))
  {
    BKE_reportf(
        t->reports, RPT_ERROR, "NURBS transform could not restore its source: %s", error.c_str());
    return false;
  }
  data.current = curve.editnurb->nurbs_edit->read_snapshot();
  data.changed = false;
  data.applied_offset = double3(0);
  data.applied_mode = TFM_DUMMY;
  return true;
}

static void recalc_trans_nurbs(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    auto &data = *static_cast<NurbsTransformData *>(tc->custom.type.data);
    const double update_start = BLI_time_now_seconds();
    if (t->state == TRANS_CANCEL || data.reset_requested) {
      data.reset_requested = false;
      restore_nurbs(t, *tc, data);
      continue;
    }
    auto fail = [&](const std::string &message) {
      NURBS_DEBUG_LOG(
                "transform update failed after %.3f ms: mode %d, selected vertices %d; %s",
                (BLI_time_now_seconds() - update_start) * 1000.0,
                t->mode,
                int(data.vertices.size()),
                message.c_str());
      BKE_report(t->reports, RPT_ERROR, message.c_str());
      t->state = TRANS_CANCEL;
      restore_nurbs(t, *tc, data);
    };
    if (!ELEM(t->mode, TFM_TRANSLATION, TFM_ROTATION, TFM_RESIZE)) {
      fail("This native NURBS transform needs a geometry adapter");
      return;
    }
    if (ELEM(t->orient[t->orient_curr].type, V3D_ORIENT_NORMAL, V3D_ORIENT_GIMBAL)) {
      fail("Native NURBS Normal and Gimbal transforms need a source orientation adapter");
      return;
    }
    if (transform_snap_is_active(t) && t->mode != TFM_TRANSLATION && validSnap(t)) {
      fail("Native NURBS snapping requires a source-precision snapping adapter");
      return;
    }
    const bool translation = t->mode == TFM_TRANSLATION;
    if (translation && data.applied_mode == t->mode && data.offset == data.applied_offset) {
      NURBS_DEBUG_LOG(
                "transform update skipped: unchanged translation, %.3f ms; %d selected vertices",
                (BLI_time_now_seconds() - update_start) * 1000.0,
                int(data.vertices.size()));
      continue;
    }
    if (!translation && data.linear == double3x3::identity()) {
      restore_nurbs(t, *tc, data);
      data.applied_mode = t->mode;
      continue;
    }
    std::string error;
    std::optional<nurbs::GeometryTransformResult> result;
    double3 local_offset(0);
    const double geometry_start = BLI_time_now_seconds();
    if (translation) {
      local_offset = data.world_to_local * data.offset;
      if (data.offset != double3(0) && local_offset == double3(0)) {
        fail("Requested NURBS movement underflows in object space");
        return;
      }
      result = nurbs::translate_vertices(data.original.geometry(),
                                         data.vertices,
                                         local_offset,
                                         std::numeric_limits<double>::max(),
                                         &error);
    }
    else {
      auto pivot = nurbs::SelectionPivot::Median;
      switch (t->around) {
        case V3D_AROUND_CENTER_BOUNDS:
          pivot = nurbs::SelectionPivot::Bounds;
          break;
        case V3D_AROUND_ACTIVE:
          pivot = nurbs::SelectionPivot::Active;
          break;
        case V3D_AROUND_LOCAL_ORIGINS:
          pivot = nurbs::SelectionPivot::Individual;
          break;
      }
      auto groups = nurbs::selection_transform_groups(
          data.original.geometry(), data.original.selection(), pivot, &error);
      if (!groups) {
        fail(error);
        return;
      }
      /* Blender's bounding box is measured in world axes, including for rotated owners.
       * Its center, cursor and explicit override are existing float UI inputs. */
      if ((t->flag & T_OVERRIDE_CENTER) ||
          ELEM(t->around, V3D_AROUND_CURSOR, V3D_AROUND_CENTER_BOUNDS))
      {
        const double3 center = data.world_to_local *
                               (double3(t->center_global) -
                                double3(tc->obedit->object_to_world().location()));
        for (auto &group : *groups) {
          group.center = center;
        }
      }
      Vector<nurbs::VertexTransform> transforms;
      const double3x3 linear = data.world_to_local * data.linear * data.local_to_world;
      for (const auto &group : *groups) {
        double4x4 matrix = double4x4::identity();
        for (int column = 0; column < 3; column++) {
          for (int row = 0; row < 3; row++) {
            matrix[column][row] = linear[column][row];
          }
        }
        matrix.location() = group.center - linear * group.center;
        for (const int64_t id : group.vertices) {
          transforms.append({id, matrix});
        }
      }
      result = nurbs::transform_vertices(
          data.original.geometry(), transforms, std::numeric_limits<double>::max(), &error);
    }
    const double geometry_ms = (BLI_time_now_seconds() - geometry_start) * 1000.0;
    if (!result) {
      fail(error);
      return;
    }
    double validation_ms = 0.0;
    if (translation && local_offset != double3(0)) {
      const double validation_start = BLI_time_now_seconds();
      const auto original_index = nurbs::TopologyIndex::build(data.original.geometry());
      const auto moved_index = nurbs::TopologyIndex::build(*result->geometry);
      bool changed_position = false;
      for (const int64_t id : data.vertices) {
        changed_position |= double3(original_index->vertex(id)->position) !=
                            double3(moved_index->vertex(id)->position);
      }
      if (!changed_position) {
        fail("Requested NURBS movement is smaller than the source coordinate precision");
        return;
      }
      validation_ms = (BLI_time_now_seconds() - validation_start) * 1000.0;
    }
    nurbs::EditSnapshot candidate{std::move(result->geometry), data.original.selection()};
    Curve &curve = *id_cast<Curve *>(tc->obedit->data);
    const double commit_start = BLI_time_now_seconds();
    if (!ed::nurbs::edit_commit(*t->bmain, curve, std::move(candidate), data.current, &error)) {
      fail(error);
      return;
    }
    const double commit_ms = (BLI_time_now_seconds() - commit_start) * 1000.0;
    data.current = curve.editnurb->nurbs_edit->read_snapshot();
    data.applied_mode = t->mode;
    data.applied_offset = data.offset;
    data.position_error_bound = result->position_error_bound;
    data.boundary_error_bound = result->boundary_error_bound;
    data.chart_error_bound = result->chart_error_bound;
    data.reparameterized_faces = result->reparameterized_faces;
    data.changed = true;
    NURBS_DEBUG_LOG(
              "transform update: %.3f ms total; geometry %.3f ms, validation %.3f ms, commit "
              "%.3f ms; mode %d, selected vertices %d, translation %d, offset "
              "(%.9g, %.9g, %.9g), error bounds position %.3g boundary %.3g chart %.3g, "
              "reparameterized faces %d",
              (BLI_time_now_seconds() - update_start) * 1000.0,
              geometry_ms,
              validation_ms,
              commit_ms,
              t->mode,
              int(data.vertices.size()),
              int(translation),
              data.offset.x,
              data.offset.y,
              data.offset.z,
              data.position_error_bound,
              data.boundary_error_bound,
              data.chart_error_bound,
              data.reparameterized_faces);
  }
}

void nurbs_affine_update(TransInfo *t, const float matrix[3][3])
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    auto &data = *static_cast<NurbsTransformData *>(tc->custom.type.data);
    data.linear = double3x3(float3x3(matrix));
  }
}

void nurbs_transform_reset(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    if (auto *data = static_cast<NurbsTransformData *>(tc->custom.type.data)) {
      data->reset_requested = true;
    }
  }
}

void nurbs_translation_init(TransInfo *t, wmOperator *op)
{
  auto &data = *static_cast<NurbsTransformData *>(t->data_container[0].custom.type.data);
  data.op = op;
  if (op == nullptr || (t->flag & T_MODAL)) {
    return;
  }
  if (PropertyRNA *property = RNA_struct_find_property(op->ptr, "value_precision")) {
    const std::string stored = RNA_property_string_get(op->ptr, property);
    if (!stored.empty()) {
      data.replay = TranslationPrecision::deserialize(stored);
      if (!data.replay) {
        BKE_report(t->reports, RPT_ERROR, "Invalid precise NURBS translation replay value");
        t->state = TRANS_CANCEL;
      }
    }
  }
  double3 exact = data.replay ? data.replay->space : double3(t->values);
  bool has_exact = false;
  for (int axis = 0; axis < 3; axis++) {
    PropertyRNA *property = RNA_struct_find_property(op->ptr,
                                                     TRANSLATION_PRECISE_PROPERTIES[axis]);
    if (!property) {
      continue;
    }
    const std::string text = RNA_property_string_get(op->ptr, property);
    if (text.empty()) {
      continue;
    }
    has_exact = true;
    /* Plain expressions are in Blender units; explicit physical units use the scene scale. */
    const int type = BKE_unit_string_contains_unit(text.c_str(), B_UNIT_LENGTH) ? B_UNIT_LENGTH :
                                                                                  B_UNIT_NONE;
    char *error = nullptr;
    const bool valid = user_string_to_number(
        t->context, text.c_str(), t->scene->unit, type, &exact[axis], nullptr, true, &error, true);
    if (!valid || !std::isfinite(exact[axis])) {
      BKE_reportf(
          t->reports, RPT_ERROR, "Invalid exact NURBS movement: %s", error ? error : text.c_str());
      t->state = TRANS_CANCEL;
    }
    MEM_SAFE_DELETE(error);
  }
  if (has_exact) {
    data.exact_space = exact;
    t->flag |= T_INPUT_IS_VALUES_FINAL;
  }
}

bool nurbs_translation_replay(TransInfo *t, double3 &r_space, double3 &r_world)
{
  const auto &data = *static_cast<const NurbsTransformData *>(
      t->data_container[0].custom.type.data);
  if (data.exact_space) {
    r_space = *data.exact_space;
    if (data.replay && r_space == data.replay->space &&
        float3x3(t->spacemtx) == data.replay->orientation)
    {
      r_world = data.replay->world;
    }
    else {
      r_world = double3x3(float3x3(t->spacemtx)) * r_space;
    }
    return true;
  }
  if (!data.replay) {
    return false;
  }
  const auto value = data.replay->replay(float3(t->values), float3x3(t->spacemtx));
  r_space = value.space;
  r_world = value.world;
  return true;
}

bool nurbs_translation_snap(TransInfo *t, double3 &r_world, std::string &r_error)
{
  if ((!ELEM(t->tsnap.target_type, SCE_SNAP_TO_POINT, SCE_SNAP_TO_EDGE_ENDPOINT) &&
       !(t->modifiers & MOD_SNAP_FORCED)) ||
      (t->tsnap.status & SNAP_MULTI_POINTS) || (t->tsnap.flag & SCE_SNAP_ROTATE) ||
      t->tsnap.snap_source_fn == nullptr)
  {
    r_error = "Native NURBS move currently supports vertex snapping with the standard snap base";
    return false;
  }
  const TransDataContainer &tc = t->data_container[0];
  const auto &data = *static_cast<const NurbsTransformData *>(tc.custom.type.data);
  const double4x4 matrix(tc.obedit->object_to_world());
  double3 target(t->tsnap.snap_target);
  if (!(t->modifiers & MOD_SNAP_FORCED) && t->tsnap.object_context &&
      t->tsnap.object_context->ret.point_location)
  {
    target = *t->tsnap.object_context->ret.point_location;
  }
  double3 source(0);
  auto median = [&]() {
    double3 center(0);
    for (const double3 &position : data.centers) {
      center += position / double(data.centers.size());
    }
    return math::transform_point(matrix, center);
  };
  if (t->tsnap.source_operation == SCE_SNAP_SOURCE_CLOSEST) {
    /* Follow Blender's chosen float picking proxy back to its double source representative.
     * If several sources share one proxy, prefer the closest authoritative point. */
    std::optional<double> distance;
    for (const int i : data.centers.index_range()) {
      float3 proxy(tc.data[i].center);
      mul_m4_v3(tc.mat, proxy);
      if (proxy != float3(t->tsnap.snap_source)) {
        continue;
      }
      const double3 position = math::transform_point(matrix, data.centers[i]);
      const double value = math::distance_squared(position, target);
      if (!distance || value < *distance) {
        source = position;
        distance = value;
      }
    }
    if (!distance) {
      r_error = "NURBS snap base no longer identifies a selected source endpoint";
      return false;
    }
  }
  else if (t->tsnap.source_operation == SCE_SNAP_SOURCE_ACTIVE ||
           (t->tsnap.source_operation == SCE_SNAP_SOURCE_CENTER &&
            t->around == V3D_AROUND_ACTIVE && !(t->flag & T_OVERRIDE_CENTER)))
  {
    const auto groups = nurbs::selection_transform_groups(data.original.geometry(),
                                                          data.original.selection(),
                                                          nurbs::SelectionPivot::Active,
                                                          &r_error);
    if (!groups || groups->is_empty()) {
      return false;
    }
    source = math::transform_point(matrix, (*groups)[0].center);
  }
  else if (t->tsnap.source_operation == SCE_SNAP_SOURCE_CENTER &&
           ((t->flag & T_OVERRIDE_CENTER) || t->around == V3D_AROUND_CURSOR))
  {
    source = double3(t->center_global);
  }
  else if (t->tsnap.source_operation == SCE_SNAP_SOURCE_CENTER &&
           t->around == V3D_AROUND_CENTER_BOUNDS)
  {
    double3 low = math::transform_point(matrix, data.centers[0]), high = low;
    for (const double3 &position : data.centers) {
      const double3 world = math::transform_point(matrix, position);
      low = math::min(low, world);
      high = math::max(high, world);
    }
    source = low * 0.5 + high * 0.5;
  }
  else {
    source = median();
  }
  r_world = target - source;
  if (t->con.mode & CON_APPLY) {
    /* Point snapping uses the same axis/plane projection as Blender's mesh transforms. */
    r_world = double3x3(float3x3(t->con.pmtx)) * r_world;
  }
  return true;
}

void nurbs_translation_update(TransInfo *t, const double3 &space, const double3 &world)
{
  auto &data = *static_cast<NurbsTransformData *>(t->data_container[0].custom.type.data);
  data.offset = world;
  data.final_value = TranslationPrecision{space, world, float3x3(t->spacemtx)};
}

static void after_trans_nurbs(bContext * /*C*/, TransInfo *t)
{
  if (t->state == TRANS_CANCEL || t->mode == TFM_DUMMY) {
    return;
  }
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    const auto &data = *static_cast<const NurbsTransformData *>(tc->custom.type.data);
    if (t->mode == TFM_TRANSLATION && data.op && data.final_value) {
      if (PropertyRNA *property = RNA_struct_find_property(data.op->ptr, "value_precision")) {
        RNA_property_string_set(data.op->ptr, property, data.final_value->serialize().c_str());
      }
      for (int axis = 0; axis < 3; axis++) {
        if (PropertyRNA *property = RNA_struct_find_property(data.op->ptr,
                                                             TRANSLATION_PRECISE_PROPERTIES[axis]))
        {
          char value[NUM_STR_REP_LEN];
          BLI_snprintf(value, sizeof(value), "%.17g", data.final_value->space[axis]);
          RNA_property_string_set(data.op->ptr, property, value);
        }
      }
    }
    if (data.changed && data.position_error_bound > 0) {
      BKE_reportf(t->reports,
                  RPT_INFO,
                  "NURBS transform coefficient rounding bound: %.17g local units",
                  data.position_error_bound);
    }
    if (data.changed && data.boundary_error_bound > 0) {
      BKE_reportf(t->reports,
                  RPT_INFO,
                  "NURBS transform additional edge/surface agreement bound: %.17g local units",
                  data.boundary_error_bound);
    }
    if (data.changed && data.reparameterized_faces) {
      BKE_reportf(t->reports,
                  RPT_INFO,
                  "NURBS transform: %d planar quad charts rebuilt; whole-face error bound %.17g "
                  "local units",
                  data.reparameterized_faces,
                  data.chart_error_bound);
    }
  }
}

TransConvertTypeInfo TransConvertType_Nurbs = {
    /*flags*/ T_EDIT | T_POINTS,
    /*create_trans_data*/ create_trans_nurbs,
    /*recalc_data*/ recalc_trans_nurbs,
    /*special_aftertrans_update*/ after_trans_nurbs,
};

}  // namespace blender::ed::transform
