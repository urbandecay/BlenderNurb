/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <climits>
#include <cmath>

#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_evaluate.hh"
#include "BLI_math_boolean.hh"

#include "MEM_guardedalloc.h"

#include "nurbs_math.hh"
#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs {

std::optional<SurfaceConstruction> conic_curve_blend(const NurbsCurveGeometry &first,
                                                     const NurbsCurveGeometry &control,
                                                     const NurbsCurveGeometry &last,
                                                     const double cross_weight,
                                                     const double max_error,
                                                     std::string *r_error)
{
  using namespace detail;
  auto fail = [&](const char *message) -> std::optional<SurfaceConstruction> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!supported_arithmetic() || first.points_num > INT_MAX / 3 || !(cross_weight > 0) ||
      !std::isfinite(cross_weight) || !std::isfinite(max_error) || max_error < 0)
  {
    return fail("NURBS conic blend requires positive finite weights and an error budget");
  }
  for (const auto *curve : {&first, &control, &last}) {
    if (!curve_validate_structure(*curve, r_error) || curve->dimension != 3 ||
        curve->order != first.order || curve->points_num != first.points_num ||
        curve->knots_num != first.knots_num ||
        !std::equal(curve->knots, curve->knots + curve->knots_num, first.knots))
    {
      return fail("NURBS conic blend requires compatible spatial curve bases");
    }
    for (const auto &point : Span(curve->points, curve->points_num)) {
      if (!(point.weight > 0)) {
        return fail("NURBS conic blend requires positive rational coefficients");
      }
    }
  }
  SurfaceConstruction result;
  result.surface.reset(MEM_new<NurbsSurfaceGeometry>(__func__));
  auto &surface = *result.surface;
  surface.order_u = first.order;
  surface.points_u = first.points_num;
  surface.knots_u_num = first.knots_num;
  surface.knots_u = MEM_dupalloc(first.knots);
  surface.order_v = surface.points_v = 3;
  surface.points_num = 3 * first.points_num;
  surface.knots_v_num = 6;
  surface.knots_v = MEM_new_array<double>(6, __func__);
  for (int i = 0; i < 6; i++) {
    surface.knots_v[i] = i < 3 ? 0 : 1;
  }
  surface.points = MEM_new_array<NurbsControlPoint>(3 * first.points_num, __func__);
  double max_relative_error = 0, extent = 0;
  const NurbsCurveGeometry *rows[] = {&first, &control, &last};
  for (int row = 0; row < 3; row++) {
    for (int i = 0; i < first.points_num; i++) {
      auto &point = surface.points[row * first.points_num + i];
      point = rows[row]->points[i];
      for (const double coordinate : point.position) {
        extent = std::max(extent, std::abs(coordinate));
      }
      if (row == 1) {
        const auto weight = bounded_multiply({point.weight}, {cross_weight});
        if (!weight.valid() || weight.value <= weight.error) {
          return fail("NURBS conic blend weight cannot be represented positively");
        }
        point.weight = weight.value;
        const auto relative = bounded_divide({weight.error},
                                             bounded_subtract({weight.value}, {weight.error}));
        max_relative_error = std::max(max_relative_error,
                                      add_upper(std::abs(relative.value), relative.error));
      }
    }
  }
  /* Only the middle-row weights change. A positive rational combination perturbed by
   * relative weight error e moves by at most 2 * coordinate_extent * e / (1-e), per axis. */
  const auto bound = bounded_divide(
      {multiply_upper(multiply_upper(6, extent), max_relative_error)},
      bounded_subtract({1}, {max_relative_error}));
  result.geometry_error_bound = add_upper(std::abs(bound.value), bound.error);
  if (!std::isfinite(result.geometry_error_bound) || result.geometry_error_bound > max_error) {
    return fail("NURBS conic blend exceeds the source construction error budget");
  }
  return result;
}

namespace {

/** A coordinate projection onto a regular plane. Selecting a well-conditioned 2D minor
 * avoids a sampled normal, an orthonormalization fit or a world-axis assumption. */
struct PlanarFrame {
  double3 origin;
  std::array<detail::BoundedScalar, 3> du, dv;
  detail::BoundedScalar determinant;
  int x = 0, y = 1;

