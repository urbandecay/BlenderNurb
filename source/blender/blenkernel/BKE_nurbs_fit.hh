/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_index_range.hh"

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

/** A position constraint on the actual curve, independent of its internal coefficients. */
struct CurveFitPoint {
  double parameter;
  double3 position;
};

struct CurveFitOptions {
  /** Absolute measured position residual allowed at each supplied parameter, in source units. */
  double max_point_error = 0.0;
  /** Reject ill-conditioned systems instead of silently regularizing/truncating the basis. */
  double min_singular_value_ratio = 1e-12;
};

struct CurveFitResult {
  OwnedCurve curve;
  double max_point_error = 0.0;
  double max_coefficient_displacement = 0.0;
  /** Addition roundoff only; excludes solving, basis evaluation and target construction. */
  double coefficient_addition_error_bound = 0.0;
  /** Zero if no active system needed solving. */
  double relative_smallest_singular_value = 0.0;
  int rank = 0;
};

/**
 * Solve explicit free-form position constraints in a fixed positive rational basis. Minimize the
 * Euclidean coefficient displacement in the editable coefficient range; every other coefficient,
 * weight, knot, order, domain and curve identity is retained. Input order does not select a shape
 * rule. Duplicate parameters require identical targets. No source or owner is modified.
 *
 * The coefficient range is an internal solver scope, not an exposed CV handle domain. The caller
 * defines anchored points, locality, continuity and analytic shape rules. Exact circles/arcs must
 * use their analytic constraint path; this free-form service is not a circle editing policy.
 * Re-evaluate every supplied constraint and report/reject its measured residual. This is not a
 * certified global approximation/deviation bound or a topology/edge/support compatibility proof.
 * Initially limited to 256 editable coefficients and 512 constraints for the dense SVD solve.
 */
std::optional<CurveFitResult> fit_freeform_curve(const NurbsCurveGeometry &source,
                                                 Span<CurveFitPoint> fit_points,
                                                 IndexRange editable_coefficients,
                                                 const CurveFitOptions &options,
                                                 std::string *r_error = nullptr);

/**
 * Construct a new open, clamped free-form interpolant with caller-specified degree/parameters,
 * unit rational weights and averaged interior knots. Never rebuild an existing curve implicitly.
 * The standalone result has identity zero; its owner must allocate identity before installation.
 * Uses the same residual/conditioning contract above; no automatic parameterization or tolerance.
 */
std::optional<CurveFitResult> interpolate_freeform_curve(Span<CurveFitPoint> fit_points,
                                                         int degree,
                                                         const CurveFitOptions &options,
                                                         std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
