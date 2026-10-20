/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>
#include <limits>

#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_nurbs.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

class NurbsGeometryTest : public BlenderGTestBase {};

TEST_F(NurbsGeometryTest, ExactRationalCircle)
{
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_nurbs_add_circle(bmain, "Circle");
  ASSERT_NE(geometry, nullptr);
  ASSERT_NE(geometry->curves.first, nullptr);

  const NurbsCurve &curve = *static_cast<const NurbsCurve *>(geometry->curves.first);
  EXPECT_EQ(curve.degree, 2);
  EXPECT_EQ(curve.order, 3);
  EXPECT_TRUE(curve.rational);
  EXPECT_EQ(curve.control_vertex_count, 9);
  EXPECT_EQ(curve.knot_count, 10);
  EXPECT_DOUBLE_EQ(curve.domain[0], 0.0);
  EXPECT_DOUBLE_EQ(curve.domain[1], 4.0);
  EXPECT_TRUE(BKE_nurbs_curve_is_valid(curve));

  for (int sample = 0; sample <= 256; sample++) {
    const double parameter = curve.domain[0] +
                             (curve.domain[1] - curve.domain[0]) * (double(sample) / 256.0);
    double position[3];
    ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, parameter, position));
    EXPECT_NEAR(std::hypot(position[0], position[1]), 1.0, 1e-12);
    EXPECT_NEAR(position[2], 0.0, 1e-12);
  }

  double out_of_domain_position[3];
  EXPECT_FALSE(BKE_nurbs_curve_evaluate(curve, curve.domain[0] - 1.0, out_of_domain_position));
  BKE_main_free(bmain);
}

TEST_F(NurbsGeometryTest, DegreeFourPathConstructor)
{
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_nurbs_add_path(bmain, "Path");
  ASSERT_NE(geometry, nullptr);
  ASSERT_NE(geometry->curves.first, nullptr);

  const NurbsCurve &curve = *static_cast<const NurbsCurve *>(geometry->curves.first);
  EXPECT_EQ(curve.degree, 4);
  EXPECT_EQ(curve.order, 5);
  EXPECT_FALSE(curve.rational);
  EXPECT_EQ(curve.control_vertex_count, 5);
  EXPECT_EQ(curve.knot_count, 8);
  EXPECT_DOUBLE_EQ(curve.domain[0], 0.0);
  EXPECT_DOUBLE_EQ(curve.domain[1], 1.0);
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(curve));

  for (int sample = 0; sample <= 8; sample++) {
    const double parameter = double(sample) / 8.0;
    double position[3];
    ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, parameter, position));
    EXPECT_NEAR(position[0], -2.0 + 4.0 * parameter, 1e-12);
    EXPECT_DOUBLE_EQ(position[1], 0.0);
    EXPECT_DOUBLE_EQ(position[2], 0.0);
  }
  BKE_main_free(bmain);
}

