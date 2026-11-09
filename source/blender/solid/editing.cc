/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"
#include "extrusion.hh"
#include "fillet_intern.hh"
#include "id_index.hh"

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
  std::unordered_map<Id, Id> group_representatives;
  for (const Face &f : m.faces)
    if (f.group) {
      auto [it, added] = group_representatives.try_emplace(f.group, f.id);
      if (!added)
        it->second = std::min(it->second, f.id);
    }
  for (Face &f : m.faces)
    if (f.group)
      f.group = group_representatives.at(f.group);
  Set curves, surfaces;
  for (const Edge &e : m.edges)
    curves.insert(e.curve);
  for (const Face &f : m.faces)
    surfaces.insert(f.surface);
  std::erase_if(m.surfaces, [&](const Surface &s) { return !surfaces.contains(s.id); });
  for (const Surface &s : m.surfaces)
    if (s.kind == SurfaceKind::Extrusion || s.kind == SurfaceKind::Ruled ||
        s.kind == SurfaceKind::Cone)
      curves.insert(s.curve);
  for (const Surface &s : m.surfaces)
    if (s.kind == SurfaceKind::Ruled)
      curves.insert(s.other_curve);
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
                      std::string *error,
                      bool ruled_quads = false)
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
  const IdIndex<Curve> curve_index(m.curves);
  for (const Edge &e : m.edges)
    if (curve_index.at(e.curve)->kind == CurveKind::Line)
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
    else if (ruled_quads && points.size() == 4) {
      if (!add_face(points, polygon.id))
        return false;
      Surface &s = m.surfaces.back();
      s.kind = SurfaceKind::Ruled;
      auto profile = [&](Id a, Id b) {
        const Id id = m.next_id++;
        const Vec3 origin = vertex(m, a)->position;
        m.curves.push_back({id, CurveKind::Line, origin, vertex(m, b)->position - origin, {}});
        return id;
      };
      s.curve = profile(points[0], points[1]);
      s.other_curve = profile(points[3], points[2]);
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
    if (surfaces.contains(s.id) &&
        (s.kind == SurfaceKind::Extrusion || s.kind == SurfaceKind::Ruled ||
         s.kind == SurfaceKind::Cone))
      curves.insert(s.curve);
  for (const Surface &s : model.surfaces)
    if (surfaces.contains(s.id) && s.kind == SurfaceKind::Ruled)
      curves.insert(s.other_curve);
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
      if (s.kind == SurfaceKind::Extrusion || s.kind == SurfaceKind::Ruled ||
          s.kind == SurfaceKind::Cone)
        s.curve = ids.at(s.curve);
      if (s.kind == SurfaceKind::Ruled)
        s.other_curve = ids.at(s.other_curve);
      out.surfaces.push_back(s);
    }
  for (Face f : model.faces)
    if (faces.contains(f.id)) {
      f.id = ids.at(f.id);
      if (f.group)
        f.group = ids.contains(f.group) ? ids.at(f.group) : 0;
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
  const IdIndex<Surface> surfaces(m.surfaces);
  const IdIndex<Edge> edges(m.edges);
  const IdIndex<Curve> curves(m.curves);
  for (const Face &f : m.faces)
    if (f.holes.empty() && surfaces.at(f.surface)->kind == SurfaceKind::Plane &&
        std::all_of(f.outer.begin(), f.outer.end(), [&](const Coedge &use) {
          return curves.at(edges.at(use.edge)->curve)->kind == CurveKind::Line;
        })) {
      auto points = boundary(m, f);
      if (f.orientation < 0)
        std::reverse(points.begin(), points.end());
      replaced.insert(f.id);
      polygons.push_back({f.id, std::move(points)});
    }
  return install_polygons(m, replaced, polygons, false, error);
}

struct FaceRegionExtrusionMaps {
  std::vector<std::pair<Id, Id>> vertices;
  std::vector<std::pair<Id, Id>> curves;
  std::vector<std::pair<Id, Id>> surfaces;
  std::vector<std::pair<Id, Id>> rail_curves;
  std::vector<Id> wall_surfaces;
};

