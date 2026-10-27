/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <array>
#include <numeric>

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_evaluate.hh"
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
  if (matrix[0] == double4(1, 0, 0, 0) && matrix[1] == double4(0, 1, 0, 0) &&
      matrix[2] == double4(0, 0, 1, 0))
  {
    return detail::translate_position(position, double3(matrix.location()), r_error_bound);
  }
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

static std::optional<GeometryTransformResult> transform_geometry_impl(
    const NurbsGeometry &source,
    const double4x4 &matrix,
    const double max_position_error,
    const bool active_only,
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
  Set<int64_t> active_curves, active_surfaces;
  if (active_only) {
    for (const auto &edge : source.edges) {
      active_curves.add(edge.curve_id);
    }
    for (const auto &face : source.faces) {
      active_surfaces.add(face.surface_id);
    }
    /* The component-edit API also checks each moved edge's geometric endpoints. A uniform
     * move can skip coefficient-assignment planning, but must retain this input-validity gate. */
    Map<int64_t, const NurbsTopologyVertex *> vertices;
    Map<int64_t, const NurbsCurveGeometry *> curves;
    for (const auto &vertex : source.vertices) {
      vertices.add_new(vertex.id, &vertex);
    }
    for (const auto &curve : source.curves) {
      curves.add_new(curve.id, &curve);
    }
    for (const auto &edge : source.edges) {
      for (int end = 0; end < 2; end++) {
        const auto point = evaluate_curve(*curves.lookup(edge.curve_id),
                                          edge.parameter_range[end],
                                          r_error);
        if (!point) {
          return std::nullopt;
        }
        const auto &vertex = *vertices.lookup(edge.vertex_ids[end]);
        const double3 difference = point->position - double3(vertex.position);
        if (std::hypot(difference.x, difference.y, difference.z) >
            std::max(vertex.tolerance, edge.tolerance))
        {
          return fail("NURBS edge endpoint disagrees with its source vertex tolerance");
        }
      }
    }
  }
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
    if (curve.dimension == 3 && (!active_only || active_curves.contains(curve.id)) &&
        !transform_points(curve.points, curve.points_num))
    {
      return fail("NURBS 3D curve transform needs positive weights and finite arithmetic");
    }
  }
  for (NurbsSurfaceGeometry &surface : geometry.surfaces) {
    if (active_only && !active_surfaces.contains(surface.id)) {
      continue;
    }
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

std::optional<GeometryTransformResult> transform_geometry(const NurbsGeometry &source,
                                                          const double4x4 &matrix,
                                                          const double max_position_error,
                                                          std::string *r_error)
{
  return transform_geometry_impl(source, matrix, max_position_error, false, r_error);
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

/**
 * Prove a complete ruled-boundary translation from source parameterization, including rectangular
 * interior trims. Topology corners identify rows, not control coefficients at trim endpoints.
 * An unrecognized correspondence leaves the existing tensor-corner rule to the caller.
 */
static std::optional<Vector<uint8_t>> ruled_row_request(
    const NurbsTopologyFace &face,
    const NurbsSurfaceGeometry &surface,
    const TopologyIndex &index,
    const Map<int64_t, const NurbsCurveGeometry *> &curves,
    const Set<int64_t> &selected,
    std::string &r_error)
{
  if (!face.loops.is_single()) {
    return std::nullopt;
  }
  const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
  if (loop.is_inner || loop.edge_uses.count() != 4) {
    return std::nullopt;
  }
  const int orders[2] = {surface.order_u, surface.order_v};
  const int counts[2] = {surface.points_u, surface.points_v};
  const int knot_counts[2] = {surface.knots_u_num, surface.knots_v_num};
  const double *knots[2] = {surface.knots_u, surface.knots_v};
  const int strides[2] = {1, surface.points_u};
  for (int linear_axis = 0; linear_axis < 2; linear_axis++) {
    if (orders[linear_axis] != 2 || counts[linear_axis] != 2 ||
        !clamped_direction(orders[linear_axis], counts[linear_axis], knots[linear_axis]))
    {
      continue;
    }
    const int curve_axis = 1 - linear_axis;
    const double low = knots[linear_axis][1], high = knots[linear_axis][2];
    const NurbsTopologyEdgeUse *rows[2] = {};
    Vector<const NurbsTopologyEdgeUse *> rails;
    bool valid = true;
    for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
      const auto &edge = *index.edge(use.edge_id);
      const auto &pcurve = *curves.lookup(use.pcurve_id);
      if (!linear_curve(pcurve, edge.parameter_range[0], edge.parameter_range[1])) {
        valid = false;
        break;
      }
      const auto &a = pcurve.points[0], &b = pcurve.points[1];
      if (a.position[linear_axis] == b.position[linear_axis] &&
          (a.position[linear_axis] == low || a.position[linear_axis] == high))
      {
        const int row = a.position[linear_axis] == high;
        if (rows[row] || a.position[curve_axis] != edge.parameter_range[0] ||
            b.position[curve_axis] != edge.parameter_range[1] || !(a.weight > 0) ||
            a.weight != b.weight)
        {
          valid = false;
          break;
        }
        rows[row] = &use;
      }
      else if (a.position[curve_axis] == b.position[curve_axis] &&
               ((a.position[linear_axis] == low && b.position[linear_axis] == high) ||
                (a.position[linear_axis] == high && b.position[linear_axis] == low)))
      {
        rails.append(&use);
      }
      else {
        valid = false;
        break;
      }
    }
    if (!valid || !rows[0] || !rows[1] || rails.size() != 2) {
      continue;
    }
    const auto &first_edge = *index.edge(rows[0]->edge_id);
    const double curve_low = first_edge.parameter_range[0];
    const double curve_high = first_edge.parameter_range[1];
    const auto coefficient = [&](const int row, const int i) -> const NurbsControlPoint & {
      return surface.points[row * strides[linear_axis] + i * strides[curve_axis]];
    };
    const NurbsCurveGeometry *row_curves[2];
    for (int row = 0; row < 2 && valid; row++) {
      const auto &edge = *index.edge(rows[row]->edge_id);
      const auto &curve = *curves.lookup(edge.curve_id);
      row_curves[row] = &curve;
      if (edge.parameter_range[0] != curve_low || edge.parameter_range[1] != curve_high ||
          curve.order != orders[curve_axis] || curve.points_num != counts[curve_axis] ||
          curve.knots_num != knot_counts[curve_axis] ||
          !std::equal(curve.knots, curve.knots + curve.knots_num, knots[curve_axis]))
      {
        valid = false;
        break;
      }
      for (int i = 0; i < counts[curve_axis]; i++) {
        if (double3(curve.points[i].position) != double3(coefficient(row, i).position) ||
            !equal_weight_products({curve.points[i].weight, coefficient(row, 0).weight, 1},
                                   {coefficient(row, i).weight, curve.points[0].weight, 1}))
        {
          valid = false;
          break;
        }
      }
    }
    for (int i = 0; i < counts[curve_axis] && valid; i++) {
      valid = equal_weight_products({coefficient(0, i).weight, coefficient(1, 0).weight, 1},
                                    {coefficient(1, i).weight, coefficient(0, 0).weight, 1});
    }
    if (!valid) {
      continue;
    }
    int64_t corners[4] = {};
    for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
      const auto &edge = *index.edge(use.edge_id);
      const auto &pcurve = *curves.lookup(use.pcurve_id);
      for (int end = 0; end < 2; end++) {
        const auto &point = pcurve.points[end];
        if ((point.position[curve_axis] != curve_low &&
             point.position[curve_axis] != curve_high) ||
            (point.position[linear_axis] != low && point.position[linear_axis] != high))
        {
          valid = false;
          break;
        }
        const int slot = int(point.position[curve_axis] == curve_high) +
                         2 * int(point.position[linear_axis] == high);
        if (corners[slot] != 0 && corners[slot] != edge.vertex_ids[end]) {
          valid = false;
          break;
        }
        corners[slot] = edge.vertex_ids[end];
      }
      if (!valid) {
        break;
      }
    }
    if (!valid || std::any_of(corners, corners + 4, [](int64_t id) { return id == 0; }) ||
        selected.contains(corners[0]) != selected.contains(corners[1]) ||
        selected.contains(corners[2]) != selected.contains(corners[3]))
    {
      continue;
    }
    bool sides[2] = {};
    for (const NurbsTopologyEdgeUse *use : rails) {
      const auto &edge = *index.edge(use->edge_id);
      const auto &curve = *curves.lookup(edge.curve_id);
      const auto &pcurve = *curves.lookup(use->pcurve_id);
      if (!linear_curve(curve, edge.parameter_range[0], edge.parameter_range[1])) {
        valid = false;
        break;
      }
      const int side = pcurve.points[0].position[curve_axis] == curve_high;
      if (sides[side]) {
        valid = false;
        break;
      }
      sides[side] = true;
      const int start_row = pcurve.points[0].position[linear_axis] == high;
      const int end_row = pcurve.points[1].position[linear_axis] == high;
      if (!equal_weight_products(
              {pcurve.points[0].weight, coefficient(start_row, 0).weight, curve.points[1].weight},
              {pcurve.points[1].weight, coefficient(end_row, 0).weight, curve.points[0].weight}))
      {
        valid = false;
        break;
      }
    }
    if (!valid || !sides[0] || !sides[1]) {
      continue;
    }
    for (const NurbsTopologyEdgeUse *use : rails) {
      const auto &edge = *index.edge(use->edge_id);
      const auto &curve = *curves.lookup(edge.curve_id);
      const auto &pcurve = *curves.lookup(use->pcurve_id);
      for (int end = 0; end < 2; end++) {
        const int row = pcurve.points[end].position[linear_axis] == high;
        const double parameter = pcurve.points[end].position[curve_axis];
        const auto point = evaluate_curve(*row_curves[row], parameter, &r_error);
        if (!point) {
          return std::nullopt;
        }
        const auto &horizontal = *index.edge(rows[row]->edge_id);
        const auto &vertex = *index.vertex(edge.vertex_ids[end]);
        const double tolerance = std::max({face.tolerance,
                                           rows[row]->tolerance,
                                           use->tolerance,
                                           horizontal.tolerance,
                                           edge.tolerance,
                                           vertex.tolerance});
        const double3 difference = point->position - double3(curve.points[end].position);
        if (std::hypot(difference.x, difference.y, difference.z) > tolerance) {
          r_error = "NURBS ruled boundary endpoint disagrees with its support tolerance";
          return std::nullopt;
        }
      }
    }
    Vector<uint8_t> request(surface.points_num);
    for (int row = 0; row < 2; row++) {
      for (int i = 0; i < counts[curve_axis]; i++) {
        request[row * strides[linear_axis] + i * strides[curve_axis]] = selected.contains(
            corners[row * 2]);
      }
    }
    return request;
  }
  return std::nullopt;
}

