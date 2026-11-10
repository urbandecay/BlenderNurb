/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_spline.hh"

#include <algorithm>
#include <cmath>
#include <limits>

namespace blender::solid {
namespace {

bool fail(std::string *error, const char *message)
{
  if (error) {
    *error = message;
  }
  return false;
}

bool finite(const Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool validate_axis(const int degree,
                   const std::vector<double> &knots,
                   const size_t count,
                   std::string *error)
{
  if (degree < 1 || size_t(degree) >= count) {
    return fail(error, "Spline degree must be positive and smaller than its pole count");
  }
  const size_t order = size_t(degree) + 1;
  if (count > std::numeric_limits<size_t>::max() - order || knots.size() != count + order) {
    return fail(error, "Spline expanded knot count must equal pole count plus degree plus one");
  }
  for (size_t i = 0; i < knots.size(); ++i) {
    if (!std::isfinite(knots[i]) || (i && knots[i] < knots[i - 1])) {
      return fail(error, "Spline knots must be finite and nondecreasing");
    }
  }
  const double low = knots[size_t(degree)], high = knots[count];
  if (!(low < high)) {
    return fail(error, "Spline active knot domain must contain a nonempty span");
  }
  for (size_t first = 0; first < knots.size();) {
    size_t last = first + 1;
    while (last < knots.size() && knots[last] == knots[first]) {
      ++last;
    }
    const bool interior = knots[first] > low && knots[first] < high;
    if (last - first > (interior ? size_t(degree) : order)) {
      return fail(error, "Spline knot multiplicity exceeds its continuous spline degree limit");
    }
    first = last;
  }
  return true;
}

bool validate_poles(const std::vector<Vec3> &poles,
                    const std::vector<double> &weights,
                    std::string *error)
{
  if (poles.size() != weights.size()) {
    return fail(error, "Spline requires one positive weight for every pole");
  }
  for (size_t i = 0; i < poles.size(); ++i) {
    if (!finite(poles[i]) || !std::isfinite(weights[i]) || !(weights[i] > 0)) {
      return fail(error, "Spline poles must be finite and weights finite and positive");
    }
  }
  return true;
}

struct Basis {
  size_t first = 0;
  std::vector<long double> values, derivatives;
};

/* Independently implemented Cox-de Boor recurrence and the derivative identity
 * N'_(i,p) = p*N_(i,p-1)/(U_(i+p)-U_i)
 *            - p*N_(i+1,p-1)/(U_(i+p+1)-U_(i+1)).
 * No OCCT implementation or runtime is used. Extended intermediates and a local
 * common weight scale avoid unnecessary overflow/underflow in homogeneous sums. */
std::vector<long double> basis_values(const std::vector<double> &knots,
                                      const size_t span,
                                      const size_t degree,
                                      const double parameter)
{
  std::vector<long double> values(degree + 1), left(degree + 1), right(degree + 1);
  values[0] = 1;
  for (size_t j = 1; j <= degree; ++j) {
    left[j] = static_cast<long double>(parameter) - knots[span + 1 - j];
    right[j] = static_cast<long double>(knots[span + j]) - parameter;
    long double saved = 0;
    for (size_t r = 0; r < j; ++r) {
      const long double denominator = right[r + 1] + left[j - r];
      const long double term = denominator == 0 ? 0 : values[r] / denominator;
      values[r] = saved + right[r + 1] * term;
      saved = left[j - r] * term;
    }
    values[j] = saved;
  }
  return values;
}

std::optional<Basis> basis(const std::vector<double> &knots,
                           const size_t count,
                           const int degree,
                           const double parameter,
                           std::string *error)
{
  const size_t p = size_t(degree);
  if (!std::isfinite(parameter) || parameter < knots[p] || parameter > knots[count]) {
    fail(error, "Spline parameter must be finite and inside the active knot domain");
    return {};
  }
  size_t span;
  if (parameter == knots[count]) {
    // The active endpoint may be repeated in a non-clamped knot vector too.
    span = size_t(std::lower_bound(knots.begin() + p, knots.begin() + count + 1, parameter) -
                  knots.begin()) - 1;
  }
  else {
    span = size_t(std::upper_bound(knots.begin() + p, knots.begin() + count + 1, parameter) -
                  knots.begin()) - 1;
  }
  Basis result;
  result.first = span - p;
  result.values = basis_values(knots, span, p, parameter);
  result.derivatives.resize(p + 1);
  const auto lower = basis_values(knots, span, p - 1, parameter);
  for (size_t j = 0; j <= p; ++j) {
    const size_t i = result.first + j;
    const long double first_den = static_cast<long double>(knots[i + p]) - knots[i];
    const long double last_den = static_cast<long double>(knots[i + p + 1]) - knots[i + 1];
    if (j && first_den != 0) {
      result.derivatives[j] += p * lower[j - 1] / first_den;
    }
    if (j < p && last_den != 0) {
      result.derivatives[j] -= p * lower[j] / last_den;
    }
  }
  return result;
}

struct Homogeneous {
  std::array<long double, 3> point{}, du{}, dv{};
  long double weight = 0, u_weight = 0, v_weight = 0;

  void add(const Vec3 pole,
           const long double w,
           const long double value,
           const long double u_value,
           const long double v_value = 0)
  {
    weight += w * value;
    u_weight += w * u_value;
    v_weight += w * v_value;
    for (int i = 0; i < 3; ++i) {
      point[i] += w * value * pole[i];
      du[i] += w * u_value * pole[i];
      dv[i] += w * v_value * pole[i];
    }
  }

  std::optional<SplineSurfaceSample> sample(std::string *error) const
  {
    if (!(weight > 0) || !std::isfinite(weight)) {
      fail(error, "Spline homogeneous denominator is not finite and positive");
      return {};
    }
    SplineSurfaceSample result;
    for (int i = 0; i < 3; ++i) {
      const long double position = point[i] / weight;
      result.position[i] = double(position);
      result.u_derivative[i] = double((du[i] - position * u_weight) / weight);
      result.v_derivative[i] = double((dv[i] - position * v_weight) / weight);
    }
    if (!finite(result.position) || !finite(result.u_derivative) || !finite(result.v_derivative)) {
      fail(error, "Spline evaluation exceeds the finite result range");
      return {};
    }
    return result;
  }
};

}  // namespace

bool validate_spline(const RationalBSplineCurve &spline, std::string *error)
{
  return validate_axis(spline.degree, spline.knots, spline.poles.size(), error) &&
         validate_poles(spline.poles, spline.weights, error);
}

bool validate_spline(const RationalBSplineSurface &spline, std::string *error)
{
  if (spline.u_count == 0 || spline.v_count == 0 ||
      spline.u_count > std::numeric_limits<size_t>::max() / spline.v_count ||
      spline.poles.size() != spline.u_count * spline.v_count)
  {
    return fail(error, "Spline surface pole count must match its nonempty tensor dimensions");
  }
  return validate_axis(spline.u_degree, spline.u_knots, spline.u_count, error) &&
         validate_axis(spline.v_degree, spline.v_knots, spline.v_count, error) &&
         validate_poles(spline.poles, spline.weights, error);
}

std::optional<SplineDomain> spline_domain(const RationalBSplineCurve &spline, std::string *error)
{
  if (!validate_spline(spline, error)) {
    return {};
  }
  return SplineDomain{spline.knots[size_t(spline.degree)], spline.knots[spline.poles.size()]};
}

std::optional<std::array<SplineDomain, 2>> spline_domain(const RationalBSplineSurface &spline,
                                                       std::string *error)
{
  if (!validate_spline(spline, error)) {
    return {};
  }
  return std::array<SplineDomain, 2>{
      SplineDomain{spline.u_knots[size_t(spline.u_degree)], spline.u_knots[spline.u_count]},
      SplineDomain{spline.v_knots[size_t(spline.v_degree)], spline.v_knots[spline.v_count]}};
}

std::optional<SplineCurveSample> evaluate_spline(const RationalBSplineCurve &spline,
                                                const double parameter,
                                                std::string *error)
{
  if (!validate_spline(spline, error)) {
    return {};
  }
  const auto b = basis(spline.knots, spline.poles.size(), spline.degree, parameter, error);
  if (!b) {
    return {};
  }
  double scale = 0;
  for (size_t j = 0; j < b->values.size(); ++j) {
    scale = std::max(scale, spline.weights[b->first + j]);
  }
  Homogeneous sum;
  for (size_t j = 0; j < b->values.size(); ++j) {
    const size_t i = b->first + j;
    sum.add(spline.poles[i],
            static_cast<long double>(spline.weights[i]) / scale,
            b->values[j],
            b->derivatives[j]);
  }
  const auto result = sum.sample(error);
  if (!result) {
    return {};
  }
  return SplineCurveSample{result->position, result->u_derivative};
}

std::optional<SplineSurfaceSample> evaluate_spline(const RationalBSplineSurface &spline,
                                                  const double u,
                                                  const double v,
                                                  std::string *error)
{
  if (!validate_spline(spline, error)) {
    return {};
  }
  const auto bu = basis(spline.u_knots, spline.u_count, spline.u_degree, u, error);
  if (!bu) {
    return {};
  }
  const auto bv = basis(spline.v_knots, spline.v_count, spline.v_degree, v, error);
  if (!bv) {
    return {};
  }
  double scale = 0;
  for (size_t j = 0; j < bv->values.size(); ++j) {
    for (size_t i = 0; i < bu->values.size(); ++i) {
      const size_t k = (bv->first + j) * spline.u_count + bu->first + i;
      scale = std::max(scale, spline.weights[k]);
    }
  }
  Homogeneous sum;
  for (size_t j = 0; j < bv->values.size(); ++j) {
    for (size_t i = 0; i < bu->values.size(); ++i) {
      const size_t k = (bv->first + j) * spline.u_count + bu->first + i;
      sum.add(spline.poles[k],
              static_cast<long double>(spline.weights[k]) / scale,
              bu->values[i] * bv->values[j],
              bu->derivatives[i] * bv->values[j],
              bu->values[i] * bv->derivatives[j]);
    }
  }
  return sum.sample(error);
}

}  // namespace blender::solid
