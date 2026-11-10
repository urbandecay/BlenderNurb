/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_boolean.hh"
#include "SOL_planar_clip.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>

namespace blender::solid {
namespace {

long double precise_dot(Vec3 a, Vec3 b)
{
  return (long double)a.x * b.x + (long double)a.y * b.y + (long double)a.z * b.z;
}

Vec3 normal(const Surface &s)
{
  const Vec3 n = cross(s.u, s.v);
  if (!(length(n) > 0) || !std::isfinite(length(n)))
    throw std::runtime_error("Native planar clipping found a degenerate plane");
  return n / length(n);
}

double loop_area(const Model &model, const std::vector<Coedge> &loop, Vec3 origin, Vec3 normal)
{
  Vec3 integral{};
  const Curve unit{0, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}, {}};
  for (Coedge use : loop) {
    const Edge &e = *edge(model, use.edge);
    const Curve &c = *curve(model, e.curve);
    const double a = use.reversed ? e.end : e.start, b = use.reversed ? e.start : e.end;
    if (c.kind == CurveKind::Line)
      integral = integral + cross(c.origin - origin, c.u) * (b - a);
    else if (c.kind == CurveKind::Conic) {
      const Vec3 first = evaluate(unit, a), last = evaluate(unit, b);
      integral = integral + cross(c.origin - origin, c.u) * (last.x - first.x) +
                 cross(c.origin - origin, c.v) * (last.y - first.y) + cross(c.u, c.v) * (b - a);
    }
    else
      throw std::runtime_error("Native planar clipping requires line/conic source trims");
  }
  return dot(integral, normal) / 2;
}

struct Range {
  long double low = std::numeric_limits<long double>::infinity();
  long double high = -std::numeric_limits<long double>::infinity();
  void add(long double value)
  {
    low = std::min(low, value);
    high = std::max(high, value);
  }
};

Range face_range(const Model &model, const Face &face, const Surface &cut)
{
  const Vec3 n = normal(cut);
  Range range;
  auto add = [&](const std::vector<Coedge> &loop) {
    for (Coedge use : loop) {
      const Edge &e = *edge(model, use.edge);
      const Curve &c = *curve(model, e.curve);
      const double low = std::min(e.start, e.end), high = std::max(e.start, e.end);
      range.add(precise_dot(evaluate(c, low) - cut.origin, n));
      range.add(precise_dot(evaluate(c, high) - cut.origin, n));
      if (c.kind == CurveKind::Conic) {
        const long double offset = precise_dot(c.origin - cut.origin, n);
        const long double a = precise_dot(c.u, n), b = precise_dot(c.v, n);
        if (a == 0 && b == 0)
          continue;
        const long double base = std::atan2(b, a), pi = std::numbers::pi_v<long double>;
        const long double amplitude = std::hypot(a, b);
        for (long double k = std::ceil((low - base) / pi); base + k * pi <= high; ++k) {
          range.add(offset + (std::fmod(std::abs(k), 2) == 0 ? amplitude : -amplitude));
          if (k + 1 == k)
            throw std::runtime_error("Native conic parameter exceeds precision");
        }
      }
    }
  };
  add(face.outer);
  for (const auto &hole : face.holes)
    add(hole);
  return range;
}

Model piece(const Model &source, const std::vector<Id> &faces)
{
  Model result = source;
  result.faces.clear();
  for (Id id : faces) {
    Face copy = *face(source, id);
    copy.group = 0;
    result.faces.push_back(std::move(copy));
  }
  return result;
}

void reverse_loop(std::vector<Coedge> &loop)
{
  std::reverse(loop.begin(), loop.end());
  for (Coedge &use : loop)
    use.reversed = !use.reversed;
}

}  // namespace

