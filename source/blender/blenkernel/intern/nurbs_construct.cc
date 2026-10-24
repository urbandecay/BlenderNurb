/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <limits>

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_nurbs_construct.hh"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {

std::optional<FaceRegionExtrusionPlan> plan_face_region_extrusion(const NurbsGeometry &source,
                                                                  const Span<int64_t> face_ids,
                                                                  std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<FaceRegionExtrusionPlan> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  if (face_ids.is_empty()) {
    return fail("NURBS region extrusion requires selected faces");
  }
  Set<int64_t> selected, interior;
  for (const int64_t id : face_ids) {
    if (index->face(id) == nullptr) {
      return fail("Selected NURBS face identity does not exist");
    }
    selected.add(id);
  }
  FaceRegionExtrusionPlan result;
  for (const NurbsTopologyFace &face : source.faces) {
    if (selected.contains(face.id)) {
      result.faces.append(face.id);
    }
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    int selected_uses = 0;
    bool unselected_use = false;
    for (const EdgeUseReference &reference : index->uses_for_edge(edge.id)) {
      if (selected.contains(reference.face_id)) {
        selected_uses++;
      }
      else {
        unselected_use = true;
      }
    }
    if (selected_uses > 2) {
      return fail("NURBS region extrusion does not support more than two selected edge uses");
    }
    if (selected_uses == 0) {
      continue;
    }
    result.remove_original_faces |= unselected_use;
    if (selected_uses == 1) {
      result.boundary_edges.append(edge.id);
    }
    else if (!unselected_use) {
      result.interior_edges.append(edge.id);
      interior.add(edge.id);
    }
  }
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    const Span<int64_t> edges = index->edges_for_vertex(vertex.id);
    if (!edges.is_empty() && std::all_of(edges.begin(), edges.end(), [&](const int64_t id) {
          return interior.contains(id);
        }))
    {
      result.interior_vertices.append(vertex.id);
    }
  }
  return result;
}

static double distance(const double3 &a, const double3 &b)
{
  const double3 d = a - b;
  return std::hypot(d.x, d.y, d.z);
}

static NurbsCurveGeometry *add_line(NurbsGeometry &geometry,
                                    const double3 &a,
                                    const double3 &b,
                                    const double start,
                                    const double end,
                                    const int dimension)
{
  auto *curve = MEM_new<NurbsCurveGeometry>(__func__);
  curve->id = geometry.next_id++;
  curve->order = curve->points_num = 2;
  curve->knots_num = 4;
  curve->dimension = dimension;
  curve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
  curve->knots = MEM_new_array_uninitialized<double>(4, __func__);
  curve->knots[0] = curve->knots[1] = start;
  curve->knots[2] = curve->knots[3] = end;
  for (int axis = 0; axis < 3; axis++) {
    curve->points[0].position[axis] = a[axis];
    curve->points[1].position[axis] = b[axis];
  }
  BLI_addtail(&geometry.curves, curve);
  return curve;
}

