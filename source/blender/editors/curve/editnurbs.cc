/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include "DNA_curve_types.h"
#include "DNA_windowmanager_types.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "BLI_function_ref.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"

#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "ED_nurbs.hh"
#include "ED_object.hh"
#include "ED_select_utils.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::nurbs {

template<typename ExpectedState>
static bool edit_commit_impl(Main &bmain,
                             Curve &curve,
                             bke::nurbs::EditSnapshot candidate,
                             const ExpectedState &expected_state,
                             std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (curve.editnurb == nullptr || curve.editnurb->nurbs_edit == nullptr) {
    if (r_error != nullptr) {
      *r_error = "Curve has no native NURBS edit source";
    }
    return false;
  }
  if (!bke::nurbs::curve_validate_source(curve, r_error) ||
      !curve.editnurb->nurbs_edit->commit(std::move(candidate), expected_state, r_error))
  {
    return false;
  }

  curve.editnurb->needs_flush_to_id = true;
  bmain.is_memfile_undo_flush_needed = true;
  DEG_id_tag_update_ex(
      &bmain, &curve.id, ID_RECALC_GEOMETRY | ID_RECALC_SELECT | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier_ex(
      static_cast<wmWindowManager *>(bmain.wm.first), nullptr, NC_GEOM | ND_DATA, &curve.id);
  return true;
}

bool edit_commit(Main &bmain,
                 Curve &curve,
                 bke::nurbs::EditSnapshot candidate,
                 const uint64_t expected_revision,
                 std::string *r_error)
{
  return edit_commit_impl(bmain, curve, std::move(candidate), expected_revision, r_error);
}

bool edit_commit(Main &bmain,
                 Curve &curve,
                 bke::nurbs::EditSnapshot candidate,
                 const bke::nurbs::ReadSnapshot &expected_state,
                 std::string *r_error)
{
  return edit_commit_impl(bmain, curve, std::move(candidate), expected_state, r_error);
}

bool select_vertices(Main &bmain,
                     Curve &curve,
                     const bke::nurbs::ReadSnapshot &expected,
                     const Span<int64_t> hits,
                     const bke::nurbs::SelectionAction action,
                     const std::optional<int64_t> active_vertex,
                     std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (!bke::nurbs::curve_validate_source(curve, r_error)) {
    return false;
  }
  if (curve.editnurb == nullptr || curve.editnurb->nurbs_edit == nullptr ||
      curve.editnurb->nurbs_edit->read_snapshot() != expected)
  {
    if (r_error != nullptr) {
      *r_error = "NURBS selection hits refer to a stale or different edit source";
    }
    return false;
  }
  auto selection = bke::nurbs::selection_apply_vertices(
      expected.geometry(), expected.selection(), hits, action, active_vertex, r_error);
  if (!selection) {
    return false;
  }
  const auto &previous = expected.selection();
  if (selection->vertices == previous.vertices && selection->edges == previous.edges &&
      selection->faces == previous.faces && selection->active == previous.active &&
      selection->history.size() == previous.history.size() &&
      std::equal(selection->history.begin(), selection->history.end(), previous.history.begin()))
  {
    return false;
  }
  auto candidate = expected.copy_for_edit();
  candidate.selection = std::move(*selection);
  return edit_commit(bmain, curve, std::move(candidate), expected, r_error);
}

static bke::nurbs::SelectionAction selection_action(const eSelectOp operation)
{
  using bke::nurbs::SelectionAction;
  switch (operation) {
    case SEL_OP_ADD:
      return SelectionAction::Add;
    case SEL_OP_SUB:
      return SelectionAction::Subtract;
    case SEL_OP_SET:
      return SelectionAction::Set;
    case SEL_OP_AND:
      return SelectionAction::Intersect;
    case SEL_OP_XOR:
      return SelectionAction::Invert;
  }
  BLI_assert_unreachable();
  return SelectionAction::Set;
}

static bool project_vertices(const ViewContext &vc,
                             const Object &object,
                             const bke::nurbs::ReadSnapshot &source,
                             FunctionRef<void(int64_t, float2)> callback,
                             std::string &error)
{
  ED_view3d_init_mats_rv3d(&object, vc.rv3d);
  ED_view3d_clipping_local(vc.rv3d, object.object_to_world().ptr());
  for (const NurbsTopologyVertex &vertex : source.geometry().vertices) {
    for (const double value : vertex.position) {
      if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
        error = "NURBS source vertex exceeds the viewport coordinate range";
        return false;
      }
    }
    const float3 position(double3(vertex.position));
    float2 screen;
    if (ED_view3d_project_float_object(vc.region, position, screen, V3D_PROJ_TEST_CLIP_DEFAULT) ==
        V3D_PROJ_RET_OK)
    {
      callback(vertex.id, screen);
    }
  }
  return true;
}

