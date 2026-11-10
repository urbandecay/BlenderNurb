/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_cells.hh"

#include "SOL_boolean.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace blender::solid {
namespace {

/* The sign is the physical traversal of the canonical sewn edge. Reversing a
 * face reverses both the edge senses and the cyclic order of its boundary. */
using Cycle = std::vector<std::pair<Id, int>>;
using FaceSignature = std::pair<Cycle, std::vector<Cycle>>;

Cycle canonical_cycle(const Cycle &cycle)
{
  Cycle result = cycle;
  for (size_t start = 1; start < cycle.size(); ++start) {
    if (cycle[start] > result.front()) {
      continue;
    }
    Cycle rotated;
    rotated.reserve(cycle.size());
    rotated.insert(rotated.end(), cycle.begin() + start, cycle.end());
    rotated.insert(rotated.end(), cycle.begin(), cycle.begin() + start);
    if (rotated < result) {
      result = std::move(rotated);
    }
  }
  return result;
}

Cycle opposite_cycle(Cycle cycle)
{
  std::reverse(cycle.begin(), cycle.end());
  for (auto &[edge, sense] : cycle) {
    sense = -sense;
  }
  return canonical_cycle(cycle);
}

Cycle physical_cycle(const std::vector<Coedge> &loop, const int orientation)
{
  Cycle cycle;
  cycle.reserve(loop.size());
  for (const Coedge use : loop) {
    cycle.emplace_back(use.edge, orientation * (use.reversed ? -1 : 1));
  }
  if (orientation < 0) {
    std::reverse(cycle.begin(), cycle.end());
  }
  return canonical_cycle(cycle);
}

FaceSignature signature(const Face &face)
{
  FaceSignature result{physical_cycle(face.outer, face.orientation), {}};
  for (const auto &hole : face.holes) {
    result.second.push_back(physical_cycle(hole, face.orientation));
  }
  std::sort(result.second.begin(), result.second.end());
  return result;
}

FaceSignature opposite_signature(FaceSignature signature)
{
  signature.first = opposite_cycle(std::move(signature.first));
  for (Cycle &hole : signature.second) {
    hole = opposite_cycle(std::move(hole));
  }
  std::sort(signature.second.begin(), signature.second.end());
  return signature;
}

FaceSignature region_key(FaceSignature signature)
{
  signature.first = std::min(signature.first, opposite_cycle(signature.first));
  std::set<Cycle> holes;
  for (const Cycle &hole : signature.second) {
    Cycle key = std::min(hole, opposite_cycle(hole));
    if (key == signature.first || !holes.insert(std::move(key)).second) {
      throw std::runtime_error("A native cell face repeats a boundary cycle");
    }
  }
  signature.second.assign(holes.begin(), holes.end());
  return signature;
}

bool identical_support(const Surface &a, const Surface &b)
{
  return a.kind == b.kind && a.origin == b.origin && a.u == b.u && a.v == b.v &&
         a.w == b.w && a.curve == b.curve && a.other_curve == b.other_curve &&
         a.start == b.start && a.end == b.end && a.reverse_first == b.reverse_first &&
         a.reverse_second == b.reverse_second && a.blend == b.blend &&
         a.blend_cot == b.blend_cot && a.blend_v_start == b.blend_v_start &&
         a.blend_v_end == b.blend_v_end;
}

bool same_support(const Surface &a, const Surface &b, const double tolerance)
{
  if (identical_support(a, b)) {
    return true;
  }
  if (a.kind != SurfaceKind::Plane || b.kind != SurfaceKind::Plane) {
    return false;
  }
  const Vec3 an = cross(a.u, a.v), bn = cross(b.u, b.v);
  /* Different planar frames are common on independently built adjacent cells.
   * Do not introduce an angular snapping policy beyond the caller's sewing. */
  if (cross(an, bn) != Vec3{}) {
    return false;
  }
  long double offset = 0;
  for (int i = 0; i < 3; ++i) {
    offset += static_cast<long double>(an[i]) *
              (static_cast<long double>(b.origin[i]) - a.origin[i]);
  }
  return std::isfinite(offset) &&
         std::abs(offset) <= static_cast<long double>(tolerance) * length(an);
}

}  // namespace

std::optional<Model> boolean_assemble_cells(const std::span<const Model> cells,
                                          const double tolerance,
                                          std::string *error)
{
  try {
    if (!std::isfinite(tolerance) || tolerance < 0) {
      throw std::runtime_error("Native cell assembly needs a finite nonnegative tolerance");
    }
    std::vector<size_t> owners;
    std::string message;
    for (size_t cell = 0; cell < cells.size(); ++cell) {
      const Model &source = cells[cell];
      if (source.faces.empty() || !validate(source, &message)) {
        throw std::runtime_error("Native cell assembly needs nonempty valid closed cells: " +
                                 message);
      }
      for (const Face &face : source.faces) {
        if (!validate_body(source, face.id, &message)) {
          throw std::runtime_error("Native cell assembly received an open or misoriented cell: " +
                                   message);
        }
        owners.push_back(cell);
      }
    }
    auto assembled = boolean_sew_faces(cells, tolerance, false, &message);
    if (!assembled) {
      throw std::runtime_error(message);
    }
    if (assembled->faces.size() != owners.size()) {
      throw std::runtime_error("Native cell sewing changed the number of source faces");
    }
    std::vector<FaceSignature> signatures;
    std::map<FaceSignature, std::vector<size_t>> regions;
    for (size_t i = 0; i < assembled->faces.size(); ++i) {
      signatures.push_back(signature(assembled->faces[i]));
      regions[region_key(signatures.back())].push_back(i);
    }
    std::set<Id> interior;
    for (const auto &[key, uses] : regions) {
      if (uses.size() > 2) {
        throw std::runtime_error("More than two native cells use the same boundary face");
      }
      if (uses.size() != 2) {
        continue;
      }
      const size_t first = uses[0], second = uses[1];
      const Face &a = assembled->faces[first], &b = assembled->faces[second];
      if (owners[first] == owners[second]) {
        throw std::runtime_error("A native cell contains duplicate boundary faces");
      }
      if (!same_support(*surface(*assembled, a.surface),
                        *surface(*assembled, b.surface),
                        tolerance))
      {
        throw std::runtime_error("Common native face cycles have different analytic supports");
      }
      if (signatures[first] == signatures[second]) {
        throw std::runtime_error("Duplicate native cell faces have the same physical orientation");
      }
      if (opposite_signature(signatures[first]) != signatures[second]) {
        throw std::runtime_error("Common native cell faces have inconsistent hole orientations");
      }
      interior.insert(a.id);
      interior.insert(b.id);
    }
    std::erase_if(assembled->faces, [&](const Face &face) { return interior.contains(face.id); });
    /* Sewing the remaining boundary also prunes the canceled faces' geometry,
     * while rejecting unmatched subdivisions and nonmanifold edge contacts. */
    auto boundary = boolean_sew_faces(std::span<const Model>(&*assembled, 1),
                                     tolerance,
                                     true,
                                     &message);
    if (!boundary) {
      throw std::runtime_error("Native cell boundary reconstruction failed: " + message);
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
