/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "BLI_math_vector.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_nurbs_refine.hh"
#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs::detail {
namespace {

static double2 domain(const double *knots, const int points, const int order)
{
  return {knots[order - 1], knots[points]};
}

static std::optional<Vector<double>> unit_knots(const double *knots,
                                               const int points,
                                               const int order,
                                               const bool reverse)
{
  const double2 range = domain(knots, points, order);
  const int size = points + order;
  for (int i = 0; i < order; i++) {
    if (knots[i] != range.x || knots[size - 1 - i] != range.y) {
      return std::nullopt;
    }
  }
  Vector<double> result(size);
  for (int i = 0; i < size; i++) {
    result[i] = reverse ? (range.y - knots[size - 1 - i]) / (range.y - range.x) :
                          (knots[i] - range.x) / (range.y - range.x);
  }
  return result;
}

static bool positive_weights(const Span<NurbsControlPoint> points)
{
  for (const auto &point : points) {
    if (!(point.weight > 0)) {
      return false;
    }
  }
  return true;
}

static const NurbsCurveGeometry *find_curve(const NurbsGeometry &source, const int64_t id)
{
  for (const auto &curve : source.curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

static const NurbsSurfaceGeometry *find_surface(const NurbsGeometry &source, const int64_t id)
{
  for (const auto &surface : source.surfaces) {
    if (surface.id == id) {
      return &surface;
    }
  }
  return nullptr;
}

struct Chart {
  const NurbsSurfaceGeometry *surface;
  int axis;
  bool reverse_u, reverse_v;
  double offset;

  int count(const int canonical_axis) const
  {
    const int source_axis = canonical_axis == 0 ? axis : 1 - axis;
    return source_axis == 0 ? surface->points_u : surface->points_v;
  }
  int order(const int canonical_axis) const
  {
    const int source_axis = canonical_axis == 0 ? axis : 1 - axis;
    return source_axis == 0 ? surface->order_u : surface->order_v;
  }
  const double *knots(const int canonical_axis) const
  {
    return (canonical_axis == 0 ? axis : 1 - axis) == 0 ? surface->knots_u : surface->knots_v;
  }
  double2 map(const double2 uv) const
  {
    const double2 u = domain(knots(0), count(0), order(0));
    const double2 v = domain(knots(1), count(1), order(1));
    return {offset + (reverse_u ? u.y - uv[axis] : uv[axis] - u.x) / (u.y - u.x),
            (reverse_v ? v.y - uv[1 - axis] : uv[1 - axis] - v.x) / (v.y - v.x)};
  }
  const NurbsControlPoint &point(int u, int v) const
  {
    if (reverse_u) {
      u = count(0) - 1 - u;
    }
    if (reverse_v) {
      v = count(1) - 1 - v;
    }
    return surface->points[axis == 0 ? v * surface->points_u + u : u * surface->points_u + v];
  }
  int orientation() const
  {
    return (axis == 0 ? 1 : -1) * (reverse_u ? -1 : 1) * (reverse_v ? -1 : 1);
  }
};

static std::optional<Chart> chart_for_side(const NurbsGeometry &source,
                                          const NurbsTopologyFace &face,
                                          const NurbsTopologyEdgeUse &use,
                                          const bool second)
{
  const auto *surface = find_surface(source, face.surface_id);
  const auto *pcurve = find_curve(source, use.pcurve_id);
  if (!surface || !pcurve || !positive_weights({surface->points, surface->points_num})) {
    return std::nullopt;
  }
  const double2 a(pcurve->points[0].position), b(pcurve->points[pcurve->points_num - 1].position);
  for (int axis = 0; axis < 2; axis++) {
    const double *knots = axis == 0 ? surface->knots_u : surface->knots_v;
    const double2 range = domain(knots,
                                 axis == 0 ? surface->points_u : surface->points_v,
                                 axis == 0 ? surface->order_u : surface->order_v);
    if (a[axis] != b[axis] || (a[axis] != range.x && a[axis] != range.y) ||
        a[1 - axis] == b[1 - axis])
    {
      continue;
    }
    for (const auto &point : Span(pcurve->points, pcurve->points_num)) {
      if (point.position[axis] != a[axis]) {
        return std::nullopt;
      }
    }
    return Chart{surface,
                 axis,
                 second ? a[axis] == range.y : a[axis] == range.x,
                 a[1 - axis] > b[1 - axis],
                 second ? 1.0 : 0.0};
  }
  return std::nullopt;
}

static bool tangent_faces(const NurbsGeometry &source,
                           const TopologyIndex &index,
                           const NurbsTopologyEdge &edge,
                           const double angle_limit)
{
  const auto uses = index.uses_for_edge(edge.id);
  if (uses.size() != 2 || uses[0].face_id == uses[1].face_id) {
    return false;
  }
  const auto *a = index.face(uses[0].face_id), *b = index.face(uses[1].face_id);
  const auto *sa = find_surface(source, a->surface_id), *sb = find_surface(source, b->surface_id);
  const auto *pa = find_curve(source, uses[0].use->pcurve_id);
  const auto *pb = find_curve(source, uses[1].use->pcurve_id);
  for (int i = 0; i < 9; i++) {
    const double t = math::interpolate(edge.parameter_range[0], edge.parameter_range[1],
                                       (i + 0.5) / 9.0);
    const auto ua = evaluate_curve(*pa, t), ub = evaluate_curve(*pb, t);
    if (!ua || !ub) {
      return false;
    }
    const auto da = evaluate_surface_differential(*sa, double2(ua->position));
    const auto db = evaluate_surface_differential(*sb, double2(ub->position));
    if (!da || !db || !da->normal || !db->normal) {
      return false;
    }
    const double cosine = math::dot(*da->normal * double(a->orientation),
                                    *db->normal * double(b->orientation));
    if (std::acos(std::clamp(cosine, -1.0, 1.0)) > angle_limit + 1e-7) {
      return false;
    }
  }
  return true;
}

static void discard_unused_resources(NurbsGeometry &geometry)
{
  Set<int64_t> curves, surfaces;
  for (const auto &edge : geometry.edges) {
    curves.add(edge.curve_id);
  }
  for (const auto &face : geometry.faces) {
    surfaces.add(face.surface_id);
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        curves.add(use.pcurve_id);
      }
    }
  }
  OwnedGeometry garbage(MEM_new<NurbsGeometry>(__func__));
  for (auto &curve : geometry.curves.items_mutable()) {
    if (!curves.contains(curve.id)) {
      BLI_remlink(&geometry.curves, &curve);
      BLI_addtail(&garbage->curves, &curve);
    }
  }
  for (auto &surface : geometry.surfaces.items_mutable()) {
    if (!surfaces.contains(surface.id)) {
      BLI_remlink(&geometry.surfaces, &surface);
      BLI_addtail(&garbage->surfaces, &surface);
    }
  }
}

static std::optional<Vector<const NurbsTopologyEdgeUse *>> outer_path(
    const NurbsTopologyFace &face, const Set<int64_t> &shared)
{
  if (!face.loops.is_single()) {
    return std::nullopt;
  }
  const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
  if (loop.is_inner) {
    return std::nullopt;
  }
  Vector<const NurbsTopologyEdgeUse *> uses;
  for (const auto &use : loop.edge_uses) {
    uses.append(&use);
  }
  int start = -1;
  for (const int i : uses.index_range()) {
    if (!shared.contains(uses[i]->edge_id) &&
        shared.contains(uses[(i + uses.size() - 1) % uses.size()]->edge_id))
    {
      if (start != -1) {
        return std::nullopt;
      }
      start = i;
    }
  }
  if (start == -1) {
    return std::nullopt;
  }
  Vector<const NurbsTopologyEdgeUse *> path;
  for (int i = 0; i < uses.size(); i++) {
    const auto *use = uses[(start + i) % uses.size()];
    if (shared.contains(use->edge_id)) {
      break;
    }
    path.append(use);
  }
  return path;
}

static bool same_surface(const NurbsSurfaceGeometry &a, const NurbsSurfaceGeometry &b)
{
  return a.order_u == b.order_u && a.order_v == b.order_v && a.points_u == b.points_u &&
         a.points_v == b.points_v &&
         std::memcmp(a.points, b.points, sizeof(NurbsControlPoint) * a.points_num) == 0 &&
         std::memcmp(a.knots_u, b.knots_u, sizeof(double) * a.knots_u_num) == 0 &&
         std::memcmp(a.knots_v, b.knots_v, sizeof(double) * a.knots_v_num) == 0;
}

/** Trimmed corner tiles can already use copies of the very same curved support. */
static std::optional<TopologyEditResult> join_trimmed_faces(const NurbsGeometry &source,
                                                           const TopologyIndex &index,
                                                           const NurbsTopologyFace &a,
                                                           const NurbsTopologyFace &b,
                                                           const Set<int64_t> &eligible,
                                                           const double angle_limit)
{
  const auto *sa = find_surface(source, a.surface_id), *sb = find_surface(source, b.surface_id);
  if (a.orientation != b.orientation || !same_surface(*sa, *sb)) {
    return std::nullopt;
  }
  Set<int64_t> shared;
  for (const auto &use : static_cast<const NurbsTopologyLoop *>(a.loops.first)->edge_uses) {
    const auto refs = index.uses_for_edge(use.edge_id);
    bool common = false;
    for (const auto &ref : refs) {
      common |= ref.face_id == b.id;
    }
    if (common) {
      if (refs.size() != 2 || !eligible.contains(use.edge_id) ||
          !tangent_faces(source, index, *index.edge(use.edge_id), angle_limit))
      {
        return std::nullopt;
      }
      shared.add(use.edge_id);
    }
  }
  if (shared.is_empty()) {
    return std::nullopt;
  }
  auto pa = outer_path(a, shared), pb = outer_path(b, shared);
  if (!pa || !pb) {
    return std::nullopt;
  }
  auto endpoint = [&](const auto &use, bool end) {
    return index.edge(use.edge_id)->vertex_ids[int(end) ^ use.reversed];
  };
  if (endpoint(*pa->last(), true) != endpoint(*pb->first(), false) ||
      endpoint(*pb->last(), true) != endpoint(*pa->first(), false))
  {
    return std::nullopt;
  }
  auto uv_endpoint = [&](const auto &use, bool end) -> std::optional<double2> {
    const auto *edge = index.edge(use.edge_id);
    const auto value = evaluate_curve(*find_curve(source, use.pcurve_id),
                                      edge->parameter_range[int(end) ^ use.reversed]);
    return value ? std::optional<double2>(double2(value->position)) : std::nullopt;
  };
  const auto end_a = uv_endpoint(*pa->last(), true), start_b = uv_endpoint(*pb->first(), false);
  const auto end_b = uv_endpoint(*pb->last(), true), start_a = uv_endpoint(*pa->first(), false);
  /* A periodic seam may identify the same 3D vertex through different UV chart boundaries.
   * Keep that seam instead of producing an open trimming loop. */
  if (!end_a || !start_b || !end_b || !start_a || *end_a != *start_b || *end_b != *start_a) {
    return std::nullopt;
  }
  TopologyBuilder builder(source);
  auto *face = builder.faces.lookup(a.id), *removed = builder.faces.lookup(b.id);
  for (auto *item : {face, removed}) {
    for (auto &loop : item->loops) {
      BLI_freelistN(&loop.edge_uses);
    }
    BLI_freelistN(&item->loops);
  }
  BLI_remlink(&builder.geometry->faces, removed);
  MEM_delete(removed);
  auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
  loop->id = builder.id();
  BLI_addtail(&face->loops, loop);
  TopologyEditResult result;
  for (const auto *path : {&*pa, &*pb}) {
    for (const auto *old : *path) {
      auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__, *old);
      use->id = builder.id();
      BLI_addtail(&loop->edge_uses, use);
      result.edges.append(use->edge_id);
      result.vertices.append(endpoint(*old, false));
    }
  }
  face->tolerance = std::max(a.tolerance, b.tolerance);
  result.source_to_result.add_new(b.id, a.id);
  for (const int64_t id : shared) {
    auto *edge = builder.edges.lookup(id);
    BLI_remlink(&builder.geometry->edges, edge);
    MEM_delete(edge);
    result.source_to_result.add_new(id, 0);
  }
  Set<int64_t> used;
  for (const auto &edge : builder.geometry->edges) {
    used.add(edge.vertex_ids[0]);
    used.add(edge.vertex_ids[1]);
  }
  for (const auto &vertex : source.vertices) {
    if (!used.contains(vertex.id) && !index.edges_for_vertex(vertex.id).is_empty()) {
      auto *old = builder.vertices.lookup(vertex.id);
      BLI_remlink(&builder.geometry->vertices, old);
      MEM_delete(old);
      result.source_to_result.add_new(vertex.id, 0);
    }
  }
  result.faces.append(face->id);
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

static std::optional<TopologyEditResult> join_faces(const NurbsGeometry &source,
                                                   const TopologyIndex &index,
                                                   const NurbsTopologyEdge &seed,
                                                   const Set<int64_t> &eligible,
                                                   const double angle_limit)
{
  const auto refs = index.uses_for_edge(seed.id);
  if (refs.size() != 2 || refs[0].face_id == refs[1].face_id) {
    return std::nullopt;
  }
  const auto *a = index.face(refs[0].face_id), *b = index.face(refs[1].face_id);
  if (a->material_index != b->material_index || !a->loops.is_single() || !b->loops.is_single()) {
    return std::nullopt;
  }
  if (auto result = join_trimmed_faces(source, index, *a, *b, eligible, angle_limit)) {
    return result;
  }
  const auto ca = chart_for_side(source, *a, *refs[0].use, false);
  const auto cb = chart_for_side(source, *b, *refs[1].use, true);
  if (!ca || !cb || ca->order(0) != cb->order(0) || ca->order(1) != cb->order(1) ||
      ca->count(1) != cb->count(1) ||
      a->orientation * ca->orientation() != b->orientation * cb->orientation())
  {
    return std::nullopt;
  }
  auto au = unit_knots(ca->knots(0), ca->count(0), ca->order(0), ca->reverse_u);
  auto av = unit_knots(ca->knots(1), ca->count(1), ca->order(1), ca->reverse_v);
  auto bu = unit_knots(cb->knots(0), cb->count(0), cb->order(0), cb->reverse_u);
  auto bv = unit_knots(cb->knots(1), cb->count(1), cb->order(1), cb->reverse_v);
  if (!au || !av || !bu || !bv || *av != *bv) {
    return std::nullopt;
  }
  Set<int64_t> shared;
  double low = 1, high = 0;
  for (const auto &use : static_cast<const NurbsTopologyLoop *>(a->loops.first)->edge_uses) {
    const auto uses = index.uses_for_edge(use.edge_id);
    bool common = false;
    for (const auto &ref : uses) {
      common |= ref.face_id == b->id;
    }
    if (!common) {
      continue;
    }
    if (uses.size() != 2 || !eligible.contains(use.edge_id) ||
        !tangent_faces(source, index, *index.edge(use.edge_id), angle_limit))
    {
      return std::nullopt;
    }
    const auto *pc = find_curve(source, use.pcurve_id);
    for (const auto &point : Span(pc->points, pc->points_num)) {
      const double2 uv = ca->map(double2(point.position));
      if (std::abs(uv.x - 1) > 1e-12) {
        return std::nullopt;
      }
      low = std::min(low, uv.y);
      high = std::max(high, uv.y);
    }
    shared.add(use.edge_id);
  }
  if (shared.is_empty() || std::abs(low) > 1e-12 || std::abs(high - 1) > 1e-12) {
    return std::nullopt;
  }
  auto path_a = outer_path(*a, shared), path_b = outer_path(*b, shared);
  if (!path_a || !path_b) {
    return std::nullopt;
  }
  auto endpoint = [&](const NurbsTopologyEdgeUse &use, bool end) {
    return index.edge(use.edge_id)->vertex_ids[int(end) ^ use.reversed];
  };
  if (endpoint(*path_a->last(), true) != endpoint(*path_b->first(), false) ||
      endpoint(*path_b->last(), true) != endpoint(*path_a->first(), false))
  {
    return std::nullopt;
  }
  /* Align the homogeneous scale at the common row, then use a C0 knot and one shared row.
   * Both pieces retain their coefficients apart from bounded seam/construction roundoff. */
  double scale = 1;
  const double ratio = ca->point(ca->count(0) - 1, 0).weight / cb->point(0, 0).weight;
  for (int v = 0; v < ca->count(1); v++) {
    const auto &pa = ca->point(ca->count(0) - 1, v), &pb = cb->point(0, v);
    scale = std::max({scale, math::length(double3(pa.position)), math::length(double3(pb.position))});
    if (math::distance(double3(pa.position), double3(pb.position)) > 1e-10 * scale ||
        std::abs(pa.weight - ratio * pb.weight) > 1e-10 * std::max(pa.weight, ratio * pb.weight))
    {
      return std::nullopt;
    }
  }
  TopologyBuilder builder(source);
  auto *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
  surface->id = builder.id();
  surface->order_u = ca->order(0);
  surface->order_v = ca->order(1);
  surface->points_u = ca->count(0) + cb->count(0) - 1;
  surface->points_v = ca->count(1);
  surface->points_num = surface->points_u * surface->points_v;
  surface->knots_u_num = surface->points_u + surface->order_u;
  surface->knots_v_num = surface->points_v + surface->order_v;
  surface->points = MEM_new_array<NurbsControlPoint>(surface->points_num, __func__);
  surface->knots_u = MEM_new_array<double>(surface->knots_u_num, __func__);
  surface->knots_v = MEM_new_array<double>(surface->knots_v_num, __func__);
  for (int v = 0; v < surface->points_v; v++) {
    for (int u = 0; u < surface->points_u; u++) {
      auto &point = surface->points[v * surface->points_u + u];
      if (u < ca->count(0)) {
        point = ca->point(u, v);
      }
      else {
        point = cb->point(u - ca->count(0) + 1, v);
        point.weight *= ratio;
      }
    }
  }
  std::copy(av->begin(), av->end(), surface->knots_v);
  std::copy(au->begin(), au->end() - 1, surface->knots_u);
  for (int i = cb->order(0); i < bu->size(); i++) {
    surface->knots_u[au->size() - 1 + i - cb->order(0)] = 1 + (*bu)[i];
  }
  BLI_addtail(&builder.geometry->surfaces, surface);
  auto *face = builder.faces.lookup(a->id);
  for (auto &loop : face->loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&face->loops);
  auto *removed = builder.faces.lookup(b->id);
  for (auto &loop : removed->loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&removed->loops);
  BLI_remlink(&builder.geometry->faces, removed);
  MEM_delete(removed);
  face->surface_id = surface->id;
  face->orientation = a->orientation * ca->orientation();
  face->tolerance = std::max(a->tolerance, b->tolerance);
  auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
  loop->id = builder.id();
  BLI_addtail(&face->loops, loop);
  TopologyEditResult result;
  Map<int64_t, double2> vertex_uv;
  auto prepare_vertices = [&](const auto &path, const Chart &chart) {
    for (const auto *old : path) {
      const auto *edge = index.edge(old->edge_id);
      const auto *pc = find_curve(source, old->pcurve_id);
      for (int end = 0; end < 2; end++) {
        const auto point = evaluate_curve(*pc, edge->parameter_range[end]);
        if (!point) {
          return false;
        }
        const double2 uv = chart.map(double2(point->position));
        const auto *existing = vertex_uv.lookup_ptr(edge->vertex_ids[end]);
        if (existing && math::distance(*existing, uv) > 1e-10) {
          return false;
        }
        vertex_uv.add(edge->vertex_ids[end], uv);
      }
    }
    return true;
  };
  if (!prepare_vertices(*path_a, *ca) || !prepare_vertices(*path_b, *cb)) {
    return std::nullopt;
  }
  auto append = [&](const auto &path, const Chart &chart) {
    for (const auto *old : path) {
      const auto *edge = index.edge(old->edge_id);
      auto cropped = restrict_curve(*find_curve(source, old->pcurve_id),
                                     double2(edge->parameter_range), 1e-9);
      if (!cropped) {
        return false;
      }
      auto *pc = cropped->curve.release();
      pc->id = builder.id();
      BLI_addtail(&builder.geometry->curves, pc);
      for (auto &point : MutableSpan(pc->points, pc->points_num)) {
        const auto uv = chart.map(double2(point.position));
        point.position[0] = uv.x;
        point.position[1] = uv.y;
      }
      for (int end = 0; end < 2; end++) {
        const auto uv = vertex_uv.lookup(edge->vertex_ids[end]);
        auto &point = pc->points[end ? pc->points_num - 1 : 0];
        point.position[0] = uv.x;
        point.position[1] = uv.y;
      }
      auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__, *old);
      use->id = builder.id();
      use->pcurve_id = pc->id;
      BLI_addtail(&loop->edge_uses, use);
      result.edges.append(use->edge_id);
      result.vertices.append(endpoint(*old, false));
    }
    return true;
  };
  if (!append(*path_a, *ca) || !append(*path_b, *cb)) {
    return std::nullopt;
  }
  result.source_to_result.add_new(b->id, a->id);
  for (const int64_t id : shared) {
    auto *edge = builder.edges.lookup(id);
    BLI_remlink(&builder.geometry->edges, edge);
    MEM_delete(edge);
    result.source_to_result.add_new(id, 0);
  }
  Set<int64_t> used_vertices;
  for (const auto &edge : builder.geometry->edges) {
    used_vertices.add(edge.vertex_ids[0]);
    used_vertices.add(edge.vertex_ids[1]);
  }
  for (const auto &vertex : source.vertices) {
    if (!used_vertices.contains(vertex.id) && !index.edges_for_vertex(vertex.id).is_empty()) {
      auto *copy = builder.vertices.lookup(vertex.id);
      BLI_remlink(&builder.geometry->vertices, copy);
      MEM_delete(copy);
      result.source_to_result.add_new(vertex.id, 0);
    }
  }
  result.faces.append(face->id);
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

static OwnedCurve join_curves(const NurbsCurveGeometry &a,
                               const NurbsCurveGeometry &b,
                               const bool reverse_a,
                               const bool reverse_b,
                               const double2 range_a,
                               const double2 range_b)
{
  if (domain(a.knots, a.points_num, a.order) != range_a ||
      domain(b.knots, b.points_num, b.order) != range_b)
  {
    auto crop_a = restrict_curve(a, range_a, 1e-9);
    auto crop_b = restrict_curve(b, range_b, 1e-9);
    if (!crop_a || !crop_b) {
      return nullptr;
    }
    auto reconcile_endpoints = [](const NurbsCurveGeometry &original,
                                   NurbsCurveGeometry &cropped,
                                   const double2 range) {
      for (int end = 0; end < 2; end++) {
        const auto value = evaluate_curve(original, range[end]);
        if (!value) {
          return false;
        }
        auto &point = cropped.points[end ? cropped.points_num - 1 : 0];
        std::copy_n(&value->position.x, 3, point.position);
      }
      return true;
    };
    /* Neighboring untouched uses still evaluate the original intervals. Preserve their exact
     * UV endpoint values rather than introducing a second interpolation rounding. */
    if (!reconcile_endpoints(a, *crop_a->curve, range_a) ||
        !reconcile_endpoints(b, *crop_b->curve, range_b))
    {
      return nullptr;
    }
    return join_curves(*crop_a->curve, *crop_b->curve, reverse_a, reverse_b, range_a, range_b);
  }
  if (a.order != b.order || a.dimension != b.dimension ||
      domain(a.knots, a.points_num, a.order) != range_a ||
      domain(b.knots, b.points_num, b.order) != range_b ||
      !positive_weights({a.points, a.points_num}) || !positive_weights({b.points, b.points_num}))
  {
    return nullptr;
  }
  const auto ka = unit_knots(a.knots, a.points_num, a.order, reverse_a);
  const auto kb = unit_knots(b.knots, b.points_num, b.order, reverse_b);
  if (!ka || !kb) {
    return nullptr;
  }
  const auto &end_a = a.points[reverse_a ? 0 : a.points_num - 1];
  const auto &start_b = b.points[reverse_b ? b.points_num - 1 : 0];
  const double scale = std::max({1.0, math::length(double3(end_a.position)),
                                 math::length(double3(start_b.position))});
  if (math::distance(double3(end_a.position), double3(start_b.position)) > 1e-10 * scale) {
    return nullptr;
  }
  const double ratio = end_a.weight / start_b.weight;
  OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
  curve->order = a.order;
  curve->dimension = a.dimension;
  curve->points_num = a.points_num + b.points_num - 1;
  curve->knots_num = curve->points_num + curve->order;
  curve->points = MEM_new_array<NurbsControlPoint>(curve->points_num, __func__);
  curve->knots = MEM_new_array<double>(curve->knots_num, __func__);
  for (int i = 0; i < a.points_num; i++) {
    curve->points[i] = a.points[reverse_a ? a.points_num - 1 - i : i];
  }
  for (int i = 1; i < b.points_num; i++) {
    auto &point = curve->points[a.points_num + i - 1];
    point = b.points[reverse_b ? b.points_num - 1 - i : i];
    point.weight *= ratio;
  }
  std::copy(ka->begin(), ka->end() - 1, curve->knots);
  for (int i = b.order; i < kb->size(); i++) {
    curve->knots[ka->size() - 1 + i - b.order] = 1 + (*kb)[i];
  }
  return curve;
}

static std::optional<TopologyEditResult> join_boundary(const NurbsGeometry &source,
                                                      const TopologyIndex &index,
                                                      const int64_t vertex_id,
                                                      const double angle_limit)
{
  const auto edges = index.edges_for_vertex(vertex_id);
  if (edges.size() != 2) {
    return std::nullopt;
  }
  const auto *a = index.edge(edges[0]), *b = index.edge(edges[1]);
  if (a->vertex_ids[0] == a->vertex_ids[1] || b->vertex_ids[0] == b->vertex_ids[1]) {
    return std::nullopt;
  }
  const bool ra = a->vertex_ids[0] == vertex_id, rb = b->vertex_ids[1] == vertex_id;
  const auto *ac = find_curve(source, a->curve_id), *bc = find_curve(source, b->curve_id);
  const double3 incoming = ra ? double3(ac->points[0].position) - double3(ac->points[1].position) :
                               double3(ac->points[ac->points_num - 1].position) -
                                   double3(ac->points[ac->points_num - 2].position);
  const double3 outgoing = rb ? double3(bc->points[bc->points_num - 2].position) -
                                   double3(bc->points[bc->points_num - 1].position) :
                               double3(bc->points[1].position) - double3(bc->points[0].position);
  if (math::is_zero(incoming) || math::is_zero(outgoing) ||
      std::acos(std::clamp(math::dot(math::normalize(incoming), math::normalize(outgoing)),
                           -1.0, 1.0)) > angle_limit + 1e-7)
  {
    return std::nullopt;
  }
  const auto uses_a = index.uses_for_edge(a->id), uses_b = index.uses_for_edge(b->id);
  if (uses_a.is_empty() || uses_a.size() != uses_b.size() || uses_a.size() > 2) {
    return std::nullopt;
  }
  auto curve = join_curves(*ac, *bc, ra, rb,
                           double2(a->parameter_range), double2(b->parameter_range));
  if (!curve) {
    return std::nullopt;
  }
  struct Replacement {
    int64_t face, loop, first, second;
    bool reversed;
    OwnedCurve pcurve;
  };
  Vector<Replacement> replacements;
  for (const auto &ua : uses_a) {
    const EdgeUseReference *ub = nullptr;
    for (const auto &candidate : uses_b) {
      if (candidate.face_id == ua.face_id && candidate.loop_id == ua.loop_id) {
        if (ub) {
          return std::nullopt;
        }
        ub = &candidate;
      }
    }
    if (!ub) {
      return std::nullopt;
    }
    const bool forward = bool(ua.use->reversed) == ra;
    if (bool(ub->use->reversed) != (forward ? rb : !rb)) {
      return std::nullopt;
    }
    const auto *first = forward ? ua.use : ub->use;
    const auto *second = forward ? ub->use : ua.use;
    const auto *next = first->next;
    if (!next) {
      const auto *face = index.face(ua.face_id);
      for (const auto &loop : face->loops) {
        if (loop.id == ua.loop_id) {
          next = static_cast<const NurbsTopologyEdgeUse *>(loop.edge_uses.first);
        }
      }
    }
    if (next != second) {
      return std::nullopt;
    }
    auto pc = join_curves(*find_curve(source, ua.use->pcurve_id),
                          *find_curve(source, ub->use->pcurve_id), ra, rb,
                          double2(a->parameter_range), double2(b->parameter_range));
    if (!pc) {
      return std::nullopt;
    }
    replacements.append({ua.face_id, ua.loop_id, first->id, second->id, !forward, std::move(pc)});
  }
  TopologyBuilder builder(source);
  curve->id = builder.id();
  auto *installed = curve.release();
  BLI_addtail(&builder.geometry->curves, installed);
  auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
  edge->id = builder.id();
  edge->curve_id = installed->id;
  edge->vertex_ids[0] = a->vertex_ids[ra ? 1 : 0];
  edge->vertex_ids[1] = b->vertex_ids[rb ? 0 : 1];
  edge->parameter_range[1] = 2;
  edge->tolerance = std::max(a->tolerance, b->tolerance);
  if (edge->vertex_ids[0] == edge->vertex_ids[1]) {
    MEM_delete(edge);
    return std::nullopt;
  }
  BLI_addtail(&builder.geometry->edges, edge);
  TopologyEditResult result;
  for (auto &replacement : replacements) {
    replacement.pcurve->id = builder.id();
    auto *pc = replacement.pcurve.release();
    BLI_addtail(&builder.geometry->curves, pc);
    auto *face = builder.faces.lookup(replacement.face);
    for (auto &loop : face->loops) {
      if (loop.id != replacement.loop) {
        continue;
      }
      ListBaseT<NurbsTopologyEdgeUse> old = loop.edge_uses;
      loop.edge_uses = {nullptr, nullptr};
      for (const auto &use : old) {
        if (use.id == replacement.second) {
          continue;
        }
        auto *copy = MEM_new<NurbsTopologyEdgeUse>(__func__, use);
        if (use.id == replacement.first) {
          copy->id = builder.id();
          copy->edge_id = edge->id;
          copy->pcurve_id = pc->id;
          copy->reversed = replacement.reversed;
        }
        BLI_addtail(&loop.edge_uses, copy);
      }
      BLI_freelistN(&old);
    }
    result.faces.append(face->id);
  }
  for (const int64_t id : {a->id, b->id}) {
    auto *old = builder.edges.lookup(id);
    BLI_remlink(&builder.geometry->edges, old);
    MEM_delete(old);
    result.source_to_result.add_new(id, edge->id);
  }
  auto *vertex = builder.vertices.lookup(vertex_id);
  BLI_remlink(&builder.geometry->vertices, vertex);
  MEM_delete(vertex);
  result.source_to_result.add_new(vertex_id, 0);
  result.edges.append(edge->id);
  result.vertices.append(edge->vertex_ids[0]);
  result.vertices.append(edge->vertex_ids[1]);
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

}  // namespace

std::optional<TopologyEditResult> dissolve_compatible_patches(const NurbsGeometry &source,
                                                              const SelectionState &selection,
                                                              const double angle_limit,
                                                              std::string *r_error)
{
  if (!std::isfinite(angle_limit)) {
    if (r_error) {
      *r_error = "Limited Dissolve requires a finite angle limit";
    }
    return std::nullopt;
  }
  auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Set<int64_t> eligible, vertices = selection.vertices;
  for (const auto &edge : source.edges) {
    const auto uses = index->uses_for_edge(edge.id);
    const bool selected = flag_is_set(selection.mode, SelectionMode::Face) ?
                              uses.size() == 2 && selection.faces.contains(uses[0].face_id) &&
                                  selection.faces.contains(uses[1].face_id) :
                              selection.edges.contains(edge.id);
    if (selected) {
      eligible.add(edge.id);
      vertices.add(edge.vertex_ids[0]);
      vertices.add(edge.vertex_ids[1]);
    }
  }
  TopologyEditResult total;
  total.vertices.extend(selection.vertices.begin(), selection.vertices.end());
  total.edges.extend(selection.edges.begin(), selection.edges.end());
  total.faces.extend(selection.faces.begin(), selection.faces.end());
  auto absorb = [&](TopologyEditResult step) {
    for (auto item : total.source_to_result.items()) {
      item.value = step.source_to_result.lookup_default(item.value, item.value);
    }
    for (const auto item : step.source_to_result.items()) {
      total.source_to_result.add_overwrite(item.key, item.value);
    }
    total.vertices.extend(step.vertices);
    total.edges.extend(step.edges);
    total.faces.extend(step.faces);
    discard_unused_resources(*step.geometry);
    total.geometry = std::move(step.geometry);
  };
  const double limit = std::clamp(angle_limit, 0.0, M_PI_2);
  bool changed;
  do {
    changed = false;
    const auto &current = total.geometry ? *total.geometry : source;
    index = TopologyIndex::build(current);
    for (const auto &edge : current.edges) {
      if (!eligible.contains(edge.id)) {
        continue;
      }
      if (auto step = join_faces(current, *index, edge, eligible, limit)) {
        absorb(std::move(*step));
        changed = true;
        break;
      }
    }
  } while (changed);
  do {
    changed = false;
    const auto &current = total.geometry ? *total.geometry : source;
    index = TopologyIndex::build(current);
    for (const auto &vertex : current.vertices) {
      if (!vertices.contains(vertex.id)) {
        continue;
      }
      if (auto step = join_boundary(current, *index, vertex.id, limit)) {
        absorb(std::move(*step));
        changed = true;
        break;
      }
    }
  } while (changed);
  if (!total.geometry) {
    return std::nullopt;
  }
  index = TopologyIndex::build(*total.geometry, r_error);
  if (!index) {
    return std::nullopt;
  }
  auto remap = [&](Vector<int64_t> &ids, auto exists) {
    Set<int64_t> kept;
    ids.remove_if([&](int64_t &id) {
      id = total.source_to_result.lookup_default(id, id);
      return !id || !exists(id) || !kept.add(id);
    });
  };
  remap(total.vertices, [&](int64_t id) { return index->vertex(id) != nullptr; });
  remap(total.edges, [&](int64_t id) { return index->edge(id) != nullptr; });
  remap(total.faces, [&](int64_t id) { return index->face(id) != nullptr; });
  return total;
}

}  // namespace blender::bke::nurbs::detail