  static std::optional<PlanarFrame> from_surface(const NurbsSurfaceGeometry &surface)
  {
    using namespace detail;
    PlanarFrame frame;
    frame.origin = double3(surface.points[0].position);
    for (int axis = 0; axis < 3; axis++) {
      frame.du[axis] = bounded_subtract({surface.points[1].position[axis]}, {frame.origin[axis]});
      frame.dv[axis] = bounded_subtract({surface.points[2].position[axis]}, {frame.origin[axis]});
    }
    for (int axis = 0; axis < 3; axis++) {
      const int a = (axis + 1) % 3, b = (axis + 2) % 3;
      const auto value = bounded_subtract(bounded_multiply(frame.du[a], frame.dv[b]),
                                          bounded_multiply(frame.du[b], frame.dv[a]));
      if (value.valid() && std::abs(value.value) > value.error &&
          std::abs(value.value) > std::abs(frame.determinant.value))
      {
        frame.x = a;
        frame.y = b;
        frame.determinant = value;
      }
    }
    return frame.determinant.value == 0 ? std::nullopt : std::optional(frame);
  }

  std::array<detail::BoundedScalar, 2> project(const double3 point) const
  {
    using namespace detail;
    const auto dx = bounded_subtract({point[x]}, {origin[x]});
    const auto dy = bounded_subtract({point[y]}, {origin[y]});
    return {
        bounded_divide(bounded_subtract(bounded_multiply(dx, dv[y]), bounded_multiply(dy, dv[x])),
                       determinant),
        bounded_divide(bounded_subtract(bounded_multiply(du[x], dy), bounded_multiply(du[y], dx)),
                       determinant)};
  }

  std::array<detail::BoundedScalar, 3> position(
      const std::array<detail::BoundedScalar, 2> &parameter) const
  {
    using namespace detail;
    std::array<BoundedScalar, 3> result;
    for (int axis = 0; axis < 3; axis++) {
      result[axis] = bounded_add({origin[axis]},
                                 bounded_add(bounded_multiply(du[axis], parameter[0]),
                                             bounded_multiply(dv[axis], parameter[1])));
    }
    return result;
  }

  double deviation(const double3 point) const
  {
    using namespace detail;
    const auto projected = position(project(point));
    double error = 0;
    for (int axis = 0; axis < 3; axis++) {
      const auto delta = bounded_subtract(projected[axis], {point[axis]});
      if (!delta.valid()) {
        return INFINITY;
      }
      error = add_upper(error, add_upper(std::abs(delta.value), delta.error));
    }
    return error;
  }
};

}  // namespace