struct VertexTransformRequests {
  CoefficientRequests curves, surfaces;
  Set<int64_t> edges, faces;
};

/** Interpolate corner displacements on a polynomial bilinear support. Triangles use an affine
 * UV field; rectangular trims use a bilinear field. Neither changes the existing parameter chart.
 * Matched rational edge/p-curve profiles receive the same field along each complete boundary. */
struct BilinearFaceDeformation {
  int64_t face_id;
  int64_t surface_id;
  int vertices_num = 0;
  std::array<int64_t, 4> vertices = {};
  std::array<std::array<detail::BoundedScalar, 4>, 4> weights = {};
};

static std::optional<Map<int64_t, double2>> bilinear_face_parameters(
    const NurbsTopologyFace &face,
    const NurbsSurfaceGeometry &surface,
    const TopologyIndex &index,
    const Map<int64_t, const NurbsCurveGeometry *> &curves,
    const int corners_num,
    std::string *r_error)
{
  auto fail = [&](const char *message) -> std::optional<Map<int64_t, double2>> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!face.loops.is_single() ||
      static_cast<const NurbsTopologyLoop *>(face.loops.first)->is_inner ||
      static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses.count() != corners_num ||
      surface.order_u != 2 || surface.order_v != 2 || surface.points_u != 2 ||
      surface.points_v != 2 || !clamped_direction(2, 2, surface.knots_u) ||
      !clamped_direction(2, 2, surface.knots_v))
  {
    return fail("Partial NURBS corner editing requires a polynomial bilinear support");
  }
  for (int i = 0; i < 4; i++) {
    if (!(surface.points[i].weight > 0) || surface.points[i].weight != surface.points[0].weight) {
      return fail("Partial NURBS corner editing requires equal positive support weights");
    }
  }
  Map<int64_t, double2> parameters;
  for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
    const auto &edge = *index.edge(use.edge_id);
    const auto &curve = *curves.lookup(edge.curve_id);
    const auto &pcurve = *curves.lookup(use.pcurve_id);
    if (!linear_curve(curve, edge.parameter_range[0], edge.parameter_range[1]) ||
        !linear_curve(pcurve, edge.parameter_range[0], edge.parameter_range[1]) ||
        !(curve.points[0].weight > 0) || !(curve.points[1].weight > 0) ||
        !(pcurve.points[0].weight > 0) || !(pcurve.points[1].weight > 0) ||
        !equal_weight_products({curve.points[0].weight, pcurve.points[1].weight, 1},
                               {curve.points[1].weight, pcurve.points[0].weight, 1}))
    {
      return fail("NURBS corner deformation needs matching complete straight boundaries");
    }
    for (int end = 0; end < 2; end++) {
      const int64_t id = edge.vertex_ids[end];
      if (double3(curve.points[end].position) != double3(index.vertex(id)->position)) {
        return fail("NURBS corner deformation needs exact source endpoint associations");
      }
      const double2 uv(pcurve.points[end].position);
      if (uv.x < surface.knots_u[1] || uv.x > surface.knots_u[2] || uv.y < surface.knots_v[1] ||
          uv.y > surface.knots_v[2])
      {
        return fail("NURBS endpoint lies outside its support domain");
      }
      if (const auto *previous = parameters.lookup_ptr(id)) {
        if (*previous != uv) {
          return fail("A NURBS endpoint has conflicting parameter associations");
        }
      }
      else {
        parameters.add_new(id, uv);
      }
    }
  }
  if (parameters.size() != corners_num) {
    return fail("NURBS corner deformation needs distinct source endpoints");
  }
  return parameters;
}

