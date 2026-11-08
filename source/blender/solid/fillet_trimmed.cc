/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "fillet_intern.hh"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace blender::solid {
namespace {
using Key = std::pair<Id, Id>;
using SlotKey = std::tuple<Id, Id, Id>;
Vec3 unit(Vec3 p)
{
  return p / length(p);
}
struct Corner {
  Id vertex, incoming, outgoing;
  double h0 = 0, h1 = 0;
};
struct Station {
  Id original;
  Vec3 motion;
  int uses = 0;
  Id vertex = 0;
};
struct Ref {
  Id curve, a, b;
  bool reverse = false;
  Curve geometry;
};
struct EdgeRef {
  Id id;
  std::array<Id, 2> vertices;
};
struct Octant {
  Vec3 center;
  std::array<Vec3, 3> axes;
  std::array<Id, 3> vertices;
};
}  // namespace

/** Shared-profile bevel network. The construction follows the native bevel's face-offset
 * stations and homogeneous conic junctions, using only independent analytic primitives. */
std::optional<EditResult> fillet_trimmed_edges(const Model &model,
                                               const Selection &selection,
                                               const std::map<Id, Vec3> &normals,
                                               const std::map<Id, std::vector<Id>> &uses,
                                               const std::map<Id, std::vector<Id>> &vertex_faces,
                                               double width,
                                               int divisions,
                                               CornerTopology corner_topology,
                                               double eps,
                                               std::string *error)
{
  auto reject = [&](const char *s) -> std::optional<EditResult> {
    if (error)
      *error = s;
    return {};
  };
  const std::set<Id> selected(selection.edges.begin(), selection.edges.end());
  std::map<Id, int> count;
  std::set<Id> affected, body_edges;
  std::map<Id, std::vector<Id>> boundaries;
  std::map<Id, std::vector<Corner>> corners;
  std::map<SlotKey, int> slots;
  std::vector<std::array<int, 2>> pairs;
  std::map<Key, std::vector<std::pair<int, double>>> rays;
  for (Id id : selected) {
    if (uses.at(id).size() != 2)
      return reject("Fillet needs two faces per selected edge");
    const Edge &e = *edge(model, id);
    for (Id v : e.vertices) {
      ++count[v];
      affected.insert(v);
    }
    for (Id f : uses.at(id)) {
      const int first = slots.size();
      slots[{id, f, e.vertices[0]}] = first;
      slots[{id, f, e.vertices[1]}] = first + 1;
      pairs.push_back({first, first + 1});
    }
  }
  for (Id v : affected)
    if (vertex_faces.at(v).size() != 3)
      return reject("Rounded bevel currently needs a three-face endpoint fan");
  for (const auto &[id, normal] : normals) {
    const Face &f = *face(model, id);
    auto &points = boundaries[id];
    for (Coedge use : f.outer) {
      const Edge &e = *edge(model, use.edge);
      points.push_back(e.vertices[use.reversed ? 1 : 0]);
      body_edges.insert(e.id);
    }
    if (f.orientation < 0)
      std::reverse(points.begin(), points.end());
    auto find_edge = [&](Id a, Id b) {
      for (Coedge use : f.outer) {
        const Edge &e = *edge(model, use.edge);
        if ((e.vertices[0] == a && e.vertices[1] == b) ||
            (e.vertices[0] == b && e.vertices[1] == a))
          return e.id;
      }
      return Id(0);
    };
    for (size_t i = 0; i < points.size(); ++i) {
      const Id v = points[i], before = points[(i + points.size() - 1) % points.size()],
               after = points[(i + 1) % points.size()];
      Corner c{v, find_edge(before, v), find_edge(v, after)};
      const Vec3 a = vertex(model, before)->position - vertex(model, v)->position,
                 b = vertex(model, after)->position - vertex(model, v)->position;
      if (selected.contains(c.outgoing))
        c.h0 = length(a - b * (dot(a, b) / dot(b, b)));
      if (selected.contains(c.incoming))
        c.h1 = length(b - a * (dot(a, b) / dot(a, a)));
      if ((selected.contains(c.outgoing) && c.h0 <= eps) ||
          (selected.contains(c.incoming) && c.h1 <= eps))
        return reject("Fillet has a degenerate planar corner");
      const bool in = selected.contains(c.incoming), out = selected.contains(c.outgoing);
      if (in != out) {
        const Id sharp = in ? c.outgoing : c.incoming;
        rays[{v, sharp}].push_back(
            {slots.at({in ? c.incoming : c.outgoing, id, v}), in ? c.h1 : c.h0});
      }
      corners[id].push_back(c);
    }
  }
  // Opposite edge ends prefer the same width; each offset also prefers the requested
  // width with weight 0.2. Shared on-edge stations impose hard displacement ratios.
  std::vector<int> groups(slots.size(), -1);
  std::vector<double> scales(slots.size(), 1), widths(slots.size(), 1);
  int columns = 0;
  for (const auto &[key, ray] : rays) {
    if (ray.size() > 2)
      return reject("Fillet has a non-manifold width chain");
    if (ray.size() != 2)
      continue;
    const int a = ray[0].first, b = ray[1].first;
    if (groups[a] != -1 || groups[b] != -1)
      return reject("Fillet width constraints are ambiguous");
    groups[a] = groups[b] = columns++;
    scales[b] = ray[1].second / ray[0].second;
  }
  for (int &group : groups)
    if (group < 0)
      group = columns++;
  std::vector<std::vector<long double>> matrix(columns, std::vector<long double>(columns));
  std::vector<long double> rhs(columns);
  for (auto pair : pairs) {
    const int a = pair[0], b = pair[1], ga = groups[a], gb = groups[b];
    matrix[ga][ga] += scales[a] * scales[a];
    matrix[gb][gb] += scales[b] * scales[b];
    matrix[ga][gb] -= scales[a] * scales[b];
    matrix[gb][ga] -= scales[a] * scales[b];
    for (int slot : pair) {
      const int g = groups[slot];
      matrix[g][g] += .04L * scales[slot] * scales[slot];
      rhs[g] += .04L * scales[slot];
    }
  }
  for (int i = 0; i < columns; ++i) {
    for (int j = 0; j <= i; ++j) {
      long double value = matrix[i][j];
      for (int k = 0; k < j; ++k)
        value -= matrix[i][k] * matrix[j][k];
      if (i == j) {
        if (value <= 1e-20L)
          return reject("Fillet width system is ill-conditioned");
        matrix[i][j] = std::sqrt(value);
      }
      else
        matrix[i][j] = value / matrix[j][j];
    }
  }
  for (int i = 0; i < columns; ++i) {
    for (int k = 0; k < i; ++k)
      rhs[i] -= matrix[i][k] * rhs[k];
    rhs[i] /= matrix[i][i];
  }
  for (int i = columns - 1; i >= 0; --i) {
    for (int k = i + 1; k < columns; ++k)
      rhs[i] -= matrix[k][i] * rhs[k];
    rhs[i] /= matrix[i][i];
  }
  for (size_t i = 0; i < widths.size(); ++i) {
    widths[i] = double(rhs[groups[i]]) * scales[i];
    if (!(widths[i] > 0) || !std::isfinite(widths[i]))
      return reject("Fillet could not balance positive face widths");
  }
  std::vector<Station> stations;
  std::map<Key, int> face_station, edge_station;
  double limit = std::numeric_limits<double>::infinity();
  for (const auto &[f, list] : corners) {
    const auto &points = boundaries.at(f);
    for (size_t i = 0; i < list.size(); ++i) {
      const Corner &c = list[i];
      const bool in = selected.contains(c.incoming), out = selected.contains(c.outgoing);
      if (!in && !out)
        continue;
      const Vec3 origin = vertex(model, c.vertex)->position,
                 a = vertex(model, points[(i + points.size() - 1) % points.size()])->position -
                     origin,
                 b = vertex(model, points[(i + 1) % points.size()])->position - origin;
      const double first = out ? widths[slots.at({c.outgoing, f, c.vertex})] / c.h0 : 0,
                   second = in ? widths[slots.at({c.incoming, f, c.vertex})] / c.h1 : 0;
      for (double factor : {first, second})
        if (factor > 0)
          limit = std::min(limit, 1 / factor);
      const Id sharp = in && out ? 0 : in ? c.outgoing : c.incoming;
      int index;
      if (sharp && edge_station.contains({c.vertex, sharp}))
        index = edge_station.at({c.vertex, sharp});
      else {
        index = stations.size();
        stations.push_back({c.vertex, {}});
        if (sharp)
          edge_station[{c.vertex, sharp}] = index;
      }
      stations[index].motion = stations[index].motion + a * first + b * second;
      ++stations[index].uses;
      face_station[{c.vertex, f}] = index;
    }
  }
  for (Station &station : stations)
    station.motion = station.motion / station.uses;
  auto motion = [&](Id v, Id e) {
    auto it = edge_station.find({v, e});
    return it == edge_station.end() ? Vec3{} : stations[it->second].motion;
  };
  for (Id id : body_edges) {
    const Edge &e = *edge(model, id);
    const Vec3 delta = vertex(model, e.vertices[1])->position -
                       vertex(model, e.vertices[0])->position;
    auto bound = [&](Vec3 a, Vec3 b) {
      const double closing = dot(a - b, delta) / dot(delta, delta);
      if (closing > 0)
        limit = std::min(limit, 1 / closing);
    };
    if (selected.contains(id))
      for (Id f : uses.at(id))
        bound(stations[face_station.at({e.vertices[0], f})].motion,
              stations[face_station.at({e.vertices[1], f})].motion);
    else
      bound(motion(e.vertices[0], id), motion(e.vertices[1], id));
  }
  if (!(limit > 0) || !std::isfinite(limit))
    return reject("Fillet has no positive collision-free width");
  width = std::min(width, limit * (1 - 1e-9));
  EditResult result{model, {}};
  Model &out = result.model;
  std::set<Id> generated_vertices;
  std::erase_if(out.faces, [&](const Face &f) { return normals.contains(f.id); });
  std::erase_if(out.edges, [&](const Edge &e) { return body_edges.contains(e.id); });
  std::erase_if(out.vertices, [&](const Vertex &v) { return affected.contains(v.id); });
  auto point = [&](Vec3 p) {
    const Id id = out.next_id++;
    out.vertices.push_back({id, p});
    generated_vertices.insert(id);
    return id;
  };
  for (Station &s : stations)
    s.vertex = point(vertex(model, s.original)->position + s.motion * width);
  auto station_vertex = [&](Id v, Id e) {
    auto it = edge_station.find({v, e});
    return it == edge_station.end() ? v : stations[it->second].vertex;
  };
  std::map<Id, Octant> octants;
  for (Id v : affected)
    if (count[v] == 3) {
      Octant corner;
      Vec3 sum = vertex(model, v)->position * -1;
      int i = 0;
      for (Id f : vertex_faces.at(v)) {
        corner.vertices[i] = stations[face_station.at({v, f})].vertex;
        sum = sum + vertex(out, corner.vertices[i])->position;
        ++i;
      }
      corner.center = sum * .5;
      for (int k = 0; k < 3; ++k)
        corner.axes[k] = vertex(out, corner.vertices[k])->position - corner.center;
      if (std::abs(dot(corner.axes[0], cross(corner.axes[1], corner.axes[2]))) <= eps * eps * eps)
        return reject("Fillet has a collapsed affine corner");
      octants[v] = corner;
    }
  std::map<Key, Ref> profiles;
  std::map<Key, EdgeRef> edges;
  std::map<Key, Id> profile_points;
  auto profile = [&](Id a, Id b, Id v, double weight) -> Ref {
    const Key key = std::minmax(a, b);
    if (profiles.contains(key)) {
      Ref ref = profiles.at(key);
      ref.reverse = ref.a != a;
      return ref;
    }
    Vec3 control = vertex(model, v)->position;
    if (octants.contains(v)) {
      const Octant &o = octants.at(v);
      control = vertex(out, a)->position + vertex(out, b)->position - o.center;
      weight = std::sqrt(.5);
    }
    const Id id = out.next_id++;
    /* Own the curve value: appending more curves may reallocate out.curves. */
    Curve geometry{id,
                   CurveKind::RationalQuadratic,
                   vertex(out, a)->position,
                   control,
                   vertex(out, b)->position,
                   {1, weight, 1}};
    out.curves.push_back(geometry);
    Ref ref{id, a, b, false, geometry};
    profiles[key] = ref;
    profile_points[{id, 0}] = a;
    profile_points[{id, Id(divisions)}] = b;
    return ref;
  };
  auto parameter = [&](int k) {
    const double t = double(k) / divisions;
    if (corner_topology != CornerTopology::Quads)
      return t;
    const double a = std::sin(std::numbers::pi * .25 * t),
                 b = std::sin(std::numbers::pi * .25 * (1 - t));
    return a / (a + b);
  };
  auto profile_point = [&](Ref ref, int k) {
    const int at = ref.reverse ? divisions - k : k;
    const Key key{ref.curve, Id(at)};
    if (!profile_points.contains(key))
      profile_points[key] = point(evaluate(ref.geometry, parameter(at)));
    return profile_points.at(key);
  };
  auto boundary = [&](Id a, Id b, Id support, double start, double end, Id reuse = 0) -> Coedge {
    const Key key = std::minmax(a, b);
    if (edges.contains(key)) {
      const EdgeRef &e = edges.at(key);
      return {e.id, e.vertices[0] != a};
    }
    const Id id = reuse ? reuse : out.next_id++;
    out.edges.push_back({id, support, {a, b}, start, end});
    edges[key] = {id, {a, b}};
    return {id, false};
  };
  auto line = [&](Id a, Id b, Id reuse = 0) {
    const Key key = std::minmax(a, b);
    if (edges.contains(key)) {
      const EdgeRef &e = edges.at(key);
      return Coedge{e.id, e.vertices[0] != a};
    }
    const Id support = out.next_id++;
    out.curves.push_back({support,
                          CurveKind::Line,
                          vertex(out, a)->position,
                          vertex(out, b)->position - vertex(out, a)->position,
                          {}});
    return boundary(a, b, support, 0, 1, reuse);
  };
  std::map<Id, double> weights;
  for (Id id : selected) {
    const auto &f = uses.at(id);
    weights[id] = std::sqrt(std::max(0.0, (1 + dot(normals.at(f[0]), normals.at(f[1]))) * .5));
  }
  std::map<Id, double> joint_weights;
  for (Id v : affected)
    if (count[v] == 2) {
      double sum = 0;
      for (Id e : selected)
        if (edge(model, e)->vertices[0] == v || edge(model, e)->vertices[1] == v)
          sum += weights[e];
      joint_weights[v] = sum * .5;
    }
  result.selection.mode = 4;
  for (Id id : selected) {
    const Edge &e = *edge(model, id);
    const auto &adjacent = uses.at(id);
    Ref refs[2];
    for (int end = 0; end < 2; ++end) {
      const Id v = e.vertices[end];
      refs[end] = profile(stations[face_station.at({v, adjacent[0]})].vertex,
                          stations[face_station.at({v, adjacent[1]})].vertex,
                          v,
                          joint_weights.contains(v) ? joint_weights.at(v) : weights.at(id));
    }
    for (int i = 0; i < divisions; ++i) {
      const double u0 = parameter(i), u1 = parameter(i + 1);
      const Id a = profile_point(refs[0], i), b = profile_point(refs[0], i + 1),
               c = profile_point(refs[1], i + 1), d = profile_point(refs[1], i);
      Coedge first = boundary(a,
                              b,
                              refs[0].curve,
                              refs[0].reverse ? 1 - u0 : u0,
                              refs[0].reverse ? 1 - u1 : u1),
             last = boundary(d,
                             c,
                             refs[1].curve,
                             refs[1].reverse ? 1 - u0 : u0,
                             refs[1].reverse ? 1 - u1 : u1);
      const Coedge right = line(b, c);
      Coedge left = line(a, d);
      left.reversed = !left.reversed;
      last.reversed = !last.reversed;
      Surface s;
      s.id = out.next_id++;
      s.kind = SurfaceKind::Ruled;
      s.curve = refs[0].curve;
      s.other_curve = refs[1].curve;
      s.reverse_first = refs[0].reverse;
      s.reverse_second = refs[1].reverse;
      s.start = u0;
      s.end = u1;
      const Vec3 n = ruled_normal(s, refs[0].geometry, refs[1].geometry, (u0 + u1) * .5, .5);
      const int orientation = dot(n, normals.at(adjacent[0]) + normals.at(adjacent[1])) > 0 ? 1 :
                                                                                              -1;
      const Id face_id = out.next_id++;
      out.surfaces.push_back(s);
      out.faces.push_back({face_id, s.id, {first, right, last, left}, {}, orientation});
      result.selection.faces.push_back(face_id);
    }
  }
  for (const auto &[f, list] : corners) {
    std::vector<Coedge> loop;
    for (size_t i = 0; i < list.size(); ++i) {
      const Corner &c = list[i], &next = list[(i + 1) % list.size()];
      if (selected.contains(c.outgoing))
        loop.push_back(line(stations[face_station.at({c.vertex, f})].vertex,
                            stations[face_station.at({next.vertex, f})].vertex));
      else
        loop.push_back(line(station_vertex(c.vertex, c.outgoing),
                            station_vertex(next.vertex, c.outgoing),
                            c.outgoing));
      if (affected.contains(next.vertex) && !face_station.contains({next.vertex, f})) {
        const Id a = station_vertex(next.vertex, next.incoming),
                 b = station_vertex(next.vertex, next.outgoing);
        const Key key = std::minmax(a, b);
        if (!profiles.contains(key))
          return reject("Fillet cap lost its shared conic profile");
        Ref ref = profiles.at(key);
        ref.reverse = ref.a != a;
        for (int k = 0; k < divisions; ++k) {
          const double u0 = parameter(k), u1 = parameter(k + 1);
          loop.push_back(boundary(profile_point(ref, k),
                                  profile_point(ref, k + 1),
                                  ref.curve,
                                  ref.reverse ? 1 - u0 : u0,
                                  ref.reverse ? 1 - u1 : u1));
        }
      }
    }
    Face rebuilt = *face(model, f);
    rebuilt.outer = std::move(loop);
    rebuilt.group = 0;
    if (rebuilt.orientation < 0) {
      std::reverse(rebuilt.outer.begin(), rebuilt.outer.end());
      for (Coedge &use : rebuilt.outer)
        use.reversed = !use.reversed;
    }
    out.faces.push_back(std::move(rebuilt));
  }
  for (const auto &[v, o] : octants) {
    auto mapped = [&](Vec3 p) {
      return o.center + o.axes[0] * p.x + o.axes[1] * p.y + o.axes[2] * p.z;
    };
    Ref ab = profiles.at(std::minmax(o.vertices[0], o.vertices[1]));
    ab.reverse = ab.a != o.vertices[0];
    Ref ac = profiles.at(std::minmax(o.vertices[0], o.vertices[2]));
    ac.reverse = ac.a != o.vertices[0];
    Ref bc = profiles.at(std::minmax(o.vertices[1], o.vertices[2]));
    bc.reverse = bc.a != o.vertices[1];
    struct Node {
      Id vertex;
      Vec3 unit;
    };
    /* Integer grid coordinates have a small, fixed bound (divisions <= 32). */
    std::vector<Node> nodes((divisions + 1) * (divisions + 1));
    auto grid = [&](int i, int j) -> Node & { return nodes[i * (divisions + 1) + j]; };
    auto quarter = [](Vec3 a, Vec3 b, double t) {
      const double w = std::sqrt(.5), d = (1 - t) * (1 - t) + 2 * w * t * (1 - t) + t * t;
      return (a * ((1 - t) * (1 - t)) + (a + b) * (2 * w * t * (1 - t)) + b * (t * t)) / d;
    };
    for (int i = 0; i <= divisions; ++i)
      for (int j = 0; j <= divisions - i; ++j) {
        Node node;
        if (j == 0) {
          node = {profile_point(ab, i), quarter({1, 0, 0}, {0, 1, 0}, parameter(i))};
        }
        else if (i == 0) {
          node = {profile_point(ac, j), quarter({1, 0, 0}, {0, 0, 1}, parameter(j))};
        }
        else if (i + j == divisions) {
          node = {profile_point(bc, j), quarter({0, 1, 0}, {0, 0, 1}, parameter(j))};
        }
        else {
          node.unit = unit({double(divisions - i - j), double(i), double(j)});
          node.vertex = point(mapped(node.unit));
        }
        grid(i, j) = node;
      }
    auto arc = [&](Node a, Node b) {
      const Key key = std::minmax(a.vertex, b.vertex);
      if (edges.contains(key)) {
        const EdgeRef &e = edges.at(key);
        return Coedge{e.id, e.vertices[0] != a.vertex};
      }
      const double cosine = std::clamp(dot(a.unit, b.unit), -1.0, 1.0), angle = std::acos(cosine);
      const Vec3 perpendicular = unit(b.unit - a.unit * cosine);
      const Id c = out.next_id++;
      out.curves.push_back({c,
                            CurveKind::Conic,
                            o.center,
                            mapped(a.unit) - o.center,
                            mapped(perpendicular) - o.center});
      return boundary(a.vertex, b.vertex, c, 0, angle);
    };
    auto sample = [&](int i, int j) { return grid(i, j); };
    auto create = [&](Vec3 q) { return Node{point(mapped(q)), q}; };
    Id shared_support = 0;
    for (const auto &loop : corner_patch_nodes(divisions, corner_topology, sample, create)) {
      std::vector<Coedge> boundary;
      boundary.reserve(loop.size());
      for (size_t i = 0; i < loop.size(); ++i) {
        const Coedge use = arc(loop[i], loop[(i + 1) % loop.size()]);
        if (!use.edge)
          return reject("Rounded bevel generated a degenerate corner patch");
        boundary.push_back(use);
      }
      Surface s;
      s.id = shared_support ? shared_support : out.next_id++;
      s.kind = SurfaceKind::Sphere;
      s.origin = o.center;
      s.u = o.axes[0];
      s.v = o.axes[1];
      s.w = o.axes[2];
      const Id f = out.next_id++;
      if (!shared_support)
        out.surfaces.push_back(s);
      if (corner_topology == CornerTopology::Quads)
        shared_support = s.id;
      out.faces.push_back({f,
                           s.id,
                           std::move(boundary),
                           {},
                           dot(s.u, cross(s.v, s.w)) > 0 ? 1 : -1});
      result.selection.faces.push_back(f);
    }
  }
  std::set<Id> used_surfaces, used_curves;
  for (const Face &f : out.faces)
    used_surfaces.insert(f.surface);
  std::erase_if(out.surfaces, [&](const Surface &s) { return !used_surfaces.contains(s.id); });
  for (const Edge &e : out.edges)
    used_curves.insert(e.curve);
  for (const Surface &s : out.surfaces) {
    if (s.kind == SurfaceKind::Extrusion || s.kind == SurfaceKind::Ruled ||
        s.kind == SurfaceKind::Cone)
      used_curves.insert(s.curve);
    if (s.kind == SurfaceKind::Ruled)
      used_curves.insert(s.other_curve);
  }
  std::erase_if(out.curves, [&](const Curve &c) { return !used_curves.contains(c.id); });
  std::set<Id> used_vertices;
  for (const Edge &e : out.edges)
    used_vertices.insert(e.vertices.begin(), e.vertices.end());
  std::erase_if(out.vertices, [&](const Vertex &v) {
    return generated_vertices.contains(v.id) && !used_vertices.contains(v.id);
  });
  result.selection.active = result.selection.faces.front();
  result.selection = normalize_selection(out, std::move(result.selection));
  if (!validate_body(out, result.selection.active, error))
    return {};
  return result;
}
}  // namespace blender::solid
