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
#include "BLI_math_boolean.hh"
#include "BLI_math_vector.hh"

#include "BKE_nurbs_construct.hh"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {
namespace {

struct FillSide {
  int64_t start, end;
  int64_t edge = 0;
  bool reversed = false;
};

struct FillProjection {
  int exponent = 0;
  int axis = 2;

  double3 scaled(const double3 &point) const
  {
    return {std::scalbn(point.x, -exponent),
            std::scalbn(point.y, -exponent),
            std::scalbn(point.z, -exponent)};
  }
  double2 point(const double3 &position) const
  {
    const double3 value = this->scaled(position);
    return {value[(axis + 1) % 3], value[(axis + 2) % 3]};
  }
};

static std::optional<FillProjection> fill_projection(const Span<double3> points)
{
  double largest = 0;
  for (const double3 &point : points) {
    for (int axis = 0; axis < 3; axis++) {
      largest = std::max(largest, std::abs(point[axis]));
    }
  }
  if (largest == 0 || !std::isfinite(largest) || points.size() < 3) {
    return std::nullopt;
  }
  FillProjection result;
  result.exponent = std::ilogb(largest);
  for (const double3 &point : points) {
    const double3 scaled = result.scaled(point);
    for (int axis = 0; axis < 3; axis++) {
      // Bound exact-predicate product exponents after an exact binary rescaling.
      if (point[axis] != 0 && (std::abs(scaled[axis]) < 0x1p-300 ||
                               std::scalbn(scaled[axis], result.exponent) != point[axis]))
      {
        return std::nullopt;
      }
    }
  }
  const double3 a = result.scaled(points.first());
  int farthest = 0;
  double distance = 0;
  for (const int i : points.index_range()) {
    const double3 delta = result.scaled(points[i]) - a;
    const double value = std::hypot(delta.x, delta.y, delta.z);
    if (value > distance) {
      distance = value;
      farthest = i;
    }
  }
  if (distance == 0) {
    return std::nullopt;
  }
  const double3 b = result.scaled(points[farthest]);
  double best = -1;
  for (const double3 &point : points) {
    const double3 c = result.scaled(point);
    const double3 normal = math::cross(b - a, c - a);
    for (int axis = 0; axis < 3; axis++) {
      const int u = (axis + 1) % 3, v = (axis + 2) % 3;
      if (orient2d({a[u], a[v]}, {b[u], b[v]}, {c[u], c[v]}) != 0 && std::abs(normal[axis]) > best)
      {
        best = std::abs(normal[axis]);
        result.axis = axis;
      }
    }
  }
  return best < 0 ? std::nullopt : std::optional(result);
}

static bool on_segment(const double2 a, const double2 b, const double2 p)
{
  return orient2d(a, b, p) == 0 && p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) &&
         p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}

static bool segments_intersect(const double2 a, const double2 b, const double2 c, const double2 d)
{
  const int abc = orient2d(a, b, c), abd = orient2d(a, b, d);
  const int cda = orient2d(c, d, a), cdb = orient2d(c, d, b);
  return (abc * abd < 0 && cda * cdb < 0) || on_segment(a, b, c) || on_segment(a, b, d) ||
         on_segment(c, d, a) || on_segment(c, d, b);
}

/** A simple control polygon is also required for a convex curved-profile certificate. */
static bool polygon_is_simple(const Span<double2> polygon)
{
  if (polygon.size() < 3 || polygon.size() > 1400) {
    return false;
  }
  for (const int i : polygon.index_range()) {
    const int next = (i + 1) % polygon.size();
    if (polygon[i] == polygon[next]) {
      return false;
    }
    for (int j = i + 1; j < polygon.size(); j++) {
      const int after = (j + 1) % polygon.size();
      if (j != next && after != i &&
          segments_intersect(polygon[i], polygon[next], polygon[j], polygon[after]))
      {
        return false;
      }
    }
  }
  return true;
}

static int polygon_turn(const Span<double2> polygon)
{
  int turn = 0;
  for (const int i : polygon.index_range()) {
    const int value = orient2d(
        polygon[i], polygon[(i + 1) % polygon.size()], polygon[(i + 2) % polygon.size()]);
    if (value != 0) {
      if (turn != 0 && turn != value) {
        return 0;
      }
      turn = value;
    }
  }
  return turn;
}

