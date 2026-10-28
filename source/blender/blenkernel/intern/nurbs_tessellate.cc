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
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_time.h"

#include "BKE_nurbs_debug.hh"
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

/** A complete Cartesian UV grid needs no general constrained triangulation. Every
 * cell is an empty rectangle, so either diagonal is Delaunay. Require the exact
 * existing perimeter and sample coordinates, with no merging or added samples. */
static std::optional<Vector<int3>> rectangular_grid_triangles(const Span<int> polygon,
                                                              Vector<double2> &parameters,
                                                              Vector<double2> &normalized,
                                                              Vector<int> &input_to_global,
                                                              const double2 origin,
                                                              const double scale,
                                                              const int max_points,
                                                              const double2 minimum,
                                                              const double2 maximum,
                                                              const Span<double> samples_u,
                                                              const Span<double> samples_v,
                                                              const double epsilon)
{
  Vector<double> coordinates[2];
  coordinates[0].append(minimum.x);
  coordinates[0].extend(samples_u);
  coordinates[0].append(maximum.x);
  coordinates[1].append(minimum.y);
  coordinates[1].extend(samples_v);
  coordinates[1].append(maximum.y);
  const int nx = coordinates[0].size(), ny = coordinates[1].size();
  if (int64_t(nx) * ny > max_points || parameters.size() != polygon.size() ||
      polygon.size() != 2LL * (nx + int64_t(ny)) - 4)
  {
    return std::nullopt;
  }
  Vector<int> grid(int64_t(nx) * ny, -1);
  for (const int i : parameters.index_range()) {
    int cell[2];
    for (const int axis : IndexRange(2)) {
      const auto &values = coordinates[axis];
      const auto found = std::lower_bound(values.begin(), values.end(), parameters[i][axis]);
      if (found == values.end() || *found != parameters[i][axis]) {
        return std::nullopt;
      }
      cell[axis] = found - values.begin();
    }
    if (cell[0] != 0 && cell[0] != nx - 1 && cell[1] != 0 && cell[1] != ny - 1) {
      return std::nullopt;
    }
    int &slot = grid[cell[1] * nx + cell[0]];
    if (slot != -1) {
      return std::nullopt;
    }
    slot = i;
  }
  Vector<int> perimeter;
  for (int i = 0; i < nx - 1; i++) {
    perimeter.append(grid[i]);
  }
  for (int j = 0; j < ny - 1; j++) {
    perimeter.append(grid[j * nx + nx - 1]);
  }
  for (int i = nx - 1; i > 0; i--) {
    perimeter.append(grid[(ny - 1) * nx + i]);
  }
  for (int j = ny - 1; j > 0; j--) {
    perimeter.append(grid[j * nx]);
  }
  if (perimeter.contains(-1)) {
    return std::nullopt;
  }
  const int first = perimeter.first_index_of_try(polygon.first());
  if (first < 0) {
    return std::nullopt;
  }
  for (const int i : polygon.index_range()) {
    if (polygon[i] != perimeter[(int64_t(first) + i) % perimeter.size()]) {
      return std::nullopt;
    }
  }
  /* Keep close or collapsed normalized coordinates on the ordinary CDT path,
   * where its epsilon/provenance rejection rules continue to apply. */
  for (int i = 1; i < nx; i++) {
    if (normalized[grid[i]].x - normalized[grid[i - 1]].x <= epsilon) {
      return std::nullopt;
    }
  }
  for (int j = 1; j < ny; j++) {
    if (normalized[grid[j * nx]].y - normalized[grid[(j - 1) * nx]].y <= epsilon) {
      return std::nullopt;
    }
  }
  for (int j = 1; j < ny - 1; j++) {
    for (int i = 1; i < nx - 1; i++) {
      const double2 parameter(coordinates[0][i], coordinates[1][j]);
      grid[j * nx + i] = parameters.size();
      parameters.append(parameter);
      normalized.append((parameter - origin) / scale);
      input_to_global.append(-1);
    }
  }
  Vector<int3> triangles;
  triangles.reserve(2LL * (nx - 1) * (ny - 1));
  for (int j = 0; j < ny - 1; j++) {
    for (int i = 0; i < nx - 1; i++) {
      const int a = grid[j * nx + i], b = grid[j * nx + i + 1];
      const int c = grid[(j + 1) * nx + i + 1], d = grid[(j + 1) * nx + i];
      triangles.append({a, b, c});
      triangles.append({a, c, d});
    }
  }
  return triangles;
}

