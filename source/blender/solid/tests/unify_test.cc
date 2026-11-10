/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_cells.hh"
#include "SOL_unify.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>

using namespace blender::solid;

static void require(const bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static Model box(const Vec3 offset)
{
  Model model;
  Selection selection;
  Id first = 0, previous = 0;
  const Vec3 points[] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  std::string error;
  for (int i = 0; i < 4; ++i) {
    require(append_curve(model,
                         *line(points[i] + offset, points[(i + 1) % 4] + offset),
                         0, 1, previous, i == 3 ? first : 0, false, &selection, &error),
            error);
    const Edge &boundary = model.edges.back();
    if (!first) {
      first = boundary.vertices[0];
    }
    previous = boundary.vertices[1];
  }
  const auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  const auto body = extrude(*profile, 1, &error);
  require(bool(body), error);
  return body->model;
}

static Model half_ring(const bool lower)
{
  Model model;
  Selection selection;
  const double pi = std::numbers::pi;
  Curve outer{0, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}};
  Curve inner{0, CurveKind::Conic, {}, {.25, 0, 0}, {0, .25, 0}};
  const double a = lower ? pi : 0, b = lower ? 2 * pi : pi;
  Id first = 0, previous = 0;
  std::string error;
  auto append = [&](const Curve &support, const double start, const double end, const bool last) {
    require(append_curve(model, support, start, end, previous, last ? first : 0,
                         false, &selection, &error), error);
    const Edge &boundary = model.edges.back();
    if (!first) {
      first = boundary.vertices[0];
    }
    previous = boundary.vertices[1];
  };
  append(outer, a, b, false);
  append(*line(evaluate(outer, b), evaluate(inner, b)), 0, 1, false);
  append(inner, b, a, false);
  append(*line(evaluate(inner, a), evaluate(outer, a)), 0, 1, true);
  const auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
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
  const auto assembled = boolean_assemble_cells(adjacent, 0, &error);
  require(bool(assembled), error);
  const auto snapshot = encode(*assembled, {});
  const auto simplified = boolean_unify_coplanar_faces(*assembled, &error);
  require(bool(simplified), error);
  require(simplified->faces.size() == 6 && simplified->edges.size() == 12 &&
              simplified->vertices.size() == 8,
          "Coplanar unification retained cell subdivision faces or boundary edges");
  require(std::abs(planar_volume(*simplified) - 2) < 1e-14,
          "Coplanar unification changed occupied volume");
  check_closed(*simplified);
  require(encode(*assembled, {}) == snapshot, "Coplanar unification mutated its source");

  std::array<Model, 2> thin_cells = adjacent;
  for (Model &cell : thin_cells) {
    Selection all;
    for (const Vertex &vertex : cell.vertices) {
      all.vertices.push_back(vertex.id);
    }
    Affine scale;
    scale.columns[1].y = 1e-11;
    const auto changed = transform_selected(cell, all, scale, &error);
    require(bool(changed), error);
    cell = changed->model;
  }
  const auto thin = boolean_assemble_cells(thin_cells, 0, &error);
  require(bool(thin), error);
  const auto thin_boundary = boolean_unify_coplanar_faces(*thin, &error);
  require(thin_boundary && thin_boundary->faces.size() == 6 &&
              std::abs(planar_volume(*thin_boundary) - 2e-11) < 1e-24,
          "Coplanar unification collapsed a thin native material region: " + error);
  check_closed(*thin_boundary);

  std::vector<Model> ring_cells;
  for (int x = 0; x < 3; ++x) {
    for (int y = 0; y < 3; ++y) {
      if (x != 1 || y != 1) {
        ring_cells.push_back(box({double(x), double(y), 0}));
      }
    }
  }
  const auto ring = boolean_assemble_cells(ring_cells, 0, &error);
  require(bool(ring), error);
  const auto ring_boundary = boolean_unify_coplanar_faces(*ring, &error);
  require(ring_boundary && ring_boundary->faces.size() == 10 &&
              ring_boundary->edges.size() == 24 && ring_boundary->vertices.size() == 16,
          "Coplanar cycle reconstruction lost an inner region or retained subdivisions: " + error);
  require(std::count_if(ring_boundary->faces.begin(), ring_boundary->faces.end(),
                        [](const Face &face) { return face.holes.size() == 1; }) == 2,
          "Canceled planar subdivisions did not reconstruct the two native hole loops");
  require(std::abs(planar_volume(*ring_boundary) - 8) < 1e-13,
          "Coplanar hole reconstruction filled or removed material");
  check_closed(*ring_boundary);

  const std::array<Model, 2> curved_cells{half_ring(false), half_ring(true)};
  const auto curved = boolean_assemble_cells(curved_cells, 0, &error);
  require(bool(curved), error);
  const auto annulus = boolean_unify_coplanar_faces(*curved, &error);
  require(annulus && annulus->faces.size() == 6,
          "Native conic boundaries failed coplanar cycle reconstruction: " + error);
  check_closed(*annulus);
  size_t caps = 0;
  for (const Face &face : annulus->faces) {
    if (surface(*annulus, face.surface)->kind != SurfaceKind::Plane) {
      continue;
    }
    ++caps;
    require(face.holes.size() == 1, "Conic cap unification lost its native trimming hole");
    auto check_conics = [&](const auto &loop) {
      for (const Coedge use : loop) {
        require(curve(*annulus, edge(*annulus, use.edge)->curve)->kind == CurveKind::Conic,
                "Conic cap unification replaced an arc by a fitted or straight boundary");
      }
    };
    check_conics(face.outer);
    check_conics(face.holes.front());
  }
  require(caps == 2, "Conic annulus did not retain exactly its two planar caps");

  Model sloped = adjacent[1];
  Selection raised;
  for (const Vertex &vertex : sloped.vertices) {
    if (vertex.position.x == 2 && vertex.position.z == 1) {
      raised.vertices.push_back(vertex.id);
    }
  }
  Affine shift;
  shift.translation.z = 1e-11;
  const auto moved = transform_selected(sloped, raised, shift, &error);
  require(bool(moved), error);
  const std::array<Model, 2> crease_cells{adjacent[0], moved->model};
  const auto crease = boolean_assemble_cells(crease_cells, 0, &error);
  require(bool(crease), error);
  const auto retained_crease = boolean_unify_coplanar_faces(*crease, &error);
  require(retained_crease && retained_crease->faces.size() == 7,
          "A nearly coplanar physical crease was silently flattened: " + error);
  check_closed(*retained_crease);

  Model open = *assembled;
  open.faces.pop_back();
  const auto open_snapshot = encode(open, {});
  require(!boolean_unify_coplanar_faces(open, &error),
          "Coplanar unification accepted an open boundary");
  require(encode(open, {}) == open_snapshot, "Rejected coplanar unification mutated its source");
  const auto empty = boolean_unify_coplanar_faces({}, &error);
  require(empty && empty->faces.empty() && empty->vertices.empty(),
          "Empty native boundary failed coplanar unification");
  std::cout << "Native coplanar face unification checks passed\n";
}
