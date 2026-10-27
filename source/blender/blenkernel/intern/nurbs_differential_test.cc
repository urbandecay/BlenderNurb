/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cfenv>
#include <cmath>

#include "BLI_memory_utils.hh"

#include "BKE_nurbs_transform.hh"

#include "nurbs_test_geometry.hh"
#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

static NurbsSurfaceGeometry &first_surface(NurbsGeometry &geometry)
{
  return *static_cast<NurbsSurfaceGeometry *>(geometry.surfaces.first);
}

TEST(nurbs_differential, PlaneFrameAndPointAgreeWithScalarEvaluationIncludingEndpoints)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const auto &surface = first_surface(*source);
  for (const double u : {0.0, 0.25, 1.0}) {
    for (const double v : {0.0, 0.75, 1.0}) {
      const auto result = evaluate_surface_differential(surface, {u, v});
      ASSERT_TRUE(result.has_value());
      ASSERT_TRUE(result->normal.has_value());
      EXPECT_EQ(result->point.position, evaluate_surface(surface, {u, v})->position);
      EXPECT_EQ(result->tangent_u, double3(2, 0, 0));
      EXPECT_EQ(result->tangent_v, double3(0, 2, 0));
      EXPECT_EQ(*result->normal, double3(0, 0, 1));
    }
  }
}

TEST(nurbs_differential, GroupedFramesMatchScalarArithmeticAndRejectInvalidSamples)
{
  for (const bool rational : {false, true}) {
    auto source = rational ? make_arc_sheet(false) : make_planar_sheet();
    ASSERT_NE(source, nullptr);
    const auto &surface = first_surface(*source);
    Vector<SurfaceDifferentialSample> samples;
    for (const double u : {0.0, 0.1, 0.5, 1.0}) {
      for (const double v : {0.0, 0.25, 0.75, 1.0}) {
        for (const auto side : {KnotSide::Left, KnotSide::Right}) {
          samples.append({{u, v}, {side, side}});
        }
      }
    }
    const auto grouped = evaluate_surface_differentials(surface, samples);
    ASSERT_TRUE(grouped);
    ASSERT_EQ(grouped->size(), samples.size());
    for (const int i : samples.index_range()) {
      const auto scalar = evaluate_surface_differential(
          surface, samples[i].parameter, samples[i].limits);
      ASSERT_TRUE(scalar);
      EXPECT_EQ((*grouped)[i].point.position, scalar->point.position);
      EXPECT_EQ((*grouped)[i].point.weight_condition, scalar->point.weight_condition);
      EXPECT_EQ((*grouped)[i].tangent_u, scalar->tangent_u);
      EXPECT_EQ((*grouped)[i].tangent_v, scalar->tangent_v);
      EXPECT_EQ((*grouped)[i].normal, scalar->normal);
    }
    samples.append({{-1, 0}, {}});
    EXPECT_FALSE(evaluate_surface_differentials(surface, samples));
    samples.last() = {{0, 0}, {KnotSide(99), KnotSide::Right}};
    EXPECT_FALSE(evaluate_surface_differentials(surface, samples));
  }
}

TEST(nurbs_differential, RationalCylinderMatchesIndependentMidArcDerivativeAndNormal)
{
  const auto source = make_arc_sheet(false);
  ASSERT_NE(source, nullptr);
  const auto &surface = first_surface(*source);
  const auto result = evaluate_surface_differential(surface, {0.5, 0.25});
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->normal.has_value());
  const double denominator = 0.5 + 0.5 * std::sqrt(0.5);
  EXPECT_NEAR(result->tangent_u.x, -1 / denominator, 1.0e-14);
  EXPECT_NEAR(result->tangent_u.y, 1 / denominator, 1.0e-14);
  EXPECT_NEAR(result->tangent_u.z, 0, 1.0e-14);
  EXPECT_LT(math::distance(result->tangent_v, double3(0, 0, 2)), 1.0e-14);
  EXPECT_NEAR(result->normal->x, std::sqrt(0.5), 1.0e-14);
  EXPECT_NEAR(result->normal->y, std::sqrt(0.5), 1.0e-14);
  EXPECT_NEAR(result->normal->z, 0, 1.0e-14);
}

