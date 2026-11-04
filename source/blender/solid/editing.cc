/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace blender::solid {
namespace {
using Set = std::unordered_set<Id>;
using Pair = std::pair<Id, Id>;
void fail(std::string *error, const char *message)
{
  if (error)
    *error = message;
}
bool finite(Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
Pair key(Id a, Id b)
{
  return std::minmax(a, b);
}
template<typename T> void erase_ids(std::vector<T> &values, const Set &ids)
{
  std::erase_if(values, [&](const T &item) { return ids.contains(item.id); });
}
std::vector<Id> boundary(const Model &m, const Face &f)
{
  std::vector<Id> result;
  for (const Coedge &use : f.outer) {
    const Edge &e = *edge(m, use.edge);
    result.push_back(e.vertices[use.reversed ? 1 : 0]);
  }
  return result;
}
bool linear_face(const Model &m, const Face &f)
{
  if (!f.holes.empty() || surface(m, f.surface)->kind != SurfaceKind::Plane)
    return false;
  for (const Coedge &use : f.outer)
    if (curve(m, edge(m, use.edge)->curve)->kind != CurveKind::Line)
      return false;
  return true;
}
Vec3 area(const Model &m, const std::vector<Id> &p)
{
  Vec3 a;
  const Vec3 origin = vertex(m, p.front())->position;
  for (size_t i = 1; i + 1 < p.size(); ++i)
    a = a + cross(vertex(m, p[i])->position - origin, vertex(m, p[i + 1])->position - origin);
  return a;
}
double tolerance(const Model &m)
{
  double scale = 1;
  for (const Vertex &v : m.vertices)
    scale = std::max(scale, length(v.position));
  return 1024 * std::numeric_limits<double>::epsilon() * scale;
}
void compact_supports(Model &m)
{
  Set curves, surfaces;
  for (const Edge &e : m.edges)
    curves.insert(e.curve);
  for (const Face &f : m.faces)
    surfaces.insert(f.surface);
  std::erase_if(m.surfaces, [&](const Surface &s) { return !surfaces.contains(s.id); });
  for (const Surface &s : m.surfaces)
    if (s.kind == SurfaceKind::Extrusion)
      curves.insert(s.curve);
  std::erase_if(m.curves, [&](const Curve &c) { return !curves.contains(c.id); });
}
struct Polygon {
  Id id = 0;
  std::vector<Id> points;
};

/** Ear clipping chooses topology only. All resulting support planes use original double
 * precision source coordinates; display samples never enter a modeling operation. */
std::optional<std::vector<std::vector<Id>>> triangles(const Model &m,
                                                      std::vector<Id> points,
                                                      std::string *error)
{
  std::vector<std::vector<Id>> result;
  Vec3 n = area(m, points);
  if (!(length(n) > 0)) {
    fail(error, "The edited face has zero area");
    return {};
  }
  n = n / length(n);
  const double eps = tolerance(m);
  while (points.size() > 3) {
    bool found = false;
    for (size_t i = 0; i < points.size(); ++i) {
      const size_t previous = (i + points.size() - 1) % points.size(),
                   next = (i + 1) % points.size();
      const Vec3 a = vertex(m, points[previous])->position, b = vertex(m, points[i])->position,
                 c = vertex(m, points[next])->position;
      if (dot(cross(b - a, c - b), n) <= eps * std::max(length(b - a), length(c - b)))
        continue;
      bool occupied = false;
      for (size_t j = 0; j < points.size(); ++j) {
        if (j == previous || j == i || j == next)
          continue;
        const Vec3 p = vertex(m, points[j])->position;
        if (dot(cross(b - a, p - a), n) >= -eps * length(b - a) &&
            dot(cross(c - b, p - b), n) >= -eps * length(c - b) &&
            dot(cross(a - c, p - c), n) >= -eps * length(a - c))
          occupied = true;
      }
      if (occupied)
        continue;
      result.push_back({points[previous], points[i], points[next]});
      points.erase(points.begin() + i);
      found = true;
      break;
    }
    if (!found) {
      fail(error, "The edited face crosses itself or cannot be triangulated");
      return {};
    }
  }
  result.push_back(std::move(points));
  return result;
}

bool install_polygons(Model &m,
                      const Set &replaced,
                      const std::vector<Polygon> &polygons,
                      bool drop_degenerate,
                      std::string *error)
{
  Set old_edges, old_vertices;
  for (const Face &f : m.faces)
    if (replaced.contains(f.id)) {
      for (const Coedge &use : f.outer) {
        old_edges.insert(use.edge);
        for (Id id : edge(m, use.edge)->vertices)
          old_vertices.insert(id);
      }
    }
  erase_ids(m.faces, replaced);
  std::map<Pair, Id> edge_map;
  for (const Edge &e : m.edges)
    if (curve(m, e.curve)->kind == CurveKind::Line)
      edge_map.try_emplace(key(e.vertices[0], e.vertices[1]), e.id);
  auto add_face = [&](const std::vector<Id> &points, Id id) {
    const Vec3 normal = area(m, points);
    if (!(length(normal) > 0))
      return false;
    const Vec3 origin = vertex(m, points.front())->position;
    Vec3 u;
    for (Id p : points) {
      u = vertex(m, p)->position - origin;
      if (length(u) > 0)
        break;
    }
    u = u / length(u);
    const Id support = m.next_id++;
    m.surfaces.push_back(
        {support, SurfaceKind::Plane, origin, u, cross(normal / length(normal), u), 0});
    Face face;
    face.id = id ? id : m.next_id++;
    face.surface = support;
    for (size_t i = 0; i < points.size(); ++i) {
      const Id a = points[i], b = points[(i + 1) % points.size()];
      Id eid;
      auto existing = edge_map.find(key(a, b));
      if (existing != edge_map.end())
        eid = existing->second;
      else {
        const Vec3 p = vertex(m, a)->position, q = vertex(m, b)->position;
        const Id cid = m.next_id++;
        eid = m.next_id++;
        m.curves.push_back({cid, CurveKind::Line, p, q - p, {}});
        m.edges.push_back({eid, cid, {a, b}, 0, 1});
        edge_map.emplace(key(a, b), eid);
      }
      face.outer.push_back({eid, edge(m, eid)->vertices[0] != a});
    }
    m.faces.push_back(std::move(face));
    return true;
  };
  const double eps = tolerance(m);
  for (const Polygon &polygon : polygons) {
    std::vector<Id> points;
    for (Id p : polygon.points)
      if (points.empty() || points.back() != p)
        points.push_back(p);
    if (points.size() > 1 && points.front() == points.back())
      points.pop_back();
    if (Set(points.begin(), points.end()).size() != points.size()) {
      fail(error, "The edit would pinch a face into multiple boundary loops");
      return false;
    }
    if (points.size() < 3 || !(length(area(m, points)) > eps * eps)) {
      if (drop_degenerate)
        continue;
      fail(error, "The edit would collapse a face to zero area");
      return false;
    }
    const Vec3 normal = area(m, points) / length(area(m, points));
    const Vec3 origin = vertex(m, points.front())->position;
    bool planar = true;
    for (Id p : points)
      planar &= std::abs(dot(vertex(m, p)->position - origin, normal)) <= eps;
    if (planar) {
      if (!add_face(points, polygon.id))
        return false;
    }
    else {
      auto split = triangles(m, points, error);
      if (!split)
        return false;
      bool first = true;
      for (const auto &triangle : *split) {
        if (!add_face(triangle, first ? polygon.id : 0))
          return false;
        first = false;
      }
    }
  }
  Set used_edges, used_vertices;
  for (const Face &f : m.faces) {
    for (const Coedge &use : f.outer)
      used_edges.insert(use.edge);
    for (const auto &hole : f.holes)
      for (const Coedge &use : hole)
        used_edges.insert(use.edge);
  }
  std::erase_if(m.edges, [&](const Edge &e) {
    return old_edges.contains(e.id) && !used_edges.contains(e.id);
  });
  for (const Edge &e : m.edges)
    for (Id id : e.vertices)
      used_vertices.insert(id);
  std::erase_if(m.vertices, [&](const Vertex &v) {
    return old_vertices.contains(v.id) && !used_vertices.contains(v.id);
  });
  compact_supports(m);
  return true;
}
bool reset_lines(Model &m, std::string *error)
{
  for (Edge &e : m.edges) {
    if (curve(m, e.curve)->kind != CurveKind::Line)
      continue;
    const Vec3 a = vertex(m, e.vertices[0])->position, b = vertex(m, e.vertices[1])->position;
    if (a == b) {
      fail(error, "The edit collapses a straight edge");
      return false;
    }
    const Id id = m.next_id++;
    m.curves.push_back({id, CurveKind::Line, a, b - a, {}});
    e.curve = id;
    e.start = 0;
    e.end = 1;
  }
  return true;
}
}  // namespace

std::optional<EditResult> duplicate_elements(const Model &model,
                                             Selection selection,
                                             std::string *error)
{
  if (!validate(model, error))
    return {};
  selection = normalize_selection(model, std::move(selection));
  if (selection.vertices.empty()) {
    fail(error, "Select vertices, edges or faces to duplicate");
    return {};
  }
  EditResult result{model, {}};
  Model &out = result.model;
  Set vertices(selection.vertices.begin(), selection.vertices.end());
  Set edges(selection.edges.begin(), selection.edges.end());
  Set faces(selection.faces.begin(), selection.faces.end());
  Set curves, surfaces;
  for (const Edge &e : model.edges)
    if (edges.contains(e.id))
      curves.insert(e.curve);
  for (const Face &f : model.faces)
    if (faces.contains(f.id))
      surfaces.insert(f.surface);
  for (const Surface &s : model.surfaces)
    if (surfaces.contains(s.id) && s.kind == SurfaceKind::Extrusion)
      curves.insert(s.curve);
  const size_t count = vertices.size() + edges.size() + faces.size() + curves.size() +
                       surfaces.size();
  if (count > std::numeric_limits<Id>::max() - out.next_id) {
    fail(error, "Duplication source identities exhausted");
    return {};
  }
  std::unordered_map<Id, Id> ids;
  auto allocate = [&](const auto &items, const Set &chosen) {
    for (const auto &item : items)
      if (chosen.contains(item.id))
        ids[item.id] = out.next_id++;
  };
  allocate(model.vertices, vertices);
  allocate(model.curves, curves);
  allocate(model.edges, edges);
  allocate(model.surfaces, surfaces);
  allocate(model.faces, faces);
  for (Vertex v : model.vertices)
    if (vertices.contains(v.id)) {
      v.id = ids.at(v.id);
      out.vertices.push_back(v);
      result.selection.vertices.push_back(v.id);
    }
  for (Curve c : model.curves)
    if (curves.contains(c.id)) {
      c.id = ids.at(c.id);
      out.curves.push_back(c);
    }
  for (Edge e : model.edges)
    if (edges.contains(e.id)) {
      e.id = ids.at(e.id);
      e.curve = ids.at(e.curve);
      for (Id &v : e.vertices)
        v = ids.at(v);
      out.edges.push_back(e);
      result.selection.edges.push_back(e.id);
    }
  for (Surface s : model.surfaces)
    if (surfaces.contains(s.id)) {
      s.id = ids.at(s.id);
      if (s.kind == SurfaceKind::Extrusion)
        s.curve = ids.at(s.curve);
      out.surfaces.push_back(s);
    }
  for (Face f : model.faces)
    if (faces.contains(f.id)) {
      f.id = ids.at(f.id);
      f.surface = ids.at(f.surface);
      for (Coedge &use : f.outer)
        use.edge = ids.at(use.edge);
      for (auto &hole : f.holes)
        for (Coedge &use : hole)
          use.edge = ids.at(use.edge);
      out.faces.push_back(f);
      result.selection.faces.push_back(f.id);
    }
  result.selection.mode = selection.mode;
  result.selection.active = ids.contains(selection.active) ? ids.at(selection.active) :
                                                             result.selection.vertices.back();
  result.selection = normalize_selection(out, std::move(result.selection));
  if (!validate(out, error))
    return {};
  return result;
}

bool refit_linear_faces(Model &m, std::string *error)
{
  Set replaced;
  std::vector<Polygon> polygons;
  for (const Face &f : m.faces)
    if (linear_face(m, f)) {
      auto points = boundary(m, f);
      if (f.orientation < 0)
        std::reverse(points.begin(), points.end());
      replaced.insert(f.id);
      polygons.push_back({f.id, std::move(points)});
    }
  return install_polygons(m, replaced, polygons, false, error);
}

std::optional<EditResult> extrude_elements(const Model &model,
                                           Selection selection,
                                           Vec3 offset,
                                           std::string *error)
{
  if (!validate(model, error))
    return {};
  selection = normalize_selection(model, std::move(selection));
  if (!finite(offset) || !(length(offset) > 0) || selection.vertices.empty()) {
    fail(error, "Select vertices or edges and use a nonzero finite extrusion offset");
    return {};
  }
  EditResult result{model, {}};
  Model &out = result.model;
  result.selection.mode = selection.mode == 2 ? 2 : 1;
  std::unordered_map<Id, Id> vertices, rails, curves, edges;
  for (Id id : selection.vertices) {
    const Vec3 a = vertex(model, id)->position, b = a + offset;
    if (a == b || !finite(b)) {
      fail(error, "Extrusion exceeds source precision or range");
      return {};
    }
    const Id next = out.next_id++;
    vertices[id] = next;
    out.vertices.push_back({next, b});
    const Id support = out.next_id++;
    out.curves.push_back({support, CurveKind::Line, a, offset, {}});
    rails[id] = out.next_id++;
    out.edges.push_back({rails[id], support, {id, next}, 0, 1});
    result.selection.vertices.push_back(next);
    result.selection.active = next;
  }
  for (Id id : selection.edges) {
    const Edge &e = *edge(model, id);
    const Curve &c = *curve(model, e.curve);
    if (c.kind == CurveKind::Line &&
        length(cross(c.u, offset)) <=
            256 * std::numeric_limits<double>::epsilon() * length(c.u) * length(offset))
    {
      fail(error, "Extruding an edge along itself would create a zero-area face");
      return {};
    }
    if (!curves.contains(e.curve)) {
      Curve shifted = c;
      shifted.id = out.next_id++;
      shifted.origin = shifted.origin + offset;
      curves[e.curve] = shifted.id;
      out.curves.push_back(shifted);
    }
    const Id next = out.next_id++;
    edges[id] = next;
    out.edges.push_back({next,
                         curves[e.curve],
                         {vertices[e.vertices[0]], vertices[e.vertices[1]]},
                         e.start,
                         e.end});
    const Id support = out.next_id++;
    if (c.kind == CurveKind::Line)
      out.surfaces.push_back({support,
                              SurfaceKind::Plane,
                              evaluate(c, e.start),
                              evaluate(c, e.end) - evaluate(c, e.start),
                              offset,
                              0});
    else
      out.surfaces.push_back(
          {support, SurfaceKind::Extrusion, {}, offset, {}, c.id, e.start, e.end});
    int sign = 1;
    for (const Face &f : model.faces)
      for (const Coedge &use : f.outer)
        if (use.edge == id)
          sign = -f.orientation * (use.reversed ? -1 : 1);
    out.faces.push_back(
        {out.next_id++,
         support,
         {{id, false}, {rails[e.vertices[1]], false}, {next, true}, {rails[e.vertices[0]], true}},
         {},
         sign});
    result.selection.edges.push_back(next);
    result.selection.active = result.selection.mode == 2 ? next : result.selection.active;
  }
  result.selection = normalize_selection(out, std::move(result.selection));
  if (!validate(out, error))
    return {};
  return result;
}

std::optional<EditResult> merge_vertices(const Model &model,
                                         Selection selection,
                                         MergeMode mode,
                                         Vec3 cursor,
                                         double threshold,
                                         std::string *error)
{
  if (!validate(model, error))
    return {};
  selection = normalize_selection(model, std::move(selection));
  if (selection.vertices.size() < 2 || !finite(cursor) || !std::isfinite(threshold) ||
      threshold < 0)
  {
    fail(error, "Select at least two source vertices to merge");
    return {};
  }
  std::unordered_map<Id, Id> parent;
  for (Id id : selection.vertices)
    parent[id] = id;
  auto root = [&](Id id) {
    while (parent.at(id) != id)
      id = parent.at(id);
    return id;
  };
  auto unite = [&](Id a, Id b) { parent[root(b)] = root(a); };
  if (mode == MergeMode::Distance) {
    for (size_t i = 0; i < selection.vertices.size(); ++i)
      for (size_t j = i + 1; j < selection.vertices.size(); ++j)
        if (length(vertex(model, selection.vertices[i])->position -
                   vertex(model, selection.vertices[j])->position) <= threshold)
          unite(selection.vertices[i], selection.vertices[j]);
  }
  else if (mode == MergeMode::Collapse) {
    for (Id id : selection.edges) {
      const Edge &e = *edge(model, id);
      unite(e.vertices[0], e.vertices[1]);
    }
  }
  else
    for (Id id : selection.vertices)
      unite(selection.vertices.front(), id);
  std::unordered_map<Id, std::vector<Id>> groups;
  for (Id id : selection.vertices)
    groups[root(id)].push_back(id);
  std::unordered_map<Id, Id> remap;
  std::unordered_map<Id, Vec3> positions;
  Set removed;
  for (auto &[id, group] : groups) {
    Id survivor = group.front();
    if (mode == MergeMode::Last)
      survivor = group.back();
    if (mode == MergeMode::Last &&
        std::find(group.begin(), group.end(), selection.active) != group.end())
      survivor = selection.active;
    Vec3 target;
    if (mode == MergeMode::Cursor)
      target = cursor;
    else if (mode == MergeMode::First || mode == MergeMode::Last)
      target = vertex(model, survivor)->position;
    else {
      for (Id v : group)
        target = target + vertex(model, v)->position;
      target = target / group.size();
    }
    positions[survivor] = target;
    for (Id v : group) {
      remap[v] = survivor;
      if (v != survivor)
        removed.insert(v);
    }
  }
  EditResult result{model, {}};
  Model &out = result.model;
  auto mapped = [&](Id id) {
    auto it = remap.find(id);
    return it == remap.end() ? id : it->second;
  };
  Set replaced, dead_edges;
  std::vector<Polygon> polygons;
  for (const Face &f : model.faces) {
    bool touched = false;
    for (const Coedge &use : f.outer)
      for (Id id : edge(model, use.edge)->vertices)
        touched |= remap.contains(id) &&
                   (mapped(id) != id || positions.at(mapped(id)) != vertex(model, id)->position);
    if (!touched)
      continue;
    if (!linear_face(model, f)) {
      bool moved = false;
      for (const Coedge &use : f.outer)
        for (Id id : edge(model, use.edge)->vertices)
          moved |= remap.contains(id) && positions.at(mapped(id)) != vertex(model, id)->position;
      if (moved) {
        fail(error, "Merging a curved face needs a curve/surface intersection rule");
        return {};
      }
      continue;
    }
    auto p = boundary(model, f);
    if (f.orientation < 0)
      std::reverse(p.begin(), p.end());
    for (Id &id : p)
      id = mapped(id);
    replaced.insert(f.id);
    polygons.push_back({f.id, std::move(p)});
  }
  for (Vertex &v : out.vertices)
    if (positions.contains(v.id))
      v.position = positions.at(v.id);
  std::map<Pair, Id> straight_edges;
  std::unordered_map<Id, Id> edge_remap;
  for (Edge &e : out.edges) {
    const bool changed = mapped(e.vertices[0]) != e.vertices[0] ||
                         mapped(e.vertices[1]) != e.vertices[1] ||
                         vertex(out, mapped(e.vertices[0]))->position !=
                             vertex(model, e.vertices[0])->position ||
                         vertex(out, mapped(e.vertices[1]))->position !=
                             vertex(model, e.vertices[1])->position;
    const bool displaced = vertex(out, mapped(e.vertices[0]))->position !=
                               vertex(model, e.vertices[0])->position ||
                           vertex(out, mapped(e.vertices[1]))->position !=
                               vertex(model, e.vertices[1])->position;
    if (changed && displaced && curve(model, e.curve)->kind != CurveKind::Line) {
      fail(error,
           "Merge cannot deform an analytic arc endpoint; select coincident endpoints to weld");
      return {};
    }
    e.vertices = {mapped(e.vertices[0]), mapped(e.vertices[1])};
    if (curve(model, e.curve)->kind == CurveKind::Line) {
      if (e.vertices[0] == e.vertices[1]) {
        dead_edges.insert(e.id);
        continue;
      }
      auto [it, added] = straight_edges.try_emplace(key(e.vertices[0], e.vertices[1]), e.id);
      if (!added) {
        dead_edges.insert(e.id);
        edge_remap[e.id] = it->second;
      }
    }
  }
  // Remap references before pruning obsolete edges from replacement faces.
  for (Face &f : out.faces)
    for (Coedge &use : f.outer)
      if (edge_remap.contains(use.edge)) {
        const Edge &old = *edge(out, use.edge), &next = *edge(out, edge_remap.at(use.edge));
        use.reversed ^= old.vertices[0] != next.vertices[0];
        use.edge = next.id;
      }
  if (!install_polygons(out, replaced, polygons, true, error))
    return {};
  erase_ids(out.edges, dead_edges);
  erase_ids(out.vertices, removed);
  for (const auto &[id, p] : positions)
    if (!vertex(out, id))
      out.vertices.push_back({id, p});
  if (!reset_lines(out, error) || !refit_linear_faces(out, error))
    return {};
  compact_supports(out);
  result.selection.mode = 1;
  for (const auto &[id, p] : positions)
    if (vertex(out, id))
      result.selection.vertices.push_back(id);
  result.selection.active = mapped(selection.active);
  result.selection = normalize_selection(out, std::move(result.selection));
  if (!validate(out, error))
    return {};
  return result;
}

std::optional<EditResult> delete_elements(const Model &model,
                                          Selection selection,
                                          DeleteMode mode,
                                          std::string *error)
{
  if (!validate(model, error))
    return {};
  selection = normalize_selection(model, std::move(selection));
  Set vertices, edges, faces(selection.faces.begin(), selection.faces.end());
  if (mode == DeleteMode::Vertices)
    vertices.insert(selection.vertices.begin(), selection.vertices.end());
  if (mode == DeleteMode::Vertices || mode == DeleteMode::Edges) {
    edges.insert(selection.edges.begin(), selection.edges.end());
    for (const Edge &e : model.edges)
      if (vertices.contains(e.vertices[0]) || vertices.contains(e.vertices[1]))
        edges.insert(e.id);
    for (const Face &f : model.faces) {
      for (const Coedge &use : f.outer)
        if (edges.contains(use.edge))
          faces.insert(f.id);
      for (const auto &hole : f.holes)
        for (const Coedge &use : hole)
          if (edges.contains(use.edge))
            faces.insert(f.id);
    }
  }
  if (mode == DeleteMode::Faces) {
    Set surviving_edges;
    for (const Face &f : model.faces) {
      for (const Coedge &use : f.outer)
        (faces.contains(f.id) ? edges : surviving_edges).insert(use.edge);
      for (const auto &hole : f.holes)
        for (const Coedge &use : hole)
          (faces.contains(f.id) ? edges : surviving_edges).insert(use.edge);
    }
    for (Id id : surviving_edges)
      edges.erase(id);
  }
  if (mode != DeleteMode::OnlyFaces) {
    for (const Edge &e : model.edges)
      if (edges.contains(e.id))
        for (Id id : e.vertices)
          vertices.insert(id);
    for (const Edge &e : model.edges)
      if (!edges.contains(e.id))
        for (Id id : e.vertices)
          vertices.erase(id);
  }
  EditResult result{model, {}};
  erase_ids(result.model.faces, faces);
  erase_ids(result.model.edges, edges);
  erase_ids(result.model.vertices, vertices);
  compact_supports(result.model);
  result.selection.mode = selection.mode;
  if (!validate(result.model, error))
    return {};
  return result;
}

std::optional<EditResult> chamfer(
    const Model &model,
    Selection selection,
    double width,
    bool vertices_only,
    std::string *error,
    BevelCornerStyle corner_style)
{
  if (!validate(model, error))
    return {};
  selection = normalize_selection(model, std::move(selection));
  if (!std::isfinite(width) || width <= 0) {
    fail(error, "Bevel width must be finite and positive");
    return {};
  }
  if (model.faces.empty()) {
    EditResult result{model, {}};
    Model &out = result.model;
    result.selection.mode = 1;
    std::map<std::pair<Id, Id>, Id> trimmed;
    Set corners;
    std::unordered_map<Id, double> requested;
    for (Id id : selection.vertices) {
      std::vector<const Edge *> adjacent;
      for (const Edge &e : model.edges)
        if (e.vertices[0] == id || e.vertices[1] == id)
          adjacent.push_back(&e);
      if (adjacent.size() != 2)
        continue;
      for (const Edge *e : adjacent)
        if (curve(model, e->curve)->kind != CurveKind::Line) {
          fail(error, "Wire bevel currently requires two straight edges at each corner");
          return {};
        }
      const Vec3 p = vertex(model, id)->position;
      const Vec3 a =
          vertex(model, adjacent[0]->vertices[adjacent[0]->vertices[0] == id ? 1 : 0])->position -
          p;
      const Vec3 b =
          vertex(model, adjacent[1]->vertices[adjacent[1]->vertices[0] == id ? 1 : 0])->position -
          p;
      if (length(cross(a, b)) <= tolerance(model) * std::max(length(a), length(b)))
        continue;
      std::vector<Id> endpoints;
      for (const Edge *e : adjacent) {
        const Id other = e->vertices[e->vertices[0] == id ? 1 : 0];
        const Vec3 direction = vertex(model, other)->position - p;
        requested[e->id] += width;
        if (requested[e->id] >= length(direction) - tolerance(model)) {
          fail(error, "Bevel width consumes an adjoining wire edge");
          return {};
        }
        const Id next = out.next_id++;
        out.vertices.push_back({next, p + direction * (width / length(direction))});
        trimmed[{e->id, id}] = next;
        endpoints.push_back(next);
        result.selection.vertices.push_back(next);
      }
      const Vec3 first = vertex(out, endpoints[0])->position,
                 last = vertex(out, endpoints[1])->position;
      auto c = line(first, last, error);
      if (!c || !append_curve(out, *c, 0, 1, endpoints[0], endpoints[1], false, nullptr, error))
        return {};
      corners.insert(id);
    }
    if (corners.empty()) {
      fail(error, "Select wire vertices with two non-collinear straight edges");
      return {};
    }
    for (Edge &e : out.edges)
      for (Id &id : e.vertices) {
        auto it = trimmed.find({e.id, id});
        if (it != trimmed.end())
          id = it->second;
      }
    erase_ids(out.vertices, corners);
    if (!reset_lines(out, error))
      return {};
    compact_supports(out);
    result.selection = normalize_selection(out, std::move(result.selection));
    if (!validate(out, error))
      return {};
    return result;
  }
  std::vector<Polygon> polygons;
  std::unordered_map<Id, Vec3> normals;
  std::unordered_map<Id, std::vector<Id>> uses;
  Set replaced, body_vertices;
  std::unordered_map<Id, std::vector<Id>> all_uses;
  for (const Face &f : model.faces) {
    for (const Coedge &use : f.outer)
      all_uses[use.edge].push_back(f.id);
    for (const auto &hole : f.holes)
      for (const Coedge &use : hole)
        all_uses[use.edge].push_back(f.id);
  }
  Set component;
  std::vector<Id> pending;
  auto add_face = [&](Id id) {
    if (component.insert(id).second)
      pending.push_back(id);
  };
  if (!selection.faces.empty())
    add_face(selection.faces.front());
  else if (!selection.edges.empty() && !all_uses[selection.edges.front()].empty())
    add_face(all_uses[selection.edges.front()].front());
  else if (!selection.vertices.empty()) {
    for (const Edge &e : model.edges) {
      if (e.vertices[0] != selection.vertices.front() &&
          e.vertices[1] != selection.vertices.front())
        continue;
      if (!all_uses[e.id].empty()) {
        add_face(all_uses[e.id].front());
        break;
      }
    }
  }
  if (pending.empty()) {
    fail(error, "Select an edge or vertex on a planar body to bevel");
    return {};
  }
  while (!pending.empty()) {
    const Face &f = *face(model, pending.back());
    pending.pop_back();
    for (const Coedge &use : f.outer)
      for (Id adjacent : all_uses[use.edge])
        add_face(adjacent);
    for (const auto &hole : f.holes)
      for (const Coedge &use : hole)
        for (Id adjacent : all_uses[use.edge])
          add_face(adjacent);
  }
  for (Id id : selection.faces)
    if (!component.contains(id)) {
      fail(error, "Bevel one connected body at a time");
      return {};
    }
  for (const Face &f : model.faces) {
    if (!component.contains(f.id))
      continue;
    if (!linear_face(model, f)) {
      fail(error, "Planar bevel currently requires straight edges and planar faces");
      return {};
    }
    auto p = boundary(model, f);
    if (f.orientation < 0)
      std::reverse(p.begin(), p.end());
    Vec3 n = area(model, p);
    if (!(length(n) > 0)) {
      fail(error, "Cannot bevel a degenerate face");
      return {};
    }
    n = n / length(n);
    normals[f.id] = n;
    replaced.insert(f.id);
    body_vertices.insert(p.begin(), p.end());
    for (const Coedge &use : f.outer)
      uses[use.edge].push_back(f.id);
    polygons.push_back({f.id, std::move(p)});
  }
  for (const auto &[id, faces] : uses)
    if (faces.size() != 2) {
      if (error)
        *error = "Selected body edge " + std::to_string(id) + " has " +
                 std::to_string(faces.size()) +
                 " attached face(s). Merge by Distance welds points but cannot create a "
                 "missing face or remove an overlapping face";
      return {};
    }
  // A global half-space construction is certified only for one convex boundary.
  const double eps = tolerance(model);
  for (const Polygon &p : polygons) {
    const Vec3 n = normals.at(p.id), a = vertex(model, p.points.front())->position;
    for (Id id : body_vertices)
      if (dot(vertex(model, id)->position - a, n) > eps) {
        fail(error,
             "This bevel construction supports convex planar solids; concave trimming is not "
             "implemented");
        return {};
      }
  }
  struct Cut {
    Vec3 normal;
    double distance;
  };
  std::vector<Cut> cuts;
  if (vertices_only) {
    for (Id id : selection.vertices) {
      if (!body_vertices.contains(id))
        continue;
      Vec3 n;
      Set adjacent;
      for (const Edge &e : model.edges)
        if (e.vertices[0] == id || e.vertices[1] == id)
          if (const auto found = uses.find(e.id); found != uses.end())
            for (Id f : found->second)
              adjacent.insert(f);
      for (Id f : adjacent)
        n = n + normals.at(f);
      if (!(length(n) > 0))
        continue;
      n = n / length(n);
      double delta = std::numeric_limits<double>::max();
      const Vec3 origin = vertex(model, id)->position;
      for (const Edge &e : model.edges)
        if (e.vertices[0] == id || e.vertices[1] == id) {
          if (!uses.contains(e.id))
            continue;
          const Vec3 along = vertex(model, e.vertices[e.vertices[0] == id ? 1 : 0])->position -
                             origin;
          delta = std::min(delta, -dot(n, along / length(along)) * width);
        }
      if (!(delta > 0)) {
        fail(error, "The selected vertex has no convex bevel plane");
        return {};
      }
      cuts.push_back({n, dot(n, origin) - delta});
    }
  }
  else
    for (Id id : selection.edges) {
      if (!uses.contains(id))
        continue;
      const auto &faces = uses.at(id);
      const Vec3 a = normals.at(faces[0]), b = normals.at(faces[1]), sum = a + b;
      const double cosine = std::clamp(dot(a, b), -1.0, 1.0);
      if (cosine > 1 - 1e-12)
        continue;
      if (!(length(sum) > 0)) {
        fail(error, "Opposite face normals do not define a bevel");
        return {};
      }
      const Vec3 n = sum / length(sum), p = vertex(model, edge(model, id)->vertices[0])->position;
      cuts.push_back({n, dot(n, p) - width * std::sqrt((1 - cosine) / 2)});
    }
  if (cuts.empty()) {
    fail(error, "Select convex solid edges or vertices to bevel");
    return {};
  }
  std::map<Id, Cut> patch_cuts;
  if (!vertices_only && corner_style == BevelCornerStyle::Patch) {
    const Set selected_edges(selection.edges.begin(), selection.edges.end());
    for (Id vertex_id : body_vertices) {
      int selected_incident_edges = 0;
      std::set<Id> adjacent_faces;
      for (const Edge &e : model.edges) {
        if (!uses.contains(e.id) ||
            (e.vertices[0] != vertex_id && e.vertices[1] != vertex_id))
          continue;
        selected_incident_edges += selected_edges.contains(e.id);
        for (Id face_id : uses.at(e.id))
          adjacent_faces.insert(face_id);
      }
      if (selected_incident_edges < 3)
        continue;
      Vec3 normal;
      for (Id face_id : adjacent_faces)
        normal = normal + normals.at(face_id);
      if (!(length(normal) > eps))
        continue;
      normal = normal / length(normal);
      const Vec3 origin = vertex(model, vertex_id)->position;
      double along_edge = std::numeric_limits<double>::max();
      for (const Edge &e : model.edges) {
        if (!uses.contains(e.id) ||
            (e.vertices[0] != vertex_id && e.vertices[1] != vertex_id))
          continue;
        const Id other = e.vertices[e.vertices[0] == vertex_id ? 1 : 0];
        const Vec3 direction = vertex(model, other)->position - origin;
        along_edge = std::min(along_edge, -dot(normal, direction / length(direction)));
      }
      if (along_edge > eps)
        patch_cuts.emplace(vertex_id,
                           Cut{normal, dot(normal, origin) - 2 * width * along_edge});
    }
  }
  EditResult result{model, {}};
  Model &out = result.model;
  Set bevel_faces;
  auto apply_cut = [&](const Cut &cut) -> bool {
    std::map<Pair, Id> intersections;
    Set cap;
    std::vector<Polygon> clipped;
    bool removed = false;
    for (const Polygon &p : polygons) {
      std::vector<Id> points;
      for (size_t i = 0; i < p.points.size(); ++i) {
        const Id a = p.points[i], b = p.points[(i + 1) % p.points.size()];
        const Vec3 pa = vertex(out, a)->position, pb = vertex(out, b)->position;
        const double da = dot(cut.normal, pa) - cut.distance,
                     db = dot(cut.normal, pb) - cut.distance;
        const bool inside_a = da <= eps, inside_b = db <= eps;
        if (inside_a) {
          points.push_back(a);
          if (std::abs(da) <= eps)
            cap.insert(a);
        }
        else
          removed = true;
        if (inside_a != inside_b) {
          Id id;
          if (std::abs(da) <= eps)
            id = a;
          else if (std::abs(db) <= eps)
            id = b;
          else {
            auto [it, created] = intersections.try_emplace(key(a, b), 0);
            if (created) {
              it->second = out.next_id++;
              out.vertices.push_back({it->second, pa + (pb - pa) * (da / (da - db))});
            }
            id = it->second;
          }
          if (points.empty() || points.back() != id)
            points.push_back(id);
          cap.insert(id);
        }
      }
      if (points.size() > 1 && points.front() == points.back())
        points.pop_back();
      if (points.size() >= 3 && length(area(out, points)) > eps * eps)
        clipped.push_back({p.id, std::move(points)});
    }
    if (!removed)
      return true;
    if (cap.size() < 3 || clipped.empty()) {
      fail(error, "Bevel width consumes the solid");
      return false;
    }
    Vec3 center;
    for (Id id : cap)
      center = center + vertex(out, id)->position;
    center = center / cap.size();
    std::vector<Id> points(cap.begin(), cap.end());
    Vec3 u = vertex(out, points.front())->position - center;
    u = u / length(u);
    const Vec3 v = cross(cut.normal, u);
    std::sort(points.begin(), points.end(), [&](Id a, Id b) {
      const Vec3 x = vertex(out, a)->position - center, y = vertex(out, b)->position - center;
      return std::atan2(dot(x, v), dot(x, u)) < std::atan2(dot(y, v), dot(y, u));
    });
    const Id id = out.next_id++;
    bevel_faces.insert(id);
    clipped.push_back({id, std::move(points)});
    polygons = std::move(clipped);
    return true;
  };
  for (const Cut &cut : cuts)
    if (!apply_cut(cut))
      return {};
  for (const auto &entry : patch_cuts) {
    if (!apply_cut(entry.second))
      return {};
  }
  if (!install_polygons(out, replaced, polygons, false, error))
    return {};
  // Intersection points generated by early cuts can disappear during later corner cuts.
  Set used;
  for (const Edge &e : out.edges)
    for (Id id : e.vertices)
      used.insert(id);
  std::erase_if(out.vertices,
                [&](const Vertex &v) { return v.id >= model.next_id && !used.contains(v.id); });
  result.selection.mode = 4;
  for (const Face &f : out.faces)
    if (bevel_faces.contains(f.id))
      result.selection.faces.push_back(f.id);
  if (!result.selection.faces.empty())
    result.selection.active = result.selection.faces.back();
  result.selection = normalize_selection(out, std::move(result.selection));
  if (result.selection.faces.empty() || !validate_body(out, result.selection.faces.front(), error))
    return {};
  return result;
}
}  // namespace blender::solid
