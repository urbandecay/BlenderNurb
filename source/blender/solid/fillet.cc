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

std::optional<EditResult> fillet(
    const Model &model,
    Selection selection,
    double width,
    int subdivisions,
    std::string *error,
    bool vertices_only,
    std::string *warning)
{
  auto reject = [&](const char *message) -> std::optional<EditResult> {
    if (error)
      *error = message;
    return {};
  };
  if (!validate(model, error))
    return {};
  if (!std::isfinite(width) || width <= 0 || subdivisions < 1 || subdivisions > 32)
    return reject("Fillet needs a positive width and 1 to 32 subdivisions");
  selection = normalize_selection(model, std::move(selection));
  if (vertices_only || selection.mode == 1)
    return fillet_vertices(model, std::move(selection), width, subdivisions, error, warning);
  if (selection.edges.empty())
    return reject("Select the edges of a closed convex planar body to fillet");
  std::map<Id, std::vector<Id>> uses;
  for (const Face &f : model.faces)
    for (const Coedge &use : f.outer)
      uses[use.edge].push_back(f.id);
  std::set<Id> body_faces, body_edges, body_vertices;
  std::vector<Id> pending;
  for (Id id : uses[selection.edges.front()]) {
    body_faces.insert(id);
    pending.push_back(id);
  }
  while (!pending.empty()) {
    const Face &f = *face(model, pending.back());
    pending.pop_back();
    for (const Coedge &use : f.outer) {
      const Edge &e = *edge(model, use.edge);
      body_edges.insert(e.id);
      body_vertices.insert(e.vertices.begin(), e.vertices.end());
      for (Id adjacent : uses[e.id])
        if (body_faces.insert(adjacent).second)
          pending.push_back(adjacent);
    }
  }
  const std::set<Id> selected(selection.edges.begin(), selection.edges.end());
  if (body_faces.empty() ||
      !std::includes(body_edges.begin(), body_edges.end(), selected.begin(), selected.end()))
    return reject("Fillet one connected body at a time");
  if (!validate_body(model, *body_faces.begin(), error))
    return {};
  double scale = 1;
  for (const Vertex &v : model.vertices)
    scale = std::max(scale, length(v.position));
  const double eps = 1e-9 * scale;
  std::map<Id, Vec3> normals;
  std::map<Id, std::vector<Id>> boundaries, vertex_faces;
  for (Id id : body_faces) {
    const Face &f = *face(model, id);
    if (surface(model, f.surface)->kind != SurfaceKind::Plane || !f.holes.empty())
      return reject("Fillet currently needs planar faces without holes");
    auto &points = boundaries[id];
    for (const Coedge &use : f.outer) {
      const Edge &e = *edge(model, use.edge);
      if (curve(model, e.curve)->kind != CurveKind::Line)
        return reject("Fillet currently needs straight source edges");
      points.push_back(e.vertices[use.reversed ? 1 : 0]);
    }
    if (f.orientation < 0)
      std::reverse(points.begin(), points.end());
    Vec3 n;
    const Vec3 origin = vertex(model, points.front())->position;
    for (size_t i = 1; i + 1 < points.size(); ++i)
      n = n + cross(vertex(model, points[i])->position - origin,
                    vertex(model, points[i + 1])->position - origin);
    if (length(n) < eps * eps)
      return reject("Fillet cannot use a degenerate face");
    normals[id] = n / length(n);
    for (Id v : body_vertices)
      if (dot(vertex(model, v)->position - origin, normals[id]) > eps)
        return reject("Fillet currently supports convex planar bodies");
    for (Id v : points)
      vertex_faces[v].push_back(id);
  }
  return fillet_trimmed_edges(
      model, selection, normals, uses, vertex_faces, width, subdivisions, eps, error);
}
}  // namespace blender::solid