std::optional<PlanarChartResult> reparameterize_affine_planes(const NurbsGeometry &source,
                                                              const Span<int64_t> faces,
                                                              const double max_error,
                                                              std::string *r_error)
{
  using namespace detail;
  auto fail = [&](const char *message) -> std::optional<PlanarChartResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!supported_arithmetic() || !std::isfinite(max_error) || max_error < 0) {
    return fail("NURBS planar charts require supported arithmetic and an error budget");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  const Set<int64_t> requested(faces);
  for (const int64_t id : requested) {
    if (!index->face(id)) {
      return fail("NURBS planar chart selection contains an unknown face");
    }
  }
  TopologyBuilder builder(source);
  builder.geometry->stored_selection = selection_storage_copy(source.stored_selection);
  PlanarChartResult result;
  for (const auto &input : source.faces) {
    if (!requested.contains(input.id)) {
      continue;
    }
    const auto &support = *builder.surfaces.lookup(input.surface_id);
    if (support.order_u != 2 || support.order_v != 2 || support.points_u != 2 ||
        support.points_v != 2 || support.knots_u[0] != support.knots_u[1] ||
        support.knots_u[2] != support.knots_u[3] || support.knots_v[0] != support.knots_v[1] ||
        support.knots_v[2] != support.knots_v[3])
    {
      return fail("NURBS planar charts require clamped polynomial bilinear supports");
    }
    for (const auto &point : Span(support.points, 4)) {
      if (!(point.weight > 0) || point.weight != support.points[0].weight) {
        return fail("NURBS planar charts require equal positive support weights");
      }
    }
    const auto frame = PlanarFrame::from_surface(support);
    if (!frame) {
      return fail("NURBS planar chart has no regular source frame");
    }
    double plane_error = 0;
    std::array<double2, 4> projected_support;
    const int boundary_order[] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
      const double3 point(support.points[boundary_order[i]].position);
      plane_error = std::max(plane_error, frame->deviation(point));
      const auto uv = frame->project(point);
      projected_support[i] = {uv[0].value, uv[1].value};
    }
    if (!std::isfinite(plane_error) || plane_error > max_error) {
      return fail("NURBS face is not planar within the explicit geometry error budget");
    }
    for (int i = 0; i < 4; i++) {
      if (orient2d(projected_support[i],
                   projected_support[(i + 1) % 4],
                   projected_support[(i + 2) % 4]) < 0)
      {
        return fail("NURBS planar chart cannot replace a folded source parameterization");
      }
    }
    double2 low(INFINITY), high(-INFINITY);
    double input_gap = 0;
    for (const auto &loop : input.loops) {
      for (const auto &use : loop.edge_uses) {
        const auto &edge = *index->edge(use.edge_id);
        const auto &curve = *builder.curves.lookup(edge.curve_id);
        const auto &pcurve = *builder.curves.lookup(use.pcurve_id);
        auto gap = linear_curve_support_deviation_bound(curve, pcurve, support);
        if (!gap) {
          gap = bilinear_curve_support_deviation_bound(curve, pcurve, support);
        }
        if (!gap || *gap > std::max({input.tolerance, use.tolerance, edge.tolerance})) {
          return fail("NURBS planar chart cannot certify its input boundary tolerance");
        }
        input_gap = std::max(input_gap, *gap);
        for (const auto &point : Span(curve.points, curve.points_num)) {
          if (!(point.weight > 0)) {
            return fail("NURBS planar charts require positive boundary weights");
          }
          const auto uv = frame->project(double3(point.position));
          for (int axis = 0; axis < 2; axis++) {
            if (!uv[axis].valid()) {
              return fail("NURBS planar chart boundary projection is not representable");
            }
            low[axis] = std::min(low[axis],
                                 std::nextafter(uv[axis].value - uv[axis].error, -INFINITY));
            high[axis] = std::max(high[axis],
                                  std::nextafter(uv[axis].value + uv[axis].error, INFINITY));
          }
        }
      }
    }
    if (!(low.x < high.x) || !(low.y < high.y)) {
      return fail("NURBS planar chart has no bounded two-dimensional boundary extent");
    }
    auto *replacement = builder.copy_surface(input.surface_id);
    if (!builder.valid()) {
      return fail("NURBS planar chart source identity range is exhausted");
    }
    replacement->knots_u[0] = replacement->knots_u[1] = 0;
    replacement->knots_v[0] = replacement->knots_v[1] = 0;
    replacement->knots_u[2] = replacement->knots_u[3] = 1;
    replacement->knots_v[2] = replacement->knots_v[3] = 1;
    double construction_error = 0;
    for (int corner = 0; corner < 4; corner++) {
      const auto point = frame->position(
          {{{corner & 1 ? high.x : low.x}, {corner & 2 ? high.y : low.y}}});
      double error = 0;
      for (int axis = 0; axis < 3; axis++) {
        if (!point[axis].valid()) {
          return fail("NURBS planar chart coefficients are not representable");
        }
        replacement->points[corner].position[axis] = point[axis].value;
        error = add_upper(error, point[axis].error);
      }
      replacement->points[corner].weight = 1;
      construction_error = std::max(construction_error, error);
    }
    double projection_error = 0;
    auto &face = *builder.faces.lookup(input.id);
    face.surface_id = replacement->id;
    for (auto &loop : face.loops) {
      for (auto &use : loop.edge_uses) {
        const auto &edge = *index->edge(use.edge_id);
        auto projected = project_curve_to_affine_surface(
            *builder.curves.lookup(edge.curve_id), *replacement, max_error, r_error);
        if (!projected) {
          return std::nullopt;
        }
        projection_error = std::max(projection_error, projected->geometry_error_bound);
        projected->curve->id = builder.id();
        if (!builder.valid()) {
          return fail("NURBS planar chart source identity range is exhausted");
        }
        use.pcurve_id = projected->curve->id;
        auto *installed = projected->curve.release();
        BLI_addtail(&builder.geometry->curves, installed);
        builder.curves.add_new(installed->id, installed);
        use.tolerance = std::max(use.tolerance, projection_error);
      }
    }
    face.tolerance = std::max(face.tolerance, projection_error);
    const double error = add_upper(add_upper(plane_error, input_gap),
                                   add_upper(construction_error, projection_error));
    if (!std::isfinite(error) || error > max_error) {
      return fail("NURBS planar chart exceeds the whole-face geometry error budget");
    }
    result.geometry_error_bound = std::max(result.geometry_error_bound, error);
  }
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