static std::optional<EditResult> extrude_face_region(const Model &model,
                                                     const Selection &selection,
                                                     const Vec3 offset,
                                                     std::string *error,
                                                     FaceRegionExtrusionMaps *maps = nullptr,
                                                     const Affine *cap_transform = nullptr)
{
  auto direction = [&](Vec3 p) {
    return cap_transform ? cap_transform->columns[0] * p.x +
                               cap_transform->columns[1] * p.y +
                               cap_transform->columns[2] * p.z : p;
  };
  auto point = [&](Vec3 p) {
    return direction(p) + (cap_transform ? cap_transform->translation : offset);
  };
  if (maps)
    *maps = {};
  struct FaceUse {
    const Face *face = nullptr;
    Coedge use;
  };
  Set selected_faces(selection.faces.begin(), selection.faces.end());
  Set selected_edges, selected_vertices, selected_curves, selected_surfaces;
  std::unordered_map<Id, std::vector<FaceUse>> edge_uses;
  for (const Face &f : model.faces) {
    auto collect = [&](const std::vector<Coedge> &loop) {
      for (const Coedge &use : loop)
        edge_uses[use.edge].push_back({&f, use});
    };
    collect(f.outer);
    for (const auto &hole : f.holes)
      collect(hole);
    if (!selected_faces.contains(f.id))
      continue;
    selected_surfaces.insert(f.surface);
    auto add_edges = [&](const std::vector<Coedge> &loop) {
      for (const Coedge &use : loop) {
        selected_edges.insert(use.edge);
        const Edge &e = *edge(model, use.edge);
        selected_vertices.insert(e.vertices.begin(), e.vertices.end());
        selected_curves.insert(e.curve);
      }
    };
    add_edges(f.outer);
    for (const auto &hole : f.holes)
      add_edges(hole);
  }
  if (selected_faces.empty()) {
    fail(error, "Select a face region to extrude");
    return {};
  }
  for (const Surface &s : model.surfaces)
    if (selected_surfaces.contains(s.id)) {
      if (s.kind == SurfaceKind::Extrusion || s.kind == SurfaceKind::Ruled ||
          s.kind == SurfaceKind::Cone)
        selected_curves.insert(s.curve);
      if (s.kind == SurfaceKind::Ruled)
        selected_curves.insert(s.other_curve);
    }

  std::unordered_map<Id, FaceUse> boundary_edges;
  Set boundary_vertices;
  for (Id id : selected_edges) {
    const auto &uses = edge_uses.at(id);
    const FaceUse *selected_use = nullptr;
    size_t selected_count = 0;
    for (const FaceUse &use : uses)
      if (selected_faces.contains(use.face->id)) {
        selected_count++;
        selected_use = &use;
      }
    if (selected_count == 0 || (selected_count == uses.size() && uses.size() > 1))
      continue;
    if (!selected_use) {
      fail(error, "The selected face region has an invalid boundary");
      return {};
    }
    boundary_edges.emplace(id, *selected_use);
    const Edge &e = *edge(model, id);
    boundary_vertices.insert(e.vertices.begin(), e.vertices.end());
  }
  const bool keep_original_shell = boundary_edges.empty();

  Set surfaces;
  for (const Surface &s : model.surfaces)
    if (selected_surfaces.contains(s.id))
      surfaces.insert(s.id);
  const uint64_t new_ids = selected_vertices.size() + selected_curves.size() +
                           selected_edges.size() + surfaces.size() + selected_faces.size() +
                           boundary_vertices.size() * 2 + boundary_edges.size() * 2;
  if (new_ids > std::numeric_limits<Id>::max() - model.next_id) {
    fail(error, "Face extrusion source identities exhausted");
    return {};
  }

  EditResult result{model, {}};
  Model &out = result.model;
  std::unordered_map<Id, Id> ids;
  auto allocate = [&](const auto &items, const Set &chosen) {
    for (const auto &item : items)
      if (chosen.contains(item.id))
        ids[item.id] = out.next_id++;
  };
  allocate(model.vertices, selected_vertices);
  allocate(model.curves, selected_curves);
  allocate(model.edges, selected_edges);
  allocate(model.surfaces, surfaces);
  allocate(model.faces, selected_faces);
  if (maps) {
    for (Id id : selected_vertices)
      maps->vertices.emplace_back(id, ids.at(id));
    for (Id id : selected_curves)
      maps->curves.emplace_back(id, ids.at(id));
    for (Id id : surfaces)
      maps->surfaces.emplace_back(id, ids.at(id));
  }
  for (const Vertex &source : model.vertices)
    if (selected_vertices.contains(source.id))
      out.vertices.push_back({ids.at(source.id), point(source.position)});
  for (const Curve &source : model.curves)
    if (selected_curves.contains(source.id)) {
      Curve copy = source;
      copy.id = ids.at(source.id);
      copy.origin = point(source.origin);
      if (copy.kind == CurveKind::RationalQuadratic) {
        copy.u = point(source.u);
        copy.v = point(source.v);
      }
      else {
        copy.u = direction(source.u);
        copy.v = direction(source.v);
        copy.w = direction(source.w);
      }
      out.curves.push_back(copy);
    }
  for (const Edge &source : model.edges)
    if (selected_edges.contains(source.id)) {
      Edge copy = source;
      copy.id = ids.at(source.id);
      copy.curve = ids.at(source.curve);
      copy.vertices = {ids.at(source.vertices[0]), ids.at(source.vertices[1])};
      out.edges.push_back(copy);
    }
  for (const Surface &source : model.surfaces)
    if (surfaces.contains(source.id)) {
      Surface copy = source;
      copy.id = ids.at(source.id);
      copy.origin = point(source.origin);
      copy.u = direction(source.u);
      copy.v = direction(source.v);
      copy.w = direction(source.w);
      for (Vec3 &coefficient : copy.blend)
        coefficient = direction(coefficient);
      if (copy.kind == SurfaceKind::Extrusion || copy.kind == SurfaceKind::Ruled ||
          copy.kind == SurfaceKind::Cone)
        copy.curve = ids.at(copy.curve);
      if (copy.kind == SurfaceKind::Ruled)
        copy.other_curve = ids.at(copy.other_curve);
      out.surfaces.push_back(copy);
    }
  for (const Face &source : model.faces)
    if (selected_faces.contains(source.id)) {
      Face copy = source;
      copy.id = ids.at(source.id);
      copy.surface = ids.at(source.surface);
      if (copy.group)
        copy.group = ids.contains(copy.group) ? ids.at(copy.group) : 0;
      auto remap = [&](std::vector<Coedge> &loop) {
        for (Coedge &use : loop)
          use.edge = ids.at(use.edge);
      };
      remap(copy.outer);
      for (auto &hole : copy.holes)
        remap(hole);
      result.selection.faces.push_back(copy.id);
      out.faces.push_back(std::move(copy));
    }

  std::unordered_map<Id, Id> rails;
  for (Id id : boundary_vertices) {
    const Vec3 from = vertex(model, id)->position;
    const Id curve_id = out.next_id++;
    out.curves.push_back({curve_id, CurveKind::Line, from, cap_transform ? point(from) - from : offset, {}});
    const Id rail = out.next_id++;
    out.edges.push_back({rail, curve_id, {id, ids.at(id)}, 0, 1});
    rails[id] = rail;
    if (maps)
      maps->rail_curves.emplace_back(curve_id, id);
  }
  for (const auto &[id, selected_use] : boundary_edges) {
    const Edge &source = *edge(model, id);
    const Curve &source_curve = *curve(model, source.curve);
    const Vec3 a = evaluate(source_curve, source.start), b = evaluate(source_curve, source.end);
    const Id support = out.next_id++;
    if (cap_transform) {
      Surface wall;
      wall.id = support;
      wall.kind = SurfaceKind::Ruled;
      wall.curve = source.curve;
      wall.other_curve = ids.at(source.curve);
      wall.start = source.start;
      wall.end = source.end;
      out.surfaces.push_back(wall);
    }
    else if (source_curve.kind == CurveKind::Line) {
      if (length(cross(b - a, offset)) <=
          tolerance(model) * std::max(length(b - a), length(offset)))
      {
        fail(error, "Extrusion along a boundary edge would create a zero-area wall");
        return {};
      }
      out.surfaces.push_back({support, SurfaceKind::Plane, a, b - a, offset, 0});
    }
    else {
      out.surfaces.push_back({support,
                              SurfaceKind::Extrusion,
                              {},
                              offset,
                              {},
                              source.curve,
                              source.start,
                              source.end});
    }
    if (maps)
      maps->wall_surfaces.push_back(support);
    const int orientation = selected_use.face->orientation * (selected_use.use.reversed ? -1 : 1);
    out.faces.push_back({out.next_id++,
                         support,
                         {{id, false},
                          {rails.at(source.vertices[1]), false},
                          {ids.at(id), true},
                          {rails.at(source.vertices[0]), true}},
                         {},
                         orientation});
  }

  if (keep_original_shell) {
    for (Face &f : out.faces)
      if (selected_faces.contains(f.id))
        f.orientation *= -1;
  }
  else {
    std::erase_if(out.faces, [&](const Face &f) { return selected_faces.contains(f.id); });
  }
  Set used_edges, used_vertices;
  for (const Face &f : out.faces) {
    for (const Coedge &use : f.outer)
      used_edges.insert(use.edge);
    for (const auto &hole : f.holes)
      for (const Coedge &use : hole)
        used_edges.insert(use.edge);
  }
  std::erase_if(out.edges, [&](const Edge &e) {
    return selected_edges.contains(e.id) && !used_edges.contains(e.id);
  });
  for (const Edge &e : out.edges)
    used_vertices.insert(e.vertices.begin(), e.vertices.end());
  std::erase_if(out.vertices, [&](const Vertex &v) {
    return selected_vertices.contains(v.id) && !used_vertices.contains(v.id);
  });
  result.selection.mode = 4;
  result.selection.active = selected_faces.contains(selection.active) ?
                                ids.at(selection.active) :
                                result.selection.faces.back();
  result.selection = normalize_selection(out, std::move(result.selection));
  compact_supports(out);
  if (!validate(out, error))
    return {};

  bool source_is_closed = true;
  for (Id id : selected_faces) {
    std::string ignored;
    source_is_closed &= validate_body(model, id, &ignored);
  }
  if (source_is_closed)
    for (Id id : result.selection.faces)
      if (!validate_body(out, id, error))
        return {};
  return result;
}

