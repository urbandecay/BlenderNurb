/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_cylinder_unify.hh"

#include "SOL_unify.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <utility>

namespace blender::solid {
namespace {

Id first_vertex(const Model &model, const Coedge use)
{
  return edge(model, use.edge)->vertices[use.reversed ? 1 : 0];
}

Id last_vertex(const Model &model, const Coedge use)
{
  return edge(model, use.edge)->vertices[use.reversed ? 0 : 1];
}

bool same_frame(const Surface &a, const Surface &b)
{
  return a.kind == SurfaceKind::Cylinder && b.kind == SurfaceKind::Cylinder &&
         a.origin == b.origin && a.u == b.u && a.v == b.v && a.w == b.w;
}

bool same_curve(const Curve &a, const Curve &b)
{
  return a.kind == b.kind && a.origin == b.origin && a.u == b.u && a.v == b.v && a.w == b.w;
}

bool certified_parallel(const Vec3 a, const Vec3 b)
{
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const long double first = static_cast<long double>(a[j]) * b[k];
    const long double second = static_cast<long double>(a[k]) * b[j];
    if (!std::isfinite(first) || !std::isfinite(second) || first != second ||
        (((a[j] != 0 && b[k] != 0) || (a[k] != 0 && b[j] != 0)) &&
         std::abs(first) < std::numeric_limits<long double>::min()) ||
        std::fma(static_cast<long double>(a[j]), static_cast<long double>(b[k]), -first) !=
            std::fma(static_cast<long double>(a[k]), static_cast<long double>(b[j]), -second))
    {
      return false;
    }
  }
  return true;
}

struct CylinderChart {
  Vec3 vxw, wxu;
  double determinant;

  explicit CylinderChart(const Surface &s)
      : vxw(cross(s.v, s.w)), wxu(cross(s.w, s.u)), determinant(dot(s.u, vxw))
  {
  }

