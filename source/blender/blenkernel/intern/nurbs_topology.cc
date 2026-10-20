/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <iterator>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_surface.hh"
#include "BKE_nurbs_topology.hh"

namespace blender {

uint64_t BKE_nurbs_component_id_next(const ListBaseT<NurbsCurve> &curves,
                                     const NurbsTopology *topology)
{
  uint64_t max_id = 0;
  for (const NurbsCurve &curve : curves) {
    max_id = std::max(max_id, curve.id);
    for (int i = 0;
         i < std::max(0, curve.control_vertex_count) && curve.control_vertices != nullptr;
         i++)
    {
      max_id = std::max(max_id, curve.control_vertices[i].id);
    }
  }

  if (topology != nullptr) {
    for (const NurbsSurface &surface : topology->surfaces) {
      max_id = std::max(max_id, surface.id);
      const int64_t cv_count = int64_t(std::max(0, surface.control_vertex_count_u)) *
                               std::max(0, surface.control_vertex_count_v);
      for (int64_t i = 0; i < cv_count && surface.control_vertices != nullptr; i++) {
        max_id = std::max(max_id, surface.control_vertices[i].id);
      }
    }
    for (const NurbsCurve &curve : topology->trim_curves) {
      max_id = std::max(max_id, curve.id);
      for (int i = 0;
           i < std::max(0, curve.control_vertex_count) && curve.control_vertices != nullptr;
           i++)
      {
        max_id = std::max(max_id, curve.control_vertices[i].id);
      }
    }
    for (const NurbsTopoVertex &vertex : topology->vertices) {
      max_id = std::max(max_id, vertex.id);
    }
    for (const NurbsTopoEdge &edge : topology->edges) {
      max_id = std::max(max_id, edge.id);
    }
    for (const NurbsTopoFace &face : topology->faces) {
      max_id = std::max(max_id, face.id);
      for (int i = 0; i < std::max(0, face.loop_count) && face.loops != nullptr; i++) {
        const NurbsTrimLoop &loop = face.loops[i];
        max_id = std::max(max_id, loop.id);
        for (int j = 0; j < std::max(0, loop.use_count) && loop.uses != nullptr; j++) {
          max_id = std::max(max_id, loop.uses[j].id);
        }
      }
    }
  }

  return max_id == std::numeric_limits<uint64_t>::max() ? 0 : max_id + 1;
}

NurbsTopology *BKE_nurbs_topology_copy(const NurbsTopology &topology)
{
  NurbsTopology *copy = MEM_new<NurbsTopology>(__func__);
  copy->tolerance = topology.tolerance;
  copy->parameter_tolerance = topology.parameter_tolerance;
  copy->active_component_id = topology.active_component_id;

  for (const NurbsSurface &surface : topology.surfaces) {
    BLI_addtail(&copy->surfaces, BKE_nurbs_surface_copy(surface));
  }
  BKE_nurbs_curve_list_copy(copy->trim_curves, topology.trim_curves);

  for (const NurbsTopoVertex &vertex : topology.vertices) {
    NurbsTopoVertex *vertex_copy = MEM_new<NurbsTopoVertex>(__func__);
    *vertex_copy = vertex;
    vertex_copy->next = nullptr;
    vertex_copy->prev = nullptr;
    std::fill(std::begin(vertex_copy->_pad), std::end(vertex_copy->_pad), 0);
    BLI_addtail(&copy->vertices, vertex_copy);
  }
  for (const NurbsTopoEdge &edge : topology.edges) {
    NurbsTopoEdge *edge_copy = MEM_new<NurbsTopoEdge>(__func__);
    *edge_copy = edge;
    edge_copy->next = nullptr;
    edge_copy->prev = nullptr;
    std::fill(std::begin(edge_copy->_pad), std::end(edge_copy->_pad), 0);
    BLI_addtail(&copy->edges, edge_copy);
  }
  for (const NurbsTopoFace &face : topology.faces) {
    NurbsTopoFace *face_copy = MEM_new<NurbsTopoFace>(__func__);
    *face_copy = face;
    face_copy->next = nullptr;
    face_copy->prev = nullptr;
    std::fill(std::begin(face_copy->_pad), std::end(face_copy->_pad), 0);
    face_copy->_pad1 = nullptr;
    const int loop_count = std::max(0, face.loop_count);
    face_copy->loops = loop_count > 0 && face.loops != nullptr ?
                           MEM_new_array<NurbsTrimLoop>(loop_count, __func__) :
                           nullptr;
    if (face_copy->loops != nullptr) {
      std::copy_n(face.loops, loop_count, face_copy->loops);
    }
    for (int i = 0; i < loop_count && face_copy->loops != nullptr; i++) {
      NurbsTrimLoop &loop_copy = face_copy->loops[i];
      std::fill(std::begin(loop_copy._pad), std::end(loop_copy._pad), 0);
      loop_copy._pad1 = nullptr;
      const int use_count = std::max(0, loop_copy.use_count);
      const NurbsTrimLoop &loop = face.loops[i];
      loop_copy.uses = use_count > 0 && loop.uses != nullptr ?
                           MEM_new_array<NurbsEdgeUse>(use_count, __func__) :
                           nullptr;
      if (loop_copy.uses != nullptr) {
        std::copy_n(loop.uses, use_count, loop_copy.uses);
      }
      for (int j = 0; j < use_count && loop_copy.uses != nullptr; j++) {
        std::fill(std::begin(loop_copy.uses[j]._pad), std::end(loop_copy.uses[j]._pad), 0);
      }
    }
    BLI_addtail(&copy->faces, face_copy);
  }
  for (const NurbsSelection &selection : topology.selection_history) {
    NurbsSelection *selection_copy = MEM_new<NurbsSelection>(__func__);
    *selection_copy = selection;
    selection_copy->next = nullptr;
    selection_copy->prev = nullptr;
    std::fill(std::begin(selection_copy->_pad), std::end(selection_copy->_pad), 0);
    BLI_addtail(&copy->selection_history, selection_copy);
  }

  return copy;
}

void BKE_nurbs_topology_free(NurbsTopology *topology)
{
  if (topology == nullptr) {
    return;
  }

  while (NurbsSurface *surface = static_cast<NurbsSurface *>(BLI_pophead(&topology->surfaces))) {
    BKE_nurbs_surface_free(surface);
  }
  BKE_nurbs_curve_list_free(topology->trim_curves);
  while (
      NurbsTopoVertex *vertex = static_cast<NurbsTopoVertex *>(BLI_pophead(&topology->vertices)))
  {
    MEM_delete(vertex);
  }
  while (NurbsTopoEdge *edge = static_cast<NurbsTopoEdge *>(BLI_pophead(&topology->edges))) {
    MEM_delete(edge);
  }
  while (NurbsTopoFace *face = static_cast<NurbsTopoFace *>(BLI_pophead(&topology->faces))) {
    for (int i = 0; i < std::max(0, face->loop_count) && face->loops != nullptr; i++) {
      MEM_delete(face->loops[i].uses);
    }
    MEM_delete(face->loops);
    MEM_delete(face);
  }
  while (NurbsSelection *selection = static_cast<NurbsSelection *>(
             BLI_pophead(&topology->selection_history)))
  {
    MEM_delete(selection);
  }
  MEM_delete(topology);
}

namespace {

char *selection_flag(NurbsTopology &topology, const char type, const uint64_t id)
{
  if (type == NURBS_TOPO_SELECT_VERTEX) {
    for (NurbsTopoVertex &component : topology.vertices) {
      if (component.id == id) {
        return &component.flag;
      }
    }
  }
  else if (type == NURBS_TOPO_SELECT_EDGE) {
    for (NurbsTopoEdge &component : topology.edges) {
      if (component.id == id) {
        return &component.flag;
      }
    }
  }
  else if (type == NURBS_TOPO_SELECT_FACE) {
    for (NurbsTopoFace &component : topology.faces) {
      if (component.id == id) {
        return &component.flag;
      }
    }
  }
  return nullptr;
}

}  // namespace

bool BKE_nurbs_topology_selection_set(NurbsTopology &topology,
                                      const char type,
                                      const uint64_t component_id,
                                      const bool select,
                                      const bool make_active)
{
  char *flag = selection_flag(topology, type, component_id);
  if (flag == nullptr) {
    return false;
  }

  NurbsSelection *history_entry = nullptr;
  for (NurbsSelection &entry : topology.selection_history) {
    if (entry.component_id == component_id) {
      history_entry = &entry;
      break;
    }
  }

  if (select) {
    *flag |= NURBS_TOPO_SELECT;
    if (history_entry == nullptr) {
      history_entry = MEM_new<NurbsSelection>(__func__);
      history_entry->component_id = component_id;
      history_entry->type = type;
      BLI_addtail(&topology.selection_history, history_entry);
    }
    if (make_active) {
      topology.active_component_id = component_id;
    }
    return true;
  }

  *flag &= ~NURBS_TOPO_SELECT;
  if (history_entry != nullptr) {
    BLI_remlink(&topology.selection_history, history_entry);
    MEM_delete(history_entry);
  }
  if (topology.active_component_id == component_id) {
    const NurbsSelection *last = static_cast<const NurbsSelection *>(
        topology.selection_history.last);
    topology.active_component_id = last != nullptr ? last->component_id : 0;
  }
  return true;
}

void BKE_nurbs_topology_selection_clear(NurbsTopology &topology)
{
  for (NurbsTopoVertex &component : topology.vertices) {
    component.flag &= ~NURBS_TOPO_SELECT;
  }
  for (NurbsTopoEdge &component : topology.edges) {
    component.flag &= ~NURBS_TOPO_SELECT;
  }
  for (NurbsTopoFace &component : topology.faces) {
    component.flag &= ~NURBS_TOPO_SELECT;
  }
  while (NurbsSelection *selection = static_cast<NurbsSelection *>(
             BLI_pophead(&topology.selection_history)))
  {
    MEM_delete(selection);
  }
  topology.active_component_id = 0;
}

bool BKE_nurbs_topology_selection_is_selected(const NurbsTopology &topology,
                                              const char type,
                                              const uint64_t component_id)
{
  if (type == NURBS_TOPO_SELECT_VERTEX) {
    for (const NurbsTopoVertex &component : topology.vertices) {
      if (component.id == component_id) {
        return (component.flag & NURBS_TOPO_SELECT) != 0;
      }
    }
  }
  else if (type == NURBS_TOPO_SELECT_EDGE) {
    for (const NurbsTopoEdge &component : topology.edges) {
      if (component.id == component_id) {
        return (component.flag & NURBS_TOPO_SELECT) != 0;
      }
    }
  }
  else if (type == NURBS_TOPO_SELECT_FACE) {
    for (const NurbsTopoFace &component : topology.faces) {
      if (component.id == component_id) {
        return (component.flag & NURBS_TOPO_SELECT) != 0;
      }
    }
  }
  return false;
}

}  // namespace blender