std::optional<ExtrusionResult> extrude_components(const NurbsGeometry &source,
                                                  const ComponentSelection &selection,
                                                  const double3 &offset,
                                                  const double max_translation_error,
                                                  std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<ExtrusionResult> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if ((selection.vertices.is_empty() && selection.edges.is_empty() &&
       selection.faces.is_empty()) ||
      !std::isfinite(max_translation_error) || max_translation_error < 0.0 ||
      !std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
  {
    return fail(
        "NURBS extrusion needs components, a finite offset and a non-negative error budget");
  }
  if (!detail::supported_arithmetic()) {
    return fail(
        "NURBS ruled construction requires round-to-nearest arithmetic and subnormal support");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  auto cap = copy_component_closure(source, selection, r_error);
  if (!cap) {
    return std::nullopt;
  }
  std::optional<FaceRegionExtrusionPlan> region;
  Set<int64_t> selected_faces, region_edges, swept_edges, covered_vertices, railed_vertices;
  if (!selection.faces.is_empty()) {
    region = plan_face_region_extrusion(source, selection.faces, r_error);
    if (!region) {
      return std::nullopt;
    }
    selected_faces.add_multiple(region->faces);
    swept_edges.add_multiple(region->boundary_edges);
    for (const int64_t id : region->faces) {
      for (const NurbsTopologyLoop &loop : index->face(id)->loops) {
        for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
          region_edges.add(use.edge_id);
        }
      }
    }
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (!cap->source_to_copy.contains(edge.id)) {
      continue;
    }
    covered_vertices.add_multiple(Span(edge.vertex_ids, 2));
    if (!region_edges.contains(edge.id)) {
      swept_edges.add(edge.id);
    }
    if (swept_edges.contains(edge.id)) {
      railed_vertices.add_multiple(Span(edge.vertex_ids, 2));
    }
  }
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (cap->source_to_copy.contains(vertex.id) && !covered_vertices.contains(vertex.id)) {
      railed_vertices.add(vertex.id);
    }
  }
  const uint64_t available = uint64_t(std::numeric_limits<int64_t>::max() -
                                      cap->geometry->next_id);
  const uint64_t rails = uint64_t(railed_vertices.size());
  if (rails > available / 2 || uint64_t(swept_edges.size()) > (available - rails * 2) / 11) {
    return fail("NURBS identity space is exhausted");
  }
  Map<int64_t, const NurbsCurveGeometry *> source_curves;
  ExtrusionResult result;
  for (const NurbsCurveGeometry &curve : source.curves) {
    source_curves.add_new(curve.id, &curve);
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (!cap->source_to_copy.contains(edge.id)) {
      continue;
    }
    const NurbsCurveGeometry &curve = *source_curves.lookup(edge.curve_id);
    if (swept_edges.contains(edge.id) && curve.points_num > std::numeric_limits<int>::max() / 2) {
      return fail("NURBS ruled support size is not representable");
    }
    for (int i = 0; i < 2; i++) {
      const auto endpoint = evaluate_curve(curve, edge.parameter_range[i], r_error);
      if (!endpoint) {
        return std::nullopt;
      }
      const NurbsTopologyVertex &vertex = *index->vertex(edge.vertex_ids[i]);
      const double deviation = distance(endpoint->position, double3(vertex.position));
      result.max_source_endpoint_deviation = std::max(result.max_source_endpoint_deviation,
                                                      deviation);
      if (deviation > std::max(edge.tolerance, vertex.tolerance)) {
        return fail("NURBS source curve endpoint disagrees with its topology vertex tolerance");
      }
    }
  }

  auto translate_points = [&](NurbsControlPoint *points, const int count) {
    for (int i = 0; i < count; i++) {
      if (points[i].weight <= 0.0 ||
          !detail::translate_position(points[i].position, offset, result.translation_error_bound))
      {
        return false;
      }
    }
    return true;
  };
  for (NurbsCurveGeometry &curve : cap->geometry->curves) {
    if (curve.dimension == 3 && !translate_points(curve.points, curve.points_num)) {
      return fail(
          "NURBS extrusion needs positive curve weights and finite translation arithmetic");
    }
  }
  for (NurbsSurfaceGeometry &surface : cap->geometry->surfaces) {
    if (!translate_points(surface.points, surface.points_num)) {
      return fail(
          "NURBS extrusion needs positive support weights and finite translation arithmetic");
    }
  }
  for (NurbsTopologyVertex &vertex : cap->geometry->vertices) {
    if (!detail::translate_position(vertex.position, offset, result.translation_error_bound)) {
      return fail("NURBS vertex translation overflowed");
    }
  }
  if (result.translation_error_bound > max_translation_error) {
    return fail("NURBS translation roundoff bound exceeds the requested error budget");
  }
  /* Allow for independent coefficient/endpoint additions when propagating source agreement. */
  const double tolerance_extra = 2.0 * result.translation_error_bound;
  for (NurbsTopologyVertex &vertex : cap->geometry->vertices) {
    vertex.tolerance = detail::expand_tolerance(vertex.tolerance, tolerance_extra);
  }
  for (NurbsTopologyEdge &edge : cap->geometry->edges) {
    edge.tolerance = detail::expand_tolerance(edge.tolerance, tolerance_extra);
  }
  for (NurbsTopologyFace &face : cap->geometry->faces) {
    face.tolerance = detail::expand_tolerance(face.tolerance, tolerance_extra);
    for (NurbsTopologyLoop &loop : face.loops) {
      for (NurbsTopologyEdgeUse &use : loop.edge_uses) {
        use.tolerance = detail::expand_tolerance(use.tolerance, tolerance_extra);
      }
    }
  }
  result.geometry.reset(geometry_copy(&source));
  NurbsGeometry &geometry = *result.geometry;
  geometry.next_id = cap->geometry->next_id;
  result.source_to_cap = std::move(cap->source_to_copy);
  BLI_movelisttolist(&geometry.curves, &cap->geometry->curves);
  BLI_movelisttolist(&geometry.surfaces, &cap->geometry->surfaces);
  BLI_movelisttolist(&geometry.faces, &cap->geometry->faces);
  BLI_movelisttolist(&geometry.vertices, &cap->geometry->vertices);
  BLI_movelisttolist(&geometry.edges, &cap->geometry->edges);
  Map<int64_t, NurbsCurveGeometry *> curves;
  Map<int64_t, NurbsTopologyVertex *> vertices;
  for (NurbsCurveGeometry &curve : geometry.curves) {
    curves.add_new(curve.id, &curve);
  }
  for (NurbsTopologyVertex &vertex : geometry.vertices) {
    vertices.add_new(vertex.id, &vertex);
  }
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (!railed_vertices.contains(vertex.id)) {
      continue;
    }
    const NurbsTopologyVertex &top = *vertices.lookup(result.source_to_cap.lookup(vertex.id));
    const int64_t curve_id =
        add_line(geometry, double3(vertex.position), double3(top.position), 0, 1, 3)->id;
    auto *rail = MEM_new<NurbsTopologyEdge>(__func__);
    rail->id = geometry.next_id++;
    rail->curve_id = curve_id;
    rail->vertex_ids[0] = vertex.id;
    rail->vertex_ids[1] = top.id;
    rail->parameter_range[1] = 1.0;
    rail->tolerance = std::max(vertex.tolerance, top.tolerance);
    result.vertex_to_rail.add_new(vertex.id, rail->id);
    BLI_addtail(&geometry.edges, rail);
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (!swept_edges.contains(edge.id)) {
      continue;
    }
    const int64_t top_id = result.source_to_cap.lookup(edge.id);
    const NurbsCurveGeometry &bottom = *source_curves.lookup(edge.curve_id);
    const NurbsCurveGeometry &top = *curves.lookup(result.source_to_cap.lookup(bottom.id));
    auto *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
    surface->id = geometry.next_id++;
    surface->order_u = bottom.order;
    surface->order_v = surface->points_v = 2;
    surface->points_u = bottom.points_num;
    surface->points_num = 2 * bottom.points_num;
    surface->knots_u_num = bottom.knots_num;
    surface->knots_v_num = 4;
    surface->knots_u = MEM_dupalloc(bottom.knots);
    surface->knots_v = MEM_new_array_zeroed<double>(4, __func__);
    surface->knots_v[2] = surface->knots_v[3] = 1.0;
    surface->points = MEM_new_array<NurbsControlPoint>(surface->points_num, __func__);
    for (int i = 0; i < bottom.points_num; i++) {
      surface->points[i] = bottom.points[i];
      surface->points[bottom.points_num + i] = top.points[i];
    }
    BLI_addtail(&geometry.surfaces, surface);
    auto *face = MEM_new<NurbsTopologyFace>(__func__);
    face->id = geometry.next_id++;
    face->surface_id = surface->id;
    std::optional<int> source_orientation;
    bool conflicting_orientations = false;
    for (const EdgeUseReference &reference : index->uses_for_edge(edge.id)) {
      const bool region_boundary = region_edges.contains(edge.id);
      if (region_boundary && !selected_faces.contains(reference.face_id)) {
        continue;
      }
      const int sense = index->face(reference.face_id)->orientation *
                        (reference.use->reversed ? -1 : 1);
      const int orientation = region_boundary ? sense : -sense;
      if (source_orientation && *source_orientation != orientation) {
        conflicting_orientations = true;
      }
      source_orientation = orientation;
    }
    if (source_orientation && !conflicting_orientations) {
      face->orientation = *source_orientation;
    }
    face->tolerance = detail::expand_tolerance(
        std::max({edge.tolerance,
                  index->vertex(edge.vertex_ids[0])->tolerance,
                  index->vertex(edge.vertex_ids[1])->tolerance}),
        tolerance_extra);
    BLI_addtail(&geometry.faces, face);
    result.edge_to_face.add_new(edge.id, face->id);
    auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
    loop->id = geometry.next_id++;
    BLI_addtail(&face->loops, loop);
    const double a = edge.parameter_range[0], b = edge.parameter_range[1];
    auto add_use = [&](const int64_t edge_id,
                       const double3 &p0,
                       const double3 &p1,
                       const double start,
                       const double end,
                       const bool reversed) {
      auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
      use->id = geometry.next_id++;
      use->edge_id = edge_id;
      use->pcurve_id = add_line(geometry, p0, p1, start, end, 2)->id;
      use->reversed = reversed;
      use->tolerance = face->tolerance;
      BLI_addtail(&loop->edge_uses, use);
    };
    add_use(edge.id, {a, 0, 0}, {b, 0, 0}, a, b, false);
    add_use(result.vertex_to_rail.lookup(edge.vertex_ids[1]), {b, 0, 0}, {b, 1, 0}, 0, 1, false);
    add_use(top_id, {a, 1, 0}, {b, 1, 0}, a, b, true);
    add_use(result.vertex_to_rail.lookup(edge.vertex_ids[0]), {a, 0, 0}, {a, 1, 0}, 0, 1, true);
  }
  if (region) {
    if (region->remove_original_faces) {
      auto deleted = delete_components(
          geometry, {{}, {}, region->faces}, DeleteMode::Faces, r_error);
      if (!deleted) {
        return std::nullopt;
      }
      result.removed = std::move(deleted->removed);
      result.geometry = std::move(deleted->geometry);
    }
    else {
      for (NurbsTopologyFace &face : geometry.faces) {
        if (selected_faces.contains(face.id)) {
          face.orientation = -face.orientation;
        }
      }
    }
  }
  if (!geometry_validate_structure(*result.geometry, r_error)) {
    return std::nullopt;
  }
  return result;
}

std::optional<EdgeExtrusionResult> extrude_edges(const NurbsGeometry &source,
                                                 const Span<int64_t> edge_ids,
                                                 const double3 &offset,
                                                 const double max_translation_error,
                                                 std::string *r_error)
{
  return extrude_components(source, {{}, edge_ids, {}}, offset, max_translation_error, r_error);
}

}  // namespace blender::bke::nurbs
