/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string_view>

#include <fmt/format.h>

#include "BLI_delaunay_2d.hh"
#include "BLI_listbase.h"
#include "BLI_math_boolean.hh"
#include "BLI_math_vector.hh"

#include "BKE_nurbs_selection.hh"
#include "BKE_nurbs_tessellate.hh"

namespace blender::bke::nurbs {

int SurfacePreview::points_num() const
{
  return int(boundary.positions.size() + interior.size());
}

double3 SurfacePreview::position(const int index) const
{
  BLI_assert(index >= 0 && index < this->points_num());
  return index < boundary.positions.size() ? boundary.positions[index] :
                                             interior[index - boundary.positions.size()].position;
}

enum class PolygonLocation { Outside, Inside, Boundary };

/** No divisions or proximity welding; robust orientation on the normalized preview coordinates. */
static PolygonLocation polygon_location(const double2 &point,
                                        const Span<int> polygon,
                                        const Span<double2> positions)
{
  bool inside = false;
  for (const int i : polygon.index_range()) {
    const double2 a = positions[polygon[i]];
    const double2 b = positions[polygon[(i + 1) % polygon.size()]];
    const int side = orient2d(a, b, point);
    if (side == 0 && point.x >= std::min(a.x, b.x) && point.x <= std::max(a.x, b.x) &&
        point.y >= std::min(a.y, b.y) && point.y <= std::max(a.y, b.y))
    {
      return PolygonLocation::Boundary;
    }
    if ((a.y <= point.y && b.y > point.y && side > 0) ||
        (b.y <= point.y && a.y > point.y && side < 0))
    {
      inside = !inside;
    }
  }
  return inside ? PolygonLocation::Inside : PolygonLocation::Outside;
}

/** Direction at an extreme corner, avoiding an area sum that can cancel on a thin polygon. */
static int polygon_orientation(const Span<int> polygon, const Span<double2> positions)
{
  int extreme = 0;
  for (const int i : polygon.index_range()) {
    const double2 a = positions[polygon[i]], b = positions[polygon[extreme]];
    if (a.x < b.x || (a.x == b.x && a.y < b.y)) {
      extreme = i;
    }
  }
  const double2 a = positions[polygon[(extreme + polygon.size() - 1) % polygon.size()]];
  const double2 b = positions[polygon[extreme]];
  for (int step = 1; step < polygon.size(); step++) {
    const int side = orient2d(a, b, positions[polygon[(extreme + step) % polygon.size()]]);
    if (side != 0) {
      return side;
    }
  }
  return 0;
}

/** Detect a rectangular chart from its sampled trim, independently of face IDs, selection and
 * the number of topology edges along each side. Test in UV, so curved supports and periodic
 * seams retain their U/V grid too. Holes and nonrectangular trims use a triangular display grid.
 * This classifies display samples only; it never changes the source trim or parameterization. */
static bool rectangular_chart(const Span<int> polygon,
                              const Span<double2> parameters,
                              const double2 minimum,
                              const double2 maximum)
{
  for (const int i : polygon.index_range()) {
    const double2 a = parameters[polygon[i]];
    const double2 b = parameters[polygon[(i + 1) % polygon.size()]];
    const bool u_side = a.x == b.x && (a.x == minimum.x || a.x == maximum.x);
    const bool v_side = a.y == b.y && (a.y == minimum.y || a.y == maximum.y);
    if (!u_side && !v_side) {
      return false;
    }
  }
  return true;
}

static std::optional<Vector<double>> axis_samples(const double minimum,
                                                  const double maximum,
                                                  const Span<double> knots,
                                                  const int subdivisions,
                                                  const int max_points,
                                                  std::string *r_error)
{
  Vector<double> breaks;
  breaks.append(minimum);
  for (const double knot : knots) {
    if (knot > minimum && knot < maximum && knot != breaks.last()) {
      breaks.append(knot);
    }
  }
  breaks.append(maximum);
  const int64_t count = (breaks.size() - 1) * subdivisions - 1;
  if (count > max_points) {
    if (r_error != nullptr) {
      *r_error = "NURBS surface preview axis exceeds its workspace limit";
    }
    return std::nullopt;
  }
  Vector<double> result;
  double previous = minimum;
  for (const int span : IndexRange(breaks.size() - 1)) {
    for (int step = 1; step < subdivisions; step++) {
      const double value = std::lerp(breaks[span], breaks[span + 1], double(step) / subdivisions);
      if (value <= previous || value >= breaks[span + 1]) {
        if (r_error != nullptr) {
          *r_error = "NURBS surface preview grid is not representable in double precision";
        }
        return std::nullopt;
      }
      result.append(value);
      previous = value;
    }
    if (span < breaks.size() - 2) {
      result.append(breaks[span + 1]);
      previous = breaks[span + 1];
    }
  }
  return result;
}

std::optional<SurfacePreview> tessellate_surfaces(const NurbsGeometry &source,
                                                  const SurfacePreviewOptions &options,
                                                  std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const std::string_view message) -> std::optional<SurfacePreview> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  const double epsilon = options.normalized_merge_epsilon;
  if (options.interior_subdivisions < 1 || options.max_points < 1 || options.max_triangles < 0 ||
      !std::isfinite(epsilon) || epsilon <= 0 || epsilon > 1 || epsilon * epsilon == 0)
  {
    return fail("Invalid NURBS surface preview options");
  }
  auto boundary = evaluate_boundaries(source, options.boundary, r_error);
  if (!boundary) {
    return std::nullopt;
  }
  if (boundary->positions.size() > options.max_points) {
    return fail("NURBS surface preview exceeds its point limit");
  }
  SurfacePreview result;
  result.boundary = std::move(*boundary);
  Map<int64_t, const EvaluatedBoundaryUse *> uses;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const EvaluatedBoundaryUse &use : result.boundary.uses) {
    uses.add_new(use.use_id, &use);
  }
  for (const NurbsSurfaceGeometry &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (const NurbsTopologyFace &face : source.faces) {
    const NurbsSurfaceGeometry &surface = *surfaces.lookup(face.surface_id);
    Vector<double2> uv;
    Vector<int> input_to_global;
    Vector<Vector<int>> loops;
    int outer_loop = -1;
    for (const NurbsTopologyLoop &loop : face.loops) {
      Vector<int> polygon;
      double2 first_uv(0), previous_uv(0);
      int first_global = -1, previous_global = -1;
      bool first = true;
      for (const NurbsTopologyEdgeUse &source_use : loop.edge_uses) {
        const EvaluatedBoundaryUse &use = *uses.lookup(source_use.id);
        const auto &samples = result.boundary.edges[use.edge_index].samples;
        const int start = use.reversed ? int(samples.size()) - 1 : 0;
        const int end = use.reversed ? 0 : int(samples.size()) - 1;
        if (!first && use.parameters[start] != previous_uv) {
          const int global = samples[start].point_index;
          if (global != previous_global ||
              !collapsed_surface_boundary_interval(
                  surface, previous_uv, use.parameters[start], result.boundary.positions[global]))
          {
            return fail(fmt::format(
                "NURBS face {} loop {} has an unsupported open UV chart", face.id, loop.id));
          }
          if (uv.size() >= std::numeric_limits<int>::max()) {
            return fail("NURBS pole chart exceeds the triangulator input index range");
          }
          polygon.append(int(uv.size()));
          uv.append(previous_uv);
          input_to_global.append(global);
        }
        if (first) {
          first_uv = use.parameters[start];
          first_global = samples[start].point_index;
          first = false;
        }
        const int direction = use.reversed ? -1 : 1;
        for (int i = start; i != end; i += direction) {
          if (uv.size() >= std::numeric_limits<int>::max()) {
            return fail("NURBS surface preview exceeds the triangulator input index range");
          }
          polygon.append(int(uv.size()));
          uv.append(use.parameters[i]);
          input_to_global.append(samples[i].point_index);
        }
        previous_uv = use.parameters[end];
        previous_global = samples[end].point_index;
      }
      if (previous_uv != first_uv) {
        if (previous_global != first_global ||
            !collapsed_surface_boundary_interval(
                surface, previous_uv, first_uv, result.boundary.positions[first_global]))
        {
          return fail(fmt::format(
              "NURBS face {} loop {} has a degenerate or open UV boundary", face.id, loop.id));
        }
        if (uv.size() >= std::numeric_limits<int>::max()) {
          return fail("NURBS pole chart exceeds the triangulator input index range");
        }
        polygon.append(int(uv.size()));
        uv.append(previous_uv);
        input_to_global.append(first_global);
      }
      if (polygon.size() < 3) {
        return fail(fmt::format(
            "NURBS face {} loop {} has a degenerate or open UV boundary", face.id, loop.id));
      }
      if (!loop.is_inner) {
        outer_loop = int(loops.size());
      }
      loops.append(std::move(polygon));
    }
    int64_t max_loop_size = 0;
    for (const Vector<int> &polygon : loops) {
      max_loop_size = std::max(max_loop_size, polygon.size());
    }
    if (std::numeric_limits<uint32_t>::max() / max_loop_size <= loops.size()) {
      return fail("NURBS surface preview exceeds the triangulator constraint identity range");
    }
    double2 minimum = uv.first(), maximum = uv.first();
    for (const double2 point : uv) {
      minimum = math::min(minimum, point);
      maximum = math::max(maximum, point);
    }
    const bool quad_grid = loops.size() == 1 &&
                           rectangular_chart(loops[outer_loop], uv, minimum, maximum);
    const double2 origin(std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y));
    const double scale = std::max(
        {maximum.x - origin.x, maximum.y - origin.y, origin.x - minimum.x, origin.y - minimum.y});
    if (!std::isfinite(scale) || scale == 0) {
      return fail(fmt::format("NURBS face {} UV normalization is degenerate", face.id));
    }
    Vector<double2> normalized;
    for (const double2 point : uv) {
      normalized.append((point - origin) / scale);
    }
    for (Vector<int> &polygon : loops) {
      const int direction = polygon_orientation(polygon, normalized);
      if (direction == 0) {
        return fail(fmt::format("NURBS face {} sampled UV polygon is degenerate", face.id));
      }
      if (direction < 0) {
        std::reverse(polygon.begin(), polygon.end());
      }
    }
    for (const int i : loops.index_range()) {
      if (i == outer_loop) {
        continue;
      }
      const double2 point = normalized[loops[i].first()];
      if (polygon_location(point, loops[outer_loop], normalized) != PolygonLocation::Inside) {
        return fail(
            fmt::format("NURBS face {} has an unsupported exterior/touching hole", face.id));
      }
      for (const int j : loops.index_range()) {
        if (j != i && j != outer_loop &&
            polygon_location(point, loops[j], normalized) != PolygonLocation::Outside)
        {
          return fail(fmt::format("NURBS face {} has unsupported nested/touching holes", face.id));
        }
      }
    }
    /* Bound workspace before constructing the grid; the final point/triangle limits are also
     * checked after trimming. Do not allocate an unbounded grid outside a small trimmed face. */
    const auto samples_u = axis_samples(minimum.x,
                                        maximum.x,
                                        {surface.knots_u, surface.knots_u_num},
                                        options.interior_subdivisions,
                                        options.max_points,
                                        r_error);
    const auto samples_v = axis_samples(minimum.y,
                                        maximum.y,
                                        {surface.knots_v, surface.knots_v_num},
                                        options.interior_subdivisions,
                                        options.max_points,
                                        r_error);
    if (!samples_u || !samples_v) {
      return std::nullopt;
    }
    if (samples_u->size() * samples_v->size() > options.max_points) {
      return fail("NURBS surface preview grid exceeds its workspace limit");
    }
    for (const double y : *samples_v) {
      for (const double x : *samples_u) {
        const double2 parameter(x, y);
        const double2 point = (parameter - origin) / scale;
        if (polygon_location(point, loops[outer_loop], normalized) != PolygonLocation::Inside) {
          continue;
        }
        bool in_hole = false;
        for (const int i : loops.index_range()) {
          if (i != outer_loop &&
              polygon_location(point, loops[i], normalized) != PolygonLocation::Outside)
          {
            in_hole = true;
            break;
          }
        }
        if (!in_hole) {
          if (uv.size() >= std::numeric_limits<int>::max()) {
            return fail("NURBS surface preview exceeds the triangulator input index range");
          }
          uv.append(parameter);
          normalized.append(point);
          input_to_global.append(-1);
        }
      }
    }
    meshintersect::CDT_input<double> input;
    input.vert = normalized.as_span();
    input.face = loops.as_span();
    input.epsilon = epsilon;
    input.need_ids = true;
    const auto cdt = meshintersect::delaunay_2d_calc(input, CDT_INSIDE_WITH_HOLES);
    Vector<int> output_to_input;
    for (const int i : cdt.vert.index_range()) {
      if (cdt.vert_orig[i].size() != 1 || cdt.vert_orig[i][0] >= input.vert.size()) {
        return fail(
            fmt::format("NURBS face {} triangulation merged or created UV points", face.id));
      }
      const int original = int(cdt.vert_orig[i][0]);
      if (cdt.vert[i] != input.vert[original]) {
        return fail(fmt::format("NURBS face {} triangulation moved a UV point", face.id));
      }
      output_to_input.append(original);
    }
    Vector<Vector<int>> constraint_counts;
    for (const Vector<int> &polygon : loops) {
      constraint_counts.append(Vector<int>(polygon.size(), 0));
    }
    for (const int i : cdt.edge.index_range()) {
      if (cdt.edge_orig[i].is_empty()) {
        continue;
      }
      if (cdt.edge_orig[i].size() != 1 || cdt.face_edge_offset == 0) {
        return fail(
            fmt::format("NURBS face {} triangulation overlaps boundary constraints", face.id));
      }
      const uint32_t encoded = cdt.edge_orig[i][0];
      const int polygon_index = int(encoded / cdt.face_edge_offset) - 1;
      const int corner = int(encoded % cdt.face_edge_offset);
      if (polygon_index < 0 || polygon_index >= loops.size() ||
          corner >= loops[polygon_index].size())
      {
        return fail("NURBS surface preview received invalid constraint provenance");
      }
      const auto &polygon = loops[polygon_index];
      const int a = output_to_input[cdt.edge[i].first];
      const int b = output_to_input[cdt.edge[i].second];
      const int expected_a = polygon[corner], expected_b = polygon[(corner + 1) % polygon.size()];
      if (!((a == expected_a && b == expected_b) || (a == expected_b && b == expected_a))) {
        return fail(
            fmt::format("NURBS face {} triangulation split a boundary sample interval", face.id));
      }
      constraint_counts[polygon_index][corner]++;
    }
    for (const Vector<int> &counts : constraint_counts) {
      if (std::any_of(counts.begin(), counts.end(), [](const int count) { return count != 1; })) {
        return fail(
            fmt::format("NURBS face {} triangulation lost or split a boundary interval", face.id));
      }
    }
    for (const Vector<int> &triangle : cdt.face) {
      if (triangle.size() != 3) {
        return fail("NURBS surface preview triangulator returned a non-triangle");
      }
      const int direction = orient2d(normalized[output_to_input[triangle[0]]],
                                     normalized[output_to_input[triangle[1]]],
                                     normalized[output_to_input[triangle[2]]]);
      if (direction == 0) {
        return fail("NURBS surface preview triangulator returned a degenerate UV triangle");
      }
      SurfacePreviewTriangle evaluated;
      evaluated.face_id = face.id;
      for (const int i : IndexRange(3)) {
        const int original = output_to_input[triangle[i]];
        int &global = input_to_global[original];
        if (global < 0) {
          if (result.points_num() >= options.max_points) {
            return fail("NURBS surface preview exceeds its point limit");
          }
          const auto point = evaluate_surface(surface, uv[original], r_error);
          if (!point) {
            return std::nullopt;
          }
          global = result.points_num();
          result.interior.append({point->position, face.id, uv[original]});
          result.max_interior_weight_condition = std::max(result.max_interior_weight_condition,
                                                          point->weight_condition);
        }
        evaluated.points[i] = global;
        evaluated.parameters[i] = uv[original];
      }
      if (evaluated.points[0] == evaluated.points[1] ||
          evaluated.points[1] == evaluated.points[2] || evaluated.points[2] == evaluated.points[0])
      {
        result.skipped_repeated_point_triangles++;
        continue;
      }
      if (result.triangles.size() >= options.max_triangles) {
        return fail("NURBS surface preview exceeds its triangle limit");
      }
      if (direction != face.orientation) {
        std::swap(evaluated.points[1], evaluated.points[2]);
        std::swap(evaluated.parameters[1], evaluated.parameters[2]);
      }
      for (const int i : IndexRange(3)) {
        const double2 a = evaluated.parameters[i];
        const double2 b = evaluated.parameters[(i + 1) % 3];
        evaluated.display_edges[i] = !quad_grid || a.x == b.x || a.y == b.y;
      }
      result.triangles.append(evaluated);
    }
  }
  return result;
}

