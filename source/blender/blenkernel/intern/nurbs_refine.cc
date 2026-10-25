/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <array>
#include <numeric>

#include "BKE_nurbs_refine.hh"
#include "MEM_guardedalloc.h"
#include "nurbs_math.hh"

namespace blender::bke::nurbs {
namespace {

/** Small outward binary64 enclosure. Exact operations retain point intervals. */
struct Interval {
  double low = 0, high = 0;
  Interval() = default;
  Interval(const double value) : low(value), high(value) {}
  Interval(const double low, const double high) : low(low), high(high) {}
  bool valid() const
  {
    return std::isfinite(low) && std::isfinite(high) && low <= high;
  }
  static Interval invalid()
  {
    return {NAN, NAN};
  }
};

static Interval operator+(const Interval a, const Interval b)
{
  if (!a.valid() || !b.valid()) {
    return Interval::invalid();
  }
  double lo, hi, elo, ehi;
  if (!detail::sum_with_error(a.low, b.low, lo, elo) ||
      !detail::sum_with_error(a.high, b.high, hi, ehi))
  {
    return Interval::invalid();
  }
  if (elo < 0) {
    lo = std::nextafter(lo, -INFINITY);
  }
  if (ehi > 0) {
    hi = std::nextafter(hi, INFINITY);
  }
  return {lo, hi};
}
static Interval operator-(const Interval a, const Interval b)
{
  return a + Interval(-b.high, -b.low);
}
static Interval operator*(const Interval a, const Interval b)
{
  if (!a.valid() || !b.valid()) {
    return Interval::invalid();
  }
  double lo = INFINITY, hi = -INFINITY;
  for (const double x : {a.low, a.high}) {
    for (const double y : {b.low, b.high}) {
      double value, error;
      if (!detail::product_with_error(x, y, value, error)) {
        return Interval::invalid();
      }
      lo = std::min(lo, error ? std::nextafter(value, -INFINITY) : value);
      hi = std::max(hi, error ? std::nextafter(value, INFINITY) : value);
    }
  }
  return {lo, hi};
}
static Interval operator/(const Interval a, const Interval b)
{
  if (!a.valid() || !b.valid() || (b.low <= 0 && b.high >= 0)) {
    return Interval::invalid();
  }
  double lo = INFINITY, hi = -INFINITY;
  for (const double x : {a.low, a.high}) {
    for (const double y : {b.low, b.high}) {
      const double value = x / y;
      double product, error;
      if (!std::isfinite(value) || !detail::product_with_error(value, y, product, error)) {
        return Interval::invalid();
      }
      const bool exact = error == 0 && product == x;
      lo = std::min(lo, exact ? value : std::nextafter(value, -INFINITY));
      hi = std::max(hi, exact ? value : std::nextafter(value, INFINITY));
    }
  }
  return {lo, hi};
}
static double midpoint(const Interval x)
{
  return x.valid() ? std::midpoint(x.low, x.high) : NAN;
}
static double radius(const Interval x, const double value)
{
  if (!x.valid() || !std::isfinite(value)) {
    return INFINITY;
  }
  const Interval lo = Interval(value) - Interval(x.low), hi = Interval(x.high) - Interval(value);
  return std::max({0.0, lo.high, hi.high});
}

struct Coefficient {
  NurbsControlPoint value;
  std::array<Interval, 4> homogeneous;
  std::array<Interval, 3> position;
  explicit Coefficient(const NurbsControlPoint &point) : value(point)
  {
    for (int axis = 0; axis < 3; axis++) {
      position[axis] = Interval(point.position[axis]);
      homogeneous[axis] = Interval(point.position[axis]) * Interval(point.weight);
    }
    homogeneous[3] = Interval(point.weight);
  }
  explicit Coefficient(const std::array<Interval, 4> &h) : homogeneous(h)
  {
    value.weight = midpoint(h[3]);
    for (int axis = 0; axis < 3; axis++) {
      position[axis] = h[axis] / h[3];
      value.position[axis] = midpoint(position[axis]);
    }
  }
  void set_position(const int axis, const Interval enclosure)
  {
    position[axis] = enclosure;
    value.position[axis] = midpoint(enclosure);
    homogeneous[axis] = enclosure * homogeneous[3];
  }
};
static Coefficient blend(const Coefficient &a, const Coefficient &b, const Interval alpha)
{
  if (alpha.low == 0 && alpha.high == 0) {
    return a;
  }
  if (alpha.low == 1 && alpha.high == 1) {
    return b;
  }
  std::array<Interval, 4> h;
  for (int i = 0; i < 4; i++) {
    h[i] = a.homogeneous[i] * (Interval(1) - alpha) + b.homogeneous[i] * alpha;
  }
  Coefficient result(h);
  /* Partition of unity preserves equal weights and constant Euclidean coordinates.
   * Keep those identities instead of losing their correlation in interval sums. */
  const bool equal_weights = a.homogeneous[3].low == a.homogeneous[3].high &&
                             a.homogeneous[3].low == b.homogeneous[3].low &&
                             a.homogeneous[3].high == b.homogeneous[3].high;
  if (equal_weights) {
    result.homogeneous[3] = a.homogeneous[3];
    result.value.weight = a.value.weight;
  }
  for (int axis = 0; axis < 3; axis++) {
    if (a.position[axis].low == a.position[axis].high &&
        a.position[axis].low == b.position[axis].low &&
        a.position[axis].high == b.position[axis].high)
    {
      result.set_position(axis, a.position[axis]);
    }
    else if (equal_weights) {
      result.set_position(axis,
                          a.position[axis] * (Interval(1) - alpha) + b.position[axis] * alpha);
    }
  }
  return result;
}

/** A positive rational basis is a convex combination. Bound position and relative-weight
 * perturbations uniformly, using radius about one coefficient to avoid world-origin inflation.
 */
static double coefficient_error(const Span<Coefficient> points)
{
  if (points.is_empty()) {
    return INFINITY;
  }
  double point_error = 0, relative_weight_error = 0, extent = 0;
  const double3 anchor(points[0].value.position);
  for (const auto &point : points) {
    if (!(point.homogeneous[3].low > 0) || !(point.value.weight > 0)) {
      return INFINITY;
    }
    double position_error = 0, distance = 0;
    for (int axis = 0; axis < 3; axis++) {
      position_error = detail::add_upper(position_error,
                                         radius(point.position[axis], point.value.position[axis]));
      const Interval delta = Interval(point.value.position[axis]) - Interval(anchor[axis]);
      if (!delta.valid()) {
        return INFINITY;
      }
      distance = detail::add_upper(distance, std::max(std::abs(delta.low), std::abs(delta.high)));
    }
    point_error = std::max(point_error, position_error);
    extent = std::max(extent, distance);
    const Interval weight_error = Interval(radius(point.homogeneous[3], point.value.weight)) /
                                  Interval(point.homogeneous[3].low);
    if (!weight_error.valid()) {
      return INFINITY;
    }
    relative_weight_error = std::max(relative_weight_error, weight_error.high);
  }
  if (relative_weight_error >= 1) {
    return INFINITY;
  }
  const Interval weight_effect = Interval(2) * Interval(relative_weight_error) /
                                 (Interval(1) - Interval(relative_weight_error)) *
                                 Interval(extent);
  if (!weight_effect.valid()) {
    return INFINITY;
  }
  return detail::add_upper(point_error, weight_effect.high);
}

struct CurveData {
  int order;
  Vector<double> knots;
  Vector<Coefficient> points;
  explicit CurveData(const NurbsCurveGeometry &source)
      : order(source.order), knots(Span<double>(source.knots, source.knots_num))
  {
    for (int i = 0; i < source.points_num; i++) {
      points.append(Coefficient(source.points[i]));
    }
  }
  CurveData(const int order, Span<double> knots, Span<Coefficient> points)
      : order(order), knots(knots), points(points)
  {
  }
  int span(const double parameter) const
  {
    if (parameter == knots[points.size()]) {
      return int(points.size()) - 1;
    }
    return int(std::upper_bound(
                   knots.begin() + order - 1, knots.begin() + points.size() + 1, parameter) -
               knots.begin()) -
           1;
  }
  int multiplicity(const double parameter) const
  {
    return int(std::count(knots.begin(), knots.end(), parameter));
  }
  bool insert(const double parameter)
  {
    const int p = order - 1, k = span(parameter), s = multiplicity(parameter);
    if (s >= order) {
      return true;
    }
    if (k < p || k >= points.size()) {
      return false;
    }
    Vector<Coefficient> output;
    output.reserve(points.size() + 1);
    for (int i = 0; i <= k - p; i++) {
      output.append(points[i]);
    }
    for (int i = k - p + 1; i <= k - s; i++) {
      const Interval alpha = (Interval(parameter) - Interval(knots[i])) /
                             (Interval(knots[i + p]) - Interval(knots[i]));
      if (!alpha.valid()) {
        return false;
      }
      output.append(blend(points[i - 1], points[i], alpha));
    }
    for (int i = k - s; i < points.size(); i++) {
      output.append(points[i]);
    }
    if (output.size() != points.size() + 1) {
      return false;
    }
    knots.insert(k + 1, parameter);
    points = std::move(output);
    return true;
  }
  bool restrict_to(const double2 &range)
  {
    if (!std::isfinite(range.x) || !std::isfinite(range.y) || range.x >= range.y ||
        range.x < knots[order - 1] || range.y > knots[points.size()])
    {
      return false;
    }
    for (const double parameter : {range.x, range.y}) {
      while (multiplicity(parameter) < order) {
        if (!insert(parameter)) {
          return false;
        }
      }
    }
    const int first = int(std::lower_bound(knots.begin(), knots.end(), range.x) - knots.begin());
    const int last = int(std::upper_bound(knots.begin(), knots.end(), range.y) - knots.begin());
    const int count = last - first - order;
    if (count < order || first + count > points.size()) {
      return false;
    }
    Vector<Coefficient> cropped(points.as_span().slice(first, count));
    Vector<double> cropped_knots(knots.as_span().slice(first, last - first));
    points = std::move(cropped);
    knots = std::move(cropped_knots);
    return true;
  }
  OwnedCurve store(const int dimension, const int64_t id = 0) const
  {
    OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
    curve->id = id;
    curve->dimension = dimension;
    curve->order = order;
    curve->points_num = int(points.size());
    curve->knots_num = int(knots.size());
    curve->points = MEM_new_array<NurbsControlPoint>(points.size(), __func__);
    curve->knots = MEM_new_array<double>(knots.size(), __func__);
    for (const int i : points.index_range()) {
      curve->points[i] = points[i].value;
    }
    std::copy(knots.begin(), knots.end(), curve->knots);
    return curve;
  }
};

static std::optional<std::pair<int, Vector<Interval>>> polynomial_basis(const int order,
                                                                        const Span<double> knots,
                                                                        const int points_num,
                                                                        const double parameter)
{
  if (!std::isfinite(parameter) || parameter < knots[order - 1] || parameter > knots[points_num]) {
    return std::nullopt;
  }
  const int span = parameter == knots[points_num] ?
                       points_num - 1 :
                       int(std::upper_bound(knots.begin() + order - 1,
                                            knots.begin() + points_num + 1,
                                            parameter) -
                           knots.begin()) -
                           1;
  Vector<Interval> values(order, Interval(0)), left(order), right(order);
  values[0] = Interval(1);
  for (int j = 1; j < order; j++) {
    left[j] = Interval(parameter) - Interval(knots[span + 1 - j]);
    right[j] = Interval(knots[span + j]) - Interval(parameter);
    Interval saved(0);
    for (int r = 0; r < j; r++) {
      const Interval divisor = right[r + 1] + left[j - r];
      const Interval term = divisor.low == 0 && divisor.high == 0 ? Interval(0) :
                                                                    values[r] / divisor;
      if (!term.valid()) {
        return std::nullopt;
      }
      values[r] = saved + right[r + 1] * term;
      saved = left[j - r] * term;
    }
    values[j] = saved;
  }
  return std::pair<int, Vector<Interval>>(span - order + 1, std::move(values));
}

static bool budget_valid(const double max_error, std::string *r_error)
{
  if (!std::isfinite(max_error) || max_error < 0 || !detail::supported_arithmetic()) {
    if (r_error) {
      *r_error =
          "NURBS refinement requires a finite error budget and round-to-nearest/subnormal "
          "arithmetic";
    }
    return false;
  }
  return true;
}
static bool accept_error(const double bound, const double budget, std::string *r_error)
{
  if (!std::isfinite(bound) || bound > budget) {
    if (r_error) {
      *r_error = "NURBS refinement construction bound exceeds the requested error budget";
    }
    return false;
  }
  return true;
}

}  // namespace

std::optional<CurveRefinementResult> restrict_curve(const NurbsCurveGeometry &source,
                                                    const double2 &range,
                                                    const double max_error,
                                                    std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!budget_valid(max_error, r_error) || !curve_validate_structure(source, r_error)) {
    return std::nullopt;
  }
  CurveData data(source);
  if (!data.restrict_to(range)) {
    if (r_error) {
      *r_error = "NURBS curve restriction needs a valid interval and regular knot insertion";
    }
    return std::nullopt;
  }
  const double bound = coefficient_error(data.points);
  if (!accept_error(bound, max_error, r_error)) {
    return std::nullopt;
  }
  auto curve = data.store(source.dimension, source.id);
  if (!curve_validate_structure(*curve, r_error)) {
    return std::nullopt;
  }
  return CurveRefinementResult{std::move(curve), bound};
}