static std::optional<Affine> normal_offset_transform(const Model &model,
                                                     const Face &selected_face,
                                                     double distance,
                                                     std::string *error)
{
  const Surface &support = *surface(model, selected_face.surface);
  if (support.kind == SurfaceKind::Plane) {
    const Vec3 n = cross(support.u, support.v) * double(selected_face.orientation);
    Affine transform;
    transform.translation = n * (distance / length(n));
    return transform;
  }
  if (support.kind != SurfaceKind::Sphere && support.kind != SurfaceKind::Cylinder &&
      support.kind != SurfaceKind::Extrusion && support.kind != SurfaceKind::Ruled) {
    fail(error, "This surface needs a general analytic offset construction");
    return {};
  }
  Vec3 center = support.origin, u = support.u, v = support.v, axis = support.w;
  const bool sphere = support.kind == SurfaceKind::Sphere;
  double sign = selected_face.orientation;
  if (sphere) {
    sign *= dot(u, cross(v, axis)) < 0 ? -1 : 1;
  }
  else {
    if (support.kind == SurfaceKind::Ruled) {
      const Curve &a = *curve(model, support.curve);
      const Curve &b = *curve(model, support.other_curve);
      const Vec3 p = evaluate(a, 0), q = evaluate(a, .5), r = evaluate(a, 1);
      const Vec3 ab = q - p, ac = r - p, n = cross(ab, ac);
      if (!(dot(n, n) > 1e-24)) {
        fail(error, "This ruled surface needs a general analytic offset surface");
        return {};
      }
      center = p + (cross(ac, n) * dot(ab, ab) + cross(n, ab) * dot(ac, ac)) /
                       (2 * dot(n, n));
      u = p - center;
      axis = evaluate(b, support.reverse_second ? 1 : 0) -
             evaluate(a, support.reverse_first ? 1 : 0);
      v = cross(n / length(n), u);
      for (double t : {0.0, .25, .5, .75, 1.0}) {
        const Vec3 pa = evaluate(a, support.reverse_first ? 1 - t : t);
        const Vec3 pb = evaluate(b, support.reverse_second ? 1 - t : t);
        if (length(pb - pa - axis) > 1e-9 * std::max(1.0, length(axis)) ||
            std::abs(length(pa - center) - length(u)) > 1e-9 * length(u))
        {
          fail(error, "This ruled surface is not a circular cylindrical strip");
          return {};
        }
      }
    }
    if (support.kind == SurfaceKind::Extrusion) {
      const Curve &profile = *curve(model, support.curve);
      if (profile.kind != CurveKind::Conic) {
        fail(error, "This swept surface needs a general analytic offset surface");
        return {};
      }
      center = profile.origin;
      u = profile.u;
      v = profile.v;
      axis = support.u;
    }
    const Coedge &use = selected_face.outer.front();
    const Edge &e = *edge(model, use.edge);
    const Curve &c = *curve(model, e.curve);
    const Vec3 p = evaluate(c, (e.start + e.end) * .5) - center;
    const Vec3 n = cross(tangent(c, (e.start + e.end) * .5) *
                             ((e.end - e.start) * (use.reversed ? -1 : 1)), axis);
    sign *= dot(n, p) < 0 ? -1 : 1;
  }
  const double radius = length(u), height = length(axis);
  if (!(radius > 0) || !(height > 0) ||
      std::abs(length(v) - radius) > 1e-9 * radius ||
      (sphere && std::abs(height - radius) > 1e-9 * radius) ||
      std::abs(dot(u, v)) > 1e-9 * radius * radius ||
      std::abs(dot(u, axis)) > 1e-9 * radius * height ||
      std::abs(dot(v, axis)) > 1e-9 * radius * height)
  {
    fail(error, "A noncircular affine surface needs a general analytic offset surface");
    return {};
  }
  const double scale = (radius + sign * distance) / radius;
  if (!(scale > 1e-10)) {
    fail(error, "Normal extrusion would collapse or invert the curved surface");
    return {};
  }
  axis = axis / height;
  Affine transform;
  for (int i = 0; i < 3; ++i) {
    Vec3 basis{};
    basis[i] = 1;
    transform.columns[i] = sphere ? basis * scale :
        basis * scale + axis * (axis[i] * (1 - scale));
  }
  transform.translation = center - (transform.columns[0] * center.x +
                                    transform.columns[1] * center.y +
                                    transform.columns[2] * center.z);
  return transform;
}

