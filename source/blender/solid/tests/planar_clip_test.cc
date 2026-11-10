/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_planar_clip.hh"

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

static Profile polygon(const std::vector<Vec3> &points)
{
  Model model;
  Selection selection;
  Id first = 0, previous = 0;
  std::string error;
  for (size_t i = 0; i < points.size(); ++i) {
    require(append_curve(model,
                         *line(points[i], points[(i + 1) % points.size()]),
                         0,
                         1,
                         previous,
                         i + 1 == points.size() ? first : 0,
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

static Profile rectangle(double left, double right, double bottom, double top, double z = 0)
{
  return polygon({{left, bottom, z}, {right, bottom, z}, {right, top, z}, {left, top, z}});
}

static Profile disc(double radius)
{
  Model model;
  Selection selection;
  std::string error;
  require(append_curve(model,
                       *circle({}, {radius, 0, 0}, {0, 0, 1}),
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
  return *profile;
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

static double area(const Model &model)
{
  double total = 0;
  for (const Face &f : model.faces) {
    const Surface &s = *surface(model, f.surface);
    const Vec3 n = cross(s.u, s.v) / length(cross(s.u, s.v));
    double value = dot(area_integral(model, f.outer), n) / 2;
    for (const auto &hole : f.holes)
      value += dot(area_integral(model, hole), n) / 2;
    total += std::abs(value);
  }
  return total;
}

static Model clip(const Profile &source,
                  const Profile &mask,
                  double expected_area,
                  IntersectionTolerance tolerance = {0, 0})
{
  const auto source_before = encode(source.model, {}), mask_before = encode(mask.model, {});
  std::string error;
  const auto result = boolean_clip_planar_face(
      source.model, source.face, mask.model, mask.face, tolerance, &error);
  require(bool(result), "Native planar clipping failed: " + error);
  require(source_before == encode(source.model, {}) && mask_before == encode(mask.model, {}),
          "Native planar clipping mutated an input");
  require(std::abs(area(*result) - expected_area) < 1e-9,
          "Native planar clipping changed analytic area: " + std::to_string(area(*result)));
  require(validate(*result, &error), error);
  return *result;
}

static void append_square_hole(Profile &profile, double size)
{
  const Vec3 points[] = {{-size, -size, 0}, {-size, size, 0}, {size, size, 0}, {size, -size, 0}};
  Id first = 0, previous = 0;
  std::vector<Coedge> hole;
  std::string error;
  for (int i = 0; i < 4; ++i) {
    Selection selection;
    require(append_curve(profile.model,
                         *line(points[i], points[(i + 1) % 4]),
                         0,
                         1,
                         previous,
                         i == 3 ? first : 0,
                         false,
                         &selection,
                         &error),
            error);
    const Edge &e = profile.model.edges.back();
    if (!first)
      first = e.vertices[0];
    previous = e.vertices[1];
    hole.push_back({e.id, false});
  }
  profile.model.faces.front().holes.push_back(hole);
}

int main()
{
  const Profile square = rectangle(-2, 2, -2, 2);
  const Profile mask = rectangle(-1, 1, -1, 1);
  clip(square, mask, 4);
  Profile reversed = mask;
  reversed.model.faces.front().orientation = -1;
  clip(square, reversed, 4);
  std::reverse(reversed.model.faces.front().outer.begin(),
               reversed.model.faces.front().outer.end());
  for (Coedge &use : reversed.model.faces.front().outer)
    use.reversed = !use.reversed;
  clip(square, reversed, 4);
  clip(square, square, 16);
  clip(square, polygon({{-1, -1, 0}, {0, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}), 4);
  Profile reparameterized = mask;
  for (Edge &e : reparameterized.model.edges) {
    Curve &c = *std::find_if(reparameterized.model.curves.begin(),
                             reparameterized.model.curves.end(),
                             [&](const Curve &value) { return value.id == e.curve; });
    c.origin = c.origin + c.u * 2;
    e.start -= 2;
    e.end -= 2;
  }
  clip(square, reparameterized, 4);
  const Model edge_contact = clip(square, rectangle(2, 3, -1, 1), 0);
  require(edge_contact.faces.empty(), "A boundary-only intersection became a planar face");
  const Model thin = clip(square, rectangle(0, 1e-11, -1, 1), 2e-11);
  require(!thin.faces.empty() && std::abs(area(thin) - 2e-11) < 2e-17,
          "Planar clipping collapsed a representable narrow region");
  const Model disjoint = clip(square, rectangle(4, 5, 4, 5), 0);
  require(disjoint.faces.empty() && disjoint.vertices.empty() && disjoint.edges.empty() &&
              disjoint.curves.empty() && disjoint.surfaces.empty(),
          "Disjoint planar intersection retained unreferenced geometry");
  const Profile circle = disc(2);
  const Model half_disc = clip(circle, rectangle(0, 3, -3, 3), 2 * std::numbers::pi);
  require(std::any_of(half_disc.curves.begin(),
                      half_disc.curves.end(),
                      [](const Curve &c) { return c.kind == CurveKind::Conic; }),
          "Planar clipping replaced the original conic with line segments");
  const Profile small_disc = disc(1);
  require(clip(small_disc, rectangle(1, 2, -1, 1), 0).faces.empty(),
          "A tangent conic contact became a planar face");
  const Model contained = clip(small_disc, square, std::numbers::pi);
  require(contained.curves.size() == 1 && contained.edges.size() == 1 &&
              contained.curves.front().kind == CurveKind::Conic &&
              contained.curves.front().origin == small_disc.model.curves.front().origin &&
              contained.curves.front().u == small_disc.model.curves.front().u &&
              contained.curves.front().v == small_disc.model.curves.front().v,
          "Contained source changed its native curve support");

  Profile annulus = rectangle(-3, 3, -3, 3);
  append_square_hole(annulus, 1);
  const Model bounded_hole = clip(annulus, square, 12);
  require(bounded_hole.faces.size() == 1 && bounded_hole.faces.front().holes.size() == 1,
          "Planar mask filled a bounded source hole");
  clip(annulus, rectangle(0, 2, -2, 2), 6);
  Profile round_annulus = circle;
  Selection inner;
  std::string error;
  require(append_curve(round_annulus.model,
                       *blender::solid::circle({}, {1, 0, 0}, {0, 0, 1}),
                       0,
                       -2 * std::numbers::pi,
                       0,
                       0,
                       true,
                       &inner,
                       &error),
          error);
  round_annulus.model.faces.front().holes.push_back({{inner.edges.front(), false}});
  clip(round_annulus, rectangle(0, 3, -3, 3), 1.5 * std::numbers::pi);
  const Profile nonconvex = polygon({{-3, -3, 0},
                                     {3, -3, 0},
                                     {3, 3, 0},
                                     {1, 3, 0},
                                     {1, -1, 0},
                                     {-1, -1, 0},
                                     {-1, 3, 0},
                                     {-3, 3, 0}});
  const Model islands = clip(nonconvex, rectangle(-4, 4, 0, 2), 8);
  require(islands.faces.size() == 2, "Convex mask lost disconnected source intersection regions");

  Affine affine;
  affine.columns = {{{1, 0, 1}, {0, 1, 0}, {-1, 0, 1}}};
  affine.translation = {3, -2, 5};
  auto transform = [&](Profile profile) {
    Selection all;
    for (const Vertex &v : profile.model.vertices)
      all.vertices.push_back(v.id);
    const auto result = transform_selected(profile.model, all, affine, &error);
    require(bool(result), error);
    profile.model = result->model;
    return profile;
  };
  clip(transform(square), transform(mask), 4 * std::sqrt(2), {1e-12, 1e-12});

  auto reject = [&](const Profile &source, const Profile &bad_mask) {
    const auto source_before = encode(source.model, {}), mask_before = encode(bad_mask.model, {});
    require(!boolean_clip_planar_face(
                source.model, source.face, bad_mask.model, bad_mask.face, {0, 0}, &error) &&
                !error.empty(),
            "Unsupported planar mask was silently accepted");
    require(source_before == encode(source.model, {}) && mask_before == encode(bad_mask.model, {}),
            "Failed planar clipping mutated an input");
  };
  reject(square, rectangle(-1, 1, -1, 1, .1));
  reject(square, circle);
  reject(square, nonconvex);
  reject(square, annulus);
  Profile unsupported = square;
  unsupported.model.curves.front().kind = CurveKind::Cubic;
  unsupported.model.curves.front().v = {0, 0, 0};
  reject(unsupported, rectangle(10, 11, 10, 11));
  require(
      !boolean_clip_planar_face(square.model, square.face, mask.model, mask.face, {-1, 0}, &error),
      "Negative support tolerance was accepted");
  std::cout << "Native convex planar-mask clipping tests passed\n";
}