Map<int64_t, double3> face_display_centers(const NurbsGeometry &source,
                                           const SurfacePreview &preview)
{
  Vector<ElementRef> faces;
  for (const NurbsTopologyFace &face : source.faces) {
    faces.append({ElementDomain::Face, face.id});
  }
  const auto centers = element_centers(source, faces);
  Map<int64_t, double3> result;
  if (!centers) {
    return result;
  }
  Map<int64_t, int> face_indices;
  Vector<double> closest;
  Vector<bool> on_face;
  for (const int i : faces.index_range()) {
    face_indices.add_new(faces[i].id, i);
    closest.append(std::numeric_limits<double>::infinity());
    on_face.append(false);
  }
  for (const auto &triangle : preview.triangles) {
    const int i = face_indices.lookup(triangle.face_id);
    if (on_face[i]) {
      continue;
    }
    const double3 a = preview.position(triangle.points.x);
    const double3 b = preview.position(triangle.points.y);
    const double3 c = preview.position(triangle.points.z);
    const double3 center = (*centers)[i];
    const double3 ab = b - a, ac = c - a, ap = center - a;
    const double3 normal = math::cross(ab, ac);
    const double normal_squared = math::length_squared(normal);
    const double scale = std::max(
        {math::length_squared(ab), math::length_squared(ac), math::length_squared(ap)});
    const double plane_distance = math::dot(ap, normal);
    // This is a display-anchor classification slack, never a source tolerance or fit budget.
    constexpr double slack = 64 * std::numeric_limits<double>::epsilon();
    if (normal_squared > 0 &&
        plane_distance * plane_distance <= slack * slack * scale * normal_squared)
    {
      const double u = math::dot(math::cross(ap, ac), normal) / normal_squared;
      const double v = math::dot(math::cross(ab, ap), normal) / normal_squared;
      if (u >= -slack && v >= -slack && u + v <= 1 + slack) {
        result.add_overwrite(triangle.face_id, center);
        on_face[i] = true;
        continue;
      }
    }
    const double3 candidate = a / 3 + b / 3 + c / 3;
    const double distance = math::length_squared(candidate - center);
    if (distance < closest[i]) {
      closest[i] = distance;
      result.add_overwrite(triangle.face_id, candidate);
    }
  }
  return result;
}

}  // namespace blender::bke::nurbs
