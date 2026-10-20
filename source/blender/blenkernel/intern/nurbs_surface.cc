/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BKE_nurbs_surface.hh"

#include "nurbs_eval.hh"

namespace blender {

static bool nurbs_surface_knot_data_is_valid(const double *knots,
                                             const int knot_count,
                                             const int control_vertex_count,
                                             const int order,
                                             const double domain[2])
{
  if (order < 2 || control_vertex_count < order ||
      int64_t(knot_count) != int64_t(control_vertex_count) + order - 2 || knots == nullptr ||
      !std::isfinite(domain[0]) || !std::isfinite(domain[1]) || domain[1] <= domain[0])
  {
    return false;
  }
  for (int i = 0; i < knot_count; i++) {
    if (!std::isfinite(knots[i]) || (i > 0 && knots[i] < knots[i - 1])) {
      return false;
    }
  }
  const int degree = order - 1;
  const double knot_domain_start = knots[degree - 1];
  const double knot_domain_end = knots[control_vertex_count - 1];
  return knot_domain_end > knot_domain_start && domain[0] == knot_domain_start &&
         domain[1] == knot_domain_end;
}

bool BKE_nurbs_surface_is_valid(const NurbsSurface &surface)
{
  if (surface.dimension != 3 || surface.degree_u < 1 || surface.degree_v < 1 ||
      surface.degree_u == std::numeric_limits<int>::max() ||
      surface.degree_v == std::numeric_limits<int>::max() ||
      surface.order_u != surface.degree_u + 1 || surface.order_v != surface.degree_v + 1 ||
      (surface.rational != 0 && surface.rational != 1) || surface.control_vertex_count_u <= 0 ||
      surface.control_vertex_count_v <= 0 ||
      int64_t(surface.control_vertex_count_u) * surface.control_vertex_count_v >
          std::numeric_limits<int>::max() ||
      surface.control_vertices == nullptr ||
      !nurbs_surface_knot_data_is_valid(surface.knots_u,
                                        surface.knot_count_u,
                                        surface.control_vertex_count_u,
                                        surface.order_u,
                                        surface.domain_u) ||
      !nurbs_surface_knot_data_is_valid(surface.knots_v,
                                        surface.knot_count_v,
                                        surface.control_vertex_count_v,
                                        surface.order_v,
                                        surface.domain_v))
  {
    return false;
  }
  const int vertex_count = surface.control_vertex_count_u * surface.control_vertex_count_v;
  for (int i = 0; i < vertex_count; i++) {
    const NurbsControlVertex &cv = surface.control_vertices[i];
    if (!std::isfinite(cv.weight) || cv.weight <= 0.0 || !std::isfinite(cv.position[0]) ||
        !std::isfinite(cv.position[1]) || !std::isfinite(cv.position[2]))
    {
      return false;
    }
  }
  return true;
}

bool BKE_nurbs_surface_evaluate(const NurbsSurface &surface,
                                const double u,
                                const double v,
                                double r_position[3])
{
  if (r_position == nullptr || !BKE_nurbs_surface_is_valid(surface) || !std::isfinite(u) ||
      !std::isfinite(v) || u < surface.domain_u[0] || u > surface.domain_u[1] ||
      v < surface.domain_v[0] || v > surface.domain_v[1])
  {
    return false;
  }
  int span_u, span_v;
  Vector<double> basis_u, basis_v;
  if (!bke::nurbs::basis_weights(surface.knots_u,
                                 surface.knot_count_u,
                                 surface.control_vertex_count_u,
                                 surface.order_u,
                                 u,
                                 span_u,
                                 basis_u) ||
      !bke::nurbs::basis_weights(surface.knots_v,
                                 surface.knot_count_v,
                                 surface.control_vertex_count_v,
                                 surface.order_v,
                                 v,
                                 span_v,
                                 basis_v))
  {
    return false;
  }
  double homogeneous[3] = {};
  double weight_sum = 0.0;
  const int first_u = span_u - surface.degree_u;
  const int first_v = span_v - surface.degree_v;
  for (int i = 0; i < surface.order_u; i++) {
    for (int j = 0; j < surface.order_v; j++) {
      const NurbsControlVertex &cv =
          surface.control_vertices[(first_u + i) * surface.control_vertex_count_v + first_v + j];
      const double weight = (surface.rational != 0) ? cv.weight : 1.0;
      const double weighted_basis = basis_u[i] * basis_v[j] * weight;
      weight_sum += weighted_basis;
      for (int axis = 0; axis < 3; axis++) {
        homogeneous[axis] += weighted_basis * cv.position[axis];
      }
    }
  }
  if (!std::isfinite(weight_sum) || weight_sum <= 0.0) {
    return false;
  }
  for (int axis = 0; axis < 3; axis++) {
    r_position[axis] = homogeneous[axis] / weight_sum;
    if (!std::isfinite(r_position[axis])) {
      return false;
    }
  }
  return true;
}

NurbsSurface *BKE_nurbs_surface_copy(const NurbsSurface &surface)
{
  NurbsSurface *copy = MEM_new<NurbsSurface>(__func__);
  *copy = surface;
  copy->next = nullptr;
  copy->prev = nullptr;
  copy->_pad1 = nullptr;
  const int64_t vertex_count_wide = surface.control_vertex_count_u > 0 &&
                                            surface.control_vertex_count_v > 0 ?
                                        int64_t(surface.control_vertex_count_u) *
                                            surface.control_vertex_count_v :
                                        0;
  const int vertex_count = vertex_count_wide <= std::numeric_limits<int>::max() ?
                               int(vertex_count_wide) :
                               0;
  copy->control_vertices = vertex_count > 0 && surface.control_vertices != nullptr ?
                               MEM_new_array<NurbsControlVertex>(vertex_count, __func__) :
                               nullptr;
  copy->knots_u = surface.knot_count_u > 0 && surface.knots_u != nullptr ?
                      MEM_new_array<double>(surface.knot_count_u, __func__) :
                      nullptr;
  copy->knots_v = surface.knot_count_v > 0 && surface.knots_v != nullptr ?
                      MEM_new_array<double>(surface.knot_count_v, __func__) :
                      nullptr;
  if (copy->control_vertices != nullptr) {
    std::copy_n(surface.control_vertices, vertex_count, copy->control_vertices);
  }
  if (copy->knots_u != nullptr) {
    std::copy_n(surface.knots_u, surface.knot_count_u, copy->knots_u);
  }
  if (copy->knots_v != nullptr) {
    std::copy_n(surface.knots_v, surface.knot_count_v, copy->knots_v);
  }
  return copy;
}

void BKE_nurbs_surface_free(NurbsSurface *surface)
{
  if (surface == nullptr) {
    return;
  }
  MEM_delete(surface->control_vertices);
  MEM_delete(surface->knots_u);
  MEM_delete(surface->knots_v);
  MEM_delete(surface);
}

}  // namespace blender