std::optional<QuadChartResult> reparameterize_planar_quads(const NurbsGeometry &source,
                                                           const Span<int64_t> faces,
                                                           const double max_error,
                                                           std::string *r_error)
{
  using namespace detail;
  auto fail = [&](const char *message) -> std::optional<QuadChartResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!supported_arithmetic() || !std::isfinite(max_error) || max_error < 0) {
    return fail("NURBS quad charts require supported arithmetic and an error budget");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  TopologyBuilder builder(source);
  builder.geometry->stored_selection = selection_storage_copy(source.stored_selection);
  QuadChartResult result;
  const Set<int64_t> requested(faces);
  for (const int64_t id : requested) {
    if (!index->face(id)) {
      return fail("NURBS quad chart selection contains an unknown face");
    }
  }
  for (const auto &source_face : source.faces) {
    const int64_t id = source_face.id;
    if (!requested.contains(id)) {
      continue;
    }
    const auto *input = &source_face;
    if (!input->loops.is_single() ||
        static_cast<const NurbsTopologyLoop *>(input->loops.first)->is_inner ||
        static_cast<const NurbsTopologyLoop *>(input->loops.first)->edge_uses.count() != 4)
    {
      return fail("NURBS quad charts require one four-sided outer boundary");
    }
    const auto &support = *builder.surfaces.lookup(input->surface_id);
    if (support.order_u != 2 || support.order_v != 2 || support.points_u != 2 ||
        support.points_v != 2 || support.knots_u[0] != support.knots_u[1] ||
        support.knots_u[2] != support.knots_u[3] || support.knots_v[0] != support.knots_v[1] ||
        support.knots_v[2] != support.knots_v[3])
    {
      return fail("NURBS quad reparameterization requires a polynomial bilinear support");
    }
    for (const auto &point : Span(support.points, 4)) {
      if (!(point.weight > 0) || point.weight != support.points[0].weight) {
        return fail("NURBS quad reparameterization requires equal positive support weights");
      }
    }
    double twist = 0, boundary_error = 0;
    for (int axis = 0; axis < 3; axis++) {
      const auto coefficient = bounded_add(
          bounded_subtract({support.points[3].position[axis]}, {support.points[1].position[axis]}),
          bounded_subtract({support.points[0].position[axis]},
                           {support.points[2].position[axis]}));
      twist = add_upper(twist, add_upper(std::abs(coefficient.value), coefficient.error));
    }
    std::array<const NurbsTopologyEdgeUse *, 4> uses;
    std::array<double2, 4> parameters;
    std::array<double3, 4> corners;
    int slot = 0;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(input->loops.first)->edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      const auto &curve = *builder.curves.lookup(edge.curve_id);
      const auto &pcurve = *builder.curves.lookup(use.pcurve_id);
      for (const auto *c : {&curve, &pcurve}) {
        if (c->order != 2 || c->points_num != 2 || c->knots_num != 4 ||
            c->knots[0] != edge.parameter_range[0] || c->knots[1] != edge.parameter_range[0] ||
            c->knots[2] != edge.parameter_range[1] || c->knots[3] != edge.parameter_range[1] ||
            !(c->points[0].weight > 0) || !(c->points[1].weight > 0))
        {
          return fail("NURBS quad charts require complete positive straight boundary curves");
        }
      }
      const auto gap = linear_curve_support_deviation_bound(curve, pcurve, support);
      if (!gap || *gap > std::max({input->tolerance, use.tolerance, edge.tolerance})) {
        return fail("NURBS quad chart cannot certify its input boundary tolerance");
      }
      boundary_error = std::max(boundary_error, *gap);
      for (int end = 0; end < 2; end++) {
        if (double3(curve.points[end].position) !=
            double3(index->vertex(edge.vertex_ids[end])->position))
        {
          return fail("NURBS quad charts require exact source endpoint associations");
        }
        const double2 uv(pcurve.points[end].position);
        if (uv.x < support.knots_u[1] || uv.x > support.knots_u[2] || uv.y < support.knots_v[1] ||
            uv.y > support.knots_v[2])
        {
          return fail("NURBS quad chart boundary lies outside its source support");
        }
      }
      uses[slot] = &use;
      parameters[slot] = double2(pcurve.points[use.reversed ? 1 : 0].position);
      corners[slot++] = double3(index->vertex(edge.vertex_ids[use.reversed ? 1 : 0])->position);
    }
    const int winding = orient2d(parameters[0], parameters[1], parameters[2]);
    for (int i = 0; i < 4; i++) {
      const auto &pcurve = *builder.curves.lookup(uses[i]->pcurve_id);
      if (double2(pcurve.points[uses[i]->reversed ? 0 : 1].position) != parameters[(i + 1) % 4]) {
        return fail("NURBS quad chart requires a closed source parameter boundary");
      }
      if (!winding ||
          orient2d(parameters[i], parameters[(i + 1) % 4], parameters[(i + 2) % 4]) != winding)
      {
        return fail("NURBS quad chart requires a strictly convex parameter boundary");
      }
    }
    /* Write S(q) = affine(q) + q.x*q.y*twist in normalized source parameters.
     * Both q.x*q.y and its corner interpolant lie in [0,1]. Their difference is
     * bounded by one. The source endpoint discrepancies are a convex combination.
     * Thus ||twist||_1 + max(boundary gap) bounds the entire new face's deviation. */
    const double error = add_upper(twist, boundary_error);
    result.geometry_error_bound = std::max(result.geometry_error_bound, error);
    if (!std::isfinite(error) || error > max_error) {
      return fail("NURBS quad reparameterization exceeds the whole-face error budget");
    }
    auto *replacement = builder.copy_surface(input->surface_id);
    replacement->knots_u[0] = replacement->knots_u[1] = 0;
    replacement->knots_v[0] = replacement->knots_v[1] = 0;
    replacement->knots_u[2] = replacement->knots_u[3] = 1;
    replacement->knots_v[2] = replacement->knots_v[3] = 1;
    double3 uv[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    if (winding < 0) {
      std::swap(uv[1], uv[3]);
    }
    for (int i = 0; i < 4; i++) {
      auto &point = replacement->points[int(uv[i].x) + 2 * int(uv[i].y)];
      std::copy_n(&corners[i].x, 3, point.position);
      point.weight = 1;
    }
    auto &face = *builder.faces.lookup(id);
    face.surface_id = replacement->id;
    int side = 0;
    for (auto &use : static_cast<NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      const auto &curve = *builder.curves.lookup(edge.curve_id);
      auto *parameter_curve = builder.copy_curve(use.pcurve_id);
      /* Boundary basis and projective speed come from the unchanged spatial line. */
      std::copy_n(curve.knots, 4, parameter_curve->knots);
      for (int end = 0; end < 2; end++) {
        const double3 value = uv[(side + (use.reversed ? 1 - end : end)) % 4];
        std::copy_n(&value.x, 3, parameter_curve->points[end].position);
        parameter_curve->points[end].weight = curve.points[end].weight;
      }
      use.pcurve_id = parameter_curve->id;
      const auto after = linear_curve_support_deviation_bound(
          curve, *parameter_curve, *replacement);
      if (!after || *after > max_error) {
        return fail("NURBS quad chart exceeds the new boundary error budget");
      }
      result.geometry_error_bound = std::max(result.geometry_error_bound, *after);
      use.tolerance = std::max(use.tolerance, *after);
      face.tolerance = std::max(face.tolerance, *after);
      side++;
    }
  }
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

std::optional<SurfaceConstruction> rational_coons_triangle(const NurbsCurveGeometry &ab,
                                                           const NurbsCurveGeometry &ac,
                                                           const NurbsCurveGeometry &bc,
                                                           const double max_error,
                                                           std::string *r_error,
                                                           const TriangleBlendShape *shape)
{
  using namespace detail;
  auto fail = [&](const char *message) -> std::optional<SurfaceConstruction> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!supported_arithmetic() || !std::isfinite(max_error) || max_error < 0) {
    return fail("NURBS Coons construction requires supported arithmetic and an error budget");
  }
  for (const auto *curve : {&ab, &ac, &bc}) {
    if (!curve_validate_structure(*curve, r_error) || curve->dimension != 3 || curve->order != 3 ||
        curve->points_num != 3 || curve->knots_num != 6 || curve->points[0].weight != 1 ||
        curve->points[2].weight != 1)
    {
      return fail("NURBS Coons triangle requires normalized rational quadratic boundaries");
    }
    for (int i = 0; i < 6; i++) {
      if (curve->knots[i] != (i < 3 ? 0 : 1)) {
        return fail("NURBS Coons boundaries require the full clamped unit parameter interval");
      }
    }
    for (const auto &point : Span(curve->points, 3)) {
      if (!(point.weight > 0)) {
        return fail("NURBS Coons boundaries require positive weights");
      }
    }
  }
  if (double3(ab.points[0].position) != double3(ac.points[0].position) ||
      double3(ab.points[2].position) != double3(bc.points[0].position) ||
      double3(ac.points[2].position) != double3(bc.points[2].position))
  {
    return fail("NURBS Coons boundaries must share their exact endpoint positions");
  }
  SurfaceConstruction result;
  result.surface.reset(MEM_new<NurbsSurfaceGeometry>(__func__));
  auto &surface = *result.surface;
  surface.order_u = surface.order_v = surface.points_u = surface.points_v = 3;
  surface.points_num = 9;
  surface.points = MEM_new_array<NurbsControlPoint>(9, __func__);
  surface.knots_u_num = surface.knots_v_num = 6;
  surface.knots_u = MEM_dupalloc(ab.knots);
  surface.knots_v = MEM_dupalloc(ac.knots);
  std::copy_n(ab.points, 3, surface.points);
  surface.points[3] = ac.points[1];
  surface.points[5] = bc.points[1];
  for (int i = 6; i < 9; i++) {
    surface.points[i] = ac.points[2];
  }
  if (shape) {
    surface.points[4] = shape->interior;
    surface.points[7].weight = shape->collapsed_weight;
    if (!(shape->interior.weight > 0) || !(shape->collapsed_weight > 0) ||
        !surface_validate_structure(surface, r_error))
    {
      return fail("NURBS triangular blend requires finite positive shape coefficients");
    }
    // All coefficients are copied exactly; the caller owns errors in constructing the shape.
    return result;
  }
  /* The Boolean sum of the four homogeneous boundary interpolants, with the top
   * collapsed to C. Its only new coefficient is
   * H11 = (H_AB1 + H_AC1 + H_BC1) / 2 - (H_A + H_B) / 4.
   * All other coefficients are the original boundary records. */
  BoundedScalar weight{-0.5};
  for (const auto *curve : {&ab, &ac, &bc}) {
    weight = bounded_add(weight, bounded_multiply({0.5}, {curve->points[1].weight}));
  }
  if (!weight.valid() || weight.value <= weight.error) {
    return fail("NURBS Coons triangle has no positive interior weight");
  }
  auto &middle = surface.points[4];
  middle.weight = weight.value;
  double error = 0, extent = 0;
  const double3 origin(ab.points[0].position);
  for (int axis = 0; axis < 3; axis++) {
    /* Translate before forming the sum to avoid coordinate-magnitude cancellation. */
    BoundedScalar numerator = bounded_multiply(
        {-0.25}, bounded_subtract({ab.points[2].position[axis]}, {origin[axis]}));
    for (const auto *curve : {&ab, &ac, &bc}) {
      numerator = bounded_add(
          numerator,
          bounded_multiply(bounded_multiply({0.5}, {curve->points[1].weight}),
                           bounded_subtract({curve->points[1].position[axis]}, {origin[axis]})));
    }
    const auto displacement = bounded_divide(numerator, weight);
    const auto coordinate = bounded_add({origin[axis]}, displacement);
    if (!coordinate.valid()) {
      return fail("NURBS Coons triangle has unrepresentable interior coefficients");
    }
    middle.position[axis] = coordinate.value;
    error = add_upper(error, coordinate.error);
    for (const auto &point : Span(surface.points, 9)) {
      if (&point != &middle) {
        const auto delta = bounded_subtract({point.position[axis]}, {origin[axis]});
        extent = add_upper(extent, add_upper(std::abs(delta.value), delta.error));
      }
    }
    extent = add_upper(extent, add_upper(std::abs(displacement.value), displacement.error));
  }
  double minimum_weight = weight.value - weight.error;
  for (const auto &point : Span(surface.points, 9)) {
    minimum_weight = std::min(minimum_weight, point.weight);
  }
  /* Positive rational coefficients enclose the whole patch. A perturbation of its
   * one interior weight contributes at most 2 * extent * dw / min(weight). */
  const auto perturbation = bounded_divide(bounded_multiply({2 * extent}, {weight.error}),
                                           {minimum_weight});
  result.geometry_error_bound = add_upper(
      error, add_upper(std::abs(perturbation.value), perturbation.error));
  if (!perturbation.valid() || !std::isfinite(result.geometry_error_bound) ||
      result.geometry_error_bound > max_error || !surface_validate_structure(surface, r_error))
  {
    return fail("NURBS Coons triangle exceeds the source construction error budget");
  }
  return result;
}

