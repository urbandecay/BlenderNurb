/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "SOL_geometry.hh"
#include <map>
#include <utility>
#include <vector>

namespace blender::solid {
using CornerPatchCoordinate = std::pair<int, int>;
using CornerPatchFace = std::vector<CornerPatchCoordinate>;

/** Build a face layout over a triangular corner patch. A negative coordinate marks the shared
 * center vertex used by the quad-dominant layout for odd subdivision counts. */
inline std::vector<CornerPatchFace> corner_patch_faces(const int divisions,
                                                       const CornerTopology topology)
{
  std::vector<CornerPatchFace> faces;
  if (topology == CornerTopology::Triangles) {
    for (int i = 0; i < divisions; ++i) {
      for (int j = 0; j < divisions - i; ++j) {
        faces.push_back({{i, j}, {i + 1, j}, {i, j + 1}});
        if (i + j + 1 < divisions) {
          faces.push_back({{i + 1, j}, {i + 1, j + 1}, {i, j + 1}});
        }
      }
    }
    return faces;
  }

  if (divisions > 1 && divisions % 2 == 0) {
    /* Tile the triangular grid with four-triangle corner blocks. Each block becomes two quads. */
    for (int i = 0; i < divisions; i += 2) {
      for (int j = 0; j < divisions - i; j += 2) {
        if (i + j + 2 <= divisions) {
          const CornerPatchCoordinate a{i, j}, ab{i + 1, j}, b{i + 2, j},
              bc{i + 1, j + 1}, c{i, j + 2}, ca{i, j + 1};
          faces.push_back({a, ab, b, bc});
          faces.push_back({a, bc, c, ca});
        }
        if (i + j + 4 <= divisions) {
          const CornerPatchCoordinate a{i + 2, j}, ac{i + 2, j + 1}, c{i + 2, j + 2},
              cb{i + 1, j + 2}, b{i, j + 2}, ba{i + 1, j + 1};
          faces.push_back({a, ac, c, cb});
          faces.push_back({a, cb, b, ba});
        }
      }
    }
    return faces;
  }

  if (divisions <= 1) {
    faces.push_back({{0, 0}, {1, 0}, {0, 1}});
    return faces;
  }

  /* An odd number of divisions gives an odd boundary edge count, so one triangle is required.
   * Pair the remaining fan sectors into quads around one shared center vertex. */
  std::vector<CornerPatchCoordinate> boundary;
  boundary.reserve(size_t(3 * divisions));
  for (int i = 0; i <= divisions; ++i) {
    boundary.emplace_back(i, 0);
  }
  for (int i = 1; i <= divisions; ++i) {
    boundary.emplace_back(divisions - i, i);
  }
  for (int j = divisions - 1; j > 0; --j) {
    boundary.emplace_back(0, j);
  }
  constexpr CornerPatchCoordinate center{-1, -1};
  for (size_t i = 0; i + 2 < boundary.size(); i += 2) {
    faces.push_back({center, boundary[i], boundary[i + 1], boundary[i + 2]});
  }
  faces.push_back({center, boundary.back(), boundary.front()});
  return faces;
}

std::optional<EditResult> fillet_trimmed_edges(const Model &model,
                                               const Selection &selection,
                                               const std::map<Id, Vec3> &normals,
                                               const std::map<Id, std::vector<Id>> &uses,
                                               const std::map<Id, std::vector<Id>> &vertex_faces,
                                               double width,
                                               int subdivisions,
                                               CornerTopology corner_topology,
                                               double eps,
                                               std::string *error);
std::optional<EditResult> fillet_vertices(const Model &model,
                                         Selection selection,
                                         double width,
                                         int subdivisions,
                                         std::string *error,
                                         std::string *warning,
                                         CornerTopology corner_topology);
std::optional<EditResult> fillet_wire_vertices(const Model &model,
                                              Selection selection,
                                              double width,
                                              int subdivisions,
                                              std::string *error);
/** Bevel a selected circular cap of an analytic extrusion. Returns handled=false when the
 * selection is not this supported case, so the general bevel path can continue. */
std::optional<EditResult> bevel_circular_cap(const Model &model,
                                             const Selection &selection,
                                             double width,
                                             int subdivisions,
                                             std::string *error,
                                             bool &handled);
}
