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
#include "BKE_nurbs_curve.hh"

#include "MEM_guardedalloc.h"

#include "testing/testing.h"

namespace blender::bke::tests {

class NurbsCurveSplitTest : public BlenderGTestBase {
 protected:
  static void free_outputs(NurbsCurve &left, NurbsCurve &right)
  {
    MEM_delete(left.control_vertices);
    MEM_delete(left.knots);
    MEM_delete(right.control_vertices);
    MEM_delete(right.knots);
  }

  static void expect_split_matches(const NurbsCurve &source, const double split)
  {
    NurbsCurve left, right;
    ASSERT_TRUE(BKE_nurbs_curve_split(source, split, left, right));
    ASSERT_TRUE(BKE_nurbs_curve_is_valid(left));
    ASSERT_TRUE(BKE_nurbs_curve_is_valid(right));
    EXPECT_NE(left.control_vertices, right.control_vertices);
    EXPECT_NE(left.knots, right.knots);
    EXPECT_EQ(left.id, 0);
    EXPECT_EQ(right.id, 0);
    for (int i = 0; i < left.control_vertex_count; i++) {
      EXPECT_EQ(left.control_vertices[i].id, 0);
      EXPECT_EQ(int(left.control_vertices[i].flag), 0);
      EXPECT_GT(left.control_vertices[i].weight, 0.0);
    }
    for (int i = 0; i < right.control_vertex_count; i++) {
      EXPECT_EQ(right.control_vertices[i].id, 0);
      EXPECT_EQ(int(right.control_vertices[i].flag), 0);
      EXPECT_GT(right.control_vertices[i].weight, 0.0);
    }
    for (int i = 0; i <= 128; i++) {
      const double t = source.domain[0] + (split - source.domain[0]) * (double(i) / 128.0);
      double expected[3], actual[3];
      ASSERT_TRUE(BKE_nurbs_curve_evaluate(source, t, expected));
      ASSERT_TRUE(BKE_nurbs_curve_evaluate(left, t, actual));
      for (int axis = 0; axis < 3; axis++) {
        EXPECT_NEAR(actual[axis], expected[axis], 1e-11);
      }
    }
    for (int i = 0; i <= 128; i++) {
      const double t = split + (source.domain[1] - split) * (double(i) / 128.0);
      double expected[3], actual[3];
      ASSERT_TRUE(BKE_nurbs_curve_evaluate(source, t, expected));
      ASSERT_TRUE(BKE_nurbs_curve_evaluate(right, t, actual));
      for (int axis = 0; axis < 3; axis++) {
        EXPECT_NEAR(actual[axis], expected[axis], 1e-11);
      }
    }
    free_outputs(left, right);
  }
};

TEST_F(NurbsCurveSplitTest, RationalCircle)
{
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_nurbs_add_circle(bmain, "Circle");
  const NurbsCurve &curve = *static_cast<const NurbsCurve *>(geometry->curves.first);
  expect_split_matches(curve, 1.25);
  BKE_main_free(bmain);
}

TEST_F(NurbsCurveSplitTest, ExactLineConstructor)
{
  Main *bmain = BKE_main_new();
  const double start[3] = {-2.0, 1.0, 3.0};
  const double end[3] = {4.0, -5.0, 7.0};
  NurbsGeometry *geometry = BKE_nurbs_add_line(bmain, "Line", start, end);
  ASSERT_NE(geometry, nullptr);
  const NurbsCurve &curve = *static_cast<const NurbsCurve *>(geometry->curves.first);
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(curve));
  EXPECT_EQ(curve.degree, 1);
  EXPECT_EQ(curve.order, 2);
  EXPECT_EQ(curve.control_vertex_count, 2);
  EXPECT_EQ(curve.knot_count, 2);
  EXPECT_DOUBLE_EQ(curve.domain[0], 0.0);
  EXPECT_DOUBLE_EQ(curve.domain[1], 1.0);
  double position[3];
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, 0.0, position));
  for (int axis = 0; axis < 3; axis++) {
    EXPECT_DOUBLE_EQ(position[axis], start[axis]);
  }
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, 1.0, position));
  for (int axis = 0; axis < 3; axis++) {
    EXPECT_DOUBLE_EQ(position[axis], end[axis]);
  }
  BKE_main_free(bmain);
}

