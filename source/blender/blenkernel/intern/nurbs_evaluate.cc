/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <string_view>

#include <fmt/format.h>

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_nurbs_evaluate.hh"

#include "nurbs_math.hh"

namespace blender::bke::nurbs {

static std::nullopt_t evaluation_error(std::string *r_error, const std::string_view message)
{
  if (r_error != nullptr) {
    *r_error = message;
  }
  return std::nullopt;
}

static bool positive_weights(const Span<NurbsControlPoint> points)
{
  return std::all_of(points.begin(), points.end(), [](const NurbsControlPoint &point) {
    return point.weight > 0.0;
  });
}

static bool continuous_knots(const double *knots,
                             const int knots_num,
                             const int order,
                             const double start,
                             const double end)
{
  for (int i = 0; i < knots_num;) {
    int next = i + 1;
    while (next < knots_num && knots[next] == knots[i]) {
      next++;
    }
    /* Scalar evaluation takes the right limit at interior knots. At a restricted edge's end,
     * that would join the left span to a potentially disconnected right-hand endpoint. */
    const bool before_end = knots[i] < end || (knots[i] == end && end < knots[knots_num - order]);
    if (knots[i] > start && before_end && next - i >= order) {
      return false;
    }
    i = next;
  }
  return true;
}

/** A measured distance, not an outward-rounded error certificate. Avoid squaring overflow. */
static double measured_distance(const double3 &a, const double3 &b)
{
  return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

std::optional<BoundaryEvaluation> evaluate_boundaries(const NurbsGeometry &source,
                                                      const BoundaryEvaluationOptions &options,
                                                      std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const std::string_view message) { return evaluation_error(r_error, message); };
  if (!detail::supported_arithmetic()) {
    return fail("NURBS boundary evaluation requires round-to-nearest and subnormal support");
  }
  if (options.segments_per_span < 1 || options.max_sample_entries < 1 ||
      !std::isfinite(options.evaluation_allowance) || options.evaluation_allowance < 0.0)
  {
    return fail("Invalid NURBS boundary sampling options");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : source.curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }

  BoundaryEvaluation result;
  int64_t remaining = options.max_sample_entries;
  auto consume = [&](const int64_t count) {
    if (count > remaining) {
      return false;
    }
    remaining -= count;
    return true;
  };
  Map<int64_t, int> vertex_indices;
  Map<int64_t, int> edge_indices;
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (!consume(1)) {
      return fail("NURBS boundary sampling exceeds the output entry limit");
    }
    vertex_indices.add_new(vertex.id, int(result.positions.size()));
    result.positions.append(double3(vertex.position));
    result.vertex_ids.append(vertex.id);
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    Set<double> breakpoints;
    breakpoints.add(edge.parameter_range[0]);
    breakpoints.add(edge.parameter_range[1]);
    auto add_curve_breaks = [&](const NurbsCurveGeometry &curve) {
      if (!positive_weights({curve.points, curve.points_num}) ||
          !continuous_knots(curve.knots,
                            curve.knots_num,
                            curve.order,
                            edge.parameter_range[0],
                            edge.parameter_range[1]))
      {
        return false;
      }
      for (int i = 0; i < curve.knots_num; i++) {
        if (curve.knots[i] > edge.parameter_range[0] && curve.knots[i] < edge.parameter_range[1]) {
          breakpoints.add(curve.knots[i]);
          if (breakpoints.size() > remaining) {
            return false;
          }
        }
      }
      return true;
    };
    const NurbsCurveGeometry &curve = *curves.lookup(edge.curve_id);
    if (!add_curve_breaks(curve)) {
      return fail(
          "NURBS boundary curve requires positive weights, continuity and sampling capacity");
    }
    for (const EdgeUseReference &reference : index->uses_for_edge(edge.id)) {
      if (!add_curve_breaks(*curves.lookup(reference.use->pcurve_id))) {
        return fail(
            "NURBS boundary p-curve requires positive weights, continuity and sampling capacity");
      }
    }
    Vector<double> breaks;
    for (const double parameter : breakpoints) {
      breaks.append(parameter);
    }
    std::sort(breaks.begin(), breaks.end());
    const int64_t sample_count = 1 + (breaks.size() - 1) * options.segments_per_span;
    if (!consume(sample_count)) {
      return fail("NURBS boundary sampling exceeds the output entry limit");
    }
    EvaluatedBoundaryEdge evaluated;
    evaluated.edge_id = edge.id;
    auto append_sample = [&](const double parameter, const int endpoint) {
      const auto point = evaluate_curve(curve, parameter, r_error);
      if (!point) {
        if (r_error != nullptr) {
          *r_error = fmt::format(
              "NURBS boundary edge {} at {:.17g}: {}", edge.id, parameter, *r_error);
        }
        return false;
      }
      int point_index;
      if (endpoint >= 0) {
        const NurbsTopologyVertex &vertex = *index->vertex(edge.vertex_ids[endpoint]);
        point_index = vertex_indices.lookup(vertex.id);
        const double deviation = measured_distance(point->position, double3(vertex.position));
        evaluated.max_endpoint_deviation = std::max(evaluated.max_endpoint_deviation, deviation);
        if (!std::isfinite(deviation) ||
            deviation > detail::add_upper(std::max(edge.tolerance, vertex.tolerance),
                                          options.evaluation_allowance))
        {
          evaluation_error(
              r_error,
              fmt::format("NURBS edge {} endpoint {}: measured deviation {:.17g} "
                          "exceeds allowed agreement {:.17g}",
                          edge.id,
                          vertex.id,
                          deviation,
                          detail::add_upper(std::max(edge.tolerance, vertex.tolerance),
                                            options.evaluation_allowance)));
          return false;
        }
      }
      else {
        point_index = int(result.positions.size());
        result.positions.append(point->position);
        result.vertex_ids.append(0);
      }
      evaluated.samples.append({parameter, point_index, point->position});
      evaluated.max_weight_condition = std::max(evaluated.max_weight_condition,
                                                point->weight_condition);
      return true;
    };
    if (!append_sample(breaks.first(), 0)) {
      return std::nullopt;
    }
    for (const int span : IndexRange(breaks.size() - 1)) {
      for (int step = 1; step <= options.segments_per_span; step++) {
        const double t = double(step) / options.segments_per_span;
        const double parameter = std::lerp(breaks[span], breaks[span + 1], t);
        if (parameter <= evaluated.samples.last().parameter ||
            (step < options.segments_per_span && parameter >= breaks[span + 1]))
        {
          return fail("NURBS boundary subdivision is not representable in double precision");
        }
        const bool is_end = span == breaks.size() - 2 && step == options.segments_per_span;
        if (!append_sample(parameter, is_end ? 1 : -1)) {
          return std::nullopt;
        }
      }
    }
    edge_indices.add_new(edge.id, int(result.edges.size()));
    result.edges.append(std::move(evaluated));
  }
  for (const NurbsTopologyFace &face : source.faces) {
    const NurbsSurfaceGeometry &surface = *surfaces.lookup(face.surface_id);
    if (!positive_weights({surface.points, surface.points_num}) ||
        !continuous_knots(surface.knots_u,
                          surface.knots_u_num,
                          surface.order_u,
                          surface.knots_u[surface.order_u - 1],
                          surface.knots_u[surface.points_u]) ||
        !continuous_knots(surface.knots_v,
                          surface.knots_v_num,
                          surface.order_v,
                          surface.knots_v[surface.order_v - 1],
                          surface.knots_v[surface.points_v]))
    {
      return fail("NURBS boundary support requires positive weights and continuity");
    }
    for (const NurbsTopologyLoop &loop : face.loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const int edge_index = edge_indices.lookup(use.edge_id);
        const EvaluatedBoundaryEdge &evaluated_edge = result.edges[edge_index];
        if (!consume(evaluated_edge.samples.size())) {
          return fail("NURBS boundary sampling exceeds the output entry limit");
        }
        const NurbsTopologyEdge &edge = *index->edge(use.edge_id);
        const NurbsCurveGeometry &pcurve = *curves.lookup(use.pcurve_id);
        EvaluatedBoundaryUse evaluated;
        evaluated.face_id = face.id;
        evaluated.loop_id = loop.id;
        evaluated.use_id = use.id;
        evaluated.edge_index = edge_index;
        evaluated.reversed = use.reversed;
        evaluated.is_inner = loop.is_inner;
        const double tolerance = std::max({edge.tolerance, use.tolerance, face.tolerance});
        for (const BoundarySample &sample : evaluated_edge.samples) {
          const auto uv = evaluate_curve(pcurve, sample.parameter, r_error);
          if (!uv) {
            if (r_error != nullptr) {
              *r_error = fmt::format("NURBS face {} use {} p-curve at {:.17g}: {}",
                                     face.id,
                                     use.id,
                                     sample.parameter,
                                     *r_error);
            }
            return std::nullopt;
          }
          const auto support = evaluate_surface(surface, double2(uv->position), r_error);
          if (!support) {
            if (r_error != nullptr) {
              *r_error = fmt::format("NURBS face {} use {} support at ({:.17g}, {:.17g}): {}",
                                     face.id,
                                     use.id,
                                     uv->position.x,
                                     uv->position.y,
                                     *r_error);
            }
            return std::nullopt;
          }
          const double curve_deviation = measured_distance(sample.curve_position,
                                                           support->position);
          const double display_deviation = measured_distance(result.positions[sample.point_index],
                                                             support->position);
          const int64_t vertex_id = result.vertex_ids[sample.point_index];
          const double display_tolerance = vertex_id == 0 ?
                                               tolerance :
                                               std::max(tolerance,
                                                        index->vertex(vertex_id)->tolerance);
          if (!std::isfinite(curve_deviation) || !std::isfinite(display_deviation) ||
              curve_deviation > detail::add_upper(tolerance, options.evaluation_allowance) ||
              display_deviation >
                  detail::add_upper(display_tolerance, options.evaluation_allowance))
          {
            return fail(
                fmt::format("NURBS face {} use {} edge/support at {:.17g}: measured curve/display "
                            "deviations {:.17g}/{:.17g} exceed allowed agreement {:.17g}/{:.17g}",
                            face.id,
                            use.id,
                            sample.parameter,
                            curve_deviation,
                            display_deviation,
                            detail::add_upper(tolerance, options.evaluation_allowance),
                            detail::add_upper(display_tolerance, options.evaluation_allowance)));
          }
          evaluated.parameters.append(double2(uv->position));
          evaluated.max_curve_support_deviation = std::max(evaluated.max_curve_support_deviation,
                                                           curve_deviation);
          evaluated.max_display_support_deviation = std::max(
              evaluated.max_display_support_deviation, display_deviation);
          evaluated.max_weight_condition = std::max(
              {evaluated.max_weight_condition, uv->weight_condition, support->weight_condition});
        }
        result.uses.append(std::move(evaluated));
      }
    }
  }
  return result;
}

}  // namespace blender::bke::nurbs
