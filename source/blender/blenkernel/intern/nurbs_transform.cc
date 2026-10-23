/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BLI_listbase.h"

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

}  // namespace blender::bke::nurbs
