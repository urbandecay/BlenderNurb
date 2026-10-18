/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cfloat>
#include <cstdlib>

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"

#include "BLI_math_rotation.h"

#include "BLT_translation.hh"

#include "BKE_customdata.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

const EnumPropertyItem rna_enum_ramp_blend_items[] = {
    {MA_RAMP_BLEND, "MIX", 0, "Mix", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_DARK, "DARKEN", 0, "Darken", ""},
    {MA_RAMP_MULT, "MULTIPLY", 0, "Multiply", ""},
    {MA_RAMP_BURN, "BURN", 0, "Color Burn", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_LIGHT, "LIGHTEN", 0, "Lighten", ""},
    {MA_RAMP_SCREEN, "SCREEN", 0, "Screen", ""},
    {MA_RAMP_DODGE, "DODGE", 0, "Color Dodge", ""},
    {MA_RAMP_ADD, "ADD", 0, "Add", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_OVERLAY, "OVERLAY", 0, "Overlay", ""},
    {MA_RAMP_SOFT, "SOFT_LIGHT", 0, "Soft Light", ""},
    {MA_RAMP_LINEAR, "LINEAR_LIGHT", 0, "Linear Light", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_DIFF, "DIFFERENCE", 0, "Difference", ""},
    {MA_RAMP_EXCLUSION, "EXCLUSION", 0, "Exclusion", ""},
    {MA_RAMP_SUB, "SUBTRACT", 0, "Subtract", ""},
    {MA_RAMP_DIV, "DIVIDE", 0, "Divide", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_HUE, "HUE", 0, "Hue", ""},
    {MA_RAMP_SAT, "SATURATION", 0, "Saturation", ""},
    {MA_RAMP_COLOR, "COLOR", 0, "Color", ""},
    {MA_RAMP_VAL, "VALUE", 0, "Value", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

}

#ifdef RNA_RUNTIME

#  include "MEM_guardedalloc.h"

#  include "DNA_meshdata_types.h"
#  include "DNA_node_types.h"
#  include "DNA_object_types.h"
#  include "DNA_screen_types.h"
#  include "DNA_space_types.h"

#  include "BLI_string_utf8.h"

#  include "BKE_attribute.h"
#  include "BKE_attribute.hh"
#  include "BKE_colorband.hh"
#  include "BKE_context.hh"
#  include "BKE_editmesh.hh"
#  include "BKE_lib_id.hh"
#  include "BKE_main.hh"
#  include "BKE_material.hh"
#  include "BKE_mesh.hh"
#  include "BKE_mesh_types.hh"
#  include "BKE_node.hh"
#  include "BKE_paint.hh"
#  include "BKE_scene.hh"
#  include "BKE_texture.h"
#  include "BKE_workspace.hh"

#  include "DEG_depsgraph.hh"
#  include "DEG_depsgraph_build.hh"

#  include "ED_image.hh"
#  include "ED_node.hh"
#  include "ED_screen.hh"

namespace blender {

static void rna_Material_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);

  DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
}

static void rna_Material_update_previews(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);

  WM_main_add_notifier(NC_MATERIAL | ND_SHADING_PREVIEW, ma);
}

static void rna_Material_draw_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);

  DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, ma);
}

static int rna_Material_blend_method_get(PointerRNA *ptr)
{
  Material *material = id_cast<Material *>(ptr->owner_id);
  switch (material->surface_render_method) {
    case MA_SURFACE_METHOD_DEFERRED:
      return MA_BM_HASHED;
    case MA_SURFACE_METHOD_FORWARD:
      return MA_BM_BLEND;
  }
  return MA_BM_HASHED;
}

static void rna_Material_blend_method_set(PointerRNA *ptr, int new_blend_method)
{
  Material *material = id_cast<Material *>(ptr->owner_id);
  switch (new_blend_method) {
    case MA_BM_SOLID:
    case MA_BM_CLIP:
    case MA_BM_HASHED:
      material->surface_render_method = MA_SURFACE_METHOD_DEFERRED;
      break;
    case MA_BM_BLEND:
      material->surface_render_method = MA_SURFACE_METHOD_FORWARD;
      break;
  }
}