std::optional<CurveRefinementResult> surface_isocurve(const NurbsSurfaceGeometry &source,
                                                      const int axis,
                                                      const double parameter,
                                                      const double max_error,
                                                      std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!budget_valid(max_error, r_error) || !surface_validate_structure(source, r_error)) {
    return std::nullopt;
  }
  if (axis != 0 && axis != 1) {
    if (r_error) {
      *r_error = "NURBS isocurve needs a U or V axis";
    }
    return std::nullopt;
  }
  const auto basis = polynomial_basis(axis ? source.order_v : source.order_u,
                                      axis ? Span(source.knots_v, source.knots_v_num) :
                                             Span(source.knots_u, source.knots_u_num),
                                      axis ? source.points_v : source.points_u,
                                      parameter);
  if (!basis) {
    if (r_error) {
      *r_error = "NURBS isocurve parameter/basis is unavailable";
    }
    return std::nullopt;
  }
  Vector<Coefficient> coefficients;
  const int count = axis ? source.points_u : source.points_v;
  for (int varying = 0; varying < count; varying++) {
    if ((axis ? source.order_v : source.order_u) == 2 && basis->second.size() == 2) {
      const int first = basis->first;
      const auto &a = source.points[axis ? first * source.points_u + varying :
                                           varying * source.points_u + first];
      const auto &b = source.points[axis ? (first + 1) * source.points_u + varying :
                                           varying * source.points_u + first + 1];
      coefficients.append(blend(Coefficient(a), Coefficient(b), basis->second[1]));
      continue;
    }
    std::array<Interval, 4> h{};
    std::optional<Coefficient> exact;
    for (const int i : basis->second.index_range()) {
      const int fixed = basis->first + i;
      const Coefficient point(source.points[axis ? fixed * source.points_u + varying :
                                                   varying * source.points_u + fixed]);
      for (int c = 0; c < 4; c++) {
        h[c] = h[c] + point.homogeneous[c] * basis->second[i];
      }
      if (basis->second[i].low == 1 && basis->second[i].high == 1) {
        exact = point;
      }
    }
    coefficients.append(exact ? *exact : Coefficient(h));
  }
  const double bound = coefficient_error(coefficients);
  if (!accept_error(bound, max_error, r_error)) {
    return std::nullopt;
  }
  const Span<double> knots = axis ? Span(source.knots_u, source.knots_u_num) :
                                    Span(source.knots_v, source.knots_v_num);
  CurveData data(axis ? source.order_u : source.order_v, knots, coefficients);
  auto curve = data.store(3);
  if (!curve_validate_structure(*curve, r_error)) {
    return std::nullopt;
  }
  return CurveRefinementResult{std::move(curve), bound};
}

