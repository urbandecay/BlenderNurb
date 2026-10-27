/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include <cfloat>
#include <cmath>
#include <cstdlib>

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLI_time.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_nurbs_debug.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_unit.hh"

#include "ED_numinput.hh"
#include "ED_nurbs.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_transform.hh"
#include "ED_util.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "curve_intern.hh"

namespace blender {
namespace {

constexpr double mouse_pixel_margin = 5.0;

struct BevelModal {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS bevel preview");
  std::unique_ptr<ed::nurbs::TopologyPreview> preview;
  ARegion *region = nullptr;
  void *draw_handle = nullptr;
  float center[2] = {};
  double start_length = 0, start_offset = 0, pixel_size = 1;
  bool precision = false, use_snap = false, valid = false;
  bke::nurbs::BevelOptions options;
  bke::nurbs::BevelOffsetInfo offset_info;
  std::optional<bke::nurbs::BevelOptions> visible_options;
  NumInput num{};
  std::string error;
  std::shared_ptr<const bke::nurbs::ObjectEvaluation> evaluation;
  double geometry_error_bound = 0;
};

struct BevelCandidate {
  std::shared_ptr<const bke::nurbs::ObjectEvaluation> evaluation;
  double geometry_error_bound;
};

static std::optional<BevelCandidate> candidate(
    const ed::nurbs::TopologyPreview &preview,
    const bke::nurbs::BevelOptions &options,
    std::string &error,
    bke::nurbs::BevelOffsetInfo *r_offset_info = nullptr,
    const bke::nurbs::ObjectEvaluation *previous = nullptr)
{
  const double candidate_start = BLI_time_now_seconds();
  const size_t source_faces = preview.source().geometry().faces.count();
  const auto &selection = preview.source().selection();
  const Vector<int64_t> edges(selection.edges.begin(), selection.edges.end());
  const double bevel_start = BLI_time_now_seconds();
  auto result = bke::nurbs::bevel_edges(
      preview.source().geometry(), edges, options, &error, r_offset_info);
  const double bevel_ms = (BLI_time_now_seconds() - bevel_start) * 1000.0;
  if (!result) {
    NURBS_DEBUG_LOG(
        "bevel candidate failed: bevel %.3f ms, total %.3f ms; %d selected edges, "
        "%zu source faces, width %.9g, segments %d; %s",
        bevel_ms,
        (BLI_time_now_seconds() - candidate_start) * 1000.0,
        int(edges.size()),
        source_faces,
        options.offset,
        options.segments,
        error.c_str());
    return std::nullopt;
  }
  // Keep the evaluated immutable candidate for the actual viewport handoff.
  const double geometry_error_bound = result->geometry_error_bound;
  const int result_faces = result->geometry->faces.count();
  const double snapshot_start = BLI_time_now_seconds();
  auto snapshot = bke::nurbs::edit_snapshot_from_topology_result(
      std::move(*result), selection, selection.mode, &error);
  const double snapshot_ms = (BLI_time_now_seconds() - snapshot_start) * 1000.0;
  if (!snapshot) {
    NURBS_DEBUG_LOG(
        "bevel candidate failed: bevel %.3f ms, snapshot %.3f ms, total %.3f ms; "
        "%d selected edges; %s",
        bevel_ms,
        snapshot_ms,
        (BLI_time_now_seconds() - candidate_start) * 1000.0,
        int(edges.size()),
        error.c_str());
    return std::nullopt;
  }
  const double edit_start = BLI_time_now_seconds();
  const auto edit = bke::nurbs::EditData::create(std::move(*snapshot), &error);
  const double edit_ms = (BLI_time_now_seconds() - edit_start) * 1000.0;
  if (!edit) {
    NURBS_DEBUG_LOG(
        "bevel candidate failed: bevel %.3f ms, snapshot %.3f ms, edit data %.3f ms, "
        "total %.3f ms; %s",
        bevel_ms,
        snapshot_ms,
        edit_ms,
        (BLI_time_now_seconds() - candidate_start) * 1000.0,
        error.c_str());
    return std::nullopt;
  }
  const double preview_start = BLI_time_now_seconds();
  auto evaluation = std::make_shared<bke::nurbs::ObjectEvaluation>();
  evaluation->options = bke::nurbs::curve_preview_options(
      *id_cast<Curve *>(preview.object().data));
  evaluation->result = bke::nurbs::evaluate_mesh(
      edit->read_snapshot(), evaluation->options, &error, previous);
  const double preview_ms = (BLI_time_now_seconds() - preview_start) * 1000.0;
  if (!evaluation->result) {
    NURBS_DEBUG_LOG(
        "bevel candidate failed: bevel %.3f ms, snapshot %.3f ms, edit data %.3f ms, "
        "preview %.3f ms, total %.3f ms; %s",
        bevel_ms,
        snapshot_ms,
        edit_ms,
        preview_ms,
        (BLI_time_now_seconds() - candidate_start) * 1000.0,
        error.c_str());
    return std::nullopt;
  }
  NURBS_DEBUG_LOG(
      "bevel candidate: bevel %.3f ms, snapshot %.3f ms, edit data %.3f ms, preview "
      "%.3f ms, total %.3f ms; %d selected edges, %zu source faces -> %d result faces, "
      "width %.9g, %d segments",
      bevel_ms,
      snapshot_ms,
      edit_ms,
      preview_ms,
      (BLI_time_now_seconds() - candidate_start) * 1000.0,
      int(edges.size()),
      source_faces,
      result_faces,
      options.offset,
      options.segments);
  return BevelCandidate{std::move(evaluation), geometry_error_bound};
}

static int modal_action_value(const wmOperatorType &type, const char *identifier)
{
  int value = 0;
  if (type.modalkeymap && type.modalkeymap->modal_items) {
    RNA_enum_value_from_id(
        static_cast<const EnumPropertyItem *>(type.modalkeymap->modal_items), identifier, &value);
  }
  return value;
}

static void status(bContext *C, wmOperator *op, const BevelModal &data)
{
  char text[512];
  if (!data.error.empty()) {
    if (data.visible_options) {
      BLI_snprintf(text,
                   sizeof(text),
                   "NURBS Bevel: %s | Showing previous valid width %.9g (%d segments) | "
                   "Reduce width or Esc: Cancel",
                   data.error.c_str(),
                   data.visible_options->offset,
                   data.visible_options->segments);
    }
    else {
      BLI_snprintf(
          text, sizeof(text), "NURBS Bevel: %s | Reduce width or Esc: Cancel", data.error.c_str());
    }
  }
  else {
    char width[128];
    if (data.offset_info.offset < data.options.offset) {
      BLI_snprintf(width,
                   sizeof(width),
                   "%.9g (limited from %.9g)",
                   data.offset_info.offset,
                   data.options.offset);
    }
    else {
      BLI_snprintf(width, sizeof(width), "%.9g", data.offset_info.offset);
    }
    BLI_snprintf(text,
                 sizeof(text),
                 "Width: %s | Segments: %d | Clamp: %s%s%s",
                 width,
                 data.options.segments,
                 data.options.clamp_overlap ? "On" : "Off",
                 data.offset_info.boundary_blend_corners ? " | Corners: Boundary Blend (G0)" : "",
                 data.offset_info.curved_blend_strips ? " | Curved Blend" : "");
  }
  ED_area_status_text(CTX_wm_area(C), text);
  WorkspaceStatus workspace_status(C);
  workspace_status.opmodal(IFACE_("Confirm"), op->type, modal_action_value(*op->type, "CONFIRM"));
  workspace_status.opmodal(IFACE_("Cancel"), op->type, modal_action_value(*op->type, "CANCEL"));
  workspace_status.opmodal(IFACE_("Precision"),
                           op->type,
                           modal_action_value(*op->type, "PRECISION_ON"),
                           data.precision);
  workspace_status.opmodal(
      IFACE_("Snap"), op->type, modal_action_value(*op->type, "SNAP_ON"), data.use_snap);
  workspace_status.opmodal(
      IFACE_("Segments +"), op->type, modal_action_value(*op->type, "SEGMENTS_UP"));
  workspace_status.opmodal(
      IFACE_("Segments -"), op->type, modal_action_value(*op->type, "SEGMENTS_DOWN"));
  workspace_status.opmodal(IFACE_("Clamp"),
                           op->type,
                           modal_action_value(*op->type, "CLAMP_OVERLAP_TOGGLE"),
                           data.options.clamp_overlap);
}

static void update(bContext *C, wmOperator *op, BevelModal &data)
{
  const double update_start = BLI_time_now_seconds();
  data.error.clear();
  bke::nurbs::BevelOffsetInfo offset_info;
  auto result = candidate(
      *data.preview, data.options, data.error, &offset_info, data.evaluation.get());
  const double candidate_ms = (BLI_time_now_seconds() - update_start) * 1000.0;
  const double preview_commit_start = BLI_time_now_seconds();
  data.valid = result &&
               (data.options.offset == 0 ?
                    data.preview->cancel_preview(C, &data.error) :
                    data.preview->update_evaluated_preview(C, result->evaluation, &data.error));
  const double preview_commit_ms = (BLI_time_now_seconds() - preview_commit_start) * 1000.0;
  NURBS_DEBUG_LOG(
      "bevel modal update: %.3f ms total; candidate %.3f ms, preview commit %.3f ms; "
      "valid %d, width %.9g, segments %d, clamp %d%s",
      (BLI_time_now_seconds() - update_start) * 1000.0,
      candidate_ms,
      preview_commit_ms,
      int(data.valid),
      data.options.offset,
      data.options.segments,
      int(data.options.clamp_overlap),
      data.error.empty() ? "" : data.error.c_str());
  if (data.valid) {
    data.evaluation = result->evaluation;
    data.geometry_error_bound = result->geometry_error_bound;
    data.offset_info = offset_info;
    data.visible_options = data.options.offset == 0 ? std::nullopt : std::optional(data.options);
    if (data.visible_options) {
      data.visible_options->offset = offset_info.offset;
    }
  }
  /* Candidate construction and commit are atomic. Preserve the last accepted preview when a
   * new width/segment request fails, rather than flashing back to the original sharp corner.
   * Confirmation still rejects the failed request; only explicit cancel/zero width restores
   * the original source. */
  status(C, op, data);
  ED_region_tag_redraw(data.region);
}

static void cleanup(bContext *C, wmOperator *op)
{
  auto *data = static_cast<BevelModal *>(op->customdata);
  if (!data) {
    return;
  }
  if (data->draw_handle) {
    ED_region_draw_cb_exit(data->region->runtime->type, data->draw_handle);
  }
  ED_workspace_status_text(C, nullptr);
  ED_area_status_text(CTX_wm_area(C), nullptr);
  ED_region_tag_redraw(data->region);
  MEM_delete(data);
  op->customdata = nullptr;
}

static void bevel_cancel(bContext *C, wmOperator *op)
{
  auto *data = static_cast<BevelModal *>(op->customdata);
  if (data) {
    std::string error;
    if (!data->preview->cancel_preview(C, &error)) {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
    }
  }
  cleanup(C, op);
}

static double read_offset(wmOperator *op)
{
  const double offset = RNA_float_get(op->ptr, "offset");
  const std::string exact = RNA_string_get(op->ptr, "offset_exact");
  if (!exact.empty()) {
    char *end = nullptr;
    const double parsed = std::strtod(exact.c_str(), &end);
    if (end != exact.c_str() + exact.size() || !std::isfinite(parsed)) {
      return NAN;
    }
    if (!RNA_struct_property_is_set(op->ptr, "offset") || float(parsed) == float(offset)) {
      return parsed;
    }
  }
  return offset;
}

static void report_planar_charts(wmOperator *op, const bke::nurbs::BevelOffsetInfo &info)
{
  if (info.planar_charts) {
    BKE_reportf(op->reports,
                RPT_INFO,
                "NURBS bevel: %d flat faces reparameterized; source geometry error bound %.17g",
                info.planar_charts,
                info.chart_error_bound);
  }
}

static wmOperatorStatus bevel_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto preview = ed::nurbs::TopologyPreview::begin(C, &error);
  if (!preview) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  const bke::nurbs::BevelOptions options{read_offset(op),
                                         RNA_int_get(op->ptr, "segments"),
                                         RNA_float_get(op->ptr, "max_error"),
                                         RNA_boolean_get(op->ptr, "clamp_overlap")};
  if (options.offset == 0) {
    return OPERATOR_CANCELLED;
  }
  bke::nurbs::BevelOffsetInfo offset_info;
  auto result = candidate(*preview, options, error, &offset_info);
  if (!result ||
      !preview->commit_evaluated(C, result->evaluation, result->geometry_error_bound, &error))
  {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  if (offset_info.offset < options.offset) {
    BKE_reportf(op->reports, RPT_INFO, "NURBS bevel width limited to %.17g", offset_info.offset);
  }
  if (offset_info.boundary_blend_corners) {
    BKE_reportf(
        op->reports,
        RPT_INFO,
        "NURBS bevel: %d boundary-interpolating corner blends (G0); tangency is not certified",
        offset_info.boundary_blend_corners);
  }
  report_planar_charts(op, offset_info);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus bevel_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!CTX_wm_region_view3d(C) || !CTX_wm_region(C)) {
    return bevel_exec(C, op);
  }
  auto *data = MEM_new<BevelModal>(__func__);
  op->customdata = data;
  data->region = CTX_wm_region(C);
  data->preview = ed::nurbs::TopologyPreview::begin(C, &data->error);
  if (!data->preview) {
    BKE_report(op->reports, RPT_ERROR, data->error.c_str());
    cleanup(C, op);
    return OPERATOR_CANCELLED;
  }
  data->options.segments = RNA_int_get(op->ptr, "segments");
  data->options.max_error = RNA_float_get(op->ptr, "max_error");
  data->options.clamp_overlap = RNA_boolean_get(op->ptr, "clamp_overlap");
  float center3d[3];
  if (!ed::transform::calculateTransformCenter(
          C, V3D_AROUND_CENTER_MEDIAN, center3d, data->center))
  {
    BKE_report(op->reports, RPT_ERROR, "Select a shared NURBS edge to bevel");
    cleanup(C, op);
    return OPERATOR_CANCELLED;
  }
  const float scale = mat4_to_scale(data->preview->object().object_to_world().ptr());
  data->pixel_size = ED_view3d_pixel_size(CTX_wm_region_view3d(C), center3d) / scale;
  if (!(data->pixel_size > 0) || !std::isfinite(data->pixel_size)) {
    BKE_report(op->reports, RPT_ERROR, "NURBS bevel cannot use this object/view scale");
    cleanup(C, op);
    return OPERATOR_CANCELLED;
  }
  /* Match mesh bevel's five-pixel launch margin. Later modifier changes rebase at the
   * current width and mouse position, without adding another dead zone. */
  data->start_length = std::hypot(event->mval[0] - data->center[0],
                                  event->mval[1] - data->center[1]) +
                       mouse_pixel_margin;
  initNumInput(&data->num);
  data->num.flag |= NUM_USE_DOUBLE;
  data->num.idx_max = 0;
  data->num.val_flag[0] |= NUM_NO_NEGATIVE;
  data->num.unit_sys = CTX_data_scene(C)->unit.system;
  data->num.unit_type[0] = B_UNIT_LENGTH;
  data->num.val_inc[0] = 0.1f;
  update(C, op, *data);
  if (!data->valid) {
    BKE_report(op->reports, RPT_ERROR, data->error.c_str());
    bevel_cancel(C, op);
    return OPERATOR_CANCELLED;
  }
  data->draw_handle = ED_region_draw_cb_activate(data->region->runtime->type,
                                                 ED_region_draw_mouse_line_cb,
                                                 data->center,
                                                 REGION_DRAW_POST_PIXEL);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus bevel_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<BevelModal *>(op->customdata);
  if (event->type == EVT_MODAL_MAP) {
    const char *action = "";
    if (op->type->modalkeymap && op->type->modalkeymap->modal_items) {
      if (!RNA_enum_identifier(
              static_cast<const EnumPropertyItem *>(op->type->modalkeymap->modal_items),
              event->val,
              &action))
      {
        return OPERATOR_RUNNING_MODAL;
      }
    }
    if (STREQ(action, "CANCEL")) {
      bevel_cancel(C, op);
      return OPERATOR_CANCELLED;
    }
    if (STREQ(action, "CONFIRM")) {
      if (!data.valid || data.options.offset == 0) {
        if (!data.valid && !data.error.empty()) {
          BKE_report(op->reports, RPT_ERROR, data.error.c_str());
        }
        bevel_cancel(C, op);
        return OPERATOR_CANCELLED;
      }
      if (!data.preview->commit_evaluated(
              C, data.evaluation, data.geometry_error_bound, &data.error))
      {
        BKE_report(op->reports, RPT_ERROR, data.error.c_str());
        bevel_cancel(C, op);
        return OPERATOR_CANCELLED;
      }
      RNA_float_set(op->ptr, "offset", float(data.options.offset));
      char exact[64];
      BLI_snprintf(exact, sizeof(exact), "%.17g", data.options.offset);
      RNA_string_set(op->ptr, "offset_exact", exact);
      RNA_int_set(op->ptr, "segments", data.options.segments);
      RNA_boolean_set(op->ptr, "clamp_overlap", data.options.clamp_overlap);
      report_planar_charts(op, data.offset_info);
      cleanup(C, op);
      return OPERATOR_FINISHED;
    }
    if (STREQ(action, "SEGMENTS_UP") || STREQ(action, "SEGMENTS_DOWN")) {
      data.options.segments = std::clamp(
          data.options.segments + (STREQ(action, "SEGMENTS_UP") ? 1 : -1), 1, 100);
      update(C, op, data);
      return OPERATOR_RUNNING_MODAL;
    }
    if (STREQ(action, "CLAMP_OVERLAP_TOGGLE")) {
      data.options.clamp_overlap = !data.options.clamp_overlap;
      update(C, op, data);
      return OPERATOR_RUNNING_MODAL;
    }
    if (STREQ(action, "SNAP_ON") || STREQ(action, "SNAP_OFF") || STREQ(action, "PRECISION_ON") ||
        STREQ(action, "PRECISION_OFF"))
    {
      /* Modifiers already held when Ctrl+B was invoked do not enable modal snapping.
       * Consume the same explicit actions as mesh bevel, including user remappings. */
      data.start_offset = data.options.offset;
      data.start_length = std::hypot(event->mval[0] - data.center[0],
                                     event->mval[1] - data.center[1]);
      if (data.start_offset == 0) {
        data.start_length += mouse_pixel_margin;
      }
      if (STREQ(action, "SNAP_ON") || STREQ(action, "SNAP_OFF")) {
        data.use_snap = STREQ(action, "SNAP_ON");
      }
      else {
        data.precision = STREQ(action, "PRECISION_ON");
      }
      status(C, op, data);
    }
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->val == KM_PRESS && handleNumInput(C, &data.num, event)) {
    applyNumInput(&data.num, &data.options.offset);
    update(C, op, data);
    return OPERATOR_RUNNING_MODAL;
  }
  /* Like mesh bevel, evaluate the final mouse move in the queued batch. Intermediate moves
   * can be numerous, and rebuilding geometry for each one causes a delayed preview. */
  if (event->type == MOUSEMOVE && !hasNumInput(&data.num)) {
    const double length = std::hypot(event->mval[0] - data.center[0],
                                     event->mval[1] - data.center[1]);
    data.options.offset = std::max(0.0,
                                   data.start_offset + (length - data.start_length) *
                                                           data.pixel_size *
                                                           (data.precision ? 0.1 : 1));
    if (data.use_snap) {
      const double step = data.precision ? 0.01 : 0.1;
      data.options.offset = std::round(data.options.offset / step) * step;
    }
    update(C, op, data);
  }
  return OPERATOR_RUNNING_MODAL;
}

}  // namespace

