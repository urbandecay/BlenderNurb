/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_spline.hh"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace blender::solid;

static void require(const bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static bool near(const Vec3 a, const Vec3 b, const double tolerance = 1e-12)
{
  return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
         std::abs(a.z - b.z) <= tolerance;
}

static Vec3 difference(const Vec3 a, const Vec3 b, const double denominator)
{
  return {(a.x - b.x) / denominator,
          (a.y - b.y) / denominator,
          (a.z - b.z) / denominator};
}

static void check_curve_derivative(const RationalBSplineCurve &curve, const double u)
{
  constexpr double step = 1e-6;
  const auto sample = evaluate_spline(curve, u);
  const auto before = evaluate_spline(curve, u - step);
  const auto after = evaluate_spline(curve, u + step);
  require(sample && before && after, "Curve derivative sample failed");
  require(near(sample->derivative, difference(after->position, before->position, 2 * step), 2e-8),
          "Analytic rational curve derivative disagrees with independent central difference");
}

static void check_surface_derivatives(const RationalBSplineSurface &surface,
                                      const double u,
                                      const double v)
{
  constexpr double step = 1e-6;
  const auto sample = evaluate_spline(surface, u, v);
  const auto u_before = evaluate_spline(surface, u - step, v);
  const auto u_after = evaluate_spline(surface, u + step, v);
  const auto v_before = evaluate_spline(surface, u, v - step);
  const auto v_after = evaluate_spline(surface, u, v + step);
  require(sample && u_before && u_after && v_before && v_after, "Surface derivative sample failed");
  require(near(sample->u_derivative,
               difference(u_after->position, u_before->position, 2 * step),
               2e-8) &&
              near(sample->v_derivative,
                   difference(v_after->position, v_before->position, 2 * step),
                   2e-8),
          "Analytic rational surface derivatives disagree with independent central differences");
}

int main()
{
  const double root_half = std::sqrt(.5);
  const RationalBSplineCurve quarter{
      2, {0, 0, 0, 1, 1, 1}, {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {1, root_half, 1}};
  require(validate_spline(quarter), "Rational quarter circle failed validation");
  const auto domain = spline_domain(quarter);
  require(domain && domain->min == 0 && domain->max == 1, "Incorrect active spline domain");
  const auto middle = evaluate_spline(quarter, .5);
  require(middle && near(middle->position, {root_half, root_half, 0}),
          "Rational quadratic did not reproduce the exact quarter circle midpoint");
  require(near(evaluate_spline(quarter, 0)->position, {1, 0, 0}) &&
              near(evaluate_spline(quarter, 1)->position, {0, 1, 0}) &&
              near(evaluate_spline(quarter, 0)->derivative, {0, 2 * root_half, 0}) &&
              near(evaluate_spline(quarter, 1)->derivative, {-2 * root_half, 0, 0}),
          "Clamped endpoints or their analytic derivatives are incorrect");
  for (const double t : {.05, .2, .5, .7, .95}) {
    const auto sample = evaluate_spline(quarter, t);
    require(sample && std::abs(sample->position.x * sample->position.x +
                                  sample->position.y * sample->position.y - 1) < 1e-12,
            "Rational quadratic left the unit circle");
    require(std::abs(sample->position.x * sample->derivative.x +
                     sample->position.y * sample->derivative.y) < 1e-12,
            "Circle derivative is not tangent to its support");
    check_curve_derivative(quarter, t);
  }

  const RationalBSplineCurve repeated{2,
                                      {0, 0, 0, .3, .3, .8, 1, 1, 1},
                                      {{0, 0, 0}, {1, 0, 0}, {1, 2, 0},
                                       {3, 2, 1}, {4, -1, 0}, {6, 0, 0}},
                                      {1, 1, 1, 1, 1, 1}};
  require(validate_spline(repeated), "Nonuniform repeated knots failed validation");
  const auto join = evaluate_spline(repeated, .3);
  require(join && near(join->position, {1, 2, 0}) && near(join->derivative, {8, 0, 4}),
          "Repeated interior knot did not use its documented right-hand derivative");
  const auto left = evaluate_spline(repeated, std::nextafter(.3, 0.));
  require(left && near(left->derivative, {0, 4 / .3, 0}, 1e-11),
          "Repeated knot left-hand derivative was lost");
  for (const double t : {.12, .42, .7, .91}) {
    check_curve_derivative(repeated, t);
  }

  const RationalBSplineCurve cubic{3,
                                   {-2, -2, -2, -2, 3, 3, 3, 3},
                                   {{-1, 2, 0}, {3, -2, 1}, {2, 5, -3}, {7, 1, 2}},
                                   {1, .3, 2, 1.7}};
  for (const double t : {.1, .4, .8}) {
    // An explicit Bernstein polynomial provides an independent higher-degree oracle.
    const double s = 1 - t;
    const double bernstein[4] = {s * s * s, 3 * t * s * s, 3 * t * t * s, t * t * t};
    double denominator = 0;
    Vec3 position{};
    for (int i = 0; i < 4; ++i) {
      const double value = cubic.weights[i] * bernstein[i];
      denominator += value;
      for (int axis = 0; axis < 3; ++axis) {
        position[axis] += value * cubic.poles[i][axis];
      }
    }
    for (int axis = 0; axis < 3; ++axis) {
      position[axis] /= denominator;
    }
    const auto sample = evaluate_spline(cubic, -2 + 5 * t);
    require(sample && near(sample->position, position),
            "General-degree evaluation disagrees with the cubic Bernstein oracle");
    check_curve_derivative(cubic, -2 + 5 * t);
  }

  const RationalBSplineCurve nonclamped{
      2, {-2, -1, 0, 1, 2, 3, 4}, {{0, 0, 0}, {1, 0, 0}, {1, 2, 0}, {0, 2, 0}}, {1, 1, 1, 1}};
  require(validate_spline(nonclamped) && near(evaluate_spline(nonclamped, 0)->position, {.5, 0, 0}) &&
              near(evaluate_spline(nonclamped, 2)->position, {.5, 2, 0}) &&
              near(evaluate_spline(nonclamped, 2)->derivative, {-1, 0, 0}),
          "Non-clamped domain endpoint evaluation is incorrect");
  RationalBSplineCurve repeated_end = nonclamped;
  repeated_end.knots = {-2, -1, 0, 1, 2, 2, 3};
  require(validate_spline(repeated_end) &&
              near(evaluate_spline(repeated_end, 2)->position, {0, 2, 0}) &&
              near(evaluate_spline(repeated_end, 2)->derivative, {-2, 0, 0}),
          "Repeated non-clamped upper endpoint must select the last nonempty left span");

  const RationalBSplineSurface bilinear{1,
                                        1,
                                        {2, 2, 5, 5},
                                        {-1, -1, 1, 1},
                                        2,
                                        2,
                                        {{0, 0, 0}, {3, 0, 0}, {0, 2, 0}, {3, 2, 4}},
                                        {1, 1, 1, 1}};
  const auto patch = evaluate_spline(bilinear, 3.5, 0);
  require(patch && near(patch->position, {1.5, 1, 1}) &&
              near(patch->u_derivative, {1, 0, 2. / 3}) &&
              near(patch->v_derivative, {0, 1, 1}),
          "Bilinear tensor-product value or parameter-scaled derivatives are incorrect");
  require(near(evaluate_spline(bilinear, 5, 1)->position, {3, 2, 4}),
          "Surface corner endpoint evaluation is incorrect");
  check_surface_derivatives(bilinear, 3.1, .3);

  RationalBSplineSurface cylinder{2,
                                  1,
                                  quarter.knots,
                                  {0, 0, 1, 1},
                                  3,
                                  2,
                                  {{1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {1, 0, 2}, {1, 1, 2}, {0, 1, 2}},
                                  {1, root_half, 1, 1, root_half, 1}};
  const auto rational_patch = evaluate_spline(cylinder, .5, .25);
  require(rational_patch && near(rational_patch->position, {root_half, root_half, .5}) &&
              near(rational_patch->v_derivative, {0, 0, 2}) &&
              near(rational_patch->u_derivative, middle->derivative),
          "Rational tensor-product surface did not reproduce a quarter cylinder");
  check_surface_derivatives(cylinder, .37, .6);
  // Nonseparable weights exercise both terms of the rational derivative quotient.
  cylinder.weights = {1, 2, 3, 4, 2, 1};
  check_surface_derivatives(cylinder, .37, .6);
  RationalBSplineSurface nonuniform_surface{2,
                                            2,
                                            quarter.knots,
                                            {-1, -1, -1, .2, 2, 2, 2},
                                            3,
                                            4,
                                            {},
                                            {}};
  for (size_t j = 0; j < 4; ++j) {
    for (size_t i = 0; i < 3; ++i) {
      nonuniform_surface.poles.push_back(
          {double(i), double(j), double(i * j) - .2 * double(j * j)});
      nonuniform_surface.weights.push_back(.4 + double(i + 2 * j));
    }
  }
  check_surface_derivatives(nonuniform_surface, .21, -.6);
  check_surface_derivatives(nonuniform_surface, .73, 1.3);
  const auto surface_domain = spline_domain(bilinear);
  require(surface_domain && (*surface_domain)[0].min == 2 && (*surface_domain)[0].max == 5 &&
              (*surface_domain)[1].min == -1 && (*surface_domain)[1].max == 1,
          "Surface domain does not respect both knot parameterizations");

  // Weight scaling does not change geometry, even near double's exponent limits.
  for (const double scale : {1e-300, 1e300}) {
    auto scaled = quarter;
    for (double &weight : scaled.weights) {
      weight *= scale;
    }
    const auto sample = evaluate_spline(scaled, .5);
    require(sample && near(sample->position, middle->position) &&
                near(sample->derivative, middle->derivative),
            "Common weight scaling changed rational spline geometry");
  }

  const double infinity = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto invalid = [&](const RationalBSplineCurve &curve) {
    std::string error;
    require(!validate_spline(curve, &error) && !error.empty() && !spline_domain(curve) &&
                !evaluate_spline(curve, .5),
            "Invalid spline data was accepted or failed without a diagnostic");
  };
  invalid(RationalBSplineCurve{});
  for (const int degree : {-1, 0, 3, std::numeric_limits<int>::max()}) {
    auto bad = quarter;
    bad.degree = degree;
    invalid(bad);
  }
  auto bad = quarter;
  bad.knots.pop_back();
  invalid(bad);
  bad = quarter;
  bad.knots[2] = .5;
  bad.knots[3] = .4;
  invalid(bad);
  bad = quarter;
  bad.knots.assign(6, 0);
  invalid(bad);
  bad = quarter;
  bad.knots = {0, 0, 0, 0, 1, 1};
  invalid(bad);
  bad = repeated;
  bad.knots = {0, 0, 0, .3, .3, .3, 1, 1, 1};
  invalid(bad);
  for (const double value : {infinity, nan}) {
    bad = quarter;
    bad.knots[3] = value;
    invalid(bad);
    bad = quarter;
    bad.poles[1].y = value;
    invalid(bad);
  }
  bad = quarter;
  bad.weights.pop_back();
  invalid(bad);
  for (const double weight : {0., -1., infinity, nan}) {
    bad = quarter;
    bad.weights[1] = weight;
    invalid(bad);
  }
  for (const double parameter : {-1., 2., infinity, nan}) {
    std::string error;
    require(!evaluate_spline(quarter, parameter, &error) && !error.empty(),
            "Invalid parameter was clamped, extrapolated, or failed without a diagnostic");
  }
  auto bad_surface = bilinear;
  bad_surface.u_count = std::numeric_limits<size_t>::max();
  require(!validate_spline(bad_surface) && !evaluate_spline(bad_surface, 3., 0.),
          "Overflowing tensor dimensions were accepted");
  bad_surface = bilinear;
  bad_surface.poles.pop_back();
  require(!validate_spline(bad_surface), "Incomplete tensor net was accepted");
  bad_surface = bilinear;
  bad_surface.v_knots[2] = nan;
  require(!validate_spline(bad_surface), "Invalid second surface knot axis was accepted");
  require(!evaluate_spline(bilinear, 3., 2.) && !evaluate_spline(bilinear, nan, 0.),
          "Invalid surface parameters were accepted");
  // A finite valid input can still have an unrepresentable derivative: report failure.
  RationalBSplineCurve overflowing{1,
                                   {0, 0, 1e-300, 1e-300},
                                   {{-1e300, 0, 0}, {1e300, 0, 0}},
                                   {1, 1}};
  require(validate_spline(overflowing) && !evaluate_spline(overflowing, 5e-301),
          "Unrepresentable spline derivative must not escape as infinity");
  std::cout << "Native rational spline math tests passed\n";
}