TEST(nurbs_differential, UniformRationalWeightScalingPreservesFrame)
{
  auto source = make_arc_sheet(false);
  ASSERT_NE(source, nullptr);
  auto &surface = first_surface(*source);
  const auto reference = evaluate_surface_differential(surface, {0.375, 0.625});
  ASSERT_TRUE(reference.has_value());
  for (const double factor : {1.0e200, -7.0}) {
    for (int i = 0; i < surface.points_num; i++) {
      surface.points[i].weight *= factor;
    }
    const auto result = evaluate_surface_differential(surface, {0.375, 0.625});
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->normal.has_value());
    EXPECT_LT(math::distance(result->point.position, reference->point.position), 1.0e-13);
    EXPECT_LT(math::distance(result->tangent_u, reference->tangent_u), 1.0e-13);
    EXPECT_LT(math::distance(result->tangent_v, reference->tangent_v), 1.0e-13);
    EXPECT_LT(math::distance(*result->normal, *reference->normal), 1.0e-13);
  }
}

TEST(nurbs_differential, RecenteredFrameRetainsSmallFeaturesAtLargeTranslation)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 matrix = double4x4::identity();
  matrix.location() = double3(1.0e12 + 0.125, -1.0e12 - 0.25, 1.0e12);
  const auto moved = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(moved.has_value());
  const auto result = evaluate_surface_differential(first_surface(*moved->geometry), {0.3, 0.7});
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->normal.has_value());
  EXPECT_NEAR(result->tangent_u.x, 2, 1.0e-14);
  EXPECT_NEAR(result->tangent_u.y, 0, 1.0e-14);
  EXPECT_NEAR(result->tangent_v.x, 0, 1.0e-14);
  EXPECT_NEAR(result->tangent_v.y, 2, 1.0e-14);
  EXPECT_EQ(*result->normal, double3(0, 0, 1));
}

TEST(nurbs_differential, NormalScalingAvoidsCrossProductOverflowAndUnderflow)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  for (const double scale : {1.0e-180, 1.0e180}) {
    double4x4 matrix = double4x4::identity();
    matrix[0][0] = matrix[1][1] = scale;
    const auto scaled = transform_geometry(*source, matrix, 0);
    ASSERT_TRUE(scaled.has_value());
    const auto result = evaluate_surface_differential(first_surface(*scaled->geometry),
                                                      {0.25, 0.75});
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->normal.has_value());
    EXPECT_EQ(*result->normal, double3(0, 0, 1));
    EXPECT_NEAR(result->tangent_u.x / scale, 2, 1.0e-14);
    EXPECT_NEAR(result->tangent_v.y / scale, 2, 1.0e-14);
  }
}

TEST(nurbs_differential, OneSidedInternalKnotLimitsPreserveCreaseAndDomainEndLimits)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  auto *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
  surface->id = source->next_id++;
  surface->order_u = surface->order_v = 2;
  surface->points_u = 3;
  surface->points_v = 2;
  surface->points_num = 6;
  surface->knots_u_num = 5;
  surface->knots_v_num = 4;
  surface->knots_u = MEM_new_array_uninitialized<double>(5, __func__);
  surface->knots_v = MEM_new_array_uninitialized<double>(4, __func__);
  const double knots_u[] = {0, 0, 0.5, 1, 1}, knots_v[] = {0, 0, 1, 1};
  std::copy_n(knots_u, 5, surface->knots_u);
  std::copy_n(knots_v, 4, surface->knots_v);
  surface->points = MEM_new_array<NurbsControlPoint>(6, __func__);
  const double3 row[] = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}};
  for (int v = 0; v < 2; v++) {
    for (int u = 0; u < 3; u++) {
      const double3 point = row[u] + double3(0, v, 0);
      std::copy_n(&point.x, 3, surface->points[v * 3 + u].position);
    }
  }
  BLI_addtail(&source->surfaces, surface);
  const auto left = evaluate_surface_differential(
      *surface, {0.5, 0.25}, {KnotSide::Left, KnotSide::Right});
  const auto right = evaluate_surface_differential(*surface, {0.5, 0.25});
  ASSERT_TRUE(left.has_value() && right.has_value());
  EXPECT_EQ(left->point.position, right->point.position);
  EXPECT_EQ(*left->normal, double3(0, 0, 1));
  EXPECT_EQ(*right->normal, double3(-1, 0, 0));
  EXPECT_EQ(left->tangent_u, double3(2, 0, 0));
  EXPECT_EQ(right->tangent_u, double3(0, 0, 2));
  const SurfaceDifferentialSample samples[] = {{{0.5, 0.25}, {KnotSide::Left, KnotSide::Right}},
                                               {{0.5, 0.25}, {}}};
  const auto grouped = evaluate_surface_differentials(*surface, {samples, 2});
  ASSERT_TRUE(grouped);
  EXPECT_EQ((*grouped)[0].normal, left->normal);
  EXPECT_EQ((*grouped)[1].normal, right->normal);
  const auto start = evaluate_surface_differential(
      *surface, {0, 0}, {KnotSide::Left, KnotSide::Left});
  const auto end = evaluate_surface_differential(*surface, {1, 1});
  ASSERT_TRUE(start.has_value() && end.has_value());
  EXPECT_EQ(*start->normal, double3(0, 0, 1));
  EXPECT_EQ(*end->normal, double3(-1, 0, 0));
}

