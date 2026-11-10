/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_intersections.hh"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <utility>

namespace blender::solid {
namespace {

bool finite(const Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

std::optional<Vec3> unit(const Vec3 p)
{
  const double size = length(p);
  if (!finite(p) || !std::isfinite(size) || !(size > 0)) {
    return {};
  }
  return p / size;
}

std::optional<Vec3> plane_normal(const Surface &plane)
{
  const auto u = unit(plane.u), v = unit(plane.v);
  return u && v && finite(plane.origin) ? unit(cross(*u, *v)) : std::nullopt;
}

/* Independent polynomial implementation. Products and sums in the sign predicates retain
 * their floating-point remainders, so a small nonzero discriminant is never rounded into
 * tangency. The isolator only partitions scalar parameter space; it does not sample geometry.
 * Unrepresentable products or distinct roots without distinct double parameters are uncertain. */
using Expansion = std::vector<long double>;

bool expansion_add(Expansion &sum, long double value)
{
  Expansion result;
  for (const long double term : sum) {
    const long double next = value + term;
    const long double virtual_term = next - value;
    const long double remainder = (value - (next - virtual_term)) + (term - virtual_term);
    if (!std::isfinite(next)) {
      return false;
    }
    if (remainder != 0) {
      result.push_back(remainder);
    }
    value = next;
  }
  if (value != 0 || result.empty()) {
    result.push_back(value);
  }
  sum = std::move(result);
  return true;
}

bool expansion_scale(Expansion &value, const long double factor)
{
  Expansion result;
  for (const long double term : value) {
    const long double product = term * factor;
    if (!std::isfinite(product) ||
        (term != 0 && factor != 0 &&
         std::abs(product) < std::numeric_limits<long double>::min()))
    {
      return false;
    }
    if (!expansion_add(result, std::fma(term, factor, -product)) ||
        !expansion_add(result, product))
    {
      return false;
    }
  }
  value = std::move(result);
  return true;
}

std::optional<Expansion> sum_products(
    const std::initializer_list<std::initializer_list<long double>> terms)
{
  Expansion sum;
  for (const auto &factors : terms) {
    Expansion product{1};
    for (const long double factor : factors) {
      if (!expansion_scale(product, factor)) {
        return {};
      }
    }
    for (const long double term : product) {
      if (!expansion_add(sum, term)) {
        return {};
      }
    }
  }
  return sum;
}

int expansion_sign(const Expansion &value)
{
  return value.back() > 0 ? 1 : value.back() < 0 ? -1 : 0;
}

long double expansion_estimate(const Expansion &value)
{
  long double result = 0;
  for (const long double term : value) {
    result += term;
  }
  return result;
}

using Polynomial = std::array<long double, 4>;

std::optional<int> polynomial_sign(const Polynomial &p, const long double t)
{
  Expansion value{p[3]};
  for (int i = 2; i >= 0; --i) {
    if (!expansion_scale(value, t) || !expansion_add(value, p[i])) {
      return {};
    }
  }
  return expansion_sign(value);
}

struct PolynomialRoots {
  std::vector<long double> parameters;
  bool tangent = false;
  bool coincident = false;
};

std::optional<PolynomialRoots> polynomial_roots(const Polynomial &p,
                                              const long double low,
                                              const long double high)
{
  for (const long double coefficient : p) {
    if (!std::isfinite(coefficient)) {
      return {};
    }
  }
  PolynomialRoots result;
  const auto first_sign = polynomial_sign(p, low), last_sign = polynomial_sign(p, high);
  if (!first_sign || !last_sign) {
    return {};
  }
  auto append = [&](long double t, const bool tangent = false) {
    /* Retain an exactly represented trim root even if division rounded its
     * computed parameter one long-double step outside the interval. */
    if (*first_sign == 0 && double(t) == low) {
      t = low;
    }
    else if (*last_sign == 0 && double(t) == high) {
      t = high;
    }
    if (std::isfinite(t) && t >= low && t <= high) {
      result.parameters.push_back(t);
      result.tangent |= tangent;
    }
  };
  if (p[3] == 0) {
    if (p[2] == 0) {
      if (p[1] == 0) {
        result.coincident = p[0] == 0;
      }
      else {
        append(-p[0] / p[1]);
      }
      return result;
    }
    const auto discriminant = sum_products({{p[1], p[1]}, {-4, p[2], p[0]}});
    if (!discriminant) {
      return {};
    }
    const int sign = expansion_sign(*discriminant);
    if (sign == 0) {
      append(-p[1] / (2 * p[2]), true);
    }
    else if (sign > 0) {
      const long double square_root = std::sqrt(expansion_estimate(*discriminant));
      const long double q = -.5L * (p[1] + std::copysign(square_root, p[1]));
      append(q / p[2]);
      append(p[0] / q);
    }
    return result;
  }

  const long double a = p[3], b = p[2], c = p[1], d = p[0];
  const auto discriminant = sum_products({{18, a, b, c, d},
                                         {-4, b, b, b, d},
                                         {b, b, c, c},
                                         {-4, a, c, c, c},
                                         {-27, a, a, d, d}});
  if (!discriminant) {
    return {};
  }
  const int discriminant_sign = expansion_sign(*discriminant);
  if (discriminant_sign == 0) {
    const auto delta = sum_products({{b, b}, {-3, a, c}});
    const auto numerator = sum_products({{9, a, d}, {-b, c}});
    if (!delta || !numerator) {
      return {};
    }
    if (expansion_sign(*delta) == 0) {
      append(-b / (3 * a), true);
    }
    else {
      const long double repeated = expansion_estimate(*numerator) /
                                   (2 * expansion_estimate(*delta));
      append(repeated, true);
      append(-b / a - 2 * repeated);
    }
    return result;
  }

  auto critical = polynomial_roots({c, 2 * b, 3 * a, 0},
                                   -std::numeric_limits<double>::max(),
                                   std::numeric_limits<double>::max());
  if (!critical) {
    return {};
  }
  std::sort(critical->parameters.begin(), critical->parameters.end());
  if (critical->parameters.size() == 2) {
    const auto first = polynomial_sign(p, critical->parameters[0]);
    const auto last = polynomial_sign(p, critical->parameters[1]);
    if (!first || !last || *first == 0 || *last == 0 ||
        (discriminant_sign > 0 &&
         (critical->parameters[0] == critical->parameters[1] ||
          *first != (a > 0 ? 1 : -1) || *last != (a > 0 ? -1 : 1))) ||
        (discriminant_sign < 0 && *first != *last))
    {
      return {};
    }
  }
  std::vector<long double> bounds{low, high};
  for (const long double t : critical->parameters) {
    if (t > low && t < high) {
      bounds.push_back(t);
    }
  }
  std::sort(bounds.begin(), bounds.end());
  for (size_t i = 0; i < bounds.size(); ++i) {
    const auto sign = polynomial_sign(p, bounds[i]);
    if (!sign || (*sign == 0 && i != 0 && i + 1 != bounds.size())) {
      /* A simple-root cubic cannot vanish at its derivative root. Its stationary
       * parameter has insufficient precision to separate these intersections. */
      return {};
    }
    if (*sign == 0) {
      append(bounds[i]);
    }
    if (i == 0) {
      continue;
    }
    const auto previous_sign = polynomial_sign(p, bounds[i - 1]);
    if (!previous_sign) {
      return {};
    }
    if (*sign == 0 || *previous_sign == 0 || *sign == *previous_sign) {
      continue;
    }
    long double left = bounds[i - 1], right = bounds[i];
    const int left_sign = *previous_sign;
    /* A finite double trim can span over two thousand binary exponents. */
    for (int iteration = 0; iteration < 2304; ++iteration) {
      const long double middle = left / 2 + right / 2;
      if (middle == left || middle == right) {
        break;
      }
      const auto middle_sign = polynomial_sign(p, middle);
      if (!middle_sign) {
        return {};
      }
      if (*middle_sign == 0) {
        left = right = middle;
        break;
      }
      if (*middle_sign == left_sign) {
        left = middle;
      }
      else {
        right = middle;
      }
    }
    append(left / 2 + right / 2);
  }
  return result;
}

struct CylinderSupport {
  Vec3 origin, u, v, axis;
};

std::optional<CylinderSupport> cylinder_support(const Model &model, const Surface &surface)
{
  CylinderSupport result;
  if (surface.kind == SurfaceKind::Cylinder) {
    result = {surface.origin, surface.u, surface.v, surface.w};
  }
  else if (surface.kind == SurfaceKind::Extrusion) {
    const Curve *profile = curve(model, surface.curve);
    if (!profile || profile->kind != CurveKind::Conic) {
      return {};
    }
    result = {profile->origin, profile->u, profile->v, surface.u};
  }
  else {
    return {};
  }
  const auto u = unit(result.u), v = unit(result.v), w = unit(result.axis);
  if (!finite(result.origin) || !u || !v || !w || dot(*u, cross(*v, *w)) == 0) {
    return {};
  }
  return result;
}

SurfaceIntersection uncertain()
{
  return {SurfaceIntersectionState::Uncertain, {}, false};
}

SurfaceIntersection planes(const Surface &a,
                           const Vec3 an,
                           const Surface &b,
                           const Vec3 bn,
                           const IntersectionTolerance tolerance)
{
  const Vec3 direction = cross(an, bn);
  const double sine = length(direction);
  const double offset = dot(bn, b.origin - a.origin);
  if (sine == 0) {
    if (offset == 0) {
      return {SurfaceIntersectionState::Coincident, {}, false};
    }
    return std::abs(offset) <= tolerance.distance ? uncertain() : SurfaceIntersection{};
  }
  if (sine <= tolerance.angle) {
    return uncertain();
  }
  const Vec3 point = a.origin + cross(direction / sine, an) * (offset / sine);
  if (!finite(point)) {
    return uncertain();
  }
  return {SurfaceIntersectionState::Curves,
          {{0, CurveKind::Line, point, direction / sine}},
          false};
}

SurfaceIntersection plane_cylinder(const Surface &plane,
                                   const Vec3 normal,
                                   const CylinderSupport &cylinder,
                                   const IntersectionTolerance tolerance)
{
  /* Substitute the exact swept conic into the plane equation:
   * a*cos(t) + b*sin(t) + c*z + d = 0. */
  const double a = dot(normal, cylinder.u), b = dot(normal, cylinder.v);
  const double c = dot(normal, cylinder.axis);
  const double d = dot(normal, cylinder.origin - plane.origin);
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d)) {
    return uncertain();
  }
  if (c != 0) {
    if (std::abs(c) <= tolerance.angle * length(cylinder.axis)) {
      return uncertain();
    }
    Curve intersection{0,
                       CurveKind::Conic,
                       cylinder.origin - cylinder.axis * (d / c),
                       cylinder.u - cylinder.axis * (a / c),
                       cylinder.v - cylinder.axis * (b / c)};
    if (!finite(intersection.origin) || !finite(intersection.u) || !finite(intersection.v)) {
      return uncertain();
    }
    return {SurfaceIntersectionState::Curves, {intersection}, false};
  }

