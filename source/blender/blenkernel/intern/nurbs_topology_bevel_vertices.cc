/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>
#include <limits>

#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_evaluate.hh"
#include "BKE_nurbs_refine.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BLI_math_vector.hh"
#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs {

std::optional<TopologyEditResult> bevel_curve_vertices(const NurbsGeometry &source,
                                                       const Span<int64_t> vertices,
                                                       const BevelOptions &options,
                                                       std::string *r_error,
                                                       BevelOffsetInfo *r_offset_info)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<TopologyEditResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (vertices.is_empty()) {
    return fail("Select a NURBS curve corner vertex to bevel");
  }
  if (!std::isfinite(options.offset) || options.offset < 0 ||
      !std::isfinite(options.max_error) || options.max_error < 0 || options.segments < 0 ||
      options.segments > 100)
  {
    return fail("NURBS corner bevel needs a finite nonnegative width and valid segment count");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  for (const auto &curve : source.curves) {
    curves.add_new(curve.id, &curve);
  }
  struct Corner {
    int64_t vertex, incident[2];
    double3 point, direction[2];
    double weight;
    Vector<int64_t> arc_edges;
  };
  Vector<Corner> corners;
  Set<int64_t> selected, affected_edges, affected_faces;
  for (const int64_t id : vertices) {
    if (!selected.add(id)) {
      continue;
    }
    const auto *vertex = index->vertex(id);
    if (!vertex) {
      return fail("NURBS corner bevel needs existing vertices");
    }
    const auto incident = index->edges_for_vertex(id);
    /* Endpoints and smooth subdivision stations are harmless in a larger selection. */
    if (incident.size() < 2) {
      continue;
    }
    if (incident.size() != 2) {
      return fail(
          "NURBS curve corner bevel requires two incident edges; branching corners need a surface "
          "bevel");
    }
    Corner corner{};
    corner.vertex = id;
    corner.point = double3(vertex->position);
    for (const int side : IndexRange(2)) {
      const auto &edge = *index->edge(incident[side]);
      const auto &curve = *curves.lookup(edge.curve_id);
      if (edge.vertex_ids[0] == edge.vertex_ids[1] || curve.order != 2 || curve.points_num != 2 ||
          curve.points[0].weight != curve.points[1].weight ||
          curve.knots[1] != edge.parameter_range[0] || curve.knots[2] != edge.parameter_range[1])
      {
        return fail("NURBS corner bevel currently requires two straight incident edges");
      }
      const int64_t other = edge.vertex_ids[edge.vertex_ids[0] == id ? 1 : 0];
      const double3 delta = double3(index->vertex(other)->position) - corner.point;
      const double length = math::length(delta);
      if (!(length > 0) || !std::isfinite(length)) {
        return fail("NURBS corner bevel cannot round a zero-length edge");
      }
      corner.incident[side] = edge.id;
      corner.direction[side] = delta / length;
    }
    const double dot = std::clamp(math::dot(corner.direction[0], corner.direction[1]), -1.0, 1.0);
    if (dot < -1 + 1e-12) {
      continue;
    }
    if (dot > 1 - 1e-12) {
      return fail("NURBS corner bevel cannot round coincident curve directions");
    }
    /* Rational quadratic with tangent intersection at the old corner: exact circular fillet. */
    corner.weight = std::sqrt((1 - dot) / 2);
    corners.append(std::move(corner));
    for (const int64_t edge : incident) {
      affected_edges.add(edge);
      for (const auto &use : index->uses_for_edge(edge)) {
        affected_faces.add(use.face_id);
      }
    }
  }
  if (corners.is_empty()) {
    return fail("Select a corner between two straight NURBS edges");
  }
  double limit = std::numeric_limits<double>::max();
  for (const int64_t id : affected_edges) {
    const auto &edge = *index->edge(id);
    int count = 0;
    for (const auto &corner : corners) {
      count += edge.vertex_ids[0] == corner.vertex || edge.vertex_ids[1] == corner.vertex;
    }
    limit = std::min(limit,
                     math::distance(double3(index->vertex(edge.vertex_ids[0])->position),
                                    double3(index->vertex(edge.vertex_ids[1])->position)) /
                         count);
  }
  limit *= 1 - 1e-6;
  const double width = options.clamp_overlap ? std::min(options.offset, limit) : options.offset;
  if (width > limit) {
    return fail("NURBS corner bevel width overlaps neighboring corners");
  }
  if (r_offset_info) {
    *r_offset_info = {};
    r_offset_info->offset = width;
    r_offset_info->limit = limit;
  }
  TopologyEditResult result;
  if (width == 0) {
    result.geometry.reset(geometry_copy(&source));
    result.vertices.extend(vertices);
    return result;
  }
  const Vector<int64_t> faces(affected_faces.begin(), affected_faces.end());
  auto chart = reparameterize_affine_planes(source, faces, options.max_error, r_error);
  if (!chart) {
    return std::nullopt;
  }
  result.geometry_error_bound = chart->geometry_error_bound;
  detail::TopologyBuilder builder(*chart->geometry);
  auto add_vertex = [&](const double3 &p) {
    auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
    vertex->id = builder.id();
    std::copy_n(&p.x, 3, vertex->position);
    BLI_addtail(&builder.geometry->vertices, vertex);
    builder.vertices.add_new(vertex->id, vertex);
    result.vertices.append(vertex->id);
    return vertex->id;
  };
  for (auto &corner : corners) {
    int64_t ends[2];
    double3 points[2];
    for (const int side : IndexRange(2)) {
      points[side] = corner.point + width * corner.direction[side];
      ends[side] = add_vertex(points[side]);
      auto &edge = *builder.edges.lookup(corner.incident[side]);
      edge.vertex_ids[edge.vertex_ids[0] == corner.vertex ? 0 : 1] = ends[side];
    }
    auto curve = OwnedCurve(MEM_new<NurbsCurveGeometry>(__func__));
    curve->dimension = 3;
    curve->order = curve->points_num = options.segments == 0 ? 2 : 3;
    curve->knots_num = 2 * curve->order;
    curve->points = MEM_new_array<NurbsControlPoint>(curve->points_num, __func__);
    curve->knots = MEM_new_array_zeroed<double>(curve->knots_num, __func__);
    std::fill_n(curve->knots + curve->order, curve->order, 1.0);
    std::copy_n(&points[0].x, 3, curve->points[0].position);
    std::copy_n(&points[1].x, 3, curve->points[curve->points_num - 1].position);
    if (options.segments > 0) {
      std::copy_n(&corner.point.x, 3, curve->points[1].position);
      curve->points[1].weight = corner.weight;
    }
    const int parts = std::max(1, options.segments);
    int64_t start = ends[0];
    for (const int i : IndexRange(parts)) {
      auto part = restrict_curve(*curve,
                                 {double(i) / parts, double(i + 1) / parts},
                                 options.max_error,
                                 r_error);
      if (!part) {
        return std::nullopt;
      }
      result.geometry_error_bound = std::max(result.geometry_error_bound,
                                             part->geometry_error_bound);
      auto *arc = part->curve.release();
      arc->id = builder.id();
      BLI_addtail(&builder.geometry->curves, arc);
      builder.curves.add_new(arc->id, arc);
      const int64_t end = i + 1 == parts ?
                              ends[1] :
                              add_vertex(double3(arc->points[arc->points_num - 1].position));
      std::copy_n(builder.vertices.lookup(start)->position, 3, arc->points[0].position);
      std::copy_n(
          builder.vertices.lookup(end)->position, 3, arc->points[arc->points_num - 1].position);
      auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
      edge->id = builder.id();
      edge->curve_id = arc->id;
      edge->vertex_ids[0] = start;
      edge->vertex_ids[1] = end;
      edge->parameter_range[0] = arc->knots[arc->order - 1];
      edge->parameter_range[1] = arc->knots[arc->points_num];
      BLI_addtail(&builder.geometry->edges, edge);
      builder.edges.add_new(edge->id, edge);
      corner.arc_edges.append(edge->id);
      result.edges.append(edge->id);
      start = end;
    }
    result.source_to_result.add_new(corner.vertex, ends[0]);
  }
  for (const int64_t id : affected_edges) {
    auto &edge = *builder.edges.lookup(id);
    auto *line = builder.copy_curve(edge.curve_id);
    edge.curve_id = line->id;
    for (const int side : IndexRange(2)) {
      std::copy_n(builder.vertices.lookup(edge.vertex_ids[side])->position,
                  3,
                  line->points[side].position);
    }
  }
  for (const int64_t face_id : faces) {
    auto &face = *builder.faces.lookup(face_id);
    const auto &s = *builder.surfaces.lookup(face.surface_id);
    const double3 origin(s.points[0].position);
    const double3 u = double3(s.points[1].position) - origin;
    const double3 v = double3(s.points[2].position) - origin;
    const double uu = math::dot(u, u), uv = math::dot(u, v), vv = math::dot(v, v);
    const double det = uu * vv - uv * uv;
    if (!(det > 0)) {
      return fail("NURBS corner bevel requires a regular planar face chart");
    }
    for (auto &loop : face.loops) {
      Vector<NurbsTopologyEdgeUse *> old_uses;
      for (auto &use : loop.edge_uses) {
        old_uses.append(&use);
      }
      for (auto *use : old_uses) {
        const auto &edge = *index->edge(use->edge_id);
        const int64_t end = edge.vertex_ids[use->reversed ? 0 : 1];
        for (const auto &corner : corners) {
          if (end != corner.vertex) {
            continue;
          }
          const bool reverse = use->edge_id != corner.incident[0];
          NurbsTopologyEdgeUse *previous = use;
          for (const int j : corner.arc_edges.index_range()) {
            auto *added = MEM_new<NurbsTopologyEdgeUse>(__func__);
            added->id = builder.id();
            added->edge_id = corner.arc_edges[reverse ? corner.arc_edges.size() - 1 - j : j];
            added->reversed = reverse;
            BLI_insertlinkafter(&loop.edge_uses, previous, added);
            previous = added;
          }
        }
      }
      for (auto &use : loop.edge_uses) {
        if (!affected_edges.contains(use.edge_id) && use.pcurve_id != 0) {
          continue;
        }
        const auto &edge = *builder.edges.lookup(use.edge_id);
        auto *pcurve = builder.copy_curve(edge.curve_id);
        pcurve->dimension = 2;
        for (auto &p : MutableSpan(pcurve->points, pcurve->points_num)) {
          const double3 delta = double3(p.position) - origin;
          const double du = math::dot(delta, u), dv = math::dot(delta, v);
          p.position[0] = std::lerp(
              s.knots_u[s.order_u - 1], s.knots_u[s.points_u], (du * vv - dv * uv) / det);
          p.position[1] = std::lerp(
              s.knots_v[s.order_v - 1], s.knots_v[s.points_v], (dv * uu - du * uv) / det);
          p.position[2] = 0;
        }
        use.pcurve_id = pcurve->id;
      }
    }
  }
  for (const auto &corner : corners) {
    auto *vertex = builder.vertices.lookup(corner.vertex);
    BLI_remlink(&builder.geometry->vertices, vertex);
    MEM_delete(vertex);
  }
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry, r_error) ||
      !evaluate_boundaries(*builder.geometry, {}, r_error))
  {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

}  // namespace blender::bke::nurbs
