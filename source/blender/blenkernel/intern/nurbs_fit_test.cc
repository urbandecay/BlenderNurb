/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>

#include "BLI_math_vector.hh"

#include "BKE_nurbs_fit.hh"

#include "nurbs_test_geometry.hh"
#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

static OwnedCurve weighted_bezier()
{
  OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
  curve->id = 42;
  curve->order = curve->points_num = 4;
  curve->knots_num = 8;
  curve->knots = MEM_new_array_zeroed<double>(8, __func__);
  std::fill_n(curve->knots + 4, 4, 1.0);
  curve->points = MEM_new_array<NurbsControlPoint>(4, __func__);
  const double3 positions[] = {{0, 0, 0}, {1, 2, 0}, {3, 2, 1}, {4, 0, 0}};
  const double weights[] = {1, 2, 0.5, 1};
  for (int i = 0; i < 4; i++) {
    std::copy_n(&positions[i].x, 3, curve->points[i].position);
    curve->points[i].weight = weights[i];
  }
  return curve;
}

static Vector<CurveFitPoint> samples(const NurbsCurveGeometry &curve,
                                     const Span<double> parameters)
{
  Vector<CurveFitPoint> points;
  for (const double parameter : parameters) {
    const auto point = evaluate_curve(curve, parameter);
    EXPECT_TRUE(point.has_value());
    if (point) {
      points.append({parameter, point->position});
    }
  }
  return points;
}

static void expect_basis_unchanged(const NurbsCurveGeometry &source,
                                   const NurbsCurveGeometry &curve)
{
  EXPECT_EQ(curve.id, source.id);
  EXPECT_EQ(curve.order, source.order);
  EXPECT_EQ(curve.dimension, source.dimension);
  ASSERT_EQ(curve.points_num, source.points_num);
  ASSERT_EQ(curve.knots_num, source.knots_num);
  EXPECT_EQ(std::memcmp(curve.knots, source.knots, sizeof(double) * source.knots_num), 0);
  for (int i = 0; i < source.points_num; i++) {
    EXPECT_EQ(curve.points[i].weight, source.points[i].weight);
  }
}

TEST(nurbs_fit, RationalBasisMatchesAuthoritativeEvaluation)
{
  const auto curve = weighted_bezier();
  for (int step = 0; step <= 32; step++) {
    const double parameter = step / 32.0;
    std::string error;
    const auto basis = evaluate_curve_basis(*curve, parameter, &error);
    ASSERT_TRUE(basis.has_value()) << error;
    const auto point = evaluate_curve(*curve, parameter, &error);
    ASSERT_TRUE(point.has_value()) << error;
    double3 expected(0);
    double sum = 0;
    for (const int i : basis->values.index_range()) {
      expected += basis->values[i] * double3(curve->points[basis->first_coefficient + i].position);
      sum += basis->values[i];
    }
    EXPECT_NEAR(sum, 1, 1e-15);
    EXPECT_EQ(basis->weight_condition, 1);
    EXPECT_LT(math::distance(expected, point->position), 2e-15);
  }
}

TEST(nurbs_fit, RationalBasisKeepsNonclampedDomainAndRepeatedKnotSides)
{
  NurbsCurveGeometry curve{};
  curve.order = 2;
  curve.points_num = 4;
  curve.knots_num = 6;
  NurbsControlPoint points[4];
  double knots[] = {-4, -2, 0, 0, 3, 8};
  curve.points = points;
  curve.knots = knots;
  const auto left = evaluate_curve_basis(curve, -2);
  const auto middle = evaluate_curve_basis(curve, 0);
  const auto right = evaluate_curve_basis(curve, 3);
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(middle.has_value());
  ASSERT_TRUE(right.has_value());
  EXPECT_EQ(left->first_coefficient, 0);
  EXPECT_EQ(left->values[0], 1);
  EXPECT_EQ(middle->first_coefficient, 2);
  EXPECT_EQ(middle->values[0], 1);
  EXPECT_EQ(right->first_coefficient, 2);
  EXPECT_EQ(right->values[1], 1);
  EXPECT_FALSE(evaluate_curve_basis(curve, -3));
  points[2].weight = 0;
  EXPECT_FALSE(evaluate_curve_basis(curve, 0));
}

