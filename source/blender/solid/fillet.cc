/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"
#include "fillet_intern.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace blender::solid {
namespace {

/** Fillet a planar circular end of an analytic extrusion. The ordinary edge fillet works from
 * three planar faces at every selected-edge endpoint; a circular cylinder end has one curved
 * extrusion face instead. Build the same quarter-round profile as a stack of analytic ruled
 * conic strips, and shorten the extrusion wall to meet the first strip. */
std::optional<EditResult> fillet_circular_cap(const Model &model,
                                             const Selection &selection,
                                             const double width,
                                             const int subdivisions,
                                             std::string *error,
                                             bool &handled)
{
  handled = false;
  /* normalize_selection also identifies a fully selected cap in vertex and edge modes.
   * Vertices that split a closed circle are seams, rather than sharp body corners. */
  if (selection.faces.size() != 1)
    return {};

  const Face &cap = *face(model, selection.faces.front());
  if (surface(model, cap.surface)->kind != SurfaceKind::Plane || !cap.holes.empty() ||
      cap.outer.empty())
  {
    return {};
  }
  if (selection.edges.size() != cap.outer.size())
    return {};
  const Edge &first_edge = *edge(model, cap.outer.front().edge);
  const Curve &ring = *curve(model, first_edge.curve);
  if (ring.kind != CurveKind::Conic)
    return {};
  for (const Coedge &use : cap.outer) {
    const Edge &e = *edge(model, use.edge);
    if (e.curve != ring.id || curve(model, e.curve)->kind != CurveKind::Conic)
      return {};
  }

  const double radius_u = length(ring.u), radius_v = length(ring.v);
  const Surface &cap_plane = *surface(model, cap.surface);
  const double scale = std::max({1.0, radius_u, radius_v, length(cap_plane.u), length(cap_plane.v)});
  const double eps = 1e-9 * scale;
  const double angle_eps = 1e-8;
  const double two_pi = 2 * std::numbers::pi;
  double coverage = 0;
  for (const Coedge &use : cap.outer) {
    const Edge &e = *edge(model, use.edge);
    const double span = std::abs(e.end - e.start);
    if (span > two_pi + angle_eps)
      return {};
    coverage += span;
  }
  if (std::abs(radius_u - radius_v) > eps ||
      std::abs(dot(ring.u, ring.v)) > eps * std::max(radius_u, radius_v) ||
      std::abs(coverage - two_pi) > angle_eps)
  {
    return {};
  }
  auto reject = [&](const char *message) -> std::optional<EditResult> {
    if (error)
      *error = message;
    return {};
  };

  const Vec3 plane_normal = cross(cap_plane.u, cap_plane.v);
  if (!(length(plane_normal) > eps))
    return reject("The circular end face has a degenerate plane");
  const Vec3 cap_normal = plane_normal / length(plane_normal) * double(cap.orientation);
  const Vec3 circle_normal = cross(ring.u, ring.v) / length(cross(ring.u, ring.v));
  if (std::abs(dot(cap_normal, circle_normal)) < 1 - 1e-7 ||
      std::abs(dot(ring.origin - cap_plane.origin, cap_normal)) > eps)
  {
    return {};
  }
  const Vec3 inward = cap_normal * -1;

  struct EdgeUse {
    Id face = 0;
  };
  std::unordered_map<Id, std::vector<EdgeUse>> edge_uses;
  for (const Face &f : model.faces) {
    for (const Coedge &use : f.outer)
      edge_uses[use.edge].push_back({f.id});
    for (const auto &hole : f.holes)
      for (const Coedge &use : hole)
        edge_uses[use.edge].push_back({f.id});
  }

  struct AdjacentWall {
    Id face = 0;
    Coedge ring_use;
  };
  std::map<Id, AdjacentWall> walls;
  std::unordered_set<Id> ring_vertices;
  bool top_end = false;
  bool have_end = false;
  Vec3 sweep{};
  Id base_curve_id = 0;
  for (const Coedge &cap_use : cap.outer) {
    const Edge &e = *edge(model, cap_use.edge);
    ring_vertices.insert(e.vertices.begin(), e.vertices.end());
    const auto found = edge_uses.find(e.id);
    if (found == edge_uses.end() || found->second.size() != 2)
      return {};
    const EdgeUse *other = nullptr;
    for (const EdgeUse &use : found->second)
      if (use.face != cap.id)
        other = &use;
    if (!other)
      return {};

    const Face &wall = *face(model, other->face);
    const Surface &wall_surface = *surface(model, wall.surface);
    if (wall_surface.kind != SurfaceKind::Extrusion || !wall.holes.empty() ||
        wall.outer.size() != 4)
    {
      return {};
    }
    size_t slot = 4;
    for (size_t i = 0; i < wall.outer.size(); ++i)
      if (wall.outer[i].edge == e.id) {
        if (slot != 4)
          return {};
        slot = i;
      }
    if (slot != 0 && slot != 2)
      return {};
    const bool this_top = slot == 2;
    if (have_end && this_top != top_end)
      return {};
    have_end = true;
    top_end = this_top;

    const Curve *base_curve = curve(model, wall_surface.curve);
    if (!base_curve || base_curve->kind != CurveKind::Conic)
      return {};
    if (!base_curve_id) {
      base_curve_id = base_curve->id;
      sweep = wall_surface.u;
    }
    else if (base_curve_id != base_curve->id || length(wall_surface.u - sweep) > eps) {
      return {};
    }

    const Vec3 expected_origin = base_curve->origin + (this_top ? wall_surface.u : Vec3{});
    if (length(ring.origin - expected_origin) > eps ||
        length(ring.u - base_curve->u) > eps || length(ring.v - base_curve->v) > eps)
    {
      return {};
    }
    walls.emplace(e.id, AdjacentWall{wall.id, wall.outer[slot]});
  }
  if (walls.size() != cap.outer.size() || !(length(sweep) > eps) ||
      (top_end ? dot(sweep / length(sweep), cap_normal) :
                 -dot(sweep / length(sweep), cap_normal)) < 1 - 1e-7)
  {
    return {};
  }
  for (Id id : selection.vertices)
    if (!ring_vertices.contains(id))
      return {};
  handled = true;
  std::string body_error;
  if (!validate_body(model, cap.id, &body_error)) {
    if (error)
      *error = body_error;
    return {};
  }
  const double radius = (radius_u + radius_v) * .5;
  const double height = length(sweep);
  const bool apex_limit = radius + eps < height;
  const bool collapse_to_apex = apex_limit && width >= radius - eps;
  if (!apex_limit && width >= height - eps)
    return reject("Fillet width must be smaller than the cylinder height");
  if (!collapse_to_apex && width >= radius - eps)
    return reject("Fillet width must be smaller than the cylinder radius");
  const double fillet_width = collapse_to_apex ? radius : width;

  std::unordered_set<Id> rail_edges;
  for (const auto &item : walls) {
    const AdjacentWall &adjacent = item.second;
    const Face &wall = *face(model, adjacent.face);
    rail_edges.insert(wall.outer[1].edge);
    rail_edges.insert(wall.outer[3].edge);
  }
  std::unordered_map<Id, std::vector<Id>> incident_edges;
  for (const Edge &e : model.edges) {
    incident_edges[e.vertices[0]].push_back(e.id);
    incident_edges[e.vertices[1]].push_back(e.id);
  }
  for (Id v : ring_vertices)
    for (Id e : incident_edges[v])
      if (!rail_edges.contains(e) &&
          std::none_of(cap.outer.begin(), cap.outer.end(), [&](const Coedge &use) {
            return use.edge == e;
          }))
      {
        return reject("The cylinder end shares vertices with geometry outside its wall");
      }
  for (Id id : rail_edges)
    if (curve(model, edge(model, id)->curve)->kind != CurveKind::Line)
      return reject("The cylinder wall has a non-linear boundary rail");

  EditResult result{model, {}};
  Model &out = result.model;
  const int steps = subdivisions;
  std::vector<Id> station_curves(steps + 1);
  const Curve source_ring = ring;
  for (int i = 0; i <= steps; ++i) {
    if (collapse_to_apex && i == steps)
      continue;
    const double angle = (std::numbers::pi * .5) * double(i) / steps;
    const double station_radius = radius_u - fillet_width + fillet_width * std::cos(angle);
    const double inset = fillet_width * (1 - std::sin(angle));
    Curve station = source_ring;
    station.origin = source_ring.origin + inward * inset;
    station.u = source_ring.u * (station_radius / radius_u);
    station.v = source_ring.v * (station_radius / radius_u);
    if (i == 0) {
      station.id = source_ring.id;
      station_curves[i] = station.id;
      *std::find_if(out.curves.begin(), out.curves.end(), [&](const Curve &c) {
        return c.id == station.id;
      }) = station;
    }
    else {
      station.id = out.next_id++;
      station_curves[i] = station.id;
      out.curves.push_back(station);
    }
  }

  std::vector<std::map<Id, Id>> station_vertices(steps + 1);
  std::vector<std::map<Id, Id>> station_edges(steps + 1);
  Id apex_vertex = 0;
  for (Id v : ring_vertices) {
    const Vec3 radial = vertex(model, v)->position - source_ring.origin;
    for (int i = 0; i <= steps; ++i) {
      if (collapse_to_apex && i == steps) {
        if (!apex_vertex) {
          apex_vertex = out.next_id++;
          out.vertices.push_back({apex_vertex, source_ring.origin});
        }
        station_vertices[i][v] = apex_vertex;
        continue;
      }
      const Curve &station = *curve(out, station_curves[i]);
      const double factor = length(station.u) / radius_u;
      const double inset = dot(station.origin - source_ring.origin, inward);
      const Vec3 position = source_ring.origin + radial * factor + inward * inset;
      if (i == 0) {
        station_vertices[i][v] = v;
        std::find_if(out.vertices.begin(), out.vertices.end(), [&](const Vertex &item) {
          return item.id == v;
        })->position = position;
      }
      else {
        const Id id = out.next_id++;
        out.vertices.push_back({id, position});
        station_vertices[i][v] = id;
      }
    }
  }

  for (const Coedge &use : cap.outer) {
    const Edge &source = *edge(model, use.edge);
    station_edges[0][source.id] = source.id;
    Edge *base = &*std::find_if(out.edges.begin(), out.edges.end(), [&](const Edge &item) {
      return item.id == source.id;
    });
    base->curve = station_curves[0];
    for (int i = 1; i <= steps; ++i) {
      if (collapse_to_apex && i == steps)
        break;
      const Id id = out.next_id++;
      out.edges.push_back({id,
                           station_curves[i],
                           {station_vertices[i].at(source.vertices[0]),
                            station_vertices[i].at(source.vertices[1])},
                           source.start,
                           source.end});
      station_edges[i][source.id] = id;
    }
  }
  if (collapse_to_apex) {
    std::erase_if(out.faces, [&](const Face &f) { return f.id == cap.id; });
  }
  else {
    Face *out_cap = &*std::find_if(out.faces.begin(), out.faces.end(), [&](const Face &f) {
      return f.id == cap.id;
    });
    for (Coedge &use : out_cap->outer)
      use.edge = station_edges[steps].at(use.edge);
  }

  for (const auto &item : walls) {
    const AdjacentWall &adjacent = item.second;
    Face *wall = &*std::find_if(out.faces.begin(), out.faces.end(), [&](const Face &f) {
      return f.id == adjacent.face;
    });
    Surface *wall_surface = &*std::find_if(out.surfaces.begin(), out.surfaces.end(),
                                           [&](const Surface &s) {
                                             return s.id == wall->surface;
                                           });
    wall_surface->u = wall_surface->u + inward * (top_end ? fillet_width : -fillet_width);
  }
  for (Id id : rail_edges) {
    Edge *rail = &*std::find_if(out.edges.begin(), out.edges.end(), [&](const Edge &e) {
      return e.id == id;
    });
    const Vec3 a = vertex(out, rail->vertices[0])->position;
    const Vec3 b = vertex(out, rail->vertices[1])->position;
    const double span = rail->end - rail->start;
    if (std::abs(span) <= std::numeric_limits<double>::epsilon())
      return reject("The cylinder wall has a degenerate seam edge");
    Curve support;
    support.id = out.next_id++;
    support.kind = CurveKind::Line;
    support.u = (b - a) / span;
    support.origin = a - support.u * rail->start;
    rail->curve = support.id;
    out.curves.push_back(support);
  }

  auto rail_between = [&](const int station, const Id source_vertex) {
    const Id a = station_vertices[station].at(source_vertex);
    const Id b = station_vertices[station + 1].at(source_vertex);
    const Vec3 pa = vertex(out, a)->position;
    const Vec3 pb = vertex(out, b)->position;
    Curve support;
    support.id = out.next_id++;
    support.kind = CurveKind::Line;
    support.origin = pa;
    support.u = pb - pa;
    const Id edge_id = out.next_id++;
    out.curves.push_back(support);
    out.edges.push_back({edge_id, support.id, {a, b}, 0, 1});
    return edge_id;
  };
  std::map<std::pair<int, Id>, Id> rails;
  for (int i = 0; i < steps; ++i)
    for (Id v : ring_vertices)
      rails[{i, v}] = rail_between(i, v);

  for (int i = 0; i < steps; ++i) {
    for (const Coedge &cap_use : cap.outer) {
      const Edge &source = *edge(model, cap_use.edge);
      Surface support;
      support.id = out.next_id++;
      support.curve = station_curves[i];
      support.start = source.start;
      support.end = source.end;
      const double parameter = (source.start + source.end) * .5;
      const bool cone_end = collapse_to_apex && i == steps - 1;
      Vec3 point, raw_normal;
      if (cone_end) {
        support.kind = SurfaceKind::Cone;
        support.origin = vertex(out, apex_vertex)->position;
        const Curve &base = *curve(out, support.curve);
        point = evaluate(base, parameter);
        raw_normal = cross(tangent(base, parameter), support.origin - point);
      }
      else {
        support.kind = SurfaceKind::Ruled;
        support.other_curve = station_curves[i + 1];
        point = evaluate_ruled(
            support, *curve(out, support.curve), *curve(out, support.other_curve), parameter, .5);
        raw_normal = ruled_normal(
            support, *curve(out, support.curve), *curve(out, support.other_curve), parameter, .5);
      }
      const Vec3 axial = inward * dot(point - source_ring.origin, inward);
      const Vec3 radial = point - source_ring.origin - axial;
      if (!(length(raw_normal) > eps) || !(length(radial) > eps))
        return reject("Fillet generated a degenerate circular strip");
      int orientation = dot(raw_normal, radial / length(radial) + cap_normal) > 0 ? 1 : -1;
      const AdjacentWall &adjacent = walls.at(source.id);
      const Face &wall = *face(model, adjacent.face);
      const int wall_sense = wall.orientation * (adjacent.ring_use.reversed ? -1 : 1);
      const bool first_reversed = orientation != -wall_sense;
      if (first_reversed) {
        std::swap(support.start, support.end);
        orientation = -orientation;
      }
      const Id lower = station_edges[i].at(source.id);
      const Id rail_start = rails.at({i, source.vertices[0]});
      const Id rail_end = rails.at({i, source.vertices[1]});
      std::vector<Coedge> loop;
      if (cone_end) {
        if (!first_reversed)
          loop = {{lower, false}, {rail_end, false}, {rail_start, true}};
        else
          loop = {{lower, true}, {rail_start, false}, {rail_end, true}};
      }
      else {
        const Id upper = station_edges[i + 1].at(source.id);
        if (!first_reversed)
          loop = {{lower, false}, {rail_end, false}, {upper, true}, {rail_start, true}};
        else
          loop = {{lower, true}, {rail_start, false}, {upper, false}, {rail_end, true}};
      }
      const Id surface_id = support.id;
      const Id face_id = out.next_id++;
      out.surfaces.push_back(support);
      out.faces.push_back({face_id, surface_id, std::move(loop), {}, orientation});
      result.selection.faces.push_back(face_id);
    }
  }

  if (result.selection.faces.empty())
    return reject("No circular wall edges were available to fillet");
  result.selection.mode = 4;
  result.selection.active = result.selection.faces.front();
  result.selection = normalize_selection(out, std::move(result.selection));
  result.selection.mode = selection.mode;
  if (selection.mode == 1)
    result.selection.active = result.selection.vertices.front();
  else if (selection.mode == 2)
    result.selection.active = result.selection.edges.front();
  result.selection = normalize_selection(out, std::move(result.selection));
  const Id seed_face = walls.begin()->second.face;
  if (!validate_body(out, seed_face, error))
    return {};
  return result;
}

}  // namespace

