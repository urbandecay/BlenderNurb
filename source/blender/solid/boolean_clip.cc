/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_boolean.hh"
#include "SOL_classify.hh"
#include "SOL_clip.hh"
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

constexpr double roundoff = 64 * std::numeric_limits<double>::epsilon();

bool finite(Vec3 v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct Range {
  long double low = std::numeric_limits<long double>::infinity();
  long double high = -std::numeric_limits<long double>::infinity();
  void add(long double v)
  {
    low = std::min(low, v);
    high = std::max(high, v);
  }
};

long double precise_dot(Vec3 a, Vec3 b)
{
  return (long double)a.x * b.x + (long double)a.y * b.y + (long double)a.z * b.z;
}

Range edge_range(const Model &model, const Edge &edge, const Surface &plane, Vec3 normal)
{
  const Curve &c = *curve(model, edge.curve);
  if (c.kind != CurveKind::Line && c.kind != CurveKind::Conic)
    throw std::runtime_error("Native body plane split requires line/conic trimming curves");
  const long double offset = precise_dot(c.origin - plane.origin, normal);
  const long double a = precise_dot(c.u, normal), b = precise_dot(c.v, normal);
  Range range;
  const double low = std::min(edge.start, edge.end), high = std::max(edge.start, edge.end);
  // Endpoints use the kernel's evaluation semantics, including exact cardinal conic values.
  // Re-evaluating these with long-double libm would invent tiny seam crossings.
  range.add(precise_dot(evaluate(c, low) - plane.origin, normal));
  range.add(precise_dot(evaluate(c, high) - plane.origin, normal));
  if (c.kind == CurveKind::Conic && (a != 0 || b != 0)) {
    const long double base = std::atan2(b, a);
    const long double pi = std::numbers::pi_v<long double>;
    const long double first = std::ceil((low - base) / pi);
    const long double amplitude = std::hypot(a, b);
    // Analytic extrema; no sampled or polygonal boundary participates in the decision.
    for (long double k = first; base + k * pi <= high; ++k) {
      range.add(offset + (std::fmod(std::abs(k), 2) == 0 ? amplitude : -amplitude));
      if (k + 1 == k)
        throw std::runtime_error("Native conic parameter exceeds precision");
    }
  }
  return range;
}

Range face_range(const Model &model, const Face &face, const Surface &plane, Vec3 normal)
{
  Range range;
  auto add = [&](const std::vector<Coedge> &loop) {
    for (Coedge use : loop) {
      const Range edge_bounds = edge_range(model, *edge(model, use.edge), plane, normal);
      range.add(edge_bounds.low);
      range.add(edge_bounds.high);
    }
  };
  add(face.outer);
  for (const auto &hole : face.holes)
    add(hole);
  return range;
}

Vec3 loop_integral(const Model &model, const std::vector<Coedge> &loop, Vec3 reference)
{
  Vec3 integral{};
  for (Coedge use : loop) {
    const Edge &e = *edge(model, use.edge);
    const Curve &c = *curve(model, e.curve);
    const double a = use.reversed ? e.end : e.start;
    const double b = use.reversed ? e.start : e.end;
    const Vec3 origin = c.origin - reference;
    if (c.kind == CurveKind::Line)
      integral = integral + cross(origin, c.u) * (b - a);
    else if (c.kind == CurveKind::Conic) {
      const Curve unit{0, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}, {}};
      const Vec3 first = evaluate(unit, a), last = evaluate(unit, b);
      integral = integral + cross(origin, c.u) * (last.x - first.x) +
                 cross(origin, c.v) * (last.y - first.y) + cross(c.u, c.v) * (b - a);
    }
    else
      throw std::runtime_error("Native cap construction requires line/conic area integrals");
  }
  return integral;
}

/** Split all incident faces' shared trims in one transaction, including tangential hits.
 * Otherwise a crossing face could introduce a vertex into its neighbor's unsplit edge. */
Model partition_section_edges(const Model &source, const Surface &plane)
{
  Model result = source;
  std::map<Id, std::vector<Id>> parts;
  std::string error;
  for (const Edge &e : source.edges) {
    const auto hits = intersect_edge_plane(source, e, plane, {0, 0}, &error);
    if (!hits)
      throw std::runtime_error(error);
    if (hits->state == EdgePlaneState::Uncertain)
      throw std::runtime_error("Native plane split encountered an uncertain edge intersection");
    if (hits->state != EdgePlaneState::Points)
      continue;
    std::vector<double> parameters = {e.start};
    for (double t : hits->parameters)
      if (t != e.start && t != e.end)
        parameters.push_back(t);
    parameters.push_back(e.end);
    if (parameters.size() == 2)
      continue;
    const Curve &c = *curve(source, e.curve);
    Id previous = e.vertices[0];
    for (size_t i = 1; i < parameters.size(); ++i) {
      const Id next = i + 1 == parameters.size() ? e.vertices[1] : result.next_id++;
      if (i + 1 != parameters.size())
        result.vertices.push_back({next, evaluate(c, parameters[i])});
      Edge part = e;
      part.id = result.next_id++;
      part.vertices = {previous, next};
      part.start = parameters[i - 1];
      part.end = parameters[i];
      result.edges.push_back(part);
      parts[e.id].push_back(part.id);
      previous = next;
    }
  }
  for (Face &f : result.faces) {
    auto remap = [&](std::vector<Coedge> &loop) {
      std::vector<Coedge> changed;
      for (Coedge use : loop) {
        const auto found = parts.find(use.edge);
        if (found == parts.end())
          changed.push_back(use);
        else if (use.reversed)
          for (auto it = found->second.rbegin(); it != found->second.rend(); ++it)
            changed.push_back({*it, true});
        else
          for (Id id : found->second)
            changed.push_back({id, false});
      }
      loop = std::move(changed);
    };
    remap(f.outer);
    for (auto &hole : f.holes)
      remap(hole);
  }
  std::erase_if(result.edges, [&](const Edge &e) { return parts.contains(e.id); });
  if (!validate(result, &error))
    throw std::runtime_error(error);
  return result;
}

