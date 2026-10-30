/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 * Click drawing and construction overlays ported from radCAD.
 */

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_fit.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_tessellate.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_unit.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_curve_types.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_workspace_types.h"
#include "ED_gizmo_utils.hh"
#include "ED_numinput.hh"
#include "ED_nurbs.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_transform_snap_object_context.hh"
#include "ED_view3d.hh"
#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "MEM_guardedalloc.h"
#include "RNA_access.hh"
#include "RNA_define.hh"
#include "UI_interface.hh"
#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"
#include "curve_intern.hh"

namespace blender {
namespace {
using namespace bke::nurbs;

struct NativeCenter {
  Object *object;
  double3 position;
  float3 normal;
  eSnapMode type;
};

enum class DrawNumericMode : uint8_t {
  Automatic,
  Length,
  Radius,
  Diameter,
  Sagitta,
  Angle,
  Segments,
  Width,
  Height,
};

struct DrawData {
  std::unique_ptr<ed::nurbs::TopologyPreview> source;
  ARegion *region = nullptr;
  ViewOpsData *navigation = nullptr;
  void *draw_handle = nullptr;
  void *draw_handle_pixel = nullptr;
  ed::transform::SnapObjectContext *snap = nullptr;
  Vector<NativeCenter> centers;
  Vector<Object *> center_objects;
  eSnapMode cached_center_modes = SCE_SNAP_TO_NONE;
  Vector<double3> points;
  Vector<int64_t> point_vertices;
  int64_t current_vertex = 0;
  Vector<float3> wire;
  double3 current = double3(0);
  eSnapMode snap_type = SCE_SNAP_TO_NONE;
  float3 plane_origin, plane_normal;
  int axis = -1;
  std::optional<float3> shift_direction;
  float3 face_normal = float3(0, 0, 1);
  bool locked = true;
  bool spline = false;
  int shape = 0; /* 1 ellipse, 2 polygon, 3 rectangle. */
  int shape_mode = 0;
  bool square = false;
  bool keep_foci = false;
  std::optional<double> width, height;
  Vector<double3> shape_points;
  int arc_mode = -1;
  bool circle = false;
  bool circle_input_radius = false;
  bool numeric_initial_circle_input_radius = false;
  DrawNumericMode numeric_mode = DrawNumericMode::Automatic;
  bool numeric_applying = false;
  bool numeric_hold = false;
  std::string numeric_label;
  int numeric_initial_segments = 0;
  int circle_solution = 0;
  bool circle_tangent_valid = false;
  double3 circle_solution_seed = double3(0);
  Vector<Vector<const NurbsTopologyEdge *>> circle_chains;
  Vector<double3> circle_contacts;
  double3 circle_center = double3(0);
  Vector<Vector<float3>> circle_curve_overlays;
  float3 arc_reference_normal = float3(0, 0, 1);
  bool arc_perpendicular = false;
  int arc_vertical_axis = -1;
  bool arc_free = false;
  double arc_previous_angle = 0;
  double arc_accumulated = 0;
  double arc_sweep = 0;
  double arc_compass_rotation = 0;
  double arc_radius = 0;
  bool arc_valid = false;
  Vector<float3> arc_markers;
  Vector<std::string> arc_hud;
  NumInput num{};
  double4x4 inverse;
};

static bool is_spline(const wmOperator *op)
{
  return STREQ(op->type->idname, "CURVE_OT_draw_spline");
}

static bool is_arc(const wmOperator *op)
{
  return STREQ(op->type->idname, "CURVE_OT_draw_arc");
}

static bool is_circle(const wmOperator *op)
{
  return STREQ(op->type->idname, "CURVE_OT_draw_circle");
}

static int shape_type(const wmOperator *op)
{
  return STREQ(op->type->idname, "CURVE_OT_draw_ellipse")   ? 1 :
         STREQ(op->type->idname, "CURVE_OT_draw_polygon")   ? 2 :
         STREQ(op->type->idname, "CURVE_OT_draw_rectangle") ? 3 :
                                                              0;
}

static double arc_sweep_get(wmOperator *op)
{
  const double value = RNA_float_get(op->ptr, "sweep");
  std::istringstream input(RNA_string_get(op->ptr, "sweep_exact"));
  double exact;
  if ((input >> exact) && std::isfinite(exact) && float(exact) == float(value)) {
    return exact;
  }
  return std::abs(value) == float(2 * M_PI) ? std::copysign(2 * M_PI, value) : value;
}

/* Construct in world coordinates, then transform the rational coefficients to object space.
 * This preserves a circular world-space arc even under nonuniform object scale and shear. */
static OwnedCurve arc_support(const Span<double3> points,
                              const int mode,
                              const double3 &normal,
                              const double sweep,
                              const double4x4 &inverse,
                              std::string &error)
{
  if (points.size() != (mode == 0 ? 2 : 3)) {
    error = "Place the arc construction points";
    return nullptr;
  }
  OwnedCurve curve;
  if (mode == 0) {
    const double3 radial = points[1] - points[0];
    double3 n = math::normalize(normal);
    /* Viewport plane normals are floats. Remove only their storage roundoff along the radius. */
    if (math::length_squared(radial) > 0 && std::abs(math::dot(math::normalize(radial), n)) < 1e-6)
    {
      n = math::normalize(n - radial * (math::dot(radial, n) / math::length_squared(radial)));
    }
    curve = circular_arc(points[0], points[1], n, sweep, &error);
  }
  else {
    double3 through = points[2];
    if (mode == 1) {
      const double3 chord = points[1] - points[0];
      const double3 side = math::normalize(math::cross(normal, chord));
      const double3 midpoint = (points[0] + points[1]) / 2;
      through = midpoint + side * math::dot(through - midpoint, side);
    }
    curve = circular_arc_through_points(points[0], through, points[1], &error);
  }
  if (curve) {
    for (int i = 0; i < curve->points_num; i++) {
      const double3 local = math::transform_point(inverse, double3(curve->points[i].position));
      std::copy_n(&local.x, 3, curve->points[i].position);
    }
  }
  return curve;
}

/* RCAD circle construction inputs become an exact closed rational support. */
static OwnedCurve circle_support(const Span<double3> points,
                                 const int mode,
                                 const double3 &normal,
                                 const double4x4 &inverse,
                                 std::string &error)
{
  if (points.size() != (mode == 2 ? 3 : 2)) {
    error = "Place the circle construction points";
    return nullptr;
  }
  double3 center = points[0], start = points[1], n = normal;
  if (mode == 1) {
    center = (points[0] + points[1]) / 2;
    start = points[0];
  }
  else if (mode == 2) {
    const double3 a = points[1] - points[0], b = points[2] - points[0];
    const double3 cross = math::cross(a, b);
    const double area_squared = math::length_squared(cross);
    if (area_squared <= 1e-20 * math::length_squared(a) * math::length_squared(b)) {
      error = "Place three distinct, noncollinear circle points";
      return nullptr;
    }
    center += (math::length_squared(a) * math::cross(b, cross) +
               math::length_squared(b) * math::cross(cross, a)) /
              (2 * area_squared);
    start = points[0];
    n = math::normalize(cross);
  }
  const std::array<double3, 2> radius = {center, start};
  return arc_support(radius, 0, n, 2 * M_PI, inverse, error);
}

/* Convert a geometric angle to the parameter of our four rational quadratic
 * quarter spans. Uniform parameters are not uniformly spaced circle angles. */
static double circle_parameter_at_angle(const double angle)
{
  if (angle >= 2 * M_PI) {
    return 0;
  }
  const int quarter = std::clamp(int(angle / (M_PI / 2)), 0, 3);
  const double local_angle = angle - quarter * (M_PI / 2);
  const double sine = std::sin(local_angle / 2);
  const double t = sine / (sine + std::sin((M_PI / 2 - local_angle) / 2));
  return (quarter + t) / 4;
}

/* Preview and commit use exactly the same source parameter divisions. Optional
 * tangency contacts are replay data; the regular divisions follow Segments. */
static std::optional<Vector<double>> circle_divisions(wmOperator *op)
{
  Vector<double> parameters;
  const int count = RNA_int_get(op->ptr, "preview_points");
  for (int i = 0; i < count; i++) {
    parameters.append(circle_parameter_at_angle(2 * M_PI * i / count));
  }
  if (RNA_enum_get(op->ptr, "mode") >= 3 && RNA_boolean_get(op->ptr, "anchor_tangent_markers")) {
    std::istringstream input(RNA_string_get(op->ptr, "tangent_parameters"));
    double t;
    int contacts = 0;
    while (input >> t) {
      if (!std::isfinite(t) || t < 0 || t >= 1 || ++contacts > 3) {
        return std::nullopt;
      }
      parameters.append(t);
    }
    if (!input.eof()) {
      return std::nullopt;
    }
  }
  std::sort(parameters.begin(), parameters.end());
  Vector<double> unique;
  for (const double t : parameters) {
    if ((unique.is_empty() || t - unique.last() > 1e-10) && 1 - t > 1e-10) {
      unique.append(t);
    }
  }
  return unique;
}

/* Convert an absolute swept angle to the parameter of an exact rational arc.
 * Its constructor uses equal-angle quadratic spans, but their parameter speed
 * is not constant. */
static double arc_parameter_at_angle(const NurbsCurveGeometry &support, const double angle)
{
  const int spans = (support.points_num - 1) / 2;
  const double span_angle = 2 * std::acos(std::clamp(support.points[1].weight, 0.0, 1.0));
  if (angle >= span_angle * spans) {
    return 1;
  }
  if (span_angle < 1e-8) {
    return angle / (span_angle * spans);
  }
  const double span_position = angle / span_angle;
  const int span = std::clamp(int(span_position), 0, spans - 1);
  const double local_angle = angle - span * span_angle;
  const double a = std::sin(local_angle / 2);
  const double b = std::sin((span_angle - local_angle) / 2);
  return (span + a / (a + b)) / spans;
}

/* Center Arc keeps its initial count of segments distributed across the sweep
 * until count * 90 degrees. Once that window is complete, the NURBS quarter
 * points stay in place and later quarter points are added as the sweep grows. */
static Vector<double> arc_divisions(const NurbsCurveGeometry &support,
                                    const int count,
                                    const bool center_arc)
{
  const int spans = (support.points_num - 1) / 2;
  const double span_angle = 2 * std::acos(std::clamp(support.points[1].weight, 0.0, 1.0));
  const double total_angle = spans * span_angle;
  const int segments = std::max(0, count);
  Vector<double> parameters;
  parameters.append(0);

  const bool quarter_steps = segments == 0 ||
                             (center_arc && total_angle > segments * (M_PI / 2) + 1e-10);
  if (quarter_steps) {
    for (double angle = M_PI / 2; angle < total_angle - 1e-10; angle += M_PI / 2) {
      parameters.append(arc_parameter_at_angle(support, angle));
    }
  }
  else {
    for (int i = 1; i < segments; i++) {
      parameters.append(arc_parameter_at_angle(support, total_angle * i / segments));
    }
  }
  parameters.append(1);
  return parameters;
}

static wmOperatorStatus arc_exec(bContext *C, wmOperator *op);
static wmOperatorStatus shape_exec(bContext *C, wmOperator *op);
static void shape_preview(wmOperator *op);
static void arc_update(bContext *C, wmOperator *op, const wmEvent *event);
static void circle_update(bContext *C, wmOperator *op, const wmEvent *event);
static void update_arc_area_status_text(bContext *C, wmOperator *op);
static void draw_apply_segment_input(wmOperator *op);
static std::array<double3, 2> arc_plane_axes(const double3 &normal);

static OwnedCurve draw_support(const Span<double3> points,
                               const bool spline,
                               const bool cv,
                               const int degree,
                               std::string &error)
{
  if (points.size() < 2 || points.size() > 256) {
    error = "Place between 2 and 256 points";
    return nullptr;
  }
  const int p = spline ? std::min<int>(degree, points.size() - 1) : 1;
  if (spline && !cv) {
    Vector<CurveFitPoint> fit;
    double length = 0;
    fit.append({0, points[0]});
    for (int i = 1; i < points.size(); i++) {
      length += math::distance(points[i - 1], points[i]);
      fit.append({length, points[i]});
    }
    if (length <= 0) {
      error = "Points must be distinct";
      return nullptr;
    }
    for (auto &point : fit) {
      point.parameter /= length;
    }
    CurveFitOptions options;
    double scale = 1;
    for (const auto &point : points) {
      scale = std::max(scale, double(math::length(point)));
    }
    options.max_point_error = scale * 1e-10;
    auto result = interpolate_freeform_curve(fit, p, options, &error);
    return result ? std::move(result->curve) : nullptr;
  }
  OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
  curve->order = p + 1;
  curve->points_num = points.size();
  curve->knots_num = points.size() + p + 1;
  curve->points = MEM_new_array<NurbsControlPoint>(curve->points_num, __func__);
  curve->knots = MEM_new_array<double>(curve->knots_num, __func__);
  for (const int i : points.index_range()) {
    std::copy_n(&points[i].x, 3, curve->points[i].position);
  }
  for (int i = 0; i < curve->knots_num; i++) {
    curve->knots[i] = i <= p ? 0 : i >= points.size() ? 1 : double(i - p) / (points.size() - p);
  }
  return curve;
}

static Vector<double3> read_points(wmOperator *op)
{
  Vector<double3> points;
  std::istringstream input(RNA_string_get(op->ptr, "points"));
  double3 point;
  while (input >> point.x >> point.y >> point.z) {
    points.append(point);
  }
  return points;
}

static eSnapTargetOP snap_target_select_from_tool_settings(const ToolSettings &tool_settings)
{
  eSnapTargetOP target_select = SCE_SNAP_TARGET_ALL;
  if (tool_settings.snap_flag & SCE_SNAP_NOT_TO_ACTIVE) {
    target_select |= SCE_SNAP_TARGET_NOT_ACTIVE;
  }
  if (!(tool_settings.snap_flag & SCE_SNAP_TO_INCLUDE_EDITED)) {
    target_select |= SCE_SNAP_TARGET_NOT_EDITED;
  }
  if (!(tool_settings.snap_flag & SCE_SNAP_TO_INCLUDE_NONEDITED)) {
    target_select |= SCE_SNAP_TARGET_NOT_NONEDITED;
  }
  if (tool_settings.snap_flag & SCE_SNAP_TO_ONLY_SELECTABLE) {
    target_select |= SCE_SNAP_TARGET_ONLY_SELECTABLE;
  }
  return target_select;
}

static ed::transform::SnapObjectParams snap_params_from_tool_settings(
    const ToolSettings &tool_settings)
{
  ed::transform::SnapObjectParams params{};
  params.snap_target_select = snap_target_select_from_tool_settings(tool_settings);
  params.edit_mode_type = ed::transform::SNAP_GEOM_EDIT;
  params.occlusion_test = ed::transform::SNAP_OCCLUSION_AS_SEEM;
  params.face_nearest_steps = tool_settings.snap_face_nearest_steps;
  params.use_backface_culling = (tool_settings.snap_flag & SCE_SNAP_BACKFACE_CULLING) != 0;
  params.keep_on_same_target = (tool_settings.snap_flag & SCE_SNAP_KEEP_ON_SAME_OBJECT) != 0;
  return params;
}

static void ensure_native_centers(bContext *C, DrawData &data, const eSnapMode modes)
{
  const eSnapMode missing = eSnapMode(
      modes & (SCE_SNAP_TO_FACE_MIDPOINT | SCE_SNAP_TO_EDGE_MIDPOINT) & ~data.cached_center_modes);
  if (missing == SCE_SNAP_TO_NONE) {
    return;
  }
  data.cached_center_modes |= missing;
  ViewLayer *layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*CTX_data_main(C), CTX_data_scene(C), layer);
  for (Base &base : *BKE_view_layer_object_bases_get(layer)) {
    Object *object = base.object;
    if (!BASE_VISIBLE(CTX_wm_view3d(C), &base) || !ELEM(object->type, OB_CURVES_LEGACY, OB_SURF)) {
      continue;
    }
    const auto captured = curve_eval_source_capture(*id_cast<Curve *>(object->data));
    const NurbsGeometry *geometry = object == &data.source->object() ?
                                        &data.source->source().geometry() :
                                    captured && captured->snapshot ?
                                        &captured->snapshot->geometry() :
                                        nullptr;
    if (!geometry) {
      continue;
    }
    if (!data.center_objects.contains(object)) {
      data.center_objects.append(object);
    }
    const double4x4 transform(object->object_to_world());
    if (missing & SCE_SNAP_TO_EDGE_MIDPOINT) {
      for (const auto &edge : geometry->edges) {
        for (const auto &curve : geometry->curves) {
          if (curve.id != edge.curve_id) {
            continue;
          }
          const auto point = curve_length_midpoint(
              curve, double2(edge.parameter_range), transform);
          if (point) {
            data.centers.append({object, *point, float3(0), SCE_SNAP_TO_EDGE_MIDPOINT});
          }
          break;
        }
      }
    }
    if ((missing & SCE_SNAP_TO_FACE_MIDPOINT) && !geometry->faces.is_empty()) {
      const auto preview = tessellate_surfaces(
          *geometry, curve_preview_options(*id_cast<Curve *>(object->data)));
      if (!preview) {
        continue;
      }
      for (const auto &face : geometry->faces) {
        for (const auto &surface : geometry->surfaces) {
          if (surface.id != face.surface_id) {
            continue;
          }
          const auto center = surface_area_center(surface, face.id, *preview, transform);
          if (center) {
            const auto d = evaluate_surface_differential(surface, center->parameter);
            float3 normal(0);
            if (d) {
              normal = float3(math::normalize(
                  math::cross(math::transform_direction(transform, d->tangent_u),
                              math::transform_direction(transform, d->tangent_v))));
            }
            data.centers.append({object, center->position, normal, SCE_SNAP_TO_FACE_MIDPOINT});
          }
          break;
        }
      }
    }
  }
}

static bool center_target_allowed(const bContext *C,
                                  const Object &object,
                                  const Object &active,
                                  const eSnapTargetOP flags)
{
  if ((flags & SCE_SNAP_TARGET_NOT_ACTIVE) && &object == &active) {
    return false;
  }
  const bool edited = object.mode & OB_MODE_EDIT;
  if ((flags & SCE_SNAP_TARGET_NOT_EDITED) && edited && &object != &active) {
    return false;
  }
  if ((flags & SCE_SNAP_TARGET_NOT_NONEDITED) && !edited) {
    return false;
  }
  if (flags & SCE_SNAP_TARGET_ONLY_SELECTABLE) {
    const Base *base = BKE_view_layer_base_find(CTX_data_view_layer(C),
                                                const_cast<Object *>(&object));
    if (!base || !(base->flag & BASE_SELECTABLE)) {
      return false;
    }
  }
  return true;
}

static void snap_incremental(const Scene &scene,
                             const View3D &v3d,
                             const ARegion &region,
                             const ToolSettings &tool_settings,
                             const Span<double3> points,
                             float3 &position)
{
  const float grid_size = ED_view3d_grid_view_scale(&scene, &v3d, &region, nullptr);
  if (grid_size <= 0.0f) {
    return;
  }

  const bool absolute = (tool_settings.snap_flag & SCE_SNAP_ABS_GRID) || points.is_empty();
  const float3 origin = absolute ? float3(0.0f) : float3(points.last());
  for (int axis = 0; axis < 3; axis++) {
    position[axis] = origin[axis] +
                     std::round((position[axis] - origin[axis]) / grid_size) * grid_size;
  }
}

static std::optional<float3> draw_constraint_axis(const DrawData &data)
{
  if (data.arc_free) {
    return std::nullopt;
  }
  if (data.axis == 3) {
    if (math::length_squared(data.face_normal) > 1e-12f) {
      return math::normalize(data.face_normal);
    }
    return std::nullopt;
  }
  if (data.axis >= 0) {
    float3 axis(0.0f);
    axis[data.axis] = 1.0f;
    return axis;
  }
  if (data.shift_direction && math::length_squared(*data.shift_direction) > 1e-12f) {
    return math::normalize(*data.shift_direction);
  }
  return std::nullopt;
}

static double3 constrain_to_axis(const double3 &anchor,
                                 const double3 &position,
                                 const float3 &axis)
{
  const double3 direction(axis);
  return anchor + direction * math::dot(position - anchor, direction);
}

static bool position_on_draw_constraint(const DrawData &data,
                                        const double3 &position,
                                        const std::optional<float3> &axis_override = std::nullopt)
{
  if (data.points.is_empty() || (data.arc_mode >= 0 && data.points.size() == 2)) {
    return true;
  }
  const std::optional<float3> axis = axis_override ? axis_override : draw_constraint_axis(data);
  if (!axis) {
    return true;
  }
  const double3 anchor = data.points.last();
  const double3 projected = constrain_to_axis(anchor, position, *axis);
  const double tolerance = 1e-6 * std::max(1.0, math::distance(anchor, position));
  return math::distance(projected, position) <= tolerance;
}

static std::optional<float3> infer_axis_from_cursor(const ARegion &region,
                                                    const float2 &mouse,
                                                    const double3 &start)
{
  float2 start_screen;
  if (ED_view3d_project_float_global(&region, float3(start), start_screen, V3D_PROJ_TEST_NOP) !=
      V3D_PROJ_RET_OK)
  {
    return std::nullopt;
  }
  float2 screen_direction = mouse - start_screen;
  if (math::length_squared(screen_direction) < 1.0f) {
    return std::nullopt;
  }
  screen_direction = math::normalize(screen_direction);

  const float3 axes[3] = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
  float best_alignment = -1.0f;
  std::optional<float3> best_axis;
  for (const float3 &axis : axes) {
    float2 axis_screen;
    if (ED_view3d_project_float_global(
            &region, float3(start + double3(axis)), axis_screen, V3D_PROJ_TEST_NOP) !=
        V3D_PROJ_RET_OK)
    {
      continue;
    }
    float2 projected_axis = axis_screen - start_screen;
    if (math::length_squared(projected_axis) < 1e-8f) {
      continue;
    }
    projected_axis = math::normalize(projected_axis);
    const float alignment = std::abs(math::dot(screen_direction, projected_axis));
    if (alignment > best_alignment) {
      best_alignment = alignment;
      best_axis = axis;
    }
  }
  /* Match radCAD's default six-degree screen-space alignment tolerance. */
  if (best_alignment < std::cos(6.0f * float(M_PI) / 180.0f)) {
    return std::nullopt;
  }
  return best_axis;
}

static bool snap_to_volume(ed::transform::SnapObjectContext *snap_context,
                           Depsgraph *depsgraph,
                           const ARegion *region,
                           const View3D *v3d,
                           const ToolSettings &tool_settings,
                           const ed::transform::SnapObjectParams &params,
                           const float2 &mouse,
                           float3 &position)
{
  ListBaseT<ed::transform::SnapObjectHitDepth> hits = {nullptr};
  const bool has_hits = ed::transform::object_project_all_view3d_ex(
      snap_context, depsgraph, region, v3d, &params, mouse, -1.0f, false, &hits);

  ed::transform::SnapObjectHitDepth *near = nullptr;
  for (ed::transform::SnapObjectHitDepth &hit : hits) {
    if (near == nullptr || hit.depth < near->depth) {
      near = &hit;
    }
  }
  ed::transform::SnapObjectHitDepth *far = nullptr;
  if (near) {
    for (ed::transform::SnapObjectHitDepth &hit : hits) {
      if (&hit == near || hit.ob_uuid != near->ob_uuid) {
        continue;
      }
      if (far == nullptr ||
          ((tool_settings.snap_flag & SCE_SNAP_PEEL_OBJECT) ? hit.depth > far->depth :
                                                              hit.depth < far->depth))
      {
        far = &hit;
      }
    }
    if (far == nullptr) {
      far = near;
    }
    position = (float3(near->co) + float3(far->co)) * 0.5f;
  }
  const bool found = near != nullptr;
  for (ed::transform::SnapObjectHitDepth &hit : hits.items_mutable()) {
    MEM_delete(&hit);
  }
  return has_hits && found;
}

static wmOperatorStatus draw_exec(bContext *C, wmOperator *op)
{
  if (shape_type(op)) {
    return shape_exec(C, op);
  }
  if (is_arc(op) || is_circle(op)) {
    return arc_exec(C, op);
  }
  std::string error;
  auto source = ed::nurbs::TopologyPreview::begin(C, &error);
  Vector<double3> points = read_points(op);
  if (source && !is_spline(op) && points.size() >= 2) {
    const double3 delta = points.last() - points[points.size() - 2];
    const double length = math::length(
        math::transform_direction(double4x4(source->object().object_to_world()), delta));
    const float requested = RNA_float_get(op->ptr, "segment_length");
    if (length > 0 && requested > 0 && requested != float(length)) {
      points.last() = points[points.size() - 2] + delta * (double(requested) / length);
    }
  }
  auto support = draw_support(points,
                              is_spline(op),
                              RNA_enum_get(op->ptr, "method") == 1,
                              RNA_int_get(op->ptr, "degree"),
                              error);
  if (!source || !support) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  TopologyEditResult result;
  result.geometry.reset(geometry_copy(&source->source().geometry()));
  auto &geometry = *result.geometry;
  support->id = is_spline(op) ? geometry.next_id++ : 0;
  const int64_t curve_id = support->id;
  /* Every polyline corner is a source vertex. A spline has one authoritative support,
   * with source vertices at its ends; its coefficients remain curve data. */
  Vector<double> parameters;
  if (!is_spline(op)) {
    for (int i = 0; i < points.size(); i++) {
      parameters.append(double(i) / (points.size() - 1));
    }
  }
  else {
    const int segments = std::clamp(RNA_int_get(op->ptr, "preview_points"), 1, 256);
    for (int i = 0; i <= segments; i++) {
      parameters.append(double(i) / segments);
    }
  }
  Vector<int64_t> references;
  std::istringstream refs(RNA_string_get(op->ptr, "vertex_references"));
  int64_t ref;
  while (refs >> ref) {
    references.append(ref);
  }
  auto index = TopologyIndex::build(geometry);
  int64_t previous = 0;
  double previous_gap = 0;
  const bool closed_line = !is_spline(op) && points.size() > 3 && points.first() == points.last();
  for (const int i : parameters.index_range()) {
    const auto evaluated = evaluate_curve(*support, parameters[i], &error);
    if (!evaluated) {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
      return OPERATOR_CANCELLED;
    }
    int64_t vertex_id;
    double gap = 0;
    const int point_index = is_spline(op) ? (i == 0                     ? 0 :
                                             i == parameters.size() - 1 ? points.size() - 1 :
                                                                          -1) :
                                            i;
    const NurbsTopologyVertex *anchor = point_index >= 0 && references.size() == points.size() &&
                                                index ?
                                            index->vertex(references[point_index]) :
                                            nullptr;
    if (anchor && double3(anchor->position) == points[point_index]) {
      vertex_id = anchor->id;
      gap = math::distance(double3(anchor->position), evaluated->position);
      result.vertices.append(vertex_id);
    }
    else if (closed_line && i == parameters.size() - 1) {
      vertex_id = result.vertices.first();
    }
    else {
      auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
      vertex->id = geometry.next_id++;
      std::copy_n(&evaluated->position.x, 3, vertex->position);
      BLI_addtail(&geometry.vertices, vertex);
      result.vertices.append(vertex->id);
      vertex_id = vertex->id;
    }
    if (previous) {
      auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
      edge->id = geometry.next_id++;
      edge->vertex_ids[0] = previous;
      edge->vertex_ids[1] = vertex_id;
      edge->curve_id = curve_id;
      edge->tolerance = std::max(previous_gap, gap);
      edge->parameter_range[0] = parameters[i - 1];
      edge->parameter_range[1] = parameters[i];
      if (!is_spline(op)) {
        auto segment = draw_support(points.as_span().slice(i - 1, 2), false, false, 1, error);
        segment->id = geometry.next_id++;
        edge->curve_id = segment->id;
        edge->parameter_range[0] = 0;
        edge->parameter_range[1] = 1;
        BLI_addtail(&geometry.curves, segment.release());
      }
      BLI_addtail(&geometry.edges, edge);
      result.edges.append(edge->id);
    }
    previous = vertex_id;
    previous_gap = gap;
  }
  if (is_spline(op)) {
    BLI_addtail(&geometry.curves, support.release());
  }
  if (!source->commit(C, std::move(result), SelectionMode::Vertex, &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  if (bToolRef *tool = WM_toolsystem_ref_from_context(C)) {
    if (STREQ(tool->idname,
              is_spline(op) ? "builtin.nurbs_draw_spline" : "builtin.nurbs_draw_line"))
    {
      PointerRNA settings;
      WM_toolsystem_ref_properties_ensure_ex(tool, op->type->idname, op->type->srna, &settings);
      RNA_enum_set(&settings, "method", RNA_enum_get(op->ptr, "method"));
      RNA_int_set(&settings, "degree", RNA_int_get(op->ptr, "degree"));
      RNA_boolean_set(&settings, "use_axis_snap", RNA_boolean_get(op->ptr, "use_axis_snap"));
    }
  }
  return OPERATOR_FINISHED;
}

static wmOperatorStatus arc_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto source = ed::nurbs::TopologyPreview::begin(C, &error);
  if (!source) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  bool invertible;
  const double4x4 transform(source->object().object_to_world());
  const double4x4 inverse = math::invert(transform, invertible);
  if (!invertible) {
    BKE_report(op->reports, RPT_ERROR, "Drawing requires an invertible object transform");
    return OPERATOR_CANCELLED;
  }
  Vector<double3> points = read_points(op);
  for (double3 &point : points) {
    point = math::transform_point(transform, point);
  }
  float normal[3];
  RNA_float_get_array(op->ptr, "plane_normal", normal);
  auto support = is_circle(op) ?
                     circle_support(
                         points, RNA_enum_get(op->ptr, "mode"), double3(normal), inverse, error) :
                     arc_support(points,
                                 RNA_enum_get(op->ptr, "mode"),
                                 double3(normal),
                                 arc_sweep_get(op),
                                 inverse,
                                 error);
  if (!support) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  TopologyEditResult result;
  result.geometry.reset(geometry_copy(&source->source().geometry()));
  auto &geometry = *result.geometry;
  support->id = geometry.next_id++;
  const auto index = TopologyIndex::build(geometry);
  int64_t refs[2] = {0, 0};
  std::istringstream references(RNA_string_get(op->ptr, "vertex_references"));
  references >> refs[0] >> refs[1];
  /* Preview divisions become shared source endpoints on the exact support. */
  const bool circle = is_circle(op);
  Vector<double> parameters = arc_divisions(*support,
                                            RNA_int_get(op->ptr, "preview_points"),
                                            !circle && RNA_enum_get(op->ptr, "mode") == 0);
  if (circle) {
    const auto divisions = circle_divisions(op);
    if (!divisions) {
      BKE_report(op->reports, RPT_ERROR, "Invalid circle tangency divisions");
      return OPERATOR_CANCELLED;
    }
    parameters = *divisions;
  }
  const int vertex_count = parameters.size();
  Vector<int64_t> vertices(vertex_count, 0);
  Vector<double> tolerances(vertex_count, 0);
  for (int i = 0; i < vertex_count; i++) {
    const auto evaluated = evaluate_curve(*support, parameters[i]);
    if (!evaluated) {
      BKE_report(op->reports, RPT_ERROR, "Cannot evaluate a circular segment endpoint");
      return OPERATOR_CANCELLED;
    }
    const double3 point = evaluated->position;
    const bool end = !circle && i == vertex_count - 1;
    const auto *anchor = index && (i == 0 || end) ? index->vertex(refs[end ? 1 : 0]) : nullptr;
    const double gap = anchor ? math::distance(point, double3(anchor->position)) : 0;
    if (end && double3(support->points[0].position) ==
                   double3(support->points[support->points_num - 1].position))
    {
      vertices[i] = vertices[0];
      tolerances[i] = tolerances[0];
    }
    else if (anchor && gap <= 1e-10 * std::max(1.0, double(math::length(point)))) {
      vertices[i] = anchor->id;
      tolerances[i] = gap;
    }
    else {
      auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
      vertex->id = geometry.next_id++;
      std::copy_n(&point.x, 3, vertex->position);
      BLI_addtail(&geometry.vertices, vertex);
      vertices[i] = vertex->id;
    }
    result.vertices.append_non_duplicates(vertices[i]);
  }
  const int edge_count = circle ? vertex_count : vertex_count - 1;
  for (int i = 0; i < edge_count; i++) {
    const int next = circle ? (i + 1) % vertex_count : i + 1;
    auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
    edge->id = geometry.next_id++;
    edge->curve_id = support->id;
    edge->vertex_ids[0] = vertices[i];
    edge->vertex_ids[1] = vertices[next];
    edge->parameter_range[0] = parameters[i];
    edge->parameter_range[1] = i + 1 == vertex_count ? 1 : parameters[i + 1];
    edge->tolerance = std::max(tolerances[i], tolerances[next]);
    result.edges.append(edge->id);
    BLI_addtail(&geometry.edges, edge);
  }
  BLI_addtail(&geometry.curves, support.release());
  if (!source->commit(C, std::move(result), SelectionMode::Vertex, &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  if (bToolRef *tool = WM_toolsystem_ref_from_context(C)) {
    if (STREQ(tool->idname,
              is_circle(op) ? "builtin.nurbs_draw_circle" : "builtin.nurbs_draw_arc"))
    {
      PointerRNA settings;
      WM_toolsystem_ref_properties_ensure_ex(tool, op->type->idname, op->type->srna, &settings);
      RNA_enum_set(&settings, "mode", RNA_enum_get(op->ptr, "mode"));
      if (is_circle(op)) {
        for (const char *name : {"show_source_curves", "show_tangents", "anchor_tangent_markers"})
        {
          RNA_boolean_set(&settings, name, RNA_boolean_get(op->ptr, name));
        }
        float color[4];
        RNA_float_get_array(op->ptr, "tangent_color", color);
        RNA_float_set_array(&settings, "tangent_color", color);
        RNA_float_set(&settings, "source_width", RNA_float_get(op->ptr, "source_width"));
      }
      for (const char *name : {"show_compass",
                               "use_axis_colors",
                               "show_measurements",
                               "use_axis_snap",
                               "use_angle_snap"})
      {
        RNA_boolean_set(&settings, name, RNA_boolean_get(op->ptr, name));
      }
      for (const char *name : {"compass_size", "preview_points", "preview_point_size"}) {
        RNA_int_set(&settings, name, RNA_int_get(op->ptr, name));
      }
      for (const char *name : {"angle_increment", "axis_color_dim"}) {
        RNA_float_set(&settings, name, RNA_float_get(op->ptr, name));
      }
      for (const char *name : {"overlay_color", "start_color", "end_color"}) {
        float color[4];
        RNA_float_get_array(op->ptr, name, color);
        RNA_float_set_array(&settings, name, color);
      }
    }
  }
  return OPERATOR_FINISHED;
}

/* Ellipses retain an exact rational support. Polygon and rectangle corners are
 * shared topology vertices, with a degree-one support for each straight side. */
static OwnedCurve ellipse_support(const Span<double3> points, std::string &error)
{
  if (points.size() != 3) {
    error = "Place both ellipse axes";
    return nullptr;
  }
  const double3 x = points[1] - points[0], y = points[2] - points[0];
  if (math::length(x) < 1e-9 || math::length(y) < 1e-9) {
    error = "Ellipse axes must have nonzero length";
    return nullptr;
  }
  auto curve = circular_arc(double3(0), double3(1, 0, 0), double3(0, 0, 1), 2 * M_PI, &error);
  if (curve) {
    for (int i = 0; i < curve->points_num; i++) {
      const double3 p(curve->points[i].position);
      const double3 q = points[0] + x * p.x + y * p.y;
      std::copy_n(&q.x, 3, curve->points[i].position);
    }
  }
  return curve;
}

static wmOperatorStatus shape_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto source = ed::nurbs::TopologyPreview::begin(C, &error);
  const Vector<double3> points = read_points(op);
  const bool ellipse = shape_type(op) == 1;
  OwnedCurve support = ellipse ? ellipse_support(points, error) :
                                 draw_support(
                                     points.as_span().take_front(std::min<int>(2, points.size())),
                                     false,
                                     false,
                                     1,
                                     error);
  if (!source || !support) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  TopologyEditResult result;
  result.geometry.reset(geometry_copy(&source->source().geometry()));
  auto &geometry = *result.geometry;
  const int count = ellipse ? RNA_int_get(op->ptr, "preview_points") : points.size() - 1;
  if (ellipse) {
    support->id = geometry.next_id++;
  }
  Vector<int64_t> vertices;
  Vector<double> parameters;
  for (int i = 0; i < count; i++) {
    const double t = ellipse ? circle_parameter_at_angle(2 * M_PI * i / count) : double(i) / count;
    const auto point = evaluate_curve(*support, t, &error);
    if (!point) {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
      return OPERATOR_CANCELLED;
    }
    auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
    vertex->id = geometry.next_id++;
    const double3 position = ellipse ? point->position : points[i];
    std::copy_n(&position.x, 3, vertex->position);
    BLI_addtail(&geometry.vertices, vertex);
    vertices.append(vertex->id);
    result.vertices.append(vertex->id);
    parameters.append(t);
  }
  for (int i = 0; i < count; i++) {
    auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
    edge->id = geometry.next_id++;
    edge->vertex_ids[0] = vertices[i];
    edge->vertex_ids[1] = vertices[(i + 1) % count];
    edge->curve_id = support->id;
    edge->parameter_range[0] = parameters[i];
    edge->parameter_range[1] = i + 1 == count ? 1 : parameters[i + 1];
    if (!ellipse) {
      auto segment = draw_support(points.as_span().slice(i, 2), false, false, 1, error);
      segment->id = geometry.next_id++;
      edge->curve_id = segment->id;
      edge->parameter_range[0] = 0;
      edge->parameter_range[1] = 1;
      BLI_addtail(&geometry.curves, segment.release());
    }
    BLI_addtail(&geometry.edges, edge);
    result.edges.append(edge->id);
  }
  if (ellipse) {
    BLI_addtail(&geometry.curves, support.release());
  }
  if (ellipse && RNA_boolean_get(op->ptr, "keep_foci")) {
    std::istringstream foci(RNA_string_get(op->ptr, "foci"));
    double3 position;
    while (foci >> position.x >> position.y >> position.z) {
      auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
      vertex->id = geometry.next_id++;
      std::copy_n(&position.x, 3, vertex->position);
      BLI_addtail(&geometry.vertices, vertex);
      result.vertices.append(vertex->id);
    }
  }
  if (!source->commit(C, std::move(result), SelectionMode::Vertex, &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  if (bToolRef *tool = WM_toolsystem_ref_from_context(C)) {
    PointerRNA settings;
    WM_toolsystem_ref_properties_ensure_ex(tool, op->type->idname, op->type->srna, &settings);
    RNA_enum_set(&settings, "mode", RNA_enum_get(op->ptr, "mode"));
    RNA_int_set(&settings, "preview_points", RNA_int_get(op->ptr, "preview_points"));
    RNA_boolean_set(&settings, "keep_foci", RNA_boolean_get(op->ptr, "keep_foci"));
  }
  return OPERATOR_FINISHED;
}

static bool shape_three_points(const DrawData &data)
{
  return (data.shape == 1 && data.shape_mode != 3) || (data.shape == 3 && data.shape_mode == 2);
}

static void shape_preview(wmOperator *op)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  data.wire.clear();
  data.arc_markers.clear();
  data.shape_points.clear();
  data.arc_valid = false;
  data.arc_hud.clear();
  if (data.points.is_empty()) {
    return;
  }
  const int mode = data.shape_mode;
  const double3 anchor = data.points[0];
  const double3 normal = math::normalize(double3(data.plane_normal));
  auto [x, y] = arc_plane_axes(normal);
  double3 target = data.current - normal * math::dot(data.current - anchor, normal);
  data.current = target;
  const double3 second = data.points.size() > 1 ? data.points[1] : target;
  const double3 bridge = second - anchor;
  const bool oriented = (data.shape == 1 && mode != 3) || data.shape == 2 ||
                        (data.shape == 3 && mode == 2);
  if (oriented && math::length(bridge) > 1e-9) {
    x = math::normalize(bridge);
    y = math::normalize(math::cross(normal, x));
  }
  const bool initial = shape_three_points(data) && data.points.size() == 1;
  if (initial) {
    data.wire = {float3(anchor), float3(second)};
    data.arc_hud.append(data.shape == 1 ? "Place major axis / second focus" : "Place first edge");
    return;
  }
  double3 center = anchor;
  double rx = 0, ry = 0;
  if (data.shape == 1) {
    if (mode == 3) {
      const double dx = math::dot(target - anchor, x), dy = math::dot(target - anchor, y);
      center += x * (dx / 2) + y * (dy / 2);
      rx = std::abs(dx) / 2;
      ry = std::abs(dy) / 2;
    }
    else {
      center = mode == 0 ? anchor : (anchor + second) / 2;
      rx = math::length(bridge) * (mode == 0 ? 1 : 0.5);
      const double minor = math::dot(target - center, y);
      ry = std::abs(minor);
      if (mode != 1) {
        data.current = center + y * minor;
      }
      if (mode == 1) {
        rx = (math::distance(target, anchor) + math::distance(target, second)) / 2;
        ry = std::sqrt(std::max(0.0, rx * rx - math::length_squared(bridge) / 4));
      }
    }
    data.shape_points = {center, center + x * rx, center + y * ry};
    std::string error;
    auto support = ellipse_support(data.shape_points, error);
    if (support) {
      for (int i = 0; i <= 192; i++) {
        if (const auto p = evaluate_curve(*support, double(i) / 192)) {
          data.wire.append(float3(p->position));
        }
      }
      const int count = RNA_int_get(op->ptr, "preview_points");
      for (int i = 0; i < count; i++) {
        const double a = 2 * M_PI * i / count;
        data.arc_markers.append(float3(center + x * (rx * std::cos(a)) + y * (ry * std::sin(a))));
      }
      data.arc_valid = true;
    }
  }
  else if (data.shape == 2) {
    int count = RNA_int_get(op->ptr, "preview_points");
    if (mode == 3 && count % 2 == 0) {
      count = std::min(255, count + 1);
      RNA_int_set(op->ptr, "preview_points", count);
    }
    rx = math::length(bridge);
    double angle = 0;
    if (mode == 1) {
      rx /= std::cos(M_PI / count);
      angle = -M_PI / count;
    }
    else if (mode == 2) {
      rx /= 2 * std::sin(M_PI / count);
      center = (anchor + second) / 2 - y * (rx * std::cos(M_PI / count));
      x = math::normalize(anchor - center);
      y = math::normalize(math::cross(normal, x));
    }
    else if (mode == 3) {
      rx /= 1 + std::cos(M_PI / count);
      center = anchor + x * rx;
      x = -x;
      y = -y;
    }
    for (int i = 0; i < count; i++) {
      const double a = angle + 2 * M_PI * i / count;
      data.shape_points.append(center + rx * (x * std::cos(a) + y * std::sin(a)));
    }
  }
  else {
    double dx = mode == 2 ? math::length(bridge) : math::dot(target - anchor, x);
    double dy = math::dot(target - anchor, y);
    const double factor = mode == 0 ? 0.5 : 1;
    if (data.width) {
      dx = *data.width * factor;
    }
    if (data.height) {
      dy = *data.height * factor;
    }
    if (data.square && mode != 2) {
      const double side = data.width  ? std::abs(*data.width) * factor :
                          data.height ? std::abs(*data.height) * factor :
                                        std::max(std::abs(dx), std::abs(dy));
      dx = std::copysign(side, dx);
      dy = std::copysign(side, dy);
    }
    rx = std::abs(dx) / factor;
    ry = std::abs(dy) / factor;
    const double3 a = mode == 0 ? anchor - x * dx - y * dy : anchor;
    const double3 u = x * (dx / factor), v = y * (dy / factor);
    data.shape_points = {a, a + u, a + u + v, a + v};
    if (std::abs(dx * dy) < 1e-18) {
      data.shape_points.clear();
    }
  }
  if (data.shape != 1 && !data.shape_points.is_empty() && rx > 1e-9) {
    data.shape_points.append(data.shape_points[0]);
    for (int i = 0; i < data.shape_points.size(); i++) {
      data.wire.append(float3(data.shape_points[i]));
      if (i + 1 < data.shape_points.size()) {
        data.arc_markers.append(float3(data.shape_points[i]));
      }
    }
    data.arc_valid = true;
  }
  std::ostringstream status;
  status << std::fixed << std::setprecision(3);
  status << (data.shape == 1 ? "Ellipse | Rx: " :
             data.shape == 2 ? "Polygon | Radius: " :
                               "Rectangle | Width: ")
         << rx;
  if (data.shape != 2) {
    status << (data.shape == 1 ? " | Ry: " : " | Height: ") << ry;
  }
  if (data.shape != 3) {
    status << " | " << (data.shape == 2 ? "Sides: " : "Segments: ")
           << RNA_int_get(op->ptr, "preview_points");
  }
  if (data.square) {
    status << " | Square";
  }
  if (RNA_boolean_get(op->ptr, "show_measurements")) {
    data.arc_hud.append(status.str());
  }
  else {
    data.arc_hud.append(data.shape == 1 ? "Ellipse: place minor radius / confirm" :
                        data.shape == 2 ? "Polygon: place endpoint / confirm" :
                                          "Rectangle: place corner / confirm");
  }
}

static void rebuild_preview(wmOperator *op)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (data.shape) {
    shape_preview(op);
    return;
  }
  data.wire.clear();
  data.arc_markers.clear();
  data.arc_valid = false;
  Vector<double3> points = data.points;
  if (points.is_empty() || math::distance(points.last(), data.current) > 1e-9) {
    points.append(data.current);
  }
  if (points.size() < 2) {
    return;
  }
  std::string error;
  if (data.arc_mode < 0) {
    for (double3 &point : points) {
      point = math::transform_point(data.inverse, point);
    }
  }
  auto support = data.circle        ? circle_support(points,
                                              data.arc_mode,
                                              double3(data.plane_normal),
                                              double4x4::identity(),
                                              error) :
                 data.arc_mode >= 0 ? arc_support(data.arc_mode == 0 && points.size() > 2 ?
                                                      points.as_span().take_front(2) :
                                                      points.as_span(),
                                                  data.arc_mode,
                                                  double3(data.plane_normal),
                                                  data.arc_sweep,
                                                  double4x4::identity(),
                                                  error) :
                                      draw_support(points,
                                                   data.spline,
                                                   RNA_enum_get(op->ptr, "method") == 1,
                                                   RNA_int_get(op->ptr, "degree"),
                                                   error);
  if (!support) {
    if (data.arc_mode > 0 && data.points.size() == 2) {
      data.arc_hud.append("Move the third point off the chord");
    }
    if (data.arc_mode >= 0) {
      for (const double3 &point : points) {
        data.wire.append(float3(point));
      }
    }
    return;
  }
  if (data.arc_mode >= 0) {
    data.arc_valid = true;
    const int markers = RNA_int_get(op->ptr, "preview_points");
    for (const double parameter :
         arc_divisions(*support, markers, !data.circle && data.arc_mode == 0))
    {
      if (const auto point = evaluate_curve(*support, parameter)) {
        data.arc_markers.append(float3(point->position));
      }
    }
    if (!data.circle && data.arc_mode == 0) {
      data.arc_radius = math::distance(points[0], points[1]);
    }
    else {
      const auto a = evaluate_curve(*support, 0);
      const auto b = evaluate_curve(*support, data.circle ? 0.25 : 0.5);
      const auto c = evaluate_curve(*support, data.circle ? 0.5 : 1);
      if (a && b && c) {
        const double3 ab = b->position - a->position, ac = c->position - a->position;
        const double area = math::length(math::cross(ab, ac));
        data.arc_radius = area > 0 ? math::length(ab) * math::length(ac) *
                                         math::distance(b->position, c->position) / (2 * area) :
                                     0;
      }
    }
    if (data.circle) {
      const auto a = evaluate_curve(*support, 0);
      const auto b = evaluate_curve(*support, 0.25);
      const auto c = evaluate_curve(*support, 0.5);
      if (a && b && c) {
        const double3 center = (a->position + c->position) / 2;
        const double3 x = math::normalize(a->position - center);
        const double3 y = math::normalize(b->position - center);
        std::ostringstream contacts;
        contacts << std::setprecision(17);
        if (data.arc_mode >= 3 && RNA_boolean_get(op->ptr, "anchor_tangent_markers")) {
          for (const double3 &contact : data.circle_contacts) {
            const double3 radial = contact - center;
            const double angle = std::atan2(math::dot(radial, y), math::dot(radial, x));
            contacts << circle_parameter_at_angle(angle < 0 ? angle + 2 * M_PI : angle) << ' ';
          }
        }
        RNA_string_set(op->ptr, "tangent_parameters", contacts.str().c_str());
        data.arc_markers.clear();
        if (const auto divisions = circle_divisions(op)) {
          for (const double t : *divisions) {
            if (const auto point = evaluate_curve(*support, t)) {
              data.arc_markers.append(float3(point->position));
            }
          }
        }
        if (!data.arc_markers.is_empty()) {
          data.arc_markers.append(data.arc_markers[0]);
        }
      }
    }
    {
      for (int i = data.arc_hud.size() - 1; i >= 0; i--) {
        if (data.arc_hud[i].rfind("R: ", 0) == 0 || data.arc_hud[i].rfind("Segments: ", 0) == 0) {
          data.arc_hud.remove(i);
        }
      }
    }
    std::ostringstream radius;
    radius << "R: " << std::fixed << std::setprecision(3) << data.arc_radius;
    data.arc_hud.append(radius.str());
    if (!data.arc_markers.is_empty()) {
      data.arc_hud.append("Segments: " + std::to_string(data.arc_markers.size() - 1));
    }
  }
  else if (data.spline) {
    const int segments = std::clamp(RNA_int_get(op->ptr, "preview_points"), 1, 256);
    const double4x4 transform(data.source->object().object_to_world());
    for (int i = 1; i < segments; i++) {
      if (const auto point = evaluate_curve(*support, double(i) / segments)) {
        data.arc_markers.append(float3(math::transform_point(transform, point->position)));
      }
    }
  }
  const int samples = data.arc_mode >= 0 ? 128 : std::max<int>(2, (points.size() - 1) * 16);
  for (int i = 0; i <= samples; i++) {
    if (auto point = evaluate_curve(*support, double(i) / samples)) {
      data.wire.append(data.arc_mode >= 0 ? float3(point->position) :
                                            float3(math::transform_point(
                                                double4x4(data.source->object().object_to_world()),
                                                point->position)));
    }
  }
}

/* RCAD's compass has a fixed screen diameter, a ticked outer circle, two inner
 * circular segments closed by chords, and a center cross. Only its in-plane
 * orientation rotates: the drawing plane and its axis color remain authoritative. */
static void draw_arc_overlay(wmOperator *op, ARegion *region)
{
  const auto &data = *static_cast<const DrawData *>(op->customdata);
  if (data.points.is_empty()) {
    return;
  }
  const float4 black(0, 0, 0, 1);
  float4 fallback;
  RNA_float_get_array(op->ptr, "overlay_color", &fallback.x);
  const bool axis_colors = RNA_boolean_get(op->ptr, "use_axis_colors");
  const float dim = RNA_float_get(op->ptr, "axis_color_dim");
  auto aligned_color = [&](const double3 &direction, const float4 &other) {
    if (axis_colors && math::length_squared(direction) > 1e-12) {
      const double3 n = math::normalize(direction);
      if (std::abs(n.x) > 0.9999) {
        return float4(1.0f * dim, 0.1f * dim, 0.1f * dim, 1);
      }
      if (std::abs(n.y) > 0.9999) {
        return float4(0.1f * dim, 0.7f * dim, 0.1f * dim, 1);
      }
      if (std::abs(n.z) > 0.9999) {
        return float4(0.2f * dim, 0.5f * dim, 1.0f * dim, 1);
      }
    }
    return other;
  };
  const auto [x, y] = arc_plane_axes(double3(data.plane_normal));
  const double3 n = math::normalize(double3(data.plane_normal));
  const auto *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  const auto depth = GPU_depth_test_get();
  const auto blend = GPU_blend_get();
  GPU_blend(GPU_BLEND_ALPHA);
  const bool depth_write = GPU_depth_mask_get();
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);
  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR);
  float viewport[4];
  GPU_viewport_size_get_f(viewport);
  immUniform2fv("viewportSize", &viewport[2]);
  immUniform1f("lineWidth", std::max(1.0f, U.pixelsize));
  auto draw_lines = [&](const Span<float3> vertices, const float4 &color) {
    if (vertices.is_empty()) {
      return;
    }
    immUniformColor4fv(color);
    immBegin(GPU_PRIM_LINES, vertices.size());
    for (const float3 &point : vertices) {
      immVertex3fv(pos, point);
    }
    immEnd();
  };
  auto guide = [&](const double3 &a, const double3 &b, const float4 &color) {
    const std::array<float3, 2> vertices = {float3(a), float3(b)};
    draw_lines(vertices, color);
  };
  /* RCAD draws the compass only in draw_preview_1point. Endpoint arc modes
   * have chord/height/through-point guides, never a compass at either stage. */
  if (!data.shape && !data.circle && data.arc_mode == 0 &&
      RNA_boolean_get(op->ptr, "show_compass"))
  {
    const double3 center = data.points[0];
    const float3 position(center);
    const double radius = ED_view3d_pixel_size(rv3d, &position.x) *
                          RNA_int_get(op->ptr, "compass_size") * 0.5 * U.pixelsize;
    Vector<float3> compass;
    auto point = [&](const double angle, const double r) {
      const double a = angle + data.arc_compass_rotation;
      return float3(center + (x * std::cos(a) + y * std::sin(a)) * r);
    };
    auto segment = [&](const float3 &a, const float3 &b) {
      compass.append(a);
      compass.append(b);
    };
    for (int i = 0; i < 72; i++) {
      segment(point(2 * M_PI * i / 72, radius), point(2 * M_PI * (i + 1) / 72, radius));
    }
    const double increment = RNA_float_get(op->ptr, "angle_increment");
    const int ticks = std::max(1, int(std::round(2 * M_PI / increment)));
    for (int i = 0; i < ticks; i++) {
      const double angle = 2 * M_PI * i / ticks;
      segment(point(angle, radius), point(angle, radius * (110.0 / 120.0)));
    }
    for (const double start : {20.0, 200.0}) {
      for (int i = 0; i < 48; i++) {
        segment(point((start + 140.0 * i / 48) * M_PI / 180, radius * 2 / 3),
                point((start + 140.0 * (i + 1) / 48) * M_PI / 180, radius * 2 / 3));
      }
      segment(point(start * M_PI / 180, radius * 2 / 3),
              point((start + 140) * M_PI / 180, radius * 2 / 3));
    }
    segment(float3(center - x * radius / 12), float3(center + x * radius / 12));
    segment(float3(center - y * radius / 12), float3(center + y * radius / 12));
    draw_lines(compass, aligned_color(n, black));
  }
  const double3 first = data.points[0];
  if (data.circle && data.arc_mode >= 3) {
    float4 cyan;
    RNA_float_get_array(op->ptr, "tangent_color", &cyan.x);
    if (RNA_boolean_get(op->ptr, "show_source_curves")) {
      immUniform1f("lineWidth", RNA_float_get(op->ptr, "source_width") * U.pixelsize);
      for (const auto &curve : data.circle_curve_overlays) {
        for (int i = 1; i < curve.size(); i++) {
          guide(double3(curve[i - 1]), double3(curve[i]), cyan);
        }
      }
    }
    if (RNA_boolean_get(op->ptr, "show_tangents")) {
      for (const double3 &contact : data.circle_contacts) {
        guide(data.circle_center, contact, float4(1, 0.8f, 0, 1));
      }
    }
    immUniform1f("lineWidth", U.pixelsize);
  }
  else if (data.shape) {
    const double3 end = data.points.size() == 1 ? data.current : data.points[1];
    if (data.shape == 1 && data.shape_mode == 3 && data.shape_points.size() == 3) {
      const double3 center = data.shape_points[0];
      const double3 u = data.shape_points[1] - center, v = data.shape_points[2] - center;
      const std::array<double3, 4> corners = {
          center - u - v, center + u - v, center + u + v, center - u + v};
      for (int i = 0; i < 4; i++) {
        guide(corners[i], corners[(i + 1) % 4], float4(0, 1, 0, 1));
      }
    }
    else if (data.shape == 1 && data.shape_mode == 0) {
      guide(first * 2 - end, end, aligned_color(end - first, fallback));
      if (data.points.size() > 1) {
        guide(first, data.current, aligned_color(data.current - first, fallback));
      }
    }
    else if (data.shape == 1 && data.shape_mode == 1 && data.points.size() > 1) {
      guide(first, data.current, float4(0, 1, 0, 1));
      guide(end, data.current, float4(0, 1, 0, 1));
    }
    else if (data.shape == 1 && data.shape_mode == 2) {
      guide(first, end, aligned_color(end - first, fallback));
      if (data.points.size() > 1) {
        const double3 center = (first + end) / 2;
        guide(center, data.current, aligned_color(data.current - center, fallback));
      }
    }
    else if (data.shape != 3 || (data.shape_mode == 2 && data.points.size() == 1)) {
      guide(first, end, aligned_color(end - first, fallback));
    }
  }
  else if (data.circle) {
    const double3 end = data.points.size() == 1 ? data.current : data.points[1];
    const float4 neutral = data.arc_mode == 2 ? float4(0.5f, 0.5f, 0.5f, 0.5f) : fallback;
    guide(first, end, aligned_color(end - first, neutral));
    if (data.points.size() == 2) {
      guide(end, data.current, aligned_color(data.current - end, neutral));
    }
  }
  else if (data.arc_mode == 0) {
    float4 start_color, end_color;
    RNA_float_get_array(op->ptr, "start_color", &start_color.x);
    RNA_float_get_array(op->ptr, "end_color", &end_color.x);
    guide(first, data.points.size() == 1 ? data.current : data.points[1], start_color);
    if (data.points.size() == 2) {
      guide(first, data.current, end_color);
    }
  }
  else {
    const double3 end = data.points.size() == 1 ? data.current : data.points[1];
    guide(first, end, aligned_color(end - first, fallback));
    if (data.points.size() == 2) {
      const double3 anchor = data.arc_mode == 1 ? (first + end) / 2 : end;
      guide(anchor, data.current, aligned_color(data.current - anchor, fallback));
    }
  }
  if (data.arc_valid && data.wire.size() >= 2) {
    float4 curve_color = black;
    if (data.circle && data.arc_mode >= 3) {
      RNA_float_get_array(op->ptr, "tangent_color", &curve_color.x);
      immUniform1f("lineWidth", RNA_float_get(op->ptr, "source_width") * U.pixelsize);
    }
    immUniformColor4fv(curve_color);
    immBegin(GPU_PRIM_LINE_STRIP, data.wire.size());
    for (const float3 &point : data.wire) {
      immVertex3fv(pos, point);
    }
    immEnd();
  }
  if (data.circle && data.arc_mode >= 3 && data.arc_markers.size() > 1) {
    immUniform1f("lineWidth", U.pixelsize);
    for (int i = 1; i < data.arc_markers.size(); i++) {
      guide(double3(data.arc_markers[i - 1]), double3(data.arc_markers[i]), black);
    }
  }
  /* Match circle previews and native endpoint display: point size is measured
   * in screen pixels, independent of the plane orientation and view zoom. */
  Vector<float3> dots;
  auto marker = [&](const float3 &point) { dots.append(point); };
  /* Match the addon's stage-specific construction markers. Center arcs use
   * their compass cross and sampled arc markers; 3 Point has no moving dot
   * until the chord has been placed. */
  if (data.shape || (data.circle && data.arc_mode < 3) || (!data.circle && data.arc_mode > 0)) {
    for (const double3 &point : data.points) {
      marker(float3(point));
    }
    if (data.circle || (data.arc_mode == 1 && data.points.size() == 1)) {
      marker(float3(data.current));
    }
  }
  for (const float3 &point : data.arc_markers) {
    marker(point);
  }
  immUnbindProgram();
  if (!dots.is_empty()) {
    const auto blend = GPU_blend_get();
    GPU_blend(GPU_BLEND_ALPHA);
    const uint dot_pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA);
    immUniform1f("size", RNA_int_get(op->ptr, "preview_point_size") * U.pixelsize);
    immUniform4fv("color", black);
    immBegin(GPU_PRIM_POINTS, dots.size());
    for (const float3 &point : dots) {
      immVertex3fv(dot_pos, point);
    }
    immEnd();
    immUnbindProgram();
    GPU_blend(blend);
  }
  GPU_depth_mask(depth_write);
  GPU_depth_test(depth);
  GPU_blend(blend);
}

