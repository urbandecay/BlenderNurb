/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>

#include "BLI_listbase.h"

#include "BKE_nurbs_selection.hh"
#include "BKE_nurbs_topology.hh"

namespace blender::bke::nurbs {

std::optional<LinkedComponents> find_linked_components(const NurbsGeometry &source,
                                                       const ComponentSelection &seeds,
                                                       const LinkedConnectivity connectivity,
                                                       std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Set<int64_t> vertices, edges, faces;
  Vector<int64_t> pending_vertices;
  auto add_vertex = [&](const int64_t id) {
    if (vertices.add(id)) {
      pending_vertices.append(id);
    }
  };
  auto add_edge_vertices = [&](const NurbsTopologyEdge &edge) {
    add_vertex(edge.vertex_ids[0]);
    add_vertex(edge.vertex_ids[1]);
  };
  auto add_face = [&](const NurbsTopologyFace &face) {
    if (faces.add(face.id)) {
      for (const NurbsTopologyLoop &loop : face.loops) {
        for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
          add_edge_vertices(*index->edge(use.edge_id));
        }
      }
    }
  };
  auto fail = [&](const char *message) -> std::optional<LinkedComponents> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!ELEM(connectivity,
            LinkedConnectivity::VertexGraph,
            LinkedConnectivity::FaceBoundaries,
            LinkedConnectivity::RequireAgreement))
  {
    return fail("Unknown NURBS linked connectivity rule");
  }
  for (const int64_t id : seeds.vertices) {
    if (index->vertex(id) == nullptr) {
      return fail("Linked NURBS vertex identity does not exist");
    }
    add_vertex(id);
  }
  for (const int64_t id : seeds.edges) {
    const auto *edge = index->edge(id);
    if (edge == nullptr) {
      return fail("Linked NURBS edge identity does not exist");
    }
    add_edge_vertices(*edge);
  }
  for (const int64_t id : seeds.faces) {
    const auto *face = index->face(id);
    if (face == nullptr) {
      return fail("Linked NURBS face identity does not exist");
    }
    add_face(*face);
  }
  while (!pending_vertices.is_empty()) {
    const int64_t vertex_id = pending_vertices.pop_last();
    for (const int64_t edge_id : index->edges_for_vertex(vertex_id)) {
      if (edges.add(edge_id)) {
        add_edge_vertices(*index->edge(edge_id));
        if (connectivity == LinkedConnectivity::FaceBoundaries) {
          for (const EdgeUseReference &use : index->uses_for_edge(edge_id)) {
            add_face(*index->face(use.face_id));
          }
        }
      }
    }
  }
  LinkedComponents result;
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (vertices.contains(vertex.id)) {
      result.vertices.append(vertex.id);
    }
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (edges.contains(edge.id)) {
      result.edges.append(edge.id);
    }
  }
  for (const NurbsTopologyFace &face : source.faces) {
    bool all_edges = true, any_edge = false;
    for (const NurbsTopologyLoop &loop : face.loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const bool included = edges.contains(use.edge_id);
        all_edges &= included;
        any_edge |= included;
      }
    }
    if (connectivity == LinkedConnectivity::RequireAgreement && any_edge && !all_edges) {
      return fail(
          "Linked selection on this NURBS face needs an explicit boundary or surface rule");
    }
    if (all_edges) {
      result.faces.append(face.id);
    }
  }
  return result;
}

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

std::optional<TopologyDeletionResult> delete_components(const NurbsGeometry &source,
                                                        const ComponentSelection &selection,
                                                        const DeleteMode mode,
                                                        std::string *r_error)
{
  auto plan = plan_component_deletion(source, selection, mode, r_error);
  if (!plan) {
    return std::nullopt;
  }
  OwnedGeometry candidate(geometry_copy(&source));
  for (NurbsTopologyFace &face : candidate->faces.items_mutable()) {
    if (plan->faces.contains(face.id)) {
      for (NurbsTopologyLoop &loop : face.loops) {
        BLI_freelistN(&loop.edge_uses);
      }
      BLI_freelistN(&face.loops);
      BLI_freelinkN(&candidate->faces, &face);
    }
  }
  for (NurbsTopologyEdge &edge : candidate->edges.items_mutable()) {
    if (plan->edges.contains(edge.id)) {
      BLI_freelinkN(&candidate->edges, &edge);
    }
  }
  for (NurbsTopologyVertex &vertex : candidate->vertices.items_mutable()) {
    if (plan->vertices.contains(vertex.id)) {
      BLI_freelinkN(&candidate->vertices, &vertex);
    }
  }
  if (source.stored_selection != nullptr) {
    const auto stored = selection_from_storage(source.stored_selection, r_error);
    if (!stored) {
      return std::nullopt;
    }
    Set<int64_t> removed = plan->vertices;
    for (const int64_t id : plan->edges) {
      removed.add(id);
    }
    for (const int64_t id : plan->faces) {
      removed.add(id);
    }
    const SelectionState surviving = selection_remove_ids(*stored, removed);
    selection_storage_free(candidate->stored_selection);
    candidate->stored_selection = selection_to_storage(surviving);
  }
  if (!geometry_validate_structure(*candidate, r_error)) {
    return std::nullopt;
  }
  return TopologyDeletionResult{std::move(candidate), std::move(*plan)};
}

}  // namespace blender::bke::nurbs
