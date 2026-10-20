/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BKE_gtest_base.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_surface_curve.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

class NurbsSurfaceCurveTest : public BlenderGTestBase {};

static void free_curve_arrays(NurbsCurve &curve)
{
  MEM_delete(curve.control_vertices);
  MEM_delete(curve.knots);
  curve.control_vertices = nullptr;
  curve.knots = nullptr;
}

TEST_F(NurbsSurfaceCurveTest, ExtractExactQuarterCylinderIsocurves)
{
  constexpr double middle_weight = 0.7071067811865475244;
  NurbsControlVertex cvs[6];
  const double circle[3][2] = {{1, 0}, {1, 1}, {0, 1}};
  const double weights[3] = {1.0, middle_weight, 1.0};
  for (int u = 0; u < 3; u++) {
    for (int v = 0; v < 2; v++) {
      NurbsControlVertex &cv = cvs[u * 2 + v];
      cv.position[0] = circle[u][0];
      cv.position[1] = circle[u][1];
      cv.position[2] = v == 0 ? 0.0 : 10.0;
      cv.weight = weights[u];
    }
  }
  double knots_u[4] = {2.0, 2.0, 5.0, 5.0};
  double knots_v[2] = {-3.0, 7.0};
  NurbsSurface surface;
  surface.degree_u = 2;
  surface.order_u = 3;
  surface.control_vertex_count_u = 3;
  surface.knot_count_u = 4;
  surface.domain_u[0] = 2.0;
  surface.domain_u[1] = 5.0;
  surface.control_vertex_count_v = 2;
  surface.knot_count_v = 2;
  surface.domain_v[0] = -3.0;
  surface.domain_v[1] = 7.0;
  surface.rational = 1;
  surface.control_vertices = cvs;
  surface.knots_u = knots_u;
  surface.knots_v = knots_v;

  NurbsCurve iso_u;
  ASSERT_TRUE(BKE_nurbs_surface_isocurve(surface, true, 2.0, iso_u));
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(iso_u));
  EXPECT_EQ(iso_u.degree, 2);
  EXPECT_EQ(iso_u.order, 3);
  EXPECT_EQ(iso_u.dimension, 3);
  EXPECT_EQ(iso_u.rational, 1);
  EXPECT_EQ(iso_u.control_vertex_count, 3);
  EXPECT_EQ(iso_u.knot_count, 4);
  EXPECT_DOUBLE_EQ(iso_u.domain[0], 2.0);
  EXPECT_DOUBLE_EQ(iso_u.domain[1], 5.0);
  for (int i = 0; i < 4; i++) {
    EXPECT_DOUBLE_EQ(iso_u.knots[i], knots_u[i]);
  }
  for (int i = 0; i < 3; i++) {
    EXPECT_DOUBLE_EQ(iso_u.control_vertices[i].weight, weights[i]);
    EXPECT_EQ(iso_u.control_vertices[i].id, 0);
    EXPECT_EQ(int(iso_u.control_vertices[i].flag), 0);
    EXPECT_DOUBLE_EQ(iso_u.control_vertices[i].position[2], 5.0);
  }
  EXPECT_DOUBLE_EQ(iso_u.control_vertices[0].position[0], 1.0);
  EXPECT_DOUBLE_EQ(iso_u.control_vertices[0].position[1], 0.0);
  EXPECT_DOUBLE_EQ(iso_u.control_vertices[1].position[0], 1.0);
  EXPECT_DOUBLE_EQ(iso_u.control_vertices[1].position[1], 1.0);
  EXPECT_DOUBLE_EQ(iso_u.control_vertices[2].position[0], 0.0);
  EXPECT_DOUBLE_EQ(iso_u.control_vertices[2].position[1], 1.0);

  NurbsCurve iso_v;
  ASSERT_TRUE(BKE_nurbs_surface_isocurve(surface, false, 3.5, iso_v));
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(iso_v));
  EXPECT_EQ(iso_v.degree, 1);
  EXPECT_EQ(iso_v.order, 2);
  EXPECT_EQ(iso_v.rational, 1);
  EXPECT_EQ(iso_v.control_vertex_count, 2);
  EXPECT_EQ(iso_v.knot_count, 2);
  EXPECT_DOUBLE_EQ(iso_v.domain[0], -3.0);
  EXPECT_DOUBLE_EQ(iso_v.domain[1], 7.0);
  EXPECT_DOUBLE_EQ(iso_v.knots[0], -3.0);
  EXPECT_DOUBLE_EQ(iso_v.knots[1], 7.0);
  const double generator_weight = 0.5 + 0.5 * middle_weight;
  for (int i = 0; i < 2; i++) {
    EXPECT_NEAR(iso_v.control_vertices[i].position[0], std::sqrt(0.5), 1e-12);
    EXPECT_NEAR(iso_v.control_vertices[i].position[1], std::sqrt(0.5), 1e-12);
    EXPECT_DOUBLE_EQ(iso_v.control_vertices[i].position[2], 10.0 * i);
    EXPECT_NEAR(iso_v.control_vertices[i].weight, generator_weight, 1e-12);
    EXPECT_EQ(iso_v.control_vertices[i].id, 0);
    EXPECT_EQ(int(iso_v.control_vertices[i].flag), 0);
  }
  double point[3];
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(iso_u, 3.5, point));
  EXPECT_NEAR(point[0], std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(point[1], std::sqrt(0.5), 1e-12);
  EXPECT_DOUBLE_EQ(point[2], 5.0);
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(iso_v, 2.0, point));
  EXPECT_NEAR(point[0], std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(point[1], std::sqrt(0.5), 1e-12);
  EXPECT_DOUBLE_EQ(point[2], 5.0);

  free_curve_arrays(iso_u);
  free_curve_arrays(iso_v);
}

