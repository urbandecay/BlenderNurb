/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_classify.hh"
#include "SOL_intersections.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>

namespace blender::solid {
namespace {

constexpr double period = 2 * std::numbers::pi;
constexpr double roundoff = 64 * std::numeric_limits<double>::epsilon();
using State = PointClassification;

bool finite(Vec3 v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void check_tolerance(ClassificationTolerance tolerance, Vec3 point)
{
  if (!finite(point) || !std::isfinite(tolerance.distance) || tolerance.distance < 0 ||
      !std::isfinite(tolerance.angle) || tolerance.angle < 0 || tolerance.angle >= 1)
    throw std::runtime_error(
        "Native classification needs finite points and nonnegative tolerances");
}

struct Support {
  Surface value;
  Vec3 normal, row_x, row_y, row_z;
  bool cylinder = false;
  bool ill_conditioned = false;

  Vec3 direction(Vec3 p) const
  {
    return {dot(p, row_x), dot(p, row_y), dot(p, row_z)};
  }
  Vec3 local(Vec3 p) const
  {
    return direction(p - value.origin);
  }
};

Support support_for(const Model &model, const Face &face)
{
  const Surface *source = surface(model, face.surface);
  if (!source)
    throw std::runtime_error("Native classification face references a missing surface");
  Support result;
  result.value = *source;
  Surface &s = result.value;
  if (s.kind == SurfaceKind::Extrusion) {
    const Curve *profile = curve(model, s.curve);
    if (!profile || profile->kind != CurveKind::Conic)
      throw std::runtime_error("Native classification only supports conic extrusion surfaces");
    s.origin = profile->origin;
    s.w = s.u;
    s.u = profile->u;
    s.v = profile->v;
    s.kind = SurfaceKind::Cylinder;
  }
  if (!finite(s.origin) || !finite(s.u) || !finite(s.v) || !finite(s.w))
    throw std::runtime_error("Native classification surface has nonfinite coefficients");
  if (s.kind == SurfaceKind::Plane) {
    result.normal = cross(s.u, s.v);
    const double size = length(result.normal);
    if (!(size > 0) || !std::isfinite(size))
      throw std::runtime_error("Native classification found a degenerate plane");
    result.normal = result.normal / size;
    result.ill_conditioned = size <= roundoff * length(s.u) * length(s.v);
  }
  else if (s.kind == SurfaceKind::Cylinder) {
    result.cylinder = true;
    const Vec3 vw = cross(s.v, s.w), wu = cross(s.w, s.u), uv = cross(s.u, s.v);
    const double determinant = dot(s.u, vw);
    if (determinant == 0 || !std::isfinite(determinant))
      throw std::runtime_error("Native classification found a degenerate cylinder frame");
    result.row_x = vw / determinant;
    result.row_y = wu / determinant;
    result.row_z = uv / determinant;
    result.ill_conditioned = std::abs(determinant) <=
                             roundoff * length(s.u) * length(s.v) * length(s.w);
    if (!finite(result.row_x) || !finite(result.row_y) || !finite(result.row_z))
      throw std::runtime_error("Native classification cannot invert the cylinder frame");
  }
  else {
    throw std::runtime_error("Native point classification does not support this surface family");
  }
  auto check_loop = [&](const std::vector<Coedge> &loop) {
    if (loop.empty())
      throw std::runtime_error("Native classification found an empty face loop");
    Id first = 0, previous = 0;
    for (Coedge use : loop) {
      const Edge *e = edge(model, use.edge);
      const Curve *c = e ? curve(model, e->curve) : nullptr;
      if (!e || !c || !vertex(model, e->vertices[0]) || !vertex(model, e->vertices[1]))
        throw std::runtime_error("Native classification found an invalid trim reference");
      if (c->kind != CurveKind::Line && c->kind != CurveKind::Conic)
        throw std::runtime_error("Native point classification requires line/conic face trims");
      if (!finite(c->origin) || !finite(c->u) || !finite(c->v) || !std::isfinite(e->start) ||
          !std::isfinite(e->end) || e->start == e->end || !(length(c->u) > 0) ||
          (c->kind == CurveKind::Conic && !(length(cross(c->u, c->v)) > 0)))
        throw std::runtime_error("Native classification found a degenerate trim curve");
      const Id start = e->vertices[use.reversed ? 1 : 0];
      const Id end = e->vertices[use.reversed ? 0 : 1];
      if (previous && start != previous)
        throw std::runtime_error("Native classification found an unconnected face loop");
      if (!first)
        first = start;
      previous = end;
    }
    if (previous != first)
      throw std::runtime_error("Native classification found an open face loop");
  };
  check_loop(face.outer);
  for (const auto &hole : face.holes)
    check_loop(hole);
  return result;
}

/** The requested tolerance creates an uncertainty band, not a geometric merge rule.
 * Roundoff scales with the actual input arithmetic; there is no unit-size floor. */
double arithmetic_error(Vec3 point, Vec3 origin, double local_scale)
{
  return roundoff * (length(point) + length(origin) + local_scale);
}

State edge_contact(const Curve &c, const Edge &e, Vec3 point, ClassificationTolerance tolerance)
{
  const double low = std::min(e.start, e.end), high = std::max(e.start, e.end);
  double parameter;
  if (c.kind == CurveKind::Line) {
    const Vec3 delta = point - c.origin;
    parameter = dot(delta, c.u) / dot(c.u, c.u);
    parameter = std::clamp(parameter, low, high);
  }
  else {
    // Dual vectors avoid cancellation in a nearly dependent ellipse Gram determinant.
    const Vec3 n = cross(c.u, c.v);
    const double denominator = dot(n, n);
    const Vec3 delta = point - c.origin;
    const double x = dot(delta, cross(c.v, n)) / denominator;
    const double y = dot(delta, cross(n, c.u)) / denominator;
    parameter = std::atan2(y, x);
    parameter += period * std::ceil((low - parameter) / period);
    if (parameter > high) {
      const double a = length(point - evaluate(c, low));
      const double b = length(point - evaluate(c, high));
      parameter = a < b ? low : high;
    }
  }
  const double residual = length(point - evaluate(c, parameter));
  const double numerical = arithmetic_error(point, c.origin, length(c.u) + length(c.v));
  if (residual == 0)
    return State::Boundary;
  // Nonzero residuals, however small, cannot certify an exact contact. This is particularly
  // important for thin trims and close parallel faces whose separation is below tolerance.
  if (residual <= std::max(tolerance.distance, numerical))
    return State::Uncertain;
  return State::Outside;
}

State trim_contact(const Model &model,
                   const Face &face,
                   Vec3 point,
                   ClassificationTolerance tolerance)
{
  State result = State::Outside;
  auto check = [&](const std::vector<Coedge> &loop) {
    for (Coedge use : loop) {
      const Edge &e = *edge(model, use.edge);
      const State contact = edge_contact(*curve(model, e.curve), e, point, tolerance);
      if (contact == State::Boundary)
        result = State::Boundary;
      else if (contact == State::Uncertain && result != State::Boundary)
        result = contact;
    }
  };
  check(face.outer);
  for (const auto &hole : face.holes)
    check(hole);
  return result;
}

bool endpoint(const Edge &e, double t)
{
  const double eps =
      roundoff *
      std::max({std::abs(t), std::abs(e.start), std::abs(e.end), std::abs(e.end - e.start)});
  return std::abs(t - e.start) <= eps || std::abs(t - e.end) <= eps;
}

State trim_inside(const Model &model,
                  const Face &face,
                  const Support &support,
                  Vec3 point,
                  ClassificationTolerance tolerance)
{
  const State contact = trim_contact(model, face, point, tolerance);
  if (contact != State::Outside)
    return contact;
  const Vec3 local = support.cylinder ? support.local(point) : Vec3{};
  // Planar faces use several in-plane directions. On a cylinder use both axial directions;
  // intersect the original native trims with the meridian plane and discard its other side.
  for (int attempt = 0; attempt < (support.cylinder ? 2 : 6); ++attempt) {
    Surface ray;
    ray.kind = SurfaceKind::Plane;
    ray.origin = point;
    Vec3 ray_direction;
    if (support.cylinder) {
      ray.origin = support.value.origin;
      ray.u = support.value.u * local.x + support.value.v * local.y;
      ray.v = support.value.w;
      ray_direction = support.value.w * (attempt == 0 ? 1 : -1);
    }
    else {
      const double rotation = .173 + attempt * .731;
      const Vec3 u = support.value.u / length(support.value.u);
      const Vec3 v = cross(support.normal, u);
      ray_direction = u * std::cos(rotation) + v * std::sin(rotation);
      ray.u = ray_direction;
      ray.v = support.normal;
    }
    const Vec3 ray_normal = cross(ray.u, ray.v);
    size_t crossings = 0;
    bool retry = false;
    auto count = [&](const std::vector<Coedge> &loop) {
      for (Coedge use : loop) {
        if (retry)
          return;
        const Edge &e = *edge(model, use.edge);
        const Curve &c = *curve(model, e.curve);
        std::string error;
        const auto hits = intersect_edge_plane(model, e, ray, {0, 0}, &error);
        if (!hits)
          throw std::runtime_error(error);
        if (hits->state == EdgePlaneState::Coincident || hits->state == EdgePlaneState::Uncertain)
        {
          retry = true;
          return;
        }
        for (double parameter : hits->parameters) {
          const Vec3 p = evaluate(c, parameter);
          double ahead;
          if (support.cylinder) {
            const Vec3 q = support.local(p);
            if (q.x * local.x + q.y * local.y <= 0)
              continue;
            ahead = (q.z - local.z) * (attempt == 0 ? 1 : -1);
          }
          else
            ahead = dot(p - point, ray_direction);
          if (ahead <= 0)
            continue;
          if (endpoint(e, parameter)) {
            retry = true;
            return;
          }
          const Vec3 derivative = tangent(c, parameter);
          const double crossing = dot(derivative, ray_normal);
          if (hits->tangent)
            continue;
          if (std::abs(crossing) <= roundoff * length(derivative) * length(ray_normal)) {
            retry = true;
            return;
          }
          ++crossings;
        }
      }
    };
    count(face.outer);
    for (const auto &hole : face.holes)
      count(hole);
    if (!retry)
      return crossings % 2 ? State::Inside : State::Outside;
  }
  return State::Uncertain;
}

struct SupportContact {
  bool exact;
  bool near;
};

SupportContact support_contact(const Support &support,
                               Vec3 point,
                               ClassificationTolerance tolerance)
{
  double residual, uncertainty;
  if (support.cylinder) {
    const Vec3 q = support.local(point);
    residual = std::abs(q.x * q.x + q.y * q.y - 1);
    const double gradient = 2 * length(support.row_x * q.x + support.row_y * q.y);
    uncertainty = gradient *
                  std::max(tolerance.distance,
                           arithmetic_error(point,
                                            support.value.origin,
                                            length(support.value.u) + length(support.value.v)));
  }
  else {
    residual = std::abs(dot(point - support.value.origin, support.normal));
    uncertainty = std::max(
        tolerance.distance,
        arithmetic_error(point, support.value.origin, length(point - support.value.origin)));
  }
  return {residual == 0, residual <= uncertainty};
}

struct RayHits {
  std::vector<double> parameters;
  bool uncertain = false;
};

RayHits ray_hits(const Support &support,
                 Vec3 point,
                 Vec3 direction,
                 ClassificationTolerance tolerance)
{
  RayHits result;
  if (!support.cylinder) {
    const double denominator = dot(direction, support.normal);
    const double numerator = dot(support.value.origin - point, support.normal);
    if (denominator == 0) {
      result.uncertain = numerator == 0;
      return result;
    }
    if (std::abs(denominator) <= tolerance.angle * length(direction)) {
      result.uncertain = true;
      return result;
    }
    const double t = numerator / denominator;
    if (!std::isfinite(t))
      result.uncertain = true;
    else if (t > 0)
      result.parameters.push_back(t);
    return result;
  }
  const Vec3 q = support.local(point), d = support.direction(direction);
  const long double a = (long double)d.x * d.x + (long double)d.y * d.y;
  const long double b = 2 * ((long double)q.x * d.x + (long double)q.y * d.y);
  const long double c = (long double)q.x * q.x + (long double)q.y * q.y - 1;
  if (a == 0) {
    result.uncertain = c == 0;
    return result;
  }
  const long double discriminant = b * b - 4 * a * c;
  const long double uncertainty = roundoff * (b * b + std::abs(4 * a * c));
  if (discriminant == 0)
    return result;  // Exact tangency does not cross the support.
  if (std::abs(discriminant) <= uncertainty) {
    result.uncertain = true;
    return result;
  }
  if (discriminant < 0)
    return result;
  // The q-form avoids cancellation when one intersection is very close to the origin.
  const long double root = -.5L * (b + std::copysign(std::sqrt(discriminant), b));
  double first = double(root / a), last = double(c / root);
  if (first > last)
    std::swap(first, last);
  if (!std::isfinite(first) || !std::isfinite(last))
    result.uncertain = true;
  else {
    if (first > 0)
      result.parameters.push_back(first);
    if (last > 0)
      result.parameters.push_back(last);
  }
  return result;
}

}  // namespace

std::optional<PointClassification> classify_face_trim_point(const Model &model,
                                                            const Face &face,
                                                            Vec3 point,
                                                            ClassificationTolerance tolerance,
                                                            std::string *error)
{
  try {
    check_tolerance(tolerance, point);
    const Support support = support_for(model, face);
    if (support.ill_conditioned)
      return State::Uncertain;
    return trim_inside(model, face, support, point, tolerance);
  }
  catch (const std::exception &exception) {
    if (error)
      *error = exception.what();
    return std::nullopt;
  }
}

std::optional<PointClassification> classify_face_point(const Model &model,
                                                       const Face &face,
                                                       Vec3 point,
                                                       ClassificationTolerance tolerance,
                                                       std::string *error)
{
  try {
    check_tolerance(tolerance, point);
    const Support support = support_for(model, face);
    if (support.ill_conditioned)
      return State::Uncertain;
    const SupportContact contact = support_contact(support, point, tolerance);
    if (!contact.near)
      return State::Outside;
    const State trim = trim_inside(model, face, support, point, tolerance);
    if (trim == State::Outside)
      return trim;
    return contact.exact ? trim : State::Uncertain;
  }
  catch (const std::exception &exception) {
    if (error)
      *error = exception.what();
    return std::nullopt;
  }
}

std::optional<PointClassification> classify_body_point(const Model &model,
                                                       const std::vector<Id> &face_ids,
                                                       Vec3 point,
                                                       ClassificationTolerance tolerance,
                                                       std::string *error)
{
  try {
    check_tolerance(tolerance, point);
    if (face_ids.empty())
      throw std::runtime_error("Native classification needs a nonempty body");
    std::vector<const Face *> faces;
    std::vector<Support> supports;
    std::map<Id, size_t> edge_uses;
    std::set<Id> unique;
    for (Id id : face_ids) {
      const Face *f = face(model, id);
      if (!f || !unique.insert(id).second)
        throw std::runtime_error("Native classification has a missing or repeated body face");
      faces.push_back(f);
      supports.push_back(support_for(model, *f));
      for (Coedge use : f->outer)
        ++edge_uses[use.edge];
      for (const auto &hole : f->holes)
        for (Coedge use : hole)
          ++edge_uses[use.edge];
    }
    for (const auto &[id, uses] : edge_uses) {
      (void)id;
      if (uses != 2)
        throw std::runtime_error("Native point classification requires a closed manifold body");
    }
    for (const Support &support : supports)
      if (support.ill_conditioned)
        return State::Uncertain;
    bool near_boundary = false;
    for (size_t i = 0; i < faces.size(); ++i) {
      const SupportContact contact = support_contact(supports[i], point, tolerance);
      if (!contact.near)
        continue;
      const State trim = trim_inside(model, *faces[i], supports[i], point, tolerance);
      if (trim == State::Outside)
        continue;
      if (contact.exact && (trim == State::Inside || trim == State::Boundary))
        return State::Boundary;
      near_boundary = true;
    }
    if (near_boundary)
      return State::Uncertain;
    // None of these rays is privileged. A failed face invalidates all crossings for the ray.
    constexpr Vec3 directions[] = {{1, .371, .613},
                                   {.293, 1, .719},
                                   {.827, .419, 1},
                                   {-1, .563, .239},
                                   {.677, -1, .433},
                                   {.193, .887, -1},
                                   {1, -.743, -.317},
                                   {-.461, 1, -.829},
                                   {-.617, -.271, 1}};
    std::optional<State> agreement;
    for (Vec3 direction : directions) {
      size_t crossings = 0;
      bool retry = false;
      for (size_t i = 0; i < faces.size() && !retry; ++i) {
        const RayHits hits = ray_hits(supports[i], point, direction, tolerance);
        if (hits.uncertain) {
          retry = true;
          break;
        }
        for (double t : hits.parameters) {
          const State trim = trim_inside(
              model, *faces[i], supports[i], point + direction * t, tolerance);
          if (trim == State::Inside)
            ++crossings;
          else if (trim != State::Outside) {
            retry = true;
            break;
          }
        }
      }
      if (retry)
        continue;
      const State state = crossings % 2 ? State::Inside : State::Outside;
      if (agreement)
        return *agreement == state ? state : State::Uncertain;
      agreement = state;
    }
    return agreement.value_or(State::Uncertain);
  }
  catch (const std::exception &exception) {
    if (error)
      *error = exception.what();
    return std::nullopt;
  }
}

std::optional<PointClassification> classify_body_point(const Model &model,
                                                       Vec3 point,
                                                       ClassificationTolerance tolerance,
                                                       std::string *error)
{
  std::vector<Id> faces;
  faces.reserve(model.faces.size());
  for (const Face &face : model.faces)
    faces.push_back(face.id);
  return classify_body_point(model, faces, point, tolerance, error);
}

}  // namespace blender::solid
