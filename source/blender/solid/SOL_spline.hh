/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "SOL_geometry.hh"

namespace blender::solid {

/** Standalone rational B-spline math. These are not yet stored Solid Curve/Surface kinds.
 * Knots are expanded, including repetitions. Degrees must be positive. Interior knot
 * multiplicity is at most degree (continuous geometry); end/exterior knots may repeat
 * degree + 1 times. Non-clamped knot vectors are supported. Weights are mandatory,
 * finite and strictly positive. No periodic wrapping or parameter clamping is implicit. */
struct RationalBSplineCurve {
  int degree = 1;
  std::vector<double> knots;
  std::vector<Vec3> poles;
  std::vector<double> weights;
};

struct RationalBSplineSurface {
  int u_degree = 1, v_degree = 1;
  std::vector<double> u_knots, v_knots;
  size_t u_count = 0, v_count = 0;
  /** Tensor-product control net, index = v * u_count + u. */
  std::vector<Vec3> poles;
  std::vector<double> weights;
};

struct SplineDomain {
  double min = 0, max = 0;
};

struct SplineCurveSample {
  Vec3 position, derivative;
};

struct SplineSurfaceSample {
  Vec3 position, u_derivative, v_derivative;
};

bool validate_spline(const RationalBSplineCurve &spline, std::string *error = nullptr);
bool validate_spline(const RationalBSplineSurface &spline, std::string *error = nullptr);

/** Active domain [knots[degree], knots[pole_count]], available only for valid data. */
std::optional<SplineDomain> spline_domain(const RationalBSplineCurve &spline,
                                         std::string *error = nullptr);
std::optional<std::array<SplineDomain, 2>> spline_domain(
    const RationalBSplineSurface &spline, std::string *error = nullptr);

/** Analytic rational first derivatives in the original knot parameterization.
 * At interior knots use the right-hand span; at the upper domain end use the left-hand
 * span. A repeated knot may have unequal one-sided derivatives. Out-of-domain/nonfinite
 * parameters and results outside finite double range fail explicitly. */
std::optional<SplineCurveSample> evaluate_spline(const RationalBSplineCurve &spline,
                                                double parameter,
                                                std::string *error = nullptr);
std::optional<SplineSurfaceSample> evaluate_spline(const RationalBSplineSurface &spline,
                                                  double u,
                                                  double v,
                                                  std::string *error = nullptr);

}  // namespace blender::solid
