/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <string_view>

#include <fmt/format.h>

#include "BLI_listbase.h"
#include "BLI_math_boolean.hh"
#include "BLI_set.hh"

#include "BKE_nurbs_evaluate.hh"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {

static std::nullopt_t evaluation_error(std::string *r_error, const std::string_view message)
{
  if (r_error != nullptr) {
    *r_error = message;
  }
  return std::nullopt;
}

static bool positive_weights(const Span<NurbsControlPoint> points)
{
  return std::all_of(points.begin(), points.end(), [](const NurbsControlPoint &point) {
    return point.weight > 0.0;
  });
}

static bool continuous_knots(const double *knots,
                             const int knots_num,
                             const int order,
                             const double start,
                             const double end)
{
  for (int i = 0; i < knots_num;) {
    int next = i + 1;
    while (next < knots_num && knots[next] == knots[i]) {
      next++;
    }
    /* Scalar evaluation takes the right limit at interior knots. At a restricted edge's end,
     * that would join the left span to a potentially disconnected right-hand endpoint. */
    const bool before_end = knots[i] < end || (knots[i] == end && end < knots[knots_num - order]);
    if (knots[i] > start && before_end && next - i >= order) {
      return false;
    }
    i = next;
  }
  return true;
}

/** A measured distance, not an outward-rounded error certificate. Avoid squaring overflow. */
static double measured_distance(const double3 &a, const double3 &b)
{
  return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

static bool exact_sum(const double a, const double b, double &value)
{
  double residual;
  return detail::sum_with_error(a, b, value, residual) && residual == 0;
}

static bool exact_product(const double a, const double b, double &value)
{
  double error;
  return detail::product_with_error(a, b, value, error) && error == 0;
}

static bool polynomial_boundary_pair(const NurbsCurveGeometry &curve,
                                     const NurbsCurveGeometry &pcurve,
                                     const NurbsSurfaceGeometry &surface)
{
  auto linear = [](const NurbsCurveGeometry &item) {
    return item.order == 2 && item.points_num == 2 && item.knots_num == 4 &&
           item.knots[0] == item.knots[1] && item.knots[2] == item.knots[3] &&
           item.points[0].weight > 0 && item.points[0].weight == item.points[1].weight;
  };
  if (curve.dimension != 3 || pcurve.dimension != 2 || !linear(curve) || !linear(pcurve) ||
      surface.order_u != 2 || surface.order_v != 2 || surface.points_u != 2 ||
      surface.points_v != 2 || surface.knots_u[0] != surface.knots_u[1] ||
      surface.knots_u[2] != surface.knots_u[3] || surface.knots_v[0] != surface.knots_v[1] ||
      surface.knots_v[2] != surface.knots_v[3])
  {
    return false;
  }
  for (const int i : IndexRange(4)) {
    if (curve.knots[i] != pcurve.knots[i] || surface.points[i].weight <= 0 ||
        surface.points[i].weight != surface.points[0].weight)
    {
      return false;
    }
  }
  for (const int end : IndexRange(2)) {
    if (pcurve.points[end].position[0] < surface.knots_u[1] ||
        pcurve.points[end].position[0] > surface.knots_u[2] ||
        pcurve.points[end].position[1] < surface.knots_v[1] ||
        pcurve.points[end].position[1] > surface.knots_v[2])
    {
      return false;
    }
  }
  return true;
}

/** Tight certificates for natural sides/diagonals and exact affine charts. */
static std::optional<double> specialized_curve_support_bound(const NurbsCurveGeometry &curve,
                                                             const NurbsCurveGeometry &pcurve,
                                                             const NurbsSurfaceGeometry &surface)
{
  auto affine_chart_identity = [&]() -> std::optional<double> {
    /* Prove each coordinate's graph is an affine function of the original UVs. Blender's
     * adaptive exact predicate avoids rounded UV normalization (e.g. (-1/3 + 1) / 2).
     * Keep its products/splits in a conservative normal-exponent range; extreme inputs
     * retain the ordinary checks instead of claiming an exact predicate certificate. */
    auto safe = [](const double value) {
      return value == 0 ||
             (std::abs(value) >= std::ldexp(1.0, -100) && std::abs(value) <= std::ldexp(1.0, 100));
    };
    const double u0 = surface.knots_u[1], u1 = surface.knots_u[2];
    const double v0 = surface.knots_v[1], v1 = surface.knots_v[2];
    if (!safe(u0) || !safe(u1) || !safe(v0) || !safe(v1)) {
      return std::nullopt;
    }
    for (const int axis : IndexRange(3)) {
      for (const int i : IndexRange(4)) {
        if (!safe(surface.points[i].position[axis])) {
          return std::nullopt;
        }
      }
      const double3 a(u0, v0, surface.points[0].position[axis]);
      const double3 b(u1, v0, surface.points[1].position[axis]);
      const double3 c(u0, v1, surface.points[2].position[axis]);
      if (orient3d(a, b, c, {u1, v1, surface.points[3].position[axis]}) != 0) {
        return std::nullopt;
      }
      for (const int end : IndexRange(2)) {
        const double3 point(pcurve.points[end].position[0],
                            pcurve.points[end].position[1],
                            curve.points[end].position[axis]);
        if (!safe(point.x) || !safe(point.y) || !safe(point.z) || orient3d(a, b, c, point) != 0) {
          return std::nullopt;
        }
      }
    }
    return 0.0;
  };
  double2 uv[2];
  for (const int end : IndexRange(2)) {
    for (const int axis : IndexRange(2)) {
      const double *knots = axis == 0 ? surface.knots_u : surface.knots_v;
      if (pcurve.points[end].position[axis] == knots[1]) {
        uv[end][axis] = 0;
        continue;
      }
      if (pcurve.points[end].position[axis] == knots[2]) {
        uv[end][axis] = 1;
        continue;
      }
      double span, delta, recovered;
      if (!exact_sum(knots[2], -knots[1], span) || span <= 0 ||
          !exact_sum(pcurve.points[end].position[axis], -knots[1], delta))
      {
        return affine_chart_identity();
      }
      uv[end][axis] = delta / span;
      if (!std::isfinite(uv[end][axis]) || uv[end][axis] < 0 || uv[end][axis] > 1 ||
          !exact_product(uv[end][axis], span, recovered) || recovered != delta)
      {
        return affine_chart_identity();
      }
    }
  }
  const bool natural_side = (uv[0].x == uv[1].x && ELEM(uv[0].x, 0, 1)) ||
                            (uv[0].y == uv[1].y && ELEM(uv[0].y, 0, 1));
  if (ELEM(uv[0].x, 0, 1) && ELEM(uv[0].y, 0, 1) && ELEM(uv[1].x, 0, 1) && ELEM(uv[1].y, 0, 1)) {
    for (const int end : IndexRange(2)) {
      const int corner = int(uv[end].x) + 2 * int(uv[end].y);
      if (double3(curve.points[end].position) != double3(surface.points[corner].position)) {
        return std::nullopt;
      }
    }
    if (natural_side) {
      return 0.0;
    }
    // Opposite corners: the quadratic support boundary differs from its straight chord by
    // t(1-t) times the bilinear mixed coefficient, at most one quarter of its norm.
    // Bound that coefficient with TwoSum residuals. This covers rounded affine triangle
    // construction without confusing independent evaluator roundoff with a modeling gap.
    double mixed_bound = 0;
    for (const int axis : IndexRange(3)) {
      double a, a_error, b, b_error, high, high_error, low, low_error, value, value_error;
      if (!detail::sum_with_error(
              surface.points[0].position[axis], surface.points[3].position[axis], a, a_error) ||
          !detail::sum_with_error(
              surface.points[1].position[axis], surface.points[2].position[axis], b, b_error) ||
          !detail::sum_with_error(a, -b, high, high_error) ||
          !detail::sum_with_error(a_error, -b_error, low, low_error) ||
          !detail::sum_with_error(high, low, value, value_error))
      {
        return std::nullopt;
      }
      double bound = detail::add_upper(std::abs(value), std::abs(value_error));
      bound = detail::add_upper(bound, std::abs(high_error));
      bound = detail::add_upper(bound, std::abs(low_error));
      mixed_bound = detail::add_upper(mixed_bound, bound);
    }
    return detail::multiply_upper(0.25, mixed_bound);
  }
  for (const int axis : IndexRange(3)) {
    const double a = surface.points[0].position[axis];
    double diagonal_a, diagonal_b, du, dv;
    if (!exact_sum(a, surface.points[3].position[axis], diagonal_a) ||
        !exact_sum(
            surface.points[1].position[axis], surface.points[2].position[axis], diagonal_b) ||
        diagonal_a != diagonal_b || !exact_sum(surface.points[1].position[axis], -a, du) ||
        !exact_sum(surface.points[2].position[axis], -a, dv))
    {
      return affine_chart_identity();
    }
    for (const int end : IndexRange(2)) {
      double u_term, v_term, partial, value;
      if (!exact_product(du, uv[end].x, u_term) || !exact_product(dv, uv[end].y, v_term) ||
          !exact_sum(u_term, v_term, partial) || !exact_sum(a, partial, value) ||
          value != curve.points[end].position[axis])
      {
        return affine_chart_identity();
      }
    }
  }
  return 0.0;
}

/** Along a straight p-curve, a bilinear support is a quadratic Bezier curve. Its difference
 * from the degree-elevated source line has three Bernstein coefficients. Their largest L1
 * norm bounds the gap over the entire edge, independently of sampled evaluator roundoff. */
static std::optional<double> bernstein_curve_support_bound(const NurbsCurveGeometry &curve,
                                                           const NurbsCurveGeometry &pcurve,
                                                           const NurbsSurfaceGeometry &surface)
{
  using detail::bounded_add;
  using detail::bounded_divide;
  using detail::bounded_multiply;
  using detail::bounded_subtract;
  using detail::BoundedScalar;
  BoundedScalar uv[2][2];
  for (const int end : IndexRange(2)) {
    for (const int axis : IndexRange(2)) {
      const double *knots = axis == 0 ? surface.knots_u : surface.knots_v;
      const double parameter = pcurve.points[end].position[axis];
      if (parameter < knots[1] || parameter > knots[2]) {
        return std::nullopt;
      }
      uv[end][axis] = bounded_divide(bounded_subtract({parameter}, {knots[1]}),
                                     bounded_subtract({knots[2]}, {knots[1]}));
      if (!uv[end][axis].valid()) {
        return std::nullopt;
      }
    }
  }
  auto difference =
      [&](const BoundedScalar u, const BoundedScalar v, const int axis, const double reference) {
        const BoundedScalar weights[] = {
            bounded_multiply(bounded_subtract({1}, u), bounded_subtract({1}, v)),
            bounded_multiply(u, bounded_subtract({1}, v)),
            bounded_multiply(bounded_subtract({1}, u), v),
            bounded_multiply(u, v)};
        BoundedScalar value;
        for (const int i : IndexRange(4)) {
          // Subtract the line coefficient first to avoid cancellation at large source origins.
          value = bounded_add(
              value,
              bounded_multiply(weights[i],
                               bounded_subtract({surface.points[i].position[axis]}, {reference})));
        }
        return value;
      };
  double bound = 0;
  for (const int coefficient : IndexRange(3)) {
    double coefficient_bound = 0;
    for (const int axis : IndexRange(3)) {
      BoundedScalar value;
      if (coefficient != 1) {
        const int end = coefficient / 2;
        value = difference(uv[end][0], uv[end][1], axis, curve.points[end].position[axis]);
      }
      else {
        value = bounded_multiply(
            {0.5},
            bounded_add(difference(uv[0][0], uv[1][1], axis, curve.points[0].position[axis]),
                        difference(uv[1][0], uv[0][1], axis, curve.points[1].position[axis])));
      }
      if (!value.valid()) {
        return std::nullopt;
      }
      coefficient_bound = detail::add_upper(coefficient_bound,
                                            detail::add_upper(std::abs(value.value), value.error));
    }
    bound = std::max(bound, coefficient_bound);
  }
  return std::isfinite(bound) ? std::optional(bound) : std::nullopt;
}

/** Along a constant U or V trim, a polynomial bilinear surface is affine in the varying
 * coordinate. Matching rational endpoint profiles therefore reduce the whole-edge discrepancy
 * to the convex hull of two coefficient gaps, including bounded coordinate normalization. */
static std::optional<double> isoparametric_curve_support_bound(const NurbsCurveGeometry &curve,
                                                               const NurbsCurveGeometry &pcurve,
                                                               const NurbsSurfaceGeometry &surface)
{
  using namespace detail;
  const auto linear = [](const NurbsCurveGeometry &item) {
    return item.order == 2 && item.points_num == 2 && item.knots_num == 4 &&
           item.knots[0] == item.knots[1] && item.knots[2] == item.knots[3] &&
           item.points[0].weight > 0 && item.points[1].weight > 0;
  };
  if (!supported_arithmetic() || curve.dimension != 3 || pcurve.dimension != 2 || !linear(curve) ||
      !linear(pcurve) || !std::equal(curve.knots, curve.knots + curve.knots_num, pcurve.knots) ||
      surface.order_u != 2 || surface.order_v != 2 || surface.points_u != 2 ||
      surface.points_v != 2 || surface.knots_u[0] != surface.knots_u[1] ||
      surface.knots_u[2] != surface.knots_u[3] || surface.knots_v[0] != surface.knots_v[1] ||
      surface.knots_v[2] != surface.knots_v[3] ||
      (pcurve.points[0].position[0] != pcurve.points[1].position[0] &&
       pcurve.points[0].position[1] != pcurve.points[1].position[1]))
  {
    return std::nullopt;
  }
  for (int corner = 0; corner < 4; corner++) {
    if (!(surface.points[corner].weight > 0) ||
        surface.points[corner].weight != surface.points[0].weight)
    {
      return std::nullopt;
    }
  }
  double a, ae, b, be;
  if (!product_with_error(curve.points[0].weight, pcurve.points[1].weight, a, ae) ||
      !product_with_error(curve.points[1].weight, pcurve.points[0].weight, b, be) || a != b ||
      ae != be)
  {
    return std::nullopt;
  }
  double bound = 0;
  for (int end = 0; end < 2; end++) {
    BoundedScalar uv[2];
    for (int axis = 0; axis < 2; axis++) {
      const double *knots = axis == 0 ? surface.knots_u : surface.knots_v;
      uv[axis] = bounded_divide(bounded_subtract({pcurve.points[end].position[axis]}, {knots[1]}),
                                bounded_subtract({knots[2]}, {knots[1]}));
    }
    const BoundedScalar weights[] = {
        bounded_multiply(bounded_subtract({1}, uv[0]), bounded_subtract({1}, uv[1])),
        bounded_multiply(uv[0], bounded_subtract({1}, uv[1])),
        bounded_multiply(bounded_subtract({1}, uv[0]), uv[1]),
        bounded_multiply(uv[0], uv[1])};
    double endpoint_bound = 0;
    for (int axis = 0; axis < 3; axis++) {
      BoundedScalar gap;
      for (int corner = 0; corner < 4; corner++) {
        gap = bounded_add(
            gap,
            bounded_multiply(weights[corner],
                             bounded_subtract({surface.points[corner].position[axis]},
                                              {curve.points[end].position[axis]})));
      }
      if (!gap.valid()) {
        return std::nullopt;
      }
      endpoint_bound = add_upper(endpoint_bound, add_upper(std::abs(gap.value), gap.error));
    }
    bound = std::max(bound, endpoint_bound);
  }
  return std::isfinite(bound) ? std::optional(bound) : std::nullopt;
}

std::optional<double> linear_curve_support_deviation_bound(const NurbsCurveGeometry &curve,
                                                           const NurbsCurveGeometry &pcurve,
                                                           const NurbsSurfaceGeometry &surface)
{
  const auto isoparametric = isoparametric_curve_support_bound(curve, pcurve, surface);
  const auto minimum = [&](const std::optional<double> other) {
    return isoparametric && other ? std::optional(std::min(*isoparametric, *other)) :
                                    (isoparametric ? isoparametric : other);
  };
  if (!detail::supported_arithmetic() || !polynomial_boundary_pair(curve, pcurve, surface)) {
    return isoparametric;
  }
  auto specialized = specialized_curve_support_bound(curve, pcurve, surface);
  if (specialized && !std::isfinite(*specialized)) {
    specialized.reset();
  }
  if (specialized == 0.0) {
    return specialized;
  }
  const auto bernstein = bernstein_curve_support_bound(curve, pcurve, surface);
  if (!specialized) {
    return minimum(bernstein);
  }
  return minimum(bernstein ? std::optional(std::min(*specialized, *bernstein)) : specialized);
}

static std::optional<double> curve_support_deviation_bound_impl(
    const NurbsCurveGeometry &curve,
    const NurbsCurveGeometry &pcurve,
    const NurbsSurfaceGeometry &surface,
    const bool bound_bilinear_twist)
{
  using namespace detail;
  if (!supported_arithmetic() || curve.dimension != 3 || pcurve.dimension != 2 ||
      curve.order != pcurve.order || curve.points_num != pcurve.points_num ||
      curve.knots_num != pcurve.knots_num || surface.order_u != 2 || surface.order_v != 2 ||
      surface.points_u != 2 || surface.points_v != 2 || surface.knots_u[0] != surface.knots_u[1] ||
      surface.knots_u[2] != surface.knots_u[3] || surface.knots_v[0] != surface.knots_v[1] ||
      surface.knots_v[2] != surface.knots_v[3] ||
      !std::equal(curve.knots, curve.knots + curve.knots_num, pcurve.knots))
  {
    return std::nullopt;
  }
  for (int i = 0; i < 4; i++) {
    if (!(surface.points[i].weight > 0) || surface.points[i].weight != surface.points[0].weight) {
      return std::nullopt;
    }
  }
  BoundedScalar affine_corners[4][3];
  double twist_bound = 0;
  for (int axis = 0; axis < 3; axis++) {
    for (int corner = 0; corner < 3; corner++) {
      affine_corners[corner][axis] = {surface.points[corner].position[axis]};
    }
    if (!bound_bilinear_twist) {
      double a, ae, b, be;
      if (!sum_with_error(
              surface.points[0].position[axis], surface.points[3].position[axis], a, ae) ||
          !sum_with_error(
              surface.points[1].position[axis], surface.points[2].position[axis], b, be) ||
          a != b || ae != be)
      {
        return std::nullopt;
      }
      affine_corners[3][axis] = {surface.points[3].position[axis]};
      continue;
    }
    const BoundedScalar expected = bounded_subtract(
        bounded_add({surface.points[1].position[axis]}, {surface.points[2].position[axis]}),
        {surface.points[0].position[axis]});
    const BoundedScalar residual = bounded_subtract({surface.points[3].position[axis]}, expected);
    if (!expected.valid() || !residual.valid()) {
      return std::nullopt;
    }
    affine_corners[3][axis] = expected;
    twist_bound = add_upper(twist_bound, add_upper(std::abs(residual.value), residual.error));
  }
  double bound = 0, maximum_u = 0, maximum_v = 0;
  for (int i = 0; i < curve.points_num; i++) {
    if (!(curve.points[i].weight > 0) || curve.points[i].weight != pcurve.points[i].weight) {
      return std::nullopt;
    }
    BoundedScalar uv[2];
    for (int axis = 0; axis < 2; axis++) {
      const double *knots = axis == 0 ? surface.knots_u : surface.knots_v;
      uv[axis] = bounded_divide(bounded_subtract({pcurve.points[i].position[axis]}, {knots[1]}),
                                bounded_subtract({knots[2]}, {knots[1]}));
      if (!uv[axis].valid()) {
        return std::nullopt;
      }
      const double maximum = add_upper(std::abs(uv[axis].value), uv[axis].error);
      if (axis == 0) {
        maximum_u = std::max(maximum_u, maximum);
      }
      else {
        maximum_v = std::max(maximum_v, maximum);
      }
    }
    const BoundedScalar weights[] = {
        bounded_multiply(bounded_subtract({1}, uv[0]), bounded_subtract({1}, uv[1])),
        bounded_multiply(uv[0], bounded_subtract({1}, uv[1])),
        bounded_multiply(bounded_subtract({1}, uv[0]), uv[1]),
        bounded_multiply(uv[0], uv[1])};
    double coefficient_bound = 0;
    for (int axis = 0; axis < 3; axis++) {
      BoundedScalar gap;
      for (int corner = 0; corner < 4; corner++) {
        gap = bounded_add(gap,
                          bounded_multiply(weights[corner],
                                           bounded_subtract(affine_corners[corner][axis],
                                                            {curve.points[i].position[axis]})));
      }
      if (!gap.valid()) {
        return std::nullopt;
      }
      coefficient_bound = add_upper(coefficient_bound, add_upper(std::abs(gap.value), gap.error));
    }
    bound = std::max(bound, coefficient_bound);
  }
  if (bound_bilinear_twist) {
    bound = add_upper(bound, multiply_upper(twist_bound, multiply_upper(maximum_u, maximum_v)));
  }
  return std::isfinite(bound) ? std::optional(bound) : std::nullopt;
}

std::optional<double> affine_curve_support_deviation_bound(const NurbsCurveGeometry &curve,
                                                           const NurbsCurveGeometry &pcurve,
                                                           const NurbsSurfaceGeometry &surface)
{
  return curve_support_deviation_bound_impl(curve, pcurve, surface, false);
}

std::optional<double> bilinear_curve_support_deviation_bound(const NurbsCurveGeometry &curve,
                                                             const NurbsCurveGeometry &pcurve,
                                                             const NurbsSurfaceGeometry &surface)
{
  return curve_support_deviation_bound_impl(curve, pcurve, surface, true);
}

/** Identity at a clamped tensor boundary. The linear p-curve maps edge parameters identically
 * to the varying support coordinate. Rational arcs and constant-weight rails retain their basis.
 */
static bool natural_curve_support_identity(const NurbsCurveGeometry &curve,
                                           const NurbsCurveGeometry &pcurve,
                                           const NurbsSurfaceGeometry &surface)
{
  if (curve.dimension != 3 || pcurve.dimension != 2 || pcurve.order != 2 ||
      pcurve.points_num != 2 || pcurve.knots[0] != pcurve.knots[1] ||
      pcurve.knots[2] != pcurve.knots[3] || !(pcurve.points[0].weight > 0) ||
      pcurve.points[0].weight != pcurve.points[1].weight)
  {
    return false;
  }
  for (int axis = 0; axis < 2; axis++) {
    const int other = 1 - axis;
    const double *knots = axis == 0 ? surface.knots_u : surface.knots_v;
    const double *fixed_knots = other == 0 ? surface.knots_u : surface.knots_v;
    const int order = axis == 0 ? surface.order_u : surface.order_v;
    const int count = axis == 0 ? surface.points_u : surface.points_v;
    const int fixed_order = other == 0 ? surface.order_u : surface.order_v;
    const int fixed_count = other == 0 ? surface.points_u : surface.points_v;
    const double fixed = pcurve.points[0].position[other];
    const bool high = fixed == fixed_knots[fixed_count];
    if (fixed != pcurve.points[1].position[other] ||
        (!high && fixed != fixed_knots[fixed_order - 1]) || curve.order != order ||
        curve.points_num != count ||
        !std::equal(curve.knots, curve.knots + curve.knots_num, knots) ||
        pcurve.knots[1] != curve.knots[order - 1] || pcurve.knots[2] != curve.knots[count] ||
        pcurve.points[0].position[axis] != pcurve.knots[1] ||
        pcurve.points[1].position[axis] != pcurve.knots[2])
    {
      continue;
    }
    bool valid = true, same_weights = true, constant_weights = true;
    const int fixed_index = high ? fixed_count - 1 : 0;
    const int first = axis == 0 ? fixed_index * surface.points_u : fixed_index;
    for (int i = 0; i < fixed_order; i++) {
      valid &= fixed_knots[high ? fixed_count + i : i] == fixed;
    }
    for (int i = 0; i < count; i++) {
      const auto &point = surface.points[axis == 0 ? first + i : first + i * surface.points_u];
      valid &= curve.points[i].weight > 0 && point.weight > 0 &&
               double3(curve.points[i].position) == double3(point.position);
      same_weights &= curve.points[i].weight == point.weight;
      constant_weights &= curve.points[i].weight == curve.points[0].weight &&
                          point.weight == surface.points[first].weight;
    }
    if (valid && (same_weights || constant_weights)) {
      return true;
    }
  }
  return false;
}

bool collapsed_surface_boundary_interval(const NurbsSurfaceGeometry &surface,
                                         const double2 &first,
                                         const double2 &second,
                                         const double3 &position)
{
  if (!std::isfinite(first.x) || !std::isfinite(first.y) || !std::isfinite(second.x) ||
      !std::isfinite(second.y))
  {
    return false;
  }
  for (int fixed = 0; fixed < 2; fixed++) {
    const int varying = 1 - fixed;
    const double *knots = fixed == 0 ? surface.knots_u : surface.knots_v;
    const double *other = varying == 0 ? surface.knots_u : surface.knots_v;
    const int order = fixed == 0 ? surface.order_u : surface.order_v;
    const int count = fixed == 0 ? surface.points_u : surface.points_v;
    const int other_order = varying == 0 ? surface.order_u : surface.order_v;
    const int other_count = varying == 0 ? surface.points_u : surface.points_v;
    const bool high = first[fixed] == knots[count];
    if (first[fixed] != second[fixed] || (!high && first[fixed] != knots[order - 1]) ||
        first[varying] < other[other_order - 1] || first[varying] > other[other_count] ||
        second[varying] < other[other_order - 1] || second[varying] > other[other_count])
    {
      continue;
    }
    bool valid = true;
    for (int i = 0; i < order; i++) {
      valid &= knots[high ? count + i : i] == first[fixed];
    }
    for (int i = 0; i < other_count; i++) {
      const int slot = fixed == 0 ? i * surface.points_u + (high ? count - 1 : 0) :
                                    (high ? count - 1 : 0) * surface.points_u + i;
      valid &= surface.points[slot].weight > 0 &&
               double3(surface.points[slot].position) == position;
    }
    if (valid) {
      return true;
    }
  }
  return false;
}

std::optional<BoundaryEvaluation> evaluate_boundaries(const NurbsGeometry &source,
                                                      const BoundaryEvaluationOptions &options,
                                                      std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const std::string_view message) { return evaluation_error(r_error, message); };
  if (!detail::supported_arithmetic()) {
    return fail("NURBS boundary evaluation requires round-to-nearest and subnormal support");
  }
  if (options.segments_per_span < 1 || options.max_sample_entries < 1 ||
      !std::isfinite(options.evaluation_allowance) || options.evaluation_allowance < 0.0)
  {
    return fail("Invalid NURBS boundary sampling options");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : source.curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }

  BoundaryEvaluation result;
  int64_t remaining = options.max_sample_entries;
  auto consume = [&](const int64_t count) {
    if (count > remaining) {
      return false;
    }
    remaining -= count;
    return true;
  };
  Map<int64_t, int> vertex_indices;
  Map<int64_t, int> edge_indices;
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (!consume(1)) {
      return fail("NURBS boundary sampling exceeds the output entry limit");
    }
    vertex_indices.add_new(vertex.id, int(result.positions.size()));
    result.positions.append(double3(vertex.position));
    result.vertex_ids.append(vertex.id);
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    Set<double> breakpoints;
    breakpoints.add(edge.parameter_range[0]);
    breakpoints.add(edge.parameter_range[1]);
    auto add_curve_breaks = [&](const NurbsCurveGeometry &curve) {
      if (!positive_weights({curve.points, curve.points_num}) ||
          !continuous_knots(curve.knots,
                            curve.knots_num,
                            curve.order,
                            edge.parameter_range[0],
                            edge.parameter_range[1]))
      {
        return false;
      }
      for (int i = 0; i < curve.knots_num; i++) {
        if (curve.knots[i] > edge.parameter_range[0] && curve.knots[i] < edge.parameter_range[1]) {
          breakpoints.add(curve.knots[i]);
          if (breakpoints.size() > remaining) {
            return false;
          }
        }
      }
      return true;
    };
    const NurbsCurveGeometry &curve = *curves.lookup(edge.curve_id);
    if (!add_curve_breaks(curve)) {
      return fail(
          "NURBS boundary curve requires positive weights, continuity and sampling capacity");
    }
    for (const EdgeUseReference &reference : index->uses_for_edge(edge.id)) {
      if (!add_curve_breaks(*curves.lookup(reference.use->pcurve_id))) {
        return fail(
            "NURBS boundary p-curve requires positive weights, continuity and sampling capacity");
      }
    }
    Vector<double> breaks;
    for (const double parameter : breakpoints) {
      breaks.append(parameter);
    }
    std::sort(breaks.begin(), breaks.end());
    const int64_t sample_count = 1 + (breaks.size() - 1) * options.segments_per_span;
    if (!consume(sample_count)) {
      return fail("NURBS boundary sampling exceeds the output entry limit");
    }
    EvaluatedBoundaryEdge evaluated;
    evaluated.edge_id = edge.id;
    auto append_sample = [&](const double parameter, const int endpoint) {
      const auto point = evaluate_curve(curve, parameter, r_error);
      if (!point) {
        if (r_error != nullptr) {
          *r_error = fmt::format(
              "NURBS boundary edge {} at {:.17g}: {}", edge.id, parameter, *r_error);
        }
        return false;
      }
      int point_index;
      if (endpoint >= 0) {
        const NurbsTopologyVertex &vertex = *index->vertex(edge.vertex_ids[endpoint]);
        point_index = vertex_indices.lookup(vertex.id);
        const double deviation = measured_distance(point->position, double3(vertex.position));
        evaluated.max_endpoint_deviation = std::max(evaluated.max_endpoint_deviation, deviation);
        if (!std::isfinite(deviation) ||
            deviation > detail::add_upper(std::max(edge.tolerance, vertex.tolerance),
                                          options.evaluation_allowance))
        {
          evaluation_error(
              r_error,
              fmt::format("NURBS edge {} endpoint {}: measured deviation {:.17g} "
                          "exceeds allowed agreement {:.17g}",
                          edge.id,
                          vertex.id,
                          deviation,
                          detail::add_upper(std::max(edge.tolerance, vertex.tolerance),
                                            options.evaluation_allowance)));
          return false;
        }
      }
      else {
        point_index = int(result.positions.size());
        result.positions.append(point->position);
        result.vertex_ids.append(0);
      }
      evaluated.samples.append({parameter, point_index, point->position});
      evaluated.max_weight_condition = std::max(evaluated.max_weight_condition,
                                                point->weight_condition);
      return true;
    };
    if (!append_sample(breaks.first(), 0)) {
      return std::nullopt;
    }
    for (const int span : IndexRange(breaks.size() - 1)) {
      for (int step = 1; step <= options.segments_per_span; step++) {
        const double t = double(step) / options.segments_per_span;
        const double parameter = std::lerp(breaks[span], breaks[span + 1], t);
        if (parameter <= evaluated.samples.last().parameter ||
            (step < options.segments_per_span && parameter >= breaks[span + 1]))
        {
          return fail("NURBS boundary subdivision is not representable in double precision");
        }
        const bool is_end = span == breaks.size() - 2 && step == options.segments_per_span;
        if (!append_sample(parameter, is_end ? 1 : -1)) {
          return std::nullopt;
        }
      }
    }
    edge_indices.add_new(edge.id, int(result.edges.size()));
    result.edges.append(std::move(evaluated));
  }
  for (const NurbsTopologyFace &face : source.faces) {
    const NurbsSurfaceGeometry &surface = *surfaces.lookup(face.surface_id);
    if (!positive_weights({surface.points, surface.points_num}) ||
        !continuous_knots(surface.knots_u,
                          surface.knots_u_num,
                          surface.order_u,
                          surface.knots_u[surface.order_u - 1],
                          surface.knots_u[surface.points_u]) ||
        !continuous_knots(surface.knots_v,
                          surface.knots_v_num,
                          surface.order_v,
                          surface.knots_v[surface.order_v - 1],
                          surface.knots_v[surface.points_v]))
    {
      return fail("NURBS boundary support requires positive weights and continuity");
    }
    for (const NurbsTopologyLoop &loop : face.loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const int edge_index = edge_indices.lookup(use.edge_id);
        const EvaluatedBoundaryEdge &evaluated_edge = result.edges[edge_index];
        if (!consume(evaluated_edge.samples.size())) {
          return fail("NURBS boundary sampling exceeds the output entry limit");
        }
        const NurbsTopologyEdge &edge = *index->edge(use.edge_id);
        const NurbsCurveGeometry &pcurve = *curves.lookup(use.pcurve_id);
        EvaluatedBoundaryUse evaluated;
        evaluated.face_id = face.id;
        evaluated.loop_id = loop.id;
        evaluated.use_id = use.id;
        evaluated.edge_index = edge_index;
        evaluated.reversed = use.reversed;
        evaluated.is_inner = loop.is_inner;
        evaluated.curve_support_deviation_bound = linear_curve_support_deviation_bound(
            *curves.lookup(edge.curve_id), pcurve, surface);
        if (natural_curve_support_identity(*curves.lookup(edge.curve_id), pcurve, surface)) {
          evaluated.curve_support_deviation_bound = 0.0;
        }
        else {
          /* Construction services also certify small bilinear twist. Use every valid
           * whole-curve certificate here, so a different, more conservative proof cannot
           * reject geometry already certified within its declared source tolerance. */
          for (const auto bound : {affine_curve_support_deviation_bound(
                                       *curves.lookup(edge.curve_id), pcurve, surface),
                                   bilinear_curve_support_deviation_bound(
                                       *curves.lookup(edge.curve_id), pcurve, surface)})
          {
            if (bound) {
              evaluated.curve_support_deviation_bound =
                  evaluated.curve_support_deviation_bound ?
                      std::min(*bound, *evaluated.curve_support_deviation_bound) :
                      *bound;
            }
          }
        }
        evaluated.exact_curve_support_identity = evaluated.curve_support_deviation_bound == 0.0;
        const double tolerance = std::max({edge.tolerance, use.tolerance, face.tolerance});
        for (const BoundarySample &sample : evaluated_edge.samples) {
          const auto uv = evaluate_curve(pcurve, sample.parameter, r_error);
          if (!uv) {
            if (r_error != nullptr) {
              *r_error = fmt::format("NURBS face {} use {} p-curve at {:.17g}: {}",
                                     face.id,
                                     use.id,
                                     sample.parameter,
                                     *r_error);
            }
            return std::nullopt;
          }
          const auto support = evaluate_surface(surface, double2(uv->position), r_error);
          if (!support) {
            if (r_error != nullptr) {
              *r_error = fmt::format("NURBS face {} use {} support at ({:.17g}, {:.17g}): {}",
                                     face.id,
                                     use.id,
                                     uv->position.x,
                                     uv->position.y,
                                     *r_error);
            }
            return std::nullopt;
          }
          const double curve_deviation = measured_distance(sample.curve_position,
                                                           support->position);
          const double display_deviation = measured_distance(result.positions[sample.point_index],
                                                             support->position);
          const int64_t vertex_id = result.vertex_ids[sample.point_index];
          const double display_tolerance = vertex_id == 0 ?
                                               tolerance :
                                               std::max(tolerance,
                                                        index->vertex(vertex_id)->tolerance);
          const double checked_curve_deviation = evaluated.curve_support_deviation_bound.value_or(
              curve_deviation);
          const double checked_display_deviation =
              evaluated.curve_support_deviation_bound ?
                  detail::add_upper(*evaluated.curve_support_deviation_bound,
                                    measured_distance(result.positions[sample.point_index],
                                                      sample.curve_position)) :
                  display_deviation;
          if (!std::isfinite(curve_deviation) || !std::isfinite(display_deviation) ||
              checked_curve_deviation >
                  detail::add_upper(tolerance, options.evaluation_allowance) ||
              checked_display_deviation >
                  detail::add_upper(display_tolerance, options.evaluation_allowance))
          {
            return fail(
                fmt::format("NURBS face {} use {} edge/support at {:.17g}: measured curve/display "
                            "deviations {:.17g}/{:.17g}, checked {:.17g}/{:.17g}, exceed allowed "
                            "agreement {:.17g}/{:.17g}",
                            face.id,
                            use.id,
                            sample.parameter,
                            curve_deviation,
                            display_deviation,
                            checked_curve_deviation,
                            checked_display_deviation,
                            detail::add_upper(tolerance, options.evaluation_allowance),
                            detail::add_upper(display_tolerance, options.evaluation_allowance)));
          }
          evaluated.parameters.append(double2(uv->position));
          evaluated.max_curve_support_deviation = std::max(evaluated.max_curve_support_deviation,
                                                           curve_deviation);
          evaluated.max_display_support_deviation = std::max(
              evaluated.max_display_support_deviation, display_deviation);
          evaluated.max_weight_condition = std::max(
              {evaluated.max_weight_condition, uv->weight_condition, support->weight_condition});
        }
        result.uses.append(std::move(evaluated));
      }
    }
  }
  return result;
}

}  // namespace blender::bke::nurbs
