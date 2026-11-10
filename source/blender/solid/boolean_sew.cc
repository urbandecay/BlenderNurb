/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_boolean.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace blender::solid {

std::optional<Model> boolean_sew_faces(const std::span<const Model> pieces,
                                       const double tolerance,
                                       const bool require_closed,
                                       std::string *error)
{
  try {
    if (!std::isfinite(tolerance) || tolerance < 0)
      throw std::runtime_error("Native face assembly needs a finite nonnegative tolerance");
    Model result;
    std::map<std::pair<Id, Id>, std::vector<Id>> candidates;
    std::string message;
    auto equal_support = [](const Curve &a, const Curve &b) {
      return a.kind == b.kind && a.origin == b.origin && a.u == b.u && a.v == b.v && a.w == b.w;
    };
    for (const Model &piece : pieces) {
      if (!validate(piece, &message))
        throw std::runtime_error(message);
      std::map<Id, Id> vertices, curves, surfaces;
      std::map<Id, Coedge> edges;
      for (const Vertex &v : piece.vertices) {
        auto match = std::find_if(
            result.vertices.begin(), result.vertices.end(), [&](const Vertex &p) {
              return length(p.position - v.position) <= tolerance;
            });
        if (match == result.vertices.end()) {
          const Id id = result.next_id++;
          result.vertices.push_back({id, v.position});
          vertices[v.id] = id;
        }
        else
          vertices[v.id] = match->id;
      }
      for (const Curve &c : piece.curves) {
        auto match = std::find_if(result.curves.begin(), result.curves.end(), [&](const Curve &p) {
          return equal_support(p, c);
        });
        if (match == result.curves.end()) {
          Curve copy = c;
          copy.id = result.next_id++;
          curves[c.id] = copy.id;
          result.curves.push_back(copy);
        }
        else
          curves[c.id] = match->id;
      }
      for (const Surface &s : piece.surfaces) {
        Surface copy = s;
        copy.id = result.next_id++;
        if (copy.curve)
          copy.curve = curves.at(copy.curve);
        if (copy.other_curve)
          copy.other_curve = curves.at(copy.other_curve);
        surfaces[s.id] = copy.id;
        result.surfaces.push_back(copy);
      }
      for (const Edge &e : piece.edges) {
        const Id a = vertices.at(e.vertices[0]), b = vertices.at(e.vertices[1]);
        const auto key = std::minmax(a, b);
        const Curve &c = *curve(piece, e.curve);
        Id match = 0;
        bool reversed = false;
        for (Id id : candidates[key]) {
          const Edge &previous = *edge(result, id);
          const Curve &previous_curve = *curve(result, previous.curve);
          if (c.kind != previous_curve.kind)
            continue;
          if (c.kind != CurveKind::Line) {
            if (!equal_support(c, previous_curve))
              continue;
            if (c.kind == CurveKind::Conic) {
              if (std::abs(std::abs(e.end - e.start) - std::abs(previous.end - previous.start)) >
                      1e-12 ||
                  length(evaluate(c, e.start + (e.end - e.start) / 2) -
                         evaluate(previous_curve,
                                  previous.start + (previous.end - previous.start) / 2)) >
                      tolerance)
                continue;
            }
            else if (std::min(e.start, e.end) != std::min(previous.start, previous.end) ||
                     std::max(e.start, e.end) != std::max(previous.start, previous.end))
              continue;
          }
          if (a == b && c.kind == CurveKind::Line)
            throw std::runtime_error("Native face assembly would collapse a straight edge");
          reversed = a == b ? (e.end - e.start) * (previous.end - previous.start) < 0 :
                              a != previous.vertices[0];
          match = id;
          break;
        }
        if (!match) {
          Edge copy = e;
          copy.id = result.next_id++;
          copy.curve = curves.at(e.curve);
          copy.vertices = {a, b};
          result.edges.push_back(copy);
          match = copy.id;
          candidates[key].push_back(match);
        }
        edges[e.id] = {match, reversed};
      }
      for (const Face &f : piece.faces) {
        Face copy = f;
        copy.id = result.next_id++;
        copy.surface = surfaces.at(f.surface);
        copy.group = 0;
        auto remap = [&](std::vector<Coedge> &loop) {
          for (Coedge &use : loop) {
            const Coedge mapped = edges.at(use.edge);
            use = {mapped.edge, use.reversed != mapped.reversed};
          }
        };
        remap(copy.outer);
        for (auto &hole : copy.holes)
          remap(hole);
        result.faces.push_back(std::move(copy));
      }
    }
    std::set<Id> used_edges, used_vertices, used_surfaces, used_curves;
    for (const Face &f : result.faces) {
      used_surfaces.insert(f.surface);
      for (Coedge use : f.outer)
        used_edges.insert(use.edge);
      for (const auto &hole : f.holes)
        for (Coedge use : hole)
          used_edges.insert(use.edge);
    }
    std::erase_if(result.edges, [&](const Edge &e) { return !used_edges.contains(e.id); });
    std::erase_if(result.surfaces,
                  [&](const Surface &s) { return !used_surfaces.contains(s.id); });
    for (const Edge &e : result.edges) {
      used_vertices.insert(e.vertices.begin(), e.vertices.end());
      used_curves.insert(e.curve);
    }
    for (const Surface &s : result.surfaces) {
      if (s.curve)
        used_curves.insert(s.curve);
      if (s.other_curve)
        used_curves.insert(s.other_curve);
    }
    std::erase_if(result.vertices, [&](const Vertex &v) { return !used_vertices.contains(v.id); });
    std::erase_if(result.curves, [&](const Curve &c) { return !used_curves.contains(c.id); });
    if (!validate(result, &message))
      throw std::runtime_error(message);
    if (require_closed) {
      for (const Face &f : result.faces)
        if (!validate_body(result, f.id, &message))
          throw std::runtime_error(message);
    }
    return result;
  }
  catch (const std::exception &exception) {
    if (error)
      *error = exception.what();
    return {};
  }
}

}  // namespace blender::solid
