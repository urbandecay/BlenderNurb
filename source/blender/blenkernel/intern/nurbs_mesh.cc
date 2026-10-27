/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "DNA_curve_types.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_math_boolean.hh"
#include "BLI_set.hh"
#include "BLI_time.h"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_nurbs_debug.hh"
#include "BKE_nurbs_mesh.hh"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {

/** Test the stored float positions exactly. Their exponent range is safe for the double
 * orientation predicate, including subnormals. An area epsilon would discard thin real faces. */
static bool display_triangle_has_area(const float3 &a, const float3 &b, const float3 &c)
{
  for (const int axis : IndexRange(3)) {
    const int other = (axis + 1) % 3;
    if (orient2d(double2(a[axis], a[other]),
                 double2(b[axis], b[other]),
                 double2(c[axis], c[other])) != 0)
    {
      return true;
    }
  }
  return false;
}

/** A uniform translation leaves every support derivative and oriented normal unchanged. */
static bool supports_differ_only_by_translation(const NurbsGeometry &source,
                                                const NurbsGeometry &previous_source,
                                                const char **r_reason)
{
  /* Refinement can leave unreferenced parent supports in the source resource collection.
   * They do not contribute display normals and need not move with the active topology. */
  Set<int64_t> used_surfaces;
  for (const auto &face : source.faces) {
    used_surfaces.add(face.surface_id);
  }
  Map<int64_t, const NurbsSurfaceGeometry *> previous_surfaces;
  for (const NurbsSurfaceGeometry &surface : previous_source.surfaces) {
    previous_surfaces.add_new(surface.id, &surface);
  }
  bool have_translation = false;
  double3 translation(0), translation_residual(0);
  for (const NurbsSurfaceGeometry &surface : source.surfaces) {
    if (!used_surfaces.contains(surface.id)) {
      continue;
    }
    const NurbsSurfaceGeometry *const *previous_ptr = previous_surfaces.lookup_ptr(surface.id);
    const NurbsSurfaceGeometry *previous = previous_ptr ? *previous_ptr : nullptr;
    if (!previous || surface.points_num != previous->points_num ||
        surface.points_u != previous->points_u || surface.points_v != previous->points_v ||
        surface.order_u != previous->order_u || surface.order_v != previous->order_v ||
        surface.knots_u_num != previous->knots_u_num ||
        surface.knots_v_num != previous->knots_v_num ||
        !std::equal(surface.knots_u, surface.knots_u + surface.knots_u_num, previous->knots_u) ||
        !std::equal(surface.knots_v, surface.knots_v + surface.knots_v_num, previous->knots_v))
    {
      *r_reason = "surface basis, knots, or identities changed";
      return false;
    }
    for (const int i : IndexRange(surface.points_num)) {
      if (surface.points[i].weight != previous->points[i].weight) {
        *r_reason = "control point weight changed";
        return false;
      }
      for (const int axis : IndexRange(3)) {
        const double old_position = previous->points[i].position[axis];
        const double new_position = surface.points[i].position[axis];
        double change, residual;
        if (!detail::sum_with_error(new_position, -old_position, change, residual)) {
          *r_reason = "control point displacement exceeds double precision range";
          return false;
        }
        if (!have_translation) {
          translation[axis] = change;
          translation_residual[axis] = residual;
        }
        else {
          if (change != translation[axis] || residual != translation_residual[axis]) {
            *r_reason = "control point displacement is not an exact uniform translation";
            return false;
          }
        }
      }
      have_translation = true;
    }
  }
  if (!have_translation) {
    *r_reason = "no support control points";
  }
  return have_translation;
}

void MeshDeleter::operator()(Mesh *mesh) const
{
  if (mesh != nullptr) {
    BKE_id_free(nullptr, mesh);
  }
}

SurfacePreviewOptions curve_preview_options(const Curve &curve, const bool for_render)
{
  const int resolution_u = for_render && curve.resolu_ren > 0 ? curve.resolu_ren : curve.resolu;
  const int resolution_v = for_render && curve.resolv_ren > 0 ? curve.resolv_ren : curve.resolv;
  SurfacePreviewOptions options;
  options.boundary.segments_per_span = 2 * std::max(resolution_u, resolution_v) - 1;
  options.interior_subdivisions = options.boundary.segments_per_span;
  return options;
}

