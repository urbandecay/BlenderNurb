/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_topology.hh"
#include "BKE_nurbs_topology_edit.hh"

#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs {

std::optional<TopologyEditResult> rip_components(const NurbsGeometry &source,
                                                 const ComponentSelection &selection,
                                                 const int64_t side_face,
                                                 const int64_t side_edge,
                                                 std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<TopologyEditResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  const auto index = TopologyIndex::build(source, r_error);
  const auto closure = component_closure(source, selection, r_error);
  if (!index || !closure) {
    return std::nullopt;
  }
  if (!closure->faces.is_empty()) {
    return fail("Cannot rip faces; select a vertex or edge path");
  }
  const bool single = closure->vertices.size() == 1 && closure->edges.is_empty();
  if (closure->vertices.is_empty() || (!single && closure->edges.is_empty())) {
    return fail("Cannot rip multiple disconnected vertices");
  }
  Vector<EdgeChain> chains;
  Map<int64_t, bool> side;
  if (!single) {
    auto traced = trace_edge_chains(source, selection.edges, r_error);
    if (!traced) {
      return std::nullopt;
    }
    chains = std::move(*traced);
    if (chains.size() != 1) {
      return fail("NURBS Rip requires one connected edge path");
    }
    const auto &chain = chains[0];
    int seed = -1;
    for (const int i : chain.edges.index_range()) {
      const auto uses = index->uses_for_edge(chain.edges[i]);
      if (uses.size() != 2 || uses[0].face_id == uses[1].face_id) {
        return fail("Cannot rip non-manifold or boundary edges");
      }
      for (const auto &use : uses) {
        if (use.face_id == side_face && (!side_edge || chain.edges[i] == side_edge)) {
          seed = i;
        }
      }
    }
    if (seed < 0) {
      return fail("NURBS Rip needs a face beside the selected path");
    }
    Vector<int64_t> chosen(chain.edges.size(), 0);
    chosen[seed] = side_face;
    auto propagate = [&](const int previous, const int next, const int64_t vertex_id) {
      Set<int64_t> reachable;
      Vector<int64_t> pending{chosen[previous]};
      reachable.add(chosen[previous]);
      while (!pending.is_empty()) {
        const int64_t face_id = pending.pop_last();
        for (const int64_t edge_id : index->edges_for_vertex(vertex_id)) {
          if (closure->edges.contains(edge_id)) {
            continue;
          }
          const auto uses = index->uses_for_edge(edge_id);
          if (uses.size() == 2 && (uses[0].face_id == face_id || uses[1].face_id == face_id)) {
            const int64_t other = uses[0].face_id == face_id ? uses[1].face_id : uses[0].face_id;
            if (reachable.add(other)) {
              pending.append(other);
            }
          }
        }
      }
      int64_t next_face = 0;
      for (const auto &use : index->uses_for_edge(chain.edges[next])) {
        if (reachable.contains(use.face_id)) {
          if (next_face) {
            return false;
          }
          next_face = use.face_id;
        }
      }
      if (!next_face || (chosen[next] && chosen[next] != next_face)) {
        return false;
      }
      chosen[next] = next_face;
      return true;
    };
    for (int i = seed + 1; i < chain.edges.size(); i++) {
      if (!propagate(i - 1, i, chain.vertices[i])) {
        return fail("NURBS Rip cannot resolve a consistent side along the edge path");
      }
    }
    for (int i = seed - 1; i >= 0; i--) {
      if (!propagate(i + 1, i, chain.vertices[i + 1])) {
        return fail("NURBS Rip cannot resolve a consistent side along the edge path");
      }
    }
    if (chain.closed && !propagate(int(chain.edges.size()) - 1, 0, chain.vertices[0])) {
      return fail("NURBS Rip cannot orient this closed edge path");
    }
    for (const int64_t face_id : chosen) {
      side.add_overwrite(face_id, true);
    }
  }

  Map<int64_t, Set<int64_t>> moving_faces;
  bool wire = false;
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (!closure->vertices.contains(vertex.id)) {
      continue;
    }
    Set<int64_t> faces, barriers;
    bool boundary = false;
    for (const int64_t edge_id : index->edges_for_vertex(vertex.id)) {
      const auto uses = index->uses_for_edge(edge_id);
      if (uses.size() > 2 || (uses.size() == 2 && uses[0].face_id == uses[1].face_id)) {
        return fail("Cannot rip non-manifold vertices or ambiguous seam fans");
      }
      boundary |= uses.size() == 1;
      for (const auto &use : uses) {
        faces.add(use.face_id);
      }
      if (closure->edges.contains(edge_id)) {
        barriers.add(edge_id);
      }
    }
    if (single && faces.is_empty()) {
      const auto *branch = index->edge(side_edge);
      if (!branch || (branch->vertex_ids[0] != vertex.id && branch->vertex_ids[1] != vertex.id) ||
          index->edges_for_vertex(vertex.id).size() < 2)
      {
        return fail("NURBS wire Rip needs a junction and a branch beside the cursor");
      }
      moving_faces.add(vertex.id, {});
      wire = true;
      continue;
    }
    if (faces.size() < 2) {
      return fail("Cannot rip a vertex with only one incident face");
    }
    if (single) {
      if (!faces.contains(side_face) || index->uses_for_edge(side_edge).size() != 2) {
        return fail("NURBS vertex Rip needs an incident manifold edge and face side");
      }
      /* Mesh's single-vertex path separates the loops beside the closest edge, rather than
       * treating that edge as the seam. A two-face boundary vertex separates one face. */
      Set<int64_t> moving;
      if (faces.size() == 2) {
        moving.add(side_face);
      }
      else {
        for (const auto &use : index->uses_for_edge(side_edge)) {
          moving.add(use.face_id);
        }
        if (!moving.contains(side_face)) {
          Set<int64_t> complement;
          for (const int64_t face_id : faces) {
            if (!moving.contains(face_id)) {
              complement.add(face_id);
            }
          }
          moving = std::move(complement);
        }
      }
      moving_faces.add_new(vertex.id, std::move(moving));
      continue;
    }
    Set<int64_t> seeds;
    for (const int64_t face_id : faces) {
      if (single ? face_id == side_face : side.contains(face_id)) {
        seeds.add(face_id);
      }
    }
    if (seeds.is_empty() || barriers.is_empty()) {
      return fail("NURBS Rip could not resolve the selected vertex fan");
    }
    /* An interior path endpoint needs a second seam through the opposite part of its fan.
     * Walk from the chosen side of the selected edge, as mesh Rip extends tagged edges locally.
     */
    if (!boundary && barriers.size() == 1) {
      const int64_t start_edge = *barriers.begin();
      int64_t face_id = *seeds.begin(), previous = start_edge;
      const int steps = std::max(1, int(faces.size() / 2));
      for (int i = 0; i < steps; i++) {
        int64_t next_edge = 0;
        for (const int64_t edge_id : index->edges_for_vertex(vertex.id)) {
          if (edge_id == previous) {
            continue;
          }
          for (const auto &use : index->uses_for_edge(edge_id)) {
            if (use.face_id == face_id) {
              if (next_edge && next_edge != edge_id) {
                return fail("NURBS Rip encountered a branched vertex fan");
              }
              next_edge = edge_id;
            }
          }
        }
        if (!next_edge) {
          return fail("NURBS Rip encountered an incomplete vertex fan");
        }
        if (i == steps - 1) {
          barriers.add(next_edge);
          break;
        }
        const auto uses = index->uses_for_edge(next_edge);
        if (uses.size() != 2) {
          return fail("NURBS Rip requires a manifold vertex fan");
        }
        face_id = uses[0].face_id == face_id ? uses[1].face_id : uses[0].face_id;
        previous = next_edge;
      }
    }
    Set<int64_t> moving;
    Vector<int64_t> pending;
    for (const int64_t seed : seeds) {
      if (moving.add(seed)) {
        pending.append(seed);
      }
    }
    while (!pending.is_empty()) {
      const int64_t face_id = pending.pop_last();
      for (const int64_t edge_id : index->edges_for_vertex(vertex.id)) {
        if (barriers.contains(edge_id)) {
          continue;
        }
        const auto uses = index->uses_for_edge(edge_id);
        if (uses.size() == 2 && (uses[0].face_id == face_id || uses[1].face_id == face_id)) {
          const int64_t other = uses[0].face_id == face_id ? uses[1].face_id : uses[0].face_id;
          if (moving.add(other)) {
            pending.append(other);
          }
        }
      }
    }
    if (moving.size() == faces.size()) {
      return fail("NURBS Rip did not divide the incident face fan");
    }
    moving_faces.add_new(vertex.id, std::move(moving));
  }

  detail::TopologyBuilder builder(source);
  TopologyEditResult result;
  Map<int64_t, int64_t> copies;
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (moving_faces.contains(vertex.id)) {
      auto *copy = MEM_dupalloc(&vertex);
      copy->id = builder.id();
      BLI_addtail(&builder.geometry->vertices, copy);
      copies.add_new(vertex.id, copy->id);
      result.source_to_result.add_new(vertex.id, copy->id);
      result.vertices.append(copy->id);
    }
  }
  Set<int64_t> changed_faces;
  for (const NurbsTopologyEdge &edge : source.edges) {
    Map<int, Vector<int64_t>> masks;
    for (const auto &reference : index->uses_for_edge(edge.id)) {
      int mask = 0;
      for (int end = 0; end < 2; end++) {
        if (const auto *moving = moving_faces.lookup_ptr(edge.vertex_ids[end])) {
          if (moving->contains(reference.face_id)) {
            mask |= 1 << end;
          }
        }
      }
      masks.lookup_or_add_default(mask).append(reference.face_id);
    }
    if (wire && edge.id == side_edge) {
      int mask = 0;
      for (int end = 0; end < 2; end++) {
        mask |= copies.contains(edge.vertex_ids[end]) ? 1 << end : 0;
      }
      masks.add_overwrite(mask, {});
    }
    if (masks.is_empty() || (masks.size() == 1 && masks.contains(0))) {
      continue;
    }
    bool retained = masks.contains(0);
    for (int mask = 1; mask <= 3; mask++) {
      const auto *faces = masks.lookup_ptr(mask);
      if (!faces) {
        continue;
      }
      NurbsTopologyEdge *changed = builder.edges.lookup(edge.id);
      if (retained) {
        changed = MEM_dupalloc(changed);
        changed->id = builder.id();
        BLI_addtail(&builder.geometry->edges, changed);
      }
      retained = true;
      changed->curve_id = builder.copy_curve(edge.curve_id)->id;
      for (int end = 0; end < 2; end++) {
        changed->vertex_ids[end] = mask & (1 << end) ? copies.lookup(edge.vertex_ids[end]) :
                                                       edge.vertex_ids[end];
      }
      for (const int64_t face_id : *faces) {
        changed_faces.add(face_id);
        for (auto &loop : builder.faces.lookup(face_id)->loops) {
          for (auto &use : loop.edge_uses) {
            if (use.edge_id == edge.id) {
              use.edge_id = changed->id;
            }
          }
        }
      }
      if (closure->edges.contains(edge.id)) {
        result.source_to_result.add_overwrite(edge.id, changed->id);
        result.edges.append(changed->id);
      }
    }
  }
  for (auto &face : builder.geometry->faces) {
    if (changed_faces.contains(face.id)) {
      face.surface_id = builder.copy_surface(face.surface_id)->id;
    }
  }
  if (!builder.valid()) {
    return fail("NURBS identity space is exhausted");
  }
  if (!geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  return result;
}

}  // namespace blender::bke::nurbs
