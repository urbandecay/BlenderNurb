/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <algorithm>
#include <cmath>
#include <limits>

#include "DNA_nurbs_topology_types.h"
#include "DNA_nurbs_types.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_nurbs.hh"
#include "BKE_object.hh"

#include "ED_transform_snap_object_context.hh"

#include "transform_snap_object.hh"

namespace blender::ed::transform {

static const NurbsCurve *find_curve(const NurbsGeometry &geometry, const uint64_t curve_id)
{
  for (const NurbsCurve &curve : geometry.curves) {
    if (curve.id == curve_id) {
      return &curve;
    }
  }
  return nullptr;
}

static float projected_distance_squared(const SnapData &snap_data, const float3 &position)
{
  if (!snap_data.clip_planes.is_empty() &&
      !isect_point_planes_v3_negated(
          reinterpret_cast<const float (*)[4]>(snap_data.clip_planes.data()),
          snap_data.clip_planes.size(),
          position))
  {
    return std::numeric_limits<float>::infinity();
  }

  const float(*pmat)[4] = snap_data.nearest_precalc.pmat;
  float2 projected(float(dot_m4_v3_row_x(pmat, position) + pmat[3][0]),
                   float(dot_m4_v3_row_y(pmat, position) + pmat[3][1]));
  if (snap_data.is_persp) {
    const float w = mul_project_m4_v3_zfac(pmat, position);
    if (std::abs(w) <= FLT_EPSILON) {
      return std::numeric_limits<float>::infinity();
    }
    projected /= w;
  }
  const float2 cursor(snap_data.nearest_precalc.mval[0], snap_data.nearest_precalc.mval[1]);
  return len_squared_v2v2(projected, cursor);
}

static bool evaluate_projected_distance(const NurbsCurve &curve,
                                        const SnapData &snap_data,
                                        const double parameter,
                                        float &r_distance_squared,
                                        float3 &r_position)
{
  double evaluated[3];
  if (!BKE_nurbs_curve_evaluate(curve, parameter, evaluated)) {
    return false;
  }
  r_position = float3(float(evaluated[0]), float(evaluated[1]), float(evaluated[2]));
  r_distance_squared = projected_distance_squared(snap_data, r_position);
  return true;
}

static bool curve_nearest_to_cursor(const NurbsCurve &curve,
                                    const double domain_start,
                                    const double domain_end,
                                    const SnapData &snap_data,
                                    float3 &r_position)
{
  if (!BKE_nurbs_curve_is_valid(curve) || !std::isfinite(domain_start) ||
      !std::isfinite(domain_end) || domain_end <= domain_start ||
      domain_start < curve.domain[0] || domain_end > curve.domain[1])
  {
    return false;
  }

  Vector<double> breakpoints;
  breakpoints.append(domain_start);
  for (int i = 0; i < curve.knot_count; i++) {
    const double knot = curve.knots[i];
    if (knot > domain_start && knot < domain_end &&
        (breakpoints.is_empty() || knot != breakpoints.last()))
    {
      breakpoints.append(knot);
    }
  }
  breakpoints.append(domain_end);

  constexpr double golden_ratio = 0.61803398874989484820;
  constexpr int sample_count_min = 8;
  constexpr int refinement_steps = 28;
  float best_distance_squared = std::numeric_limits<float>::infinity();
  double best_parameter = domain_start;
  const int sample_count = std::max(sample_count_min, curve.degree * 8);

  for (int span = 0; span + 1 < breakpoints.size(); span++) {
    const double span_start = breakpoints[span];
    const double span_end = breakpoints[span + 1];
    if (span_end <= span_start) {
      continue;
    }

    Vector<float> sample_distances(sample_count + 1);
    for (int sample = 0; sample <= sample_count; sample++) {
      const double parameter = span_start + (span_end - span_start) *
                                                 (double(sample) / sample_count);
      float distance_squared;
      float3 position;
      if (!evaluate_projected_distance(
              curve, snap_data, parameter, distance_squared, position))
      {
        return false;
      }
      sample_distances[sample] = distance_squared;
      if (distance_squared < best_distance_squared) {
        best_distance_squared = distance_squared;
        best_parameter = parameter;
        r_position = position;
      }
    }

    for (int sample = 1; sample < sample_count; sample++) {
      if (sample_distances[sample] > sample_distances[sample - 1] ||
          sample_distances[sample] > sample_distances[sample + 1])
      {
        continue;
      }

      double left = span_start + (span_end - span_start) * (double(sample - 1) / sample_count);
      double right = span_start + (span_end - span_start) * (double(sample + 1) / sample_count);
      double first = right - (right - left) * golden_ratio;
      double second = left + (right - left) * golden_ratio;
      float first_distance, second_distance;
      float3 first_position, second_position;
      if (!evaluate_projected_distance(
              curve, snap_data, first, first_distance, first_position) ||
          !evaluate_projected_distance(
              curve, snap_data, second, second_distance, second_position))
      {
        return false;
      }

      for (int iteration = 0; iteration < refinement_steps; iteration++) {
        if (first_distance <= second_distance) {
          right = second;
          second = first;
          second_distance = first_distance;
          second_position = first_position;
          first = right - (right - left) * golden_ratio;
          if (!evaluate_projected_distance(
                  curve, snap_data, first, first_distance, first_position))
          {
            return false;
          }
        }
        else {
          left = first;
          first = second;
          first_distance = second_distance;
          first_position = second_position;
          second = left + (right - left) * golden_ratio;
          if (!evaluate_projected_distance(
                  curve, snap_data, second, second_distance, second_position))
          {
            return false;
          }
        }
      }

      if (first_distance < best_distance_squared) {
        best_distance_squared = first_distance;
        best_parameter = first;
        r_position = first_position;
      }
      if (second_distance < best_distance_squared) {
        best_distance_squared = second_distance;
        best_parameter = second;
        r_position = second_position;
      }
    }
  }

  return std::isfinite(best_distance_squared) && best_parameter >= domain_start &&
         best_parameter <= domain_end;
}

eSnapMode snapNurbs(SnapObjectContext *sctx, const Object *ob_eval, const float4x4 &obmat)
{
  constexpr eSnapMode native_snap_flags = eSnapMode(SCE_SNAP_TO_POINT | SCE_SNAP_TO_EDGE |
                                                    SCE_SNAP_TO_EDGE_ENDPOINT |
                                                    SCE_SNAP_TO_EDGE_MIDPOINT);
  const eSnapMode snap_flags = eSnapMode(sctx->runtime.snap_to_flag & native_snap_flags);
  if (snap_flags == SCE_SNAP_TO_NONE) {
    return SCE_SNAP_TO_NONE;
  }

  const NurbsGeometry *geometry = id_cast<const NurbsGeometry *>(ob_eval->data);
  if (geometry == nullptr) {
    return SCE_SNAP_TO_NONE;
  }

  SnapData nearest2d(sctx, obmat);
  nearest2d.clip_planes_enable(sctx, ob_eval, true);

  const bool use_obedit = BKE_object_is_in_editmode(ob_eval);
  const bool skip_selected =
      use_obedit &&
      (sctx->runtime.params.snap_target_select & SCE_SNAP_TARGET_NOT_SELECTED) != 0;
  eSnapMode result = SCE_SNAP_TO_NONE;
  auto try_snap = [&](const float3 &position, const eSnapMode snap_mode, const int index) {
    const float previous_distance = nearest2d.nearest_point.dist_sq;
    if (nearest2d.snap_point(position, index) &&
        nearest2d.nearest_point.dist_sq < previous_distance)
    {
      result = snap_mode;
    }
  };

  if (geometry->topology != nullptr) {
    if (snap_flags & SCE_SNAP_TO_POINT) {
      for (const NurbsTopoVertex &vertex : geometry->topology->vertices) {
        if (skip_selected && (vertex.flag & NURBS_TOPO_SELECT) != 0) {
          continue;
        }
        try_snap(float3(float(vertex.position[0]),
                        float(vertex.position[1]),
                        float(vertex.position[2])),
                 SCE_SNAP_TO_POINT,
                 -1);
      }
    }

    for (const NurbsTopoEdge &edge : geometry->topology->edges) {
      if (skip_selected && (edge.flag & NURBS_TOPO_SELECT) != 0) {
        continue;
      }
      const NurbsCurve *curve = find_curve(*geometry, edge.curve_id);
      if (curve == nullptr) {
        continue;
      }
      if (snap_flags & SCE_SNAP_TO_EDGE_ENDPOINT) {
        for (const uint64_t vertex_id : edge.vertex_ids) {
          for (const NurbsTopoVertex &vertex : geometry->topology->vertices) {
            if (vertex.id == vertex_id) {
              try_snap(float3(float(vertex.position[0]),
                              float(vertex.position[1]),
                              float(vertex.position[2])),
                       SCE_SNAP_TO_EDGE_ENDPOINT,
                       -1);
              break;
            }
          }
        }
      }
      if (snap_flags & SCE_SNAP_TO_EDGE_MIDPOINT) {
        float3 midpoint;
        float distance_squared;
        if (evaluate_projected_distance(*curve,
                                        nearest2d,
                                        (edge.domain[0] + edge.domain[1]) * 0.5,
                                        distance_squared,
                                        midpoint))
        {
          try_snap(midpoint, SCE_SNAP_TO_EDGE_MIDPOINT, -1);
        }
      }
      if (snap_flags & SCE_SNAP_TO_EDGE) {
        float3 closest;
        if (curve_nearest_to_cursor(
                *curve, edge.domain[0], edge.domain[1], nearest2d, closest))
        {
          try_snap(closest, SCE_SNAP_TO_EDGE, -1);
        }
      }
    }
  }
  else {
    int curve_index = 0;
    for (const NurbsCurve &curve : geometry->curves) {
      if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
        curve_index++;
        continue;
      }
      bool has_selected_control_vertex = false;
      for (const NurbsControlVertex &control_vertex :
           Span<NurbsControlVertex>(curve.control_vertices, curve.control_vertex_count))
      {
        has_selected_control_vertex |= (control_vertex.flag & NURBS_CV_SELECT) != 0;
        if ((snap_flags & SCE_SNAP_TO_POINT) &&
            !(skip_selected && (control_vertex.flag & NURBS_CV_SELECT) != 0))
        {
          try_snap(float3(float(control_vertex.position[0]),
                          float(control_vertex.position[1]),
                          float(control_vertex.position[2])),
                   SCE_SNAP_TO_POINT,
                   curve_index);
        }
      }

      if (!(skip_selected && has_selected_control_vertex)) {
        if (snap_flags & SCE_SNAP_TO_EDGE_ENDPOINT) {
          for (const double parameter : curve.domain) {
            float3 endpoint;
            float distance_squared;
            if (evaluate_projected_distance(
                    curve, nearest2d, parameter, distance_squared, endpoint))
            {
              try_snap(endpoint, SCE_SNAP_TO_EDGE_ENDPOINT, curve_index);
            }
          }
        }
        if (snap_flags & SCE_SNAP_TO_EDGE_MIDPOINT) {
          float3 midpoint;
          float distance_squared;
          if (evaluate_projected_distance(curve,
                                          nearest2d,
                                          (curve.domain[0] + curve.domain[1]) * 0.5,
                                          distance_squared,
                                          midpoint))
          {
            try_snap(midpoint, SCE_SNAP_TO_EDGE_MIDPOINT, curve_index);
          }
        }
        if (snap_flags & SCE_SNAP_TO_EDGE) {
          float3 closest;
          if (curve_nearest_to_cursor(
                  curve, curve.domain[0], curve.domain[1], nearest2d, closest))
          {
            try_snap(closest, SCE_SNAP_TO_EDGE, curve_index);
          }
        }
      }
      curve_index++;
    }
  }

  if (result == SCE_SNAP_TO_NONE) {
    return SCE_SNAP_TO_NONE;
  }

  nearest2d.register_result(sctx, ob_eval, &geometry->id);
  return result;
}

}  // namespace blender::ed::transform
