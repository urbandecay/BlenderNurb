/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_geometry.hh"
#include "extrusion.hh"
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
    Selection selected_face;
    selected_face.mode = 4;
    selected_face.faces = {cube.faces[1].id};
    const auto raised = extrude_elements(cube, selected_face, {0, 0, .4}, &error);
    check(bool(raised), "Planar face extrusion: " + error);
    check(raised->selection.mode == 4 && raised->selection.faces.size() == 1,
          "Face extrusion must keep the translated cap selected in face mode");
    check(validate_body(raised->model, raised->selection.active, &error), error);
    check(raised->model.faces.size() == cube.faces.size() + 4,
          "Extruding a body face must add a cap and four side walls");
  }
  {
    Selection selected_faces;
    selected_faces.mode = 4;
    selected_faces.faces = {cube.faces[0].id, cube.faces[1].id};
    const std::vector<Vec3> offsets{{0, 0, -.4}, {0, 0, .4}};
    const auto extruded = extrude_faces_individually(cube, selected_faces, offsets, &error);
    check(bool(extruded), "Individual planar face extrusion: " + error);
    check(extruded->selection.mode == 4 && extruded->selection.faces.size() == 2,
          "Individual face extrusion must keep both caps selected in face mode");
    check(extruded->model.faces.size() == cube.faces.size() + 8,
          "Extruding two cube faces individually must add four walls per face");
    check(validate_body(extruded->model, extruded->selection.active, &error), error);
    check(std::abs(volume(extruded->model) - 11.2) < 1e-10,
          "Individual cap extrusions must extend both ends by their own offsets");
  }
  {
    Model current = cube;
    Selection cap;
    cap.mode = 4;
    cap.faces = {cube.faces[1].id};
    cap.active = cap.faces.front();
    for (int i = 0; i < 3; ++i) {
      const auto pushed = extrude_manifold(current, cap, {0, 0, .4}, &error);
      check(bool(pushed), "Manifold push-pull: " + error);
      current = pushed->model;
      cap = pushed->selection;
      check(cap.mode == 4 && cap.faces.size() == 1,
            "Manifold push-pull must keep its moved cap selected");
      check(current.faces.size() == cube.faces.size(),
            "Manifold push-pull must dissolve the coplanar side seams after each step");
      check(validate_body(current, cap.active, &error), error);
    }
    check(std::abs(volume(current) - 12.8) < 1e-10,
          "Repeated manifold push-pull must extend one continuous solid prism");
    const int merged_sides = int(std::count_if(current.faces.begin(),
                                               current.faces.end(),
                                               [&](const Face &f) {
                                                 return f.id != cap.active && f.outer.size() > 4;
                                               }));
    check(merged_sides >= 4,
          "Manifold push-pull must merge each new side wall into its coplanar side face");
  }
  {
    Selection corner_vertex;
    corner_vertex.mode = 1;
    corner_vertex.vertices = {cube.vertices.front().id};
    const auto rounded = fillet(cube, corner_vertex, .2, 3, &error, true);
    check(bool(rounded), error);
    const Face *corner_patch = nullptr;
    for (const Face &f : rounded->model.faces)
      if (surface(rounded->model, f.surface)->kind == SurfaceKind::Sphere && f.outer.size() == 4) {
        corner_patch = &f;
        break;
      }
    check(corner_patch != nullptr, "Find a curved quad on the rounded solid corner");
    Selection selected_patch;
    selected_patch.mode = 4;
    selected_patch.faces = {corner_patch->id};
    const Surface &original_sphere = *surface(rounded->model, corner_patch->surface);
    for (double amount : {.04, -.04}) {
      const auto offset_patch = extrude_region_normals(
          rounded->model, selected_patch, amount, false, &error);
      check(bool(offset_patch), "Curved normal extrusion: " + error);
      const Face &cap = *face(offset_patch->model, offset_patch->selection.active);
      const Surface &offset_sphere = *surface(offset_patch->model, cap.surface);
      check(offset_sphere.kind == SurfaceKind::Sphere &&
                length(offset_sphere.origin - original_sphere.origin) < 1e-10,
            "Normal extrusion must preserve the sphere center rather than translate its face");
      const double sign = corner_patch->orientation *
          (dot(original_sphere.u, cross(original_sphere.v, original_sphere.w)) < 0 ? -1 : 1);
      check(std::abs(length(offset_sphere.u) - length(original_sphere.u) - sign * amount) < 1e-10,
            "Every point of a spherical cap must move by the requested normal distance");
      check(validate_body(offset_patch->model, cap.id, &error), error);
      check(bool(tessellate(offset_patch->model, 32, &error)), error);
      const auto individual = extrude_individual_normals(
          rounded->model, selected_patch, amount, &error);
      check(bool(individual), "Individual curved normal extrusion: " + error);
    }
    std::vector<Id> corner_patches;
    for (const Face &f : rounded->model.faces)
      if (f.surface == corner_patch->surface && f.outer.size() == 4)
        corner_patches.push_back(f.id);
    check(corner_patches.size() >= 3, "Find neighboring curved corner faces");
    Selection selected_corner_patches;
    selected_corner_patches.mode = 4;
    selected_corner_patches.faces.assign(corner_patches.begin(), corner_patches.begin() + 3);
    selected_corner_patches.active = selected_corner_patches.faces.back();
    const auto individual_corner = extrude_individual_normals(
        rounded->model, selected_corner_patches, .03, &error);
    check(bool(individual_corner), "Individual extrusion of adjacent corner faces: " + error);
    check(individual_corner->selection.mode == 4 &&
              individual_corner->selection.faces.size() == 3,
          "Individual corner extrusion must select each new cap in face mode");
    check(validate_body(individual_corner->model,
                        individual_corner->selection.active,
                        &error),
          "Adjacent individual corner extrusion must remain closed: " + error);
    std::vector<Vec3> individual_offsets;
    for (Id id : selected_corner_patches.faces) {
      const Face &f = *face(rounded->model, id);
      std::vector<Vec3> points;
      for (const Coedge &use : f.outer) {
        const Edge &e = *edge(rounded->model, use.edge);
        points.push_back(vertex(rounded->model, e.vertices[use.reversed ? 1 : 0])->position);
      }
      Vec3 normal{};
      for (size_t i = 1; i + 1 < points.size(); ++i)
        normal = normal + cross(points[i] - points[0], points[i + 1] - points[0]);
      normal = normal * double(f.orientation);
      normal = normal * (.03 / length(normal));
      individual_offsets.push_back(normal);
    }
    const auto discrete_corner = extrude_faces_individually(
        rounded->model, selected_corner_patches, individual_offsets, &error);
    check(bool(discrete_corner), "Discrete extrusion of adjacent corner faces: " + error);
    check(discrete_corner->selection.mode == 4 && discrete_corner->selection.faces.size() == 3,
          "Discrete corner extrusion must select each new cap in face mode");
    check(validate_body(discrete_corner->model, discrete_corner->selection.active, &error),
          "Discrete individual corner extrusion must remain closed: " + error);
    Model scaled = rounded->model;
    auto scale_point = [](Vec3 p) { return Vec3{2 * p.x, p.y, p.z}; };
    for (Vertex &v : scaled.vertices)
      v.position = scale_point(v.position);
    for (Curve &c : scaled.curves) {
      c.origin = scale_point(c.origin);
      c.u = scale_point(c.u);
      c.v = scale_point(c.v);
      c.w = scale_point(c.w);
    }
    for (Surface &s : scaled.surfaces) {
      s.origin = scale_point(s.origin);
      s.u = scale_point(s.u);
      s.v = scale_point(s.v);
      s.w = scale_point(s.w);
      for (Vec3 &p : s.blend)
        p = scale_point(p);
    }
    check(validate(scaled, &error), "Non-uniformly scaled rounded solid: " + error);
    const auto scaled_extrusion = extrude_region_normals(
        scaled, selected_patch, .03, false, &error);
    check(bool(scaled_extrusion), "Elliptical corner normal extrusion: " + error);
    const Face &scaled_source = *face(scaled, corner_patch->id);
    const Surface &ellipsoid = *surface(scaled, scaled_source.surface);
    Vec3 center;
    for (const Coedge &use : scaled_source.outer) {
      const Edge &e = *edge(scaled, use.edge);
      center = center + vertex(scaled, e.vertices[0])->position;
    }
    center = center / double(scaled_source.outer.size());
    const double det = dot(ellipsoid.u, cross(ellipsoid.v, ellipsoid.w));
    const Vec3 delta = center - ellipsoid.origin;
    const Vec3 q{dot(delta, cross(ellipsoid.v, ellipsoid.w)) / det,
                 dot(delta, cross(ellipsoid.w, ellipsoid.u)) / det,
                 dot(delta, cross(ellipsoid.u, ellipsoid.v)) / det};
    Vec3 expected_normal = cross(ellipsoid.v, ellipsoid.w) * q.x +
                           cross(ellipsoid.w, ellipsoid.u) * q.y +
                           cross(ellipsoid.u, ellipsoid.v) * q.z;
    expected_normal = expected_normal * (scaled_source.orientation * (det < 0 ? -1.0 : 1.0));
    expected_normal = expected_normal * (.03 / length(expected_normal));
    const Face &scaled_cap = *face(scaled_extrusion->model, scaled_extrusion->selection.active);
    const Surface &scaled_cap_surface = *surface(scaled_extrusion->model, scaled_cap.surface);
    check(length((scaled_cap_surface.origin - ellipsoid.origin) - expected_normal) < 1e-10,
          "An elliptical corner face must extrude along its representative analytic normal");
    check(validate_body(scaled_extrusion->model, scaled_cap.id, &error), error);
    check(bool(tessellate(scaled_extrusion->model, 32, &error)), error);
    const auto raised = extrude_elements(rounded->model, selected_patch, {.13, .07, -.2}, &error);
    check(bool(raised), "Curved face extrusion: " + error);
    check(raised->selection.mode == 4 && raised->selection.faces.size() == 1,
          "Curved face extrusion must stay in face mode and select its cap");
    check(face(raised->model, corner_patch->id) == nullptr,
          "Curved face extrusion must move the source face and replace it with the cap");
    check(validate_body(raised->model, raised->selection.active, &error), error);
    const auto display = tessellate(raised->model, 32, &error);
    check(bool(display), error);
    std::map<std::pair<int, int>, int> edge_uses;
    for (const auto &polygon : display->polygons)
      for (size_t i = 0; i < polygon.size(); ++i)
        edge_uses[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])]++;
    for (const auto &[edge_pair, uses] : edge_uses)
      check(uses == 2, "Curved face extrusion must not leave a hole in the displayed solid");

    const Selection cap_selection = normalize_selection(raised->model, raised->selection);
    check(bool(extrusion_cap_wall_surfaces(raised->model, cap_selection)),
          "Recognize the curved cap's extrusion walls");
    const auto cap_samples = tessellate(raised->model, 64, &error);
    check(bool(cap_samples), error);
    for (Vec3 delta : {Vec3{.03, -.02, .05}, Vec3{-.01, .04, -.03}, Vec3{}}) {
      Affine translation;
      translation.translation = delta;
      auto moved = transform_selected(raised->model, cap_selection, translation, &error);
      check(bool(moved), "Translate a curved extrusion cap: " + error);
      check(validate_body(moved->model, moved->selection.active, &error), error);
      check(moved->selection.mode == 4 && moved->selection.faces == cap_selection.faces,
            "Moving a curved cap must preserve face mode and selected source faces");
      auto cached = translated_cap_samples(raised->model, cap_selection, moved->model,
                                          *cap_samples, delta, &error);
      auto fresh = tessellate(moved->model, 64, &error);
      check(bool(cached) && bool(fresh), error);
      check(cached->polygons == fresh->polygons && cached->positions.size() == fresh->positions.size(),
            "Moving a curved cap must preserve display topology");
      for (size_t i = 0; i < fresh->positions.size(); ++i)
        check(length(cached->positions[i] - fresh->positions[i]) < 1e-10,
              "Cached curved cap translation disagrees with exact tessellation");
      for (size_t i = 0; i < fresh->corner_normals.size(); ++i)
        for (size_t j = 0; j < fresh->corner_normals[i].size(); ++j)
          check(length(cached->corner_normals[i][j] - fresh->corner_normals[i][j]) < 1e-10,
                "Curved cap translation must retain exact wall shading");
    }

    auto prepared = PreparedFaceRegionExtrusion::create(
        std::make_shared<const Model>(rounded->model), selected_patch, &error);
    check(bool(prepared), "Prepare a curved face-region extrusion: " + error);
    for (const Vec3 offset : {Vec3{.13, .07, -.2}, Vec3{.26, .14, -.4}, Vec3{.1, .2, -.15}}) {
      auto regular = extrude_elements(rounded->model, selected_patch, offset, &error);
      auto preview = prepared->preview(offset, true, &error);
      check(bool(regular) && bool(preview), "Prepared curved extrusion preview: " + error);
      check(encode(regular->model, regular->selection) ==
                encode(*preview->model, preview->selection),
            "Prepared curved extrusion changed exact geometry or selection");
      check(preview->validation && preview->validation->matches(preview->model),
            "Curved extrusion validation must belong to its exact preview model");
      auto reference = tessellate(regular->model, 64, &error, true);
      check(bool(reference), error);
      check(preview->samples->positions.size() == reference->positions.size() &&
                preview->samples->polygons == reference->polygons &&
                preview->samples->vertex_source == reference->vertex_source &&
                preview->samples->face_source == reference->face_source,
            "Prepared curved extrusion changed display geometry or selectable identities");
      for (size_t i = 0; i < reference->positions.size(); ++i)
        check(length(preview->samples->positions[i] - reference->positions[i]) < 1e-10,
              "Prepared curved extrusion sample coordinates must match exact evaluation");
      for (size_t i = 0; i < reference->corner_normals.size(); ++i)
        for (size_t j = 0; j < reference->corner_normals[i].size(); ++j)
          check(length(preview->samples->corner_normals[i][j] -
                       reference->corner_normals[i][j]) < 1e-12,
                "Prepared curved extrusion changed shading normals");
    }
    const Vec3 native_offset{.2, .1, -.1};
    auto native_regular = extrude_elements(rounded->model, selected_patch, native_offset, &error);
    auto native_preview = prepared->preview(native_offset, false, &error);
    auto native_reference = native_regular ?
                                tessellate(native_regular->model, 64, &error, false) :
                                std::nullopt;
    check(bool(native_regular) && bool(native_preview) && bool(native_reference), error);
    check(native_preview->samples->positions == native_reference->positions &&
              native_preview->samples->polygons == native_reference->polygons,
          "Native-normal mode changed prepared curved extrusion geometry");
    for (const auto &normals : native_preview->samples->corner_normals)
      check(normals.empty(), "Native-normal previews must not retain analytic corner normals");
    check(!prepared->preview({0, 0, 0}, true, &error),
          "Prepared curved extrusion accepted a zero offset");
  }
  for (int subdivisions : {1, 2, 3}) {
    Selection selected;
    selected.mode = 1;
    selected.vertices = {cube.vertices.front().id};
    const auto rounded = fillet(cube, selected, .2, subdivisions, &error, true);
    check(bool(rounded), error);
    check(rounded->selection.mode == 1, "Vertex fillet must preserve vertex selection mode");
    check(validate_body(rounded->model, cube.faces.front().id, &error), error);
    int spherical_faces = 0;
    for (const Face &f : rounded->model.faces)
      spherical_faces += surface(rounded->model, f.surface)->kind == SurfaceKind::Sphere;
    const int expected_corner_faces = subdivisions == 1 ?
                                          1 :
                                          3 * (subdivisions / 2) *
                                                  (subdivisions - subdivisions / 2) +
                                              subdivisions % 2;
    check(spherical_faces == expected_corner_faces,
          "Vertex fillet must create three structured quad grids");
    const auto display = tessellate(rounded->model, 32, &error);
    check(bool(display), error);
    std::map<std::pair<int, int>, int> display_edges;
    for (const auto &polygon : display->polygons)
      for (size_t i = 0; i < polygon.size(); ++i)
        display_edges[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])]++;
    for (const auto &[edge_pair, uses] : display_edges)
      check(uses == 2, "Vertex fillet display must remain watertight");
    if (subdivisions == 2)
      check(volume(rounded->model) > 0 && volume(rounded->model) < volume(cube),
            "Vertex fillet must remove the sharp corner without inverting the solid");
  }
  {
    Selection top_face;
    top_face.mode = 4;
    top_face.faces = {cube.faces[1].id};
    auto top_rounded = fillet(cube, top_face, .12, 2, &error);
    check(bool(top_rounded), error);
    std::set<Id> existing_curved_surfaces;
    for (const Surface &s : top_rounded->model.surfaces)
      if (s.kind != SurfaceKind::Plane)
        existing_curved_surfaces.insert(s.id);

    Selection bottom_edge;
    bottom_edge.mode = 2;
    for (const Edge &e : top_rounded->model.edges) {
      const Vec3 a = vertex(top_rounded->model, e.vertices[0])->position;
      const Vec3 b = vertex(top_rounded->model, e.vertices[1])->position;
      if (a.z == -1 && b.z == -1 && a.y == -1 && b.y == -1) {
        bottom_edge.edges = {e.id};
        break;
      }
    }
    check(bottom_edge.edges.size() == 1, "Find bottom edge beside the existing top fillet");
    auto bottom_rounded = fillet(top_rounded->model, bottom_edge, .1, 1, &error);
    check(bool(bottom_rounded), error);
    check(validate_body(bottom_rounded->model, bottom_rounded->selection.active, &error), error);
    for (Id id : existing_curved_surfaces)
      check(surface(bottom_rounded->model, id),
            "A new edge fillet must preserve earlier curved fillet surfaces");
    const auto display = tessellate(bottom_rounded->model, 32, &error);
    check(bool(display), error);
    std::map<std::pair<int, int>, int> display_edges;
    for (const auto &polygon : display->polygons)
      for (size_t i = 0; i < polygon.size(); ++i)
        display_edges[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])]++;
    for (const auto &[edge_pair, uses] : display_edges)
      check(uses == 2, "Repeated solid fillets must keep the displayed body watertight");
  }
  {
    Selection all_vertices;
    all_vertices.mode = 1;
    for (const Vertex &v : cube.vertices)
      all_vertices.vertices.push_back(v.id);
    auto vertex_chamfer = chamfer(cube, all_vertices, .1, true, &error);
    check(bool(vertex_chamfer), error);

    Selection all_edges;
    all_edges.mode = 2;
    for (const Edge &e : vertex_chamfer->model.edges)
      all_edges.edges.push_back(e.id);
    auto edge_chamfer = chamfer(vertex_chamfer->model, all_edges, .1, false, &error);
    check(bool(edge_chamfer), error);

    auto face_area = [](const Model &model, const Face &f) {
      Vec3 sum;
      for (size_t i = 0; i < f.outer.size(); ++i) {
        auto loop_start = [&](const Coedge &use) {
          const Edge &e = *edge(model, use.edge);
          return e.vertices[use.reversed ? 1 : 0];
        };
        const Vec3 a = vertex(model, loop_start(f.outer[i]))->position;
        const Vec3 b = vertex(model, loop_start(f.outer[(i + 1) % f.outer.size()]))->position;
        sum = sum + cross(a, b);
      }
      return length(sum) * .5;
    };
    const Face *largest_face = &edge_chamfer->model.faces.front();
    for (const Face &f : edge_chamfer->model.faces)
      if (surface(edge_chamfer->model, f.surface)->kind == SurfaceKind::Plane &&
          face_area(edge_chamfer->model, f) > face_area(edge_chamfer->model, *largest_face))
        largest_face = &f;
    Selection selected_face;
    selected_face.mode = 4;
    selected_face.faces = {largest_face->id};
    auto face_fillet = fillet(edge_chamfer->model, selected_face, .05, 2, &error);
    check(bool(face_fillet), error);

    Selection all_after_face_fillet;
    all_after_face_fillet.mode = 1;
    for (const Vertex &v : face_fillet->model.vertices)
      all_after_face_fillet.vertices.push_back(v.id);
    std::string warning;
    auto vertex_fillet = fillet(
        face_fillet->model, all_after_face_fillet, .05, 2, &error, true, &warning);
    check(bool(vertex_fillet), error);
    check(validate_body(vertex_fillet->model, face_fillet->model.faces.front().id, &error), error);
    check(!warning.empty(), "Mixed vertex selections must explain which corners were skipped");
  }
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
  for (int subdivisions : {1, 2, 3, 32}) {
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
    check(spheres == 8, "Quad fillets must share one exact sphere support per corner");
    for (const Face &f : rounded->model.faces) {
      const Surface &support = *surface(rounded->model, f.surface);
      if (support.kind != SurfaceKind::Sphere || f.outer.size() != 4)
        continue;
      auto local = [&](Vec3 p) {
        p = p - support.origin;
        const double det = dot(support.u, cross(support.v, support.w));
        return Vec3{dot(p, cross(support.v, support.w)) / det,
                    dot(p, cross(support.w, support.u)) / det,
                    dot(p, cross(support.u, support.v)) / det};
      };
      std::vector<Vec3> points;
      for (const Coedge &use : f.outer) {
        const Edge &e = *edge(rounded->model, use.edge);
        points.push_back(local(vertex(rounded->model, e.vertices[use.reversed ? 1 : 0])->position));
      }
      for (int i = 0; i < 4; ++i) {
        const Vec3 q = points[i];
        Vec3 a = points[(i + 3) % 4], b = points[(i + 1) % 4];
        a = a - q * dot(a, q);
        b = b - q * dot(b, q);
        check(length(cross(a, b)) > 1e-4 * length(a) * length(b),
              "A corner quad must have four genuine corners, without a straight middle vertex");
      }
    }
    Model detached = rounded->model;
    for (auto it = detached.surfaces.rbegin(); it != detached.surfaces.rend(); ++it)
      if (it->kind == SurfaceKind::Sphere) {
        it->origin.x += .01;
        break;
      }
    check(!validate(detached, &error),
          "Cached shared-edge samples must still be checked against each corner support");
    const auto display = tessellate(rounded->model, 32, &error);
    check(bool(display), error);
    const auto native_display = tessellate(rounded->model, 32, &error, false);
    check(bool(native_display), error);
    check(native_display->positions == display->positions &&
              native_display->polygons == display->polygons &&
              native_display->vertex_source == display->vertex_source &&
              native_display->edge_source == display->edge_source &&
              native_display->face_source == display->face_source,
          "Normal method must preserve fillet geometry and selection identities");
    check(!native_display->has_analytic_normals,
          "Native shading must not request analytic normals");
    for (const auto &normals : native_display->corner_normals)
      check(normals.empty(), "Native shading must not generate analytic corner normals");
    std::map<std::pair<int, int>, int> incidence;
    for (const auto &polygon : display->polygons)
      for (size_t i = 0; i < polygon.size(); ++i)
        incidence[std::minmax(polygon[i], polygon[(i + 1) % polygon.size()])]++;
    for (const auto &[edge, count] : incidence)
      check(count == 2, "Fillet display must have a watertight shared boundary");
    if (subdivisions == 1 || subdivisions == 32) {
      Selection cap_selection;
      cap_selection.mode = 4;
      for (const Face &f : rounded->model.faces)
        if (surface(rounded->model, f.surface)->kind == SurfaceKind::Plane) {
          cap_selection.faces = {f.id};
          break;
        }
      auto cap_profile = prepare_profile(rounded->model, cap_selection, &error);
      check(bool(cap_profile), error);
      auto prepared = PreparedExtrusion::create(*cap_profile, &error);
      check(bool(prepared), error);
      for (double distance : {.1, .7}) {
        auto regular = extrude(*cap_profile, distance, &error);
        auto cached = prepared->evaluate(distance, &error);
        check(bool(regular) && bool(cached), error);
        check(encode(regular->model, regular->selection) ==
                  encode(cached->model, cached->selection),
              "Prepared extrusion changed exact geometry or selection");
        for (bool analytic : {true, true, false, false}) {
          auto preview = prepared->preview(distance, analytic, &error);
          auto reference = tessellate(regular->model, 64, &error, analytic);
          check(bool(preview) && bool(reference), error);
          check(preview->validation && preview->validation->matches(preview->model),
                "Extrusion preview validation must belong to its exact immutable model");
          check(validate_body(*preview->model, preview->selection.active, &error), error);
          check(preview->samples->positions == reference->positions &&
                    preview->samples->polygons == reference->polygons &&
                    preview->samples->vertex_source == reference->vertex_source &&
                    preview->samples->face_source == reference->face_source,
                "Extrusion sample cache changed geometry or selectable identities");
          for (size_t i = 0; i < reference->corner_normals.size(); ++i)
            for (size_t j = 0; j < reference->corner_normals[i].size(); ++j)
              check(length(preview->samples->corner_normals[i][j] -
                           reference->corner_normals[i][j]) < 1e-12,
                    "Extrusion sample cache changed shading normals");
        }
      }
      check(!prepared->evaluate(0, &error), "Prepared extrusion accepted zero distance");
      check(!prepared->evaluate(-1, &error), "Prepared extrusion accepted an unsupported inward cut");
    }
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
  auto wire_vertex_fillet = fillet(tip->model, tip_selection, .1, 1, &error, true);
  check(bool(wire_vertex_fillet), error);
  check(wire_vertex_fillet->selection.mode == 1 && wire_vertex_fillet->model.edges.size() == 3 &&
            std::any_of(wire_vertex_fillet->model.curves.begin(),
                        wire_vertex_fillet->model.curves.end(),
                        [](const Curve &c) { return c.kind == CurveKind::Conic; }),
        "A selected wire vertex must make a curved fillet and stay in vertex mode");
  Selection wire_edge_selection;
  wire_edge_selection.mode = 2;
  for (const Edge &e : tip->model.edges)
    if (e.vertices[0] == tip->model.vertices[1].id ||
        e.vertices[1] == tip->model.vertices[1].id)
    {
      wire_edge_selection.edges = {e.id};
      break;
    }
  auto selected_wire_edge_bevel = chamfer(tip->model, wire_edge_selection, .1, true, &error);
  check(bool(selected_wire_edge_bevel), error);
  check(selected_wire_edge_bevel->selection.mode == 2 &&
            !selected_wire_edge_bevel->selection.edges.empty() &&
            selected_wire_edge_bevel->model.faces.empty(),
        "Ctrl+Shift+B on a wire edge must keep edge mode and bevel its corners");
  auto selected_wire_edge_fillet = fillet(tip->model, wire_edge_selection, .1, 3, &error, true);
  check(bool(selected_wire_edge_fillet), error);
  check(selected_wire_edge_fillet->selection.mode == 2 &&
            selected_wire_edge_fillet->selection.edges.size() == 3 &&
            selected_wire_edge_fillet->model.faces.empty(),
        "Segmented Ctrl+Shift+B on a wire edge must keep edge mode and select its fillet");
  double wire_fillet_angle = 0;
  for (Id id : selected_wire_edge_fillet->selection.edges) {
    const Edge &e = *edge(selected_wire_edge_fillet->model, id);
    const Curve &arc = *curve(selected_wire_edge_fillet->model, e.curve);
    check(arc.kind == CurveKind::Conic,
          "A segmented wire fillet must use exact circular arcs, not straight chamfer edges");
    check(std::abs(length(arc.u) - .1) < 1e-10,
          "Wire fillet radius must match the requested width at a right-angle corner");
    wire_fillet_angle += std::abs(e.end - e.start);
  }
  check(std::abs(wire_fillet_angle - std::acos(-1.0) * .5) < 1e-10,
        "Wire fillet subdivisions must share one exact quarter-circle");
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
