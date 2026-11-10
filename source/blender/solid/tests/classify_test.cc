/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_classify.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>

using namespace blender::solid;
using State = PointClassification;

static void require(bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static Profile rectangular_profile(double size)
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
  auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  return *profile;
}

static Model box(double size = 1)
{
  std::string error;
  const auto body = extrude(rectangular_profile(size), 2 * size, &error);
  require(bool(body), error);
  return body->model;
}

static Model cylinder()
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
  const auto body = extrude(*profile, 2, &error);
  require(bool(body), error);
  return body->model;
}

static void expect(const Model &model,
                   Vec3 point,
                   State state,
                   ClassificationTolerance tolerance = {})
{
  std::string error;
  const auto actual = classify_body_point(model, point, tolerance, &error);
  require(bool(actual), error);
  if (*actual != state) {
    std::cerr << "Point " << point.x << ',' << point.y << ',' << point.z << " expected "
              << int(state) << " got " << int(*actual) << '\n';
    std::exit(1);
  }
}

int main()
{
  const Model cube = box();
  expect(cube, {0, 0, 1}, State::Inside);
  expect(cube, {1.5, .1, 1}, State::Outside);
  expect(cube, {0, 0, -1}, State::Outside);
  expect(cube, {0, 0, 3}, State::Outside);
  expect(cube, {1, 0, 1}, State::Boundary);
  expect(cube, {1, 1, 1}, State::Boundary);
  expect(cube, {1, 1, 2}, State::Boundary);
  expect(cube, {1 + 1e-11, 0, 1}, State::Uncertain);
  expect(cube, {1 - 1e-11, 0, 1}, State::Uncertain);
  expect(cube, {1 + 1e-11, 0, 1}, State::Outside, {0, 0});
  expect(cube, {1 - 1e-11, 0, 1}, State::Inside, {0, 0});
  // The first test ray passes exactly through the top right cube vertex. The entire ray
  // must be discarded before a fresh direction supplies parity.
  expect(cube, {0, 1 - .371, 2 - .613}, State::Inside);

  const Model round = cylinder();
  expect(round, {0, 0, 1}, State::Inside);
  expect(round, {.9, .1, 1}, State::Inside);
  expect(round, {2, .1, 1}, State::Outside);
  expect(round, {0, 0, -1}, State::Outside);
  expect(round, {0, 0, 3}, State::Outside);
  expect(round, {1, 0, 1}, State::Boundary);
  expect(round, {1, 0, 0}, State::Boundary);
  expect(round, {0, 0, 0}, State::Boundary);
  expect(round, {1 + 1e-11, 0, 1}, State::Uncertain);
  expect(round, {1 - 1e-11, 0, 1}, State::Inside, {0, 0});
  expect(round, {1 + 1e-11, 0, 1}, State::Outside, {0, 0});
  // The first body ray touches the cylinder tangentially. A tangent contributes no
  // crossing; a numerically unresolved discriminant must instead retry the entire ray.
  const Vec3 first_ray{1, .371, .613};
  const double radial_length = std::sqrt(1 + .371 * .371);
  const Vec3 tangent_point{.371 / radial_length, -1 / radial_length, 1};
  expect(round, tangent_point - first_ray * .1, State::Outside);

  // Re-express the same wall using the affine Cylinder support family.
  Model explicit_cylinder = round;
  for (Surface &s : explicit_cylinder.surfaces) {
    if (s.kind != SurfaceKind::Extrusion)
      continue;
    const Curve &c = *curve(explicit_cylinder, s.curve);
    s.kind = SurfaceKind::Cylinder;
    s.origin = c.origin;
    s.w = s.u;
    s.u = c.u;
    s.v = c.v;
  }
  expect(explicit_cylinder, {.3, .2, .7}, State::Inside);
  expect(explicit_cylinder, {1.3, .2, .7}, State::Outside);

  // Analytic affine geometry includes tilted axes, ellipses, and oblique cap planes.
  Selection all;
  for (const Vertex &v : round.vertices)
    all.vertices.push_back(v.id);
  Affine matrix;
  matrix.columns = {{{2, .2, .1}, {.7, 1.3, .3}, {.2, .4, 3}}};
  matrix.translation = {3, -2, 5};
  std::string error;
  const auto changed = transform_selected(round, all, matrix, &error);
  require(bool(changed), error);
  auto transformed = [&](Vec3 p) {
    return matrix.translation + matrix.columns[0] * p.x + matrix.columns[1] * p.y +
           matrix.columns[2] * p.z;
  };
  expect(changed->model, transformed({.2, .3, .7}), State::Inside);
  expect(changed->model, transformed({1.2, .3, .7}), State::Outside);

  // A native annular cap and its extrusion preserve holes in face and body classification.
  Profile annulus = rectangular_profile(2);
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
  const Face &cap = annulus.model.faces.front();
  require(classify_face_point(annulus.model, cap, {1.5, 0, 0}) == State::Inside,
          "Planar trim lost material outside its hole");
  require(classify_face_point(annulus.model, cap, {0, 0, 0}) == State::Outside,
          "Planar trim filled its native hole");
  require(classify_face_point(annulus.model, cap, {1, 0, 0}) == State::Boundary,
          "Planar inner rim was not classified as boundary");
  const auto tube = extrude(annulus, 2, &error);
  require(bool(tube), error);
  expect(tube->model, {0, 0, 1}, State::Outside);
  expect(tube->model, {1.5, 0, 1}, State::Inside);
  expect(tube->model, {1, 0, 1}, State::Boundary);

  // A cylindrical face may have arbitrary native bounded loops, including holes. Axial
  // parity is evaluated against conics and generator lines without unwrapping a sampled mesh.
  Model patch;
  auto ring = [&](double low, double high, double bottom, double top) {
    std::vector<Coedge> uses;
    Id ring_first = 0, ring_previous = 0;
    auto add = [&](Curve c, double a, double b, bool last) {
      Selection selection;
      require(append_curve(
                  patch, c, a, b, ring_previous, last ? ring_first : 0, false, &selection, &error),
              error);
      const Edge &e = patch.edges.back();
      if (!ring_first)
        ring_first = e.vertices[0];
      ring_previous = e.vertices[1];
      uses.push_back({e.id, false});
    };
    add({0, CurveKind::Conic, {0, 0, bottom}, {1, 0, 0}, {0, 1, 0}}, low, high, false);
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
  Face curved_face;
  curved_face.outer = ring(-.7, .7, 0, 1);
  curved_face.holes.push_back(ring(-.3, .3, .3, .7));
  Surface curved_support;
  curved_support.id = patch.next_id++;
  curved_support.kind = SurfaceKind::Cylinder;
  curved_support.u = {1, 0, 0};
  curved_support.v = {0, 1, 0};
  curved_support.w = {0, 0, 1};
  patch.surfaces.push_back(curved_support);
  curved_face.surface = curved_support.id;
  curved_face.id = patch.next_id++;
  patch.faces.push_back(curved_face);
  require(classify_face_point(patch, curved_face, {1, 0, .2}) == State::Inside,
          "Cylinder trim lost material outside its hole");
  require(classify_face_point(patch, curved_face, {1, 0, .5}) == State::Outside,
          "Cylinder trim filled its native hole");
  require(classify_face_point(patch, curved_face, {1, 0, .3}) == State::Boundary,
          "Cylinder inner conic rim was not classified as boundary");

  // A feature much smaller than the default distance remains classifiable with an explicitly
  // tighter policy. The default reports uncertainty instead of inventing a contact.
  const Model tiny = box(1e-12);
  expect(tiny, {0, 0, 1e-12}, State::Inside, {0, 0});
  expect(tiny, {2e-12, 0, 1e-12}, State::Outside, {0, 0});
  expect(tiny, {0, 0, 1e-12}, State::Uncertain);

  Model unsupported = cube;
  unsupported.surfaces.front().kind = SurfaceKind::Sphere;
  require(!classify_body_point(unsupported, {100, 100, 100}, {}, &error) && !error.empty(),
          "Unsupported surfaces were silently skipped");
  Model open = cube;
  open.faces.pop_back();
  require(!classify_body_point(open, {0, 0, 1}, {}, &error), "Open body was accepted");
  require(!classify_body_point(cube, {0, 0, 1}, {-1, 0}, &error),
          "Negative classification tolerance was accepted");
  require(!classify_body_point(cube, {0, 0, 1}, {0, 1}, &error),
          "Invalid angle classification tolerance was accepted");
  require(!classify_body_point(cube, {std::numeric_limits<double>::quiet_NaN(), 0, 1}, {}, &error),
          "Nonfinite point was accepted");
  std::cout << "Native analytic classification tests passed\n";
}