struct TranslationComponents {
  Map<int64_t, double3> vertices;
  Map<int64_t, double3> edges;
  Map<int64_t, double3> faces;
  Map<int64_t, double3> curves;
  Map<int64_t, double3> surfaces;
};

/** Compare active source records. Each independent display surface or edge curve must be either
 * unchanged or translated as a whole; a partial control-net deformation falls back to sampling.
 * Unused parent supports left by refinement do not contribute to the display. */
static bool source_matches_component_translation(const NurbsGeometry &source,
                                                 const NurbsGeometry &original,
                                                 const double3 &translation,
                                                 TranslationComponents &r_components)
{
  if (source.next_id != original.next_id || source.vertices.count() != original.vertices.count() ||
      source.edges.count() != original.edges.count() ||
      source.faces.count() != original.faces.count() ||
      source.curves.count() != original.curves.count() ||
      source.surfaces.count() != original.surfaces.count())
  {
    return false;
  }
  for (const int axis : IndexRange(3)) {
    if (!std::isfinite(translation[axis])) {
      return false;
    }
  }
  auto positions_translation = [&](const auto positions,
                                   const auto old_positions,
                                   const int count,
                                   double3 &r_component_translation) {
    bool unchanged = true;
    bool translated = true;
    for (const int i : IndexRange(count)) {
      for (const int axis : IndexRange(3)) {
        const double current = positions(i)[axis];
        const double previous = old_positions(i)[axis];
        if (!std::isfinite(current) || !std::isfinite(previous)) {
          return false;
        }
        unchanged &= current == previous;
        translated &= current == previous + translation[axis];
      }
    }
    if (unchanged) {
      r_component_translation = double3(0);
      return true;
    }
    if (translated) {
      r_component_translation = translation;
      return true;
    }
    return false;
  };
  const auto *old_vertex = static_cast<const NurbsTopologyVertex *>(original.vertices.first);
  for (const auto &vertex : source.vertices) {
    double3 vertex_translation;
    if (vertex.id != old_vertex->id || vertex.tolerance < old_vertex->tolerance ||
        !positions_translation([&](const int) { return vertex.position; },
                               [&](const int) { return old_vertex->position; },
                               1,
                               vertex_translation))
    {
      return false;
    }
    r_components.vertices.add_new(vertex.id, vertex_translation);
    old_vertex = old_vertex->next;
  }
  Set<int64_t> active_curves, active_surfaces;
  const auto *old_edge = static_cast<const NurbsTopologyEdge *>(original.edges.first);
  for (const auto &edge : source.edges) {
    if (edge.id != old_edge->id || edge.curve_id != old_edge->curve_id ||
        !std::equal(edge.vertex_ids, edge.vertex_ids + 2, old_edge->vertex_ids) ||
        !std::equal(edge.parameter_range, edge.parameter_range + 2, old_edge->parameter_range) ||
        edge.tolerance < old_edge->tolerance)
    {
      return false;
    }
    active_curves.add(edge.curve_id);
    old_edge = old_edge->next;
  }
  const auto *old_face = static_cast<const NurbsTopologyFace *>(original.faces.first);
  for (const auto &face : source.faces) {
    if (face.id != old_face->id || face.surface_id != old_face->surface_id ||
        face.orientation != old_face->orientation ||
        face.material_index != old_face->material_index || face.tolerance < old_face->tolerance ||
        face.loops.count() != old_face->loops.count())
    {
      return false;
    }
    active_surfaces.add(face.surface_id);
    const auto *old_loop = static_cast<const NurbsTopologyLoop *>(old_face->loops.first);
    for (const auto &loop : face.loops) {
      if (loop.id != old_loop->id || loop.is_inner != old_loop->is_inner ||
          loop.edge_uses.count() != old_loop->edge_uses.count())
      {
        return false;
      }
      const auto *old_use = static_cast<const NurbsTopologyEdgeUse *>(old_loop->edge_uses.first);
      for (const auto &use : loop.edge_uses) {
        if (use.id != old_use->id || use.edge_id != old_use->edge_id ||
            use.pcurve_id != old_use->pcurve_id || use.reversed != old_use->reversed ||
            use.tolerance < old_use->tolerance)
        {
          return false;
        }
        active_curves.add(use.pcurve_id);
        old_use = old_use->next;
      }
      old_loop = old_loop->next;
    }
    old_face = old_face->next;
  }
  auto coefficients_unchanged =
      [&](const NurbsControlPoint *a, const NurbsControlPoint *b, const int count) {
        for (const int i : IndexRange(count)) {
          if (a[i].weight != b[i].weight ||
              !std::equal(a[i].position, a[i].position + 3, b[i].position))
          {
            return false;
          }
        }
        return true;
      };
  const auto *old_curve = static_cast<const NurbsCurveGeometry *>(original.curves.first);
  for (const auto &curve : source.curves) {
    if (curve.id != old_curve->id) {
      return false;
    }
    if (active_curves.contains(curve.id)) {
      if (curve.dimension != old_curve->dimension || curve.order != old_curve->order ||
          curve.points_num != old_curve->points_num || curve.knots_num != old_curve->knots_num ||
          !std::equal(curve.knots, curve.knots + curve.knots_num, old_curve->knots))
      {
        return false;
      }
      double3 curve_translation(0);
      if (curve.dimension == 3) {
        if (!positions_translation([&](const int i) { return curve.points[i].position; },
                                   [&](const int i) { return old_curve->points[i].position; },
                                   curve.points_num,
                                   curve_translation))
        {
          return false;
        }
      }
      else if (!coefficients_unchanged(curve.points, old_curve->points, curve.points_num)) {
        return false;
      }
      for (const int i : IndexRange(curve.points_num)) {
        if (curve.points[i].weight != old_curve->points[i].weight) {
          return false;
        }
      }
      r_components.curves.add_new(curve.id, curve_translation);
    }
    old_curve = old_curve->next;
  }
  const auto *old_surface = static_cast<const NurbsSurfaceGeometry *>(original.surfaces.first);
  for (const auto &surface : source.surfaces) {
    if (surface.id != old_surface->id) {
      return false;
    }
    if (active_surfaces.contains(surface.id)) {
      if (surface.points_u != old_surface->points_u || surface.points_v != old_surface->points_v ||
          surface.order_u != old_surface->order_u || surface.order_v != old_surface->order_v ||
          surface.points_num != old_surface->points_num ||
          surface.knots_u_num != old_surface->knots_u_num ||
          surface.knots_v_num != old_surface->knots_v_num ||
          !std::equal(
              surface.knots_u, surface.knots_u + surface.knots_u_num, old_surface->knots_u) ||
          !std::equal(
              surface.knots_v, surface.knots_v + surface.knots_v_num, old_surface->knots_v))
      {
        return false;
      }
      double3 surface_translation(0);
      if (!positions_translation([&](const int i) { return surface.points[i].position; },
                                 [&](const int i) { return old_surface->points[i].position; },
                                 surface.points_num,
                                 surface_translation))
      {
        return false;
      }
      for (const int i : IndexRange(surface.points_num)) {
        if (surface.points[i].weight != old_surface->points[i].weight) {
          return false;
        }
      }
      r_components.surfaces.add_new(surface.id, surface_translation);
    }
    old_surface = old_surface->next;
  }
  for (const auto &edge : source.edges) {
    r_components.edges.add_new(edge.id, r_components.curves.lookup(edge.curve_id));
  }
  for (const auto &face : source.faces) {
    r_components.faces.add_new(face.id, r_components.surfaces.lookup(face.surface_id));
  }
  return true;
}

