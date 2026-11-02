/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "BKE_nurbs_curve_bevel.hh"
#include "BKE_nurbs_evaluate.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.hh"

namespace blender::bke::nurbs {

std::optional<CurveControlBevelResult> bevel_curve_controls(const NurbsGeometry &source,
                                                            const SelectionState &selection,
                                                            const BevelOptions &options,
                                                            std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<CurveControlBevelResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!std::isfinite(options.offset) || options.offset < 0 || options.segments < 0 ||
      options.segments > 100 || !control_point_selection_validate(source, selection, r_error))
  {
    return fail("Control-polygon bevel requires selected handles and a finite nonnegative width");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  struct Corner {
    int64_t curve;
    int point;
    double3 direction[2];
    double lengths[2], weight;
  };
  Vector<Corner> corners;
  Map<int64_t, const NurbsCurveGeometry *> curves;
  for (const auto &curve : source.curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const auto &ref : selection.control_points) {
    const auto &curve = *curves.lookup(ref.curve_id);
    const int i = ref.point_index;
    if (i == 0 || i == curve.points_num - 1) {
      continue;
    }
    int owners = 0;
    for (const auto &edge : source.edges) {
      if (edge.curve_id != curve.id) {
        continue;
      }
      owners++;
      if (!index->uses_for_edge(edge.id).is_empty()) {
        return fail(
            "Beveling surface boundary controls needs a compatible surface control-net edit");
      }
    }
    if (owners == 0) {
      return fail("Control-polygon bevel needs a wire curve support");
    }
    for (const int j : IndexRange(curve.order)) {
      if (curve.knots[j] != curve.knots[curve.order - 1] ||
          curve.knots[curve.points_num + j] != curve.knots[curve.points_num])
      {
        return fail("Control-polygon bevel currently requires a clamped NURBS curve");
      }
    }
    Corner corner{};
    corner.curve = curve.id;
    corner.point = i;
    for (const int side : IndexRange(2)) {
      const double3 delta = double3(curve.points[i + (side ? 1 : -1)].position) -
                            double3(curve.points[i].position);
      corner.lengths[side] = math::length(delta);
      if (!(corner.lengths[side] > 0) || !std::isfinite(corner.lengths[side])) {
        return fail("Control-polygon bevel cannot round coincident neighboring handles");
      }
      corner.direction[side] = delta / corner.lengths[side];
    }
    const double dot = std::clamp(math::dot(corner.direction[0], corner.direction[1]), -1.0, 1.0);
    if (dot < -1 + 1e-12) {
      continue;
    }
    if (dot > 1 - 1e-12) {
      return fail("Control-polygon bevel cannot round overlapping cage edges");
    }
    corner.weight = std::sqrt((1 - dot) / 2);
    if (std::none_of(corners.begin(), corners.end(), [&](const Corner &other) {
          return other.curve == corner.curve && other.point == corner.point;
        }))
    {
      corners.append(corner);
    }
  }
  if (corners.is_empty()) {
    return fail("Select an interior control-polygon corner to bevel");
  }
  double limit = std::numeric_limits<double>::max();
  for (const auto &corner : corners) {
    for (const int side : IndexRange(2)) {
      const int neighbor = corner.point + (side ? 1 : -1);
      const bool selected_neighbor = std::any_of(
          corners.begin(), corners.end(), [&](const Corner &other) {
            return other.curve == corner.curve && other.point == neighbor;
          });
      limit = std::min(limit, corner.lengths[side] / (selected_neighbor ? 2 : 1));
    }
  }
  limit *= 1 - 1e-6;
  const double width = options.clamp_overlap ? std::min(options.offset, limit) : options.offset;
  if (width > limit) {
    return fail("Control-polygon bevel width overlaps neighboring handles");
  }
  CurveControlBevelResult result;
  result.offset_info.offset = width;
  result.offset_info.limit = limit;
  result.snapshot = {OwnedGeometry(geometry_copy(&source)), selection};
  selection_storage_free(result.snapshot.geometry->stored_selection);
  result.snapshot.geometry->stored_selection = nullptr;
  if (width == 0) {
    return result;
  }
  /* A rounded control cage needs a middle handle even when the curve has no topology split.
   * Higher segment counts retain the existing cage-profile sampling. */
  const int profile_segments = options.segments == 0 ? 1 : std::max(2, options.segments);
  auto &selected = result.snapshot.selection;
  selected.control_points.clear();
  selected.active_control_point.reset();
  Set<int64_t> changed_curves;
  for (auto &curve : result.snapshot.geometry->curves) {
    if (std::none_of(corners.begin(), corners.end(), [&](const Corner &corner) {
          return corner.curve == curve.id;
        }))
    {
      continue;
    }
    changed_curves.add(curve.id);
    Vector<NurbsControlPoint> points;
    Vector<double> knots(Span(curve.knots, curve.knots_num));
    const double low = curve.knots[curve.order - 1], high = curve.knots[curve.points_num];
    auto greville = [&](const int i) {
      double value = 0;
      for (int j = 1; j < curve.order; j++) {
        value += curve.knots[i + j] / double(curve.order - 1);
      }
      return value;
    };
    for (const int i : IndexRange(curve.points_num)) {
      const auto corner = std::find_if(corners.begin(), corners.end(), [&](const Corner &item) {
        return item.curve == curve.id && item.point == i;
      });
      const CurveControlPointRef old_ref{curve.id, i};
      if (corner == corners.end()) {
        const CurveControlPointRef mapped{curve.id, int(points.size())};
        if (selection.control_points.contains(old_ref)) {
          selected.control_points.append(mapped);
          if (selection.active_control_point == old_ref) {
            selected.active_control_point = mapped;
          }
        }
        points.append(curve.points[i]);
        continue;
      }
      const double3 origin(curve.points[i].position);
      const double3 a = origin + width * corner->direction[0];
      const double3 b = origin + width * corner->direction[1];
      const double wa = std::lerp(
          curve.points[i].weight, curve.points[i - 1].weight, width / corner->lengths[0]);
      const double wb = std::lerp(
          curve.points[i].weight, curve.points[i + 1].weight, width / corner->lengths[1]);
      for (const int j : IndexRange(profile_segments + 1)) {
        const double t = double(j) / profile_segments;
        double3 position;
        if (options.segments == 0 || j == 0 || j == profile_segments) {
          position = j == 0 ? a : b;
        }
        else {
          const double middle = 2 * corner->weight * t * (1 - t);
          const double denominator = (1 - t) * (1 - t) + middle + t * t;
          position = a + (origin - a) * (middle / denominator) + (b - a) * (t * t / denominator);
        }
        NurbsControlPoint point = curve.points[i];
        std::copy_n(&position.x, 3, point.position);
        point.weight = std::lerp(wa, wb, t);
        selected.control_points.append({curve.id, int(points.size())});
        if (selection.active_control_point == old_ref) {
          selected.active_control_point = selected.control_points.last();
        }
        points.append(point);
      }
      /* Add local basis intervals for the additional cage vertices. Retain existing knots
       * and the full clamped domain; unrelated control coefficients remain bit-identical. */
      double left = std::max(low, std::midpoint(greville(i - 1), greville(i)));
      double right = std::min(high, std::midpoint(greville(i), greville(i + 1)));
      if (!(left < right)) {
        left = low;
        right = high;
      }
      for (const int j : IndexRange(profile_segments)) {
        double parameter = std::lerp(left, right, double(j + 1) / (profile_segments + 1));
        /* Keep distinct knots far enough apart for boundary/display subdivision. Moving a
         * duplicate by one ULP creates an interval that cannot be sampled reliably. */
        const double clearance = 128 * std::numeric_limits<double>::epsilon() *
                                 std::max({std::abs(low), std::abs(high), high - low});
        for (const double knot : knots) {
          if (std::abs(knot - parameter) <= clearance) {
            parameter = knot;
            break;
          }
        }
        if (knots.contains(parameter)) {
          double below = left, above = right;
          for (const double knot : knots) {
            if (knot < parameter) {
              below = std::max(below, knot);
            }
            if (knot > parameter) {
              above = std::min(above, knot);
            }
          }
          parameter = parameter - below >= above - parameter ? std::midpoint(below, parameter) :
                                                               std::midpoint(parameter, above);
        }
        if (!(low < parameter && parameter < high && parameter < right)) {
          return fail("Control-polygon bevel has no representable local knot interval");
        }
        knots.append(parameter);
      }
    }
    std::sort(knots.begin(), knots.end());
    MEM_delete(curve.points);
    MEM_delete(curve.knots);
    curve.points_num = points.size();
    curve.knots_num = knots.size();
    curve.points = MEM_new_array<NurbsControlPoint>(points.size(), __func__);
    curve.knots = MEM_new_array<double>(knots.size(), __func__);
    std::copy(points.begin(), points.end(), curve.points);
    std::copy(knots.begin(), knots.end(), curve.knots);
    if (!curve_validate_structure(curve, r_error)) {
      return std::nullopt;
    }
  }
  Map<int64_t, double3> requests;
  for (const auto &edge : result.snapshot.geometry->edges) {
    if (!changed_curves.contains(edge.curve_id)) {
      continue;
    }
    const NurbsCurveGeometry *curve = nullptr;
    for (const auto &item : result.snapshot.geometry->curves) {
      if (item.id == edge.curve_id) {
        curve = &item;
        break;
      }
    }
    for (const int end : IndexRange(2)) {
      auto evaluated = evaluate_curve(*curve, edge.parameter_range[end], r_error);
      if (!evaluated) {
        return std::nullopt;
      }
      const int64_t vertex = edge.vertex_ids[end];
      /* A restricted support can carry endpoint evaluation roundoff already covered by its
       * boundary tolerance. If this edit leaves its evaluated endpoint unchanged, preserve the
       * authoritative shared topology vertex rather than moving its other incident curves. */
      const auto before = evaluate_curve(*curves.lookup(edge.curve_id),
                                          edge.parameter_range[end], r_error);
      if (!before) {
        return std::nullopt;
      }
      const double3 target = before->position == evaluated->position ?
                                 double3(index->vertex(vertex)->position) : evaluated->position;
      if (const auto *previous = requests.lookup_ptr(vertex)) {
        if (*previous != target) {
          return fail("Edited wire intervals request conflicting shared vertex positions");
        }
      }
      for (const int64_t incident : index->edges_for_vertex(vertex)) {
        if (!changed_curves.contains(index->edge(incident)->curve_id) &&
            target != double3(index->vertex(vertex)->position))
        {
          return fail(
              "Beveling controls at an attached wire junction needs a shared deformation rule");
        }
      }
      requests.add_overwrite(vertex, target);
    }
  }
  for (auto &vertex : result.snapshot.geometry->vertices) {
    if (const auto *position = requests.lookup_ptr(vertex.id)) {
      std::copy_n(&position->x, 3, vertex.position);
    }
  }
  if (!geometry_validate_structure(*result.snapshot.geometry, r_error) ||
      !control_point_selection_validate(*result.snapshot.geometry, selected, r_error) ||
      !evaluate_boundaries(*result.snapshot.geometry, {}, r_error))
  {
    return std::nullopt;
  }
  return result;
}

}  // namespace blender::bke::nurbs