static bool full_clamped_curve(const NurbsCurveGeometry &curve, const NurbsTopologyEdge &edge)
{
  if (curve.order < 2 || edge.parameter_range[0] != curve.knots[curve.order - 1] ||
      edge.parameter_range[1] != curve.knots[curve.points_num])
  {
    return false;
  }
  for (int i = 0; i < curve.order; i++) {
    if (curve.knots[i] != edge.parameter_range[0] ||
        curve.knots[curve.knots_num - 1 - i] != edge.parameter_range[1])
    {
      return false;
    }
  }
  for (int first = curve.order; first < curve.points_num;) {
    int end = first + 1;
    while (end < curve.points_num && curve.knots[end] == curve.knots[first]) {
      end++;
    }
    if (end - first >= curve.order) {
      return false;
    }
    first = end;
  }
  for (int i = 0; i < curve.points_num; i++) {
    if (!(curve.points[i].weight > 0)) {
      return false;
    }
  }
  return true;
}

static bool composite_bezier_monotone(const NurbsCurveGeometry &curve, const FillProjection &frame)
{
  const int degree = curve.order - 1;
  if (degree <= 0 || (curve.points_num - 1) % degree != 0) {
    return false;
  }
  for (int first = curve.order; first < curve.points_num;) {
    int end = first + 1;
    while (end < curve.points_num && curve.knots[end] == curve.knots[first]) {
      end++;
    }
    if (end - first != degree) {
      return false;
    }
    first = end;
  }
  for (int start = 0; start + degree < curve.points_num; start += degree) {
    bool monotone = false;
    for (int axis = 0; axis < 2; axis++) {
      const double first = frame.point(double3(curve.points[start].position))[axis];
      const double last = frame.point(double3(curve.points[start + degree].position))[axis];
      if (first == last) {
        continue;
      }
      bool ordered = true;
      double previous = first;
      for (int i = 1; i <= degree; i++) {
        const double value = frame.point(double3(curve.points[start + i].position))[axis];
        ordered &= last > first ? value >= previous : value <= previous;
        previous = value;
      }
      monotone |= ordered;
    }
    if (!monotone) {
      return false;
    }
  }
  return true;
}

/** Certify an arch and its endpoint chord using positive rational control hulls.
 * Each Bezier span is monotone. Separating lines keep different span hulls disjoint,
 * apart from consecutive shared endpoints, and the chord stays outside the arch hulls. */
static bool simple_arch(const NurbsCurveGeometry &curve, const FillProjection &frame)
{
  if (!composite_bezier_monotone(curve, frame)) {
    return false;
  }
  Vector<double2> points;
  for (int i = 0; i < curve.points_num; i++) {
    points.append(frame.point(double3(curve.points[i].position)));
  }
  const double2 a = points.first(), b = points.last();
  int side = 0;
  for (const double2 p : points) {
    if (p == a || p == b) {
      continue;
    }
    const int turn = orient2d(a, b, p);
    if (turn == 0 || (side != 0 && side != turn)) {
      return false;
    }
    side = turn;
  }
  if (side == 0) {
    return false;
  }
  const int degree = curve.order - 1;
  for (int i = 0; i + degree < points.size(); i += degree) {
    for (int j = i + degree; j + degree < points.size(); j += degree) {
      const bool adjacent = j == i + degree;
      const double2 shared = points[j];
      auto separates = [&](const double2 origin, const double2 end) {
        if (origin == end) {
          return false;
        }
        int sign = 0;
        for (int k = i; k <= i + degree; k++) {
          if (adjacent && points[k] == shared) {
            continue;
          }
          const int turn = orient2d(origin, end, points[k]);
          if (turn == 0 || (sign != 0 && turn != sign)) {
            return false;
          }
          sign = turn;
        }
        for (int k = j; k <= j + degree; k++) {
          if (adjacent && points[k] == shared) {
            continue;
          }
          if (orient2d(origin, end, points[k]) != -sign) {
            return false;
          }
        }
        return sign != 0;
      };
      /* A transverse line at a shared endpoint separates the neighboring hulls. */
      const double2 chord = points[j + degree] - points[i];
      bool separated = adjacent && separates(shared, shared + double2(-chord.y, chord.x));
      /* Strict separation avoids special overlap cases along the separating line.
       * Try lines halfway between pairs of hull vertices, normal to their separation. */
      for (int u = i; !separated && u <= i + degree; u++) {
        for (int v = j; !separated && v <= j + degree; v++) {
          const double2 delta = points[v] - points[u];
          const double2 middle = (points[u] + points[v]) * 0.5;
          separated = separates(middle, middle + double2(-delta.y, delta.x));
        }
      }
      if (!separated) {
        return false;
      }
    }
  }
  return true;
}