bool select_box(const ViewContext &vc, const rcti &rect, const eSelectOp operation)
{
  if (!XRAY_FLAG_ENABLED(vc.v3d)) {
    WM_global_report(RPT_ERROR, "Native NURBS point selection currently requires X-Ray");
    return false;
  }
  Curve &curve = *id_cast<Curve *>(vc.obedit->data);
  const auto expected = curve.editnurb->nurbs_edit->read_snapshot();
  Vector<int64_t> hits;
  std::string error;
  if (!project_vertices(
          vc,
          *vc.obedit,
          expected,
          [&](const int64_t id, const float2 screen) {
            if (screen.x >= rect.xmin && screen.x <= rect.xmax && screen.y >= rect.ymin &&
                screen.y <= rect.ymax)
            {
              hits.append(id);
            }
          },
          error))
  {
    WM_global_report(RPT_ERROR, error.c_str());
    return false;
  }
  const bool changed = select_vertices(
      *vc.bmain, curve, expected, hits, selection_action(operation), std::nullopt, &error);
  if (!error.empty()) {
    WM_global_report(RPT_ERROR, error.c_str());
  }
  return changed;
}

struct SelectionTarget {
  Object *object;
  bke::nurbs::ReadSnapshot source;
};

/** Retain source leases so both click and linked-pick commit against the identities projected. */
static bool find_vertex(const ViewContext &vc,
                        const int mval[2],
                        const int distance,
                        Vector<SelectionTarget> &targets,
                        Object *&hit_object,
                        int64_t &hit_id,
                        std::string &error)
{
  if (!XRAY_FLAG_ENABLED(vc.v3d)) {
    error = "Native NURBS point selection currently requires X-Ray";
    return false;
  }
  double closest = double(distance) * distance;
  for (Object *object : BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
           *vc.bmain, vc.scene, vc.view_layer, vc.v3d))
  {
    Curve &curve = *id_cast<Curve *>(object->data);
    if (curve.editnurb->nurbs_edit == nullptr) {
      error = "Mixed native and legacy NURBS selection is unsupported";
      return false;
    }
    targets.append({object, curve.editnurb->nurbs_edit->read_snapshot()});
    if (!project_vertices(
            vc,
            *object,
            targets.last().source,
            [&](const int64_t id, const float2 screen) {
              const double dx = double(screen.x) - mval[0], dy = double(screen.y) - mval[1];
              const double squared_distance = dx * dx + dy * dy;
              if (squared_distance < closest) {
                closest = squared_distance;
                hit_object = object;
                hit_id = id;
              }
            },
            error))
    {
      return false;
    }
  }
  return true;
}

bool select_pick(bContext *C,
                 const int mval[2],
                 const int distance,
                 const SelectPick_Params &params)
{
  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_ensure_evaluated_depsgraph(C));
  Vector<SelectionTarget> targets;
  Object *hit_object = nullptr;
  int64_t hit_id = 0;
  std::string error;
  if (!find_vertex(vc, mval, distance, targets, hit_object, hit_id, error)) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
    return false;
  }
  if (hit_object == nullptr && !(params.sel_op == SEL_OP_SET && params.deselect_all)) {
    return false;
  }
  if (hit_object != nullptr && params.sel_op == SEL_OP_SET && params.select_passthrough) {
    for (const SelectionTarget &target : targets) {
      if (target.object == hit_object && target.source.selection().vertices.contains(hit_id)) {
        return false;
      }
    }
  }
  bool changed = false;
  for (const SelectionTarget &target : targets) {
    const bool hit = target.object == hit_object;
    if (params.sel_op != SEL_OP_SET && !hit) {
      continue;
    }
    changed |= select_vertices(*vc.bmain,
                               *id_cast<Curve *>(target.object->data),
                               target.source,
                               hit ? Span<int64_t>(&hit_id, 1) : Span<int64_t>(),
                               selection_action(params.sel_op),
                               hit ? std::optional(hit_id) : std::nullopt,
                               &error);
    if (!error.empty()) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
  }
  if (hit_object != nullptr) {
    ed::object::base_activate(C, BKE_view_layer_base_find(vc.view_layer, hit_object));
  }
  return changed;
}