TEST_F(NurbsGeometryTest, CurveAndListOwnershipHelpers)
{
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_nurbs_add_circle(bmain, "Circle");
  const NurbsCurve &source = *static_cast<const NurbsCurve *>(geometry->curves.first);

  NurbsCurve *curve_copy = BKE_nurbs_curve_copy(source);
  ASSERT_NE(curve_copy, nullptr);
  EXPECT_NE(curve_copy->control_vertices, source.control_vertices);
  EXPECT_NE(curve_copy->knots, source.knots);
  EXPECT_EQ(curve_copy->id, source.id);
  EXPECT_EQ(curve_copy->degree, source.degree);
  EXPECT_EQ(curve_copy->order, source.order);
  EXPECT_EQ(curve_copy->dimension, source.dimension);
  EXPECT_EQ(curve_copy->rational, source.rational);
  EXPECT_DOUBLE_EQ(curve_copy->domain[0], source.domain[0]);
  EXPECT_DOUBLE_EQ(curve_copy->domain[1], source.domain[1]);
  curve_copy->control_vertices[0].position[0] = 12.0;
  curve_copy->knots[0] = -1.0;
  EXPECT_DOUBLE_EQ(source.control_vertices[0].position[0], 1.0);
  EXPECT_DOUBLE_EQ(source.knots[0], 0.0);
  BKE_nurbs_curve_free(curve_copy);

  ListBaseT<NurbsCurve> copied_curves = {};
  BKE_nurbs_curve_list_copy(copied_curves, geometry->curves);
  ASSERT_NE(copied_curves.first, nullptr);
  const NurbsCurve &list_copy = *static_cast<const NurbsCurve *>(copied_curves.first);
  EXPECT_NE(list_copy.control_vertices, source.control_vertices);
  EXPECT_NE(list_copy.knots, source.knots);
  EXPECT_EQ(BKE_nurbs_curve_list_memory_size(copied_curves),
            sizeof(NurbsCurve) + sizeof(NurbsControlVertex) * source.control_vertex_count +
                sizeof(double) * source.knot_count);
  BKE_nurbs_curve_list_copy(copied_curves, geometry->curves);
  ASSERT_NE(copied_curves.first, nullptr);
  EXPECT_EQ(BKE_nurbs_curve_list_memory_size(copied_curves),
            sizeof(NurbsCurve) + sizeof(NurbsControlVertex) * source.control_vertex_count +
                sizeof(double) * source.knot_count);
  BKE_nurbs_curve_list_free(copied_curves);
  EXPECT_EQ(copied_curves.first, nullptr);
  EXPECT_EQ(copied_curves.last, nullptr);

  BKE_main_free(bmain);
}

TEST_F(NurbsGeometryTest, CurveCopyAcceptsBorrowedArrays)
{
  NurbsControlVertex control_vertices[2];
  control_vertices[1].id = 42;
  control_vertices[1].position[0] = 3.0;
  double knots[2] = {2.0, 5.0};
  NurbsCurve source;
  source.control_vertex_count = 2;
  source.knot_count = 2;
  source.control_vertices = control_vertices;
  source.knots = knots;
  source.domain[0] = 2.0;
  source.domain[1] = 5.0;

  NurbsCurve *copy = BKE_nurbs_curve_copy(source);
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->control_vertices, source.control_vertices);
  ASSERT_NE(copy->knots, source.knots);
  EXPECT_EQ(copy->control_vertices[1].id, 42);
  EXPECT_DOUBLE_EQ(copy->control_vertices[1].position[0], 3.0);
  EXPECT_DOUBLE_EQ(copy->knots[0], 2.0);
  copy->control_vertices[1].position[0] = 7.0;
  copy->knots[0] = -1.0;
  EXPECT_DOUBLE_EQ(source.control_vertices[1].position[0], 3.0);
  EXPECT_DOUBLE_EQ(source.knots[0], 2.0);
  BKE_nurbs_curve_free(copy);
}

TEST_F(NurbsGeometryTest, NonUnitKnotDomain)
{
  NurbsControlVertex control_vertices[2];
  control_vertices[1].position[0] = 2.0;
  control_vertices[1].position[1] = 4.0;
  control_vertices[1].position[2] = 6.0;
  double knots[2] = {2.0, 5.0};
  NurbsCurve curve;
  curve.control_vertex_count = 2;
  curve.knot_count = 2;
  curve.control_vertices = control_vertices;
  curve.knots = knots;
  curve.domain[0] = 2.0;
  curve.domain[1] = 5.0;

  double position[3];
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, 2.0, position));
  EXPECT_DOUBLE_EQ(position[0], 0.0);
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, 3.5, position));
  EXPECT_DOUBLE_EQ(position[0], 1.0);
  EXPECT_DOUBLE_EQ(position[1], 2.0);
  EXPECT_DOUBLE_EQ(position[2], 3.0);
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, 5.0, position));
  EXPECT_DOUBLE_EQ(position[2], 6.0);

  /* A stored domain must describe the knots, without silently remapping parameters. */
  curve.domain[0] = 0.0;
  EXPECT_FALSE(BKE_nurbs_curve_is_valid(curve));
}