static std::optional<BilinearFaceDeformation> triangle_deformation(
    const NurbsTopologyFace &face,
    const NurbsSurfaceGeometry &surface,
    const TopologyIndex &index,
    const Map<int64_t, const NurbsCurveGeometry *> &curves,
    std::string *r_error)
{
  const auto parameters = bilinear_face_parameters(face, surface, index, curves, 3, r_error);
  if (!parameters) {
    return std::nullopt;
  }
  BilinearFaceDeformation result;
  result.face_id = face.id;
  result.surface_id = surface.id;
  result.vertices_num = 3;
  std::copy(parameters->keys().begin(), parameters->keys().end(), result.vertices.begin());
  std::sort(result.vertices.begin(), result.vertices.begin() + result.vertices_num);
  using detail::bounded_divide;
  using detail::bounded_multiply;
  using detail::bounded_subtract;
  using detail::BoundedScalar;
  const double2 a = parameters->lookup(result.vertices[0]);
  const double2 b = parameters->lookup(result.vertices[1]);
  const double2 c = parameters->lookup(result.vertices[2]);
  std::array<int, 3> natural_corners;
  bool natural = true;
  for (int i = 0; i < 3; i++) {
    const double2 uv = parameters->lookup(result.vertices[i]);
    int slot = 0;
    for (int axis = 0; axis < 2; axis++) {
      const double *knots = axis == 0 ? surface.knots_u : surface.knots_v;
      natural &= uv[axis] == knots[1] || uv[axis] == knots[2];
      slot += uv[axis] == knots[2] ? 1 << axis : 0;
    }
    natural_corners[i] = slot;
  }
  natural &= natural_corners[0] != natural_corners[1] &&
             natural_corners[0] != natural_corners[2] && natural_corners[1] != natural_corners[2];
  if (natural) {
    for (int corner = 0; corner < 4; corner++) {
      const auto found = std::find(natural_corners.begin(), natural_corners.end(), corner);
      for (int i = 0; i < 3; i++) {
        result.weights[corner][i] = {found != natural_corners.end() ?
                                         double(i == found - natural_corners.begin()) :
                                         (natural_corners[i] == (corner ^ 3) ? -1.0 : 1.0)};
      }
    }
    return result;
  }
  const BoundedScalar bu = bounded_subtract({b.x}, {a.x});
  const BoundedScalar bv = bounded_subtract({b.y}, {a.y});
  const BoundedScalar cu = bounded_subtract({c.x}, {a.x});
  const BoundedScalar cv = bounded_subtract({c.y}, {a.y});
  const BoundedScalar determinant = bounded_subtract(bounded_multiply(bu, cv),
                                                     bounded_multiply(bv, cu));
  for (int corner = 0; corner < 4; corner++) {
    const BoundedScalar u = bounded_subtract({surface.knots_u[corner & 1 ? 2 : 1]}, {a.x});
    const BoundedScalar v = bounded_subtract({surface.knots_v[corner & 2 ? 2 : 1]}, {a.y});
    auto &weights = result.weights[corner];
    weights[1] = bounded_divide(bounded_subtract(bounded_multiply(u, cv), bounded_multiply(v, cu)),
                                determinant);
    weights[2] = bounded_divide(bounded_subtract(bounded_multiply(bu, v), bounded_multiply(bv, u)),
                                determinant);
    weights[0] = bounded_subtract(bounded_subtract({1}, weights[1]), weights[2]);
    for (const auto &weight : weights) {
      if (!weight.valid()) {
        if (r_error) {
          *r_error =
              "Triangular NURBS parameterization cannot certify an affine displacement field";
        }
        return std::nullopt;
      }
    }
  }
  return result;
}