std::optional<SurfaceConstruction> rational_octant_surface(const double3 &center,
                                                           const std::array<double3, 3> &axes,
                                                           const double max_error,
                                                           std::string *r_error)
{
  using namespace detail;
  auto fail = [&](const char *message) -> std::optional<SurfaceConstruction> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!supported_arithmetic() || !std::isfinite(max_error) || max_error < 0) {
    return fail("Rational octant construction requires supported arithmetic and an error budget");
  }
  const double weight = std::sqrt(0.5);
  const auto residual = bounded_subtract(bounded_multiply({weight}, {weight}), {0.5});
  const auto quotient = bounded_divide({add_upper(std::abs(residual.value), residual.error)},
                                       {weight});
  const double weight_error = add_upper(std::abs(quotient.value), quotient.error);
  SurfaceConstruction result;
  result.surface.reset(MEM_new<NurbsSurfaceGeometry>(__func__));
  auto &surface = *result.surface;
  surface.order_u = surface.order_v = surface.points_u = surface.points_v = 3;
  surface.points_num = 9;
  surface.points = MEM_new_array<NurbsControlPoint>(9, __func__);
  surface.knots_u_num = surface.knots_v_num = 6;
  surface.knots_u = MEM_new_array_zeroed<double>(6, __func__);
  surface.knots_v = MEM_new_array_zeroed<double>(6, __func__);
  surface.knots_u[3] = surface.knots_u[4] = surface.knots_u[5] = 1;
  surface.knots_v[3] = surface.knots_v[4] = surface.knots_v[5] = 1;
  const int coefficients[9][3] = {{1, 0, 0},
                                  {1, 1, 0},
                                  {0, 1, 0},
                                  {1, 0, 1},
                                  {1, 1, 1},
                                  {0, 1, 1},
                                  {0, 0, 1},
                                  {0, 0, 1},
                                  {0, 0, 1}};
  double extent = 0;
  for (const auto &axis : axes) {
    for (int i = 0; i < 3; i++) {
      extent = add_upper(extent, std::abs(axis[i]));
    }
  }
  for (int i = 0; i < 9; i++) {
    auto &point = surface.points[i];
    double error = 0;
    for (int coordinate = 0; coordinate < 3; coordinate++) {
      BoundedScalar value{center[coordinate]};
      for (int axis = 0; axis < 3; axis++) {
        if (coefficients[i][axis]) {
          value = bounded_add(value, {axes[axis][coordinate]});
        }
      }
      if (!value.valid()) {
        return fail("Rational octant exceeds representable source coordinates");
      }
      point.position[coordinate] = value.value;
      error = add_upper(error, value.error);
    }
    const int u = i % 3, v = i / 3;
    point.weight = u == 1 && v == 1 ? 0.5 : u == 1 || v == 1 ? weight : 1;
    result.geometry_error_bound = std::max(result.geometry_error_bound, error);
  }
  const auto perturbation = bounded_divide(bounded_multiply({2 * extent}, {weight_error}),
                                           bounded_subtract({weight}, {weight_error}));
  result.geometry_error_bound = add_upper(
      result.geometry_error_bound, add_upper(std::abs(perturbation.value), perturbation.error));
  if (!std::isfinite(result.geometry_error_bound) || result.geometry_error_bound > max_error ||
      !surface_validate_structure(surface, r_error))
  {
    return fail("Rational octant exceeds the source construction error budget");
  }
  return result;
}