/** Sufficient exact-affine correspondence check; a nonzero arithmetic residual rejects. */
static std::optional<double> plane_height(
    const double a, const double b, const double c, const double u, const double v)
{
  double x, y, value, residual;
  if (!detail::product_with_error(a, u, x, residual) || residual != 0 ||
      !detail::product_with_error(b, v, y, residual) || residual != 0 ||
      !detail::sum_with_error(x, y, value, residual) || residual != 0 ||
      !detail::sum_with_error(value, c, value, residual) || residual != 0)
  {
    return std::nullopt;
  }
  return value;
}

static NurbsCurveGeometry *add_fill_line(NurbsGeometry &geometry, const double3 a, const double3 b)
{
  auto *curve = MEM_new<NurbsCurveGeometry>(__func__);
  curve->id = geometry.next_id++;
  curve->order = curve->points_num = 2;
  curve->knots_num = 4;
  curve->knots = MEM_new_array_zeroed<double>(4, __func__);
  curve->knots[2] = curve->knots[3] = 1;
  curve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
  std::copy_n(&a.x, 3, curve->points[0].position);
  std::copy_n(&b.x, 3, curve->points[1].position);
  BLI_addtail(&geometry.curves, curve);
  return curve;
}

/** Fourth corner of the affine chart through a, b and c. Try each addition order to avoid
 * unnecessary overflow/roundoff, retaining an outward bound for the stored coefficient.
 */
}  // namespace