static std::optional<SurfacePreview> tessellate_surface_subset(
    const NurbsGeometry &source,
    const SurfacePreviewOptions &options,
    std::string *r_error,
    std::optional<BoundaryEvaluation> supplied_boundary,
    const Set<int64_t> *face_filter)
{
  const double tessellation_start = BLI_time_now_seconds();
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const std::string_view message) -> std::optional<SurfacePreview> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    NURBS_DEBUG_LOG("surface tessellation failed after %.3f ms; %d faces, %s",
                    (BLI_time_now_seconds() - tessellation_start) * 1000.0,
                    source.faces.count(),
                    message.data());
    return std::nullopt;
  };
  const double epsilon = options.normalized_merge_epsilon;
  if (options.interior_subdivisions < 1 || options.max_points < 1 || options.max_triangles < 0 ||
      !std::isfinite(epsilon) || epsilon <= 0 || epsilon > 1 || epsilon * epsilon == 0)
  {
    return fail("Invalid NURBS surface preview options");
  }
  const double boundary_start = BLI_time_now_seconds();
  auto boundary = supplied_boundary ? std::move(supplied_boundary) :
                                      evaluate_boundaries(source, options.boundary, r_error);
  const double boundary_ms = (BLI_time_now_seconds() - boundary_start) * 1000.0;
  if (!boundary) {
    NURBS_DEBUG_LOG("boundary evaluation failed after %.3f ms; %d faces, %s",
                    (BLI_time_now_seconds() - tessellation_start) * 1000.0,
                    source.faces.count(),
                    r_error ? r_error->c_str() : "no diagnostic");
    return std::nullopt;
  }
  if (boundary->positions.size() > options.max_points) {
    return fail("NURBS surface preview exceeds its point limit");
  }
  SurfacePreview result;
  result.boundary = std::move(*boundary);
  const double face_tessellation_start = BLI_time_now_seconds();
  int grid_faces = 0;
  Map<int64_t, const EvaluatedBoundaryUse *> uses;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const EvaluatedBoundaryUse &use : result.boundary.uses) {
    uses.add_new(use.use_id, &use);
  }
  for (const NurbsSurfaceGeometry &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (const NurbsTopologyFace &face : source.faces) {
    if (face_filter && !face_filter->contains(face.id)) {
      continue;
    }
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
    Vector<int3> input_triangles;
    auto grid_triangles = quad_grid ? rectangular_grid_triangles(loops[outer_loop],
                                                                 uv,
                                                                 normalized,
                                                                 input_to_global,
                                                                 origin,
                                                                 scale,
                                                                 options.max_points,
                                                                 minimum,
                                                                 maximum,
                                                                 *samples_u,
                                                                 *samples_v,
                                                                 epsilon) :
                                      std::nullopt;
    if (grid_triangles) {
      input_triangles = std::move(*grid_triangles);
      grid_faces++;
    }
    else {
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
        const int expected_a = polygon[corner],
                  expected_b = polygon[(corner + 1) % polygon.size()];
        if (!((a == expected_a && b == expected_b) || (a == expected_b && b == expected_a))) {
          return fail(fmt::format("NURBS face {} triangulation split a boundary sample interval",
                                  face.id));
        }
        constraint_counts[polygon_index][corner]++;
      }
      for (const Vector<int> &counts : constraint_counts) {
        if (std::any_of(counts.begin(), counts.end(), [](const int count) { return count != 1; }))
        {
          return fail(fmt::format("NURBS face {} triangulation lost or split a boundary interval",
                                  face.id));
        }
      }
      for (const Vector<int> &triangle : cdt.face) {
        if (triangle.size() != 3) {
          return fail("NURBS surface preview triangulator returned a non-triangle");
        }
        input_triangles.append({output_to_input[triangle[0]],
                                output_to_input[triangle[1]],
                                output_to_input[triangle[2]]});
      }
    }
    Vector<double2> interior_parameters;
    for (const int3 triangle : input_triangles) {
      const int direction = orient2d(
          normalized[triangle[0]], normalized[triangle[1]], normalized[triangle[2]]);
      if (direction == 0) {
        return fail("NURBS surface preview triangulator returned a degenerate UV triangle");
      }
      SurfacePreviewTriangle evaluated;
      evaluated.face_id = face.id;
      for (const int i : IndexRange(3)) {
        const int original = triangle[i];
        int &global = input_to_global[original];
        if (global < 0) {
          const int64_t next = int64_t(result.points_num()) + interior_parameters.size();
          if (next >= options.max_points) {
            return fail("NURBS surface preview exceeds its point limit");
          }
          /* Assign indices in the same first-use order, then evaluate the face as one batch. */
          global = int(next);
          interior_parameters.append(uv[original]);
        }
        evaluated.points[i] = global;
        evaluated.parameters[i] = uv[original];
      }
      if (evaluated.points[0] == evaluated.points[1] ||
          evaluated.points[1] == evaluated.points[2] || evaluated.points[2] == evaluated.points[0])
      {
        result.skipped_repeated_point_triangles++;
        result.skipped_triangles_by_face.lookup_or_add_default(face.id)++;
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
    if (!interior_parameters.is_empty()) {
      const auto points = evaluate_surface_points(surface, interior_parameters, r_error);
      if (!points) {
        return std::nullopt;
      }
      for (const int i : interior_parameters.index_range()) {
        const auto &point = (*points)[i];
        result.interior.append({point.position, face.id, interior_parameters[i]});
        result.max_interior_weight_condition = std::max(result.max_interior_weight_condition,
                                                        point.weight_condition);
      }
    }
  }
  NURBS_DEBUG_LOG(
      "surface tessellation: %.3f ms total; boundary %.3f ms, face sampling/triangulation "
      "%.3f ms; %d faces, %d boundaries, %d preview points, %d triangles, "
      "%d interior subdivisions, %d rectangle grids",
      (BLI_time_now_seconds() - tessellation_start) * 1000.0,
      boundary_ms,
      (BLI_time_now_seconds() - face_tessellation_start) * 1000.0,
      source.faces.count(),
      int(result.boundary.edges.size()),
      result.points_num(),
      int(result.triangles.size()),
      options.interior_subdivisions,
      grid_faces);
  return result;
}

std::optional<SurfacePreview> tessellate_surfaces(const NurbsGeometry &source,
                                                  const SurfacePreviewOptions &options,
                                                  std::string *r_error)
{
  return tessellate_surface_subset(source, options, r_error, std::nullopt, nullptr);
}

std::optional<SurfacePreview> refresh_surface_preview(
    const NurbsGeometry &source,
    const SurfacePreviewOptions &options,
    const NurbsGeometry &previous_source,
    const SurfacePreviewOptions &previous_options,
    const SurfacePreview &previous,
    bool &r_reused,
    std::string *r_error)
{
  const double refresh_start = BLI_time_now_seconds();
  r_reused = false;
  std::optional<BoundaryEvaluation> boundary;
  auto rebuild = [&](const char *reason) {
    NURBS_DEBUG_LOG("preview layout cache miss (%s) after %.3f ms validation; rebuilding",
                    reason,
                    (BLI_time_now_seconds() - refresh_start) * 1000.0);
    return tessellate_surface_subset(source, options, r_error, std::move(boundary), nullptr);
  };
  if (options.boundary.segments_per_span != previous_options.boundary.segments_per_span ||
      options.boundary.max_sample_entries != previous_options.boundary.max_sample_entries ||
      options.boundary.evaluation_allowance != previous_options.boundary.evaluation_allowance ||
      options.interior_subdivisions != previous_options.interior_subdivisions ||
      options.max_points != previous_options.max_points ||
      options.max_triangles != previous_options.max_triangles ||
      options.normalized_merge_epsilon != previous_options.normalized_merge_epsilon ||
      source.faces.count() != previous_source.faces.count())
  {
    return rebuild("options or face count changed");
  }
  const double boundary_start = BLI_time_now_seconds();
  const BoundaryEvaluationCache cache{
      previous_source, previous_options.boundary, previous.boundary};
  boundary = evaluate_boundaries(source, options.boundary, r_error, &cache);
  const double boundary_ms = (BLI_time_now_seconds() - boundary_start) * 1000.0;
  if (!boundary) {
    return std::nullopt;
  }
  const auto &old = previous.boundary;
  if (boundary->positions.size() != old.positions.size() ||
      boundary->vertex_ids.size() != old.vertex_ids.size() ||
      boundary->edges.size() != old.edges.size() || boundary->uses.size() != old.uses.size() ||
      !std::equal(
          boundary->vertex_ids.begin(), boundary->vertex_ids.end(), old.vertex_ids.begin()))
  {
    return rebuild("boundary sample count or vertex identities changed");
  }
  for (const int i : boundary->edges.index_range()) {
    const auto &a = boundary->edges[i], &b = old.edges[i];
    if (a.edge_id != b.edge_id || a.samples.size() != b.samples.size()) {
      return rebuild("boundary edge identity or sample count changed");
    }
    for (const int j : a.samples.index_range()) {
      if (a.samples[j].parameter != b.samples[j].parameter ||
          a.samples[j].point_index != b.samples[j].point_index)
      {
        return rebuild("boundary parameter layout changed");
      }
    }
  }
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces, old_surfaces, face_surfaces;
  for (const auto &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (const auto &surface : previous_source.surfaces) {
    old_surfaces.add_new(surface.id, &surface);
  }
  for (const auto &face : source.faces) {
    face_surfaces.add_new(face.id, surfaces.lookup(face.surface_id));
  }
  Set<int64_t> dirty_faces;
  Set<int64_t> unchanged_surfaces;
  int64_t loop_id = 0, current_face_id = 0;
  int first_global = -1, end_global = -1;
  double2 first_uv, end_uv;
  bool in_loop = false;
  auto valid_join = [&](const int64_t face_id,
                        const double2 previous_uv,
                        const int previous_global,
                        const double2 next_uv,
                        const int next_global) {
    return previous_uv == next_uv ||
           (previous_global == next_global &&
            collapsed_surface_boundary_interval(*face_surfaces.lookup(face_id),
                                                previous_uv,
                                                next_uv,
                                                boundary->positions[next_global]));
  };
  for (const int i : boundary->uses.index_range()) {
    const auto &a = boundary->uses[i], &b = old.uses[i];
    if (a.face_id != b.face_id || a.loop_id != b.loop_id || a.use_id != b.use_id ||
        a.edge_index != b.edge_index || a.reversed != b.reversed || a.is_inner != b.is_inner ||
        a.parameters.size() != b.parameters.size() || a.parameters.is_empty())
    {
      return rebuild("boundary use or trim parameters changed");
    }
    if (!std::equal(a.parameters.begin(), a.parameters.end(), b.parameters.begin())) {
      dirty_faces.add(a.face_id);
    }
    const double2 start = a.reversed ? a.parameters.last() : a.parameters.first();
    const double2 end = a.reversed ? a.parameters.first() : a.parameters.last();
    const auto &samples = boundary->edges[a.edge_index].samples;
    const int start_global = a.reversed ? samples.last().point_index : samples.first().point_index;
    const int next_end_global = a.reversed ? samples.first().point_index :
                                             samples.last().point_index;
    if (!in_loop || loop_id != a.loop_id) {
      if (in_loop && !valid_join(current_face_id, end_uv, end_global, first_uv, first_global)) {
        return rebuild("boundary use or trim parameters changed");
      }
      loop_id = a.loop_id;
      current_face_id = a.face_id;
      first_uv = start;
      first_global = start_global;
      in_loop = true;
    }
    else if (!valid_join(current_face_id, end_uv, end_global, start, start_global)) {
      return rebuild("boundary loop join changed");
    }
    end_uv = end;
    end_global = next_end_global;
  }
  if (in_loop && !valid_join(current_face_id, end_uv, end_global, first_uv, first_global)) {
    return rebuild("closed boundary loop join changed");
  }
  const auto *old_face = static_cast<const NurbsTopologyFace *>(previous_source.faces.first);
  for (const auto &face : source.faces) {
    if (!old_face || face.id != old_face->id || face.surface_id != old_face->surface_id ||
        face.orientation != old_face->orientation)
    {
      return rebuild("face identity or orientation changed");
    }
    const auto &surface = *surfaces.lookup(face.surface_id);
    const auto &old_surface = *old_surfaces.lookup(old_face->surface_id);
    if (surface.order_u != old_surface.order_u || surface.order_v != old_surface.order_v ||
        surface.points_u != old_surface.points_u || surface.points_v != old_surface.points_v ||
        surface.knots_u_num != old_surface.knots_u_num ||
        surface.knots_v_num != old_surface.knots_v_num ||
        !std::equal(surface.knots_u, surface.knots_u + surface.knots_u_num, old_surface.knots_u) ||
        !std::equal(surface.knots_v, surface.knots_v + surface.knots_v_num, old_surface.knots_v))
    {
      return rebuild("surface support or knot vectors changed");
    }
    bool unchanged = surface.points_num == old_surface.points_num;
    for (int i = 0; unchanged && i < surface.points_num; i++) {
      unchanged = surface.points[i].weight == old_surface.points[i].weight &&
                  std::equal(surface.points[i].position,
                             surface.points[i].position + 3,
                             old_surface.points[i].position);
    }
    if (unchanged) {
      unchanged_surfaces.add(surface.id);
    }
    old_face = old_face->next;
  }
  if (!dirty_faces.is_empty()) {
    if (dirty_faces.size() == source.faces.count()) {
      return rebuild("all face trims changed");
    }
    auto changed = tessellate_surface_subset(
        source, options, r_error, std::move(boundary), &dirty_faces);
    if (!changed) {
      return std::nullopt;
    }
    SurfacePreview result;
    result.boundary = std::move(changed->boundary);
    Map<int64_t, Vector<int>> old_points, new_points, old_triangles, new_triangles;
    for (const int i : previous.interior.index_range()) {
      old_points.lookup_or_add_default(previous.interior[i].face_id).append(i);
    }
    for (const int i : changed->interior.index_range()) {
      new_points.lookup_or_add_default(changed->interior[i].face_id).append(i);
    }
    for (const int i : previous.triangles.index_range()) {
      old_triangles.lookup_or_add_default(previous.triangles[i].face_id).append(i);
    }
    for (const int i : changed->triangles.index_range()) {
      new_triangles.lookup_or_add_default(changed->triangles[i].face_id).append(i);
    }
    const int boundary_points = result.boundary.positions.size();
    Vector<int> old_to_new(previous.points_num(), -1);
    Vector<int> changed_to_new(boundary_points + changed->interior.size(), -1);
    for (const int i : IndexRange(boundary_points)) {
      old_to_new[i] = changed_to_new[i] = i;
    }
    for (const auto &face : source.faces) {
      const bool dirty = dirty_faces.contains(face.id);
      const auto &input = dirty ? *changed : previous;
      auto &point_map = dirty ? changed_to_new : old_to_new;
      const auto &points = dirty ? new_points : old_points;
      const auto &triangles = dirty ? new_triangles : old_triangles;
      const int skipped = input.skipped_triangles_by_face.lookup_default(face.id, 0);
      result.skipped_repeated_point_triangles += skipped;
      if (skipped) {
        result.skipped_triangles_by_face.add_new(face.id, skipped);
      }
      if (!dirty) {
        result.reused_faces++;
      }
      if (const auto *indices = points.lookup_ptr(face.id)) {
        Vector<PointEvaluation> evaluated_points;
        const bool evaluate_positions = !dirty && !unchanged_surfaces.contains(face.surface_id);
        if (evaluate_positions) {
          Vector<double2> parameters;
          parameters.reserve(indices->size());
          for (const int i : *indices) {
            parameters.append(input.interior[i].parameter);
          }
          auto evaluated = evaluate_surface_points(
              *face_surfaces.lookup(face.id), parameters, r_error);
          if (!evaluated) {
            return std::nullopt;
          }
          evaluated_points = std::move(*evaluated);
        }
        for (const int j : indices->index_range()) {
          const int i = (*indices)[j];
          auto point = input.interior[i];
          double condition = 1;
          if (evaluate_positions) {
            point.position = evaluated_points[j].position;
            condition = evaluated_points[j].weight_condition;
          }
          result.max_interior_weight_condition = std::max(result.max_interior_weight_condition,
                                                          condition);
          if (result.interior.size() >= options.max_points - boundary_points) {
            if (r_error) {
              *r_error = "NURBS partially refreshed preview exceeds its point limit";
            }
            return std::nullopt;
          }
          point_map[boundary_points + i] = result.points_num();
          result.interior.append(point);
        }
      }
      if (const auto *indices = triangles.lookup_ptr(face.id)) {
        for (const int i : *indices) {
          if (result.triangles.size() >= options.max_triangles) {
            if (r_error) {
              *r_error = "NURBS partially refreshed preview exceeds its triangle limit";
            }
            return std::nullopt;
          }
          auto triangle = input.triangles[i];
          for (const int j : IndexRange(3)) {
            triangle.points[j] = point_map[triangle.points[j]];
            BLI_assert(triangle.points[j] >= 0);
          }
          result.triangles.append(triangle);
        }
      }
      if (result.points_num() > options.max_points ||
          result.triangles.size() > options.max_triangles)
      {
        if (r_error) {
          *r_error = "NURBS partially refreshed preview exceeds its output limits";
        }
        return std::nullopt;
      }
    }
    result.max_interior_weight_condition = std::max(
        result.max_interior_weight_condition,
        std::max(changed->max_interior_weight_condition, previous.max_interior_weight_condition));
    NURBS_DEBUG_LOG("preview partial layout hit: %.3f ms total; %d reused faces, %d rebuilt faces",
                    (BLI_time_now_seconds() - refresh_start) * 1000.0,
                    result.reused_faces,
                    int(dirty_faces.size()));
    return result;
  }
  SurfacePreview result;
  result.boundary = std::move(*boundary);
  result.interior = previous.interior;
  result.triangles = previous.triangles;
  result.skipped_repeated_point_triangles = previous.skipped_repeated_point_triangles;
  result.skipped_triangles_by_face = previous.skipped_triangles_by_face;
  result.reused_faces = source.faces.count();
  result.max_interior_weight_condition = previous.max_interior_weight_condition;
  const double interior_start = BLI_time_now_seconds();
  Map<int64_t, Vector<int>> interior_by_face;
  for (const int i : result.interior.index_range()) {
    interior_by_face.lookup_or_add_default(result.interior[i].face_id).append(i);
  }
  for (const auto &face : source.faces) {
    const auto *indices = interior_by_face.lookup_ptr(face.id);
    if (!indices || unchanged_surfaces.contains(face.surface_id)) {
      continue;
    }
    Vector<double2> parameters;
    parameters.reserve(indices->size());
    for (const int i : *indices) {
      parameters.append(result.interior[i].parameter);
    }
    const auto evaluated = evaluate_surface_points(
        *face_surfaces.lookup(face.id), parameters, r_error);
    if (!evaluated) {
      return std::nullopt;
    }
    for (const int j : indices->index_range()) {
      result.interior[(*indices)[j]].position = (*evaluated)[j].position;
      result.max_interior_weight_condition = std::max(result.max_interior_weight_condition,
                                                      (*evaluated)[j].weight_condition);
    }
  }
  const double interior_ms = (BLI_time_now_seconds() - interior_start) * 1000.0;
  r_reused = true;
  NURBS_DEBUG_LOG(
      "preview layout cache hit: %.3f ms total; boundary %.3f ms, interior evaluation "
      "%.3f ms, layout validation %.3f ms; %d faces, %d boundary samples, %d interior "
      "samples, %d reused triangles",
      (BLI_time_now_seconds() - refresh_start) * 1000.0,
      boundary_ms,
      interior_ms,
      (BLI_time_now_seconds() - refresh_start) * 1000.0 - boundary_ms - interior_ms,
      source.faces.count(),
      int(result.boundary.positions.size()),
      int(result.interior.size()),
      int(result.triangles.size()));
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

namespace {
/* Orthogonal closest point, including degenerate triangle boundaries. */
template<typename V> V center_closest_triangle(const V &p, const V &a, const V &b, const V &c)
{
  const V ab = b - a, ac = c - a, ap = p - a;
  const double d1 = math::dot(ab, ap), d2 = math::dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) {
    return a;
  }
  const V bp = p - b;
  const double d3 = math::dot(ab, bp), d4 = math::dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) {
    return b;
  }
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0 && d1 != d3) {
    return a + ab * (d1 / (d1 - d3));
  }
  const V cp = p - c;
  const double d5 = math::dot(ab, cp), d6 = math::dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) {
    return c;
  }
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0 && d2 != d6) {
    return a + ac * (d2 / (d2 - d6));
  }
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && d4 >= d3 && d5 >= d6 && d4 - d3 + d5 - d6 > 0) {
    return b + (c - b) * ((d4 - d3) / (d4 - d3 + d5 - d6));
  }
  const double sum = va + vb + vc;
  return sum > 0 ? a + ab * (vb / sum) + ac * (vc / sum) : a;
}
}  // namespace