std::optional<EditResult> extrude_region_normals(const Model &model,
                                                Selection selection,
                                                double distance,
                                                bool even_offset,
                                                std::string *error)
{
  if (!validate(model, error) || !std::isfinite(distance) || distance == 0)
    return {};
  selection = normalize_selection(model, std::move(selection));
  if (selection.mode != 4 || selection.faces.empty()) {
    fail(error, "Select a face region to extrude along normals");
    return {};
  }
  /* A planar region translates rigidly, including exact arcs and circles. */
  Vec3 plane_normal{};
  bool parallel_planes = true;
  for (Id id : selection.faces) {
    const Face &f = *face(model, id);
    const Surface &s = *surface(model, f.surface);
    if (s.kind != SurfaceKind::Plane) {
      parallel_planes = false;
      break;
    }
    Vec3 n = cross(s.u, s.v) * double(f.orientation);
    n = n / length(n);
    if (length(plane_normal) == 0)
      plane_normal = n;
    else
      parallel_planes &= dot(n, plane_normal) > 1 - 1e-10;
  }
  if (parallel_planes)
    return extrude_elements(model, selection, plane_normal * distance, error);
  bool curved_region = false;
  for (Id id : selection.faces)
    curved_region |= surface(model, face(model, id)->surface)->kind != SurfaceKind::Plane;
  if (curved_region) {
    auto direction = [](const Affine &a, Vec3 p) {
      return a.columns[0] * p.x + a.columns[1] * p.y + a.columns[2] * p.z;
    };
    auto point = [&](const Affine &a, Vec3 p) { return direction(a, p) + a.translation; };
    std::unordered_map<Id, Affine> vertices, curves, surfaces;
    Affine seed;
    bool first = true;
    const double eps = std::max(1e-9, tolerance(model) * 16);
    for (Id id : selection.faces) {
      const Face &f = *face(model, id);
      const Surface &support = *surface(model, f.surface);
      auto transform = normal_offset_transform(model, f, distance, error);
      if (!transform)
        return {};
      if (first) {
        seed = *transform;
        first = false;
      }
      surfaces[f.surface] = *transform;
      auto collect = [&](const std::vector<Coedge> &loop) {
        for (const Coedge &use : loop) {
          const Edge &e = *edge(model, use.edge);
          const Curve &c = *curve(model, e.curve);
          for (Id v : e.vertices) {
            const Vec3 p = vertex(model, v)->position;
            if (vertices.contains(v) &&
                length(point(vertices.at(v), p) - point(*transform, p)) > eps)
              return false;
            vertices.try_emplace(v, *transform);
          }
          if (curves.contains(e.curve))
            for (double fraction : {0.0, .25, .5, .75, 1.0}) {
              const Vec3 p = evaluate(c, e.start + (e.end - e.start) * fraction);
              if (length(point(curves.at(e.curve), p) - point(*transform, p)) > eps)
                return false;
            }
          curves.try_emplace(e.curve, *transform);
        }
        return true;
      };
      if (!collect(f.outer)) {
        fail(error, "These offset faces need a new intersection at their shared boundary");
        return {};
      }
      for (const auto &hole : f.holes)
        if (!collect(hole)) {
          fail(error, "Offset hole boundaries need a new surface intersection");
          return {};
        }
      if (support.kind == SurfaceKind::Extrusion || support.kind == SurfaceKind::Ruled ||
          support.kind == SurfaceKind::Cone)
        curves.try_emplace(support.curve, *transform);
      if (support.kind == SurfaceKind::Ruled)
        curves.try_emplace(support.other_curve, *transform);
    }
    FaceRegionExtrusionMaps maps;
    auto result = extrude_face_region(model, selection, {}, error, &maps, &seed);
    if (!result)
      return {};
    Model &out = result->model;
    for (const auto &[original, copy] : maps.vertices) {
      const Vec3 p = vertex(model, original)->position;
      for (Vertex &v : out.vertices)
        if (v.id == copy) {
          v.position = point(vertices.at(original), p);
          break;
        }
    }
    for (const auto &[original, copy] : maps.curves) {
      const Curve &source = *curve(model, original);
      const Affine &a = curves.at(original);
      for (Curve &c : out.curves)
        if (c.id == copy) {
          c.origin = point(a, source.origin);
          if (source.kind == CurveKind::RationalQuadratic) {
            c.u = point(a, source.u);
            c.v = point(a, source.v);
          }
          else {
            c.u = direction(a, source.u);
            c.v = direction(a, source.v);
            c.w = direction(a, source.w);
          }
          break;
        }
    }
    for (const auto &[original, copy] : maps.surfaces) {
      const Surface &source = *surface(model, original);
      const Affine &a = surfaces.at(original);
      for (Surface &s : out.surfaces)
        if (s.id == copy) {
          s.origin = point(a, source.origin);
          s.u = direction(a, source.u);
          s.v = direction(a, source.v);
          s.w = direction(a, source.w);
          for (size_t i = 0; i < s.blend.size(); ++i)
            s.blend[i] = direction(a, source.blend[i]);
          break;
        }
    }
    for (const auto &[rail, source] : maps.rail_curves) {
      const Vec3 p = vertex(model, source)->position;
      for (Curve &c : out.curves)
        if (c.id == rail) {
          c.u = point(vertices.at(source), p) - p;
          break;
        }
    }
    if (!validate(out, error))
      return {};
    return result;
  }
  struct Contribution { Vec3 normal; double angle; };
  struct BoundaryUse { Id a, b; int count = 0; };
  std::unordered_map<Id, std::vector<Contribution>> contributions;
  std::unordered_map<Id, BoundaryUse> uses;
  const Set selected(selection.faces.begin(), selection.faces.end());
  std::vector<Polygon> caps;
  bool delete_original = false;
  for (const Face &f : model.faces) {
    if (!selected.contains(f.id))
      continue;
    if (!linear_face(model, f)) {
      fail(error, "Connected normal extrusion needs straight analytic boundaries for this region");
      return {};
    }
    std::vector<Id> points = boundary(model, f);
    if (f.orientation < 0)
      std::reverse(points.begin(), points.end());
    const Vec3 n = area(model, points);
    const Vec3 normal = n / length(n);
    for (size_t i = 0; i < points.size(); ++i) {
      const Id id = points[i], next = points[(i + 1) % points.size()];
      const Vec3 center = vertex(model, id)->position;
      const Vec3 a = vertex(model, points[(i + points.size() - 1) % points.size()])->position - center;
      const Vec3 b = vertex(model, next)->position - center;
      const double angle = std::acos(std::clamp(dot(a, b) / (length(a) * length(b)), -1.0, 1.0));
      contributions[id].push_back({normal, angle});
    }
    for (const Coedge &use : f.outer) {
      const Edge &e = *edge(model, use.edge);
      auto &entry = uses[use.edge];
      entry.a = e.vertices[(use.reversed != (f.orientation < 0)) ? 1 : 0];
      entry.b = e.vertices[(use.reversed != (f.orientation < 0)) ? 0 : 1];
      ++entry.count;
    }
    caps.push_back({f.id, std::move(points)});
  }
  for (const Face &f : model.faces)
    if (!selected.contains(f.id))
      for (const Coedge &use : f.outer)
        delete_original |= uses.contains(use.edge);

  EditResult result{model, {}};
  Model &out = result.model;
  std::unordered_map<Id, Id> copies;
  for (const auto &[id, entries] : contributions) {
    Vec3 normal{};
    for (const Contribution &entry : entries)
      normal = normal + entry.normal * entry.angle;
    if (!(length(normal) > 1e-12)) {
      fail(error, "Selected face normals cancel at a shared vertex");
      return {};
    }
    normal = normal / length(normal);
    double shell = 0, angle = 0;
    for (const Contribution &entry : entries) {
      const double cosine = std::abs(dot(normal, entry.normal));
      shell += entry.angle * (cosine < 1e-8 ? 1 : 1 / cosine);
      angle += entry.angle;
    }
    const Id copy = out.next_id++;
    copies[id] = copy;
    out.vertices.push_back({copy, vertex(model, id)->position +
                                      normal * (distance * (even_offset ? shell / angle : 1))});
  }
  std::vector<Polygon> polygons;
  result.selection.mode = 4;
  for (const Polygon &cap : caps) {
    Polygon moved{out.next_id++, {}};
    for (Id id : cap.points)
      moved.points.push_back(copies.at(id));
    result.selection.faces.push_back(moved.id);
    if (cap.id == selection.active)
      result.selection.active = moved.id;
    polygons.push_back(std::move(moved));
  }
  for (const auto &[id, use] : uses)
    if (use.count == 1)
      polygons.push_back({out.next_id++, {use.a, use.b, copies.at(use.b), copies.at(use.a)}});
  if (!delete_original)
    for (Face &f : out.faces)
      if (selected.contains(f.id))
        f.orientation *= -1;
  if (!install_polygons(out, delete_original ? selected : Set{}, polygons, false, error, true))
    return {};
  if (!result.selection.active)
    result.selection.active = result.selection.faces.back();
  result.selection = normalize_selection(out, std::move(result.selection));
  if (!validate(out, error))
    return {};
  return result;
}