static void rna_Material_render_method_set(PointerRNA *ptr, int new_render_method)
{
  Material *material = id_cast<Material *>(ptr->owner_id);
  material->surface_render_method = eMaterial_SurfaceRenderMethod(new_render_method);

  /* Still sets the legacy property for forward compatibility. */
  switch (new_render_method) {
    case MA_SURFACE_METHOD_DEFERRED:
      material->blend_method = MA_BM_HASHED;
      break;
    case MA_SURFACE_METHOD_FORWARD:
      material->blend_method = MA_BM_BLEND;
      break;
  }
}
static void rna_Material_transparent_shadow_set(PointerRNA *ptr, bool new_value)
{
  Material *material = id_cast<Material *>(ptr->owner_id);
  SET_FLAG_FROM_TEST(material->blend_flag, new_value, MA_BL_TRANSPARENT_SHADOW);
  /* Still sets the legacy property for forward compatibility. */
  material->blend_shadow = new_value ? MA_BS_HASHED : MA_BS_SOLID;
}

static bool rna_Material_use_nodes_get(PointerRNA * /*ptr*/)
{
  /* #use_nodes is deprecated. All materials now use nodes. */
  return true;
}

static void rna_Material_use_nodes_set(PointerRNA * /*ptr*/, bool /*new_value*/)
{
  /* #use_nodes is deprecated. Setting the property has no effect.
   * Note: Users will get a warning through the RNA deprecation warning, so no need to log a
   * warning here. */
  return;
}

MTex *rna_mtex_texture_slots_add(ID *self_id, bContext *C, ReportList *reports)
{
  MTex *mtex = BKE_texture_mtex_add_id(self_id, -1);
  if (mtex == nullptr) {
    BKE_reportf(reports, RPT_ERROR, "Maximum number of textures added %d", MAX_MTEX);
    return nullptr;
  }

  /* for redraw only */
  WM_event_add_notifier(C, NC_TEXTURE, CTX_data_scene(C));

  return mtex;
}

MTex *rna_mtex_texture_slots_create(ID *self_id, bContext *C, ReportList *reports, int index)
{
  MTex *mtex;

  if (index < 0 || index >= MAX_MTEX) {
    BKE_reportf(reports, RPT_ERROR, "Index %d is invalid", index);
    return nullptr;
  }

  mtex = BKE_texture_mtex_add_id(self_id, index);

  /* for redraw only */
  WM_event_add_notifier(C, NC_TEXTURE, CTX_data_scene(C));

  return mtex;
}

void rna_mtex_texture_slots_clear(ID *self_id, bContext *C, ReportList *reports, int index)
{
  MTex **mtex_ar;
  short act;

  give_active_mtex(self_id, &mtex_ar, &act);

  if (mtex_ar == nullptr) {
    BKE_report(reports, RPT_ERROR, "Mtex not found for this type");
    return;
  }

  if (index < 0 || index >= MAX_MTEX) {
    BKE_reportf(reports, RPT_ERROR, "Index %d is invalid", index);
    return;
  }

  if (mtex_ar[index]) {
    id_us_min(id_cast<ID *>(mtex_ar[index]->tex));
    MEM_delete(mtex_ar[index]);
    mtex_ar[index] = nullptr;
    DEG_id_tag_update(self_id, 0);
  }

  /* for redraw only */
  WM_event_add_notifier(C, NC_TEXTURE, CTX_data_scene(C));
}

}  // namespace blender

#else