std::optional<double3> curve_length_midpoint(const NurbsCurveGeometry &curve,
                                             const double2 interval,
                                             const double4x4 &transform)
{
  if (!curve_validate_structure(curve) || interval.x >= interval.y) {
    return std::nullopt;
  }
  /* A ruled support reuses the authoritative rational derivative evaluator for C'(t). */
  Vector<NurbsControlPoint> points;
  for (int row = 0; row < 2; row++) {
    for (int i = 0; i < curve.points_num; i++) {
      points.append(curve.points[i]);
      points.last().position[2] += row;
    }
  }
  double v_knots[4] = {0, 0, 1, 1};
  NurbsSurfaceGeometry ruled;
  ruled.points = points.data();
  ruled.points_num = points.size();
  ruled.points_u = curve.points_num;
  ruled.points_v = 2;
  ruled.order_u = curve.order;
  ruled.order_v = 2;
  ruled.knots_u = curve.knots;
  ruled.knots_u_num = curve.knots_num;
  ruled.knots_v = v_knots;
  ruled.knots_v_num = 4;
  bool valid = true;
  auto gauss = [&](const double a, const double b) {
    constexpr double nodes[] = {0.3399810435848563, 0.8611363115940526};
    constexpr double weights[] = {0.6521451548625461, 0.3478548451374538};
    double result = 0;
    for (int i = 0; i < 2; i++) {
      for (const double sign : {-1.0, 1.0}) {
        const auto d = evaluate_surface_differential(
            ruled, {(a + b) * 0.5 + sign * (b - a) * 0.5 * nodes[i], 0});
        if (!d) {
          valid = false;
          return 0.0;
        }
        result += weights[i] * math::length(math::transform_direction(transform, d->tangent_u));
      }
    }
    return result * (b - a) * 0.5;
  };
  auto integrate = [&](auto &&self, const double a, const double b, const int depth) -> double {
    const double coarse = gauss(a, b), mid = (a + b) * 0.5;
    const double fine = gauss(a, mid) + gauss(mid, b);
    if (!valid || depth == 10 || std::abs(fine - coarse) <= 1e-9 * std::max(fine, 1e-30)) {
      return fine;
    }
    return self(self, a, mid, depth + 1) + self(self, mid, b, depth + 1);
  };
  Vector<double> bounds, lengths;
  bounds.append(interval.x);
  for (int i = 0; i < curve.knots_num; i++) {
    if (curve.knots[i] > bounds.last() && curve.knots[i] < interval.y) {
      bounds.append(curve.knots[i]);
    }
  }
  bounds.append(interval.y);
  double total = 0;
  for (int i = 1; i < bounds.size(); i++) {
    const double length = integrate(integrate, bounds[i - 1], bounds[i], 0);
    lengths.append(length);
    total += length;
  }
  if (!valid || !std::isfinite(total) || total <= 0) {
    return std::nullopt;
  }
  double remaining = total * 0.5;
  for (const int i : lengths.index_range()) {
    if (remaining > lengths[i]) {
      remaining -= lengths[i];
      continue;
    }
    double a = bounds[i], b = bounds[i + 1];
    for (int j = 0; j < 32; j++) {
      const double mid = (a + b) * 0.5;
      const double length = integrate(integrate, bounds[i], mid, 0);
      if (length < remaining) {
        a = mid;
      }
      else {
        b = mid;
      }
    }
    const auto point = evaluate_curve(curve, (a + b) * 0.5);
    if (!valid || !point) {
      return std::nullopt;
    }
    return math::transform_point(transform, point->position);
  }
  return std::nullopt;
}

