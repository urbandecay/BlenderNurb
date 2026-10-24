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
#include "BLI_math_matrix.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"

#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

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

bool select_components(Main &bmain,
                       Curve &curve,
                       const bke::nurbs::ReadSnapshot &expected,
                       const Span<bke::nurbs::ElementRef> hits,
                       const bke::nurbs::SelectionAction action,
                       const std::optional<bke::nurbs::ElementRef> active,
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
  auto selection = bke::nurbs::selection_apply(
      expected.geometry(), expected.selection(), hits, action, active, r_error);
  if (!selection) {
    return false;
  }
  const auto &previous = expected.selection();
  if (selection->mode == previous.mode && selection->vertices == previous.vertices &&
      selection->edges == previous.edges && selection->faces == previous.faces &&
      selection->active == previous.active &&
      selection->history.as_span() == previous.history.as_span())
  {
    return false;
  }
  auto candidate = expected.copy_for_edit();
  candidate.selection = std::move(*selection);
  return edit_commit(bmain, curve, std::move(candidate), expected, r_error);
}

bool select_vertices(Main &bmain,
                     Curve &curve,
                     const bke::nurbs::ReadSnapshot &expected,
                     const Span<int64_t> hits,
                     const bke::nurbs::SelectionAction action,
                     const std::optional<int64_t> active_vertex,
                     std::string *r_error)
{
  Vector<bke::nurbs::ElementRef> elements;
  for (const int64_t id : hits) {
    elements.append({bke::nurbs::ElementDomain::Vertex, id});
  }
  return select_components(bmain,
                           curve,
                           expected,
                           elements,
                           action,
                           active_vertex ?
                               std::optional(bke::nurbs::ElementRef{
                                   bke::nurbs::ElementDomain::Vertex, *active_vertex}) :
                               std::nullopt,
                           r_error);
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

struct SelectionTarget {
  Object *object;
  bke::nurbs::ReadSnapshot source;
};

/** Pixel/depth proxies only. Source positions and coefficients are never written from these. */
static std::optional<float3> project_position(const ViewContext &vc,
                                              const double3 &position,
                                              std::string &error)
{
  for (int axis = 0; axis < 3; axis++) {
    const double value = position[axis];
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
      error = "NURBS source position exceeds the viewport coordinate range";
      return std::nullopt;
    }
  }
  const float3 co(position);
  float2 screen;
  if (ED_view3d_project_float_object(
          vc.region, co, screen, V3D_PROJ_TEST_CLIP_NEAR | V3D_PROJ_TEST_CLIP_BB) !=
      V3D_PROJ_RET_OK)
  {
    return std::nullopt;
  }
  float4 clip;
  mul_v4_m4v3(clip, vc.rv3d->persmatob, co);
  if (clip.w == 0 || !std::isfinite(clip.w) || !std::isfinite(screen.x) ||
      !std::isfinite(screen.y))
  {
    return std::nullopt;
  }
  if (std::abs(screen.x) > 1.0e9f || std::abs(screen.y) > 1.0e9f) {
    error = "NURBS picking exceeds finite screen-coordinate arithmetic";
    return std::nullopt;
  }
  const float depth = (clip.z / clip.w + 1.0f) * 0.5f;
  if (!std::isfinite(depth)) {
    return std::nullopt;
  }
  return float3(screen.x, screen.y, depth);
}

/** Use the viewport's scene depth, including occluders outside the native edit owners. */
struct SelectionDepth {
  ViewDepths *depths = nullptr;
  bool required = false;

  explicit SelectionDepth(const ViewContext &vc)
  {
    required = !XRAY_FLAG_ENABLED(vc.v3d);
    if (required) {
      ED_view3d_depth_override(
          vc.depsgraph, vc.region, vc.v3d, vc.obact, V3D_DEPTH_ALL, false, &depths);
    }
  }
  ~SelectionDepth()
  {
    if (depths != nullptr) {
      ED_view3d_depths_free(depths);
    }
  }
  bool visible(const float3 &point) const
  {
    if (depths == nullptr) {
      return true;
    }
    const int pixel[2] = {int(std::floor(point.x)), int(std::floor(point.y))};
    if (pixel[0] < 0 || pixel[1] < 0 || pixel[0] >= depths->w || pixel[1] >= depths->h) {
      return false;
    }
    float scene_depth;
    // UI depth-buffer quantization slack. This is never a source geometry tolerance.
    return !ED_view3d_depth_read_cached(depths, pixel, 0, &scene_depth) ||
           point.z <= scene_depth + 2.0e-5f;
  }
};

