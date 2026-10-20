/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <algorithm>

#include "DNA_nurbs_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_utildefines.h"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "transform.hh"
#include "transform_snap.hh"

/* Own include. */
#include "transform_convert.hh"

namespace blender::ed::transform {

struct TransDataNurbs {
  NurbsControlVertex *control_vertex;
  double initial_position[3];
  float position[3];
};

struct NurbsTransformData {
  TransDataNurbs *control_vertices = nullptr;
  NurbsGeometry initial_geometry = {};
  uint64_t boundary_support_id = 0;
  float boundary_position[3] = {};
};

static void nurbs_geometry_restore(const NurbsGeometry &initial, NurbsGeometry &geometry)
{
  BKE_nurbs_curve_list_copy(geometry.curves, initial.curves);
  BKE_nurbs_topology_free(geometry.topology);
  geometry.topology = BKE_nurbs_topology_copy(*initial.topology);
  geometry.next_component_id = initial.next_component_id;
  geometry.active_control_vertex_id = initial.active_control_vertex_id;
}

static void trans_data_nurbs_free(TransInfo * /*t*/,
                                  TransDataContainer * /*tc*/,
                                  TransCustomData *custom_data)
{
  auto *data = static_cast<NurbsTransformData *>(custom_data->data);
  if (data == nullptr) {
    return;
  }
  MEM_delete(data->control_vertices);
  BKE_nurbs_curve_list_free(data->initial_geometry.curves);
  BKE_nurbs_topology_free(data->initial_geometry.topology);
  MEM_delete(data);
  custom_data->data = nullptr;
}

static bool control_vertex_is_selected(const NurbsControlVertex &control_vertex)
{
  return (control_vertex.flag & NURBS_CV_SELECT) != 0;
}

static float nurbs_control_vertex_world_distance(const TransDataNurbs &a,
                                                 const TransDataNurbs &b,
                                                 const TransData &td)
{
  float delta[3] = {float(b.initial_position[0] - a.initial_position[0]),
                    float(b.initial_position[1] - a.initial_position[1]),
                    float(b.initial_position[2] - a.initial_position[2])};
  mul_m3_v3(td.mtx, delta);
  return len_v3(delta);
}

static void calc_nurbs_curve_connected_distances(TransData *trans_data,
                                                 const TransDataNurbs *nurbs_data,
                                                 const int count)
{
  if (count <= 0) {
    return;
  }
  for (int i = 0; i < count; i++) {
    trans_data[i].dist = (trans_data[i].flag & TD_SELECTED) ? 0.0f : FLT_MAX;
  }

  for (int i = 1; i < count; i++) {
    if (trans_data[i - 1].dist == FLT_MAX) {
      continue;
    }
    const float distance = trans_data[i - 1].dist +
                           nurbs_control_vertex_world_distance(
                               nurbs_data[i - 1], nurbs_data[i], trans_data[i - 1]);
    trans_data[i].dist = std::min(trans_data[i].dist, distance);
  }
  for (int i = count - 2; i >= 0; i--) {
    if (trans_data[i + 1].dist == FLT_MAX) {
      continue;
    }
    const float distance = trans_data[i + 1].dist +
                           nurbs_control_vertex_world_distance(
                               nurbs_data[i], nurbs_data[i + 1], trans_data[i]);
    trans_data[i].dist = std::min(trans_data[i].dist, distance);
  }
}

static bool create_trans_nurbs_boundary(TransInfo *t,
                                        TransDataContainer *tc,
                                        NurbsGeometry &geometry)
{
  if (t->mode != TFM_TRANSLATION || (t->flag & T_PROP_EDIT)) {
    BKE_report(
        t->reports, RPT_ERROR, "This NURBS topology currently supports boundary translation");
    return false;
  }
  Set<uint64_t> support_ids;
  for (const NurbsTopoEdge &edge : geometry.topology->edges) {
    if (edge.flag & NURBS_TOPO_SELECT) {
      support_ids.add(edge.curve_id);
    }
  }
  if (support_ids.size() != 1) {
    BKE_report(t->reports, RPT_ERROR, "Select one complete NURBS boundary to translate");
    return false;
  }
  const uint64_t support_id = *support_ids.begin();
  for (const NurbsTopoEdge &edge : geometry.topology->edges) {
    if (edge.curve_id == support_id && !(edge.flag & NURBS_TOPO_SELECT)) {
      BKE_report(t->reports, RPT_ERROR, "Select all parts of the NURBS boundary to translate");
      return false;
    }
  }
  const NurbsCurve *support = nullptr;
  for (const NurbsCurve &curve : geometry.curves) {
    if (curve.id == support_id) {
      support = &curve;
      break;
    }
  }
  double center[3];
  if (support == nullptr ||
      !BKE_nurbs_curve_evaluate(*support, (support->domain[0] + support->domain[1]) * 0.5, center))
  {
    BKE_report(t->reports, RPT_ERROR, "Cannot evaluate the selected NURBS boundary");
    return false;
  }

  auto *data = MEM_new<NurbsTransformData>(__func__);
  BKE_nurbs_curve_list_copy(data->initial_geometry.curves, geometry.curves);
  data->initial_geometry.topology = BKE_nurbs_topology_copy(*geometry.topology);
  data->initial_geometry.next_component_id = geometry.next_component_id;
  data->initial_geometry.active_control_vertex_id = geometry.active_control_vertex_id;
  data->boundary_support_id = support_id;
  tc->custom.type.data = data;
  tc->custom.type.free_cb = trans_data_nurbs_free;
  tc->data_len = 1;
  tc->data = MEM_new_array_zeroed<TransData>(1, __func__);
  TransData &td = tc->data[0];
  for (int axis = 0; axis < 3; axis++) {
    data->boundary_position[axis] = float(center[axis]);
  }
  td.loc = data->boundary_position;
  copy_v3_v3(td.iloc, td.loc);
  copy_v3_v3(td.center, td.loc);
  td.flag = TD_SELECTED;
  td.dist = FLT_MAX;
  copy_m3_m4(td.mtx, tc->obedit->object_to_world().ptr());
  pseudoinverse_m3_m3(td.smtx, td.mtx, PSEUDOINVERSE_EPSILON);
  unit_m3(td.axismtx);
  return true;
}

static void create_trans_nurbs_vertices(bContext * /*C*/, TransInfo *t)
{
  const bool use_proportional_edit = (t->flag & T_PROP_EDIT) != 0;
  const bool use_individual_origin = t->around == V3D_AROUND_LOCAL_ORIGINS;
  int selected_count_all = 0;
  bool creation_failed = false;

  t->data_len_all = 0;
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    NurbsGeometry &geometry = *id_cast<NurbsGeometry *>(tc->obedit->data);
    if (geometry.topology != nullptr) {
      tc->data_len = 0;
      const char mode = t->scene->toolsettings->selectmode;
      bool selected = false;
      for (const NurbsTopoVertex &vertex : geometry.topology->vertices) {
        selected |= (mode & SCE_SELECT_VERTEX) && (vertex.flag & NURBS_TOPO_SELECT);
      }
      for (const NurbsTopoEdge &edge : geometry.topology->edges) {
        selected |= (mode & SCE_SELECT_EDGE) && (edge.flag & NURBS_TOPO_SELECT);
      }
      for (const NurbsTopoFace &face : geometry.topology->faces) {
        selected |= (mode & SCE_SELECT_FACE) && (face.flag & NURBS_TOPO_SELECT);
      }
      if (!selected) {
        continue;
      }
      if (mode != SCE_SELECT_EDGE) {
        BKE_report(t->reports, RPT_ERROR, "Select complete NURBS boundary edges to move");
        creation_failed = true;
        continue;
      }
      if (create_trans_nurbs_boundary(t, tc, geometry)) {
        selected_count_all++;
      }
      else {
        creation_failed = true;
      }
      continue;
    }
    int control_vertex_count = 0;
    int selected_count = 0;

    for (NurbsCurve &curve : geometry.curves) {
      if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
        continue;
      }
      control_vertex_count += curve.control_vertex_count;
      for (int i = 0; i < curve.control_vertex_count; i++) {
        selected_count += control_vertex_is_selected(curve.control_vertices[i]);
      }
    }

