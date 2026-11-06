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

namespace blender::solid {
namespace {

using Pair = std::pair<Id, Id>;

Pair edge_corner(Id edge_id, Id corner_id)
{
  return {edge_id, corner_id};
}

void fail(std::string *error, const char *message)
{
  if (error)
    *error = message;
}

struct WireCorner {
  Id vertex = 0;
  std::array<Id, 2> edges{};
  std::array<Vec3, 2> directions{};
  std::array<Id, 2> tangent_vertices{};
  Curve arc;
  double end_angle = 0;
};

}  // namespace

std::optional<EditResult> fillet_wire_vertices(const Model &model,
                                               Selection selection,
                                               double width,
                                               int subdivisions,
                                               std::string *error)
{
  auto reject = [&](const char *message) -> std::optional<EditResult> {
    fail(error, message);
    return {};
  };
  if (!std::isfinite(width) || width <= 0 || subdivisions < 1 || subdivisions > 32)
    return reject("Fillet needs a positive width and 1 to 32 subdivisions");

  selection = normalize_selection(model, std::move(selection));
  std::map<Id, std::vector<Id>> vertex_edges;
  for (const Edge &e : model.edges) {
    vertex_edges[e.vertices[0]].push_back(e.id);
    vertex_edges[e.vertices[1]].push_back(e.id);
  }

  Vec3 minimum{std::numeric_limits<double>::max(),
               std::numeric_limits<double>::max(),
               std::numeric_limits<double>::max()};
  Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
  double coordinate_scale = 1;
  for (const Vertex &v : model.vertices) {
    for (int axis = 0; axis < 3; ++axis) {
      minimum[axis] = std::min(minimum[axis], v.position[axis]);
      maximum[axis] = std::max(maximum[axis], v.position[axis]);
    }
    coordinate_scale = std::max(coordinate_scale, length(v.position));
  }
  const double eps = std::max(1e-10 * std::max(1.0, length(maximum - minimum)),
                              128 * std::numeric_limits<double>::epsilon() * coordinate_scale);
  if (width <= eps)
    return reject("Fillet width is too small for the wire geometry");

  std::vector<WireCorner> corners;
  std::map<Id, double> requested_trim;
  for (Id id : selection.vertices) {
    const auto &adjacent = vertex_edges[id];
    if (adjacent.size() != 2)
      continue;
    const Vec3 origin = vertex(model, id)->position;
    WireCorner corner;
    corner.vertex = id;
    std::array<double, 2> lengths{};
    bool supported = true;
    for (int i = 0; i < 2; ++i) {
      const Edge &e = *edge(model, adjacent[i]);
      if (curve(model, e.curve)->kind != CurveKind::Line) {
        supported = false;
        break;
      }
      const Id other = e.vertices[e.vertices[0] == id ? 1 : 0];
      const Vec3 direction = vertex(model, other)->position - origin;
      lengths[i] = length(direction);
      if (!(lengths[i] > eps)) {
        supported = false;
        break;
      }
      corner.edges[i] = e.id;
      corner.directions[i] = direction / lengths[i];
    }
    if (!supported)
      return reject("Wire fillet currently needs two straight edges at each selected corner");

    const double cosine = std::clamp(dot(corner.directions[0], corner.directions[1]), -1.0, 1.0);
    const double angle = std::acos(cosine);
    const Vec3 bisector = corner.directions[0] + corner.directions[1];
    const double bisector_length = length(bisector);
    if (!(angle > 1e-8) || !(angle < std::numbers::pi - 1e-8) || !(bisector_length > 1e-10))
      continue;

    const double half_angle = angle * 0.5;
    const double cos_half = std::cos(half_angle);
    const double radius = width * std::tan(half_angle);
    if (!(cos_half > 0) || !(radius > eps) || !std::isfinite(radius))
      continue;
    const Vec3 center = origin + bisector * (width / (bisector_length * cos_half));
    const Vec3 a = origin + corner.directions[0] * width;
    const Vec3 b = origin + corner.directions[1] * width;
    const Vec3 radial_sum = (a - center) + (b - center);
    const double radial_sum_length = length(radial_sum);
    if (!(radial_sum_length > eps))
      continue;
    const Vec3 through = center + radial_sum * (radius / radial_sum_length);
    auto arc = circle_through(a, through, b, error);
    if (!arc)
      return {};
    const Vec3 radial_b = b - arc->origin;
    const double u2 = dot(arc->u, arc->u), v2 = dot(arc->v, arc->v);
    if (!(u2 > 0) || !(v2 > 0))
      continue;
    corner.end_angle = std::atan2(dot(radial_b, arc->v) / v2, dot(radial_b, arc->u) / u2);
    const double mid_angle = std::atan2(dot(through - arc->origin, arc->v) / v2,
                                        dot(through - arc->origin, arc->u) / u2);
    if (std::abs(corner.end_angle) <= 1e-10 ||
        std::abs(corner.end_angle) >= std::numbers::pi + 1e-8 ||
        mid_angle * corner.end_angle <= 0 || std::abs(mid_angle) >= std::abs(corner.end_angle))
    {
      arc->v = arc->v * -1;
      corner.end_angle = -corner.end_angle;
    }
    if (std::abs(corner.end_angle) <= 1e-10 ||
        std::abs(corner.end_angle) >= std::numbers::pi + 1e-8)
      continue;

    for (int i = 0; i < 2; ++i) {
      requested_trim[corner.edges[i]] += width;
      if (requested_trim[corner.edges[i]] >= lengths[i] - eps)
        return reject("Fillet width consumes an adjoining wire edge");
    }
    corner.arc = *arc;
    corners.push_back(std::move(corner));
  }
  if (corners.empty())
    return reject("Select wire corners with two non-collinear straight edges to fillet");

  EditResult result{model, {}};
  Model &out = result.model;
  result.selection.mode = selection.mode;
  std::map<Pair, Id> trimmed;
  std::set<Id> removed_corners;
  for (WireCorner &corner : corners) {
    const Vec3 origin = vertex(model, corner.vertex)->position;
    for (int i = 0; i < 2; ++i) {
      const Id id = out.next_id++;
      corner.tangent_vertices[i] = id;
      out.vertices.push_back({id, origin + corner.directions[i] * width});
      trimmed[edge_corner(corner.edges[i], corner.vertex)] = id;
      result.selection.vertices.push_back(id);
    }
    corner.arc.id = out.next_id++;
    const Id curve_id = corner.arc.id;
    out.curves.push_back(corner.arc);
    Id previous = corner.tangent_vertices[0];
    for (int i = 1; i <= subdivisions; ++i) {
      Id next = corner.tangent_vertices[1];
      if (i < subdivisions) {
        next = out.next_id++;
        out.vertices.push_back(
            {next, evaluate(corner.arc, corner.end_angle * double(i) / subdivisions)});
        result.selection.vertices.push_back(next);
      }
      const Id edge_id = out.next_id++;
      out.edges.push_back({edge_id,
                           curve_id,
                           {previous, next},
                           corner.end_angle * double(i - 1) / subdivisions,
                           corner.end_angle * double(i) / subdivisions});
      previous = next;
    }
    removed_corners.insert(corner.vertex);
  }

  for (size_t i = 0; i < model.edges.size(); ++i) {
    Edge &e = out.edges[i];
    const Id first = e.vertices[0], second = e.vertices[1];
    if (const auto it = trimmed.find(edge_corner(e.id, first)); it != trimmed.end())
      e.vertices[0] = it->second;
    if (const auto it = trimmed.find(edge_corner(e.id, second)); it != trimmed.end())
      e.vertices[1] = it->second;
    if (first != e.vertices[0] || second != e.vertices[1]) {
      const Vec3 a = vertex(out, e.vertices[0])->position;
      const Vec3 b = vertex(out, e.vertices[1])->position;
      const Id curve_id = out.next_id++;
      out.curves.push_back({curve_id, CurveKind::Line, a, b - a, {}});
      e.curve = curve_id;
      e.start = 0;
      e.end = 1;
    }
  }
  std::erase_if(out.vertices, [&](const Vertex &v) { return removed_corners.contains(v.id); });
  std::set<Id> used_curves;
  for (const Edge &e : out.edges)
    used_curves.insert(e.curve);
  std::erase_if(out.curves, [&](const Curve &c) { return !used_curves.contains(c.id); });

  if (result.selection.mode == 2) {
    const std::set<Id> selected_vertices(result.selection.vertices.begin(),
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

}  // namespace blender::solid
