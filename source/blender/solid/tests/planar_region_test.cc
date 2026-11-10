/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_planar_region.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <set>

using namespace blender::solid;

static void require(const bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static void common_plane(Model &model)
{
  for (Surface &surface : model.surfaces) {
    if (surface.kind == SurfaceKind::Plane) {
      surface.origin = {};
      surface.u = {1, 0, 0};
      surface.v = {0, 1, 0};
    }
  }
}

static Model polygon(const std::vector<Vec3> &points)
{
  Model model;
  Selection selection;
  Id first = 0, previous = 0;
  std::string error;
  for (size_t i = 0; i < points.size(); ++i) {
    require(append_curve(model,
                         *line(points[i], points[(i + 1) % points.size()]),
                         0, 1, previous, i + 1 == points.size() ? first : 0,
                         false, &selection, &error),
            error);
    const Edge &boundary = model.edges.back();
    if (!first) {
      first = boundary.vertices[0];
    }
    previous = boundary.vertices[1];
  }
  const auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  Model result = profile->model;
  common_plane(result);
  return result;
}

static Model rectangle(const double x0, const double y0, const double x1, const double y1)
{
  return polygon({{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}});
}

static Model disc(const Vec3 center, const double radius, const bool bottom_seam = false)
{
  Model model;
  Selection selection;
  std::string error;
  const Vec3 start = center + (bottom_seam ? Vec3{0, -radius, 0} : Vec3{radius, 0, 0});
  require(append_curve(model, *circle(center, start, {0, 0, 1}),
                       0, 2 * std::numbers::pi, 0, 0, true, &selection, &error), error);
  const auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  Model result = profile->model;
  common_plane(result);
  return result;
}

static Model half_disc(const double radius)
{
  Model model;
  Selection selection;
  std::string error;
  Curve support{0, CurveKind::Conic, {}, {radius, 0, 0}, {0, radius, 0}};
  require(append_curve(model, support, 0, std::numbers::pi,
                       0, 0, false, &selection, &error), error);
  const Id first = model.edges.front().vertices[0], previous = model.edges.front().vertices[1];
  require(append_curve(model,
                       *line(evaluate(support, std::numbers::pi), evaluate(support, 0)),
                       0, 1, previous, first, false, &selection, &error), error);
  const auto profile = prepare_profile(model, selection, &error);
  require(bool(profile), error);
  Model result = profile->model;
  common_plane(result);
  return result;
}

static void add_hole(Model &model, const Model &hole)
{
  /* Append only the hole's native curves, edges and vertices, retaining its
   * own analytic parameterization and reversing its boundary traversal. */
  std::map<Id, Id> vertices, curves, edges;
  for (Vertex vertex : hole.vertices) {
    const Id old = vertex.id;
    vertex.id = model.next_id++;
    vertices[old] = vertex.id;
    model.vertices.push_back(vertex);
  }
  for (Curve curve : hole.curves) {
    const Id old = curve.id;
    curve.id = model.next_id++;
    curves[old] = curve.id;
    model.curves.push_back(curve);
  }
  for (Edge edge : hole.edges) {
    const Id old = edge.id;
    edge.id = model.next_id++;
    edge.curve = curves.at(edge.curve);
    for (Id &vertex : edge.vertices) {
      vertex = vertices.at(vertex);
    }
    edges[old] = edge.id;
    model.edges.push_back(edge);
  }
  std::vector<Coedge> loop;
  for (auto i = hole.faces.front().outer.rbegin(); i != hole.faces.front().outer.rend(); ++i) {
    loop.push_back({edges.at(i->edge), !i->reversed});
  }
  model.faces.front().holes.push_back(std::move(loop));
  std::string error;
  require(validate(model, &error), error);
}

static double area(const Model &model)
{
  double result = 0;
  for (const Face &face : model.faces) {
    auto integrate = [&](const std::vector<Coedge> &loop) {
      for (const Coedge use : loop) {
        const Edge &e = *edge(model, use.edge);
        const Curve &c = *curve(model, e.curve);
        const double a = use.reversed ? e.end : e.start;
        const double b = use.reversed ? e.start : e.end;
        Vec3 integral;
        if (c.kind == CurveKind::Line) {
          integral = cross(c.origin, c.u) * (b - a);
        }
        else {
          integral = cross(c.origin, c.u) * (std::cos(b) - std::cos(a)) +
                     cross(c.origin, c.v) * (std::sin(b) - std::sin(a)) +
                     cross(c.u, c.v) * (b - a);
        }
        result += integral.z / 2;
      }
    };
    integrate(face.outer);
    for (const auto &hole : face.holes) {
      integrate(hole);
    }
  }
  return result;
}

static void check_result(const Model &model, const double expected)
{
  std::string error;
  require(validate(model, &error), error);
  require(std::abs(area(model) - expected) < 1e-11,
          "Native planar difference changed the expected material area");
  std::set<Id> used_edges, used_vertices, used_surfaces, used_curves;
  for (const Face &face : model.faces) {
    used_surfaces.insert(face.surface);
    auto count = [&](const auto &loop) {
      for (const Coedge use : loop) {
        used_edges.insert(use.edge);
      }
    };
    count(face.outer);
    for (const auto &hole : face.holes) {
      count(hole);
    }
  }
  for (const Edge &edge : model.edges) {
    used_vertices.insert(edge.vertices.begin(), edge.vertices.end());
    used_curves.insert(edge.curve);
  }
  require(used_edges.size() == model.edges.size() &&
              used_vertices.size() == model.vertices.size() &&
              used_surfaces.size() == model.surfaces.size() &&
              used_curves.size() == model.curves.size(),
          "Native planar difference retained unused geometry");
}

static void map_plane(Model &model, const Vec3 origin, const Vec3 u, const Vec3 v)
{
  auto direction = [&](const Vec3 p) { return u * p.x + v * p.y; };
  for (Vertex &vertex : model.vertices) {
    vertex.position = origin + direction(vertex.position);
  }
  for (Curve &curve : model.curves) {
    curve.origin = origin + direction(curve.origin);
    curve.u = direction(curve.u);
    curve.v = direction(curve.v);
    curve.w = direction(curve.w);
  }
  for (Surface &surface : model.surfaces) {
    surface.origin = origin;
    surface.u = u;
    surface.v = v;
  }
}

int main()
{
  const Model source = rectangle(0, 0, 4, 4);
  const Id source_face = source.faces.front().id;
  const auto source_snapshot = encode(source, {});
  std::string error;
  auto subtract = [&](const std::span<const Model> regions) {
    return boolean_subtract_planar_regions(source, source_face, regions, 0, &error);
  };
  const auto unchanged = subtract({});
  require(bool(unchanged), error);
  check_result(*unchanged, 16);

  const std::array<Model, 1> interior{rectangle(1, 1, 3, 3)};
  const auto interior_snapshot = encode(interior[0], {});
  const auto hole = subtract(interior);
  require(hole && hole->faces.size() == 1 && hole->faces.front().holes.size() == 1,
          "A contained planar region did not become a native hole: " + error);
  check_result(*hole, 12);
  const auto solid = extrude(Profile{source, source_face, {0, 0, 1}, false}, 1, &error);
  require(bool(solid), error);
  const auto solid_snapshot = encode(solid->model, {});
  const auto from_solid = boolean_subtract_planar_regions(
      solid->model, source_face, interior, 0, &error);
  require(from_solid && from_solid->faces.size() == 1,
          "A source face could not be extracted from its whole native body: " + error);
  check_result(*from_solid, 12);
  require(encode(solid->model, {}) == solid_snapshot,
          "Planar difference mutated unrelated faces of the source body");

  const std::array<Model, 1> notch{rectangle(1, 0, 3, 1)};
  const auto notched = subtract(notch);
  require(notched && notched->faces.size() == 1 && notched->faces.front().holes.empty() &&
              notched->faces.front().outer.size() == 8,
          "Boundary vertices did not jointly split and cancel a native notch: " + error);
  check_result(*notched, 14);

  const std::array<Model, 1> cover{source};
  const auto covered = subtract(cover);
  require(covered && covered->faces.empty(), "Complete planar coverage was not empty: " + error);
  check_result(*covered, 0);
  const std::array<Model, 4> tiles{rectangle(0, 0, 2, 2), rectangle(2, 0, 4, 2),
                                  rectangle(0, 2, 2, 4), rectangle(2, 2, 4, 4)};
  const auto tiled = subtract(tiles);
  require(tiled && tiled->faces.empty(),
          "Jointly partitioned regions did not cancel shared internal boundaries: " + error);
  check_result(*tiled, 0);

  Model annular = rectangle(1, 1, 3, 3);
  add_hole(annular, rectangle(1.5, 1.5, 2.5, 2.5));
  const std::array<Model, 1> annular_region{annular};
  const auto island = subtract(annular_region);
  require(island && island->faces.size() == 2,
          "A region hole did not remain as a separate native island: " + error);
  check_result(*island, 13);
  Model nested_ring = rectangle(1, 1, 3, 3);
  add_hole(nested_ring, rectangle(1.25, 1.25, 2.75, 2.75));
  const std::array<Model, 2> nested_regions{rectangle(1.5, 1.5, 2.5, 2.5), nested_ring};
  const auto nested_island = subtract(nested_regions);
  require(nested_island && nested_island->faces.size() == 2 &&
              std::all_of(nested_island->faces.begin(), nested_island->faces.end(),
                          [](const Face &face) { return face.holes.size() == 1; }),
          "Region order changed a remaining island's hole ownership: " + error);
  check_result(*nested_island, 13.25);

  Model full_ring = source;
  add_hole(full_ring, rectangle(1, 1, 3, 3));
  const std::array<Model, 1> full_ring_region{full_ring};
  const auto only_island = subtract(full_ring_region);
  require(only_island && only_island->faces.size() == 1 &&
              only_island->faces.front().holes.empty(),
          "A full-cover region with a hole did not leave only its island: " + error);
  check_result(*only_island, 4);
  const std::array<Model, 1> same_ring{full_ring};
  const auto covered_ring = boolean_subtract_planar_regions(
      full_ring, full_ring.faces.front().id, same_ring, 0, &error);
  require(covered_ring && covered_ring->faces.empty(),
          "Matching source and region holes did not cancel completely: " + error);
  const std::array<Model, 1> covered_source_hole{rectangle(.25, .25, 3.75, 3.75)};
  require(!boolean_subtract_planar_regions(full_ring, full_ring.faces.front().id,
                                          covered_source_hole, 0, &error),
          "A supposedly contained region covered excluded source-hole material");

  const Model round = disc({}, 2);
  const std::array<Model, 1> inner_disc{disc({}, 1)};
  const auto round_ring = boolean_subtract_planar_regions(
      round, round.faces.front().id, inner_disc, 0, &error);
  require(round_ring && round_ring->faces.size() == 1 &&
              round_ring->faces.front().holes.size() == 1 && round_ring->curves.size() == 2 &&
              std::all_of(round_ring->curves.begin(), round_ring->curves.end(),
                          [](const Curve &curve) { return curve.kind == CurveKind::Conic; }),
          "Native conic difference fitted or discarded its exact circular trims: " + error);
  check_result(*round_ring, 3 * std::numbers::pi);
  const std::array<Model, 1> upper_half{half_disc(2)};
  const auto curved_notch = boolean_subtract_planar_regions(
      round, round.faces.front().id, upper_half, 0, &error);
  require(curved_notch && curved_notch->faces.size() == 1 &&
              curved_notch->faces.front().holes.empty() && curved_notch->edges.size() == 2,
          "A shared conic span was not split and canceled natively: " + error);
  check_result(*curved_notch, 2 * std::numbers::pi);

  const std::array<Model, 2> circles{disc({1, 1, 0}, .5), disc({3, 3, 0}, .5)};
  const auto two_holes = subtract(circles);
  require(two_holes && two_holes->faces.front().holes.size() == 2,
          "Disjoint conic subtraction regions failed native reconstruction: " + error);
  check_result(*two_holes, 16 - .5 * std::numbers::pi);

  const std::array<Model, 2> crossing{disc({1.5, 1.5, 0}, 1), disc({1.5, 2.5, 0}, 1)};
  require(!subtract(crossing), "Unmarked conic intersections were silently accepted");
  const std::array<Model, 2> duplicate{interior[0], interior[0]};
  require(!subtract(duplicate), "Duplicate planar subtraction regions were silently accepted");
  const std::array<Model, 1> outside{rectangle(-1, 1, 1, 2)};
  require(!subtract(outside), "A region outside the source face was accepted");

  const Model touching_source = polygon({{0, 0, 0}, {2, 0, 0}, {4, 0, 0},
                                          {4, 4, 0}, {0, 4, 0}});
  const std::array<Model, 1> tangent{disc({2, 1, 0}, 1, true)};
  require(!boolean_subtract_planar_regions(touching_source, touching_source.faces.front().id,
                                          tangent, 0, &error),
          "A tangential hole/outer contact was silently converted into a valid trim");
  const std::array<Model, 1> close_hole{disc({2, 1 + 1e-11, 0}, 1, true)};
  const auto separated_hole = subtract(close_hole);
  require(separated_hole && separated_hole->faces.front().holes.size() == 1,
          "A separated near-contact hole was snapped onto the source boundary: " + error);
  check_result(*separated_hole, 16 - std::numbers::pi);
  require(std::all_of(separated_hole->vertices.begin(), separated_hole->vertices.end(),
                      [](const Vertex &vertex) {
                        return vertex.position.x != 2 || vertex.position.y > 0;
                      }),
          "Near-contact planar subtraction moved a retained native vertex");

  Model tilted = source, tilted_region = interior[0];
  const Vec3 tilted_origin{.3, -.2, 2}, tilted_u{1, .3, .2}, tilted_v{-.1, 1, .4};
  map_plane(tilted, tilted_origin, tilted_u, tilted_v);
  map_plane(tilted_region, tilted_origin, tilted_u, tilted_v);
  tilted_region.surfaces.front().v = tilted_v * -1;
  tilted_region.faces.front().orientation = -1;
  const std::array<Model, 1> flipped_plane_region{tilted_region};
  const auto tilted_hole = boolean_subtract_planar_regions(
      tilted, tilted.faces.front().id, flipped_plane_region, 0, &error);
  require(tilted_hole && tilted_hole->faces.size() == 1 &&
              tilted_hole->faces.front().holes.size() == 1,
          "Literal sign flips of a tilted native plane lost support identity: " + error);
  require(validate(*tilted_hole, &error), error);
  Model reversed_source = source;
  reversed_source.faces.front().orientation = -1;
  const auto reversed_result = boolean_subtract_planar_regions(
      reversed_source, reversed_source.faces.front().id, interior, 0, &error);
  require(reversed_result && reversed_result->faces.front().orientation == -1,
          "Planar difference changed the source physical face orientation: " + error);
  check_result(*reversed_result, 12);

  require(encode(source, {}) == source_snapshot && encode(interior[0], {}) == interior_snapshot,
          "Native planar subtraction mutated one of its inputs");
  require(!boolean_subtract_planar_regions(source, source_face, interior, -1, &error),
          "Negative planar subtraction tolerance was accepted");
  require(!boolean_subtract_planar_regions(source, source_face, interior,
                                          std::numeric_limits<double>::quiet_NaN(), &error),
          "Nonfinite planar subtraction tolerance was accepted");
  std::cout << "Native planar region subtraction checks passed\n";
}
