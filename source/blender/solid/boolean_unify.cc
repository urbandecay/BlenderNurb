/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_unify.hh"

#include "SOL_boolean.hh"
#include "SOL_classify.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
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

std::vector<const std::vector<Coedge> *> loops(const Face &face)
{
  std::vector<const std::vector<Coedge> *> result{&face.outer};
  for (const auto &hole : face.holes) {
    result.push_back(&hole);
  }
  return result;
}

using WideVector = std::array<long double, 3>;

bool exact_sum(const long double a, const long double b, long double &result)
{
  result = a + b;
  const long double virtual_b = result - a;
  return std::isfinite(result) &&
         (a - (result - virtual_b)) + (b - virtual_b) == 0;
}

bool exact_product(const long double a, const long double b, long double &result)
{
  result = a * b;
  return std::isfinite(result) &&
         (a == 0 || b == 0 || std::abs(result) >= std::numeric_limits<long double>::min()) &&
         std::fma(a, b, -result) == 0;
}

std::optional<WideVector> exact_difference(const Vec3 a, const Vec3 b)
{
  WideVector difference;
  for (int i = 0; i < 3; ++i) {
    if (!exact_sum(a[i], -static_cast<long double>(b[i]), difference[i])) {
      return {};
    }
  }
  return difference;
}

bool certified_parallel(const WideVector &a, const WideVector &b)
{
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const long double first = a[j] * b[k], second = a[k] * b[j];
    if (!std::isfinite(first) || !std::isfinite(second) || first != second ||
        (((a[j] != 0 && b[k] != 0) || (a[k] != 0 && b[j] != 0)) &&
         std::abs(first) < std::numeric_limits<long double>::min()))
    {
      return false;
    }
    if (std::fma(a[j], b[k], -first) != std::fma(a[k], b[j], -second)) {
      return false;
    }
  }
  return true;
}

bool certified_zero_triple(const Vec3 a, const Vec3 b, const WideVector &c)
{
  long double value = 0;
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    long double first, second, difference, term;
    if (!exact_product(a[j], b[k], first) || !exact_product(a[k], b[j], second) ||
        !exact_sum(first, -second, difference) || !exact_product(difference, c[i], term) ||
        !exact_sum(value, term, value))
    {
      return false;
    }
  }
  return value == 0;
}

long double triple(const Vec3 a, const Vec3 b, const Vec3 n)
{
  long double value = 0;
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    value += (static_cast<long double>(a[j]) * b[k] -
              static_cast<long double>(a[k]) * b[j]) * n[i];
  }
  return value;
}

long double triple_magnitude(const Vec3 a, const Vec3 b, const Vec3 n)
{
  long double value = 0;
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    value += (std::abs(static_cast<long double>(a[j]) * b[k]) +
              std::abs(static_cast<long double>(a[k]) * b[j])) * std::abs(n[i]);
  }
  return value;
}

long double loop_area(const Model &model, const std::vector<Coedge> &loop, const Vec3 normal)
{
  const Vec3 reference = vertex(model, first_vertex(model, loop.front()))->position;
  long double integral = 0, magnitude = 0;
  for (const Coedge use : loop) {
    const Edge &boundary = *edge(model, use.edge);
    const Curve &support = *curve(model, boundary.curve);
    const double a = use.reversed ? boundary.end : boundary.start;
    const double b = use.reversed ? boundary.start : boundary.end;
    const long double delta = static_cast<long double>(b) - a;
    const Vec3 origin = support.origin - reference;
    if (support.kind == CurveKind::Line) {
      integral += triple(origin, support.u, normal) * delta;
      magnitude += triple_magnitude(origin, support.u, normal) * std::abs(delta);
    }
    else if (support.kind == CurveKind::Conic) {
      /* Stable trigonometric differences avoid subtracting nearly equal
       * endpoint evaluations on a short native arc. */
      const long double middle = static_cast<long double>(a) / 2 +
                                 static_cast<long double>(b) / 2;
      const long double half_sine = 2 * std::sin(delta / 2);
      const long double cosine_delta = -std::sin(middle) * half_sine;
      const long double sine_delta = std::cos(middle) * half_sine;
      integral += triple(origin, support.u, normal) * cosine_delta +
                  triple(origin, support.v, normal) * sine_delta +
                  triple(support.u, support.v, normal) * delta;
      magnitude += triple_magnitude(origin, support.u, normal) * std::abs(cosine_delta) +
                   triple_magnitude(origin, support.v, normal) * std::abs(sine_delta) +
                   triple_magnitude(support.u, support.v, normal) * std::abs(delta);
    }
    else {
      throw std::runtime_error("Coplanar unification needs native Line/Conic trim integrals");
    }
  }
  if (!std::isfinite(integral) || !std::isfinite(magnitude) ||
      std::abs(integral) <= 128 * std::numeric_limits<double>::epsilon() * magnitude)
  {
    throw std::runtime_error("Coplanar unification found an unresolved zero-area boundary");
  }
  return integral / 2;
}

