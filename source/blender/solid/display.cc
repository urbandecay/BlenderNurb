/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

namespace blender::solid {

std::optional<DisplayMesh> tessellate(const Model &model, int segments, std::string *error)
{
  if (!validate(model, error))
    return {};
  segments = std::clamp(segments, 8, 4096);
  DisplayMesh mesh;
  std::unordered_map<Id, int> vertices;
  std::unordered_map<Id, const Curve *> curves;
  std::unordered_map<Id, const Surface *> surfaces;
  std::unordered_map<Id, const Edge *> edges;
  std::unordered_map<Id, std::vector<int>> paths;
  auto point = [&](Vec3 p, Id id) {
    const int index = int(mesh.positions.size());
    mesh.positions.push_back(p);
    mesh.vertex_source.push_back(id);
    return index;
  };
  for (const auto &v : model.vertices)
    vertices[v.id] = point(v.position, v.id);
  for (const auto &c : model.curves)
    curves[c.id] = &c;
  for (const auto &s : model.surfaces)
    surfaces[s.id] = &s;
  for (const auto &e : model.edges) {
    edges[e.id] = &e;
    const auto &c = *curves.at(e.curve);
    const int count = c.kind == CurveKind::Line ?
                          1 :
                          std::max(2,
                                   int(std::ceil(std::abs(e.end - e.start) /
                                                 (2 * std::numbers::pi) * segments)));
    auto &path = paths[e.id];
    path.reserve(count + 1);
    path.push_back(vertices.at(e.vertices[0]));
    for (int i = 1; i < count; i++) {
      path.push_back(point(evaluate(c, e.start + (e.end - e.start) * double(i) / count), 0));
    }
    path.push_back(vertices.at(e.vertices[1]));
    for (int i = 0; i < count; i++) {
      mesh.edges.push_back({path[i], path[i + 1]});
      mesh.edge_source.push_back(e.id);
    }
  }
  auto polygon = [&](std::vector<int> points, std::vector<Vec3> normals, const Face &f) {
    if (f.orientation < 0) {
      std::reverse(points.begin(), points.end());
      std::reverse(normals.begin(), normals.end());
      for (auto &n : normals)
        n = n * -1;
    }
    mesh.polygons.push_back(std::move(points));
    mesh.corner_normals.push_back(std::move(normals));
    mesh.face_source.push_back(f.id);
  };
  for (const auto &f : model.faces) {
    const auto &s = *surfaces.at(f.surface);
    if (s.kind == SurfaceKind::Plane) {
      if (!f.holes.empty()) {
        if (error)
          *error = "Planar hole triangulation is not implemented in the analytic display adapter";
        return {};
      }
      std::vector<int> boundary;
      for (const auto &use : f.outer) {
        const auto &path = paths.at(use.edge);
        for (size_t i = 0; i + 1 < path.size(); i++)
          boundary.push_back(path[use.reversed ? path.size() - 1 - i : i]);
      }
      if (boundary.size() < 3) {
        if (error)
          *error = "A planar face needs a nondegenerate boundary";
        return {};
      }
      const Vec3 normal = cross(s.u, s.v) / length(cross(s.u, s.v));
      polygon(boundary, std::vector<Vec3>(boundary.size(), normal), f);
    }
    else {
      if (f.outer.size() != 4 || !f.holes.empty()) {
        if (error)
          *error = "An extrusion wall needs its four analytic boundary uses";
        return {};
      }
      const auto &bottom = paths.at(f.outer[0].edge);
      const auto &top = paths.at(f.outer[2].edge);
      const auto &e = *edges.at(f.outer[0].edge);
      const auto &c = *curves.at(s.curve);
      if (bottom.size() != top.size()) {
        if (error)
          *error = "Extrusion boundary parameterizations do not agree";
        return {};
      }
      for (size_t i = 0; i + 1 < bottom.size(); i++) {
        std::vector<Vec3> normals;
        for (size_t j : {i, i + 1, i + 1, i}) {
          const double t = e.start + (e.end - e.start) * double(j) / (bottom.size() - 1);
          Vec3 n = cross(tangent(c, t) * (e.end - e.start), s.u);
          const double size = length(n);
          if (!(size > 0)) {
            if (error)
              *error = "Degenerate extrusion wall normal";
            return {};
          }
          normals.push_back(n / size);
        }
        polygon({bottom[i], bottom[i + 1], top[i + 1], top[i]}, std::move(normals), f);
      }
    }
  }
  return mesh;
}

}  // namespace blender::solid