TEST(nurbs_fit, FixedRationalBasisMovesOneFitPointAndKeepsOtherAnchors)
{
  const auto source = weighted_bezier();
  auto fit = samples(*source, {0, 0.25, 0.75, 1});
  fit[1].position.z += 0.25;
  const std::array<NurbsControlPoint, 4> before = {
      source->points[0], source->points[1], source->points[2], source->points[3]};
  std::string error;
  const auto result = fit_freeform_curve(*source, fit, IndexRange(1, 2), {1e-12, 1e-12}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  expect_basis_unchanged(*source, *result->curve);
  EXPECT_EQ(std::memcmp(before.data(), source->points, sizeof(before)), 0);
  EXPECT_EQ(std::memcmp(source->points, result->curve->points, sizeof(NurbsControlPoint)), 0);
  EXPECT_EQ(std::memcmp(source->points + 3, result->curve->points + 3, sizeof(NurbsControlPoint)),
            0);
  EXPECT_EQ(result->rank, 2);
  EXPECT_LE(result->max_point_error, 1e-12);
  EXPECT_GT(result->max_coefficient_displacement, 0.25);
  for (const auto &point : fit) {
    const auto actual = evaluate_curve(*result->curve, point.parameter);
    ASSERT_TRUE(actual.has_value());
    EXPECT_LE(math::distance(actual->position, point.position), 1e-12);
  }
}

TEST(nurbs_fit, LocalCoefficientScopeLeavesOtherSpansExactlyUnchanged)
{
  OwnedCurve source(MEM_new<NurbsCurveGeometry>(__func__));
  source->points_num = 7;
  source->order = 4;
  source->knots_num = 11;
  source->points = MEM_new_array<NurbsControlPoint>(7, __func__);
  source->knots = MEM_new_array<double>(11, __func__);
  const double knots[] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2};
  std::copy_n(knots, 11, source->knots);
  for (int i = 0; i < 7; i++) {
    source->points[i].position[0] = i;
    source->points[i].position[1] = i % 2;
  }
  auto fit = samples(*source, {0, 0.25, 0.5, 1, 1.5, 2});
  fit[2].position.z = 0.5;
  std::string error;
  const auto result = fit_freeform_curve(*source, fit, IndexRange(1, 2), {1e-12, 1e-12}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  for (int step = 0; step <= 16; step++) {
    const double parameter = 1 + step / 16.0;
    const auto before = evaluate_curve(*source, parameter);
    const auto after = evaluate_curve(*result->curve, parameter);
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->position, after->position);
  }
  for (int i = 3; i < 7; i++) {
    EXPECT_EQ(
        std::memcmp(source->points + i, result->curve->points + i, sizeof(NurbsControlPoint)), 0);
  }
}