Vec3 plane_normal(const Surface &surface)
{
  const Vec3 normal = cross(surface.u, surface.v);
  return normal / length(normal);
}

bool same_plane(const Surface &a, const Surface &b)
{
  if (a.origin == b.origin &&
      (((a.u == b.u || a.u == b.u * -1) && (a.v == b.v || a.v == b.v * -1)) ||
       ((a.u == b.v || a.u == b.v * -1) && (a.v == b.u || a.v == b.u * -1)))) {
    return true;
  }
  const auto difference = exact_difference(b.origin, a.origin);
  return difference && certified_zero_triple(a.u, a.v, {b.u.x, b.u.y, b.u.z}) &&
         certified_zero_triple(a.u, a.v, {b.v.x, b.v.y, b.v.z}) &&
         certified_zero_triple(a.u, a.v, *difference);
}

std::vector<std::vector<Coedge>> close_loops(const Model &model,
                                          const std::map<Id, std::vector<Coedge>> &uses)
{
  std::map<Id, Coedge> outgoing, remaining;
  std::set<Id> incoming;
  for (const auto &[id, occurrences] : uses) {
    if (occurrences.size() == 2) {
      if (occurrences[0].reversed == occurrences[1].reversed) {
        throw std::runtime_error("Coplanar faces have inconsistent internal edge senses");
      }
      continue;
    }
    if (occurrences.size() != 1) {
      throw std::runtime_error("Coplanar unification found a nonmanifold edge");
    }
    const Coedge use = occurrences.front();
    if (!outgoing.emplace(first_vertex(model, use), use).second ||
        !incoming.insert(last_vertex(model, use)).second)
    {
      throw std::runtime_error("Coplanar unification found an ambiguous boundary junction");
    }
    remaining.emplace(id, use);
  }
  std::vector<std::vector<Coedge>> result;
  while (!remaining.empty()) {
    std::vector<Coedge> loop;
    Coedge current = remaining.begin()->second;
    const Id first = first_vertex(model, current);
    for (;;) {
      if (!remaining.erase(current.edge)) {
        throw std::runtime_error("Coplanar boundary did not close into simple native cycles");
      }
      loop.push_back(current);
      const Id last = last_vertex(model, current);
      if (last == first) {
        break;
      }
      const auto next = outgoing.find(last);
      if (next == outgoing.end()) {
        throw std::runtime_error("Coplanar unification produced an open boundary");
      }
      current = next->second;
    }
    result.push_back(std::move(loop));
  }
  return result;
}

bool replace_pair(const Model &model,
                  std::vector<Coedge> &loop,
                  const Id first,
                  const Id second,
                  const Id junction)
{
  size_t count_first = 0, count_second = 0;
  Coedge retained;
  for (const Coedge use : loop) {
    if (use.edge == first) {
      ++count_first;
      retained = use;
    }
    count_second += use.edge == second;
  }
  if (count_first == 0 && count_second == 0) {
    return true;
  }
  if (count_first != 1 || count_second != 1) {
    return false;
  }
  for (size_t i = 0; i < loop.size(); ++i) {
    const Coedge a = loop[i], b = loop[(i + 1) % loop.size()];
    if (((a.edge == first && b.edge == second) ||
         (a.edge == second && b.edge == first)) &&
        last_vertex(model, a) == junction && first_vertex(model, b) == junction)
    {
      std::rotate(loop.begin(), loop.begin() + i, loop.end());
      loop.erase(loop.begin(), loop.begin() + 2);
      loop.insert(loop.begin(), retained);
      return true;
    }
  }
  return false;
}

