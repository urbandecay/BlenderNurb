/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"
#include "fillet_intern.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace blender::solid {
namespace {

using Pair = std::pair<Id, Id>;

Pair ordered_pair(Id a, Id b)
{
  return a < b ? Pair{a, b} : Pair{b, a};
}

Id begin_vertex(const Model &model, const Coedge &use)
{
  const Edge &e = *edge(model, use.edge);
  return e.vertices[use.reversed ? 1 : 0];
}

Id end_vertex(const Model &model, const Coedge &use)
{
  const Edge &e = *edge(model, use.edge);
  return e.vertices[use.reversed ? 0 : 1];
}

struct PatchNode {
  Id id = 0;
  Vec3 q;
};

struct VertexPatch {
  Vec3 origin;
  std::array<Vec3, 3> axes{};
  std::map<std::pair<int, int>, PatchNode> nodes;
  std::map<Pair, Id> patch_edges;
  Id surface = 0;
  int face_orientation = -1;
};

}  // namespace

std::optional<EditResult> fillet_vertices(const Model &model,
                                          Selection selection,
                                          double width,
                                          int subdivisions,
                                          std::string *error,
                                          std::string *warning)
{
  auto reject = [&](const char *message) -> std::optional<EditResult> {
    if (error)
      *error = message;
    return {};
  };
  if (!std::isfinite(width) || width <= 0 || subdivisions < 1 || subdivisions > 32)
    return reject("Fillet needs a positive width and 1 to 32 subdivisions");
  selection = normalize_selection(model, std::move(selection));
  if (selection.vertices.empty())
    return reject("Select vertices on a closed body to fillet");

  std::map<Id, std::vector<Id>> edge_faces;
  std::map<Id, std::vector<Id>> vertex_edges;
  for (const Face &f : model.faces) {
    for (const Coedge &use : f.outer)
      edge_faces[use.edge].push_back(f.id);
    for (const auto &hole : f.holes)
      for (const Coedge &use : hole)
        edge_faces[use.edge].push_back(f.id);
  }
  for (const Edge &e : model.edges) {
    vertex_edges[e.vertices[0]].push_back(e.id);
    vertex_edges[e.vertices[1]].push_back(e.id);
  }

  Id seed_face = 0;
  for (Id v : selection.vertices) {
    for (Id edge_id : vertex_edges[v]) {
      if (!edge_faces[edge_id].empty()) {
        seed_face = edge_faces[edge_id].front();
        break;
      }
    }
    if (seed_face)
      break;
  }
  if (!seed_face)
    return reject("Select vertices on a closed body to fillet");
  if (!validate_body(model, seed_face, error))
    return {};

  std::set<Id> body_faces, body_edges, body_vertices;
  std::vector<Id> pending{seed_face};
  while (!pending.empty()) {
    const Id face_id = pending.back();
    pending.pop_back();
    if (!body_faces.insert(face_id).second)
      continue;
    const Face &f = *face(model, face_id);
    auto visit = [&](const Coedge &use) {
      body_edges.insert(use.edge);
      const Edge &e = *edge(model, use.edge);
      body_vertices.insert(e.vertices.begin(), e.vertices.end());
      for (Id adjacent : edge_faces[use.edge])
        if (!body_faces.contains(adjacent))
          pending.push_back(adjacent);
    };
    for (const Coedge &use : f.outer)
      visit(use);
    for (const auto &hole : f.holes)
      for (const Coedge &use : hole)
        visit(use);
  }

  Vec3 minimum{std::numeric_limits<double>::max(),
               std::numeric_limits<double>::max(),
               std::numeric_limits<double>::max()};
  Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
  for (Id id : body_vertices) {
    const Vec3 p = vertex(model, id)->position;
    for (int axis = 0; axis < 3; ++axis) {
      minimum[axis] = std::min(minimum[axis], p[axis]);
      maximum[axis] = std::max(maximum[axis], p[axis]);
    }
  }
  const double eps = 1e-9 * std::max(1.0, length(maximum - minimum));

  struct Candidate {
    Id vertex = 0;
    std::array<Id, 3> edges{};
    std::map<Id, std::pair<int, int>> face_axes;
    int patch_orientation = -1;
  };
  std::vector<Candidate> candidates;
  size_t skipped = 0;
  for (Id v : selection.vertices) {
    if (!body_vertices.contains(v)) {
      ++skipped;
      continue;
    }
    std::vector<Id> incident_edges;
    std::set<Id> incident_faces;
    for (Id e : vertex_edges[v]) {
      if (!body_edges.contains(e))
        continue;
      incident_edges.push_back(e);
      incident_faces.insert(edge_faces[e].begin(), edge_faces[e].end());
    }
    if (incident_edges.size() != 3 || incident_faces.size() != 3) {
      ++skipped;
      continue;
    }

    std::sort(incident_edges.begin(), incident_edges.end());
    Candidate candidate;
    candidate.vertex = v;
    std::copy(incident_edges.begin(), incident_edges.end(), candidate.edges.begin());
    const Vec3 origin = vertex(model, v)->position;
    std::array<Vec3, 3> directions;
    bool supported = true;
    for (int i = 0; i < 3; ++i) {
      const Edge &e = *edge(model, candidate.edges[i]);
      if (curve(model, e.curve)->kind != CurveKind::Line) {
        supported = false;
        break;
      }
      const Id other = e.vertices[e.vertices[0] == v ? 1 : 0];
      directions[i] = vertex(model, other)->position - origin;
      const double edge_length = length(directions[i]);
      if (!(edge_length > eps)) {
        supported = false;
        break;
      }
      directions[i] = directions[i] / edge_length;
    }
    if (!supported) {
      ++skipped;
      continue;
    }

    double determinant = dot(directions[0], cross(directions[1], directions[2]));
    if (std::abs(determinant) <= 1e-10) {
      ++skipped;
      continue;
    }
    if (determinant < 0) {
      std::swap(candidate.edges[1], candidate.edges[2]);
    }

    int body_orientation = 0;
    for (Id face_id : incident_faces) {
      const Face &f = *face(model, face_id);
      const Surface &s = *surface(model, f.surface);
      if (s.kind != SurfaceKind::Plane || !f.holes.empty()) {
        supported = false;
        break;
      }
      Vec3 normal = cross(s.u, s.v) * double(f.orientation);
      const double normal_length = length(normal);
      if (!(normal_length > eps)) {
        supported = false;
        break;
      }
      normal = normal / normal_length;
      double min_distance = std::numeric_limits<double>::max();
      double max_distance = -std::numeric_limits<double>::max();
      for (Id body_vertex : body_vertices) {
        const double distance = dot(vertex(model, body_vertex)->position - s.origin, normal);
        min_distance = std::min(min_distance, distance);
        max_distance = std::max(max_distance, distance);
      }
      int face_orientation = 0;
      if (max_distance <= eps)
        face_orientation = 1;
      else if (min_distance >= -eps)
        face_orientation = -1;
      else {
        supported = false;
        break;
      }
      if (body_orientation && body_orientation != face_orientation) {
        supported = false;
        break;
      }
      body_orientation = face_orientation;

      std::vector<int> axes;
      for (int axis = 0; axis < 3; ++axis) {
        const std::vector<Id> &uses = edge_faces[candidate.edges[axis]];
        if (std::find(uses.begin(), uses.end(), face_id) != uses.end())
          axes.push_back(axis);
      }
      if (axes.size() != 2) {
        supported = false;
        break;
      }
      candidate.face_axes[face_id] = {axes[0], axes[1]};
    }
    if (!supported || candidate.face_axes.size() != 3) {
      ++skipped;
      continue;
    }
    /* The canonical patch frame points into the solid. Its face orientation must oppose the
     * source body's orientation to keep the sewn boundary consistently oriented. */
    candidate.patch_orientation = -body_orientation;
    candidates.push_back(std::move(candidate));
  }
  if (candidates.empty())
    return reject("Vertex fillet needs convex three-face corners with planar adjoining faces");

  /* Keep neighboring corner patches from meeting on a shared face. The mapped unit-octant
   * patch stays within sqrt(3) * width of its source corner. */
  double usable_width = width;
  const std::set<Id> selected_vertices(selection.vertices.begin(), selection.vertices.end());
  for (const Candidate &candidate : candidates) {
    const Vec3 origin = vertex(model, candidate.vertex)->position;
    for (Id edge_id : candidate.edges) {
      const Edge &e = *edge(model, edge_id);
      const Id other = e.vertices[e.vertices[0] == candidate.vertex ? 1 : 0];
      const double edge_length = length(vertex(model, other)->position - origin);
      usable_width = std::min(usable_width,
                              edge_length * (selected_vertices.contains(other) ? 0.49 : 0.95));
    }
  }
  for (size_t i = 0; i < candidates.size(); ++i) {
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      bool share_face = false;
      for (const auto &[face_id, axes] : candidates[i].face_axes) {
        (void)axes;
        share_face |= candidates[j].face_axes.contains(face_id);
      }
      if (share_face) {
        const double separation = length(vertex(model, candidates[i].vertex)->position -
                                         vertex(model, candidates[j].vertex)->position);
        usable_width = std::min(usable_width, separation * 0.28);
      }
    }
  }
  if (!(usable_width > eps))
    return reject("The selected vertex fillets have no collision-free width");

  EditResult result{model, {}};
  Model &out = result.model;
  std::map<Id, VertexPatch> patches;
  std::map<Pair, Id> replacement_vertices;
  std::set<Id> processed;
  std::set<Id> generated_vertices;
  auto add_point = [&](Vec3 p) {
    const Id id = out.next_id++;
    out.vertices.push_back({id, p});
    generated_vertices.insert(id);
    return id;
  };

  for (const Candidate &candidate : candidates) {
    VertexPatch patch;
    patch.origin = vertex(model, candidate.vertex)->position;
    patch.face_orientation = candidate.patch_orientation;
    for (int axis = 0; axis < 3; ++axis) {
      const Edge &e = *edge(model, candidate.edges[axis]);
      const Id other = e.vertices[e.vertices[0] == candidate.vertex ? 1 : 0];
      const Vec3 direction = vertex(model, other)->position - patch.origin;
      patch.axes[axis] = direction / length(direction) * usable_width;
    }
    const double determinant = dot(patch.axes[0], cross(patch.axes[1], patch.axes[2]));
    if (!(determinant > eps * eps * eps))
      return reject("The selected vertex fillet has a degenerate corner frame");

    Surface support;
    support.id = out.next_id++;
    support.kind = SurfaceKind::Sphere;
    support.origin = patch.origin;
    support.u = patch.axes[0];
    support.v = patch.axes[1];
    support.w = patch.axes[2];
    patch.surface = support.id;
    out.surfaces.push_back(support);

    for (int i = 0; i <= subdivisions; ++i) {
      for (int j = 0; j <= subdivisions - i; ++j) {
        Vec3 q{double(subdivisions - i - j), double(i), double(j)};
        q = q / length(q);
        Id id = 0;
        if (i + j == 0)
          id = add_point(patch.origin + patch.axes[0]);
        else if (j == 0 && i == subdivisions)
          id = add_point(patch.origin + patch.axes[1]);
        else if (i == 0 && j == subdivisions)
          id = add_point(patch.origin + patch.axes[2]);
        else
          id = add_point(patch.origin + patch.axes[0] * q.x + patch.axes[1] * q.y +
                         patch.axes[2] * q.z);
        patch.nodes[{i, j}] = {id, q};
      }
    }
    for (int axis = 0; axis < 3; ++axis) {
      const int i = axis == 1 ? subdivisions : 0;
      const int j = axis == 2 ? subdivisions : 0;
      replacement_vertices[{candidate.edges[axis], candidate.vertex}] =
          patch.nodes.at({i, j}).id;
    }
    patches.emplace(candidate.vertex, std::move(patch));
    processed.insert(candidate.vertex);
  }

  auto boundary_path = [&](VertexPatch &patch, int first_axis, int last_axis) {
    std::vector<PatchNode> path;
    const int low = std::min(first_axis, last_axis), high = std::max(first_axis, last_axis);
    for (int k = 0; k <= subdivisions; ++k) {
      std::array<int, 3> count{};
      count[low] = subdivisions - k;
      count[high] = k;
      path.push_back(patch.nodes.at({count[1], count[2]}));
    }
    if (first_axis != low)
      std::reverse(path.begin(), path.end());
    return path;
  };

  std::map<Pair, std::vector<PatchNode>> face_corner_paths;
  for (const Candidate &candidate : candidates) {
    VertexPatch &patch = patches.at(candidate.vertex);
    std::map<Id, int> edge_axes;
    for (int axis = 0; axis < 3; ++axis)
      edge_axes[candidate.edges[axis]] = axis;
    for (const auto &[face_id, axes] : candidate.face_axes) {
      const Face &f = *face(model, face_id);
      int incoming = -1, outgoing = -1;
      for (size_t i = 0; i < f.outer.size(); ++i) {
        const Coedge &use = f.outer[i];
        if (end_vertex(model, use) == candidate.vertex)
          incoming = int(i);
        if (begin_vertex(model, use) == candidate.vertex)
          outgoing = int(i);
      }
      if (incoming < 0 || outgoing < 0 ||
          (incoming + 1) % int(f.outer.size()) != outgoing)
        return reject("Vertex fillet could not follow an adjoining face boundary");
      const int first_axis = edge_axes.at(f.outer[incoming].edge);
      const int last_axis = edge_axes.at(f.outer[outgoing].edge);
      (void)axes;
      face_corner_paths[{face_id, candidate.vertex}] =
          boundary_path(patch, first_axis, last_axis);
    }
  }

  auto add_patch_edge = [&](VertexPatch &patch, const PatchNode &a, const PatchNode &b) {
    const Pair key = ordered_pair(a.id, b.id);
    if (const auto found = patch.patch_edges.find(key); found != patch.patch_edges.end()) {
      const Edge &e = *edge(out, found->second);
      return Coedge{e.id, e.vertices[0] != a.id};
    }
    const double cosine = std::clamp(dot(a.q, b.q), -1.0, 1.0);
    const double angle = std::acos(cosine);
    const Vec3 perpendicular = b.q - a.q * cosine;
    const double perpendicular_length = length(perpendicular);
    if (!(angle > 0) || !(perpendicular_length > 0))
      return Coedge{};
    const Id curve_id = out.next_id++;
    const Vec3 u = patch.axes[0] * a.q.x + patch.axes[1] * a.q.y + patch.axes[2] * a.q.z;
    const Vec3 v = (patch.axes[0] * perpendicular.x + patch.axes[1] * perpendicular.y +
                    patch.axes[2] * perpendicular.z) /
                   perpendicular_length;
    out.curves.push_back({curve_id, CurveKind::Conic, patch.origin, u, v});
    const Id edge_id = out.next_id++;
    out.edges.push_back({edge_id, curve_id, {a.id, b.id}, 0, angle});
    patch.patch_edges.emplace(key, edge_id);
    return Coedge{edge_id, false};
  };

  /* Trim each original straight edge back to its new patch boundary. */
  for (Edge &e : out.edges) {
    const Id original_first = e.vertices[0], original_second = e.vertices[1];
    if (const auto found = replacement_vertices.find({e.id, original_first});
        found != replacement_vertices.end())
      e.vertices[0] = found->second;
    if (const auto found = replacement_vertices.find({e.id, original_second});
        found != replacement_vertices.end())
      e.vertices[1] = found->second;
    if (e.vertices[0] != original_first || e.vertices[1] != original_second) {
      const Vec3 a = vertex(out, e.vertices[0])->position;
      const Vec3 b = vertex(out, e.vertices[1])->position;
      if (!(length(b - a) > eps))
        return reject("Vertex fillet width consumes an adjoining edge");
      const Id curve_id = out.next_id++;
      out.curves.push_back({curve_id, CurveKind::Line, a, b - a, {}});
      e.curve = curve_id;
      e.start = 0;
      e.end = 1;
    }
  }

  for (Face &f : out.faces) {
    const Face &source = *face(model, f.id);
    std::vector<Coedge> loop;
    loop.reserve(source.outer.size() + 3 * size_t(subdivisions));
    for (const Coedge &use : source.outer) {
      loop.push_back(use);
      const Id ending_vertex = end_vertex(model, use);
      const auto path_it = face_corner_paths.find({source.id, ending_vertex});
      if (path_it == face_corner_paths.end())
        continue;
      VertexPatch &patch = patches.at(ending_vertex);
      const std::vector<PatchNode> &path = path_it->second;
      for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Coedge bevel_edge = add_patch_edge(patch, path[i], path[i + 1]);
        if (!bevel_edge.edge)
          return reject("Vertex fillet generated a degenerate boundary arc");
        loop.push_back(bevel_edge);
      }
    }
    f.outer = std::move(loop);
  }

  for (auto &[vertex_id, patch] : patches) {
    auto node = [&](int i, int j) -> const PatchNode & { return patch.nodes.at({i, j}); };
    auto triangle = [&](const PatchNode &a, const PatchNode &b, const PatchNode &c) {
      const Coedge ab = add_patch_edge(patch, a, b), bc = add_patch_edge(patch, b, c),
                   ca = add_patch_edge(patch, c, a);
      if (!ab.edge || !bc.edge || !ca.edge)
        return false;
      const Id face_id = out.next_id++;
      out.faces.push_back({face_id, patch.surface, {ab, bc, ca}, {}, patch.face_orientation});
      return true;
    };
    for (int i = 0; i < subdivisions; ++i) {
      for (int j = 0; j < subdivisions - i; ++j) {
        if (!triangle(node(i, j), node(i + 1, j), node(i, j + 1)))
          return reject("Vertex fillet generated a degenerate spherical patch");
        if (i + j + 1 < subdivisions &&
            !triangle(node(i + 1, j), node(i + 1, j + 1), node(i, j + 1)))
          return reject("Vertex fillet generated a degenerate spherical patch");
      }
    }
    (void)vertex_id;
  }

  std::erase_if(out.vertices, [&](const Vertex &v) { return processed.contains(v.id); });
  std::set<Id> used_surfaces, used_curves;
  for (const Face &f : out.faces)
    used_surfaces.insert(f.surface);
  std::erase_if(out.surfaces, [&](const Surface &s) { return !used_surfaces.contains(s.id); });
  for (const Edge &e : out.edges)
    used_curves.insert(e.curve);
  for (const Surface &s : out.surfaces) {
    if (s.kind == SurfaceKind::Extrusion || s.kind == SurfaceKind::Ruled)
      used_curves.insert(s.curve);
    if (s.kind == SurfaceKind::Ruled)
      used_curves.insert(s.other_curve);
  }
  std::erase_if(out.curves, [&](const Curve &c) { return !used_curves.contains(c.id); });

  result.selection.mode = 1;
  result.selection.vertices.assign(generated_vertices.begin(), generated_vertices.end());
  for (Id v : selection.vertices)
    if (!processed.contains(v) && vertex(out, v))
      result.selection.vertices.push_back(v);
  if (!result.selection.vertices.empty())
    result.selection.active = result.selection.vertices.back();
  result.selection = normalize_selection(out, std::move(result.selection));
  if (!validate_body(out, seed_face, error))
    return {};
  if (warning && skipped > 0) {
    *warning = "Rounded " + std::to_string(processed.size()) + " of " +
               std::to_string(selection.vertices.size()) +
               " selected vertices; skipped " + std::to_string(skipped) +
               " that are not simple corners between three flat faces";
  }
  return result;
}

}  // namespace blender::solid