static void draw_arc_hud(const bContext *, ARegion *region, void *arg)
{
  const auto *op = static_cast<wmOperator *>(arg);
  const auto &data = *static_cast<const DrawData *>(op->customdata);
  if (!RNA_boolean_get(op->ptr, "show_measurements") || data.points.is_empty()) {
    return;
  }
  float2 position;
  if (ED_view3d_project_float_global(
          region, float3(data.current), position, V3D_PROJ_TEST_CLIP_DEFAULT) != V3D_PROJ_RET_OK)
  {
    return;
  }
  const auto blend = GPU_blend_get();
  GPU_blend(GPU_BLEND_ALPHA);
  position += float2(24, 20) * U.pixelsize;
  const float4 foreground(1, 1, 1, 1);
  const float4 background = hasNumInput(&data.num) ? float4(0.2f, 0.25f, 0.35f, 0.95f) :
                                                     float4(0.06f, 0.06f, 0.06f, 0.88f);
  for (const std::string &label : data.arc_hud) {
    ui::fontstyle_draw_simple_backdrop(
        &ui::style_get()->widget, position.x, position.y, label, foreground, background);
    position.y -= 23 * U.pixelsize;
  }
  GPU_blend(blend);
}

static void draw_preview(const bContext *, ARegion *region, void *arg)
{
  auto &data = *static_cast<DrawData *>(static_cast<wmOperator *>(arg)->customdata);
  const auto previous_blend = GPU_blend_get();
  GPU_blend(GPU_BLEND_ALPHA);
  if (data.arc_mode >= 0 || data.shape) {
    draw_arc_overlay(static_cast<wmOperator *>(arg), region);
  }
  else {
    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4f(1.0f, 0.55f, 0.15f, 1.0f);
    if (data.wire.size() >= 2) {
      immBegin(GPU_PRIM_LINE_STRIP, data.wire.size());
      for (const float3 &point : data.wire) {
        immVertex3fv(pos, point);
      }
      immEnd();
    }
    if (!data.points.is_empty()) {
      GPU_point_size(6.0f);
      immBegin(GPU_PRIM_POINTS, data.points.size());
      for (const double3 &point : data.points) {
        immVertex3fv(pos, float3(point));
      }
      immEnd();
      GPU_point_size(1.0f);
    }
    if (!data.arc_markers.is_empty()) {
      immUniformColor4f(0.05f, 0.05f, 0.05f, 1.0f);
      GPU_point_size(4.0f * U.pixelsize);
      immBegin(GPU_PRIM_POINTS, data.arc_markers.size());
      for (const float3 &point : data.arc_markers) {
        immVertex3fv(pos, point);
      }
      immEnd();
      GPU_point_size(1.0f);
    }
    immUnbindProgram();
  }
  GPU_blend(previous_blend);

  /* Use Blender's standard snap glyphs so the preview gives the same visual feedback as
   * viewport mesh snapping. The draw utility expects view matrices to already be active, as
   * they are for this REGION_DRAW_POST_VIEW callback. */
  if (data.snap_type != SCE_SNAP_TO_NONE) {
    const uchar snap_color[4] = {
        255, uchar(data.arc_mode >= 0 ? 204 : 255), uchar(data.arc_mode >= 0 ? 0 : 255), 255};
    ED_view3d_cursor_snap_draw_util(static_cast<RegionView3D *>(region->regiondata),
                                    nullptr,
                                    float3(data.current),
                                    SCE_SNAP_TO_NONE,
                                    data.snap_type,
                                    snap_color,
                                    snap_color);
    if (data.snap_type == SCE_SNAP_TO_GRID) {
      /* Blender's standard snap glyph intentionally omits grid targets. Draw a small cross in
       * the view plane so grid and increment snaps are still visible during drawing. */
      RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
      const float3 center(data.current);
      const float3 right(rv3d->viewinv[0]);
      const float3 up(rv3d->viewinv[1]);
      const float size = 4.0f * U.pixelsize * ED_view3d_pixel_size(rv3d, &center.x);
      GPU_blend(GPU_BLEND_ALPHA);
      GPU_line_smooth(true);
      GPU_line_width(1.5f);
      const uint pos = GPU_vertformat_attr_add(
          immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
      immUniformColor4ubv(snap_color);
      immBegin(GPU_PRIM_LINES, 4);
      immVertex3fv(pos, center - right * size);
      immVertex3fv(pos, center + right * size);
      immVertex3fv(pos, center - up * size);
      immVertex3fv(pos, center + up * size);
      immEnd();
      immUnbindProgram();
      GPU_line_smooth(false);
      GPU_blend(GPU_BLEND_NONE);
    }
    GPU_line_width(1.0f);
  }
}

static void draw_exit(bContext *C, wmOperator *op)
{
  auto *data = static_cast<DrawData *>(op->customdata);
  if (!data) {
    return;
  }
  ED_region_draw_cb_exit(data->region->runtime->type, data->draw_handle);
  if (data->draw_handle_pixel) {
    ED_region_draw_cb_exit(data->region->runtime->type, data->draw_handle_pixel);
  }
  if (data->navigation) {
    ED_view3d_navigation_free(C, data->navigation);
  }
  ed::transform::snap_object_context_destroy(data->snap);
  ED_region_tag_redraw(data->region);
  ED_workspace_status_text(C, nullptr);
  ED_area_status_text(CTX_wm_area(C), nullptr);
  MEM_delete(data);
  op->customdata = nullptr;
}

static void update_mouse(bContext *C,
                         wmOperator *op,
                         const wmEvent *event,
                         const bool tag_redraw = true)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (data.numeric_applying && data.numeric_mode == DrawNumericMode::Segments) {
    draw_apply_segment_input(op);
    rebuild_preview(op);
    update_arc_area_status_text(C, op);
    if (tag_redraw) {
      ED_region_tag_redraw(data.region);
    }
    return;
  }
  /* Typing edits the pending value only. Keep the construction at its last
   * accepted position until Enter applies the input. */
  const bool pending = data.numeric_mode != DrawNumericMode::Automatic || hasNumInput(&data.num);
  if ((pending && !data.numeric_applying) ||
      (data.numeric_hold && !pending && ELEM(event->type, LEFTMOUSE, EVT_RETKEY, EVT_PADENTER)))
  {
    update_arc_area_status_text(C, op);
    if (tag_redraw) {
      ED_region_tag_redraw(data.region);
    }
    return;
  }
  data.numeric_hold = false;
  data.arc_free = data.arc_mode >= 0 && (event->modifier & KM_ALT);
  auto *v3d = CTX_wm_view3d(C);
  const Scene *scene = CTX_data_scene(C);
  const ToolSettings *tool_settings = scene->toolsettings;
  draw_apply_segment_input(op);
  const eSnapMode snap_mode = eSnapMode(tool_settings->snap_mode);
  const bool use_snap = bool(tool_settings->snap_flag & SCE_SNAP) !=
                        bool(event->modifier & KM_CTRL);
  float3 origin, direction, target;
  ED_view3d_win_to_ray(data.region, float2(event->mval), origin, direction);
  const float denominator = math::dot(direction, data.plane_normal);
  /* A nominally edge-on orthographic view still has float rotation roundoff. Avoid
   * intersecting an arc's floor plane millions of units away in that case. */
  if (std::abs(denominator) > (data.arc_mode >= 0 ? 1e-5f : 1e-8f)) {
    target = origin +
             direction * (math::dot(data.plane_origin - origin, data.plane_normal) / denominator);
  }
  else {
    ED_view3d_win_to_3d(v3d, data.region, data.plane_origin, float2(event->mval), target);
  }
  if (!data.points.is_empty() && !(event->modifier & KM_SHIFT)) {
    data.shift_direction.reset();
  }
  const bool linear_constraints = (data.shape && data.shape != 3 &&
                                   !(data.shape == 1 && data.shape_mode == 3) &&
                                   data.points.size() == 1) ||
                                  (!data.shape &&
                                   (data.arc_mode < 0 ||
                                    (!(data.circle && data.arc_mode >= 3) &&
                                     data.points.size() == 1 && !(event->modifier & KM_ALT))));
  if (!data.points.is_empty() && linear_constraints) {
    const float3 anchor(data.points.last());
    if (!(event->modifier & KM_SHIFT)) {
      data.shift_direction.reset();
    }
    else if (!data.shift_direction) {
      /* Shift locks to the axis the cursor is already approaching when it is close to X/Y/Z,
       * matching radCAD's Shift behavior. Only fall back to the raw cursor direction when no
       * world axis is a clear screen-space match. */
      if (data.axis >= 0) {
        data.shift_direction = draw_constraint_axis(data);
      }
      else if (RNA_boolean_get(op->ptr, "use_axis_snap")) {
        data.shift_direction = infer_axis_from_cursor(
            *data.region, float2(event->mval), double3(anchor));
      }
      if (!data.shift_direction && math::length_squared(target - anchor) > 1e-12f) {
        data.shift_direction = math::normalize(target - anchor);
      }
    }
  }
  bool snapped = false;
  std::optional<float3> inferred_axis;
  data.snap_type = SCE_SNAP_TO_NONE;
  float3 snap_location(target);
  float snap_distance = 12.0f * U.pixelsize;
  eSnapMode snap_result = SCE_SNAP_TO_NONE;
  eSnapMode object_snap_modes = eSnapMode(
      snap_mode & (SCE_SNAP_TO_GEOM | SCE_SNAP_TO_GRID | SCE_SNAP_INDIVIDUAL_NEAREST));
  /* Projecting a placed point onto a face is the face snap mode for this tool. The transform
   * individual-project mode operates on a transformed source element, which a drawing point
   * does not have. */
  if (snap_mode & SCE_SNAP_INDIVIDUAL_PROJECT) {
    object_snap_modes |= SCE_SNAP_TO_FACE;
  }
  if (object_snap_modes & SCE_SNAP_TO_EDGE_MIDPOINT) {
    object_snap_modes &= ~SCE_SNAP_TO_EDGE;
  }
  if (use_snap) {
    ensure_native_centers(C, data, object_snap_modes);
  }
  std::optional<double3> native_center_position;
  ed::transform::SnapObjectParams snap_params = snap_params_from_tool_settings(*tool_settings);
  snap_params.grid_size = ED_view3d_grid_view_scale(scene, v3d, data.region, nullptr);
  float3 previous_point;
  const float *previous_point_ptr = nullptr;
  if (!data.points.is_empty()) {
    previous_point = float3(data.points.last());
    previous_point_ptr = previous_point;
  }
  const float3 initial_snap_point(target);
  const float *initial_snap_point_ptr = nullptr;
  if (object_snap_modes & SCE_SNAP_INDIVIDUAL_NEAREST) {
    initial_snap_point_ptr = &initial_snap_point.x;
  }
  /* Blender's snap API uses one geometry representation per query. Keep vertex/edge snapping on
   * the live edit cage, but query faces (and other surface-based targets) against the evaluated
   * surface. In Edit Mode, Blender intentionally skips face raycasts on NURBS surfaces unless
   * SNAP_GEOM_FINAL is requested. */
  eSnapMode evaluated_snap_modes = eSnapMode(
      object_snap_modes &
      (SCE_SNAP_TO_FACE | SCE_SNAP_TO_FACE_MIDPOINT | SCE_SNAP_INDIVIDUAL_NEAREST));
  if (evaluated_snap_modes & SCE_SNAP_TO_FACE_MIDPOINT) {
    /* Face Center is a precise target: don't let Face or Face Nearest turn it into an arbitrary
     * point on the surface when the cursor isn't close enough to a face center. */
    evaluated_snap_modes = SCE_SNAP_TO_FACE_MIDPOINT;
  }
  eSnapMode edit_snap_modes = eSnapMode(object_snap_modes &
                                        ~(SCE_SNAP_TO_FACE | SCE_SNAP_TO_FACE_MIDPOINT |
                                          SCE_SNAP_INDIVIDUAL_NEAREST | SCE_SNAP_TO_GRID));
  eSnapMode evaluated_snap_modes_with_grid = evaluated_snap_modes;
  if (evaluated_snap_modes == SCE_SNAP_TO_NONE) {
    edit_snap_modes |= eSnapMode(object_snap_modes & SCE_SNAP_TO_GRID);
  }
  else {
    evaluated_snap_modes_with_grid |= eSnapMode(object_snap_modes & SCE_SNAP_TO_GRID);
  }
  if (use_snap && object_snap_modes != SCE_SNAP_TO_NONE) {
    float3 normal(0);
    const Object *snap_object = nullptr;
    const float3 unsnapped_target(target);
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    if (edit_snap_modes != SCE_SNAP_TO_NONE) {
      snap_result = ed::transform::snap_object_project_view3d_ex(data.snap,
                                                                 depsgraph,
                                                                 data.region,
                                                                 v3d,
                                                                 edit_snap_modes,
                                                                 &snap_params,
                                                                 nullptr,
                                                                 float2(event->mval),
                                                                 previous_point_ptr,
                                                                 &snap_distance,
                                                                 target,
                                                                 normal,
                                                                 nullptr,
                                                                 &snap_object,
                                                                 nullptr,
                                                                 nullptr);
    }
    if (snap_result == SCE_SNAP_TO_NONE && evaluated_snap_modes_with_grid != SCE_SNAP_TO_NONE) {
      ed::transform::SnapObjectParams evaluated_params = snap_params;
      evaluated_params.edit_mode_type = ed::transform::SNAP_GEOM_FINAL;
      /* Drawing onto a surface should work from either side, regardless of the global snap
       * backface-culling preference. Keep this override local to the evaluated-surface query. */
      evaluated_params.use_backface_culling = false;
      snap_result = ed::transform::snap_object_project_view3d_ex(data.snap,
                                                                 depsgraph,
                                                                 data.region,
                                                                 v3d,
                                                                 evaluated_snap_modes_with_grid,
                                                                 &evaluated_params,
                                                                 initial_snap_point_ptr,
                                                                 float2(event->mval),
                                                                 previous_point_ptr,
                                                                 &snap_distance,
                                                                 target,
                                                                 normal,
                                                                 nullptr,
                                                                 &snap_object,
                                                                 nullptr,
                                                                 nullptr);
    }
    if (snap_object && (snap_result & (SCE_SNAP_TO_FACE_MIDPOINT | SCE_SNAP_TO_EDGE_MIDPOINT)) &&
        data.center_objects.contains(const_cast<Object *>(DEG_get_original(snap_object))))
    {
      snap_result = SCE_SNAP_TO_NONE;
      target = unsnapped_target;
      normal = float3(0);
    }
    /* Linear locks project the picked target, matching constrained mesh translation.
     * Other construction constraints still require a target on the construction. */
    if (!linear_constraints && snap_result != SCE_SNAP_TO_NONE &&
        !position_on_draw_constraint(data, double3(target)))
    {
      snap_result = SCE_SNAP_TO_NONE;
      target = unsnapped_target;
      normal = float3(0);
      snap_distance = 12.0f * U.pixelsize;
    }
    float center_distance = snap_result != SCE_SNAP_TO_NONE ? snap_distance : 12.0f * U.pixelsize;
    float center_depth = snap_result != SCE_SNAP_TO_NONE ? math::dot(target - origin, direction) :
                                                           FLT_MAX;
    for (const NativeCenter &center : data.centers) {
      if (!(snap_mode & center.type) ||
          !center_target_allowed(
              C, *center.object, data.source->object(), snap_params.snap_target_select) ||
          (!linear_constraints && !position_on_draw_constraint(data, center.position)))
      {
        continue;
      }
      float2 screen;
      if (ED_view3d_project_float_global(
              data.region, float3(center.position), screen, V3D_PROJ_TEST_CLIP_DEFAULT) !=
          V3D_PROJ_RET_OK)
      {
        continue;
      }
      const float distance = math::distance(screen, float2(event->mval));
      const float depth = math::dot(float3(center.position) - origin, direction);
      if (depth < 0 || distance > center_distance + 1e-4f ||
          (std::abs(distance - center_distance) <= 1e-4f && depth >= center_depth))
      {
        continue;
      }
      center_depth = depth;
      center_distance = snap_distance = distance;
      native_center_position = center.position;
      target = float3(center.position);
      normal = center.normal;
      snap_result = center.type;
    }
    snapped = snap_result != SCE_SNAP_TO_NONE;
    if (snapped) {
      data.snap_type = snap_result;
      snap_location = target;
    }
    if (snapped && math::length_squared(normal) > 0) {
      data.face_normal = math::normalize(normal);
    }
    if (snapped && (data.points.is_empty() || !data.locked) && math::length_squared(normal) > 0) {
      data.plane_normal = math::normalize(normal);
      data.plane_origin = target;
      if ((data.arc_mode >= 0 || data.shape) && data.points.size() < 2 && !data.arc_perpendicular)
      {
        data.arc_reference_normal = data.plane_normal;
      }
    }
  }
  if (use_snap && !snapped && (snap_mode & SCE_SNAP_TO_VOLUME)) {
    snapped = snap_to_volume(data.snap,
                             CTX_data_ensure_evaluated_depsgraph(C),
                             data.region,
                             v3d,
                             *tool_settings,
                             snap_params,
                             float2(event->mval),
                             target);
    if (snapped) {
      data.snap_type = SCE_SNAP_TO_VOLUME;
      snap_location = target;
      data.face_normal = float3(0.0f, 0.0f, 1.0f);
    }
  }
  if (!data.points.is_empty() && linear_constraints) {
    const float3 anchor(data.points.last());
    if (const std::optional<float3> axis = draw_constraint_axis(data)) {
      const float projection = math::dot(direction, *axis);
      const float denominator_axis = 1 - projection * projection;
      float distance = math::dot(target - anchor, *axis);
      if (!snapped && denominator_axis > 1e-8f) {
        distance = (math::dot(origin - anchor, *axis) -
                    projection * math::dot(origin - anchor, direction)) /
                   denominator_axis;
      }
      else if (!snapped && denominator_axis <= 1e-8f) {
        /* A view ray parallel to the constrained line cannot determine its depth. Preserve the
         * last preview's position along the line instead of letting the endpoint jump. */
        distance = math::dot(float3(data.current) - anchor, *axis);
      }
      target = anchor + *axis * distance;
    }
    else if (!snapped && RNA_boolean_get(op->ptr, "use_axis_snap")) {
      inferred_axis = infer_axis_from_cursor(*data.region, float2(event->mval), double3(anchor));
      if (inferred_axis) {
        const float projection = math::dot(direction, *inferred_axis);
        const float denominator_axis = 1.0f - projection * projection;
        if (denominator_axis > 1e-8f) {
          const float distance = (math::dot(origin - anchor, *inferred_axis) -
                                  projection * math::dot(origin - anchor, direction)) /
                                 denominator_axis;
          target = anchor + *inferred_axis * distance;
        }
      }
    }
  }
  data.current = native_center_position.value_or(double3(target));
  data.current_vertex = 0;
  std::optional<float3> active_axis = linear_constraints ? draw_constraint_axis(data) :
                                                           std::nullopt;
  if (!active_axis) {
    active_axis = inferred_axis;
  }
  if (!data.points.is_empty()) {
    if (active_axis) {
      data.current = constrain_to_axis(data.points.last(), data.current, *active_axis);
    }
  }
  if (use_snap && !snapped && (snap_mode & SCE_SNAP_TO_INCREMENT)) {
    float3 incremented(data.current);
    snap_incremental(*scene, *v3d, *data.region, *tool_settings, data.points, incremented);
    data.current = double3(incremented);
    if (math::distance_squared(float3(data.current), float3(target)) > 1e-12f) {
      data.snap_type = SCE_SNAP_TO_GRID;
      snap_location = incremented;
    }
  }
  /* Increment/grid snapping is evaluated after the pointer constraint, but the constraint must
   * remain authoritative. In particular, absolute grid snapping must not move the endpoint off
   * the locked X/Y/Z or Shift direction line. */
  if (!data.points.is_empty()) {
    if (active_axis) {
      data.current = constrain_to_axis(data.points.last(), data.current, *active_axis);
    }
  }
  // Prefer native endpoints over vertices of the display tessellation.
  const bool can_snap_active_object = !(snap_params.snap_target_select &
                                        SCE_SNAP_TARGET_NOT_ACTIVE);
  if (use_snap) {
    float best = 12.0f * U.pixelsize;
    if (snap_result != SCE_SNAP_TO_NONE) {
      best = snap_distance;
    }
    const double4x4 transform(data.source->object().object_to_world());
    if ((snap_mode & SCE_SNAP_TO_EDGE) && !(snap_mode & SCE_SNAP_TO_EDGE_MIDPOINT) &&
        can_snap_active_object)
    {
      const auto hit = ed::nurbs::topology_edge_under_cursor(
          C, *data.source, event->mval, nullptr);
      if (hit) {
        const auto index = TopologyIndex::build(data.source->source().geometry());
        const auto *edge = index ? index->edge(*hit) : nullptr;
        const NurbsCurveGeometry *curve = nullptr;
        if (edge) {
          for (const auto &support : data.source->source().geometry().curves) {
            if (support.id == edge->curve_id) {
              curve = &support;
              break;
            }
          }
        }
        if (curve) {
          auto distance_at = [&](const double t) {
            const auto point = evaluate_curve(*curve, t);
            float2 screen;
            if (!point || ED_view3d_project_float_global(
                              data.region,
                              float3(math::transform_point(transform, point->position)),
                              screen,
                              V3D_PROJ_TEST_NOP) != V3D_PROJ_RET_OK)
            {
              return FLT_MAX;
            }
            return math::distance(screen, float2(event->mval));
          };
          const double a = edge->parameter_range[0], b = edge->parameter_range[1];
          int nearest = 0;
          float distance = FLT_MAX;
          for (int i = 0; i <= 32; i++) {
            const float value = distance_at(a + (b - a) * i / 32.0);
            if (value < distance) {
              distance = value;
              nearest = i;
            }
          }
          double left = a + (b - a) * std::max(0, nearest - 1) / 32.0;
          double right = a + (b - a) * std::min(32, nearest + 1) / 32.0;
          for (int i = 0; i < 24; i++) {
            const double u = (2 * left + right) / 3, v = (left + 2 * right) / 3;
            if (distance_at(u) < distance_at(v)) {
              right = v;
            }
            else {
              left = u;
            }
          }
          const double t = (left + right) / 2;
          const float edge_distance = distance_at(t);
          if (const auto evaluation = evaluate_curve(*curve, t)) {
            const double3 edge_position = math::transform_point(transform, evaluation->position);
            if (edge_distance < best &&
                (linear_constraints || position_on_draw_constraint(data, edge_position, inferred_axis)))
            {
              best = edge_distance;
              data.current = edge_position;
              data.snap_type = SCE_SNAP_TO_EDGE;
              snap_location = float3(data.current);
            }
          }
        }
      }
    }
    if (snap_mode & SCE_SNAP_TO_VERTEX) {
      for (const double3 &point : data.points) {
        float2 screen;
        if (ED_view3d_project_float_global(
                data.region, float3(point), screen, V3D_PROJ_TEST_NOP) == V3D_PROJ_RET_OK)
        {
          const float distance = math::distance(screen, float2(event->mval));
          if (distance < best &&
              (linear_constraints || position_on_draw_constraint(data, point, inferred_axis))) {
            best = distance;
            data.current = point;
            data.snap_type = SCE_SNAP_TO_POINT;
            snap_location = float3(point);
          }
        }
      }
      if (can_snap_active_object) {
        for (const auto &vertex : data.source->source().geometry().vertices) {
          const double3 world = math::transform_point(transform, double3(vertex.position));
          float2 screen;
          if (ED_view3d_project_float_global(
                  data.region, float3(world), screen, V3D_PROJ_TEST_NOP) == V3D_PROJ_RET_OK)
          {
            const float distance = math::distance(screen, float2(event->mval));
            /* Keep the native vertex identity when Blender's generic snap found the same
             * point at an equal screen distance. */
            if (distance <= best + 1e-4f &&
                (linear_constraints || position_on_draw_constraint(data, world, inferred_axis)))
            {
              best = distance;
              data.current = world;
              data.current_vertex = position_on_draw_constraint(data, world, inferred_axis) ?
                                        vertex.id :
                                        0;
              data.snap_type = SCE_SNAP_TO_POINT;
              snap_location = float3(world);
            }
          }
        }
      }
    }
  }
  /* Native candidates are chosen after the generic picker. Apply the lock to those too,
   * and keep snap feedback at the constrained result. A projected target is not a weld. */
  if (!data.points.is_empty() && active_axis) {
    data.current = constrain_to_axis(data.points.last(), data.current, *active_axis);
    if (data.snap_type != SCE_SNAP_TO_NONE) {
      snap_location = float3(
          constrain_to_axis(data.points.last(), double3(snap_location), *active_axis));
    }
  }
  if (!data.shape && !data.points.is_empty() && hasNumInput(&data.num) &&
      data.numeric_mode != DrawNumericMode::Segments &&
      (data.arc_mode < 0 || (data.points.size() == 1 && !(data.circle && data.arc_mode >= 3))))
  {
    data.current_vertex = 0;
    double3 delta = data.current - data.points.last();
    if ((!data.circle && data.arc_mode == 0) || data.arc_mode == 2) {
      const double3 n = math::normalize(double3(data.plane_normal));
      delta -= n * math::dot(delta, n);
    }
    double length = math::length(delta);
    applyNumInput(&data.num, &length);
    if (data.circle && data.arc_mode == 1 && data.circle_input_radius) {
      length *= 2;
    }
    double3 direction = math::length_squared(delta) > 0 ? math::normalize(delta) :
                                                          double3(1, 0, 0);
    if (math::length_squared(delta) == 0 && data.axis >= 0) {
      direction = data.axis == 3 ? double3(data.face_normal) : double3(0);
      if (data.axis < 3) {
        direction[data.axis] = 1;
      }
    }
    data.current = data.points.last() + direction * length;
  }
  if (data.arc_mode >= 0) {
    if (data.circle) {
      circle_update(C, op, event);
    }
    else {
      arc_update(C, op, event);
      rebuild_preview(op);
      update_arc_area_status_text(C, op);
    }
  }
  if (data.shape) {
    shape_preview(op);
  }
  if (data.snap_type != SCE_SNAP_TO_NONE) {
    const double snap_tolerance = 1e-6 * std::max(1.0, double(math::length(snap_location)));
    if (math::distance(data.current, double3(snap_location)) > snap_tolerance) {
      /* Axis constraints and numeric input can move the preview off the snap target. Don't show
       * a snap marker in that case. */
      data.snap_type = SCE_SNAP_TO_NONE;
    }
  }
  rebuild_preview(op);
  update_arc_area_status_text(C, op);
  if (tag_redraw) {
    ED_region_tag_redraw(data.region);
  }
}

