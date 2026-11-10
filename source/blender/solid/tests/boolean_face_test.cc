/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_boolean.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <numbers>
#include <set>

using namespace blender::solid;
static void require(bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static double area(const Model &model, const Face &face)
{
  double result = 0;
  auto add = [&](const std::vector<Coedge> &loop) {
    for (Coedge use : loop) {
      const Edge &e = *edge(model, use.edge);
      const Curve &c = *curve(model, e.curve);
      const double a = use.reversed ? e.end : e.start, b = use.reversed ? e.start : e.end;
      Vec3 integral = cross(c.origin, c.u) * (b - a);
      if (c.kind == CurveKind::Conic)
        integral = cross(c.origin, c.u) * (std::cos(b) - std::cos(a)) +
                   cross(c.origin, c.v) * (std::sin(b) - std::sin(a)) + cross(c.u, c.v) * (b - a);
      result += integral.z / 2;
    }
  };
  add(face.outer);
  for (const auto &hole : face.holes)
    add(hole);
  return result;
}

int main()
{
  {
    Model circle_model;
    Selection selected;
    std::string error;
    Curve circle{0, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}};
    require(
        append_curve(circle_model, circle, 0, 2 * std::numbers::pi, 0, 0, true, &selected, &error),
        error);
    auto profile = prepare_profile(circle_model, selected, &error);
    require(bool(profile), error);
    for (double height : {2., -2.}) {
      auto body = extrude(*profile, height, &error);
      require(bool(body), error);
      auto charts = boolean_partition_periodic_faces(body->model, &error);
      require(bool(charts), error);
      require(charts->faces.size() == 4,
              "Periodic wall did not become two bounded analytic charts");
      for (const Face &f : charts->faces)
        require(validate_body(*charts, f.id, &error), error);
      require(bool(tessellate(*charts, 64, &error)), error);
      int walls = 0;
      for (const Face &f : charts->faces) {
        if (surface(*charts, f.surface)->kind != SurfaceKind::Cylinder)
          continue;
        ++walls;
        Surface cut;
        cut.origin = {0, 0, height / 2};
        cut.u = {1, 0, .13};
        cut.v = {0, 1, .19};
        const auto split = boolean_split_face_by_plane(*charts, f.id, cut, &error);
        require(bool(split), "Periodic cylinder chart could not be split: " + error);
        require(!split->negative.empty() && !split->positive.empty(),
                "Periodic chart split lost a side");
      }
      require(walls == 2, "Periodic partition changed analytic wall types");
      require(body->model.faces.size() == 3, "Periodic partition mutated its source");
    }
  }
  // A cylindrical wall with a split boundary and an optional interior trim.
  // This cannot be represented by the original four-edge wall template.
  for (bool hole : {false, true}) {
    Model model;
    std::string error;
    auto ring = [&](double low, double high, double bottom, double top, bool split) {
      std::vector<Coedge> uses;
      Id first = 0, previous = 0;
      auto add = [&](Curve c, double a, double b, bool last) {
        Selection selection;
        require(
            append_curve(model, c, a, b, previous, last ? first : 0, false, &selection, &error),
            error);
        const Edge &e = model.edges.back();
        if (!first)
          first = e.vertices[0];
        previous = e.vertices[1];
        uses.push_back({e.id, false});
      };
      Curve lower{0, CurveKind::Conic, {0, 0, bottom}, {1, 0, 0}, {0, 1, 0}};
      if (split) {
        add(lower, low, 0, false);
        add(lower, 0, high, false);
      }
      else
        add(lower, low, high, false);
      add(*line({std::cos(high), std::sin(high), bottom}, {std::cos(high), std::sin(high), top}),
          0,
          1,
          false);
      add({0, CurveKind::Conic, {0, 0, top}, {1, 0, 0}, {0, 1, 0}}, high, low, false);
      add(*line({std::cos(low), std::sin(low), top}, {std::cos(low), std::sin(low), bottom}),
          0,
          1,
          true);
      return uses;
    };
    auto outer = ring(-.7, .7, 0, 1, true);
    std::vector<Coedge> inner;
    if (hole) {
      inner = ring(-.3, .3, .3, .7, false);
      std::reverse(inner.begin(), inner.end());
      for (Coedge &use : inner)
        use.reversed = !use.reversed;
    }
    Surface support;
    support.id = model.next_id++;
    support.kind = SurfaceKind::Cylinder;
    support.u = {1, 0, 0};
    support.v = {0, 1, 0};
    support.w = {0, 0, 1};
    model.surfaces.push_back(support);
    Face patch;
    patch.id = model.next_id++;
    patch.surface = support.id;
    patch.outer = outer;
    if (hole)
      patch.holes.push_back(inner);
    model.faces.push_back(patch);
    require(validate(model, &error), error);
    for (const Surface &cut :
         {Surface{0, SurfaceKind::Plane, {0, 0, .5}, {1, 0, .17}, {0, 1, -.13}},
          Surface{0, SurfaceKind::Plane, {0, 0, .3}, {1, 0, 0}, {0, 1, 0}},
          Surface{0, SurfaceKind::Plane, {.98, 0, 0}, {0, 1, 0}, {0, 0, 1}}})
    {
      const auto parts = boolean_split_face_by_plane(model, patch.id, cut, &error);
      require(bool(parts), "Cylinder wall splitting failed: " + error);
      require(!parts->negative.empty() && !parts->positive.empty(),
              "Cylinder wall cut lost one side of its trim");
      require(validate(parts->model, &error), error);
      const auto display_parts = tessellate(parts->model, 64, &error);
      require(bool(display_parts), error);
      double split_area = 0;
      for (const auto &polygon : display_parts->polygons)
        for (size_t i = 1; i + 1 < polygon.size(); ++i) {
          const Vec3 a = display_parts->positions[polygon[0]],
                     b = display_parts->positions[polygon[i]],
                     c = display_parts->positions[polygon[i + 1]];
          split_area += length(cross(b - a, c - a)) / 2;
        }
      require(std::abs(split_area - (hole ? 1.16 : 1.4)) < .01,
              "Cylinder wall split lost or duplicated a trimmed region");
      bool conic_section = false;
      for (const Face &part : parts->model.faces) {
        require(surface(parts->model, part.surface)->kind == SurfaceKind::Cylinder,
                "Wall splitting changed its native cylindrical surface");
        for (Coedge use : part.outer) {
          const Edge &e = *edge(parts->model, use.edge);
          if (curve(parts->model, e.curve)->kind == CurveKind::Conic)
            conic_section = true;
        }
      }
      require(conic_section, "Cylinder wall lost its analytic boundary arcs");
    }
    auto display = tessellate(model, 64, &error);
    require(bool(display), error);
    std::map<std::pair<int, int>, int> uses;
    std::set<std::pair<int, int>> boundary;
    for (const auto &e : display->edges)
      boundary.insert(std::minmax(e[0], e[1]));
    double area = 0;
    for (const auto &polygon : display->polygons) {
      require(polygon.size() == 3, "General cylinder display must triangulate parameter regions");
      for (size_t i = 0; i < polygon.size(); ++i)
        ++uses[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])];
      const Vec3 a = display->positions[polygon[0]], b = display->positions[polygon[1]],
                 c = display->positions[polygon[2]];
      area += length(cross(b - a, c - a)) / 2;
    }
    for (const auto &[edge, count] : uses)
      require(count == (boundary.contains(edge) ? 1 : 2),
              "Cylinder display introduced a crack or a T junction");
    require(std::abs(area - (hole ? 1.16 : 1.4)) < .01,
            "Trimmed cylinder display filled a hole or lost a region");
    require(model.faces.size() == 1 && model.faces[0].outer.size() == 5 &&
                surface(model, patch.surface)->kind == SurfaceKind::Cylinder,
            "Cylinder display changed native analytic topology");
    Selection selected;
    auto bytes = encode(model, selected);
    require(!bytes.empty(), "Trimmed cylinder source could not be encoded");
    auto restored = decode(bytes.data(), bytes.size(), &error);
    require(bool(restored) && bool(tessellate(restored->model, 64, &error)),
            "Trimmed cylinder source did not survive persistence: " + error);
    Model rational = model;
    Edge &arc = rational.edges.front();
    Curve &rq = rational.curves.front();
    const double mid = (arc.start + arc.end) / 2, half = (arc.end - arc.start) / 2;
    const Vec3 from = evaluate(rq, arc.start), to = evaluate(rq, arc.end);
    rq.kind = CurveKind::RationalQuadratic;
    rq.origin = from;
    rq.u = {std::cos(mid) / std::cos(half), std::sin(mid) / std::cos(half), 0};
    rq.v = to;
    rq.w = {1, std::cos(half), 1};
    arc.start = 0;
    arc.end = 1;
    require(validate(rational, &error), "Rational arc on a cylinder was rejected: " + error);
    const auto rational_display = tessellate(rational, 64, &error);
    require(bool(rational_display), error);
    uses.clear();
    boundary.clear();
    for (const auto &e : rational_display->edges)
      boundary.insert(std::minmax(e[0], e[1]));
    for (const auto &polygon : rational_display->polygons)
      for (size_t i = 0; i < polygon.size(); ++i)
        ++uses[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])];
    for (const auto &[edge, count] : uses)
      require(count == (boundary.contains(edge) ? 1 : 2),
              "Rational trim sampling introduced a cylinder display crack");
    Selection all;
    for (const Vertex &v : model.vertices)
      all.vertices.push_back(v.id);
    Affine shear;
    shear.columns = {Vec3{-2, .1, 0}, Vec3{.4, 1.3, 0}, Vec3{.2, .3, 1.7}};
    shear.translation = {3, -2, 4};
    const auto transformed = transform_selected(model, all, shear, &error);
    require(bool(transformed), error);
    const auto reflected_display = tessellate(transformed->model, 64, &error);
    require(bool(reflected_display), "Affine cylinder trims failed to display: " + error);
    Model invalid = model;
    invalid.curves[0].u.x *= 1.1;
    require(!validate(invalid, &error), "Off-surface trim was accepted on a cylinder");
  }
  for (bool hole : {false, true}) {
    Model input;
    Selection selection;
    std::string error;
    Curve circle{0, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}};
    require(append_curve(input, circle, 0, 2 * std::numbers::pi, 0, 0, true, &selection, &error),
            error);
    const Id outer = input.edges.back().id;
    Id inner = 0;
    if (hole) {
      circle.u = {.5, 0, 0};
      circle.v = {0, .5, 0};
      require(append_curve(input, circle, 0, 2 * std::numbers::pi, 0, 0, true, &selection, &error),
              error);
      inner = input.edges.back().id;
    }
    Surface support;
    support.id = input.next_id++;
    support.u = {1, 0, 0};
    support.v = {0, 1, 0};
    input.surfaces.push_back(support);
    Face original;
    original.id = input.next_id++;
    original.surface = support.id;
    original.outer = {{outer, false}};
    if (hole)
      original.holes = {{{inner, true}}};
    input.faces.push_back(original);
    Surface cutter;
    cutter.origin = {.2, 0, 0};
    cutter.u = {0, 1, 0};
    cutter.v = {0, 0, 1};
    auto split = boolean_split_planar_face(input, original.id, cutter, &error);
    require(bool(split), error);
    require(split->negative.size() == 1 && split->positive.size() == 1,
            "A disk/annulus secant must produce one face on each side");
    double total = 0;
    std::map<Id, int> uses;
    for (const Face &f : split->model.faces) {
      total += area(split->model, f);
      for (Coedge use : f.outer)
        ++uses[use.edge];
      for (const auto &ring : f.holes)
        for (Coedge use : ring)
          ++uses[use.edge];
    }
    require(std::abs(total - std::numbers::pi * (hole ? .75 : 1)) < 1e-12,
            "Trimmed circular faces lost exact area");
    int shared = 0;
    for (const Edge &e : split->model.edges) {
      const Curve &c = *curve(split->model, e.curve);
      if (c.kind == CurveKind::Line) {
        require(uses[e.id] == 2, "Section edge must be shared by both face pieces");
        ++shared;
      }
      else
        require(c.kind == CurveKind::Conic, "Source arc was converted to fitted geometry");
    }
    require(shared == (hole ? 2 : 1), "Section was not trimmed against inner loops");
    require(validate(split->model, &error), error);
    require(bool(tessellate(split->model, 64, &error)), error);
    const double expected_cap = std::acos(.2) - .2 * std::sqrt(.96);
    if (!hole)
      require(std::abs(area(split->model, *face(split->model, split->positive[0])) -
                       expected_cap) < 1e-12,
              "Secant segment area is incorrect");
    auto repeated = boolean_split_planar_face(
        split->model,
        split->negative[0],
        Surface{0, SurfaceKind::Plane, {0, .1, 0}, {0, 0, 1}, {1, 0, 0}},
        &error);
    require(bool(repeated), error);
    cutter.origin.x = .7;
    const auto off_center = boolean_split_planar_face(input, original.id, cutter, &error);
    require(bool(off_center), error);
    require(off_center->negative.size() == 1 && off_center->positive.size() == 1,
            "Off-center cut lost a face piece");
    require(face(off_center->model, off_center->negative[0])->holes.size() == size_t(hole),
            "An uncut inner loop was lost or assigned to the wrong piece");
    require(bool(tessellate(off_center->model, 64, &error)), error);
    cutter.origin.x = 1;
    const auto contact = boolean_split_planar_face(input, original.id, cutter, &error);
    require(bool(contact), error);
    require(contact->positive.empty() && contact->negative.size() == 1,
            "Exact tangency introduced a spurious face");
    require(std::abs(area(contact->model, contact->model.faces[0]) -
                     std::numbers::pi * (hole ? .75 : 1)) < 1e-12,
            "Tangency changed the exact curved face area");
    require(input.faces.size() == 1 && input.edges.size() == size_t(hole ? 2 : 1),
            "Face splitter mutated the source topology");
    input.faces[0].group = input.faces[0].id;
    cutter.origin.x = .2;
    require(bool(boolean_split_planar_face(input, original.id, cutter, &error)),
            "Face split retained a stale logical face group: " + error);
  }
  std::cout << "Native curved face split checks passed\n";
}