Model face_piece(const Model &source, const std::vector<Id> &ids)
{
  Model piece = source;
  piece.faces.clear();
  for (Id id : ids) {
    Face copy = *face(source, id);
    copy.group = 0;
    piece.faces.push_back(std::move(copy));
  }
  return piece;
}

Id start_vertex(const Model &model, Coedge use)
{
  return edge(model, use.edge)->vertices[use.reversed ? 1 : 0];
}
Id end_vertex(const Model &model, Coedge use)
{
  return edge(model, use.edge)->vertices[use.reversed ? 0 : 1];
}

Model close_side(const std::vector<Model> &pieces,
                 const Surface &plane,
                 Vec3 normal,
                 int sign,
                 double tolerance)
{
  if (pieces.empty())
    return {};
  std::string error;
  auto assembled = boolean_sew_faces(pieces, tolerance, false, &error);
  if (!assembled)
    throw std::runtime_error(error);
  Model result = std::move(*assembled);
  std::map<Id, std::vector<Coedge>> uses;
  for (const Face &f : result.faces) {
    auto add = [&](const std::vector<Coedge> &loop) {
      for (Coedge use : loop) {
        use.reversed = use.reversed != (f.orientation < 0);
        uses[use.edge].push_back(use);
      }
    };
    add(f.outer);
    for (const auto &hole : f.holes)
      add(hole);
  }
  std::map<Id, Coedge> outgoing;
  for (const auto &[id, adjacent] : uses) {
    if (adjacent.size() == 2) {
      if (adjacent[0].reversed == adjacent[1].reversed)
        throw std::runtime_error("Native plane split produced inconsistent face orientations");
      continue;
    }
    if (adjacent.size() != 1)
      throw std::runtime_error("Native plane split produced a nonmanifold boundary");
    const Edge &e = *edge(result, id);
    const Curve &c = *curve(result, e.curve);
    const Range range = edge_range(result, e, plane, normal);
    const double numerical = roundoff *
                             (length(plane.origin) + length(c.origin) + length(c.u) + length(c.v));
    if (std::max(std::abs(range.low), std::abs(range.high)) > std::max(tolerance, numerical))
      throw std::runtime_error("Native plane split left a gap away from the section plane");
    Coedge cap_use = adjacent.front();
    cap_use.reversed = !cap_use.reversed;
    if (!outgoing.emplace(start_vertex(result, cap_use), cap_use).second)
      throw std::runtime_error("Native plane cap has an ambiguous boundary junction");
  }
  std::vector<std::vector<Coedge>> loops;
  while (!outgoing.empty()) {
    const Id first = outgoing.begin()->first;
    Id current = first;
    std::vector<Coedge> loop;
    do {
      const auto next = outgoing.find(current);
      if (next == outgoing.end())
        throw std::runtime_error("Native plane cap has an open boundary");
      const Coedge use = next->second;
      outgoing.erase(next);
      loop.push_back(use);
      current = end_vertex(result, use);
    } while (current != first);
    loops.push_back(std::move(loop));
  }
  if (!loops.empty()) {
    Surface cap_support = plane;
    cap_support.id = result.next_id++;
    if (sign < 0)
      cap_support.v = cap_support.v * -1;
    result.surfaces.push_back(cap_support);
    std::vector<Face> caps;
    std::vector<std::pair<double, std::vector<Coedge>>> holes;
    std::vector<double> cap_area;
    for (const auto &loop : loops) {
      const double area = dot(loop_integral(result, loop, plane.origin), normal) * sign;
      if (area == 0 || !std::isfinite(area))
        throw std::runtime_error("Native plane cap has an unresolved zero-area loop");
      if (area > 0) {
        Face cap;
        cap.id = result.next_id++;
        cap.surface = cap_support.id;
        cap.outer = loop;
        caps.push_back(std::move(cap));
        cap_area.push_back(area);
      }
      else
        holes.push_back({-area, loop});
    }
    for (const auto &[area, loop] : holes) {
      (void)area;
      const Vec3 point = vertex(result, start_vertex(result, loop.front()))->position;
      size_t containing = caps.size();
      double smallest = std::numeric_limits<double>::infinity();
      for (size_t i = 0; i < caps.size(); ++i) {
        const auto inside = classify_face_trim_point(result, caps[i], point, {0, 0}, &error);
        if (!inside)
          throw std::runtime_error(error);
        if (*inside == PointClassification::Boundary || *inside == PointClassification::Uncertain)
          throw std::runtime_error("Native plane cap has an unresolved touching inner loop");
        if (*inside == PointClassification::Inside && cap_area[i] < smallest) {
          containing = i;
          smallest = cap_area[i];
        }
      }
      if (containing == caps.size())
        throw std::runtime_error("Native plane cap inner loop has no containing outer loop");
      caps[containing].holes.push_back(loop);
    }
    for (Face &cap : caps)
      result.faces.push_back(std::move(cap));
  }
  for (const Face &f : result.faces)
    if (!validate_body(result, f.id, &error))
      throw std::runtime_error(error);
  return result;
}

}  // namespace

