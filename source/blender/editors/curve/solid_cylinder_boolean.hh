/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

namespace blender::ed::solid_editor::cylinder_boolean {
using namespace solid;
constexpr double tau = 2 * std::numbers::pi;
struct Cylinder {
  Vec3 center, axis, radial_x, radial_y;
  double radius_x, radius_y, low, high;
  /** Offsets around a planar prism profile; empty for an ellipse. */
  std::vector<Vec3> polygon;
};
inline std::optional<Cylinder> recognize(const Model &model, const std::vector<Id> &faces)
{
  if (faces.size() != 3)
    return {};
  const Surface *wall = nullptr;
  for (Id id : faces) {
    const Surface *s = surface(model, face(model, id)->surface);
    if (s->kind == SurfaceKind::Extrusion)
      wall = s;
    else if (s->kind != SurfaceKind::Plane)
      return {};
  }
  if (!wall)
    return {};
  const solid::Curve *c = curve(model, wall->curve);
  if (!c || c->kind != CurveKind::Conic || std::abs(wall->end - wall->start - tau) > 1e-9)
    return {};
  const double rx = length(c->u), ry = length(c->v), h = length(wall->u);
  if (!(rx > 0 && ry > 0 && h > 0) ||
      std::abs(dot(c->u, c->v)) > rx * ry * 1e-10 ||
      std::abs(dot(c->u, wall->u)) > rx * h * 1e-10 ||
      std::abs(dot(c->v, wall->u)) > ry * h * 1e-10)
    return {};
  return Cylinder{c->origin, wall->u / h, c->u / rx, c->v / ry, rx, ry, 0, h, {}};
}

inline std::optional<Cylinder> recognize_prism(const Model &model,
                                              const std::vector<Id> &faces,
                                              Vec3 axis)
{
  std::vector<const Face *> caps;
  for (Id id : faces) {
    const Face &f = *face(model, id);
    const Surface &s = *surface(model, f.surface);
    if (s.kind != SurfaceKind::Plane || !f.holes.empty())
      return {};
    const Vec3 normal = cross(s.u, s.v) / length(cross(s.u, s.v));
    const double alignment = std::abs(dot(normal, axis));
    if (alignment > 1 - 1e-10)
      caps.push_back(&f);
    else if (alignment > 1e-10)
      return {};
    for (Coedge use : f.outer)
      if (curve(model, edge(model, use.edge)->curve)->kind != CurveKind::Line)
        return {};
  }
  if (caps.size() != 2)
    return {};
  auto ring = [&](const Face &f) {
    std::vector<Vec3> result;
    for (Coedge use : f.outer) {
      const Edge &e = *edge(model, use.edge);
      result.push_back(vertex(model, e.vertices[use.reversed ? 1 : 0])->position);
    }
    return result;
  };
  auto lower = ring(*caps[0]), upper = ring(*caps[1]);
  if (lower.size() < 3 || upper.size() != lower.size() || faces.size() != lower.size() + 2)
    return {};
  if (dot(upper[0] - lower[0], axis) < 0)
    std::swap(lower, upper);
  const Vec3 center = lower[0];
  const double height = dot(upper[0] - center, axis);
  if (height < 1e-10)
    return {};
  for (Vec3 p : lower) {
    if (std::abs(dot(p - center, axis)) > 1e-9 ||
        std::none_of(upper.begin(), upper.end(), [&](Vec3 q) {
          return length(q - p - axis * height) < 1e-9;
        }))
      return {};
  }
  double area = 0;
  for (size_t i = 0; i < lower.size(); ++i)
    area += dot(cross(lower[i] - center, lower[(i + 1) % lower.size()] - center), axis);
  if (area < 0)
    std::reverse(lower.begin(), lower.end());
  for (Vec3 &p : lower)
    p = p - center;
  Vec3 x = lower[1] - lower[0];
  x = x / length(x);
  return Cylinder{center, axis, x, cross(axis, x), 1, 1, 0, height, std::move(lower)};
}

/* Exact circular and elliptical boundaries in a shared frame. Only intersection events create
 * topology;
 * viewport sampling density never determines source vertices, edges or faces. */
struct Builder {
  Model &m;
  Vec3 axis, x, y;
  std::array<Cylinder, 2> c;
  std::array<std::vector<double>, 2> cuts;
  struct SavedEdge {
    Id id;
    Vec3 midpoint;
  };
  std::vector<SavedEdge> saved;
  std::array<std::vector<Id>, 2> walls;
  size_t first_vertex = 0;
  bool nested_circular_cross_section = false;
  Id vertex_at(Vec3 p)
  {
    for (size_t i = first_vertex; i < m.vertices.size(); ++i)
      if (length(m.vertices[i].position - p) < 1e-10 * std::max(1.0, length(p)))
        return m.vertices[i].id;
    Id id = m.next_id++;
    m.vertices.push_back({id, p});
    return id;
  }
  Vec3 point(int i, double t, double z) const
  {
    if (!c[i].polygon.empty()) {
      const int count = int(c[i].polygon.size());
      t = std::fmod(t, double(count));
      if (t < 0)
        t += count;
      const int j = int(std::floor(t));
      return c[i].center + axis * z + c[i].polygon[j] * (1 - (t - j)) +
             c[i].polygon[(j + 1) % count] * (t - j);
    }
    return c[i].center + axis * z + c[i].radial_x * (std::cos(t) * c[i].radius_x) +
           c[i].radial_y * (std::sin(t) * c[i].radius_y);
  }
  Id edge_at(solid::Curve curve, double a, double b)
  {
    Vec3 p = evaluate(curve, a), q = evaluate(curve, b), mid = evaluate(curve, (a + b) / 2);
    Id va = vertex_at(p), vb = vertex_at(q);
    for (const SavedEdge &s : saved) {
      const Edge &e = *edge(m, s.id);
      if ((e.vertices == std::array<Id, 2>{va, vb} ||
           e.vertices == std::array<Id, 2>{vb, va}) &&
          length(s.midpoint - mid) < 1e-10 * std::max(1.0, length(mid)))
        return e.id;
    }
    curve.id = m.next_id++;
    m.curves.push_back(curve);
    Id id = m.next_id++;
    m.edges.push_back({id, curve.id, {va, vb}, a, b});
    saved.push_back({id, mid});
    return id;
  }
  Id arc(int i, double a, double b, double z)
  {
    if (!c[i].polygon.empty())
      return rail(point(i, a, z), point(i, b, z));
    solid::Curve curve;
    curve.kind = CurveKind::Conic;
    curve.origin = c[i].center + axis * z;
    curve.u = c[i].radial_x * c[i].radius_x;
    curve.v = c[i].radial_y * c[i].radius_y;
    return edge_at(curve, a, b);
  }
  Id rail(Vec3 p, Vec3 q)
  {
    solid::Curve c;
    c.origin = p;
    c.u = q - p;
    return edge_at(c, 0, 1);
  }
  bool inside_other(int i, double t) const
  {
    Vec3 delta = point(i, t, 0) - c[1 - i].center;
    const Cylinder &other = c[1 - i];
    if (!other.polygon.empty()) {
      bool inside = false;
      for (size_t j = 0, k = other.polygon.size() - 1; j < other.polygon.size(); k = j++) {
        const Vec3 a = other.polygon[j], b = other.polygon[k];
        const double ay = dot(a, y), by = dot(b, y), py = dot(delta, y);
        if ((ay > py) != (by > py) &&
            dot(delta, x) < dot(a, x) + (py - ay) * dot(b - a, x) / (by - ay))
          inside = !inside;
      }
      return inside;
    }
    const double u = dot(delta, other.radial_x) / other.radius_x;
    const double v = dot(delta, other.radial_y) / other.radius_y;
    return u * u + v * v < 1.0;
  }
  bool value(int op, bool a, bool b) const
  {
    return op == 0 ? a || b : op == 1 ? a && !b : a && b;
  }
  bool value_at(int op, int i, bool own, bool other, double z) const
  {
    bool a = (i == 0 ? own : other) && z > c[0].low && z < c[0].high;
    bool b = (i == 1 ? own : other) && z > c[1].low && z < c[1].high;
    return value(op, a, b);
  }
  void wall(int i, double a, double b, double z0, double z1, int sign)
  {
    Id bottom = arc(i, a, b, z0), top = arc(i, a, b, z1);
    Id end = rail(point(i, b, z0), point(i, b, z1)),
       start = rail(point(i, a, z0), point(i, a, z1));
    const Edge e = *edge(m, bottom);
    Surface s;
    s.id = m.next_id++;
    s.kind = SurfaceKind::Extrusion;
    s.curve = e.curve;
    s.start = a;
    s.end = b;
    s.u = axis * (z1 - z0);
    if (!c[i].polygon.empty()) {
      s.kind = SurfaceKind::Plane;
      s.curve = 0;
      s.origin = point(i, a, z0);
      s.u = point(i, b, z0) - s.origin;
      s.v = axis * (z1 - z0);
    }
    m.surfaces.push_back(s);
    Id id = m.next_id++;
    m.faces.push_back(
        {id, s.id, {{bottom, false}, {end, false}, {top, true}, {start, true}}, {}, sign});
    walls[i].push_back(id);
  }
  bool cap(int op, double z, double below, double above, int sign, std::string *error)
  {
    std::vector<Coedge> uses;
    for (int i = 0; i < 2; ++i)
      for (size_t j = 0; j + 1 < cuts[i].size(); ++j) {
        double a = cuts[i][j], b = cuts[i][j + 1];
        bool other = inside_other(i, (a + b) / 2);
        auto region = [&](bool own) {
          bool lo = value_at(op, i, own, other, below), hi = value_at(op, i, own, other, above);
          return sign > 0 ? lo && !hi : hi && !lo;
        };
        bool inner = region(true), outer = region(false);
        if (inner != outer)
          uses.push_back({arc(i, a, b, z), !inner});
      }
    std::vector<std::vector<Coedge>> loops;
    while (!uses.empty()) {
      std::vector<Coedge> loop{uses.back()};
      uses.pop_back();
      auto endpoint = [&](Coedge u, bool end) {
        return edge(m, u.edge)->vertices[(end != u.reversed) ? 1 : 0];
      };
      while (endpoint(loop.back(), true) != endpoint(loop.front(), false)) {
        Id next = endpoint(loop.back(), true);
        auto it = std::find_if(
            uses.begin(), uses.end(), [&](Coedge u) { return endpoint(u, false) == next; });
        if (it == uses.end()) {
          if (error)
            *error = "Cylinder Boolean cap boundary could not be joined";
          return false;
        }
        loop.push_back(*it);
        uses.erase(it);
      }
      loops.push_back(std::move(loop));
    }
    auto area = [&](const std::vector<Coedge> &loop) {
      double value = 0;
      for (Coedge use : loop) {
        const Edge &e = *edge(m, use.edge);
        const solid::Curve &curve = *solid::curve(m, e.curve);
        Vec3 origin = curve.origin - c[0].center;
        double a = e.start, b = e.end;
        auto cross_2d = [&](Vec3 u, Vec3 v) {
          return dot(u, x) * dot(v, y) - dot(u, y) * dot(v, x);
        };
        const double term = curve.kind == CurveKind::Line ?
                                cross_2d(evaluate(curve, a) - c[0].center,
                                         evaluate(curve, b) - c[0].center) :
                            cross_2d(origin, curve.u) * (std::cos(a) - std::cos(b)) +
                            cross_2d(origin, curve.v) * (std::sin(b) - std::sin(a)) +
                            cross_2d(curve.u, curve.v) * (b - a);
        value += (use.reversed ? -1 : 1) * term;
      }
      return value;
    };
    std::vector<double> areas;
    for (const auto &loop : loops)
      areas.push_back(area(loop));
    std::vector<int> negative, positive;
    for (int i = 0; i < int(areas.size()); ++i)
      (areas[i] < 0 ? negative : positive).push_back(i);
    if (nested_circular_cross_section && loops.size() == 2 && negative.size() == 1 &&
        positive.size() == 1)
    {
      if (negative.size() != 1 || positive.size() != 1 || loops[negative[0]].size() != 2 ||
          loops[positive[0]].size() != 2 || c[0].polygon.size() || c[1].polygon.size())
      {
        if (error)
          *error = "This Boolean cap has a hole pattern that needs analytic trimming";
        return false;
      }
      /* Split a circular annulus into two planar sectors. The radial seams keep the source face
       * representation simple while preserving exact circular boundaries. */
      auto start_vertex = [&](Coedge use) {
        return edge(m, use.edge)->vertices[use.reversed ? 1 : 0];
      };
      auto end_vertex = [&](Coedge use) {
        return edge(m, use.edge)->vertices[use.reversed ? 0 : 1];
      };
      const int outer_index = areas[positive[0]] * sign > 0 ? positive[0] : negative[0];
      const int hole_index = outer_index == positive[0] ? negative[0] : positive[0];
      std::vector<Coedge> outer = loops[outer_index], hole = loops[hole_index];
      std::array<bool, 2> used_holes{};
      auto bridge = [&](Vec3 from, Vec3 to) {
        const Id id = rail(from, to);
        return Coedge{id, edge(m, id)->vertices[0] != vertex_at(from)};
      };
      for (Coedge outer_arc : outer) {
        const Vec3 os = vertex(m, start_vertex(outer_arc))->position;
        const Vec3 oe = vertex(m, end_vertex(outer_arc))->position;
        int best = -1;
        double best_cost = std::numeric_limits<double>::infinity();
        for (int j = 0; j < 2; ++j) {
          if (used_holes[j])
            continue;
          const Vec3 hs = vertex(m, start_vertex(hole[j]))->position;
          const Vec3 he = vertex(m, end_vertex(hole[j]))->position;
          const double cost = length(oe - hs) + length(he - os);
          if (cost < best_cost) {
            best = j;
            best_cost = cost;
          }
        }
        if (best < 0)
          return false;
        used_holes[best] = true;
        const Coedge hole_arc = hole[best];
        const Coedge outer_to_hole = bridge(oe, vertex(m, start_vertex(hole_arc))->position);
        const Coedge hole_to_outer = bridge(vertex(m, end_vertex(hole_arc))->position, os);
        Surface s;
        s.id = m.next_id++;
        s.kind = SurfaceKind::Plane;
        s.origin = c[0].center + axis * z;
        s.u = x;
        s.v = y;
        m.surfaces.push_back(s);
        m.faces.push_back({m.next_id++,
                           s.id,
                           {outer_arc,
                            outer_to_hole,
                            hole_arc,
                            hole_to_outer},
                           {},
                           sign});
      }
      return true;
    }
    if (nested_circular_cross_section && loops.size() > 1 && !negative.empty()) {
      if (error)
        *error = "This Boolean cap has multiple internal loops that need analytic trimming";
      return false;
    }
    for (std::vector<Coedge> &loop : loops) {
      const int orientation = nested_circular_cross_section && area(loop) < 0 ? -sign : sign;
      Surface s;
      s.id = m.next_id++;
      s.kind = SurfaceKind::Plane;
      s.origin = c[0].center + axis * z;
      s.u = x;
      s.v = y;
      m.surfaces.push_back(s);
      m.faces.push_back({m.next_id++, s.id, std::move(loop), {}, orientation});
    }
    return true;
  }
  bool run(int op, std::string *error)
  {
    std::vector<double> z{c[0].low, c[0].high, c[1].low, c[1].high};
    std::sort(z.begin(), z.end());
    z.erase(std::unique(
                z.begin(), z.end(), [](double a, double b) { return std::abs(a - b) < 1e-10; }),
            z.end());
    for (int i = 0; i < 2; ++i)
      for (size_t j = 0; j + 1 < cuts[i].size(); ++j) {
        double a = cuts[i][j], b = cuts[i][j + 1];
        bool other = inside_other(i, (a + b) / 2);
        for (size_t k = 0; k + 1 < z.size(); ++k) {
          double mid = (z[k] + z[k + 1]) / 2;
          int sign = int(value_at(op, i, true, other, mid)) -
                     int(value_at(op, i, false, other, mid));
          if (sign)
            wall(i, a, b, z[k], z[k + 1], sign);
        }
      }
    for (size_t k = 0; k < z.size(); ++k) {
      double below = k ? (z[k - 1] + z[k]) / 2 : z[k] - 1;
      double above = k + 1 < z.size() ? (z[k] + z[k + 1]) / 2 : z[k] + 1;
      if (!cap(op, z[k], below, above, 1, error) || !cap(op, z[k], below, above, -1, error))
        return false;
    }
    for (int i = 0; i < 2; ++i) {
      if (!c[i].polygon.empty())
        continue;
      const auto &ids = walls[i];
      std::vector<Id> remaining = ids;
      while (!remaining.empty()) {
        Id group = remaining.back();
        remaining.pop_back();
        std::vector<Id> stack{group};
        while (!stack.empty()) {
          Id current = stack.back();
          stack.pop_back();
          for (Face &f : m.faces)
            if (f.id == current)
              f.group = group;
          const Face *f = face(m, current);
          for (auto it = remaining.begin(); it != remaining.end();) {
            const Face *other = face(m, *it);
            bool adjacent = false;
            for (Coedge a : f->outer)
              for (Coedge b : other->outer)
                adjacent |= a.edge == b.edge;
            if (adjacent) {
              stack.push_back(*it);
              it = remaining.erase(it);
            }
            else
              ++it;
          }
        }
      }
    }
    return true;
  }
};
inline bool append(Model &out, Cylinder a, Cylinder b, int op, std::string *error)
{
  Vec3 axis = a.axis, x = a.radial_x, y = cross(axis, x);
  if (length(cross(axis, b.axis)) > 1e-10) {
    if (error)
      *error = "Analytic cylinder Boolean requires parallel axes";
    return false;
  }
  double offset = dot(b.center - a.center, axis);
  double extent = dot(b.axis, axis) * (b.high - b.low);
  b.center = b.center - axis * offset;
  b.low = std::min(offset, offset + extent);
  b.high = std::max(offset, offset + extent);
  Builder build{out, axis, x, y, {a, b}, {}, {}, {}};
  Vec3 delta = b.center - a.center;
  double distance = length(delta);
  build.first_vertex = out.vertices.size();
  if (!a.polygon.empty() || !b.polygon.empty()) {
    const int p = a.polygon.empty() ? 1 : 0, e = 1 - p;
    const Cylinder &ellipse = build.c[e];
    const int count = int(build.c[p].polygon.size());
    for (int j = 0; j <= count; ++j)
      build.cuts[p].push_back(double(j));
    for (int j = 0; j < count; ++j) {
      const Vec3 start = build.point(p, j, 0), end = build.point(p, j + 1, 0);
      const Vec3 d = start - ellipse.center, direction = end - start;
      const double u = dot(d, ellipse.radial_x) / ellipse.radius_x;
      const double v = dot(d, ellipse.radial_y) / ellipse.radius_y;
      const double du = dot(direction, ellipse.radial_x) / ellipse.radius_x;
      const double dv = dot(direction, ellipse.radial_y) / ellipse.radius_y;
      const double aa = du * du + dv * dv, bb = 2 * (u * du + v * dv);
      const double cc = u * u + v * v - 1;
      const double discriminant = bb * bb - 4 * aa * cc;
      if (discriminant <= 1e-14 * aa * aa)
        continue;
      for (double root : {(-bb - std::sqrt(discriminant)) / (2 * aa),
                          (-bb + std::sqrt(discriminant)) / (2 * aa)})
      {
        if (root < -1e-10 || root > 1 + 1e-10)
          continue;
        root = std::clamp(root, 0.0, 1.0);
        build.cuts[p].push_back(j + root);
        double angle = std::atan2(v + dv * root, u + du * root);
        if (angle < 0)
          angle += tau;
        build.cuts[e].push_back(angle);
      }
    }
    for (int i = 0; i < 2; ++i) {
      auto &cuts = build.cuts[i];
      std::sort(cuts.begin(), cuts.end());
      cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double lhs, double rhs) {
                   return std::abs(lhs - rhs) < 1e-9;
                 }),
                 cuts.end());
    }
    if (build.cuts[e].empty())
      build.cuts[e] = {0, tau};
    else
      build.cuts[e].push_back(build.cuts[e].front() + tau);
    return build.run(op, error);
  }
  const bool circles = std::abs(a.radius_x - a.radius_y) < a.radius_x * 1e-10 &&
                       std::abs(b.radius_x - b.radius_y) < b.radius_x * 1e-10;
  const bool aligned_circle_frames = circles && dot(a.radial_x, b.radial_x) > 1.0 - 1e-10 &&
                                     dot(a.radial_y, b.radial_y) > 1.0 - 1e-10 &&
                                     dot(a.radial_y, y) > 1.0 - 1e-10;
  if (aligned_circle_frames && distance > std::abs(a.radius_x - b.radius_x) + 1e-10 &&
      distance < a.radius_x + b.radius_x - 1e-10)
  {
    double along = (a.radius_x * a.radius_x - b.radius_x * b.radius_x + distance * distance) /
                   (2 * distance);
    double h = std::sqrt(std::max(0.0, a.radius_x * a.radius_x - along * along));
    Vec3 center = a.center + delta * (along / distance), side = cross(axis, delta / distance) * h;
    for (int i = 0; i < 2; ++i) {
      for (Vec3 p : {center + side, center - side}) {
        Vec3 d = p - build.c[i].center;
        double t = std::atan2(dot(d, y), dot(d, x));
        if (t < 0)
          t += tau;
        build.cuts[i].push_back(t);
      }
      std::sort(build.cuts[i].begin(), build.cuts[i].end());
      build.cuts[i].push_back(build.cuts[i][0] + tau);
    }
  }
  else {
    /* Find crossings of the exact conic boundaries. Sampling is only used to bracket roots;
     * intersection edges remain analytic conics and the sampling does not create topology. */
    constexpr int root_brackets = 2048;
    auto implicit = [&](int i, double t) {
      Vec3 delta = build.point(i, t, 0) - build.c[1 - i].center;
      const Cylinder &other = build.c[1 - i];
      const double u = dot(delta, other.radial_x) / other.radius_x;
      const double v = dot(delta, other.radial_y) / other.radius_y;
      return u * u + v * v - 1.0;
    };
    double nearest_boundary = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 2; ++i) {
      std::vector<double> roots;
      double t0 = 0.0, f0 = implicit(i, t0);
      nearest_boundary = std::min(nearest_boundary, std::abs(f0));
      for (int j = 1; j <= root_brackets; ++j) {
        const double t1 = tau * j / root_brackets;
        const double f1 = implicit(i, t1);
        nearest_boundary = std::min(nearest_boundary, std::abs(f1));
        if ((f0 < 0) != (f1 < 0)) {
          double lo = t0, hi = t1, flo = f0;
          for (int k = 0; k < 64; ++k) {
            const double mid = (lo + hi) * 0.5;
            const double fm = implicit(i, mid);
            if ((flo < 0) != (fm < 0))
              hi = mid;
            else {
              lo = mid;
              flo = fm;
            }
          }
          roots.push_back((lo + hi) * 0.5);
        }
        t0 = t1;
        f0 = f1;
      }
      std::sort(roots.begin(), roots.end());
      roots.erase(std::unique(roots.begin(),
                              roots.end(),
                              [](double lhs, double rhs) { return std::abs(lhs - rhs) < 1e-9; }),
                  roots.end());
      if (roots.size() > 1 && roots.size() % 2 == 0) {
        build.cuts[i] = std::move(roots);
        build.cuts[i].push_back(build.cuts[i][0] + tau);
      }
      else if (!roots.empty()) {
        if (error)
          *error = "Elliptical cylinder intersection could not be resolved safely";
        return false;
      }
    }
    const bool any_roots = !build.cuts[0].empty() || !build.cuts[1].empty();
    if (!any_roots && nearest_boundary < 1e-10) {
      if (error)
        *error = "Tangent cylinder Boolean needs trimmed cap support";
      return false;
    }
    if (any_roots) {
      if (build.cuts[0].empty() || build.cuts[1].empty()) {
        if (error)
          *error = "Elliptical cylinder intersection could not be resolved safely";
        return false;
      }
    }
    else {
      const bool nested = build.inside_other(0, 0) || build.inside_other(1, 0);
      if (nested && !circles) {
        if (error)
          *error = "Nested elliptical cylinder Boolean needs analytic cap trimming";
        return false;
      }
      if (nested) {
        build.nested_circular_cross_section = true;
        const Vec3 direction = distance > 1e-10 ? delta / distance : x;
        for (int i = 0; i < 2; ++i) {
          const Vec3 radial = i == 0 ? direction : direction * -1;
          double angle = std::atan2(dot(radial, build.c[i].radial_y),
                                    dot(radial, build.c[i].radial_x));
          if (angle < 0)
            angle += tau;
          build.cuts[i] = {angle, angle + std::numbers::pi, angle + tau};
        }
      }
      else
        build.cuts = {std::vector<double>{0, tau}, std::vector<double>{0, tau}};
    }
  }
  return build.run(op, error);
}
}  // namespace blender::ed::solid_editor::cylinder_boolean