std::optional<EditResult> extrude_individual_normals(const Model &model,
                                                    Selection selection,
                                                    double distance,
                                                    std::string *error)
{
  selection = normalize_selection(model, std::move(selection));
  if (selection.mode != 4 || selection.faces.empty()) {
    fail(error, "Select faces to extrude individually");
    return {};
  }
  EditResult result{model, {}};
  result.selection.mode = 4;
  for (Id id : selection.faces) {
    Selection single;
    single.mode = 4;
    single.faces = {id};
    single.active = id;
    auto next = extrude_region_normals(result.model, single, distance, false, error);
    if (!next)
      return {};
    result.model = std::move(next->model);
    result.selection.faces.insert(result.selection.faces.end(),
                                  next->selection.faces.begin(), next->selection.faces.end());
    if (id == selection.active || !result.selection.active)
      result.selection.active = next->selection.active;
  }
  result.selection = normalize_selection(result.model, std::move(result.selection));
  return result;
}

std::optional<EditResult> extrude_faces_individually(const Model &model,
                                                     Selection selection,
                                                     const std::vector<Vec3> &offsets,
                                                     std::string *error)
{
  if (!validate(model, error))
    return {};
  selection = normalize_selection(model, std::move(selection));
  if (selection.mode != 4 || selection.faces.empty() || offsets.size() != selection.faces.size()) {
    fail(error, "Select faces with one extrusion offset for each face");
    return {};
  }

  Model result_model = model;
  Selection result_selection;
  result_selection.mode = 4;
  for (size_t i = 0; i < selection.faces.size(); ++i) {
    const Id source_face = selection.faces[i];
    if (!face(result_model, source_face) || !finite(offsets[i]) || !(length(offsets[i]) > 0)) {
      fail(error, "Each selected face needs a finite nonzero extrusion offset");
      return {};
    }
    Selection single_face;
    single_face.mode = 4;
    single_face.faces = {source_face};
    single_face.active = source_face;
    auto extruded = extrude_face_region(result_model, single_face, offsets[i], error);
    if (!extruded)
      return {};
    result_model = std::move(extruded->model);
    result_selection.faces.insert(result_selection.faces.end(),
                                  extruded->selection.faces.begin(),
                                  extruded->selection.faces.end());
    result_selection.active = extruded->selection.active;
  }
  result_selection = normalize_selection(result_model, std::move(result_selection));
  if (!validate(result_model, error))
    return {};
  return EditResult{std::move(result_model), std::move(result_selection)};
}

