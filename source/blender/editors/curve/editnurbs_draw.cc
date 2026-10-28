/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 * Click drawing inspired by radCAD's polyline and interpolation tools.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "BKE_context.hh"
#include "BKE_layer.hh"
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

struct DrawData {
  std::unique_ptr<ed::nurbs::TopologyPreview> source;
  ARegion *region = nullptr;
  void *draw_handle = nullptr;
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
  NumInput num{};
  double4x4 inverse;
};

static bool is_spline(const wmOperator *op)
{
  return STREQ(op->type->idname, "CURVE_OT_draw_spline");
}

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
    parameters = {0, 1};
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
    const int point_index = is_spline(op) && i == 1 ? points.size() - 1 : i;
    const NurbsTopologyVertex *anchor = references.size() == points.size() && index ?
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

static void rebuild_preview(wmOperator *op)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  data.wire.clear();
  Vector<double3> points = data.points;
  if (points.is_empty() || math::distance(points.last(), data.current) > 1e-9) {
    points.append(data.current);
  }
  if (points.size() < 2) {
    return;
  }
  std::string error;
  for (double3 &point : points) {
    point = math::transform_point(data.inverse, point);
  }
  auto support = draw_support(points,
                              data.spline,
                              RNA_enum_get(op->ptr, "method") == 1,
                              RNA_int_get(op->ptr, "degree"),
                              error);
  if (!support) {
    return;
  }
  const int samples = std::max<int>(2, (points.size() - 1) * 16);
  for (int i = 0; i <= samples; i++) {
    if (auto point = evaluate_curve(*support, double(i) / samples)) {
      data.wire.append(float3(math::transform_point(
          double4x4(data.source->object().object_to_world()), point->position)));
    }
  }
}

static void draw_preview(const bContext *, ARegion *region, void *arg)
{
  auto &data = *static_cast<DrawData *>(static_cast<wmOperator *>(arg)->customdata);
  GPU_blend(GPU_BLEND_ALPHA);
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
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);

  /* Use Blender's standard snap glyphs so the preview gives the same visual feedback as
   * viewport mesh snapping. The draw utility expects view matrices to already be active, as
   * they are for this REGION_DRAW_POST_VIEW callback. */
  if (data.snap_type != SCE_SNAP_TO_NONE) {
    const uchar snap_color[4] = {255, 255, 255, 255};
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
  ed::transform::snap_object_context_destroy(data->snap);
  ED_region_tag_redraw(data->region);
  ED_workspace_status_text(C, nullptr);
  MEM_delete(data);
  op->customdata = nullptr;
}