std::optional<FillResult> fill_components(const NurbsGeometry &source,
                                          const ComponentSelection &selection,
                                          const double max_construction_error,
                                          std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<FillResult> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!detail::supported_arithmetic()) {
    return fail("NURBS fill requires round-to-nearest arithmetic and subnormal support");
  }
  if (!std::isfinite(max_construction_error) || max_construction_error < 0) {
    return fail("NURBS fill needs a finite non-negative construction error budget");
  }
  const auto closure = component_closure(source, selection, r_error);
  if (!closure) {
    return std::nullopt;
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  auto unchanged = [&]() {
    FillResult result;
    result.geometry.reset(geometry_copy(&source));
    return result;
  };
  if ((closure->vertices.size() < 2 && closure->edges.is_empty()) || closure->faces.size() == 1) {
    return unchanged();
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  for (const NurbsCurveGeometry &curve : source.curves) {
    curves.add_new(curve.id, &curve);
  }
  Vector<int64_t> edge_ids;
  if (closure->faces.is_empty()) {
    for (const NurbsTopologyEdge &edge : source.edges) {
      if (closure->edges.contains(edge.id)) {
        edge_ids.append(edge.id);
      }
    }
  }
  else {
    const Vector<int64_t> faces(closure->faces.begin(), closure->faces.end());
    const auto face_closure = component_closure(source, {{}, {}, faces}, r_error);
    if (!face_closure) {
      return std::nullopt;
    }
    if (closure->vertices != face_closure->vertices || closure->edges != face_closure->edges) {
      return fail("Fill the face region separately from independent selected wires or vertices");
    }
    const auto region = plan_face_region_extrusion(source, faces, r_error);
    if (!region) {
      return std::nullopt;
    }
    edge_ids = region->boundary_edges;
    if (edge_ids.is_empty()) {
      return fail("Selected faces have no boundary for an exact fill");
    }
  }
  Vector<FillSide> sides;
  const bool connect_two_vertices = closure->faces.is_empty() && closure->vertices.size() == 2 &&
                                    edge_ids.size() <= 1;
  if (connect_two_vertices) {
    Vector<int64_t> vertices;
    for (const NurbsTopologyVertex &vertex : source.vertices) {
      if (closure->vertices.contains(vertex.id)) {
        vertices.append(vertex.id);
      }
    }
    sides.append({vertices[0], vertices[1]});
  }
  else if (!edge_ids.is_empty()) {
    const auto chains = trace_edge_chains(source, edge_ids, r_error);
    if (!chains) {
      return std::nullopt;
    }
    EdgeChain chain;
    if (chains->size() == 2 && closure->faces.is_empty() && (*chains)[0].edges.size() == 1 &&
        (*chains)[1].edges.size() == 1 && !(*chains)[0].closed && !(*chains)[1].closed)
    {
      // Match BMesh edgenet_prepare: compare the two triangle-normal pairings to
      // avoid a bow tie. This chooses layout; construction checks follow below.
      const auto &a = (*chains)[0];
      const auto &b = (*chains)[1];
      double largest = 0;
      for (const auto &part : *chains) {
        for (const int64_t id : part.vertices) {
          for (const double coordinate : index->vertex(id)->position) {
            largest = std::max(largest, std::abs(coordinate));
          }
        }
      }
      const int exponent = largest == 0 ? 0 : std::ilogb(largest);
      auto point = [&](const int64_t id) {
        const double *position = index->vertex(id)->position;
        return double3(std::scalbn(position[0], -exponent),
                       std::scalbn(position[1], -exponent),
                       std::scalbn(position[2], -exponent));
      };
      auto normal = [](const double3 a, const double3 b, const double3 c) {
        const double3 value = math::cross(b - a, c - a);
        const double length = std::hypot(value.x, value.y, value.z);
        return length == 0 ? double3(0) : value / length;
      };
      const double3 v1 = point(a.vertices.first()), v2 = point(a.vertices.last());
      const double3 v3 = point(b.vertices.first()), v4 = point(b.vertices.last());
      const double dot_24 = math::dot(normal(v1, v2, v4), normal(v1, v4, v3));
      const double dot_13 = math::dot(normal(v1, v2, v3), normal(v1, v3, v4));
      const bool reverse = dot_24 >= dot_13;
      chain = a;
      chain.vertices.append(b.vertices[reverse ? 1 : 0]);
      chain.vertices.append(b.vertices[reverse ? 0 : 1]);
      chain.vertices.append(a.vertices.first());
      chain.edges.append(0);
      chain.edges.append(b.edges.first());
      chain.edges.append(0);
      chain.reversed.append(false);
      chain.reversed.append(b.reversed.first() != reverse);
      chain.reversed.append(false);
      chain.closed = true;
    }
    else if (chains->size() == 1) {
      chain = chains->first();
    }
    else {
      return fail(
          "NURBS fill requires one boundary or two open edges; separate or nested loops need "
          "an explicit layout");
    }
    Set<int64_t> boundary_vertices;
    for (const int64_t id : chain.vertices) {
      boundary_vertices.add(id);
    }
    Vector<int64_t> loose;
    if (closure->faces.is_empty()) {
      for (const NurbsTopologyVertex &vertex : source.vertices) {
        if (closure->vertices.contains(vertex.id) && !boundary_vertices.contains(vertex.id)) {
          loose.append(vertex.id);
        }
      }
    }
    if (loose.size() > 1 || (chain.closed && !loose.is_empty())) {
      return fail("Selected vertices do not define one NURBS fill boundary");
    }
    for (const int i : chain.edges.index_range()) {
      sides.append({chain.vertices[i], chain.vertices[i + 1], chain.edges[i], chain.reversed[i]});
    }
    if (!chain.closed) {
      if (chain.vertices.size() < 3 && loose.is_empty()) {
        return unchanged();
      }
      if (!loose.is_empty()) {
        sides.append({chain.vertices.last(), loose.first()});
        sides.append({loose.first(), chain.vertices.first()});
      }
      else {
        sides.append({chain.vertices.last(), chain.vertices.first()});
      }
    }
  }
  else {
    Vector<int64_t> vertices;
    Vector<double3> points;
    for (const NurbsTopologyVertex &vertex : source.vertices) {
      if (closure->vertices.contains(vertex.id)) {
        vertices.append(vertex.id);
        points.append(double3(vertex.position));
      }
    }
    const auto frame = fill_projection(points);
    if (!frame) {
      return fail("Selected vertices do not define a supported nondegenerate fill");
    }
    double2 center(0);
    for (const double3 &point : points) {
      center += frame->point(point) / double(points.size());
    }
    std::sort(vertices.begin(), vertices.end(), [&](const int64_t a, const int64_t b) {
      const double2 pa = frame->point(double3(index->vertex(a)->position)) - center;
      const double2 pb = frame->point(double3(index->vertex(b)->position)) - center;
      const double aa = std::atan2(pa.y, pa.x), ab = std::atan2(pb.y, pb.x);
      return aa != ab ? aa < ab : a < b;
    });
    for (const int i : vertices.index_range()) {
      sides.append({vertices[i], vertices[(i + 1) % vertices.size()]});
    }
  }
  if (sides.is_empty()) {
    return unchanged();
  }
  for (FillSide &side : sides) {
    if (side.edge != 0) {
      continue;
    }
    for (const NurbsTopologyEdge &edge : source.edges) {
      if ((edge.vertex_ids[0] == side.start && edge.vertex_ids[1] == side.end) ||
          (edge.vertex_ids[1] == side.start && edge.vertex_ids[0] == side.end))
      {
        /* Two endpoints of one arc request its straight closing chord. The existing
         * curved edge is retained as a distinct boundary between the same vertices. */
        const auto &curve = *curves.lookup(edge.curve_id);
        if (connect_two_vertices && (curve.order != 2 || curve.points_num != 2)) {
          continue;
        }
        if (side.edge != 0) {
          return fail("Endpoint pair has multiple NURBS edges; select the intended boundary");
        }
        side.edge = edge.id;
        side.reversed = edge.vertex_ids[0] != side.start;
      }
    }
  }
  if (sides.size() == 1 && sides.first().start != sides.first().end) {
    if (sides.first().edge != 0) {
      return unchanged();
    }
    if (source.next_id > std::numeric_limits<int64_t>::max() - 2) {
      return fail("NURBS identity space is exhausted");
    }
    FillResult result;
    result.geometry.reset(geometry_copy(&source));
    const auto *curve = add_fill_line(*result.geometry,
                                      double3(index->vertex(sides.first().start)->position),
                                      double3(index->vertex(sides.first().end)->position));
    auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
    edge->id = result.geometry->next_id++;
    edge->curve_id = curve->id;
    edge->vertex_ids[0] = sides.first().start;
    edge->vertex_ids[1] = sides.first().end;
    edge->parameter_range[1] = 1;
    BLI_addtail(&result.geometry->edges, edge);
    result.created_edges.append(edge->id);
    if (!geometry_validate_structure(*result.geometry, r_error)) {
      return std::nullopt;
    }
    return result;
  }
  Set<int64_t> boundary_ids;
  for (const auto &side : sides) {
    if (side.edge != 0) {
      boundary_ids.add(side.edge);
    }
  }
  if (boundary_ids.size() == sides.size() && closure->faces.is_empty()) {
    for (const NurbsTopologyFace &face : source.faces) {
      for (const NurbsTopologyLoop &loop : face.loops) {
        if (loop.is_inner || loop.edge_uses.count() != sides.size()) {
          continue;
        }
        bool same = true;
        for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
          same &= boundary_ids.contains(use.edge_id);
        }
        if (same) {
          return unchanged();
        }
      }
    }
  }
  Vector<double3> polygon;
  bool straight_quad = sides.size() == 4;
  bool straight_triangle = sides.size() == 3;
  bool piecewise_linear = true;
  for (const FillSide &side : sides) {
    if (side.edge == 0) {
      polygon.append(double3(index->vertex(side.start)->position));
      continue;
    }
    const auto &edge = *index->edge(side.edge);
    const auto &curve = *curves.lookup(edge.curve_id);
    if (!full_clamped_curve(curve, edge) ||
        double3(curve.points[0].position) !=
            double3(index->vertex(edge.vertex_ids[0])->position) ||
        double3(curve.points[curve.points_num - 1].position) !=
            double3(index->vertex(edge.vertex_ids[1])->position))
    {
      return fail(
          "Exact fill requires full clamped curves with exact shared endpoints and positive "
          "weights");
    }
    straight_quad &= curve.order == 2 && curve.points_num == 2;
    straight_triangle &= curve.order == 2 && curve.points_num == 2;
    piecewise_linear &= curve.order == 2;
    for (int i = 0; i < curve.points_num - 1; i++) {
      const int point = side.reversed ? curve.points_num - 1 - i : i;
      polygon.append(double3(curve.points[point].position));
    }
  }
  const auto frame = fill_projection(polygon);
  if (!frame) {
    return fail("NURBS fill cannot certify this boundary's coordinate range or area");
  }
  Vector<double2> projected;
  for (const auto &point : polygon) {
    projected.append(frame->point(point));
  }
  bool nonplanar_quad = false;
  if (straight_quad && closure->faces.is_empty()) {
    double3 corners[4];
    for (int i = 0; i < 4; i++) {
      corners[i] = frame->scaled(double3(index->vertex(sides[i].start)->position));
    }
    nonplanar_quad = orient3d(corners[0], corners[1], corners[2], corners[3]) != 0;
  }
  if (!nonplanar_quad && !polygon_is_simple(projected)) {
    return fail("NURBS fill boundary intersects itself or exceeds the certification budget");
  }
  const int turn = polygon_turn(projected);
  const bool bilinear = straight_quad && (nonplanar_quad || turn != 0);
  if (!piecewise_linear && !bilinear) {
    bool arch_and_chord = false;
    if (sides.size() == 2 && sides[0].edge != 0 && sides[1].edge != 0) {
      for (int i = 0; i < 2; i++) {
        const auto &line = *curves.lookup(index->edge(sides[i].edge)->curve_id);
        const auto &arch = *curves.lookup(index->edge(sides[1 - i].edge)->curve_id);
        arch_and_chord |= line.order == 2 && line.points_num == 2 && simple_arch(arch, *frame);
      }
    }
    if (turn == 0 && !arch_and_chord) {
      return fail("Curved planar fill currently requires a simple convex control polygon");
    }
    for (const auto &side : sides) {
      if (side.edge != 0 &&
          !composite_bezier_monotone(*curves.lookup(index->edge(side.edge)->curve_id), *frame))
      {
        return fail("Curved boundary needs an unavailable curve-span simplicity certificate");
      }
    }
  }
  double plane_a = 0, plane_b = 0, plane_c = 0;
  bool affine_triangle = false;
  const int u_axis = (frame->axis + 1) % 3, v_axis = (frame->axis + 2) % 3;
  auto check_plane = [&](const double3 &point) {
    const auto height = plane_height(plane_a, plane_b, plane_c, point[u_axis], point[v_axis]);
    return height && *height == point[frame->axis];
  };
  if (!bilinear || !closure->faces.is_empty()) {
    const double3 origin = polygon.first();
    bool found = false;
    for (int i = 1; i < polygon.size() && !found; i++) {
      for (int j = i + 1; j < polygon.size(); j++) {
        const double3 b = polygon[i] - origin, c = polygon[j] - origin;
        const double determinant = b[u_axis] * c[v_axis] - b[v_axis] * c[u_axis];
        if (determinant == 0 || !std::isfinite(determinant)) {
          continue;
        }
        plane_a = (b[frame->axis] * c[v_axis] - c[frame->axis] * b[v_axis]) / determinant;
        plane_b = (b[u_axis] * c[frame->axis] - c[u_axis] * b[frame->axis]) / determinant;
        plane_c = origin[frame->axis] - plane_a * origin[u_axis] - plane_b * origin[v_axis];
        found = std::isfinite(plane_a) && std::isfinite(plane_b) && std::isfinite(plane_c);
        if (found) {
          break;
        }
      }
    }
    const bool exact_plane = found && std::all_of(polygon.begin(), polygon.end(), check_plane);
    if (!exact_plane) {
      if (straight_triangle && closure->faces.is_empty()) {
        affine_triangle = true;
      }
      else {
        return fail(found ? "Boundary needs a nonplanar or tolerance-managed construction; no "
                            "fitting was applied" :
                            "Exact planar fill correspondence is not representable");
      }
    }
  }
  if (!closure->faces.is_empty()) {
    for (const NurbsSurfaceGeometry &surface : source.surfaces) {
      if (!closure->surfaces.contains(surface.id)) {
        continue;
      }
      for (int i = 0; i < surface.points_num; i++) {
        if (!(surface.points[i].weight > 0) || !check_plane(double3(surface.points[i].position))) {
          return fail("Selected curved supports cannot be replaced by an exact planar fill");
        }
      }
    }
  }
  // Orient the new UV loop positively. A four-corner chart always follows its own corner order.
  if (!bilinear && !affine_triangle) {
    double area = 0;
    for (const int i : projected.index_range()) {
      const auto &a = projected[i], &b = projected[(i + 1) % projected.size()];
      area += a.x * b.y - a.y * b.x;
    }
    if (area == 0 || !std::isfinite(area)) {
      return fail("NURBS fill boundary has no certifiable signed area");
    }
    if (area < 0) {
      std::reverse(sides.begin(), sides.end());
      for (auto &side : sides) {
        std::swap(side.start, side.end);
        side.reversed = !side.reversed;
      }
    }
  }
  int orientation = 1;
  std::optional<int> incidence_orientation;
  bool conflicting_hints = false;
  int material = 0;
  std::optional<int> selected_material;
  for (const NurbsTopologyFace &face : source.faces) {
    if (closure->faces.contains(face.id)) {
      if (selected_material && *selected_material != face.material_index) {
        return fail("Exact face merge requires matching face materials");
      }
      selected_material = face.material_index;
      material = face.material_index;
    }
  }
  for (const auto &side : sides) {
    if (side.edge == 0) {
      continue;
    }
    std::optional<int> edge_orientation;
    bool conflicting_uses = false;
    for (const auto &reference : index->uses_for_edge(side.edge)) {
      const bool selected = closure->faces.contains(reference.face_id);
      if (!closure->faces.is_empty() && !selected) {
        continue;
      }
      const int sense = index->face(reference.face_id)->orientation *
                        (reference.use->reversed ? -1 : 1);
      const int requested = (selected ? sense : -sense) * (side.reversed ? -1 : 1);
      conflicting_uses |= edge_orientation && *edge_orientation != requested;
      edge_orientation = requested;
    }
    if (conflicting_uses) {
      if (!closure->faces.is_empty()) {
        return fail("Selected faces have conflicting merge orientations");
      }
      // An existing manifold edge already has opposing uses. A third use cannot
      // oppose both, but remains a valid editing state, as with edge extrusion.
      continue;
    }
    if (edge_orientation) {
      conflicting_hints |= incidence_orientation && *incidence_orientation != *edge_orientation;
      incidence_orientation = edge_orientation;
    }
  }
  if (conflicting_hints && !closure->faces.is_empty()) {
    return fail("Selected faces have conflicting merge orientations");
  }
  if (incidence_orientation && !conflicting_hints) {
    orientation = *incidence_orientation;
  }
  double2 minimum(std::numeric_limits<double>::infinity()),
      maximum(-std::numeric_limits<double>::infinity());
  for (const double3 &point : polygon) {
    minimum.x = std::min(minimum.x, point[u_axis]);
    maximum.x = std::max(maximum.x, point[u_axis]);
    minimum.y = std::min(minimum.y, point[v_axis]);
    maximum.y = std::max(maximum.y, point[v_axis]);
  }
  double3 support_points[4];
  for (int i = 0; i < 4; i++) {
    if (bilinear) {
      const int corner = i == 2 ? 3 : i == 3 ? 2 : i;
      support_points[i] = double3(index->vertex(sides[corner].start)->position);
    }
    else {
      double3 point(0);
      point[u_axis] = i & 1 ? maximum.x : minimum.x;
      point[v_axis] = i & 2 ? maximum.y : minimum.y;
      const auto height = plane_height(plane_a, plane_b, plane_c, point[u_axis], point[v_axis]);
      if (!height) {
        if (straight_triangle && closure->faces.is_empty()) {
          affine_triangle = true;
          break;
        }
        return fail(
            "Exact planar support coefficients are not representable; no fitting was applied");
      }
      point[frame->axis] = *height;
      support_points[i] = point;
    }
  }
  double construction_error = 0;
  if (affine_triangle) {
    support_points[0] = double3(index->vertex(sides[0].start)->position);
    support_points[1] = double3(index->vertex(sides[1].start)->position);
    support_points[2] = double3(index->vertex(sides[2].start)->position);
    const auto fourth = detail::affine_fourth_corner(
        support_points[0], support_points[1], support_points[2], construction_error);
    if (!fourth || construction_error > max_construction_error) {
      return fail("NURBS triangle support construction exceeds the requested roundoff budget");
    }
    support_points[3] = *fourth;
  }
  const bool corner_chart = bilinear || affine_triangle;
  uint64_t missing = 0;
  for (const auto &side : sides) {
    missing += side.edge == 0;
  }
  const uint64_t available = uint64_t(std::numeric_limits<int64_t>::max() - source.next_id);
  if (missing > available / 2 || uint64_t(sides.size()) > (available - 2 * missing) / 2 ||
      3 > available - 2 * missing - 2 * uint64_t(sides.size()))
  {
    return fail("NURBS identity space is exhausted");
  }
  FillResult result;
  result.construction_error_bound = construction_error;
  result.geometry.reset(geometry_copy(&source));
  auto &geometry = *result.geometry;
  for (auto &side : sides) {
    if (side.edge != 0) {
      continue;
    }
    const auto *curve = add_fill_line(geometry,
                                      double3(index->vertex(side.start)->position),
                                      double3(index->vertex(side.end)->position));
    auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
    edge->id = geometry.next_id++;
    edge->curve_id = curve->id;
    edge->vertex_ids[0] = side.start;
    edge->vertex_ids[1] = side.end;
    edge->parameter_range[1] = 1;
    BLI_addtail(&geometry.edges, edge);
    side.edge = edge->id;
    side.reversed = false;
    curves.add_new(curve->id, curve);
    result.created_edges.append(edge->id);
  }
  const auto candidate_index = TopologyIndex::build(geometry, r_error);
  if (!candidate_index) {
    return std::nullopt;
  }
  auto *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
  surface->id = geometry.next_id++;
  surface->order_u = surface->order_v = surface->points_u = surface->points_v = 2;
  surface->points_num = 4;
  surface->knots_u_num = surface->knots_v_num = 4;
  surface->points = MEM_new_array<NurbsControlPoint>(4, __func__);
  surface->knots_u = MEM_new_array<double>(4, __func__);
  surface->knots_v = MEM_new_array<double>(4, __func__);
  for (int i = 0; i < 4; i++) {
    std::copy_n(&support_points[i].x, 3, surface->points[i].position);
    surface->knots_u[i] = i < 2 ? (corner_chart ? 0 : minimum.x) : (corner_chart ? 1 : maximum.x);
    surface->knots_v[i] = i < 2 ? (corner_chart ? 0 : minimum.y) : (corner_chart ? 1 : maximum.y);
  }
  BLI_addtail(&geometry.surfaces, surface);
  auto *face = MEM_new<NurbsTopologyFace>(__func__);
  face->id = geometry.next_id++;
  face->surface_id = surface->id;
  face->orientation = orientation;
  face->material_index = material;
  face->tolerance = construction_error;
  BLI_addtail(&geometry.faces, face);
  auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
  loop->id = geometry.next_id++;
  BLI_addtail(&face->loops, loop);
  const double2 corners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  const double2 triangle_corners[3] = {{0, 0}, {1, 0}, {0, 1}};
  for (const int i : sides.index_range()) {
    const auto &side = sides[i];
    const auto &edge = *candidate_index->edge(side.edge);
    const auto &curve = *curves.lookup(edge.curve_id);
    auto *pcurve = MEM_dupalloc(&curve);
    pcurve->id = geometry.next_id++;
    pcurve->dimension = 2;
    pcurve->knots = MEM_dupalloc(curve.knots);
    pcurve->points = MEM_dupalloc(curve.points);
    for (int j = 0; j < pcurve->points_num; j++) {
      double2 uv;
      if (corner_chart) {
        const int corner = (i + (side.reversed ? 1 - j : j)) % sides.size();
        uv = affine_triangle ? triangle_corners[corner] : corners[corner];
      }
      else {
        uv = {curve.points[j].position[u_axis], curve.points[j].position[v_axis]};
      }
      pcurve->points[j].position[0] = uv.x;
      pcurve->points[j].position[1] = uv.y;
      pcurve->points[j].position[2] = 0;
    }
    BLI_addtail(&geometry.curves, pcurve);
    auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
    use->id = geometry.next_id++;
    use->edge_id = edge.id;
    use->pcurve_id = pcurve->id;
    use->reversed = side.reversed;
    use->tolerance = construction_error;
    BLI_addtail(&loop->edge_uses, use);
  }
  result.created_faces.append(face->id);
  if (!closure->faces.is_empty()) {
    const Vector<int64_t> original_faces(closure->faces.begin(), closure->faces.end());
    auto deleted = delete_components(
        geometry, {{}, {}, original_faces}, DeleteMode::Faces, r_error);
    if (!deleted) {
      return std::nullopt;
    }
    result.geometry = std::move(deleted->geometry);
    result.removed = std::move(deleted->removed);
  }
  if (!geometry_validate_structure(*result.geometry, r_error)) {
    return std::nullopt;
  }
  return result;
}

std::optional<FillResult> fill_components(const NurbsGeometry &source,
                                          const ComponentSelection &selection,
                                          std::string *r_error)
{
  return fill_components(source, selection, 0, r_error);
}

}  // namespace blender::bke::nurbs
