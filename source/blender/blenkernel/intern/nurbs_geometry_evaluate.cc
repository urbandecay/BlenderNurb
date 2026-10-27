/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <numeric>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"

#include "BKE_nurbs_evaluate.hh"
#include "BKE_nurbs_geometry.hh"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {

static std::nullopt_t evaluation_error(std::string *r_error, const char *message)
{
  if (r_error != nullptr) {
    *r_error = message;
  }
  return std::nullopt;
}

struct BasisWeights {
  int start;
  Array<double, 12> values;
};

/** Cox-de Boor recurrence on an already selected non-empty span. */
static std::optional<BasisWeights> calculate_basis_on_span(const double *knots,
                                                           const int span,
                                                           const int order,
                                                           const double parameter,
                                                           std::string *r_error)
{
  const int degree = order - 1;
  BasisWeights basis{span - degree, Array<double, 12>(order)};
  Array<double, 12> left(order), right(order);
  basis.values[0] = 1.0;
  for (int j = 1; j < order; j++) {
    left[j] = parameter - knots[span + 1 - j];
    right[j] = knots[span + j] - parameter;
    double saved = 0.0;
    for (int r = 0; r < j; r++) {
      const double interval = right[r + 1] + left[j - r];
      if (!std::isfinite(interval) || interval <= 0.0) {
        return evaluation_error(r_error,
                                "NURBS knot interval cannot be evaluated in double precision");
      }
      const double previous = basis.values[r];
      basis.values[r] = saved + (right[r + 1] / interval) * previous;
      saved = (left[j - r] / interval) * previous;
    }
    basis.values[j] = saved;
  }
  return basis;
}

static std::optional<BasisWeights> calculate_basis(const double *knots,
                                                   const int points_num,
                                                   const int order,
                                                   const double parameter,
                                                   std::string *r_error,
                                                   const KnotSide side = KnotSide::Right)
{
  const int degree = order - 1;
  if (!std::isfinite(parameter) || parameter < knots[degree] || parameter > knots[points_num]) {
    return evaluation_error(r_error, "NURBS parameter is outside the source domain");
  }
  const bool from_left = parameter == knots[points_num] ||
                         (side == KnotSide::Left && parameter > knots[degree]);
  const double *span_end = from_left ?
                               std::lower_bound(
                                   knots + degree, knots + points_num + 1, parameter) :
                               std::upper_bound(knots + degree, knots + points_num + 1, parameter);
  return calculate_basis_on_span(knots, int(span_end - knots) - 1, order, parameter, r_error);
}

static std::optional<Array<double, 12>> derivative_basis(const double *knots,
                                                         const int order,
                                                         const double parameter,
                                                         const BasisWeights &basis,
                                                         std::string *r_error)
{
  const int degree = order - 1;
  const auto lower = calculate_basis_on_span(
      knots, basis.start + degree, order - 1, parameter, r_error);
  if (!lower) {
    return std::nullopt;
  }
  auto lower_value = [&](const int index) {
    return index < lower->start || index >= lower->start + lower->values.size() ?
               0.0 :
               lower->values[index - lower->start];
  };
  Array<double, 12> result(order, 0.0);
  for (int i = 0; i < order; i++) {
    const int index = basis.start + i;
    const double left = knots[index + degree] - knots[index];
    const double right = knots[index + degree + 1] - knots[index + 1];
    if (left != 0.0 && lower_value(index) != 0.0) {
      result[i] += (degree / left) * lower_value(index);
    }
    if (right != 0.0 && lower_value(index + 1) != 0.0) {
      result[i] -= (degree / right) * lower_value(index + 1);
    }
    if (!std::isfinite(result[i])) {
      return evaluation_error(r_error, "NURBS derivative basis exceeds double precision range");
    }
  }
  return result;
}

struct WeightedPoint {
  const NurbsControlPoint *point;
  double basis;
};

struct CompensatedSum {
  double value = 0.0;
  double correction = 0.0;

  void add(const double term)
  {
    const double corrected = term - correction;
    const double next = value + corrected;
    correction = (next - value) - corrected;
    value = next;
  }
};

struct WeightedEvaluation {
  PointEvaluation point;
  double3 origin;
  double3 relative_position;
  double denominator;
  double weight_scale;
};

