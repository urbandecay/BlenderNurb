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

}  // namespace blender::bke::nurbs