std::optional<Model> boolean_clip_planar_face(const Model &source,
                                              Id source_id,
                                              const Model &mask,
                                              Id mask_id,
                                              IntersectionTolerance tolerance,
                                              std::string *error)
{
  try {
    std::string message;
    if (!std::isfinite(tolerance.distance) || tolerance.distance < 0 ||
        !std::isfinite(tolerance.angle) || tolerance.angle < 0 || tolerance.angle >= 1)
      throw std::runtime_error("Native planar clipping requires finite nonnegative tolerances");
    if (!validate(source, &message) || !validate(mask, &message))
      throw std::runtime_error(message);
    const Face *source_face = face(source, source_id), *mask_face = face(mask, mask_id);
    const Surface *source_support = source_face ? surface(source, source_face->surface) : nullptr;
    const Surface *mask_support = mask_face ? surface(mask, mask_face->surface) : nullptr;
    if (!source_support || !mask_support || source_support->kind != SurfaceKind::Plane ||
        mask_support->kind != SurfaceKind::Plane)
      throw std::runtime_error("Native planar clipping needs two bounded planar faces");
    if (!mask_face->holes.empty())
      throw std::runtime_error("Native planar clipping requires a mask without holes");
    const Vec3 source_normal = normal(*source_support), mask_normal = normal(*mask_support);
    if (length(cross(source_normal, mask_normal)) > tolerance.angle ||
        std::abs(precise_dot(mask_support->origin - source_support->origin, source_normal)) >
            tolerance.distance ||
        std::abs(precise_dot(source_support->origin - mask_support->origin, mask_normal)) >
            tolerance.distance)
      throw std::runtime_error("Native planar clipping requires the same planar support");

    // Check every source trim family before any early disjoint/contained result.
    const double source_winding = loop_area(
        source, source_face->outer, source_support->origin, source_normal);
    if (source_winding == 0 || !std::isfinite(source_winding))
      throw std::runtime_error("Native planar clipping found a zero-area source boundary");
    for (const auto &hole : source_face->holes) {
      const double area = loop_area(source, hole, source_support->origin, source_normal);
      if (area == 0 || !std::isfinite(area))
        throw std::runtime_error("Native planar clipping found a zero-area source inner loop");
    }

    std::vector<Vec3> points;
    std::set<Id> vertices;
    std::vector<Surface> half_planes;
    const double winding = loop_area(mask, mask_face->outer, mask_support->origin, source_normal);
    if (winding == 0 || !std::isfinite(winding))
      throw std::runtime_error("Native planar clipping found a zero-area mask boundary");
    for (Coedge use : mask_face->outer) {
      const Edge &e = *edge(mask, use.edge);
      const Curve &c = *curve(mask, e.curve);
      if (c.kind != CurveKind::Line)
        throw std::runtime_error("Native planar clipping requires a straight-boundary mask");
      const double start = use.reversed ? e.end : e.start, end = use.reversed ? e.start : e.end;
      const Vec3 point = evaluate(c, start), last = evaluate(c, end);
      const Id vertex = e.vertices[use.reversed ? 1 : 0];
      if (!vertices.insert(vertex).second ||
          std::find(points.begin(), points.end(), point) != points.end() || point == last)
        throw std::runtime_error("Native planar clipping requires a simple nondegenerate mask");
      if (std::abs(precise_dot(point - source_support->origin, source_normal)) >
          tolerance.distance)
        throw std::runtime_error("Native planar mask boundary leaves the source support");
      points.push_back(point);
      Surface cut;
      cut.kind = SurfaceKind::Plane;
      cut.origin = point;
      cut.u = c.u * (end > start ? 1 : -1);
      cut.v = source_normal * (winding > 0 ? 1 : -1);
      half_planes.push_back(cut);
    }
    if (points.size() < 3)
      throw std::runtime_error("Native planar clipping needs at least three mask sides");
    for (size_t i = 0; i < half_planes.size(); ++i) {
      const Surface &cut = half_planes[i];
      const Vec3 n = normal(cut);
      for (size_t j = 0; j < points.size(); ++j) {
        // These two endpoints are certified by this native line's parameter interval.
        // Re-testing them with a normalized cross product can invent a small positive side.
        if (j == i || j == (i + 1) % points.size())
          continue;
        if (precise_dot(points[j] - cut.origin, n) > 0)
          throw std::runtime_error("Native planar clipping requires a convex mask");
      }
    }

    Model initial = piece(source, {source_id});
    for (auto &hole : initial.faces.front().holes)
      if (loop_area(initial, hole, source_support->origin, source_normal) * source_winding > 0)
        reverse_loop(hole);
    const std::vector<Model> initial_pieces{std::move(initial)};
    auto assembled = boolean_sew_faces(initial_pieces, 0, false, &message);
    if (!assembled)
      throw std::runtime_error(message);
    Model current = std::move(*assembled);
    for (const Surface &cut : half_planes) {
      std::vector<Model> kept;
      for (const Face &f : current.faces) {
        const Range range = face_range(current, f, cut);
        if (range.high <= 0)
          kept.push_back(piece(current, {f.id}));
        else if (range.low < 0) {
          const auto split = boolean_split_face_by_plane(current, f.id, cut, &message);
          if (!split)
            throw std::runtime_error(message);
          if (!split->negative.empty())
            kept.push_back(piece(split->model, split->negative));
        }
      }
      if (kept.empty())
        return Model{};
      assembled = boolean_sew_faces(kept, 0, false, &message);
      if (!assembled)
        throw std::runtime_error(message);
      current = std::move(*assembled);
    }
    return current;
  }
  catch (const std::exception &exception) {
    if (error)
      *error = exception.what();
    return std::nullopt;
  }
}

}  // namespace blender::solid
