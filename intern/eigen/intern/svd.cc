/* SPDX-FileCopyrightText: 2015 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_eigen
 */

#ifndef __EIGEN3_SVD_C_API_CC__
#define __EIGEN3_SVD_C_API_CC__

/* Eigen gives annoying huge amount of warnings here, silence them! */
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic ignored "-Wlogical-op"
#endif

#ifdef __EIGEN3_SVD_C_API_CC__
/* Quiet warning. */
#endif

#include <algorithm>
#include <cmath>

#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/SVD>

#include "svd.h"

using Eigen::JacobiSVD;

using Eigen::NoQRPreconditioner;

using Eigen::ComputeThinU;
using Eigen::ComputeThinV;

using Eigen::Map;
using Eigen::MatrixXf;
using Eigen::VectorXf;

using Eigen::Matrix4f;

void EIG_svd_square_matrix(const int size, const float *matrix, float *r_U, float *r_S, float *r_V)
{
  /* Since our matrix is squared, we can use thinU/V. */
  unsigned int flags = (r_U ? ComputeThinU : 0) | (r_V ? ComputeThinV : 0);

  /* Blender and Eigen matrices are both column-major. */
  JacobiSVD<MatrixXf, NoQRPreconditioner> svd(Map<MatrixXf>((float *)matrix, size, size), flags);

  if (r_U) {
    Map<MatrixXf>(r_U, size, size) = svd.matrixU();
  }

  if (r_S) {
    Map<VectorXf>(r_S, size) = svd.singularValues();
  }

  if (r_V) {
    Map<MatrixXf>(r_V, size, size) = svd.matrixV();
  }
}

bool EIG_svd_solve_least_squares_double(const int rows,
                                        const int columns,
                                        const int rhs_count,
                                        const double *matrix,
                                        const double *rhs,
                                        const double min_singular_ratio,
                                        double *r_solution,
                                        int *r_rank,
                                        double *r_singular_ratio)
{
  if (r_rank) {
    *r_rank = 0;
  }
  if (r_singular_ratio) {
    *r_singular_ratio = 0;
  }
  if (rows <= 0 || columns <= 0 || rhs_count <= 0 || !matrix || !rhs || !r_solution ||
      !std::isfinite(min_singular_ratio) || min_singular_ratio <= 0 || min_singular_ratio > 1)
  {
    return false;
  }
  using Matrix = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  const Eigen::Map<const Matrix> a(matrix, rows, columns), b(rhs, rows, rhs_count);
  if (!a.allFinite() || !b.allFinite()) {
    return false;
  }
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(a, Eigen::ComputeThinU | Eigen::ComputeThinV);
  if (svd.info() != Eigen::Success) {
    return false;
  }
  /* Diagnose conditioning explicitly instead of allowing solve() to drop small values. */
  svd.setThreshold(0.0);
  const auto singular = svd.singularValues();
  const int rank = int(svd.rank());
  const double ratio = singular[0] > 0 ? singular[singular.size() - 1] / singular[0] : 0;
  if (r_rank) {
    *r_rank = rank;
  }
  if (r_singular_ratio) {
    *r_singular_ratio = ratio;
  }
  if (rank != std::min(rows, columns) || !std::isfinite(ratio) || ratio < min_singular_ratio) {
    return false;
  }
  const Eigen::MatrixXd solution = svd.solve(b);
  if (!solution.allFinite()) {
    return false;
  }
  Eigen::Map<Matrix>(r_solution, columns, rhs_count) = solution;
  return true;
}

#endif /* __EIGEN3_SVD_C_API_CC__ */
