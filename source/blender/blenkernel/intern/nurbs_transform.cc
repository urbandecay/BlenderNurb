/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <array>
#include <numeric>

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_nurbs_transform.hh"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {

/** Combine induced-norm and Gram-matrix bounds; signed axis permutations/scales are exact. */
static double linear_norm_bound(const double4x4 &matrix)
{
  int row_entries[3] = {}, column_entries[3] = {};
  double row_sums[3] = {}, column_sums[3] = {};
  double maximum = 0.0;
  for (int column = 0; column < 3; column++) {
    for (int row = 0; row < 3; row++) {
      const double value = std::abs(matrix[column][row]);
      maximum = std::max(maximum, value);
      row_entries[row] += value != 0.0;
      column_entries[column] += value != 0.0;
      row_sums[row] = detail::add_upper(row_sums[row], value);
      column_sums[column] = detail::add_upper(column_sums[column], value);
    }
  }
  bool monomial = true;
  for (int i = 0; i < 3; i++) {
    monomial &= row_entries[i] <= 1 && column_entries[i] <= 1;
  }
  if (monomial) {
    return maximum;
  }
  const double one = *std::max_element(column_sums, column_sums + 3);
  const double infinity = *std::max_element(row_sums, row_sums + 3);
  const double root_one = std::nextafter(std::sqrt(one), std::numeric_limits<double>::infinity());
  const double root_infinity = std::nextafter(std::sqrt(infinity),
                                              std::numeric_limits<double>::infinity());
  const double induced_bound = detail::multiply_upper(root_one, root_infinity);
  /* A bound from A^T A stays close to one for rotations. Repeatedly using only the induced
   * bound would inflate existing model tolerances even when the transform is nearly rigid. */
  double gram_row_bound = 0.0;
  for (int i = 0; i < 3; i++) {
    double row_bound = 0.0;
    for (int j = 0; j < 3; j++) {
      double value = 0.0, error = 0.0;
      for (int k = 0; k < 3; k++) {
        double product, product_error, sum, sum_error;
        if (!detail::product_with_error(matrix[i][k], matrix[j][k], product, product_error) ||
            !detail::sum_with_error(value, product, sum, sum_error))
        {
          return induced_bound;
        }
        value = sum;
        error = detail::add_upper(error, product_error);
        error = detail::add_upper(error, std::abs(sum_error));
      }
      row_bound = detail::add_upper(row_bound, detail::add_upper(std::abs(value), error));
    }
    gram_row_bound = std::max(gram_row_bound, row_bound);
  }
  const double gram_bound = std::nextafter(std::sqrt(gram_row_bound),
                                           std::numeric_limits<double>::infinity());
  return std::min(induced_bound, gram_bound);
}

static bool transform_position(double position[3], const double4x4 &matrix, double &r_error_bound)
{
  const double3 source(position);
  double point_error = 0.0;
  for (int row = 0; row < 3; row++) {
    double value = matrix[3][row];
    double coordinate_error = 0.0;
    for (int column = 0; column < 3; column++) {
      double product, product_error, sum, sum_error;
      if (!detail::product_with_error(
              matrix[column][row], source[column], product, product_error) ||
          !detail::sum_with_error(value, product, sum, sum_error))
      {
        return false;
      }
      value = sum;
      coordinate_error = detail::add_upper(coordinate_error, product_error);
      coordinate_error = detail::add_upper(coordinate_error, std::abs(sum_error));
    }
    position[row] = value;
    point_error = detail::add_upper(point_error, coordinate_error);
  }
  r_error_bound = std::max(r_error_bound, point_error);
  return std::isfinite(r_error_bound);
}