std::optional<SurfaceRefinementResult> restrict_surface(const NurbsSurfaceGeometry &source,
                                                        const double2 &low,
                                                        const double2 &high,
                                                        const double max_error,
                                                        std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!budget_valid(max_error, r_error) || !surface_validate_structure(source, r_error)) {
    return std::nullopt;
  }
  Vector<Coefficient> rows;
  Vector<double> u_knots, v_knots;
  int nu = 0, nv = 0;
  for (int v = 0; v < source.points_v; v++) {
    Vector<Coefficient> points;
    for (int u = 0; u < source.points_u; u++) {
      points.append(Coefficient(source.points[v * source.points_u + u]));
    }
    CurveData row(source.order_u, {source.knots_u, source.knots_u_num}, points);
    if (!row.restrict_to({low.x, high.x})) {
      if (r_error) {
        *r_error = "NURBS support restriction needs a valid U interval";
      }
      return std::nullopt;
    }
    nu = int(row.points.size());
    u_knots = std::move(row.knots);
    rows.extend(row.points);
  }
  Vector<Vector<Coefficient>> columns;
  for (int u = 0; u < nu; u++) {
    Vector<Coefficient> points;
    for (int v = 0; v < source.points_v; v++) {
      points.append(rows[v * nu + u]);
    }
    CurveData column(source.order_v, {source.knots_v, source.knots_v_num}, points);
    if (!column.restrict_to({low.y, high.y})) {
      if (r_error) {
        *r_error = "NURBS support restriction needs a valid V interval";
      }
      return std::nullopt;
    }
    nv = int(column.points.size());
    v_knots = std::move(column.knots);
    columns.append(std::move(column.points));
  }
  Vector<Coefficient> points;
  for (int v = 0; v < nv; v++) {
    for (int u = 0; u < nu; u++) {
      points.append(columns[u][v]);
    }
  }
  const double bound = coefficient_error(points);
  if (!accept_error(bound, max_error, r_error)) {
    return std::nullopt;
  }
  OwnedSurface surface(MEM_new<NurbsSurfaceGeometry>(__func__));
  surface->id = source.id;
  surface->order_u = source.order_u;
  surface->order_v = source.order_v;
  surface->points_u = nu;
  surface->points_v = nv;
  surface->points_num = nu * nv;
  surface->knots_u_num = int(u_knots.size());
  surface->knots_v_num = int(v_knots.size());
  surface->points = MEM_new_array<NurbsControlPoint>(points.size(), __func__);
  surface->knots_u = MEM_new_array<double>(u_knots.size(), __func__);
  surface->knots_v = MEM_new_array<double>(v_knots.size(), __func__);
  for (const int i : points.index_range()) {
    surface->points[i] = points[i].value;
  }
  std::copy(u_knots.begin(), u_knots.end(), surface->knots_u);
  std::copy(v_knots.begin(), v_knots.end(), surface->knots_v);
  if (!surface_validate_structure(*surface, r_error)) {
    return std::nullopt;
  }
  return SurfaceRefinementResult{std::move(surface), bound};
}

std::optional<BoundedCurvePoint> evaluate_curve_bounded(const NurbsCurveGeometry &source,
                                                        const double parameter,
                                                        const double max_error,
                                                        std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!budget_valid(max_error, r_error) || !curve_validate_structure(source, r_error)) {
    return std::nullopt;
  }
  const auto basis = polynomial_basis(
      source.order, {source.knots, source.knots_num}, source.points_num, parameter);
  if (!basis) {
    if (r_error) {
      *r_error = "NURBS point parameter/basis is unavailable";
    }
    return std::nullopt;
  }
  std::array<Interval, 4> h{};
  for (const int i : basis->second.index_range()) {
    const Coefficient point(source.points[basis->first + i]);
    for (int c = 0; c < 4; c++) {
      h[c] = h[c] + point.homogeneous[c] * basis->second[i];
    }
  }
  const Coefficient evaluated(h);
  const double error = coefficient_error({&evaluated, 1});
  if (!accept_error(error, max_error, r_error)) {
    return std::nullopt;
  }
  return BoundedCurvePoint{double3(evaluated.value.position), error};
}

}  // namespace blender::bke::nurbs
