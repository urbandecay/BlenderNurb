/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"

#include "BKE_nurbs_fit.hh"

#include "eigen_capi.h"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {

void CurveDeleter::operator()(NurbsCurveGeometry *curve) const
{
  if (curve) {
    MEM_SAFE_DELETE(curve->points);
    MEM_SAFE_DELETE(curve->knots);
    MEM_delete(curve);
  }
}

static OwnedCurve copy_curve(const NurbsCurveGeometry &source)
{
  OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__, source));
  curve->next = curve->prev = nullptr;
  curve->points = nullptr;
  curve->knots = nullptr;
  curve->points = MEM_new_array<NurbsControlPoint>(source.points_num, __func__);
  curve->knots = MEM_new_array_uninitialized<double>(source.knots_num, __func__);
  std::copy_n(source.points, source.points_num, curve->points);
  std::copy_n(source.knots, source.knots_num, curve->knots);
  return curve;
}

static bool options_valid(const CurveFitOptions &options)
{
  return std::isfinite(options.max_point_error) && options.max_point_error >= 0 &&
         std::isfinite(options.min_singular_value_ratio) && options.min_singular_value_ratio > 0 &&
         options.min_singular_value_ratio <= 1;
}

static bool point_valid(const CurveFitPoint &point)
{
  return std::isfinite(point.parameter) && std::isfinite(point.position.x) &&
         std::isfinite(point.position.y) && std::isfinite(point.position.z);
}

static double distance(const double3 &a, const double3 &b)
{
  const double3 difference = a - b;
  return std::hypot(difference.x, difference.y, difference.z);
}

std::optional<CurveFitResult> fit_freeform_curve(const NurbsCurveGeometry &source,
                                                 const Span<CurveFitPoint> fit_points,
                                                 const IndexRange editable_coefficients,
                                                 const CurveFitOptions &options,
                                                 std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const std::string &message) -> std::optional<CurveFitResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!detail::supported_arithmetic() || !options_valid(options)) {
    return fail(
        "NURBS fitting needs a finite residual budget, a positive conditioning threshold "
        "and round-to-nearest/subnormal arithmetic");
  }
  if (!curve_validate_structure(source, r_error)) {
    return std::nullopt;
  }
  if (fit_points.is_empty() || fit_points.size() > 512 || editable_coefficients.size() > 256 ||
      editable_coefficients.start() < 0 ||
      editable_coefficients.one_after_last() > source.points_num)
  {
    return fail(
        "NURBS fitting needs 1-512 constraints and an editable range of at most "
        "256 source coefficients");
  }
  for (int i = 0; i < source.points_num; i++) {
    if (!(source.points[i].weight > 0)) {
      return fail("Free-form fitting currently requires positive rational weights");
    }
  }
  Vector<CurveFitPoint> points(fit_points);
  for (const CurveFitPoint &point : points) {
    if (!point_valid(point) || (source.dimension == 2 && point.position.z != 0)) {
      return fail("NURBS fit targets must be finite and respect the curve's dimension");
    }
  }
  std::sort(points.begin(), points.end(), [](const auto &a, const auto &b) {
    return a.parameter < b.parameter;
  });
  Vector<CurveFitPoint> unique;
  for (const auto &point : points) {
    if (!unique.is_empty() && point.parameter == unique.last().parameter) {
      if (point.position != unique.last().position) {
        return fail("One NURBS fit parameter has conflicting target positions");
      }
    }
    else {
      unique.append(point);
    }
  }
  const int columns = editable_coefficients.size();
  Array<double> matrix(unique.size() * columns, 0.0);
  Array<double> rhs(unique.size() * 3, 0.0);
  int rows = 0;
  bool requested_change = false;
  for (const CurveFitPoint &point : unique) {
    const auto basis = evaluate_curve_basis(source, point.parameter, r_error);
    const auto evaluated = evaluate_curve(source, point.parameter, r_error);
    if (!basis || !evaluated) {
      return std::nullopt;
    }
    const double3 delta = point.position - evaluated->position;
    requested_change |= point.position != evaluated->position;
    bool has_coefficient = false;
    for (const int i : basis->values.index_range()) {
      const int coefficient = basis->first_coefficient + i;
      if (editable_coefficients.contains(coefficient) && basis->values[i] != 0) {
        matrix[rows * columns + coefficient - editable_coefficients.start()] = basis->values[i];
        has_coefficient = true;
      }
    }
    if (!has_coefficient) {
      if (distance(point.position, evaluated->position) > options.max_point_error) {
        return fail("A NURBS fit target lies outside the editable support and cannot be reached");
      }
      continue;
    }
    for (int axis = 0; axis < 3; axis++) {
      if (!std::isfinite(delta[axis])) {
        return fail("NURBS fit target displacement exceeds double precision range");
      }
      rhs[rows * 3 + axis] = delta[axis];
    }
    rows++;
  }

  CurveFitResult result;
  result.curve = copy_curve(source);
  bool changed = false;
  if (rows > 0) {
    Array<double> deltas(columns * 3);
    if (!EIG_svd_solve_least_squares_double(rows,
                                            columns,
                                            3,
                                            matrix.data(),
                                            rhs.data(),
                                            options.min_singular_value_ratio,
                                            deltas.data(),
                                            &result.rank,
                                            &result.relative_smallest_singular_value))
    {
      return fail(
          fmt::format("NURBS fit decomposition failed or is ill-conditioned: rank {}, "
                      "singular ratio {:.17g}, required {:.17g}",
                      result.rank,
                      result.relative_smallest_singular_value,
                      options.min_singular_value_ratio));
    }
    for (const int64_t i : editable_coefficients.index_range()) {
      const int64_t coefficient = editable_coefficients[i];
      auto &point = result.curve->points[coefficient];
      const double3 original(point.position);
      double addition_error = 0;
      for (int axis = 0; axis < source.dimension; axis++) {
        const double delta = deltas[i * 3 + axis];
        if (delta == 0) {
          continue;
        }
        double position, residual;
        if (!detail::sum_with_error(point.position[axis], delta, position, residual)) {
          return fail("NURBS fitted coefficient addition overflowed");
        }
        point.position[axis] = position;
        addition_error = detail::add_upper(addition_error, std::abs(residual));
      }
      const double displacement = distance(original, double3(point.position));
      if (!std::isfinite(displacement)) {
        return fail("NURBS fitted coefficient displacement is not representable");
      }
      result.max_coefficient_displacement = std::max(result.max_coefficient_displacement,
                                                     displacement);
      result.coefficient_addition_error_bound = std::max(result.coefficient_addition_error_bound,
                                                         addition_error);
      changed |= original != double3(point.position);
    }
  }
  if (!curve_validate_structure(*result.curve, r_error)) {
    return std::nullopt;
  }
  for (const CurveFitPoint &point : unique) {
    const auto evaluated = evaluate_curve(*result.curve, point.parameter, r_error);
    if (!evaluated) {
      return std::nullopt;
    }
    const double residual = distance(evaluated->position, point.position);
    if (!std::isfinite(residual)) {
      return fail("NURBS fit residual is not finite");
    }
    result.max_point_error = std::max(result.max_point_error, residual);
  }
  if (result.max_point_error > options.max_point_error) {
    return fail(
        fmt::format("NURBS fit point residual {:.17g} exceeds allowed {:.17g} source units",
                    result.max_point_error,
                    options.max_point_error));
  }
  if (requested_change && !changed) {
    return fail("Requested fit-point edit did not change source coefficient positions");
  }
  return result;
}

