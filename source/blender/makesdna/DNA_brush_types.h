/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_brush_enums.h"
#include "DNA_curve_types.h"
#include "DNA_defs.h"
#include "DNA_object_enums.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h" /* for MTex */

namespace blender {

struct CurveMapping;
struct Image;
struct MTex;
struct BrushCurvesSculptSettings {
  /** Number of curves added by the add brush. */
  int add_amount = 0;
  /** Number of control points in new curves added by the add brush. */
  int points_per_curve = 0;
  eBrushCurvesSculptFlag flag = {};
  /** When shrinking curves, they shouldn't become shorter than this length. */
  float minimum_length = 0;
  /** Length of newly added curves when it is not interpolated from other curves. */
  float curve_length = 0;
  /** Minimum distance between curve root points used by the Density brush. */
  float minimum_distance = 0;
  /** The initial radius of curve. */
  float curve_radius = 0;
  /** How often the Density brush tries to add a new curve. */
  int density_add_attempts = 0;
  eBrushCurvesSculptDensityMode density_mode = BRUSH_CURVES_SCULPT_DENSITY_MODE_AUTO;
  char _pad[7] = {};
  struct CurveMapping *curve_parameter_falloff = nullptr;
};

/** Max number of propagation steps for automasking settings. */
#define AUTOMASKING_BOUNDARY_EDGES_MAX_PROPAGATION_STEPS 20
/**
 * \note Any change to members that is user visible and that may make the brush differ from the one
 * saved in the asset library should be followed by a #BKE_brush_tag_unsaved_changes() call.
 */
struct Brush {
#ifdef __cplusplus
  DNA_DEFINE_CXX_METHODS(Brush)
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_BR;
#endif

  ID id;

  struct CurveMapping *curve_distance_falloff = nullptr;
  struct MTex mtex;
  struct MTex mask_mtex;

  PreviewImage *preview = nullptr;
  /** Color gradient. */
  struct ColorBand *gradient = nullptr;
  struct PaintCurve *paint_curve = nullptr;

  float normal_weight = 0.0f;
  /** Rake actual data (not texture), used for sculpt. */
  float rake_factor = 0;

  /** Blend mode. */
  short blend = 0;
  /** #eObjectMode: to see if the brush is compatible, use for display only. */
  short ob_mode = OB_MODE_ALL_PAINT;
  /** Brush weight. */
  float weight = 1.0f; /* weight of brush 0 - 1.0 */
  /** Brush diameter. */
  int size = 70; /* diameter of the brush in pixels */
  /** General purpose flags. */
  eBrushFlags flag = BRUSH_ALPHA_PRESSURE | BRUSH_SPACE_ATTEN;
  eBrushFlags2 flag2 = {};
  eBrushSamplingFlags sampling_flag = BRUSH_PAINT_ANTIALIASING;

  /**
   * How the stroke behaves when used via the modal operators.
   */
  eBrushStrokeType stroke_method = BRUSH_STROKE_SPACE;
  char _pad[7] = {};
  /** Number of samples used to smooth the stroke. */
  int input_samples = 1;

  /** Pressure influence for mask. */
  BrushMaskPressureFlags mask_pressure = {};
  /** Jitter the position of the brush. */
  float jitter = 0.0f;
  /** Absolute jitter in pixels. */
  int jitter_absolute = 0;
  eOverlayFlags overlay_flags = {};
  /** Spacing of paint operations. */
  int spacing = 10;
  /** Turning radius (in pixels) for smooth stroke. */
  int smooth_stroke_radius = 75;
  /** Higher values limit fast changes in the stroke direction. */
  float smooth_stroke_factor = 0.9f;
  /** Paint operations / second (airbrush). */
  float rate = 0.1f;

  /** Color. */
  float color[3] = {1.0f, 1.0f, 1.0f};
  eBrushColorJitterSettings_Flag color_jitter_flag = {};
  float hsv_jitter[3] = {};

  /** Color jitter pressure curves. */
  struct CurveMapping *curve_rand_hue = nullptr;
  struct CurveMapping *curve_rand_saturation = nullptr;
  struct CurveMapping *curve_rand_value = nullptr;

  struct CurveMapping *curve_size = nullptr;
  struct CurveMapping *curve_strength = nullptr;
  struct CurveMapping *curve_jitter = nullptr;

  /** Opacity. */
  float alpha = 1.0f; /* brush strength/intensity probably variable should be renamed? */
  /** Hardness */
  float hardness = 0.0f;
  /** Flow */
  float flow = 0;
  /** Wet Mix */
  float wet_mix = 0;
  float wet_persistence = 0;
  /** Density */
  float density = 0;
  ePaintBrush_flag paint_flags = {};