TEST(nurbs_fit, UnderdeterminedEditUsesMinimumCoefficientDisplacement)
{
  NurbsControlPoint points[3];
  double knots[] = {0, 0, 0, 1, 1, 1};
  NurbsCurveGeometry source{};
  source.points = points;
  source.knots = knots;
  source.points_num = source.order = 3;
  source.knots_num = 6;
  std::string error;
  const auto result = fit_freeform_curve(
      source, {{0.5, {0, 0, 1}}}, IndexRange(3), {1e-12, 1e-12}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  // Row [1/4, 1/2, 1/4] has the minimum-norm solution [2/3, 4/3, 2/3].
  EXPECT_NEAR(result->curve->points[0].position[2], 2.0 / 3, 1e-14);
  EXPECT_NEAR(result->curve->points[1].position[2], 4.0 / 3, 1e-14);
  EXPECT_NEAR(result->curve->points[2].position[2], 2.0 / 3, 1e-14);
  EXPECT_EQ(points[1].position[2], 0);
}

TEST(nurbs_fit, NoopRationalArcAndCircleRetainEverySourceCoefficient)
{
  for (const bool closed : {false, true}) {
    const auto geometry = make_arc_sheet(closed);
    ASSERT_NE(geometry, nullptr);
    const auto &source = *static_cast<const NurbsCurveGeometry *>(geometry->curves.first);
    Vector<double> parameters;
    for (int i = 0; i < source.points_num; i++) {
      parameters.append(double(i) * (closed ? 4 : 1) / (source.points_num - 1));
    }
    const auto fit = samples(source, parameters);
    std::string error;
    const auto result = fit_freeform_curve(
        source, fit, IndexRange(source.points_num), {0, 1e-12}, &error);
    ASSERT_TRUE(result.has_value()) << error;
    expect_basis_unchanged(source, *result->curve);
    EXPECT_EQ(std::memcmp(source.points,
                          result->curve->points,
                          sizeof(NurbsControlPoint) * source.points_num),
              0);
    EXPECT_EQ(result->max_point_error, 0);
    EXPECT_EQ(result->max_coefficient_displacement, 0);
  }
}

TEST(nurbs_fit, ConstraintOrderAndIdenticalDuplicatesHaveNoEffect)
{
  const auto source = weighted_bezier();
  auto fit = samples(*source, {0, 0.25, 0.75, 1});
  fit[1].position.z = 0.5;
  std::string error;
  const auto a = fit_freeform_curve(*source, fit, IndexRange(1, 2), {1e-12, 1e-12}, &error);
  ASSERT_TRUE(a.has_value()) << error;
  fit.append(fit[1]);
  std::reverse(fit.begin(), fit.end());
  const auto b = fit_freeform_curve(*source, fit, IndexRange(1, 2), {1e-12, 1e-12}, &error);
  ASSERT_TRUE(b.has_value()) << error;
  EXPECT_EQ(std::memcmp(a->curve->points, b->curve->points, sizeof(NurbsControlPoint) * 4), 0);
  // Add an explicit conflicting target at the same parameter.
  fit.append({fit.first().parameter, fit.first().position + double3(1, 0, 0)});
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(4), {1e-12, 1e-12}, &error));
  EXPECT_NE(error.find("conflicting"), std::string::npos);
}

TEST(nurbs_fit, LockedSupportUnreachableTargetsRejectWithoutMutation)
{
  const auto source = weighted_bezier();
  const auto start = evaluate_curve(*source, 0);
  ASSERT_TRUE(start.has_value());
  std::string error;
  EXPECT_FALSE(fit_freeform_curve(*source,
                                  {{0, start->position + double3(0, 0, 1)}},
                                  IndexRange(1, 2),
                                  {1e-12, 1e-12},
                                  &error));
  EXPECT_NE(error.find("outside"), std::string::npos);
  EXPECT_EQ(source->points[0].position[2], 0);
  EXPECT_FALSE(fit_freeform_curve(
      *source, {{0, start->position + double3(0, 0, 1e-4)}}, IndexRange(), {1e-3, 1e-12}, &error));
  EXPECT_NE(error.find("did not change"), std::string::npos);
}

TEST(nurbs_fit, UnsatisfiablePointResidualAndIllConditioningAreReported)
{
  const auto source = weighted_bezier();
  auto fit = samples(*source, {0.25, 0.5, 0.75});
  fit[1].position.z += 1;
  std::string error;
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(1, 1), {1e-12, 1e-12}, &error));
  EXPECT_NE(error.find("residual"), std::string::npos);
  fit = samples(*source, {0.5, 0.500000000001});
  fit[0].position.z += 1;
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(1, 2), {1e-12, 1e-6}, &error));
  EXPECT_NE(error.find("condition"), std::string::npos);
}

TEST(nurbs_fit, InterpolatingLineIsExactAndHasNoAllocatedIdentity)
{
  const CurveFitPoint points[] = {{-7, {-2, 1, 0}}, {13, {2, 3, 3}}};
  std::string error;
  const auto result = interpolate_freeform_curve(
      Span<CurveFitPoint>(points, std::size(points)), 1, {0, 1e-12}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->curve->id, 0);
  EXPECT_EQ(result->curve->next, nullptr);
  EXPECT_EQ(result->curve->prev, nullptr);
  EXPECT_EQ(result->curve->order, 2);
  EXPECT_EQ(double3(result->curve->points[0].position), points[0].position);
  EXPECT_EQ(double3(result->curve->points[1].position), points[1].position);
  EXPECT_EQ(result->curve->knots[0], -7);
  EXPECT_EQ(result->curve->knots[3], 13);
  EXPECT_EQ(result->max_point_error, 0);
  const CurveFitPoint tiny[] = {{0, {0, 0, 0}}, {1, {0, 0, 1e-200}}};
  const auto precise = interpolate_freeform_curve(
      Span<CurveFitPoint>(tiny, 2), 1, {0, 1e-12}, &error);
  ASSERT_TRUE(precise.has_value()) << error;
  EXPECT_EQ(precise->curve->points[1].position[2], 1e-200);
  EXPECT_NE(precise->curve->points[1].position[2], double(float(1e-200)));
}

