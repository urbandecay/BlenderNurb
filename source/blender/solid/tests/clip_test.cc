/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_classify.hh"
#include "SOL_clip.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>

using namespace blender::solid;

static void require(bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static Profile rectangle(double size)
{
  Model model;
  Selection selection;
  Id first = 0, previous = 0;
  const Vec3 points[] = {{-size, -size, 0}, {size, -size, 0}, {size, size, 0}, {-size, size, 0}};
  std::string error;
  for (int i = 0; i < 4; ++i) {
    require(append_curve(model,
                         *line(points[i], points[(i + 1) % 4]),
                         0,
                         1,
                         previous,
                         i == 3 ? first : 0,
                         false,
                         &selection,
                         &error),
            error);
    const Edge &e = model.edges.back();
    if (!first)
      first = e.vertices[0];
    previous = e.vertices[1];
  }
  const auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  return *profile;
}

static Model cylinder(double distance)
{
  Model model;
  Selection selection;
  std::string error;
  require(append_curve(model,
                       *circle({}, {1, 0, 0}, {0, 0, 1}),
                       0,
                       2 * std::numbers::pi,
                       0,
                       0,
                       true,
                       &selection,
                       &error),
          error);
  const auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  const auto body = extrude(*profile, distance, &error);
  require(bool(body), error);
  return body->model;
}

static Vec3 area_integral(const Model &model, const std::vector<Coedge> &loop)
{
  Vec3 integral{};
  for (Coedge use : loop) {
    const Edge &e = *edge(model, use.edge);
    const Curve &c = *curve(model, e.curve);
    const double a = use.reversed ? e.end : e.start, b = use.reversed ? e.start : e.end;
    if (c.kind == CurveKind::Line)
      integral = integral + cross(c.origin, c.u) * (b - a);
    else
      integral = integral + cross(c.origin, c.u) * (std::cos(b) - std::cos(a)) +
                 cross(c.origin, c.v) * (std::sin(b) - std::sin(a)) + cross(c.u, c.v) * (b - a);
  }
  return integral;
}

// Exact divergence/Green integrals for the supported native surface and trim families.
// This validation does not use the display mesh, including for oblique cylindrical sections.
static double volume(const Model &model)
{
  double result = 0;
  for (const Face &face : model.faces) {
    Surface support = *surface(model, face.surface);
    if (support.kind == SurfaceKind::Extrusion) {
      const Curve &c = *curve(model, support.curve);
      support.origin = c.origin;
      support.w = support.u;
      support.u = c.u;
      support.v = c.v;
      support.kind = SurfaceKind::Cylinder;
    }
    auto add = [&](const std::vector<Coedge> &loop) {
      if (support.kind == SurfaceKind::Plane) {
        result += dot(support.origin, area_integral(model, loop)) * face.orientation / 6;
        return;
      }
      require(support.kind == SurfaceKind::Cylinder, "Unexpected volume surface");
      const double determinant = dot(support.u, cross(support.v, support.w));
      auto direction = [&](Vec3 p) {
        return Vec3{dot(p, cross(support.v, support.w)) / determinant,
                    dot(p, cross(support.w, support.u)) / determinant,
                    dot(p, cross(support.u, support.v)) / determinant};
      };
      const double gx = dot(support.origin, cross(support.v, support.w));
      const double gy = -dot(support.origin, cross(support.u, support.w));
      for (Coedge use : loop) {
        const Edge &e = *edge(model, use.edge);
        const Curve &c = *curve(model, e.curve);
        if (c.kind == CurveKind::Line)
          continue;  // Cylinder generators have d(theta)=0.
        require(c.kind == CurveKind::Conic, "Unexpected volume trim");
        const Vec3 p = direction(c.origin - support.origin);
        const Vec3 u = direction(c.u), v = direction(c.v);
        const double rate = u.x * v.y - u.y * v.x > 0 ? 1 : -1;
        const double a = use.reversed ? e.end : e.start, b = use.reversed ? e.start : e.end;
        const double g0 = determinant + gx * p.x + gy * p.y;
        const double gc = gx * u.x + gy * u.y, gs = gx * v.x + gy * v.y;
        const double delta = b - a, sin_delta = std::sin(b) - std::sin(a);
        const double cos_delta = std::cos(a) - std::cos(b);
        const double cos_square = delta / 2 + (std::sin(2 * b) - std::sin(2 * a)) / 4;
        const double sin_square = delta / 2 - (std::sin(2 * b) - std::sin(2 * a)) / 4;
        const double sin_cos = (std::sin(b) * std::sin(b) - std::sin(a) * std::sin(a)) / 2;
        const double integral = p.z * g0 * delta + (p.z * gc + u.z * g0) * sin_delta +
                                (p.z * gs + v.z * g0) * cos_delta + u.z * gc * cos_square +
                                v.z * gs * sin_square + (u.z * gs + v.z * gc) * sin_cos;
        result -= rate * integral * face.orientation / 3;
      }
    };
    add(face.outer);
    for (const auto &hole : face.holes)
      add(hole);
  }
  return result;
}

static Surface plane(Vec3 origin, Vec3 u, Vec3 v)
{
  Surface result;
  result.kind = SurfaceKind::Plane;
  result.origin = origin;
  result.u = u;
  result.v = v;
  return result;
}

static BooleanBodyPlaneSplit split(const Model &source,
                                   const Surface &cut,
                                   double negative_volume,
                                   double positive_volume,
                                   double tolerance = 0)
{
  std::string error;
  const auto before = encode(source, {});
  const auto result = boolean_split_body_by_plane(source, cut, tolerance, &error);
  require(bool(result), "Body plane split failed: " + error);
  require(before == encode(source, {}), "Body plane split mutated its source");
  const double neg = volume(result->negative), pos = volume(result->positive);
  require(std::abs(neg - negative_volume) < 1e-9 && std::abs(pos - positive_volume) < 1e-9,
          "Body plane split changed analytic volume: " + std::to_string(neg) + ", " +
              std::to_string(pos));
  for (const Model *side : {&result->negative, &result->positive})
    for (const Face &f : side->faces)
      require(validate_body(*side, f.id, &error), error);
  return *result;
}

int main()
{
  std::string error;
  const auto cube = extrude(rectangle(1), 2, &error);
  require(bool(cube), error);
  require(std::abs(volume(cube->model) - 8) < 1e-12, "Box volume reference failed");
  split(cube->model, plane({.2, 0, 0}, {0, 1, 0}, {0, 0, 1}), 4.8, 3.2);
  split(cube->model, plane({0, 0, .7}, {1, 0, 0}, {0, 1, 0}), 2.8, 5.2);
  split(cube->model, plane({0, 0, 1}, {1, 0, .21}, {0, 1, -.13}), 4, 4);
  split(cube->model, plane({}, {1, 1, 0}, {0, 0, 1}), 4, 4);
  split(cube->model, plane({1, 0, 0}, {0, 1, 0}, {0, 0, 1}), 8, 0);
  split(cube->model, plane({-1, 0, 0}, {0, 1, 0}, {0, 0, 1}), 0, 8);
  split(cube->model, plane({2, 0, 0}, {0, 1, 0}, {0, 0, 1}), 8, 0);
  for (double distance : {2., -2.}) {
    const Model round = cylinder(distance);
    const double full = 2 * std::numbers::pi;
    require(std::abs(volume(round) - full) < 1e-12, "Cylinder volume reference failed");
    split(round, plane({0, 0, distance / 2}, {1, 0, 0}, {0, 1, 0}), full / 2, full / 2);
    split(round, plane({0, 0, distance / 2}, {1, 0, .21}, {0, 1, -.13}), full / 2, full / 2);
    split(round, plane({}, {0, 1, 0}, {0, 0, 1}), full / 2, full / 2);
    split(round, plane({}, {0, 0, 1}, {1, 0, 0}), full / 2, full / 2);
    split(round, plane({1, 0, 0}, {0, 1, 0}, {0, 0, 1}), full, 0);
  }

  Profile annulus = rectangle(2);
  const Vec3 inner[] = {{-1, -1, 0}, {-1, 1, 0}, {1, 1, 0}, {1, -1, 0}};
  Id first = 0, previous = 0;
  std::vector<Coedge> hole;
  for (int i = 0; i < 4; ++i) {
    Selection selection;
    require(append_curve(annulus.model,
                         *line(inner[i], inner[(i + 1) % 4]),
                         0,
                         1,
                         previous,
                         i == 3 ? first : 0,
                         false,
                         &selection,
                         &error),
            error);
    const Edge &e = annulus.model.edges.back();
    if (!first)
      first = e.vertices[0];
    previous = e.vertices[1];
    hole.push_back({e.id, false});
  }
  annulus.model.faces.front().holes.push_back(hole);
  const auto tube = extrude(annulus, 2, &error);
  require(bool(tube), error);
  const auto halves = split(tube->model, plane({0, 0, 1}, {1, 0, 0}, {0, 1, 0}), 12, 12);
  for (const Model *half : {&halves.negative, &halves.positive}) {
    require(std::any_of(half->faces.begin(),
                        half->faces.end(),
                        [](const Face &f) { return !f.holes.empty(); }),
            "Plane cap filled its inner loop");
    require(classify_body_point(*half, {0, 0, .5}) == PointClassification::Outside,
            "Plane cap filled a through-hole");
  }
  // Circular cavity walls create conic inner cap loops as well as conic outer loops.
  Model circular;
  Selection outer_selection;
  require(append_curve(circular,
                       *circle({}, {2, 0, 0}, {0, 0, 1}),
                       0,
                       2 * std::numbers::pi,
                       0,
                       0,
                       true,
                       &outer_selection,
                       &error),
          error);
  auto circular_profile = prepare_profile(circular, outer_selection, &error);
  require(bool(circular_profile), error);
  Selection inner_selection;
  require(append_curve(circular_profile->model,
                       *circle({}, {1, 0, 0}, {0, 0, 1}),
                       0,
                       -2 * std::numbers::pi,
                       0,
                       0,
                       true,
                       &inner_selection,
                       &error),
          error);
  circular_profile->model.faces.front().holes.push_back({{inner_selection.edges.front(), false}});
  const auto circular_tube = extrude(*circular_profile, 2, &error);
  require(bool(circular_tube), error);
  split(circular_tube->model,
        plane({0, 0, 1}, {1, 0, 0}, {0, 1, 0}),
        3 * std::numbers::pi,
        3 * std::numbers::pi);
  Model unsupported = cube->model;
  unsupported.surfaces.front().kind = SurfaceKind::Sphere;
  require(!boolean_split_body_by_plane(unsupported, plane({}, {1, 0, 0}, {0, 1, 0}), 0, &error),
          "Unsupported surface was accepted");
  require(!boolean_split_body_by_plane(cube->model, Surface{}, 0, &error),
          "Degenerate clipping plane was accepted");
  require(!boolean_split_body_by_plane(cube->model, plane({}, {1, 0, 0}, {0, 1, 0}), -1, &error),
          "Negative joining tolerance was accepted");
  std::cout << "Native closed-body plane clipping tests passed\n";
}