struct DrawSnapGizmo {
  wmGizmo gizmo;
  ed::transform::SnapObjectContext *snap;
};

static void draw_snap_gizmo_setup(wmGizmo *gizmo)
{
  reinterpret_cast<DrawSnapGizmo *>(gizmo)->snap = ed::transform::snap_object_context_create();
  gizmo->flag |= WM_GIZMO_HIDDEN_KEYMAP;
}

static void draw_snap_gizmo_free(wmGizmo *gizmo)
{
  ed::transform::snap_object_context_destroy(reinterpret_cast<DrawSnapGizmo *>(gizmo)->snap);
}

/* Tool-owned hover gizmo. Query the same unanchored point used by draw_invoke,
 * without starting a modal operation or creating source geometry. */
static void draw_snap_hover(const bContext *context, wmGizmo *gizmo)
{
  bContext *C = const_cast<bContext *>(context);
  bToolRef *tool = WM_toolsystem_ref_from_context(C);
  ARegion *region = CTX_wm_region(C);
  const auto *rv3d = CTX_wm_region_view3d(C);
  const wmEvent *event_state = CTX_wm_window(C)->runtime->eventstate;
  if (!tool || !rv3d || !event_state || (rv3d->rflag & RV3D_NAVIGATING) ||
      !ed::nurbs::topology_edit_poll(C))
  {
    return;
  }
  const char *operator_id =
      STREQ(tool->idname, "builtin.nurbs_draw_line")      ? "CURVE_OT_draw_line" :
      STREQ(tool->idname, "builtin.nurbs_draw_spline")    ? "CURVE_OT_draw_spline" :
      STREQ(tool->idname, "builtin.nurbs_draw_arc")       ? "CURVE_OT_draw_arc" :
      STREQ(tool->idname, "builtin.nurbs_draw_ellipse")   ? "CURVE_OT_draw_ellipse" :
      STREQ(tool->idname, "builtin.nurbs_draw_polygon")   ? "CURVE_OT_draw_polygon" :
      STREQ(tool->idname, "builtin.nurbs_draw_rectangle") ? "CURVE_OT_draw_rectangle" :
      STREQ(tool->idname, "builtin.nurbs_draw_circle")    ? "CURVE_OT_draw_circle" :
                                                            nullptr;
  if (!operator_id) {
    return;
  }
  for (const char *id : {"CURVE_OT_draw_line",
                         "CURVE_OT_draw_spline",
                         "CURVE_OT_draw_arc",
                         "CURVE_OT_draw_circle",
                         "CURVE_OT_draw_ellipse",
                         "CURVE_OT_draw_polygon",
                         "CURVE_OT_draw_rectangle"})
  {
    if (WM_operator_find_modal_by_type(CTX_wm_window(C), WM_operatortype_find(id, false))) {
      return;
    }
  }
  wmEvent event = *event_state;
  event.mval[0] = event.xy[0] - region->winrct.xmin;
  event.mval[1] = event.xy[1] - region->winrct.ymin;
  if (event.mval[0] < 0 || event.mval[1] < 0 || event.mval[0] >= region->winx ||
      event.mval[1] >= region->winy)
  {
    return;
  }
  const auto *settings = CTX_data_scene(C)->toolsettings;
  if (bool(settings->snap_flag & SCE_SNAP) == bool(event.modifier & KM_CTRL)) {
    return;
  }
  DrawData data;
  data.source = ed::nurbs::TopologyPreview::begin(C, nullptr);
  if (!data.source) {
    return;
  }
  bool invertible;
  data.inverse = math::invert(double4x4(data.source->object().object_to_world()), invertible);
  if (!invertible) {
    return;
  }
  PointerRNA properties;
  wmOperator op{};
  op.type = WM_operatortype_find(operator_id, true);
  WM_toolsystem_ref_properties_ensure_ex(tool, op.type->idname, op.type->srna, &properties);
  op.ptr = &properties;
  op.customdata = &data;
  data.spline = is_spline(&op);
  data.circle = is_circle(&op);
  data.shape = shape_type(&op);
  data.shape_mode = data.shape ? RNA_enum_get(op.ptr, "mode") : 0;
  data.arc_mode = is_arc(&op) || data.circle ? RNA_enum_get(op.ptr, "mode") : -1;
  data.region = region;
  data.plane_origin = CTX_data_scene(C)->cursor.location;
  data.plane_normal = (data.arc_mode >= 0 || data.shape) ?
                          float3(0, 0, 1) :
                          math::normalize(float3(rv3d->viewinv[2]));
  data.snap = reinterpret_cast<DrawSnapGizmo *>(gizmo)->snap;
  update_mouse(C, &op, &event, false);
  draw_preview(C, region, &op);
}

