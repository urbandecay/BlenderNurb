/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "BLI_math_base.h"
#include "BLI_string_utf8_symbols.h"

#include "BLT_translation.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "IMB_imbuf.hh"

#include "WM_types.hh"

namespace blender {

static const EnumPropertyItem prop_direction_items[] = {
    {0, "ADD", ICON_ADD, "Add", "Add effect of brush"},
    {BRUSH_DIR_IN, "SUBTRACT", ICON_REMOVE, "Subtract", "Subtract effect of brush"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem sculpt_stroke_method_items[] = {
    {BRUSH_STROKE_DOTS, "DOTS", 0, "Dots", "Apply paint on each mouse move step"},
    {BRUSH_STROKE_DRAG_DOT,
     "DRAG_DOT",
     0,
     "Drag Dot",
     "Allows a single dot to be carefully positioned"},
    {BRUSH_STROKE_SPACE,
     "SPACE",
     0,
     "Space",
     "Limit brush application to the distance specified by spacing"},
    {BRUSH_STROKE_AIRBRUSH,
     "AIRBRUSH",
     0,
     "Airbrush",
     "Keep applying paint effect while holding mouse (spray)"},
    {BRUSH_STROKE_ANCHORED,
     "ANCHORED",
     0,
     "Anchored",
     "Keep the brush anchored to the initial location"},
    {BRUSH_STROKE_LINE, "LINE", 0, "Line", "Draw a line with dabs separated according to spacing"},
    {BRUSH_STROKE_CURVE,
     "CURVE",
     0,
     "Curve",
     "Define the stroke curve with a Bézier curve (dabs are separated according to spacing)"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_brush_texture_slot_map_all_mode_items[] = {
    {MTEX_MAP_MODE_VIEW, "VIEW_PLANE", 0, "View Plane", ""},
    {MTEX_MAP_MODE_AREA, "AREA_PLANE", 0, "Area Plane", ""},
    {MTEX_MAP_MODE_TILED, "TILED", 0, "Tiled", ""},
    {MTEX_MAP_MODE_3D, "3D", 0, "3D", ""},
    {MTEX_MAP_MODE_RANDOM, "RANDOM", 0, "Random", ""},
    {MTEX_MAP_MODE_STENCIL, "STENCIL", 0, "Stencil", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem rna_enum_brush_curve_preset_items[] = {
    {BRUSH_CURVE_CUSTOM, "CUSTOM", ICON_RNDCURVE, "Custom", ""},
    {BRUSH_CURVE_SMOOTH, "SMOOTH", ICON_SMOOTHCURVE, "Smooth", ""},
    {BRUSH_CURVE_SMOOTHER, "SMOOTHER", ICON_SMOOTHCURVE, "Smoother", ""},
    {BRUSH_CURVE_SPHERE, "SPHERE", ICON_SPHERECURVE, "Sphere", ""},
    {BRUSH_CURVE_ROOT, "ROOT", ICON_ROOTCURVE, "Root", ""},
    {BRUSH_CURVE_SHARP, "SHARP", ICON_SHARPCURVE, "Sharp", ""},
    {BRUSH_CURVE_LIN, "LIN", ICON_LINCURVE, "Linear", ""},
    {BRUSH_CURVE_POW4, "POW4", ICON_SHARPCURVE, "Sharper", ""},
    {BRUSH_CURVE_INVSQUARE, "INVSQUARE", ICON_INVERSESQUARECURVE, "Inverse Square", ""},
    {BRUSH_CURVE_CONSTANT, "CONSTANT", ICON_NOCURVE, "Constant", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

/* NOTE: we don't actually turn these into a single enum bit-mask property,
 * instead we construct individual boolean properties. */
const EnumPropertyItem rna_enum_shared_automasking_flag_items[] = {
    {BRUSH_AUTOMASKING_TOPOLOGY,
     "use_automasking_topology",
     0,
     "Topology",
     "Affect only vertices connected to the active vertex under the brush"},
    {BRUSH_AUTOMASKING_FACE_SETS,
     "use_automasking_face_sets",
     0,
     "Face Sets",
     "Affect only vertices that share face sets with the active vertex"},
    {BRUSH_AUTOMASKING_BOUNDARY_EDGES,
     "use_automasking_boundary_edges",
     0,
     "Mesh Boundary Auto-Masking",
     "Do not affect non manifold boundary edges"},
    {BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS,
     "use_automasking_boundary_face_sets",
     0,
     "Face Sets Boundary Automasking",
     "Do not affect vertices that belong to a face set boundary"},
    {BRUSH_AUTOMASKING_CAVITY_NORMAL,
     "use_automasking_cavity",
     0,
     "Cavity Mask",
     "Do not affect vertices on peaks, based on the surface curvature"},
    {BRUSH_AUTOMASKING_CAVITY_INVERTED,
     "use_automasking_cavity_inverted",
     0,
     "Inverted Cavity Mask",
     "Do not affect vertices within crevices, based on the surface curvature"},
    {BRUSH_AUTOMASKING_CAVITY_USE_CURVE,
     "use_automasking_custom_cavity_curve",
     0,
     "Custom Cavity Curve",
     "Use custom curve"},
    {BRUSH_AUTOMASKING_BRUSH_NORMAL,
     "use_automasking_start_normal",
     0,
     "Area Normal",
     "Affect only vertices with a similar normal to where the stroke starts"},
    {BRUSH_AUTOMASKING_VIEW_NORMAL,
     "use_automasking_view_normal",
     0,
     "View Normal",
     "Affect only vertices with a normal that faces the viewer"},
    {BRUSH_AUTOMASKING_VIEW_OCCLUSION,
     "use_automasking_view_occlusion",
     0,
     "Occlusion",
     "Only affect vertices that are not occluded by other faces (slower performance)"},
    {0, nullptr, 0, nullptr, nullptr}};

const EnumPropertyItem rna_enum_brush_sculpt_brush_type_items[] = {
    {SCULPT_BRUSH_TYPE_DRAW, "DRAW", 0, "Draw", ""},
    {SCULPT_BRUSH_TYPE_DRAW_SHARP, "DRAW_SHARP", 0, "Draw Sharp", ""},
    {SCULPT_BRUSH_TYPE_CLAY, "CLAY", 0, "Clay", ""},
    {SCULPT_BRUSH_TYPE_CLAY_STRIPS, "CLAY_STRIPS", 0, "Clay Strips", ""},
    {SCULPT_BRUSH_TYPE_CLAY_THUMB, "CLAY_THUMB", 0, "Clay Thumb", ""},
    {SCULPT_BRUSH_TYPE_LAYER, "LAYER", 0, "Layer", ""},
    {SCULPT_BRUSH_TYPE_INFLATE, "INFLATE", 0, "Inflate", ""},
    {SCULPT_BRUSH_TYPE_BLOB, "BLOB", 0, "Blob", ""},
    {SCULPT_BRUSH_TYPE_CREASE, "CREASE", 0, "Crease", ""},
    RNA_ENUM_ITEM_SEPR,
    {SCULPT_BRUSH_TYPE_SMOOTH, "SMOOTH", 0, "Smooth", ""},
    {SCULPT_BRUSH_TYPE_PLANE, "PLANE", 0, "Plane", ""},
    {SCULPT_BRUSH_TYPE_MULTIPLANE_SCRAPE, "MULTIPLANE_SCRAPE", 0, "Multi-plane Scrape", ""},
    {SCULPT_BRUSH_TYPE_PINCH, "PINCH", 0, "Pinch", ""},
    RNA_ENUM_ITEM_SEPR,
    {SCULPT_BRUSH_TYPE_GRAB, "GRAB", 0, "Grab", ""},
    {SCULPT_BRUSH_TYPE_ELASTIC_DEFORM, "ELASTIC_DEFORM", 0, "Elastic Deform", ""},
    {SCULPT_BRUSH_TYPE_SNAKE_HOOK, "SNAKE_HOOK", 0, "Snake Hook", ""},
    {SCULPT_BRUSH_TYPE_THUMB, "THUMB", 0, "Thumb", ""},
    {SCULPT_BRUSH_TYPE_POSE, "POSE", 0, "Pose", ""},
    {SCULPT_BRUSH_TYPE_NUDGE, "NUDGE", 0, "Nudge", ""},
    {SCULPT_BRUSH_TYPE_ROTATE, "ROTATE", 0, "Rotate", ""},
    {SCULPT_BRUSH_TYPE_SLIDE_RELAX, "TOPOLOGY", 0, "Slide Relax", ""},
    {SCULPT_BRUSH_TYPE_BOUNDARY, "BOUNDARY", 0, "Boundary", ""},
    {SCULPT_BRUSH_TYPE_SCENE_PROJECT, "SCENE_PROJECT", 0, "Scene Project", ""},
    RNA_ENUM_ITEM_SEPR,
    {SCULPT_BRUSH_TYPE_CLOTH, "CLOTH", 0, "Cloth", ""},
    {SCULPT_BRUSH_TYPE_SIMPLIFY, "SIMPLIFY", 0, "Simplify", ""},
    {SCULPT_BRUSH_TYPE_MASK, "MASK", 0, "Mask", ""},
    {SCULPT_BRUSH_TYPE_DRAW_FACE_SETS, "DRAW_FACE_SETS", 0, "Draw Face Sets", ""},
    {SCULPT_BRUSH_TYPE_DISPLACEMENT_ERASER,
     "DISPLACEMENT_ERASER",
     0,
     "Multires Displacement Eraser",
     ""},
    {SCULPT_BRUSH_TYPE_DISPLACEMENT_SMEAR,
     "DISPLACEMENT_SMEAR",
     0,
     "Multires Displacement Smear",
     ""},
    {SCULPT_BRUSH_TYPE_PAINT, "PAINT", 0, "Paint", ""},
    {SCULPT_BRUSH_TYPE_SMEAR, "SMEAR", 0, "Smear", ""},
    {SCULPT_BRUSH_TYPE_BLUR, "BLUR", 0, "Blur", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem rna_enum_brush_vertex_brush_type_items[] = {
    {VPAINT_BRUSH_TYPE_DRAW, "DRAW", 0, "Draw", ""},
    {VPAINT_BRUSH_TYPE_BLUR, "BLUR", 0, "Blur", ""},
    {VPAINT_BRUSH_TYPE_AVERAGE, "AVERAGE", 0, "Average", ""},
    {VPAINT_BRUSH_TYPE_SMEAR, "SMEAR", 0, "Smear", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem rna_enum_brush_weight_brush_type_items[] = {
    {WPAINT_BRUSH_TYPE_DRAW, "DRAW", 0, "Draw", ""},
    {WPAINT_BRUSH_TYPE_BLUR, "BLUR", 0, "Blur", ""},
    {WPAINT_BRUSH_TYPE_AVERAGE, "AVERAGE", 0, "Average", ""},
    {WPAINT_BRUSH_TYPE_SMEAR, "SMEAR", 0, "Smear", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem rna_enum_brush_image_brush_type_items[] = {
    {IMAGE_PAINT_BRUSH_TYPE_DRAW, "DRAW", 0, "Draw", ""},
    {IMAGE_PAINT_BRUSH_TYPE_SOFTEN, "SOFTEN", 0, "Soften", ""},
    {IMAGE_PAINT_BRUSH_TYPE_SMEAR, "SMEAR", 0, "Smear", ""},
    {IMAGE_PAINT_BRUSH_TYPE_CLONE, "CLONE", 0, "Clone", ""},
    {IMAGE_PAINT_BRUSH_TYPE_FILL, "FILL", 0, "Fill", ""},
    {IMAGE_PAINT_BRUSH_TYPE_MASK, "MASK", 0, "Mask", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem rna_enum_brush_curves_sculpt_brush_type_items[] = {
    {CURVES_SCULPT_BRUSH_TYPE_SELECTION_PAINT, "SELECTION_PAINT", 0, "Paint Selection", ""},
    RNA_ENUM_ITEM_SEPR,
    {CURVES_SCULPT_BRUSH_TYPE_ADD, "ADD", 0, "Add", ""},
    {CURVES_SCULPT_BRUSH_TYPE_DELETE, "DELETE", 0, "Delete", ""},
    {CURVES_SCULPT_BRUSH_TYPE_DENSITY, "DENSITY", 0, "Density", ""},
    RNA_ENUM_ITEM_SEPR,
    {CURVES_SCULPT_BRUSH_TYPE_COMB, "COMB", 0, "Comb", ""},
    {CURVES_SCULPT_BRUSH_TYPE_SNAKE_HOOK, "SNAKE_HOOK", 0, "Snake Hook", ""},
    {CURVES_SCULPT_BRUSH_TYPE_GROW_SHRINK, "GROW_SHRINK", 0, "Grow / Shrink", ""},
    {CURVES_SCULPT_BRUSH_TYPE_PINCH, "PINCH", 0, "Pinch", ""},
    {CURVES_SCULPT_BRUSH_TYPE_PUFF, "PUFF", 0, "Puff", ""},
    {CURVES_SCULPT_BRUSH_TYPE_SMOOTH, "SMOOTH", 0, "Smooth", ""},
    {CURVES_SCULPT_BRUSH_TYPE_SLIDE, "SLIDE", 0, "Slide", ""},
    {0, nullptr, 0, nullptr, nullptr},
};


}  // namespace blender
#ifdef RNA_RUNTIME

#  include "DNA_material_types.h"

#  include "RNA_access.hh"

#  include "BKE_brush.hh"
#  include "BKE_colorband.hh"
#  include "BKE_context.hh"
#  include "BKE_icons.hh"
#  include "BKE_layer.hh"
#  include "BKE_material.hh"
#  include "BKE_paint.hh"
#  include "BKE_paint_types.hh"
#  include "BKE_preview_image.hh"

#  include "WM_api.hh"

namespace blender {

static bool rna_TextureCapabilities_has_random_texture_angle_get(PointerRNA *ptr)
{
  MTex *mtex = static_cast<MTex *>(ptr->data);
  return ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_VIEW, MTEX_MAP_MODE_AREA, MTEX_MAP_MODE_RANDOM);
}

static bool rna_TextureCapabilities_has_texture_angle_get(PointerRNA *ptr)
{
  MTex *mtex = static_cast<MTex *>(ptr->data);
  return mtex->brush_map_mode != MTEX_MAP_MODE_3D;
}

static bool rna_TextureCapabilities_has_texture_angle_source_get(PointerRNA *ptr)
{
  MTex *mtex = static_cast<MTex *>(ptr->data);
  return ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_VIEW, MTEX_MAP_MODE_AREA, MTEX_MAP_MODE_RANDOM);
}

static bool rna_BrushCapabilities_has_overlay_get(PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  return ELEM(
      br->mtex.brush_map_mode, MTEX_MAP_MODE_VIEW, MTEX_MAP_MODE_TILED, MTEX_MAP_MODE_STENCIL);
}

static bool rna_BrushCapabilities_has_random_texture_angle_get(PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  return !(br->stroke_method == BRUSH_STROKE_ANCHORED);
}

static bool rna_BrushCapabilities_has_smooth_stroke_get(PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  return (!(ELEM(br->stroke_method,
                 BRUSH_STROKE_ANCHORED,
                 BRUSH_STROKE_DRAG_DOT,
                 BRUSH_STROKE_LINE,
                 BRUSH_STROKE_CURVE)));
}

static bool rna_BrushCapabilities_has_spacing_get(PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  return (!(br->stroke_method == BRUSH_STROKE_ANCHORED));
}

static bool rna_BrushCapabilitiesSculpt_has_accumulate_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_accumulate(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_topology_rake_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_topology_rake(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_auto_smooth_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_auto_smooth(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_hardness_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_hardness(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_height_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_height(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_plane_height_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_plane_height(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_plane_depth_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_plane_depth(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_jitter_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_jitter(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_normal_weight_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_normal_weight(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_rake_factor_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_rake_factor(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_persistence_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_persistence(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_pinch_factor_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_pinch_factor(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_plane_offset_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_plane_offset(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_random_texture_angle_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_random_texture_angle(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_sculpt_plane_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_sculpt_plane(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_color_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_color(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_secondary_color_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_secondary_cursor_color(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_smooth_stroke_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_smooth_stroke(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_space_attenuation_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_space_attenuation(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_strength_pressure_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_strength_pressure(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_size_pressure_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_size_pressure(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_auto_smooth_pressure_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_auto_smooth_pressure(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_normal_radius_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_normal_radius(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_hardness_pressure_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_hardness_pressure(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_direction_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_inverted_direction(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_gravity_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_gravity(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_tilt_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_tilt(*br);
}

static bool rna_BrushCapabilitiesSculpt_has_dyntopo_get(PointerRNA *ptr)
{
  const Brush *br = static_cast<const Brush *>(ptr->data);
  return bke::brush::supports_dyntopo(*br);
}

static bool rna_BrushCapabilitiesImagePaint_has_accumulate_get(PointerRNA *ptr)
{
  /* only support for draw brush */
  Brush *br = static_cast<Brush *>(ptr->data);

  return (ELEM(br->stroke_method,
               BRUSH_STROKE_AIRBRUSH,
               BRUSH_STROKE_DRAG_DOT,
               BRUSH_STROKE_ANCHORED) ||
          (br->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_SOFTEN) ||
          (br->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_SMEAR) ||
          (br->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_FILL) ||
          (br->mtex.tex && !ELEM(br->mtex.brush_map_mode,
                                 MTEX_MAP_MODE_TILED,
                                 MTEX_MAP_MODE_STENCIL,
                                 MTEX_MAP_MODE_3D))) ?
             false :
             true;
}

static bool rna_BrushCapabilitiesImagePaint_has_radius_get(PointerRNA *ptr)
{
  /* only support for draw brush */
  Brush *br = static_cast<Brush *>(ptr->data);

  return (br->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_FILL);
}

static bool rna_BrushCapabilitiesImagePaint_has_space_attenuation_get(PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  return (ELEM(br->stroke_method, BRUSH_STROKE_SPACE, BRUSH_STROKE_LINE, BRUSH_STROKE_CURVE)) &&
         br->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_FILL;
}

static bool rna_BrushCapabilitiesImagePaint_has_color_get(PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  return ELEM(br->image_brush_type, IMAGE_PAINT_BRUSH_TYPE_DRAW, IMAGE_PAINT_BRUSH_TYPE_FILL);
}

static bool rna_BrushCapabilitiesVertexPaint_has_color_get(PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  return ELEM(br->vertex_brush_type, VPAINT_BRUSH_TYPE_DRAW);
}

static bool rna_BrushCapabilitiesWeightPaint_has_weight_get(PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  return ELEM(br->weight_brush_type, WPAINT_BRUSH_TYPE_DRAW);
}

static PointerRNA rna_Sculpt_brush_capabilities_get(PointerRNA *ptr)
{
  BLI_assert(ptr->owner_id == ptr->data);
  return RNA_pointer_create_with_parent(*ptr, RNA_BrushCapabilitiesSculpt, ptr->data);
}

static PointerRNA rna_Imapaint_brush_capabilities_get(PointerRNA *ptr)
{
  BLI_assert(ptr->owner_id == ptr->data);
  return RNA_pointer_create_with_parent(*ptr, RNA_BrushCapabilitiesImagePaint, ptr->data);
}

static PointerRNA rna_Vertexpaint_brush_capabilities_get(PointerRNA *ptr)
{
  BLI_assert(ptr->owner_id == ptr->data);
  return RNA_pointer_create_with_parent(*ptr, RNA_BrushCapabilitiesVertexPaint, ptr->data);
}

static PointerRNA rna_Weightpaint_brush_capabilities_get(PointerRNA *ptr)
{
  BLI_assert(ptr->owner_id == ptr->data);
  return RNA_pointer_create_with_parent(*ptr, RNA_BrushCapabilitiesWeightPaint, ptr->data);
}

static PointerRNA rna_Brush_capabilities_get(PointerRNA *ptr)
{
  BLI_assert(ptr->owner_id == ptr->data);
  return RNA_pointer_create_with_parent(*ptr, RNA_BrushCapabilities, ptr->data);
}

static void rna_Brush_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  BKE_brush_tag_unsaved_changes(br);
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, br);
  // WM_main_add_notifier(NC_SPACE | ND_SPACE_VIEW3D, nullptr);
}

static void rna_Brush_color_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  rna_Brush_update(bmain, scene, ptr);
  BKE_brush_color_sync_legacy(br);
}

static void rna_Brush_material_update(bContext * /*C*/, PointerRNA *ptr)
{
  Brush *br = static_cast<Brush *>(ptr->data);
  BKE_brush_tag_unsaved_changes(br);
  /* number of material users changed */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
}

static void rna_Brush_main_tex_update(bContext *C, PointerRNA *ptr)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Brush *br = static_cast<Brush *>(ptr->data);
  BKE_paint_invalidate_overlay_tex(*bmain, scene, view_layer, br->mtex.tex);
  rna_Brush_update(bmain, scene, ptr);
}

static void rna_Brush_secondary_tex_update(bContext *C, PointerRNA *ptr)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Brush *br = static_cast<Brush *>(ptr->data);
  BKE_paint_invalidate_overlay_tex(*bmain, scene, view_layer, br->mask_mtex.tex);
  rna_Brush_update(bmain, scene, ptr);
}

static void rna_Brush_size_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  BKE_paint_invalidate_overlay_all();
  rna_Brush_update(bmain, scene, ptr);
}

static void rna_Brush_stroke_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, scene);
  rna_Brush_update(bmain, scene, ptr);
}

static void rna_TextureSlot_brush_angle_update(bContext *C, PointerRNA *ptr)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  MTex *mtex = static_cast<MTex *>(ptr->data);
  /* skip invalidation of overlay for stencil mode */
  if (mtex->brush_map_mode != MTEX_MAP_MODE_STENCIL) {
    ViewLayer *view_layer = CTX_data_view_layer(C);
    BKE_paint_invalidate_overlay_tex(*bmain, scene, view_layer, mtex->tex);
  }

  rna_TextureSlot_update(C, ptr);
}

static void rna_Brush_set_size(PointerRNA *ptr, int value)
{
  Brush *brush = static_cast<Brush *>(ptr->data);

  /* scale unprojected size so it stays consistent with brush size */
  BKE_brush_scale_unprojected_size(&brush->unprojected_size, value, brush->size);
  brush->size = value;
}

static void rna_Brush_use_gradient_set(PointerRNA *ptr, int value)
{
  Brush *br = static_cast<Brush *>(ptr->data);

  if (value & BRUSH_USE_GRADIENT) {
    br->flag |= BRUSH_USE_GRADIENT;
  }
  else {
    br->flag &= ~BRUSH_USE_GRADIENT;
  }

  if ((br->flag & BRUSH_USE_GRADIENT) && br->gradient == nullptr) {
    br->gradient = BKE_colorband_add(true);
  }
}

static void rna_Brush_set_unprojected_size(PointerRNA *ptr, float value)
{
  Brush *brush = static_cast<Brush *>(ptr->data);

  /* scale brush size so it stays consistent with unprojected_size */
  BKE_brush_scale_size(&brush->size, value, brush->unprojected_size);
  brush->unprojected_size = value;
}

static const EnumPropertyItem *rna_Brush_direction_itemf(bContext *C,
                                                         PointerRNA *ptr,
                                                         PropertyRNA * /*prop*/,
                                                         bool * /*r_free*/)
{
  if (C == nullptr) {
    return rna_enum_dummy_DEFAULT_items;
  }

  PaintMode mode = BKE_paintmode_get_active_from_context(C);

  /* sculpt mode */
  static const EnumPropertyItem prop_smooth_direction_items[] = {
      {0,
       "SMOOTH",
       ICON_ADD,
       CTX_N_(BLT_I18NCONTEXT_ID_BRUSH, "Smooth"),
       N_("Smooth the surface")},
      {BRUSH_DIR_IN,
       "ENHANCE_DETAILS",
       ICON_REMOVE,
       CTX_N_(BLT_I18NCONTEXT_ID_BRUSH, "Enhance Details"),
       N_("Enhance the surface detail")},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_pinch_magnify_items[] = {
      {BRUSH_DIR_IN,
       "MAGNIFY",
       ICON_ADD,
       CTX_N_(BLT_I18NCONTEXT_ID_BRUSH, "Magnify"),
       N_("Subtract effect of brush")},
      {0,
       "PINCH",
       ICON_REMOVE,
       CTX_N_(BLT_I18NCONTEXT_ID_BRUSH, "Pinch"),
       N_("Add effect of brush")},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_inflate_deflate_items[] = {
      {0,
       "INFLATE",
       ICON_ADD,
       CTX_N_(BLT_I18NCONTEXT_ID_BRUSH, "Inflate"),
       N_("Add effect of brush")},
      {BRUSH_DIR_IN,
       "DEFLATE",
       ICON_REMOVE,
       CTX_N_(BLT_I18NCONTEXT_ID_BRUSH, "Deflate"),
       N_("Subtract effect of brush")},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* texture paint mode */
  static const EnumPropertyItem prop_soften_sharpen_items[] = {
      {BRUSH_DIR_IN,
       "SHARPEN",
       ICON_ADD,
       CTX_N_(BLT_I18NCONTEXT_ID_BRUSH, "Sharpen"),
       N_("Sharpen effect of brush")},
      {0,
       "SOFTEN",
       ICON_REMOVE,
       CTX_N_(BLT_I18NCONTEXT_ID_BRUSH, "Soften"),
       N_("Blur effect of brush")},
      {0, nullptr, 0, nullptr, nullptr},
  };

  Brush *me = static_cast<Brush *>(ptr->data);

  switch (mode) {
    case PaintMode::Sculpt:
      switch (me->sculpt_brush_type) {
        case SCULPT_BRUSH_TYPE_DRAW:
        case SCULPT_BRUSH_TYPE_DRAW_SHARP:
        case SCULPT_BRUSH_TYPE_CREASE:
        case SCULPT_BRUSH_TYPE_BLOB:
        case SCULPT_BRUSH_TYPE_LAYER:
        case SCULPT_BRUSH_TYPE_CLAY:
        case SCULPT_BRUSH_TYPE_CLAY_STRIPS:
        case SCULPT_BRUSH_TYPE_PLANE:
        case SCULPT_BRUSH_TYPE_SCENE_PROJECT:
          return prop_direction_items;
        case SCULPT_BRUSH_TYPE_SMOOTH:
          return prop_smooth_direction_items;
        case SCULPT_BRUSH_TYPE_MASK:
          switch (BrushMaskTool(me->mask_tool)) {
            case BRUSH_MASK_DRAW:
              return prop_direction_items;

            case BRUSH_MASK_SMOOTH:
              return rna_enum_dummy_DEFAULT_items;

            default:
              return rna_enum_dummy_DEFAULT_items;
          }

        case SCULPT_BRUSH_TYPE_PINCH:
          return prop_pinch_magnify_items;

        case SCULPT_BRUSH_TYPE_INFLATE:
          return prop_inflate_deflate_items;

        default:
          return rna_enum_dummy_DEFAULT_items;
      }

    case PaintMode::Texture2D:
    case PaintMode::Texture3D:
      switch (me->image_brush_type) {
        case IMAGE_PAINT_BRUSH_TYPE_SOFTEN:
          return prop_soften_sharpen_items;

        default:
          return rna_enum_dummy_DEFAULT_items;
      }
    case PaintMode::SculptCurves:
      switch (me->curves_sculpt_brush_type) {
        case CURVES_SCULPT_BRUSH_TYPE_GROW_SHRINK:
        case CURVES_SCULPT_BRUSH_TYPE_SELECTION_PAINT:
        case CURVES_SCULPT_BRUSH_TYPE_PINCH:
          return prop_direction_items;
        default:
          return rna_enum_dummy_DEFAULT_items;
      }
    default:
      return rna_enum_dummy_DEFAULT_items;
  }
}

static const EnumPropertyItem *rna_Brush_stroke_itemf(bContext *C,
                                                      PointerRNA * /*ptr*/,
                                                      PropertyRNA * /*prop*/,
                                                      bool * /*r_free*/)
{
  PaintMode mode = (C) ? BKE_paintmode_get_active_from_context(C) : PaintMode::Invalid;

  static const EnumPropertyItem brush_stroke_method_items[] = {
      {BRUSH_STROKE_DOTS, "DOTS", 0, "Dots", "Apply paint on each mouse move step"},
      {BRUSH_STROKE_SPACE,
       "SPACE",
       0,
       "Space",
       "Limit brush application to the distance specified by spacing"},
      {BRUSH_STROKE_AIRBRUSH,
       "AIRBRUSH",
       0,
       "Airbrush",
       "Keep applying paint effect while holding mouse (spray)"},
      {BRUSH_STROKE_LINE,
       "LINE",
       0,
       "Line",
       "Draw a line with dabs separated according to spacing"},
      {BRUSH_STROKE_CURVE,
       "CURVE",
       0,
       "Curve",
       "Define the stroke curve with a Bézier curve. Dabs are separated according to spacing."},
      {0, nullptr, 0, nullptr, nullptr},
  };

  switch (mode) {
    case PaintMode::Sculpt:
    case PaintMode::Texture2D:
    case PaintMode::Texture3D:
      return sculpt_stroke_method_items;

    default:
      return brush_stroke_method_items;
  }
}

static void rna_BrushCurvesSculptSettings_update(Main * /*bmain*/,
                                                 Scene * /*scene*/,
                                                 PointerRNA *ptr)
{
  Brush *br = reinterpret_cast<Brush *>(ptr->owner_id);
  BKE_brush_tag_unsaved_changes(br);
}

static const EnumPropertyItem *rna_BrushTextureSlot_map_mode_itemf(bContext *C,
                                                                   PointerRNA * /*ptr*/,
                                                                   PropertyRNA * /*prop*/,
                                                                   bool * /*r_free*/)
{
  static const EnumPropertyItem rna_enum_brush_texture_slot_map_texture_mode_items[] = {
      {MTEX_MAP_MODE_VIEW, "VIEW_PLANE", 0, "View Plane", ""},
      {MTEX_MAP_MODE_TILED, "TILED", 0, "Tiled", ""},
      {MTEX_MAP_MODE_3D, "3D", 0, "3D", ""},
      {MTEX_MAP_MODE_RANDOM, "RANDOM", 0, "Random", ""},
      {MTEX_MAP_MODE_STENCIL, "STENCIL", 0, "Stencil", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  if (C == nullptr) {
    return rna_enum_brush_texture_slot_map_all_mode_items;
  }

#  define rna_enum_brush_texture_slot_map_sculpt_mode_items \
    rna_enum_brush_texture_slot_map_all_mode_items;

  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  if (mode == PaintMode::Sculpt) {
    return rna_enum_brush_texture_slot_map_sculpt_mode_items;
  }
  return rna_enum_brush_texture_slot_map_texture_mode_items;

#  undef rna_enum_brush_texture_slot_map_sculpt_mode_items
}

static std::optional<std::string> rna_BrushCurvesSculptSettings_path(const PointerRNA * /*ptr*/)
{
  return "curves_sculpt_settings";
}

}  // namespace blender

#else

namespace blender {

static void rna_def_brush_texture_slot(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem prop_mask_paint_map_mode_items[] = {
      {MTEX_MAP_MODE_VIEW, "VIEW_PLANE", 0, "View Plane", ""},
      {MTEX_MAP_MODE_TILED, "TILED", 0, "Tiled", ""},
      {MTEX_MAP_MODE_RANDOM, "RANDOM", 0, "Random", ""},
      {MTEX_MAP_MODE_STENCIL, "STENCIL", 0, "Stencil", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

#  define TEXTURE_CAPABILITY(prop_name_, ui_name_) \
    prop = RNA_def_property(srna, #prop_name_, PROP_BOOLEAN, PROP_NONE); \
    RNA_def_property_clear_flag(prop, PROP_EDITABLE); \
    RNA_def_property_boolean_funcs(prop, "rna_TextureCapabilities_" #prop_name_ "_get", nullptr); \
    RNA_def_property_ui_text(prop, ui_name_, nullptr)

  srna = RNA_def_struct(brna, "BrushTextureSlot", "TextureSlot");
  RNA_def_struct_sdna(srna, "MTex");
  RNA_def_struct_ui_text(
      srna, "Brush Texture Slot", "Texture slot for textures in a Brush data-block");

  prop = RNA_def_property(srna, "angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "rot");
  RNA_def_property_range(prop, 0, M_PI * 2);
  RNA_def_property_ui_text(prop, "Angle", "Brush texture rotation");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, 0, "rna_TextureSlot_brush_angle_update");

  prop = RNA_def_property(srna, "map_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "brush_map_mode");
  RNA_def_property_enum_items(prop, rna_enum_brush_texture_slot_map_all_mode_items);
  RNA_def_property_enum_funcs(prop, nullptr, nullptr, "rna_BrushTextureSlot_map_mode_itemf");
  RNA_def_property_ui_text(prop, "Mode", "");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, 0, "rna_TextureSlot_update");

  prop = RNA_def_property(srna, "mask_map_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "brush_map_mode");
  RNA_def_property_enum_items(prop, prop_mask_paint_map_mode_items);
  RNA_def_property_ui_text(prop, "Mode", "");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, 0, "rna_TextureSlot_update");

  prop = RNA_def_property(srna, "use_rake", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "brush_angle_mode", MTEX_ANGLE_RAKE);
  RNA_def_property_ui_text(prop, "Rake", "");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, 0, "rna_TextureSlot_update");

  prop = RNA_def_property(srna, "use_random", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "brush_angle_mode", MTEX_ANGLE_RANDOM);
  RNA_def_property_ui_text(prop, "Random", "");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, 0, "rna_TextureSlot_update");

  prop = RNA_def_property(srna, "random_angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_range(prop, 0, M_PI * 2);
  RNA_def_property_ui_text(prop, "Random Angle", "Brush texture random angle");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, 0, "rna_TextureSlot_update");

  TEXTURE_CAPABILITY(has_texture_angle_source, "Has Texture Angle Source");
  TEXTURE_CAPABILITY(has_random_texture_angle, "Has Random Texture Angle");
  TEXTURE_CAPABILITY(has_texture_angle, "Has Texture Angle Source");

#  undef TEXTURE_CAPABILITY
}

static void rna_def_sculpt_capabilities(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "BrushCapabilitiesSculpt", nullptr);
  RNA_def_struct_sdna(srna, "Brush");
  RNA_def_struct_nested(brna, srna, "Brush");
  RNA_def_struct_ui_text(srna,
                         "Sculpt Capabilities",
                         "Read-only indications of which brush operations "
                         "are supported by the current sculpt tool");

#  define SCULPT_BRUSH_CAPABILITY(prop_name_, ui_name_) \
    prop = RNA_def_property(srna, #prop_name_, PROP_BOOLEAN, PROP_NONE); \
    RNA_def_property_clear_flag(prop, PROP_EDITABLE); \
    RNA_def_property_boolean_funcs( \
        prop, "rna_BrushCapabilitiesSculpt_" #prop_name_ "_get", nullptr); \
    RNA_def_property_ui_text(prop, ui_name_, nullptr)

  SCULPT_BRUSH_CAPABILITY(has_accumulate, "Has Accumulate");
  SCULPT_BRUSH_CAPABILITY(has_auto_smooth, "Has Auto Smooth");
  SCULPT_BRUSH_CAPABILITY(has_normal_radius, "Has Normal Radius");
  SCULPT_BRUSH_CAPABILITY(has_hardness, "Has Hardness");
  SCULPT_BRUSH_CAPABILITY(has_topology_rake, "Has Topology Rake");
  SCULPT_BRUSH_CAPABILITY(has_height, "Has Height");
  SCULPT_BRUSH_CAPABILITY(has_plane_depth, "Has Plane Depth");
  SCULPT_BRUSH_CAPABILITY(has_plane_height, "Has Plane Height");
  SCULPT_BRUSH_CAPABILITY(has_jitter, "Has Jitter");
  SCULPT_BRUSH_CAPABILITY(has_normal_weight, "Has Crease/Pinch Factor");
  SCULPT_BRUSH_CAPABILITY(has_rake_factor, "Has Rake Factor");
  SCULPT_BRUSH_CAPABILITY(has_persistence, "Has Persistence");
  SCULPT_BRUSH_CAPABILITY(has_pinch_factor, "Has Pinch Factor");
  SCULPT_BRUSH_CAPABILITY(has_plane_offset, "Has Plane Offset");
  SCULPT_BRUSH_CAPABILITY(has_random_texture_angle, "Has Random Texture Angle");
  SCULPT_BRUSH_CAPABILITY(has_sculpt_plane, "Has Sculpt Plane");
  SCULPT_BRUSH_CAPABILITY(has_color, "Has Color");
  SCULPT_BRUSH_CAPABILITY(has_secondary_color, "Has Secondary Color");
  SCULPT_BRUSH_CAPABILITY(has_smooth_stroke, "Has Smooth Stroke");
  SCULPT_BRUSH_CAPABILITY(has_space_attenuation, "Has Space Attenuation");
  SCULPT_BRUSH_CAPABILITY(has_strength_pressure, "Has Strength Pressure");
  SCULPT_BRUSH_CAPABILITY(has_size_pressure, "Has Size Pressure");
  SCULPT_BRUSH_CAPABILITY(has_auto_smooth_pressure, "Has Auto-Smooth Pressure");
  SCULPT_BRUSH_CAPABILITY(has_hardness_pressure, "Has Hardness Pressure");
  SCULPT_BRUSH_CAPABILITY(has_direction, "Has Direction");
  SCULPT_BRUSH_CAPABILITY(has_gravity, "Has Gravity");
  SCULPT_BRUSH_CAPABILITY(has_tilt, "Has Tilt");
  SCULPT_BRUSH_CAPABILITY(has_dyntopo, "Has Dyntopo");

#  undef SCULPT_CAPABILITY
}

static void rna_def_brush_capabilities(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "BrushCapabilities", nullptr);
  RNA_def_struct_sdna(srna, "Brush");
  RNA_def_struct_nested(brna, srna, "Brush");
  RNA_def_struct_ui_text(
      srna, "Brush Capabilities", "Read-only indications of supported operations");

#  define BRUSH_CAPABILITY(prop_name_, ui_name_) \
    prop = RNA_def_property(srna, #prop_name_, PROP_BOOLEAN, PROP_NONE); \
    RNA_def_property_clear_flag(prop, PROP_EDITABLE); \
    RNA_def_property_boolean_funcs(prop, "rna_BrushCapabilities_" #prop_name_ "_get", nullptr); \
    RNA_def_property_ui_text(prop, ui_name_, nullptr)

  BRUSH_CAPABILITY(has_overlay, "Has Overlay");
  BRUSH_CAPABILITY(has_random_texture_angle, "Has Random Texture Angle");
  BRUSH_CAPABILITY(has_spacing, "Has Spacing");
  BRUSH_CAPABILITY(has_smooth_stroke, "Has Smooth Stroke");

#  undef BRUSH_CAPABILITY
}

static void rna_def_image_paint_capabilities(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "BrushCapabilitiesImagePaint", nullptr);
  RNA_def_struct_sdna(srna, "Brush");
  RNA_def_struct_nested(brna, srna, "Brush");
  RNA_def_struct_ui_text(
      srna, "Image Paint Capabilities", "Read-only indications of supported operations");

#  define IMAPAINT_BRUSH_CAPABILITY(prop_name_, ui_name_) \
    prop = RNA_def_property(srna, #prop_name_, PROP_BOOLEAN, PROP_NONE); \
    RNA_def_property_clear_flag(prop, PROP_EDITABLE); \
    RNA_def_property_boolean_funcs( \
        prop, "rna_BrushCapabilitiesImagePaint_" #prop_name_ "_get", nullptr); \
    RNA_def_property_ui_text(prop, ui_name_, nullptr)

  IMAPAINT_BRUSH_CAPABILITY(has_accumulate, "Has Accumulate");
  IMAPAINT_BRUSH_CAPABILITY(has_space_attenuation, "Has Space Attenuation");
  IMAPAINT_BRUSH_CAPABILITY(has_radius, "Has Radius");
  IMAPAINT_BRUSH_CAPABILITY(has_color, "Has Color");

#  undef IMAPAINT_BRUSH_CAPABILITY
}

static void rna_def_vertex_paint_capabilities(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "BrushCapabilitiesVertexPaint", nullptr);
  RNA_def_struct_sdna(srna, "Brush");
  RNA_def_struct_nested(brna, srna, "Brush");
  RNA_def_struct_ui_text(
      srna, "Vertex Paint Capabilities", "Read-only indications of supported operations");

#  define VPAINT_BRUSH_CAPABILITY(prop_name_, ui_name_) \
    prop = RNA_def_property(srna, #prop_name_, PROP_BOOLEAN, PROP_NONE); \
    RNA_def_property_clear_flag(prop, PROP_EDITABLE); \
    RNA_def_property_boolean_funcs( \
        prop, "rna_BrushCapabilitiesVertexPaint_" #prop_name_ "_get", nullptr); \
    RNA_def_property_ui_text(prop, ui_name_, nullptr)

  VPAINT_BRUSH_CAPABILITY(has_color, "Has Color");

#  undef VPAINT_BRUSH_CAPABILITY
}

static void rna_def_weight_paint_capabilities(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "BrushCapabilitiesWeightPaint", nullptr);
  RNA_def_struct_sdna(srna, "Brush");
  RNA_def_struct_nested(brna, srna, "Brush");
  RNA_def_struct_ui_text(
      srna, "Weight Paint Capabilities", "Read-only indications of supported operations");

#  define WPAINT_BRUSH_CAPABILITY(prop_name_, ui_name_) \
    prop = RNA_def_property(srna, #prop_name_, PROP_BOOLEAN, PROP_NONE); \
    RNA_def_property_clear_flag(prop, PROP_EDITABLE); \
    RNA_def_property_boolean_funcs( \
        prop, "rna_BrushCapabilitiesWeightPaint_" #prop_name_ "_get", nullptr); \
    RNA_def_property_ui_text(prop, ui_name_, nullptr)

  WPAINT_BRUSH_CAPABILITY(has_weight, "Has Weight");

#  undef WPAINT_BRUSH_CAPABILITY
}

static void rna_def_curves_sculpt_options(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem density_mode_items[] = {
      {BRUSH_CURVES_SCULPT_DENSITY_MODE_AUTO,
       "AUTO",
       ICON_AUTO,
       "Auto",
       "Either add or remove curves depending on the minimum distance of the curves under the "
       "cursor"},
      {BRUSH_CURVES_SCULPT_DENSITY_MODE_ADD,
       "ADD",
       ICON_ADD,
       "Add",
       "Add new curves between existing curves, taking the minimum distance into account"},
      {BRUSH_CURVES_SCULPT_DENSITY_MODE_REMOVE,
       "REMOVE",
       ICON_REMOVE,
       "Remove",
       "Remove curves whose root points are too close"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "BrushCurvesSculptSettings", nullptr);
  RNA_def_struct_path_func(srna, "rna_BrushCurvesSculptSettings_path");
  RNA_def_struct_sdna(srna, "BrushCurvesSculptSettings");
  RNA_def_struct_ui_text(srna, "Curves Sculpt Brush Settings", "");

  prop = RNA_def_property(srna, "add_amount", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 1, INT32_MAX);
  RNA_def_property_ui_text(prop, "Count", "Number of curves added by the Add brush");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "points_per_curve", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 2, INT32_MAX);
  RNA_def_property_ui_text(
      prop, "Points per Curve", "Number of control points in a newly added curve");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "use_uniform_scale", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_CURVES_SCULPT_FLAG_SCALE_UNIFORM);
  RNA_def_property_ui_text(prop,
                           "Scale Uniform",
                           "Grow or shrink curves by changing their size uniformly instead of "
                           "using trimming or extrapolation");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "minimum_length", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(
      prop, "Minimum Length", "Avoid shrinking curves shorter than this length");

  prop = RNA_def_property(srna, "use_length_interpolate", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "flag", BRUSH_CURVES_SCULPT_FLAG_INTERPOLATE_LENGTH);
  RNA_def_property_ui_text(
      prop, "Interpolate Length", "Use length of the curves in close proximity");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "use_radius_interpolate", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "flag", BRUSH_CURVES_SCULPT_FLAG_INTERPOLATE_RADIUS);
  RNA_def_property_boolean_default(prop, true);
  RNA_def_property_ui_text(
      prop, "Interpolate Radius", "Use radius of the curves in close proximity");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "use_point_count_interpolate", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "flag", BRUSH_CURVES_SCULPT_FLAG_INTERPOLATE_POINT_COUNT);
  RNA_def_property_ui_text(prop,
                           "Interpolate Point Count",
                           "Use the number of points from the curves in close proximity");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "use_shape_interpolate", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_CURVES_SCULPT_FLAG_INTERPOLATE_SHAPE);
  RNA_def_property_ui_text(
      prop, "Interpolate Shape", "Use shape of the curves in close proximity");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "curve_length", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.0, FLT_MAX);
  RNA_def_property_ui_text(
      prop,
      "Curve Length",
      "Length of newly added curves when it is not interpolated from other curves");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "minimum_distance", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0, 1000.0f, 0.001, 2);
  RNA_def_property_ui_text(
      prop, "Minimum Distance", "Goal distance between curve roots for the Density brush");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "curve_radius", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.0, FLT_MAX);
  RNA_def_property_float_default(prop, 0.01f);
  RNA_def_property_ui_range(prop, 0.0, 1000.0f, 0.001, 2);
  RNA_def_property_ui_text(
      prop,
      "Curve Radius",
      "Radius of newly added curves when it is not interpolated from other curves");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "density_add_attempts", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, INT32_MAX);
  RNA_def_property_ui_text(
      prop, "Density Add Attempts", "How many times the Density brush tries to add a new curve");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "density_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, density_mode_items);
  RNA_def_property_ui_text(
      prop, "Density Mode", "Determines whether the brush adds or removes curves");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_OPERATOR_DEFAULT);
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");

  prop = RNA_def_property(srna, "curve_parameter_falloff", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(prop,
                           "Curve Parameter Falloff",
                           "Falloff that is applied from the tip to the root of each curve");
  RNA_def_property_update(prop, 0, "rna_BrushCurvesSculptSettings_update");
}

static void rna_def_brush(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem prop_blend_items[] = {
      {IMB_BLEND_MIX, "MIX", 0, "Mix", "Use Mix blending mode while painting"},
      RNA_ENUM_ITEM_SEPR,
      {IMB_BLEND_DARKEN, "DARKEN", 0, "Darken", "Use Darken blending mode while painting"},
      {IMB_BLEND_MUL, "MUL", 0, "Multiply", "Use Multiply blending mode while painting"},
      {IMB_BLEND_COLORBURN,
       "COLORBURN",
       0,
       "Color Burn",
       "Use Color Burn blending mode while painting"},
      {IMB_BLEND_LINEARBURN,
       "LINEARBURN",
       0,
       "Linear Burn",
       "Use Linear Burn blending mode while painting"},
      RNA_ENUM_ITEM_SEPR,
      {IMB_BLEND_LIGHTEN, "LIGHTEN", 0, "Lighten", "Use Lighten blending mode while painting"},
      {IMB_BLEND_SCREEN, "SCREEN", 0, "Screen", "Use Screen blending mode while painting"},
      {IMB_BLEND_COLORDODGE,
       "COLORDODGE",
       0,
       "Color Dodge",
       "Use Color Dodge blending mode while painting"},
      {IMB_BLEND_ADD, "ADD", 0, "Add", "Use Add blending mode while painting"},
      RNA_ENUM_ITEM_SEPR,
      {IMB_BLEND_OVERLAY, "OVERLAY", 0, "Overlay", "Use Overlay blending mode while painting"},
      {IMB_BLEND_SOFTLIGHT,
       "SOFTLIGHT",
       0,
       "Soft Light",
       "Use Soft Light blending mode while painting"},
      {IMB_BLEND_HARDLIGHT,
       "HARDLIGHT",
       0,
       "Hard Light",
       "Use Hard Light blending mode while painting"},
      {IMB_BLEND_VIVIDLIGHT,
       "VIVIDLIGHT",
       0,
       "Vivid Light",
       "Use Vivid Light blending mode while painting"},
      {IMB_BLEND_LINEARLIGHT,
       "LINEARLIGHT",
       0,
       "Linear Light",
       "Use Linear Light blending mode while painting"},
      {IMB_BLEND_PINLIGHT,
       "PINLIGHT",
       0,
       "Pin Light",
       "Use Pin Light blending mode while painting"},
      RNA_ENUM_ITEM_SEPR,
      {IMB_BLEND_DIFFERENCE,
       "DIFFERENCE",
       0,
       "Difference",
       "Use Difference blending mode while painting"},
      {IMB_BLEND_EXCLUSION,
       "EXCLUSION",
       0,
       "Exclusion",
       "Use Exclusion blending mode while painting"},
      {IMB_BLEND_SUB, "SUB", 0, "Subtract", "Use Subtract blending mode while painting"},
      RNA_ENUM_ITEM_SEPR,
      {IMB_BLEND_HUE, "HUE", 0, "Hue", "Use Hue blending mode while painting"},
      {IMB_BLEND_SATURATION,
       "SATURATION",
       0,
       "Saturation",
       "Use Saturation blending mode while painting"},
      {IMB_BLEND_COLOR, "COLOR", 0, "Color", "Use Color blending mode while painting"},
      {IMB_BLEND_LUMINOSITY, "LUMINOSITY", 0, "Value", "Use Value blending mode while painting"},
      RNA_ENUM_ITEM_SEPR,
      {IMB_BLEND_ERASE_ALPHA, "ERASE_ALPHA", 0, "Erase Alpha", "Erase alpha while painting"},
      {IMB_BLEND_ADD_ALPHA, "ADD_ALPHA", 0, "Add Alpha", "Add alpha while painting"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_sculpt_plane_items[] = {
      {SCULPT_DISP_DIR_AREA, "AREA", 0, "Area Plane", ""},
      {SCULPT_DISP_DIR_VIEW, "VIEW", 0, "View Plane", ""},
      {SCULPT_DISP_DIR_X, "X", 0, "X Plane", ""},
      {SCULPT_DISP_DIR_Y, "Y", 0, "Y Plane", ""},
      {SCULPT_DISP_DIR_Z, "Z", 0, "Z Plane", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_mask_tool_items[] = {
      {BRUSH_MASK_DRAW, "DRAW", 0, "Draw", ""},
      {BRUSH_MASK_SMOOTH, "SMOOTH", 0, "Smooth", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_blur_mode_items[] = {
      {KERNEL_BOX, "BOX", 0, "Box", ""},
      {KERNEL_GAUSSIAN, "GAUSSIAN", 0, "Gaussian", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_gradient_items[] = {
      {BRUSH_GRADIENT_PRESSURE, "PRESSURE", 0, "Pressure", ""},
      {BRUSH_GRADIENT_SPACING_REPEAT, "SPACING_REPEAT", 0, "Repeat", ""},
      {BRUSH_GRADIENT_SPACING_CLAMP, "SPACING_CLAMP", 0, "Clamp", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_gradient_fill_items[] = {
      {BRUSH_GRADIENT_LINEAR, "LINEAR", 0, "Linear", ""},
      {BRUSH_GRADIENT_RADIAL, "RADIAL", 0, "Radial", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_mask_pressure_items[] = {
      {0, "NONE", 0, "Off", ""},
      {BRUSH_MASK_PRESSURE_RAMP, "RAMP", ICON_STYLUS_PRESSURE, "Ramp", ""},
      {BRUSH_MASK_PRESSURE_CUTOFF, "CUTOFF", ICON_STYLUS_PRESSURE, "Cutoff", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_size_unit_items[] = {
      {0, "VIEW", 0, "View", "Measure brush size relative to the view"},
      {BRUSH_LOCK_SIZE, "SCENE", 0, "Scene", "Measure brush size relative to the scene"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem color_gradient_items[] = {
      {0, "COLOR", 0, "Color", "Paint with a single color"},
      {BRUSH_USE_GRADIENT, "GRADIENT", 0, "Gradient", "Paint with a gradient"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_spacing_unit_items[] = {
      {0, "VIEW", 0, "View", "Calculate brush spacing relative to the view"},
      {BRUSH_SCENE_SPACING,
       "SCENE",
       0,
       "Scene",
       "Calculate brush spacing relative to the scene using the stroke location"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_jitter_unit_items[] = {
      {BRUSH_ABSOLUTE_JITTER, "VIEW", 0, "View", "Jittering happens in screen space, in pixels"},
      {0, "BRUSH", 0, "Brush", "Jittering happens relative to the brush size"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem falloff_shape_unit_items[] = {
      {0, "SPHERE", 0, "Sphere", "Apply brush influence in a Sphere, outwards from the center"},
      {PAINT_FALLOFF_SHAPE_TUBE,
       "PROJECTED",
       0,
       "Projected",
       "Apply brush influence in a 2D circle, projected from the view"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_deformation_target_items[] = {
      {BRUSH_DEFORM_TARGET_GEOMETRY,
       "GEOMETRY",
       0,
       "Geometry",
       "Brush deformation displaces the vertices of the mesh"},
      {BRUSH_DEFORM_TARGET_CLOTH_SIM,
       "CLOTH_SIM",
       0,
       "Cloth Simulation",
       "Brush deforms the mesh by deforming the constraints of a cloth simulation"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_elastic_deform_type_items[] = {
      {BRUSH_ELASTIC_DEFORM_GRAB, "GRAB", 0, "Grab", ""},
      {BRUSH_ELASTIC_DEFORM_GRAB_BISCALE, "GRAB_BISCALE", 0, "Bi-Scale Grab", ""},
      {BRUSH_ELASTIC_DEFORM_GRAB_TRISCALE, "GRAB_TRISCALE", 0, "Tri-Scale Grab", ""},
      {BRUSH_ELASTIC_DEFORM_SCALE, "SCALE", 0, "Scale", ""},
      {BRUSH_ELASTIC_DEFORM_TWIST, "TWIST", 0, "Twist", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_snake_hook_deform_type_items[] = {
      {BRUSH_SNAKE_HOOK_DEFORM_FALLOFF,
       "FALLOFF",
       0,
       "Radius Falloff",
       "Applies the brush falloff in the tip of the brush"},
      {BRUSH_SNAKE_HOOK_DEFORM_ELASTIC,
       "ELASTIC",
       0,
       "Elastic",
       "Modifies the entire mesh using elastic deform"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_plane_inversion_mode_items[] = {
      {BRUSH_PLANE_INVERT_DISPLACEMENT,
       "INVERT_DISPLACEMENT",
       0,
       "Invert Displacement",
       "Displace the vertices away from the plane."},
      {BRUSH_PLANE_SWAP_HEIGHT_AND_DEPTH,
       "SWAP_DEPTH_AND_HEIGHT",
       0,
       "Swap Height and Depth",
       "Swap the roles of Height and Depth."},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_project_ray_direction_type_items[] = {
      {BRUSH_PROJECT_RAY_DIRECTION_VIEW_NORMAL,
       "VIEW_NORMAL",
       0,
       "View Normal",
       "Project the vertices along the view normal."},
      {BRUSH_PROJECT_RAY_DIRECTION_PLANE_NORMAL,
       "PLANE_NORMAL",
       0,
       "Plane Normal",
       "Project the vertices along the plane normal."},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_cloth_deform_type_items[] = {
      {BRUSH_CLOTH_DEFORM_DRAG, "DRAG", 0, "Drag", ""},
      {BRUSH_CLOTH_DEFORM_PUSH, "PUSH", 0, "Push", ""},
      {BRUSH_CLOTH_DEFORM_PINCH_POINT, "PINCH_POINT", 0, "Pinch Point", ""},
      {BRUSH_CLOTH_DEFORM_PINCH_PERPENDICULAR,
       "PINCH_PERPENDICULAR",
       0,
       "Pinch Perpendicular",
       ""},
      {BRUSH_CLOTH_DEFORM_INFLATE, "INFLATE", 0, "Inflate", ""},
      {BRUSH_CLOTH_DEFORM_GRAB, "GRAB", 0, "Grab", ""},
      {BRUSH_CLOTH_DEFORM_EXPAND, "EXPAND", 0, "Expand", ""},
      {BRUSH_CLOTH_DEFORM_SNAKE_HOOK, "SNAKE_HOOK", 0, "Snake Hook", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_cloth_force_falloff_type_items[] = {
      {BRUSH_CLOTH_FORCE_FALLOFF_RADIAL, "RADIAL", 0, "Radial", ""},
      {BRUSH_CLOTH_FORCE_FALLOFF_PLANE, "PLANE", 0, "Plane", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_boundary_falloff_type_items[] = {
      {BRUSH_BOUNDARY_FALLOFF_CONSTANT,
       "CONSTANT",
       0,
       "Constant",
       "Applies the same deformation in the entire boundary"},
      {BRUSH_BOUNDARY_FALLOFF_RADIUS,
       "RADIUS",
       0,
       "Brush Radius",
       "Applies the deformation in a localized area limited by the brush radius"},
      {BRUSH_BOUNDARY_FALLOFF_LOOP,
       "LOOP",
       0,
       "Loop",
       "Applies the brush falloff in a loop pattern"},
      {BRUSH_BOUNDARY_FALLOFF_LOOP_INVERT,
       "LOOP_INVERT",
       0,
       "Loop and Invert",
       "Applies the falloff radius in a loop pattern, inverting the displacement direction in "
       "each pattern repetition"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_cloth_simulation_area_type_items[] = {
      {BRUSH_CLOTH_SIMULATION_AREA_LOCAL,
       "LOCAL",
       0,
       "Local",
       "Simulates only a specific area around the brush limited by a fixed radius"},
      {BRUSH_CLOTH_SIMULATION_AREA_GLOBAL, "GLOBAL", 0, "Global", "Simulates the entire mesh"},
      {BRUSH_CLOTH_SIMULATION_AREA_DYNAMIC,
       "DYNAMIC",
       0,
       "Dynamic",
       "The active simulation area moves with the brush"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_smooth_deform_type_items[] = {
      {BRUSH_SMOOTH_DEFORM_LAPLACIAN,
       "LAPLACIAN",
       0,
       "Laplacian",
       "Smooths the surface and the volume"},
      {BRUSH_SMOOTH_DEFORM_SURFACE,
       "SURFACE",
       0,
       "Surface",
       "Smooths the surface of the mesh, preserving the volume"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_pose_deform_type_items[] = {
      {BRUSH_POSE_DEFORM_ROTATE_TWIST, "ROTATE_TWIST", 0, "Rotate/Twist", ""},
      {BRUSH_POSE_DEFORM_SCALE_TRANSLATE, "SCALE_TRANSLATE", 0, "Scale/Translate", ""},
      {BRUSH_POSE_DEFORM_SQUASH_STRETCH, "SQUASH_STRETCH", 0, "Squash & Stretch", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_pose_origin_type_items[] = {
      {BRUSH_POSE_ORIGIN_TOPOLOGY,
       "TOPOLOGY",
       0,
       "Topology",
       "Sets the rotation origin automatically using the topology and shape of the mesh as a "
       "guide"},
      {BRUSH_POSE_ORIGIN_FACE_SETS,
       "FACE_SETS",
       0,
       "Face Sets",
       "Creates a pose segment per face set, starting from the active face set"},
      {BRUSH_POSE_ORIGIN_FACE_SETS_FK,
       "FACE_SETS_FK",
       0,
       "Face Sets FK",
       "Simulates an FK deformation using the face set under the cursor as control"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_smear_deform_type_items[] = {
      {BRUSH_SMEAR_DEFORM_DRAG, "DRAG", 0, "Drag", ""},
      {BRUSH_SMEAR_DEFORM_PINCH, "PINCH", 0, "Pinch", ""},
      {BRUSH_SMEAR_DEFORM_EXPAND, "EXPAND", 0, "Expand", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_slide_deform_type_items[] = {
      {BRUSH_SLIDE_DEFORM_DRAG, "DRAG", 0, "Drag", ""},
      {BRUSH_SLIDE_DEFORM_PINCH, "PINCH", 0, "Pinch", ""},
      {BRUSH_SLIDE_DEFORM_EXPAND, "EXPAND", 0, "Expand", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem brush_boundary_deform_type_items[] = {
      {BRUSH_BOUNDARY_DEFORM_BEND, "BEND", 0, "Bend", ""},
      {BRUSH_BOUNDARY_DEFORM_EXPAND, "EXPAND", 0, "Expand", ""},
      {BRUSH_BOUNDARY_DEFORM_INFLATE, "INFLATE", 0, "Inflate", ""},
      {BRUSH_BOUNDARY_DEFORM_GRAB, "GRAB", 0, "Grab", ""},
      {BRUSH_BOUNDARY_DEFORM_TWIST, "TWIST", 0, "Twist", ""},
      {BRUSH_BOUNDARY_DEFORM_SMOOTH, "SMOOTH", 0, "Smooth", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "Brush", "ID");
  RNA_def_struct_ui_text(
      srna, "Brush", "Brush data-block for storing brush settings for painting and sculpting");
  RNA_def_struct_ui_icon(srna, ICON_BRUSH_DATA);

  prop = RNA_def_property(srna, "has_unsaved_changes", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Has unsaved changes",
                           "Indicates that there are any user visible changes since the brush has "
                           "been imported or read from the file");

  /* enums */
  prop = RNA_def_property(srna, "blend", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_blend_items);
  RNA_def_property_ui_text(prop, "Blending Mode", "Brush blending mode");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_COLOR);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /**
   * Begin per-mode brush type properties.
   *
   * keep in sync with #BKE_paint_get_tool_prop_id_from_paintmode
   */
  prop = RNA_def_property(srna, "sculpt_brush_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "sculpt_brush_type");
  RNA_def_property_enum_items(prop, rna_enum_brush_sculpt_brush_type_items);
  RNA_def_property_ui_text(prop, "Brush Type", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "vertex_brush_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "vertex_brush_type");
  RNA_def_property_enum_items(prop, rna_enum_brush_vertex_brush_type_items);
  RNA_def_property_ui_text(prop, "Brush Type", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "weight_brush_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "weight_brush_type");
  RNA_def_property_enum_items(prop, rna_enum_brush_weight_brush_type_items);
  RNA_def_property_ui_text(prop, "Brush Type", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "image_brush_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "image_brush_type");
  RNA_def_property_enum_items(prop, rna_enum_brush_image_brush_type_items);
  RNA_def_property_ui_text(prop, "Brush Type", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_IMAGE, "rna_Brush_update");

  prop = RNA_def_property(srna, "curves_sculpt_brush_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "curves_sculpt_brush_type");
  RNA_def_property_enum_items(prop, rna_enum_brush_curves_sculpt_brush_type_items);
  RNA_def_property_ui_text(prop, "Brush Type", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_CURVES);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /** End per mode brush type properties. */

  prop = RNA_def_property(srna, "direction", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "flag");
  RNA_def_property_enum_items(prop, prop_direction_items);
  RNA_def_property_enum_funcs(prop, nullptr, nullptr, "rna_Brush_direction_itemf");
  RNA_def_property_ui_text(prop, "Direction", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "stroke_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, sculpt_stroke_method_items);
  RNA_def_property_enum_funcs(prop, nullptr, nullptr, "rna_Brush_stroke_itemf");
  RNA_def_property_ui_text(prop, "Stroke Method", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_stroke_update");

  prop = RNA_def_property(srna, "sculpt_plane", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_sculpt_plane_items);
  RNA_def_property_ui_text(prop, "Sculpt Plane", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "mask_tool", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_mask_tool_items);
  RNA_def_property_ui_text(prop, "Mask Tool", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_MASK);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "curve_distance_falloff_preset", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_brush_curve_preset_items);
  RNA_def_property_ui_text(prop, "Falloff Curve Preset", "");
  RNA_def_property_translation_context(prop,
                                       BLT_I18NCONTEXT_ID_CURVE_LEGACY); /* Abusing id_curve :/ */
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "deform_target", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_deformation_target_items);
  RNA_def_property_ui_text(
      prop, "Deformation Target", "How the deformation of the brush will affect the object");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "elastic_deform_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_elastic_deform_type_items);
  RNA_def_property_ui_text(prop, "Deformation", "Deformation type that is used in the brush");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "snake_hook_deform_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_snake_hook_deform_type_items);
  RNA_def_property_ui_text(prop, "Deformation", "Deformation type that is used in the brush");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "plane_inversion_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_plane_inversion_mode_items);
  RNA_def_property_ui_text(prop, "Inversion Mode", "Inversion Mode");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "project_ray_direction_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_project_ray_direction_type_items);
  RNA_def_property_ui_text(prop, "Ray Direction", "Ray Direction");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "minimum_distance", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "minimum_distance");
  RNA_def_property_float_default(prop, 0);
  RNA_def_property_range(prop, 0.0f, 10.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.1, 3);
  RNA_def_property_ui_text(prop,
                           "Minimum Distance",
                           "Minimum distance to other scene objects after projecting onto them");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cloth_deform_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_cloth_deform_type_items);
  RNA_def_property_ui_text(prop, "Deformation", "Deformation type that is used in the brush");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cloth_force_falloff_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_cloth_force_falloff_type_items);
  RNA_def_property_ui_text(
      prop, "Force Falloff", "Shape used in the brush to apply force to the cloth");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cloth_simulation_area_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_cloth_simulation_area_type_items);
  RNA_def_property_ui_text(
      prop,
      "Simulation Area",
      "Part of the mesh that is going to be simulated when the stroke is active");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "boundary_falloff_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_boundary_falloff_type_items);
  RNA_def_property_ui_text(
      prop, "Boundary Falloff", "How the brush falloff is applied across the boundary");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "smooth_deform_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_smooth_deform_type_items);
  RNA_def_property_ui_text(prop, "Deformation", "Deformation type that is used in the brush");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "smear_deform_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_smear_deform_type_items);
  RNA_def_property_ui_text(prop, "Deformation", "Deformation type that is used in the brush");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "slide_deform_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_slide_deform_type_items);
  RNA_def_property_ui_text(prop, "Deformation", "Deformation type that is used in the brush");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "boundary_deform_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_boundary_deform_type_items);
  RNA_def_property_ui_text(prop, "Deformation", "Deformation type that is used in the brush");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "pose_deform_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_pose_deform_type_items);
  RNA_def_property_ui_text(prop, "Deformation", "Deformation type that is used in the brush");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "pose_origin_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_pose_origin_type_items);
  RNA_def_property_ui_text(prop,
                           "Rotation Origins",
                           "Method to set the rotation origins for the segments of the brush");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "jitter_unit", PROP_ENUM, PROP_NONE); /* as an enum */
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "flag");
  RNA_def_property_enum_items(prop, brush_jitter_unit_items);
  RNA_def_property_ui_text(
      prop, "Jitter Unit", "Jitter in screen space or relative to brush size");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "falloff_shape", PROP_ENUM, PROP_NONE); /* as an enum */
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "falloff_shape");
  RNA_def_property_enum_items(prop, falloff_shape_unit_items);
  RNA_def_property_ui_text(prop, "Falloff Shape", "Use projected or spherical falloff");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /* number values */
  prop = RNA_def_property(srna, "size", PROP_INT, PROP_PIXEL_DIAMETER);
  RNA_def_property_int_funcs(prop, nullptr, "rna_Brush_set_size", nullptr);
  RNA_def_property_range(prop, 1, MAX_BRUSH_PIXEL_DIAMETER * 10);
  RNA_def_property_ui_range(prop, 1, MAX_BRUSH_PIXEL_DIAMETER, 1, -1);
  RNA_def_property_ui_text(prop, "Size", "Diameter of the brush in pixels");
  RNA_def_property_update(prop, 0, "rna_Brush_size_update");

  prop = RNA_def_property(srna, "unprojected_size", PROP_FLOAT, PROP_DISTANCE_DIAMETER);
  RNA_def_property_float_funcs(prop, nullptr, "rna_Brush_set_unprojected_size", nullptr);
  RNA_def_property_range(prop, 0.001, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.001, 1, 1, -1);
  RNA_def_property_ui_text(prop, "Unprojected Size", "Diameter of brush in Blender units");
  RNA_def_property_update(prop, 0, "rna_Brush_size_update");

  prop = RNA_def_property(srna, "input_samples", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "input_samples");
  RNA_def_property_range(prop, 1, PAINT_MAX_INPUT_SAMPLES);
  RNA_def_property_ui_range(prop, 1, PAINT_MAX_INPUT_SAMPLES, 1, -1);
  RNA_def_property_ui_text(
      prop,
      "Input Samples",
      "Number of input samples to average together to smooth the brush stroke");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "jitter", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "jitter");
  RNA_def_property_range(prop, 0.0f, 1000.0f);
  RNA_def_property_ui_range(prop, 0.0f, 2.0f, 0.1, 4);
  RNA_def_property_ui_text(prop, "Jitter", "Jitter the position of the brush while painting");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "jitter_absolute", PROP_INT, PROP_PIXEL);
  RNA_def_property_int_sdna(prop, nullptr, "jitter_absolute");
  RNA_def_property_range(prop, 0, 1000000);
  RNA_def_property_ui_text(
      prop, "Jitter", "Jitter the position of the brush in pixels while painting");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_BRUSH);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "spacing", PROP_INT, PROP_PERCENTAGE);
  RNA_def_property_int_sdna(prop, nullptr, "spacing");
  RNA_def_property_range(prop, 1, 1000);
  RNA_def_property_ui_range(prop, 1, 500, 5, -1);
  RNA_def_property_ui_text(
      prop, "Spacing", "Spacing between brush daubs as a percentage of brush diameter");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "grad_spacing", PROP_INT, PROP_PIXEL);
  RNA_def_property_int_sdna(prop, nullptr, "gradient_spacing");
  RNA_def_property_range(prop, 1, 10000);
  RNA_def_property_ui_range(prop, 1, 10000, 5, -1);
  RNA_def_property_ui_text(
      prop, "Gradient Spacing", "Spacing before brush gradient goes full circle");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_color_jitter", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_JITTER_COLOR);
  RNA_def_property_ui_text(prop, "Use Color Jitter", "Jitter brush color");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "hue_jitter", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "hsv_jitter[0]");
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1, 0.05, 2);
  RNA_def_property_ui_text(prop, "Hue Jitter", "Color jitter effect on hue");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "saturation_jitter", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "hsv_jitter[1]");
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1, 0.05, 2);
  RNA_def_property_ui_text(prop, "Saturation Jitter", "Color jitter effect on saturation");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "value_jitter", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "hsv_jitter[2]");
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1, 0.05, 2);
  RNA_def_property_ui_text(prop, "Value Jitter", "Color jitter effect on value");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_stroke_random_hue", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_HUE_AT_STROKE);
  RNA_def_property_ui_icon(prop, ICON_GP_SELECT_STROKES, 0);
  RNA_def_property_ui_text(prop, "Stroke Random", "Use randomness at stroke level");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);

  prop = RNA_def_property(srna, "use_stroke_random_sat", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_SAT_AT_STROKE);
  RNA_def_property_ui_icon(prop, ICON_GP_SELECT_STROKES, 0);
  RNA_def_property_ui_text(prop, "Stroke Random", "Use randomness at stroke level");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);

  prop = RNA_def_property(srna, "use_stroke_random_val", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_VAL_AT_STROKE);
  RNA_def_property_ui_icon(prop, ICON_GP_SELECT_STROKES, 0);
  RNA_def_property_ui_text(prop, "Stroke Random", "Use randomness at stroke level");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);

  prop = RNA_def_property(srna, "use_random_press_hue", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_HUE_RAND_PRESS);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure", "Use pressure to modulate randomness");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);

  prop = RNA_def_property(srna, "use_random_press_sat", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_SAT_RAND_PRESS);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure", "Use pressure to modulate randomness");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);

  prop = RNA_def_property(srna, "use_random_press_val", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_VAL_RAND_PRESS);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure", "Use pressure to modulate randomness");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);

  prop = RNA_def_property(srna, "curve_random_hue", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "curve_rand_hue");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(prop, "Random Curve", "Curve used for modulating effect");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "curve_random_saturation", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "curve_rand_saturation");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(prop, "Random Curve", "Curve used for modulating effect");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "curve_random_value", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "curve_rand_value");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(prop, "Random Curve", "Curve used for modulating effect");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "curve_size", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "curve_size");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(
      prop, "Pressure Size Mapping", "Curve used to map pressure to brush size");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "curve_strength", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "curve_strength");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(
      prop, "Pressure Strength Mapping", "Curve used to map pressure to brush strength");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "curve_jitter", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "curve_jitter");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(
      prop, "Pressure Jitter Mapping", "Curve used to map pressure to brush jitter");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "smooth_stroke_radius", PROP_INT, PROP_PIXEL);
  RNA_def_property_range(prop, 10, 200);
  RNA_def_property_ui_text(
      prop, "Smooth Stroke Radius", "Minimum distance from last point before stroke continues");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "smooth_stroke_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0.5, 0.99);
  RNA_def_property_ui_text(prop, "Smooth Stroke Factor", "Higher values give a smoother stroke");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "rate", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "rate");
  RNA_def_property_range(prop, 0.0001f, 10000.0f);
  RNA_def_property_ui_range(prop, 0.01f, 1.0f, 1, 3);
  RNA_def_property_ui_text(prop, "Rate", "Interval between paints for Airbrush");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_float_sdna(prop, nullptr, "color");
  RNA_def_property_ui_text(prop, "Color", "");
  RNA_def_property_update(prop, 0, "rna_Brush_color_update");

  prop = RNA_def_property(srna, "secondary_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_float_sdna(prop, nullptr, "secondary_color");
  RNA_def_property_ui_text(prop, "Secondary Color", "");
  RNA_def_property_update(prop, 0, "rna_Brush_color_update");

  prop = RNA_def_property(srna, "weight", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(prop, "Weight", "Vertex weight when brush is applied");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "strength", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "alpha");
  RNA_def_property_range(prop, 0.0f, 10.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(
      prop, "Strength", "How powerful the effect of the brush is when applied");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "flow", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "flow");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(prop, "Flow", "Amount of paint that is applied per stroke sample");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "wet_mix", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "wet_mix");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(
      prop, "Wet Mix", "Amount of paint that is picked from the surface into the brush color");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "wet_persistence", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "wet_persistence");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(
      prop,
      "Wet Persistence",
      "Amount of wet paint that stays in the brush after applying paint to the surface");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "density", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "density");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(
      prop, "Density", "Amount of random elements that are going to be affected by the brush");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "tip_scale_x", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "tip_scale_x");
  RNA_def_property_range(prop, 0.0001f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0001f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(prop, "Tip Scale X", "Scale of the brush tip in the X axis");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_hardness_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_flags", BRUSH_PAINT_HARDNESS_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure for Hardness", "Use pressure to modulate hardness");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "invert_hardness_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "paint_flags", BRUSH_PAINT_HARDNESS_PRESSURE_INVERT);
  RNA_def_property_ui_icon(prop, ICON_ARROW_LEFTRIGHT, 0);
  RNA_def_property_ui_text(
      prop, "Invert Pressure for Hardness", "Invert the modulation of pressure in hardness");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_flow_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_flags", BRUSH_PAINT_FLOW_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure for Flow", "Use pressure to modulate flow");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "invert_flow_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_flags", BRUSH_PAINT_FLOW_PRESSURE_INVERT);
  RNA_def_property_ui_icon(prop, ICON_ARROW_LEFTRIGHT, 0);
  RNA_def_property_ui_text(
      prop, "Invert Pressure for Flow", "Invert the modulation of pressure in flow");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_wet_mix_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_flags", BRUSH_PAINT_WET_MIX_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure for Wet Mix", "Use pressure to modulate wet mix");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "invert_wet_mix_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_flags", BRUSH_PAINT_WET_MIX_PRESSURE_INVERT);
  RNA_def_property_ui_icon(prop, ICON_ARROW_LEFTRIGHT, 0);
  RNA_def_property_ui_text(
      prop, "Invert Pressure for Wet Mix", "Invert the modulation of pressure in wet mix");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_wet_persistence_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "paint_flags", BRUSH_PAINT_WET_PERSISTENCE_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(
      prop, "Use Pressure for Wet Persistence", "Use pressure to modulate wet persistence");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "invert_wet_persistence_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "paint_flags", BRUSH_PAINT_WET_PERSISTENCE_PRESSURE_INVERT);
  RNA_def_property_ui_icon(prop, ICON_ARROW_LEFTRIGHT, 0);
  RNA_def_property_ui_text(prop,
                           "Invert Pressure for Wet Persistence",
                           "Invert the modulation of pressure in wet persistence");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_density_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_flags", BRUSH_PAINT_DENSITY_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure for Density", "Use pressure to modulate density");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "invert_density_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_flags", BRUSH_PAINT_DENSITY_PRESSURE_INVERT);
  RNA_def_property_ui_icon(prop, ICON_ARROW_LEFTRIGHT, 0);
  RNA_def_property_ui_text(
      prop, "Invert Pressure for Density", "Invert the modulation of pressure in density");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "dash_ratio", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "dash_ratio");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(
      prop, "Dash Ratio", "Ratio of samples in a cycle that the brush is enabled");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "dash_samples", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "dash_samples");
  RNA_def_property_range(prop, 1, 10000);
  RNA_def_property_ui_range(prop, 1, 10000, 5, -1);
  RNA_def_property_ui_text(
      prop, "Dash Length", "Length of a dash cycle measured in stroke samples");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "plane_offset", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "plane_offset");
  RNA_def_property_float_default(prop, 0);
  RNA_def_property_range(prop, -2.0f, 2.0f);
  RNA_def_property_ui_range(prop, -0.5f, 0.5f, 0.001, 3);
  RNA_def_property_ui_text(
      prop,
      "Plane Offset",
      "Adjust plane on which the brush acts towards or away from the object surface");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "plane_trim", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "plane_trim");
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_text(
      prop,
      "Plane Trim",
      "If a vertex is further away from offset plane than this, then it is not affected");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "height", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "height");
  RNA_def_property_float_default(prop, 0.5f);
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 0.2f, 1, 3);
  RNA_def_property_ui_text(
      prop,
      "Brush Height",
      "Affectable height of brush (i.e. the layer height for the layer tool)");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "plane_depth", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "plane_depth");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1.0f, 1, 3);
  RNA_def_property_ui_text(prop,
                           "Depth",
                           "The maximum distance below the plane for affected vertices. "
                           "Increasing the depth affects vertices farther below the plane.");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "plane_height", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "plane_height");
  RNA_def_property_float_default(prop, 1.0f);
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1.0f, 1, 3);
  RNA_def_property_ui_text(prop,
                           "Height",
                           "The maximum distance above the plane for affected vertices. "
                           "Increasing the height affects vertices farther above the plane.");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "stabilize_normal", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "stabilize_normal");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1.0f, 1, 3);
  RNA_def_property_ui_text(prop,
                           "Stabilize Normal",
                           "How stable the plane normal is over the course of the stroke. "
                           "A value of 0 corresponds to using the current normal, "
                           "and a value of 1 corresponds to using the initial normal.");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "stabilize_plane", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "stabilize_plane");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1.0f, 1, 3);
  RNA_def_property_ui_text(prop,
                           "Stabilize Center",
                           "How stable the plane center is over the course of the stroke. "
                           "A value of 0 corresponds to using the current center, "
                           "and a value of 1 corresponds to using the initial center.");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "texture_sample_bias", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "texture_sample_bias");
  RNA_def_property_float_default(prop, 0);
  RNA_def_property_range(prop, -1, 1);
  RNA_def_property_ui_text(prop, "Texture Sample Bias", "Value added to texture samples");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_color_as_displacement", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_USE_COLOR_AS_DISPLACEMENT);
  RNA_def_property_ui_text(
      prop,
      "Vector Displacement",
      "Handle each pixel color as individual vector for displacement (area plane mapping only)");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "normal_weight", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "normal_weight");
  RNA_def_property_float_default(prop, 0);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(
      prop, "Normal Weight", "How much grab will pull vertices out of surface during a grab");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "elastic_deform_volume_preservation", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "elastic_deform_volume_preservation");
  RNA_def_property_range(prop, 0.0f, 0.9f);
  RNA_def_property_ui_range(prop, 0.0f, 0.9f, 0.01f, 3);
  RNA_def_property_ui_text(prop,
                           "Volume Preservation",
                           "Poisson ratio for elastic deformation. Higher values preserve volume "
                           "more, but also lead to more bulging.");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "rake_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "rake_factor");
  RNA_def_property_float_default(prop, 0);
  RNA_def_property_range(prop, 0.0f, 10.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(prop, "Rake", "How much grab will follow cursor rotation");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "crease_pinch_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "crease_pinch_factor");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Crease Brush Pinch Factor", "How much the crease brush pinches");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "pose_offset", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "pose_offset");
  RNA_def_property_range(prop, 0.0f, 2.0f);
  RNA_def_property_ui_text(
      prop, "Pose Origin Offset", "Offset of the pose origin in relation to the brush radius");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "disconnected_distance_max", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "disconnected_distance_max");
  RNA_def_property_range(prop, 0.0f, 10.0f);
  RNA_def_property_ui_text(prop,
                           "Max Element Distance",
                           "Maximum distance to search for disconnected loose parts in the mesh");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "boundary_offset", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "boundary_offset");
  RNA_def_property_range(prop, 0.0f, 30.0f);
  RNA_def_property_ui_text(prop,
                           "Boundary Origin Offset",
                           "Offset of the boundary origin in relation to the brush radius");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "surface_smooth_shape_preservation", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "surface_smooth_shape_preservation");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(
      prop, "Shape Preservation", "How much of the original shape is preserved when smoothing");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "surface_smooth_current_vertex", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "surface_smooth_current_vertex");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(
      prop,
      "Per Vertex Displacement",
      "How much the position of each individual vertex influences the final result");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "surface_smooth_iterations", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "surface_smooth_iterations");
  RNA_def_property_range(prop, 1, 10);
  RNA_def_property_ui_range(prop, 1, 10, 1, 3);
  RNA_def_property_ui_text(prop, "Iterations", "Number of smoothing iterations per brush step");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "multiplane_scrape_angle", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "multiplane_scrape_angle");
  RNA_def_property_range(prop, 0.0f, 160.0f);
  RNA_def_property_ui_text(prop, "Plane Angle", "Angle between the planes of the crease");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "pose_smooth_iterations", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "pose_smooth_iterations");
  RNA_def_property_range(prop, 0, 100);
  RNA_def_property_ui_text(
      prop,
      "Smooth Iterations",
      "Smooth iterations applied after calculating the pose factor of each vertex");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "pose_ik_segments", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "pose_ik_segments");
  RNA_def_property_range(prop, 1, 20);
  RNA_def_property_ui_range(prop, 1, 20, 1, 3);
  RNA_def_property_ui_text(
      prop,
      "Pose IK Segments",
      "Number of segments of the inverse kinematics chain that will deform the mesh");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "tip_roundness", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "tip_roundness");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Tip Roundness", "Roundness of the brush tip");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cloth_mass", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "cloth_mass");
  RNA_def_property_range(prop, 0.01f, 2.0f);
  RNA_def_property_ui_text(prop, "Cloth Mass", "Mass of each simulation particle");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cloth_damping", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "cloth_damping");
  RNA_def_property_range(prop, 0.01f, 1.0f);
  RNA_def_property_ui_text(
      prop, "Cloth Damping", "How much the applied forces are propagated through the cloth");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cloth_sim_limit", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "cloth_sim_limit");
  RNA_def_property_range(prop, 0.1f, 10.0f);
  RNA_def_property_ui_text(
      prop,
      "Simulation Limit",
      "Factor added relative to the size of the radius to limit the cloth simulation effects");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cloth_sim_falloff", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "cloth_sim_falloff");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop,
                           "Simulation Falloff",
                           "Area to apply deformation falloff to the effects of the simulation");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cloth_constraint_softbody_strength", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "cloth_constraint_softbody_strength");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(
      prop,
      "Soft Body Plasticity",
      "How much the cloth preserves the original shape, acting as a soft body");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "hardness", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "hardness");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(
      prop, "Hardness", "How close the brush falloff starts from the edge of the brush");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "auto_smooth_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "autosmooth_factor");
  RNA_def_property_float_default(prop, 0);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(
      prop, "Auto-Smooth", "Amount of smoothing to automatically apply to each stroke");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "topology_rake_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "topology_rake_factor");
  RNA_def_property_float_default(prop, 0);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(prop,
                           "Topology Rake",
                           "Automatically align edges to the brush direction to "
                           "generate cleaner topology and define sharp features. "
                           "Best used on low-poly meshes as it has a performance impact.");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "tilt_strength_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "tilt_strength_factor");
  RNA_def_property_float_default(prop, 0);
  RNA_def_property_range(prop, -1.0f, 1.0f);
  RNA_def_property_ui_range(prop, -1.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(prop,
                           "Tilt Strength",
                           "How much the tilt of the pen will affect the brush. Negative values "
                           "indicate inverting the tilt directions.");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "normal_radius_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "normal_radius_factor");
  RNA_def_property_range(prop, 0.0f, 2.0f);
  RNA_def_property_ui_range(prop, 0.0f, 2.0f, 0.001, 3);
  RNA_def_property_ui_text(prop,
                           "Normal Radius",
                           "Ratio between the brush radius and the radius that is going to be "
                           "used to sample the normal");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "area_radius_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "area_radius_factor");
  RNA_def_property_range(prop, 0.0f, 2.0f);
  RNA_def_property_ui_range(prop, 0.0f, 2.0f, 0.001, 3);
  RNA_def_property_ui_text(prop,
                           "Area Radius",
                           "Ratio between the brush radius and the radius that is going to be "
                           "used to sample the area center");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "wet_paint_radius_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "wet_paint_radius_factor");
  RNA_def_property_range(prop, 0.0f, 2.0f);
  RNA_def_property_ui_range(prop, 0.0f, 2.0f, 0.001, 3);
  RNA_def_property_ui_text(prop,
                           "Wet Paint Radius",
                           "Ratio between the brush radius and the radius that is going to be "
                           "used to sample the color to blend in wet paint");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "stencil_pos", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, nullptr, "stencil_pos");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Stencil Position", "Position of stencil in viewport");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "stencil_dimension", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, nullptr, "stencil_dimension");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Stencil Dimensions", "Dimensions of stencil in viewport");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "mask_stencil_pos", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, nullptr, "mask_stencil_pos");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Mask Stencil Position", "Position of mask stencil in viewport");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "mask_stencil_dimension", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, nullptr, "mask_stencil_dimension");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(
      prop, "Mask Stencil Dimensions", "Dimensions of mask stencil in viewport");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "sharp_threshold", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 100.0);
  RNA_def_property_ui_range(prop, 0.0, 1.0, 1, 3);
  RNA_def_property_float_sdna(prop, nullptr, "sharp_threshold");
  RNA_def_property_ui_text(
      prop, "Sharp Threshold", "Threshold below which, no sharpening is done");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "fill_threshold", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 100.0);
  RNA_def_property_ui_range(prop, 0.0, 1.0, 1, 3);
  RNA_def_property_float_sdna(prop, nullptr, "fill_threshold");
  RNA_def_property_ui_text(
      prop, "Fill Threshold", "Threshold above which filling is not propagated");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "blur_kernel_radius", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "blur_kernel_radius");
  RNA_def_property_range(prop, 1, 10000);
  RNA_def_property_ui_range(prop, 1, 50, 1, -1);
  RNA_def_property_ui_text(
      prop, "Kernel Radius", "Radius of kernel used for soften and sharpen in pixels");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "blur_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_blur_mode_items);
  RNA_def_property_ui_text(prop, "Blur Mode", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "falloff_angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "falloff_angle");
  RNA_def_property_range(prop, 0, M_PI_2);
  RNA_def_property_ui_text(
      prop,
      "Falloff Angle",
      "Paint most on faces pointing towards the view according to this angle");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /* flag */
  prop = RNA_def_property(srna, "use_original_normal", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_ORIGINAL_NORMAL);
  RNA_def_property_ui_text(prop,
                           "Original Normal",
                           "When locked keep using normal of surface where stroke was initiated");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_original_plane", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_ORIGINAL_PLANE);
  RNA_def_property_ui_text(
      prop,
      "Original Plane",
      "When locked keep using the plane origin of surface where stroke was initiated");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_scene_spacing", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "flag");
  RNA_def_property_enum_items(prop, brush_spacing_unit_items);
  RNA_def_property_ui_text(
      prop, "Spacing Distance", "Calculate the brush spacing using view or scene distance");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_grab_active_vertex", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_GRAB_ACTIVE_VERTEX);
  RNA_def_property_ui_text(
      prop,
      "Grab Active Vertex",
      "Apply the maximum grab strength to the active vertex instead of the cursor location");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_grab_silhouette", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_GRAB_SILHOUETTE);
  RNA_def_property_ui_text(
      prop, "Grab Silhouette", "Grabs trying to automask the silhouette of the object");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_paint_antialiasing", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "sampling_flag", BRUSH_PAINT_ANTIALIASING);
  RNA_def_property_ui_text(prop, "Anti-Aliasing", "Smooths the edges of the strokes");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_multiplane_scrape_dynamic", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_MULTIPLANE_SCRAPE_DYNAMIC);
  RNA_def_property_ui_text(prop,
                           "Dynamic Mode",
                           "The angle between the planes changes during the stroke to fit the "
                           "surface under the cursor");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "show_multiplane_scrape_planes_preview", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_MULTIPLANE_SCRAPE_PLANES_PREVIEW);
  RNA_def_property_ui_text(
      prop, "Show Cursor Preview", "Preview the scrape planes in the cursor during the stroke");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_pose_ik_anchored", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_POSE_IK_ANCHORED);
  RNA_def_property_ui_text(
      prop, "Keep Anchor Point", "Keep the position of the last segment in the IK chain fixed");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_pose_lock_rotation", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_POSE_USE_LOCK_ROTATION);
  RNA_def_property_ui_text(prop,
                           "Lock Rotation When Scaling",
                           "Do not rotate the segment when using the scale deform mode");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_connected_only", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_USE_CONNECTED_ONLY);
  RNA_def_property_ui_text(prop, "Connected Only", "Affect only topologically connected elements");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_cloth_pin_simulation_boundary", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_CLOTH_PIN_SIMULATION_BOUNDARY);
  RNA_def_property_ui_text(
      prop,
      "Pin Simulation Boundary",
      "Lock the position of the vertices in the simulation falloff area to avoid artifacts and "
      "create a softer transition with unaffected areas");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_cloth_collision", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_CLOTH_USE_COLLISION);
  RNA_def_property_ui_text(prop, "Enable Collision", "Collide with objects during the simulation");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "invert_to_scrape_fill", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_INVERT_TO_SCRAPE_FILL);
  RNA_def_property_ui_text(prop,
                           "Invert to Scrape or Fill",
                           "Use Scrape or Fill brush when inverting this brush instead of "
                           "inverting its displacement direction");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_pressure_strength", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_ALPHA_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(
      prop, "Strength Pressure", "Enable tablet pressure sensitivity for strength");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_offset_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_OFFSET_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(
      prop, "Plane Offset Pressure", "Enable tablet pressure sensitivity for offset");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_pressure_area_radius", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_AREA_RADIUS_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(
      prop, "Area Radius Pressure", "Enable tablet pressure sensitivity for area radius");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_pressure_size", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_SIZE_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Size Pressure", "Enable tablet pressure sensitivity for size");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_pressure_jitter", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_JITTER_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(
      prop, "Jitter Pressure", "Enable tablet pressure sensitivity for jitter");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_pressure_spacing", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_SPACING_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(
      prop, "Spacing Pressure", "Enable tablet pressure sensitivity for spacing");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_pressure_masking", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "mask_pressure");
  RNA_def_property_enum_items(prop, brush_mask_pressure_items);
  RNA_def_property_ui_text(
      prop, "Mask Pressure Mode", "Pen pressure makes texture influence smaller");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_inverse_smooth_pressure", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_INVERSE_SMOOTH_PRESSURE);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(
      prop, "Inverse Smooth Pressure", "Lighter pressure causes more smoothing to be applied");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_plane_trim", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_PLANE_TRIM);
  RNA_def_property_ui_text(
      prop,
      "Use Plane Trim",
      "Limit the distance from the offset plane that a vertex can be affected");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_frontface", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_FRONTFACE);
  RNA_def_property_ui_text(
      prop, "Use Front-Face", "Brush only affects vertices that face the viewer");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_frontface_falloff", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_FRONTFACE_FALLOFF);
  RNA_def_property_ui_text(
      prop, "Use Front-Face Falloff", "Blend brush influence by how much they face the front");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_smooth_stroke", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_SMOOTH_STROKE);
  RNA_def_property_ui_text(
      prop, "Smooth Stroke", "Brush lags behind mouse and follows a smoother path");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_persistent", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_PERSISTENT);
  RNA_def_property_ui_text(prop, "Persistent", "Sculpt on a persistent layer of the mesh");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_bidirectional", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag2", BRUSH_PROJECT_USE_BIDIRECTIONAL);
  RNA_def_property_ui_text(prop,
                           "Bidirectional",
                           "Project vertices both along the projection direction and its "
                           "inverse, choosing the closest intersection.");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_accumulate", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_ACCUMULATE);
  RNA_def_property_ui_text(prop, "Accumulate", "Accumulate stroke daubs on top of each other");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_space_attenuation", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_SPACE_ATTEN);
  RNA_def_property_ui_text(
      prop,
      "Adjust Strength for Spacing",
      "Automatically adjust strength to give consistent results for different spacings");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /* adaptive space is not implemented yet */
  prop = RNA_def_property(srna, "use_adaptive_space", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_ADAPTIVE_SPACE);
  RNA_def_property_ui_text(prop,
                           "Adaptive Spacing",
                           "Space daubs according to surface orientation instead of screen space");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_locked_size", PROP_ENUM, PROP_NONE); /* as an enum */
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "flag");
  RNA_def_property_enum_items(prop, brush_size_unit_items);
  RNA_def_property_ui_text(
      prop, "Size Unit", "Measure brush size relative to the view or the scene");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "color_type", PROP_ENUM, PROP_NONE); /* as an enum */
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "flag");
  RNA_def_property_enum_items(prop, color_gradient_items);
  RNA_def_property_enum_funcs(prop, nullptr, "rna_Brush_use_gradient_set", nullptr);
  RNA_def_property_ui_text(prop, "Color Type", "Use single color or gradient when painting");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_edge_to_edge", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", BRUSH_EDGE_TO_EDGE);
  RNA_def_property_ui_text(prop, "Edge-to-Edge", "Drag anchor brush from edge-to-edge");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /* only for projection paint & vertex paint, TODO: other paint modes. */
  prop = RNA_def_property(srna, "use_alpha", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "flag", BRUSH_LOCK_ALPHA);
  RNA_def_property_ui_text(
      prop, "Affect Alpha", "When this is disabled, lock alpha while painting");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "curve_distance_falloff", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_ui_text(prop, "Falloff Curve", "Editable falloff curve");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "paint_curve", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Paint Curve", "Active paint curve");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "gradient", PROP_POINTER, PROP_NEVER_NULL);
  RNA_def_property_pointer_sdna(prop, nullptr, "gradient");
  RNA_def_property_struct_type(prop, "ColorRamp");
  RNA_def_property_ui_text(prop, "Gradient", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /* gradient source */
  prop = RNA_def_property(srna, "gradient_stroke_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_gradient_items);
  RNA_def_property_ui_text(prop, "Gradient Stroke Mode", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "gradient_fill_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, brush_gradient_fill_items);
  RNA_def_property_ui_text(prop, "Gradient Fill Mode", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /* overlay flags */
  prop = RNA_def_property(srna, "use_primary_overlay", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "overlay_flags", BRUSH_OVERLAY_PRIMARY);
  RNA_def_property_ui_text(prop, "Use Texture Overlay", "Show texture in viewport");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_secondary_overlay", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "overlay_flags", BRUSH_OVERLAY_SECONDARY);
  RNA_def_property_ui_text(prop, "Use Texture Overlay", "Show texture in viewport");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_cursor_overlay", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "overlay_flags", BRUSH_OVERLAY_CURSOR);
  RNA_def_property_ui_text(prop, "Use Cursor Overlay", "Show cursor in viewport");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_cursor_overlay_override", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "overlay_flags", BRUSH_OVERLAY_CURSOR_OVERRIDE_ON_STROKE);
  RNA_def_property_ui_text(prop, "Override Overlay", "Don't show overlay during a stroke");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_primary_overlay_override", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "overlay_flags", BRUSH_OVERLAY_PRIMARY_OVERRIDE_ON_STROKE);
  RNA_def_property_ui_text(prop, "Override Overlay", "Don't show overlay during a stroke");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_secondary_overlay_override", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "overlay_flags", BRUSH_OVERLAY_SECONDARY_OVERRIDE_ON_STROKE);
  RNA_def_property_ui_text(prop, "Override Overlay", "Don't show overlay during a stroke");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /* paint mode flags */
  prop = RNA_def_property(srna, "use_paint_sculpt", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "ob_mode", OB_MODE_SCULPT);
  RNA_def_property_ui_text(prop, "Use Sculpt", "Use this brush in sculpt mode");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_paint_uv_sculpt", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "ob_mode", OB_MODE_EDIT);
  RNA_def_property_ui_text(prop, "Use UV Sculpt", "Use this brush in UV sculpt mode");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_paint_vertex", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "ob_mode", OB_MODE_VERTEX_PAINT);
  RNA_def_property_ui_text(prop, "Use Vertex", "Use this brush in vertex paint mode");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_paint_weight", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "ob_mode", OB_MODE_WEIGHT_PAINT);
  RNA_def_property_ui_text(prop, "Use Weight", "Use this brush in weight paint mode");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_paint_image", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "ob_mode", OB_MODE_TEXTURE_PAINT);
  RNA_def_property_ui_text(prop, "Use Texture", "Use this brush in texture paint mode");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "use_paint_sculpt_curves", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "ob_mode", OB_MODE_SCULPT_CURVES);
  RNA_def_property_ui_text(prop, "Use Sculpt", "Use this brush in sculpt curves mode");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  /* texture */
  prop = RNA_def_property(srna, "texture_slot", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "BrushTextureSlot");
  RNA_def_property_pointer_sdna(prop, nullptr, "mtex");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Texture Slot", "");

  prop = RNA_def_property(srna, "texture", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "mtex.tex");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_CONTEXT_UPDATE);
  RNA_def_property_ui_text(prop, "Texture", "");
  RNA_def_property_update(prop, NC_TEXTURE, "rna_Brush_main_tex_update");

  prop = RNA_def_property(srna, "mask_texture_slot", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "BrushTextureSlot");
  RNA_def_property_pointer_sdna(prop, nullptr, "mask_mtex");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Mask Texture Slot", "");

  prop = RNA_def_property(srna, "mask_texture", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "mask_mtex.tex");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_CONTEXT_UPDATE);
  RNA_def_property_ui_text(prop, "Mask Texture", "");
  RNA_def_property_update(prop, NC_TEXTURE, "rna_Brush_secondary_tex_update");

  prop = RNA_def_property(srna, "texture_overlay_alpha", PROP_INT, PROP_PERCENTAGE);
  RNA_def_property_int_sdna(prop, nullptr, "texture_overlay_alpha");
  RNA_def_property_range(prop, 0, 100);
  RNA_def_property_ui_text(prop, "Texture Overlay Alpha", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "mask_overlay_alpha", PROP_INT, PROP_PERCENTAGE);
  RNA_def_property_int_sdna(prop, nullptr, "mask_overlay_alpha");
  RNA_def_property_range(prop, 0, 100);
  RNA_def_property_ui_text(prop, "Mask Texture Overlay Alpha", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cursor_overlay_alpha", PROP_INT, PROP_PERCENTAGE);
  RNA_def_property_int_sdna(prop, nullptr, "cursor_overlay_alpha");
  RNA_def_property_range(prop, 0, 100);
  RNA_def_property_ui_text(prop, "Mask Texture Overlay Alpha", "");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cursor_color_add", PROP_FLOAT, PROP_COLOR_GAMMA);
  RNA_def_property_float_sdna(prop, nullptr, "add_col");
  RNA_def_property_array(prop, 4);
  RNA_def_property_ui_text(prop, "Add Color", "Color of cursor when adding");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "cursor_color_subtract", PROP_FLOAT, PROP_COLOR_GAMMA);
  RNA_def_property_float_sdna(prop, nullptr, "sub_col");
  RNA_def_property_array(prop, 4);
  RNA_def_property_ui_text(prop, "Subtract Color", "Color of cursor when subtracting");
  RNA_def_property_update(prop, 0, "rna_Brush_update");

  prop = RNA_def_property(srna, "brush_capabilities", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "BrushCapabilities");
  RNA_def_property_pointer_funcs(prop, "rna_Brush_capabilities_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Brush Capabilities", "Brush's capabilities");

  /* brush capabilities (mode-dependent) */
  prop = RNA_def_property(srna, "sculpt_capabilities", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "BrushCapabilitiesSculpt");
  RNA_def_property_pointer_funcs(
      prop, "rna_Sculpt_brush_capabilities_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Sculpt Capabilities", "");

  prop = RNA_def_property(srna, "image_paint_capabilities", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "BrushCapabilitiesImagePaint");
  RNA_def_property_pointer_funcs(
      prop, "rna_Imapaint_brush_capabilities_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Image Paint Capabilities", "");

  prop = RNA_def_property(srna, "vertex_paint_capabilities", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "BrushCapabilitiesVertexPaint");
  RNA_def_property_pointer_funcs(
      prop, "rna_Vertexpaint_brush_capabilities_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Vertex Paint Capabilities", "");

  prop = RNA_def_property(srna, "weight_paint_capabilities", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "BrushCapabilitiesWeightPaint");
  RNA_def_property_pointer_funcs(
      prop, "rna_Weightpaint_brush_capabilities_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Weight Paint Capabilities", "");

  prop = RNA_def_property(srna, "curves_sculpt_settings", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "BrushCurvesSculptSettings");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Curves Sculpt Settings", "");

  prop = RNA_def_property(srna, "mesh_automasking_settings", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MeshAutomaskingSettings");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Mesh Automasking Settings", nullptr);
}

/**
 * A brush stroke is a list of changes to the brush that
 * can occur during a stroke
 *
 * - 3D location of the brush
 * - 2D mouse location
 * - Tablet pressure
 * - Brush type switch
 * - Time
 */
static void rna_def_operator_stroke_element(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "OperatorStrokeElement", "PropertyGroup");
  RNA_def_struct_ui_text(srna, "Operator Stroke Element", "");

  prop = RNA_def_property(srna, "location", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Location", "");

  prop = RNA_def_property(srna, "mouse", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Mouse", "");

  prop = RNA_def_property(srna, "mouse_event", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Mouse Event", "");

  prop = RNA_def_property(srna, "pressure", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Pressure", "Tablet pressure");

  prop = RNA_def_property(srna, "size", PROP_FLOAT, PROP_NONE);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop, "Brush Size", "Brush size in screen space");

  prop = RNA_def_property(srna, "x_tilt", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_range(prop, -1.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Tilt X", "Pen tilt from left (-1.0) to right (+1.0)");

  prop = RNA_def_property(srna, "y_tilt", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_range(prop, -1.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Tilt Y", "Pen tilt from backward (-1.0) to forward (+1.0)");

  /* used in uv painting */
  prop = RNA_def_property(srna, "time", PROP_FLOAT, PROP_UNSIGNED);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_ui_text(prop, "Time", "");

  /* Used for tracking the beginning of a stroke. */
  prop = RNA_def_property(srna, "is_start", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_IDPROPERTY);
  RNA_def_property_ui_text(prop, "Is Stroke Start", "");

  /* XXX: Tool: this will be for pressing a modifier key for a different brush,
   *      e.g. switching to a Smooth brush in the middle of the stroke */

  /* XXX: i don't think blender currently supports the ability to properly do a remappable modifier
   *      in the middle of a stroke */
}

void RNA_def_brush(BlenderRNA *brna)
{
  rna_def_brush(brna);
  rna_def_brush_capabilities(brna);
  rna_def_sculpt_capabilities(brna);
  rna_def_image_paint_capabilities(brna);
  rna_def_vertex_paint_capabilities(brna);
  rna_def_weight_paint_capabilities(brna);
  rna_def_curves_sculpt_options(brna);
  rna_def_brush_texture_slot(brna);
  rna_def_operator_stroke_element(brna);
}

}  // namespace blender

#endif
