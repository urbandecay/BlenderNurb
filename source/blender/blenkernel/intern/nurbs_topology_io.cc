/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cstdint>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_surface.hh"
#include "BKE_nurbs_topology.hh"

#include "BLO_read_write.hh"

namespace blender {

namespace {

template<typename T> bool read_owned_array(BlendDataReader *reader, T **data, const int64_t count)
{
  if (count < 0) {
    *data = nullptr;
    return false;
  }
  if (!BLO_read_array(reader, data, count)) {
    *data = nullptr;
    return false;
  }
  return true;
}

}  // namespace

void BKE_nurbs_topology_blend_write(BlendWriter *writer, const NurbsTopology &topology)
{
  writer->write_struct_list(&topology.surfaces);
  for (const NurbsSurface &surface : topology.surfaces) {
    const int64_t cv_count = int64_t(surface.control_vertex_count_u) *
                             surface.control_vertex_count_v;
    writer->write_struct_array(cv_count, surface.control_vertices);
    writer->write_double_array(surface.knot_count_u, surface.knots_u);
    writer->write_double_array(surface.knot_count_v, surface.knots_v);
  }

  writer->write_struct_list(&topology.trim_curves);
  for (const NurbsCurve &curve : topology.trim_curves) {
    writer->write_struct_array(curve.control_vertex_count, curve.control_vertices);
    writer->write_double_array(curve.knot_count, curve.knots);
  }

  writer->write_struct_list(&topology.vertices);
  writer->write_struct_list(&topology.edges);
  writer->write_struct_list(&topology.faces);
  for (const NurbsTopoFace &face : topology.faces) {
    writer->write_struct_array(face.loop_count, face.loops);
    for (int i = 0; i < face.loop_count; i++) {
      const NurbsTrimLoop &loop = face.loops[i];
      writer->write_struct_array(loop.use_count, loop.uses);
    }
  }
  writer->write_struct_list(&topology.selection_history);
}

bool BKE_nurbs_topology_blend_read(BlendDataReader *reader, NurbsTopology &topology)
{
  bool valid = true;
  BLO_read_struct_list(reader, NurbsSurface, &topology.surfaces);
  for (NurbsSurface *surface = static_cast<NurbsSurface *>(topology.surfaces.first);
       surface != nullptr;
       surface = surface->next)
  {
    const int64_t cv_count_wide = int64_t(surface->control_vertex_count_u) *
                                  surface->control_vertex_count_v;
    const bool cv_count_ok = surface->control_vertex_count_u > 0 &&
                             surface->control_vertex_count_v > 0;
    const int64_t cv_count = cv_count_ok ? cv_count_wide : -1;
    valid &= cv_count_ok;
    if (!read_owned_array(reader, &surface->control_vertices, cv_count)) {
      surface->control_vertex_count_u = 0;
      surface->control_vertex_count_v = 0;
      valid = false;
    }
    if (!read_owned_array(reader, &surface->knots_u, surface->knot_count_u)) {
      surface->knot_count_u = 0;
      valid = false;
    }
    if (!read_owned_array(reader, &surface->knots_v, surface->knot_count_v)) {
      surface->knot_count_v = 0;
      valid = false;
    }
    surface->_pad1 = nullptr;
  }

  BLO_read_struct_list(reader, NurbsCurve, &topology.trim_curves);
  for (NurbsCurve *curve = static_cast<NurbsCurve *>(topology.trim_curves.first); curve != nullptr;
       curve = curve->next)
  {
    if (!read_owned_array(reader, &curve->control_vertices, curve->control_vertex_count)) {
      curve->control_vertex_count = 0;
      valid = false;
    }
    if (!read_owned_array(reader, &curve->knots, curve->knot_count)) {
      curve->knot_count = 0;
      valid = false;
    }
  }

  BLO_read_struct_list(reader, NurbsTopoVertex, &topology.vertices);
  for (NurbsTopoVertex *vertex = static_cast<NurbsTopoVertex *>(topology.vertices.first);
       vertex != nullptr;
       vertex = vertex->next)
  {
    std::fill(std::begin(vertex->_pad), std::end(vertex->_pad), 0);
  }
  BLO_read_struct_list(reader, NurbsTopoEdge, &topology.edges);
  for (NurbsTopoEdge *edge = static_cast<NurbsTopoEdge *>(topology.edges.first); edge != nullptr;
       edge = edge->next)
  {
    std::fill(std::begin(edge->_pad), std::end(edge->_pad), 0);
  }

  BLO_read_struct_list(reader, NurbsTopoFace, &topology.faces);
  for (NurbsTopoFace *face = static_cast<NurbsTopoFace *>(topology.faces.first); face != nullptr;
       face = face->next)
  {
    if (!read_owned_array(reader, &face->loops, face->loop_count)) {
      face->loop_count = 0;
      valid = false;
      face->_pad1 = nullptr;
      continue;
    }
    face->_pad1 = nullptr;
    std::fill(std::begin(face->_pad), std::end(face->_pad), 0);
    for (int i = 0; i < face->loop_count; i++) {
      NurbsTrimLoop &loop = face->loops[i];
      std::fill(std::begin(loop._pad), std::end(loop._pad), 0);
      loop._pad1 = nullptr;
      if (!read_owned_array(reader, &loop.uses, loop.use_count)) {
        loop.use_count = 0;
        valid = false;
        continue;
      }
      for (int j = 0; j < loop.use_count; j++) {
        std::fill(std::begin(loop.uses[j]._pad), std::end(loop.uses[j]._pad), 0);
      }
    }
  }

  BLO_read_struct_list(reader, NurbsSelection, &topology.selection_history);
  for (NurbsSelection *selection = static_cast<NurbsSelection *>(topology.selection_history.first);
       selection != nullptr;
       selection = selection->next)
  {
    std::fill(std::begin(selection->_pad), std::end(selection->_pad), 0);
  }
  return valid;
}

std::size_t BKE_nurbs_topology_memory_size(const NurbsTopology &topology)
{
  std::size_t size = sizeof(NurbsTopology);
  for (const NurbsSurface &surface : topology.surfaces) {
    const std::size_t cv_count = surface.control_vertex_count_u > 0 &&
                                         surface.control_vertex_count_v > 0 ?
                                     std::size_t(surface.control_vertex_count_u) *
                                         std::size_t(surface.control_vertex_count_v) :
                                     0;
    size += sizeof(NurbsSurface) + cv_count * sizeof(NurbsControlVertex) +
            sizeof(double) * std::size_t(std::max(0, surface.knot_count_u)) +
            sizeof(double) * std::size_t(std::max(0, surface.knot_count_v));
  }
  size += BKE_nurbs_curve_list_memory_size(topology.trim_curves);
  for (const NurbsTopoVertex &vertex : topology.vertices) {
    size += sizeof(vertex);
  }
  for (const NurbsTopoEdge &edge : topology.edges) {
    size += sizeof(edge);
  }
  for (const NurbsTopoFace &face : topology.faces) {
    size += sizeof(face) + sizeof(NurbsTrimLoop) * std::size_t(std::max(0, face.loop_count));
    for (int i = 0; i < std::max(0, face.loop_count) && face.loops != nullptr; i++) {
      size += sizeof(NurbsEdgeUse) * std::size_t(std::max(0, face.loops[i].use_count));
    }
  }
  for (const NurbsSelection &selection : topology.selection_history) {
    size += sizeof(selection);
  }
  return size;
}

}  // namespace blender
