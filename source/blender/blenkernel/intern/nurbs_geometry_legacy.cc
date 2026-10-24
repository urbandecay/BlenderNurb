/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <memory>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_curve.hh"
#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

enum class LegacyIDKind { Vertex, Edge, Face };

static void copy_point(NurbsControlPoint &dst, const BPoint &src)
{
  for (int axis = 0; axis < 3; axis++) {
    dst.position[axis] = double(src.vec[axis]);
  }
  dst.weight = double(src.vec[3]);
}

static double *copy_knots(const float *src)
{
  double *knots = MEM_new_array_uninitialized<double>(4, __func__);
  for (int i = 0; i < 4; i++) {
    knots[i] = double(src[i]);
  }
  return knots;
}

static NurbsCurveGeometry *add_curve(NurbsGeometry &geometry,
                                     const BPoint &a,
                                     const BPoint &b,
                                     const float *knots)
{
  NurbsCurveGeometry *curve = MEM_new<NurbsCurveGeometry>(__func__);
  curve->id = geometry.next_id++;
  curve->order = curve->points_num = 2;
  curve->knots_num = 4;
  curve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
  copy_point(curve->points[0], a);
  copy_point(curve->points[1], b);
  curve->knots = copy_knots(knots);
  BLI_addtail(&geometry.curves, curve);
  return curve;
}