std::optional<SurfaceAreaCenter> surface_area_center(const NurbsSurfaceGeometry &surface,
                                                     const int64_t face_id,
                                                     const SurfacePreview &preview,
                                                     const double4x4 &transform)
{
  struct Moment {
    double area = 0;
    double3 first = double3(0);
  };
  Vector<const SurfacePreviewTriangle *> triangles;
  for (const auto &tri : preview.triangles) {
    if (tri.face_id == face_id) {
      triangles.append(&tri);
    }
  }
  if (triangles.is_empty()) {
    return std::nullopt;
  }
  const auto reference = evaluate_surface(surface, triangles[0]->parameters[0]);
  if (!reference) {
    return std::nullopt;
  }
  const double3 origin = math::transform_point(transform, reference->position);
  bool valid = true;
  auto sample_triangle = [&](const double2 a, const double2 b, const double2 c) {
    Moment m;
    const double determinant = std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    for (const double2 uv : {(4 * a + b + c) / 6, (a + 4 * b + c) / 6, (a + b + 4 * c) / 6}) {
      const auto d = evaluate_surface_differential(surface, uv);
      if (!d) {
        valid = false;
        return Moment{};
      }
      const double weight = determinant / 6 *
                            math::length(
                                math::cross(math::transform_direction(transform, d->tangent_u),
                                            math::transform_direction(transform, d->tangent_v)));
      m.area += weight;
      m.first += (math::transform_point(transform, d->point.position) - origin) * weight;
    }
    return m;
  };
  auto integrate =
      [&](auto &&self, const double2 a, const double2 b, const double2 c, int depth) -> Moment {
    const Moment coarse = sample_triangle(a, b, c);
    const double2 ab = (a + b) * 0.5, bc = (b + c) * 0.5, ca = (c + a) * 0.5;
    Moment fine;
    for (const auto &t :
         {std::array<double2, 3>{a, ab, ca}, {ab, b, bc}, {ca, bc, c}, {ab, bc, ca}})
    {
      const Moment m = sample_triangle(t[0], t[1], t[2]);
      fine.area += m.area;
      fine.first += m.first;
    }
    if (!valid || depth == 5 ||
        (std::abs(fine.area - coarse.area) <= 1e-6 * std::max(fine.area, 1e-30) &&
         math::length(fine.first - coarse.first) <=
             1e-6 * std::max(math::length(fine.first), fine.area)))
    {
      return fine;
    }
    Moment result;
    for (const auto &t :
         {std::array<double2, 3>{a, ab, ca}, {ab, b, bc}, {ca, bc, c}, {ab, bc, ca}})
    {
      const Moment m = self(self, t[0], t[1], t[2], depth + 1);
      result.area += m.area;
      result.first += m.first;
    }
    return result;
  };
  Moment total;
  for (const auto *tri : triangles) {
    const Moment m = integrate(
        integrate, tri->parameters[0], tri->parameters[1], tri->parameters[2], 0);
    total.area += m.area;
    total.first += m.first;
  }
  if (!valid || !std::isfinite(total.area) || total.area <= 0) {
    return std::nullopt;
  }
  SurfaceAreaCenter result;
  result.area = total.area;
  result.centroid = origin + total.first / total.area;
  struct Seed {
    double distance;
    double2 uv;
    const SurfacePreviewTriangle *triangle;
  };
  Vector<Seed> seeds;
  for (const auto *tri : triangles) {
    const auto pa = evaluate_surface(surface, tri->parameters[0]);
    const auto pb = evaluate_surface(surface, tri->parameters[1]);
    const auto pc = evaluate_surface(surface, tri->parameters[2]);
    if (!pa || !pb || !pc) {
      return std::nullopt;
    }
    const double3 a = math::transform_point(transform, pa->position);
    const double3 b = math::transform_point(transform, pb->position);
    const double3 c = math::transform_point(transform, pc->position);
    const double3 point = center_closest_triangle(result.centroid, a, b, c);
    const double3 ab = b - a, ac = c - a, ap = point - a;
    const double aa = math::dot(ab, ab), bb = math::dot(ab, ac), cc = math::dot(ac, ac),
                 det = aa * cc - bb * bb;
    if (det <= 0) {
      continue;
    }
    const double u = (math::dot(ap, ab) * cc - math::dot(ap, ac) * bb) / det;
    const double v = (math::dot(ap, ac) * aa - math::dot(ap, ab) * bb) / det;
    seeds.append({math::distance_squared(point, result.centroid),
                  tri->parameters[0] + (tri->parameters[1] - tri->parameters[0]) * u +
                      (tri->parameters[2] - tri->parameters[0]) * v,
                  tri});
  }
  std::sort(seeds.begin(), seeds.end(), [](const Seed &a, const Seed &b) {
    return a.distance < b.distance;
  });
  double best = std::numeric_limits<double>::infinity();
  for (int i = 0; i < std::min<int>(8, seeds.size()); i++) {
    double2 uv = seeds[i].uv;
    const auto &t = seeds[i].triangle->parameters;
    uv = center_closest_triangle(uv, t[0], t[1], t[2]);
    for (int step = 0; step < 30; step++) {
      const auto d = evaluate_surface_differential(surface, uv);
      if (!d) {
        break;
      }
      const double3 p = math::transform_point(transform, d->point.position);
      const double distance = math::distance_squared(p, result.centroid);
      if (distance < best) {
        best = distance;
        result.position = p;
        result.parameter = uv;
      }
      const double3 du = math::transform_direction(transform, d->tangent_u);
      const double3 dv = math::transform_direction(transform, d->tangent_v);
      const double aa = math::dot(du, du), bb = math::dot(du, dv), cc = math::dot(dv, dv),
                   det = aa * cc - bb * bb;
      if (det <= 0) {
        break;
      }
      const double3 delta = result.centroid - p;
      const double2 update((math::dot(delta, du) * cc - math::dot(delta, dv) * bb) / det,
                           (math::dot(delta, dv) * aa - math::dot(delta, du) * bb) / det);
      bool improved = false;
      for (double scale = 1; scale >= 1.0 / 128; scale *= 0.5) {
        const double2 next = center_closest_triangle(uv + update * scale, t[0], t[1], t[2]);
        const auto q = evaluate_surface(surface, next);
        if (q && math::distance_squared(math::transform_point(transform, q->position),
                                        result.centroid) < distance)
        {
          uv = next;
          improved = true;
          break;
        }
      }
      if (!improved) {
        break;
      }
    }
  }
  return std::isfinite(best) ? std::optional(result) : std::nullopt;
}

}  // namespace blender::bke::nurbs
