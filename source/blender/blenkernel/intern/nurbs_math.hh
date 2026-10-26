/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <optional>

#include "BLI_math_vector_types.hh"

namespace blender::bke::nurbs::detail {

inline bool supported_arithmetic()
{
  static_assert(std::numeric_limits<double>::is_iec559);
  if (std::fegetround() != FE_TONEAREST) {
    return false;
  }
  volatile double minimum = std::numeric_limits<double>::denorm_min();
  volatile double twice_minimum = minimum + minimum;
  const double evaluated = twice_minimum;
  return std::bit_cast<uint64_t>(evaluated) == 2;
}

/** Outward bounds for non-negative values, keeping additions of zero exact. */
inline double add_upper(const double a, const double b)
{
  if (a == 0.0) {
    return b;
  }
  if (b == 0.0) {
    return a;
  }
  return std::nextafter(a + b, std::numeric_limits<double>::infinity());
}

/** TwoSum residual of a finite stored-double addition; callers check the arithmetic mode. */
inline bool sum_with_error(const double a, const double b, double &r_value, double &r_error)
{
  r_value = a + b;
  if (!std::isfinite(r_value)) {
    return false;
  }
  const double b_virtual = r_value - a;
  r_error = (a - (r_value - b_virtual)) + (b - b_virtual);
  return std::isfinite(r_error);
}

/** Exponent of the least non-zero binary digit of a finite, non-zero binary64 value. */
inline int least_bit_exponent(const double value)
{
  const uint64_t bits = std::bit_cast<uint64_t>(value);
  const int encoded_exponent = int((bits >> 52) & 0x7ff);
  uint64_t significand = bits & ((uint64_t(1) << 52) - 1);
  int exponent = -1074;
  if (encoded_exponent != 0) {
    significand |= uint64_t(1) << 52;
    exponent = encoded_exponent - 1023 - 52;
  }
  return exponent + std::countr_zero(significand);
}

/** FMA residual, with a conservative allowance when the exact product residual may underflow. */
inline bool product_with_error(const double a, const double b, double &r_value, double &r_error)
{
  r_value = a * b;
  if (!std::isfinite(r_value)) {
    return false;
  }
  if (a == 0.0 || b == 0.0 || std::abs(a) == 1.0 || std::abs(b) == 1.0) {
    r_error = 0.0;
    return true;
  }
  r_error = std::abs(std::fma(a, b, -r_value));
  /* Product trailing-zero counts add, so this test needs no wide-integer product. If the exact
   * product lies on the subnormal grid, the FMA residual cannot silently round to zero. Otherwise
   * allow for residual underflow, even when the rounded product itself is normal. */
  if (least_bit_exponent(a) + least_bit_exponent(b) < -1074) {
    r_error = add_upper(r_error, std::numeric_limits<double>::denorm_min());
  }
  return std::isfinite(r_error);
}

inline double multiply_upper(const double a, const double b)
{
  if (a == 0.0 || b == 0.0) {
    return 0.0;
  }
  double product, error;
  if (!product_with_error(a, b, product, error)) {
    return std::numeric_limits<double>::infinity();
  }
  return add_upper(product, error);
}

inline double expand_tolerance(const double tolerance, const double extra)
{
  return add_upper(tolerance, extra);
}

/** Stored value and an absolute arithmetic error bound. Inputs are exact source doubles unless
 * an error is supplied. Requires supported_arithmetic(); invalid operations propagate infinity. */
struct BoundedScalar {
  double value = 0;
  double error = 0;

  bool valid() const
  {
    return std::isfinite(value) && std::isfinite(error) && error >= 0;
  }
};

inline BoundedScalar bounded_add(const BoundedScalar a, const BoundedScalar b)
{
  double value, residual;
  if (!a.valid() || !b.valid() || !sum_with_error(a.value, b.value, value, residual)) {
    return {0, std::numeric_limits<double>::infinity()};
  }
  return {value, add_upper(add_upper(a.error, b.error), std::abs(residual))};
}

inline BoundedScalar bounded_subtract(const BoundedScalar a, const BoundedScalar b)
{
  return bounded_add(a, {-b.value, b.error});
}

inline BoundedScalar bounded_multiply(const BoundedScalar a, const BoundedScalar b)
{
  double value, error;
  if (!a.valid() || !b.valid() || !product_with_error(a.value, b.value, value, error)) {
    return {0, std::numeric_limits<double>::infinity()};
  }
  error = add_upper(error, multiply_upper(std::abs(a.value), b.error));
  error = add_upper(error, multiply_upper(std::abs(b.value), a.error));
  error = add_upper(error, multiply_upper(a.error, b.error));
  return {value, error};
}

inline BoundedScalar bounded_divide(const BoundedScalar a, const BoundedScalar b)
{
  if (!a.valid() || !b.valid() || std::abs(b.value) <= b.error) {
    return {0, std::numeric_limits<double>::infinity()};
  }
  const double value = a.value / b.value;
  double product, product_error, residual, residual_error;
  if (!std::isfinite(value) || !product_with_error(value, b.value, product, product_error) ||
      !sum_with_error(a.value, -product, residual, residual_error))
  {
    return {0, std::numeric_limits<double>::infinity()};
  }
  double error = add_upper(std::abs(residual), std::abs(residual_error));
  error = add_upper(error, product_error);
  error = add_upper(error, a.error);
  error = add_upper(error, multiply_upper(std::abs(value), b.error));
  if (error == 0) {
    return {value, 0};
  }
  const double lower = b.error == 0 ? std::abs(b.value) :
                                      std::nextafter(std::abs(b.value) - b.error, 0.0);
  return {value,
          lower > 0 ? std::nextafter(error / lower, std::numeric_limits<double>::infinity()) :
                      std::numeric_limits<double>::infinity()};
}

inline bool translate_position(double position[3], const double3 &offset, double &r_error_bound)
{
  double bound = 0.0;
  for (int axis = 0; axis < 3; axis++) {
    if (offset[axis] == 0.0) {
      continue;
    }
    double sum, residual;
    if (!sum_with_error(position[axis], offset[axis], sum, residual)) {
      return false;
    }
    position[axis] = sum;
    bound = add_upper(bound, std::abs(residual));
  }
  r_error_bound = std::max(r_error_bound, bound);
  return std::isfinite(r_error_bound);
}

/** Fourth coefficient of an affine bilinear chart, with an outward addition roundoff bound.
 * Try the three addition orders to retain exact cancellation whenever it is representable. */
inline std::optional<double3> affine_fourth_corner(const double3 a,
                                                   const double3 b,
                                                   const double3 c,
                                                   double &r_error_bound)
{
  double3 corner;
  r_error_bound = 0;
  for (int axis = 0; axis < 3; axis++) {
    double best_error = std::numeric_limits<double>::infinity();
    const double terms[3][3] = {
        {b[axis], -a[axis], c[axis]}, {c[axis], -a[axis], b[axis]}, {b[axis], c[axis], -a[axis]}};
    for (const auto &order : terms) {
      double partial, first_error, value, second_error;
      if (!sum_with_error(order[0], order[1], partial, first_error) ||
          !sum_with_error(partial, order[2], value, second_error))
      {
        continue;
      }
      const double error = add_upper(std::abs(first_error), std::abs(second_error));
      if (error < best_error) {
        corner[axis] = value;
        best_error = error;
      }
    }
    if (!std::isfinite(best_error)) {
      return std::nullopt;
    }
    r_error_bound = add_upper(r_error_bound, best_error);
  }
  return corner;
}

}  // namespace blender::bke::nurbs::detail
