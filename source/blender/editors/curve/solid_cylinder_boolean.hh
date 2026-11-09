/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

namespace blender::ed::solid_editor::cylinder_boolean {
using namespace solid;
constexpr double tau = 2 * std::numbers::pi;
struct Cylinder {
  Vec3 center, axis, radial;
  double radius, low, high;
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
  const double r = length(c->u), h = length(wall->u);
  if (!(r > 0 && h > 0) || std::abs(length(c->v) - r) > r * 1e-10 ||
      std::abs(dot(c->u, c->v)) > r * r * 1e-10 || std::abs(dot(c->u, wall->u)) > r * h * 1e-10 ||
      std::abs(dot(c->v, wall->u)) > r * h * 1e-10)
    return {};
  return Cylinder{c->origin, wall->u / h, c->u / r, r, 0, h};
}

/* Exact circular boundaries in a shared frame. Only intersection events create topology;
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
    return c[i].center + axis * z + (x * std::cos(t) + y * std::sin(t)) * c[i].radius;
  }
  Id edge_at(solid::Curve curve, double a, double b)
  {
    Vec3 p = evaluate(curve, a), q = evaluate(curve, b), mid = evaluate(curve, (a + b) / 2);
    Id va = vertex_at(p), vb = vertex_at(q);
    for (const SavedEdge &s : saved) {
      const Edge &e = *edge(m, s.id);
      if (e.vertices == std::array<Id, 2>{va, vb} &&
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
    solid::Curve curve;
    curve.kind = CurveKind::Conic;
    curve.origin = c[i].center + axis * z;
    curve.u = x * c[i].radius;
    curve.v = y * c[i].radius;
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
    return dot(delta, delta) < c[1 - i].radius * c[1 - i].radius;
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
      double area = 0;
      for (Coedge use : loop) {
        const Edge &e = *edge(m, use.edge);
        const solid::Curve &curve = *solid::curve(m, e.curve);
        Vec3 origin = curve.origin - c[0].center;
        double a = e.start, b = e.end;
        double r = length(curve.u);
        double term = r * (dot(origin, x) * (std::sin(b) - std::sin(a)) -
                           dot(origin, y) * (std::cos(b) - std::cos(a))) +
                      r * r * (b - a);
        area += (use.reversed ? -1 : 1) * term;
      }
      if (area < 0) {
        if (error)
          *error = "Cylinder Boolean with an internal circular hole needs trimmed cap support";
        return false;
      }
      Surface s;
      s.id = m.next_id++;
      s.kind = SurfaceKind::Plane;
      s.origin = c[0].center + axis * z;
      s.u = x;
      s.v = y;
      m.surfaces.push_back(s);
      m.faces.push_back({m.next_id++, s.id, std::move(loop), {}, sign});
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
    for (const auto &ids : walls) {
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
  Vec3 axis = a.axis, x = a.radial, y = cross(axis, x);
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
  if (distance > std::abs(a.radius - b.radius) + 1e-10 && distance < a.radius + b.radius - 1e-10) {
    double along = (a.radius * a.radius - b.radius * b.radius + distance * distance) /
                   (2 * distance);
    double h = std::sqrt(std::max(0.0, a.radius * a.radius - along * along));
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
    if (distance <= std::abs(a.radius - b.radius) + 1e-10) {
      if (error)
        *error = "Nested or tangent cylinder Boolean needs trimmed cap support";
      return false;
    }
    build.cuts = {std::vector<double>{0, tau}, std::vector<double>{0, tau}};
  }
  return build.run(op, error);
}
}  // namespace blender::ed::solid_editor::cylinder_boolean