/** Interpolate the four movements over a rectangular trim, not the unused support corners.
 * The resulting bilinear displacement restricts to the same endpoint interpolation as each
 * shared straight boundary. Add it to the existing support, retaining its previous interior
 * geometry, weights, knots and p-curves without support reconstruction or fitting. */
static std::optional<BilinearFaceDeformation> rectangular_deformation(
    const NurbsTopologyFace &face,
    const NurbsSurfaceGeometry &surface,
    const TopologyIndex &index,
    const Map<int64_t, const NurbsCurveGeometry *> &curves)
{
  const auto parameters = bilinear_face_parameters(face, surface, index, curves, 4, nullptr);
  if (!parameters) {
    return std::nullopt;
  }
  double2 low(std::numeric_limits<double>::infinity());
  double2 high(-std::numeric_limits<double>::infinity());
  for (const auto uv : parameters->values()) {
    for (int axis = 0; axis < 2; axis++) {
      low[axis] = std::min(low[axis], uv[axis]);
      high[axis] = std::max(high[axis], uv[axis]);
    }
  }
  if (low.x == high.x || low.y == high.y ||
      (low == double2(surface.knots_u[1], surface.knots_v[1]) &&
       high == double2(surface.knots_u[2], surface.knots_v[2])))
  {
    // Complete tensor corners retain their existing rational-support transform route.
    return std::nullopt;
  }
  BilinearFaceDeformation result;
  result.face_id = face.id;
  result.surface_id = surface.id;
  result.vertices_num = 4;
  for (const auto item : parameters->items()) {
    int corner = 0;
    for (int axis = 0; axis < 2; axis++) {
      if (item.value[axis] != low[axis] && item.value[axis] != high[axis]) {
        return std::nullopt;
      }
      corner |= item.value[axis] == high[axis] ? 1 << axis : 0;
    }
    if (result.vertices[corner] != 0) {
      return std::nullopt;
    }
    result.vertices[corner] = item.key;
  }
  for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
    const auto &edge = *index.edge(use.edge_id);
    const double2 a = parameters->lookup(edge.vertex_ids[0]);
    const double2 b = parameters->lookup(edge.vertex_ids[1]);
    if ((a.x == b.x) == (a.y == b.y)) {
      return std::nullopt;
    }
  }
  using namespace detail;
  for (int corner = 0; corner < 4; corner++) {
    BoundedScalar fraction[2];
    for (int axis = 0; axis < 2; axis++) {
      const double *knots = axis == 0 ? surface.knots_u : surface.knots_v;
      const double coordinate = knots[corner & (1 << axis) ? 2 : 1];
      fraction[axis] = bounded_divide(bounded_subtract({coordinate}, {low[axis]}),
                                      bounded_subtract({high[axis]}, {low[axis]}));
    }
    for (int point = 0; point < 4; point++) {
      auto &weight = result.weights[corner][point];
      weight = bounded_multiply(point & 1 ? fraction[0] : bounded_subtract({1}, fraction[0]),
                                point & 2 ? fraction[1] : bounded_subtract({1}, fraction[1]));
      if (!weight.valid()) {
        return std::nullopt;
      }
    }
  }
  return result;
}

