/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_evaluate.hh"
#include "BLI_math_matrix_types.hh"

namespace blender::bke::nurbs {

struct SurfacePreviewPoint {
  double3 position;
  int64_t face_id;
  double2 parameter;
};

struct SurfacePreviewTriangle {
  /** Boundary points first, followed by interior points; see SurfacePreview::position. */
  int3 points;
  int64_t face_id;
  /** Face-local UVs, including different UVs for shared seam positions. */
  double2 parameters[3];
  /** Display wires for (0, 1), (1, 2), (2, 0). Rectangular charts omit cell diagonals;
   * other trims keep the triangular preview grid. These are never editable source edges. */
  std::array<bool, 3> display_edges;
};

struct SurfacePreview {
  BoundaryEvaluation boundary;
  Vector<SurfacePreviewPoint> interior;
  Vector<SurfacePreviewTriangle> triangles;
  int skipped_repeated_point_triangles = 0;
  Map<int64_t, int> skipped_triangles_by_face;
  /** Faces whose UV triangulation was retained. Identical source supports can also retain their
   * evaluated positions; changed supports are sampled again. */
  int reused_faces = 0;
  double max_interior_weight_condition = 1.0;

  int points_num() const;
  double3 position(int index) const;
};

/** Display-only face anchors shared by overlays and X-Ray box selection. Keep the source
 * boundary-vertex mean when it lies in a preview triangle; otherwise use the nearest triangle
 * centroid. A holed/curved face's dot must not be placed in an empty trim region. No source
 * modification, exact trim-validity claim or geometric center certificate.
 */
Map<int64_t, double3> face_display_centers(const NurbsGeometry &source,
                                           const SurfacePreview &preview);

/** Numerical arc-length midpoint in world space. Integrates rational derivatives over knot
 * spans; independent of display sampling. Returns absent for degenerate/failed evaluations. */
std::optional<double3> curve_length_midpoint(const NurbsCurveGeometry &curve,
                                             double2 interval,
                                             const double4x4 &transform);

struct SurfaceAreaCenter {
  double area;
  double3 centroid;
  /** Closest-point approximation on the exact support, constrained to the sampled trim domain. */
  double3 position;
  double2 parameter;
};

/** Integrate |Su x Sv| and its first moment using adaptive quadrature on a face's preview UV
 * triangles, evaluating the rational source, not its display chords. Trim boundaries have the
 * preview's sampling accuracy. Projects the centroid onto the support with constrained numerical
 * minimization. Neither quadrature nor projection is a certified global error bound. */
std::optional<SurfaceAreaCenter> surface_area_center(const NurbsSurfaceGeometry &surface,
                                                     int64_t face_id,
                                                     const SurfacePreview &preview,
                                                     const double4x4 &transform);

struct SurfacePreviewOptions {
  BoundaryEvaluationOptions boundary;
  /** Subdivide each support knot span inside the sampled trim bounds. Keep interior knots too. */
  int interior_subdivisions = 8;
  int max_points = 1 << 20;
  int max_triangles = 1 << 21;
  /**
   * Positive CDT epsilon after normalizing a face's sampled UV bounds to [-1, 1]. Any actual
   * merge, coordinate movement, intersection or boundary split is rejected, never written back.
   * This controls triangulator proximity checks, not model-space approximation or fitting.
   */
  double normalized_merge_epsilon = 1.0e-10;
  bool operator==(const SurfacePreviewOptions &) const = default;
};

/**
 * Derived, uniformly sampled surface preview with common edge samples and source provenance.
 * Each face has one simple outer sampled loop and zero or more disjoint, unnested holes. Uses
 * Blender's CDT, checking one-to-one input/output boundary correspondence. Rejects unsupported
 * merges, intersections, constraint splits, non-closed UV charts and exhausted output budgets.
 * UV normalization and triangulation never modify the source parameterization or geometry.
 *
 * Boundary evaluation restrictions also apply. This validates the sampled polygons, not exact
 * curved trims between samples. Sampling has no certified world-space approximation bound.
 * No fitting, source topology generation, derivatives/normals, picking or float display
 * conversion. Zero-area model triangles are permitted editing intermediates; repeated point
 * indices are omitted and counted. No manifoldness, smoothness or solidness is implied. The caller
 * retains the immutable source lifetime/token as with evaluate_boundaries.
 */
std::optional<SurfacePreview> tessellate_surfaces(const NurbsGeometry &source,
                                                  const SurfacePreviewOptions &options,
                                                  std::string *r_error = nullptr);

/** Refresh positions while retaining unchanged face UV triangulations. Stable boundary sample
 * identities, face orientations, support knots and preview options are required; changed trims
 * rebuild only their own faces. Boundary evaluations with identical inputs and interior positions
 * on identical supports are reused. Changed inputs and collapsed pole joins are checked for the
 * current source. r_reused means the entire layout was retained; reused_faces
 * also reports partial reuse. The prior preview must belong to the supplied source/options. */
std::optional<SurfacePreview> refresh_surface_preview(
    const NurbsGeometry &source,
    const SurfacePreviewOptions &options,
    const NurbsGeometry &previous_source,
    const SurfacePreviewOptions &previous_options,
    const SurfacePreview &previous,
    bool &r_reused,
    std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