static std::optional<bke::nurbs::LinkedComponents> linked_from_selection(
    const bke::nurbs::ReadSnapshot &expected, std::string *r_error)
{
  const auto &selection = expected.selection();
  const Vector<int64_t> vertices(selection.vertices.begin(), selection.vertices.end());
  const Vector<int64_t> edges(selection.edges.begin(), selection.edges.end());
  const Vector<int64_t> faces(selection.faces.begin(), selection.faces.end());
  return bke::nurbs::find_linked_components(expected.geometry(),
                                            {vertices, edges, faces},
                                            bke::nurbs::LinkedConnectivity::RequireAgreement,
                                            r_error);
}

bool select_linked(Main &bmain, Curve &curve, std::string *r_error)
{
  if (curve.editnurb == nullptr || curve.editnurb->nurbs_edit == nullptr) {
    if (r_error != nullptr) {
      *r_error = "Curve has no native NURBS edit source";
    }
    return false;
  }
  const auto expected = curve.editnurb->nurbs_edit->read_snapshot();
  const auto linked = linked_from_selection(expected, r_error);
  return linked && select_vertices(bmain,
                                   curve,
                                   expected,
                                   linked->vertices,
                                   bke::nurbs::SelectionAction::Add,
                                   std::nullopt,
                                   r_error);
}

bool select_linked(bContext *C)
{
  Main &bmain = *CTX_data_main(C);
  Vector<SelectionTarget> targets;
  Vector<bke::nurbs::LinkedComponents> components;
  std::string error;
  // Resolve every owner's request before making any selection changes. A rejected connectivity
  // policy in one owner must not leave another owner changed without a finished undo operation.
  for (Object *object : BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
           bmain, CTX_data_scene(C), CTX_data_view_layer(C), CTX_wm_view3d(C)))
  {
    Curve &curve = *id_cast<Curve *>(object->data);
    if (curve.editnurb->nurbs_edit == nullptr) {
      BKE_report(
          CTX_wm_reports(C), RPT_ERROR, "Mixed native and legacy NURBS selection is unsupported");
      return false;
    }
    if (!bke::nurbs::curve_validate_source(curve, &error)) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
    targets.append({object, curve.editnurb->nurbs_edit->read_snapshot()});
    auto linked = linked_from_selection(targets.last().source, &error);
    if (!linked) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
    components.append(std::move(*linked));
  }
  bool changed = false;
  for (const int i : targets.index_range()) {
    changed |= select_vertices(bmain,
                               *id_cast<Curve *>(targets[i].object->data),
                               targets[i].source,
                               components[i].vertices,
                               bke::nurbs::SelectionAction::Add,
                               std::nullopt,
                               &error);
    if (!error.empty()) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
  }
  return changed;
}

bool select_linked_pick(bContext *C, const int mval[2], const int distance, const bool deselect)
{
  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_ensure_evaluated_depsgraph(C));
  Vector<SelectionTarget> targets;
  Object *hit_object = nullptr;
  int64_t hit_id = 0;
  std::string error;
  if (!find_vertex(vc, mval, distance, targets, hit_object, hit_id, error)) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
    return false;
  }
  for (const SelectionTarget &target : targets) {
    if (target.object != hit_object) {
      continue;
    }
    const auto linked = bke::nurbs::find_linked_components(
        target.source.geometry(),
        {{&hit_id, 1}, {}, {}},
        bke::nurbs::LinkedConnectivity::RequireAgreement,
        &error);
    const bool changed = linked &&
                         select_vertices(*vc.bmain,
                                         *id_cast<Curve *>(target.object->data),
                                         target.source,
                                         linked->vertices,
                                         deselect ? bke::nurbs::SelectionAction::Subtract :
                                                    bke::nurbs::SelectionAction::Add,
                                         std::nullopt,
                                         &error);
    if (!error.empty()) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
    }
    return changed;
  }
  return false;
}

}  // namespace blender::ed::nurbs
