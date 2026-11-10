/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_boolean.hh"
#include "SOL_classify.hh"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
using namespace blender::solid;
void check(bool value, const std::string &message)
{
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
Model box(Vec3 offset, double angle = 0)
{
  Model m;
  Selection s;
  Id first = 0, previous = 0;
  Vec3 p[] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
  for (auto &v : p) {
    v = {std::cos(angle) * v.x - std::sin(angle) * v.y,
         std::sin(angle) * v.x + std::cos(angle) * v.y,
         0};
    v = v + offset;
  }
  std::string error;
  for (int i = 0; i < 4; ++i) {
    auto c = line(p[i], p[(i + 1) % 4], &error);
    check(bool(c), error);
    check(append_curve(m, *c, 0, 1, previous, i == 3 ? first : 0, false, &s, &error), error);
    if (!first)
      first = m.edges.back().vertices[0];
    previous = m.edges.back().vertices[1];
  }
  auto profile = prepare_profile(m, s, &error);
  check(bool(profile), error);
  auto body = extrude(*profile, 2, &error);
  check(bool(body), error);
  return body->model;
}
std::vector<Id> append(Model &to, Model other)
{
  const Id offset = to.next_id;
  for (auto &v : other.vertices)
    v.id += offset;
  for (auto &c : other.curves)
    c.id += offset;
  for (auto &e : other.edges) {
    e.id += offset;
    e.curve += offset;
    for (auto &v : e.vertices)
      v += offset;
  }
  for (auto &s : other.surfaces) {
    s.id += offset;
    if (s.curve)
      s.curve += offset;
    if (s.other_curve)
      s.other_curve += offset;
  }
  std::vector<Id> ids;
  for (auto &f : other.faces) {
    f.id += offset;
    f.surface += offset;
    if (f.group)
      f.group += offset;
    for (auto &e : f.outer)
      e.edge += offset;
    for (auto &h : f.holes)
      for (auto &e : h)
        e.edge += offset;
    ids.push_back(f.id);
  }
  to.vertices.insert(to.vertices.end(), other.vertices.begin(), other.vertices.end());
  to.curves.insert(to.curves.end(), other.curves.begin(), other.curves.end());
  to.edges.insert(to.edges.end(), other.edges.begin(), other.edges.end());
  to.surfaces.insert(to.surfaces.end(), other.surfaces.begin(), other.surfaces.end());
  to.faces.insert(to.faces.end(), other.faces.begin(), other.faces.end());
  to.next_id = other.next_id + offset;
  return ids;
}
double volume(const Model &m)
{
  double result = 0;
  for (const auto &f : m.faces) {
    auto loop = [&](const auto &ring) {
      const Vec3 reference =
          vertex(m, edge(m, ring[0].edge)->vertices[ring[0].reversed ? 1 : 0])->position;
      for (size_t i = 1; i + 1 < ring.size(); ++i) {
        const Edge *e = edge(m, ring[i].edge), *next = edge(m, ring[i + 1].edge);
        result += dot(reference,
                      cross(vertex(m, e->vertices[ring[i].reversed ? 1 : 0])->position,
                            vertex(m, next->vertices[ring[i + 1].reversed ? 1 : 0])->position)) /
                  6;
      }
    };
    loop(f.outer);
    for (const auto &hole : f.holes)
      loop(hole);
  }
  return result;
}
int main()
{
  {
    Model profile_model;
    Selection selected;
    std::string error;
    Curve round{0, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}};
    check(append_curve(profile_model, round, 0, 2 * std::acos(-1.), 0, 0, true, &selected, &error),
          error);
    auto profile = prepare_profile(profile_model, selected, &error);
    check(bool(profile), error);
    auto cylinder = extrude(*profile, 2, &error);
    check(bool(cylinder), error);
    Model cutter = box({});
    Selection all;
    for (const Vertex &v : cutter.vertices)
      all.vertices.push_back(v.id);
    const double angle = .37, c = std::cos(angle), s = std::sin(angle);
    Affine matrix;
    matrix.columns = {Vec3{20 * c, 0, -20 * s}, Vec3{0, 20, 0}, Vec3{20 * s, 0, 20 * c}};
    matrix.translation = {-20 * c - 20 * s, 0, 1 + 20 * s - 20 * c};
    auto tilted = transform_selected(cutter, all, matrix, &error);
    check(bool(tilted), error);
    Model source;
    auto target = append(source, cylinder->model), tool = append(source, tilted->model);
    for (int mode = 0; mode < 4; ++mode) {
      const BooleanOperation operation = mode == 0 ? BooleanOperation::Intersect :
                                         mode == 2 ? BooleanOperation::Union :
                                                     BooleanOperation::Subtract;
      const bool reverse = mode == 3;
      auto result = reverse ? boolean_bodies(source, tool, target, operation, &error) :
                              boolean_bodies(source, target, tool, operation, &error);
      check(bool(result), "Curved native Boolean against an oblique planar cutter: " + error);
      for (const Face &f : result->faces)
        check(validate_body(*result, f.id, &error), error);
      check(std::any_of(result->surfaces.begin(),
                        result->surfaces.end(),
                        [](const Surface &s) { return s.kind == SurfaceKind::Cylinder; }),
            "Oblique cutter Boolean lost analytic cylinders");
      for (double x : {-.7, -.1, .2, .7})
        for (double y : {-.4, .4})
          for (double z : {.4, 1.6}) {
            const bool in_cutter = x * c - (z - 1) * s < 0;
            const bool expected = reverse                                  ? false :
                                  operation == BooleanOperation::Union     ? true :
                                  operation == BooleanOperation::Intersect ? in_cutter :
                                                                             !in_cutter;
            const auto actual = classify_body_point(*result, {x, y, z}, {0, 0}, &error);
            check(bool(actual), error);
            check(*actual ==
                      (expected ? PointClassification::Inside : PointClassification::Outside),
                  "Curved native Boolean changed material membership");
          }
      check(bool(tessellate(*result, 64, &error)), error);
    }
    matrix.columns = {Vec3{.4 * c, 0, -.4 * s}, Vec3{0, .6, 0}, Vec3{1.2 * s, 0, 1.2 * c}};
    matrix.translation = {.1 - 1.2 * s, .05, 1 - 1.2 * c};
    tilted = transform_selected(cutter, all, matrix, &error);
    check(bool(tilted), error);
    source = {};
    target = append(source, cylinder->model);
    tool = append(source, tilted->model);
    for (int mode = 0; mode < 4; ++mode) {
      const BooleanOperation operation = mode == 0 ? BooleanOperation::Intersect :
                                         mode == 2 ? BooleanOperation::Union :
                                                     BooleanOperation::Subtract;
      const bool reverse = mode == 3;
      auto result = reverse ? boolean_bodies(source, tool, target, operation, &error) :
                              boolean_bodies(source, target, tool, operation, &error);
      check(bool(result), "Multi-plane curved native Boolean: " + error);
      for (const Face &f : result->faces)
        check(validate_body(*result, f.id, &error), error);
      for (double x : {-.75, -.25, .25, .75})
        for (double y : {-.75, -.25, .25, .75})
          for (double z : {.25, .75, 1.25, 1.75}) {
            const Vec3 q = {x - .1, y - .05, z - 1};
            const bool in_cutter = std::abs((q.x * c - q.z * s) / .4) < 1 &&
                                   std::abs(q.y / .6) < 1 &&
                                   std::abs((q.x * s + q.z * c) / 1.2) < 1;
            const bool in_target = x * x + y * y < 1;
            const bool expected = reverse ?
                                      in_cutter && !in_target :
                                  operation == BooleanOperation::Union ?
                                      in_target || in_cutter :
                                      in_target &&
                                          (operation == BooleanOperation::Intersect ? in_cutter :
                                                                                      !in_cutter);
            const auto actual = classify_body_point(*result, {x, y, z}, {0, 0}, &error);
            check(bool(actual), error);
            check(*actual ==
                      (expected ? PointClassification::Inside : PointClassification::Outside),
                  "Multi-plane curved Boolean changed material membership");
          }
      check(bool(tessellate(*result, 64, &error)), error);
    }
  }
  {
    const Model original = box({});
    std::vector<Model> pieces;
    for (const Face &f : original.faces) {
      Model piece = original;
      piece.faces = {f};
      // Identical straight boundaries can carry different line parameters.
      for (Edge &e : piece.edges) {
        Curve &c = *std::find_if(piece.curves.begin(), piece.curves.end(), [&](const Curve &c) {
          return c.id == e.curve;
        });
        c.origin = c.origin + c.u * 2;
        e.start -= 2;
        e.end -= 2;
      }
      pieces.push_back(std::move(piece));
    }
    std::string error;
    auto closed = boolean_sew_faces(pieces, 0, true, &error);
    check(bool(closed), error);
    check(closed->vertices.size() == 8 && closed->edges.size() == 12 && closed->faces.size() == 6,
          "Native face assembly retained duplicate or unreferenced topology");
    check(std::abs(volume(*closed) - 8) < 1e-12, "Native face assembly changed volume");
    pieces.pop_back();
    check(!boolean_sew_faces(pieces, 0, true, &error),
          "Open Boolean shell was accepted as closed");
    check(bool(boolean_sew_faces(pieces, 0, false, &error)), "Open face assembly was rejected");
    pieces.push_back(pieces.front());
    check(!boolean_sew_faces(pieces, 0, true, &error), "Duplicate Boolean face was accepted");
    check(!boolean_sew_faces(pieces, -1, false, &error), "Negative assembly tolerance accepted");
  }
  for (Vec3 shift : {Vec3{.8, .4, .3},
                     Vec3{.8, .4, 0},
                     Vec3{.8, 0, 0},
                     Vec3{0, 0, 0},
                     Vec3{3, 0, 0},
                     Vec3{2, 0, 0}})
  {
    Model source;
    auto a = append(source, box({})), b = append(source, box(shift));
    double overlap = std::max(0., 2 - std::abs(shift.x)) * std::max(0., 2 - std::abs(shift.y)) *
                     std::max(0., 2 - std::abs(shift.z));
    for (auto operation :
         {BooleanOperation::Union, BooleanOperation::Subtract, BooleanOperation::Intersect})
    {
      std::string error;
      auto result = boolean_bodies(source, a, b, operation, &error);
      std::cerr << "case " << shift.x << ',' << shift.y << ',' << shift.z << " op "
                << int(operation) << '\n';
      check(bool(result), error);
      double expected = operation == BooleanOperation::Union    ? 16 - overlap :
                        operation == BooleanOperation::Subtract ? 8 - overlap :
                                                                  overlap;
      check(std::abs(volume(*result) - expected) < 1e-9, "Incorrect Boolean volume");
      check(validate(*result, &error), error);
      if (shift == Vec3{.8, 0, 0})
        check(result->faces.size() == 6 && result->edges.size() == 12 &&
                  result->vertices.size() == 8,
              "Coplanar Boolean retained redundant geometry");
    }
  }
  // A through-hole creates inner loops; use that result in another Boolean.
  {
    Model source;
    auto a = append(source, box({}));
    Model cutter = box({});
    Selection all;
    for (const auto &v : cutter.vertices)
      all.vertices.push_back(v.id);
    Affine matrix;
    matrix.columns[0] = {.4, 0, 0};
    matrix.columns[1] = {0, .4, 0};
    matrix.columns[2] = {0, 0, 2};
    matrix.translation = {0, 0, -1};
    std::string error;
    auto changed = transform_selected(cutter, all, matrix, &error);
    check(bool(changed), error);
    auto b = append(source, changed->model);
    auto hole = boolean_bodies(source, a, b, BooleanOperation::Subtract, &error);
    check(bool(hole), error);
    check(std::abs(volume(*hole) - 6.72) < 1e-9, "Through-hole volume failed");
    check(std::any_of(hole->faces.begin(),
                      hole->faces.end(),
                      [](const Face &f) { return !f.holes.empty(); }),
          "Through-hole did not preserve native inner loops");
    auto display = tessellate(*hole, 64, &error);
    check(bool(display), error);
    std::map<std::pair<int, int>, int> edge_uses;
    for (const auto &polygon : display->polygons)
      for (size_t i = 0; i < polygon.size(); ++i)
        ++edge_uses[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])];
    for (const auto &[key, count] : edge_uses)
      check(count == 2, "Hole display is not closed manifold");
    Model repeated;
    auto h = append(repeated, *hole), c = append(repeated, box({.8, .4, .3}));
    auto united = boolean_bodies(repeated, h, c, BooleanOperation::Union, &error);
    check(bool(united), error);
    auto common = boolean_bodies(repeated, h, c, BooleanOperation::Intersect, &error);
    check(bool(common), error);
    check(std::abs(volume(*united) + volume(*common) - 14.72) < 1e-9,
          "Repeated Boolean with inner loops failed");
  }
  // Oblique faces exercise native intersections and classification away from a shared axis.
  Model source;
  auto a = append(source, box({})), b = append(source, box({.4, .2, .3}, .37));
  double volumes[3];
  for (int i = 0; i < 3; ++i) {
    std::string error;
    auto result = boolean_bodies(source, a, b, BooleanOperation(i), &error);
    check(bool(result), error);
    volumes[i] = volume(*result);
    check(volumes[i] >= 0, "Inverted output orientation");
  }
  check(std::abs(volumes[0] + volumes[2] - 16) < 1e-9 &&
            std::abs(volumes[1] + volumes[2] - 8) < 1e-9,
        "Oblique Boolean set identities failed");
  std::cout << "Native Boolean checks passed\n";
}