static std::optional<WeightedEvaluation> evaluate_weighted_points(const Span<WeightedPoint> points,
                                                                  std::string *r_error)
{
  double weight_scale = 0.0;
  double3 minimum(0.0), maximum(0.0);
  for (const WeightedPoint &term : points) {
    if (term.basis == 0.0 || term.point->weight == 0.0) {
      continue;
    }
    if (weight_scale == 0.0) {
      minimum = maximum = double3(term.point->position);
    }
    for (int axis = 0; axis < 3; axis++) {
      minimum[axis] = std::min(minimum[axis], term.point->position[axis]);
      maximum[axis] = std::max(maximum[axis], term.point->position[axis]);
    }
    weight_scale = std::max(weight_scale, std::abs(term.point->weight));
  }
  if (weight_scale == 0.0) {
    return evaluation_error(r_error, "NURBS rational denominator is zero");
  }

  /* Recenter before summation to retain small features far from the origin. Midpoint avoids
   * overflowing the subtraction when finite coefficients have opposite signs. Scaling all
   * weights equally preserves the rational source and avoids needless homogeneous overflow. */
  double3 origin;
  for (int axis = 0; axis < 3; axis++) {
    origin[axis] = std::midpoint(minimum[axis], maximum[axis]);
  }
  CompensatedSum denominator, absolute_weight;
  CompensatedSum numerator[3];
  for (const WeightedPoint &term : points) {
    if (term.basis == 0.0 || term.point->weight == 0.0) {
      continue;
    }
    const double weight = term.basis * (term.point->weight / weight_scale);
    denominator.add(weight);
    absolute_weight.add(std::abs(weight));
    for (int axis = 0; axis < 3; axis++) {
      numerator[axis].add(weight * (term.point->position[axis] - origin[axis]));
    }
  }
  if (denominator.value == 0.0) {
    return evaluation_error(r_error, "NURBS rational denominator is zero");
  }
  WeightedEvaluation result;
  result.origin = origin;
  result.denominator = denominator.value;
  result.weight_scale = weight_scale;
  result.point.weight_condition = absolute_weight.value / std::abs(denominator.value);
  if (!std::isfinite(result.point.weight_condition)) {
    return evaluation_error(r_error,
                            "NURBS denominator conditioning exceeds double precision range");
  }
  for (int axis = 0; axis < 3; axis++) {
    result.relative_position[axis] = numerator[axis].value / denominator.value;
    result.point.position[axis] = origin[axis] + result.relative_position[axis];
    if (!std::isfinite(result.point.position[axis])) {
      return evaluation_error(r_error, "NURBS position cannot be evaluated in double precision");
    }
  }
  return result;
}

std::optional<CurveBasis> evaluate_curve_basis(const NurbsCurveGeometry &curve,
                                               const double parameter,
                                               std::string *r_error)
{
  if (!detail::supported_arithmetic()) {
    return evaluation_error(r_error,
                            "NURBS basis evaluation needs round-to-nearest/subnormal arithmetic");
  }
  if (!curve_validate_structure(curve, r_error)) {
    return std::nullopt;
  }
  const auto basis = calculate_basis(
      curve.knots, curve.points_num, curve.order, parameter, r_error);
  if (!basis) {
    return std::nullopt;
  }
  double scale = 0;
  for (int i = 0; i < curve.order; i++) {
    if (basis->values[i] != 0) {
      scale = std::max(scale, std::abs(curve.points[basis->start + i].weight));
    }
  }
  if (scale == 0) {
    return evaluation_error(r_error, "NURBS rational denominator is zero");
  }
  CurveBasis result;
  result.first_coefficient = basis->start;
  CompensatedSum denominator, absolute;
  for (int i = 0; i < curve.order; i++) {
    const double value = basis->values[i] * (curve.points[basis->start + i].weight / scale);
    result.values.append(value);
    denominator.add(value);
    absolute.add(std::abs(value));
  }
  if (denominator.value == 0) {
    return evaluation_error(r_error, "NURBS rational denominator is zero");
  }
  result.weight_condition = absolute.value / std::abs(denominator.value);
  if (!std::isfinite(result.weight_condition)) {
    return evaluation_error(r_error, "NURBS rational basis conditioning is not finite");
  }
  for (double &value : result.values) {
    value /= denominator.value;
    if (!std::isfinite(value)) {
      return evaluation_error(r_error, "NURBS rational basis exceeds double precision range");
    }
  }
  return result;
}

