/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_geometry.hh"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>

using namespace blender::solid;
static void check(bool condition, const std::string &error)
{
  if (!condition) {
    std::cerr << error << '\n';
    std::exit(1);
  }
}
static Model polyhedron(const std::vector<Vec3> &points,
                        const std::vector<std::vector<int>> &faces)
{
  Model m;
  std::vector<Id> vertices;
  for (Vec3 p : points) {
    vertices.push_back(m.next_id);
    m.vertices.push_back({m.next_id++, p});
  }
  std::map<std::pair<int, int>, Id> edges;
  for (const auto &indices : faces) {
    Face f;
    for (size_t i = 0; i < indices.size(); ++i) {
      const int a = indices[i], b = indices[(i + 1) % indices.size()];
      auto key = std::minmax(a, b);
      if (!edges.contains(key)) {
        Selection added;
        check(append_curve(
                  m, *line(points[a], points[b]), 0, 1, vertices[a], vertices[b], false, &added),
              "Build edge");
        edges[key] = added.edges.front();
      }
      const Id id = edges.at(key);
      f.outer.push_back({id, edge(m, id)->vertices[0] != vertices[a]});
    }
    std::string error;
    const Vec3 origin = points[indices[0]], u = points[indices[1]] - origin;
    Vec3 n = cross(u, points[indices[2]] - origin);
    n = n / length(n);
    const Id support = m.next_id++;
    m.surfaces.push_back({support, SurfaceKind::Plane, origin, u, cross(n, u), 0});
    f.id = m.next_id++;
    f.surface = support;
    Vec3 center;
    for (int i : indices)
      center = center + points[i];
    center = center / double(indices.size());
    // Fixtures are convex and centered at the origin.
    if (dot(n, center) < 0)
      f.orientation = -f.orientation;
    m.faces.push_back(std::move(f));
  }
  std::string error;
  check(validate_body(m, m.faces.front().id, &error), error);
  return m;
}
static double volume(const Model &m)
{
  std::string error;
  auto display = tessellate(m, 64, &error);
  check(bool(display), error);
  double result = 0;
  for (const auto &p : display->polygons)
    for (size_t i = 1; i + 1 < p.size(); ++i)
      result += dot(display->positions[p[0]],
                    cross(display->positions[p[i]], display->positions[p[i + 1]])) /
                6;
  return result;
}
int main()
{
  std::string error;
  const Model cube = polyhedron(
      {{-1, -1, -1},
       {1, -1, -1},
       {1, 1, -1},
       {-1, 1, -1},
       {-1, -1, 1},
       {1, -1, 1},
       {1, 1, 1},
       {-1, 1, 1}},
      {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}});
  check(std::abs(volume(cube) - 8) < 1e-12, "Fixture volume");
  for (unsigned mask = 1; mask < (1u << cube.edges.size()); ++mask) {
    Selection s;
    s.mode = 2;
    for (size_t i = 0; i < cube.edges.size(); ++i)
      if (mask & (1u << i))
        s.edges.push_back(cube.edges[i].id);
    auto r = chamfer(cube, s, .125, false, &error);
    if (!r)
      std::cerr << "Bevel mask " << mask << ": ";
    check(bool(r), error);
    check(validate_body(r->model, r->model.faces.front().id, &error), error);
    check(volume(r->model) > 0 && volume(r->model) < 8, "Bevel changed body volume incorrectly");
    if (mask == 4095) {
      auto bytes = encode(r->model, r->selection);
      check(bool(decode(bytes.data(), bytes.size(), &error)), error);
    }
  }
  for (const Model &m :
       {cube,
        polyhedron({{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}},
                   {{0, 2, 4},
                    {2, 1, 4},
                    {1, 3, 4},
                    {3, 0, 4},
                    {2, 0, 5},
                    {1, 2, 5},
                    {3, 1, 5},
                    {0, 3, 5}})})
  {
    Selection s;
    s.mode = 1;
    for (const auto &v : m.vertices)
      s.vertices.push_back(v.id);
    auto r = chamfer(m, s, .1, true, &error);
    check(bool(r), error);
    check(validate_body(r->model, r->model.faces.front().id, &error), error);
    check(volume(r->model) > 0 && volume(r->model) < volume(m), "Vertex bevel volume");
  }
  Selection s;
  s.vertices = {cube.edges[0].vertices[0], cube.edges[0].vertices[1]};
  auto merged = merge_vertices(cube, s, MergeMode::Center, {}, 0, &error);
  check(bool(merged), error);
  check(merged->model.vertices.size() == 7, "Edge merge vertex count");
  check(validate_body(merged->model, merged->model.faces.front().id, &error), error);
  s.vertices.clear();
  for (const Vertex &v : cube.vertices)
    s.vertices.push_back(v.id);
  merged = merge_vertices(cube, s, MergeMode::Center, {}, 0, &error);
  check(bool(merged), error);
  check(merged->model.vertices.size() == 1 && merged->model.edges.empty() &&
            merged->model.faces.empty(),
        "Collapse all vertices");
  s = {};
  s.mode = 4;
  s.faces = {cube.faces[1].id};
  Affine shift;
  shift.translation = {.25, .5, 1};
  auto moved = transform_selected(cube, s, shift, &error);
  check(bool(moved), error);
  check(validate_body(moved->model, cube.faces[1].id, &error), error);
  check(std::abs(volume(moved->model) - 12) < 1e-10, "Moved cap volume");
  s = {};
  s.vertices = {cube.vertices[0].id};
  auto warped = transform_selected(cube, s, shift, &error);
  check(bool(warped), error);
  check(validate_body(warped->model, cube.faces[0].id, &error), error);
  check(warped->model.faces.size() > 6, "Warped quad must split into exact planes");
  Model wire;
  check(append_curve(wire, *line({0, 0, 0}, {1, 0, 0}), 0, 1, 0, 0, false, &s, &error), error);
  s = {};
  s.vertices = {wire.vertices[1].id};
  auto tip = extrude_elements(wire, s, {0, 2, 0}, &error);
  check(bool(tip), error);
  check(tip->model.vertices.size() == 3 && tip->model.edges.size() == 2, "Vertex extrusion");
  s = {};
  s.mode = 2;
  s.edges = {wire.edges[0].id};
  auto sheet = extrude_elements(wire, s, {0, 2, 0}, &error);
  check(bool(sheet), error);
  check(sheet->model.faces.size() == 1 && sheet->model.edges.size() == 4, "Edge extrusion");
  auto next = extrude_elements(sheet->model, sheet->selection, {0, 1, 1}, &error);
  check(bool(next), error);
  check(next->model.faces.size() == 2, "Repeated edge extrusion");
  check(!extrude_elements(wire, s, {1, 0, 0}, &error), "Reject collapsed sheet");
  Selection tip_selection;
  tip_selection.vertices = {tip->model.vertices[1].id};
  auto wire_bevel = chamfer(tip->model, tip_selection, .1, true, &error);
  check(bool(wire_bevel), error);
  check(wire_bevel->model.vertices.size() == 4 && wire_bevel->model.edges.size() == 3,
        "Wire vertex bevel");
  Model separate;
  Selection endpoints;
  check(append_curve(separate, *line({0, 0, 0}, {1, 0, 0}), 0, 1, 0, 0, false, nullptr, &error),
        error);
  check(append_curve(separate, *line({1, 0, 0}, {2, 0, 0}), 0, 1, 0, 0, false, nullptr, &error),
        error);
  endpoints.vertices = {separate.vertices[1].id, separate.vertices[2].id};
  auto welded = merge_vertices(separate, endpoints, MergeMode::Distance, {}, 1e-6, &error);
  check(bool(welded), error);
  check(welded->model.vertices.size() == 3 && welded->model.edges.size() == 2,
        "Weld coincident endpoints");
  endpoints.vertices = {separate.vertices.front().id, separate.vertices.back().id};
  endpoints.active = endpoints.vertices.back();
  auto last = merge_vertices(separate, endpoints, MergeMode::Last, {}, 0, &error);
  check(bool(last), error);
  check(vertex(last->model, endpoints.active)->position == Vec3{2, 0, 0},
        "Merge at active vertex");
  auto cursor = merge_vertices(separate, endpoints, MergeMode::Cursor, {1, 2, 3}, 0, &error);
  check(bool(cursor), error);
  check(vertex(cursor->model, cursor->selection.vertices.front())->position == Vec3{1, 2, 3},
        "Merge at cursor");
  s = {};
  s.mode = 4;
  s.faces = {cube.faces[0].id};
  auto removed = delete_elements(cube, s, DeleteMode::OnlyFaces, &error);
  check(bool(removed), error);
  check(removed->model.faces.size() == 5 && removed->model.edges.size() == 12,
        "Only faces deletion");
  std::cout << "Source editing checks passed (4,095 bevel edge combinations, valence-4 corners, "
               "merge, transforms, extrusion, deletion)\n";
}
