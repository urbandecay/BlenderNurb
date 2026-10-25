/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.hh"
#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_evaluate.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_unit.hh"

#include "ED_numinput.hh"
#include "ED_nurbs.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "curve_intern.hh"

namespace blender {
namespace {

using bke::nurbs::SelectionMode;

struct LoopCutModal {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS loop cut preview");
  std::unique_ptr<ed::nurbs::TopologyPreview> preview;
  std::optional<bke::nurbs::TopologyEditResult> candidate;
  ARegion *region = nullptr;
  void *draw_handle = nullptr;
  Vector<float3> draw_lines;
  int64_t edge_id = 0;
  int cuts = 1;
  double factor = 0, max_error = 1e-9;
  bool sliding = false;
  float2 slide_start = float2(0), slide_direction = float2(200, 0);
  NumInput num{};
  std::string error;
};

static std::optional<bke::nurbs::TopologyEditResult> make_candidate(
    const ed::nurbs::TopologyPreview &preview,
    const int64_t edge,
    const int cuts,
    const double factor,
    const double max_error,
    std::string &error)
{
  auto plan = bke::nurbs::plan_loop_cut(
      preview.source().geometry(), edge, cuts, -factor, max_error, &error);
  if (!plan) {
    return std::nullopt;
  }
  auto cut = bke::nurbs::partition_components(
      preview.source().geometry(), *plan, max_error, &error);
  if (!cut) {
    return std::nullopt;
  }
  // Check shared source boundary agreement before any owner transition or commit.
  if (!bke::nurbs::evaluate_boundaries(*cut->edit.geometry, {}, &error)) {
    return std::nullopt;
  }
  return std::move(cut->edit);
}

static void draw_preview(const bContext *, ARegion *, void *arg)
{
  auto &data = *static_cast<LoopCutModal *>(arg);
  if (data.draw_lines.is_empty()) {
    return;
  }
  const auto depth = GPU_depth_test_get();
  const auto blend = GPU_blend_get();
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_matrix_push();
  GPU_matrix_mul(data.preview->object().object_to_world().ptr());
  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  float color[4];
  ui::theme::get_color_4fv(data.sliding ? TH_EDGE_SELECT : TH_TRANSFORM, color);
  immUniformColor4fv(color);
  GPU_line_width(2);
  immBegin(GPU_PRIM_LINES, uint(data.draw_lines.size()));
  for (const float3 &point : data.draw_lines) {
    immVertex3fv(pos, point);
  }
  immEnd();
  immUnbindProgram();
  GPU_line_width(1);
  GPU_matrix_pop();
  GPU_blend(blend);
  GPU_depth_test(depth);
}

static void update_status(bContext *C, const LoopCutModal &data)
{
  char text[512];
  if (!data.error.empty()) {
    BLI_snprintf(text, sizeof(text), "NURBS Loop Cut: %s | Esc: Cancel", data.error.c_str());
  }
  else if (data.sliding) {
    BLI_snprintf(text,
                 sizeof(text),
                 "NURBS Loop Cut: %d cuts | Slide %.9g | LMB/Enter: Confirm | RMB/Esc: Center",
                 data.cuts,
                 data.factor);
  }
  else {
    BLI_snprintf(text,
                 sizeof(text),
                 "NURBS Loop Cut: %d cuts | Wheel/Number: Cut Count | LMB/Enter: Cut and Slide | "
                 "RMB/Esc: Cancel",
                 data.cuts);
  }
  ED_workspace_status_text(C, text);
}

static void update_candidate(bContext *C, LoopCutModal &data)
{
  data.draw_lines.clear();
  data.candidate.reset();
  data.error.clear();
  if (!data.edge_id) {
    update_status(C, data);
    ED_region_tag_redraw(data.region);
    return;
  }
  data.candidate = make_candidate(
      *data.preview, data.edge_id, data.cuts, data.factor, data.max_error, data.error);
  if (data.candidate) {
    auto boundary = bke::nurbs::evaluate_boundaries(*data.candidate->geometry, {}, &data.error);
    if (!boundary) {
      data.candidate.reset();
    }
    else {
      const Set<int64_t> selected(data.candidate->edges.as_span());
      for (const auto &edge : boundary->edges) {
        if (!selected.contains(edge.edge_id)) {
          continue;
        }
        std::optional<float3> previous;
        for (const auto &sample : edge.samples) {
          const double3 point = boundary->positions[sample.point_index];
          if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
              std::max({std::abs(point.x), std::abs(point.y), std::abs(point.z)}) > FLT_MAX)
          {
            data.error = "Preview exceeds the viewport coordinate range";
            data.candidate.reset();
            data.draw_lines.clear();
            break;
          }
          const float3 proxy(point);
          if (previous) {
            data.draw_lines.append(*previous);
            data.draw_lines.append(proxy);
          }
          previous = proxy;
        }
        if (!data.candidate) {
          break;
        }
      }
    }
  }
  update_status(C, data);
  ED_region_tag_redraw(data.region);
}

static void cleanup(bContext *C, wmOperator *op)
{
  auto *data = static_cast<LoopCutModal *>(op->customdata);
  if (!data) {
    return;
  }
  if (data->draw_handle) {
    ED_region_draw_cb_exit(data->region->runtime->type, data->draw_handle);
  }
  ED_workspace_status_text(C, nullptr);
  ED_region_tag_redraw(data->region);
  MEM_delete(data);
  op->customdata = nullptr;
}

static wmOperatorStatus finish(bContext *C, wmOperator *op, const bool confirm, const bool center)
{
  auto &data = *static_cast<LoopCutModal *>(op->customdata);
  if (confirm && center) {
    data.factor = 0;
    update_candidate(C, data);
  }
  bool changed = false;
  if (confirm) {
    if (!data.candidate) {
      BKE_report(op->reports,
                 RPT_ERROR,
                 data.error.empty() ? "No valid NURBS cut under the cursor" : data.error.c_str());
    }
    else {
      RNA_string_set(op->ptr, "edge_id", std::to_string(data.edge_id).c_str());
      RNA_int_set(op->ptr, "number_cuts", data.cuts);
      RNA_float_set(op->ptr, "factor", float(data.factor));
      char exact[64];
      BLI_snprintf(exact, sizeof(exact), "%.17g", data.factor);
      RNA_string_set(op->ptr, "factor_exact", exact);
      changed = data.preview->commit(
          C, std::move(*data.candidate), data.preview->source().selection().mode, &data.error);
      if (!changed) {
        BKE_report(op->reports, RPT_ERROR, data.error.c_str());
      }
    }
  }
  cleanup(C, op);
  return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static void numeric_init(LoopCutModal &data)
{
  initNumInput(&data.num);
  data.num.flag |= NUM_USE_DOUBLE;
  data.num.idx_max = 0;
  data.num.unit_type[0] = B_UNIT_NONE;
  data.num.val_inc[0] = data.sliding ? 0.1f : 1.0f;
  if (!data.sliding) {
    data.num.val_flag[0] |= NUM_NO_NEGATIVE | NUM_NO_FRACTION | NUM_NO_ZERO;
  }
}

static wmOperatorStatus loopcut_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!CTX_wm_region_view3d(C) || !CTX_wm_region(C)) {
    BKE_report(op->reports, RPT_ERROR, "Interactive NURBS loop cut requires a 3D View region");
    return OPERATOR_CANCELLED;
  }
  auto *data = MEM_new<LoopCutModal>(__func__);
  op->customdata = data;
  data->region = CTX_wm_region(C);
  data->preview = ed::nurbs::TopologyPreview::begin(C, &data->error);
  if (!data->preview) {
    BKE_report(op->reports, RPT_ERROR, data->error.c_str());
    cleanup(C, op);
    return OPERATOR_CANCELLED;
  }
  data->cuts = RNA_int_get(op->ptr, "number_cuts");
  data->max_error = RNA_float_get(op->ptr, "max_error");
  numeric_init(*data);
  data->edge_id = ed::nurbs::topology_edge_under_cursor(
                      C, *data->preview, event->mval, &data->error)
                      .value_or(0);
  if (data->region->runtime->type) {
    data->draw_handle = ED_region_draw_cb_activate(
        data->region->runtime->type, draw_preview, data, REGION_DRAW_POST_VIEW);
  }
  update_candidate(C, *data);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus loopcut_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<LoopCutModal *>(op->customdata);
  if (event->val == KM_PRESS) {
    if (ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE)) {
      return finish(C, op, data.sliding, data.sliding);
    }
    if (ELEM(event->type, LEFTMOUSE, EVT_RETKEY, EVT_PADENTER)) {
      if (data.sliding) {
        return finish(C, op, true, false);
      }
      if (!data.candidate) {
        return finish(C, op, true, false);
      }
      data.sliding = true;
      data.slide_start = float2(event->mval[0], event->mval[1]);
      const auto screen = ed::nurbs::topology_edge_screen(
          C, *data.preview, data.edge_id, &data.error);
      if (screen && math::length_squared((*screen)[1] - (*screen)[0]) > 1) {
        data.slide_direction = ((*screen)[0] - (*screen)[1]) / float(data.cuts + 1);
      }
      numeric_init(data);
      update_status(C, data);
      ED_region_tag_redraw(data.region);
      return OPERATOR_RUNNING_MODAL;
    }
    if (!data.sliding && ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
      data.cuts = std::clamp(data.cuts + (event->type == WHEELUPMOUSE ? 1 : -1), 1, 1000);
      numeric_init(data);
      update_candidate(C, data);
      return OPERATOR_RUNNING_MODAL;
    }
    if (handleNumInput(C, &data.num, event)) {
      double value = data.sliding ? data.factor : data.cuts;
      if (applyNumInput(&data.num, &value)) {
        if (data.sliding) {
          data.factor = value;
        }
        else {
          if (std::isfinite(value)) {
            data.cuts = int(std::clamp(value, 1.0, 1000.0));
          }
        }
      }
      update_candidate(C, data);
      return OPERATOR_RUNNING_MODAL;
    }
  }
  if (ELEM(event->type, MOUSEMOVE, INBETWEEN_MOUSEMOVE)) {
    if (!data.sliding) {
      const int64_t edge = ed::nurbs::topology_edge_under_cursor(
                               C, *data.preview, event->mval, &data.error)
                               .value_or(0);
      if (edge != data.edge_id) {
        data.edge_id = edge;
        update_candidate(C, data);
      }
    }
    else if (!hasNumInput(&data.num)) {
      const float2 delta = float2(event->mval[0], event->mval[1]) - data.slide_start;
      data.factor = std::clamp(double(math::dot(delta, data.slide_direction) /
                                      math::length_squared(data.slide_direction)),
                               -0.999999,
                               0.999999);
      update_candidate(C, data);
    }
    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void loopcut_cancel(bContext *C, wmOperator *op)
{
  cleanup(C, op);
}

static wmOperatorStatus loopcut_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto preview = ed::nurbs::TopologyPreview::begin(C, &error);
  if (!preview) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  const std::string edge_text = RNA_string_get(op->ptr, "edge_id");
  int64_t edge = 0;
  const auto parsed = std::from_chars(edge_text.data(), edge_text.data() + edge_text.size(), edge);
  if (parsed.ec != std::errc() || parsed.ptr != edge_text.data() + edge_text.size() || edge <= 0) {
    BKE_report(op->reports,
               RPT_ERROR,
               "NURBS loop cut execution needs a source seed edge identity; invoke in a 3D View "
               "to pick it");
    return OPERATOR_CANCELLED;
  }
  double factor = RNA_float_get(op->ptr, "factor");
  const std::string exact = RNA_string_get(op->ptr, "factor_exact");
  if (!exact.empty()) {
    char *end = nullptr;
    const double precise = std::strtod(exact.c_str(), &end);
    if (end != exact.c_str() + exact.size() || !std::isfinite(precise)) {
      BKE_report(op->reports, RPT_ERROR, "NURBS loop cut slide value is invalid");
      return OPERATOR_CANCELLED;
    }
    // A visible redo-property edit supersedes the original precise replay value.
    if (!RNA_struct_property_is_set(op->ptr, "factor") || float(precise) == float(factor)) {
      factor = precise;
    }
  }
  LoopCutModal data;
  data.preview = std::move(preview);
  data.cuts = RNA_int_get(op->ptr, "number_cuts");
  auto candidate = make_candidate(
      *data.preview, edge, data.cuts, factor, RNA_float_get(op->ptr, "max_error"), error);
  if (!candidate || !data.preview->commit(
                        C, std::move(*candidate), data.preview->source().selection().mode, &error))
  {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

}  // namespace

void CURVE_OT_loopcut_slide(wmOperatorType *ot)
{
  ot->name = "Loop Cut and Slide NURBS";
  ot->idname = "CURVE_OT_loopcut_slide";
  ot->description =
      "Partition connected NURBS faces with shared source edges and slide the new cuts";
  ot->poll = ed::nurbs::topology_edit_poll;
  ot->invoke = loopcut_invoke;
  ot->modal = loopcut_modal;
  ot->exec = loopcut_exec;
  ot->cancel = loopcut_cancel;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
  auto *prop = RNA_def_int(ot->srna, "number_cuts", 1, 1, 1000, "Number of Cuts", "", 1, 100);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_float(ot->srna,
                "factor",
                0,
                -1,
                1,
                "Factor",
                "Slide the cuts in their source parameter direction",
                -1,
                1);
  prop = RNA_def_string(ot->srna,
                        "edge_id",
                        nullptr,
                        64,
                        "Seed Edge",
                        "Source edge identity for execution and redo");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_string(
      ot->srna, "factor_exact", nullptr, 128, "Exact Factor", "Double source slide replay value");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
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