std::optional<PointEvaluation> evaluate_curve(const NurbsCurveGeometry &curve,
                                              const double parameter,
                                              std::string *r_error)
{
  if (!detail::supported_arithmetic()) {
    return evaluation_error(
        r_error, "NURBS evaluation requires round-to-nearest arithmetic and subnormal support");
  }
  if (!curve_validate_structure(curve, r_error)) {
    return std::nullopt;
  }
  const auto basis = calculate_basis(
      curve.knots, curve.points_num, curve.order, parameter, r_error);
  if (!basis) {
    return std::nullopt;
  }
  Vector<WeightedPoint, 12> points;
  for (int i = 0; i < curve.order; i++) {
    points.append({&curve.points[basis->start + i], basis->values[i]});
  }
  const auto evaluated = evaluate_weighted_points(points, r_error);
  if (!evaluated) {
    return std::nullopt;
  }
  PointEvaluation result = evaluated->point;
  if (curve.dimension == 2 && curve.order == 2) {
    const NurbsControlPoint &a = curve.points[basis->start];
    const NurbsControlPoint &b = curve.points[basis->start + 1];
    if (a.weight != 0.0 && a.weight == b.weight) {
      const double start = curve.knots[basis->start + 1];
      const double end = curve.knots[basis->start + 2];
      for (int axis = 0; axis < curve.dimension; axis++) {
        if (a.position[axis] == start && b.position[axis] == end) {
          /* A degree-one span with equal weights and these coefficients is exactly t.
           * Preserve that identity rather than introducing a second interpolation roundoff
           * when a p-curve parameter is subsequently used to evaluate its support. */
          result.position[axis] = parameter;
        }
      }
    }
  }
  return result;
}

struct SurfaceBasisEvaluation {
  BasisWeights u;
  BasisWeights v;
  WeightedEvaluation value;
};

static std::optional<WeightedEvaluation> evaluate_surface_point_with_basis(
    const NurbsSurfaceGeometry &surface,
    const BasisWeights &basis_u,
    const BasisWeights &basis_v,
    std::string *r_error)
{
  Vector<WeightedPoint, 64> points;
  for (int v = 0; v < surface.order_v; v++) {
    for (int u = 0; u < surface.order_u; u++) {
      const int index = (basis_v.start + v) * surface.points_u + basis_u.start + u;
      points.append({&surface.points[index], basis_u.values[u] * basis_v.values[v]});
    }
  }
  return evaluate_weighted_points(points, r_error);
}

static std::optional<SurfaceBasisEvaluation> evaluate_surface_basis(
    const NurbsSurfaceGeometry &surface,
    const double2 parameter,
    const SurfaceLimits limits,
    std::string *r_error)
{
  if (!detail::supported_arithmetic()) {
    return evaluation_error(
        r_error, "NURBS evaluation requires round-to-nearest arithmetic and subnormal support");
  }
  if ((limits.u != KnotSide::Right && limits.u != KnotSide::Left) ||
      (limits.v != KnotSide::Right && limits.v != KnotSide::Left))
  {
    return evaluation_error(r_error, "Invalid NURBS surface knot-side request");
  }
  if (!surface_validate_structure(surface, r_error)) {
    return std::nullopt;
  }
  const auto basis_u = calculate_basis(
      surface.knots_u, surface.points_u, surface.order_u, parameter.x, r_error, limits.u);
  if (!basis_u) {
    return std::nullopt;
  }
  const auto basis_v = calculate_basis(
      surface.knots_v, surface.points_v, surface.order_v, parameter.y, r_error, limits.v);
  if (!basis_v) {
    return std::nullopt;
  }
  const auto evaluated = evaluate_surface_point_with_basis(surface, *basis_u, *basis_v, r_error);
  if (!evaluated) {
    return std::nullopt;
  }
  return SurfaceBasisEvaluation{*basis_u, *basis_v, *evaluated};
}

std::optional<PointEvaluation> evaluate_surface(const NurbsSurfaceGeometry &surface,
                                                const double2 parameter,
                                                std::string *r_error)
{
  const auto evaluated = evaluate_surface_basis(surface, parameter, {}, r_error);
  return evaluated ? std::optional(evaluated->value.point) : std::nullopt;
}