static int draw_snap_hover_test_select(bContext *C, wmGizmo *, const int[2])
{
  ED_region_tag_redraw_editor_overlays(CTX_wm_region(C));
  return -1; /* Feedback never captures a drawing click. */
}

static void draw_snap_hover_setup(const bContext *, wmGizmoGroup *group)
{
  WM_gizmo_new("VIEW3D_GT_nurbs_draw_snap", group, nullptr);
}

static void arc_num_reset(bContext *C, DrawData &data)
{
  initNumInput(&data.num);
  data.num.idx_max = 0;
  data.num.unit_sys = CTX_data_scene(C)->unit.system;
  data.num.unit_type[0] = data.arc_mode == 0 && data.points.size() == 2 ? B_UNIT_ROTATION :
                                                                          B_UNIT_LENGTH;
  data.num.flag |= NUM_USE_DOUBLE;
  data.numeric_mode = DrawNumericMode::Automatic;
  data.numeric_label.clear();
}

static void draw_num_input_begin(bContext *C,
                                 DrawData &data,
                                 const DrawNumericMode mode,
                                 const char *label)
{
  initNumInput(&data.num);
  data.num.idx_max = 0;
  data.num.unit_sys = CTX_data_scene(C)->unit.system;
  data.num.unit_type[0] = mode == DrawNumericMode::Angle    ? B_UNIT_ROTATION :
                          mode == DrawNumericMode::Segments ? B_UNIT_NONE :
                                                              B_UNIT_LENGTH;
  if (mode == DrawNumericMode::Segments) {
    data.num.val_flag[0] |= NUM_NO_FRACTION;
  }
  data.num.flag |= NUM_USE_DOUBLE;
  data.numeric_mode = mode;
  data.numeric_label = label;
}

