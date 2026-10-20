/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <limits>

#include "BKE_gtest_base.hh"
#include "BKE_nurbs_surface.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

class NurbsSurfaceTest : public BlenderGTestBase {};

TEST_F(NurbsSurfaceTest, BilinearPlanarSurface)
{
  NurbsControlVertex cvs[4];
  const double positions[4][3] = {{0, 0, 0}, {0, 1, 0}, {2, 0, 0}, {2, 1, 0}};
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

  double position[3];
  ASSERT_TRUE(BKE_nurbs_surface_is_valid(surface));
  ASSERT_TRUE(BKE_nurbs_surface_evaluate(surface, 0.25, 0.75, position));
  EXPECT_DOUBLE_EQ(position[0], 0.5);
  EXPECT_DOUBLE_EQ(position[1], 0.75);
  EXPECT_DOUBLE_EQ(position[2], 0.0);
}

TEST_F(NurbsSurfaceTest, RationalQuarterCylinderAndIndependentDomains)
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

  double position[3];
  ASSERT_TRUE(BKE_nurbs_surface_is_valid(surface));
  ASSERT_TRUE(BKE_nurbs_surface_evaluate(surface, 3.5, 2.0, position));
  EXPECT_NEAR(std::hypot(position[0], position[1]), 1.0, 1e-12);
  EXPECT_NEAR(position[0], std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(position[1], std::sqrt(0.5), 1e-12);
  EXPECT_DOUBLE_EQ(position[2], 5.0);
  EXPECT_FALSE(BKE_nurbs_surface_evaluate(surface, 1.9, 2.0, position));
  EXPECT_FALSE(BKE_nurbs_surface_evaluate(surface, 3.0, 7.1, position));

  NurbsSurface *copy = BKE_nurbs_surface_copy(surface);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy->control_vertices, surface.control_vertices);
  EXPECT_NE(copy->knots_u, surface.knots_u);
  EXPECT_NE(copy->knots_v, surface.knots_v);
  copy->control_vertices[0].position[0] = 19.0;
  copy->knots_u[0] = -20.0;
  EXPECT_DOUBLE_EQ(surface.control_vertices[0].position[0], 1.0);
  EXPECT_DOUBLE_EQ(surface.knots_u[0], 2.0);
  BKE_nurbs_surface_free(copy);
  BKE_nurbs_surface_free(nullptr);
}

TEST_F(NurbsSurfaceTest, InvalidSurfaceData)
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
  double position[3];
  ASSERT_TRUE(BKE_nurbs_surface_is_valid(surface));

  surface.rational = 2;
  EXPECT_FALSE(BKE_nurbs_surface_is_valid(surface));
  surface.rational = 0;
  surface.degree_v = std::numeric_limits<int>::max();
  EXPECT_FALSE(BKE_nurbs_surface_is_valid(surface));
  surface.degree_v = 1;
  cvs[3].position[2] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(BKE_nurbs_surface_evaluate(surface, 0.5, 0.5, position));
  cvs[3].position[2] = 0.0;
  knots[1] = 0.5;
  EXPECT_FALSE(BKE_nurbs_surface_is_valid(surface));
}

}  // namespace blender::bke::tests