/** Shared correspondence proof for translation and affine component edits. */
static std::optional<VertexTransformRequests> vertex_transform_requests(
    const NurbsGeometry &source,
    const TopologyIndex &index,
    const Set<int64_t> &selected,
    const Set<int64_t> &deformed_faces,
    std::string *r_error)
{
  auto fail = [&](const char *message) -> std::optional<VertexTransformRequests> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  VertexTransformRequests result;
  auto &curve_requests = result.curves;
  auto &surface_requests = result.surfaces;
  auto &moved_edges = result.edges;
  auto &moved_faces = result.faces;
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : source.curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    const NurbsCurveGeometry &curve = *curves.lookup(edge.curve_id);
    const bool a = selected.contains(edge.vertex_ids[0]);
    const bool b = selected.contains(edge.vertex_ids[1]);
    Vector<uint8_t> request;
    request.resize(curve.points_num);
    std::fill(request.begin(), request.end(), uint8_t(a && b));
    if (a != b) {
      if (!linear_curve(curve, edge.parameter_range[0], edge.parameter_range[1])) {
        return fail(
            "Transforming part of a curved NURBS edge requires a fit-point deformation rule");
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
        const auto &vertex = *index.vertex(edge.vertex_ids[end]);
        const double3 difference = point->position - double3(vertex.position);
        if (std::hypot(difference.x, difference.y, difference.z) >
            std::max(vertex.tolerance, edge.tolerance))
        {
          return fail("NURBS edge endpoint disagrees with its source vertex tolerance");
        }
      }
    }
    if (!add_request(curve_requests, curve.id, std::move(request))) {
      return fail("Shared NURBS curve owners request conflicting coefficient transforms");
    }
  }
  for (const NurbsTopologyFace &face : source.faces) {
    const NurbsSurfaceGeometry &surface = *surfaces.lookup(face.surface_id);
    bool any = false, all = true;
    for (const NurbsTopologyLoop &loop : face.loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const auto &edge = *index.edge(use.edge_id);
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
    if (deformed_faces.contains(face.id)) {
      continue;
    }
    if (any && !all) {
      std::string row_error;
      auto ruled = ruled_row_request(face, surface, index, curves, selected, row_error);
      if (!row_error.empty()) {
        return fail(row_error.c_str());
      }
      if (ruled) {
        request = std::move(*ruled);
      }
      else {
        if (!face.loops.is_single() ||
            static_cast<const NurbsTopologyLoop *>(face.loops.first)->is_inner ||
            static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses.count() != 4 ||
            !clamped_direction(surface.order_u, surface.points_u, surface.knots_u) ||
            !clamped_direction(surface.order_v, surface.points_v, surface.knots_v))
        {
          return fail("Partial NURBS editing currently requires a complete clamped tensor face");
        }
        const bool linear_u = surface.order_u == 2 && surface.points_u == 2;
        const bool linear_v = surface.order_v == 2 && surface.points_v == 2;
        if (!linear_u && !linear_v) {
          return fail("Partial curved NURBS surface editing requires a deformation rule");
        }
        const double low[2] = {surface.knots_u[surface.order_u - 1],
                               surface.knots_v[surface.order_v - 1]};
        const double high[2] = {surface.knots_u[surface.points_u],
                                surface.knots_v[surface.points_v]};
        int64_t corners[4] = {};
        const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
        for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
          const auto &edge = *index.edge(use.edge_id);
          const auto &pcurve = *curves.lookup(use.pcurve_id);
          if (!linear_curve(pcurve, edge.parameter_range[0], edge.parameter_range[1])) {
            return fail("Partial NURBS editing requires complete linear boundary p-curves");
          }
          int slots[2];
          for (int end = 0; end < 2; end++) {
            int slot = 0;
            for (int axis = 0; axis < 2; axis++) {
              const double coordinate = pcurve.points[end].position[axis];
              if (coordinate != low[axis] && coordinate != high[axis]) {
                return fail("Partial trimmed NURBS face editing requires a deformation rule");
              }
              slot += (coordinate == high[axis]) ? (1 << axis) : 0;
            }
            slots[end] = slot;
            const auto &vertex = *index.vertex(edge.vertex_ids[end]);
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
          return fail("NURBS partial editing needs all four tensor corner associations");
        }
        if ((!linear_u && (selected.contains(corners[0]) != selected.contains(corners[1]) ||
                           selected.contains(corners[2]) != selected.contains(corners[3]))) ||
            (!linear_v && (selected.contains(corners[0]) != selected.contains(corners[2]) ||
                           selected.contains(corners[1]) != selected.contains(corners[3]))))
        {
          return fail(
              "Transform a complete ruled NURBS boundary or define a curved deformation rule");
        }
        for (int v = 0; v < surface.points_v; v++) {
          for (int u = 0; u < surface.points_u; u++) {
            const int slot = (linear_u ? u : 0) + (linear_v ? 2 * v : 0);
            request[v * surface.points_u + u] = selected.contains(corners[slot]);
          }
        }
      }
    }
    if (!add_request(surface_requests, surface.id, std::move(request))) {
      return fail("Shared NURBS support owners request conflicting coefficient transforms");
    }
  }
  return result;
}

