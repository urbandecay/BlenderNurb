/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_cells.hh"
#include "SOL_cylinder_unify.hh"
#include "SOL_partition.hh"
#include "SOL_planar_clip.hh"
#include "SOL_planar_region.hh"
#include "SOL_unify.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace blender::solid {
namespace {
struct Prepared {
  Model target, cutter;
  std::vector<Surface> planes;
  double tolerance;
};
Model face_model(const Model &model, const Face &face)
{
  Model result = model;
  result.faces = {face};
  result.faces.front().group = 0;
  return result;
}
Prepared prepare(const Model &source,
                 std::span<const Id> target_ids,
                 std::span<const Id> cutter_ids)
{
  auto extract = [&](std::span<const Id> ids) {
    Model piece = source;
    piece.faces.clear();
    for (Id id : ids) {
      const Face *f = face(source, id);
      if (!f)
        throw std::runtime_error("Native body references a missing face");
      piece.faces.push_back(*f);
      piece.faces.back().group = 0;
    }
    std::string error;
    auto result = boolean_sew_faces(std::span<const Model>(&piece, 1), 0, true, &error);
    if (!result)
      throw std::runtime_error(error);
    return std::move(*result);
  };
  Prepared prepared;
  prepared.target = extract(target_ids);
  auto cutter = extract(cutter_ids);
  std::string error;
  auto unified = boolean_unify_coplanar_faces(cutter, &error);
  if (!unified)
    throw std::runtime_error(error);
  prepared.cutter = std::move(*unified);
  if (prepared.target.faces.empty() || prepared.cutter.faces.empty())
    throw std::runtime_error("Native convex cutter needs two closed nonempty operands");
  double scale = 1;
  for (const Model *m : {&prepared.target, &prepared.cutter})
    for (const Vertex &v : m->vertices)
      scale = std::max(scale, length(v.position));
  prepared.tolerance = 128 * std::numeric_limits<double>::epsilon() * scale;
  for (const Face &f : prepared.cutter.faces) {
    const Surface &support = *surface(prepared.cutter, f.surface);
    if (support.kind != SurfaceKind::Plane || !f.holes.empty())
      throw std::runtime_error("Native plane partition requires a convex planar cutter");
    for (Coedge use : f.outer)
      if (curve(prepared.cutter, edge(prepared.cutter, use.edge)->curve)->kind != CurveKind::Line)
        throw std::runtime_error("Native convex cutter must have straight outer boundaries");
    Vec3 normal = cross(support.u, support.v);
    normal = normal * (double(f.orientation) / length(normal));
    Surface plane = support;
    plane.v = support.v * double(f.orientation);
    for (const Vertex &v : prepared.cutter.vertices)
      if (dot(v.position - plane.origin, normal) > prepared.tolerance)
        throw std::runtime_error("Native plane partition does not support a concave cutter yet");
    prepared.planes.push_back(plane);
  }
  return prepared;
}
Model selected_partition(const Prepared &prepared, bool inside)
{
  std::string error;
  auto partition = boolean_partition_body(
      prepared.target, prepared.planes, prepared.tolerance, 4096, &error);
  if (!partition)
    throw std::runtime_error(error);
  std::vector<Model> kept;
  for (BooleanCell &cell : *partition) {
    const bool all_inside = std::all_of(
        cell.sides.begin(), cell.sides.end(), [](int side) { return side < 0; });
    if (inside == all_inside)
      kept.push_back(std::move(cell.model));
  }
  auto result = boolean_assemble_cells(kept, prepared.tolerance, &error);
  if (!result)
    throw std::runtime_error(error);
  auto unified = boolean_unify_coplanar_faces(*result, &error);
  if (!unified)
    throw std::runtime_error(error);
  auto cylinders = boolean_unify_cylindrical_faces(*unified, &error);
  if (!cylinders)
    throw std::runtime_error(error);
  return std::move(*cylinders);
}

// Caps on the shared planar interface are already bounded by the closed cells.
// Subtract these certified regions from both boundaries, then sew the remaining
// surfaces. No new intersection geometry is approximated during reconstruction.
Model combine_with_planar_boundary(const Prepared &prepared,
                                   const Model &partition,
                                   const bool reverse_subtract)
{
  std::string error;
  if (partition.faces.empty())
    return prepared.cutter;
  std::map<Id, std::vector<Model>> region_on_partition;
  std::vector<Model> pieces;
  for (const Face &a : prepared.cutter.faces) {
    const Surface &as = *surface(prepared.cutter, a.surface);
    const Vec3 an = cross(as.u, as.v) * double(a.orientation);
    std::vector<Model> regions;
    for (const Face &b : partition.faces) {
      const Surface &bs = *surface(partition, b.surface);
      if (!boolean_same_planar_support(as, bs))
        continue;
      const Vec3 bn = cross(bs.u, bs.v) * double(b.orientation);
      const bool same_sense = dot(an, bn) > 0;
      if (same_sense != reverse_subtract)
        continue;
      auto overlap = boolean_clip_planar_face(
          partition,
          b.id,
          prepared.cutter,
          a.id,
          {prepared.tolerance, 64 * std::numeric_limits<double>::epsilon()},
          &error);
      if (!overlap)
        throw std::runtime_error(error);
      for (const Face &f : overlap->faces) {
        Model region = face_model(*overlap, f);
        regions.push_back(region);
        region_on_partition[b.id].push_back(std::move(region));
      }
    }
    auto remaining = boolean_subtract_planar_regions(
        prepared.cutter, a.id, regions, prepared.tolerance, &error);
    if (!remaining)
      throw std::runtime_error(error);
    if (!remaining->faces.empty())
      pieces.push_back(std::move(*remaining));
  }
  for (const Face &b : partition.faces) {
    const auto found = region_on_partition.find(b.id);
    if (reverse_subtract) {
      // The common solid is inside the planar operand, so every matching cap
      // is entirely on that operand's exterior; its other faces bound cavities.
      if (found != region_on_partition.end())
        continue;
      Model cavity = face_model(partition, b);
      cavity.faces.front().orientation *= -1;
      pieces.push_back(std::move(cavity));
    }
    else if (found == region_on_partition.end())
      pieces.push_back(face_model(partition, b));
    else {
      auto remaining = boolean_subtract_planar_regions(
          partition, b.id, found->second, prepared.tolerance, &error);
      if (!remaining)
        throw std::runtime_error(error);
      if (!remaining->faces.empty())
        pieces.push_back(std::move(*remaining));
    }
  }
  auto result = boolean_sew_faces(pieces, prepared.tolerance, true, &error);
  if (!result)
    throw std::runtime_error(error);
  auto unified = boolean_unify_coplanar_faces(*result, &error);
  if (!unified)
    throw std::runtime_error(error);
  auto cylinders = boolean_unify_cylindrical_faces(*unified, &error);
  if (!cylinders)
    throw std::runtime_error(error);
  return std::move(*cylinders);
}
}  // namespace

std::optional<Model> boolean_with_convex_planar_cutter(const Model &source,
                                                       std::span<const Id> target_faces,
                                                       std::span<const Id> cutter_faces,
                                                       BooleanOperation operation,
                                                       std::string *error)
{
  try {
    const Prepared prepared = prepare(source, target_faces, cutter_faces);
    if (operation == BooleanOperation::Union) {
      const Model outside = selected_partition(prepared, false);
      return combine_with_planar_boundary(prepared, outside, false);
    }
    return selected_partition(prepared, operation == BooleanOperation::Intersect);
  }
  catch (const std::exception &failure) {
    if (error)
      *error = failure.what();
    return {};
  }
}
std::optional<Model> boolean_planar_minus_body(const Model &source,
                                               std::span<const Id> planar_faces,
                                               std::span<const Id> body_faces,
                                               std::string *error)
{
  try {
    const Prepared prepared = prepare(source, body_faces, planar_faces);
    const Model common = selected_partition(prepared, true);
    return combine_with_planar_boundary(prepared, common, true);
  }
  catch (const std::exception &failure) {
    if (error)
      *error = failure.what();
    return {};
  }
}
}  // namespace blender::solid
