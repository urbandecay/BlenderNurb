/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_boolean.hh"
#include "SOL_intersections.hh"
#include "boolean_pave.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>

namespace blender::solid {
namespace {

Id first_vertex(const Model &model, const Coedge use)
{
  return edge(model, use.edge)->vertices[use.reversed ? 1 : 0];
}
Id last_vertex(const Model &model, const Coedge use)
{
  return edge(model, use.edge)->vertices[use.reversed ? 0 : 1];
}

struct CylinderChart {
  const Surface &surface;
  Vec3 vw, wu, uv;
  double det;
  explicit CylinderChart(const Surface &s)
      : surface(s),
        vw(cross(s.v, s.w)),
        wu(cross(s.w, s.u)),
        uv(cross(s.u, s.v)),
        det(dot(s.u, vw))
  {
  }
  Vec3 direction(Vec3 p) const
  {
    return {dot(p, vw) / det, dot(p, wu) / det, dot(p, uv) / det};
  }
  Vec3 local(Vec3 p) const
  {
    return direction(p - surface.origin);
  }
};

double cylinder_center(const Model &model, const Face &face, const CylinderChart &chart)
{
  constexpr double period = 2 * std::numbers::pi;
  double previous = 0, low = 0, high = 0;
  bool first = true;
  for (Coedge use : face.outer) {
    const Edge &e = *edge(model, use.edge);
    const Curve &c = *curve(model, e.curve);
    const double start = use.reversed ? e.end : e.start, end = use.reversed ? e.start : e.end;
    const Vec3 a = chart.local(evaluate(c, start));
    double theta = std::atan2(a.y, a.x);
    if (!first)
      theta += period * std::round((previous - theta) / period);
    else {
      low = high = theta;
      first = false;
    }
    low = std::min(low, theta);
    high = std::max(high, theta);
    if (c.kind == CurveKind::Conic) {
      const Vec3 u = chart.direction(c.u), v = chart.direction(c.v);
      theta += (u.x * v.y - u.y * v.x > 0 ? 1 : -1) * (end - start);
    }
    else if (c.kind != CurveKind::Line) {
      throw std::runtime_error("Cylinder trim chart needs a native curve parameter mapping");
    }
    previous = theta;
    low = std::min(low, theta);
    high = std::max(high, theta);
  }
  if (high - low >= period - 1e-12)
    throw std::runtime_error("Cylinder face split needs periodic seam partitioning");
  return (low + high) / 2;
}

bool cylinder_contains(const Model &model, const Face &face, Vec3 point, const Surface &support)
{
  const CylinderChart chart(support);
  const Vec3 q = chart.local(point);
  const double center = cylinder_center(model, face, chart), period = 2 * std::numbers::pi;
  auto angle = [&](Vec3 p) {
    double value = std::atan2(p.y, p.x);
    return value + period * std::round((center - value) / period);
  };
  const double theta = angle(q);
  for (bool vertical : {false, true})
    for (int sign : {1, -1}) {
      Surface ray;
      if (vertical) {
        ray.origin = support.origin;
        ray.u = support.u * std::cos(theta) + support.v * std::sin(theta);
        ray.v = support.w;
      }
      else {
        ray.origin = support.origin + support.w * q.z;
        ray.u = support.u;
        ray.v = support.v;
      }
      size_t crossings = 0;
      bool retry = false;
      auto count = [&](const std::vector<Coedge> &loop) {
        for (Coedge use : loop) {
          const Edge &e = *edge(model, use.edge);
          const Curve &c = *curve(model, e.curve);
          std::string error;
          auto hits = intersect_edge_plane(model, e, ray, {0, 0}, &error);
          if (!hits)
            throw std::runtime_error(error);
          if (hits->state == EdgePlaneState::Coincident ||
              hits->state == EdgePlaneState::Uncertain)
          {
            retry = true;
            return;
          }
          for (double t : hits->parameters) {
            const Vec3 p = chart.local(evaluate(c, t)), d = chart.direction(tangent(c, t));
            const double a = angle(p);
            if (vertical && std::abs(a - theta) > 1e-8)
              continue;
            if (sign * (vertical ? p.z - q.z : a - theta) <= 0)
              continue;
            if (t == e.start || t == e.end) {
              retry = true;
              return;
            }
            const double crossing = vertical ? p.x * d.y - p.y * d.x : d.z;
            if (hits->tangent || std::abs(crossing) <= 1e-13 * std::max(1., length(d)))
              continue;
            ++crossings;
          }
        }
      };
      count(face.outer);
      for (const auto &hole : face.holes)
        count(hole);
      if (!retry)
        return crossings % 2 != 0;
    }
  throw std::runtime_error(
      "Cylinder trim classification encountered an unresolved boundary contact");
}

double loop_area(const Model &model,
                 const std::vector<Coedge> &loop,
                 Vec3 normal,
                 const Surface *support = nullptr)
{
  if (support && support->kind == SurfaceKind::Cylinder) {
    const CylinderChart chart(*support);
    double area = 0;
    for (Coedge use : loop) {
      const Edge &e = *edge(model, use.edge);
      const Curve &c = *curve(model, e.curve);
      if (c.kind == CurveKind::Line)
        continue;
      if (c.kind != CurveKind::Conic)
        throw std::runtime_error("Cylinder trim needs a native curve area integral");
      const Vec3 p = chart.local(c.origin), u = chart.direction(c.u), v = chart.direction(c.v);
      const double a = use.reversed ? e.end : e.start, b = use.reversed ? e.start : e.end;
      const double sign = u.x * v.y - u.y * v.x > 0 ? 1 : -1;
      area -= sign * (p.z * (b - a) + u.z * (std::sin(b) - std::sin(a)) -
                      v.z * (std::cos(b) - std::cos(a)));
    }
    return area;
  }
  Vec3 integral{};
  for (const Coedge use : loop) {
    const Edge &e = *edge(model, use.edge);
    const Curve &c = *curve(model, e.curve);
    const double a = use.reversed ? e.end : e.start;
    const double b = use.reversed ? e.start : e.end;
    if (c.kind == CurveKind::Line) {
      integral = integral + cross(c.origin, c.u) * (b - a);
    }
    else if (c.kind == CurveKind::Conic) {
      integral = integral + cross(c.origin, c.u) * (std::cos(b) - std::cos(a)) +
                 cross(c.origin, c.v) * (std::sin(b) - std::sin(a)) + cross(c.u, c.v) * (b - a);
    }
    else {
      throw std::runtime_error("Native face splitting needs a curve area integral");
    }
  }
  return dot(integral, normal) / 2;
}

// Ray crossings against the original curves, including inner loops. Only the
// display adapter may sample curves; topology classification uses parameters.
bool contains(const Model &model, const Face &face, Vec3 point, const Surface &support)
{
  if (support.kind == SurfaceKind::Cylinder)
    return cylinder_contains(model, face, point, support);
  const Vec3 normal = cross(support.u, support.v) / length(cross(support.u, support.v));
  for (const double rotation : {0.173, 0.617, 1.291, 2.113}) {
    const Vec3 u = support.u / length(support.u);
    const Vec3 v = cross(normal, u);
    const Vec3 direction = u * std::cos(rotation) + v * std::sin(rotation);
    Surface ray_plane;
    ray_plane.origin = point;
    ray_plane.u = direction;
    ray_plane.v = normal;
    const Vec3 ray_normal = cross(direction, normal);
    size_t crossings = 0;
    bool retry = false;
    auto count = [&](const std::vector<Coedge> &loop) {
      for (const Coedge use : loop) {
        const Edge &e = *edge(model, use.edge);
        std::string error;
        const auto hits = intersect_edge_plane(model, e, ray_plane, {0, 0}, &error);
        if (!hits)
          throw std::runtime_error(error);
        if (hits->state == EdgePlaneState::Coincident || hits->state == EdgePlaneState::Uncertain)
        {
          retry = true;
          return;
        }
        const Curve &c = *curve(model, e.curve);
        for (const double t : hits->parameters) {
          if (dot(evaluate(c, t) - point, direction) <= 0)
            continue;
          if (t == e.start || t == e.end) {
            retry = true;
            return;
          }
          if (hits->tangent ||
              std::abs(dot(tangent(c, t), ray_normal)) <= 1e-13 * length(tangent(c, t)))
            continue;
          ++crossings;
        }
      }
    };
    count(face.outer);
    for (const auto &hole : face.holes)
      count(hole);
    if (!retry)
      return crossings % 2 != 0;
  }
  throw std::runtime_error("Native face intersection could not classify a section interval");
}

std::vector<std::vector<Coedge>> close_loops(const Model &model, const std::vector<Coedge> &uses)
{
  std::map<Id, Coedge> outgoing;
  for (const Coedge use : uses) {
    if (!outgoing.emplace(first_vertex(model, use), use).second)
      throw std::runtime_error("Native face split has an ambiguous tangent junction");
  }
  std::vector<std::vector<Coedge>> loops;
  while (!outgoing.empty()) {
    const Id start = outgoing.begin()->first;
    Id current = start;
    std::vector<Coedge> loop;
    do {
      auto next = outgoing.find(current);
      if (next == outgoing.end())
        throw std::runtime_error("Native face split could not close a trim loop");
      const Coedge use = next->second;
      loop.push_back(use);
      outgoing.erase(next);
      current = last_vertex(model, use);
    } while (current != start);
    loops.push_back(std::move(loop));
  }
  return loops;
}

}  // namespace

std::optional<BooleanFaceSplit> boolean_split_face_by_plane(const Model &source,
                                                            const Id face_id,
                                                            const Surface &plane,
                                                            std::string *error)
{
  try {
    std::string message;
    if (!validate(source, &message))
      throw std::runtime_error(message);
    const Face *input = face(source, face_id);
    const Surface *support = input ? surface(source, input->surface) : nullptr;
    if (!support ||
        (support->kind != SurfaceKind::Plane && support->kind != SurfaceKind::Cylinder) ||
        plane.kind != SurfaceKind::Plane)
      throw std::runtime_error("Native face split requires a plane and a supported face surface");
    const auto section = intersect_surfaces(source, *support, plane, {0, 0}, &message);
    if (!section)
      throw std::runtime_error(message);
    BooleanFaceSplit result;
    Model &model = result.model;
    model = source;
    model.faces.clear();
    const Vec3 n = cross(plane.u, plane.v) / length(cross(plane.u, plane.v));
    const Vec3 face_normal = cross(support->u, support->v) / length(cross(support->u, support->v));
    if (section->state == SurfaceIntersectionState::Coincident ||
        section->state == SurfaceIntersectionState::Uncertain)
      throw std::runtime_error("Coincident face regions need Boolean coincidence classification");

    std::vector<Coedge> negative, positive;
    std::set<Id> section_vertices;
    std::vector<Id> coincident_edges;
    const bool cylindrical = support->kind == SurfaceKind::Cylinder;
    const double winding = loop_area(source, input->outer, face_normal, support);
    if (winding == 0)
      throw std::runtime_error("Cannot split a zero-area native face");
    auto split_loop = [&](const std::vector<Coedge> &loop) {
      for (const Coedge use : loop) {
        const Edge original = *edge(source, use.edge);
        const Curve &c = *curve(source, original.curve);
        const auto hits = intersect_edge_plane(source, original, plane, {0, 0}, &message);
        if (!hits)
          throw std::runtime_error(message);
        if (hits->state == EdgePlaneState::Uncertain)
          throw std::runtime_error("Trim edge contact needs resolved intersection data");
        if (hits->state == EdgePlaneState::Coincident) {
          const double mid = original.start + (original.end - original.start) / 2;
          const Vec3 midpoint = evaluate(c, mid);
          Vec3 normal = face_normal;
          if (cylindrical) {
            const Vec3 q = CylinderChart(*support).local(midpoint);
            normal = cross(support->u * (-q.y) + support->v * q.x, support->w);
          }
          const Vec3 traversal = tangent(c, mid) *
                                 ((original.end - original.start) * (use.reversed ? -1 : 1));
          const double interior = dot(cross(normal, traversal), n) * (winding < 0 ? -1 : 1);
          if (interior == 0)
            throw std::runtime_error("Coincident trim edge has an unresolved tangent interior");
          (interior < 0 ? negative : positive).push_back(use);
          section_vertices.insert(original.vertices.begin(), original.vertices.end());
          coincident_edges.push_back(original.id);
          continue;
        }
        std::vector<BooleanPave> paves;
        for (double t : hits->parameters) {
          Id vertex_id;
          if (t == original.start)
            vertex_id = original.vertices[0];
          else if (t == original.end)
            vertex_id = original.vertices[1];
          else {
            vertex_id = model.next_id++;
            model.vertices.push_back({vertex_id, evaluate(c, t)});
          }
          paves.push_back({vertex_id, t});
          // An exact tangency does not cross the face boundary.
          if (!hits->tangent)
            section_vertices.insert(vertex_id);
        }
        const auto blocks = boolean_pave_blocks(original.id,
                                                {original.vertices[0], original.start},
                                                {original.vertices[1], original.end},
                                                paves,
                                                &message);
        if (!blocks)
          throw std::runtime_error(message);
        std::vector<Coedge> parts;
        for (const BooleanPaveBlock &block : *blocks) {
          Edge part = original;
          part.id = model.next_id++;
          part.vertices = {block.first.vertex, block.last.vertex};
          part.start = block.first.parameter;
          part.end = block.last.parameter;
          model.edges.push_back(part);
          parts.push_back({part.id, use.reversed});
        }
        if (use.reversed)
          std::reverse(parts.begin(), parts.end());
        for (const Coedge part : parts) {
          const Edge &e = *edge(model, part.edge);
          const double side = dot(evaluate(c, e.start + (e.end - e.start) / 2) - plane.origin, n);
          (side < 0 ? negative : positive).push_back(part);
        }
      }
    };
    split_loop(input->outer);
    for (const auto &hole : input->holes)
      split_loop(hole);

    const double center = cylindrical ? cylinder_center(source, *input, CylinderChart(*support)) :
                                        0;
    auto on_coincident_trim = [&](Vec3 point) {
      for (Id id : coincident_edges) {
        const Edge &e = *edge(source, id);
        const Curve &c = *curve(source, e.curve);
        const double epsilon =
            256 * std::numeric_limits<double>::epsilon() *
            std::max({1., length(point), length(c.origin), length(c.u), length(c.v)});
        double t;
        if (c.kind == CurveKind::Line) {
          t = dot(point - c.origin, c.u) / dot(c.u, c.u);
        }
        else if (c.kind == CurveKind::Conic) {
          const Vec3 q = point - c.origin;
          const double uu = dot(c.u, c.u), vv = dot(c.v, c.v), uv = dot(c.u, c.v);
          const double det = uu * vv - uv * uv;
          const double x = (dot(q, c.u) * vv - dot(q, c.v) * uv) / det;
          const double y = (dot(q, c.v) * uu - dot(q, c.u) * uv) / det;
          t = std::atan2(y, x);
          const double period = 2 * std::numbers::pi;
          t += period * std::ceil((std::min(e.start, e.end) - t) / period);
        }
        else
          throw std::runtime_error("Coincident trim needs a native curve parameter projection");
        if (t >= std::min(e.start, e.end) && t <= std::max(e.start, e.end) &&
            length(evaluate(c, t) - point) <= epsilon)
          return true;
      }
      return false;
    };
    for (Curve connector : section->curves) {
      std::vector<std::pair<double, Id>> ordered;
      for (Id id : section_vertices) {
        const Vec3 p = vertex(model, id)->position;
        double parameter;
        if (connector.kind == CurveKind::Conic) {
          const Vec3 q = CylinderChart(*support).local(p);
          parameter = std::atan2(q.y, q.x);
          const double period = 2 * std::numbers::pi;
          parameter += period * std::round((center - parameter) / period);
        }
        else {
          parameter = dot(p - connector.origin, connector.u) / dot(connector.u, connector.u);
          if (length(evaluate(connector, parameter) - p) > 1e-8 * std::max(1., length(p)))
            continue;
        }
        ordered.push_back({parameter, id});
      }
      std::sort(ordered.begin(), ordered.end());
      connector.id = model.next_id++;
      model.curves.push_back(connector);
      for (size_t i = 1; i < ordered.size(); ++i) {
        const auto [a, ai] = ordered[i - 1];
        const auto [b, bi] = ordered[i];
        if (a == b)
          continue;
        const Vec3 midpoint = evaluate(connector, a + (b - a) / 2);
        if (on_coincident_trim(midpoint))
          continue;
        if (!contains(source, *input, midpoint, *support))
          continue;
        Vec3 normal = face_normal;
        if (cylindrical) {
          const Vec3 q = CylinderChart(*support).local(midpoint);
          normal = cross(support->u * (-q.y) + support->v * q.x, support->w);
        }
        const bool reversed = (dot(cross(normal, n), tangent(connector, (a + b) / 2)) < 0) !=
                              (winding < 0);
        const Id eid = model.next_id++;
        model.edges.push_back({eid, connector.id, {ai, bi}, a, b});
        negative.push_back({eid, reversed});
        positive.push_back({eid, !reversed});
      }
    }
    auto build_faces = [&](const std::vector<Coedge> &uses, std::vector<Id> &ids) {
      auto loops = close_loops(model, uses);
      for (const auto &outer : loops) {
        if (loop_area(model, outer, face_normal, support) * winding <= 0)
          continue;
        Face piece = *input;
        piece.id = model.next_id++;
        piece.group = 0;
        piece.outer = outer;
        piece.holes.clear();
        for (const auto &hole : loops) {
          if (loop_area(model, hole, face_normal, support) * winding >= 0)
            continue;
          const Vec3 point = vertex(model, first_vertex(model, hole.front()))->position;
          if (contains(model, piece, point, *support))
            piece.holes.push_back(hole);
        }
        ids.push_back(piece.id);
        model.faces.push_back(std::move(piece));
      }
    };
    build_faces(negative, result.negative);
    build_faces(positive, result.positive);
    if (model.faces.size() == 1 && (result.negative.empty() || result.positive.empty())) {
      // Contact without a material split retains the source trim topology.
      model.faces[0].outer = input->outer;
      model.faces[0].holes = input->holes;
    }

    // Discard unreferenced source topology. Shared section edges remain one
    // native edge referenced in opposite directions by neighboring pieces.
    std::set<Id> used_edges, used_vertices, used_curves;
    for (const Face &f : model.faces) {
      for (Coedge use : f.outer)
        used_edges.insert(use.edge);
      for (const auto &hole : f.holes)
        for (Coedge use : hole)
          used_edges.insert(use.edge);
    }
    std::erase_if(model.edges, [&](const Edge &e) { return !used_edges.contains(e.id); });
    for (const Edge &e : model.edges) {
      used_vertices.insert(e.vertices.begin(), e.vertices.end());
      used_curves.insert(e.curve);
    }
    std::erase_if(model.vertices, [&](const Vertex &v) { return !used_vertices.contains(v.id); });
    std::erase_if(model.curves, [&](const Curve &c) { return !used_curves.contains(c.id); });
    std::erase_if(model.surfaces, [&](const Surface &s) { return s.id != support->id; });
    if (!validate(model, &message))
      throw std::runtime_error(message);
    return result;
  }
  catch (const std::runtime_error &exception) {
    if (error)
      *error = exception.what();
    return {};
  }
}

std::optional<BooleanFaceSplit> boolean_split_planar_face(const Model &source,
                                                          Id id,
                                                          const Surface &plane,
                                                          std::string *error)
{
  const Face *f = face(source, id);
  const Surface *s = f ? surface(source, f->surface) : nullptr;
  if (!s || s->kind != SurfaceKind::Plane) {
    if (error)
      *error = "Native planar face split requires a planar source face";
    return {};
  }
  return boolean_split_face_by_plane(source, id, plane, error);
}

}  // namespace blender::solid