static void draw_apply_segment_input(wmOperator *op)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (data.numeric_mode != DrawNumericMode::Segments || !hasNumInput(&data.num)) {
    return;
  }
  double count = RNA_int_get(op->ptr, "preview_points");
  if (!applyNumInput(&data.num, &count)) {
    return;
  }
  const int minimum = data.circle ? 4 : data.arc_mode >= 0 ? 0 : 1;
  RNA_int_set(op->ptr, "preview_points", std::clamp(int(std::lround(count)), minimum, 256));
}

static double3 arc_project(const DrawData &data, const double3 &point)
{
  const double3 normal = math::normalize(double3(data.plane_normal));
  return point - normal * math::dot(point - data.points[0], normal);
}

static std::array<double3, 2> arc_plane_axes(const double3 &normal)
{
  const double3 n = math::normalize(normal);
  const double3 reference = std::abs(n.x) < 0.99 ? double3(1, 0, 0) : double3(0, 1, 0);
  const double3 y = math::normalize(math::cross(n, reference));
  return {math::normalize(math::cross(y, n)), y};
}

static void arc_set_chord_plane(DrawData &data)
{
  const double3 chord = math::normalize(data.points[1] - data.points[0]);
  double3 normal(data.arc_reference_normal);
  if (data.arc_perpendicular) {
    normal = math::cross(chord, normal);
  }
  normal -= chord * math::dot(chord, normal);
  if (math::length_squared(normal) < 1e-8) {
    /* Vertical chord: choose the reference-plane axis facing the view most directly. */
    const auto [x, y] = arc_plane_axes(double3(data.arc_reference_normal));
    const auto *rv3d = static_cast<RegionView3D *>(data.region->regiondata);
    const double3 view(rv3d->viewinv[2]);
    normal = data.arc_vertical_axis == 0                                 ? x :
             data.arc_vertical_axis == 1                                 ? y :
             std::abs(math::dot(x, view)) > std::abs(math::dot(y, view)) ? x :
                                                                           y;
    normal -= chord * math::dot(chord, normal);
  }
  data.plane_normal = float3(math::normalize(normal));
  data.plane_origin = float3(data.points[0]);
}

static void arc_update(bContext * /*C*/, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (data.points.is_empty()) {
    return;
  }
  const double3 original = data.current;
  /* Endpoint chords may leave the initial plane through snapping or axis constraints.
   * Once the chord is fixed, use a plane containing it. */
  if (data.arc_mode == 0 || data.points.size() == 2) {
    data.current = arc_project(data, data.current);
  }
  const bool free = event->modifier & KM_ALT;
  const bool geometry_snap = data.snap_type != SCE_SNAP_TO_NONE &&
                             data.snap_type != SCE_SNAP_TO_GRID;
  const double increment = RNA_float_get(op->ptr, "angle_increment");
  auto snap_angle = [&](const double angle) {
    if (free || geometry_snap || !RNA_boolean_get(op->ptr, "use_angle_snap")) {
      return angle;
    }
    const double nearest = std::round(angle / increment) * increment;
    return std::abs(nearest - angle) < 6 * M_PI / 180 ? nearest : angle;
  };
  if (data.points.size() == 1 && data.arc_mode == 0 && data.axis < 0 && !data.shift_direction &&
      (!hasNumInput(&data.num) || data.numeric_mode == DrawNumericMode::Segments))
  {
    const auto [x, y] = arc_plane_axes(double3(data.plane_normal));
    const double3 radial = data.current - data.points[0];
    const double angle = snap_angle(std::atan2(math::dot(radial, y), math::dot(radial, x)));
    data.current = data.points[0] +
                   math::length(radial) * (x * std::cos(angle) + y * std::sin(angle));
  }
  const auto [compass_x, compass_y] = arc_plane_axes(double3(data.plane_normal));
  const double3 compass_direction = (data.points.size() == 2 ? data.points[1] : data.current) -
                                    data.points[0];
  if (math::length_squared(compass_direction) > 1e-20) {
    data.arc_compass_rotation = std::atan2(math::dot(compass_direction, compass_y),
                                           math::dot(compass_direction, compass_x));
  }
  if (data.points.size() == 2) {
    if (data.arc_mode == 0) {
      if (data.numeric_mode == DrawNumericMode::Radius && hasNumInput(&data.num)) {
        const double3 x = math::normalize(data.points[1] - data.points[0]);
        const double3 y = math::normalize(math::cross(double3(data.plane_normal), x));
        double radius = math::distance(data.points[0], data.points[1]);
        applyNumInput(&data.num, &radius);
        radius = std::abs(radius);
        data.points[1] = data.points[0] + x * radius;
        if (data.point_vertices.size() > 1) {
          data.point_vertices[1] = 0;
        }
        data.current_vertex = 0;
        data.arc_radius = radius;
        data.current = data.points[0] +
                       radius * (x * std::cos(data.arc_sweep) + y * std::sin(data.arc_sweep));
      }
      else {
        const double3 x = math::normalize(data.points[1] - data.points[0]);
        const double3 y = math::normalize(math::cross(double3(data.plane_normal), x));
        const double3 radial = data.current - data.points[0];
        const double raw = std::atan2(math::dot(radial, y), math::dot(radial, x));
        if (math::length_squared(radial) > 1e-20) {
          data.arc_accumulated = std::clamp(
              data.arc_accumulated + std::remainder(raw - data.arc_previous_angle, 2 * M_PI),
              -2 * M_PI,
              2 * M_PI);
          data.arc_previous_angle = raw;
        }
        /* RCAD snaps angles in the drawing-plane basis, including when the start was
         * geometry-snapped between angular increments. Keep that offset in the sweep. */
        const auto [plane_x, plane_y] = arc_plane_axes(double3(data.plane_normal));
        const double start_angle = std::atan2(math::dot(x, plane_y), math::dot(x, plane_x));
        data.arc_sweep = snap_angle(data.arc_accumulated + start_angle) - start_angle;
        if (hasNumInput(&data.num) && data.numeric_mode != DrawNumericMode::Segments) {
          applyNumInput(&data.num, &data.arc_sweep);
        }
        data.arc_sweep = std::clamp(data.arc_sweep, -2 * M_PI, 2 * M_PI);
        data.current = data.points[0] +
                       math::distance(data.points[0], data.points[1]) *
                           (x * std::cos(data.arc_sweep) + y * std::sin(data.arc_sweep));
      }
    }
    else if (data.arc_mode == 1) {
      const double3 chord = data.points[1] - data.points[0];
      const double3 midpoint = (data.points[0] + data.points[1]) / 2;
      const double3 side = math::normalize(math::cross(double3(data.plane_normal), chord));
      double height = math::dot(data.current - midpoint, side);
      const double current_side = height < 0 ? -1.0 : 1.0;
      const double half_chord = math::length(chord) / 2;
      if (!free && !geometry_snap && RNA_boolean_get(op->ptr, "use_angle_snap") &&
          std::abs(std::abs(height) - half_chord) < math::length(chord) * 0.06)
      {
        height = std::copysign(half_chord, height);
      }
      if (hasNumInput(&data.num) && data.numeric_mode != DrawNumericMode::Segments) {
        applyNumInput(&data.num, &height);
        if (ELEM(data.numeric_mode, DrawNumericMode::Radius, DrawNumericMode::Sagitta)) {
          height = std::copysign(std::abs(height), current_side);
        }
      }
      data.current = midpoint + side * height;
    }
    else if (hasNumInput(&data.num) && data.numeric_mode != DrawNumericMode::Segments) {
      const double3 delta = data.current - data.points[0];
      double length = math::length(delta);
      applyNumInput(&data.num, &length);
      if (math::length_squared(delta) > 0) {
        data.current = data.points[0] + math::normalize(delta) * length;
      }
    }
  }
  if (math::distance(original, data.current) > 1e-8) {
    data.current_vertex = 0;
  }
  std::ostringstream status;
  status << std::fixed << std::setprecision(3);
  if (data.points.size() == 1) {
    status << (data.arc_mode == 0 ? "Radius: " : "Chord: ")
           << math::distance(data.points[0], data.current);
  }
  else if (data.arc_mode == 0) {
    status << "Sweep: " << data.arc_sweep * 180 / M_PI << " degrees";
  }
  else if (data.arc_mode == 1) {
    const double3 midpoint = (data.points[0] + data.points[1]) / 2;
    const double3 side = math::normalize(
        math::cross(double3(data.plane_normal), data.points[1] - data.points[0]));
    status << "S: " << math::dot(data.current - midpoint, side);
  }
  else {
    status << "Chord: " << math::distance(data.points[0], data.points[1]);
  }
  data.arc_hud.clear();
  data.arc_hud.append(data.arc_mode == 0 ? "Center Arc" :
                      data.arc_mode == 1 ? "2 Point Arc" :
                                           "3 Point Arc");
  data.arc_hud.append(data.points.size() == 1 ?
                          (data.arc_mode == 0 ? "Place start / radius (2/3)" : "Place end (2/3)") :
                          (data.arc_mode == 0 ? "Place sweep (3/3)" :
                           data.arc_mode == 1 ? "Place bulge (3/3)" :
                                                "Place point on arc (3/3)"));
  data.arc_hud.append(status.str());
}

/* Keep Arc construction information in the tool header instead of over the
 * compass and curve preview, where the cursor HUD can obscure the geometry. */
static void update_arc_area_status_text(bContext *C, wmOperator *op)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  std::ostringstream status;
  bool first = true;
  const auto append = [&](const std::string &text) {
    if (!first) {
      status << "  |  ";
    }
    status << text;
    first = false;
  };

  const bool has_input = hasNumInput(&data.num);
  if (data.shape) {
    for (const std::string &line : data.arc_hud) {
      append(line);
    }
  }
  else if (data.arc_mode < 0) {
    if (!data.spline && data.numeric_mode == DrawNumericMode::Automatic && !has_input) {
      ED_area_status_text(CTX_wm_area(C), nullptr);
      return;
    }
    if (data.spline) {
      append("Segments: " + std::to_string(RNA_int_get(op->ptr, "preview_points")));
    }
  }
  else if (data.circle) {
    if (data.numeric_mode == DrawNumericMode::Automatic && !has_input) {
      return;
    }
    if (data.arc_hud.size() > 1) {
      append(data.arc_hud[1]);
    }
  }
  else {
    for (int i = 0; i < std::min<int>(2, data.arc_hud.size()); i++) {
      append(data.arc_hud[i]);
    }
    const bool show_measurements = RNA_boolean_get(op->ptr, "show_measurements");
    for (int i = 2; i < data.arc_hud.size(); i++) {
      const std::string &line = data.arc_hud[i];
      if (line.rfind("R: ", 0) == 0 || line.rfind("Segments: ", 0) == 0) {
        continue;
      }
      if (i == 2 && !show_measurements) {
        continue;
      }
      append(line);
    }
    if (show_measurements && data.arc_valid) {
      if (data.points.size() > 1) {
        std::ostringstream radius;
        radius << "R: " << std::fixed << std::setprecision(3) << data.arc_radius;
        append(radius.str());
      }
      append("Segments: " + std::to_string(std::max<int>(0, data.arc_markers.size() - 1)));
    }
  }

  if (data.numeric_mode != DrawNumericMode::Automatic || has_input) {
    const char *label = data.numeric_label.c_str();
    if (data.numeric_mode == DrawNumericMode::Automatic) {
      if (data.arc_mode < 0) {
        label = "Length";
      }
      else if (data.circle) {
        label = data.arc_mode == 0 ? "Radius" :
                data.arc_mode == 1 ? (data.circle_input_radius ? "Radius" : "Diameter") :
                                     "Distance";
      }
      else if (data.arc_mode == 0) {
        label = data.points.size() == 1 ? "Radius" : "Sweep";
      }
      else if (data.arc_mode == 1) {
        label = data.points.size() < 2 ? "Chord" : "Sagitta";
      }
      else {
        label = data.points.size() < 2 ? "Chord" : "Distance";
      }
    }
    char value[NUM_STR_REP_LEN] = "";
    if (has_input) {
      outputNumInput(&data.num, value, CTX_data_scene(C)->unit);
    }
    append(std::string(label) + ": " + value);
  }
  ED_area_status_text(CTX_wm_area(C), status.str().c_str());
}