  /** Tip Shape */
  /* Factor that controls the shape of the brush tip by rounding the corners of a square. */
  /* 0.0 value produces a square, 1.0 produces a circle. */
  float tip_roundness = 1.0f;
  float tip_scale_x = 1.0f;

  /** Background color. */
  float secondary_color[3] = {0, 0, 0};

  /* Deprecated sRGB color for forward compatibility. */
  DNA_DEPRECATED float rgb[3] = {1.0f, 1.0f, 1.0f};
  DNA_DEPRECATED float secondary_rgb[3] = {0, 0, 0};

  /** Rate */
  float dash_ratio = 1.0f;
  int dash_samples = 20;

  /** The direction of movement for sculpt vertices. */
  eBrushSculpt_DispDir sculpt_plane = SCULPT_DISP_DIR_AREA;

  /** Offset for plane brushes (clay, flatten, fill, scrape). */
  float plane_offset = 0.0f;

  int gradient_spacing = 0;
  /** Source for stroke color gradient application. */
  eBrushGradientSourceStroke gradient_stroke_mode = BRUSH_GRADIENT_PRESSURE;
  /** Source for fill brush color gradient application. */
  eBrushGradientSourceFill gradient_fill_mode = BRUSH_GRADIENT_LINEAR;

  /**
   * Tag to indicate to the user that the brush has been changed since being imported. Only set for
   * brushes that are actually imported (must have #ID.lib set). Runtime only.
   */
  char has_unsaved_changes = 0;

  /** Projection shape (sphere, circle). */
  eBrushFalloffShape falloff_shape = PAINT_FALLOFF_SHAPE_SPHERE;
  float falloff_angle = 0;

  /** Active sculpt brush type. */
  eBrushSculptType sculpt_brush_type = SCULPT_BRUSH_TYPE_DRAW;
  /** Active vertex paint. */
  eBrushVertexPaintType vertex_brush_type = VPAINT_BRUSH_TYPE_DRAW;
  /** Active weight paint. */
  eBrushWeightPaintType weight_brush_type = WPAINT_BRUSH_TYPE_DRAW;
  /** Active image paint brush type. */
  eBrushImagePaintType image_brush_type = IMAGE_PAINT_BRUSH_TYPE_DRAW;
  /** Enum eBrushMaskTool, only used if sculpt_brush_type is SCULPT_BRUSH_TYPE_MASK. */
  BrushMaskTool mask_tool = BRUSH_MASK_DRAW;
  /* Reserved for the four removed Grease Pencil brush-mode enums. */
  char _pad_gpencil_brush_modes[4] = {};
  /** Active curves sculpt brush type. */
  eBrushCurvesSculptType curves_sculpt_brush_type = CURVES_SCULPT_BRUSH_TYPE_COMB;

  char _pad1[2] = {};

  float autosmooth_factor = 0.0f;

  float tilt_strength_factor = 0;

  float topology_rake_factor = 0.0f;

  float crease_pinch_factor = 0.5f;

  float normal_radius_factor = 0.5f;
  float area_radius_factor = 0.5f;
  float wet_paint_radius_factor = 0.5f;

  float plane_trim = 0.5f;
  /** Affectable height of brush (layer height for layer tool, i.e.). */
  float height = 0;

  /* Plane Brush */
  float plane_height = 0;
  float plane_depth = 0;
  float stabilize_normal = 0;
  float stabilize_plane = 0;
  eBrushPlaneInversionMode plane_inversion_mode = BRUSH_PLANE_INVERT_DISPLACEMENT;

  float texture_sample_bias = 0; /* value to added to texture samples */

  /**
   * This preset is used to specify an exact function used for the distance falloff instead
   * of doing a Bezier spline evaluation via CurveMapping for performance reasons.
   * \see #eBrushCurvePreset and #eCurveMappingPreset
   */
  eBrushCurvePreset curve_distance_falloff_preset = BRUSH_CURVE_CUSTOM;

  /* Maximum distance to search fake neighbors from a vertex. */
  float disconnected_distance_max = 0.1f;

  eBrushDeformTarget deform_target = BRUSH_DEFORM_TARGET_GEOMETRY;

  /* automasking */
  DNA_DEPRECATED int automasking_flags = 0;
  DNA_DEPRECATED int automasking_boundary_edges_propagation_steps = 1;

  DNA_DEPRECATED float automasking_start_normal_limit = 0.34906585f; /* 20 degrees */
  DNA_DEPRECATED float automasking_start_normal_falloff = 0.25f;
  DNA_DEPRECATED float automasking_view_normal_limit = 1.570796; /* 90 degrees */
  DNA_DEPRECATED float automasking_view_normal_falloff = 0.25f;

  eBrushElasticDeformType elastic_deform_type = BRUSH_ELASTIC_DEFORM_GRAB;
  float elastic_deform_volume_preservation = 0;

