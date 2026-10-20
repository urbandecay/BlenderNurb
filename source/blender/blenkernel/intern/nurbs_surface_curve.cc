/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>

#include "MEM_guardedalloc.h"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_surface.hh"
#include "BKE_nurbs_surface_curve.hh"

#include "nurbs_eval.hh"

namespace blender {

bool BKE_nurbs_surface_isocurve(const NurbsSurface &surface,
                                const bool varying_u,
                                const double fixed_parameter,
                                NurbsCurve &r_curve)
{
  if (r_curve.next != nullptr || r_curve.prev != nullptr || r_curve.id != 0 ||
      r_curve.degree != 1 || r_curve.order != 2 || r_curve.dimension != 3 ||
      r_curve.rational != 0 || r_curve.control_vertices != nullptr || r_curve.knots != nullptr ||
      r_curve.control_vertex_count != 0 || r_curve.knot_count != 0 || r_curve.domain[0] != 0.0 ||
      r_curve.domain[1] != 1.0 || !BKE_nurbs_surface_is_valid(surface) ||
      !std::isfinite(fixed_parameter))
  {
    return false;
  }

  const int fixed_control_vertex_count = varying_u ? surface.control_vertex_count_v :
                                                     surface.control_vertex_count_u;
  const int fixed_order = varying_u ? surface.order_v : surface.order_u;
  const int fixed_knot_count = varying_u ? surface.knot_count_v : surface.knot_count_u;
  const double *fixed_knots = varying_u ? surface.knots_v : surface.knots_u;
  const double *fixed_domain = varying_u ? surface.domain_v : surface.domain_u;
  if (fixed_parameter < fixed_domain[0] || fixed_parameter > fixed_domain[1]) {
    return false;
  }

  int fixed_span;
  Vector<double> fixed_basis;
  if (!bke::nurbs::basis_weights(fixed_knots,
                                 fixed_knot_count,
                                 fixed_control_vertex_count,
                                 fixed_order,
                                 fixed_parameter,
                                 fixed_span,
                                 fixed_basis))
  {
    return false;
  }
  const int first_fixed_control_vertex = fixed_span - (fixed_order - 1);
  const int varying_control_vertex_count = varying_u ? surface.control_vertex_count_u :
                                                       surface.control_vertex_count_v;
  const int varying_order = varying_u ? surface.order_u : surface.order_v;
  const int varying_knot_count = varying_u ? surface.knot_count_u : surface.knot_count_v;
  const int varying_degree = varying_u ? surface.degree_u : surface.degree_v;
  const double *varying_knots = varying_u ? surface.knots_u : surface.knots_v;
  const double *varying_domain = varying_u ? surface.domain_u : surface.domain_v;

  Vector<NurbsControlVertex> control_vertices(varying_control_vertex_count);
  for (int varying_index = 0; varying_index < varying_control_vertex_count; varying_index++) {
    double homogeneous_position[3] = {};
    double homogeneous_weight = 0.0;
    for (int fixed_offset = 0; fixed_offset < fixed_order; fixed_offset++) {
      const int fixed_index = first_fixed_control_vertex + fixed_offset;
      const int surface_index = varying_u ?
                                    varying_index * surface.control_vertex_count_v + fixed_index :
                                    fixed_index * surface.control_vertex_count_v + varying_index;
      const NurbsControlVertex &surface_cv = surface.control_vertices[surface_index];
      const double weight = surface.rational != 0 ? surface_cv.weight : 1.0;
      const double basis_weight = fixed_basis[fixed_offset] * weight;
      homogeneous_weight += basis_weight;
      for (int axis = 0; axis < 3; axis++) {
        homogeneous_position[axis] += basis_weight * surface_cv.position[axis];
      }
    }
    if (!std::isfinite(homogeneous_weight) || homogeneous_weight <= 0.0) {
      return false;
    }
    NurbsControlVertex &curve_cv = control_vertices[varying_index];
    curve_cv.weight = surface.rational != 0 ? homogeneous_weight : 1.0;
    for (int axis = 0; axis < 3; axis++) {
      curve_cv.position[axis] = homogeneous_position[axis] / homogeneous_weight;
      if (!std::isfinite(curve_cv.position[axis])) {
        return false;
      }
    }
  }

  NurbsCurve curve;
  curve.degree = varying_degree;
  curve.order = varying_order;
  curve.dimension = 3;
  curve.rational = surface.rational;
  curve.control_vertex_count = varying_control_vertex_count;
  curve.knot_count = varying_knot_count;
  curve.domain[0] = varying_domain[0];
  curve.domain[1] = varying_domain[1];
  curve.control_vertices = MEM_new_array<NurbsControlVertex>(varying_control_vertex_count,
                                                             __func__);
  curve.knots = MEM_new_array<double>(varying_knot_count, __func__);
  for (int i = 0; i < varying_control_vertex_count; i++) {
    curve.control_vertices[i] = control_vertices[i];
  }
  for (int i = 0; i < varying_knot_count; i++) {
    curve.knots[i] = varying_knots[i];
  }
  if (!BKE_nurbs_curve_is_valid(curve)) {
    MEM_delete(curve.control_vertices);
    MEM_delete(curve.knots);
    return false;
  }

  r_curve = curve;
  return true;
}

}  // namespace blender