static wmOperatorStatus arc_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (event->type == RIGHTMOUSE && event->val == KM_PRESS) {
    draw_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS &&
      (data.numeric_mode == DrawNumericMode::Segments ||
       (data.numeric_mode != DrawNumericMode::Automatic && !hasNumInput(&data.num)) ||
       (data.points.size() > 1 && hasNumInput(&data.num))))
  {
    if (hasNumInput(&data.num)) {
      update_mouse(C, op, event);
    }
    arc_num_reset(C, data);
    data.numeric_hold = true;
    update_arc_area_status_text(C, op);
    return OPERATOR_RUNNING_MODAL;
  }
  const bool confirm = ELEM(event->type, LEFTMOUSE, EVT_RETKEY, EVT_PADENTER) &&
                       event->val == KM_PRESS;
  if (confirm) {
    update_mouse(C, op, event);
    if (data.points.size() == 1) {
      if (math::distance(data.points[0], data.current) <= 1e-9) {
        return OPERATOR_RUNNING_MODAL;
      }
      data.points.append(data.current);
      data.point_vertices.append(data.current_vertex);
      if (data.arc_mode != 0) {
        arc_set_chord_plane(data);
      }
      data.locked = true;
      data.axis = -1;
      data.shift_direction.reset();
      data.arc_previous_angle = data.arc_accumulated = data.arc_sweep = 0;
      arc_num_reset(C, data);
      data.numeric_hold = false;
      update_mouse(C, op, event);
      return OPERATOR_RUNNING_MODAL;
    }
    Vector<double3> points = data.points;
    if (data.arc_mode != 0) {
      points.append(data.current);
    }
    std::string error;
    if (!arc_support(points,
                     data.arc_mode,
                     double3(data.plane_normal),
                     data.arc_sweep,
                     data.inverse,
                     error))
    {
      BKE_report(op->reports, RPT_WARNING, error.c_str());
      return OPERATOR_RUNNING_MODAL;
    }
    std::ostringstream input;
    input << std::setprecision(17);
    const auto index = TopologyIndex::build(data.source->source().geometry());
    for (const int i : points.index_range()) {
      const auto *vertex = i < data.point_vertices.size() && index ?
                               index->vertex(data.point_vertices[i]) :
                               nullptr;
      const double3 local = vertex && math::distance(
                                          math::transform_point(
                                              double4x4(data.source->object().object_to_world()),
                                              double3(vertex->position)),
                                          points[i]) <= 1e-10 ?
                                double3(vertex->position) :
                                math::transform_point(data.inverse, points[i]);
      input << local.x << ' ' << local.y << ' ' << local.z << '\n';
    }
    RNA_string_set(op->ptr, "points", input.str().c_str());
    RNA_float_set_array(op->ptr, "plane_normal", &data.plane_normal.x);
    RNA_float_set(op->ptr, "sweep", float(data.arc_sweep));
    std::ostringstream sweep;
    sweep << std::setprecision(17) << data.arc_sweep;
    RNA_string_set(op->ptr, "sweep_exact", sweep.str().c_str());
    std::ostringstream references;
    references << data.point_vertices[data.arc_mode == 0 ? 1 : 0] << ' '
               << (data.arc_mode == 0 ? data.current_vertex : data.point_vertices[1]);
    RNA_string_set(op->ptr, "vertex_references", references.str().c_str());
    draw_exit(C, op);
    const wmOperatorStatus result = arc_exec(C, op);
    WM_toolsystem_ref_set_by_id(C, "builtin.select_box");
    return result;
  }
  if (event->val == KM_PRESS && handleNumInput(C, &data.num, event)) {
    update_mouse(C, op, event);
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->val == KM_PRESS) {
    if (event->type == EVT_BACKSPACEKEY && data.points.size() > 1) {
      data.points.remove_last();
      data.point_vertices.remove_last();
      data.plane_normal = data.arc_reference_normal;
      data.arc_sweep = data.arc_accumulated = data.arc_previous_angle = 0;
      arc_num_reset(C, data);
    }
    else if (event->type == EVT_LKEY && data.points.size() == 1) {
      data.locked = !data.locked;
      if (data.locked) {
        data.arc_reference_normal = data.plane_normal;
      }
    }
    else if (event->type == EVT_PKEY) {
      data.arc_perpendicular = !data.arc_perpendicular;
      if (data.points.size() == 2) {
        const double3 chord = math::normalize(data.points[1] - data.points[0]);
        if (std::abs(math::dot(chord, double3(data.arc_reference_normal))) > 0.99) {
          /* For a vertical chord, P alternates its two reference-plane orientations. */
          const double3 x = arc_plane_axes(double3(data.arc_reference_normal))[0];
          data.arc_vertical_axis = std::abs(math::dot(x, double3(data.plane_normal))) > 0.5 ? 1 :
                                                                                              0;
        }
        arc_set_chord_plane(data);
      }
      else {
        const double3 chord = data.current - data.points[0];
        const double3 n = math::cross(chord, double3(data.arc_reference_normal));
        if (data.arc_perpendicular && math::length_squared(n) > 1e-12) {
          data.plane_normal = float3(math::normalize(n));
        }
        else {
          data.plane_normal = data.arc_reference_normal;
        }
      }
      data.arc_previous_angle = data.arc_accumulated = data.arc_sweep = 0;
    }
    else if (ELEM(event->type, EVT_XKEY, EVT_YKEY, EVT_ZKEY)) {
      const int axis = event->type == EVT_XKEY ? 0 : event->type == EVT_YKEY ? 1 : 2;
      if (data.points.size() == 2 || data.arc_mode == 0) {
        double3 normal(0);
        normal[axis] = 1;
        const double3 bridge = data.points.size() == 2 ? data.points[1] - data.points[0] :
                                                         double3(0);
        if (math::length_squared(bridge) == 0 ||
            std::abs(math::dot(math::normalize(bridge), normal)) < 1e-6)
        {
          data.plane_normal = float3(normal);
          if (data.points.size() == 1) {
            data.arc_reference_normal = data.plane_normal;
            data.arc_perpendicular = false;
          }
        }
        data.arc_previous_angle = data.arc_accumulated = data.arc_sweep = 0;
      }
      else {
        data.axis = data.axis == axis ? -1 : axis;
      }
    }
    else if (event->type == EVT_NKEY && data.points.size() == 1) {
      data.axis = data.axis == 3 ? -1 : 3;
    }
  }
  if (event->type == MOUSEMOVE || event->val == KM_PRESS ||
      ELEM(event->type, EVT_LEFTSHIFTKEY, EVT_RIGHTSHIFTKEY))
  {
    update_mouse(C, op, event);
    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
}

