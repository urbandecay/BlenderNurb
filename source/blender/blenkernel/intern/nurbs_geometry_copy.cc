/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

std::optional<ComponentClosure> component_closure(const NurbsGeometry &source,
                                                  const ComponentSelection &selection,
                                                  std::string *r_error)
{
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  auto fail = [&](const char *message) -> std::optional<ComponentClosure> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  ComponentClosure result;
  auto &vertices = result.vertices;
  auto &edges = result.edges;
  auto &faces = result.faces;
  auto &curves = result.curves;
  auto &surfaces = result.surfaces;
  for (const int64_t id : selection.vertices) {
    if (index->vertex(id) == nullptr) {
      return fail("Selected NURBS vertex identity does not exist");
    }
    vertices.add(id);
  }
  for (const int64_t id : selection.edges) {
    if (index->edge(id) == nullptr) {
      return fail("Selected NURBS edge identity does not exist");
    }
    edges.add(id);
  }
  for (const int64_t id : selection.faces) {
    if (index->face(id) == nullptr) {
      return fail("Selected NURBS face identity does not exist");
    }
    faces.add(id);
  }
  auto &boundary_records = result.boundary_records;
  for (const NurbsTopologyFace &face : source.faces) {
    if (!faces.contains(face.id)) {
      continue;
    }
    surfaces.add(face.surface_id);
    for (const NurbsTopologyLoop &loop : face.loops) {
      boundary_records++;
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        boundary_records++;
        edges.add(use.edge_id);
        curves.add(use.pcurve_id);
      }
    }
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (edges.contains(edge.id)) {
      vertices.add(edge.vertex_ids[0]);
      vertices.add(edge.vertex_ids[1]);
      curves.add(edge.curve_id);
    }
  }
  return result;
}

std::optional<GeometryCopyResult> copy_component_closure(const NurbsGeometry &source,
                                                         const ComponentSelection &selection,
                                                         std::string *r_error)
{
  const auto closure = component_closure(source, selection, r_error);
  if (!closure) {
    return std::nullopt;
  }
  const auto &vertices = closure->vertices;
  const auto &edges = closure->edges;
  const auto &faces = closure->faces;
  const auto &curves = closure->curves;
  const auto &surfaces = closure->surfaces;
  const uint64_t boundary_records = closure->boundary_records;
  auto fail = [&](const char *message) -> std::optional<GeometryCopyResult> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  const uint64_t records = uint64_t(vertices.size()) + edges.size() + faces.size() +
                           curves.size() + surfaces.size() + boundary_records;
  if (records > uint64_t(std::numeric_limits<int64_t>::max() - source.next_id)) {
    return fail("NURBS identity space is exhausted");
  }

  GeometryCopyResult result;
  result.geometry.reset(MEM_new<NurbsGeometry>(__func__));
  result.geometry->next_id = source.next_id;
  auto allocate_id = [&](const int64_t source_id) {
    const int64_t id = result.geometry->next_id++;
    result.source_to_copy.add_new(source_id, id);
    return id;
  };
  /* Traverse source order, rather than selection/hash order, to make the ID map deterministic. */
  for (const NurbsCurveGeometry &curve : source.curves) {
    if (!curves.contains(curve.id)) {
      continue;
    }
    NurbsCurveGeometry *copy = MEM_dupalloc(&curve);
    copy->id = allocate_id(curve.id);
    copy->points = MEM_dupalloc(curve.points);
    copy->knots = MEM_dupalloc(curve.knots);
    BLI_addtail(&result.geometry->curves, copy);
  }
  for (const NurbsSurfaceGeometry &surface : source.surfaces) {
    if (!surfaces.contains(surface.id)) {
      continue;
    }
    NurbsSurfaceGeometry *copy = MEM_dupalloc(&surface);
    copy->id = allocate_id(surface.id);
    copy->points = MEM_dupalloc(surface.points);
    copy->knots_u = MEM_dupalloc(surface.knots_u);
    copy->knots_v = MEM_dupalloc(surface.knots_v);
    BLI_addtail(&result.geometry->surfaces, copy);
  }
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (!vertices.contains(vertex.id)) {
      continue;
    }
    NurbsTopologyVertex *copy = MEM_dupalloc(&vertex);
    copy->id = allocate_id(vertex.id);
    BLI_addtail(&result.geometry->vertices, copy);
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (!edges.contains(edge.id)) {
      continue;
    }
    NurbsTopologyEdge *copy = MEM_dupalloc(&edge);
    copy->id = allocate_id(edge.id);
    copy->curve_id = result.source_to_copy.lookup(edge.curve_id);
    copy->vertex_ids[0] = result.source_to_copy.lookup(edge.vertex_ids[0]);
    copy->vertex_ids[1] = result.source_to_copy.lookup(edge.vertex_ids[1]);
    BLI_addtail(&result.geometry->edges, copy);
  }
  for (const NurbsTopologyFace &face : source.faces) {
    if (!faces.contains(face.id)) {
      continue;
    }
    NurbsTopologyFace *copy = MEM_dupalloc(&face);
    copy->id = allocate_id(face.id);
    copy->surface_id = result.source_to_copy.lookup(face.surface_id);
    copy->loops.clear_no_delete();
    BLI_addtail(&result.geometry->faces, copy);
    for (const NurbsTopologyLoop &loop : face.loops) {
      NurbsTopologyLoop *loop_copy = MEM_dupalloc(&loop);
      loop_copy->id = allocate_id(loop.id);
      loop_copy->edge_uses.clear_no_delete();
      BLI_addtail(&copy->loops, loop_copy);
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        NurbsTopologyEdgeUse *use_copy = MEM_dupalloc(&use);
        use_copy->id = allocate_id(use.id);
        use_copy->edge_id = result.source_to_copy.lookup(use.edge_id);
        use_copy->pcurve_id = result.source_to_copy.lookup(use.pcurve_id);
        BLI_addtail(&loop_copy->edge_uses, use_copy);
      }
    }
  }
  if (!geometry_validate_structure(*result.geometry, r_error)) {
    return std::nullopt;
  }
  return result;
}

std::optional<GeometryCopyResult> duplicate_components(const NurbsGeometry &source,
                                                       const ComponentSelection &selection,
                                                       std::string *r_error)
{
  auto copy = copy_component_closure(source, selection, r_error);
  if (!copy) {
    return std::nullopt;
  }
  OwnedGeometry candidate(geometry_copy(&source));
  candidate->next_id = copy->geometry->next_id;
  BLI_movelisttolist(&candidate->curves, &copy->geometry->curves);
  BLI_movelisttolist(&candidate->surfaces, &copy->geometry->surfaces);
  BLI_movelisttolist(&candidate->vertices, &copy->geometry->vertices);
  BLI_movelisttolist(&candidate->edges, &copy->geometry->edges);
  BLI_movelisttolist(&candidate->faces, &copy->geometry->faces);
  if (!geometry_validate_structure(*candidate, r_error)) {
    return std::nullopt;
  }
  copy->geometry = std::move(candidate);
  return copy;
}

}  // namespace blender::bke::nurbs