NurbsGeometry *geometry_from_tagged_legacy(const ListBaseT<Nurb> &nurbs,
                                           LegacyGeometryMap *r_map,
                                           std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> NurbsGeometry * {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return nullptr;
  };
  std::unique_ptr<NurbsGeometry, decltype(&geometry_free)> geometry(
      MEM_new<NurbsGeometry>(__func__), geometry_free);
  LegacyGeometryMap source_map;
  Map<int64_t, LegacyIDKind> id_kinds;
  Map<int64_t, NurbsTopologyVertex *> vertices;
  Map<int64_t, NurbsTopologyEdge *> edges;
  Map<int64_t, NurbsCurveGeometry *> edge_curves;
  Set<int64_t> wire_ids, face_edge_ids, loose_point_ids;
  auto claim_id = [&](const int64_t id, const LegacyIDKind kind) {
    if (id <= 0) {
      return false;
    }
    if (const LegacyIDKind *existing = id_kinds.lookup_ptr(id)) {
      return *existing == kind && kind != LegacyIDKind::Face;
    }
    id_kinds.add_new(id, kind);
    geometry->next_id = std::max(geometry->next_id, id + 1);
    return true;
  };

  /* Reserve every legacy identity before allocating geometry resources or edge uses. */
  for (const Nurb &nurb : nurbs) {
    if (nurb.type == CU_NURBS && nurb.bp != nullptr && nurb.pntsu == 1 && nurb.pntsv == 1) {
      if (nurb.topology_face_id != 0 ||
          std::any_of(nurb.topology_edge_ids,
                      nurb.topology_edge_ids + 4,
                      [](const int id) { return id != 0; }) ||
          nurb.topology_vertex_ids[1] != 0 || nurb.topology_vertex_ids[2] != 0 ||
          nurb.topology_vertex_ids[3] != 0 ||
          !claim_id(nurb.topology_vertex_ids[0], LegacyIDKind::Vertex))
      {
        return fail("A legacy loose point has unexpected topology identities");
      }
      for (const float value : nurb.bp[0].vec) {
        if (!std::isfinite(value)) {
          return fail("Legacy loose NURBS point coordinates and weight must be finite");
        }
      }
      const int64_t id = nurb.topology_vertex_ids[0];
      if (vertices.contains(id)) {
        return fail("A legacy loose point identity is already owned by another source record");
      }
      auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
      vertex->id = id;
      for (int axis = 0; axis < 3; axis++) {
        vertex->position[axis] = nurb.bp[0].vec[axis];
      }
      vertices.add_new(id, vertex);
      loose_point_ids.add(id);
      BLI_addtail(&geometry->vertices, vertex);
      continue;
    }
    const bool wire = nurb.pntsv == 1;
    if (nurb.type != CU_NURBS || nurb.bp == nullptr || nurb.pntsu != 2 || nurb.orderu != 2 ||
        !ELEM(nurb.pntsv, 1, 2) || (nurb.flagu & CU_NURB_CYCLIC) ||
        (!wire && (nurb.orderv != 2 || (nurb.flagv & CU_NURB_CYCLIC))) || nurb.knotsu == nullptr ||
        (!wire && nurb.knotsv == nullptr))
    {
      return fail(
          "Legacy import requires non-cyclic tagged two-point lines or two-by-two patches");
    }
    if (wire && (nurb.topology_face_id != 0 || nurb.topology_vertex_ids[2] != 0 ||
                 nurb.topology_vertex_ids[3] != 0 || nurb.topology_edge_ids[1] != 0 ||
                 nurb.topology_edge_ids[2] != 0 || nurb.topology_edge_ids[3] != 0))
    {
      return fail("A legacy line has unexpected patch identities");
    }
    if (!wire && !claim_id(nurb.topology_face_id, LegacyIDKind::Face)) {
      return fail("Missing, repeated or conflicting legacy face identity");
    }
    /* Same-sign nonzero weights make these degree-one supports regular. Mixed signs need
     * a denominator-domain check, not acceptance based on a finite set of samples. */
    const bool negative = std::signbit(nurb.bp[0].vec[3]);
    for (int i = 0; i < 2 * nurb.pntsv; i++) {
      for (int axis = 0; axis < 4; axis++) {
        if (!std::isfinite(nurb.bp[i].vec[axis])) {
          return fail("Legacy NURBS coefficients and weights must be finite");
        }
      }
      if (nurb.bp[i].vec[3] == 0.0f || std::signbit(nurb.bp[i].vec[3]) != negative) {
        return fail("Legacy import requires same-sign nonzero rational weights");
      }
    }
    const int point_indices[4] = {0, 1, 3, 2};
    for (int corner = 0; corner < (wire ? 2 : 4); corner++) {
      const int64_t id = nurb.topology_vertex_ids[corner];
      if (loose_point_ids.contains(id) || !claim_id(id, LegacyIDKind::Vertex)) {
        return fail("Missing or conflicting legacy vertex identity");
      }
      const BPoint &point = nurb.bp[point_indices[corner]];
      if (NurbsTopologyVertex *const *existing = vertices.lookup_ptr(id)) {
        for (int axis = 0; axis < 3; axis++) {
          if ((*existing)->position[axis] != double(point.vec[axis])) {
            return fail("Repeated legacy vertex identity has inconsistent positions");
          }
        }
      }
      else {
        NurbsTopologyVertex *vertex = MEM_new<NurbsTopologyVertex>(__func__);
        vertex->id = id;
        for (int axis = 0; axis < 3; axis++) {
          vertex->position[axis] = double(point.vec[axis]);
        }
        BLI_addtail(&geometry->vertices, vertex);
        vertices.add_new(id, vertex);
      }
    }
    for (int i = 0; i < (wire ? 1 : 4); i++) {
      const int64_t id = nurb.topology_edge_ids[i];
      if (!claim_id(id, LegacyIDKind::Edge)) {
        return fail("Missing or conflicting legacy edge identity");
      }
      if (wire) {
        if (!wire_ids.add(id) || face_edge_ids.contains(id)) {
          return fail("A legacy wire identity is stored more than once");
        }
      }
      else {
        if (wire_ids.contains(id)) {
          return fail("A legacy wire identity is also stored as a patch boundary");
        }
        face_edge_ids.add(id);
      }
    }
  }

  auto ensure_edge = [&](const int64_t id,
                         const int64_t start_vertex,
                         const int64_t end_vertex,
                         const BPoint &a,
                         const BPoint &b,
                         const float *knots) -> NurbsTopologyEdge * {
    if (NurbsTopologyEdge *const *existing = edges.lookup_ptr(id)) {
      const NurbsTopologyEdge &edge = **existing;
      if ((edge.vertex_ids[0] == start_vertex && edge.vertex_ids[1] == end_vertex) ||
          (edge.vertex_ids[0] == end_vertex && edge.vertex_ids[1] == start_vertex))
      {
        return *existing;
      }
      fail("Repeated legacy edge identity has inconsistent endpoint identities");
      return nullptr;
    }
    NurbsCurveGeometry *curve = add_curve(*geometry, a, b, knots);
    if (!curve_validate_structure(*curve, r_error)) {
      return nullptr;
    }
    NurbsTopologyEdge *edge = MEM_new<NurbsTopologyEdge>(__func__);
    edge->id = id;
    edge->vertex_ids[0] = start_vertex;
    edge->vertex_ids[1] = end_vertex;
    edge->curve_id = curve->id;
    edge->parameter_range[0] = curve->knots[1];
    edge->parameter_range[1] = curve->knots[2];
    BLI_addtail(&geometry->edges, edge);
    edges.add_new(id, edge);
    edge_curves.add_new(id, curve);
    return edge;
  };

  for (const Nurb &nurb : nurbs) {
    if (nurb.pntsu == 1 && nurb.pntsv == 1) {
      continue;
    }
    if (nurb.pntsv == 1) {
      NurbsTopologyEdge *edge = ensure_edge(nurb.topology_edge_ids[0],
                                            nurb.topology_vertex_ids[0],
                                            nurb.topology_vertex_ids[1],
                                            nurb.bp[0],
                                            nurb.bp[1],
                                            nurb.knotsu);
      if (edge == nullptr) {
        return nullptr;
      }
      source_map.curves.add_new(&nurb, edge->curve_id);
      continue;
    }
    NurbsSurfaceGeometry *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
    surface->id = geometry->next_id++;
    surface->points_num = 4;
    surface->points_u = surface->points_v = surface->order_u = surface->order_v = 2;
    surface->knots_u_num = surface->knots_v_num = 4;
    surface->points = MEM_new_array<NurbsControlPoint>(4, __func__);
    for (int i = 0; i < 4; i++) {
      copy_point(surface->points[i], nurb.bp[i]);
    }
    surface->knots_u = copy_knots(nurb.knotsu);
    surface->knots_v = copy_knots(nurb.knotsv);
    BLI_addtail(&geometry->surfaces, surface);
    if (!surface_validate_structure(*surface, r_error)) {
      return nullptr;
    }
    source_map.surfaces.add_new(&nurb, surface->id);
    NurbsTopologyFace *face = MEM_new<NurbsTopologyFace>(__func__);
    face->id = nurb.topology_face_id;
    face->surface_id = surface->id;
    face->material_index = nurb.mat_nr;
    BLI_addtail(&geometry->faces, face);
    NurbsTopologyLoop *loop = MEM_new<NurbsTopologyLoop>(__func__);
    loop->id = geometry->next_id++;
    BLI_addtail(&face->loops, loop);
    const int point_starts[4] = {0, 1, 2, 0};
    const int point_ends[4] = {1, 3, 3, 2};
    const int vertex_starts[4] = {0, 1, 3, 0};
    const int vertex_ends[4] = {1, 2, 2, 3};
    const double u0 = surface->knots_u[1], u1 = surface->knots_u[2];
    const double v0 = surface->knots_v[1], v1 = surface->knots_v[2];
    const double2 uv[4] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
    for (int i = 0; i < 4; i++) {
      const int start = point_starts[i], end = point_ends[i];
      const int64_t vertex_a = nurb.topology_vertex_ids[vertex_starts[i]];
      const int64_t vertex_b = nurb.topology_vertex_ids[vertex_ends[i]];
      NurbsTopologyEdge *edge = ensure_edge(nurb.topology_edge_ids[i],
                                            vertex_a,
                                            vertex_b,
                                            nurb.bp[start],
                                            nurb.bp[end],
                                            i % 2 == 0 ? nurb.knotsu : nurb.knotsv);
      if (edge == nullptr) {
        return nullptr;
      }
      const bool reversed = edge->vertex_ids[0] != vertex_a || edge->vertex_ids[1] != vertex_b;
      const int aligned[2] = {reversed ? end : start, reversed ? start : end};
      const NurbsCurveGeometry &canonical = *edge_curves.lookup(edge->id);
      NurbsCurveGeometry *pcurve = MEM_new<NurbsCurveGeometry>(__func__);
      pcurve->id = geometry->next_id++;
      pcurve->dimension = 2;
      pcurve->order = pcurve->points_num = 2;
      pcurve->knots_num = 4;
      pcurve->knots = MEM_dupalloc(canonical.knots);
      pcurve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
      for (int j = 0; j < 2; j++) {
        pcurve->points[j].position[0] = uv[aligned[j]].x;
        pcurve->points[j].position[1] = uv[aligned[j]].y;
        /* Exact projective parameter change: cross-products of two float source weights
         * fit in double precision. This preserves correspondence even when patches use
         * different weight ratios, rather than changing either surface or fitting a seam. */
        pcurve->points[j].weight = canonical.points[j].weight *
                                   double(nurb.bp[aligned[1 - j]].vec[3]);
      }
      BLI_addtail(&geometry->curves, pcurve);
      NurbsTopologyEdgeUse *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
      use->id = geometry->next_id++;
      use->edge_id = edge->id;
      use->pcurve_id = pcurve->id;
      use->reversed = reversed != (i >= 2);
      BLI_addtail(&loop->edge_uses, use);
    }
  }
  if (!geometry_validate_structure(*geometry, r_error)) {
    return nullptr;
  }
  if (r_map != nullptr) {
    *r_map = std::move(source_map);
  }
  return geometry.release();
}

}  // namespace blender::bke::nurbs
