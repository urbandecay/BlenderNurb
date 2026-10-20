/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_topology.hh"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_surface.hh"

namespace blender {

namespace {

constexpr int kAgreementSamples = 9;

bool fail(std::string *r_error, const char *message)
{
  if (r_error != nullptr) {
    *r_error = message;
  }
  return false;
}

bool add_id(Set<uint64_t> &ids, const uint64_t id)
{
  if (id == 0 || ids.contains(id)) {
    return false;
  }
  ids.add(id);
  return true;
}

const NurbsCurve *find_curve(const ListBaseT<NurbsCurve> &curves, const uint64_t id)
{
  for (const NurbsCurve &curve : curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

const NurbsCurve *find_trim_curve(const NurbsTopology &topology, const uint64_t id)
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

const NurbsTopoVertex *find_vertex(const NurbsTopology &topology, const uint64_t id)
{
  for (const NurbsTopoVertex &vertex : topology.vertices) {
    if (vertex.id == id) {
      return &vertex;
    }
  }
  return nullptr;
}

const NurbsTopoEdge *find_edge(const NurbsTopology &topology, const uint64_t id)
{
  for (const NurbsTopoEdge &edge : topology.edges) {
    if (edge.id == id) {
      return &edge;
    }
  }
  return nullptr;
}

char selection_type_for_id(const NurbsTopology &topology, const uint64_t id)
{
  for (const NurbsTopoVertex &vertex : topology.vertices) {
    if (vertex.id == id) {
      return NURBS_TOPO_SELECT_VERTEX;
    }
  }
  for (const NurbsTopoEdge &edge : topology.edges) {
    if (edge.id == id) {
      return NURBS_TOPO_SELECT_EDGE;
    }
  }
  for (const NurbsTopoFace &face : topology.faces) {
    if (face.id == id) {
      return NURBS_TOPO_SELECT_FACE;
    }
  }
  return 0;
}

bool component_selected(const NurbsTopology &topology, const char type, const uint64_t id)
{
  if (type == NURBS_TOPO_SELECT_VERTEX) {
    const NurbsTopoVertex *vertex = find_vertex(topology, id);
    return vertex != nullptr && (vertex->flag & NURBS_TOPO_SELECT) != 0;
  }
  if (type == NURBS_TOPO_SELECT_EDGE) {
    const NurbsTopoEdge *edge = find_edge(topology, id);
    return edge != nullptr && (edge->flag & NURBS_TOPO_SELECT) != 0;
  }
  if (type == NURBS_TOPO_SELECT_FACE) {
    for (const NurbsTopoFace &face : topology.faces) {
      if (face.id == id) {
        return (face.flag & NURBS_TOPO_SELECT) != 0;
      }
    }
  }
  return false;
}

bool points_close(const double a[3], const double b[3], const double tolerance)
{
  return std::hypot(std::hypot(a[0] - b[0], a[1] - b[1]), a[2] - b[2]) <= tolerance;
}

bool normalized_uv_close(const double a[2],
                         const double b[2],
                         const NurbsSurface &surface,
                         const double tolerance)
{
  const double du = (a[0] - b[0]) / (surface.domain_u[1] - surface.domain_u[0]);
  const double dv = (a[1] - b[1]) / (surface.domain_v[1] - surface.domain_v[0]);
  return std::hypot(du, dv) <= tolerance;
}

bool curve_support_valid(const NurbsCurve &curve, const bool planar)
{
  if (!BKE_nurbs_curve_is_valid(curve) || curve.dimension != (planar ? 2 : 3) ||
      (curve.rational != 0 && curve.rational != 1))
  {
    return false;
  }
  if (planar) {
    for (int i = 0; i < curve.control_vertex_count; i++) {
      if (curve.control_vertices[i].position[2] != 0.0) {
        return false;
      }
    }
  }
  return true;
}

bool add_curve_ids(Set<uint64_t> &ids, const NurbsCurve &curve)
{
  if (!add_id(ids, curve.id)) {
    return false;
  }
  for (int i = 0; i < curve.control_vertex_count; i++) {
    if (!add_id(ids, curve.control_vertices[i].id)) {
      return false;
    }
  }
  return true;
}

bool add_surface_ids(Set<uint64_t> &ids, const NurbsSurface &surface)
{
  if (!add_id(ids, surface.id)) {
    return false;
  }
  const int count = surface.control_vertex_count_u * surface.control_vertex_count_v;
  for (int i = 0; i < count; i++) {
    if (!add_id(ids, surface.control_vertices[i].id)) {
      return false;
    }
  }
  return true;
}

bool use_start_end(const NurbsEdgeUse &use,
                   const NurbsTopoEdge &edge,
                   uint64_t &r_start,
                   uint64_t &r_end)
{
  if (use.reversed != 0 && use.reversed != 1) {
    return false;
  }
  r_start = edge.vertex_ids[use.reversed ? 1 : 0];
  r_end = edge.vertex_ids[use.reversed ? 0 : 1];
  return true;
}

}  // namespace

bool BKE_nurbs_topology_is_valid(const ListBaseT<NurbsCurve> &curves,
                                 const NurbsTopology &topology,
                                 std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (!std::isfinite(topology.tolerance) || topology.tolerance <= 0.0) {
    return fail(r_error, "Topology tolerance must be finite and positive.");
  }
  if (!std::isfinite(topology.parameter_tolerance) || topology.parameter_tolerance <= 0.0) {
    return fail(r_error, "Topology parameter tolerance must be finite and positive.");
  }

  Set<uint64_t> ids;
  for (const NurbsCurve &curve : curves) {
    if (!curve_support_valid(curve, false) || !add_curve_ids(ids, curve)) {
      return fail(r_error, "Invalid 3D curve support or duplicate component ID.");
    }
  }
  for (const NurbsSurface &surface : topology.surfaces) {
    if (!BKE_nurbs_surface_is_valid(surface) || !add_surface_ids(ids, surface)) {
      return fail(r_error, "Invalid surface support or duplicate component ID.");
    }
  }
  for (const NurbsCurve &curve : topology.trim_curves) {
    if (!curve_support_valid(curve, true) || !add_curve_ids(ids, curve)) {
      return fail(r_error, "Invalid planar trim support or duplicate component ID.");
    }
  }
  for (const NurbsTopoVertex &vertex : topology.vertices) {
    if (!add_id(ids, vertex.id)) {
      return fail(r_error, "Topological vertex ID is zero or duplicated.");
    }
    for (const double coordinate : vertex.position) {
      if (!std::isfinite(coordinate)) {
        return fail(r_error, "Topological vertex position is not finite.");
      }
    }
  }
  for (const NurbsTopoEdge &edge : topology.edges) {
    if (!add_id(ids, edge.id) || !std::isfinite(edge.domain[0]) ||
        !std::isfinite(edge.domain[1]) || edge.domain[1] <= edge.domain[0])
    {
      return fail(r_error, "Invalid topological edge or duplicate component ID.");
    }
  }
  for (const NurbsTopoFace &face : topology.faces) {
    if (!add_id(ids, face.id) || face.loop_count < 1 || face.loops == nullptr ||
        (face.reversed != 0 && face.reversed != 1))
    {
      return fail(r_error, "Invalid topological face or duplicate component ID.");
    }
  }
  for (const NurbsTopoFace &face : topology.faces) {
    if (find_surface(topology, face.surface_id) == nullptr) {
      return fail(r_error, "Face references a missing surface support.");
    }
    int outer_count = 0;
    for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
      const NurbsTrimLoop &loop = face.loops[loop_index];
      if (!add_id(ids, loop.id) || loop.use_count < 1 || loop.uses == nullptr ||
          (loop.role != 0 && loop.role != 1))
      {
        return fail(r_error, "Invalid trim loop or duplicate component ID.");
      }
      outer_count += loop.role == 0;
      for (int use_index = 0; use_index < loop.use_count; use_index++) {
        const NurbsEdgeUse &use = loop.uses[use_index];
        if (!add_id(ids, use.id) || find_edge(topology, use.edge_id) == nullptr ||
            find_trim_curve(topology, use.trim_curve_id) == nullptr ||
            (use.reversed != 0 && use.reversed != 1))
        {
          return fail(r_error, "Invalid edge use reference or duplicate component ID.");
        }
      }
    }
    if (outer_count != 1) {
      return fail(r_error, "Each face must have exactly one outer trim loop.");
    }
  }

  Set<uint64_t> history_ids;
  for (const NurbsSelection &selection : topology.selection_history) {
    if (selection.type != NURBS_TOPO_SELECT_VERTEX && selection.type != NURBS_TOPO_SELECT_EDGE &&
        selection.type != NURBS_TOPO_SELECT_FACE)
    {
      return fail(r_error, "Selection history contains an unknown component type.");
    }
    if (selection.component_id == 0 || history_ids.contains(selection.component_id) ||
        selection_type_for_id(topology, selection.component_id) != selection.type ||
        !component_selected(topology, selection.type, selection.component_id))
    {
      return fail(r_error, "Selection history contains a duplicate, missing, or unselected ID.");
    }
    history_ids.add(selection.component_id);
  }
  if (topology.active_component_id != 0 &&
      !component_selected(topology,
                          selection_type_for_id(topology, topology.active_component_id),
                          topology.active_component_id))
  {
    return fail(r_error, "Active topology component is missing or not selected.");
  }

  for (const NurbsTopoEdge &edge : topology.edges) {
    const NurbsTopoVertex *v0 = find_vertex(topology, edge.vertex_ids[0]);
    const NurbsTopoVertex *v1 = find_vertex(topology, edge.vertex_ids[1]);
    const NurbsCurve *curve = find_curve(curves, edge.curve_id);
    if (v0 == nullptr || v1 == nullptr || curve == nullptr || edge.domain[0] < curve->domain[0] ||
        edge.domain[1] > curve->domain[1])
    {
      return fail(r_error, "Edge references a missing component or exceeds its curve domain.");
    }
    double endpoint[3];
    if (!BKE_nurbs_curve_evaluate(*curve, edge.domain[0], endpoint) ||
        !points_close(endpoint, v0->position, topology.tolerance) ||
        !BKE_nurbs_curve_evaluate(*curve, edge.domain[1], endpoint) ||
        !points_close(endpoint, v1->position, topology.tolerance))
    {
      return fail(r_error, "Edge endpoint vertices disagree with its 3D curve support.");
    }
  }

  for (const NurbsTopoFace &face : topology.faces) {
    const NurbsSurface &surface = *find_surface(topology, face.surface_id);
    for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
      const NurbsTrimLoop &loop = face.loops[loop_index];
      uint64_t previous_end = 0;
      uint64_t first_start = 0;
      double previous_uv_end[2] = {};
      double first_uv_start[2] = {};
      bool have_previous_uv = false;
      bool have_first_uv = false;
      bool have_area_point = false;
      double signed_area_twice = 0.0;
      double prior_sample[2] = {};
      for (int use_index = 0; use_index < loop.use_count; use_index++) {
        const NurbsEdgeUse &use = loop.uses[use_index];
        const NurbsTopoEdge &edge = *find_edge(topology, use.edge_id);
        uint64_t start, end;
        if (!use_start_end(use, edge, start, end) || (use_index > 0 && previous_end != start)) {
          return fail(r_error, "Trim loop edge uses are not continuously oriented.");
        }
        if (use_index == 0) {
          first_start = start;
        }
        previous_end = end;

        const NurbsCurve &trim = *find_trim_curve(topology, use.trim_curve_id);
        if (edge.domain[0] < trim.domain[0] || edge.domain[1] > trim.domain[1]) {
          return fail(r_error, "Trim support does not contain its edge parameter interval.");
        }
        const NurbsCurve &edge_curve = *find_curve(curves, edge.curve_id);
        double uv_start[3], uv_end[3];
        const double traversal_start = use.reversed ? edge.domain[1] : edge.domain[0];
        const double traversal_end = use.reversed ? edge.domain[0] : edge.domain[1];
        if (!BKE_nurbs_curve_evaluate(trim, traversal_start, uv_start) ||
            !BKE_nurbs_curve_evaluate(trim, traversal_end, uv_end) || uv_start[2] != 0.0 ||
            uv_end[2] != 0.0)
        {
          return fail(r_error, "Could not evaluate planar trim endpoints.");
        }
        const double current_uv_start[2] = {uv_start[0], uv_start[1]};
        const double current_uv_end[2] = {uv_end[0], uv_end[1]};
        if (have_previous_uv &&
            !normalized_uv_close(
                previous_uv_end, current_uv_start, surface, topology.parameter_tolerance))
        {
          return fail(r_error, "Consecutive trim UV endpoints do not agree.");
        }
        if (!have_first_uv) {
          first_uv_start[0] = current_uv_start[0];
          first_uv_start[1] = current_uv_start[1];
          have_first_uv = true;
        }
        previous_uv_end[0] = current_uv_end[0];
        previous_uv_end[1] = current_uv_end[1];
        have_previous_uv = true;

        /* This is a measured agreement check at nine evenly spaced parameters, not a certified
         * geometric bound between samples. The data-block tolerances are acceptance thresholds. */
        for (int sample = 0; sample < kAgreementSamples; sample++) {
          const double fraction = double(sample) / double(kAgreementSamples - 1);
          const double t = use.reversed ?
                               edge.domain[1] - (edge.domain[1] - edge.domain[0]) * fraction :
                               edge.domain[0] + (edge.domain[1] - edge.domain[0]) * fraction;
          double edge_position[3], uv[3], surface_position[3];
          if (!BKE_nurbs_curve_evaluate(edge_curve, t, edge_position) ||
              !BKE_nurbs_curve_evaluate(trim, t, uv) || uv[2] != 0.0 ||
              !BKE_nurbs_surface_evaluate(surface, uv[0], uv[1], surface_position) ||
              !points_close(edge_position, surface_position, topology.tolerance))
          {
            return fail(r_error,
                        "3D edge and surface evaluated at its UV trim disagree at a sample.");
          }
          const double normalized_uv[2] = {
              (uv[0] - surface.domain_u[0]) / (surface.domain_u[1] - surface.domain_u[0]),
              (uv[1] - surface.domain_v[0]) / (surface.domain_v[1] - surface.domain_v[0])};
          if (!have_area_point) {
            prior_sample[0] = normalized_uv[0];
            prior_sample[1] = normalized_uv[1];
            have_area_point = true;
          }
          else {
            signed_area_twice += prior_sample[0] * normalized_uv[1] -
                                 normalized_uv[0] * prior_sample[1];
            prior_sample[0] = normalized_uv[0];
            prior_sample[1] = normalized_uv[1];
          }
        }
      }
      if (previous_end != first_start) {
        return fail(r_error, "Trim loop does not close topologically.");
      }
      if (!normalized_uv_close(
              previous_uv_end, first_uv_start, surface, topology.parameter_tolerance))
      {
        return fail(r_error, "Trim loop does not close in normalized UV space.");
      }
      /* Ordered trim samples determine the signed area in normalized surface coordinates. This
       * winding diagnostic is sampled, not a certified area bound. */
      const double first_normalized_u = (first_uv_start[0] - surface.domain_u[0]) /
                                        (surface.domain_u[1] - surface.domain_u[0]);
      const double first_normalized_v = (first_uv_start[1] - surface.domain_v[0]) /
                                        (surface.domain_v[1] - surface.domain_v[0]);
      signed_area_twice += prior_sample[0] * first_normalized_v -
                           first_normalized_u * prior_sample[1];
      if (std::abs(signed_area_twice) <= topology.parameter_tolerance) {
        return fail(r_error, "Trim loop sampled UV area is degenerate or uncertain.");
      }
      const bool should_be_ccw = loop.role == 0;
      if ((signed_area_twice > 0.0) != should_be_ccw) {
        return fail(r_error, "Outer loops must be CCW and hole loops CW in support UV.");
      }
    }
  }
  return true;
}

bool BKE_nurbs_topology_edge_split(const ListBaseT<NurbsCurve> &curves,
                                   NurbsTopology &topology,
                                   uint64_t &next_component_id,
                                   const uint64_t edge_id,
                                   const double parameter,
                                   NurbsEdgeSplitResult &r_result,
                                   std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (!BKE_nurbs_topology_is_valid(curves, topology, r_error)) {
    return false;
  }
  const NurbsTopoEdge *source_edge = find_edge(topology, edge_id);
  if (source_edge == nullptr || !std::isfinite(parameter) || parameter <= source_edge->domain[0] ||
      parameter >= source_edge->domain[1])
  {
    return fail(r_error, "Split requires an existing edge and a finite interior parameter.");
  }
  const NurbsCurve *support = find_curve(curves, source_edge->curve_id);
  double position[3];
  if (support == nullptr || !BKE_nurbs_curve_evaluate(*support, parameter, position)) {
    return fail(r_error, "Could not evaluate edge support at the split parameter.");
  }

  const uint64_t first_unused_id = BKE_nurbs_component_id_next(curves, &topology);
  if (next_component_id == 0 || first_unused_id == 0 || next_component_id < first_unused_id) {
    return fail(r_error, "Next component ID must be greater than every allocated ID.");
  }

  int affected_use_count = 0;
  for (const NurbsTopoFace &face : topology.faces) {
    for (int i = 0; i < face.loop_count; i++) {
      const NurbsTrimLoop &loop = face.loops[i];
      for (int j = 0; j < loop.use_count; j++) {
        affected_use_count += loop.uses[j].edge_id == edge_id;
      }
    }
  }
  const uint64_t ids_needed = uint64_t(3) + uint64_t(affected_use_count) * 2;
  if (next_component_id > std::numeric_limits<uint64_t>::max() - ids_needed) {
    return fail(r_error, "Component ID allocation would overflow.");
  }

  NurbsTopology *replacement = BKE_nurbs_topology_copy(topology);
  if (replacement == nullptr) {
    return fail(r_error, "Could not clone topology for transactional edge split.");
  }
  NurbsEdgeSplitResult result;
  result.source_edge_id = edge_id;
  uint64_t prospective_next_id = next_component_id;
  result.vertex_id = prospective_next_id++;
  result.child_edge_ids[0] = prospective_next_id++;
  result.child_edge_ids[1] = prospective_next_id++;
  result.edge_uses.reserve(affected_use_count);

  NurbsTopoVertex *new_vertex = MEM_new<NurbsTopoVertex>(__func__);
  new_vertex->id = result.vertex_id;
  new_vertex->flag = (source_edge->flag & NURBS_TOPO_SELECT) ? NURBS_TOPO_SELECT : 0;
  copy_v3_v3_db(new_vertex->position, position);
  BLI_addtail(&replacement->vertices, new_vertex);

  const NurbsTopoEdge *copied_source = find_edge(*replacement, edge_id);
  NurbsTopoEdge child_a = *copied_source;
  NurbsTopoEdge child_b = *copied_source;
  child_a.id = result.child_edge_ids[0];
  child_a.vertex_ids[1] = result.vertex_id;
  child_a.domain[1] = parameter;
  child_b.id = result.child_edge_ids[1];
  child_b.vertex_ids[0] = result.vertex_id;
  child_b.domain[0] = parameter;

  NurbsTopoEdge *child_a_owned = MEM_new<NurbsTopoEdge>(__func__);
  NurbsTopoEdge *child_b_owned = MEM_new<NurbsTopoEdge>(__func__);
  *child_a_owned = child_a;
  *child_b_owned = child_b;
  child_a_owned->next = child_a_owned->prev = nullptr;
  child_b_owned->next = child_b_owned->prev = nullptr;

  NurbsTopoEdge *mutable_old_edge = nullptr;
  for (NurbsTopoEdge &edge : replacement->edges) {
    if (edge.id == edge_id) {
      mutable_old_edge = &edge;
      break;
    }
  }
  BLI_remlink(&replacement->edges, mutable_old_edge);
  MEM_delete(mutable_old_edge);
  BLI_addtail(&replacement->edges, child_a_owned);
  BLI_addtail(&replacement->edges, child_b_owned);

  for (NurbsTopoFace &face : replacement->faces) {
    for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
      NurbsTrimLoop &loop = face.loops[loop_index];
      int use_count = 0;
      for (int i = 0; i < loop.use_count; i++) {
        use_count += loop.uses[i].edge_id == edge_id;
      }
      if (use_count == 0) {
        continue;
      }
      NurbsEdgeUse *new_uses = MEM_new_array<NurbsEdgeUse>(loop.use_count + use_count, __func__);
      int new_index = 0;
      for (int i = 0; i < loop.use_count; i++) {
        const NurbsEdgeUse &old_use = loop.uses[i];
        if (old_use.edge_id != edge_id) {
          new_uses[new_index++] = old_use;
          continue;
        }
        NurbsEdgeUseSplit mapping;
        mapping.source_id = old_use.id;
        const bool reversed = old_use.reversed != 0;
        const uint64_t first_child = reversed ? result.child_edge_ids[1] :
                                                result.child_edge_ids[0];
        const uint64_t second_child = reversed ? result.child_edge_ids[0] :
                                                 result.child_edge_ids[1];
        mapping.child_ids[0] = prospective_next_id++;
        mapping.child_ids[1] = prospective_next_id++;
        result.edge_uses.append(mapping);

        NurbsEdgeUse first = old_use;
        first.id = mapping.child_ids[0];
        first.edge_id = first_child;
        NurbsEdgeUse second = old_use;
        second.id = mapping.child_ids[1];
        second.edge_id = second_child;
        new_uses[new_index++] = first;
        new_uses[new_index++] = second;
      }
      MEM_delete(loop.uses);
      loop.uses = new_uses;
      loop.use_count += use_count;
    }
  }

  if (replacement->active_component_id == edge_id) {
    replacement->active_component_id = result.child_edge_ids[1];
  }
  for (NurbsSelection &selection : replacement->selection_history) {
    if (selection.type != NURBS_TOPO_SELECT_EDGE || selection.component_id != edge_id) {
      continue;
    }
    selection.component_id = result.child_edge_ids[0];
    NurbsSelection *right_selection = MEM_new<NurbsSelection>(__func__);
    right_selection->component_id = result.child_edge_ids[1];
    right_selection->type = NURBS_TOPO_SELECT_EDGE;
    BLI_insertlinkafter(&replacement->selection_history, &selection, right_selection);
    break;
  }

  if (!BKE_nurbs_topology_is_valid(curves, *replacement, r_error)) {
    BKE_nurbs_topology_free(replacement);
    return false;
  }
  NurbsTopology old = topology;
  topology = *replacement;
  *replacement = old;
  BKE_nurbs_topology_free(replacement);
  next_component_id = prospective_next_id;
  r_result = std::move(result);
  return true;
}

}  // namespace blender