TEST_F(NurbsCurveSplitTest, ExactRationalArcConstructor)
{
  Main *bmain = BKE_main_new();
  const double pi = std::acos(-1.0);
  NurbsGeometry *geometry = BKE_nurbs_add_arc(bmain, "Arc", 2.5, pi * 0.25, pi);
  ASSERT_NE(geometry, nullptr);
  const NurbsCurve &curve = *static_cast<const NurbsCurve *>(geometry->curves.first);
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(curve));
  EXPECT_EQ(curve.degree, 2);
  EXPECT_EQ(curve.order, 3);
  EXPECT_TRUE(curve.rational);
  EXPECT_EQ(curve.control_vertex_count, 5);
  EXPECT_EQ(curve.knot_count, 6);
  EXPECT_DOUBLE_EQ(curve.domain[0], 0.0);
  EXPECT_DOUBLE_EQ(curve.domain[1], 2.0);
  EXPECT_NEAR(curve.control_vertices[1].weight, std::sqrt(0.5), 1e-14);
  EXPECT_NEAR(curve.control_vertices[3].weight, std::sqrt(0.5), 1e-14);
  for (int sample = 0; sample <= 256; sample++) {
    const double parameter = curve.domain[0] + (curve.domain[1] - curve.domain[0]) *
                                                    (double(sample) / 256.0);
    double position[3];
    ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, parameter, position));
    EXPECT_NEAR(std::hypot(position[0], position[1]), 2.5, 1e-11);
    EXPECT_NEAR(position[2], 0.0, 1e-13);
  }
  BKE_main_free(bmain);
}

TEST_F(NurbsCurveSplitTest, ArcConstructorRejectsInvalidSweep)
{
  Main *bmain = BKE_main_new();
  EXPECT_EQ(BKE_nurbs_add_arc(bmain, "Zero Arc", 1.0, 0.0, 0.0), nullptr);
  EXPECT_EQ(BKE_nurbs_add_arc(bmain, "Overfull Arc", 1.0, 0.0, 3.0 * std::acos(-1.0)),
            nullptr);
  BKE_main_free(bmain);
}

TEST_F(NurbsCurveSplitTest, UVLinePreservesParameterAndPlanarity)
{
  NurbsControlVertex cvs[2];
  cvs[0].position[0] = 1.0;
  cvs[0].position[1] = 2.0;
  cvs[1].position[0] = 5.0;
  cvs[1].position[1] = 6.0;
  double knots[2] = {-2.0, 3.0};
  NurbsCurve curve;
  curve.dimension = 2;
  curve.control_vertex_count = 2;
  curve.knot_count = 2;
  curve.control_vertices = cvs;
  curve.knots = knots;
  curve.domain[0] = -2.0;
  curve.domain[1] = 3.0;

  NurbsCurve left, right;
  ASSERT_TRUE(BKE_nurbs_curve_split(curve, 0.5, left, right));
  EXPECT_EQ(left.dimension, 2);
  EXPECT_EQ(right.dimension, 2);
  EXPECT_DOUBLE_EQ(left.domain[0], -2.0);
  EXPECT_DOUBLE_EQ(left.domain[1], 0.5);
  EXPECT_DOUBLE_EQ(right.domain[0], 0.5);
  EXPECT_DOUBLE_EQ(right.domain[1], 3.0);
  for (int i = 0; i < left.control_vertex_count; i++) {
    EXPECT_DOUBLE_EQ(left.control_vertices[i].position[2], 0.0);
  }
  for (int i = 0; i < right.control_vertex_count; i++) {
    EXPECT_DOUBLE_EQ(right.control_vertices[i].position[2], 0.0);
  }
  expect_split_matches(curve, 0.5);
  free_outputs(left, right);
}

