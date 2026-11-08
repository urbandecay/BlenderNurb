/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"
#include "id_index.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

namespace blender::solid {

static std::optional<DisplayMesh> tessellate_impl(const Model &model,
                                      int segments,
                                      std::string *error,
                                      const bool calculate_normals)
{
  const bool log_breakdown = std::getenv("BLENDER_BEVEL_PROFILE") != nullptr;
  segments = std::clamp(segments, 8, 4096);
  DisplayMesh mesh;
  mesh.has_analytic_normals = calculate_normals;
  std::unordered_map<Id, int> vertices;
  const IdIndex<Curve> curves(model.curves);
  const IdIndex<Surface> surfaces(model.surfaces);
  const IdIndex<Edge> edges(model.edges);
  std::unordered_map<Id, std::vector<int>> paths;
  std::unordered_map<Id, std::vector<Id>> edge_groups;
  std::unordered_map<Id, int> ruled_rails;
  std::unordered_set<Id> small_sphere_edges, large_sphere_edges;
  std::unordered_map<Id, size_t> sphere_support_face_count;
  /* Build the index before inspecting faces: the generic ID helpers scan the model. */
  vertices.reserve(model.vertices.size());
  paths.reserve(model.edges.size());
  edge_groups.reserve(model.edges.size());
  ruled_rails.reserve(model.edges.size());
  small_sphere_edges.reserve(model.edges.size());
  large_sphere_edges.reserve(model.edges.size());
  std::array<size_t, 7> source_faces_by_kind{};
  std::array<size_t, 7> display_faces_by_kind{};
  std::map<std::pair<int, int>, size_t> sphere_patch_histogram;
  for (const Face &f : model.faces) {
    const Surface &face_surface = *surfaces.at(f.surface);
    if (face_surface.kind == SurfaceKind::Sphere)
      sphere_support_face_count[f.surface]++;
    if (log_breakdown) {
      source_faces_by_kind[size_t(face_surface.kind)]++;
    }
    for (const Coedge &use : f.outer)
      edge_groups[use.edge].push_back(f.group ? f.group : f.id);
    if (face_surface.kind == SurfaceKind::Sphere) {
      /* Fillet corner spheres are already split into triangular source patches. A
       * short exact conic edge on those patches is one source subdivision; splitting
       * it again to satisfy the global display target over-tessellates low segment
       * counts. Keep one display span when every adjacent sphere patch is this small. */
      bool small_patch = f.outer.size() == 3 || f.outer.size() == 4;
      for (const Coedge &use : f.outer) {
        const Edge &e = *edges.at(use.edge);
        const Curve &c = *curves.at(e.curve);
        small_patch &= c.kind == CurveKind::Conic &&
                       std::abs(e.end - e.start) <= std::numbers::pi / 16;
      }
      for (const Coedge &use : f.outer)
        (small_patch ? small_sphere_edges : large_sphere_edges).insert(use.edge);
    }
    if (face_surface.kind == SurfaceKind::Ruled && f.outer.size() == 4) {
      const Surface &s = face_surface;
      const Curve &a = *curves.at(s.curve), &b = *curves.at(s.other_curve);
      const Vec3 offset = evaluate(b, s.reverse_second ? 1 : 0) -
                          evaluate(a, s.reverse_first ? 1 : 0);
      const bool translated = a.kind == CurveKind::RationalQuadratic &&
                              b.kind == CurveKind::RationalQuadratic && a.w == b.w &&
                              length(b.u - a.u - offset) < 1e-11 &&
                              length(evaluate(b, s.reverse_second ? 0 : 1) -
                                     evaluate(a, s.reverse_first ? 0 : 1) - offset) < 1e-11;
      const int count = translated ? 1 : 4;
      for (int side : {1, 3})
        ruled_rails[f.outer[side].edge] = std::max(ruled_rails[f.outer[side].edge], count);
    }
  }
  for (Id edge_id : large_sphere_edges)
    small_sphere_edges.erase(edge_id);
  auto point = [&](Vec3 p, Id id) {
    const int index = int(mesh.positions.size());
    mesh.positions.push_back(p);
    mesh.vertex_source.push_back(id);
    return index;
  };
  for (const auto &v : model.vertices)
    vertices[v.id] = point(v.position, v.id);
  for (const auto &e : model.edges) {
    const auto &c = *curves.at(e.curve);
    const int count = c.kind == CurveKind::Cubic ?
                          std::max(2, int(std::ceil(std::abs(e.end - e.start) * segments / 4))) :
                      c.kind == CurveKind::RationalQuadratic ?
                          std::max(1, int(std::ceil(std::abs(e.end - e.start) * segments / 4))) :
                      c.kind == CurveKind::Line ?
                          (ruled_rails.contains(e.id) ? ruled_rails.at(e.id) : 1) :
                      small_sphere_edges.contains(e.id) ?
                          1 :
                          std::max(1,
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
      const auto &groups = edge_groups[e.id];
      mesh.edge_source.push_back(groups.size() == 2 && groups[0] == groups[1] ? 0 : e.id);
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
    if (!calculate_normals)
      normals.clear();
    mesh.corner_normals.push_back(std::move(normals));
    mesh.face_source.push_back(f.group ? f.group : f.id);
    if (log_breakdown) {
      display_faces_by_kind[size_t(surfaces.at(f.surface)->kind)]++;
    }
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
      const Vec3 normal = calculate_normals ? cross(s.u, s.v) / length(cross(s.u, s.v)) :
                                              Vec3{};
      polygon(boundary, std::vector<Vec3>(calculate_normals ? boundary.size() : 0, normal), f);
    }
    else if (s.kind == SurfaceKind::Ruled) {
      auto oriented = [&](const Coedge &use, bool reverse) {
        auto path = paths.at(use.edge);
        if (use.reversed != reverse)
          std::reverse(path.begin(), path.end());
        return path;
      };
      const auto first = oriented(f.outer[0], false), last = oriented(f.outer[2], true),
                 left = oriented(f.outer[3], true), right = oriented(f.outer[1], false);
      if (first.size() != last.size() || left.size() != right.size()) {
        if (error)
          *error = "Ruled fillet boundary samples do not agree";
        return {};
      }
      const Curve &a = *curves.at(s.curve), &b = *curves.at(s.other_curve);
      const int rows = first.size() - 1, columns = left.size() - 1;
      std::vector<std::vector<int>> grid(rows + 1, std::vector<int>(columns + 1));
      for (int i = 0; i <= rows; ++i)
        for (int j = 0; j <= columns; ++j) {
          if (i == 0)
            grid[i][j] = left[j];
          else if (i == rows)
            grid[i][j] = right[j];
          else if (j == 0)
            grid[i][j] = first[i];
          else if (j == columns)
            grid[i][j] = last[i];
          else
            grid[i][j] = point(
                evaluate_ruled(
                    s, a, b, s.start + (s.end - s.start) * double(i) / rows, double(j) / columns),
                0);
        }
      for (int i = 0; i < rows; ++i)
        for (int j = 0; j < columns; ++j) {
          std::vector<int> points;
          std::vector<Vec3> normals;
          for (const auto &[r, c] : {std::pair{i, j},
                                     std::pair{i + 1, j},
                                     std::pair{i + 1, j + 1},
                                     std::pair{i, j + 1}})
          {
            Vec3 n = calculate_normals ?
                         ruled_normal(s, a, b, s.start + (s.end - s.start) * double(r) / rows,
                                      double(c) / columns) : Vec3{0, 0, 1};
            if (!(length(n) > 0)) {
              if (error)
                *error = "Degenerate ruled fillet tangent frame";
              return {};
            }
            points.push_back(grid[r][c]);
            if (calculate_normals)
              normals.push_back(n / length(n));
          }
          polygon(std::move(points), std::move(normals), f);
        }
    }
    else if (s.kind == SurfaceKind::Cone) {
      auto oriented = [&](const Coedge &use) {
        auto path = paths.at(use.edge);
        if (use.reversed)
          std::reverse(path.begin(), path.end());
        return path;
      };
      const auto base_path = oriented(f.outer[0]);
      const auto apex_path = oriented(f.outer[1]);
      if (base_path.size() < 2 || apex_path.size() < 2 ||
          base_path.back() != apex_path.front())
      {
        if (error)
          *error = "Conical fillet boundaries do not meet";
        return {};
      }
      const int apex = apex_path.back();
      const Curve &base = *curves.at(s.curve);
      for (size_t i = 0; i + 1 < base_path.size(); ++i) {
        const double t0 = s.start + (s.end - s.start) * double(i) / (base_path.size() - 1);
        const double t1 = s.start + (s.end - s.start) * double(i + 1) / (base_path.size() - 1);
        const double tm = (t0 + t1) * .5;
        const Vec3 n0 = cone_normal(s, base, t0), n1 = cone_normal(s, base, t1),
                   nm = cone_normal(s, base, tm);
        if (!(length(n0) > 0) || !(length(n1) > 0) || !(length(nm) > 0)) {
          if (error)
            *error = "Degenerate conical fillet normal";
          return {};
        }
        polygon({base_path[i], base_path[i + 1], apex},
                calculate_normals ?
                    std::vector<Vec3>{n0 / length(n0), n1 / length(n1), nm / length(nm)} :
                    std::vector<Vec3>{},
                f);
      }
    }
    else if (s.kind == SurfaceKind::Blend) {
      auto oriented = [&](const Coedge &use, bool reverse) {
        auto path = paths.at(use.edge);
        if (use.reversed != reverse)
          std::reverse(path.begin(), path.end());
        return path;
      };
      const auto side0 = oriented(f.outer.front(), false);
      const auto side1 = oriented(f.outer[f.outer.size() - 2], true);
      const auto top = oriented(f.outer.back(), true);
      const bool collapsed = f.outer.size() == 3;
      const auto bottom = collapsed ? std::vector<int>{} : oriented(f.outer[1], false);
      if (side0.size() != side1.size() || (!collapsed && top.size() != bottom.size())) {
        if (error)
          *error = "Corner blend boundary sampling does not agree";
        return {};
      }
      const int rows = side0.size() - 1, columns = top.size() - 1;
      std::vector<std::vector<int>> grid(rows + 1, std::vector<int>(columns + 1));
      for (int i = 0; i <= rows; ++i)
        for (int j = 0; j <= columns; ++j) {
          if (j == 0)
            grid[i][j] = side0[i];
          else if (j == columns)
            grid[i][j] = side1[i];
          else if (i == 0)
            grid[i][j] = top[j];
          else if (i == rows)
            grid[i][j] = collapsed ? side0.back() : bottom[j];
          else {
            const double theta = s.start + (s.end - s.start) * double(i) / rows,
                         v = s.blend_v_start +
                             (s.blend_v_end - s.blend_v_start) * double(j) / columns;
            grid[i][j] = point(evaluate_blend(s, theta, v), 0);
          }
        }
      for (int i = 0; i < rows; ++i)
        for (int j = 0; j < columns; ++j) {
          std::vector<int> points;
          std::vector<Vec3> normals;
          for (const auto &[r, c] : {std::pair{i, j},
                                     std::pair{i + 1, j},
                                     std::pair{i + 1, j + 1},
                                     std::pair{i, j + 1}})
          {
            if (!points.empty() && points.back() == grid[r][c])
              continue;
            const double theta = s.start + (s.end - s.start) * double(r) / rows,
                         v = s.blend_v_start +
                             (s.blend_v_end - s.blend_v_start) * double(c) / columns;
            Vec3 normal = calculate_normals ?
                              cross(blend_theta_tangent(s, theta, v), blend_v_direction(s, v)) :
                              Vec3{0, 0, 1};
            if (!(length(normal) > 0)) {
              if (error)
                *error = "Degenerate corner blend normal";
              return {};
            }
            points.push_back(grid[r][c]);
            if (calculate_normals)
              normals.push_back(normal / length(normal));
          }
          polygon(std::move(points), std::move(normals), f);
        }
    }
    else if (s.kind == SurfaceKind::Sphere) {
      const double det = dot(s.u, cross(s.v, s.w));
      auto local = [&](Vec3 p) {
        p = p - s.origin;
        return Vec3{dot(p, cross(s.v, s.w)) / det,
                    dot(p, cross(s.w, s.u)) / det,
                    dot(p, cross(s.u, s.v)) / det};
      };
      auto normal = [&](Vec3 q) {
        if (!calculate_normals)
          return Vec3{};
        Vec3 n = (cross(s.v, s.w) * q.x + cross(s.w, s.u) * q.y + cross(s.u, s.v) * q.z) /
                 std::abs(det);
        return n / length(n);
      };
      std::vector<int> boundary;
      for (const Coedge &use : f.outer) {
        const auto &path = paths.at(use.edge);
        for (size_t i = 0; i + 1 < path.size(); ++i)
          boundary.push_back(path[use.reversed ? path.size() - 1 - i : i]);
      }
      Vec3 center;
      for (int id : boundary)
        center = center + local(mesh.positions[id]);
      center = center / length(center);
      /* A fillet may already be split into hundreds of small spherical source
       * patches. Sampling every patch like a complete corner multiplies the
       * display density with each source subdivision. Scale interior sampling
       * by this patch's angular extent; shared boundary samples stay unchanged. */
      double extent = 0;
      for (int id : boundary) {
        Vec3 q = local(mesh.positions[id]);
        q = q / length(q);
        extent = std::max(extent, std::acos(std::clamp(dot(center, q), -1.0, 1.0)));
      }
      const bool shared_sphere_support = sphere_support_face_count[s.id] > 1;
      const int rings = shared_sphere_support ?
                            1 :
                            std::max(1, int(std::ceil(extent * std::max(2, segments / 8))));
      if (log_breakdown) {
        sphere_patch_histogram[{rings, int(boundary.size())}]++;
      }
      /* Small analytic patches already subdivide the corner surface. Keep the
       * sampled boundary as one smooth display polygon instead of adding a center
       * vertex and fanning the patch into several display triangles. */
      if (rings == 1) {
        std::vector<Vec3> normals;
        normals.reserve(boundary.size());
        for (int id : boundary)
          if (calculate_normals)
            normals.push_back(normal(local(mesh.positions[id])));
        polygon(boundary, std::move(normals), f);
        continue;
      }
      const int center_id = point(s.origin + s.u * center.x + s.v * center.y + s.w * center.z, 0);
      std::vector<int> previous(boundary.size(), center_id);
      for (int ring = 1; ring <= rings; ++ring) {
        std::vector<int> current;
        for (int id : boundary) {
          if (ring == rings) {
            current.push_back(id);
            continue;
          }
          Vec3 q = center * (1 - double(ring) / rings) +
                   local(mesh.positions[id]) * (double(ring) / rings);
          q = q / length(q);
          current.push_back(point(s.origin + s.u * q.x + s.v * q.y + s.w * q.z, 0));
        }
        for (size_t i = 0; i < boundary.size(); ++i) {
          const size_t j = (i + 1) % boundary.size();
          std::vector<int> points = ring == 1 ?
                                        std::vector<int>{center_id, current[i], current[j]} :
                                        std::vector<int>{
                                            previous[i], current[i], current[j], previous[j]};
          std::vector<Vec3> normals;
          for (int id : points)
            if (calculate_normals)
              normals.push_back(normal(local(mesh.positions[id])));
          polygon(std::move(points), std::move(normals), f);
        }
        previous = std::move(current);
      }
    }
    else {
      if (f.outer.size() != 4 || !f.holes.empty()) {
        if (error)
          *error = "An extrusion wall needs its four analytic boundary uses";
        return {};
      }
      auto bottom = paths.at(f.outer[0].edge);
      auto top = paths.at(f.outer[2].edge);
      if (s.kind == SurfaceKind::Cylinder) {
        if (f.outer[0].reversed)
          std::reverse(bottom.begin(), bottom.end());
        if (!f.outer[2].reversed)
          std::reverse(top.begin(), top.end());
      }
      const auto &e = *edges.at(f.outer[0].edge);
      const auto &c = *curves.at(s.kind == SurfaceKind::Cylinder ? e.curve : s.curve);
      if (bottom.size() != top.size()) {
        if (error)
          *error = "Extrusion boundary parameterizations do not agree";
        return {};
      }
      for (size_t i = 0; i + 1 < bottom.size(); i++) {
        std::vector<Vec3> normals;
        for (size_t j : {i, i + 1, i + 1, i}) {
          const bool reversed = s.kind == SurfaceKind::Cylinder && f.outer[0].reversed;
          const double fraction = double(j) / (bottom.size() - 1);
          const double t = e.start + (e.end - e.start) * (reversed ? 1 - fraction : fraction);
          Vec3 n = calculate_normals ?
                       cross(tangent(c, t) * ((reversed ? -1 : 1) * (e.end - e.start)),
                             s.kind == SurfaceKind::Cylinder ? s.w : s.u) :
                       Vec3{0, 0, 1};
          const double size = length(n);
          if (!(size > 0)) {
            if (error)
              *error = "Degenerate extrusion wall normal";
            return {};
          }
          if (calculate_normals)
            normals.push_back(n / size);
        }
        polygon({bottom[i], bottom[i + 1], top[i + 1], top[i]}, std::move(normals), f);
      }
    }
  }
  if (log_breakdown) {
    static constexpr const char *kind_names[] = {
        "plane", "extrusion", "sphere", "cylinder", "blend", "ruled"};
    fprintf(stderr, "SOLID_TESSELLATION_BREAKDOWN");
    for (size_t i = 0; i < source_faces_by_kind.size(); ++i) {
      fprintf(stderr,
              " %s_source=%zu %s_display=%zu",
              kind_names[i],
              source_faces_by_kind[i],
              kind_names[i],
              display_faces_by_kind[i]);
    }
    fprintf(stderr, "\n");
    if (!sphere_patch_histogram.empty()) {
      fprintf(stderr, "SOLID_SPHERE_PATCH_HISTOGRAM");
      for (const auto &[key, count] : sphere_patch_histogram) {
        fprintf(stderr,
                " rings_%d_boundary_%d=%zu",
                key.first,
                key.second,
                count);
      }
      fprintf(stderr, "\n");
    }
  }
  return mesh;
}

std::optional<DisplayMesh> tessellate(const Model &model, int segments,
                                      std::string *error, bool calculate_normals)
{
  if (!validate(model, error))
    return {};
  return tessellate_impl(model, segments, error, calculate_normals);
}

std::optional<DisplayMesh> tessellate(const ValidatedModel &model, int segments,
                                      std::string *error, bool calculate_normals)
{
  return tessellate_impl(model.model(), segments, error, calculate_normals);
}

}  // namespace blender::solid
