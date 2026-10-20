/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_nurbs_types.h"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_curve.hh"
#include "BKE_nurbs_tessellate.hh"
#include "BKE_nurbs_topology.hh"

#include "GPU_batch.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "draw_cache_impl.hh" /* own include */

namespace blender::draw {

struct NurbsBatchCache {
  gpu::Batch *edit_edges;
  gpu::Batch *selected_topology_edges;
  gpu::Batch *active_topology_edge;
  gpu::Batch *selected_topology_faces;
  gpu::Batch *active_topology_face;
  gpu::Batch *edit_faces;
  gpu::Batch *edit_verts;
  bool edit_edges_created;
  bool selected_topology_edges_created;
  bool active_topology_edge_created;
  bool selected_topology_faces_created;
  bool active_topology_face_created;
  bool edit_faces_created;
  bool edit_verts_created;
  bool is_dirty;
};

static void nurbs_batch_cache_clear(NurbsGeometry *geometry)
{
  NurbsBatchCache *cache = geometry->batch_cache;
  if (cache == nullptr) {
    return;
  }

  GPU_BATCH_DISCARD_SAFE(cache->edit_edges);
  GPU_BATCH_DISCARD_SAFE(cache->selected_topology_edges);
  GPU_BATCH_DISCARD_SAFE(cache->active_topology_edge);
  GPU_BATCH_DISCARD_SAFE(cache->selected_topology_faces);
  GPU_BATCH_DISCARD_SAFE(cache->active_topology_face);
  GPU_BATCH_DISCARD_SAFE(cache->edit_faces);
  GPU_BATCH_DISCARD_SAFE(cache->edit_verts);
}

static bool nurbs_batch_cache_valid(const NurbsGeometry *geometry)
{
  const NurbsBatchCache *cache = geometry->batch_cache;
  return cache != nullptr && !cache->is_dirty;
}

static void nurbs_batch_cache_init(NurbsGeometry *geometry)
{
  if (geometry->batch_cache == nullptr) {
    geometry->batch_cache = MEM_new_zeroed<NurbsBatchCache>(__func__);
  }
  else {
    *geometry->batch_cache = {};
  }
}

void DRW_nurbs_batch_cache_validate(NurbsGeometry *geometry)
{
  if (!nurbs_batch_cache_valid(geometry)) {
    nurbs_batch_cache_clear(geometry);
    nurbs_batch_cache_init(geometry);
  }
}

void DRW_nurbs_batch_cache_dirty_tag(NurbsGeometry *geometry, const int mode)
{
  switch (mode) {
    case BKE_NURBS_BATCH_DIRTY_ALL:
      if (geometry->batch_cache != nullptr) {
        geometry->batch_cache->is_dirty = true;
      }
      break;
    case BKE_NURBS_BATCH_DIRTY_SELECT:
      if (geometry->batch_cache != nullptr) {
        GPU_BATCH_DISCARD_SAFE(geometry->batch_cache->edit_verts);
        geometry->batch_cache->edit_verts_created = false;
        if (geometry->topology != nullptr) {
          GPU_BATCH_DISCARD_SAFE(geometry->batch_cache->edit_edges);
          GPU_BATCH_DISCARD_SAFE(geometry->batch_cache->edit_faces);
          GPU_BATCH_DISCARD_SAFE(geometry->batch_cache->selected_topology_edges);
          GPU_BATCH_DISCARD_SAFE(geometry->batch_cache->active_topology_edge);
          GPU_BATCH_DISCARD_SAFE(geometry->batch_cache->selected_topology_faces);
          GPU_BATCH_DISCARD_SAFE(geometry->batch_cache->active_topology_face);
          geometry->batch_cache->edit_edges_created = false;
          geometry->batch_cache->edit_faces_created = false;
          geometry->batch_cache->selected_topology_edges_created = false;
          geometry->batch_cache->active_topology_edge_created = false;
          geometry->batch_cache->selected_topology_faces_created = false;
          geometry->batch_cache->active_topology_face_created = false;
        }
      }
      break;
    default:
      BLI_assert_unreachable();
  }
}

void DRW_nurbs_batch_cache_free(NurbsGeometry *geometry)
{
  nurbs_batch_cache_clear(geometry);
  MEM_SAFE_DELETE(geometry->batch_cache);
}

static bool nurbs_curve_control_points_get(const NurbsCurve &curve, Vector<float3> &r_positions)
{
  if (curve.control_vertex_count <= 0 || curve.control_vertices == nullptr) {
    return false;
  }

  r_positions.reserve(curve.control_vertex_count);
  for (int i = 0; i < curve.control_vertex_count; i++) {
    const NurbsControlVertex &control_vertex = curve.control_vertices[i];
    const float3 position(float(control_vertex.position[0]),
                          float(control_vertex.position[1]),
                          float(control_vertex.position[2]));
    if (!std::isfinite(control_vertex.position[0]) || !std::isfinite(control_vertex.position[1]) ||
        !std::isfinite(control_vertex.position[2]) || !std::isfinite(position.x) ||
        !std::isfinite(position.y) || !std::isfinite(position.z))
    {
      return false;
    }
    r_positions.append(position);
  }
  return true;
}

static const NurbsCurve *nurbs_curve_find(const NurbsGeometry &geometry, const uint64_t id)
{
  for (const NurbsCurve &curve : geometry.curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

static bool nurbs_curve_sample(const NurbsCurve &curve,
                               const double domain_start,
                               const double domain_end,
                               const int sample,
                               const int sample_count,
                               float3 &r_position)
{
  const double fraction = double(sample) / double(sample_count);
  const double parameter = domain_start + (domain_end - domain_start) * fraction;
  double position[3];
  if (!BKE_nurbs_curve_evaluate(curve, parameter, position)) {
    return false;
  }
  r_position = float3(float(position[0]), float(position[1]), float(position[2]));
  return std::isfinite(r_position.x) && std::isfinite(r_position.y) && std::isfinite(r_position.z);
}

static void nurbs_batch_cache_create_edit_verts(NurbsGeometry *geometry)
{
  NurbsBatchCache *cache = geometry->batch_cache;
  Vector<float3> control_points;
  Vector<uint32_t> control_point_data;
  Vector<int> control_point_indices;

  if (geometry->topology != nullptr) {
    int source_index = 0;
    for (const NurbsTopoVertex &vertex : geometry->topology->vertices) {
      const float3 position(
          float(vertex.position[0]), float(vertex.position[1]), float(vertex.position[2]));
      if (!std::isfinite(vertex.position[0]) || !std::isfinite(vertex.position[1]) ||
          !std::isfinite(vertex.position[2]) || !std::isfinite(position.x) ||
          !std::isfinite(position.y) || !std::isfinite(position.z))
      {
        cache->edit_verts_created = true;
        return;
      }
      const bool is_selected = (vertex.flag & NURBS_TOPO_SELECT) != 0;
      uint32_t data = 0;
      if (is_selected) {
        data |= VFLAG_VERT_SELECTED;
        if (vertex.id == geometry->topology->active_component_id) {
          data |= VFLAG_VERT_ACTIVE;
        }
      }
      control_points.append(position);
      control_point_data.append(data);
      control_point_indices.append(source_index++);
    }
  }
  else {
    int source_index = 0;
    for (const NurbsCurve &curve : geometry->curves) {
      Vector<float3> curve_positions;
      if (!nurbs_curve_control_points_get(curve, curve_positions)) {
        source_index += std::max(curve.control_vertex_count, 0);
        continue;
      }

      for (const int i : curve_positions.index_range()) {
        const NurbsControlVertex &control_vertex = curve.control_vertices[i];
        const bool is_selected = (control_vertex.flag & NURBS_CV_SELECT) != 0;
        uint32_t data = 0;
        if (is_selected) {
          data |= VFLAG_VERT_SELECTED;
          if (control_vertex.id == geometry->active_control_vertex_id) {
            data |= VFLAG_VERT_ACTIVE;
          }
        }
        control_points.append(curve_positions[i]);
        control_point_data.append(data);
        control_point_indices.append(source_index + i);
      }
      source_index += curve.control_vertex_count;
    }
  }

  if (!control_points.is_empty()) {
    static struct {
      uint pos, data, index;
    } attr_id;
    static const GPUVertFormat format = [&]() {
      GPUVertFormat format{};
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
      attr_id.data = GPU_vertformat_attr_add(&format, "data", gpu::VertAttrType::UINT_32);
      attr_id.index = GPU_vertformat_attr_add(&format, "index", gpu::VertAttrType::SINT_32);
      return format;
    }();

    gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, control_points.size());
    for (const int i : control_points.index_range()) {
      GPU_vertbuf_attr_set(vbo, attr_id.pos, i, &control_points[i]);
      GPU_vertbuf_attr_set(vbo, attr_id.data, i, &control_point_data[i]);
      GPU_vertbuf_attr_set(vbo, attr_id.index, i, &control_point_indices[i]);
    }
    cache->edit_verts = GPU_batch_create_ex(GPU_PRIM_POINTS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  cache->edit_verts_created = true;
}

static void nurbs_batch_cache_create_edit_edges(NurbsGeometry *geometry)
{
  NurbsBatchCache *cache = geometry->batch_cache;
  Vector<float3> control_polygon_edges;
  Vector<float> edge_weights;
  Vector<int> edge_indices;

  if (geometry->topology != nullptr) {
    constexpr int sample_intervals = 64;
    int edge_index = 0;
    for (const NurbsTopoEdge &edge : geometry->topology->edges) {
      const NurbsCurve *curve = nurbs_curve_find(*geometry, edge.curve_id);
      if (curve == nullptr || !BKE_nurbs_curve_is_valid(*curve)) {
        edge_index++;
        continue;
      }
      for (int sample = 0; sample < sample_intervals; sample++) {
        float3 position_a, position_b;
        if (!nurbs_curve_sample(
                *curve, edge.domain[0], edge.domain[1], sample, sample_intervals, position_a) ||
            !nurbs_curve_sample(
                *curve, edge.domain[0], edge.domain[1], sample + 1, sample_intervals, position_b))
        {
          continue;
        }
        control_polygon_edges.append(position_a);
        control_polygon_edges.append(position_b);
        edge_weights.extend({666.0f, 666.0f});
        edge_indices.extend({edge_index, edge_index});
      }
      edge_index++;
    }
  }
  else {
    for (const NurbsCurve &curve : geometry->curves) {
      Vector<float3> curve_positions;
      if (!nurbs_curve_control_points_get(curve, curve_positions)) {
        continue;
      }
      for (const int i : curve_positions.index_range().drop_front(1)) {
        control_polygon_edges.append(curve_positions[i - 1]);
        control_polygon_edges.append(curve_positions[i]);
      }
    }
  }

  if (!control_polygon_edges.is_empty()) {
    static struct {
      uint pos, weight, index;
    } attr_id;
    static const GPUVertFormat format = [&]() {
      GPUVertFormat format{};
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
      attr_id.weight = GPU_vertformat_attr_add(&format, "weight", gpu::VertAttrType::SFLOAT_32);
      attr_id.index = GPU_vertformat_attr_add(&format, "index", gpu::VertAttrType::SINT_32);
      return format;
    }();

    gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, control_polygon_edges.size());
    for (const int i : control_polygon_edges.index_range()) {
      GPU_vertbuf_attr_set(vbo, attr_id.pos, i, &control_polygon_edges[i]);
      const float weight = geometry->topology != nullptr ? edge_weights[i] : 666.0f;
      GPU_vertbuf_attr_set(vbo, attr_id.weight, i, &weight);
      const int index = geometry->topology != nullptr ? edge_indices[i] : i / 2;
      GPU_vertbuf_attr_set(vbo, attr_id.index, i, &index);
    }
    cache->edit_edges = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  cache->edit_edges_created = true;
}

static void nurbs_batch_cache_create_edit_faces(NurbsGeometry *geometry)
{
  NurbsBatchCache *cache = geometry->batch_cache;
  if (geometry->topology == nullptr) {
    cache->edit_faces_created = true;
    return;
  }

  NurbsTessellation tessellation;
  if (!BKE_nurbs_topology_tessellate(*geometry, 16, tessellation) ||
      tessellation.triangles.size() > std::numeric_limits<int>::max() / 3)
  {
    cache->edit_faces_created = true;
    return;
  }

  Vector<float3> positions;
  Vector<int> face_indices;
  positions.reserve(tessellation.triangles.size() * 3);
  face_indices.reserve(tessellation.triangles.size() * 3);
  for (const int triangle_index : tessellation.triangles.index_range()) {
    int face_index = 0;
    bool found_face = false;
    int source_face_index = 0;
    for (const NurbsTopoFace &face : geometry->topology->faces) {
      if (face.id == tessellation.face_ids[triangle_index]) {
        face_index = source_face_index;
        found_face = true;
        break;
      }
      source_face_index++;
    }
    if (!found_face) {
      cache->edit_faces_created = true;
      return;
    }
    const int3 triangle = tessellation.triangles[triangle_index];
    for (const int vertex_index : {triangle.x, triangle.y, triangle.z}) {
      const double3 &position = tessellation.positions[vertex_index];
      const float3 converted(float(position.x), float(position.y), float(position.z));
      if (!std::isfinite(converted.x) || !std::isfinite(converted.y) ||
          !std::isfinite(converted.z))
      {
        cache->edit_faces_created = true;
        return;
      }
      positions.append(converted);
      face_indices.append(face_index);
    }
  }

  if (!positions.is_empty()) {
    static struct {
      uint pos, index;
    } attr_id;
    static const GPUVertFormat format = [&]() {
      GPUVertFormat format{};
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
      attr_id.index = GPU_vertformat_attr_add(&format, "index", gpu::VertAttrType::SINT_32);
      return format;
    }();

    gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, positions.size());
    for (const int i : positions.index_range()) {
      GPU_vertbuf_attr_set(vbo, attr_id.pos, i, &positions[i]);
      GPU_vertbuf_attr_set(vbo, attr_id.index, i, &face_indices[i]);
    }
    cache->edit_faces = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  cache->edit_faces_created = true;
}

static gpu::Batch *nurbs_topology_edges_batch_create(const NurbsGeometry &geometry,
                                                      const bool active_only)
{
  if (geometry.topology == nullptr) {
    return nullptr;
  }
  constexpr int sample_intervals = 64;
  Vector<float3> positions;
  for (const NurbsTopoEdge &edge : geometry.topology->edges) {
    const bool selected = (edge.flag & NURBS_TOPO_SELECT) != 0;
    const bool active = edge.id == geometry.topology->active_component_id;
    if ((active_only && !active) || (!active_only && !selected)) {
      continue;
    }
    const NurbsCurve *curve = nurbs_curve_find(geometry, edge.curve_id);
    if (curve == nullptr || !BKE_nurbs_curve_is_valid(*curve)) {
      continue;
    }
    for (int sample = 0; sample < sample_intervals; sample++) {
      float3 a, b;
      if (nurbs_curve_sample(*curve, edge.domain[0], edge.domain[1], sample, sample_intervals, a) &&
          nurbs_curve_sample(*curve, edge.domain[0], edge.domain[1], sample + 1, sample_intervals, b))
      {
        positions.extend({a, b});
      }
    }
  }
  if (positions.is_empty()) {
    return nullptr;
  }
  static struct { uint pos; } attr_id;
  static const GPUVertFormat format = [&]() {
    GPUVertFormat format{};
    attr_id.pos = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    return format;
  }();
  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, positions.size());
  for (const int i : positions.index_range()) {
    GPU_vertbuf_attr_set(vbo, attr_id.pos, i, &positions[i]);
  }
  return GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
}

static gpu::Batch *nurbs_topology_faces_batch_create(const NurbsGeometry &geometry,
                                                      const bool active_only)
{
  if (geometry.topology == nullptr) {
    return nullptr;
  }
  NurbsTessellation tessellation;
  if (!BKE_nurbs_topology_tessellate(geometry, 16, tessellation) ||
      tessellation.triangles.size() > std::numeric_limits<int>::max() / 3)
  {
    return nullptr;
  }
  Vector<float3> positions;
  positions.reserve(tessellation.triangles.size() * 3);
  for (const int triangle_index : tessellation.triangles.index_range()) {
    const uint64_t face_id = tessellation.face_ids[triangle_index];
    const NurbsTopoFace *source_face = nullptr;
    for (const NurbsTopoFace &face : geometry.topology->faces) {
      if (face.id == face_id) {
        source_face = &face;
        break;
      }
    }
    if (source_face == nullptr) {
      return nullptr;
    }
    const bool selected = (source_face->flag & NURBS_TOPO_SELECT) != 0;
    const bool active = source_face->id == geometry.topology->active_component_id;
    if ((active_only && !active) || (!active_only && !selected)) {
      continue;
    }
    const int3 triangle = tessellation.triangles[triangle_index];
    for (const int vertex_index : {triangle.x, triangle.y, triangle.z}) {
      const double3 &position = tessellation.positions[vertex_index];
      const float3 converted(float(position.x), float(position.y), float(position.z));
      if (!std::isfinite(converted.x) || !std::isfinite(converted.y) ||
          !std::isfinite(converted.z))
      {
        return nullptr;
      }
      positions.append(converted);
    }
  }
  if (positions.is_empty()) {
    return nullptr;
  }
  static struct { uint pos; } attr_id;
  static const GPUVertFormat format = [&]() {
    GPUVertFormat format{};
    attr_id.pos = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    return format;
  }();
  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, positions.size());
  for (const int i : positions.index_range()) {
    GPU_vertbuf_attr_set(vbo, attr_id.pos, i, &positions[i]);
  }
  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
}

static NurbsBatchCache *nurbs_batch_cache_get(NurbsGeometry *geometry)
{
  DRW_nurbs_batch_cache_validate(geometry);
  return geometry->batch_cache;
}

gpu::Batch *DRW_nurbs_batch_cache_get_edit_edges(NurbsGeometry *geometry)
{
  NurbsBatchCache *cache = nurbs_batch_cache_get(geometry);
  if (!cache->edit_edges_created) {
    nurbs_batch_cache_create_edit_edges(geometry);
  }
  return cache->edit_edges;
}

gpu::Batch *DRW_nurbs_batch_cache_get_edit_verts(NurbsGeometry *geometry)
{
  NurbsBatchCache *cache = nurbs_batch_cache_get(geometry);
  if (!cache->edit_verts_created) {
    nurbs_batch_cache_create_edit_verts(geometry);
  }
  return cache->edit_verts;
}

gpu::Batch *DRW_nurbs_batch_cache_get_edit_faces(NurbsGeometry *geometry)
{
  NurbsBatchCache *cache = nurbs_batch_cache_get(geometry);
  if (!cache->edit_faces_created) {
    nurbs_batch_cache_create_edit_faces(geometry);
  }
  return cache->edit_faces;
}

gpu::Batch *DRW_nurbs_batch_cache_get_topology_edges(NurbsGeometry *geometry, const bool active)
{
  NurbsBatchCache *cache = nurbs_batch_cache_get(geometry);
  gpu::Batch **batch = active ? &cache->active_topology_edge : &cache->selected_topology_edges;
  bool *created = active ? &cache->active_topology_edge_created :
                           &cache->selected_topology_edges_created;
  if (!*created) {
    *batch = nurbs_topology_edges_batch_create(*geometry, active);
    *created = true;
  }
  return *batch;
}

gpu::Batch *DRW_nurbs_batch_cache_get_topology_faces(NurbsGeometry *geometry, const bool active)
{
  NurbsBatchCache *cache = nurbs_batch_cache_get(geometry);
  gpu::Batch **batch = active ? &cache->active_topology_face : &cache->selected_topology_faces;
  bool *created = active ? &cache->active_topology_face_created :
                           &cache->selected_topology_faces_created;
  if (!*created) {
    *batch = nurbs_topology_faces_batch_create(*geometry, active);
    *created = true;
  }
  return *batch;
}

}  // namespace blender::draw