static std::shared_ptr<const bke::nurbs::ObjectEvaluation> selection_evaluation(
    const ViewContext &vc, const SelectionTarget &target, std::string &error)
{
  const Object *evaluated = DEG_get_evaluated(vc.depsgraph, target.object);
  const auto result = evaluated->runtime->nurbs_evaluation;
  if (result == nullptr || !result->result) {
    error = result != nullptr ? result->error :
                                "NURBS component picking has no evaluated source preview";
    return nullptr;
  }
  if (result->result->source != target.source) {
    error = "NURBS component picking preview refers to a stale source; redraw and try again";
    return nullptr;
  }
  return result;
}

static bool triangle_depth(
    const float2 &point, const float3 &a, const float3 &b, const float3 &c, float &r_depth)
{
  const float2 ab = float2(b.x - a.x, b.y - a.y), ac = float2(c.x - a.x, c.y - a.y);
  const float2 ap = point - float2(a.x, a.y);
  const float determinant = ab.x * ac.y - ab.y * ac.x;
  if (determinant == 0) {
    return false;
  }
  const float u = (ap.x * ac.y - ap.y * ac.x) / determinant;
  const float v = (ab.x * ap.y - ab.y * ap.x) / determinant;
  if (u < 0 || v < 0 || u + v > 1) {
    return false;
  }
  r_depth = a.z + u * (b.z - a.z) + v * (c.z - a.z);
  return true;
}

/** Canonical samples retain the source edge ID; subdivisions are never selectable edges. */
static void project_edge_segments(const ViewContext &vc,
                                  const bke::nurbs::SurfacePreview &preview,
                                  FunctionRef<void(int64_t, float3, float3)> callback,
                                  std::string &error)
{
  for (const auto &edge : preview.boundary.edges) {
    std::optional<float3> previous;
    for (const auto &sample : edge.samples) {
      const auto point = project_position(
          vc, preview.boundary.positions[sample.point_index], error);
      if (previous && point) {
        callback(edge.edge_id, *previous, *point);
      }
      previous = point;
    }
  }
}

static void project_face_triangles(const ViewContext &vc,
                                   const bke::nurbs::SurfacePreview &preview,
                                   FunctionRef<void(int64_t, float3, float3, float3)> callback,
                                   std::string &error)
{
  for (const auto &triangle : preview.triangles) {
    const auto a = project_position(vc, preview.position(triangle.points.x), error);
    const auto b = project_position(vc, preview.position(triangle.points.y), error);
    const auto c = project_position(vc, preview.position(triangle.points.z), error);
    if (a && b && c) {
      callback(triangle.face_id, *a, *b, *c);
    }
  }
}