std::optional<GeometryTransformResult> transform_geometry(const NurbsGeometry &source,
                                                          const double4x4 &matrix,
                                                          const double max_position_error,
                                                          std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<GeometryTransformResult> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!std::isfinite(max_position_error) || max_position_error < 0.0) {
    return fail("NURBS transform needs a finite non-negative error budget");
  }
  for (int column = 0; column < 4; column++) {
    for (int row = 0; row < 4; row++) {
      if (!std::isfinite(matrix[column][row])) {
        return fail("NURBS transform matrix is not finite");
      }
    }
  }
  if (matrix[0][3] != 0.0 || matrix[1][3] != 0.0 || matrix[2][3] != 0.0 || matrix[3][3] != 1.0) {
    return fail("NURBS source transform requires an affine matrix");
  }
  if (!detail::supported_arithmetic()) {
    return fail(
        "NURBS source transform requires round-to-nearest arithmetic and subnormal support");
  }
  if (!geometry_validate_structure(source, r_error)) {
    return std::nullopt;
  }
  GeometryTransformResult result;
  result.tolerance_scale_bound = linear_norm_bound(matrix);
  if (!std::isfinite(result.tolerance_scale_bound)) {
    return fail("NURBS transform tolerance scale is not representable");
  }
  result.geometry.reset(geometry_copy(&source));
  NurbsGeometry &geometry = *result.geometry;
  auto transform_points = [&](NurbsControlPoint *points, const int points_num) {
    for (int i = 0; i < points_num; i++) {
      if (points[i].weight <= 0.0 ||
          !transform_position(points[i].position, matrix, result.position_error_bound))
      {
        return false;
      }
    }
    return true;
  };
  for (NurbsCurveGeometry &curve : geometry.curves) {
    if (curve.dimension == 3 && !transform_points(curve.points, curve.points_num)) {
      return fail("NURBS 3D curve transform needs positive weights and finite arithmetic");
    }
  }
  for (NurbsSurfaceGeometry &surface : geometry.surfaces) {
    if (!transform_points(surface.points, surface.points_num)) {
      return fail("NURBS support transform needs positive weights and finite arithmetic");
    }
  }
  for (NurbsTopologyVertex &vertex : geometry.vertices) {
    if (!transform_position(vertex.position, matrix, result.position_error_bound)) {
      return fail("NURBS vertex transform overflowed");
    }
  }
  if (result.position_error_bound > max_position_error) {
    return fail("NURBS transform roundoff bound exceeds the requested error budget");
  }
  const double extra = detail::multiply_upper(2.0, result.position_error_bound);
  auto expand = [&](const double tolerance) {
    return detail::add_upper(detail::multiply_upper(tolerance, result.tolerance_scale_bound),
                             extra);
  };
  for (NurbsTopologyVertex &vertex : geometry.vertices) {
    vertex.tolerance = expand(vertex.tolerance);
  }
  for (NurbsTopologyEdge &edge : geometry.edges) {
    edge.tolerance = expand(edge.tolerance);
  }
  for (NurbsTopologyFace &face : geometry.faces) {
    face.tolerance = expand(face.tolerance);
    for (NurbsTopologyLoop &loop : face.loops) {
      for (NurbsTopologyEdgeUse &use : loop.edge_uses) {
        use.tolerance = expand(use.tolerance);
      }
    }
  }
  if (!geometry_validate_structure(geometry, r_error)) {
    return std::nullopt;
  }
  return result;
}

/** Exact comparison of positive binary64 products without overflowing intermediate products. */
static bool equal_weight_products(const std::array<double, 3> &left,
                                  const std::array<double, 3> &right)
{
  uint64_t a[3], b[3];
  int exponent = 0;
  auto split = [](const double value, uint64_t &r_odd) {
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    const int encoded = int((bits >> 52) & 0x7ff);
    uint64_t significand = bits & ((uint64_t(1) << 52) - 1);
    int result = -1074;
    if (encoded != 0) {
      significand |= uint64_t(1) << 52;
      result = encoded - 1023 - 52;
    }
    const int zeros = std::countr_zero(significand);
    r_odd = significand >> zeros;
    return result + zeros;
  };
  for (int i = 0; i < 3; i++) {
    if (!(left[i] > 0) || !(right[i] > 0) || !std::isfinite(left[i]) || !std::isfinite(right[i])) {
      return false;
    }
    exponent += split(left[i], a[i]) - split(right[i], b[i]);
  }
  if (exponent != 0) {
    return false;
  }
  for (uint64_t &numerator : a) {
    for (uint64_t &denominator : b) {
      const uint64_t factor = std::gcd(numerator, denominator);
      numerator /= factor;
      denominator /= factor;
    }
  }
  return std::all_of(a, a + 3, [](const uint64_t value) { return value == 1; }) &&
         std::all_of(b, b + 3, [](const uint64_t value) { return value == 1; });
}