TEST(nurbs_fit, NonuniformSpatialInterpolationUsesRequestedDegreeAndDomain)
{
  const CurveFitPoint points[] = {
      {-3, {0, 0, 0}}, {-2, {1, 2, 1}}, {-0.1, {3, -1, 2}}, {2, {4, 1, -2}}, {4.5, {7, 0, 1}}};
  std::string error;
  const auto result = interpolate_freeform_curve(
      Span<CurveFitPoint>(points, std::size(points)), 3, {1e-11, 1e-12}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->curve->order, 4);
  EXPECT_EQ(result->curve->points_num, 5);
  EXPECT_EQ(result->curve->knots[3], -3);
  EXPECT_EQ(result->curve->knots[5], 4.5);
  EXPECT_NEAR(result->curve->knots[4], (-2 - 0.1 + 2) / 3, 1e-15);
  for (const auto &point : points) {
    const auto actual = evaluate_curve(*result->curve, point.parameter);
    ASSERT_TRUE(actual.has_value());
    EXPECT_LE(math::distance(actual->position, point.position), 1e-11);
  }
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(result->curve->points[i].weight, 1);
  }
}

TEST(nurbs_fit, LargeTranslationRetainsSmallSourceFeatures)
{
  const double origin = 0x1p30;
  const CurveFitPoint points[] = {{0, {origin, origin, 0}},
                                  {0.25, {origin + 0.125, origin + 0.25, 0.5}},
                                  {0.75, {origin + 0.25, origin - 0.125, 1}},
                                  {1, {origin + 0.5, origin, 0}}};
  std::string error;
  const auto result = interpolate_freeform_curve(
      Span<CurveFitPoint>(points, std::size(points)), 3, {1e-6, 1e-12}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  for (const auto &point : points) {
    const auto actual = evaluate_curve(*result->curve, point.parameter);
    ASSERT_TRUE(actual.has_value());
    EXPECT_LE(math::distance(actual->position, point.position), 1e-6);
  }
  EXPECT_NE(result->curve->points[1].position[0],
            double(float(result->curve->points[1].position[0])));
}

TEST(nurbs_fit, InvalidWeightsTargetsDimensionsAndOptionsReject)
{
  const auto source = weighted_bezier();
  auto fit = samples(*source, {0, 1});
  std::string error;
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(4), {-1, 1e-12}, &error));
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(4), {1, 0}, &error));
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(8), {1, 1e-12}, &error));
  source->points[1].weight = -1;
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(4), {1, 1e-12}, &error));
  source->points[1].weight = 1;
  fit[0].position.x = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(4), {1, 1e-12}, &error));
  fit[0].position.x = 0;
  fit[0].parameter = -1;
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(4), {1, 1e-12}, &error));
  source->dimension = 2;
  source->points[2].position[2] = 0;
  fit[0] = {0, {0, 0, 1}};
  EXPECT_FALSE(fit_freeform_curve(*source, fit, IndexRange(4), {1, 1e-12}, &error));
}

TEST(nurbs_fit, InvalidInterpolationNeverSilentlyRepairsParametersOrDegree)
{
  const CurveFitPoint points[] = {{0, {0, 0, 0}}, {0, {1, 0, 0}}, {1, {2, 0, 0}}};
  std::string error;
  EXPECT_FALSE(interpolate_freeform_curve(
      Span<CurveFitPoint>(points, std::size(points)), 2, {1e-12, 1e-12}, &error));
  EXPECT_NE(error.find("increasing"), std::string::npos);
  EXPECT_FALSE(interpolate_freeform_curve(
      Span<CurveFitPoint>(points, std::size(points)), 3, {1e-12, 1e-12}, &error));
  EXPECT_FALSE(interpolate_freeform_curve({}, 1, {1e-12, 1e-12}, &error));
}

}  // namespace blender::bke::nurbs::tests