    selected_count_all += selected_count;
    tc->data_len = use_proportional_edit ? control_vertex_count : selected_count;
    if (tc->data_len == 0) {
      continue;
    }

    tc->data = MEM_new_array_zeroed<TransData>(tc->data_len, __func__);
    auto *custom = MEM_new<NurbsTransformData>(__func__);
    custom->control_vertices = MEM_new_array_zeroed<TransDataNurbs>(tc->data_len, __func__);
    tc->custom.type.data = custom;
    tc->custom.type.free_cb = trans_data_nurbs_free;

    float mtx[3][3], smtx[3][3];
    copy_m3_m4(mtx, tc->obedit->object_to_world().ptr());
    pseudoinverse_m3_m3(smtx, mtx, PSEUDOINVERSE_EPSILON);

    TransDataNurbs *nurbs_data = custom->control_vertices;
    int trans_data_index = 0;
    for (NurbsCurve &curve : geometry.curves) {
      if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
        continue;
      }
      const int curve_data_start = trans_data_index;
      float curve_center[3] = {};
      int curve_selected_count = 0;
      if (use_individual_origin) {
        for (int i = 0; i < curve.control_vertex_count; i++) {
          const NurbsControlVertex &control_vertex = curve.control_vertices[i];
          if (control_vertex_is_selected(control_vertex)) {
            for (int axis = 0; axis < 3; axis++) {
              curve_center[axis] += float(control_vertex.position[axis]);
            }
            curve_selected_count++;
          }
        }
        if (curve_selected_count > 0) {
          mul_v3_fl(curve_center, 1.0f / float(curve_selected_count));
        }
      }
      for (int i = 0; i < curve.control_vertex_count; i++) {
        NurbsControlVertex &control_vertex = curve.control_vertices[i];
        const bool is_selected = control_vertex_is_selected(control_vertex);
        if (!use_proportional_edit && !is_selected) {
          continue;
        }

        TransData &td = tc->data[trans_data_index];
        TransDataNurbs &data = nurbs_data[trans_data_index];
        data.control_vertex = &control_vertex;
        for (int axis = 0; axis < 3; axis++) {
          data.initial_position[axis] = control_vertex.position[axis];
          data.position[axis] = float(control_vertex.position[axis]);
        }

        td.extra = &data;
        td.loc = data.position;
        copy_v3_v3(td.iloc, td.loc);
        copy_v3_v3(td.center,
                   use_individual_origin && curve_selected_count > 0 ? curve_center : td.loc);
        td.flag = is_selected ? TD_SELECTED : 0;
        td.dist = FLT_MAX;
        td.val = nullptr;
        td.con = nullptr;
        td.hdata = nullptr;
        td.protectflag = 0;
        copy_m3_m3(td.mtx, mtx);
        copy_m3_m3(td.smtx, smtx);
        unit_m3(td.axismtx);

        trans_data_index++;
      }
      if (use_proportional_edit && (t->flag & T_PROP_CONNECTED) &&
          trans_data_index > curve_data_start)
      {
        calc_nurbs_curve_connected_distances(tc->data + curve_data_start,
                                            nurbs_data + curve_data_start,
                                            trans_data_index - curve_data_start);
      }
    }
    BLI_assert(trans_data_index == tc->data_len);
  }

  if (creation_failed) {
    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      if (tc->custom.type.data != nullptr) {
        trans_data_nurbs_free(t, tc, &tc->custom.type);
      }
      tc->custom.type.free_cb = nullptr;
      MEM_delete(tc->data);
      tc->data = nullptr;
      tc->data_len = 0;
    }
    selected_count_all = 0;
  }
  transform_around_single_fallback_ex(t, selected_count_all);
  t->data_len_all = -1;
}