namespace blender {

static void rna_def_material_display(StructRNA *srna)
{
  PropertyRNA *prop;

  prop = RNA_def_property(srna, "diffuse_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "r");
  RNA_def_property_array(prop, 4);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Diffuse Color", "Diffuse color of the material");
  /* See #82514 for details, for now re-define defaults here. Keep in sync with
   * #DNA_material_defaults.h */
  static const float diffuse_color_default[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  RNA_def_property_float_array_default(prop, diffuse_color_default);
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "specular_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "specr");
  RNA_def_property_array(prop, 3);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Specular Color", "Specular color of the material");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "roughness", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "roughness");
  RNA_def_property_range(prop, 0, 1);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Roughness", "Roughness of the material");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "specular_intensity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "spec");
  RNA_def_property_range(prop, 0, 1);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Specular", "How intense (bright) the specular reflection is");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "metallic", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "metallic");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Metallic", "Amount of mirror reflection for raytrace");
  RNA_def_property_update(prop, 0, "rna_Material_update");

  /* Freestyle line color */
  prop = RNA_def_property(srna, "line_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "line_col");
  RNA_def_property_array(prop, 4);
  RNA_def_property_ui_text(prop, "Line Color", "Line color used for Freestyle line rendering");
  RNA_def_property_update(prop, 0, "rna_Material_update");

  prop = RNA_def_property(srna, "line_priority", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "line_priority");
  RNA_def_property_range(prop, 0, 32767);
  RNA_def_property_ui_text(
      prop, "Line Priority", "The line color of a higher priority is used at material boundaries");
  RNA_def_property_update(prop, 0, "rna_Material_update");
}

void RNA_def_material(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* Render Preview Types */
  static const EnumPropertyItem preview_type_items[] = {
      {MA_FLAT, "FLAT", ICON_MATPLANE, "Flat", "Flat XY plane"},
      {MA_SPHERE, "SPHERE", ICON_MATSPHERE, "Sphere", "Sphere"},
      {MA_CUBE, "CUBE", ICON_MATCUBE, "Cube", "Cube"},
      {MA_HAIR, "HAIR", ICON_CURVES, "Hair", "Hair strands"},
      {MA_SHADERBALL, "SHADERBALL", ICON_MATSHADERBALL, "Shader Ball", "Shader ball"},
      {MA_CLOTH, "CLOTH", ICON_MATCLOTH, "Cloth", "Cloth"},
      {MA_FLUID, "FLUID", ICON_MATFLUID, "Fluid", "Fluid"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_eevee_volume_isect_method_items[] = {
      {MA_VOLUME_ISECT_FAST,
       "FAST",
       0,
       "Fast",
       "Each face is considered as a medium interface. Gives correct results for manifold "
       "geometry that contains no inner parts."},
      {MA_VOLUME_ISECT_ACCURATE,
       "ACCURATE",
       0,
       "Accurate",
       "Faces are considered as medium interface only when they have different consecutive "
       "facing. Gives correct results as long as the max ray depth is not exceeded. Have "
       "significant memory overhead compared to the fast method."},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_eevee_thickness_method_items[] = {
      {MA_THICKNESS_SPHERE,
       "SPHERE",
       0,
       "Sphere",
       "Approximate the object as a sphere whose diameter is equal to the thickness defined by "
       "the node tree"},
      {MA_THICKNESS_SLAB,
       "SLAB",
       0,
       "Slab",
       "Approximate the object as an infinite slab of thickness defined by the node tree"},
      {0, nullptr, 0, nullptr, nullptr},
  };

#  if 1 /* Delete this section once we remove old eevee. */
  static const EnumPropertyItem prop_eevee_blend_items[] = {
      {MA_BM_SOLID, "OPAQUE", 0, "Opaque", "Render surface without transparency"},
      {MA_BM_CLIP,
       "CLIP",
       0,
       "Alpha Clip",
       "Use the alpha threshold to clip the visibility (binary visibility)"},
      {MA_BM_HASHED,
       "HASHED",
       0,
       "Alpha Hashed",
       "Use noise to dither the binary visibility (works well with multi-samples)"},
      {MA_BM_BLEND,
       "BLEND",
       0,
       "Alpha Blend",
       "Render polygon transparent, depending on alpha channel of the texture"},
      {0, nullptr, 0, nullptr, nullptr},
  };
#  endif

  static const EnumPropertyItem prop_eevee_surface_render_method_items[] = {
      {MA_SURFACE_METHOD_DEFERRED,
       "DITHERED",
       0,
       "Dithered",
       "Allows for grayscale hashed transparency, and compatible with render passes and "
       "raytracing. Also known as deferred rendering."},
      {MA_SURFACE_METHOD_FORWARD,
       "BLENDED",
       0,
       "Blended",
       "Allows for colored transparency, but incompatible with render passes and raytracing. Also "
       "known as forward rendering."},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_displacement_method_items[] = {
      {MA_DISPLACEMENT_BUMP,
       "BUMP",
       0,
       "Bump Only",
       "Bump mapping to simulate the appearance of displacement"},
      {MA_DISPLACEMENT_DISPLACE,
       "DISPLACEMENT",
       0,
       "Displacement Only",
       "Use true displacement of surface only, requires fine subdivision"},
      {MA_DISPLACEMENT_BOTH,
       "BOTH",
       0,
       "Displacement and Bump",
       "Combination of true displacement and bump mapping for finer detail"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "Material", "ID");
  RNA_def_struct_ui_text(
      srna,
      "Material",
      "Material data-block to define the appearance of geometric objects for rendering");
  RNA_def_struct_ui_icon(srna, ICON_MATERIAL_DATA);

  prop = RNA_def_property(srna, "surface_render_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_eevee_surface_render_method_items);
  RNA_def_property_ui_text(prop,
                           "Surface Render Method",
                           "Controls the blending and the compatibility with certain features");
  /* Setter function for forward compatibility. */
  RNA_def_property_enum_funcs(prop, nullptr, "rna_Material_render_method_set", nullptr);
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "displacement_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_displacement_method_items);
  RNA_def_property_ui_text(prop, "Displacement Method", "Method to use for the displacement");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

#  if 1 /* Delete this section once we remove old eevee. */
  /* Blending (only Eevee for now) */
  prop = RNA_def_property(srna, "blend_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_eevee_blend_items);
  RNA_def_property_ui_text(
      prop,
      "Blend Mode",
      "Blend Mode for Transparent Faces (Deprecated: use 'surface_render_method')");
  RNA_def_property_enum_funcs(
      prop, "rna_Material_blend_method_get", "rna_Material_blend_method_set", nullptr);
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_MATERIAL);
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "alpha_threshold", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0, 1);
  RNA_def_property_ui_text(prop,
                           "Clip Threshold",
                           "A pixel is rendered only if its alpha value is above this threshold");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");
#  endif

  prop = RNA_def_property(srna, "use_transparency_overlap", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "blend_flag", MA_BL_HIDE_BACKFACE);
  RNA_def_property_ui_text(prop,
                           "Use Transparency Overlap",
                           "Render multiple transparent layers "
                           "(may introduce transparency sorting problems)");

#  if 1 /* This should be deleted in Blender 4.5 */
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");
  prop = RNA_def_property(srna, "show_transparent_back", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "blend_flag", MA_BL_HIDE_BACKFACE);
  RNA_def_property_ui_text(
      prop,
      "Show Backface",
      "Render multiple transparent layers "
      "(may introduce transparency sorting problems) (Deprecated: use 'use_tranparency_overlap')");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");
#  endif

  prop = RNA_def_property(srna, "use_backface_culling", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_CULL_BACKFACE);
  RNA_def_property_ui_text(
      prop, "Backface Culling", "Use back face culling to hide the back side of faces");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_backface_culling_shadow", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_CULL_BACKFACE_SHADOW);
  RNA_def_property_ui_text(
      prop, "Shadow Backface Culling", "Use back face culling when casting shadows");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_backface_culling_lightprobe_volume", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(
      prop, nullptr, "blend_flag", MA_BL_LIGHTPROBE_VOLUME_DOUBLE_SIDED);
  RNA_def_property_ui_text(
      prop,
      "Light Probe Volume Backface Culling",
      "Consider material single sided for light probe volume capture. "
      "Additionally helps rejecting probes inside the object to avoid light leaks.");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_transparent_shadow", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_TRANSPARENT_SHADOW);
  RNA_def_property_boolean_funcs(prop, nullptr, "rna_Material_transparent_shadow_set");
  RNA_def_property_ui_text(
      prop,
      "Transparent Shadows",
      "Use transparent shadows for this material if it contains a Transparent BSDF, "
      "disabling will render faster but not give accurate shadows");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_raytrace_refraction", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_SS_REFRACTION);
  RNA_def_property_ui_text(
      prop,
      "Raytrace Transmission",
      "Use raytracing to determine transmitted color instead of using only light probes. "
      "This prevents the surface from contributing to the lighting of surfaces not using this "
      "setting.");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

#  if 1 /* This should be deleted in Blender 4.5 */
  prop = RNA_def_property(srna, "use_screen_refraction", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_SS_REFRACTION);
  RNA_def_property_ui_text(
      prop,
      "Raytrace Transmission",
      "Use raytracing to determine transmitted color instead of using only light probes. "
      "This prevents the surface from contributing to the lighting of surfaces not using this "
      "setting. Deprecated: use 'use_raytrace_refraction'.");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_sss_translucency", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_TRANSLUCENCY);
  RNA_def_property_ui_text(
      prop, "Subsurface Translucency", "Add translucency effect to subsurface (Deprecated)");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "refraction_depth", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "refract_depth");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop,
                           "Refraction Depth",
                           "Approximate the thickness of the object to compute two refraction "
                           "events (0 is disabled) (Deprecated)");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");
#  endif

  prop = RNA_def_property(srna, "thickness_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_eevee_thickness_method_items);
  RNA_def_property_ui_text(prop,
                           "Thickness Mode",
                           "Approximation used to model the light interactions inside the object");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_thickness_from_shadow", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_THICKNESS_FROM_SHADOW);
  RNA_def_property_ui_text(prop,
                           "Thickness From Shadow",
                           "Use the shadow maps from shadow casting lights "
                           "to refine the thickness defined by the material node tree");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "volume_intersection_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_eevee_volume_isect_method_items);
  RNA_def_property_ui_text(
      prop,
      "Volume Intersection Method",
      "Determines which inner part of the mesh will produce volumetric effect");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "max_vertex_displacement", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "inflate_bounds");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop,
                           "Max Vertex Displacement",
                           "The max distance a vertex can be displaced. "
                           "Displacements over this threshold may cause visibility issues.");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  /* For Preview Render */
  prop = RNA_def_property(srna, "preview_render_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "pr_type");
  RNA_def_property_enum_items(prop, preview_type_items);
  RNA_def_property_ui_text(prop, "Preview Render Type", "Type of preview render");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_MATERIAL);
  RNA_def_property_update(prop, 0, "rna_Material_update_previews");

  prop = RNA_def_property(srna, "use_preview_world", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "pr_flag", MA_PREVIEW_WORLD);
  RNA_def_property_ui_text(
      prop, "Preview World", "Use the current world background to light the preview render");
  RNA_def_property_update(prop, 0, "rna_Material_update_previews");

  prop = RNA_def_property(srna, "pass_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "index");
  RNA_def_property_ui_text(
      prop, "Pass Index", "Index number for the \"Material Index\" render pass");
  RNA_def_property_update(prop, NC_OBJECT, "rna_Material_update");

  /* nodetree */
  prop = RNA_def_property(srna, "node_tree", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "nodetree");
  RNA_def_property_clear_flag(prop, PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Node Tree", "Node tree for node based materials");

  prop = RNA_def_property(srna, "use_nodes", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_nodes", 1);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Use Nodes", "Use shader nodes to render the material");
  RNA_def_property_boolean_funcs(prop, "rna_Material_use_nodes_get", "rna_Material_use_nodes_set");
  RNA_def_property_deprecated(prop,
                              "Unused but kept for compatibility reasons. Setting the property "
                              "has no effect, and getting it always returns True.",
                              500,
                              600);

  /* common */
  rna_def_animdata_common(srna);

  rna_def_material_display(srna);

  RNA_api_material(srna);
}

static void rna_def_texture_slots(BlenderRNA *brna,
                                  PropertyRNA *cprop,
                                  const char *structname,
                                  const char *structname_slots)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, structname_slots);
  srna = RNA_def_struct(brna, structname_slots, nullptr);
  RNA_def_struct_sdna(srna, "ID");
  RNA_def_struct_ui_text(srna, "Texture Slots", "Collection of texture slots");

  /* functions */
  func = RNA_def_function(srna, "add", "rna_mtex_texture_slots_add");
  RNA_def_function_flag(func,
                        FUNC_USE_SELF_ID | FUNC_NO_SELF | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "mtex", structname, "", "The newly initialized mtex");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "create", "rna_mtex_texture_slots_create");
  RNA_def_function_flag(func,
                        FUNC_USE_SELF_ID | FUNC_NO_SELF | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_int(
      func, "index", 0, 0, INT_MAX, "Index", "Slot index to initialize", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "mtex", structname, "", "The newly initialized mtex");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "clear", "rna_mtex_texture_slots_clear");
  RNA_def_function_flag(func,
                        FUNC_USE_SELF_ID | FUNC_NO_SELF | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_int(func, "index", 0, 0, INT_MAX, "Index", "Slot index to clear", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
}

void rna_def_mtex_common(BlenderRNA *brna,
                         StructRNA *srna,
                         const char *begin,
                         const char *activeget,
                         const char *activeset,
                         const char *activeeditable,
                         const char *structname,
                         const char *structname_slots,
                         const char *update,
                         const char *update_index)
{
  PropertyRNA *prop;

  /* mtex */
  prop = RNA_def_property(srna, "texture_slots", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, structname);
  RNA_def_property_collection_funcs(prop,
                                    begin,
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_dereference_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop, "Textures", "Texture slots defining the mapping and influence of textures");
  rna_def_texture_slots(brna, prop, structname, structname_slots);

  prop = RNA_def_property(srna, "active_texture", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Texture");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_REFCOUNT);
  if (activeeditable) {
    RNA_def_property_editable_func(prop, activeeditable);
  }
  RNA_def_property_pointer_funcs(prop, activeget, activeset, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Active Texture", "Active texture slot being displayed");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING_LINKS, update);

  prop = RNA_def_property(srna, "active_texture_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "texact");
  RNA_def_property_range(prop, 0, MAX_MTEX - 1);
  RNA_def_property_ui_text(prop, "Active Texture Index", "Index of active texture slot");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING_LINKS, update_index);
}

}  // namespace blender

#endif