  std::optional<int> rate(const Curve &c) const
  {
    if (c.kind == CurveKind::Line) {
      return 0;
    }
    if (c.kind != CurveKind::Conic) {
      return {};
    }
    const long double ux = dot(c.u, vxw) / determinant;
    const long double uy = dot(c.u, wxu) / determinant;
    const long double vx = dot(c.v, vxw) / determinant;
    const long double vy = dot(c.v, wxu) / determinant;
    const long double sign = ux * vy - uy * vx;
    if (!std::isfinite(sign) || std::abs(sign) < .5L) {
      return {};
    }
    return sign > 0 ? 1 : -1;
  }
};

using Uses = std::map<Id, std::vector<Coedge>>;

std::optional<std::vector<std::vector<Coedge>>> boundary_cycles(const Model &model,
                                                               const Uses &uses)
{
  std::map<Id, Coedge> outgoing, remaining;
  std::set<Id> incoming;
  for (const auto &[id, occurrences] : uses) {
    if (occurrences.size() == 2 && occurrences[0].reversed != occurrences[1].reversed) {
      continue;
    }
    if (occurrences.size() != 1) {
      return {};
    }
    const Coedge use = occurrences.front();
    if (!outgoing.emplace(first_vertex(model, use), use).second ||
        !incoming.insert(last_vertex(model, use)).second)
    {
      return {};
    }
    remaining.emplace(id, use);
  }
  std::vector<std::vector<Coedge>> result;
  while (!remaining.empty()) {
    Coedge current = remaining.begin()->second;
    const Id start = first_vertex(model, current);
    std::vector<Coedge> loop;
    for (;;) {
      if (!remaining.erase(current.edge)) {
        return {};
      }
      loop.push_back(current);
      const Id last = last_vertex(model, current);
      if (last == start) {
        break;
      }
      const auto next = outgoing.find(last);
      if (next == outgoing.end()) {
        return {};
      }
      current = next->second;
    }
    result.push_back(std::move(loop));
  }
  return result;
}

std::optional<int> winding(const Model &model,
                            const std::vector<Coedge> &loop,
                            const CylinderChart &chart)
{
  long double total = 0, magnitude = 0;
  for (const Coedge use : loop) {
    const Edge &e = *edge(model, use.edge);
    const auto rate = chart.rate(*curve(model, e.curve));
    if (!rate) {
      return {};
    }
    const long double delta = (static_cast<long double>(e.end) - e.start) *
                              (use.reversed ? -1 : 1) * *rate;
    total += delta;
    magnitude += std::abs(delta);
  }
  const long double period = 2 * std::numbers::pi_v<long double>;
  const long double turns = std::round(total / period);
  if (!std::isfinite(total) || std::abs(turns) > 1 ||
      std::abs(total - turns * period) >
          128 * std::numeric_limits<double>::epsilon() * std::max(1.L, magnitude))
  {
    return {};
  }
  return int(turns);
}

bool complete_conic(const Model &model, const std::vector<Coedge> &loop)
{
  const Curve &support = *curve(model, edge(model, loop.front().edge)->curve);
  if (support.kind != CurveKind::Conic) {
    return false;
  }
  int direction = 0;
  for (const Coedge use : loop) {
    const Edge &e = *edge(model, use.edge);
    if (!same_curve(support, *curve(model, e.curve))) {
      return false;
    }
    const double delta = (e.end - e.start) * (use.reversed ? -1 : 1);
    const int sense = delta > 0 ? 1 : -1;
    if (delta == 0 || (direction && direction != sense)) {
      return false;
    }
    direction = sense;
  }
  return true;
}

std::set<Id> cycle_vertices(const Model &model, const std::vector<Coedge> &loop)
{
  std::set<Id> result;
  for (const Coedge use : loop) {
    result.insert(first_vertex(model, use));
  }
  return result;
}

bool contiguous_cycle_at(const Model &model,
                          const std::vector<Coedge> &loop,
                          const Id start_vertex)
{
  const auto first = std::find_if(loop.begin(), loop.end(), [&](const Coedge use) {
    return first_vertex(model, use) == start_vertex;
  });
  if (first == loop.end()) {
    return false;
  }
  double previous = 0;
  for (size_t j = 0; j < loop.size(); ++j) {
    const Coedge use = loop[(size_t(first - loop.begin()) + j) % loop.size()];
    const Edge &e = *edge(model, use.edge);
    const double start = use.reversed ? e.end : e.start;
    if (j && start != previous) {
      return false;
    }
    previous = use.reversed ? e.start : e.end;
  }
  return true;
}

std::optional<std::vector<Coedge>> seam_path(const Model &model,
                                            const Surface &support,
                                            const Uses &uses,
                                            const std::vector<Coedge> &first,
                                            const std::vector<Coedge> &last)
{
  const auto starts = cycle_vertices(model, first), ends = cycle_vertices(model, last);
  std::map<Id, std::vector<Id>> incidence;
  for (const auto &[id, occurrences] : uses) {
    if (occurrences.size() != 2 || occurrences[0].reversed == occurrences[1].reversed) {
      continue;
    }
    const Edge &e = *edge(model, id);
    const Curve &c = *curve(model, e.curve);
    if (c.kind != CurveKind::Line || e.vertices[0] == e.vertices[1] ||
        !certified_parallel(c.u, support.w))
    {
      continue;
    }
    incidence[e.vertices[0]].push_back(id);
    incidence[e.vertices[1]].push_back(id);
  }
  for (const Id start : starts) {
    if (!incidence.contains(start) || incidence.at(start).size() != 1 || ends.contains(start) ||
        !contiguous_cycle_at(model, first, start))
    {
      continue;
    }
    std::vector<Coedge> path;
    std::set<Id> visited{start};
    Id current = start, previous_edge = 0;
    for (;;) {
      const auto adjacent = incidence.find(current);
      if (adjacent == incidence.end() || adjacent->second.size() > 2) {
        break;
      }
      Id next = 0;
      for (const Id id : adjacent->second) {
        if (id != previous_edge) {
          if (next) {
            break;
          }
          next = id;
        }
      }
      if (!next) {
        break;
      }
      const Edge &e = *edge(model, next);
      const Coedge use{next, e.vertices[0] != current};
      path.push_back(use);
      current = last_vertex(model, use);
      if (!visited.insert(current).second || starts.contains(current)) {
        break;
      }
      if (ends.contains(current)) {
        if (incidence.at(current).size() == 1 && contiguous_cycle_at(model, last, current)) {
          return path;
        }
        break;
      }
      previous_edge = next;
    }
  }
  return {};
}

void rotate_cycle(const Model &model, std::vector<Coedge> &cycle, const Id vertex_id)
{
  const auto start = std::find_if(cycle.begin(), cycle.end(), [&](const Coedge use) {
    return first_vertex(model, use) == vertex_id;
  });
  std::rotate(cycle.begin(), start, cycle.end());
}

std::optional<Edge> joined_seam(const Model &model, const std::vector<Coedge> &path)
{
  const Coedge first_use = path.front();
  Edge result = *edge(model, first_use.edge);
  const Curve &support = *curve(model, result.curve);
  double previous = first_use.reversed ? result.end : result.start;
  int direction = 0;
  for (const Coedge use : path) {
    const Edge &part = *edge(model, use.edge);
    if (!same_curve(support, *curve(model, part.curve))) {
      return {};
    }
    const double start = use.reversed ? part.end : part.start;
    const double end = use.reversed ? part.start : part.end;
    const int sense = end > start ? 1 : -1;
    if (start != previous || start == end || (direction && direction != sense)) {
      return {};
    }
    previous = end;
    direction = sense;
  }
  // All intervals share exactly the same native line and are contiguous in its
  // original parameterization. Reuse the first edge and extend only its trim;
  // no new geometric support or endpoint evaluation is needed.
  result.vertices[first_use.reversed ? 0 : 1] = last_vertex(model, path.back());
  (first_use.reversed ? result.start : result.end) = previous;
  return result;
}

struct MergedComponent {
  Face face;
  std::optional<Edge> seam;
};

std::optional<MergedComponent> merged_component(const Model &model,
                                               const std::vector<size_t> &members)
{
  Face result = model.faces[members.front()];
  Uses uses;
  for (const size_t member : members) {
    const Face &face = model.faces[member];
    if (!face.holes.empty()) {
      return {};
    }
    for (const Coedge use : face.outer) {
      uses[use.edge].push_back(use);
    }
  }
  auto cycles = boundary_cycles(model, uses);
  if (!cycles || cycles->empty() || cycles->size() > 2) {
    return {};
  }
  const Surface &support = *surface(model, result.surface);
  const CylinderChart chart(support);
  const auto first_winding = winding(model, cycles->front(), chart);
  if (!first_winding) {
    return {};
  }
  std::optional<Edge> seam;
  if (cycles->size() == 1) {
    if (*first_winding != 0) {
      return {};
    }
    result.outer = std::move(cycles->front());
  }
  else {
    const auto last_winding = winding(model, cycles->back(), chart);
    if (!last_winding || *first_winding == 0 || *first_winding != -*last_winding ||
        !complete_conic(model, cycles->front()) || !complete_conic(model, cycles->back()))
    {
      return {};
    }
    const auto path = seam_path(model, support, uses, cycles->front(), cycles->back());
    if (!path || path->empty()) {
      return {};
    }
    seam = joined_seam(model, *path);
    if (!seam) {
      return {};
    }
    rotate_cycle(model, cycles->front(), first_vertex(model, path->front()));
    rotate_cycle(model, cycles->back(), last_vertex(model, path->back()));
    result.outer = std::move(cycles->front());
    result.outer.push_back(path->front());
    result.outer.insert(result.outer.end(), cycles->back().begin(), cycles->back().end());
    result.outer.push_back({path->front().edge, !path->front().reversed});
  }
  result.group = 0;
  return MergedComponent{std::move(result), seam};
}

}  // namespace