  const double radius = std::hypot(a, b);
  if (!(radius > 0) || !std::isfinite(radius)) {
    return uncertain();
  }
  const double gap = std::abs(d) - radius;
  /* A near tangency is not permission to move either input surface or drop a
   * thin region. Exact tangency has one generator; an unresolved nearby case
   * is reported to the topology stage without inventing a curve. */
  if (gap != 0 && std::abs(gap) <= tolerance.distance) {
    return uncertain();
  }
  if (gap > 0) {
    return {};
  }
  const double phase = std::atan2(b, a);
  const double delta = std::acos(std::clamp(-d / radius, -1.0, 1.0));
  SurfaceIntersection result;
  result.state = SurfaceIntersectionState::Curves;
  result.tangent = gap == 0;
  for (const double t : {phase + delta, phase - delta}) {
    const Vec3 origin = cylinder.origin + cylinder.u * std::cos(t) +
                        cylinder.v * std::sin(t);
    result.curves.push_back({0, CurveKind::Line, origin, cylinder.axis});
    if (result.tangent) {
      break;
    }
  }
  return result;
}

}  // namespace

std::optional<EdgePlaneIntersection> intersect_edge_plane(
    const Model &model,
    const Edge &edge,
    const Surface &plane,
    const IntersectionTolerance tolerance,
    std::string *error)
{
  auto fail = [&](const char *message) -> std::optional<EdgePlaneIntersection> {
    if (error) {
      *error = message;
    }
    return {};
  };
  const Curve *support = curve(model, edge.curve);
  const auto normal = plane_normal(plane);
  if (plane.kind != SurfaceKind::Plane || !normal || !support ||
      !finite(support->origin) || !finite(support->u) || !finite(support->v) ||
      !std::isfinite(edge.start) || !std::isfinite(edge.end) || edge.start == edge.end ||
      !std::isfinite(tolerance.distance) || tolerance.distance < 0 ||
      !std::isfinite(tolerance.angle) || tolerance.angle < 0 || tolerance.angle >= 1)
  {
    return fail("Edge/plane intersection received invalid geometry or tolerance");
  }
  const double a = dot(*normal, support->u);
  const double b = dot(*normal, support->v);
  const double d = dot(*normal, support->origin - plane.origin);
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(d)) {
    return fail("Edge/plane intersection overflowed its coordinate range");
  }
  EdgePlaneIntersection result;
  const double low = std::min(edge.start, edge.end);
  const double high = std::max(edge.start, edge.end);
  auto append = [&](const double t) {
    if (std::isfinite(t) && t >= low && t <= high) {
      result.parameters.push_back(t);
    }
  };
  if (support->kind == CurveKind::Line) {
    if (!(length(support->u) > 0)) {
      return fail("Edge/plane intersection received a degenerate line");
    }
    if (a == 0) {
      result.state = d == 0 ? EdgePlaneState::Coincident :
                     std::abs(d) <= tolerance.distance ? EdgePlaneState::Uncertain :
                                                        EdgePlaneState::Separated;
      return result;
    }
    if (std::abs(a) <= tolerance.angle * length(support->u)) {
      // As in IntAna_IntConicQuad's bounded-line check, angular proximity
      // alone cannot decide whether a long, nearly parallel edge crosses.
      const double first_distance = a * low + d;
      const double last_distance = a * high + d;
      if (!std::isfinite(first_distance) || !std::isfinite(last_distance)) {
        return fail("Edge/plane intersection overflowed its bounded line range");
      }
      if ((first_distance > tolerance.distance && last_distance > tolerance.distance) ||
          (first_distance < -tolerance.distance && last_distance < -tolerance.distance))
      {
        return result;
      }
      if (std::abs(a * (high - low)) <= tolerance.distance) {
        return EdgePlaneIntersection{EdgePlaneState::Uncertain, {}, false};
      }
    }
    append(-d / a);
  }
  else if (support->kind == CurveKind::Conic) {
    if (!unit(cross(support->u, support->v))) {
      return fail("Edge/plane intersection received a degenerate conic");
    }
    const double radius = std::hypot(a, b);
    if (!std::isfinite(radius)) {
      return fail("Edge/plane intersection overflowed its conic coefficients");
    }
    if (radius == 0) {
      result.state = d == 0 ? EdgePlaneState::Coincident :
                     std::abs(d) <= tolerance.distance ? EdgePlaneState::Uncertain :
                                                        EdgePlaneState::Separated;
      return result;
    }
    const double gap = std::abs(d) - radius;
    if (gap != 0 && std::abs(gap) <= tolerance.distance) {
      return EdgePlaneIntersection{EdgePlaneState::Uncertain, {}, false};
    }
    if (gap > 0) {
      return result;
    }
    result.tangent = gap == 0;
    // Preserve the native affine ellipse. Only solve its scalar plane equation:
    // a*cos(t) + b*sin(t) + d = 0; no circular approximation is needed.
    const double phase = std::atan2(b, a);
    const double delta = std::acos(std::clamp(-d / radius, -1.0, 1.0));
    constexpr double period = 2 * std::numbers::pi;
    if (std::max(std::abs(low), std::abs(high)) > 1e12 || high - low > period * 100000) {
      return fail("Conic trim exceeds the supported periodic parameter range");
    }
    const int count = result.tangent ? 1 : 2;
    for (int i = 0; i < count; ++i) {
      const double root = phase + (i == 0 ? delta : -delta);
      const double first = std::ceil((low - root) / period);
      const double last = std::floor((high - root) / period);
      for (double turn = first; turn <= last; ++turn) {
        append(root + turn * period);
      }
    }
  }
  else if (support->kind == CurveKind::Cubic ||
           support->kind == CurveKind::RationalQuadratic)
  {
    if (!finite(support->w)) {
      return fail("Edge/plane intersection received nonfinite polynomial data");
    }
    Polynomial numerator;
    if (support->kind == CurveKind::Cubic) {
      if (support->u == Vec3{} && support->v == Vec3{} && support->w == Vec3{}) {
        return fail("Edge/plane intersection received a degenerate cubic");
      }
      const double c = dot(*normal, support->w);
      if (!std::isfinite(c)) {
        return fail("Edge/plane intersection overflowed its cubic coefficients");
      }
      numerator = {d, a, b, c};
    }
    else {
      if (!(support->w.x > 0) || !(support->w.y > 0) || !(support->w.z > 0) ||
          support->origin == support->v)
      {
        return fail("Edge/plane intersection received an invalid rational quadratic");
      }
      const long double w0 = support->w.x, w1 = support->w.y, w2 = support->w.z;
      const Polynomial denominator{w0, 2 * (w1 - w0), w0 - 2 * w1 + w2, 0};
      const auto poles = polynomial_roots(denominator, low, high);
      if (!poles) {
        return EdgePlaneIntersection{EdgePlaneState::Uncertain, {}, false};
      }
      if (poles->coincident || !poles->parameters.empty()) {
        return fail("Rational quadratic has a denominator pole in its edge trim");
      }
      const double d1 = dot(*normal, support->u - plane.origin);
      const double d2 = dot(*normal, support->v - plane.origin);
      if (!std::isfinite(d1) || !std::isfinite(d2)) {
        return fail("Edge/plane intersection overflowed its rational coefficients");
      }
      /* Homogeneous Bernstein numerator. Its zeros are exactly the plane
       * intersections wherever the separately checked denominator is nonzero. */
      const long double h0 = w0 * d, h1 = w1 * d1, h2 = w2 * d2;
      numerator = {h0, 2 * (h1 - h0), h0 - 2 * h1 + h2, 0};
    }
    const auto roots = polynomial_roots(numerator, low, high);
    if (!roots) {
      return EdgePlaneIntersection{EdgePlaneState::Uncertain, {}, false};
    }
    if (roots->coincident) {
      result.state = EdgePlaneState::Coincident;
      return result;
    }
    result.tangent = roots->tangent;
    for (const long double root : roots->parameters) {
      const double parameter = double(root);
      if (!std::isfinite(parameter) || parameter < low || parameter > high ||
          (parameter == 0 && root != 0) ||
          ((parameter == low || parameter == high) &&
           polynomial_sign(numerator, parameter) != std::optional<int>(0)) ||
          std::find(result.parameters.begin(), result.parameters.end(), parameter) !=
              result.parameters.end())
      {
        /* Do not merge distinct roots, or snap an interior root to a trim endpoint. */
        return EdgePlaneIntersection{EdgePlaneState::Uncertain, {}, false};
      }
      if (!finite(evaluate(*support, parameter))) {
        return fail("Edge/plane intersection overflowed its evaluated polynomial point");
      }
      append(parameter);
    }
  }
  else {
    return fail("This native curve type needs an edge/plane intersection implementation");
  }
  std::sort(result.parameters.begin(), result.parameters.end());
  result.parameters.erase(std::unique(result.parameters.begin(), result.parameters.end()),
                          result.parameters.end());
  if (edge.start > edge.end) {
    std::reverse(result.parameters.begin(), result.parameters.end());
  }
  if (!result.parameters.empty()) {
    result.state = EdgePlaneState::Points;
  }
  return result;
}