std::optional<Edge> joined_edge(const Model &model, const Edge &a, const Edge &b, const Id junction)
{
  if (a.vertices[0] == a.vertices[1] || b.vertices[0] == b.vertices[1]) {
    return {};
  }
  const Curve &ac = *curve(model, a.curve), &bc = *curve(model, b.curve);
  if (ac.kind != bc.kind) {
    return {};
  }
  const bool at_end = a.vertices[1] == junction;
  const bool b_at_end = b.vertices[1] == junction;
  const Id other = b.vertices[b_at_end ? 0 : 1];
  const double other_parameter = b_at_end ? b.start : b.end;
  long double parameter;
  if (ac.kind == CurveKind::Line) {
    const auto difference = exact_difference(bc.origin, ac.origin);
    const WideVector direction{ac.u.x, ac.u.y, ac.u.z};
    if (!difference || !certified_parallel(direction, {bc.u.x, bc.u.y, bc.u.z}) ||
        !certified_parallel(direction, *difference))
    {
      return {};
    }
    long double numerator = 0, denominator = 0;
    for (int i = 0; i < 3; ++i) {
      numerator += ((static_cast<long double>(bc.origin[i]) - ac.origin[i]) +
                    static_cast<long double>(bc.u[i]) * other_parameter) * ac.u[i];
      denominator += static_cast<long double>(ac.u[i]) * ac.u[i];
    }
    parameter = numerator / denominator;
  }
  else if (ac.kind == CurveKind::Conic && ac.origin == bc.origin && ac.u == bc.u &&
           ac.v == bc.v && ac.w == bc.w &&
           (at_end ? a.end : a.start) == (b_at_end ? b.end : b.start))
  {
    parameter = other_parameter;
  }
  else {
    return {};
  }
  const long double direction = static_cast<long double>(a.end) - a.start;
  if (!std::isfinite(parameter) || !std::isfinite(double(parameter)) ||
      (at_end ? double(parameter) - a.end : a.start - double(parameter)) * direction <= 0)
  {
    return {};
  }
  Edge result = a;
  result.vertices[at_end ? 1 : 0] = other;
  (at_end ? result.end : result.start) = double(parameter);
  if ((result.vertices[0] == result.vertices[1] && ac.kind == CurveKind::Line) ||
      (ac.kind == CurveKind::Conic &&
       std::abs(result.end - result.start) > 64 * std::numbers::pi))
  {
    return {};
  }
  return result;
}

void simplify_boundary_edges(Model &model)
{
  for (;;) {
    std::map<Id, std::vector<Id>> incidence, faces;
    for (const Edge &boundary : model.edges) {
      incidence[boundary.vertices[0]].push_back(boundary.id);
      incidence[boundary.vertices[1]].push_back(boundary.id);
    }
    for (const Face &face : model.faces) {
      for (const auto *loop : loops(face)) {
        for (const Coedge use : *loop) {
          faces[use.edge].push_back(face.id);
        }
      }
    }
    bool changed = false;
    for (const auto &[junction, incident] : incidence) {
      if (incident.size() != 2 || incident[0] == incident[1] ||
          faces[incident[0]] != faces[incident[1]])
      {
        continue;
      }
      const Edge &a = *edge(model, incident[0]), &b = *edge(model, incident[1]);
      const auto merged = joined_edge(model, a, b, junction);
      if (!merged) {
        continue;
      }
      Model trial = model;
      bool compatible = true;
      for (Face &face : trial.faces) {
        compatible &= replace_pair(model, face.outer, a.id, b.id, junction);
        for (auto &hole : face.holes) {
          compatible &= replace_pair(model, hole, a.id, b.id, junction);
        }
      }
      if (!compatible) {
        continue;
      }
      *std::find_if(trial.edges.begin(), trial.edges.end(),
                    [&](const Edge &boundary) { return boundary.id == a.id; }) = *merged;
      std::erase_if(trial.edges, [&](const Edge &boundary) { return boundary.id == b.id; });
      /* Structured nonplanar patches may require a particular edge template.
       * A merge is optional and must preserve that native representation. */
      if (!validate(trial)) {
        continue;
      }
      model = std::move(trial);
      changed = true;
      break;
    }
    if (!changed) {
      return;
    }
  }
}

}  // namespace

bool boolean_same_planar_support(const Surface &first, const Surface &second)
{
  return first.kind == SurfaceKind::Plane && second.kind == SurfaceKind::Plane &&
         length(cross(first.u, first.v)) > 0 && length(cross(second.u, second.v)) > 0 &&
         same_plane(first, second);
}