static void update_mouse(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  auto *v3d = CTX_wm_view3d(C);
  const Scene *scene = CTX_data_scene(C);
  const ToolSettings *tool_settings = scene->toolsettings;
  const eSnapMode snap_mode = eSnapMode(tool_settings->snap_mode);
  const bool use_snap = bool(tool_settings->snap_flag & SCE_SNAP) !=
                        bool(event->modifier & KM_CTRL);
  float3 origin, direction, target;
  ED_view3d_win_to_ray(data.region, float2(event->mval), origin, direction);
  const float denominator = math::dot(direction, data.plane_normal);
  if (std::abs(denominator) > 1e-8f) {
    target = origin +
             direction * (math::dot(data.plane_origin - origin, data.plane_normal) / denominator);
  }
  else {
    ED_view3d_win_to_3d(v3d, data.region, data.plane_origin, float2(event->mval), target);
  }
  bool snapped = false;
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
    float center_distance = snap_result != SCE_SNAP_TO_NONE ? snap_distance : 12.0f * U.pixelsize;
    float center_depth = snap_result != SCE_SNAP_TO_NONE ? math::dot(target - origin, direction) :
                                                           FLT_MAX;
    if (data.axis < 0 && !(event->modifier & KM_SHIFT)) {
      for (const NativeCenter &center : data.centers) {
        if (!(snap_mode & center.type) ||
            !center_target_allowed(
                C, *center.object, data.source->object(), snap_params.snap_target_select))
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
  if (!data.points.is_empty()) {
    const float3 anchor(data.points.last());
    if (!(event->modifier & KM_SHIFT)) {
      data.shift_direction.reset();
    }
    else if (!data.shift_direction && math::length_squared(target - anchor) > 1e-12f) {
      data.shift_direction = math::normalize(target - anchor);
    }
    if (data.axis >= 0 || data.shift_direction) {
      float3 axis = data.shift_direction.value_or(float3(0));
      if (data.axis == 3) {
        axis = data.face_normal;
      }
      else if (data.axis >= 0) {
        axis = float3(0);
        axis[data.axis] = 1;
      }
      const float projection = math::dot(direction, axis);
      const float denominator_axis = 1 - projection * projection;
      float distance = math::dot(target - anchor, axis);
      if (!snapped && denominator_axis > 1e-8f) {
        distance = (math::dot(origin - anchor, axis) -
                    projection * math::dot(origin - anchor, direction)) /
                   denominator_axis;
      }
      target = anchor + axis * distance;
    }
    else if (!snapped && RNA_boolean_get(op->ptr, "use_axis_snap")) {
      const float3 delta = target - anchor;
      const float length = math::length(delta);
      for (int axis = 0; axis < 3 && length > 0; axis++) {
        if (std::abs(delta[axis]) / length > std::cos(6.0f * float(M_PI) / 180.0f)) {
          target = anchor;
          target[axis] += delta[axis];
          break;
        }
      }
    }
  }
  data.current = native_center_position.value_or(double3(target));
  data.current_vertex = 0;
  if (!data.points.is_empty() && (data.axis >= 0 || data.shift_direction)) {
    double3 axis = double3(data.shift_direction.value_or(float3(0)));
    if (data.axis == 3) {
      axis = double3(data.face_normal);
    }
    else if (data.axis >= 0) {
      axis = double3(0);
      axis[data.axis] = 1;
    }
    data.current = data.points.last() + axis * math::dot(data.current - data.points.last(), axis);
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
        can_snap_active_object && data.axis < 0 && !data.shift_direction)
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
          if (edge_distance < best) {
            best = edge_distance;
            data.current = math::transform_point(transform, evaluate_curve(*curve, t)->position);
            data.snap_type = SCE_SNAP_TO_EDGE;
            snap_location = float3(data.current);
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
          if (distance < best && data.axis < 0 && !data.shift_direction) {
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
            if (distance <= best + 1e-4f && data.axis < 0 && !data.shift_direction) {
              best = distance;
              data.current = world;
              data.current_vertex = vertex.id;
              data.snap_type = SCE_SNAP_TO_POINT;
              snap_location = float3(world);
            }
          }
        }
      }
    }
  }
  if (!data.points.is_empty() && hasNumInput(&data.num)) {
    data.current_vertex = 0;
    const double3 delta = data.current - data.points.last();
    double length = math::length(delta);
    applyNumInput(&data.num, &length);
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
  if (data.snap_type != SCE_SNAP_TO_NONE) {
    const double snap_tolerance = 1e-6 * std::max(1.0, double(math::length(snap_location)));
    if (math::distance(data.current, double3(snap_location)) > snap_tolerance) {
      /* Axis constraints and numeric input can move the preview off the snap target. Don't show
       * a snap marker in that case. */
      data.snap_type = SCE_SNAP_TO_NONE;
    }
  }
  rebuild_preview(op);
  ED_region_tag_redraw(data.region);
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
  data->plane_normal = math::normalize(float3(rv3d->viewinv[2]));
  data->snap = ed::transform::snap_object_context_create();
  op->customdata = data;
  data->draw_handle = ED_region_draw_cb_activate(
      data->region->runtime->type, draw_preview, op, REGION_DRAW_POST_VIEW);
  update_mouse(C, op, event);
  data->points.append(data->current);
  data->point_vertices.append(data->current_vertex);
  data->plane_origin = float3(data->current);
  ED_workspace_status_text(
      C,
      "Click: Add point | Enter / Right Click: Finish | Esc: Cancel | "
      "Backspace: Remove point | X/Y/Z: Axis | N: Normal | Shift: Direction | L: Plane lock | "
      "Tab: Fit Points / CVs | Ctrl: Toggle snap");
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus draw_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<DrawData *>(op->customdata);
  if (CTX_data_edit_object(C) != &data.source->object()) {
    draw_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  if (ELEM(event->type, MIDDLEMOUSE, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
    return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
  }
  if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
    draw_exit(C, op);
    return OPERATOR_CANCELLED;
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
    rebuild_preview(op);
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
  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING;
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
}  // namespace

void CURVE_OT_draw_line(wmOperatorType *ot)
{
  draw_operator(ot, false);
}
void CURVE_OT_draw_spline(wmOperatorType *ot)
{
  draw_operator(ot, true);
}
}  // namespace blender