std::optional<SurfaceIntersection> intersect_surfaces(const Model &model,
                                                     const Surface &first,
                                                     const Surface &second,
                                                     const IntersectionTolerance tolerance,
                                                     std::string *error)
{
  auto fail = [&](const char *message) -> std::optional<SurfaceIntersection> {
    if (error) {
      *error = message;
    }
    return {};
  };
  if (!std::isfinite(tolerance.distance) || tolerance.distance < 0 ||
      !std::isfinite(tolerance.angle) || tolerance.angle < 0 || tolerance.angle >= 1)
  {
    return fail("Surface intersection requires finite nonnegative tolerances");
  }
  const Surface *plane = first.kind == SurfaceKind::Plane ? &first :
                         second.kind == SurfaceKind::Plane ? &second : nullptr;
  if (!plane) {
    return fail("This surface pair needs a native intersection implementation");
  }
  const auto normal = plane_normal(*plane);
  if (!normal) {
    return fail("Surface intersection received an invalid plane");
  }
  const Surface &other = plane == &first ? second : first;
  if (other.kind == SurfaceKind::Plane) {
    const auto other_normal = plane_normal(other);
    if (!other_normal) {
      return fail("Surface intersection received an invalid plane");
    }
    return planes(*plane, *normal, other, *other_normal, tolerance);
  }
  const auto cylinder = cylinder_support(model, other);
  if (!cylinder) {
    return fail("This surface pair needs a native intersection implementation");
  }
  return plane_cylinder(*plane, *normal, *cylinder, tolerance);
}

}  // namespace blender::solid
