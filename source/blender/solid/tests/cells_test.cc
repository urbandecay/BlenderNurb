/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_cells.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>

using namespace blender::solid;

static void require(const bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static Model box(const Vec3 offset, const double depth = 1)
{
  Model model;
  Selection selection;
  Id first = 0, previous = 0;
  const Vec3 points[] = {{0, 0, 0}, {1, 0, 0}, {1, depth, 0}, {0, depth, 0}};
  std::string error;
  for (int i = 0; i < 4; ++i) {
    require(append_curve(model,
                         *line(points[i] + offset, points[(i + 1) % 4] + offset),
                         0,
                         1,
                         previous,
                         i == 3 ? first : 0,
                         false,
                         &selection,
                         &error),
            error);
    const Edge &edge = model.edges.back();
    if (!first) {
      first = edge.vertices[0];
    }
    previous = edge.vertices[1];
  }
  const auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  const auto body = extrude(*profile, 1, &error);
  require(bool(body), error);
  return body->model;
}

static Model ring_cell(const double z,
                       const double hole_radius = .25,
                       const bool two_holes = false)
{
  Model model;
  Selection selection;
  std::string error;
  const Vec3 origin{0, 0, z};
  require(append_curve(model,
                       *circle(origin, origin + Vec3{1, 0, 0}, {0, 0, 1}),
                       0,
                       2 * std::numbers::pi,
                       0,
                       0,
                       true,
                       &selection,
                       &error),
          error);
  auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  for (int i = 0; i < (two_holes ? 2 : 1); ++i) {
    const Vec3 center = origin + Vec3{two_holes ? i - .5 : 0, 0, 0};
    require(append_curve(profile->model,
                         *circle(center, center + Vec3{hole_radius, 0, 0}, {0, 0, 1}),
                         0,
                         2 * std::numbers::pi,
                         0,
                         0,
                         true,
                         nullptr,
                         &error),
            error);
    profile->model.faces.front().holes.push_back({{profile->model.edges.back().id, true}});
  }
  const auto body = extrude(*profile, 1, &error);
  require(bool(body), error);
  return body->model;
}

static double planar_volume(const Model &model)
{
  double volume = 0;
  for (const Face &face : model.faces) {
    auto integrate = [&](const std::vector<Coedge> &loop) {
      auto point = [&](const size_t i) {
        const Coedge use = loop[i];
        const Edge &boundary = *edge(model, use.edge);
        return vertex(model, boundary.vertices[use.reversed ? 1 : 0])->position;
      };
      for (size_t i = 1; i + 1 < loop.size(); ++i) {
        volume += face.orientation * dot(point(0), cross(point(i), point(i + 1))) / 6;
      }
    };
    integrate(face.outer);
    for (const auto &hole : face.holes) {
      integrate(hole);
    }
  }
  return volume;
}

static void check_closed(const Model &model)
{
  std::string error;
  require(validate(model, &error), error);
  for (const Face &face : model.faces) {
    require(validate_body(model, face.id, &error), error);
  }
}

int main()
{
  std::string error;
  const std::array<Model, 2> adjacent{box({}), box({1, 0, 0})};
  const auto first_snapshot = encode(adjacent[0], {});
  const auto second_snapshot = encode(adjacent[1], {});
  auto boundary = boolean_assemble_cells(adjacent, 0, &error);
  require(bool(boundary), error);
  require(boundary->faces.size() == 10 && boundary->edges.size() == 20 &&
              boundary->vertices.size() == 12,
          "Adjacent cells did not remove exactly their two interface faces");
  require(std::abs(planar_volume(*boundary) - 2) < 1e-14,
          "Cell reconstruction changed the occupied volume");
  check_closed(*boundary);
  for (const Face &face : boundary->faces) {
    bool interface = true;
    for (const Coedge use : face.outer) {
      for (const Id id : edge(*boundary, use.edge)->vertices) {
        interface &= vertex(*boundary, id)->position.x == 1;
      }
    }
    require(!interface, "A common cell interface survived in the exterior boundary");
  }
  require(encode(adjacent[0], {}) == first_snapshot &&
              encode(adjacent[1], {}) == second_snapshot,
          "Successful native cell assembly mutated its sources");

  /* Cycle origins and face storage orientation are representation choices.
   * Physical boundary senses must determine interface cancellation. */
  auto reparameterized = adjacent;
  for (Face &face : reparameterized[1].faces) {
    face.orientation = -face.orientation;
    std::reverse(face.outer.begin(), face.outer.end());
    for (Coedge &use : face.outer) {
      use.reversed = !use.reversed;
    }
    std::rotate(face.outer.begin(), face.outer.begin() + 1, face.outer.end());
  }
  auto equivalent = boolean_assemble_cells(reparameterized, 0, &error);
  require(equivalent && equivalent->faces.size() == 10,
          "Equivalent physical face traversal prevented common-face cancellation: " + error);
  check_closed(*equivalent);

  const std::array<Model, 2> disjoint{adjacent[0], box({2, 0, 0})};
  const auto disconnected = boolean_assemble_cells(disjoint, 0, &error);
  require(disconnected && disconnected->faces.size() == 12,
          "Different native regions were falsely canceled: " + error);
  check_closed(*disconnected);

  const std::array<Model, 2> mismatched{adjacent[0], box({1, 0, 0}, .5)};
  require(!boolean_assemble_cells(mismatched, 0, &error),
          "Unpartitioned partial interfaces were silently treated as full common faces");
  auto reversed = adjacent;
  reversed[1].faces.front().orientation *= -1;
  const auto reversed_snapshot = encode(reversed[1], {});
  require(!boolean_assemble_cells(reversed, 0, &error),
          "A cell with one reversed face was accepted");
  require(encode(reversed[1], {}) == reversed_snapshot,
          "Rejected native cell assembly mutated its source");

  const std::array<Model, 2> duplicate{adjacent[0], adjacent[0]};
  require(!boolean_assemble_cells(duplicate, 0, &error) &&
              error.find("same physical orientation") != std::string::npos,
          "Same-orientation duplicate faces were silently canceled");
  const std::array<Model, 3> triple{adjacent[0], adjacent[0], adjacent[0]};
  require(!boolean_assemble_cells(triple, 0, &error) &&
              error.find("More than two") != std::string::npos,
          "More than two uses of a common cell face were accepted");

  const std::array<Model, 2> rings{ring_cell(0), ring_cell(1)};
  const auto annular = boolean_assemble_cells(rings, 0, &error);
  require(annular && annular->faces.size() == 6,
          "Native common faces with holes failed reconstruction: " + error);
  check_closed(*annular);
  require(std::count_if(annular->faces.begin(), annular->faces.end(),
                        [](const Face &face) { return face.holes.size() == 1; }) == 2,
          "Common-face cancellation lost exterior trimming holes");
  require(std::count_if(annular->curves.begin(), annular->curves.end(),
                        [](const Curve &curve) { return curve.kind == CurveKind::Conic; }) == 6,
          "Native cell reconstruction replaced or discarded retained conic boundaries");
  require(std::count_if(annular->surfaces.begin(), annular->surfaces.end(),
                        [](const Surface &surface) {
                          return surface.kind == SurfaceKind::Extrusion;
                        }) == 4,
          "Native cell reconstruction changed analytic extrusion supports");

  const std::array<Model, 2> mismatched_holes{rings[0], ring_cell(1, .5)};
  require(!boolean_assemble_cells(mismatched_holes, 0, &error),
          "Equal outer loops with different hole trims were falsely canceled");
  std::array<Model, 2> two_holes{ring_cell(0, .2, true), ring_cell(1, .2, true)};
  for (Face &face : two_holes[1].faces) {
    std::reverse(face.holes.begin(), face.holes.end());
  }
  const auto reordered_holes = boolean_assemble_cells(two_holes, 0, &error);
  require(reordered_holes && reordered_holes->faces.size() == 8,
          "Hole storage order changed a common native face signature: " + error);
  check_closed(*reordered_holes);
  require(!boolean_assemble_cells(adjacent, -1, &error),
          "Negative cell sewing tolerance was accepted");
  require(!boolean_assemble_cells(adjacent, std::numeric_limits<double>::quiet_NaN(), &error),
          "Nonfinite cell sewing tolerance was accepted");
  const auto empty = boolean_assemble_cells({}, 0, &error);
  require(empty && empty->faces.empty() && empty->edges.empty() && empty->curves.empty() &&
              empty->surfaces.empty() && empty->vertices.empty(),
          "Empty cell input did not produce an empty native boundary");
  std::cout << "Native cell boundary reconstruction checks passed\n";
}
