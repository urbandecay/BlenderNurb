/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <array>
#include <limits>

#include "eigen_capi.h"
#include "testing/testing.h"

#include "BLI_math_solvers.h"

namespace blender {

TEST(math_solvers, Tridiagonal1)
{
  const float a[1] = {1};  // ignored
  const float b[1] = {2};
  const float c[1] = {1};  // ignored
  const float d[1] = {4};
  float x[1];

  EXPECT_TRUE(BLI_tridiagonal_solve(a, b, c, d, x, 1));
  EXPECT_FLOAT_EQ(x[0], 2);
}

TEST(math_solvers, Tridiagonal3)
{
  const float a[3] = {1, 2, 3};  // 1 ignored
  const float b[3] = {4, 5, 6};
  const float c[3] = {7, 8, 9};  // 9 ignored
  const float d[3] = {18, 36, 24};
  float x[3];

  EXPECT_TRUE(BLI_tridiagonal_solve(a, b, c, d, x, 3));
  EXPECT_FLOAT_EQ(x[0], 1);
  EXPECT_FLOAT_EQ(x[1], 2);
  EXPECT_FLOAT_EQ(x[2], 3);
}

TEST(math_solvers, CyclicTridiagonal1)
{
  const float a[1] = {1};
  const float b[1] = {2};
  const float c[1] = {1};
  const float d[1] = {4};
  float x[1];

  EXPECT_TRUE(BLI_tridiagonal_solve_cyclic(a, b, c, d, x, 1));
  EXPECT_FLOAT_EQ(x[0], 1);
}

TEST(math_solvers, CyclicTridiagonal2)
{
  const float a[2] = {1, 2};
  const float b[2] = {3, 4};
  const float c[2] = {5, 6};
  const float d[2] = {15, 16};
  float x[2];

  EXPECT_TRUE(BLI_tridiagonal_solve_cyclic(a, b, c, d, x, 2));
  EXPECT_FLOAT_EQ(x[0], 1);
  EXPECT_FLOAT_EQ(x[1], 2);
}

TEST(math_solvers, CyclicTridiagonal3)
{
  const float a[3] = {1, 2, 3};
  const float b[3] = {4, 5, 6};
  const float c[3] = {7, 8, 9};
  const float d[3] = {21, 36, 33};
  float x[3];

  EXPECT_TRUE(BLI_tridiagonal_solve_cyclic(a, b, c, d, x, 3));
  EXPECT_FLOAT_EQ(x[0], 1);
  EXPECT_FLOAT_EQ(x[1], 2);
  EXPECT_FLOAT_EQ(x[2], 3);
}

TEST(math_solvers, DoubleSVDMinimumNormAndMultipleRightHandSides)
{
  const double matrix[] = {1, 2, 1};
  const double rhs[] = {6, -3};
  double result[6];
  int rank = 0;
  double ratio = 0;
  ASSERT_TRUE(
      EIG_svd_solve_least_squares_double(1, 3, 2, matrix, rhs, 1e-12, result, &rank, &ratio));
  EXPECT_EQ(rank, 1);
  EXPECT_EQ(ratio, 1);
  const double expected[] = {1, -0.5, 2, -1, 1, -0.5};
  for (int i = 0; i < 6; i++) {
    EXPECT_NEAR(result[i], expected[i], 1e-14);
  }
}

TEST(math_solvers, DoubleSVDOverdeterminedLeastSquares)
{
  const double matrix[] = {1, 0, 0, 1, 1, 1};
  const double rhs[] = {1, 2, 4};
  double result[2];
  ASSERT_TRUE(
      EIG_svd_solve_least_squares_double(3, 2, 1, matrix, rhs, 1e-12, result, nullptr, nullptr));
  EXPECT_NEAR(result[0], 4.0 / 3, 1e-14);
  EXPECT_NEAR(result[1], 7.0 / 3, 1e-14);
}

TEST(math_solvers, DoubleSVDConditioningRejectsInsteadOfTruncating)
{
  const double matrix[] = {1, 0, 0, 1e-10};
  const double rhs[] = {1, 1e-10};
  double result[] = {7, 8};
  int rank = 0;
  double ratio = 0;
  EXPECT_FALSE(
      EIG_svd_solve_least_squares_double(2, 2, 1, matrix, rhs, 1e-6, result, &rank, &ratio));
  EXPECT_EQ(result[0], 7);
  EXPECT_EQ(result[1], 8);
  EXPECT_EQ(rank, 2);
  EXPECT_EQ(ratio, 1e-10);
  ASSERT_TRUE(
      EIG_svd_solve_least_squares_double(2, 2, 1, matrix, rhs, 1e-12, result, &rank, &ratio));
  EXPECT_EQ(result[0], 1);
  EXPECT_EQ(result[1], 1);
}

TEST(math_solvers, DoubleSVDInvalidDataAndRankFailuresPreserveOutput)
{
  double matrix[] = {1, 1, 1, 1};
  const double rhs[] = {2, 2};
  double result[] = {7, 8};
  EXPECT_FALSE(
      EIG_svd_solve_least_squares_double(2, 2, 1, matrix, rhs, 1e-12, result, nullptr, nullptr));
  matrix[0] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(
      EIG_svd_solve_least_squares_double(2, 2, 1, matrix, rhs, 1e-12, result, nullptr, nullptr));
  EXPECT_FALSE(
      EIG_svd_solve_least_squares_double(0, 2, 1, matrix, rhs, 1e-12, result, nullptr, nullptr));
  EXPECT_EQ(result[0], 7);
  EXPECT_EQ(result[1], 8);
}

}  // namespace blender
