/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_boolean.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>

namespace blender::solid {

std::optional<Model> boolean_partition_periodic_faces(const Model &source, std::string *error)
{
  try {
    std::string message;
    if (!validate(source, &message))
      throw std::runtime_error(message);
    Model result = source;
    for (Surface &s : result.surfaces) {
      if (s.kind != SurfaceKind::Extrusion)
        continue;
      const Curve *profile = curve(source, s.curve);
      if (!profile || profile->kind != CurveKind::Conic)
        continue;
      const Vec3 axis = s.u;
      s.kind = SurfaceKind::Cylinder;
      s.origin = profile->origin;
      s.u = profile->u;
      s.v = profile->v;
      s.w = axis;
      s.curve = 0;
      s.other_curve = 0;
    }
    std::map<Id, std::array<Id, 2>> parts;
    auto split_edge = [&](Id id) {
      if (parts.contains(id))
        return parts.at(id);
      const Edge old = *edge(result, id);
      const Curve &c = *curve(result, old.curve);
      const double middle = old.start + (old.end - old.start) / 2;
      if (middle == old.start || middle == old.end)
        throw std::runtime_error("Periodic edge has no representable split parameter");
      const Id vertex_id = result.next_id++;
      result.vertices.push_back({vertex_id, evaluate(c, middle)});
      Edge first = old, last = old;
      first.id = result.next_id++;
      last.id = result.next_id++;
      first.vertices[1] = vertex_id;
      first.end = middle;
      last.vertices[0] = vertex_id;
      last.start = middle;
      result.edges.push_back(first);
      result.edges.push_back(last);
      parts[id] = {first.id, last.id};
      return parts.at(id);
    };
    auto split_use = [&](Coedge use) {
      const auto ids = split_edge(use.edge);
      return use.reversed ? std::array<Coedge, 2>{{{ids[1], true}, {ids[0], true}}} :
                            std::array<Coedge, 2>{{{ids[0], false}, {ids[1], false}}};
    };
    std::vector<Face> faces;
    for (const Face &original : source.faces) {
      const Surface *support = surface(result, original.surface);
      if (support->kind != SurfaceKind::Cylinder || original.outer.size() != 4 ||
          !original.holes.empty())
      {
        faces.push_back(original);
        continue;
      }
      const Edge a = *edge(source, original.outer[0].edge),
                 b = *edge(source, original.outer[2].edge);
      if (curve(source, a.curve)->kind != CurveKind::Conic ||
          curve(source, b.curve)->kind != CurveKind::Conic ||
          std::abs(std::abs(a.end - a.start) - 2 * std::numbers::pi) > 1e-12 ||
          std::abs(std::abs(b.end - b.start) - 2 * std::numbers::pi) > 1e-12)
      {
        faces.push_back(original);
        continue;
      }
      const auto bottom = split_use(original.outer[0]), top = split_use(original.outer[2]);
      const Edge &lower = *edge(result, bottom[0].edge), &upper = *edge(result, top[0].edge);
      const Id lower_mid = lower.vertices[bottom[0].reversed ? 0 : 1];
      const Id upper_mid = upper.vertices[top[0].reversed ? 0 : 1];
      const Vec3 from = vertex(result, lower_mid)->position,
                 to = vertex(result, upper_mid)->position;
      auto generator = line(from, to, &message);
      if (!generator)
        throw std::runtime_error(message);
      generator->id = result.next_id++;
      result.curves.push_back(*generator);
      const Id generator_id = result.next_id++;
      result.edges.push_back({generator_id, generator->id, {lower_mid, upper_mid}, 0, 1});
      Face first = original, last = original;
      last.id = result.next_id++;
      first.group = original.group ? original.group : first.id;
      last.group = first.group;
      first.outer = {bottom[0], {generator_id, false}, top[1], original.outer[3]};
      last.outer = {bottom[1], original.outer[1], top[0], {generator_id, true}};
      faces.push_back(std::move(first));
      faces.push_back(std::move(last));
    }
    for (Face &f : faces) {
      auto remap = [&](std::vector<Coedge> &loop) {
        std::vector<Coedge> changed;
        for (Coedge use : loop) {
          if (!parts.contains(use.edge))
            changed.push_back(use);
          else {
            const auto ids = parts.at(use.edge);
            if (use.reversed) {
              changed.push_back({ids[1], true});
              changed.push_back({ids[0], true});
            }
            else {
              changed.push_back({ids[0], false});
              changed.push_back({ids[1], false});
            }
          }
        }
        loop = std::move(changed);
      };
      remap(f.outer);
      for (auto &hole : f.holes)
        remap(hole);
    }
    result.faces = std::move(faces);
    std::erase_if(result.edges, [&](const Edge &e) { return parts.contains(e.id); });
    if (!validate(result, &message))
      throw std::runtime_error(message);
    return result;
  }
  catch (const std::exception &exception) {
    if (error)
      *error = exception.what();
    return {};
  }
}

}  // namespace blender::solid