std::optional<CurveFitResult> interpolate_freeform_curve(const Span<CurveFitPoint> fit_points,
                                                         const int degree,
                                                         const CurveFitOptions &options,
                                                         std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<CurveFitResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!detail::supported_arithmetic() || !options_valid(options) || fit_points.size() < 2 ||
      fit_points.size() > 256 || degree < 1 || degree >= fit_points.size())
  {
    return fail("NURBS interpolation needs 2-256 ordered points, a valid degree and fit options");
  }
  for (const int i : fit_points.index_range()) {
    if (!point_valid(fit_points[i]) ||
        (i && fit_points[i - 1].parameter >= fit_points[i].parameter))
    {
      return fail("NURBS interpolation parameters must be finite and strictly increasing");
    }
  }
  OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
  curve->order = degree + 1;
  curve->points_num = fit_points.size();
  curve->knots_num = curve->points_num + curve->order;
  curve->points = MEM_new_array<NurbsControlPoint>(curve->points_num, __func__);
  curve->knots = MEM_new_array<double>(curve->knots_num, __func__);
  for (int i = 0; i < curve->points_num; i++) {
    std::copy_n(&fit_points[0].position.x, 3, curve->points[i].position);
  }
  std::fill_n(curve->knots, curve->order, fit_points.first().parameter);
  std::fill_n(curve->knots + curve->points_num, curve->order, fit_points.last().parameter);
  for (int j = 1; j < curve->points_num - degree; j++) {
    double mean = fit_points[j].parameter;
    for (int i = 1; i < degree; i++) {
      mean = std::lerp(mean, fit_points[j + i].parameter, 1.0 / (i + 1));
    }
    curve->knots[j + degree] = mean;
  }
  return fit_freeform_curve(*curve, fit_points, IndexRange(curve->points_num), options, r_error);
}

}  // namespace blender::bke::nurbs