  /* snake hook */
  eBrushSnakeHookDeformType snake_hook_deform_type = BRUSH_SNAKE_HOOK_DEFORM_FALLOFF;

  /* pose */
  eBrushPoseDeformType pose_deform_type = BRUSH_POSE_DEFORM_ROTATE_TWIST;
  float pose_offset = 0;
  int pose_smooth_iterations = 4;
  int pose_ik_segments = 1;
  eBrushPoseOriginType pose_origin_type = BRUSH_POSE_ORIGIN_TOPOLOGY;

  /* boundary */
  eBrushBoundaryDeformType boundary_deform_type = BRUSH_BOUNDARY_DEFORM_BEND;
  eBrushBoundaryFalloffType boundary_falloff_type = BRUSH_BOUNDARY_FALLOFF_CONSTANT;
  float boundary_offset = 0;

  /* cloth */
  eBrushClothDeformType cloth_deform_type = BRUSH_CLOTH_DEFORM_DRAG;
  eBrushClothForceFalloffType cloth_force_falloff_type = BRUSH_CLOTH_FORCE_FALLOFF_RADIAL;
  eBrushClothSimulationAreaType cloth_simulation_area_type = BRUSH_CLOTH_SIMULATION_AREA_LOCAL;

  float cloth_mass = 1;
  float cloth_damping = 0.01;

  float cloth_sim_limit = 2.5f;
  float cloth_sim_falloff = 0.75f;

  float cloth_constraint_softbody_strength = 0;

  /* smooth */
  eBrushSmoothDeformType smooth_deform_type = BRUSH_SMOOTH_DEFORM_LAPLACIAN;
  float surface_smooth_shape_preservation = 0;
  float surface_smooth_current_vertex = 0;
  int surface_smooth_iterations = 0;

  /* multiplane scrape */
  float multiplane_scrape_angle = 0;

  /* smear */
  eBrushSmearDeformType smear_deform_type = BRUSH_SMEAR_DEFORM_DRAG;

  /* slide/relax */
  eBrushSlideDeformType slide_deform_type = BRUSH_SLIDE_DEFORM_DRAG;

  /* Scene Project brush */
  eBrushProjectRayDirection project_ray_direction_type = BRUSH_PROJECT_RAY_DIRECTION_VIEW_NORMAL;
  char _pad2[3] = {};
  float minimum_distance = 0.0f;

  /* overlay */
  int texture_overlay_alpha = 33;
  int mask_overlay_alpha = 33;
  int cursor_overlay_alpha = 33;

  float unprojected_size = 0.10f; /* diameter of the brush in Blender units */

  /* soften/sharpen */
  float sharp_threshold = 0;
  int blur_kernel_radius = 2;
  eBlurKernelType blur_mode = KERNEL_GAUSSIAN;

  /* fill tool */
  float fill_threshold = 0.2f;

  float add_col[4] = {1.0, 0.39, 0.39, 0.9};
  float sub_col[4] = {0.39, 0.39, 1.0, 0.9};

  float stencil_pos[2] = {256, 256};
  float stencil_dimension[2] = {256, 256};

  float mask_stencil_pos[2] = {256, 256};
  float mask_stencil_dimension[2] = {256, 256};

  /* Reserved for the removed Grease Pencil brush settings pointer. */
  void *_pad_gpencil_settings = nullptr;
  struct BrushCurvesSculptSettings *curves_sculpt_settings = nullptr;

  DNA_DEPRECATED int automasking_cavity_blur_steps = 0;
  DNA_DEPRECATED float automasking_cavity_factor = 1.0f;

  DNA_DEPRECATED struct CurveMapping *automasking_cavity_curve = nullptr;
  struct MeshAutomaskingSettings *mesh_automasking_settings = nullptr;
};

struct PaletteColor {
  struct PaletteColor *next = nullptr, *prev = nullptr;
  /* Two values, one to store color, other to store values for sculpt/weight. */
  float color[3] = {};
  float value = 0;

  /* For forward compatibility. */
  DNA_DEPRECATED float rgb[3] = {};
  float _pad = {};
};

struct Palette {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_PAL;
#endif

  ID id;

  /** Pointer to individual colors. */
  ListBaseT<PaletteColor> colors = {nullptr, nullptr};

  int active_color = 0;
  char _pad[4] = {};
};

struct PaintCurvePoint {
  /** Bezier handle. */
  BezTriple bez = {};
  /** Pressure on that point. */
  float pressure = 0;
};

struct PaintCurve {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_PC;
#endif

  ID id;
  /** Points of curve. */
  PaintCurvePoint *points = nullptr;
  int tot_points = 0;
  /** Index where next point will be added. */
  int add_index = 0;
};

}  // namespace blender
