/* SPDX-FileCopyrightText: 2015 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_eigen
 */

#ifndef __EIGEN3_SVD_C_API_H__
#define __EIGEN3_SVD_C_API_H__

#ifdef __cplusplus
extern "C" {
#endif

void EIG_svd_square_matrix(
    const int size, const float *matrix, float *r_U, float *r_S, float *r_V);

/**
 * Double rectangular least-squares/minimum-norm solve with row-major A, B and result arrays.
 * A is rows-by-columns, B rows-by-rhs_count, result columns-by-rhs_count. Require full numerical
 * rank and a smallest/largest singular-value ratio at least min_singular_ratio; no truncation or
 * regularization. Rank/ratio outputs are optional diagnostics. Failure leaves result unchanged.
 */
bool EIG_svd_solve_least_squares_double(int rows,
                                        int columns,
                                        int rhs_count,
                                        const double *matrix,
                                        const double *rhs,
                                        double min_singular_ratio,
                                        double *r_solution,
                                        int *r_rank,
                                        double *r_singular_ratio);

#ifdef __cplusplus
}
#endif

#endif /* __EIGEN3_SVD_C_API_H__ */
