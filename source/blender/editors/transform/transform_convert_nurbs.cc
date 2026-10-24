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
#include "BLI_string.h"

#include "BKE_curve.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_transform.hh"
#include "BKE_report.hh"
#include "BKE_unit.hh"

#include "ED_nurbs.hh"

#include "RNA_access.hh"

#include "WM_types.hh"

#include "transform_convert.hh"
#include "transform_snap.hh"
#include "transform_translate_precision.hh"

namespace blender::ed::transform {

namespace nurbs = bke::nurbs;

struct NurbsTransformData {
  nurbs::ReadSnapshot original;
  nurbs::ReadSnapshot current;
  Vector<int64_t> vertices;
  Array<float3> proxies;
  double3x3 world_to_local;
  double3 offset = double3(0);
  double3 applied_offset = double3(0);
  double position_error_bound = 0;
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
  if (!ELEM(t->mode, TFM_TRANSLATION, TFM_DUMMY)) {
    fail("Native NURBS currently supports Move; other modal transforms need geometry adapters");
    return;
  }
  if (t->data_container_len != 1) {
    fail("Native NURBS movement currently requires a single edit object");
    return;
  }
  if (t->flag & T_PROP_EDIT_ALL) {
    fail("Native NURBS proportional movement requires a fit-point deformation rule");
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
    fail("Native NURBS movement cannot invert this object's scale");
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
  data->proxies.reinitialize(data->vertices.size());
  tc.data_len = data->vertices.size();
  tc.data = MEM_new_array_zeroed<TransData>(tc.data_len, __func__);
  for (int i = 0; i < tc.data_len; i++) {
    TransData &td = tc.data[i];
    data->proxies[i] = float3(0);
    td.loc = data->proxies[i];
    td.flag = TD_SELECTED;
    td.factor = 1;
    const auto &vertex = *index->vertex(data->vertices[i]);
    for (int axis = 0; axis < 3; axis++) {
      td.center[axis] = float(vertex.position[axis]);
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
        t->reports, RPT_ERROR, "NURBS movement could not restore its source: %s", error.c_str());
    return false;
  }
  data.current = curve.editnurb->nurbs_edit->read_snapshot();
  data.changed = false;
  data.applied_offset = double3(0);
  return true;
}

static void recalc_trans_nurbs(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    auto &data = *static_cast<NurbsTransformData *>(tc->custom.type.data);
    if (t->state == TRANS_CANCEL) {
      restore_nurbs(t, *tc, data);
      continue;
    }
    auto fail = [&](const std::string &message) {
      BKE_report(t->reports, RPT_ERROR, message.c_str());
      t->state = TRANS_CANCEL;
      restore_nurbs(t, *tc, data);
    };
    if (t->mode != TFM_TRANSLATION) {
      fail("Native NURBS supports Move only in this transform adapter");
      return;
    }
    if (ELEM(t->orient[t->orient_curr].type, V3D_ORIENT_NORMAL, V3D_ORIENT_GIMBAL)) {
      fail("Native NURBS Normal and Gimbal movement need a source orientation adapter");
      return;
    }
    if (transform_snap_is_active(t)) {
      fail("Native NURBS snapping requires a source-precision snapping adapter");
      return;
    }
    if (data.offset == data.applied_offset) {
      continue;
    }
    const double3 local_offset = data.world_to_local * data.offset;
    if (data.offset != double3(0) && local_offset == double3(0)) {
      fail("Requested NURBS movement underflows in object space");
      return;
    }
    std::string error;
    auto result = nurbs::translate_vertices(data.original.geometry(),
                                            data.vertices,
                                            local_offset,
                                            std::numeric_limits<double>::max(),
                                            &error);
    if (!result) {
      fail(error);
      return;
    }
    if (local_offset != double3(0)) {
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
    }
    nurbs::EditSnapshot candidate{std::move(result->geometry), data.original.selection()};
    Curve &curve = *id_cast<Curve *>(tc->obedit->data);
    if (!ed::nurbs::edit_commit(*t->bmain, curve, std::move(candidate), data.current, &error)) {
      fail(error);
      return;
    }
    data.current = curve.editnurb->nurbs_edit->read_snapshot();
    data.applied_offset = data.offset;
    data.position_error_bound = result->position_error_bound;
    data.changed = true;
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
    if (data.op && data.final_value) {
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
                  "NURBS move coefficient rounding bound: %.17g local units",
                  data.position_error_bound);
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