static std::optional<SurfaceDifferential> evaluate_surface_derivatives(
    const NurbsSurfaceGeometry &surface,
    const SurfaceBasisEvaluation &evaluated,
    const Span<double> derivative_u,
    const Span<double> derivative_v,
    std::string *r_error)
{
  CompensatedSum numerator_u[3], numerator_v[3];
  for (int v = 0; v < surface.order_v; v++) {
    for (int u = 0; u < surface.order_u; u++) {
      const NurbsControlPoint &point =
          surface.points[(evaluated.v.start + v) * surface.points_u + evaluated.u.start + u];
      const double basis_u = derivative_u[u] * evaluated.v.values[v];
      const double basis_v = evaluated.u.values[u] * derivative_v[v];
      if ((basis_u == 0 && basis_v == 0) || point.weight == 0) {
        continue;
      }
      const double weight = point.weight / evaluated.value.weight_scale;
      const double du = weight * basis_u;
      const double dv = weight * basis_v;
      /* Quotient rule in the recentered frame. Do not subtract the rounded absolute position
       * from large coefficients: that would discard small source features in the derivatives. */
      for (const int axis : IndexRange(3)) {
        const double delta = (point.position[axis] - evaluated.value.origin[axis]) -
                             evaluated.value.relative_position[axis];
        numerator_u[axis].add(du * delta);
        numerator_v[axis].add(dv * delta);
      }
    }
  }
  SurfaceDifferential result;
  result.point = evaluated.value.point;
  for (const int axis : IndexRange(3)) {
    result.tangent_u[axis] = numerator_u[axis].value / evaluated.value.denominator;
    result.tangent_v[axis] = numerator_v[axis].value / evaluated.value.denominator;
    if (!std::isfinite(result.tangent_u[axis]) || !std::isfinite(result.tangent_v[axis])) {
      return evaluation_error(r_error, "NURBS surface derivative exceeds double precision range");
    }
  }
  const double scale_u = std::max(
      {std::abs(result.tangent_u.x), std::abs(result.tangent_u.y), std::abs(result.tangent_u.z)});
  const double scale_v = std::max(
      {std::abs(result.tangent_v.x), std::abs(result.tangent_v.y), std::abs(result.tangent_v.z)});
  if (scale_u != 0 && scale_v != 0) {
    /* Scale before crossing to avoid unnecessary overflow/underflow in model units. */
    const double3 cross = math::cross(result.tangent_u / scale_u, result.tangent_v / scale_v);
    const double length = std::hypot(cross.x, cross.y, cross.z);
    if (length != 0) {
      result.normal = cross / length;
    }
  }
  return result;
}

std::optional<SurfaceDifferential> evaluate_surface_differential(
    const NurbsSurfaceGeometry &surface,
    const double2 parameter,
    const SurfaceLimits limits,
    std::string *r_error)
{
  const auto evaluated = evaluate_surface_basis(surface, parameter, limits, r_error);
  if (!evaluated) {
    return std::nullopt;
  }
  const auto derivative_u = derivative_basis(
      surface.knots_u, surface.order_u, parameter.x, evaluated->u, r_error);
  const auto derivative_v = derivative_basis(
      surface.knots_v, surface.order_v, parameter.y, evaluated->v, r_error);
  if (!derivative_u || !derivative_v) {
    return std::nullopt;
  }
  return evaluate_surface_derivatives(surface, *evaluated, *derivative_u, *derivative_v, r_error);
}

std::optional<Vector<SurfaceDifferential>> evaluate_surface_differentials(
    const NurbsSurfaceGeometry &surface,
    const Span<SurfaceDifferentialSample> samples,
    std::string *r_error)
{
  if (!detail::supported_arithmetic()) {
    return evaluation_error(
        r_error, "NURBS evaluation requires round-to-nearest arithmetic and subnormal support");
  }
  if (!surface_validate_structure(surface, r_error)) {
    return std::nullopt;
  }
  struct AxisBasis {
    BasisWeights basis;
    Array<double, 12> derivative;
  };
  Map<std::pair<double, int>, AxisBasis> cache[2];
  auto basis_at =
      [&](const int axis, const double parameter, const KnotSide side) -> const AxisBasis * {
    const std::pair<double, int> key{parameter, int(side)};
    if (const auto *basis = cache[axis].lookup_ptr(key)) {
      return basis;
    }
    const double *knots = axis ? surface.knots_v : surface.knots_u;
    const int count = axis ? surface.points_v : surface.points_u;
    const int order = axis ? surface.order_v : surface.order_u;
    auto basis = calculate_basis(knots, count, order, parameter, r_error, side);
    if (!basis) {
      return nullptr;
    }
    auto derivative = derivative_basis(knots, order, parameter, *basis, r_error);
    if (!derivative) {
      return nullptr;
    }
    cache[axis].add_new(key, AxisBasis{std::move(*basis), std::move(*derivative)});
    return cache[axis].lookup_ptr(key);
  };
  Vector<SurfaceDifferential> result;
  result.reserve(samples.size());
  for (const auto &sample : samples) {
    if ((sample.limits.u != KnotSide::Right && sample.limits.u != KnotSide::Left) ||
        (sample.limits.v != KnotSide::Right && sample.limits.v != KnotSide::Left))
    {
      return evaluation_error(r_error, "Invalid NURBS surface knot-side request");
    }
    const auto *u = basis_at(0, sample.parameter.x, sample.limits.u);
    const auto *v = basis_at(1, sample.parameter.y, sample.limits.v);
    if (!u || !v) {
      return std::nullopt;
    }
    const auto point = evaluate_surface_point_with_basis(surface, u->basis, v->basis, r_error);
    if (!point) {
      return std::nullopt;
    }
    const auto differential = evaluate_surface_derivatives(
        surface, {u->basis, v->basis, *point}, u->derivative, v->derivative, r_error);
    if (!differential) {
      return std::nullopt;
    }
    result.append(*differential);
  }
  return result;
}