static void recalc_data_nurbs(TransInfo *t)
{
  if (t->state != TRANS_CANCEL) {
    transform_snap_project_individual_apply(t);
  }

  bool failed = false;
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    NurbsGeometry &geometry = *id_cast<NurbsGeometry *>(tc->obedit->data);
    auto *custom = static_cast<NurbsTransformData *>(tc->custom.type.data);
    if (custom == nullptr) {
      continue;
    }
    if (custom->boundary_support_id != 0) {
      nurbs_geometry_restore(custom->initial_geometry, geometry);
      if (t->state != TRANS_CANCEL) {
        const TransData &td = tc->data[0];
        const double3 delta(double(td.loc[0] - td.iloc[0]),
                            double(td.loc[1] - td.iloc[1]),
                            double(td.loc[2] - td.iloc[2]));
        std::string error;
        if (!BKE_nurbs_topology_translate_boundary(
                geometry, custom->boundary_support_id, delta, &error))
        {
          BKE_report(t->reports, RPT_ERROR, error.c_str());
          failed = true;
        }
      }
      BKE_nurbs_batch_cache_dirty_tag(&geometry, BKE_NURBS_BATCH_DIRTY_ALL);
      DEG_id_tag_update(&geometry.id, ID_RECALC_GEOMETRY);
      continue;
    }
    for (int i = 0; i < tc->data_len; i++) {
      const TransData &td = tc->data[i];
      const TransDataNurbs &data = *static_cast<const TransDataNurbs *>(td.extra);
      for (int axis = 0; axis < 3; axis++) {
        data.control_vertex->position[axis] = t->state == TRANS_CANCEL ?
                                                  data.initial_position[axis] :
                                                  data.initial_position[axis] +
                                                      double(td.loc[axis] - td.iloc[axis]);
      }
    }

    BKE_nurbs_batch_cache_dirty_tag(&geometry, BKE_NURBS_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&geometry.id, ID_RECALC_GEOMETRY);
  }
  if (failed) {
    t->state = TRANS_CANCEL;
    recalc_data_nurbs(t);
  }
}

TransConvertTypeInfo TransConvertType_Nurbs = {
    /*flags*/ (T_EDIT | T_POINTS),
    /*create_trans_data*/ create_trans_nurbs_vertices,
    /*recalc_data*/ recalc_data_nurbs,
    /*special_aftertrans_update*/ nullptr,
};

}  // namespace blender::ed::transform