struct PreparedFaceRegionExtrusion::Impl {
  std::shared_ptr<const Model> source;
  Selection source_selection;
  std::shared_ptr<const Model> template_model;
  Selection result_selection;
  FaceRegionExtrusionMaps maps;
  std::vector<bool> translated_samples;
  std::shared_ptr<const DisplayMesh> display_cache;
  Vec3 display_offset;
  bool display_analytic = true;
};

static bool update_face_region_wall_normals(const Model &model,
                                            const std::vector<Id> &wall_surfaces,
                                            DisplayMesh &display,
                                            std::string *error)
{
  if (!display.has_analytic_normals)
    return true;
  const Set walls(wall_surfaces.begin(), wall_surfaces.end());
  std::unordered_map<Id, const Face *> faces;
  std::unordered_map<Id, size_t> polygon_counts, polygon_steps;
  for (const Face &face : model.faces)
    if (walls.contains(face.surface))
      faces[face.id] = &face;
  for (Id source : display.face_source)
    if (faces.contains(source))
      ++polygon_counts[source];

  const IdIndex<Surface> surfaces(model.surfaces);
  const IdIndex<Edge> edges(model.edges);
  const IdIndex<Curve> curves(model.curves);
  for (size_t i = 0; i < display.polygons.size(); ++i) {
    const auto found = faces.find(display.face_source[i]);
    if (found == faces.end())
      continue;
    const Face &face = *found->second;
    const Surface &surface = *surfaces.at(face.surface);
    auto &normals = display.corner_normals[i];
    if (normals.size() != display.polygons[i].size()) {
      fail(error, "Curved extrusion display normals do not match the face boundary");
      return false;
    }
    if (surface.kind == SurfaceKind::Plane) {
      Vec3 normal = cross(surface.u, surface.v);
      const double size = length(normal);
      if (!(size > 0) || !std::isfinite(size)) {
        fail(error, "Degenerate planar extrusion wall normal");
        return false;
      }
      normal = normal / size;
      if (face.orientation < 0)
        normal = normal * -1;
      std::fill(normals.begin(), normals.end(), normal);
      continue;
    }
    if (surface.kind != SurfaceKind::Extrusion || face.outer.size() != 4 ||
        display.polygons[i].size() != 4)
    {
      fail(error, "Unsupported curved extrusion wall display");
      return false;
    }
    const Edge &edge = *edges.at(face.outer[0].edge);
    const Curve &curve = *curves.at(surface.curve);
    const size_t step = polygon_steps[face.id]++;
    const size_t segments = polygon_counts.at(face.id);
    if (segments == 0) {
      fail(error, "Curved extrusion wall has no display segments");
      return false;
    }
    auto normal_at = [&](const size_t sample) -> std::optional<Vec3> {
      const double fraction = double(sample) / segments;
      const double parameter = edge.start + (edge.end - edge.start) * fraction;
      Vec3 normal = cross(tangent(curve, parameter) * (edge.end - edge.start), surface.u);
      double size = length(normal);
      if (size <= 1e-12 * length(tangent(curve, parameter)) *
                      std::abs(edge.end - edge.start) * length(surface.u) &&
          (sample == 0 || sample == segments))
      {
        const double inside = sample == 0 ? 1e-6 : 1 - 1e-6;
        const double limit_parameter = edge.start + (edge.end - edge.start) * inside;
        normal = cross(tangent(curve, limit_parameter) * (edge.end - edge.start), surface.u);
        size = length(normal);
      }
      if (!(size > 0) || !std::isfinite(size))
        return {};
      return normal / size;
    };
    auto first = normal_at(step), second = normal_at(step + 1);
    if (!first || !second) {
      fail(error, "Degenerate extrusion wall normal");
      return false;
    }
    normals = {*first, *second, *second, *first};
    if (face.orientation < 0) {
      std::reverse(normals.begin(), normals.end());
      for (Vec3 &normal : normals)
        normal = normal * -1;
    }
  }
  return true;
}