std::optional<MeshEvaluation> translate_mesh_preview(const ReadSnapshot &source,
                                                     const ObjectEvaluation &previous,
                                                     const Mesh &display_mesh,
                                                     const double3 &translation)
{
  const double start = BLI_time_now_seconds();
  if (!previous.result || previous.result->is_translation_preview ||
      previous.result->undefined_normal_corners != 0 ||
      display_mesh.verts_num != previous.result->preview->points_num() ||
      display_mesh.faces_num != previous.result->mesh_to_preview_triangle.size())
  {
    return std::nullopt;
  }
  const auto &original = *previous.result;
  TranslationComponents components;
  if (!source_matches_component_translation(
          source.geometry(), original.source.geometry(), translation, components))
  {
    return std::nullopt;
  }
  auto preview = std::make_shared<SurfacePreview>(*original.preview);
  MeshEvaluation result{source, preview, nullptr, original.mesh_to_preview_triangle};
  Array<double3> point_translations(preview->points_num(), double3(0));
  Array<uint8_t> assigned(preview->points_num(), 0);
  auto assign_translation = [&](const int point_index, const double3 &point_translation) {
    if (point_index < 0 || point_index >= point_translations.size()) {
      return false;
    }
    if (assigned[point_index]) {
      return point_translations[point_index] == point_translation;
    }
    point_translations[point_index] = point_translation;
    assigned[point_index] = 1;
    return true;
  };
  for (const SurfacePreviewTriangle &triangle : preview->triangles) {
    const double3 face_translation = components.faces.lookup(triangle.face_id);
    for (const int corner : IndexRange(3)) {
      const int point = triangle.points[corner];
      if (!assign_translation(point, face_translation)) {
        return std::nullopt;
      }
    }
  }
  for (const int i : preview->boundary.positions.index_range()) {
    const int64_t vertex_id = preview->boundary.vertex_ids[i];
    if (vertex_id != 0 && !assign_translation(i, components.vertices.lookup(vertex_id))) {
      return std::nullopt;
    }
  }
  for (const EvaluatedBoundaryEdge &edge : preview->boundary.edges) {
    const double3 edge_translation = components.edges.lookup(edge.edge_id);
    for (const BoundarySample &sample : edge.samples) {
      if (!assign_translation(sample.point_index, edge_translation)) {
        return std::nullopt;
      }
    }
  }
  for (const int i : preview->boundary.positions.index_range()) {
    preview->boundary.positions[i] += point_translations[i];
  }
  for (auto &edge : preview->boundary.edges) {
    const double3 edge_translation = components.edges.lookup(edge.edge_id);
    for (auto &sample : edge.samples) {
      sample.curve_position += edge_translation;
      for (const int axis : IndexRange(3)) {
        if (!std::isfinite(sample.curve_position[axis])) {
          return std::nullopt;
        }
      }
    }
  }
  for (auto &point : preview->interior) {
    point.position += components.faces.lookup(point.face_id);
  }
  Array<float3> positions(preview->points_num());
  for (const int i : positions.index_range()) {
    const double3 position = preview->position(i);
    for (const int axis : IndexRange(3)) {
      if (!std::isfinite(position[axis]) ||
          std::abs(position[axis]) > std::numeric_limits<float>::max())
      {
        return std::nullopt;
      }
      positions[i][axis] = float(position[axis]);
    }
    const double3 difference = position - double3(positions[i]);
    result.max_float_position_deviation = std::max(
        result.max_float_position_deviation, std::hypot(difference.x, difference.y, difference.z));
  }
  /* Translation at large coordinates can collapse or recover float triangles. Retain the
   * cached connectivity only when the complete visibility map stays identical. */
  int displayed = 0;
  for (const int i : preview->triangles.index_range()) {
    const int3 triangle = preview->triangles[i].points;
    const bool visible = display_triangle_has_area(
        positions[triangle.x], positions[triangle.y], positions[triangle.z]);
    const bool was_visible = displayed < result.mesh_to_preview_triangle.size() &&
                             result.mesh_to_preview_triangle[displayed] == i;
    if (visible != was_visible) {
      return std::nullopt;
    }
    displayed += visible;
  }
  result.mesh.reset(BKE_mesh_copy_for_eval(display_mesh));
  /* The object evaluator attaches its current material slots after this derived evaluation. */
  MEM_SAFE_DELETE(result.mesh->mat);
  result.mesh->totcol = 0;
  result.mesh->vert_positions_for_write().copy_from(positions);
  result.mesh->tag_positions_changed_uniformly();
  result.corner_normals = original.corner_normals;
  result.skipped_degenerate_float_triangles = original.skipped_degenerate_float_triangles;
  result.skipped_repeated_point_segments = original.skipped_repeated_point_segments;
  preview->reused_faces = source.geometry().faces.count();
  result.reused_tessellation = result.reused_normals = result.is_translation_preview = true;
  NURBS_DEBUG_LOG(
      "translation display preview: %.3f ms; %d points, %d triangles; "
      "component display transported, no source sampling",
      (BLI_time_now_seconds() - start) * 1000.0,
      int(positions.size()),
      result.mesh->faces_num);
  return result;
}