TEST_F(NurbsCurveSplitTest, NonUnitDomainLine)
{
  NurbsControlVertex cvs[2];
  cvs[1].position[0] = 8.0;
  double knots[2] = {2.0, 5.0};
  NurbsCurve curve;
  curve.control_vertex_count = 2;
  curve.knot_count = 2;
  curve.control_vertices = cvs;
  curve.knots = knots;
  curve.domain[0] = 2.0;
  curve.domain[1] = 5.0;
  expect_split_matches(curve, 3.2);

  NurbsCurve left, right;
  ASSERT_TRUE(BKE_nurbs_curve_split(curve, 3.2, left, right));
  EXPECT_DOUBLE_EQ(left.domain[0], 2.0);
  EXPECT_DOUBLE_EQ(left.domain[1], 3.2);
  EXPECT_DOUBLE_EQ(right.domain[0], 3.2);
  EXPECT_DOUBLE_EQ(right.domain[1], 5.0);
  ASSERT_EQ(left.knot_count, 2);
  ASSERT_EQ(right.knot_count, 2);
  EXPECT_DOUBLE_EQ(left.knots[0], 2.0);
  EXPECT_DOUBLE_EQ(left.knots[1], 3.2);
  EXPECT_DOUBLE_EQ(right.knots[0], 3.2);
  EXPECT_DOUBLE_EQ(right.knots[1], 5.0);
  free_outputs(left, right);
}

TEST_F(NurbsCurveSplitTest, ExistingRepeatedKnot)
{
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_nurbs_add_circle(bmain, "Circle");
  const NurbsCurve &curve = *static_cast<const NurbsCurve *>(geometry->curves.first);
  expect_split_matches(curve, 1.0);

  NurbsCurve left, right;
  ASSERT_TRUE(BKE_nurbs_curve_split(curve, 1.0, left, right));
  EXPECT_DOUBLE_EQ(left.domain[0], 0.0);
  EXPECT_DOUBLE_EQ(left.domain[1], 1.0);
  EXPECT_DOUBLE_EQ(right.domain[0], 1.0);
  EXPECT_DOUBLE_EQ(right.domain[1], 4.0);
  ASSERT_EQ(left.knot_count, 4);
  ASSERT_EQ(right.knot_count, 8);
  const double expected_left_knots[4] = {0.0, 0.0, 1.0, 1.0};
  const double expected_right_knots[8] = {1.0, 1.0, 2.0, 2.0, 3.0, 3.0, 4.0, 4.0};
  for (int i = 0; i < left.knot_count; i++) {
    EXPECT_DOUBLE_EQ(left.knots[i], expected_left_knots[i]);
  }
  for (int i = 0; i < right.knot_count; i++) {
    EXPECT_DOUBLE_EQ(right.knots[i], expected_right_knots[i]);
  }
  free_outputs(left, right);
  BKE_main_free(bmain);
}

TEST_F(NurbsCurveSplitTest, ClosedWrappedEndpointKeepsStoredDomain)
{
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_nurbs_add_circle(bmain, "Circle");
  const NurbsCurve &curve = *static_cast<const NurbsCurve *>(geometry->curves.first);
  ASSERT_EQ(curve.domain[0], 0.0);
  ASSERT_EQ(curve.domain[1], 4.0);
  /* This model represents a closed circle with a repeated endpoint CV rather than a periodic flag.
   * The split near the end exercises that wrapped endpoint while preserving the active domain. */
  expect_split_matches(curve, 3.75);
  NurbsCurve left, right;
  ASSERT_TRUE(BKE_nurbs_curve_split(curve, 3.75, left, right));
  EXPECT_DOUBLE_EQ(left.domain[0], 0.0);
  EXPECT_DOUBLE_EQ(left.domain[1], 3.75);
  EXPECT_DOUBLE_EQ(right.domain[0], 3.75);
  EXPECT_DOUBLE_EQ(right.domain[1], 4.0);
  free_outputs(left, right);
  BKE_main_free(bmain);
}

