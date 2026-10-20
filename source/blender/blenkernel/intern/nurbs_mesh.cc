/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \\file
 * \\ingroup bke
 */

#include "BKE_nurbs_mesh.hh"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_curve.hh"
#include "BKE_nurbs_tessellate.hh"
#include "BKE_nurbs_topology.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

namespace {

constexpr int kCurveSampleIntervals = 64;

const NurbsCurve *find_curve(const ListBaseT<NurbsCurve> &curves, const uint64_t id)
{
  for (const NurbsCurve &curve : curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

bool face_uses_edge(const NurbsTopology &topology, const uint64_t edge_id)
{
  for (const NurbsTopoFace &face : topology.faces) {
    for (const NurbsTrimLoop &loop : Span(face.loops, face.loop_count)) {
      for (const NurbsEdgeUse &use : Span(loop.uses, loop.use_count)) {
        if (use.edge_id == edge_id) {
          return true;
        }
      }
    }
  }
  return false;
}

bool contains_id(const Span<uint64_t> ids, const uint64_t id)
{
  return ids.contains(id);
}

bool append_sampled_curve(const NurbsCurve &curve,
                          const double domain_start,
                          const double domain_end,
                          Vector<float3> &positions,
                          Vector<int2> &loose_edges)
{
  if (!BKE_nurbs_curve_is_valid(curve)) {
    return false;
  }
  if (positions.size() > std::numeric_limits<int>::max() - (kCurveSampleIntervals + 1) ||
      loose_edges.size() > std::numeric_limits<int>::max() - kCurveSampleIntervals)
  {
    return false;
  }

  const int first_vertex = int(positions.size());
  for (int sample = 0; sample <= kCurveSampleIntervals; sample++) {
    const double fraction = double(sample) / double(kCurveSampleIntervals);
    const double parameter = domain_start + (domain_end - domain_start) * fraction;
    double position[3];
    if (!BKE_nurbs_curve_evaluate(curve, parameter, position) || !std::isfinite(position[0]) ||
        !std::isfinite(position[1]) || !std::isfinite(position[2]))
    {
      return false;
    }
    const float3 converted = float3(float(position[0]), float(position[1]), float(position[2]));
    if (!std::isfinite(converted.x) || !std::isfinite(converted.y) || !std::isfinite(converted.z))
    {
      return false;
    }
    positions.append(converted);
  }
  for (int sample = 0; sample < kCurveSampleIntervals; sample++) {
    loose_edges.append(int2(first_vertex + sample, first_vertex + sample + 1));
  }
  return true;
}

}  // namespace

Mesh *BKE_nurbs_mesh_create(const NurbsGeometry &geometry, const int resolution)
{
  NurbsTessellation tessellation;
  if (geometry.topology != nullptr) {
    /* A topology can own wires before it owns any faces. Validate those records, but don't ask
     * the face tessellator to reject a valid wire-only or empty topology. */
    if (geometry.topology->faces.first == nullptr) {
      if (!BKE_nurbs_topology_is_valid(geometry.curves, *geometry.topology)) {
        return nullptr;
      }
    }
    else if (!BKE_nurbs_topology_tessellate(geometry, resolution, tessellation)) {
      return nullptr;
    }
  }

  Vector<float3> positions;
  Vector<int2> loose_edges;
  Vector<uint64_t> support_curve_ids;
  for (const double3 &position : tessellation.positions) {
    const float3 converted(float(position.x), float(position.y), float(position.z));
    if (!std::isfinite(converted.x) || !std::isfinite(converted.y) || !std::isfinite(converted.z))
    {
      return nullptr;
    }
    positions.append(converted);
  }
  if (geometry.topology != nullptr) {
    for (const NurbsTopoEdge &edge : geometry.topology->edges) {
      support_curve_ids.append(edge.curve_id);
      if (!face_uses_edge(*geometry.topology, edge.id)) {
        const NurbsCurve *curve = find_curve(geometry.curves, edge.curve_id);
        if (curve == nullptr ||
            !append_sampled_curve(*curve, edge.domain[0], edge.domain[1], positions, loose_edges))
        {
          return nullptr;
        }
      }
    }
  }

  /* Topology edges own their support curves. Only geometry curves not referenced by topology are
   * editable loose wires in the derived mesh. */
  for (const NurbsCurve &curve : geometry.curves) {
    if (contains_id(support_curve_ids, curve.id)) {
      continue;
    }
    if (!append_sampled_curve(curve, curve.domain[0], curve.domain[1], positions, loose_edges)) {
      return nullptr;
    }
  }

  if (positions.size() > std::numeric_limits<int>::max() ||
      tessellation.triangles.size() > std::numeric_limits<int>::max() / 3 ||
      loose_edges.size() > std::numeric_limits<int>::max())
  {
    return nullptr;
  }

  const int face_count = int(tessellation.triangles.size());
  const int corner_count = face_count * 3;
  Mesh *mesh = BKE_mesh_new_nomain(
      int(positions.size()), int(loose_edges.size()), face_count, corner_count);
  mesh->vert_positions_for_write().copy_from(positions);
  mesh->edges_for_write().copy_from(loose_edges);

  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  for (const int face : IndexRange(face_count)) {
    face_offsets[face] = face * 3;
    const int3 triangle = tessellation.triangles[face];
    corner_verts[face * 3] = triangle.x;
    corner_verts[face * 3 + 1] = triangle.y;
    corner_verts[face * 3 + 2] = triangle.z;
  }
  if (face_count > 0) {
    face_offsets[face_count] = corner_count;
  }

  /* Preserve the explicit loose edges above while adding edges implied by triangle faces. */
  bke::mesh_calc_edges(*mesh, true, false);

  if (face_count > 0) {
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    bke::SpanAttributeWriter<int> low_ids = attributes.lookup_or_add_for_write_only_span<int>(
        ".nurbs_face_id_low", bke::AttrDomain::Face);
    bke::SpanAttributeWriter<int> high_ids = attributes.lookup_or_add_for_write_only_span<int>(
        ".nurbs_face_id_high", bke::AttrDomain::Face);
    if (low_ids.span.size() != face_count || high_ids.span.size() != face_count) {
      low_ids.finish();
      high_ids.finish();
      BKE_id_free(nullptr, mesh);
      return nullptr;
    }
    for (const int face : IndexRange(face_count)) {
      const uint64_t source_id = tessellation.face_ids[face];
      low_ids.span[face] = std::bit_cast<int32_t>(uint32_t(source_id));
      high_ids.span[face] = std::bit_cast<int32_t>(uint32_t(source_id >> 32));
    }
    low_ids.finish();
    high_ids.finish();
  }
  return mesh;
}

}  // namespace blender
