/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "SOL_geometry.hh"
#include "fillet_corner_grid.hh"
#include <map>
#include <utility>
#include <vector>

namespace blender::solid {
/** Three rectangular grids meet at the corner center. Boundary nodes are reused verbatim so
 * the corner remains sewn to the adjacent fillet strips. Odd counts leave one central triangle. */
template<typename Sample, typename Create>
auto corner_patch_nodes(const int divisions,
                        const CornerTopology topology,
                        Sample sample,
                        Create create)
{
  using Node = decltype(sample(0, 0));
  std::vector<std::vector<Node>> faces;
  if (topology == CornerTopology::Triangles || divisions == 1) {
    for (int i = 0; i < divisions; ++i) {
      for (int j = 0; j < divisions - i; ++j) {
        faces.push_back({sample(i, j), sample(i + 1, j), sample(i, j + 1)});
        if (i + j + 1 < divisions)
          faces.push_back({sample(i + 1, j), sample(i + 1, j + 1), sample(i, j + 1)});
      }
    }
    return faces;
  }
  const FilletCornerGrid shape = fillet_corner_grid(divisions);
  const FilletCornerGrid topology_grid(divisions);
  std::map<int, Node> nodes;
  auto node = [&](int side, int row, int column) {
    const int index = topology_grid.index(side, row, column);
    if (nodes.contains(index))
      return nodes.at(index);
    column = index % (divisions + 1);
    row = (index / (divisions + 1)) % (divisions / 2 + 1);
    side = index / ((divisions + 1) * (divisions / 2 + 1));
    Node result;
    if (row == 0) {
      result = side == 0 ? sample(column, 0) :
               side == 1 ? sample(divisions - column, column) : sample(0, divisions - column);
    }
    else if (column == 0) {
      result = side == 0 ? sample(0, row) :
               side == 1 ? sample(divisions - row, 0) : sample(row, divisions - row);
    }
    else {
      const double r = double(row) * shape.segments / divisions,
                   c = double(column) * shape.segments / divisions;
      const int r0 = int(std::floor(r)), c0 = int(std::floor(c));
      const double u = r - r0, v = c - c0;
      const int r1 = r0 + (u > 1e-12), c1 = c0 + (v > 1e-12);
      Vec3 q = shape.at(side, r0, c0) * ((1 - u) * (1 - v)) +
               shape.at(side, r1, c0) * (u * (1 - v)) +
               shape.at(side, r0, c1) * ((1 - u) * v) +
               shape.at(side, r1, c1) * (u * v);
      result = create(q / length(q));
    }
    nodes.emplace(index, result);
    return result;
  };
  const int half = divisions / 2, odd = divisions % 2;
  for (int side = 0; side < 3; ++side)
    for (int row = 0; row < half; ++row)
      for (int column = 0; column < half + odd; ++column)
        faces.push_back({node(side, row, column), node(side, row, column + 1),
                         node(side, row + 1, column + 1), node(side, row + 1, column)});
  if (odd)
    faces.push_back({node(0, half, half), node(1, half, half), node(2, half, half)});
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
