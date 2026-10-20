/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_tessellate.hh"

#include <algorithm>
#include <cmath>
#include <limits>

#include "BLI_array.hh"
#include "BLI_delaunay_2d.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_surface.hh"
#include "BKE_nurbs_topology.hh"

namespace blender {

namespace {

/* The display service is intentionally bounded. This is a screen-independent sampling control,
 * not a geometric error tolerance. */
constexpr int kMaximumResolution = 128;
constexpr int64_t kMaximumInputVerticesPerFace = 1'000'000;

bool fail(std::string *r_error, const char *message)
{
  if (r_error != nullptr) {
    *r_error = message;
  }
  return false;
}

const NurbsCurve *find_trim(const NurbsTopology &topology, const uint64_t id)
{
  for (const NurbsCurve &curve : topology.trim_curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

const NurbsSurface *find_surface(const NurbsTopology &topology, const uint64_t id)
{
  for (const NurbsSurface &surface : topology.surfaces) {
    if (surface.id == id) {
      return &surface;
    }
  }
  return nullptr;
}

double2 normalize_uv(const NurbsSurface &surface, const double uv[3])
{
  return double2((uv[0] - surface.domain_u[0]) / (surface.domain_u[1] - surface.domain_u[0]),
                 (uv[1] - surface.domain_v[0]) / (surface.domain_v[1] - surface.domain_v[0]));
}

bool tessellate_face(const NurbsTopology &topology,
                     const NurbsTopoFace &face,
                     const int resolution,
                     NurbsTessellation &r_output,
                     std::string *r_error)
{
  const NurbsSurface *surface = find_surface(topology, face.surface_id);
  if (surface == nullptr || !BKE_nurbs_surface_is_valid(*surface) || face.loops == nullptr ||
      face.loop_count <= 0 || (face.reversed != 0 && face.reversed != 1))
  {
    return fail(r_error, "A face has invalid surface or loop data.");
  }

  int64_t estimated_boundary_samples = 0;
  for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
    const NurbsTrimLoop &loop = face.loops[loop_index];
    if (loop.uses == nullptr || loop.use_count < 1) {
      return fail(r_error, "A face loop has invalid edge-use data.");
    }
    estimated_boundary_samples += int64_t(loop.use_count) * std::max(resolution, 1);
  }
  const int64_t grid_samples = resolution > 1 ? int64_t(resolution - 1) * (resolution - 1) : 0;
  if (estimated_boundary_samples + grid_samples > kMaximumInputVerticesPerFace) {
    return fail(r_error, "Requested tessellation exceeds the per-face input point limit.");
  }

  Vector<double2> points;
  Vector<Vector<int>> loop_faces;
  loop_faces.reserve(face.loop_count);
  const int boundary_steps = std::max(resolution, 1);
  for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
    const NurbsTrimLoop &loop = face.loops[loop_index];
    Vector<int> loop_indices;
    loop_indices.reserve(loop.use_count * boundary_steps);
    for (int use_index = 0; use_index < loop.use_count; use_index++) {
      const NurbsEdgeUse &use = loop.uses[use_index];
      const NurbsTopoEdge *edge = nullptr;
      for (const NurbsTopoEdge &candidate : topology.edges) {
        if (candidate.id == use.edge_id) {
          edge = &candidate;
          break;
        }
      }
      const NurbsCurve *trim = find_trim(topology, use.trim_curve_id);
      if (edge == nullptr || trim == nullptr || (use.reversed != 0 && use.reversed != 1)) {
        return fail(r_error, "A face loop references a missing edge or trim curve.");
      }
      for (int sample = 0; sample < boundary_steps; sample++) {
        const double fraction = double(sample) / double(boundary_steps);
        const double t = use.reversed ?
                             edge->domain[1] - (edge->domain[1] - edge->domain[0]) * fraction :
                             edge->domain[0] + (edge->domain[1] - edge->domain[0]) * fraction;
        double uv[3];
        if (!BKE_nurbs_curve_evaluate(*trim, t, uv) || uv[2] != 0.0) {
          return fail(r_error, "A trim curve could not be evaluated at a boundary sample.");
        }
        const double2 point = normalize_uv(*surface, uv);
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
          return fail(r_error, "A trim sample produced non-finite normalized UV coordinates.");
        }
        if (points.size() >= std::numeric_limits<int>::max()) {
          return fail(r_error, "Tessellation index range was exceeded.");
        }
        loop_indices.append(int(points.size()));
        points.append(point);
      }
    }
    loop_faces.append(std::move(loop_indices));
  }