TEST_F(NurbsCurveSplitTest, UnclampedQuadratic)
{
  NurbsControlVertex cvs[4];
  cvs[1].position[0] = 1.0;
  cvs[1].position[1] = 2.0;
  cvs[2].position[0] = 3.0;
  cvs[2].position[1] = -1.0;
  cvs[3].position[0] = 4.0;
  double knots[5] = {0.0, 1.0, 2.0, 3.0, 4.0};
  NurbsCurve curve;
  curve.degree = 2;
  curve.order = 3;
  curve.control_vertex_count = 4;
  curve.knot_count = 5;
  curve.control_vertices = cvs;
  curve.knots = knots;
  curve.domain[0] = 1.0;
  curve.domain[1] = 3.0;
  expect_split_matches(curve, 2.25);
}

TEST_F(NurbsCurveSplitTest, FullMultiplicityInteriorKnotIsDiscontinuous)
{
  NurbsControlVertex cvs[6];
  cvs[1].position[0] = 1.0;
  cvs[2].position[0] = 2.0;
  cvs[3].position[0] = 10.0;
  cvs[4].position[0] = 11.0;
  cvs[5].position[0] = 12.0;
  double knots[7] = {0.0, 0.0, 1.0, 1.0, 1.0, 2.0, 2.0};
  NurbsCurve curve;
  curve.degree = 2;
  curve.order = 3;
  curve.control_vertex_count = 6;
  curve.knot_count = 7;
  curve.control_vertices = cvs;
  curve.knots = knots;
  curve.domain[0] = 0.0;
  curve.domain[1] = 2.0;

  NurbsCurve left, right;
  ASSERT_TRUE(BKE_nurbs_curve_split(curve, 1.0, left, right));
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(left));
  ASSERT_TRUE(BKE_nurbs_curve_is_valid(right));
  double left_end[3], source_at_split[3], right_start[3];
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(left, 1.0, left_end));
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(curve, 1.0, source_at_split));
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(right, 1.0, right_start));
  EXPECT_DOUBLE_EQ(left_end[0], 2.0);
  EXPECT_DOUBLE_EQ(source_at_split[0], 10.0);
  EXPECT_DOUBLE_EQ(right_start[0], 10.0);
  /* At an order-multiplicity break the source evaluator chooses the right-hand span at the knot.
   * Splitting preserves the left span's endpoint as the left child endpoint and the source's
   * right-continuous value as the right child endpoint. */
  MEM_delete(left.control_vertices);
  MEM_delete(left.knots);
  MEM_delete(right.control_vertices);
  MEM_delete(right.knots);
}

TEST_F(NurbsCurveSplitTest, InvalidParameterLeavesOutputsUnchanged)
{
  NurbsControlVertex cvs[2];
  double knots[2] = {0.0, 1.0};
  NurbsCurve curve;
  curve.control_vertex_count = 2;
  curve.knot_count = 2;
  curve.control_vertices = cvs;
  curve.knots = knots;
  for (const double parameter : {0.0, 1.0, std::numeric_limits<double>::quiet_NaN()}) {
    NurbsCurve left, right;
    EXPECT_FALSE(BKE_nurbs_curve_split(curve, parameter, left, right));
    EXPECT_EQ(left.control_vertices, nullptr);
    EXPECT_EQ(left.knots, nullptr);
    EXPECT_EQ(right.control_vertices, nullptr);
    EXPECT_EQ(right.knots, nullptr);
  }

  NurbsCurve nonempty;
  nonempty.id = 5;
  NurbsCurve other;
  EXPECT_FALSE(BKE_nurbs_curve_split(curve, 0.5, nonempty, other));
  EXPECT_EQ(nonempty.id, 5);
  EXPECT_FALSE(BKE_nurbs_curve_split(curve, 0.5, const_cast<NurbsCurve &>(curve), other));
  NurbsCurve *alias_right = MEM_new<NurbsCurve>(__func__);
  EXPECT_FALSE(BKE_nurbs_curve_split(curve, 0.5, *alias_right, *alias_right));
  MEM_delete(alias_right);
}

}  // namespace blender::bke::tests
