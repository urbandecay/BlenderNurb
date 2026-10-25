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

static OwnedGeometry apply_deletion_plan(const NurbsGeometry &source,
                                         const TopologyDeletionPlan &plan,
                                         std::string *r_error)
{
  OwnedGeometry candidate(geometry_copy(&source));
  for (NurbsTopologyFace &face : candidate->faces.items_mutable()) {
    if (plan.faces.contains(face.id)) {
      for (NurbsTopologyLoop &loop : face.loops) {
        BLI_freelistN(&loop.edge_uses);
      }
      BLI_freelistN(&face.loops);
      BLI_freelinkN(&candidate->faces, &face);
    }
  }
  for (NurbsTopologyEdge &edge : candidate->edges.items_mutable()) {
    if (plan.edges.contains(edge.id)) {
      BLI_freelinkN(&candidate->edges, &edge);
    }
  }
  for (NurbsTopologyVertex &vertex : candidate->vertices.items_mutable()) {
    if (plan.vertices.contains(vertex.id)) {
      BLI_freelinkN(&candidate->vertices, &vertex);
    }
  }
  if (source.stored_selection != nullptr) {
    const auto stored = selection_from_storage(source.stored_selection, r_error);
    if (!stored) {
      return nullptr;
    }
    Set<int64_t> removed = plan.vertices;
    for (const int64_t id : plan.edges) {
      removed.add(id);
    }
    for (const int64_t id : plan.faces) {
      removed.add(id);
    }
    const SelectionState surviving = selection_remove_ids(*stored, removed);
    selection_storage_free(candidate->stored_selection);
    candidate->stored_selection = selection_to_storage(surviving, r_error);
    if (candidate->stored_selection == nullptr) {
      return nullptr;
    }
  }
  if (!geometry_validate_structure(*candidate, r_error)) {
    return nullptr;
  }
  return candidate;
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
  auto candidate = apply_deletion_plan(source, *plan, r_error);
  if (!candidate) {
    return std::nullopt;
  }
  return TopologyDeletionResult{std::move(candidate), std::move(*plan)};
}

std::optional<ComponentExtractionResult> extract_components(const NurbsGeometry &source,
                                                            const ComponentSelection &selection,
                                                            std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto extracted = copy_component_closure(source, selection, r_error);
  if (!extracted) {
    return std::nullopt;
  }
  const auto closure = component_closure(source, selection, r_error);
  const auto index = TopologyIndex::build(source, r_error);
  if (!closure || !index) {
    return std::nullopt;
  }
  TopologyDeletionPlan removed;
  removed.vertices.add_multiple(selection.vertices);
  removed.edges = closure->edges;
  removed.faces = closure->faces;
  for (const int64_t face_id : closure->faces) {
    for (const NurbsTopologyLoop &loop : index->face(face_id)->loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        removed.vertices.add_multiple(Span(index->edge(use.edge_id)->vertex_ids, 2));
      }
    }
  }
  /* Like BMesh Split's DEL_FACES context, retain every original dependency needed by an
   * unselected face or surviving edge. A copied wire endpoint not in the input vertex selection
   * remains as an original loose point: copying dependencies does not select them for deletion. */
  for (const int64_t id : closure->edges) {
    for (const EdgeUseReference &use : index->uses_for_edge(id)) {
      if (!removed.faces.contains(use.face_id)) {
        removed.edges.remove(id);
        break;
      }
    }
  }
  for (const int64_t id : closure->vertices) {
    for (const int64_t edge_id : index->edges_for_vertex(id)) {
      if (!removed.edges.contains(edge_id)) {
        removed.vertices.remove(id);
        break;
      }
    }
  }
  auto remainder = apply_deletion_plan(source, removed, r_error);
  if (!remainder) {
    return std::nullopt;
  }
  /* Both results allocate from one source identity space. Only the fragment uses new IDs,
   * but reserving its counter on the remainder also makes a later reunion safe. */
  remainder->next_id = extracted->geometry->next_id;
  return ComponentExtractionResult{
      std::move(remainder), std::move(*extracted), std::move(removed)};
}

std::optional<GeometryCopyResult> split_components(const NurbsGeometry &source,
                                                   const ComponentSelection &selection,
                                                   std::string *r_error)
{
  auto extraction = extract_components(source, selection, r_error);
  if (!extraction) {
    return std::nullopt;
  }
  auto &candidate = extraction->remainder;
  auto &fragment = extraction->extracted.geometry;
  BLI_movelisttolist(&candidate->curves, &fragment->curves);
  BLI_movelisttolist(&candidate->surfaces, &fragment->surfaces);
  BLI_movelisttolist(&candidate->vertices, &fragment->vertices);
  BLI_movelisttolist(&candidate->edges, &fragment->edges);
  BLI_movelisttolist(&candidate->faces, &fragment->faces);
  if (!geometry_validate_structure(*candidate, r_error)) {
    return std::nullopt;
  }
  return GeometryCopyResult{std::move(candidate), std::move(extraction->extracted.source_to_copy)};
}

std::optional<Vector<EdgeChain>> trace_edge_chains(const NurbsGeometry &source,
                                                   const Span<int64_t> edge_ids,
                                                   std::string *r_error)
{
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  Set<int64_t> selected;
  for (const int64_t id : edge_ids) {
    if (index->edge(id) == nullptr) {
      if (r_error != nullptr) {
        *r_error = "Boundary tracing references an unknown NURBS edge";
      }
      return std::nullopt;
    }
    selected.add(id);
  }
  Map<int64_t, Vector<int64_t>> incidence;
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (selected.contains(edge.id)) {
      // A closed edge contributes both incidences at the same vertex.
      incidence.lookup_or_add_default(edge.vertex_ids[0]).append(edge.id);
      incidence.lookup_or_add_default(edge.vertex_ids[1]).append(edge.id);
    }
  }
  for (const auto &entry : incidence.items()) {
    if (entry.value.size() > 2) {
      if (r_error != nullptr) {
        *r_error = "Selected NURBS edges branch; a boundary pairing is required";
      }
      return std::nullopt;
    }
  }
  Vector<EdgeChain> result;
  Set<int64_t> visited;
  auto trace = [&](const int64_t start) {
    EdgeChain chain;
    chain.vertices.append(start);
    int64_t current = start;
    while (true) {
      int64_t next_edge = 0;
      for (const int64_t id : incidence.lookup(current)) {
        if (!visited.contains(id)) {
          next_edge = id;
          break;
        }
      }
      if (next_edge == 0) {
        break;
      }
      const auto &edge = *index->edge(next_edge);
      const bool reversed = edge.vertex_ids[0] != current;
      current = edge.vertex_ids[reversed ? 0 : 1];
      visited.add(next_edge);
      chain.edges.append(next_edge);
      chain.reversed.append(reversed);
      chain.vertices.append(current);
      if (current == start) {
        chain.closed = true;
        break;
      }
    }
    if (!chain.edges.is_empty()) {
      result.append(std::move(chain));
    }
  };
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (const auto *edges = incidence.lookup_ptr(vertex.id)) {
      if (edges->size() == 1 && !visited.contains(edges->first())) {
        trace(vertex.id);
      }
    }
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (selected.contains(edge.id) && !visited.contains(edge.id)) {
      trace(edge.vertex_ids[0]);
    }
  }
  return result;
}

}  // namespace blender::bke::nurbs
