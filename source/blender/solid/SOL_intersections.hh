/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"

namespace blender::solid {

struct IntersectionTolerance {
  double distance = 1e-9;
  double angle = 1e-12;
};

enum class SurfaceIntersectionState { Separated, Curves, Coincident, Uncertain };

struct SurfaceIntersection {
  SurfaceIntersectionState state = SurfaceIntersectionState::Separated;
  /** Unbounded support curves, in the existing Solid parameterization.
   * Face boundaries must trim these before creating topology. */
  std::vector<Curve> curves;
  bool tangent = false;
};

enum class EdgePlaneState { Separated, Points, Coincident, Uncertain };

struct EdgePlaneIntersection {
  EdgePlaneState state = EdgePlaneState::Separated;
  /** Parameters on the original curve, ordered in the edge's traversal direction.
   * Keep both occurrences at a periodic seam; they bound different intervals. */
  std::vector<double> parameters;
  bool tangent = false;
};

/** Native line/conic/cubic/rational-quadratic edge against an unbounded plane.
 * Uses analytic equations and bounded root isolation; does not sample or fit
 * geometry. The face intersection stage must still classify these points
 * against the plane face's trimming loops. */
std::optional<EdgePlaneIntersection> intersect_edge_plane(
    const Model &model,
    const Edge &edge,
    const Surface &plane,
    IntersectionTolerance tolerance = {},
    std::string *error = nullptr);

/** Native support-surface intersection. No display samples or mesh geometry are used.
 * Planes and affine cylinders (including conic extrusion supports) are accepted for
 * plane/plane and plane/cylinder pairs. Other pairs return an explicit error.
 * Uncertain denotes a configuration within tolerance which cannot safely be
 * classified as exactly tangent, coincident, or separated. */
std::optional<SurfaceIntersection> intersect_surfaces(
    const Model &model,
    const Surface &first,
    const Surface &second,
    IntersectionTolerance tolerance = {},
    std::string *error = nullptr);

}  // namespace blender::solid
