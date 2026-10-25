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
  double tolerance_scale_bound = 1.0;
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

struct VertexTransform {
  int64_t vertex_id;
  double4x4 matrix;
};

/**
 * Apply affine transforms to selected source identities and their incident geometry. Equal
 * transforms form one request group: whole curves/supports keep their rational representation,
 * linear corners and proven ruled rows admit partial edits. Different groups support individual
 * selection origins. Shared geometry owners must request identical coefficient assignments.
 * Unselected coefficients, p-curves, weights, knots, topology and selection are retained.
 *
 * The input is never modified. Unsupported partial curved/trimmed deformation, inconsistent
 * ownership, non-affine/non-finite matrices and exceeded arithmetic budgets reject atomically.
 * Tolerances of affected components scale conservatively by the largest supplied linear norm
 * (at least one, including the unchanged part) plus twice the reported coefficient roundoff.
 * The error bound covers application of the supplied matrices, not their construction or NURBS
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
 * two support rows have proportional weight profiles. Other partial curved/trimmed edits are
 * rejected; no endpoint-only control-coefficient rule or fitting fallback is inferred.
 *
 * P-curves, rational weights, bases, identities and unreferenced geometry remain unchanged.
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