  /* Interior sites improve the display of curved supports. The CDT's even-odd filling keeps
   * sites inside trim holes out of the output triangles. */
  if (resolution > 1) {
    for (int u_index = 1; u_index < resolution; u_index++) {
      for (int v_index = 1; v_index < resolution; v_index++) {
        if (points.size() >= kMaximumInputVerticesPerFace ||
            points.size() >= std::numeric_limits<int>::max())
        {
          return fail(r_error, "Tessellation index range was exceeded.");
        }
        points.append(
            double2(double(u_index) / double(resolution), double(v_index) / double(resolution)));
      }
    }
  }

  Array<Vector<int>> cdt_faces(loop_faces.size());
  for (const int i : loop_faces.index_range()) {
    cdt_faces[i] = std::move(loop_faces[i]);
  }
  meshintersect::CDT_input<double> input;
  input.vert = Array<double2>(points.as_span());
  input.face = std::move(cdt_faces);
  input.epsilon = std::max(1e-12, std::min(topology.parameter_tolerance, 1e-8));
  input.need_ids = false;
  const meshintersect::CDT_result<double> result = meshintersect::delaunay_2d_calc(
      input, CDT_INSIDE_WITH_HOLES);
  if (result.face.is_empty()) {
    return fail(r_error, "A trimmed face produced no display triangles.");
  }

  if (r_output.positions.size() > std::numeric_limits<int>::max()) {
    return fail(r_error, "Tessellation output index range was exceeded.");
  }
  const int vertex_offset = int(r_output.positions.size());
  if (result.vert.size() > std::numeric_limits<int>::max() - vertex_offset) {
    return fail(r_error, "Tessellation output index range was exceeded.");
  }
  for (const double2 &normalized_uv : result.vert) {
    /* CDT intersection arithmetic can put boundary vertices a few ULPs outside the normalized
     * trim domain. Snap only that numerical noise back to the source boundary. */
    const double boundary_epsilon = std::max(
        input.epsilon * 4.0, std::numeric_limits<double>::epsilon() * 64.0);
    if (normalized_uv.x < -boundary_epsilon || normalized_uv.x > 1.0 + boundary_epsilon ||
        normalized_uv.y < -boundary_epsilon || normalized_uv.y > 1.0 + boundary_epsilon)
    {
      return fail(r_error, "A CDT vertex fell outside the normalized source surface domain.");
    }
    const double normalized_u = std::clamp(normalized_uv.x, 0.0, 1.0);
    const double normalized_v = std::clamp(normalized_uv.y, 0.0, 1.0);
    const double u = surface->domain_u[0] +
                     normalized_u * (surface->domain_u[1] - surface->domain_u[0]);
    const double v = surface->domain_v[0] +
                     normalized_v * (surface->domain_v[1] - surface->domain_v[0]);
    double position[3];
    if (!BKE_nurbs_surface_evaluate(*surface, u, v, position)) {
      return fail(r_error, "A CDT vertex could not be evaluated on its source surface.");
    }
    r_output.positions.append(double3(position[0], position[1], position[2]));
  }
  for (const Vector<int> &cdt_face : result.face) {
    if (cdt_face.size() < 3) {
      return fail(r_error, "The CDT returned a face with fewer than three vertices.");
    }
    for (int i = 1; i + 1 < cdt_face.size(); i++) {
      const int a = cdt_face[0];
      const int b = cdt_face[i];
      const int c = cdt_face[i + 1];
      if (a < 0 || b < 0 || c < 0 || a >= result.vert.size() || b >= result.vert.size() ||
          c >= result.vert.size())
      {
        return fail(r_error, "The CDT returned an invalid vertex index.");
      }
      const int3 triangle = face.reversed ?
                                int3(a + vertex_offset, c + vertex_offset, b + vertex_offset) :
                                int3(a + vertex_offset, b + vertex_offset, c + vertex_offset);
      r_output.triangles.append(triangle);
      r_output.face_ids.append(face.id);
    }
  }
  return true;
}

}  // namespace

bool BKE_nurbs_topology_tessellate(const NurbsGeometry &geometry,
                                   const int resolution,
                                   NurbsTessellation &r_tessellation,
                                   std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (geometry.topology == nullptr) {
    return fail(r_error, "The NURBS geometry has no topology to tessellate.");
  }
  if (resolution < 3 || resolution > kMaximumResolution) {
    return fail(r_error, "Tessellation resolution must be between 3 and 128.");
  }
  if (!BKE_nurbs_topology_is_valid(geometry.curves, *geometry.topology, r_error)) {
    return false;
  }

  NurbsTessellation staged;
  for (const NurbsTopoFace &face : geometry.topology->faces) {
    if (!tessellate_face(*geometry.topology, face, resolution, staged, r_error)) {
      return false;
    }
  }
  if (staged.triangles.is_empty() || staged.triangles.size() != staged.face_ids.size()) {
    return fail(r_error, "Topology contains no displayable faces.");
  }
  r_tessellation = std::move(staged);
  return true;
}

}  // namespace blender