std::optional<BooleanBodyPlaneSplit> boolean_split_body_by_plane(const Model &source,
                                                                 const Surface &plane,
                                                                 double assembly_tolerance,
                                                                 std::string *error)
{
  try {
    std::string message;
    if (plane.kind != SurfaceKind::Plane || !finite(plane.origin) || !finite(plane.u) ||
        !finite(plane.v) || !(length(cross(plane.u, plane.v)) > 0) ||
        !std::isfinite(length(cross(plane.u, plane.v))) || !std::isfinite(assembly_tolerance) ||
        assembly_tolerance < 0)
      throw std::runtime_error(
          "Native body plane split needs a valid plane and joining tolerance");
    if (source.faces.empty())
      throw std::runtime_error("Native body plane split needs a closed body");
    for (const Face &f : source.faces) {
      if (!validate_body(source, f.id, &message))
        throw std::runtime_error(message);
      const Surface &s = *surface(source, f.surface);
      if (s.kind != SurfaceKind::Plane && s.kind != SurfaceKind::Cylinder &&
          (s.kind != SurfaceKind::Extrusion || !curve(source, s.curve) ||
           curve(source, s.curve)->kind != CurveKind::Conic))
        throw std::runtime_error("Native body plane split does not support this surface family");
    }
    const Vec3 normal = cross(plane.u, plane.v) / length(cross(plane.u, plane.v));
    Range whole;
    for (const Face &f : source.faces) {
      const Range range = face_range(source, f, plane, normal);
      whole.add(range.low);
      whole.add(range.high);
    }
    BooleanBodyPlaneSplit result;
    if (whole.high <= 0 && whole.low < 0) {
      result.negative = source;
      return result;
    }
    if (whole.low >= 0 && whole.high > 0) {
      result.positive = source;
      return result;
    }
    if (whole.low == 0 && whole.high == 0)
      throw std::runtime_error("Native plane split cannot classify a zero-volume body");
    const auto periodic = boolean_partition_periodic_faces(source, &message);
    if (!periodic)
      throw std::runtime_error(message);
    const Model partitioned = partition_section_edges(*periodic, plane);
    std::vector<Model> negative, positive;
    for (const Face &f : partitioned.faces) {
      const Range range = face_range(partitioned, f, plane, normal);
      const Surface &s = *surface(partitioned, f.surface);
      bool coplanar = false;
      if (s.kind == SurfaceKind::Plane) {
        const auto intersection = intersect_surfaces(partitioned, s, plane, {0, 0}, &message);
        if (!intersection)
          throw std::runtime_error(message);
        if (intersection->state == SurfaceIntersectionState::Uncertain)
          throw std::runtime_error("Native plane split encountered uncertain coplanarity");
        coplanar = intersection->state == SurfaceIntersectionState::Coincident;
      }
      if (coplanar || (range.low == 0 && range.high == 0)) {
        if (s.kind != SurfaceKind::Plane)
          throw std::runtime_error("Native plane split has an unresolved coplanar curved face");
        const double orientation = dot(loop_integral(partitioned, f.outer, plane.origin), normal) *
                                   f.orientation;
        if (orientation == 0)
          throw std::runtime_error("Native plane split cannot orient a coplanar face");
        (orientation > 0 ? negative : positive).push_back(face_piece(partitioned, {f.id}));
      }
      else if (range.high <= 0)
        negative.push_back(face_piece(partitioned, {f.id}));
      else if (range.low >= 0)
        positive.push_back(face_piece(partitioned, {f.id}));
      else {
        const auto split = boolean_split_face_by_plane(partitioned, f.id, plane, &message);
        if (!split)
          throw std::runtime_error(message);
        if (!split->negative.empty())
          negative.push_back(face_piece(split->model, split->negative));
        if (!split->positive.empty())
          positive.push_back(face_piece(split->model, split->positive));
      }
    }
    result.negative = close_side(negative, plane, normal, 1, assembly_tolerance);
    result.positive = close_side(positive, plane, normal, -1, assembly_tolerance);
    return result;
  }
  catch (const std::exception &exception) {
    if (error)
      *error = exception.what();
    return std::nullopt;
  }
}

}  // namespace blender::solid
