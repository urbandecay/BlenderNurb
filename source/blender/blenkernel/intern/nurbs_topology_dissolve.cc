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
#include "BLI_math_vector.hh"

#include "BKE_nurbs_evaluate.hh"
#include "BKE_nurbs_topology_edit.hh"

#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs {
namespace {

struct BoundaryUse {
  int64_t edge_id;
  int64_t start_vertex;
  int64_t end_vertex;
  bool reversed;
  double tolerance;
};

static double3 vertex_position(const NurbsTopologyVertex &vertex)
{
  return double3(vertex.position);
}

static const NurbsSurfaceGeometry *surface_by_id(const NurbsGeometry &geometry, const int64_t id)
{
  for (const NurbsSurfaceGeometry &surface : geometry.surfaces) {
    if (surface.id == id) {
      return &surface;
    }
  }
  return nullptr;
}

static const NurbsCurveGeometry *curve_by_id(const NurbsGeometry &geometry, const int64_t id)
{
  for (const NurbsCurveGeometry &curve : geometry.curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

static bool planar_patch_normal(const NurbsTopologyFace &face,
                                const NurbsSurfaceGeometry &surface,
                                const double tolerance,
                                double3 &r_normal)
{
  if (surface.points_num != 4 || surface.points_u != 2 || surface.points_v != 2 ||
      surface.order_u != 2 || surface.order_v != 2 || surface.knots_u_num != 4 ||
      surface.knots_v_num != 4)
  {
    return false;
  }
  const double2 center((surface.knots_u[1] + surface.knots_u[2]) * 0.5,
                       (surface.knots_v[1] + surface.knots_v[2]) * 0.5);
  const auto differential = evaluate_surface_differential(surface, center);
  if (!differential || !differential->normal) {
    return false;
  }
  r_normal = *differential->normal * double(face.orientation);
  const double3 origin(surface.points[0].position);
  for (int i = 0; i < surface.points_num; i++) {
    if (std::abs(math::dot(r_normal, double3(surface.points[i].position) - origin)) > tolerance) {
      return false;
    }
  }
  return true;
}

static std::optional<Vector<BoundaryUse>> face_boundary_without_edge(
    const NurbsTopologyFace &face,
    const int64_t removed_edge,
    const TopologyIndex &index,
    std::string *r_error)
{
  if (!face.loops.is_single()) {
    if (r_error) {
      *r_error = "Limited Dissolve needs adjacent NURBS faces with one outer boundary each";
    }
    return std::nullopt;
  }
  const NurbsTopologyLoop &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
  if (loop.is_inner || loop.edge_uses.count() != 4) {
    if (r_error) {
      *r_error = "Limited Dissolve currently supports four-sided NURBS source patches";
    }
    return std::nullopt;
  }
  Vector<BoundaryUse> uses;
  int removed_index = -1;
  for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
    const NurbsTopologyEdge *edge = index.edge(use.edge_id);
    if (!edge) {
      if (r_error) {
        *r_error = "Limited Dissolve found an invalid NURBS edge reference";
      }
      return std::nullopt;
    }
    const bool reversed = use.reversed != 0;
    uses.append({edge->id,
                 edge->vertex_ids[reversed ? 1 : 0],
                 edge->vertex_ids[reversed ? 0 : 1],
                 reversed,
                 use.tolerance});
    if (edge->id == removed_edge) {
      removed_index = uses.size() - 1;
    }
  }
  if (removed_index == -1) {
    if (r_error) {
      *r_error = "Limited Dissolve could not find the shared edge on both face boundaries";
    }
    return std::nullopt;
  }
  Vector<BoundaryUse> result;
  for (int offset = 1; offset < uses.size(); offset++) {
    result.append(uses[(removed_index + offset) % uses.size()]);
  }
  return result;
}

static std::optional<double2> boundary_uv(const double3 point,
                                          const std::array<double3, 4> &corners,
                                          const double tolerance)
{
  constexpr std::array<double2, 4> square = {
      double2(0, 0), double2(1, 0), double2(1, 1), double2(0, 1)};
  for (int side = 0; side < 4; side++) {
    const double3 start = corners[side];
    const double3 direction = corners[(side + 1) % 4] - start;
    const double length_squared = math::length_squared(direction);
    if (length_squared == 0) {
      continue;
    }
    const double parameter = math::dot(point - start, direction) / length_squared;
    if (parameter < 0 || parameter > 1) {
      continue;
    }
    const double3 closest = start + direction * parameter;
    if (math::distance(point, closest) <= tolerance) {
      return math::interpolate(square[side], square[(side + 1) % 4], parameter);
    }
  }
  return std::nullopt;
}

static void remove_face(NurbsGeometry &geometry, NurbsTopologyFace &face)
{
  for (NurbsTopologyLoop &loop : face.loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&face.loops);
  BLI_remlink(&geometry.faces, &face);
  MEM_delete(&face);
}

/** Remove redundant boundary stations only when no surviving wire or other face needs them. */
static void simplify_boundary(detail::TopologyBuilder &builder,
                              const TopologyIndex &source_index,
                              const int64_t shared_edge,
                              const int64_t face_a,
                              const int64_t face_b,
                              const Set<int64_t> &redundant_vertices,
                              Vector<BoundaryUse> &boundary,
                              TopologyEditResult &result)
{
  for (int i = 0; i < boundary.size();) {
    const int64_t vertex_id = boundary[i].start_vertex;
    if (!redundant_vertices.contains(vertex_id)) {
      i++;
      continue;
    }
    const int previous = (i + boundary.size() - 1) % boundary.size();
    const BoundaryUse before = boundary[previous], after = boundary[i];
    bool needed_elsewhere = false;
    for (const int64_t incident : source_index.edges_for_vertex(vertex_id)) {
      if (incident != shared_edge && incident != before.edge_id && incident != after.edge_id) {
        needed_elsewhere = true;
      }
    }
    for (const int64_t edge_id : {before.edge_id, after.edge_id}) {
      for (const EdgeUseReference &reference : source_index.uses_for_edge(edge_id)) {
        if (reference.face_id != face_a && reference.face_id != face_b) {
          needed_elsewhere = true;
        }
      }
    }
    if (needed_elsewhere) {
      i++;
      continue;
    }

    const NurbsTopologyEdge *old_a = builder.edges.lookup(before.edge_id);
    const NurbsTopologyEdge *old_b = builder.edges.lookup(after.edge_id);
    auto *curve = MEM_new<NurbsCurveGeometry>(__func__);
    curve->id = builder.id();
    curve->dimension = 3;
    curve->order = curve->points_num = 2;
    curve->knots_num = 4;
    curve->knots = MEM_new_array<double>(4, __func__);
    curve->knots[0] = curve->knots[1] = 0;
    curve->knots[2] = curve->knots[3] = 1;
    curve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
    std::copy_n(builder.vertices.lookup(before.start_vertex)->position,
                3,
                curve->points[0].position);
    std::copy_n(builder.vertices.lookup(after.end_vertex)->position,
                3,
                curve->points[1].position);
    BLI_addtail(&builder.geometry->curves, curve);
    builder.curves.add_new(curve->id, curve);

    auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
    edge->id = builder.id();
    edge->curve_id = curve->id;
    edge->vertex_ids[0] = before.start_vertex;
    edge->vertex_ids[1] = after.end_vertex;
    edge->parameter_range[0] = 0;
    edge->parameter_range[1] = 1;
    edge->tolerance = std::max(old_a->tolerance, old_b->tolerance);
    BLI_addtail(&builder.geometry->edges, edge);
    builder.edges.add_new(edge->id, edge);

    for (const int64_t old_id : {before.edge_id, after.edge_id}) {
      auto *old_edge = builder.edges.lookup(old_id);
      BLI_remlink(&builder.geometry->edges, old_edge);
      MEM_delete(old_edge);
      builder.edges.remove(old_id);
      result.source_to_result.add_overwrite(old_id, edge->id);
    }
    auto *vertex = builder.vertices.lookup(vertex_id);
    BLI_remlink(&builder.geometry->vertices, vertex);
    MEM_delete(vertex);
    builder.vertices.remove(vertex_id);
    result.source_to_result.add_overwrite(vertex_id, 0);

    boundary[previous] = {edge->id,
                          before.start_vertex,
                          after.end_vertex,
                          false,
                          std::max(before.tolerance, after.tolerance)};
    boundary.remove(i);
  }
}

static void append_boundary_uses(detail::TopologyBuilder &builder,
                                 NurbsTopologyLoop &loop,
                                 const Span<BoundaryUse> boundary,
                                 const Map<int64_t, double2> &vertex_uv,
                                 TopologyEditResult &result)
{
  for (const BoundaryUse &item : boundary) {
    NurbsTopologyEdge *edge = builder.edges.lookup(item.edge_id);
    NurbsCurveGeometry *edge_curve = builder.curves.lookup(edge->curve_id);
    auto *pcurve = MEM_new<NurbsCurveGeometry>(__func__);
    pcurve->id = builder.id();
    pcurve->dimension = 2;
    pcurve->order = pcurve->points_num = 2;
    pcurve->knots_num = 4;
    pcurve->knots = MEM_dupalloc(edge_curve->knots);
    pcurve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
    for (int endpoint = 0; endpoint < 2; endpoint++) {
      const double2 uv = vertex_uv.lookup(edge->vertex_ids[endpoint]);
      pcurve->points[endpoint].position[0] = uv.x;
      pcurve->points[endpoint].position[1] = uv.y;
      pcurve->points[endpoint].weight = edge_curve->points[endpoint].weight;
    }
    BLI_addtail(&builder.geometry->curves, pcurve);
    builder.curves.add_new(pcurve->id, pcurve);
    auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
    use->id = builder.id();
    use->edge_id = edge->id;
    use->pcurve_id = pcurve->id;
    use->tolerance = item.tolerance;
    use->reversed = item.reversed;
    BLI_addtail(&loop.edge_uses, use);
    result.edges.append(edge->id);
    result.vertices.append(item.start_vertex);
  }
}

/** Also clean up the six-sided face left by earlier versions of Limited Dissolve. */
static std::optional<TopologyEditResult> dissolve_boundary_vertices(
    const NurbsGeometry &source,
    const TopologyIndex &index,
    const NurbsTopologyFace &face,
    const SelectionState &selection,
    std::string *r_error)
{
  if (!face.loops.is_single()) {
    return std::nullopt;
  }
  const auto &source_loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
  const auto *surface = surface_by_id(source, face.surface_id);
  if (source_loop.is_inner || source_loop.edge_uses.count() <= 4 || !surface) {
    return std::nullopt;
  }
  double scale = 1;
  for (const auto &point : Span(surface->points, surface->points_num)) {
    scale = std::max(scale, math::length(double3(point.position)));
  }
  const double tolerance = std::max(face.tolerance,
                                     64 * std::numeric_limits<double>::epsilon() * scale);
  double3 normal;
  if (!planar_patch_normal(face, *surface, tolerance, normal)) {
    return std::nullopt;
  }
  for (const auto &point : Span(surface->points, surface->points_num)) {
    if (point.weight != surface->points[0].weight) {
      return std::nullopt;
    }
  }
  Vector<BoundaryUse> boundary;
  Map<int64_t, double2> vertex_uv;
  for (const auto &use : source_loop.edge_uses) {
    const auto *edge = index.edge(use.edge_id);
    const auto *curve = edge ? curve_by_id(source, edge->curve_id) : nullptr;
    const auto *pcurve = curve_by_id(source, use.pcurve_id);
    if (!edge || !curve || !pcurve || curve->order != 2 || curve->points_num != 2 ||
        curve->knots_num != 4 || pcurve->order != 2 || pcurve->points_num != 2 ||
        pcurve->knots_num != 4 || curve->knots[1] != edge->parameter_range[0] ||
        curve->knots[2] != edge->parameter_range[1] || curve->knots[0] != curve->knots[1] ||
        curve->knots[2] != curve->knots[3] || pcurve->knots[0] != pcurve->knots[1] ||
        pcurve->knots[2] != pcurve->knots[3])
    {
      return std::nullopt;
    }
    const double2 uv_a(pcurve->points[0].position), uv_b(pcurve->points[1].position);
    /* On a natural side of a bilinear patch, linear UV interpolation is also linear in 3D. */
    const bool natural_u = uv_a.x == uv_b.x &&
                           (uv_a.x == surface->knots_u[1] || uv_a.x == surface->knots_u[2]);
    const bool natural_v = uv_a.y == uv_b.y &&
                           (uv_a.y == surface->knots_v[1] || uv_a.y == surface->knots_v[2]);
    if (!natural_u && !natural_v) {
      return std::nullopt;
    }
    for (int endpoint = 0; endpoint < 2; endpoint++) {
      const double2 uv(pcurve->points[endpoint].position);
      const auto point = evaluate_surface(*surface, uv);
      if (!point || math::distance(point->position, double3(curve->points[endpoint].position)) >
                        tolerance)
      {
        return std::nullopt;
      }
      vertex_uv.add_overwrite(edge->vertex_ids[endpoint], uv);
    }
    boundary.append({edge->id,
                     edge->vertex_ids[use.reversed ? 1 : 0],
                     edge->vertex_ids[use.reversed ? 0 : 1],
                     use.reversed != 0,
                     use.tolerance});
  }
  Set<int64_t> redundant;
  for (const int i : boundary.index_range()) {
    const auto &before = boundary[(i + boundary.size() - 1) % boundary.size()];
    const auto &after = boundary[i];
    const int64_t vertex_id = after.start_vertex;
    if (!selection.faces.contains(face.id) && !selection.vertices.contains(vertex_id)) {
      continue;
    }
    const double3 previous = vertex_position(*index.vertex(before.start_vertex));
    const double3 current = vertex_position(*index.vertex(vertex_id));
    const double3 next = vertex_position(*index.vertex(after.end_vertex));
    const double3 first = current - previous, second = next - current;
    const double2 uv_first = vertex_uv.lookup(vertex_id) - vertex_uv.lookup(before.start_vertex);
    const double2 uv_second = vertex_uv.lookup(after.end_vertex) - vertex_uv.lookup(vertex_id);
    if (math::dot(first, second) > 0 &&
        math::length(math::cross(first, second)) <=
            tolerance * (math::length(first) + math::length(second)) &&
        ((uv_first.x == 0 && uv_second.x == 0) || (uv_first.y == 0 && uv_second.y == 0)))
    {
      redundant.add(vertex_id);
    }
  }
  if (redundant.is_empty()) {
    return std::nullopt;
  }
  detail::TopologyBuilder builder(source);
  TopologyEditResult result;
  simplify_boundary(builder, index, 0, face.id, 0, redundant, boundary, result);
  if (result.source_to_result.is_empty()) {
    return std::nullopt;
  }
  auto *loop = static_cast<NurbsTopologyLoop *>(builder.faces.lookup(face.id)->loops.first);
  BLI_freelistN(&loop->edge_uses);
  append_boundary_uses(builder, *loop, boundary, vertex_uv, result);
  result.faces.append(face.id);
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

static std::optional<TopologyEditResult> dissolve_edge(const NurbsGeometry &source,
                                                       const TopologyIndex &index,
                                                       const int64_t edge_id,
                                                       const double angle_limit,
                                                       std::string *r_error)
{
  const Span<EdgeUseReference> references = index.uses_for_edge(edge_id);
  if (references.size() != 2 || references[0].face_id == references[1].face_id) {
    return std::nullopt;
  }
  const NurbsTopologyFace *face_a = index.face(references[0].face_id);
  const NurbsTopologyFace *face_b = index.face(references[1].face_id);
  const NurbsTopologyEdge *shared_edge = index.edge(edge_id);
  if (!face_a || !face_b || !shared_edge) {
    return std::nullopt;
  }

  const NurbsSurfaceGeometry *surface_a = surface_by_id(source, face_a->surface_id);
  const NurbsSurfaceGeometry *surface_b = surface_by_id(source, face_b->surface_id);
  if (!surface_a || !surface_b) {
    return std::nullopt;
  }
  double scale = 0;
  double tolerance = std::max({face_a->tolerance, face_b->tolerance, shared_edge->tolerance});
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    tolerance = std::max(tolerance, vertex.tolerance);
  }
  for (const NurbsControlPoint &point : Span(surface_a->points, surface_a->points_num)) {
    scale = std::max(scale, math::length(double3(point.position)));
  }
  for (const NurbsControlPoint &point : Span(surface_b->points, surface_b->points_num)) {
    scale = std::max(scale, math::length(double3(point.position)));
  }
  tolerance = std::max(tolerance,
                       64.0 * std::numeric_limits<double>::epsilon() * std::max(scale, 1.0));
  double3 normal_a, normal_b;
  if (!planar_patch_normal(*face_a, *surface_a, tolerance, normal_a) ||
      !planar_patch_normal(*face_b, *surface_b, tolerance, normal_b))
  {
    if (r_error) {
      *r_error = "These NURBS patches need compatible support charts to dissolve their shared edge";
    }
    return std::nullopt;
  }
  const double angle = std::acos(std::clamp(math::dot(normal_a, normal_b), -1.0, 1.0));
  if (angle > angle_limit) {
    return std::nullopt;
  }
  const double3 plane_origin(surface_a->points[0].position);
  for (const NurbsControlPoint &point : Span(surface_b->points, surface_b->points_num)) {
    if (std::abs(math::dot(normal_a, double3(point.position) - plane_origin)) > tolerance) {
      return std::nullopt;
    }
  }

  auto path_a = face_boundary_without_edge(*face_a, edge_id, index, r_error);
  auto path_b = face_boundary_without_edge(*face_b, edge_id, index, r_error);
  if (!path_a || !path_b) {
    return std::nullopt;
  }
  const bool reversed_a = references[0].use->reversed != 0;
  const bool reversed_b = references[1].use->reversed != 0;
  const int64_t shared_start_a = shared_edge->vertex_ids[reversed_a ? 1 : 0];
  const int64_t shared_end_a = shared_edge->vertex_ids[reversed_a ? 0 : 1];
  const int64_t shared_start_b = shared_edge->vertex_ids[reversed_b ? 1 : 0];
  const int64_t shared_end_b = shared_edge->vertex_ids[reversed_b ? 0 : 1];
  if (shared_start_a != shared_end_b || shared_end_a != shared_start_b ||
      path_a->first().start_vertex != shared_end_a ||
      path_a->last().end_vertex != shared_start_a ||
      path_b->first().start_vertex != shared_end_b ||
      path_b->last().end_vertex != shared_start_b)
  {
    if (r_error) {
      *r_error = "Limited Dissolve found inconsistent NURBS face loop directions";
    }
    return std::nullopt;
  }
  Vector<BoundaryUse> boundary = std::move(*path_a);
  boundary.extend(*path_b);
  if (boundary.size() != 6) {
    return std::nullopt;
  }
  for (const int i : boundary.index_range()) {
    if (boundary[i].end_vertex != boundary[(i + 1) % boundary.size()].start_vertex) {
      if (r_error) {
        *r_error = "Limited Dissolve could not form one closed outer boundary";
      }
      return std::nullopt;
    }
    const NurbsTopologyEdge *edge = index.edge(boundary[i].edge_id);
    const NurbsCurveGeometry *curve = edge ? curve_by_id(source, edge->curve_id) : nullptr;
    if (!edge || !curve || curve->dimension != 3 || curve->order != 2 || curve->points_num != 2 ||
        curve->knots_num != 4 || curve->knots[1] != edge->parameter_range[0] ||
        curve->knots[2] != edge->parameter_range[1] || curve->knots[0] != curve->knots[1] ||
        curve->knots[2] != curve->knots[3])
    {
      if (r_error) {
        *r_error = "Limited Dissolve currently needs straight NURBS boundary edges";
      }
      return std::nullopt;
    }
    for (int p = 0; p < curve->points_num; p++) {
      if (std::abs(math::dot(normal_a, double3(curve->points[p].position) - plane_origin)) >
          tolerance)
      {
        if (r_error) {
          *r_error = "Limited Dissolve found a boundary edge outside the shared face plane";
        }
        return std::nullopt;
      }
    }
    const auto *start_vertex = index.vertex(edge->vertex_ids[0]);
    const auto *end_vertex = index.vertex(edge->vertex_ids[1]);
    if (!start_vertex || !end_vertex ||
        math::distance(double3(curve->points[0].position), vertex_position(*start_vertex)) >
            tolerance ||
        math::distance(double3(curve->points[1].position), vertex_position(*end_vertex)) >
            tolerance)
    {
      if (r_error) {
        *r_error = "Limited Dissolve found a boundary curve that does not match its vertices";
      }
      return std::nullopt;
    }
  }

  Vector<int64_t> polygon;
  for (const BoundaryUse &use : boundary) {
    polygon.append(use.start_vertex);
  }
  Vector<int> corners;
  for (const int i : polygon.index_range()) {
    corners.append(i);
  }
  bool removed_collinear = true;
  while (removed_collinear && corners.size() > 4) {
    removed_collinear = false;
    for (const int i : corners.index_range()) {
      const double3 previous = vertex_position(*index.vertex(polygon[corners[(i + corners.size() - 1) % corners.size()]]));
      const double3 current = vertex_position(*index.vertex(polygon[corners[i]]));
      const double3 next = vertex_position(*index.vertex(polygon[corners[(i + 1) % corners.size()]]));
      const double3 first = current - previous;
      const double3 second = next - current;
      const double lengths = math::length(first) + math::length(second);
      if (lengths > 0 && math::length(math::cross(first, second)) <= tolerance * lengths &&
          math::dot(first, second) >= 0)
      {
        corners.remove(i);
        removed_collinear = true;
        break;
      }
    }
  }
  if (corners.size() != 4) {
    if (r_error) {
      *r_error = "Limited Dissolve needs the merged NURBS boundary to reduce to four corners";
    }
    return std::nullopt;
  }
  std::array<double3, 4> corner_positions;
  for (const int i : IndexRange(4)) {
    const auto *vertex = index.vertex(polygon[corners[i]]);
    if (!vertex) {
      return std::nullopt;
    }
    corner_positions[i] = vertex_position(*vertex);
  }

  Map<int64_t, double2> vertex_uv;
  for (const int64_t id : polygon) {
    const auto *vertex = index.vertex(id);
    const auto uv = vertex ? boundary_uv(vertex_position(*vertex), corner_positions, tolerance) :
                             std::nullopt;
    if (!uv) {
      if (r_error) {
        *r_error = "Limited Dissolve could not map the outer boundary onto one planar patch";
      }
      return std::nullopt;
    }
    vertex_uv.add_overwrite(id, *uv);
  }

  detail::TopologyBuilder builder(source);
  if (!builder.valid()) {
    return std::nullopt;
  }
  TopologyEditResult result;
  result.source_to_result.add_overwrite(face_b->id, face_a->id);
  result.source_to_result.add_overwrite(edge_id, 0);

  NurbsSurfaceGeometry *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
  surface->id = builder.id();
  surface->points_num = 4;
  surface->points_u = surface->points_v = 2;
  surface->order_u = surface->order_v = 2;
  surface->knots_u_num = surface->knots_v_num = 4;
  surface->points = MEM_new_array<NurbsControlPoint>(4, __func__);
  surface->knots_u = MEM_new_array<double>(4, __func__);
  surface->knots_v = MEM_new_array<double>(4, __func__);
  std::copy_n(std::array<double, 4>{0, 0, 1, 1}.begin(), 4, surface->knots_u);
  std::copy_n(std::array<double, 4>{0, 0, 1, 1}.begin(), 4, surface->knots_v);
  const int point_corner[4] = {0, 1, 3, 2};
  for (int i = 0; i < 4; i++) {
    std::copy_n(&corner_positions[point_corner[i]].x, 3, surface->points[i].position);
    surface->points[i].weight = 1.0;
  }
  BLI_addtail(&builder.geometry->surfaces, surface);
  builder.surfaces.add_new(surface->id, surface);

  NurbsTopologyFace *merged_face = builder.faces.lookup(face_a->id);
  NurbsTopologyFace *removed_face = builder.faces.lookup(face_b->id);
  remove_face(*builder.geometry, *merged_face);
  remove_face(*builder.geometry, *removed_face);
  builder.faces.remove(face_a->id);
  builder.faces.remove(face_b->id);
  Set<int64_t> redundant_vertices;
  for (const int i : polygon.index_range()) {
    if (!corners.contains(i)) {
      redundant_vertices.add(polygon[i]);
    }
  }
  simplify_boundary(builder,
                    index,
                    edge_id,
                    face_a->id,
                    face_b->id,
                    redundant_vertices,
                    boundary,
                    result);
  merged_face = MEM_new<NurbsTopologyFace>(__func__);
  merged_face->id = face_a->id;
  merged_face->surface_id = surface->id;
  const double3 surface_normal = math::normalize(
      math::cross(corner_positions[1] - corner_positions[0],
                  corner_positions[3] - corner_positions[0]));
  merged_face->orientation = math::dot(surface_normal, normal_a) >= 0 ? 1 : -1;
  merged_face->material_index = face_a->material_index;
  NurbsTopologyLoop *loop = MEM_new<NurbsTopologyLoop>(__func__);
  loop->id = builder.id();
  BLI_addtail(&merged_face->loops, loop);
  BLI_addtail(&builder.geometry->faces, merged_face);
  builder.faces.add_new(merged_face->id, merged_face);

  append_boundary_uses(builder, *loop, boundary, vertex_uv, result);
  BLI_remlink(&builder.geometry->edges, builder.edges.lookup(edge_id));
  MEM_delete(builder.edges.lookup(edge_id));
  builder.edges.remove(edge_id);
  result.faces.append(merged_face->id);
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

}  // namespace

std::optional<TopologyEditResult> dissolve_limited(const NurbsGeometry &source,
                                                   const SelectionState &selection,
                                                   const double angle_limit,
                                                   std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (auto result = detail::dissolve_compatible_patches(source, selection, angle_limit, r_error)) {
    return result;
  }
  if (r_error && !r_error->empty()) {
    return std::nullopt;
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Set<int64_t> candidates;
  if (flag_is_set(selection.mode, SelectionMode::Face)) {
    for (const NurbsTopologyEdge &edge : source.edges) {
      const Span<EdgeUseReference> uses = index->uses_for_edge(edge.id);
      if (uses.size() == 2 && selection.faces.contains(uses[0].face_id) &&
          selection.faces.contains(uses[1].face_id))
      {
        candidates.add(edge.id);
      }
    }
  }
  else if (flag_is_set(selection.mode, SelectionMode::Edge)) {
    for (const int64_t id : selection.edges) {
      candidates.add(id);
    }
  }
  else {
    for (const int64_t id : selection.edges) {
      candidates.add(id);
    }
    for (const NurbsTopologyEdge &edge : source.edges) {
      if (selection.vertices.contains(edge.vertex_ids[0]) &&
          selection.vertices.contains(edge.vertex_ids[1]))
      {
        candidates.add(edge.id);
      }
    }
  }
  const double limit = std::clamp(angle_limit, 0.0, M_PI_2);
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (!candidates.contains(edge.id)) {
      continue;
    }
    auto result = dissolve_edge(source, *index, edge.id, limit, r_error);
    if (result || (r_error && !r_error->empty())) {
      return result;
    }
  }
  for (const NurbsTopologyFace &face : source.faces) {
    auto result = dissolve_boundary_vertices(source, *index, face, selection, r_error);
    if (result || (r_error && !r_error->empty())) {
      return result;
    }
  }
  return std::nullopt;
}

}  // namespace blender::bke::nurbs