static const NurbsCurveGeometry *find_curve(const NurbsGeometry &geometry, const int64_t id)
{
  for (const auto &curve : geometry.curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

/* Selected native edges replace RCAD's mesh-chain Catmull-Rom reconstruction.
 * Projection refines parameters on the original rational supports, never on display wires. */
static bool circle_load_chains(DrawData &data, std::string &error)
{
  const auto &snapshot = data.source->source();
  const auto &geometry = snapshot.geometry();
  Vector<const NurbsTopologyEdge *> pending;
  for (const auto &edge : geometry.edges) {
    if (snapshot.selection().edges.contains(edge.id)) {
      pending.append(&edge);
    }
  }
  while (!pending.is_empty()) {
    Vector<const NurbsTopologyEdge *> chain;
    chain.append(pending.pop_last());
    for (int i = 0; i < chain.size(); i++) {
      for (int j = pending.size() - 1; j >= 0; j--) {
        const auto *a = chain[i], *b = pending[j];
        if (ELEM(a->vertex_ids[0], b->vertex_ids[0], b->vertex_ids[1]) ||
            ELEM(a->vertex_ids[1], b->vertex_ids[0], b->vertex_ids[1]))
        {
          chain.append(b);
          pending.remove_and_reorder(j);
        }
      }
    }
    data.circle_chains.append(std::move(chain));
  }
  const int count = data.arc_mode == 3 ? 2 : 3;
  if (data.circle_chains.size() != count) {
    error = count == 2 ? "Select two separate native curve chains for Tangent to 2 Curves" :
                         "Select three separate native curve chains for Tangent to 3 Curves";
    return false;
  }
  const double4x4 transform(data.source->object().object_to_world());
  Vector<double3> plane_points;
  for (const auto &chain : data.circle_chains) {
    for (const auto *edge : chain) {
      const auto *curve = find_curve(geometry, edge->curve_id);
      if (!curve) {
        return false;
      }
      Vector<float3> overlay;
      for (int i = 0; i <= 32; i++) {
        const double t = edge->parameter_range[0] +
                         (edge->parameter_range[1] - edge->parameter_range[0]) * i / 32;
        const auto sample = evaluate_curve(*curve, t);
        if (!sample) {
          return false;
        }
        const double3 world = math::transform_point(transform, sample->position);
        overlay.append(float3(world));
        plane_points.append(world);
      }
      data.circle_curve_overlays.append(std::move(overlay));
    }
  }
  double3 normal(0);
  const double3 origin = plane_points[0];
  double3 direction(0);
  for (const double3 &point : plane_points) {
    if (math::length_squared(point - origin) > math::length_squared(direction)) {
      direction = point - origin;
    }
  }
  for (const double3 &point : plane_points) {
    const double3 cross = math::cross(direction, point - origin);
    if (math::length_squared(cross) > math::length_squared(normal)) {
      normal = cross;
    }
  }
  if (math::length_squared(normal) < 1e-20) {
    error = "Tangent curves must define a common plane";
    return false;
  }
  normal = math::normalize(normal);
  const double scale = std::max(1.0, math::length(direction));
  for (const double3 &point : plane_points) {
    if (std::abs(math::dot(point - origin, normal)) > scale * 1e-6) {
      error = "Tangent circle construction requires coplanar curves";
      return false;
    }
  }
  data.circle_solution_seed = double3(0);
  for (const double3 &point : plane_points) {
    data.circle_solution_seed += point / plane_points.size();
  }
  data.plane_origin = float3(origin);
  data.plane_normal = data.arc_reference_normal = float3(normal);
  return true;
}

static double3 circle_contact(const DrawData &data,
                              const int chain_index,
                              const double3 &center,
                              const bool farthest = false,
                              double *tangent_error = nullptr)
{
  const auto &geometry = data.source->source().geometry();
  const double4x4 transform(data.source->object().object_to_world());
  double best = farthest ? -DBL_MAX : DBL_MAX;
  double3 contact(0);
  for (const auto *edge : data.circle_chains[chain_index]) {
    const auto *curve = find_curve(geometry, edge->curve_id);
    auto position = [&](const double t) {
      return math::transform_point(transform, evaluate_curve(*curve, t)->position);
    };
    auto score = [&](const double t) {
      const double distance = math::length_squared(position(t) - center);
      return farthest ? -distance : distance;
    };
    const double lo = edge->parameter_range[0], hi = edge->parameter_range[1];
    double parameter = lo, best_score = DBL_MAX;
    for (int i = 0; i <= 64; i++) {
      const double t = lo + (hi - lo) * i / 64;
      const double value = score(t);
      if (value < best_score) {
        best_score = value;
        parameter = t;
      }
    }
    double a = std::max(lo, parameter - (hi - lo) / 64);
    double b = std::min(hi, parameter + (hi - lo) / 64);
    for (int j = 0; j < 32; j++) {
      const double l = a + (b - a) / 3, r = b - (b - a) / 3;
      if (score(l) < score(r)) {
        b = r;
      }
      else {
        a = l;
      }
    }
    for (const double t : {lo, (a + b) / 2, hi}) {
      const double3 point = position(t);
      const double distance = math::length_squared(point - center);
      if ((farthest && distance > best) || (!farthest && distance < best)) {
        best = distance;
        contact = point;
        if (tangent_error) {
          const double h = (hi - lo) * 1e-6;
          const double3 tangent = position(std::min(hi, t + h)) - position(std::max(lo, t - h));
          *tangent_error = math::length_squared(tangent) > 0 && distance > 0 ?
                               std::abs(math::dot(math::normalize(tangent),
                                                  math::normalize(center - point))) :
                               1;
        }
      }
    }
  }
  return contact;
}

static bool circle_solve_tangent(DrawData &data, const double3 &seed)
{
  const auto [x, y] = arc_plane_axes(double3(data.plane_normal));
  const double3 n = math::normalize(double3(data.plane_normal));
  double3 center = seed - n * math::dot(seed - double3(data.plane_origin), n);
  static constexpr int signs[8][3] = {{1, 1, 1},
                                      {-1, -1, -1},
                                      {1, 1, -1},
                                      {1, -1, 1},
                                      {-1, 1, 1},
                                      {-1, -1, 1},
                                      {-1, 1, -1},
                                      {1, -1, -1}};
  auto contact = [&](const int i, const double3 &c) {
    return circle_contact(data, i, c, data.arc_mode == 4 && signs[data.circle_solution][i] < 0);
  };
  for (int iteration = 0; iteration < 40; iteration++) {
    const double3 a = contact(0, center), b = contact(1, center);
    const double ra = math::distance(center, a), rb = math::distance(center, b);
    if (ra < 1e-9 || rb < 1e-9) {
      return false;
    }
    if (data.arc_mode == 3) {
      const double3 diff = b - a;
      const double3 rail = math::cross(diff, n);
      if (math::length_squared(rail) < 1e-18) {
        return false;
      }
      const double3 mid = (a + b) / 2;
      const double3 next = mid +
                           rail * (math::dot(center - mid, rail) / math::length_squared(rail));
      if (math::distance(center, next) < 1e-9) {
        break;
      }
      center = next;
    }
    else {
      const double3 c = contact(2, center);
      const double rc = math::distance(center, c);
      if (rc < 1e-9) {
        return false;
      }
      const double f = rb - ra, g = rc - ra;
      if (std::max(std::abs(f), std::abs(g)) < 1e-8 * std::max(1.0, ra)) {
        break;
      }
      const double3 df = (center - b) / rb - (center - a) / ra;
      const double3 dg = (center - c) / rc - (center - a) / ra;
      const double a00 = math::dot(df, x), a01 = math::dot(df, y);
      const double a10 = math::dot(dg, x), a11 = math::dot(dg, y);
      const double det = a00 * a11 - a01 * a10;
      if (std::abs(det) < 1e-12) {
        return false;
      }
      const double3 step = x * ((-f * a11 + g * a01) / det) + y * ((-g * a00 + f * a10) / det);
      center += step * 0.8;
      if (!std::isfinite(math::length_squared(center))) {
        return false;
      }
    }
  }
  data.circle_contacts.clear();
  double radius = 0;
  for (const int i : data.circle_chains.index_range()) {
    double tangent_error = 1;
    const bool farthest = data.arc_mode == 4 && signs[data.circle_solution][i] < 0;
    const double3 point = circle_contact(data, i, center, farthest, &tangent_error);
    if (tangent_error > 1e-4) {
      data.circle_contacts.clear();
      return false;
    }
    data.circle_contacts.append(point);
    radius += math::distance(center, data.circle_contacts.last()) / data.circle_chains.size();
  }
  for (const double3 &p : data.circle_contacts) {
    if (std::abs(math::distance(center, p) - radius) > 1e-6 * std::max(1.0, radius)) {
      data.circle_contacts.clear();
      return false;
    }
  }
  data.circle_center = center;
  data.arc_radius = radius;
  data.current = center + x * radius;
  data.points[0] = center;
  return radius > 1e-8;
}

static void circle_update(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (data.points.is_empty()) {
    return;
  }
  data.arc_hud.clear();
  data.arc_hud.append(data.arc_mode == 0 ? "Center Circle" :
                      data.arc_mode == 1 ? "2 Point Circle" :
                      data.arc_mode == 2 ? "3 Point Circle" :
                      data.arc_mode == 3 ? "Circle Tangent to 2 Curves" :
                                           "Circle Tangent to 3 Curves");
  if (data.arc_mode >= 3) {
    /* The addon holds the three-curve solution while the pointer moves. Tab
     * solves the next contact configuration from the source curves again. */
    if (data.arc_mode == 3 || !data.circle_tangent_valid) {
      data.circle_tangent_valid = circle_solve_tangent(
          data, data.arc_mode == 4 ? data.circle_solution_seed : data.current);
    }
    if (!data.circle_tangent_valid) {
      data.circle_contacts.clear();
      data.current = data.points[0];
      data.arc_hud.append("No tangent solution here; move the pointer or press Tab");
    }
    else {
      data.points[0] = data.circle_center;
      data.current = data.circle_center +
                     arc_plane_axes(double3(data.plane_normal))[0] * data.arc_radius;
      data.arc_hud.append(data.arc_mode == 4 ? "Tab: Next solution | Click: Confirm" :
                                               "Move center | Click: Confirm");
    }
    return;
  }
  /* Circle planes follow the live radius/diameter before the last placement. */
  if (data.points.size() == 1) {
    const double3 bridge = data.current - data.points[0];
    const double3 up(data.arc_reference_normal);
    if (math::length_squared(bridge) > 1e-16) {
      const double3 dir = math::normalize(bridge);
      if (data.arc_perpendicular || std::abs(math::dot(dir, up)) > 0.995) {
        double3 normal = math::cross(dir, up);
        if (math::length_squared(normal) < 1e-12) {
          const auto [x, y] = arc_plane_axes(up);
          const auto *rv3d = static_cast<RegionView3D *>(data.region->regiondata);
          const double3 view(rv3d->viewinv[2]);
          normal = data.arc_vertical_axis == 0                                 ? x :
                   data.arc_vertical_axis == 1                                 ? y :
                   std::abs(math::dot(view, x)) > std::abs(math::dot(view, y)) ? x :
                                                                                 y;
        }
        data.plane_normal = float3(math::normalize(normal));
      }
      else {
        data.plane_normal = data.arc_reference_normal;
      }
    }
    data.current = arc_project(data, data.current);
  }
  else if (data.arc_mode == 2) {
    /* RCAD's three-point circle lets the third point establish its plane. */
    const double3 n = math::cross(data.points[1] - data.points[0], data.current - data.points[0]);
    if (math::length_squared(n) > 1e-16 && !data.arc_perpendicular) {
      data.plane_normal = float3(math::normalize(n));
    }
    data.current = arc_project(data, data.current);
  }
  data.arc_hud.append(data.points.size() == 2 ? "Place third point (3/3)" :
                      data.arc_mode == 0      ? "Place radius (2/2)" :
                      data.arc_mode == 1      ? "Place diameter endpoint (2/2)" :
                                                "Place second point (2/3)");
  if (data.points.size() == 1) {
    std::ostringstream label;
    label << (data.arc_mode == 0 ? "R: " : "D: ") << std::fixed << std::setprecision(3)
          << math::distance(data.points[0], data.current);
    data.arc_hud.append(label.str());
  }
  ED_area_status_text(CTX_wm_area(C), data.arc_hud.last().c_str());
  UNUSED_VARS(event);
}

static wmOperatorStatus circle_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (event->type == RIGHTMOUSE && event->val == KM_PRESS) {
    draw_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS &&
      (hasNumInput(&data.num) || data.numeric_mode != DrawNumericMode::Automatic))
  {
    if (hasNumInput(&data.num)) {
      update_mouse(C, op, event);
    }
    arc_num_reset(C, data);
    data.numeric_hold = true;
    update_arc_area_status_text(C, op);
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->val == KM_PRESS && ELEM(event->type, LEFTMOUSE, EVT_RETKEY, EVT_PADENTER)) {
    update_mouse(C, op, event);
    if (data.arc_mode >= 3 && !data.circle_tangent_valid) {
      BKE_report(op->reports, RPT_WARNING, "No valid tangent circle to confirm");
      return OPERATOR_RUNNING_MODAL;
    }
    if (data.arc_mode == 2 && data.points.size() == 1) {
      if (math::distance(data.current, data.points[0]) <= 1e-9) {
        return OPERATOR_RUNNING_MODAL;
      }
      data.points.append(data.current);
      data.point_vertices.append(data.current_vertex);
      data.locked = true;
      data.axis = -1;
      arc_set_chord_plane(data);
      arc_num_reset(C, data);
      data.numeric_hold = false;
      update_mouse(C, op, event);
      return OPERATOR_RUNNING_MODAL;
    }
    Vector<double3> points = data.points;
    points.append(data.current);
    std::string error;
    if (!circle_support(points, data.arc_mode, double3(data.plane_normal), data.inverse, error)) {
      BKE_report(op->reports, RPT_WARNING, error.c_str());
      return OPERATOR_RUNNING_MODAL;
    }
    std::ostringstream input;
    input << std::setprecision(17);
    for (const double3 &world : points) {
      const double3 local = math::transform_point(data.inverse, world);
      input << local.x << ' ' << local.y << ' ' << local.z << '\n';
    }
    RNA_string_set(op->ptr, "points", input.str().c_str());
    RNA_float_set_array(op->ptr, "plane_normal", &data.plane_normal.x);
    const int64_t seam = data.arc_mode == 0 ? data.current_vertex : data.point_vertices[0];
    std::ostringstream references;
    references << (data.arc_mode >= 3 ? 0 : seam) << ' ' << (data.arc_mode >= 3 ? 0 : seam);
    RNA_string_set(op->ptr, "vertex_references", references.str().c_str());
    draw_exit(C, op);
    const wmOperatorStatus result = arc_exec(C, op);
    WM_toolsystem_ref_set_by_id(C, "builtin.select_box");
    return result;
  }
  if (event->val == KM_PRESS && handleNumInput(C, &data.num, event)) {
    update_mouse(C, op, event);
    return OPERATOR_RUNNING_MODAL;
  }
  if (data.arc_mode >= 3 && event->type == EVT_TKEY && event->val == KM_PRESS) {
    RNA_boolean_set(
        op->ptr, "anchor_tangent_markers", !RNA_boolean_get(op->ptr, "anchor_tangent_markers"));
    rebuild_preview(op);
    ED_region_tag_redraw(data.region);
    return OPERATOR_RUNNING_MODAL;
  }
  if (data.arc_mode >= 3) {
    if (event->type == EVT_TABKEY && event->val == KM_PRESS && data.arc_mode == 4) {
      data.circle_solution = (data.circle_solution + 1) % 8;
      data.circle_tangent_valid = false;
    }
    if (event->type == MOUSEMOVE || event->val == KM_PRESS) {
      update_mouse(C, op, event);
    }
    return OPERATOR_RUNNING_MODAL;
  }
  /* Plane locking, perpendicular toggling, axis keys and numeric length entry share
   * the native arc input path. Confirmation above has circle-specific stage counts. */
  if (ELEM(event->type, EVT_XKEY, EVT_YKEY, EVT_ZKEY) && event->val == KM_PRESS &&
      data.points.size() == 1)
  {
    const int axis = event->type == EVT_XKEY ? 0 : event->type == EVT_YKEY ? 1 : 2;
    const double3 bridge = data.current - data.points[0];
    if (math::length_squared(bridge) > 1e-16 &&
        std::abs(math::dot(math::normalize(bridge), double3(data.arc_reference_normal))) > 0.99 &&
        axis < 2)
    {
      data.arc_vertical_axis = axis;
    }
    else {
      data.axis = data.axis == axis ? -1 : axis;
    }
    update_mouse(C, op, event);
    return OPERATOR_RUNNING_MODAL;
  }
  return arc_modal(C, op, event);
}

static wmOperatorStatus draw_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  std::string error;
  auto source = ed::nurbs::TopologyPreview::begin(C, &error);
  if (!source) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  auto *data = MEM_new<DrawData>(__func__);
  data->source = std::move(source);
  data->spline = is_spline(op);
  data->shape = shape_type(op);
  data->shape_mode = data->shape ? RNA_enum_get(op->ptr, "mode") : 0;
  data->keep_foci = data->shape == 1 && data->shape_mode == 1 &&
                    RNA_boolean_get(op->ptr, "keep_foci");
  data->circle = is_circle(op);
  if (data->circle) {
    data->locked = false;
  }
  data->arc_mode = is_arc(op) || data->circle ? RNA_enum_get(op->ptr, "mode") : -1;
  bool invertible;
  data->inverse = math::invert(double4x4(data->source->object().object_to_world()), invertible);
  if (!invertible) {
    MEM_delete(data);
    BKE_report(op->reports, RPT_ERROR, "Drawing requires an invertible object transform");
    return OPERATOR_CANCELLED;
  }
  initNumInput(&data->num);
  data->num.idx_max = 0;
  data->num.unit_sys = CTX_data_scene(C)->unit.system;
  data->num.unit_type[0] = B_UNIT_LENGTH;
  data->num.flag |= NUM_USE_DOUBLE;
  data->region = CTX_wm_region(C);
  data->plane_origin = CTX_data_scene(C)->cursor.location;
  const auto *rv3d = static_cast<RegionView3D *>(data->region->regiondata);
  data->plane_normal = (data->arc_mode >= 0 || data->shape) ?
                           float3(0, 0, 1) :
                           math::normalize(float3(rv3d->viewinv[2]));
  data->snap = ed::transform::snap_object_context_create();
  data->navigation = ED_view3d_navigation_init(C, nullptr);
  op->customdata = data;
  data->draw_handle = ED_region_draw_cb_activate(
      data->region->runtime->type, draw_preview, op, REGION_DRAW_POST_VIEW);
  if (data->circle && data->arc_mode >= 0) {
    data->draw_handle_pixel = ED_region_draw_cb_activate(
        data->region->runtime->type, draw_arc_hud, op, REGION_DRAW_POST_PIXEL);
  }
  if (data->circle && data->arc_mode >= 3 && !circle_load_chains(*data, error)) {
    draw_exit(C, op);
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  update_mouse(C, op, event);
  data->points.append(data->current);
  data->point_vertices.append(data->current_vertex);
  data->plane_origin = float3(data->current);
  data->arc_reference_normal = data->plane_normal;
  if (data->arc_mode >= 0) {
    if (data->circle) {
      circle_update(C, op, event);
    }
    else {
      arc_update(C, op, event);
    }
    rebuild_preview(op);
    update_arc_area_status_text(C, op);
  }
  ED_workspace_status_text(
      C,
      data->shape ?
          "Click: Place construction point | Enter: Accept value / finish | Esc / Right Click: "
          "Cancel | "
          "Backspace: Previous stage | P: Perpendicular | X/Y: Plane / rectangle dimensions | "
          "R/D: Size | S: Segments / sides | Shift: Square | Ctrl: Toggle snap" :
      data->circle ?
          "Click / Enter: Confirm circle placement | Esc / Right Click: Cancel | Backspace: "
          "Previous stage | "
          "P: Perpendicular plane | X/Y/Z: Axis | L: Plane lock | S: Segments | R/D: Size | "
          "Tab: Tangent solution | C: Angle snap | Ctrl: Toggle snap | Alt: Free" :
      data->arc_mode >= 0 ?
          "Click: Radius / endpoint, then finish arc | Enter: Confirm | Esc / Right Click: Cancel "
          "| "
          "Backspace: Previous stage | P: Perpendicular plane | X/Y/Z: Axis | S: Segments | R: "
          "Radius / length | D: Chord / sagitta | A: Sweep | H: Sagitta | C: Angle snap | "
          "Ctrl: Toggle snap | Alt: Free" :
      data->spline ?
          "Click: Add point | Enter / Right Click: Finish | Esc: Cancel | "
          "Backspace: Remove point | R: Length | S: Segments | X/Y/Z: Axis | N: Normal | "
          "Shift: Direction | L: Plane lock | Tab: Fit Points / CVs | Ctrl: Toggle snap" :
          "Click: Add point | Enter / Right Click: Finish | Esc: Cancel | "
          "Backspace: Remove point | R: Length | X/Y/Z: Axis | N: Normal | Shift: Direction | "
          "L: Plane lock | Ctrl: Toggle snap");
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static bool draw_numeric_shortcut(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (event->val != KM_PRESS) {
    return false;
  }
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (data.numeric_mode != DrawNumericMode::Automatic || hasNumInput(&data.num)) {
    return false;
  }
  DrawNumericMode mode = DrawNumericMode::Automatic;
  const char *label = nullptr;
  if (event->type == EVT_CKEY && data.arc_mode >= 0) {
    RNA_boolean_set(op->ptr, "use_angle_snap", !RNA_boolean_get(op->ptr, "use_angle_snap"));
    update_mouse(C, op, event);
    return true;
  }
  if (event->type == EVT_RKEY && !data.points.is_empty()) {
    if (data.circle) {
      if (data.arc_mode == 0 && data.points.size() == 1) {
        mode = DrawNumericMode::Radius;
      }
      else if (data.arc_mode == 1 && data.points.size() == 1) {
        mode = DrawNumericMode::Radius;
        data.numeric_initial_circle_input_radius = data.circle_input_radius;
        data.circle_input_radius = true;
      }
    }
    else if (data.arc_mode >= 0 || !data.points.is_empty()) {
      mode = DrawNumericMode::Radius;
    }
    if (mode == DrawNumericMode::Radius) {
      label = data.circle                                  ? "Radius" :
              data.arc_mode == 0                           ? "Radius" :
              data.arc_mode == 1 && data.points.size() > 1 ? "Sagitta" :
              data.arc_mode == 1                           ? "Chord" :
                                                             "Length";
    }
  }
  else if (event->type == EVT_SKEY && (data.arc_mode >= 0 || data.spline)) {
    mode = DrawNumericMode::Segments;
    label = "Segments";
    data.numeric_initial_segments = RNA_int_get(op->ptr, "preview_points");
  }
  else if (event->type == EVT_DKEY && data.circle && data.arc_mode == 1 && data.points.size() == 1)
  {
    mode = DrawNumericMode::Diameter;
    data.numeric_initial_circle_input_radius = data.circle_input_radius;
    data.circle_input_radius = false;
    label = "Diameter";
  }
  else if (event->type == EVT_DKEY && !data.circle && data.arc_mode == 1 &&
           data.points.size() == 1)
  {
    mode = DrawNumericMode::Length;
    label = "Chord";
  }
  else if (event->type == EVT_DKEY && !data.circle && data.arc_mode == 1 &&
           data.points.size() == 2)
  {
    mode = DrawNumericMode::Sagitta;
    label = "Sagitta";
  }
  else if (event->type == EVT_AKEY && !data.circle && data.arc_mode == 0 &&
           data.points.size() == 2)
  {
    mode = DrawNumericMode::Angle;
    label = "Angle";
  }
  else if (event->type == EVT_HKEY && !data.circle && data.arc_mode == 1 &&
           data.points.size() == 2)
  {
    mode = DrawNumericMode::Sagitta;
    label = "Sagitta";
  }
  if (mode == DrawNumericMode::Automatic) {
    return false;
  }
  draw_num_input_begin(C, data, mode, label);
  update_arc_area_status_text(C, op);
  ED_region_tag_redraw(data.region);
  return true;
}

static void shape_plane(DrawData &data)
{
  double3 normal(data.arc_reference_normal);
  if (data.arc_perpendicular) {
    if (data.points.size() > 1) {
      normal = math::cross(data.points[1] - data.points[0], normal);
    }
    else {
      const auto axes = arc_plane_axes(normal);
      const auto *rv3d = static_cast<const RegionView3D *>(data.region->regiondata);
      const double3 view(rv3d->viewinv[2]);
      normal = data.arc_vertical_axis == 0                                             ? axes[0] :
               data.arc_vertical_axis == 1                                             ? axes[1] :
               std::abs(math::dot(view, axes[0])) > std::abs(math::dot(view, axes[1])) ? axes[0] :
                                                                                         axes[1];
    }
  }
  if (math::length_squared(normal) > 1e-12) {
    data.plane_normal = float3(math::normalize(normal));
  }
}

static wmOperatorStatus shape_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  const bool enter = event->val == KM_PRESS && ELEM(event->type, EVT_RETKEY, EVT_PADENTER);
  const bool pending = data.numeric_mode != DrawNumericMode::Automatic || hasNumInput(&data.num);
  if (pending) {
    if (enter) {
      double value = 0;
      if (applyNumInput(&data.num, &value)) {
        const auto mode = data.numeric_mode;
        value = std::abs(value);
        if (mode == DrawNumericMode::Segments) {
          RNA_int_set(op->ptr,
                      "preview_points",
                      std::clamp(int(std::lround(value)), data.shape == 1 ? 4 : 3, 256));
        }
        else if (mode == DrawNumericMode::Width || mode == DrawNumericMode::Height) {
          auto [x, y] = arc_plane_axes(double3(data.plane_normal));
          if (data.shape_mode == 2) {
            const double3 edge = (data.points.size() > 1 ? data.points[1] : data.current) -
                                 data.points[0];
            if (math::length(edge) > 1e-9) {
              x = math::normalize(edge);
              y = math::normalize(math::cross(double3(data.plane_normal), x));
            }
          }
          const double sign = math::dot(data.current - data.points[0],
                                        mode == DrawNumericMode::Width ? x : y);
          (mode == DrawNumericMode::Width ? data.width : data.height) = std::copysign(value, sign);
          if (data.shape_mode == 2 && data.points.size() == 1 && mode == DrawNumericMode::Width) {
            const double3 delta = data.current - data.points[0];
            const double3 x = math::length(delta) > 1e-9 ?
                                  math::normalize(delta) :
                                  arc_plane_axes(double3(data.plane_normal))[0];
            data.points.append(data.points[0] + x * value);
            data.point_vertices.append(0);
          }
        }
        else if (data.shape == 1) {
          const double3 anchor = data.points[0];
          auto [x, y] = arc_plane_axes(double3(data.plane_normal));
          const double3 delta = (data.points.size() > 1 ? data.points[1] : data.current) - anchor;
          if (math::length(delta) > 1e-9) {
            x = math::normalize(delta);
            y = math::normalize(math::cross(double3(data.plane_normal), x));
          }
          if (data.shape_mode == 1 || mode == DrawNumericMode::Diameter) {
            const double length = mode == DrawNumericMode::Diameter && data.shape_mode == 0 ?
                                      value / 2 :
                                      value;
            if (data.points.size() == 1) {
              data.points.append(anchor + x * length);
              data.point_vertices.append(0);
            }
            else {
              data.points[1] = anchor + x * length;
            }
          }
          else {
            if (data.points.size() == 1) {
              data.points.append(data.current);
              data.point_vertices.append(0);
            }
            const double3 center = data.shape_mode == 0 ? anchor : (anchor + data.points[1]) / 2;
            data.current = center + y * value;
          }
        }
        else {
          const double3 delta = data.current - data.points[0];
          const double3 x = math::length(delta) > 1e-9 ?
                                math::normalize(delta) :
                                arc_plane_axes(double3(data.plane_normal))[0];
          data.current = data.points[0] + x * value;
        }
        data.current_vertex = 0;
        data.snap_type = SCE_SNAP_TO_NONE;
      }
      arc_num_reset(C, data);
      data.numeric_hold = true;
      rebuild_preview(op);
      update_arc_area_status_text(C, op);
      ED_region_tag_redraw(data.region);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->val == KM_PRESS && event->type != LEFTMOUSE) {
      handleNumInput(C, &data.num, event);
      update_arc_area_status_text(C, op);
      ED_region_tag_redraw(data.region);
    }
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == RIGHTMOUSE && event->val == KM_PRESS) {
    draw_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  if (event->val == KM_PRESS) {
    DrawNumericMode mode = DrawNumericMode::Automatic;
    const char *label = "Length";
    if (event->type == EVT_SKEY && data.shape != 3) {
      mode = DrawNumericMode::Segments;
      label = data.shape == 2 ? "Sides" : "Segments";
      data.numeric_initial_segments = RNA_int_get(op->ptr, "preview_points");
    }
    else if (data.shape == 3 && ELEM(event->type, EVT_XKEY, EVT_YKEY)) {
      mode = event->type == EVT_XKEY ? DrawNumericMode::Width : DrawNumericMode::Height;
      label = event->type == EVT_XKEY ? "Width" : "Height";
    }
    else if (event->type == EVT_RKEY &&
             !(data.shape == 1 &&
               (data.shape_mode == 3 || (data.shape_mode == 1 && data.points.size() > 1))) &&
             data.shape != 3)
    {
      mode = DrawNumericMode::Radius;
      label = data.shape == 1 ? (data.shape_mode == 1 ? "Focus distance" : "Minor radius") :
                                "Radius";
    }
    else if (event->type == EVT_DKEY && data.shape == 1 && ELEM(data.shape_mode, 0, 2)) {
      mode = DrawNumericMode::Diameter;
      label = "Major diameter";
    }
    else if ((event->type == EVT_AKEY && data.shape == 2 && data.shape_mode == 1) ||
             (event->type == EVT_LKEY && data.shape == 2 && data.shape_mode >= 2))
    {
      mode = DrawNumericMode::Length;
      label = data.shape_mode == 1 ? "Apothem" : "Length";
    }
    if (mode != DrawNumericMode::Automatic) {
      draw_num_input_begin(C, data, mode, label);
      update_arc_area_status_text(C, op);
      ED_region_tag_redraw(data.region);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->type == EVT_PKEY) {
      data.arc_perpendicular = !data.arc_perpendicular;
      shape_plane(data);
    }
    else if (ELEM(event->type, EVT_XKEY, EVT_YKEY) && data.shape != 3) {
      data.arc_vertical_axis = event->type == EVT_XKEY ? 0 : 1;
      shape_plane(data);
    }
    else if (event->type == EVT_LKEY) {
      data.locked = !data.locked;
    }
    else if (event->type == EVT_KKEY && data.shape == 1 && data.shape_mode == 1) {
      data.keep_foci = !data.keep_foci;
      RNA_boolean_set(op->ptr, "keep_foci", data.keep_foci);
    }
    else if (ELEM(event->type, EVT_LEFTSHIFTKEY, EVT_RIGHTSHIFTKEY) && data.shape == 3 &&
             data.shape_mode != 2)
    {
      data.square = !data.square;
    }
    else if (event->type == EVT_BACKSPACEKEY && data.points.size() > 1) {
      data.points.remove_last();
      data.point_vertices.remove_last();
      data.width.reset();
      data.height.reset();
    }
    else if (event->type == LEFTMOUSE || enter) {
      update_mouse(C, op, event);
      if (shape_three_points(data) && data.points.size() == 1) {
        if (math::distance(data.current, data.points[0]) > 1e-9) {
          data.points.append(data.current);
          data.point_vertices.append(data.current_vertex);
          shape_plane(data);
          data.numeric_hold = false;
          rebuild_preview(op);
          update_arc_area_status_text(C, op);
          ED_region_tag_redraw(data.region);
        }
        return OPERATOR_RUNNING_MODAL;
      }
      if (!data.arc_valid) {
        return OPERATOR_RUNNING_MODAL;
      }
      std::ostringstream points;
      points << std::setprecision(17);
      for (const double3 &world : data.shape_points) {
        const double3 local = math::transform_point(data.inverse, world);
        points << local.x << ' ' << local.y << ' ' << local.z << '\n';
      }
      RNA_string_set(op->ptr, "points", points.str().c_str());
      if (data.keep_foci && data.points.size() > 1) {
        std::ostringstream foci;
        foci << std::setprecision(17);
        for (const double3 &world : data.points) {
          const double3 local = math::transform_point(data.inverse, world);
          foci << local.x << ' ' << local.y << ' ' << local.z << '\n';
        }
        RNA_string_set(op->ptr, "foci", foci.str().c_str());
      }
      draw_exit(C, op);
      const auto result = shape_exec(C, op);
      WM_toolsystem_ref_set_by_id(C, "builtin.select_box");
      return result;
    }
    else if (((data.shape == 2) ||
              (data.shape == 1 && data.shape_mode != 3 &&
               !(data.shape_mode == 1 && data.points.size() > 1)) ||
              (data.shape == 3 && data.square)) &&
             handleNumInput(C, &data.num, event))
    {
      /* Direct typing chooses the same stage-dependent default as radCAD. */
      data.numeric_mode = data.shape == 3 ? DrawNumericMode::Width :
                          data.shape == 1 && data.points.size() == 1 && data.shape_mode != 1 ?
                                            DrawNumericMode::Diameter :
                                            DrawNumericMode::Radius;
      data.numeric_label = data.shape == 3 ? "Width" : "Size";
      update_arc_area_status_text(C, op);
      ED_region_tag_redraw(data.region);
      return OPERATOR_RUNNING_MODAL;
    }
  }
  if (event->type == MOUSEMOVE || event->val == KM_PRESS) {
    update_mouse(C, op, event);
  }
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus draw_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  /* Include overlapping headers/toolbars when deciding which region owns input.
   * PASS_THROUGH alone lets regular UI handlers run; combining it with RUNNING_MODAL
   * stops dispatch before those handlers. The modal operator remains registered. */
  if (ED_area_find_region_xy_visual(CTX_wm_area(C), RGN_TYPE_ANY, event->xy) != data.region) {
    return OPERATOR_PASS_THROUGH;
  }
  data.numeric_applying = event->val == KM_PRESS && ELEM(event->type, EVT_RETKEY, EVT_PADENTER);
  if (CTX_data_edit_object(C) != &data.source->object()) {
    draw_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  const auto *rv3d = static_cast<const RegionView3D *>(data.region->regiondata);
  /* Keep wheel segment adjustment, while native navigation owns complete orbit,
   * pan and zoom gestures. Drawing inputs stay fixed until navigation finishes. */
  const bool segment_wheel = (data.arc_mode >= 0 || data.spline || data.shape) &&
                             ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE) &&
                             !(event->modifier & KM_CTRL) && !(rv3d->rflag & RV3D_NAVIGATING);
  if (!segment_wheel && ED_view3d_navigation_do(C, data.navigation, event, nullptr)) {
    return OPERATOR_RUNNING_MODAL;
  }
  if ((data.arc_mode >= 0 || data.spline || (data.shape && data.shape != 3)) &&
      ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE) && event->val == KM_PRESS &&
      !(event->modifier & KM_CTRL))
  {
    const int minimum = data.shape         ? (data.shape == 1 ? 4 : 3) :
                        data.circle        ? 4 :
                        data.arc_mode >= 0 ? 0 :
                                             1;
    RNA_int_set(op->ptr,
                "preview_points",
                std::clamp(RNA_int_get(op->ptr, "preview_points") +
                               (event->type == WHEELUPMOUSE ? 1 : -1) *
                                   (data.shape == 2 && data.shape_mode == 3 ? 2 : 1),
                           minimum,
                           256));
    rebuild_preview(op);
    update_arc_area_status_text(C, op);
    ED_region_tag_redraw(data.region);
    return OPERATOR_RUNNING_MODAL;
  }
  if (ELEM(event->type, MIDDLEMOUSE, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
    return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
  }
  if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
    if (data.numeric_mode != DrawNumericMode::Automatic || hasNumInput(&data.num)) {
      if (data.numeric_mode == DrawNumericMode::Segments) {
        RNA_int_set(op->ptr, "preview_points", data.numeric_initial_segments);
      }
      if (data.circle && data.arc_mode == 1 &&
          ELEM(data.numeric_mode, DrawNumericMode::Radius, DrawNumericMode::Diameter))
      {
        data.circle_input_radius = data.numeric_initial_circle_input_radius;
      }
      if (data.arc_mode >= 0) {
        arc_num_reset(C, data);
      }
      else {
        initNumInput(&data.num);
        data.num.unit_sys = CTX_data_scene(C)->unit.system;
        data.num.unit_type[0] = B_UNIT_LENGTH;
        data.num.flag |= NUM_USE_DOUBLE;
        data.numeric_mode = DrawNumericMode::Automatic;
        data.numeric_label.clear();
      }
      rebuild_preview(op);
      update_arc_area_status_text(C, op);
      ED_region_tag_redraw(data.region);
      return OPERATOR_RUNNING_MODAL;
    }
    draw_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  if (data.shape) {
    return shape_modal(C, op, event);
  }
  if (draw_numeric_shortcut(C, op, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == LEFTMOUSE && event->val == KM_PRESS &&
      (data.numeric_mode != DrawNumericMode::Automatic || hasNumInput(&data.num)))
  {
    return OPERATOR_RUNNING_MODAL;
  }
  if (data.circle) {
    return circle_modal(C, op, event);
  }
  if (data.arc_mode >= 0) {
    return arc_modal(C, op, event);
  }
  if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS &&
      data.numeric_mode != DrawNumericMode::Automatic && !hasNumInput(&data.num))
  {
    initNumInput(&data.num);
    data.num.unit_sys = CTX_data_scene(C)->unit.system;
    data.num.unit_type[0] = B_UNIT_LENGTH;
    data.num.flag |= NUM_USE_DOUBLE;
    data.numeric_mode = DrawNumericMode::Automatic;
    data.numeric_label.clear();
    update_arc_area_status_text(C, op);
    return OPERATOR_RUNNING_MODAL;
  }
  if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS &&
      data.numeric_mode == DrawNumericMode::Segments)
  {
    if (hasNumInput(&data.num)) {
      update_mouse(C, op, event);
    }
    initNumInput(&data.num);
    data.num.unit_sys = CTX_data_scene(C)->unit.system;
    data.num.unit_type[0] = B_UNIT_LENGTH;
    data.num.flag |= NUM_USE_DOUBLE;
    data.numeric_mode = DrawNumericMode::Automatic;
    data.numeric_label.clear();
    rebuild_preview(op);
    update_arc_area_status_text(C, op);
    return OPERATOR_RUNNING_MODAL;
  }
  if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS &&
      hasNumInput(&data.num))
  {
    update_mouse(C, op, event);
    if (math::distance(data.points.last(), data.current) > 1e-9 && data.points.size() < 256) {
      data.points.append(data.current);
      data.point_vertices.append(data.current_vertex);
    }
    initNumInput(&data.num);
    data.num.unit_sys = CTX_data_scene(C)->unit.system;
    data.num.unit_type[0] = B_UNIT_LENGTH;
    data.num.flag |= NUM_USE_DOUBLE;
    data.numeric_mode = DrawNumericMode::Automatic;
    data.numeric_label.clear();
    rebuild_preview(op);
    update_arc_area_status_text(C, op);
    return OPERATOR_RUNNING_MODAL;
  }
  if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER, RIGHTMOUSE) && event->val == KM_PRESS) {
    if (data.points.size() < 2) {
      draw_exit(C, op);
      WM_toolsystem_ref_set_by_id(C, "builtin.select_box");
      return OPERATOR_CANCELLED;
    }
    /* Preview coordinates are world-space. Store authoritative local input at full double
     * precision so redo does not round-trip native geometry through stroke floats. */
    const double4x4 inverse = data.inverse;
    std::ostringstream points;
    points << std::setprecision(17);
    const auto index = TopologyIndex::build(data.source->source().geometry());
    for (const int i : data.points.index_range()) {
      const auto *vertex = index ? index->vertex(data.point_vertices[i]) : nullptr;
      const double3 local = vertex ? double3(vertex->position) :
                                     math::transform_point(inverse, data.points[i]);
      points << local.x << ' ' << local.y << ' ' << local.z << '\n';
    }
    RNA_string_set(op->ptr, "points", points.str().c_str());
    const Vector<double3> local_points = read_points(op);
    if (!data.spline && local_points.size() > 1) {
      const double3 delta = local_points.last() - local_points[local_points.size() - 2];
      RNA_float_set(op->ptr,
                    "segment_length",
                    float(math::length(math::transform_direction(
                        double4x4(data.source->object().object_to_world()), delta))));
    }
    std::ostringstream references;
    for (const int64_t vertex : data.point_vertices) {
      references << vertex << ' ';
    }
    RNA_string_set(op->ptr, "vertex_references", references.str().c_str());
    draw_exit(C, op);
    const wmOperatorStatus status = draw_exec(C, op);
    WM_toolsystem_ref_set_by_id(C, "builtin.select_box");
    return status;
  }
  if (event->val == KM_PRESS && !data.points.is_empty() &&
      !(event->type == EVT_TABKEY && data.spline) && handleNumInput(C, &data.num, event))
  {
    update_mouse(C, op, event);
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->val == KM_PRESS) {
    if (ELEM(event->type, EVT_XKEY, EVT_YKEY, EVT_ZKEY)) {
      const int axis = event->type == EVT_XKEY ? 0 : event->type == EVT_YKEY ? 1 : 2;
      data.axis = data.axis == axis ? -1 : axis;
    }
    else if (event->type == EVT_NKEY) {
      data.axis = data.axis == 3 ? -1 : 3;
    }
    else if (event->type == EVT_LKEY) {
      data.locked = !data.locked;
    }
    else if (event->type == EVT_TABKEY && data.spline) {
      RNA_enum_set(op->ptr, "method", 1 - RNA_enum_get(op->ptr, "method"));
    }
    else if (event->type == EVT_BACKSPACEKEY) {
      if (data.points.size() > 1) {
        data.points.remove_last();
        data.point_vertices.remove_last();
      }
    }
    else if (event->type == LEFTMOUSE) {
      update_mouse(C, op, event);
      if (math::distance(data.points.last(), data.current) > 1e-9 && data.points.size() < 256) {
        data.points.append(data.current);
        data.point_vertices.append(data.current_vertex);
        data.axis = -1;
        data.shift_direction.reset();
        initNumInput(&data.num);
        data.num.unit_sys = CTX_data_scene(C)->unit.system;
        data.num.unit_type[0] = B_UNIT_LENGTH;
        data.num.flag |= NUM_USE_DOUBLE;
        data.numeric_mode = DrawNumericMode::Automatic;
        data.numeric_label.clear();
      }
    }
  }
  if (event->type == MOUSEMOVE || event->val == KM_PRESS) {
    update_mouse(C, op, event);
    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
}