std::optional<GeometryTransformResult> translate_vertices(const NurbsGeometry &source,
                                                          const Span<int64_t> vertex_ids,
                                                          const double3 &offset,
                                                          const double max_position_error,
                                                          std::string *r_error)
{
  auto matrix = double4x4::identity();
  matrix.location() = offset;
  if (offset != double3(0) && !vertex_ids.is_empty() &&
      vertex_ids.size() >= source.vertices.count())
  {
    const Set<int64_t> selected(vertex_ids);
    if (selected.size() == source.vertices.count() &&
        std::all_of(source.vertices.begin(), source.vertices.end(), [&](const auto &vertex) {
          return selected.contains(vertex.id);
        }))
    {
      /* Every topology vertex receives the same translation. There are no partial-support
       * requests to reconcile. Preserve unused parent supports left by refinement. */
      return transform_geometry_impl(source, matrix, max_position_error, true, r_error);
    }
  }
  Vector<VertexTransform> transforms;
  transforms.reserve(vertex_ids.size());
  for (const int64_t id : vertex_ids) {
    transforms.append({id, matrix});
  }
  return transform_vertices(source, transforms, max_position_error, r_error);
}

std::optional<GeometryTransformResult> transform_vertices(const NurbsGeometry &source,
                                                          const Span<VertexTransform> transforms,
                                                          const double max_position_error,
                                                          std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<GeometryTransformResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (transforms.is_empty() || !std::isfinite(max_position_error) || max_position_error < 0 ||
      !detail::supported_arithmetic())
  {
    return fail(
        "NURBS affine editing requires transforms, a finite error budget and "
        "round-to-nearest/subnormal arithmetic");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Vector<double4x4> matrices;
  Vector<Set<int64_t>> groups;
  Map<int64_t, int> assignments;
  GeometryTransformResult result;
  for (const VertexTransform &transform : transforms) {
    if (!index->vertex(transform.vertex_id)) {
      return fail("NURBS affine editing references an unavailable vertex");
    }
    const double4x4 &matrix = transform.matrix;
    for (int column = 0; column < 4; column++) {
      for (int row = 0; row < 4; row++) {
        if (!std::isfinite(matrix[column][row])) {
          return fail("NURBS component transform matrix is not finite");
        }
      }
    }
    if (matrix[0][3] != 0 || matrix[1][3] != 0 || matrix[2][3] != 0 || matrix[3][3] != 1) {
      return fail("NURBS component editing requires an affine matrix");
    }
    int group = 0;
    while (group < matrices.size() && matrices[group] != matrix) {
      group++;
    }
    if (group == matrices.size()) {
      matrices.append(matrix);
      groups.append({});
      result.tolerance_scale_bound = std::max(result.tolerance_scale_bound,
                                              linear_norm_bound(matrix));
    }
    if (const int *previous = assignments.lookup_ptr(transform.vertex_id)) {
      if (*previous != group) {
        return fail("One NURBS vertex requests conflicting affine transforms");
      }
    }
    else {
      assignments.add_new(transform.vertex_id, group);
      groups[group].add(transform.vertex_id);
    }
  }
  if (!std::isfinite(result.tolerance_scale_bound)) {
    return fail("NURBS affine tolerance scale is not representable");
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const auto &curve : source.curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const auto &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  Vector<BilinearFaceDeformation> deformations;
  Set<int64_t> deformed_faces;
  Vector<int64_t> rechart_faces;
  for (const auto &face : source.faces) {
    if (!face.loops.is_single()) {
      continue;
    }
    const int corners_num =
        static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses.count();
    if (corners_num != 3 && corners_num != 4) {
      continue;
    }
    int first_group = -2;
    bool different = false;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      for (const int64_t vertex : index->edge(use.edge_id)->vertex_ids) {
        int group = assignments.lookup_default(vertex, -1);
        if (group >= 0 && matrices[group] == double4x4::identity()) {
          group = -1;
        }
        if (first_group == -2) {
          first_group = group;
        }
        different |= first_group != group;
      }
    }
    if (different) {
      const auto plan = corners_num == 3 ?
                            triangle_deformation(
                                face, *surfaces.lookup(face.surface_id), *index, curves, r_error) :
                            rectangular_deformation(
                                face, *surfaces.lookup(face.surface_id), *index, curves);
      if (!plan) {
        if (corners_num == 3) {
          return std::nullopt;
        }
        const auto &surface = *surfaces.lookup(face.surface_id);
        if (const auto parameters = bilinear_face_parameters(
                face, surface, *index, curves, 4, nullptr))
        {
          bool natural = true;
          for (const auto uv : parameters->values()) {
            natural &= (uv.x == surface.knots_u[1] || uv.x == surface.knots_u[2]) &&
                       (uv.y == surface.knots_v[1] || uv.y == surface.knots_v[2]);
          }
          if (!natural) {
            rechart_faces.append(face.id);
          }
        }
        continue;
      }
      deformations.append(*plan);
      deformed_faces.add(face.id);
    }
  }
  if (!rechart_faces.is_empty()) {
    auto charts = reparameterize_planar_quads(source, rechart_faces, max_position_error, r_error);
    if (!charts) {
      return std::nullopt;
    }
    auto transformed = transform_vertices(
        *charts->geometry, transforms, max_position_error, r_error);
    if (!transformed) {
      return std::nullopt;
    }
    transformed->chart_error_bound = detail::add_upper(
        transformed->chart_error_bound,
        detail::multiply_upper(charts->geometry_error_bound, transformed->tolerance_scale_bound));
    transformed->reparameterized_faces += rechart_faces.size();
    if (detail::add_upper(transformed->chart_error_bound,
                          detail::add_upper(transformed->position_error_bound,
                                            transformed->boundary_error_bound)) >
        max_position_error)
    {
      return fail("NURBS quad chart and transform exceed the combined operation error budget");
    }
    return transformed;
  }
  Map<int64_t, Vector<int>> curve_assignments, surface_assignments;
  Set<int64_t> moved_vertices, moved_edges, moved_faces;
  auto merge = [&](Map<int64_t, Vector<int>> &destination,
                   const CoefficientRequests &requests,
                   const int group) {
    for (const auto item : requests.items()) {
      Vector<int> &slots = destination.lookup_or_add(item.key, Vector<int>(item.value.size(), -1));
      for (const int i : item.value.index_range()) {
        if (item.value[i]) {
          if (slots[i] != -1 && slots[i] != group) {
            return false;
          }
          slots[i] = group;
        }
      }
    }
    return true;
  };
  for (const int group : matrices.index_range()) {
    if (matrices[group] == double4x4::identity()) {
      continue;
    }
    const auto requests = vertex_transform_requests(
        source, *index, groups[group], deformed_faces, r_error);
    if (!requests) {
      return std::nullopt;
    }
    if (!merge(curve_assignments, requests->curves, group) ||
        !merge(surface_assignments, requests->surfaces, group))
    {
      return fail("Shared NURBS coefficients request incompatible affine transforms");
    }
    for (const int64_t id : groups[group]) {
      moved_vertices.add(id);
    }
    for (const int64_t id : requests->edges) {
      moved_edges.add(id);
    }
    for (const int64_t id : requests->faces) {
      moved_faces.add(id);
    }
  }
  result.geometry.reset(geometry_copy(&source));
  auto apply = [&](NurbsControlPoint *points, const Vector<int> &slots) {
    if (std::any_of(slots.begin(), slots.end(), [](const int slot) { return slot >= 0; })) {
      for (const int i : slots.index_range()) {
        if (!(points[i].weight > 0)) {
          return false;
        }
      }
    }
    for (const int i : slots.index_range()) {
      if (slots[i] >= 0 &&
          !transform_position(points[i].position, matrices[slots[i]], result.position_error_bound))
      {
        return false;
      }
    }
    return true;
  };
  for (NurbsCurveGeometry &curve : result.geometry->curves) {
    if (const auto *slots = curve_assignments.lookup_ptr(curve.id)) {
      if (!apply(curve.points, *slots)) {
        return fail("NURBS affine curve edit needs positive weights and finite arithmetic");
      }
    }
  }
  for (NurbsSurfaceGeometry &surface : result.geometry->surfaces) {
    if (const auto *slots = surface_assignments.lookup_ptr(surface.id)) {
      if (!apply(surface.points, *slots)) {
        return fail("NURBS affine support edit needs positive weights and finite arithmetic");
      }
    }
  }
  Map<int64_t, NurbsTopologyVertex *> vertices;
  Map<int64_t, double> vertex_errors;
  for (NurbsTopologyVertex &vertex : result.geometry->vertices) {
    vertices.add_new(vertex.id, &vertex);
    if (moved_vertices.contains(vertex.id)) {
      double error = 0;
      if (!transform_position(vertex.position, matrices[assignments.lookup(vertex.id)], error)) {
        return fail("NURBS affine vertex edit overflowed");
      }
      vertex_errors.add_new(vertex.id, error);
      result.position_error_bound = std::max(result.position_error_bound, error);
    }
  }
  Map<int64_t, std::array<double3, 4>> deformed_positions;
  for (const auto &deformation : deformations) {
    using detail::BoundedScalar;
    const auto &surface = *surfaces.lookup(deformation.surface_id);
    std::array<std::array<BoundedScalar, 3>, 4> displacements;
    for (int i = 0; i < deformation.vertices_num; i++) {
      const int64_t id = deformation.vertices[i];
      for (int axis = 0; axis < 3; axis++) {
        auto &delta = displacements[i][axis];
        delta = detail::bounded_subtract(
            {vertices.lookup(id)->position[axis], vertex_errors.lookup_default(id, 0)},
            {index->vertex(id)->position[axis]});
      }
    }
    std::array<double3, 4> positions;
    for (int corner = 0; corner < 4; corner++) {
      double point_error = 0;
      for (int axis = 0; axis < 3; axis++) {
        BoundedScalar change;
        for (int i = 0; i < deformation.vertices_num; i++) {
          change = detail::bounded_add(
              change,
              detail::bounded_multiply(deformation.weights[corner][i], displacements[i][axis]));
        }
        if (change.value == 0 && change.error == 0) {
          positions[corner][axis] = surface.points[corner].position[axis];
          continue;
        }
        const auto value = detail::bounded_add({surface.points[corner].position[axis]}, change);
        if (!value.valid()) {
          return fail("NURBS corner deformation overflowed its arithmetic bound");
        }
        positions[corner][axis] = value.value;
        point_error = detail::add_upper(point_error, value.error);
      }
      result.position_error_bound = std::max(result.position_error_bound, point_error);
    }
    if (const auto *previous = deformed_positions.lookup_ptr(deformation.surface_id)) {
      if (*previous != positions) {
        return fail("Shared NURBS supports request incompatible corner deformation fields");
      }
    }
    else {
      deformed_positions.add_new(deformation.surface_id, positions);
    }
  }
  for (auto &surface : result.geometry->surfaces) {
    const auto *positions = deformed_positions.lookup_ptr(surface.id);
    if (!positions) {
      continue;
    }
    for (int corner = 0; corner < 4; corner++) {
      if (surface_assignments.contains(surface.id) &&
          double3(surface.points[corner].position) != (*positions)[corner])
      {
        return fail("Shared NURBS support owners request incompatible corner/tensor deformations");
      }
      std::copy_n(&(*positions)[corner].x, 3, surface.points[corner].position);
    }
  }
  Map<int64_t, const NurbsCurveGeometry *> output_curves;
  Map<int64_t, const NurbsSurfaceGeometry *> output_surfaces;
  for (const auto &curve : result.geometry->curves) {
    output_curves.add_new(curve.id, &curve);
  }
  for (const auto &surface : result.geometry->surfaces) {
    output_surfaces.add_new(surface.id, &surface);
  }
  Map<int64_t, double> boundary_bounds;
  for (const auto &deformation : deformations) {
    if (deformation.vertices_num != 4) {
      continue;
    }
    const auto &face = *index->face(deformation.face_id);
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      const auto &pcurve = *curves.lookup(use.pcurve_id);
      const double tolerance = std::max({face.tolerance, edge.tolerance, use.tolerance});
      const auto before = linear_curve_support_deviation_bound(
          *curves.lookup(edge.curve_id), pcurve, *surfaces.lookup(face.surface_id));
      if (!before || *before > tolerance) {
        return fail("Trimmed NURBS deformation cannot certify the input boundary tolerance");
      }
      const auto after = linear_curve_support_deviation_bound(
          *output_curves.lookup(edge.curve_id), pcurve, *output_surfaces.lookup(face.surface_id));
      if (!after) {
        return fail("Trimmed NURBS deformation cannot certify its new boundary correspondence");
      }
      boundary_bounds.add_new(use.id, *after);
      const double scaled_tolerance = detail::multiply_upper(tolerance,
                                                             result.tolerance_scale_bound);
      if (*after > scaled_tolerance) {
        const auto excess = detail::bounded_subtract({*after}, {scaled_tolerance});
        result.boundary_error_bound = std::max(result.boundary_error_bound,
                                               detail::add_upper(excess.value, excess.error));
      }
    }
  }
  if (result.position_error_bound > max_position_error) {
    return fail("NURBS affine edit roundoff exceeds the requested error budget");
  }
  if (result.boundary_error_bound > max_position_error) {
    return fail("Trimmed NURBS boundary correspondence exceeds the requested error budget");
  }
  const double extra = detail::multiply_upper(2, result.position_error_bound);
  auto expand = [&](const double tolerance) {
    return detail::add_upper(detail::multiply_upper(tolerance, result.tolerance_scale_bound),
                             extra);
  };
  for (NurbsTopologyVertex &vertex : result.geometry->vertices) {
    if (moved_vertices.contains(vertex.id)) {
      vertex.tolerance = expand(vertex.tolerance);
    }
  }
  for (NurbsTopologyEdge &edge : result.geometry->edges) {
    if (moved_edges.contains(edge.id)) {
      edge.tolerance = expand(edge.tolerance);
    }
  }
  for (NurbsTopologyFace &face : result.geometry->faces) {
    if (moved_faces.contains(face.id)) {
      face.tolerance = expand(face.tolerance);
    }
    for (NurbsTopologyLoop &loop : face.loops) {
      for (NurbsTopologyEdgeUse &use : loop.edge_uses) {
        if (moved_faces.contains(face.id) || moved_edges.contains(use.edge_id)) {
          use.tolerance = expand(use.tolerance);
        }
        if (const auto *bound = boundary_bounds.lookup_ptr(use.id)) {
          use.tolerance = std::max(use.tolerance, *bound);
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