/** Retain every source lease before resolving a click/linked hit. */
static bool find_component(const ViewContext &vc,
                           const int mval[2],
                           const int distance,
                           Vector<SelectionTarget> &targets,
                           Object *&hit_object,
                           bke::nurbs::ElementRef &hit,
                           std::string &error)
{
  using namespace bke::nurbs;
  SelectionDepth depth(vc);
  if (depth.required && depth.depths == nullptr) {
    error = "NURBS component selection could not acquire viewport scene depth";
    return false;
  }
  int closest_domain = 3;
  double closest = double(distance) * distance;
  float closest_depth = std::numeric_limits<float>::infinity();
  auto consider = [&](Object *object,
                      const ElementRef element,
                      const float3 &screen,
                      const double distance_squared) {
    if (distance_squared > double(distance) * distance || !depth.visible(screen)) {
      return;
    }
    const int domain = int(element.domain);
    if (domain < closest_domain ||
        (domain == closest_domain && (distance_squared < closest ||
                                      (distance_squared == closest && screen.z < closest_depth))))
    {
      closest_domain = domain;
      closest = distance_squared;
      closest_depth = screen.z;
      hit_object = object;
      hit = element;
    }
  };
  for (Object *object : BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
           *vc.bmain, vc.scene, vc.view_layer, vc.v3d))
  {
    Curve &curve = *id_cast<Curve *>(object->data);
    if (curve.editnurb->nurbs_edit == nullptr || !curve_validate_source(curve, &error)) {
      if (error.empty()) {
        error = "Mixed native and legacy NURBS selection is unsupported";
      }
      return false;
    }
    targets.append({object, curve.editnurb->nurbs_edit->read_snapshot()});
    const auto &target = targets.last();
    const auto mode = target.source.selection().mode;
    ED_view3d_init_mats_rv3d(object, vc.rv3d);
    ED_view3d_clipping_local(vc.rv3d, object->object_to_world().ptr());
    if (flag_is_set(mode, SelectionMode::Vertex)) {
      for (const NurbsTopologyVertex &vertex : target.source.geometry().vertices) {
        if (const auto point = project_position(vc, double3(vertex.position), error)) {
          const float2 delta = float2(point->x - mval[0], point->y - mval[1]);
          consider(
              object, {ElementDomain::Vertex, vertex.id}, *point, math::length_squared(delta));
        }
      }
    }
    if (flag_is_set(mode, SelectionMode::Edge) || flag_is_set(mode, SelectionMode::Face)) {
      const auto evaluated = selection_evaluation(vc, target, error);
      if (!evaluated) {
        return false;
      }
      const auto &preview = evaluated->result->preview;
      if (flag_is_set(mode, SelectionMode::Edge)) {
        project_edge_segments(
            vc,
            preview,
            [&](int64_t id, const float3 a, const float3 b) {
              const float2 delta(b.x - a.x, b.y - a.y);
              const float length_squared = math::length_squared(delta);
              const float factor = length_squared == 0 ?
                                       0 :
                                       std::clamp(
                                           math::dot(float2(mval[0] - a.x, mval[1] - a.y), delta) /
                                               length_squared,
                                           0.0f,
                                           1.0f);
              const float3 point = a + factor * (b - a);
              consider(object,
                       {ElementDomain::Edge, id},
                       point,
                       math::length_squared(float2(point.x - mval[0], point.y - mval[1])));
            },
            error);
      }
      if (flag_is_set(mode, SelectionMode::Face)) {
        project_face_triangles(
            vc,
            preview,
            [&](int64_t id, const float3 a, const float3 b, const float3 c) {
              float value;
              if (triangle_depth(float2(mval[0], mval[1]), a, b, c, value)) {
                consider(object, {ElementDomain::Face, id}, float3(mval[0], mval[1], value), 0);
              }
            },
            error);
      }
    }
    if (!error.empty()) {
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
  using namespace bke::nurbs;
  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_ensure_evaluated_depsgraph(C));
  Vector<SelectionTarget> targets;
  Object *hit_object = nullptr;
  ElementRef hit{ElementDomain::Vertex, 0};
  std::string error;
  if (!find_component(vc, mval, distance, targets, hit_object, hit, error)) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
    return false;
  }
  if (hit_object == nullptr && !(params.sel_op == SEL_OP_SET && params.deselect_all)) {
    return false;
  }
  if (hit_object != nullptr && params.sel_op == SEL_OP_SET && params.select_passthrough) {
    for (const SelectionTarget &target : targets) {
      if (target.object == hit_object && selection_contains(target.source.selection(), hit)) {
        return false;
      }
    }
  }
  bool changed = false;
  for (const SelectionTarget &target : targets) {
    const bool is_hit = target.object == hit_object;
    if (params.sel_op != SEL_OP_SET && !is_hit) {
      continue;
    }
    changed |= select_components(*vc.bmain,
                                 *id_cast<Curve *>(target.object->data),
                                 target.source,
                                 is_hit ? Span<ElementRef>(&hit, 1) : Span<ElementRef>(),
                                 selection_action(params.sel_op),
                                 is_hit ? std::optional(hit) : std::nullopt,
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

bool select_box(const ViewContext &vc, const rcti &rect, const eSelectOp operation)
{
  using namespace bke::nurbs;
  Curve &curve = *id_cast<Curve *>(vc.obedit->data);
  const SelectionTarget target{vc.obedit, curve.editnurb->nurbs_edit->read_snapshot()};
  const auto mode = target.source.selection().mode;
  SelectionDepth depth(vc);
  if (depth.required && depth.depths == nullptr) {
    WM_global_report(RPT_ERROR,
                     "NURBS component selection could not acquire viewport scene depth");
    return false;
  }
  ED_view3d_init_mats_rv3d(vc.obedit, vc.rv3d);
  ED_view3d_clipping_local(vc.rv3d, vc.obedit->object_to_world().ptr());
  Vector<ElementRef> hits;
  std::string error;
  const rctf box{float(rect.xmin), float(rect.xmax), float(rect.ymin), float(rect.ymax)};
  auto inside = [&](const float3 &point) { return BLI_rctf_isect_pt(&box, point.x, point.y); };
  if (flag_is_set(mode, SelectionMode::Vertex)) {
    for (const NurbsTopologyVertex &vertex : target.source.geometry().vertices) {
      if (const auto point = project_position(vc, double3(vertex.position), error)) {
        if (inside(*point) && depth.visible(*point)) {
          hits.append({ElementDomain::Vertex, vertex.id});
        }
      }
    }
  }
  if (flag_is_set(mode, SelectionMode::Edge) || flag_is_set(mode, SelectionMode::Face)) {
    const auto evaluated = selection_evaluation(vc, target, error);
    if (!evaluated) {
      WM_global_report(RPT_ERROR, error.c_str());
      return false;
    }
    const auto &preview = evaluated->result->preview;
    if (flag_is_set(mode, SelectionMode::Edge)) {
      Set<int64_t> intersecting, not_fully_inside;
      project_edge_segments(
          vc,
          preview,
          [&](int64_t id, const float3 a, const float3 b) {
            if (!inside(a) || !inside(b)) {
              not_fully_inside.add(id);
            }
            if (!BLI_rctf_isect_segment(&box, a, b)) {
              return;
            }
            // Sample the clipped screen segment at pixel spacing for visibility. No source
            // fitting.
            float low = 0, high = 1;
            for (int axis = 0; axis < 2; axis++) {
              const float delta = b[axis] - a[axis];
              const float minimum = axis == 0 ? box.xmin : box.ymin;
              const float maximum = axis == 0 ? box.xmax : box.ymax;
              if (delta != 0) {
                const float first = (minimum - a[axis]) / delta;
                const float last = (maximum - a[axis]) / delta;
                low = std::max(low, std::min(first, last));
                high = std::min(high, std::max(first, last));
              }
            }
            if (low > high) {
              return;
            }
            const float3 start = a + low * (b - a), end = a + high * (b - a);
            const int steps = std::max(
                1, int(std::ceil(std::max(std::abs(end.x - start.x), std::abs(end.y - start.y)))));
            for (int i = 0; i <= steps; i++) {
              if (depth.visible(start + (float(i) / steps) * (end - start))) {
                intersecting.add(id);
                break;
              }
            }
          },
          error);
      bool fully_inside = false;
      for (const int64_t id : intersecting) {
        fully_inside |= !not_fully_inside.contains(id);
      }
      for (const auto &edge : preview.boundary.edges) {
        if (intersecting.contains(edge.edge_id) &&
            (!fully_inside || !not_fully_inside.contains(edge.edge_id)))
        {
          hits.append({ElementDomain::Edge, edge.edge_id});
        }
      }
    }
    if (flag_is_set(mode, SelectionMode::Face)) {
      if (depth.depths == nullptr) {
        const auto centers = face_display_centers(target.source.geometry(), preview);
        for (const NurbsTopologyFace &face : target.source.geometry().faces) {
          if (const auto *center = centers.lookup_ptr(face.id)) {
            if (const auto point = project_position(vc, *center, error)) {
              if (inside(*point)) {
                hits.append({ElementDomain::Face, face.id});
              }
            }
          }
        }
      }
      else {
        Set<int64_t> visible_faces;
        int probes = 0;
        project_face_triangles(
            vc,
            preview,
            [&](int64_t id, const float3 a, const float3 b, const float3 c) {
              if (visible_faces.contains(id) || !error.empty()) {
                return;
              }
              const int xmin = std::max(rect.xmin, int(std::floor(std::min({a.x, b.x, c.x}))));
              const int xmax = std::min(rect.xmax, int(std::ceil(std::max({a.x, b.x, c.x}))));
              const int ymin = std::max(rect.ymin, int(std::floor(std::min({a.y, b.y, c.y}))));
              const int ymax = std::min(rect.ymax, int(std::ceil(std::max({a.y, b.y, c.y}))));
              for (int y = ymin; y <= ymax; y++) {
                for (int x = xmin; x <= xmax; x++) {
                  if (++probes > (1 << 22)) {
                    error = "NURBS face box selection exceeded its viewport probe budget";
                    return;
                  }
                  float value;
                  if (triangle_depth(float2(x + 0.5f, y + 0.5f), a, b, c, value) &&
                      depth.visible(float3(x + 0.5f, y + 0.5f, value)))
                  {
                    visible_faces.add(id);
                    return;
                  }
                }
              }
            },
            error);
        for (const NurbsTopologyFace &face : target.source.geometry().faces) {
          if (visible_faces.contains(face.id)) {
            hits.append({ElementDomain::Face, face.id});
          }
        }
      }
    }
  }
  if (!error.empty()) {
    WM_global_report(RPT_ERROR, error.c_str());
    return false;
  }
  const bool changed = select_components(
      *vc.bmain, curve, target.source, hits, selection_action(operation), std::nullopt, &error);
  if (!error.empty()) {
    WM_global_report(RPT_ERROR, error.c_str());
  }
  return changed;
}

bool select_mode(bContext *C,
                 const bke::nurbs::SelectionMode mode,
                 const bool extend,
                 bool *r_changed)
{
  using namespace bke::nurbs;
  if (r_changed != nullptr) {
    *r_changed = false;
  }
  const auto current =
      id_cast<Curve *>(CTX_data_edit_object(C)->data)->editnurb->nurbs_edit->selection().mode;
  const SelectionMode requested = extend ? SelectionMode(int(current) ^ int(mode)) : mode;
  if (int(requested) == 0) {
    return true;
  }
  Main &bmain = *CTX_data_main(C);
  Vector<SelectionTarget> targets;
  Vector<EditSnapshot> candidates;
  std::string error;
  for (Object *object : BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
           bmain, CTX_data_scene(C), CTX_data_view_layer(C), CTX_wm_view3d(C)))
  {
    Curve &curve = *id_cast<Curve *>(object->data);
    if (curve.editnurb->nurbs_edit == nullptr || !curve_validate_source(curve, &error)) {
      if (error.empty()) {
        error = "Mixed native and legacy NURBS selection modes are unsupported";
      }
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
    targets.append({object, curve.editnurb->nurbs_edit->read_snapshot()});
    const auto selected = selection_change_mode(
        targets.last().source.geometry(), targets.last().source.selection(), requested, &error);
    if (!selected) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
    auto candidate = targets.last().source.copy_for_edit();
    candidate.selection = *selected;
    candidates.append(std::move(candidate));
  }
  bool changed = false;
  for (const int i : targets.index_range()) {
    if (targets[i].source.selection().mode == requested) {
      continue;
    }
    if (!edit_commit(bmain,
                     *id_cast<Curve *>(targets[i].object->data),
                     std::move(candidates[i]),
                     targets[i].source,
                     &error))
    {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
    changed = true;
  }
  if (r_changed != nullptr) {
    *r_changed = changed;
  }
  return true;
}

static Vector<bke::nurbs::ElementRef> linked_hits(const bke::nurbs::LinkedComponents &linked,
                                                  const bke::nurbs::SelectionMode mode)
{
  using namespace bke::nurbs;
  Vector<ElementRef> result;
  if (flag_is_set(mode, SelectionMode::Vertex)) {
    for (const int64_t id : linked.vertices) {
      result.append({ElementDomain::Vertex, id});
    }
  }
  if (flag_is_set(mode, SelectionMode::Edge)) {
    for (const int64_t id : linked.edges) {
      result.append({ElementDomain::Edge, id});
    }
  }
  if (flag_is_set(mode, SelectionMode::Face)) {
    for (const int64_t id : linked.faces) {
      result.append({ElementDomain::Face, id});
    }
  }
  return result;
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
  return linked && select_components(bmain,
                                     curve,
                                     expected,
                                     linked_hits(*linked, expected.selection().mode),
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
    changed |= select_components(bmain,
                                 *id_cast<Curve *>(targets[i].object->data),
                                 targets[i].source,
                                 linked_hits(components[i], targets[i].source.selection().mode),
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

enum class ComponentEdit { Duplicate, Extrude, Delete };

/** Resolve a complete kernel candidate before any owner mutation or notification. */
static std::optional<bke::nurbs::EditSnapshot> component_edit_candidate(
    const bke::nurbs::ReadSnapshot &expected,
    const ComponentEdit operation,
    const bke::nurbs::DeleteMode delete_mode,
    double &r_endpoint_deviation,
    std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  r_endpoint_deviation = 0;
  const auto &selection = expected.selection();
  if (selection.vertices.is_empty() && selection.edges.is_empty() && selection.faces.is_empty()) {
    return std::nullopt;
  }
  const auto normalized = bke::nurbs::selection_flush(
      expected.geometry(), selection, selection.mode, r_error);
  if (!normalized) {
    return std::nullopt;
  }
  if (normalized->vertices != selection.vertices || normalized->edges != selection.edges ||
      normalized->faces != selection.faces)
  {
    if (r_error != nullptr) {
      *r_error = "Native NURBS component editing requires a normalized component-mode selection";
    }
    return std::nullopt;
  }
  const Vector<int64_t> vertices(selection.vertices.begin(), selection.vertices.end());
  const Vector<int64_t> edges(selection.edges.begin(), selection.edges.end());
  const Vector<int64_t> faces(selection.faces.begin(), selection.faces.end());
  const bke::nurbs::ComponentSelection components{vertices, edges, faces};
  bke::nurbs::OwnedGeometry geometry;
  Map<int64_t, int64_t> id_map;
  if (operation == ComponentEdit::Delete) {
    auto deleted = bke::nurbs::delete_components(
        expected.geometry(), components, delete_mode, r_error);
    if (!deleted || (deleted->removed.vertices.is_empty() && deleted->removed.edges.is_empty() &&
                     deleted->removed.faces.is_empty()))
    {
      return std::nullopt;
    }
    Set<int64_t> removed = std::move(deleted->removed.vertices);
    for (const int64_t id : deleted->removed.edges) {
      removed.add(id);
    }
    for (const int64_t id : deleted->removed.faces) {
      removed.add(id);
    }
    auto selected = bke::nurbs::selection_flush(
        *deleted->geometry,
        bke::nurbs::selection_remove_ids(selection, removed),
        selection.mode,
        r_error);
    if (!selected) {
      return std::nullopt;
    }
    return bke::nurbs::EditSnapshot{std::move(deleted->geometry), std::move(*selected)};
  }
  if (operation == ComponentEdit::Duplicate) {
    auto copy = bke::nurbs::duplicate_components(expected.geometry(), components, r_error);
    if (!copy) {
      return std::nullopt;
    }
    geometry = std::move(copy->geometry);
    id_map = std::move(copy->source_to_copy);
  }
  else {
    auto swept = bke::nurbs::extrude_components(
        expected.geometry(), components, {0, 0, 0}, 0, r_error);
    if (!swept) {
      return std::nullopt;
    }
    r_endpoint_deviation = swept->max_source_endpoint_deviation;
    geometry = std::move(swept->geometry);
    id_map = std::move(swept->source_to_cap);
  }
  auto selected = bke::nurbs::selection_remap(
      expected.geometry(), *geometry, selection, id_map, selection.mode, r_error);
  if (!selected) {
    return std::nullopt;
  }
  return bke::nurbs::EditSnapshot{std::move(geometry), std::move(*selected)};
}

static bool component_edit(Main &bmain,
                           Curve &curve,
                           const ComponentEdit operation,
                           const bke::nurbs::DeleteMode mode,
                           std::string *r_error,
                           double *r_endpoint_deviation = nullptr)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (r_endpoint_deviation != nullptr) {
    *r_endpoint_deviation = 0;
  }
  if (!bke::nurbs::curve_validate_source(curve, r_error)) {
    return false;
  }
  if (curve.editnurb == nullptr || curve.editnurb->nurbs_edit == nullptr) {
    if (r_error != nullptr) {
      *r_error = "Curve has no native NURBS edit source";
    }
    return false;
  }
  const auto expected = curve.editnurb->nurbs_edit->read_snapshot();
  double deviation;
  auto candidate = component_edit_candidate(expected, operation, mode, deviation, r_error);
  if (!candidate || !edit_commit(bmain, curve, std::move(*candidate), expected, r_error)) {
    return false;
  }
  if (r_endpoint_deviation != nullptr) {
    *r_endpoint_deviation = deviation;
  }
  return true;
}

bool duplicate(Main &bmain, Curve &curve, std::string *r_error)
{
  return component_edit(
      bmain, curve, ComponentEdit::Duplicate, bke::nurbs::DeleteMode::Vertices, r_error);
}

bool extrude(Main &bmain, Curve &curve, std::string *r_error, double *r_source_endpoint_deviation)
{
  return component_edit(bmain,
                        curve,
                        ComponentEdit::Extrude,
                        bke::nurbs::DeleteMode::Vertices,
                        r_error,
                        r_source_endpoint_deviation);
}

bool delete_components(Main &bmain,
                       Curve &curve,
                       const bke::nurbs::DeleteMode mode,
                       std::string *r_error)
{
  return component_edit(bmain, curve, ComponentEdit::Delete, mode, r_error);
}

static bool component_edit(bContext *C,
                           const ComponentEdit operation,
                           const bke::nurbs::DeleteMode mode)
{
  Main &bmain = *CTX_data_main(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      bmain, CTX_data_scene(C), CTX_data_view_layer(C), CTX_wm_view3d(C));
  if (operation != ComponentEdit::Delete && objects.size() != 1) {
    BKE_report(
        CTX_wm_reports(C),
        RPT_ERROR,
        "Native NURBS duplication, extrusion and movement currently require a single edit object");
    return false;
  }
  Vector<SelectionTarget> targets;
  Vector<std::optional<bke::nurbs::EditSnapshot>> candidates;
  double max_endpoint_deviation = 0;
  std::string error;
  // Resolve every owner before changing any of them, including mixed native/legacy rejection.
  for (Object *object : objects) {
    Curve &curve = *id_cast<Curve *>(object->data);
    if (curve.editnurb->nurbs_edit == nullptr) {
      BKE_report(CTX_wm_reports(C),
                 RPT_ERROR,
                 "Mixed native and legacy NURBS component editing is unsupported");
      return false;
    }
    if (!bke::nurbs::curve_validate_source(curve, &error)) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
    targets.append({object, curve.editnurb->nurbs_edit->read_snapshot()});
    double deviation;
    candidates.append(
        component_edit_candidate(targets.last().source, operation, mode, deviation, &error));
    if (!error.empty()) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
    max_endpoint_deviation = std::max(max_endpoint_deviation, deviation);
  }
  bool changed = false;
  for (const int i : targets.index_range()) {
    if (!candidates[i]) {
      continue;
    }
    if (!edit_commit(bmain,
                     *id_cast<Curve *>(targets[i].object->data),
                     std::move(*candidates[i]),
                     targets[i].source,
                     &error))
    {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
      return false;
    }
    changed = true;
  }
  if (changed && max_endpoint_deviation != 0) {
    BKE_reportf(CTX_wm_reports(C),
                RPT_INFO,
                "NURBS source endpoint deviation %.17g is within its declared source tolerance",
                max_endpoint_deviation);
  }
  return changed;
}

bool duplicate(bContext *C)
{
  return component_edit(C, ComponentEdit::Duplicate, bke::nurbs::DeleteMode::Vertices);
}

bool extrude(bContext *C)
{
  return component_edit(C, ComponentEdit::Extrude, bke::nurbs::DeleteMode::Vertices);
}

bool delete_components(bContext *C, const bke::nurbs::DeleteMode mode)
{
  return component_edit(C, ComponentEdit::Delete, mode);
}

bool select_linked_pick(bContext *C, const int mval[2], const int distance, const bool deselect)
{
  view3d_operator_needs_gpu(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_ensure_evaluated_depsgraph(C));
  Vector<SelectionTarget> targets;
  Object *hit_object = nullptr;
  using namespace bke::nurbs;
  ElementRef hit{ElementDomain::Vertex, 0};
  std::string error;
  if (!find_component(vc, mval, distance, targets, hit_object, hit, error)) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, error.c_str());
    return false;
  }
  for (const SelectionTarget &target : targets) {
    if (target.object != hit_object) {
      continue;
    }
    const ComponentSelection seeds{
        hit.domain == ElementDomain::Vertex ? Span<int64_t>(&hit.id, 1) : Span<int64_t>(),
        hit.domain == ElementDomain::Edge ? Span<int64_t>(&hit.id, 1) : Span<int64_t>(),
        hit.domain == ElementDomain::Face ? Span<int64_t>(&hit.id, 1) : Span<int64_t>()};
    const auto linked = find_linked_components(
        target.source.geometry(), seeds, LinkedConnectivity::RequireAgreement, &error);
    const bool changed = linked && select_components(
                                       *vc.bmain,
                                       *id_cast<Curve *>(target.object->data),
                                       target.source,
                                       linked_hits(*linked, target.source.selection().mode),
                                       deselect ? SelectionAction::Subtract : SelectionAction::Add,
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