static void draw_operator(wmOperatorType *ot, const bool spline)
{
  ot->name = spline ? "Draw NURBS Spline" : "Draw NURBS Line";
  ot->idname = spline ? "CURVE_OT_draw_spline" : "CURVE_OT_draw_line";
  ot->description = spline ?
                        "Place fit points or control vertices to draw a native NURBS spline" :
                        "Place successive points to draw connected native NURBS line segments";
  ot->poll = ed::nurbs::topology_edit_poll;
  ot->invoke = draw_invoke;
  ot->exec = draw_exec;
  ot->modal = draw_modal;
  ot->cancel = draw_exit;
  ot->flag = OPTYPE_UNDO;
  auto *prop = RNA_def_string(
      ot->srna, "points", nullptr, 0, "Points", "Drawing input in local coordinates");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_string(ot->srna,
                        "vertex_references",
                        nullptr,
                        0,
                        "Snapped Vertices",
                        "Explicit endpoint snap identities");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  static const EnumPropertyItem methods[] = {
      {0, "FIT_POINTS", 0, "Fit Points", "Curve passes through the placed points"},
      {1, "CONTROL_VERTICES", 0, "CVs", "Placed points form the NURBS control polygon"},
      {0, nullptr, 0, nullptr, nullptr}};
  prop = RNA_def_enum(ot->srna, "method", methods, 0, "Drawing Method", "");
  if (!spline) {
    RNA_def_property_flag(prop, PROP_HIDDEN);
  }
  prop = RNA_def_int(ot->srna,
                     "degree",
                     3,
                     1,
                     5,
                     "Degree",
                     "Spline degree, limited by the number of points",
                     1,
                     5);
  if (!spline) {
    RNA_def_property_flag(prop, PROP_HIDDEN);
  }
  else {
    RNA_def_int(ot->srna,
                "preview_points",
                32,
                1,
                256,
                "Segments",
                "Exact NURBS edge divisions used by the preview and committed spline",
                1,
                128);
  }
  if (!spline) {
    prop = RNA_def_float_distance(ot->srna,
                                  "segment_length",
                                  0,
                                  0,
                                  FLT_MAX,
                                  "Last Segment Length",
                                  "Adjust the final line segment along its drawn direction",
                                  0,
                                  100);
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  }
  RNA_def_boolean(
      ot->srna, "use_axis_snap", true, "Axis Inference", "Align near-axis segments to world axes");
}
static void arc_operator(wmOperatorType *ot, const bool circle = false)
{
  ot->name = circle ? "Draw NURBS Circle" : "Draw NURBS Arc";
  ot->idname = circle ? "CURVE_OT_draw_circle" : "CURVE_OT_draw_arc";
  ot->description =
      "Draw an exact circular NURBS arc by center and sweep, endpoints and bulge, or three points";
  ot->poll = ed::nurbs::topology_edit_poll;
  ot->invoke = draw_invoke;
  ot->exec = draw_exec;
  ot->modal = draw_modal;
  ot->cancel = draw_exit;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  static const EnumPropertyItem modes[] = {
      {0, "CENTER", 0, "Center", "Center, start point, then signed sweep"},
      {1, "ENDPOINTS", 0, "2 Point", "Start and end points, then bulge height"},
      {2, "THREE_POINTS", 0, "3 Point", "Start and end points, then a point on the arc"},
      {0, nullptr, 0, nullptr, nullptr}};
  static const EnumPropertyItem circle_modes[] = {
      {0, "CENTER", 0, "Center", "Center and radius point"},
      {1, "DIAMETER", 0, "2 Point", "Opposite ends of the diameter"},
      {2, "THREE_POINTS", 0, "3 Point", "Three points on the circle"},
      {3,
       "TAN_TAN",
       0,
       "Tangent to 2 Curves",
       "Select two separate curve chains, then move and confirm the tangent circle"},
      {4,
       "TAN_TAN_TAN",
       0,
       "Tangent to 3 Curves",
       "Select three separate coplanar curve chains; Tab cycles tangent solutions"},
      {0, nullptr, 0, nullptr, nullptr}};
  RNA_def_enum(ot->srna, "mode", circle ? circle_modes : modes, 0, "Construction", "");
  if (circle) {
    ot->description =
        "Draw an exact NURBS circle by radius, diameter, three points or selected tangent curves";
    RNA_def_boolean(ot->srna,
                    "show_source_curves",
                    true,
                    "Source Curves",
                    "Show the selected native curves in cyan");
    RNA_def_boolean(ot->srna,
                    "show_tangents",
                    true,
                    "Tangency Guides",
                    "Show yellow center to contact radius guides");
    RNA_def_boolean(ot->srna,
                    "anchor_tangent_markers",
                    false,
                    "Tangent Markers",
                    "Include exact tangency contacts as circle endpoints; T toggles this");
    auto *contact_prop = RNA_def_string(ot->srna,
                                        "tangent_parameters",
                                        nullptr,
                                        0,
                                        "Tangent Parameters",
                                        "Source parameters of tangent endpoints for replay");
    RNA_def_property_flag(contact_prop, PROP_HIDDEN | PROP_SKIP_SAVE);
    static const float cyan[4] = {0, 0.8f, 1, 0.5f};
    RNA_def_float_color(ot->srna, "tangent_color", 4, cyan, 0, 1, "Curve Color", "", 0, 1);
    RNA_def_float(ot->srna, "source_width", 2, 1, 10, "Curve Width", "", 1, 5);
  }
  auto *prop = RNA_def_string(
      ot->srna, "points", nullptr, 0, "Points", "Construction points in local coordinates");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_string(ot->srna,
                        "vertex_references",
                        nullptr,
                        0,
                        "Snapped Vertices",
                        "Explicit endpoint snap identities");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  static const float normal[3] = {0, 0, 1};
  prop = RNA_def_float_vector(ot->srna,
                              "plane_normal",
                              3,
                              normal,
                              -1,
                              1,
                              "Plane Normal",
                              "World-space drawing plane normal",
                              -1,
                              1);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_float_rotation(ot->srna,
                                "sweep",
                                0,
                                nullptr,
                                -2 * M_PI,
                                2 * M_PI,
                                "Sweep",
                                "Signed center arc sweep",
                                -2 * M_PI,
                                2 * M_PI);
  prop = RNA_def_string(ot->srna,
                        "sweep_exact",
                        nullptr,
                        0,
                        "Exact Sweep",
                        "Full precision interactive sweep for replay");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_boolean(
      ot->srna, "use_axis_snap", true, "Axis Inference", "Align near-axis chords to world axes");
  RNA_def_boolean(ot->srna,
                  "use_angle_snap",
                  true,
                  "Angle Snapping",
                  "Snap near angle increments and semicircular bulges");
  prop = RNA_def_float_rotation(ot->srna,
                                "angle_increment",
                                0,
                                nullptr,
                                M_PI / 180,
                                M_PI,
                                "Angle Increment",
                                "Soft angular snapping increment",
                                M_PI / 180,
                                M_PI / 2);
  RNA_def_property_float_default(prop, M_PI / 12);
  RNA_def_boolean(ot->srna,
                  "show_compass",
                  true,
                  "Compass",
                  "Show the rotating drawing-plane compass for the Center arc");
  RNA_def_int(ot->srna,
              "compass_size",
              125,
              50,
              500,
              "Compass Size",
              "Screen diameter of the compass in pixels",
              50,
              500);
  RNA_def_boolean(ot->srna,
                  "use_axis_colors",
                  true,
                  "Axis Colors",
                  "Color the compass by its plane normal and guides by X, Y or Z alignment");
  RNA_def_float(ot->srna, "axis_color_dim", 1, 0, 1, "Axis Color Brightness", "", 0, 1);
  static const float overlay_color[4] = {0.1f, 0.1f, 0.1f, 1};
  static const float radius_color[4] = {0.2f, 0.8f, 0.2f, 1};
  RNA_def_float_color(ot->srna,
                      "overlay_color",
                      4,
                      overlay_color,
                      0,
                      1,
                      "Guide Color",
                      "Guide color when not aligned to an axis",
                      0,
                      1);
  RNA_def_float_color(ot->srna,
                      "start_color",
                      4,
                      radius_color,
                      0,
                      1,
                      "Start Radius",
                      "Center arc start radius color",
                      0,
                      1);
  RNA_def_float_color(ot->srna,
                      "end_color",
                      4,
                      radius_color,
                      0,
                      1,
                      "End Radius",
                      "Center arc end radius color",
                      0,
                      1);
  RNA_def_int(ot->srna,
              "preview_points",
              32,
              circle ? 4 : 0,
              256,
              "Segments",
              circle ? "Exact circular edges and shared endpoints; matches the preview divisions" :
                       "Arc edges and shared endpoints; zero uses 90 degree divisions",
              circle ? 4 : 0,
              128);
  RNA_def_int(ot->srna, "preview_point_size", 5, 1, 50, "Marker Size", "", 1, 20);
  RNA_def_boolean(ot->srna,
                  "show_measurements",
                  true,
                  "Measurements",
                  "Show construction stage and live measurements beside the pointer");
}
static void shape_operator(wmOperatorType *ot, const int shape)
{
  arc_operator(ot);
  ot->name = shape == 1 ? "Draw NURBS Ellipse" :
             shape == 2 ? "Draw NURBS Polygon" :
                          "Draw NURBS Rectangle";
  ot->idname = shape == 1 ? "CURVE_OT_draw_ellipse" :
               shape == 2 ? "CURVE_OT_draw_polygon" :
                            "CURVE_OT_draw_rectangle";
  ot->description = "Draw native NURBS geometry with radCAD construction modes";
  ot->exec = shape_exec;
  static const EnumPropertyItem ellipse_modes[] = {
      {0, "RADIUS", 0, "Radius", "Center, major radius, minor radius"},
      {1, "FOCI", 0, "Foci", "Two foci and a point on the ellipse; K keeps the foci"},
      {2, "ENDPOINTS", 0, "Endpoints", "Major axis endpoints, then minor radius"},
      {3, "CORNERS", 0, "Corners", "Opposite corners of the bounding rectangle"},
      {0, nullptr, 0, nullptr, nullptr}};
  static const EnumPropertyItem polygon_modes[] = {
      {0, "CENTER_CORNER", 0, "Center–Corner", "Center and a vertex"},
      {1, "CENTER_TANGENT", 0, "Center–Tangent", "Center and the midpoint of a side"},
      {2, "CORNER_CORNER", 0, "Corner–Corner", "Two adjacent vertices"},
      {3, "EDGE", 0, "Edge", "Vertex and opposite side midpoint; odd side count"},
      {0, nullptr, 0, nullptr, nullptr}};
  static const EnumPropertyItem rectangle_modes[] = {
      {0, "CENTER_CORNER", 0, "Center–Corner", "Center and a corner"},
      {1, "CORNER_CORNER", 0, "Corner–Corner", "Opposite corners"},
      {2, "THREE_POINTS", 0, "3 Point", "Two adjacent corners, then signed height"},
      {0, nullptr, 0, nullptr, nullptr}};
  auto *prop = RNA_struct_type_find_property(ot->srna, "mode");
  RNA_def_property_enum_items(prop,
                              shape == 1 ? ellipse_modes :
                              shape == 2 ? polygon_modes :
                                           rectangle_modes);
  prop = RNA_struct_type_find_property(ot->srna, "preview_points");
  RNA_def_property_int_default(prop, shape == 1 ? 32 : shape == 2 ? 6 : 4);
  RNA_def_property_range(prop, shape == 1 ? 4 : 3, 256);
  RNA_def_property_ui_text(
      prop, shape == 2 ? "Sides" : "Segments", "Preview and committed endpoints");
  if (shape == 3) {
    RNA_def_property_flag(prop, PROP_HIDDEN);
  }
  RNA_def_property_boolean_default(RNA_struct_type_find_property(ot->srna, "show_compass"), false);
  RNA_def_boolean(
      ot->srna, "keep_foci", false, "Keep Foci", "Keep the two ellipse foci as native vertices");
  prop = RNA_def_string(ot->srna, "foci", nullptr, 0, "Foci", "Local focus positions");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}
}  // namespace

void VIEW3D_GT_nurbs_draw_snap(wmGizmoType *type)
{
  type->idname = "VIEW3D_GT_nurbs_draw_snap";
  type->struct_size = sizeof(DrawSnapGizmo);
  type->setup = draw_snap_gizmo_setup;
  type->free = draw_snap_gizmo_free;
  type->draw = draw_snap_hover;
  type->test_select = draw_snap_hover_test_select;
}

void VIEW3D_GGT_nurbs_draw_snap(wmGizmoGroupType *type)
{
  type->idname = "VIEW3D_GGT_nurbs_draw_snap";
  type->name = "NURBS Drawing Snap Preview";
  type->flag = WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP;
  type->gzmap_params.spaceid = SPACE_VIEW3D;
  type->gzmap_params.regionid = RGN_TYPE_WINDOW;
  type->poll = ED_gizmo_poll_or_unlink_delayed_from_tool;
  type->setup = draw_snap_hover_setup;
}

void CURVE_OT_draw_line(wmOperatorType *ot)
{
  draw_operator(ot, false);
}
void CURVE_OT_draw_arc(wmOperatorType *ot)
{
  arc_operator(ot);
}
void CURVE_OT_draw_circle(wmOperatorType *ot)
{
  arc_operator(ot, true);
}
void CURVE_OT_draw_ellipse(wmOperatorType *ot)
{
  shape_operator(ot, 1);
}
void CURVE_OT_draw_polygon(wmOperatorType *ot)
{
  shape_operator(ot, 2);
}
void CURVE_OT_draw_rectangle(wmOperatorType *ot)
{
  shape_operator(ot, 3);
}
void CURVE_OT_draw_spline(wmOperatorType *ot)
{
  draw_operator(ot, true);
}
}  // namespace blender
