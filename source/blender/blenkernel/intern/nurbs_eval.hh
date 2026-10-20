/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cmath>
#include <cstdint>

#include "BLI_vector.hh"

namespace blender::bke::nurbs {

/**
 * Compute the active span and its `order` nonzero basis values for a reduced Rhino knot vector.
 * `knots` has `control_vertex_count + order - 2` values. The active CV indices are
 * `r_span - (order - 1)` through `r_span`, inclusive. Returns false for malformed inputs.
 */
inline bool basis_weights(const double *knots,
                          const int knot_count,
                          const int control_vertex_count,
                          const int order,
                          const double parameter,
                          int &r_span,
                          Vector<double> &r_basis)
{
  if (knots == nullptr || order < 2 || control_vertex_count < order ||
      int64_t(knot_count) != int64_t(control_vertex_count) + order - 2 ||
      !std::isfinite(parameter))
  {
    return false;
  }
  for (int i = 0; i < knot_count; i++) {
    if (!std::isfinite(knots[i]) || (i > 0 && knots[i] < knots[i - 1])) {
      return false;
    }
  }
  const int degree = order - 1;
  const auto full_knot_at = [&](const int index) {
    if (index == 0) {
      return knots[0];
    }
    if (index == knot_count + 1) {
      return knots[knot_count - 1];
    }
    return knots[index - 1];
  };
  const double start = full_knot_at(degree);
  const double end = full_knot_at(control_vertex_count);
  if (!std::isfinite(start) || !std::isfinite(end) || parameter < start || parameter > end ||
      end <= start)
  {
    return false;
  }

  const int last_control_vertex = control_vertex_count - 1;
  r_span = degree;
  if (parameter >= end) {
    r_span = last_control_vertex;
  }
  else {
    while (r_span < last_control_vertex && parameter >= full_knot_at(r_span + 1)) {
      r_span++;
    }
  }

  r_basis.clear();
  r_basis.resize(order, 0.0);
  Vector<double> left(order, 0.0), right(order, 0.0);
  r_basis[0] = 1.0;
  for (int j = 1; j <= degree; j++) {
    left[j] = parameter - full_knot_at(r_span + 1 - j);
    right[j] = full_knot_at(r_span + j) - parameter;
    if (!std::isfinite(left[j]) || !std::isfinite(right[j])) {
      return false;
    }
    double saved = 0.0;
    for (int r = 0; r < j; r++) {
      const double denominator = right[r + 1] + left[j - r];
      if (!std::isfinite(denominator)) {
        return false;
      }
      const double term = denominator != 0.0 ? r_basis[r] / denominator : 0.0;
      if (!std::isfinite(term)) {
        return false;
      }
      r_basis[r] = saved + right[r + 1] * term;
      saved = left[j - r] * term;
      if (!std::isfinite(r_basis[r]) || !std::isfinite(saved)) {
        return false;
      }
    }
    r_basis[j] = saved;
    if (!std::isfinite(r_basis[j])) {
      return false;
    }
  }
  return true;
}

}  // namespace blender::bke::nurbs