std::optional<SurfaceConstruction> rational_spherical_octant_surface(
    const double3 &center,
    const std::array<double3, 3> &axes,
    const double radius,
    const double max_error,
    std::string *r_error)
{
  using namespace detail;
  auto fail = [&](const char *message) -> std::optional<SurfaceConstruction> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!supported_arithmetic() || !std::isfinite(radius) || !(radius > 0)) {
    return fail(
        "Spherical NURBS octant requires a finite positive radius and supported arithmetic");
  }
  /* For unit x on the reference octant, |length(Ax)^2 - r^2| is at most
   * ||A^T A - r^2 I||_infinity. Dividing by r bounds radial distance because
   * length(Ax) + r >= r. This covers the entire support, rather than sampled points. */
  const auto radius_squared = bounded_multiply({radius}, {radius});
  double gram_bound = 0;
  for (int i = 0; i < 3; i++) {
    double row_bound = 0;
    for (int j = 0; j < 3; j++) {
      BoundedScalar product;
      for (int axis = 0; axis < 3; axis++) {
        product = bounded_add(product, bounded_multiply({axes[i][axis]}, {axes[j][axis]}));
      }
      if (i == j) {
        product = bounded_subtract(product, radius_squared);
      }
      if (!product.valid()) {
        return fail("Spherical NURBS octant has no finite radial error bound");
      }
      row_bound = add_upper(row_bound, add_upper(std::abs(product.value), product.error));
    }
    gram_bound = std::max(gram_bound, row_bound);
  }
  const auto radial_bound = bounded_divide({gram_bound}, {radius});
  auto result = rational_octant_surface(center, axes, max_error, r_error);
  if (!result) {
    return std::nullopt;
  }
  result->geometry_error_bound = add_upper(
      result->geometry_error_bound, add_upper(std::abs(radial_bound.value), radial_bound.error));
  if (!radial_bound.valid() || result->geometry_error_bound > max_error) {
    return fail("Spherical NURBS octant radial deviation exceeds the construction error budget");
  }
  return result;
}