std::optional<DisplayMesh> translated_cap_samples(const Model &source,
                                                  const Selection &selection,
                                                  const Model &result,
                                                  const DisplayMesh &samples,
                                                  Vec3 translation,
                                                  std::string *error)
{
  const auto walls = extrusion_cap_wall_surfaces(source, selection);
  if (!walls) {
    fail(error, "Select an extrusion cap to translate its wall samples");
    return {};
  }
  const Set wall_supports(walls->begin(), walls->end());
  const Set selected(selection.faces.begin(), selection.faces.end());
  Set cap_sources;
  std::vector<Id> updated_walls;
  for (const Face &f : source.faces) {
    if (selected.contains(f.id))
      cap_sources.insert(f.group ? f.group : f.id);
    if (wall_supports.contains(f.surface)) {
      const Face *updated = face(result, f.id);
      if (!updated) {
        fail(error, "Cap translation changed wall topology");
        return {};
      }
      updated_walls.push_back(updated->surface);
    }
  }
  DisplayMesh display = samples;
  std::vector<bool> moving(display.positions.size(), false);
  for (size_t i = 0; i < display.polygons.size(); ++i)
    if (cap_sources.contains(display.face_source[i]))
      for (int vertex : display.polygons[i])
        moving[size_t(vertex)] = true;
  for (size_t i = 0; i < moving.size(); ++i)
    if (moving[i])
      display.positions[i] = display.positions[i] + translation;
  if (!update_face_region_wall_normals(result, updated_walls, display, error))
    return {};
  return display;
}

