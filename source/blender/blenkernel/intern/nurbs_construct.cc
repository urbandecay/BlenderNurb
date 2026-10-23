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

#include "BKE_nurbs_construct.hh"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {

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

std::optional<EdgeExtrusionResult> extrude_edges(const NurbsGeometry &source,
                                                 const Span<int64_t> edge_ids,
                                                 const double3 &offset,
                                                 const double max_translation_error,
                                                 std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<EdgeExtrusionResult> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (edge_ids.is_empty() || !std::isfinite(max_translation_error) ||
      max_translation_error < 0.0 || !std::isfinite(offset.x) || !std::isfinite(offset.y) ||
      !std::isfinite(offset.z))
  {
    return fail(
        "NURBS edge extrusion needs edges, a finite offset and a non-negative error budget");
  }
  if (!detail::supported_arithmetic()) {
    return fail(
        "NURBS ruled construction requires round-to-nearest arithmetic and subnormal support");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  auto cap = copy_component_closure(source, {{}, edge_ids, {}}, r_error);
  if (!cap) {
    return std::nullopt;
  }
  const uint64_t extra_records = uint64_t(cap->geometry->vertices.count()) * 2 +
                                 uint64_t(cap->geometry->edges.count()) * 11;
  if (extra_records > uint64_t(std::numeric_limits<int64_t>::max() - cap->geometry->next_id)) {
    return fail("NURBS identity space is exhausted");
  }
  Map<int64_t, const NurbsCurveGeometry *> source_curves;
  EdgeExtrusionResult result;
  for (const NurbsCurveGeometry &curve : source.curves) {
    source_curves.add_new(curve.id, &curve);
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (!cap->source_to_copy.contains(edge.id)) {
      continue;
    }
    const NurbsCurveGeometry &curve = *source_curves.lookup(edge.curve_id);
    if (curve.points_num > std::numeric_limits<int>::max() / 2) {
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

  for (NurbsCurveGeometry &curve : cap->geometry->curves) {
    for (int i = 0; i < curve.points_num; i++) {
      if (curve.points[i].weight <= 0.0) {
        return fail("NURBS ruled construction currently requires positive rational weights");
      }
      if (!detail::translate_position(
              curve.points[i].position, offset, result.translation_error_bound))
      {
        return fail("NURBS coefficient translation overflowed");
      }
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
  result.geometry.reset(geometry_copy(&source));
  NurbsGeometry &geometry = *result.geometry;
  geometry.next_id = cap->geometry->next_id;
  result.source_to_cap = std::move(cap->source_to_copy);
  BLI_movelisttolist(&geometry.curves, &cap->geometry->curves);
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
    const int64_t *cap_id = result.source_to_cap.lookup_ptr(vertex.id);
    if (cap_id == nullptr) {
      continue;
    }
    const NurbsTopologyVertex &top = *vertices.lookup(*cap_id);
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
    const int64_t *top_id = result.source_to_cap.lookup_ptr(edge.id);
    if (top_id == nullptr) {
      continue;
    }
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
      const int orientation = index->face(reference.face_id)->orientation *
                              (reference.use->reversed ? -1 : 1);
      if (source_orientation && *source_orientation != orientation) {
        conflicting_orientations = true;
      }
      source_orientation = orientation;
    }
    if (source_orientation && !conflicting_orientations) {
      face->orientation = -*source_orientation;
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
    add_use(*top_id, {a, 1, 0}, {b, 1, 0}, a, b, true);
    add_use(result.vertex_to_rail.lookup(edge.vertex_ids[0]), {a, 0, 0}, {a, 1, 0}, 0, 1, true);
  }
  if (!geometry_validate_structure(geometry, r_error)) {
    return std::nullopt;
  }
  return result;
}

}  // namespace blender::bke::nurbs
