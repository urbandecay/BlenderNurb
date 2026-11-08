/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"
#include <cmath>
#include <numbers>
#include <vector>

namespace blender::solid {

/** Circular three-way corner template. This is the numerical construction used by
 * make_cube_corner_adj_vmesh/cubic_subdiv in bmesh_bevel.cc, kept independent of BMesh.
 * Coordinates stay unprojected during subdivision and are snapped to the exact sphere last. */
struct FilletCornerGrid {
  int segments;
  std::vector<Vec3> points;

  explicit FilletCornerGrid(int segments)
      : segments(segments), points(3 * (segments / 2 + 1) * (segments + 1))
  {
  }

  int index(int side, int row, int column) const
  {
    const int half = segments / 2, odd = segments % 2;
    if (!odd && row == half && column == half)
      side = 0;
    else if (row <= half - 1 + odd && column <= half) {
      /* Already canonical. */
    }
    else if (column <= half) {
      side = (side + 2) % 3;
      const int old_row = row;
      row = column;
      column = segments - old_row;
    }
    else {
      side = (side + 1) % 3;
      const int old_row = row;
      row = segments - column;
      column = old_row;
    }
    return (side * (segments / 2 + 1) + row) * (segments + 1) + column;
  }

  Vec3 &at(int side, int row, int column) { return points[index(side, row, column)]; }
  const Vec3 &at(int side, int row, int column) const
  {
    return points[index(side, row, column)];
  }
};

inline Vec3 fillet_corner_profile(int side, int k, int segments)
{
  const double angle = std::numbers::pi * .5 * double(k) / segments;
  std::array<double, 3> q{};
  q[side] = std::cos(angle);
  q[(side + 1) % 3] = std::sin(angle);
  return {q[0], q[1], q[2]};
}

inline FilletCornerGrid subdivide_fillet_corner(FilletCornerGrid in)
{
  const int n = in.segments, half = n / 2;
  FilletCornerGrid out(2 * n);
  for (int side = 0; side < 3; ++side) {
    out.at(side, 0, 0) = in.at(side, 0, 0);
    for (int k = 1; k < n; ++k) {
      const Vec3 q = in.at(side, 0, k);
      out.at(side, 0, 2 * k) = q -
          (in.at(side, 0, k - 1) + in.at(side, 0, k + 1) - q * 2) / 6;
    }
  }
  for (int side = 0; side < 3; ++side)
    for (int k = 1; k < 2 * n; k += 2) {
      const Vec3 q = fillet_corner_profile(side, k, 2 * n);
      out.at(side, 0, k) = q -
          (out.at(side, 0, k - 1) + out.at(side, 0, k + 1) - q * 2) / 6;
    }
  for (int side = 0; side < 3; ++side)
    for (int k = 0; k < n; ++k)
      in.at(side, 0, k) = out.at(side, 0, 2 * k);

  for (int side = 0; side < 3; ++side)
    for (int j = 0; j < half; ++j)
      for (int k = 0; k < half; ++k)
        out.at(side, 2 * j + 1, 2 * k + 1) =
            (in.at(side, j, k) + in.at(side, j, k + 1) +
             in.at(side, j + 1, k) + in.at(side, j + 1, k + 1)) * .25;

  for (int side = 0; side < 3; ++side)
    for (int j = 0; j < half; ++j)
      for (int k = 1; k <= half; ++k)
        out.at(side, 2 * j + 1, 2 * k) =
            (in.at(side, j, k) + in.at(side, j + 1, k) +
             out.at(side, 2 * j + 1, 2 * k - 1) +
             out.at(side, 2 * j + 1, 2 * k + 1)) * .25;

  for (int side = 0; side < 3; ++side)
    for (int j = 1; j < half; ++j)
      for (int k = 0; k < half; ++k)
        out.at(side, 2 * j, 2 * k + 1) =
            (in.at(side, j, k) + in.at(side, j, k + 1) +
             out.at(side, 2 * j - 1, 2 * k + 1) +
             out.at(side, 2 * j + 1, 2 * k + 1)) * .25;

  for (int side = 0; side < 3; ++side)
    for (int j = 1; j < half; ++j)
      for (int k = 1; k <= half; ++k) {
        const Vec3 edges = (out.at(side, 2 * j, 2 * k - 1) +
                            out.at(side, 2 * j, 2 * k + 1) +
                            out.at(side, 2 * j - 1, 2 * k) +
                            out.at(side, 2 * j + 1, 2 * k)) * .25;
        const Vec3 faces = (out.at(side, 2 * j - 1, 2 * k - 1) +
                            out.at(side, 2 * j + 1, 2 * k - 1) +
                            out.at(side, 2 * j - 1, 2 * k + 1) +
                            out.at(side, 2 * j + 1, 2 * k + 1)) * .25;
        out.at(side, 2 * j, 2 * k) = edges - faces * .25 + in.at(side, j, k) * .25;
      }

  Vec3 edges{}, faces{};
  for (int side = 0; side < 3; ++side) {
    edges = edges + out.at(side, n, n - 1);
    faces = faces + out.at(side, n - 1, n - 1) + out.at(side, n - 1, n + 1);
  }
  constexpr double gamma = .065247584;
  out.at(0, n, n) = edges / 3 - faces * (gamma / 6) + in.at(0, half, half) * gamma;
  for (int side = 0; side < 3; ++side)
    for (int k = 0; k <= 2 * n; ++k)
      out.at(side, 0, k) = fillet_corner_profile(side, k, 2 * n);
  return out;
}

inline FilletCornerGrid fillet_corner_grid(int segments)
{
  FilletCornerGrid grid(2);
  for (int side = 0; side < 3; ++side)
    for (int k = 0; k <= 2; ++k)
      grid.at(side, 0, k) = fillet_corner_profile(side, k, 2);
  const double center = (segments > 2 ? 1.4 : 1) / std::sqrt(3.0);
  grid.at(0, 1, 1) = {center, center, center};
  while (grid.segments < segments)
    grid = subdivide_fillet_corner(std::move(grid));
  return grid;
}
}  // namespace blender::solid
