/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>

#include "BLI_listbase.h"

#include "BKE_nurbs_topology.hh"

namespace blender::bke::nurbs {

std::optional<TopologyDeletionPlan> plan_component_deletion(const NurbsGeometry &source,
                                                            const ComponentSelection &selection,
                                                            const DeleteMode mode,
                                                            std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<TopologyDeletionPlan> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  for (const int64_t id : selection.vertices) {
    if (index->vertex(id) == nullptr) {
      return fail("Selected NURBS vertex identity does not exist");
    }
  }
  for (const int64_t id : selection.edges) {
    if (index->edge(id) == nullptr) {
      return fail("Selected NURBS edge identity does not exist");
    }
  }
  for (const int64_t id : selection.faces) {
    if (index->face(id) == nullptr) {
      return fail("Selected NURBS face identity does not exist");
    }
  }
  TopologyDeletionPlan result;
  Set<int64_t> candidate_vertices, candidate_edges;
  switch (mode) {
    case DeleteMode::Vertices:
      result.vertices.add_multiple(selection.vertices);
      for (const NurbsTopologyEdge &edge : source.edges) {
        if (result.vertices.contains(edge.vertex_ids[0]) ||
            result.vertices.contains(edge.vertex_ids[1]))
        {
          result.edges.add(edge.id);
        }
      }
      break;
    case DeleteMode::Edges:
      result.edges.add_multiple(selection.edges);
      for (const int64_t id : result.edges) {
        candidate_vertices.add_multiple(Span(index->edge(id)->vertex_ids, 2));
      }
      break;
    case DeleteMode::Faces:
    case DeleteMode::FacesOnly:
      result.faces.add_multiple(selection.faces);
      for (const int64_t id : result.faces) {
        for (const NurbsTopologyLoop &loop : index->face(id)->loops) {
          for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
            candidate_edges.add(use.edge_id);
            candidate_vertices.add_multiple(Span(index->edge(use.edge_id)->vertex_ids, 2));
          }
        }
      }
      if (mode == DeleteMode::FacesOnly) {
        return result;
      }
      for (const int64_t id : candidate_edges) {
        const Span<EdgeUseReference> uses = index->uses_for_edge(id);
        if (std::all_of(uses.begin(), uses.end(), [&](const EdgeUseReference &use) {
              return result.faces.contains(use.face_id);
            }))
        {
          result.edges.add(id);
        }
      }
      break;
    default:
      return fail("Unknown NURBS component deletion mode");
  }
  for (const int64_t id : result.edges) {
    for (const EdgeUseReference &use : index->uses_for_edge(id)) {
      result.faces.add(use.face_id);
    }
  }
  for (const int64_t id : candidate_vertices) {
    const Span<int64_t> edges = index->edges_for_vertex(id);
    if (std::all_of(edges.begin(), edges.end(), [&](const int64_t edge_id) {
          return result.edges.contains(edge_id);
        }))
    {
      result.vertices.add(id);
    }
  }
  return result;
}

}  // namespace blender::bke::nurbs