TEST_F(NurbsGeometryTest, AffineTransformPreservesCurveData)
{
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_nurbs_add_circle(bmain, "Circle");
  NurbsCurve &curve = *static_cast<NurbsCurve *>(geometry->curves.first);
  const double weight = curve.control_vertices[1].weight;
  const uint64_t curve_id = curve.id;
  const uint64_t control_vertex_id = curve.control_vertices[1].id;
  float4x4 matrix = float4x4::identity();
  matrix[0][0] = 2.0f;
  matrix[1][1] = 3.0f;
  matrix[2][2] = 4.0f;
  matrix.location() = float3(5.0f, 6.0f, 7.0f);

  BKE_nurbs_transform(*geometry, matrix);

  EXPECT_DOUBLE_EQ(curve.control_vertices[1].position[0], 7.0);
  EXPECT_DOUBLE_EQ(curve.control_vertices[1].position[1], 9.0);
  EXPECT_DOUBLE_EQ(curve.control_vertices[1].position[2], 7.0);
  EXPECT_DOUBLE_EQ(curve.control_vertices[1].weight, weight);
  EXPECT_EQ(curve.id, curve_id);
  EXPECT_EQ(curve.control_vertices[1].id, control_vertex_id);
  EXPECT_EQ(curve.degree, 2);
  EXPECT_EQ(curve.order, 3);
  EXPECT_EQ(curve.knot_count, 10);
  EXPECT_DOUBLE_EQ(curve.knots[4], 2.0);
  EXPECT_DOUBLE_EQ(curve.domain[1], 4.0);
  EXPECT_TRUE(BKE_nurbs_curve_is_valid(curve));
  BKE_main_free(bmain);
}

TEST_F(NurbsGeometryTest, InvalidCurveData)
{
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_nurbs_add_circle(bmain, "Circle");
  NurbsCurve &curve = *static_cast<NurbsCurve *>(geometry->curves.first);

  /* Invalid CVs outside the evaluated span must also reject the curve. */
  curve.control_vertices[8].weight = -1.0;
  double position[3];
  EXPECT_FALSE(BKE_nurbs_curve_evaluate(curve, 0.5, position));
  curve.control_vertices[8].weight = 1.0;
  curve.control_vertices[8].position[2] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(BKE_nurbs_curve_is_valid(curve));
  curve.control_vertices[8].position[2] = 0.0;

  curve.dimension = 4;
  EXPECT_FALSE(BKE_nurbs_curve_is_valid(curve));
  curve.dimension = 3;
  curve.order = std::numeric_limits<int>::max();
  EXPECT_FALSE(BKE_nurbs_curve_is_valid(curve));
  curve.order = 3;
  curve.knots[3] = -1.0;
  EXPECT_FALSE(BKE_nurbs_curve_is_valid(curve));
  curve.knots[3] = 1.0;
  EXPECT_TRUE(BKE_nurbs_curve_is_valid(curve));

  for (int i = 0; i < curve.control_vertex_count; i++) {
    curve.control_vertices[i].position[0] = std::numeric_limits<double>::max();
    curve.control_vertices[i].weight = 2.0;
  }
  EXPECT_FALSE(BKE_nurbs_curve_evaluate(curve, 0.5, position));
  BKE_main_free(bmain);
}

TEST_F(NurbsGeometryTest, UVCurveEvaluationRequiresZeroZ)
{
  NurbsControlVertex cvs[2];
  cvs[1].position[0] = 2.0;
  cvs[1].position[1] = 4.0;
  double knots[2] = {-2.0, 3.0};
  NurbsCurve curve;
  curve.dimension = 2;
  curve.control_vertex_count = 2;
  curve.knot_count = 2;
  curve.control_vertices = cvs;
  curve.knots = knots;
  curve.domain[0] = -2.0;
  curve.domain[1] = 3.0;

  double position[3];
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(curve));
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, 0.5, position));
  EXPECT_DOUBLE_EQ(position[0], 1.0);
  EXPECT_DOUBLE_EQ(position[1], 2.0);
  EXPECT_DOUBLE_EQ(position[2], 0.0);
  cvs[1].position[2] = 1e-30;
  EXPECT_FALSE(BKE_nurbs_curve_is_valid(curve));
  EXPECT_FALSE(BKE_nurbs_curve_evaluate(curve, 0.5, position));
}

}  // namespace blender::bke::tests