OwnedSurface ruled_surface(const NurbsCurveGeometry &first,
                           const NurbsCurveGeometry &second,
                           std::string *r_error)
{
  auto fail = [&](const char *message) -> OwnedSurface {
    if (r_error) {
      *r_error = message;
    }
    return nullptr;
  };
  if (!curve_validate_structure(first, r_error) || !curve_validate_structure(second, r_error)) {
    return nullptr;
  }
  if (first.dimension != 3 || second.dimension != 3 || first.points_num > INT_MAX / 2 ||
      first.order != second.order || first.points_num != second.points_num ||
      first.knots_num != second.knots_num ||
      !std::equal(first.knots, first.knots + first.knots_num, second.knots))
  {
    return fail("Ruled NURBS construction requires matching spatial curve bases");
  }
  for (int i = 0; i < first.points_num; i++) {
    if (!(first.points[i].weight > 0) || !(second.points[i].weight > 0)) {
      return fail("Ruled NURBS construction requires positive weights");
    }
  }
  OwnedSurface result(MEM_new<NurbsSurfaceGeometry>(__func__));
  result->order_u = first.order;
  result->points_u = first.points_num;
  result->order_v = result->points_v = 2;
  result->points_num = first.points_num * 2;
  result->points = MEM_new_array<NurbsControlPoint>(result->points_num, __func__);
  std::copy_n(first.points, first.points_num, result->points);
  std::copy_n(second.points, second.points_num, result->points + first.points_num);
  result->knots_u_num = first.knots_num;
  result->knots_u = MEM_dupalloc(first.knots);
  result->knots_v_num = 4;
  result->knots_v = MEM_new_array_zeroed<double>(4, __func__);
  result->knots_v[2] = result->knots_v[3] = 1;
  return result;
}

