/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_geometry.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>

using namespace blender::solid;
static void check(bool condition, const std::string &error)
{
  if (!condition) {
    std::cerr << error << '\n';
    std::exit(1);
  }
}

static void check_shared_profile_joins(const Model &model)
{
  std::map<Id, std::vector<std::pair<Id, size_t>>> uses;
  int strips = 0;
  for (const Face &f : model.faces) {
    const Surface &surface_ref = *surface(model, f.surface);
    check(surface_ref.kind != SurfaceKind::Blend,
          "Reference fillet must not add setback corner wedges");
    if (surface_ref.kind == SurfaceKind::Ruled) {
      ++strips;
      check(curve(model, surface_ref.curve)->kind == CurveKind::RationalQuadratic &&
                curve(model, surface_ref.other_curve)->kind == CurveKind::RationalQuadratic,
            "Ruled fillet needs shared rational conic profiles");
    }
    for (size_t i = 0; i < f.outer.size(); ++i)
      uses[f.outer[i].edge].push_back({f.id, i});
  }
  check(strips > 0, "Fillet must contain ruled conic strips");
  auto normal = [&](const Face &f, size_t index, double fraction) {
    const Surface &s = *surface(model, f.surface);
    if (s.kind == SurfaceKind::Plane) {
      Vec3 n = cross(s.u, s.v) * f.orientation;
      return n / length(n);
    }
    const double t = f.outer[index].reversed ? 1 - fraction : fraction;
    const double u = s.start + (s.end - s.start) * (index == 0 ? t :
                                                    index == 2 ? 1 - t :
                                                    index == 1 ? 1 :
                                                                 0);
    const double v = index == 0 ? 0 : index == 2 ? 1 : index == 1 ? t : 1 - t;
    Vec3 n = ruled_normal(s, *curve(model, s.curve), *curve(model, s.other_curve), u, v) *
             f.orientation;
    return n / length(n);
  };
  for (const auto &[id, adjacent] : uses) {
    check(adjacent.size() == 2, "Reference bevel must have a shared closed boundary");
    const Face &a = *face(model, adjacent[0].first), &b = *face(model, adjacent[1].first);
    const auto ka = surface(model, a.surface)->kind, kb = surface(model, b.surface)->kind;
    if ((ka != SurfaceKind::Ruled && ka != SurfaceKind::Plane) ||
        (kb != SurfaceKind::Ruled && kb != SurfaceKind::Plane))
      continue;
    if (curve(model, edge(model, id)->curve)->kind != CurveKind::Line)
      continue;
    if (ka != SurfaceKind::Ruled && kb != SurfaceKind::Ruled)
      continue;
    for (double t : {.125, .25, .5, .75, .875})
      check(dot(normal(a, adjacent[0].second, t), normal(b, adjacent[1].second, t)) > 1 - 1e-9,
            "Fillet rails must remain tangent to their original planes and neighboring "
            "subdivisions");
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
  {
    Selection selected;
    selected.mode = 2;
    selected.edges = {cube.edges.front().id};
    const auto rounded = fillet(cube, selected, .2, 1, &error);
    check(bool(rounded), error);
    Selection cap;
    cap.mode = 4;
    for (const Face &f : rounded->model.faces) {
      if (surface(rounded->model, f.surface)->kind != SurfaceKind::Plane)
        continue;
      for (const Coedge &use : f.outer)
        if (curve(rounded->model, edge(rounded->model, use.edge)->curve)->kind ==
            CurveKind::RationalQuadratic)
          cap.faces = {f.id};
    }
    check(!cap.faces.empty(), "Single-edge fillet must leave an exact rounded planar end face");
    const auto copy = duplicate_elements(rounded->model, cap, &error);
    check(bool(copy), error);
    const auto profile = prepare_profile(copy->model, copy->selection, &error);
    check(bool(profile), error);
    const auto extended = extrude(*profile, .3, &error);
    check(bool(extended), error);
    check(validate_body(extended->model, extended->selection.active, &error), error);
    check(bool(tessellate(extended->model, 32, &error)), error);
  }
  {
    const Model angled = polyhedron(
        {{-1, -1, -1},
         {1, -1, -1},
         {1, 1, -1},
         {-1, 1, -1},
         {-.6, -1, 1},
         {1.4, -1, 1},
         {1.4, 1, 1},
         {-.6, 1, 1}},
        {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}});
    Selection cap;
    cap.mode = 4;
    cap.faces = {angled.faces[1].id};
    auto rounded = fillet(angled, cap, .35, 1, &error);
    check(bool(rounded), error);
    check_shared_profile_joins(rounded->model);
    const auto display = tessellate(rounded->model, 64, &error);
    check(bool(display), error);
    std::set<Id> logical_faces(display->face_source.begin(), display->face_source.end());
    check(logical_faces.size() == 10,
          "One segment must match six planes and four shared-profile strips");
    const auto bytes = encode(rounded->model, rounded->selection);
    const auto restored = decode(bytes.data(), bytes.size(), &error);
    check(bool(restored), error);
    check_shared_profile_joins(restored->model);
    Selection all;
    all.mode = 4;
    for (const Face &f : restored->model.faces)
      all.faces.push_back(f.id);
    Affine reflection;
    reflection.columns = {{{-1.1, 0, 0}, {.15, 1.2, 0}, {0, 0, .9}}};
    const auto transformed = transform_selected(restored->model, all, reflection, &error);
    check(bool(transformed), error);
    check_shared_profile_joins(transformed->model);
    const auto copied = duplicate_elements(transformed->model, all, &error);
    check(bool(copied), error);
    check(validate_body(copied->model, copied->selection.faces.front(), &error), error);
    Affine move;
    move.translation = {4, 0, 0};
    const auto moved = transform_selected(copied->model, copied->selection, move, &error);
    check(bool(moved), error);
    check_shared_profile_joins(moved->model);
  }
  {
    const Model skew = polyhedron(
        {{-1.4, -1, -1},
         {.6, -1, -1},
         {1.4, 1, -1},
         {-.6, 1, -1},
         {-1.4, -1, 1},
         {.6, -1, 1},
         {1.4, 1, 1},
         {-.6, 1, 1}},
        {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}});
    Selection selected;
    selected.mode = 4;
    selected.faces = {skew.faces[1].id};
    for (int divisions : {1, 3}) {
      auto rounded = fillet(skew, selected, .12, divisions, &error);
      check(bool(rounded), error);
      check_shared_profile_joins(rounded->model);
      check(validate_body(rounded->model, rounded->selection.active, &error), error);
      check(bool(tessellate(rounded->model, 32, &error)), error);
      const auto bytes = encode(rounded->model, rounded->selection);
      check(bool(decode(bytes.data(), bytes.size(), &error)), error);
    }
  }
  for (const Face &f : cube.faces) {
    Selection selected;
    selected.mode = 4;
    selected.faces = {f.id};
    for (int divisions : {1, 3}) {
      auto rounded = fillet(cube, selected, .12, divisions, &error);
      check(bool(rounded), error);
      check_shared_profile_joins(rounded->model);
      check(validate_body(rounded->model, rounded->selection.active, &error), error);
      const auto display = tessellate(rounded->model, 32, &error);
      check(bool(display), error);
      std::map<std::pair<int, int>, int> incidence;
      for (const auto &polygon : display->polygons)
        for (size_t i = 0; i < polygon.size(); ++i)
          incidence[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])]++;
      for (const auto &[pair, count] : incidence)
        check(count == 2, "Face perimeter fillet display must be watertight");
    }
  }
  for (const Model &oblique :
       {polyhedron(
            {{-1.4, -1, -1},
             {.6, -1, -1},
             {.6, 1, -1},
             {-1.4, 1, -1},
             {-.6, -1, 1},
             {1.4, -1, 1},
             {1.4, 1, 1},
             {-.6, 1, 1}},
            {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}}),
        polyhedron(
            {{-1, -1, -1},
             {1, -1, -1},
             {1, 1, -1},
             {-1, 1, -1},
             {-.65, -.8, 1},
             {.65, -.8, 1},
             {.65, .8, 1},
             {-.65, .8, 1}},
            {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}})})
  {
    for (const Edge &e : oblique.edges) {
      Selection selected;
      selected.mode = 2;
      selected.edges = {e.id};
      for (int divisions : {1, 3}) {
        auto rounded = fillet(oblique, selected, .12, divisions, &error);
        check(bool(rounded), error);
        check(validate_body(rounded->model, rounded->selection.active, &error), error);
        const auto display = tessellate(rounded->model, 32, &error);
        check(bool(display), error);
        std::map<std::pair<int, int>, int> incidence;
        for (const auto &polygon : display->polygons)
          for (size_t i = 0; i < polygon.size(); ++i)
            incidence[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])]++;
        for (const auto &[pair, count] : incidence)
          check(count == 2, "Oblique fillet display must be watertight");
        const auto bytes = encode(rounded->model, rounded->selection);
        check(bool(decode(bytes.data(), bytes.size(), &error)), error);
      }
    }
  }
  for (const Model &angled :
       {polyhedron(
            {{-1.4, -1, -1},
             {.6, -1, -1},
             {.6, 1, -1},
             {-1.4, 1, -1},
             {-.6, -1, 1},
             {1.4, -1, 1},
             {1.4, 1, 1},
             {-.6, 1, 1}},
            {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}}),
        polyhedron(
            {{-1, -1, -1},
             {1, -1, -1},
             {1, 1, -1},
             {-1, 1, -1},
             {-.65, -.8, 1},
             {.65, -.8, 1},
             {.65, .8, 1},
             {-.65, .8, 1}},
            {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}})})
  {
    for (const Face &f : angled.faces) {
      Selection selected;
      selected.mode = 4;
      selected.faces = {f.id};
      for (int divisions : {1, 3}) {
        auto rounded = fillet(angled, selected, .12, divisions, &error);
        check(bool(rounded), error);
        check_shared_profile_joins(rounded->model);
        check(validate_body(rounded->model, rounded->selection.active, &error), error);
        const auto display = tessellate(rounded->model, 32, &error);
        check(bool(display), error);
        std::map<std::pair<int, int>, int> incidence;
        for (const auto &polygon : display->polygons)
          for (size_t i = 0; i < polygon.size(); ++i)
            incidence[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])]++;
        for (const auto &[pair, count] : incidence)
          check(count == 2, "Mixed-angle face fillet must have a watertight display");
      }
    }
  }
  for (const Edge &e : cube.edges) {
    Selection selected;
    selected.mode = 2;
    selected.edges = {e.id};
    for (int subdivisions : {1, 3}) {
      auto rounded = fillet(cube, selected, .2, subdivisions, &error);
      check(bool(rounded), error);
      check(validate_body(rounded->model, rounded->selection.active, &error), error);
      check(bool(tessellate(rounded->model, 32, &error)), error);
    }
  }
  {
    Selection selected;
    selected.mode = 2;
    for (const Edge &e : cube.edges)
      if (std::abs(vertex(cube, e.vertices[1])->position.z -
                   vertex(cube, e.vertices[0])->position.z) > 1)
        selected.edges.push_back(e.id);
    auto rounded = fillet(cube, selected, .2, 2, &error);
    check(bool(rounded), error);
    check(validate_body(rounded->model, rounded->selection.active, &error), error);
    check(bool(tessellate(rounded->model, 32, &error)), error);
  }
  for (int subdivisions : {1, 2, 3}) {
    Selection selected;
    selected.mode = 2;
    for (const Edge &e : cube.edges)
      selected.edges.push_back(e.id);
    auto rounded = fillet(cube, selected, .2, subdivisions, &error);
    check(bool(rounded), error);
    check(validate_body(rounded->model, rounded->selection.active, &error), error);
    int cylinders = 0, spheres = 0;
    for (const Surface &s : rounded->model.surfaces) {
      cylinders += s.kind == SurfaceKind::Ruled;
      spheres += s.kind == SurfaceKind::Sphere;
    }
    check(cylinders == 12 * subdivisions, "Fillet must split exact cylinder strips");
    check(spheres == 8 * subdivisions * subdivisions, "Fillet must split exact sphere corners");
    const auto display = tessellate(rounded->model, 32, &error);
    check(bool(display), error);
    std::map<std::pair<int, int>, int> incidence;
    for (const auto &polygon : display->polygons)
      for (size_t i = 0; i < polygon.size(); ++i)
        incidence[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])]++;
    for (const auto &[edge, count] : incidence)
      check(count == 2, "Fillet display must have a watertight shared boundary");
    const auto bytes = encode(rounded->model, rounded->selection);
    check(bool(decode(bytes.data(), bytes.size(), &error)), error);
  }
  Selection loose_face_selection;
  loose_face_selection.mode = 4;
  loose_face_selection.faces = {cube.faces.front().id};
  auto with_loose_face = duplicate_elements(cube, loose_face_selection, &error);
  check(bool(with_loose_face), error);
  Selection body_edge_selection;
  body_edge_selection.mode = 2;
  body_edge_selection.edges = {cube.edges.front().id};
  auto body_bevel = chamfer(with_loose_face->model, body_edge_selection, .125, false, &error);
  check(bool(body_bevel), error);
  check(validate_body(body_bevel->model, body_bevel->selection.active, &error), error);
  check(body_bevel->model.faces.size() == 8, "Bevel must leave the unrelated loose face alone");
  check(!chamfer(with_loose_face->model, with_loose_face->selection, .125, false, &error),
        "Bevel of an open face must refuse to claim it is a closed body");
  check(error.find("1 attached face") != std::string::npos,
        "Open-edge error must name the problem");
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
      auto patched = chamfer(cube, s, .125, false, &error, BevelCornerStyle::Patch);
      check(bool(patched), error);
      check(validate_body(patched->model, patched->selection.active, &error), error);
      check(patched->model.faces.size() == r->model.faces.size() + 8,
            "Patch join adds a planar cap at each cube corner");
      int mesh_style_corners = 0;
      for (const Face &face : patched->model.faces) {
        if (face.outer.size() != 3)
          continue;
        bool mesh_style = true;
        for (const Coedge &use : face.outer) {
          const Edge &edge_use = *edge(patched->model, use.edge);
          const Vec3 p = vertex(patched->model, edge_use.vertices[use.reversed ? 1 : 0])->position;
          const std::array<double, 3> coordinates = {std::abs(p.x), std::abs(p.y), std::abs(p.z)};
          int original_planes = 0;
          for (double coordinate : coordinates)
            original_planes += std::abs(coordinate - 1) < 1e-9;
          mesh_style &= original_planes == 1;
        }
        mesh_style_corners += mesh_style;
      }
      check(mesh_style_corners == 8,
            "Patch corners must meet the original faces like the mesh bevel");
      check(volume(patched->model) > 0 && volume(patched->model) < volume(r->model),
            "Corner patches must trim the sharp bevel join");
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
