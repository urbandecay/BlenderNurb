/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <cmath>
#include <cstdlib>

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "IMB_imbuf_types.hh"
#include "IMB_interp.hh"

#include "ED_image.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "ED_mesh.hh" /* for face mask functions */

#include "UI_interface_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "IMB_colormanagement.hh"

#include "paint_intern.hh"

#include "mesh/sculpt_color.hh"
#include "mesh/sculpt_intern.hh"

/* -------------------------------------------------------------------- */
/** \name Sample Color Operator
 * \{ */

namespace blender::ed::sculpt_paint {

static std::optional<float3> sample_mesh_attribute_color(ViewContext &vc,
                                                         Object &object,
                                                         int2 mval)
{
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::GAttributeReader color_attribute = color::active_color_attribute(mesh);

  std::optional<ActiveElementInfo> active_element = active_element_info_get(vc, float2(mval));

  if (!color_attribute || !active_element || !std::holds_alternative<int>(active_element->vert)) {
    return std::nullopt;
  }

  const GVArraySpan colors = *color_attribute;
  return color::color_vert_get(faces,
                               corner_verts,
                               vert_to_face_map,
                               colors,
                               color_attribute.domain,
                               std::get<int>(active_element->vert))
      .xyz();
}

static void apply_sampled_color(Main &bMain,
                                Paint &paint,
                                const float3 &sampled_color,
                                const bool use_palette)
{
  if (use_palette) {
    Palette *palette = BKE_paint_palette(&paint);
    if (!palette) {
      palette = BKE_palette_add(&bMain, "Palette");
      BKE_paint_palette_set(&paint, palette);
    }

    PaletteColor *color = BKE_palette_color_add(palette);
    palette->active_color = palette->colors.count() - 1;
    BKE_palette_color_set(color, sampled_color);
  }
  else {
    Brush *br = BKE_paint_brush(&paint);
    BKE_brush_color_set(&paint, br, sampled_color);
  }
}

static float3 paint_sample_color(bContext *C,
                                 ARegion *region,
                                 const int2 mval,
                                 const bool use_merged_texture)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Paint *paint = BKE_paint_get_active_from_context(C);
  const PaintMode mode = paint->runtime->paint_mode;

  SpaceImage *sima = CTX_wm_space_image(C);
  const View3D *v3d = CTX_wm_view3d(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  std::optional<float3> sampled_color;
  if (v3d && !use_merged_texture) {
    ViewLayer *view_layer = CTX_data_view_layer(C);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    Object *ob = BKE_view_layer_active_object_get(view_layer);

    if (ELEM(mode, PaintMode::Sculpt, PaintMode::Vertex)) {
      BKE_sculpt_update_object_for_edit(depsgraph, ob, false);
      sampled_color = sample_mesh_attribute_color(vc, *ob, mval);
    }
  }
  else if (sima != nullptr) {
    /* Sample from the active image buffer. The sampled color is in
     * Linear Scene Reference Space. */
    float3 rgba_f;
    bool is_data;
    if (ED_space_image_color_sample(sima, region, mval, rgba_f, &is_data)) {
      sampled_color = rgba_f;
    }
  }

  /* No sample found; sample directly from the GPU front buffer. */
  if (!sampled_color) {
    float3 rgb_f;
    WM_window_pixels_read_sample(C,
                                 CTX_wm_window(C),
                                 int2(mval.x + region->winrct.xmin, mval.y + region->winrct.ymin),
                                 rgb_f);

    /* The sampled color is in display colorspace, convert to scene linear. */
    const ColorManagedDisplay *display = IMB_colormanagement_display_get_named(
        scene->display_settings.display_device);
    IMB_colormanagement_display_to_scene_linear_v3(rgb_f, display);
    sampled_color = rgb_f;
  }

  BLI_assert(sampled_color);
  return sampled_color.value_or(float3(0.0f));
}

struct SampleColorData {
  bool show_cursor;
  short launch_event;
  float3 initial_color;
  bool sample_palette;
  float3 accum_color;
  int num_samples;
};

static void sample_color_update_header(SampleColorData *data, bContext *C)
{
  char msg[UI_MAX_DRAW_STR];
  ScrArea *area = CTX_wm_area(C);

  if (area) {
    SNPRINTF_UTF8(msg,
                  IFACE_("Sample color for %s"),
                  !data->sample_palette ?
                      IFACE_("Brush. Use Left Click to sample for palette instead") :
                      IFACE_("Palette. Use Left Click to sample more colors"));
    ED_workspace_status_text(C, msg);
  }
}

static float3 sample_average_color(SampleColorData *data, const float3 &sampled_color)
{
  if (data == nullptr) {
    return sampled_color;
  }

  data->accum_color += sampled_color;
  data->num_samples++;

  return data->accum_color / float(data->num_samples);
}

static wmOperatorStatus sample_color_exec(bContext *C, wmOperator *op)
{
  Scene &scene = *CTX_data_scene(C);
  Object &object = *CTX_data_active_object(C);
  SampleColorData *data = static_cast<SampleColorData *>(op->customdata);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);
  ARegion *region = CTX_wm_region(C);
  wmWindow *win = CTX_wm_window(C);