std::optional<MeshEvaluation> evaluate_mesh(const ReadSnapshot &source,
                                            const SurfacePreviewOptions &options,
                                            std::string *r_error,
                                            const ObjectEvaluation *previous)
{
  const double evaluation_start = BLI_time_now_seconds();
  /* A temporary translated display is never the starting point of another source edit or
   * final evaluation. Only the identical prepared candidate can hand it to the renderer. */
  if (previous && previous->result && previous->result->is_translation_preview &&
      &previous->result->source.geometry() != &source.geometry())
  {
    previous = nullptr;
  }
  /* A preflighted edit can hand its exact immutable source and owned display mesh to the
   * depsgraph. Copy the derived Mesh for independent renderer ownership, without sampling or
   * calculating source normals a second time. Share the immutable preview, while retaining the
   * current source lease for picking and stale-state checks. Released renderer meshes cannot
   * take this path. */
  if (previous && previous->result && previous->result->mesh && previous->options == options &&
      &previous->result->source.geometry() == &source.geometry())
  {
    const MeshEvaluation &cached = *previous->result;
    MeshEvaluation result{source,
                          cached.preview,
                          OwnedMesh(BKE_mesh_copy_for_eval(*cached.mesh)),
                          cached.mesh_to_preview_triangle};
    result.corner_normals = cached.corner_normals;
    result.skipped_degenerate_float_triangles = cached.skipped_degenerate_float_triangles;
    result.max_float_position_deviation = cached.max_float_position_deviation;
    result.skipped_repeated_point_segments = cached.skipped_repeated_point_segments;
    result.undefined_normal_corners = cached.undefined_normal_corners;
    result.reused_tessellation = result.reused_normals = result.reused_evaluation = true;
    result.is_translation_preview = cached.is_translation_preview;
    if (r_error) {
      r_error->clear();
    }
    NURBS_DEBUG_LOG("mesh eval prepared preview hit: %.3f ms total; %d faces, %d triangles",
                    (BLI_time_now_seconds() - evaluation_start) * 1000.0,
                    source.geometry().faces.count(),
                    int(result.preview->triangles.size()));
    return result;
  }
  double position_ms = 0.0, triangle_filter_ms = 0.0, edge_build_ms = 0.0;
  double mesh_build_ms = 0.0, normal_ms = 0.0;
  int normal_evaluations = 0;
  auto fail = [&](const char *message) -> std::optional<MeshEvaluation> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  bool reused = false;
  auto preview = previous && previous->result ?
                     refresh_surface_preview(source.geometry(),
                                             options,
                                             previous->result->source.geometry(),
                                             previous->options,
                                             *previous->result->preview,
                                             reused,
                                             r_error) :
                     tessellate_surfaces(source.geometry(), options, r_error);
  if (!preview) {
    NURBS_DEBUG_LOG("preview failed after %.3f ms: %s",
                    (BLI_time_now_seconds() - evaluation_start) * 1000.0,
                    r_error ? r_error->c_str() : "no diagnostic");
    return std::nullopt;
  }
  if (preview->triangles.size() > std::numeric_limits<int>::max() / 3) {
    return fail("NURBS display mesh exceeds the corner index range");
  }
  MeshEvaluation result{source,
                        std::make_shared<const SurfacePreview>(std::move(*preview)),
                        nullptr,
                        {}};
  result.reused_tessellation = reused;
  Array<float3> positions(result.preview->points_num());
  const double position_start = BLI_time_now_seconds();
  for (const int i : positions.index_range()) {
    const double3 position = result.preview->position(i);
    for (const int axis : IndexRange(3)) {
      if (!std::isfinite(position[axis]) ||
          std::abs(position[axis]) > std::numeric_limits<float>::max())
      {
        return fail("NURBS source exceeds the float display coordinate range");
      }
      positions[i][axis] = float(position[axis]);
    }
    const double3 difference = position - double3(positions[i]);
    result.max_float_position_deviation = std::max(
        result.max_float_position_deviation, std::hypot(difference.x, difference.y, difference.z));
  }
  position_ms = (BLI_time_now_seconds() - position_start) * 1000.0;
  const double triangle_filter_start = BLI_time_now_seconds();
  for (const int i : result.preview->triangles.index_range()) {
    const int3 triangle = result.preview->triangles[i].points;
    if (display_triangle_has_area(
            positions[triangle.x], positions[triangle.y], positions[triangle.z]))
    {
      result.mesh_to_preview_triangle.append(i);
    }
    else {
      result.skipped_degenerate_float_triangles++;
    }
  }
  triangle_filter_ms = (BLI_time_now_seconds() - triangle_filter_start) * 1000.0;
  const double edge_build_start = BLI_time_now_seconds();
  Vector<int2> edges;
  Set<int2> edge_keys;
  for (const EvaluatedBoundaryEdge &edge : result.preview->boundary.edges) {
    for (const int i : IndexRange(edge.samples.size() - 1)) {
      const int a = edge.samples[i].point_index, b = edge.samples[i + 1].point_index;
      if (a == b) {
        result.skipped_repeated_point_segments++;
        continue;
      }
      /* Deduplication is by derived indices, not coordinates. Coincident source vertices remain
       * distinct indices. Separate source edges with the same sampled interval retain their own
       * identities/parameters in the preview, independently of this display edge representation.
       */
      if (edge_keys.add(int2(std::min(a, b), std::max(a, b)))) {
        edges.append({a, b});
      }
    }
  }
  edge_build_ms = (BLI_time_now_seconds() - edge_build_start) * 1000.0;
  const int faces_num = int(result.mesh_to_preview_triangle.size());
  const double mesh_build_start = BLI_time_now_seconds();
  result.mesh.reset(BKE_mesh_new_nomain(positions.size(), edges.size(), faces_num, faces_num * 3));
  Mesh &mesh = *result.mesh;
  mesh.vert_positions_for_write().copy_from(positions);
  mesh.edges_for_write().copy_from(edges);
  MutableSpan<int> offsets = mesh.face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh.corner_verts_for_write();
  for (const int i : offsets.index_range()) {
    offsets[i] = i * 3;
  }
  if (faces_num > 0) {
    const double normal_start = BLI_time_now_seconds();
    const auto index = TopologyIndex::build(source.geometry(), r_error);
    if (!index) {
      return std::nullopt;
    }
    Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
    for (const NurbsSurfaceGeometry &surface : source.geometry().surfaces) {
      surfaces.add_new(surface.id, &surface);
    }
    Array<float3> normals(mesh.corners_num, float3(0));
    const MeshEvaluation *previous_result = previous && previous->result ? &*previous->result :
                                                                           nullptr;
    const bool same_display_triangles = previous_result &&
                                        previous_result->mesh_to_preview_triangle.size() ==
                                            result.mesh_to_preview_triangle.size() &&
                                        std::equal(
                                            previous_result->mesh_to_preview_triangle.begin(),
                                            previous_result->mesh_to_preview_triangle.end(),
                                            result.mesh_to_preview_triangle.begin());
    const char *normal_reuse_reason = nullptr;
    const bool support_translation =
        previous_result && same_display_triangles &&
                previous_result->corner_normals.size() == mesh.corners_num ?
            supports_differ_only_by_translation(
                source.geometry(), previous_result->source.geometry(), &normal_reuse_reason) :
            false;
    const bool reuse_normals = reused && same_display_triangles && previous_result &&
                               previous_result->corner_normals.size() == mesh.corners_num &&
                               support_translation;
    if (!reused) {
      normal_reuse_reason = "preview layout was rebuilt";
    }
    else if (!same_display_triangles) {
      normal_reuse_reason = "display triangle map changed";
    }
    else if (!previous_result) {
      normal_reuse_reason = "no previous object evaluation";
    }
    else if (previous_result->corner_normals.size() != mesh.corners_num) {
      normal_reuse_reason = "previous normal array size changed";
    }
    result.reused_normals = reuse_normals;
    NURBS_DEBUG_LOG("normal cache %s: %s",
                    reuse_normals ? "reused" : "not reused",
                    reuse_normals ? "uniform support translation" : normal_reuse_reason);
    if (reuse_normals) {
      std::copy(previous_result->corner_normals.begin(),
                previous_result->corner_normals.end(),
                normals.begin());
      result.undefined_normal_corners = previous_result->undefined_normal_corners;
    }
    Map<int64_t, Vector<int>> face_triangles;
    for (const int i : result.mesh_to_preview_triangle.index_range()) {
      const auto &triangle = result.preview->triangles[result.mesh_to_preview_triangle[i]];
      for (const int j : IndexRange(3)) {
        corner_verts[i * 3 + j] = triangle.points[j];
      }
      if (!reuse_normals) {
        face_triangles.lookup_or_add_default(triangle.face_id).append(i);
      }
    }
    if (!reuse_normals) {
      Array<int> corner_samples(mesh.corners_num);
      for (const auto &face : source.geometry().faces) {
        const auto *triangles = face_triangles.lookup_ptr(face.id);
        if (!triangles) {
          continue;
        }
        const NurbsSurfaceGeometry &surface = *surfaces.lookup(face.surface_id);
        Map<std::pair<double2, int>, int> sample_indices;
        Vector<SurfaceDifferentialSample> samples;
        for (const int i : *triangles) {
          const auto &triangle = result.preview->triangles[result.mesh_to_preview_triangle[i]];
          double2 center;
          for (const int axis : IndexRange(2)) {
            center[axis] = std::lerp(
                std::midpoint(triangle.parameters[0][axis], triangle.parameters[1][axis]),
                triangle.parameters[2][axis],
                1.0 / 3.0);
          }
          for (const int j : IndexRange(3)) {
            const double2 parameter = triangle.parameters[j];
            const SurfaceLimits limits{center.x < parameter.x ? KnotSide::Left : KnotSide::Right,
                                       center.y < parameter.y ? KnotSide::Left : KnotSide::Right};
            /* Adjacent triangles share one source normal unless a meaningful knot side differs. */
            const bool left_u = limits.u == KnotSide::Left &&
                                std::binary_search(surface.knots_u + surface.order_u,
                                                   surface.knots_u + surface.points_u,
                                                   parameter.x);
            const bool left_v = limits.v == KnotSide::Left &&
                                std::binary_search(surface.knots_v + surface.order_v,
                                                   surface.knots_v + surface.points_v,
                                                   parameter.y);
            const std::pair<double2, int> key{parameter, int(left_u) | (int(left_v) << 1)};
            const int sample = sample_indices.lookup_or_add(key, samples.size());
            if (sample == samples.size()) {
              samples.append({parameter, limits});
            }
            corner_samples[i * 3 + j] = sample;
          }
        }
        /* Validate once per support and reuse U/V basis and derivative weights across its grid.
         * This is sequential; it avoids repeating the same basis work at each UV combination. */
        const auto differentials = evaluate_surface_differentials(surface, samples, r_error);
        if (!differentials) {
          return std::nullopt;
        }
        normal_evaluations += samples.size();
        Vector<std::optional<float3>> source_normals;
        source_normals.reserve(samples.size());
        for (const int sample : samples.index_range()) {
          const auto &differential = (*differentials)[sample];
          const auto normal = differential.normal ?
                                  differential.normal :
                                  evaluate_surface_pole_normal(
                                      surface, samples[sample].parameter, samples[sample].limits);
          source_normals.append(normal ? std::optional(float3(*normal * face.orientation)) :
                                         std::nullopt);
        }
        for (const int i : *triangles) {
          for (const int j : IndexRange(3)) {
            const auto &normal = source_normals[corner_samples[i * 3 + j]];
            if (normal) {
              normals[i * 3 + j] = *normal;
            }
            else {
              result.undefined_normal_corners++;
            }
          }
        }
      }
    }
    auto attributes = mesh.attributes_for_write();
    auto materials = attributes.lookup_or_add_for_write_span<int>("material_index",
                                                                  AttrDomain::Face);
    auto sharp_faces = attributes.lookup_or_add_for_write_span<bool>("sharp_face",
                                                                     AttrDomain::Face);
    for (const int i : result.mesh_to_preview_triangle.index_range()) {
      const auto &triangle = result.preview->triangles[result.mesh_to_preview_triangle[i]];
      materials.span[i] = index->face(triangle.face_id)->material_index;
      sharp_faces.span[i] = false;
    }
    materials.finish();
    sharp_faces.finish();
    mesh_calc_edges(mesh, true, false);
    if (result.undefined_normal_corners == 0) {
      /* Blender's free corner normals accept these already normalized source vectors directly.
       * Encoding corner-fan spaces would calculate mesh normals, split fans, and quantize the
       * values only for the renderer to decode them again. */
      auto custom_normals = attributes.lookup_or_add_for_write_span<float3>("custom_normal",
                                                                            AttrDomain::Corner);
      custom_normals.span.copy_from(normals);
      custom_normals.finish();
    }
    else {
      /* Zero source frames retain Blender's existing automatic-normal fallback semantics. */
      mesh_set_custom_normals_normalized(mesh, normals);
    }
    result.corner_normals.extend(normals.as_span());
    normal_ms = (BLI_time_now_seconds() - normal_start) * 1000.0;

    /* Use the mesh renderer's existing optimal-display pipeline, also used by subdivision.
     * The evaluated mesh remains triangulated for shading/picking; its wire display follows
     * each source face's preview grid. Boundary and loose source edges stay visible. */
    mesh.runtime->subsurf_optimal_display_edges.resize(mesh.edges_num, false);
    for (const int i : mesh.edges().index_range()) {
      const int2 edge = mesh.edges()[i];
      mesh.runtime->subsurf_optimal_display_edges[i].set(
          edge_keys.contains(int2(std::min(edge.x, edge.y), std::max(edge.x, edge.y))));
    }
    const Span<int> corner_edges = mesh.corner_edges();
    for (const int i : result.mesh_to_preview_triangle.index_range()) {
      for (const int corner : IndexRange(3)) {
        if (result.preview->triangles[result.mesh_to_preview_triangle[i]].display_edges[corner]) {
          mesh.runtime->subsurf_optimal_display_edges[corner_edges[3 * i + corner]].set();
        }
      }
    }
  }
  mesh_build_ms = (BLI_time_now_seconds() - mesh_build_start) * 1000.0;
  NURBS_DEBUG_LOG(
      "mesh eval %s: %.3f ms total; %.3f preview, %.3f positions, %.3f triangle filter, "
      "%.3f edges, %.3f mesh build, %.3f normals; %d faces, %d boundaries, %d preview "
      "points, %d triangles, %d display triangles, %d source normal evaluations, normals "
      "%s, %d degenerate triangles",
      reused ? "cache hit" : "cache miss",
      (BLI_time_now_seconds() - evaluation_start) * 1000.0,
      (mesh_build_start - evaluation_start) * 1000.0,
      position_ms,
      triangle_filter_ms,
      edge_build_ms,
      mesh_build_ms,
      normal_ms,
      source.geometry().faces.count(),
      int(result.preview->boundary.edges.size()),
      result.preview->points_num(),
      int(result.preview->triangles.size()),
      faces_num,
      normal_evaluations,
      result.reused_normals ? "reused" : "recalculated",
      result.skipped_degenerate_float_triangles);
  return result;
}

}  // namespace blender::bke::nurbs