void CURVE_OT_bevel(wmOperatorType *ot)
{
  ot->name = "Bevel NURBS Edges";
  ot->idname = "CURVE_OT_bevel";
  ot->description = "Bevel shared NURBS edges with chamfer or smooth rational surface strips";
  ot->poll = ed::nurbs::topology_edit_poll;
  ot->invoke = bevel_invoke;
  ot->exec = bevel_exec;
  ot->modal = bevel_modal;
  ot->cancel = bevel_cancel;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_GRAB_CURSOR_XY;
  RNA_def_float_distance(ot->srna,
                         "offset",
                         0.1f,
                         0,
                         FLT_MAX,
                         "Width",
                         "Perpendicular width along each adjacent face",
                         0,
                         100);
  RNA_def_int(ot->srna,
              "segments",
              1,
              1,
              100,
              "Segments",
              "One makes a chamfer; more partition a smooth rounded NURBS surface",
              1,
              32);
  auto *prop = RNA_def_string(ot->srna,
                              "offset_exact",
                              nullptr,
                              64,
                              "Precise Width",
                              "Source-precision numeric width for replay and Python execution");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_boolean(ot->srna,
                  "clamp_overlap",
                  true,
                  "Clamp Overlap",
                  "Limit bevel width before neighboring NURBS boundaries collide");
  WM_operatortype_props_advanced_begin(ot);
  RNA_def_float(ot->srna,
                "max_error",
                1e-9f,
                0,
                FLT_MAX,
                "Maximum Error",
                "Maximum allowed source construction roundoff in object units",
                0,
                0.001f);
}

}  // namespace blender
