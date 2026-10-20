/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_curve.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_vector.hh"

#include "BKE_nurbs.hh"

namespace blender {

namespace {

using HomogeneousPoint = std::array<double, 4>;

double full_knot_at(const NurbsCurve &curve, const int index)
{
  if (index == 0) {
    return curve.knots[0];
  }
  if (index == curve.knot_count + 1) {
    return curve.knots[curve.knot_count - 1];
  }
  return curve.knots[index - 1];
}

bool insert_knot_once(Vector<double> &knots,
                      Vector<HomogeneousPoint> &points,
                      const int degree,
                      const double parameter)
{
  const int n = int(points.size()) - 1;
  int span = degree;
  while (span < n && parameter >= knots[span + 1]) {
    span++;
  }
  int multiplicity = 0;
  for (const double knot : knots) {
    multiplicity += knot == parameter;
  }
  if (multiplicity > degree) {
    return false;
  }

  Vector<HomogeneousPoint> inserted(points.size() + 1);
  for (int i = 0; i <= span - degree; i++) {
    inserted[i] = points[i];
  }
  for (int i = span - multiplicity; i <= n; i++) {
    inserted[i + 1] = points[i];
  }
  for (int i = span - degree + 1; i <= span - multiplicity; i++) {
    const double denominator = knots[i + degree] - knots[i];
    if (denominator <= 0.0) {
      return false;
    }
    const double alpha = (parameter - knots[i]) / denominator;
    for (int axis = 0; axis < 4; axis++) {
      inserted[i][axis] = alpha * points[i][axis] + (1.0 - alpha) * points[i - 1][axis];
    }
  }
  knots.insert(span + 1, parameter);
  points = std::move(inserted);
  return true;
}

bool make_child(const NurbsCurve &source,
                const Vector<HomogeneousPoint> &points,
                const Vector<double> &full_knots,
                const int point_start,
                const int point_count,
                const int knot_start,
                const double domain_start,
                const double domain_end,
                NurbsCurve &r_child)
{
  const int reduced_knot_count = point_count + source.order - 2;
  if (point_count < source.order || knot_start < 0 ||
      knot_start + reduced_knot_count + 2 > int(full_knots.size()))
  {
    return false;
  }
  NurbsCurve child;
  child.degree = source.degree;
  child.order = source.order;
  child.dimension = source.dimension;
  child.rational = source.rational;
  child.control_vertex_count = point_count;
  child.knot_count = reduced_knot_count;
  child.domain[0] = domain_start;
  child.domain[1] = domain_end;
  child.control_vertices = MEM_new_array<NurbsControlVertex>(point_count, __func__);
  child.knots = MEM_new_array<double>(reduced_knot_count, __func__);

  for (int i = 0; i < point_count; i++) {
    const HomogeneousPoint &point = points[point_start + i];
    const double weight = point[3];
    if (!std::isfinite(weight) || weight <= 0.0) {
      MEM_delete(child.control_vertices);
      MEM_delete(child.knots);
      return false;
    }
    NurbsControlVertex &cv = child.control_vertices[i];
    cv.weight = source.rational ? weight : 1.0;
    for (int axis = 0; axis < 3; axis++) {
      cv.position[axis] = point[axis] / weight;
      if (!std::isfinite(cv.position[axis])) {
        MEM_delete(child.control_vertices);
        MEM_delete(child.knots);
        return false;
      }
    }
  }
  for (int i = 0; i < reduced_knot_count; i++) {
    child.knots[i] = full_knots[knot_start + i + 1];
  }
  if (!BKE_nurbs_curve_is_valid(child)) {
    MEM_delete(child.control_vertices);
    MEM_delete(child.knots);
    return false;
  }
  r_child = child;
  return true;
}

}  // namespace

bool BKE_nurbs_curve_split(const NurbsCurve &source,
                           const double parameter,
                           NurbsCurve &r_left,
                           NurbsCurve &r_right)
{
  if (&r_left == &r_right || &source == &r_left || &source == &r_right || r_left.next != nullptr ||
      r_left.prev != nullptr || r_left.id != 0 || r_left.control_vertices != nullptr ||
      r_left.knots != nullptr || r_left.control_vertex_count != 0 || r_left.knot_count != 0 ||
      r_right.next != nullptr || r_right.prev != nullptr || r_right.id != 0 ||
      r_right.control_vertices != nullptr || r_right.knots != nullptr ||
      r_right.control_vertex_count != 0 || r_right.knot_count != 0 ||
      !BKE_nurbs_curve_is_valid(source) || !std::isfinite(parameter) ||
      parameter <= source.domain[0] || parameter >= source.domain[1])
  {
    return false;
  }

  const int degree = source.degree;
  Vector<double> knots(source.knot_count + 2);
  for (int i = 0; i < int(knots.size()); i++) {
    knots[i] = full_knot_at(source, i);
  }
  Vector<HomogeneousPoint> points(source.control_vertex_count);
  for (int i = 0; i < source.control_vertex_count; i++) {
    const NurbsControlVertex &cv = source.control_vertices[i];
    const double weight = source.rational ? cv.weight : 1.0;
    for (int axis = 0; axis < 3; axis++) {
      points[i][axis] = cv.position[axis] * weight;
    }
    points[i][3] = weight;
  }

  int multiplicity = int(std::count(knots.begin(), knots.end(), parameter));
  while (multiplicity < degree + 1) {
    if (!insert_knot_once(knots, points, degree, parameter)) {
      return false;
    }
    multiplicity++;
  }
  const auto first = std::lower_bound(knots.begin(), knots.end(), parameter);
  const auto after_last = std::upper_bound(first, knots.end(), parameter);
  const int block_start = int(first - knots.begin());
  const int block_end = int(after_last - knots.begin()) - 1;
  const int left_point_count = block_end - degree;
  const int right_point_count = int(points.size()) - left_point_count;

  NurbsCurve left, right;
  const bool left_ok = make_child(
      source, points, knots, 0, left_point_count, 0, source.domain[0], parameter, left);
  if (!left_ok) {
    return false;
  }
  const bool right_ok = make_child(source,
                                   points,
                                   knots,
                                   left_point_count,
                                   right_point_count,
                                   block_start,
                                   parameter,
                                   source.domain[1],
                                   right);
  if (!right_ok) {
    MEM_delete(left.control_vertices);
    MEM_delete(left.knots);
    return false;
  }
  r_left = left;
  r_right = right;
  return true;
}

}  // namespace blender
