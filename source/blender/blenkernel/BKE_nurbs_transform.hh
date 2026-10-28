/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_math_matrix_types.hh"

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

struct GeometryTransformResult {
  OwnedGeometry geometry;
  double position_error_bound = 0.0;
  /** Additional certified edge/support allowance needed after trimmed-corner deformation,
   * beyond the scaled input tolerance. Bounded by the same explicit operation error budget. */
  double boundary_error_bound = 0.0;
  double tolerance_scale_bound = 1.0;
  /** Whole-face deviation introduced by explicit planar quad reparameterization. */
  double chart_error_bound = 0.0;
  int reparameterized_faces = 0;
};

/**
 * Transform the complete source by a finite affine double matrix, preserving all identities,
 * incidence, basis, weights, parameter domains and selection storage. Transform 3D coefficients,
 * vertices and supports once each; p-curves stay in surface parameter space. No fitting occurs.
 *
 * The explicit absolute error budget bounds coefficient transformation roundoff relative to the
 * supplied matrix, not error in constructing the matrix or evaluating the resulting geometry.
 * Source tolerances scale by a conservative linear norm bound and expand for reported roundoff.
 * Positive 3D rational weights and round-to-nearest/subnormal arithmetic are required.
 * Singular transforms are legal editing states. Face orientation flags and loop traversal remain
 * unchanged, including under reflection; a deliberate orientation correction is a separate edit.
 * This does not implement partial-support deformation or Blender's modal transform adapter.
 */
std::optional<GeometryTransformResult> transform_geometry(const NurbsGeometry &source,
                                                          const double4x4 &matrix,
                                                          double max_position_error,
                                                          std::string *r_error = nullptr);

/** Transform every topology vertex and the curve/surface supports referenced by its topology
 * with one affine matrix. Unreferenced parent supports and UV p-curves remain unchanged. */
std::optional<GeometryTransformResult> transform_all_vertices(const NurbsGeometry &source,
                                                              const double4x4 &matrix,
                                                              double max_position_error,
                                                              std::string *r_error = nullptr);

struct VertexTransform {
  int64_t vertex_id;
  double4x4 matrix;
};

/**
 * Apply affine transforms to selected source identities and their incident geometry. Equal
 * transforms form one request group: whole curves/supports keep their rational representation,
 * linear corners and proven ruled rows admit partial edits. Three-sided polynomial bilinear
 * patches with matching straight edge/p-curve parameterizations admit an affine UV displacement
 * field through their three corner movements; this preserves the support's existing mixed term.
 * Rectangular trims of polynomial bilinear supports interpolate four corner displacements in
 * their actual trim rectangle. Bound and record any additional edge/support discrepancy.
 * Convex nonrectangular straight quads on affine supports first receive private full-corner
 * charts through reparameterize_planar_quads(). Record the whole-face chart error separately
 * and preserve other owners of the old chart. A genuine unsupported support twist rejects.
 * Different groups support individual
 * selection origins. Shared geometry owners must request identical coefficient assignments.
 * Other coefficients, topology and selection are retained. Explicit quad reparameterization
 * allocates private supports/p-curves and changes their basis, weights and parameter domains.
 *
 * The input is never modified. Unsupported partial curved/trimmed deformation, inconsistent
 * ownership, non-affine/non-finite matrices and exceeded arithmetic budgets reject atomically.
 * Tolerances of affected components scale conservatively by the largest supplied linear norm
 * (at least one, including the unchanged part) plus twice the reported coefficient roundoff.
 * Rectangular trim uses also record the new certified boundary correspondence when needed;
 * the additional allowance must fit the explicit error budget and cannot heal an invalid input.
 * The error bound covers application of the supplied matrices and construction/application of
 * triangular/rectangular displacement fields, not construction of the matrices or NURBS
 * evaluation. Singular scales and reflections are legal editing states; face senses are retained.
 */
std::optional<GeometryTransformResult> transform_vertices(const NurbsGeometry &source,
                                                          Span<VertexTransform> transforms,
                                                          double max_position_error,
                                                          std::string *r_error = nullptr);

/**
 * Translate source vertex identities and their incident geometry, preserving shared ownership.
 * Whole edges/supports translate exactly in the NURBS representation. Partial linear edges and
 * complete, clamped tensor faces support corner edits in linear directions and translations of
 * complete boundary rows in ruled directions. Rectangular interior trims admit complete ruled
 * rows when boundary bases, coefficients and projective parameterization match exactly and the
 * two support rows have proportional weight profiles. Triangular polynomial bilinear patches
 * use an affine UV displacement field; rectangular polynomial trims use the bilinear corner rule
 * in transform_vertices(). Unsupported layouts reject without an endpoint-only control-coefficient
 * rule or fitting fallback.
 *
 * Outside explicit planar quad reparameterization, p-curves, rational weights, bases,
 * identities and unreferenced geometry remain unchanged.
 * Shared geometry resources must receive identical coefficient requests from all their owners.
 * The explicit roundoff budget and arithmetic requirements of transform_geometry apply; source
 * tolerances on affected components expand for the reported coefficient/vertex addition bound.
 * Input order and repeated IDs have no effect. The input is never modified, including on failure.
 */
std::optional<GeometryTransformResult> translate_vertices(const NurbsGeometry &source,
                                                          Span<int64_t> vertex_ids,
                                                          const double3 &offset,
                                                          double max_position_error,
                                                          std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