std::optional<double3> evaluate_surface_pole_normal(const NurbsSurfaceGeometry &surface,
                                                    const double2 parameter,
                                                    const SurfaceLimits limits)
{
  if (!detail::supported_arithmetic() || !surface_validate_structure(surface, nullptr)) {
    return std::nullopt;
  }
  for (const int fixed : IndexRange(2)) {
    const int varying = 1 - fixed;
    const double *knots = fixed == 0 ? surface.knots_u : surface.knots_v;
    const int count = fixed == 0 ? surface.points_u : surface.points_v;
    const int order = fixed == 0 ? surface.order_u : surface.order_v;
    const bool high = parameter[fixed] == knots[count];
    if (!high && parameter[fixed] != knots[order - 1]) {
      continue;
    }
    const int boundary = high ? count - 1 : 0;
    const double3 pole(
        surface.points[fixed == 0 ? boundary : boundary * surface.points_u].position);
    if (!collapsed_surface_boundary_interval(surface, parameter, parameter, pole)) {
      continue;
    }
    const int adjacent = high ? count - 2 : 1;
    const int other_count = varying == 0 ? surface.points_u : surface.points_v;
    const int other_order = varying == 0 ? surface.order_u : surface.order_v;
    const double *other_knots = varying == 0 ? surface.knots_u : surface.knots_v;
    const auto basis = calculate_basis(other_knots,
                                       other_count,
                                       other_order,
                                       parameter[varying],
                                       nullptr,
                                       varying == 0 ? limits.u : limits.v);
    if (!basis) {
      return std::nullopt;
    }
    const auto derivative = derivative_basis(
        other_knots, other_order, parameter[varying], *basis, nullptr);
    if (!derivative) {
      return std::nullopt;
    }
    Vector<WeightedPoint, 64> points;
    for (const int i : IndexRange(other_order)) {
      const int j = basis->start + i;
      const auto &point = surface.points[fixed == 0 ? j * surface.points_u + adjacent :
                                                      adjacent * surface.points_u + j];
      if (point.weight <= 0) {
        return std::nullopt;
      }
      points.append({&point, basis->values[i]});
    }
    const auto value = evaluate_weighted_points(points, nullptr);
    if (!value) {
      return std::nullopt;
    }
    CompensatedSum numerator[3];
    for (const int i : IndexRange(other_order)) {
      const auto &point = *points[i].point;
      const double coefficient = (point.weight / value->weight_scale) * (*derivative)[i];
      for (const int axis : IndexRange(3)) {
        numerator[axis].add(coefficient * ((point.position[axis] - value->origin[axis]) -
                                           value->relative_position[axis]));
      }
    }
    double3 tangent, radial;
    for (const int axis : IndexRange(3)) {
      tangent[axis] = numerator[axis].value / value->denominator;
      radial[axis] = (value->origin[axis] - pole[axis]) + value->relative_position[axis];
      if (!std::isfinite(tangent[axis]) || !std::isfinite(radial[axis])) {
        return std::nullopt;
      }
    }
    const double tangent_scale = math::reduce_max(math::abs(tangent));
    const double radial_scale = math::reduce_max(math::abs(radial));
    if (tangent_scale == 0 || radial_scale == 0) {
      continue;
    }
    const double3 cross = fixed == 0 ?
                              math::cross(radial / radial_scale, tangent / tangent_scale) :
                              math::cross(tangent / tangent_scale, radial / radial_scale);
    const double length = std::hypot(cross.x, cross.y, cross.z);
    if (length != 0 && std::isfinite(length)) {
      return cross * ((high ? -1.0 : 1.0) / length);
    }
  }
  return std::nullopt;
}

}  // namespace blender::bke::nurbs