std::optional<Model> boolean_unify_coplanar_faces(const Model &source, std::string *error)
{
  try {
    std::string message;
    if (!validate(source, &message)) {
      throw std::runtime_error(message);
    }
    std::map<Id, std::vector<size_t>> adjacent;
    std::vector<size_t> parent(source.faces.size());
    for (size_t i = 0; i < source.faces.size(); ++i) {
      parent[i] = i;
      const Face &face = source.faces[i];
      if (!validate_body(source, face.id, &message)) {
        throw std::runtime_error("Coplanar unification needs a closed native boundary: " + message);
      }
      for (const auto *loop : loops(face)) {
        for (const Coedge use : *loop) {
          adjacent[use.edge].push_back(i);
        }
      }
    }
    auto root = [&](size_t node) {
      while (parent[node] != node) {
        node = parent[node];
      }
      return node;
    };
    for (const auto &[id, uses] : adjacent) {
      if (uses.size() != 2 || uses[0] == uses[1]) {
        continue;
      }
      const Face &a = source.faces[uses[0]], &b = source.faces[uses[1]];
      const Surface &as = *surface(source, a.surface), &bs = *surface(source, b.surface);
      if (as.kind != SurfaceKind::Plane || bs.kind != SurfaceKind::Plane || !same_plane(as, bs)) {
        continue;
      }
      const Vec3 normal = plane_normal(as);
      const long double aa = loop_area(source, a.outer, normal) * a.orientation;
      const long double ba = loop_area(source, b.outer, normal) * b.orientation;
      if ((aa > 0) != (ba > 0)) {
        continue;
      }
      parent[root(uses[1])] = root(uses[0]);
    }
    std::map<size_t, std::vector<size_t>> components;
    for (size_t i = 0; i < source.faces.size(); ++i) {
      components[root(i)].push_back(i);
    }
    Model result = source;
    result.faces.clear();
    for (const auto &[representative, members] : components) {
      const Face &original = source.faces[representative];
      if (members.size() == 1) {
        Face copy = original;
        copy.group = 0;
        result.faces.push_back(std::move(copy));
        continue;
      }
      const Vec3 normal = plane_normal(*surface(source, original.surface));
      const bool positive_outer = loop_area(source, original.outer, normal) > 0;
      std::map<Id, std::vector<Coedge>> uses;
      for (const size_t member : members) {
        const Face &face = source.faces[member];
        for (const auto *loop : loops(face)) {
          for (Coedge use : *loop) {
            use.reversed ^= face.orientation != original.orientation;
            uses[use.edge].push_back(use);
          }
        }
      }
      std::vector<Face> outer_faces;
      std::vector<std::vector<Coedge>> holes;
      for (auto &loop : close_loops(source, uses)) {
        if ((loop_area(source, loop, normal) > 0) != positive_outer) {
          holes.push_back(std::move(loop));
          continue;
        }
        if (result.next_id == std::numeric_limits<Id>::max()) {
          throw std::runtime_error("Coplanar unification exhausted native identities");
        }
        outer_faces.push_back(
            {result.next_id++, original.surface, std::move(loop), {}, original.orientation});
      }
      for (const auto &hole : holes) {
        const Vec3 point = vertex(source, first_vertex(source, hole.front()))->position;
        Face *owner = nullptr;
        for (Face &outer : outer_faces) {
          const auto state = classify_face_trim_point(source, outer, point, {0, 0}, &message);
          if (!state || *state == PointClassification::Boundary ||
              *state == PointClassification::Uncertain)
          {
            throw std::runtime_error("Coplanar hole ownership is unresolved: " + message);
          }
          if (*state == PointClassification::Inside) {
            if (owner) {
              throw std::runtime_error("A coplanar hole belongs to multiple outer boundaries");
            }
            owner = &outer;
          }
        }
        if (!owner) {
          throw std::runtime_error("A coplanar hole has no containing outer boundary");
        }
        owner->holes.push_back(hole);
      }
      if (outer_faces.empty()) {
        throw std::runtime_error("Coplanar unification removed an entire face component");
      }
      result.faces.insert(result.faces.end(), outer_faces.begin(), outer_faces.end());
    }
    auto pruned = boolean_sew_faces(std::span<const Model>(&result, 1), 0, true, &message);
    if (!pruned) {
      throw std::runtime_error(message);
    }
    simplify_boundary_edges(*pruned);
    auto boundary = boolean_sew_faces(std::span<const Model>(&*pruned, 1), 0, true, &message);
    if (!boundary) {
      throw std::runtime_error(message);
    }
    return boundary;
  }
  catch (const std::exception &exception) {
    if (error) {
      *error = exception.what();
    }
    return {};
  }
}

}  // namespace blender::solid
