/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

#include "BKE_context.hh"
#include "BKE_report.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "DNA_object_types.h"
#include "ED_solid.hh"
#include "MEM_guardedalloc.h"
#include "transform.hh"
#include "transform_convert.hh"

namespace blender::ed::transform {
namespace {
struct SolidTransform {
  std::unique_ptr<solid_editor::Session> session;
  Vector<float3> proxies;
  double3 offset = double3(0);
  double3x3 linear = double3x3::identity();
  bool changed = false;
};
void create_solid(bContext *C, TransInfo *t)
{
  if (t->data_container_len != 1 ||
      !ELEM(t->mode, TFM_TRANSLATION, TFM_ROTATION, TFM_RESIZE, TFM_DUMMY) ||
      (t->flag & T_PROP_EDIT_ALL))
  {
    if (t->mode != TFM_DUMMY)
      BKE_report(t->reports,
                 RPT_ERROR,
                 "Analytic solids support move, rotate and scale of exact components");
    return;
  }
  std::string error;
  auto session = solid_editor::Session::begin(C, &error);
  if (!session) {
    BKE_report(t->reports, RPT_ERROR, error.c_str());
    return;
  }
  auto *data = MEM_new<SolidTransform>(__func__);
  data->session = std::move(session);
  auto &tc = t->data_container[0];
  tc.custom.type.data = data;
  tc.custom.type.free_cb = [](TransInfo *, TransDataContainer *, TransCustomData *custom) {
    MEM_delete(static_cast<SolidTransform *>(custom->data));
    custom->data = nullptr;
  };
  const auto &source = data->session->source();
  /* A closed analytic conic may have just one topological seam vertex. Its geometric
   * center, rather than that seam, supplies the same median pivot as a divided circle. */
  std::unordered_set<solid::Id> selected(source.selection.vertices.begin(),
                                         source.selection.vertices.end());
  std::unordered_map<solid::Id, solid::Vec3> conic_centers;
  for (const auto &curve : source.model->curves) {
    if (curve.kind != solid::CurveKind::Conic)
      continue;
    bool all = true;
    double sweep = 0;
    std::vector<solid::Id> vertices;
    for (const auto &edge : source.model->edges)
      if (edge.curve == curve.id) {
        sweep += std::abs(edge.end - edge.start);
        for (solid::Id id : edge.vertices) {
          all &= selected.contains(id);
          vertices.push_back(id);
        }
      }
    if (all && std::abs(sweep - 2 * std::numbers::pi) < 1e-10) {
      for (solid::Id id : vertices)
        conic_centers[id] = curve.origin;
    }
  }
  tc.data_len = source.selection.vertices.size();
  tc.data = MEM_new_array_zeroed<TransData>(tc.data_len, __func__);
  data->proxies.resize(tc.data_len);
  for (int i = 0; i < tc.data_len; ++i) {
    const auto &p = solid::vertex(*source.model, source.selection.vertices[i])->position;
    data->proxies[i] = float3(p.x, p.y, p.z);
    auto &td = tc.data[i];
    td.loc = data->proxies[i];
    copy_v3_v3(td.iloc, td.loc);
    const auto center = conic_centers.find(source.selection.vertices[i]);
    if (center != conic_centers.end())
      copy_v3_v3(td.center, float3(center->second.x, center->second.y, center->second.z));
    else
      copy_v3_v3(td.center, td.loc);
    td.flag = TD_SELECTED;
    td.factor = 1;
    copy_m3_m3(td.mtx, tc.mat3);
    copy_m3_m3(td.smtx, tc.imat3);
    unit_m3(td.axismtx);
  }
}
void recalc_solid(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    auto *data = static_cast<SolidTransform *>(tc->custom.type.data);
    if (!data)
      continue;
    std::string error;
    if (t->state == TRANS_CANCEL) {
      if (data->changed && !data->session->restore(t->context, &error))
        BKE_report(t->reports, RPT_ERROR, error.c_str());
      data->changed = false;
      continue;
    }
    const auto &source = data->session->source();
    const double3x3 world(float3x3(tc->obedit->object_to_world()));
    bool invertible;
    const auto inverse = math::invert(world, invertible);
    if (!invertible) {
      BKE_report(t->reports, RPT_ERROR, "Analytic transform requires invertible object scale");
      t->state = TRANS_CANCEL;
      continue;
    }
    const auto local = inverse * data->linear * world;
    const double3 center = inverse * (double3(t->center_global) -
                                      double3(tc->obedit->object_to_world().location()));
    const double3 translation = t->mode == TFM_TRANSLATION ? inverse * data->offset :
                                                             center - local * center;
    solid::Affine matrix;
    for (int i = 0; i < 3; ++i)
      matrix.columns[i] = {local[i].x, local[i].y, local[i].z};
    matrix.translation = {translation.x, translation.y, translation.z};
    if (data->session->can_preview_affine(matrix)) {
      if (data->session->preview_affine(t->context, matrix, &error)) {
        data->changed = true;
        continue;
      }
      BKE_report(t->reports, RPT_ERROR, error.empty() ? "Solid display transform failed" : error.c_str());
      t->state = TRANS_CANCEL;
      if (data->changed)
        data->session->restore(t->context, nullptr);
      data->changed = false;
      continue;
    }
    /* Changing from a rigid drag to a deformation returns to the source first. */
    if (data->session->has_affine_preview() && !data->session->restore(t->context, &error)) {
      BKE_report(t->reports, RPT_ERROR, error.c_str());
      t->state = TRANS_CANCEL;
      continue;
    }
    auto candidate = solid::transform_selected(*source.model, source.selection, matrix, &error);
    const bool complete = source.selection.vertices.size() == source.model->vertices.size();
    if (!candidate ||
        !data->session->apply(t->context, std::move(*candidate), &error, complete, &matrix))
    {
      BKE_report(t->reports, RPT_ERROR, error.c_str());
      t->state = TRANS_CANCEL;
      if (data->changed)
        data->session->restore(t->context, nullptr);
      data->changed = false;
      continue;
    }
    data->changed = true;
  }
}
void finish_solid(bContext *C, TransInfo *t)
{
  if (t->state == TRANS_CANCEL) {
    recalc_solid(t);
    return;
  }
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    auto *data = static_cast<SolidTransform *>(tc->custom.type.data);
    if (!data || !data->changed)
      continue;
    std::string error;
    if (!data->session->commit_affine(C, &error)) {
      BKE_report(t->reports, RPT_ERROR, error.c_str());
      data->session->restore(C, nullptr);
      t->state = TRANS_CANCEL;
      data->changed = false;
    }
  }
}
}  // namespace
void solid_affine_update(TransInfo *t, const float matrix[3][3])
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc)
    if (auto *data = static_cast<SolidTransform *>(tc->custom.type.data))
      data->linear = double3x3(float3x3(matrix));
}
void solid_translation_update(TransInfo *t, const double3 &offset)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc)
    if (auto *data = static_cast<SolidTransform *>(tc->custom.type.data)) {
      data->offset = offset;
      data->linear = double3x3::identity();
    }
}
TransConvertTypeInfo TransConvertType_Solid = {
    T_EDIT | T_POINTS, create_solid, recalc_solid, finish_solid};
}  // namespace blender::ed::transform