std::optional<PlanarCurveProjection> project_curve_to_affine_surface(
    const NurbsCurveGeometry &curve,
    const NurbsSurfaceGeometry &surface,
    const double max_error,
    std::string *r_error)
{
  using namespace detail;
  auto fail = [&](const char *message) -> std::optional<PlanarCurveProjection> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!supported_arithmetic() || !std::isfinite(max_error) || max_error < 0 ||
      !curve_validate_structure(curve, r_error) || !surface_validate_structure(surface, r_error))
  {
    return fail("Planar NURBS projection requires valid records, arithmetic and an error budget");
  }
  if (curve.dimension != 3 || surface.order_u != 2 || surface.order_v != 2 ||
      surface.points_u != 2 || surface.points_v != 2 || surface.knots_u[0] != surface.knots_u[1] ||
      surface.knots_u[2] != surface.knots_u[3] || surface.knots_v[0] != surface.knots_v[1] ||
      surface.knots_v[2] != surface.knots_v[3])
  {
    return fail("Planar NURBS projection requires a clamped 2x2 tensor support");
  }
  for (int i = 0; i < 4; i++) {
    if (!(surface.points[i].weight > 0) || surface.points[i].weight != surface.points[0].weight) {
      return fail("Planar NURBS projection requires a polynomial support");
    }
  }
  const auto frame = PlanarFrame::from_surface(surface);
  if (!frame) {
    return fail("Planar NURBS projection has no regular affine coordinate frame");
  }
  PlanarCurveProjection result;
  result.curve.reset(MEM_new<NurbsCurveGeometry>(__func__));
  auto &pcurve = *result.curve;
  pcurve.dimension = 2;
  pcurve.order = curve.order;
  pcurve.points_num = curve.points_num;
  pcurve.knots_num = curve.knots_num;
  pcurve.knots = MEM_dupalloc(curve.knots);
  pcurve.points = MEM_dupalloc(curve.points);
  for (int i = 0; i < curve.points_num; i++) {
    if (!(curve.points[i].weight > 0)) {
      return fail("Planar NURBS projection requires positive curve weights");
    }
    const auto fractions = frame->project(double3(curve.points[i].position));
    for (int axis = 0; axis < 2; axis++) {
      const double *knots = axis == 0 ? surface.knots_u : surface.knots_v;
      const auto uv = bounded_add(
          {knots[1]}, bounded_multiply(fractions[axis], bounded_subtract({knots[2]}, {knots[1]})));
      if (!uv.valid()) {
        return fail("Planar NURBS projection exceeds representable coordinates");
      }
      double parameter = uv.value;
      const double minimum = knots[1], maximum = knots[2];
      double model_span = 0;
      for (int coordinate = 0; coordinate < 3; coordinate++) {
        const BoundedScalar &basis = axis == 0 ? frame->du[coordinate] : frame->dv[coordinate];
        model_span = add_upper(model_span, add_upper(std::abs(basis.value), basis.error));
      }
      if (parameter < minimum) {
        const double correction = multiply_upper(minimum - parameter, model_span);
        if (minimum - parameter > uv.error && correction > max_error) {
          return fail("Planar NURBS projection falls outside the support domain/error budget");
        }
        parameter = minimum;
      }
      else if (parameter > maximum) {
        const double correction = multiply_upper(parameter - maximum, model_span);
        if (parameter - maximum > uv.error && correction > max_error) {
          return fail("Planar NURBS projection falls outside the support domain/error budget");
        }
        parameter = maximum;
      }
      pcurve.points[i].position[axis] = parameter;
    }
    pcurve.points[i].position[2] = 0;
  }
  const auto curve_bound = bilinear_curve_support_deviation_bound(curve, pcurve, surface);
  if (!curve_bound) {
    return fail("Planar NURBS projection could not bound its bilinear source correspondence");
  }
  result.geometry_error_bound = *curve_bound;
  if (result.geometry_error_bound > max_error) {
    return fail("Planar NURBS projection exceeds the source construction error budget");
  }
  return result;
}

}  // namespace blender::bke::nurbs