std::optional<EditResult> bevel_circular_cap(const Model &model,
                                             const Selection &selection,
                                             const double width,
                                             const int subdivisions,
                                             std::string *error,
                                             bool &handled)
{
  handled = false;
  if (selection.faces.size() <= 1)
    return fillet_circular_cap(model, selection, width, subdivisions, error, handled);

  std::vector<Id> cap_ids;
  std::unordered_set<Id> adjacent_walls;
  for (Id face_id : selection.faces) {
    Selection single;
    single.mode = 4;
    single.faces = {face_id};
    single = normalize_selection(model, std::move(single));
    bool face_handled = false;
    auto probe = fillet_circular_cap(model, single, width, subdivisions, error, face_handled);
    if (face_handled) {
      handled = true;
      if (!probe)
        return {};
      cap_ids.push_back(face_id);
      const Face &cap = *face(model, face_id);
      for (const Coedge &use : cap.outer)
        for (const Face &adjacent : model.faces)
          if (adjacent.id != face_id &&
              std::any_of(adjacent.outer.begin(), adjacent.outer.end(), [&](const Coedge &item) {
                return item.edge == use.edge;
              }))
          {
            adjacent_walls.insert(adjacent.id);
          }
    }
  }
  if (cap_ids.empty())
    return {};
  for (Id face_id : selection.faces) {
    if (std::find(cap_ids.begin(), cap_ids.end(), face_id) != cap_ids.end())
      continue;
    if (selection.mode != 1 || !adjacent_walls.contains(face_id) ||
        surface(model, face(model, face_id)->surface)->kind != SurfaceKind::Extrusion)
    {
      return {};
    }
  }

  handled = true;
  EditResult result{model, {}};
  std::vector<Id> generated_faces;
  for (Id cap_id : cap_ids) {
    Selection single;
    single.mode = 4;
    single.faces = {cap_id};
    single = normalize_selection(result.model, std::move(single));
    bool face_handled = false;
    auto beveled = fillet_circular_cap(
        result.model, single, width, subdivisions, error, face_handled);
    if (!face_handled || !beveled)
      return {};
    std::unordered_set<Id> previous;
    for (const Face &f : result.model.faces)
      previous.insert(f.id);
    for (const Face &f : beveled->model.faces)
      if (!previous.contains(f.id))
        generated_faces.push_back(f.id);
    result.model = std::move(beveled->model);
  }
  result.selection.mode = 4;
  result.selection.faces = std::move(generated_faces);
  result.selection.active = result.selection.faces.front();
  result.selection = normalize_selection(result.model, std::move(result.selection));
  result.selection.mode = selection.mode;
  if (selection.mode == 1)
    result.selection.active = result.selection.vertices.front();
  else if (selection.mode == 2)
    result.selection.active = result.selection.edges.front();
  result.selection = normalize_selection(result.model, std::move(result.selection));
  if (!validate_body(result.model, cap_ids.front(), error))
    return {};
  return result;
}

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
  if (model.faces.empty())
    return fillet_wire_vertices(model, std::move(selection), width, subdivisions, error);
  {
    bool handled = false;
    auto circular_cap = bevel_circular_cap(
        model, selection, width, subdivisions, error, handled);
    if (handled)
      return circular_cap;
  }
  if (vertices_only || selection.mode == 1)
    return fillet_vertices(model, std::move(selection), width, subdivisions, error, warning);
  if (selection.edges.empty())
    return reject("Select the edges of a closed convex planar body to fillet");
  std::map<Id, std::vector<Id>> uses;
  for (const Face &f : model.faces) {
    for (const Coedge &use : f.outer)
      uses[use.edge].push_back(f.id);
    for (const auto &hole : f.holes)
      for (const Coedge &use : hole)
        uses[use.edge].push_back(f.id);
  }
  std::map<Id, std::vector<Id>> incident_edges;
  for (const Edge &e : model.edges) {
    incident_edges[e.vertices[0]].push_back(e.id);
    incident_edges[e.vertices[1]].push_back(e.id);
  }
  std::set<Id> body_faces, body_edges, body_vertices;
  std::vector<Id> pending;
  const auto seed_uses = uses.find(selection.edges.front());
  if (seed_uses != uses.end()) {
    for (Id id : seed_uses->second) {
      body_faces.insert(id);
      pending.push_back(id);
    }
  }
  while (!pending.empty()) {
    const Face &f = *face(model, pending.back());
    pending.pop_back();
    auto visit_loop = [&](const std::vector<Coedge> &loop) {
      for (const Coedge &use : loop) {
        const Edge &e = *edge(model, use.edge);
        body_edges.insert(e.id);
        body_vertices.insert(e.vertices.begin(), e.vertices.end());
        for (Id adjacent : uses[e.id])
          if (body_faces.insert(adjacent).second)
            pending.push_back(adjacent);
      }
    };
    visit_loop(f.outer);
    for (const auto &hole : f.holes)
      visit_loop(hole);
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
  std::set<Id> affected, local_faces;
  for (Id id : selected) {
    const Edge &e = *edge(model, id);
    for (Id v : e.vertices) {
      affected.insert(v);
      for (Id incident : incident_edges[v]) {
        if (!body_edges.contains(incident))
          return reject("Fillet endpoint also belongs to loose wire geometry");
        const auto found = uses.find(incident);
        if (found != uses.end())
          local_faces.insert(found->second.begin(), found->second.end());
      }
    }
  }
  std::map<Id, Vec3> normals;
  std::map<Id, std::vector<Id>> vertex_faces;
  for (Id v : affected) {
    std::set<Id> adjacent_faces;
    for (Id incident : incident_edges[v])
      for (Id f : uses[incident])
        adjacent_faces.insert(f);
    vertex_faces[v].assign(adjacent_faces.begin(), adjacent_faces.end());
  }
  for (Id id : local_faces) {
    const Face &f = *face(model, id);
    if (surface(model, f.surface)->kind != SurfaceKind::Plane || !f.holes.empty())
      return reject("Fillet needs planar, hole-free faces around the selected edge endpoints");
    std::vector<Id> points;
    for (const Coedge &use : f.outer) {
      const Edge &e = *edge(model, use.edge);
      if ((affected.contains(e.vertices[0]) || affected.contains(e.vertices[1])) &&
          curve(model, e.curve)->kind != CurveKind::Line)
        return reject("Fillet needs straight edges meeting each selected edge endpoint");
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
        return reject("Fillet currently supports convex bodies at the selected edge");
  }
  return fillet_trimmed_edges(
      model, selection, normals, uses, vertex_faces, width, subdivisions, eps, error);
}
}  // namespace blender::solid
