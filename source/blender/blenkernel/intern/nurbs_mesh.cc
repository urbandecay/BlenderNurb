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

#include "DNA_curve_types.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_math_boolean.hh"
#include "BLI_set.hh"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_nurbs_mesh.hh"

namespace blender::bke::nurbs {

/** Test the stored float positions exactly. Their exponent range is safe for the double
 * orientation predicate, including subnormals. An area epsilon would discard thin real faces. */
static bool display_triangle_has_area(const float3 &a, const float3 &b, const float3 &c)
{
  for (const int axis : IndexRange(3)) {
    const int other = (axis + 1) % 3;
    if (orient2d(double2(a[axis], a[other]),
                 double2(b[axis], b[other]),
                 double2(c[axis], c[other])) != 0)
    {
      return true;
    }
  }
  return false;
}

void MeshDeleter::operator()(Mesh *mesh) const
{
  if (mesh != nullptr) {
    BKE_id_free(nullptr, mesh);
  }
}

SurfacePreviewOptions curve_preview_options(const Curve &curve, const bool for_render)
{
  const int resolution_u = for_render && curve.resolu_ren > 0 ? curve.resolu_ren : curve.resolu;
  const int resolution_v = for_render && curve.resolv_ren > 0 ? curve.resolv_ren : curve.resolv;
  SurfacePreviewOptions options;
  options.boundary.segments_per_span = 2 * std::max(resolution_u, resolution_v) - 1;
  options.interior_subdivisions = options.boundary.segments_per_span;
  return options;
}

std::optional<MeshEvaluation> evaluate_mesh(const ReadSnapshot &source,
                                            const SurfacePreviewOptions &options,
                                            std::string *r_error)
{
  auto fail = [&](const char *message) -> std::optional<MeshEvaluation> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  auto preview = tessellate_surfaces(source.geometry(), options, r_error);
  if (!preview) {
    return std::nullopt;
  }
  if (preview->triangles.size() > std::numeric_limits<int>::max() / 3) {
    return fail("NURBS display mesh exceeds the corner index range");
  }
  MeshEvaluation result{source, std::move(*preview), nullptr, {}};
  Array<float3> positions(result.preview.points_num());
  for (const int i : positions.index_range()) {
    const double3 position = result.preview.position(i);
    for (const int axis : IndexRange(3)) {
      if (!std::isfinite(position[axis]) ||
          std::abs(position[axis]) > std::numeric_limits<float>::max())
      {
        return fail("NURBS source exceeds the float display coordinate range");
      }
      positions[i][axis] = float(position[axis]);
    }
    const double3 difference = position - double3(positions[i]);
    result.max_float_position_deviation = std::max(
        result.max_float_position_deviation, std::hypot(difference.x, difference.y, difference.z));
  }
  for (const int i : result.preview.triangles.index_range()) {
    const int3 triangle = result.preview.triangles[i].points;
    if (display_triangle_has_area(
            positions[triangle.x], positions[triangle.y], positions[triangle.z]))
    {
      result.mesh_to_preview_triangle.append(i);
    }
    else {
      result.skipped_degenerate_float_triangles++;
    }
  }
  Vector<int2> edges;
  Set<int2> edge_keys;
  for (const EvaluatedBoundaryEdge &edge : result.preview.boundary.edges) {
    for (const int i : IndexRange(edge.samples.size() - 1)) {
      const int a = edge.samples[i].point_index, b = edge.samples[i + 1].point_index;
      if (a == b) {
        result.skipped_repeated_point_segments++;
        continue;
      }
      /* Deduplication is by derived indices, not coordinates. Coincident source vertices remain
       * distinct indices. Separate source edges with the same sampled interval retain their own
       * identities/parameters in the preview, independently of this display edge representation.
       */
      if (edge_keys.add(int2(std::min(a, b), std::max(a, b)))) {
        edges.append({a, b});
      }
    }
  }
  const int faces_num = int(result.mesh_to_preview_triangle.size());
  result.mesh.reset(BKE_mesh_new_nomain(positions.size(), edges.size(), faces_num, faces_num * 3));
  Mesh &mesh = *result.mesh;
  mesh.vert_positions_for_write().copy_from(positions);
  mesh.edges_for_write().copy_from(edges);
  MutableSpan<int> offsets = mesh.face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh.corner_verts_for_write();
  for (const int i : offsets.index_range()) {
    offsets[i] = i * 3;
  }
  if (faces_num > 0) {
    const auto index = TopologyIndex::build(source.geometry(), r_error);
    if (!index) {
      return std::nullopt;
    }
    Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
    for (const NurbsSurfaceGeometry &surface : source.geometry().surfaces) {
      surfaces.add_new(surface.id, &surface);
    }
    Array<float3> normals(mesh.corners_num, float3(0));
    for (const int i : result.mesh_to_preview_triangle.index_range()) {
      const auto &triangle = result.preview.triangles[result.mesh_to_preview_triangle[i]];
      const auto &face = *index->face(triangle.face_id);
      const NurbsSurfaceGeometry &surface = *surfaces.lookup(face.surface_id);
      double2 center;
      for (const int axis : IndexRange(2)) {
        center[axis] = std::lerp(
            std::midpoint(triangle.parameters[0][axis], triangle.parameters[1][axis]),
            triangle.parameters[2][axis],
            1.0 / 3.0);
      }
      for (const int j : IndexRange(3)) {
        corner_verts[i * 3 + j] = triangle.points[j];
        const double2 parameter = triangle.parameters[j];
        const SurfaceLimits limits{center.x < parameter.x ? KnotSide::Left : KnotSide::Right,
                                   center.y < parameter.y ? KnotSide::Left : KnotSide::Right};
        const auto differential = evaluate_surface_differential(
            surface, parameter, limits, r_error);
        if (!differential) {
          return std::nullopt;
        }
        if (differential->normal) {
          normals[i * 3 + j] = float3(*differential->normal * face.orientation);
        }
        else {
          result.undefined_normal_corners++;
        }
      }
    }
    auto attributes = mesh.attributes_for_write();
    auto materials = attributes.lookup_or_add_for_write_span<int>("material_index",
                                                                  AttrDomain::Face);
    auto sharp_faces = attributes.lookup_or_add_for_write_span<bool>("sharp_face",
                                                                     AttrDomain::Face);
    for (const int i : result.mesh_to_preview_triangle.index_range()) {
      const auto &triangle = result.preview.triangles[result.mesh_to_preview_triangle[i]];
      materials.span[i] = index->face(triangle.face_id)->material_index;
      sharp_faces.span[i] = false;
    }
    materials.finish();
    sharp_faces.finish();
    mesh_calc_edges(mesh, true, false);
    mesh_set_custom_normals_normalized(mesh, normals);

    /* Use the mesh renderer's existing optimal-display pipeline, also used by subdivision.
     * The evaluated mesh remains triangulated for shading/picking; its wire display follows
     * each source face's preview grid. Boundary and loose source edges stay visible. */
    mesh.runtime->subsurf_optimal_display_edges.resize(mesh.edges_num, false);
    for (const int i : mesh.edges().index_range()) {
      const int2 edge = mesh.edges()[i];
      mesh.runtime->subsurf_optimal_display_edges[i].set(
          edge_keys.contains(int2(std::min(edge.x, edge.y), std::max(edge.x, edge.y))));
    }
    const Span<int> corner_edges = mesh.corner_edges();
    for (const int i : result.mesh_to_preview_triangle.index_range()) {
      for (const int corner : IndexRange(3)) {
        if (result.preview.triangles[result.mesh_to_preview_triangle[i]].display_edges[corner]) {
          mesh.runtime->subsurf_optimal_display_edges[corner_edges[3 * i + corner]].set();
        }
      }
    }
  }
  return result;
}

}  // namespace blender::bke::nurbs
