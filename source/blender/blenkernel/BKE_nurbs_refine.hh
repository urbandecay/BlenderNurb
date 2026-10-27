/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

struct CurveRefinementResult {
  OwnedCurve curve;
  double geometry_error_bound = 0;
};
struct SurfaceRefinementResult {
  OwnedSurface surface;
  double geometry_error_bound = 0;
};
struct BoundedCurvePoint {
  double3 position;
  double error_bound = 0;
};

/** Shape-preserving positive-rational knot insertion and clamped interval extraction.
 * Keep source parameter values, degree and rational construction. No fitting or point sampling.
 * Outward interval arithmetic encloses coefficient construction; positive-basis bounds cover
 * the resulting curve/surface's uniform position deviation from the input representation.
 * Exact binary constructions report zero; any nonzero bound must fit the explicit budget.
 * Original coefficient resources and domains remain unchanged, including on failure.
 */
std::optional<CurveRefinementResult> restrict_curve(const NurbsCurveGeometry &source,
                                                    const double2 &range,
                                                    double max_error,
                                                    std::string *r_error = nullptr);
std::optional<SurfaceRefinementResult> restrict_surface(const NurbsSurfaceGeometry &source,
                                                        const double2 &low,
                                                        const double2 &high,
                                                        double max_error,
                                                        std::string *r_error = nullptr);

/** Exact tensor isoparametric construction, with the same positive-rational error contract.
 * axis names the fixed U (0) or V (1) coordinate. Output identity is unassigned.
 */
std::optional<CurveRefinementResult> surface_isocurve(const NurbsSurfaceGeometry &source,
                                                      int axis,
                                                      double parameter,
                                                      double max_error,
                                                      std::string *r_error = nullptr);

/** Compose one clamped biquadratic rational span with an affine UV segment. The resulting
 * quadratic isocurve or quartic diagonal is exact apart from bounded coefficient roundoff.
 * Its parameter domain is [0, 1], from start to end. No sampled fitting or source mutation. */
std::optional<CurveRefinementResult> surface_parameter_line(const NurbsSurfaceGeometry &source,
                                                            double2 start,
                                                            double2 end,
                                                            double max_error,
                                                            std::string *r_error = nullptr);

/** Enclose evaluation of a source curve at the supplied binary64 parameter. */
std::optional<BoundedCurvePoint> evaluate_curve_bounded(const NurbsCurveGeometry &source,
                                                        double parameter,
                                                        double max_error,
                                                        std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