TEST(nurbs_differential, ComputedDegenerateFrameIsExplicitWithoutChangingSource)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 matrix = double4x4::identity();
  matrix[1][1] = 0;
  const auto collapsed = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(collapsed.has_value());
  auto &surface = first_surface(*collapsed->geometry);
  const auto result = evaluate_surface_differential(surface, {0.5, 0.5});
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->normal.has_value());
  EXPECT_EQ(result->tangent_u, double3(2, 0, 0));
  EXPECT_EQ(result->tangent_v, double3(0));
  EXPECT_EQ(surface.points_num, 4);
  EXPECT_EQ(surface.points[0].weight, 1);
}

TEST(nurbs_differential, RationalTensorDerivativesMatchIndependentBilinearQuotientRule)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto &surface = first_surface(*source);
  const double3 points[] = {{0, 0, 0}, {2, 0, 1}, {0, 3, -1}, {2, 3, 2}};
  const double weights[] = {1, 2, 3, 5};
  for (int i = 0; i < 4; i++) {
    std::copy_n(&points[i].x, 3, surface.points[i].position);
    surface.points[i].weight = weights[i];
  }
  for (int i = 0; i < 4; i++) {
    surface.knots_u[i] = i < 2 ? 2 : 5;
    surface.knots_v[i] = i < 2 ? -4 : 6;
  }
  const double u = (3.1 - 2) / 3, v = (-0.7 + 4) / 10;
  double denominator = 0, denominator_u = 0, denominator_v = 0;
  double3 numerator(0), numerator_u(0), numerator_v(0);
  for (int i = 0; i < 4; i++) {
    const double bu = i % 2 ? u : 1 - u, bv = i / 2 ? v : 1 - v;
    const double du = (i % 2 ? 1.0 : -1.0) / 3, dv = (i / 2 ? 1.0 : -1.0) / 10;
    denominator += weights[i] * bu * bv;
    denominator_u += weights[i] * du * bv;
    denominator_v += weights[i] * bu * dv;
    numerator += points[i] * (weights[i] * bu * bv);
    numerator_u += points[i] * (weights[i] * du * bv);
    numerator_v += points[i] * (weights[i] * bu * dv);
  }
  const double3 expected_u = (numerator_u * denominator - numerator * denominator_u) /
                             (denominator * denominator);
  const double3 expected_v = (numerator_v * denominator - numerator * denominator_v) /
                             (denominator * denominator);
  const auto result = evaluate_surface_differential(surface, {3.1, -0.7});
  ASSERT_TRUE(result.has_value());
  EXPECT_LT(math::distance(result->point.position, numerator / denominator), 1.0e-13);
  EXPECT_LT(math::distance(result->tangent_u, expected_u), 1.0e-13);
  EXPECT_LT(math::distance(result->tangent_v, expected_v), 1.0e-13);
}

TEST(nurbs_differential, InvalidRequestsAndUnsupportedArithmeticFailExplicitly)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto &surface = first_surface(*source);
  EXPECT_FALSE(evaluate_surface_differential(surface, {-1, 0}));
  EXPECT_FALSE(
      evaluate_surface_differential(surface, {0.5, 0.5}, {KnotSide(99), KnotSide::Right}));
  for (int i = 0; i < 4; i++) {
    surface.points[i].weight = i % 2 ? -1 : 1;
  }
  EXPECT_FALSE(evaluate_surface_differential(surface, {0.5, 0.5}));
  const int saved = std::fegetround();
  BLI_SCOPED_DEFER([&]() { std::fesetround(saved); });
  ASSERT_EQ(std::fesetround(FE_UPWARD), 0);
  EXPECT_FALSE(evaluate_surface_differential(surface, {0, 0}));
}

}  // namespace blender::bke::nurbs::tests