TEST_F(NurbsSurfaceCurveTest, ExtractNonRationalBilinearIsocurve)
{
  NurbsControlVertex cvs[4];
  const double positions[4][3] = {{0, 0, 0}, {0, 2, 0}, {4, 0, 2}, {4, 2, 2}};
  for (int i = 0; i < 4; i++) {
    for (int axis = 0; axis < 3; axis++) {
      cvs[i].position[axis] = positions[i][axis];
    }
  }
  double knots[2] = {0.0, 1.0};
  NurbsSurface surface;
  surface.control_vertex_count_u = 2;
  surface.control_vertex_count_v = 2;
  surface.knot_count_u = 2;
  surface.knot_count_v = 2;
  surface.control_vertices = cvs;
  surface.knots_u = knots;
  surface.knots_v = knots;

  NurbsCurve curve;
  ASSERT_TRUE(BKE_nurbs_surface_isocurve(surface, true, 0.25, curve));
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(curve));
  EXPECT_EQ(curve.rational, 0);
  EXPECT_EQ(curve.degree, 1);
  EXPECT_EQ(curve.order, 2);
  EXPECT_EQ(curve.control_vertex_count, 2);
  EXPECT_EQ(curve.knot_count, 2);
  for (int i = 0; i < curve.control_vertex_count; i++) {
    EXPECT_DOUBLE_EQ(curve.control_vertices[i].weight, 1.0);
    EXPECT_DOUBLE_EQ(curve.control_vertices[i].position[1], 0.5);
  }
  EXPECT_DOUBLE_EQ(curve.control_vertices[0].position[0], 0.0);
  EXPECT_DOUBLE_EQ(curve.control_vertices[0].position[2], 0.0);
  EXPECT_DOUBLE_EQ(curve.control_vertices[1].position[0], 4.0);
  EXPECT_DOUBLE_EQ(curve.control_vertices[1].position[2], 2.0);
  EXPECT_DOUBLE_EQ(curve.knots[0], 0.0);
  EXPECT_DOUBLE_EQ(curve.knots[1], 1.0);
  free_curve_arrays(curve);
}

TEST_F(NurbsSurfaceCurveTest, FailureLeavesOutputUnchanged)
{
  NurbsControlVertex cvs[4];
  double knots[2] = {0.0, 1.0};
  NurbsSurface surface;
  surface.control_vertex_count_u = 2;
  surface.control_vertex_count_v = 2;
  surface.knot_count_u = 2;
  surface.knot_count_v = 2;
  surface.control_vertices = cvs;
  surface.knots_u = knots;
  surface.knots_v = knots;

  NurbsCurve output;
  output.id = 42;
  EXPECT_FALSE(BKE_nurbs_surface_isocurve(surface, true, 0.5, output));
  EXPECT_EQ(output.id, 42);
  EXPECT_EQ(output.control_vertices, nullptr);
  EXPECT_EQ(output.knots, nullptr);
  EXPECT_EQ(output.control_vertex_count, 0);

  output = NurbsCurve{};
  output.control_vertices = cvs;
  EXPECT_FALSE(BKE_nurbs_surface_isocurve(surface, true, 0.5, output));
  EXPECT_EQ(output.control_vertices, cvs);
  EXPECT_EQ(output.knots, nullptr);
  output.control_vertices = nullptr;

  output = NurbsCurve{};
  EXPECT_FALSE(BKE_nurbs_surface_isocurve(surface, true, 1.1, output));
  EXPECT_EQ(output.id, 0);
  EXPECT_EQ(output.control_vertices, nullptr);
  EXPECT_EQ(output.knots, nullptr);
  EXPECT_EQ(output.control_vertex_count, 0);

  cvs[3].position[2] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(BKE_nurbs_surface_isocurve(surface, true, 0.5, output));
  EXPECT_EQ(output.control_vertices, nullptr);
  EXPECT_EQ(output.knots, nullptr);
  EXPECT_EQ(output.control_vertex_count, 0);
}

}  // namespace blender::bke::tests
