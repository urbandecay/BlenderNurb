/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <array>
#include <limits>
#include <numeric>

#include "BLI_math_vector.hh"

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
  if (a.low == 0 && a.high == 0) {
    return b;
  }
  if (b.low == 0 && b.high == 0) {
    return a;
  }
  double lo, hi, elo, ehi;
  if (!detail::sum_with_error(a.low, b.low, lo, elo)) {
    return Interval::invalid();
  }
  if (a.low == a.high && b.low == b.high) {
    hi = lo;
    ehi = elo;
  }
  else if (!detail::sum_with_error(a.high, b.high, hi, ehi)) {
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
  if ((a.low == 0 && a.high == 0) || (b.low == 0 && b.high == 0)) {
    return Interval(0);
  }
  if (a.low == 1 && a.high == 1) {
    return b;
  }
  if (b.low == 1 && b.high == 1) {
    return a;
  }
  auto product = [](const double x, const double y) {
    double value, error;
    if (!detail::product_with_error(x, y, value, error)) {
      return Interval::invalid();
    }
    return Interval(error ? std::nextafter(value, -INFINITY) : value,
                    error ? std::nextafter(value, INFINITY) : value);
  };
  /* Multiplication is monotone within each sign quadrant. Only the two extremal
   * corners matter, except when both intervals cross zero. Keep the same outward
   * endpoint enclosures without evaluating the other corners. */
  double lx, ly, hx, hy;
  if (a.low >= 0) {
    lx = b.low >= 0 ? a.low : a.high;
    ly = b.low;
    hx = b.high <= 0 ? a.low : a.high;
    hy = b.high;
  }
  else if (a.high <= 0) {
    lx = b.high <= 0 ? a.high : a.low;
    ly = b.high;
    hx = b.low >= 0 ? a.high : a.low;
    hy = b.low;
  }
  else if (b.low >= 0) {
    lx = a.low;
    hx = a.high;
    ly = hy = b.high;
  }
  else if (b.high <= 0) {
    lx = a.high;
    hx = a.low;
    ly = hy = b.low;
  }
  else {
    const auto low_a = product(a.low, b.high), low_b = product(a.high, b.low);
    const auto high_a = product(a.low, b.low), high_b = product(a.high, b.high);
    if (!low_a.valid() || !low_b.valid() || !high_a.valid() || !high_b.valid()) {
      return Interval::invalid();
    }
    return {std::min(low_a.low, low_b.low), std::max(high_a.high, high_b.high)};
  }
  const auto low = product(lx, ly);
  const auto high = lx == hx && ly == hy ? low : product(hx, hy);
  return low.valid() && high.valid() ? Interval(low.low, high.high) : Interval::invalid();
}
static Interval operator/(const Interval a, const Interval b)
{
  if (!a.valid() || !b.valid() || (b.low <= 0 && b.high >= 0)) {
    return Interval::invalid();
  }
  if (a.low == 0 && a.high == 0) {
    return Interval(0);
  }
  if (b.low == 1 && b.high == 1) {
    return a;
  }
  if (b.high < 0) {
    return Interval(-a.high, -a.low) / Interval(-b.high, -b.low);
  }
  auto quotient = [](const double x, const double y) {
    const double value = x / y;
    double product, error;
    if (!std::isfinite(value) || !detail::product_with_error(value, y, product, error)) {
      return Interval::invalid();
    }
    const bool exact = error == 0 && product == x;
    return Interval(exact ? value : std::nextafter(value, -INFINITY),
                    exact ? value : std::nextafter(value, INFINITY));
  };
  const double low_divisor = a.low >= 0 ? b.high : b.low;
  const double high_divisor = a.high <= 0 ? b.high : b.low;
  const auto low = quotient(a.low, low_divisor);
  const auto high = a.low == a.high && low_divisor == high_divisor ?
                        low :
                        quotient(a.high, high_divisor);
  return low.valid() && high.valid() ? Interval(low.low, high.high) : Interval::invalid();
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
  if (x.low == value && x.high == value) {
    return 0;
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
  const Interval beta = Interval(1) - alpha;
  const bool equal_weights = a.homogeneous[3].low == a.homogeneous[3].high &&
                             a.homogeneous[3].low == b.homogeneous[3].low &&
                             a.homogeneous[3].high == b.homogeneous[3].high;
  if (equal_weights) {
    /* Equal weights cancel analytically. The general homogeneous blend below previously did
     * all four components and three divisions, then overwrote them with these same values. */
    Coefficient result(a);
    for (int axis = 0; axis < 3; axis++) {
      const bool constant = a.position[axis].low == a.position[axis].high &&
                            a.position[axis].low == b.position[axis].low &&
                            a.position[axis].high == b.position[axis].high;
      if (!constant) {
        result.set_position(axis, a.position[axis] * beta + b.position[axis] * alpha);
      }
    }
    return result;
  }
  std::array<Interval, 4> h;
  for (int i = 0; i < 4; i++) {
    h[i] = a.homogeneous[i] * beta + b.homogeneous[i] * alpha;
  }
  Coefficient result(h);
  /* Partition of unity preserves equal weights and constant Euclidean coordinates.
   * Keep those identities instead of losing their correlation in interval sums. */
  for (int axis = 0; axis < 3; axis++) {
    if (a.position[axis].low == a.position[axis].high &&
        a.position[axis].low == b.position[axis].low &&
        a.position[axis].high == b.position[axis].high)
    {
      result.set_position(axis, a.position[axis]);
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

/** Knot insertion depends on the knot vector and requested interval, independently of
 * the coefficient row. A tensor restriction uses one plan per axis for every row/column. */
struct RestrictionPlan {
  struct Insertion {
    int prefix, suffix;
    Vector<Interval, 12> weights;
  };
  Vector<Insertion> insertions;
  Vector<double> knots;
  int first = 0, count = 0;

  static std::optional<RestrictionPlan> create(const int order,
                                               const Span<double> source_knots,
                                               const int source_points,
                                               const double2 range)
  {
    if (!std::isfinite(range.x) || !std::isfinite(range.y) || range.x >= range.y ||
        range.x < source_knots[order - 1] || range.y > source_knots[source_points])
    {
      return std::nullopt;
    }
    RestrictionPlan plan;
    plan.knots = source_knots;
    int points = source_points;
    const int degree = order - 1;
    for (const double parameter : {range.x, range.y}) {
      int multiplicity = int(std::count(plan.knots.begin(), plan.knots.end(), parameter));
      while (multiplicity < order) {
        /* Insert after the full existing knot run, including an unclamped upper
         * domain endpoint. Using its left evaluation span would discard the last
         * active coefficient before the endpoint has been clamped. */
        const int span = int(std::upper_bound(plan.knots.begin(), plan.knots.end(), parameter) -
                             plan.knots.begin()) -
                         1;
        Insertion insertion{span - degree + 1, span - multiplicity, {}};
        if (span < degree || insertion.prefix > points || insertion.suffix < 0 ||
            insertion.suffix >= points)
        {
          return std::nullopt;
        }
        for (int i = insertion.prefix; i <= insertion.suffix; i++) {
          const Interval alpha = (Interval(parameter) - Interval(plan.knots[i])) /
                                 (Interval(plan.knots[i + degree]) - Interval(plan.knots[i]));
          if (!alpha.valid()) {
            return std::nullopt;
          }
          insertion.weights.append(alpha);
        }
        plan.insertions.append(std::move(insertion));
        plan.knots.insert(span + 1, parameter);
        points++;
        multiplicity++;
      }
    }
    plan.first = int(std::lower_bound(plan.knots.begin(), plan.knots.end(), range.x) -
                     plan.knots.begin());
    const int last = int(std::upper_bound(plan.knots.begin(), plan.knots.end(), range.y) -
                         plan.knots.begin());
    plan.count = last - plan.first - order;
    if (plan.count < order || plan.first + plan.count > points) {
      return std::nullopt;
    }
    if (plan.first != 0 || last != plan.knots.size()) {
      plan.knots = Vector<double>(plan.knots.as_span().slice(plan.first, last - plan.first));
    }
    return plan;
  }

  void apply(Vector<Coefficient> &points, Vector<Coefficient> &output) const
  {
    if (!insertions.is_empty()) {
      const int capacity = int(points.size() + insertions.size());
      points.reserve(capacity);
      output.reserve(capacity);
      for (const auto &insertion : insertions) {
        output.clear();
        output.extend(points.as_span().take_front(insertion.prefix));
        for (const int i : insertion.weights.index_range()) {
          const int point = insertion.prefix + i;
          output.append(blend(points[point - 1], points[point], insertion.weights[i]));
        }
        output.extend(points.as_span().drop_front(insertion.suffix));
        std::swap(points, output);
      }
    }
    if (first != 0) {
      for (const int i : IndexRange(count)) {
        points[i] = points[first + i];
      }
    }
    while (points.size() > count) {
      points.remove_last();
    }
  }
};

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
  bool restrict_to(const double2 &range)
  {
    auto plan = RestrictionPlan::create(order, knots, points.size(), range);
    if (!plan) {
      return false;
    }
    Vector<Coefficient> scratch;
    plan->apply(points, scratch);
    knots = std::move(plan->knots);
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
  const auto u_plan = RestrictionPlan::create(
      source.order_u, {source.knots_u, source.knots_u_num}, source.points_u, {low.x, high.x});
  const auto v_plan = RestrictionPlan::create(
      source.order_v, {source.knots_v, source.knots_v_num}, source.points_v, {low.y, high.y});
  if (!u_plan || !v_plan) {
    if (r_error) {
      *r_error = !u_plan ? "NURBS support restriction needs a valid U interval" :
                           "NURBS support restriction needs a valid V interval";
    }
    return std::nullopt;
  }
  const int nu = u_plan->count, nv = v_plan->count;
  const auto &u_knots = u_plan->knots, &v_knots = v_plan->knots;
  Vector<Coefficient> rows, columns, points, scratch;
  rows.reserve(nu * source.points_v);
  columns.reserve(nu * nv);
  for (int v = 0; v < source.points_v; v++) {
    points.clear();
    for (int u = 0; u < source.points_u; u++) {
      points.append(Coefficient(source.points[v * source.points_u + u]));
    }
    u_plan->apply(points, scratch);
    rows.extend(points);
  }
  for (int u = 0; u < nu; u++) {
    points.clear();
    for (int v = 0; v < source.points_v; v++) {
      points.append(rows[v * nu + u]);
    }
    v_plan->apply(points, scratch);
    columns.extend(points);
  }
  /* Bounds are maxima over the coefficients with the same first anchor. They do
   * not require a row-major copy; transpose only the final stored point records. */
  const double bound = coefficient_error(columns);
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
  surface->points = MEM_new_array<NurbsControlPoint>(nu * nv, __func__);
  surface->knots_u = MEM_new_array<double>(u_knots.size(), __func__);
  surface->knots_v = MEM_new_array<double>(v_knots.size(), __func__);
  for (int v = 0; v < nv; v++) {
    for (int u = 0; u < nu; u++) {
      surface->points[v * nu + u] = columns[u * nv + v].value;
    }
  }
  std::copy(u_knots.begin(), u_knots.end(), surface->knots_u);
  std::copy(v_knots.begin(), v_knots.end(), surface->knots_v);
  if (!surface_validate_structure(*surface, r_error)) {
    return std::nullopt;
  }
  return SurfaceRefinementResult{std::move(surface), bound};
}

std::optional<CurveRefinementResult> surface_parameter_line(const NurbsSurfaceGeometry &source,
                                                            const double2 start,
                                                            const double2 end,
                                                            const double max_error,
                                                            std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!budget_valid(max_error, r_error) || !surface_validate_structure(source, r_error)) {
    return std::nullopt;
  }
  auto fail = [&](const char *message) -> std::optional<CurveRefinementResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (source.order_u != 3 || source.order_v != 3 || source.points_u != 3 || source.points_v != 3 ||
      source.knots_u_num != 6 || source.knots_v_num != 6 || start == end)
  {
    return fail("NURBS parameter line requires one biquadratic span and distinct UV endpoints");
  }
  for (int axis = 0; axis < 2; axis++) {
    const double *knots = axis ? source.knots_v : source.knots_u;
    for (int i = 0; i < 3; i++) {
      if (knots[i] != knots[2] || knots[i + 3] != knots[3]) {
        return fail("NURBS parameter line requires a clamped Bezier span");
      }
    }
    if (!std::isfinite(start[axis]) || !std::isfinite(end[axis]) ||
        std::min(start[axis], end[axis]) < knots[2] || std::max(start[axis], end[axis]) > knots[3])
    {
      return fail("NURBS parameter line endpoints must lie inside the support domain");
    }
  }
  if (start.x == end.x || start.y == end.y) {
    const int fixed = start.x == end.x ? 0 : 1;
    auto iso = surface_isocurve(source, fixed, start[fixed], max_error, r_error);
    if (!iso) {
      return std::nullopt;
    }
    const double a = start[1 - fixed], b = end[1 - fixed];
    auto restricted = restrict_curve(
        *iso->curve, {std::min(a, b), std::max(a, b)}, max_error, r_error);
    if (!restricted) {
      return std::nullopt;
    }
    restricted->geometry_error_bound = detail::add_upper(iso->geometry_error_bound,
                                                         restricted->geometry_error_bound);
    if (!accept_error(restricted->geometry_error_bound, max_error, r_error)) {
      return std::nullopt;
    }
    auto &curve = *restricted->curve;
    if (a > b) {
      std::reverse(curve.points, curve.points + curve.points_num);
    }
    for (int i = 0; i < curve.knots_num; i++) {
      curve.knots[i] = i < curve.order ? 0 : 1;
    }
    curve.id = 0;
    return restricted;
  }

  /* A Bezier surface restricted to an affine UV line is a quartic rational curve.
   * Restrict its nine homogeneous controls directly with the quadratic blossom.
   * The generic surface restriction allocates, validates and inserts knots for every
   * individual corner edge even though all of them use this same 3x3 support. */
  auto normalized = [&](const int axis, const double parameter) {
    const double *knots = axis ? source.knots_v : source.knots_u;
    return (Interval(parameter) - Interval(knots[2])) / (Interval(knots[3]) - Interval(knots[2]));
  };
  const Interval u0 = normalized(0, start.x), u1 = normalized(0, end.x);
  const Interval v0 = normalized(1, start.y), v1 = normalized(1, end.y);
  if (!u0.valid() || !u1.valid() || !v0.valid() || !v1.valid()) {
    return fail("NURBS parameter line has an unrepresentable support domain");
  }
  using Homogeneous = std::array<Interval, 4>;
  /* The common case admits a cheap conservative roundoff enclosure. All blossom
   * parameters are in [0,1], so each homogeneous intermediate lies in the convex
   * hull of the nine source controls. A quartic coefficient uses at most six
   * levels of affine interpolation. The factor below covers their arithmetic,
   * the source homogeneous products, and uncertainty in the four normalized UV
   * endpoints. If that bound is insufficient, use outward interval arithmetic. */
  auto fast_curve = [&]() -> std::optional<CurveRefinementResult> {
    struct Parameter {
      long double value, uncertainty;
    };
    auto parameter = [](const Interval enclosure) {
      const long double low = enclosure.low, high = enclosure.high;
      const long double value = std::clamp(std::midpoint(low, high), 0.0L, 1.0L);
      return Parameter{value, std::max(std::abs(value - low), std::abs(high - value))};
    };
    const Parameter uv[] = {parameter(u0), parameter(u1), parameter(v0), parameter(v1)};
    long double uncertainty = 0;
    for (const Parameter &coordinate : uv) {
      uncertainty = std::max(uncertainty, coordinate.uncertainty);
    }
    using LongHomogeneous = std::array<long double, 4>;
    std::array<LongHomogeneous, 9> controls;
    long double magnitude = 0;
    for (int i = 0; i < 9; i++) {
      const NurbsControlPoint &point = source.points[i];
      for (int axis = 0; axis < 3; axis++) {
        controls[i][axis] = static_cast<long double>(point.position[axis]) * point.weight;
      }
      controls[i][3] = point.weight;
      for (const long double component : controls[i]) {
        magnitude = std::max(magnitude, std::abs(component));
      }
    }
    auto mix_long =
        [](const LongHomogeneous &a, const LongHomogeneous &b, const long double alpha) {
          LongHomogeneous result;
          for (int axis = 0; axis < 4; axis++) {
            result[axis] = (1 - alpha) * a[axis] + alpha * b[axis];
          }
          return result;
        };
    auto restrict_long =
        [&](const std::array<LongHomogeneous, 3> &row, const long double a, const long double b) {
          const LongHomogeneous a0 = mix_long(row[0], row[1], a);
          const LongHomogeneous a1 = mix_long(row[1], row[2], a);
          const LongHomogeneous b0 = mix_long(row[0], row[1], b);
          const LongHomogeneous b1 = mix_long(row[1], row[2], b);
          return std::array<LongHomogeneous, 3>{
              mix_long(a0, a1, a), mix_long(a0, a1, b), mix_long(b0, b1, b)};
        };
    for (int v = 0; v < 3; v++) {
      const auto row = restrict_long(
          {controls[3 * v], controls[3 * v + 1], controls[3 * v + 2]}, uv[0].value, uv[1].value);
      for (int u = 0; u < 3; u++) {
        controls[3 * v + u] = row[u];
      }
    }
    for (int u = 0; u < 3; u++) {
      const auto column = restrict_long(
          {controls[u], controls[3 + u], controls[6 + u]}, uv[2].value, uv[3].value);
      for (int v = 0; v < 3; v++) {
        controls[3 * v + u] = column[v];
      }
    }
    auto point = [&](const int u, const int v) { return controls[3 * v + u]; };
    const std::array<LongHomogeneous, 5> quartic = {
        point(0, 0),
        mix_long(point(1, 0), point(0, 1), 0.5L),
        mix_long(mix_long(point(2, 0), point(0, 2), 0.5L), point(1, 1), 2.0L / 3.0L),
        mix_long(point(2, 1), point(1, 2), 0.5L),
        point(2, 2),
    };
    const long double radius = std::nextafter(
        2 * magnitude * (1024 * std::numeric_limits<long double>::epsilon() + 64 * uncertainty) +
            1024 * std::numeric_limits<long double>::denorm_min(),
        std::numeric_limits<long double>::infinity());
    if (!std::isfinite(radius)) {
      return std::nullopt;
    }
    Vector<Coefficient> points;
    for (const LongHomogeneous &control : quartic) {
      Homogeneous enclosure;
      for (int axis = 0; axis < 4; axis++) {
        const long double lower = std::nextafter(control[axis] - radius,
                                                 -std::numeric_limits<long double>::infinity());
        const long double upper = std::nextafter(control[axis] + radius,
                                                 std::numeric_limits<long double>::infinity());
        double low = static_cast<double>(lower), high = static_cast<double>(upper);
        if (!std::isfinite(low) || !std::isfinite(high)) {
          return std::nullopt;
        }
        if (static_cast<long double>(low) > lower) {
          low = std::nextafter(low, -INFINITY);
        }
        if (static_cast<long double>(high) < upper) {
          high = std::nextafter(high, INFINITY);
        }
        enclosure[axis] = Interval(low, high);
      }
      points.append(Coefficient(enclosure));
    }
    const double error = coefficient_error(points);
    if (!std::isfinite(error) || error > max_error) {
      return std::nullopt;
    }
    const double knots[] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    CurveData data(5, {knots, 10}, points);
    auto curve = data.store(3);
    std::string validation_error;
    if (!curve_validate_structure(*curve, &validation_error)) {
      return std::nullopt;
    }
    return CurveRefinementResult{std::move(curve), error};
  };
  if (auto fast = fast_curve()) {
    return fast;
  }
  auto patch = restrict_surface(
      source, math::min(start, end), math::max(start, end), max_error, r_error);
  if (!patch) {
    return std::nullopt;
  }
  auto point = [&](const int i, const int j) {
    const int u = start.x < end.x ? i : 2 - i;
    const int v = start.y < end.y ? j : 2 - j;
    return Coefficient(patch->surface->points[3 * v + u]);
  };
  /* Product of two quadratic Bernstein bases, grouped by i + j. The five quartic
   * coefficients have weights 1; (1,1)/2; (1,4,1)/6; (1,1)/2; 1. */
  Vector<Coefficient> points;
  points.append(point(0, 0));
  points.append(blend(point(1, 0), point(0, 1), Interval(0.5)));
  points.append(blend(
      blend(point(2, 0), point(0, 2), Interval(0.5)), point(1, 1), Interval(2) / Interval(3)));
  points.append(blend(point(2, 1), point(1, 2), Interval(0.5)));
  points.append(point(2, 2));
  const double error = detail::add_upper(patch->geometry_error_bound, coefficient_error(points));
  if (!accept_error(error, max_error, r_error)) {
    return std::nullopt;
  }
  const double knots[] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
  CurveData data(5, {knots, 10}, points);
  auto curve = data.store(3);
  if (!curve_validate_structure(*curve, r_error)) {
    return std::nullopt;
  }
  return CurveRefinementResult{std::move(curve), error};
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
  if (source.order == 2 && source.points_num == 2 && source.knots[0] == source.knots[1] &&
      source.knots[2] == source.knots[3] && source.points[0].weight > 0 &&
      source.points[0].weight == source.points[1].weight && std::isfinite(parameter) &&
      parameter >= source.knots[1] && parameter <= source.knots[2])
  {
    /* Canonical trim lines are exactly t in one coordinate and constant in the others.
     * Their equal rational weights cancel. Preserve that identity directly instead of
     * rebuilding interval basis/coefficient data for every partition boundary endpoint. */
    double3 position;
    bool identity = true;
    for (const int axis : IndexRange(3)) {
      const double a = source.points[0].position[axis], b = source.points[1].position[axis];
      if (a == b) {
        position[axis] = a;
      }
      else if (a == source.knots[1] && b == source.knots[2]) {
        position[axis] = parameter;
      }
      else {
        identity = false;
        break;
      }
    }
    if (identity) {
      return BoundedCurvePoint{position, 0};
    }
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