std::optional<Model> boolean_unify_cylindrical_faces(const Model &source, std::string *error)
{
  auto fail = [&](const std::string &message) -> std::optional<Model> {
    if (error) {
      *error = message;
    }
    return {};
  };
  std::string message;
  if (!validate(source, &message)) {
    return fail(message);
  }
  std::map<Id, std::vector<size_t>> adjacent;
  std::vector<size_t> parent(source.faces.size());
  for (size_t i = 0; i < source.faces.size(); ++i) {
    parent[i] = i;
    const Face &face = source.faces[i];
    if (!validate_body(source, face.id, &message)) {
      return fail("Cylinder unification needs a closed native boundary: " + message);
    }
    for (const Coedge use : face.outer) {
      adjacent[use.edge].push_back(i);
    }
    for (const auto &hole : face.holes) {
      for (const Coedge use : hole) {
        adjacent[use.edge].push_back(i);
      }
    }
  }
  auto root = [&](size_t i) {
    while (parent[i] != i) {
      i = parent[i];
    }
    return i;
  };
  for (const auto &[id, members] : adjacent) {
    if (members.size() != 2 || members[0] == members[1]) {
      continue;
    }
    const Face &a = source.faces[members[0]], &b = source.faces[members[1]];
    if (a.orientation == b.orientation &&
        same_frame(*surface(source, a.surface), *surface(source, b.surface)))
    {
      parent[root(members[1])] = root(members[0]);
    }
  }
  std::map<size_t, std::vector<size_t>> components;
  for (size_t i = 0; i < source.faces.size(); ++i) {
    components[root(i)].push_back(i);
  }
  Model result = source;
  result.faces.clear();
  for (const auto &[representative, members] : components) {
    const auto merged = members.size() > 1 ? merged_component(source, members) : std::nullopt;
    if (merged) {
      result.faces.push_back(merged->face);
      if (merged->seam) {
        *std::find_if(result.edges.begin(), result.edges.end(), [&](const Edge &e) {
          return e.id == merged->seam->id;
        }) = *merged->seam;
      }
    }
    else {
      for (const size_t member : members) {
        result.faces.push_back(source.faces[member]);
      }
    }
  }
  // The shared planar unifier prunes cancelled geometry, checks closure, and
  // performs the existing globally safe native joins. Seam endpoints keep their
  // valence-three cap junctions while intermediate circular-rim vertices disappear.
  return boolean_unify_coplanar_faces(result, error);
}

}  // namespace blender::solid