PreparedFaceRegionExtrusion::PreparedFaceRegionExtrusion(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

PreparedFaceRegionExtrusion::PreparedFaceRegionExtrusion(PreparedFaceRegionExtrusion &&) noexcept =
    default;
PreparedFaceRegionExtrusion &PreparedFaceRegionExtrusion::operator=(
    PreparedFaceRegionExtrusion &&) noexcept = default;
PreparedFaceRegionExtrusion::~PreparedFaceRegionExtrusion() = default;

std::optional<PreparedFaceRegionExtrusion> PreparedFaceRegionExtrusion::create(
    std::shared_ptr<const Model> model, Selection selection, std::string *error)
{
  if (!model || !validate(*model, error))
    return {};
  selection = normalize_selection(*model, std::move(selection));
  if (selection.mode != 4 || selection.faces.empty()) {
    fail(error, "Select a face region to extrude");
    return {};
  }
  auto impl = std::make_unique<Impl>();
  impl->source = std::move(model);
  impl->source_selection = std::move(selection);
  return PreparedFaceRegionExtrusion(std::move(impl));
}

std::optional<PreparedFaceRegionExtrusion::Preview> PreparedFaceRegionExtrusion::preview(
    Vec3 offset, bool analytic, std::string *error)
{
  if (!impl_ || !finite(offset) || !(length(offset) > 0)) {
    fail(error, "Use a nonzero finite extrusion offset");
    return {};
  }

  if (!impl_->template_model) {
    auto result = extrude_face_region(
        *impl_->source, impl_->source_selection, offset, error, &impl_->maps);
    if (!result)
      return {};
    impl_->template_model = std::make_shared<const Model>(std::move(result->model));
    impl_->result_selection = std::move(result->selection);

    std::unordered_set<Id> cap_sources;
    for (Id id : impl_->result_selection.faces) {
      const Face *cap = face(*impl_->template_model, id);
      if (cap)
        cap_sources.insert(cap->group ? cap->group : cap->id);
    }

    auto validation = ValidatedModel::from_validated_extrusion(impl_->template_model);
    auto samples = tessellate(validation, 64, error, analytic);
    if (!samples)
      return {};
    impl_->translated_samples.assign(samples->positions.size(), false);
    for (size_t i = 0; i < samples->polygons.size(); ++i)
      if (cap_sources.contains(samples->face_source[i]))
        for (int vertex : samples->polygons[i])
          impl_->translated_samples[size_t(vertex)] = true;
    impl_->display_offset = offset;
    impl_->display_analytic = analytic;
    impl_->display_cache = std::make_shared<const DisplayMesh>(std::move(*samples));
    return Preview{impl_->template_model,
                   impl_->result_selection,
                   std::move(validation),
                   impl_->display_cache};
  }

  Model model = *impl_->template_model;
  std::unordered_map<Id, Vertex *> vertices;
  std::unordered_map<Id, Curve *> curves;
  std::unordered_map<Id, Surface *> surfaces;
  vertices.reserve(model.vertices.size());
  curves.reserve(model.curves.size());
  surfaces.reserve(model.surfaces.size());
  for (Vertex &item : model.vertices)
    vertices[item.id] = &item;
  for (Curve &item : model.curves)
    curves[item.id] = &item;
  for (Surface &item : model.surfaces)
    surfaces[item.id] = &item;

  const IdIndex<Vertex> source_vertices(impl_->source->vertices);
  for (const auto &[source, copy] : impl_->maps.vertices)
    vertices.at(copy)->position = source_vertices.at(source)->position + offset;
  for (const auto &[source, copy] : impl_->maps.curves) {
    Curve &curve = *curves.at(copy);
    const Curve &original = *solid::curve(*impl_->source, source);
    curve.origin = original.origin + offset;
    if (curve.kind == CurveKind::RationalQuadratic) {
      curve.u = original.u + offset;
      curve.v = original.v + offset;
    }
  }
  for (const auto &[source, copy] : impl_->maps.surfaces)
    surfaces.at(copy)->origin = surface(*impl_->source, source)->origin + offset;
  for (const auto &[rail_curve, source_vertex] : impl_->maps.rail_curves) {
    Curve &curve = *curves.at(rail_curve);
    curve.origin = source_vertices.at(source_vertex)->position;
    curve.u = offset;
  }
  for (Id id : impl_->maps.wall_surfaces) {
    Surface &surface = *surfaces.at(id);
    if (surface.kind == SurfaceKind::Plane)
      surface.v = offset;
    else if (surface.kind == SurfaceKind::Extrusion)
      surface.u = offset;
    else {
      fail(error, "Unsupported face-region extrusion wall");
      return {};
    }
  }
  if (!validate(model, error))
    return {};
  auto model_ptr = std::make_shared<const Model>(std::move(model));
  auto validation = ValidatedModel::from_validated_extrusion(model_ptr);
  std::shared_ptr<const DisplayMesh> samples;
  if (impl_->display_cache && impl_->display_analytic == analytic) {
    DisplayMesh updated = *impl_->display_cache;
    const Vec3 sample_delta = offset - impl_->display_offset;
    for (size_t i = 0; i < impl_->translated_samples.size(); ++i)
      if (impl_->translated_samples[i])
        updated.positions[i] = updated.positions[i] + sample_delta;
    if (!update_face_region_wall_normals(*model_ptr, impl_->maps.wall_surfaces, updated, error))
      return {};
    samples = std::make_shared<const DisplayMesh>(std::move(updated));
  }
  else {
    auto generated = tessellate(validation, 64, error, analytic);
    if (!generated)
      return {};
    impl_->display_offset = offset;
    impl_->display_analytic = analytic;
    impl_->translated_samples.assign(generated->positions.size(), false);
    std::unordered_set<Id> cap_sources;
    for (Id id : impl_->result_selection.faces) {
      const Face *cap = face(*model_ptr, id);
      if (cap)
        cap_sources.insert(cap->group ? cap->group : cap->id);
    }
    for (size_t i = 0; i < generated->polygons.size(); ++i)
      if (cap_sources.contains(generated->face_source[i]))
        for (int vertex : generated->polygons[i])
          impl_->translated_samples[size_t(vertex)] = true;
    impl_->display_cache = std::make_shared<const DisplayMesh>(*generated);
    samples = impl_->display_cache;
  }
  return Preview{std::move(model_ptr), impl_->result_selection, std::move(validation), samples};
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
    fail(error, "Select faces, vertices, or edges and use a nonzero finite extrusion offset");
    return {};
  }
  if (selection.mode == 4 && !selection.faces.empty())
    return extrude_face_region(model, selection, offset, error);
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
      if (shifted.kind == CurveKind::RationalQuadratic) {
        shifted.u = shifted.u + offset;
        shifted.v = shifted.v + offset;
      }
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

std::optional<EditResult> chamfer(const Model &model,
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
  {
    bool handled = false;
    auto circular_cap = bevel_circular_cap(model, selection, width, 1, error, handled);
    if (handled)
      return circular_cap;
  }
  if (model.faces.empty()) {
    EditResult result{model, {}};
    Model &out = result.model;
    result.selection.mode = selection.mode;
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
    if (result.selection.mode == 2) {
      const Set selected_vertices(result.selection.vertices.begin(),
                                  result.selection.vertices.end());
      for (const Edge &e : out.edges)
        if (selected_vertices.contains(e.vertices[0]) && selected_vertices.contains(e.vertices[1]))
          result.selection.edges.push_back(e.id);
      if (!result.selection.edges.empty())
        result.selection.active = result.selection.edges.back();
    }
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
        if (!uses.contains(e.id) || (e.vertices[0] != vertex_id && e.vertices[1] != vertex_id))
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
        if (!uses.contains(e.id) || (e.vertices[0] != vertex_id && e.vertices[1] != vertex_id))
          continue;
        const Id other = e.vertices[e.vertices[0] == vertex_id ? 1 : 0];
        const Vec3 direction = vertex(model, other)->position - origin;
        along_edge = std::min(along_edge, -dot(normal, direction / length(direction)));
      }
      if (along_edge > eps)
        patch_cuts.emplace(vertex_id, Cut{normal, dot(normal, origin) - 2 * width * along_edge});
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