static bool linear_curve(const NurbsCurveGeometry &curve, const double start, const double end)
{
  return curve.order == 2 && curve.points_num == 2 && curve.knots[0] == start &&
         curve.knots[1] == start && curve.knots[2] == end && curve.knots[3] == end;
}

static bool clamped_direction(const int order, const int points_num, const double *knots)
{
  for (int i = 0; i < order; i++) {
    if (knots[i] != knots[order - 1] || knots[points_num + i] != knots[points_num]) {
      return false;
    }
  }
  return true;
}

/** A byte per coefficient is a movement request, not a selectable control-point domain. */
using CoefficientRequests = Map<int64_t, Vector<uint8_t>>;

static bool add_request(CoefficientRequests &requests, const int64_t id, Vector<uint8_t> request)
{
  if (const Vector<uint8_t> *previous = requests.lookup_ptr(id)) {
    return previous->size() == request.size() &&
           std::equal(previous->begin(), previous->end(), request.begin());
  }
  requests.add_new(id, std::move(request));
  return true;
}

std::optional<GeometryTransformResult> translate_vertices(const NurbsGeometry &source,
                                                          const Span<int64_t> vertex_ids,
                                                          const double3 &offset,
                                                          const double max_position_error,
                                                          std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<GeometryTransformResult> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (vertex_ids.is_empty() || !std::isfinite(offset.x) || !std::isfinite(offset.y) ||
      !std::isfinite(offset.z) || !std::isfinite(max_position_error) || max_position_error < 0)
  {
    return fail("NURBS vertex translation needs vertices, a finite offset and an error budget");
  }
  if (!detail::supported_arithmetic()) {
    return fail(
        "NURBS vertex translation requires round-to-nearest arithmetic and subnormal support");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Set<int64_t> selected, moved_edges, moved_faces;
  for (const int64_t id : vertex_ids) {
    if (index->vertex(id) == nullptr) {
      return fail("Selected NURBS vertex identity does not exist");
    }
    selected.add(id);
  }
  if (offset == double3(0)) {
    GeometryTransformResult result;
    result.geometry.reset(geometry_copy(&source));
    return result;
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : source.curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  CoefficientRequests curve_requests, surface_requests;
  for (const NurbsTopologyEdge &edge : source.edges) {
    const NurbsCurveGeometry &curve = *curves.lookup(edge.curve_id);
    const bool a = selected.contains(edge.vertex_ids[0]);
    const bool b = selected.contains(edge.vertex_ids[1]);
    Vector<uint8_t> request;
    request.resize(curve.points_num);
    std::fill(request.begin(), request.end(), uint8_t(a && b));
    if (a != b) {
      if (!linear_curve(curve, edge.parameter_range[0], edge.parameter_range[1])) {
        return fail("Moving part of a curved NURBS edge requires a fit-point deformation rule");
      }
      request[0] = a;
      request[1] = b;
    }
    if (a || b) {
      moved_edges.add(edge.id);
      for (int end = 0; end < 2; end++) {
        const auto point = evaluate_curve(curve, edge.parameter_range[end], r_error);
        if (!point) {
          return std::nullopt;
        }
        const auto &vertex = *index->vertex(edge.vertex_ids[end]);
        const double3 difference = point->position - double3(vertex.position);
        if (std::hypot(difference.x, difference.y, difference.z) >
            std::max(vertex.tolerance, edge.tolerance))
        {
          return fail("NURBS edge endpoint disagrees with its source vertex tolerance");
        }
      }
    }
    if (!add_request(curve_requests, curve.id, std::move(request))) {
      return fail("Shared NURBS curve owners request conflicting geometry translations");
    }
  }
  for (const NurbsTopologyFace &face : source.faces) {
    const NurbsSurfaceGeometry &surface = *surfaces.lookup(face.surface_id);
    bool any = false, all = true;
    for (const NurbsTopologyLoop &loop : face.loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const auto &edge = *index->edge(use.edge_id);
        for (const int64_t id : edge.vertex_ids) {
          any |= selected.contains(id);
          all &= selected.contains(id);
        }
      }
    }
    Vector<uint8_t> request;
    request.resize(surface.points_num);
    std::fill(request.begin(), request.end(), uint8_t(all));
    if (any) {
      moved_faces.add(face.id);
    }
    if (any && !all) {
      if (!face.loops.is_single() ||
          static_cast<const NurbsTopologyLoop *>(face.loops.first)->is_inner ||
          static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses.count() != 4 ||
          !clamped_direction(surface.order_u, surface.points_u, surface.knots_u) ||
          !clamped_direction(surface.order_v, surface.points_v, surface.knots_v))
      {
        return fail("Partial NURBS movement currently requires a complete clamped tensor face");
      }
      const bool linear_u = surface.order_u == 2 && surface.points_u == 2;
      const bool linear_v = surface.order_v == 2 && surface.points_v == 2;
      if (!linear_u && !linear_v) {
        return fail("Partial curved NURBS surface movement requires a deformation rule");
      }
      const double low[2] = {surface.knots_u[surface.order_u - 1],
                             surface.knots_v[surface.order_v - 1]};
      const double high[2] = {surface.knots_u[surface.points_u],
                              surface.knots_v[surface.points_v]};
      int64_t corners[4] = {};
      const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const auto &edge = *index->edge(use.edge_id);
        const auto &pcurve = *curves.lookup(use.pcurve_id);
        if (!linear_curve(pcurve, edge.parameter_range[0], edge.parameter_range[1])) {
          return fail("Partial NURBS movement requires complete linear boundary p-curves");
        }
        int slots[2];
        for (int end = 0; end < 2; end++) {
          int slot = 0;
          for (int axis = 0; axis < 2; axis++) {
            const double coordinate = pcurve.points[end].position[axis];
            if (coordinate != low[axis] && coordinate != high[axis]) {
              return fail("Partial trimmed NURBS face movement requires a deformation rule");
            }
            slot += (coordinate == high[axis]) ? (1 << axis) : 0;
          }
          slots[end] = slot;
          const auto &vertex = *index->vertex(edge.vertex_ids[end]);
          if (corners[slot] != 0 && corners[slot] != vertex.id) {
            return fail("NURBS tensor corner has inconsistent source vertex identity");
          }
          corners[slot] = vertex.id;
          const int coefficient = (slot & 1 ? surface.points_u - 1 : 0) +
                                  (slot & 2 ? surface.points_u * (surface.points_v - 1) : 0);
          const double3 difference = double3(surface.points[coefficient].position) -
                                     double3(vertex.position);
          const double tolerance = std::max(
              {face.tolerance, use.tolerance, edge.tolerance, vertex.tolerance});
          if (std::hypot(difference.x, difference.y, difference.z) > tolerance) {
            return fail("NURBS support corner disagrees with its source vertex tolerance");
          }
        }
        if ((slots[0] ^ slots[1]) != 1 && (slots[0] ^ slots[1]) != 2) {
          return fail("NURBS boundary p-curve is not a complete tensor boundary");
        }
        if (selected.contains(edge.vertex_ids[0]) != selected.contains(edge.vertex_ids[1])) {
          const auto &curve = *curves.lookup(edge.curve_id);
          const auto corner_weight = [&](const int slot) {
            const int coefficient = (slot & 1 ? surface.points_u - 1 : 0) +
                                    (slot & 2 ? surface.points_u * (surface.points_v - 1) : 0);
            return surface.points[coefficient].weight;
          };
          if (!equal_weight_products(
                  {pcurve.points[0].weight, corner_weight(slots[0]), curve.points[1].weight},
                  {pcurve.points[1].weight, corner_weight(slots[1]), curve.points[0].weight}))
          {
            return fail("NURBS boundary rational parameterization does not match its support");
          }
        }
      }
      if (std::any_of(corners, corners + 4, [](const int64_t id) { return id == 0; })) {
        return fail("NURBS partial movement needs all four tensor corner associations");
      }
      if ((!linear_u && (selected.contains(corners[0]) != selected.contains(corners[1]) ||
                         selected.contains(corners[2]) != selected.contains(corners[3]))) ||
          (!linear_v && (selected.contains(corners[0]) != selected.contains(corners[2]) ||
                         selected.contains(corners[1]) != selected.contains(corners[3]))))
      {
        return fail("Move a complete ruled NURBS boundary or define a curved deformation rule");
      }
      for (int v = 0; v < surface.points_v; v++) {
        for (int u = 0; u < surface.points_u; u++) {
          const int slot = (linear_u ? u : 0) + (linear_v ? 2 * v : 0);
          request[v * surface.points_u + u] = selected.contains(corners[slot]);
        }
      }
    }
    if (!add_request(surface_requests, surface.id, std::move(request))) {
      return fail("Shared NURBS support owners request conflicting geometry translations");
    }
  }
  GeometryTransformResult result;
  result.geometry.reset(geometry_copy(&source));
  auto translate = [&](double position[3]) {
    const double3 original(position);
    if (!detail::translate_position(position, offset, result.position_error_bound)) {
      return false;
    }
    for (int axis = 0; axis < 3; axis++) {
      if (offset[axis] == 0) {
        position[axis] = original[axis];
      }
    }
    return true;
  };
  auto apply = [&](NurbsControlPoint *points, const Vector<uint8_t> &request) {
    if (std::any_of(
            request.begin(), request.end(), [](const uint8_t value) { return value != 0; }))
    {
      for (const int i : request.index_range()) {
        if (points[i].weight <= 0) {
          return false;
        }
      }
    }
    for (const int i : request.index_range()) {
      if (request[i] && !translate(points[i].position)) {
        return false;
      }
    }
    return true;
  };
  for (NurbsCurveGeometry &curve : result.geometry->curves) {
    if (const Vector<uint8_t> *request = curve_requests.lookup_ptr(curve.id)) {
      if (!apply(curve.points, *request)) {
        return fail(
            "NURBS curve movement needs positive weights and finite coefficient additions");
      }
    }
  }
  for (NurbsSurfaceGeometry &surface : result.geometry->surfaces) {
    if (const Vector<uint8_t> *request = surface_requests.lookup_ptr(surface.id)) {
      if (!apply(surface.points, *request)) {
        return fail(
            "NURBS surface movement needs positive weights and finite coefficient additions");
      }
    }
  }
  for (NurbsTopologyVertex &vertex : result.geometry->vertices) {
    if (selected.contains(vertex.id) && !translate(vertex.position)) {
      return fail("NURBS vertex movement overflowed");
    }
  }
  if (result.position_error_bound > max_position_error) {
    return fail("NURBS vertex translation roundoff exceeds the requested error budget");
  }
  const double extra = detail::multiply_upper(2, result.position_error_bound);
  for (NurbsTopologyVertex &vertex : result.geometry->vertices) {
    if (selected.contains(vertex.id)) {
      vertex.tolerance = detail::expand_tolerance(vertex.tolerance, extra);
    }
  }
  for (NurbsTopologyEdge &edge : result.geometry->edges) {
    if (moved_edges.contains(edge.id)) {
      edge.tolerance = detail::expand_tolerance(edge.tolerance, extra);
    }
  }
  for (NurbsTopologyFace &face : result.geometry->faces) {
    if (moved_faces.contains(face.id)) {
      face.tolerance = detail::expand_tolerance(face.tolerance, extra);
    }
    for (NurbsTopologyLoop &loop : face.loops) {
      for (NurbsTopologyEdgeUse &use : loop.edge_uses) {
        if (moved_edges.contains(use.edge_id)) {
          use.tolerance = detail::expand_tolerance(use.tolerance, extra);
        }
      }
    }
  }
  if (!geometry_validate_structure(*result.geometry, r_error)) {
    return std::nullopt;
  }
  return result;
}

}  // namespace blender::bke::nurbs