  const bool use_merged_texture = RNA_boolean_get(op->ptr, "merged");
  const PaintMode mode = paint->runtime->paint_mode;
  if (ELEM(mode, PaintMode::Vertex, PaintMode::Sculpt) && !use_merged_texture) {
    if (!color_supported_check(scene, object, op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }

  const bool show_cursor = ((paint->flags & PAINT_SHOW_BRUSH) != 0);
  paint->flags &= ~PAINT_SHOW_BRUSH;

  /* force redraw without cursor */
  WM_paint_cursor_tag_redraw(win, region);
  WM_redraw_windows(C);

  int2 location;
  RNA_int_get_array(op->ptr, "location", location);
  location.x = std::clamp(location.x, 0, int(region->winx));
  location.y = std::clamp(location.y, 0, int(region->winy));

  const bool use_palette = RNA_boolean_get(op->ptr, "palette");

  const float3 sampled_color = paint_sample_color(C, region, location, use_merged_texture);
  const float3 average_color = sample_average_color(data, sampled_color);
  apply_sampled_color(*CTX_data_main(C), *paint, average_color, use_palette);

  if (show_cursor) {
    paint->flags |= PAINT_SHOW_BRUSH;
  }

  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus sample_color_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Scene &scene = *CTX_data_scene(C);
  Object &object = *CTX_data_active_object(C);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);
  ARegion *region = CTX_wm_region(C);
  wmWindow *win = CTX_wm_window(C);

  const bool use_merged_texture = RNA_boolean_get(op->ptr, "merged");
  const PaintMode mode = paint->runtime->paint_mode;
  if (ELEM(mode, PaintMode::Vertex, PaintMode::Sculpt) && !use_merged_texture) {
    if (!color_supported_check(scene, object, op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }

  SampleColorData *data = MEM_new<SampleColorData>(__func__);

  data->launch_event = WM_userdef_event_type_from_keymap_type(event->type);
  data->show_cursor = ((paint->flags & PAINT_SHOW_BRUSH) != 0);
  data->initial_color = BKE_brush_color_get(paint, brush);
  data->sample_palette = false;
  op->customdata = data;
  paint->flags &= ~PAINT_SHOW_BRUSH;

  data->accum_color = float3(0.0f);
  data->num_samples = 0;

  sample_color_update_header(data, C);

  WM_event_add_modal_handler(C, op);

  /* force redraw without cursor */
  WM_paint_cursor_tag_redraw(win, region);
  WM_redraw_windows(C);

  RNA_int_set_array(op->ptr, "location", event->mval);

  int2 mval(std::clamp(event->mval[0], 0, int(region->winx)),
            std::clamp(event->mval[1], 0, int(region->winy)));
  const float3 sampled_color = paint_sample_color(C, region, mval, use_merged_texture);
  const float3 average_color = sample_average_color(data, sampled_color);
  /* On initial invoke, we never sample to the palette. */
  apply_sampled_color(*CTX_data_main(C), *paint, average_color, false);

  WM_cursor_modal_set(win, WM_CURSOR_EYEDROPPER);

  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus sample_color_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  SampleColorData *data = static_cast<SampleColorData *>(op->customdata);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);
  Main &bmain = *CTX_data_main(C);

  if ((event->type == data->launch_event) && (event->val == KM_RELEASE)) {
    if (data->show_cursor) {
      paint->flags |= PAINT_SHOW_BRUSH;
    }

    if (data->sample_palette) {
      /* If we sampled any colors to the palette, reset the brush to its initial color */
      BKE_brush_color_set(paint, brush, data->initial_color);
      RNA_boolean_set(op->ptr, "palette", true);
      WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
    }
    WM_cursor_modal_restore(CTX_wm_window(C));
    MEM_delete(data);
    ED_workspace_status_text(C, nullptr);

    return OPERATOR_FINISHED;
  }
  ARegion *region = CTX_wm_region(C);
  int2 mval(std::clamp(event->mval[0], 0, int(region->winx)),
            std::clamp(event->mval[1], 0, int(region->winy)));

  const bool use_merged_texture = RNA_boolean_get(op->ptr, "merged");

  switch (event->type) {
    case MOUSEMOVE: {
      RNA_int_set_array(op->ptr, "location", event->mval);
      const float3 sampled_color = paint_sample_color(C, region, mval, use_merged_texture);
      const float3 average_color = sample_average_color(data, sampled_color);
      apply_sampled_color(bmain, *paint, average_color, false);
      WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
      break;
    }

    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        RNA_int_set_array(op->ptr, "location", event->mval);
        const float3 sampled_color = paint_sample_color(C, region, mval, use_merged_texture);
        const float3 average_color = sample_average_color(data, sampled_color);
        apply_sampled_color(bmain, *paint, average_color, true);
        if (!data->sample_palette) {
          data->sample_palette = true;
          sample_color_update_header(data, C);
          BKE_report(op->reports, RPT_INFO, "Sampling color for palette");
        }
        WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
      }
      break;
    default: {
      break;
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

static bool sample_color_poll(bContext *C)
{
  /* Requires a window with pixel-data. */
  if (G.background) {
    return false;
  }
  return (image_paint_poll_ignore_tool(C) || vertex_paint_poll_ignore_tool(C) ||
          sculpt_mode_poll(C));
}

void PAINT_OT_sample_color(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Sample Color";
  ot->idname = "PAINT_OT_sample_color";
  ot->description = "Use the mouse to sample a color in the image";

  /* API callbacks. */
  ot->exec = sample_color_exec;
  ot->invoke = sample_color_invoke;
  ot->modal = sample_color_modal;
  ot->poll = sample_color_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  /* properties */
  PropertyRNA *prop;

  prop = RNA_def_int_vector(
      ot->srna, "location", 2, nullptr, 0, INT_MAX, "Location", "", 0, 16384);
  RNA_def_property_flag(prop, (PROP_SKIP_SAVE | PROP_HIDDEN));

  RNA_def_boolean(ot->srna, "merged", false, "Sample Merged", "Sample the output display color");
  RNA_def_boolean(ot->srna, "palette", false, "Add to Palette", "");
}

}  // namespace blender::ed::sculpt_paint

/** \} */
