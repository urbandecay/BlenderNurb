/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_intersections.hh"
#include "boolean_pave.hh"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>

using namespace blender::solid;

static void require(const bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static Surface plane(const Vec3 origin, const Vec3 u, const Vec3 v)
{
  Surface result;
  result.kind = SurfaceKind::Plane;
  result.origin = origin;
  result.u = u;
  result.v = v;
  return result;
}

int main()
{
  Model model;
  const Surface xy = plane({}, {1, 0, 0}, {0, 1, 0});
  const Surface yz = plane({.3, 0, 0}, {0, 1, 0}, {0, 0, 1});
  auto result = intersect_surfaces(model, xy, yz);
  require(result && result->state == SurfaceIntersectionState::Curves &&
              result->curves.size() == 1 && result->curves[0].kind == CurveKind::Line,
          "Crossing planes must produce one native line");
  for (double t : {-10., 0., 3.}) {
    const Vec3 p = evaluate(result->curves[0], t);
    require(std::abs(p.z) < 1e-12 && std::abs(p.x - .3) < 1e-12,
            "Plane intersection line left its support surfaces");
  }
  require(intersect_surfaces(model, xy, xy)->state == SurfaceIntersectionState::Coincident,
          "Identical planes must be classified as coincident");
  Surface parallel = xy;
  parallel.origin.z = 2;
  require(intersect_surfaces(model, xy, parallel)->state == SurfaceIntersectionState::Separated,
          "Parallel offset planes must be separated");
  parallel.origin.z = 1e-11;
  require(intersect_surfaces(model, xy, parallel)->state == SurfaceIntersectionState::Uncertain,
          "Near-coincident planes must not be silently merged");

  Surface cylinder;
  cylinder.kind = SurfaceKind::Cylinder;
  cylinder.origin = {3, -2, 5};
  cylinder.u = {2, 0, 0};
  cylinder.v = {.7, 1.3, 0};
  cylinder.w = {.2, .4, 3};
  // Check exact intersection residuals for circular, elliptical and sheared sections.
  for (const Surface &section : {xy, yz, plane({1, 2, 3}, {1, 1, 0}, {0, 1, 1})}) {
    result = intersect_surfaces(model, section, cylinder);
    require(result && result->state == SurfaceIntersectionState::Curves &&
                result->curves.size() == 1 && result->curves[0].kind == CurveKind::Conic,
            "A transverse plane must cut an affine cylinder in an exact conic");
    const Vec3 n = cross(section.u, section.v);
    const double determinant = dot(cylinder.u, cross(cylinder.v, cylinder.w));
    for (int i = 0; i < 41; ++i) {
      const Vec3 p = evaluate(result->curves[0], i * .17);
      const Vec3 q = p - cylinder.origin;
      const double x = dot(q, cross(cylinder.v, cylinder.w)) / determinant;
      const double y = dot(q, cross(cylinder.w, cylinder.u)) / determinant;
      require(std::abs(dot(p - section.origin, n)) < 1e-10 &&
                  std::abs(x * x + y * y - 1) < 1e-10,
              "Intersection conic left one of its support surfaces");
    }
    const auto swapped = intersect_surfaces(model, cylinder, section);
    require(swapped && swapped->curves[0].origin == result->curves[0].origin,
            "Swapping surface operands changed their intersection");
  }

  cylinder.origin = {};
  cylinder.u = {1, 0, 0};
  cylinder.v = {0, 1, 0};
  cylinder.w = {0, 0, 1};
  for (const double x : {0., 1., 2., 1. + 1e-11, 1. - 1e-11}) {
    const Surface section = plane({x, 0, 0}, {0, 1, 0}, {0, 0, 1});
    result = intersect_surfaces(model, section, cylinder);
    require(bool(result), "Valid plane/cylinder intersection failed");
    if (x == 0 || x == 1) {
      require(result->state == SurfaceIntersectionState::Curves &&
                  result->curves.size() == (x == 0 ? 2 : 1) && result->tangent == (x == 1),
              "Crossing and tangent generators were misclassified");
      for (const Curve &curve : result->curves) {
        for (double t : {-2., 0., 7.}) {
          const Vec3 p = evaluate(curve, t);
          require(std::abs(p.x - x) < 1e-12 && std::abs(p.x * p.x + p.y * p.y - 1) < 1e-12,
                  "Generator line left its exact supporting surfaces");
        }
      }
    }
    else {
      require(result->state == (x == 2 ? SurfaceIntersectionState::Separated :
                                         SurfaceIntersectionState::Uncertain),
              "Near tangency was silently snapped or a separated plane intersected");
    }
  }

  model.curves.push_back({1, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}});
  Surface extrusion;
  extrusion.kind = SurfaceKind::Extrusion;
  extrusion.curve = 1;
  extrusion.u = {0, 0, -2};
  result = intersect_surfaces(model, xy, extrusion);
  require(result && result->state == SurfaceIntersectionState::Curves,
          "A reversed native conic extrusion must intersect as a cylinder");
  Surface invalid = xy;
  invalid.u = {};
  require(!intersect_surfaces(model, invalid, cylinder), "Invalid plane was accepted");
  require(!intersect_surfaces(model, xy, cylinder, {std::numeric_limits<double>::quiet_NaN(), 0}),
          "Invalid tolerance was accepted");

  // Ellipse cuts retain the original support and parameterization. The cut
  // crosses a periodic seam, which must survive as two endpoint occurrences.
  model.curves.push_back({2, CurveKind::Conic, {3, -2, 5}, {2, 0, 0}, {.7, 1.3, 0}});
  const Curve &ellipse = model.curves.back();
  const double period = 2 * std::numbers::pi;
  const Edge full{20, 2, {10, 10}, 0, period};
  const Surface seam_plane = plane(ellipse.origin + ellipse.u,
                                   ellipse.u, {0, 0, 1});
  const auto hits = intersect_edge_plane(model, full, seam_plane);
  require(hits && hits->state == EdgePlaneState::Points && hits->parameters.size() == 3,
          "Periodic edge must retain both seam occurrences and the opposite crossing");
  std::vector<BooleanPave> paves;
  for (size_t i = 0; i < hits->parameters.size(); ++i) {
    const double t = hits->parameters[i];
    require(std::abs(dot(evaluate(ellipse, t) - seam_plane.origin,
                         cross(seam_plane.u, seam_plane.v))) < 1e-12,
            "Analytic edge intersection left the original ellipse or plane");
    paves.push_back({i == 1 ? Id(11) : Id(10), t});
  }
  const auto blocks = boolean_pave_blocks(full.id, {10, 0}, {10, period}, paves);
  require(blocks && blocks->size() == 2 && blocks->front().first.vertex == 10 &&
              blocks->back().last.vertex == 10 &&
              blocks->front().last.vertex == blocks->back().first.vertex,
          "Edge partition lost shared vertices or the periodic seam");
  double covered = 0;
  for (const auto &block : *blocks) {
    covered += block.last.parameter - block.first.parameter;
    require(block.original_edge == full.id, "Edge partition lost its source history");
  }
  require(covered == period, "Curve partition changed the covered parameter range");
  const auto reverse = boolean_pave_blocks(full.id, {10, period}, {10, 0}, paves);
  require(reverse && reverse->size() == 2 &&
              reverse->front().first.parameter == period && reverse->back().last.parameter == 0,
          "Reversed edge partition changed its traversal");
  const std::vector<BooleanPave> conflict{{99, 0}};
  require(!boolean_pave_blocks(full.id, {10, 0}, {10, period}, conflict),
          "Unreconciled coincident intersection vertices were accepted");
  const std::vector<BooleanPave> outside{{11, period + 1}};
  require(!boolean_pave_blocks(full.id, {10, 0}, {10, period}, outside),
          "Intersection outside the source trim was accepted");
  const double almost_half = std::nextafter(.5, 1.);
  const std::vector<BooleanPave> thin{{11, .5}, {12, almost_half}};
  const auto thin_blocks = boolean_pave_blocks(20, {10, 0}, {13, 1}, thin);
  require(thin_blocks && thin_blocks->size() == 3 &&
              (*thin_blocks)[1].last.parameter > (*thin_blocks)[1].first.parameter,
          "Edge partition silently collapsed a thin interval");

  model.curves.push_back({3, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}});
  const Edge unit_circle{21, 3, {14, 14}, -period, period};
  const auto contact = intersect_edge_plane(model, unit_circle,
      plane({1, 0, 0}, {0, 1, 0}, {0, 0, 1}));
  require(contact && contact->tangent && contact->parameters.size() == 3,
          "Tangent contact must produce one point per periodic occurrence");
  const auto near_contact = intersect_edge_plane(model, unit_circle,
      plane({1 + 1e-11, 0, 0}, {0, 1, 0}, {0, 0, 1}));
  require(near_contact && near_contact->state == EdgePlaneState::Uncertain,
          "Near tangency was silently converted to contact");
  Edge reversed_edge = unit_circle;
  std::swap(reversed_edge.start, reversed_edge.end);
  const auto reversed_hits = intersect_edge_plane(model, reversed_edge, yz);
  require(reversed_hits && reversed_hits->parameters.size() == 4 &&
              reversed_hits->parameters.front() > reversed_hits->parameters.back(),
          "Reversed conic edge lost intersections or traversal order");
  model.curves.push_back({4, CurveKind::Line, {0, 0, -2}, {0, 0, 4}});
  require(intersect_edge_plane(model, Edge{22, 4, {15, 16}, 0, 1}, xy)->parameters ==
              std::vector<double>{.5},
          "Line/plane intersection changed the source parameter");
  require(intersect_edge_plane(model, full, xy)->state == EdgePlaneState::Separated,
          "Coplanar-support conic in an offset plane was not separated");
  model.curves.push_back({5, CurveKind::Line, {0, 0, -1}, {1, 0, 1e-13}});
  const auto long_crossing = intersect_edge_plane(model, Edge{23, 5, {17, 18}, 0, 2e13}, xy);
  require(long_crossing && long_crossing->state == EdgePlaneState::Points &&
              long_crossing->parameters == std::vector<double>{1e13},
          "Angular tolerance incorrectly discarded a long bounded-line crossing");
  require(intersect_edge_plane(model, Edge{23, 5, {17, 18}, 0, 1}, xy)->state ==
              EdgePlaneState::Separated,
          "A separated bounded line was classified using its unbounded support");

  /* Polynomial cuts preserve their original parameter and all bounded roots,
   * including repeated roots and intervals much thinner than the distance tolerance. */
  auto cut = [&](Curve support, const double start = 0, const double end = 1) {
    support.id = model.curves.size() + 1;
    model.curves.push_back(support);
    return intersect_edge_plane(model, Edge{30, support.id, {20, 21}, start, end}, xy);
  };
  const Curve three_roots{0, CurveKind::Cubic, {0, 0, -.09375},
                          {1, 0, .6875}, {0, 0, -1.5}, {0, 0, 1}};
  const auto cubic = cut(three_roots);
  require(cubic && cubic->state == EdgePlaneState::Points && !cubic->tangent &&
              cubic->parameters == std::vector<double>{.25, .5, .75},
          "Cubic/plane intersection lost or moved one of three native roots");
  require(cut(three_roots, .5, .75)->parameters == std::vector<double>{.5, .75},
          "Cubic/plane intersection lost exact trim endpoint roots");
  require(cut(three_roots, 1, 0)->parameters == std::vector<double>{.75, .5, .25},
          "Cubic/plane intersection lost reversed edge traversal");
  require(cut(three_roots, .26, .49)->state == EdgePlaneState::Separated,
          "Cubic/plane intersection used roots outside its bounded trim");
  const double separation = std::ldexp(1., -20);
  const double square_separation = separation * separation;
  const Curve clustered{0, CurveKind::Cubic, {0, 0, -1 + square_separation},
                        {1, 0, 3 - square_separation}, {0, 0, -3}, {0, 0, 1}};
  const auto clustered_roots = cut(clustered, 0, 2);
  require(clustered_roots && !clustered_roots->tangent &&
              clustered_roots->parameters ==
                  std::vector<double>{1 - separation, 1, 1 + separation},
          "Discriminant cancellation merged three distinct cubic intersections");

  Curve repeated{0, CurveKind::Cubic, {0, 0, -.046875},
                 {1, 0, .4375}, {0, 0, -1.25}, {0, 0, 1}};
  const auto double_root = cut(repeated);
  require(double_root && double_root->tangent &&
              double_root->parameters == std::vector<double>{.25, .75},
          "Cubic/plane intersection lost a double root or its separate crossing");
  require(!cut(repeated, .5, 1)->tangent,
          "A repeated root outside the trim marked the bounded crossing tangent");
  repeated.origin.z = -.125;
  repeated.u.z = .75;
  repeated.v.z = -1.5;
  const auto triple_root = cut(repeated);
  require(triple_root && triple_root->tangent &&
              triple_root->parameters == std::vector<double>{.5},
          "Cubic/plane intersection lost a stationary triple root");

  Curve quadratic{0, CurveKind::Cubic, {0, 0, .0625},
                  {1, 0, -.5}, {0, 0, 1}, {}};
  require(cut(quadratic)->tangent &&
              cut(quadratic)->parameters == std::vector<double>{.25},
          "Degree-reduced cubic tangency was not solved natively");
  quadratic.origin.z = std::nextafter(.0625, 1.);
  require(cut(quadratic)->state == EdgePlaneState::Separated,
          "A near-contact polynomial was silently snapped onto the plane");
  quadratic.origin.z = std::nextafter(.0625, 0.);
  const auto thin_polynomial = cut(quadratic);
  require(thin_polynomial && thin_polynomial->state == EdgePlaneState::Points &&
              !thin_polynomial->tangent && thin_polynomial->parameters.size() == 2 &&
              thin_polynomial->parameters[0] < .25 && thin_polynomial->parameters[1] > .25,
          "Two close polynomial crossings were silently merged into tangency");

  Curve coplanar = three_roots;
  coplanar.origin.z = coplanar.u.z = coplanar.v.z = coplanar.w.z = 0;
  require(cut(coplanar)->state == EdgePlaneState::Coincident,
          "A coplanar cubic was not classified as coincident");
  const Curve unrepresentable{0, CurveKind::Cubic,
                              {0, 0, -std::numeric_limits<double>::denorm_min()},
                              {1, 0, 1e308}, {}, {}};
  const auto tiny_root = cut(unrepresentable, -1, 1);
  require(tiny_root && tiny_root->state == EdgePlaneState::Uncertain,
          "An unrepresentable polynomial parameter was silently snapped to zero");

  /* Weighted controls are points, rather than the power coefficients used by
   * Cubic. The plane equation is solved in homogeneous form without fitting. */
  Curve rational{0, CurveKind::RationalQuadratic, {0, 0, .1875},
                 {.5, 0, -.15625}, {1, 0, .1875}, {1, 2, 1}};
  const auto weighted = cut(rational);
  require(weighted && weighted->parameters == std::vector<double>{.25, .75} &&
              !weighted->tangent,
          "Weighted rational quadratic did not retain its native plane roots");
  for (const double t : weighted->parameters) {
    require(std::abs(evaluate(rational, t).z) < 1e-15,
            "Rational intersection left its original support and plane");
  }
  require(cut(rational, 1, 0)->parameters == std::vector<double>{.75, .25},
          "Rational intersections changed reversed trim traversal");
  rational.origin.z = rational.v.z = .25;
  rational.u.z = -.125;
  const auto rational_tangent = cut(rational);
  require(rational_tangent && rational_tangent->tangent &&
              rational_tangent->parameters == std::vector<double>{.5},
          "Rational quadratic exact tangency was lost");
  rational.u.z = std::nextafter(-.125, 0.);
  const auto rational_near_contact = cut(rational);
  require(rational_near_contact && !rational_near_contact->tangent &&
              rational_near_contact->state == EdgePlaneState::Separated,
          "Rational near contact was silently snapped onto the plane");
  rational.origin.z = rational.u.z = rational.v.z = 0;
  require(cut(rational)->state == EdgePlaneState::Coincident,
          "A coplanar rational quadratic was not classified as coincident");
  rational.w = {1, 3, 1};
  require(!cut(rational, -1, 1),
          "A rational denominator pole inside the trim was accepted");
  rational.w = {1, 2, 1};
  require(bool(cut(rational, -.1, 1.1)),
          "A regular rational extension outside the unit interval was rejected");
  rational.w.y = 0;
  require(!cut(rational), "A nonpositive rational control weight was accepted");
  rational.w.y = std::numeric_limits<double>::infinity();
  require(!cut(rational), "A nonfinite rational control weight was accepted");
  repeated.w.z = std::numeric_limits<double>::quiet_NaN();
  require(!cut(repeated), "A nonfinite cubic power coefficient was accepted");
  std::cout << "Native surface intersection checks passed\n";
}
