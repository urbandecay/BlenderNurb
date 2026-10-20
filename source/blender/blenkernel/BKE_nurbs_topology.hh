/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <cstddef>
#include <string>

#include "BLI_vector.hh"

#include "DNA_nurbs_topology_types.h"

namespace blender {

struct BlendDataReader;
struct BlendWriter;

/** Remapping of one edge use to its two ordered child uses after an edge split. */
struct NurbsEdgeUseSplit {
  uint64_t source_id = 0;
  uint64_t child_ids[2] = {};
};

/** Stable-ID remapping produced by an exact topological edge split. */
struct NurbsEdgeSplitResult {
  uint64_t source_edge_id = 0;
  uint64_t child_edge_ids[2] = {};
  uint64_t vertex_id = 0;
  Vector<NurbsEdgeUseSplit> edge_uses;
};

/** Deep-copy topology supports and incidence records, preserving component IDs. */
NurbsTopology *BKE_nurbs_topology_copy(const NurbsTopology &topology);

/** Free topology and all support, loop, and edge-use allocations it owns. */
void BKE_nurbs_topology_free(NurbsTopology *topology);

/** Set or clear one topology component's selection state and update ordered selection history. */
bool BKE_nurbs_topology_selection_set(NurbsTopology &topology,
                                      char type,
                                      uint64_t component_id,
                                      bool select,
                                      bool make_active = true);

/** Clear all component selection flags, history entries, and the active component. */
void BKE_nurbs_topology_selection_clear(NurbsTopology &topology);

/** Check whether a topology component of the given type is selected. */
bool BKE_nurbs_topology_selection_is_selected(const NurbsTopology &topology,
                                              char type,
                                              uint64_t component_id);

/** Write and read topology-owned arrays. The caller writes/reads the topology struct itself and
 * validates references to object-local 3D curves after reading. */
void BKE_nurbs_topology_blend_write(BlendWriter *writer, const NurbsTopology &topology);
bool BKE_nurbs_topology_blend_read(BlendDataReader *reader, NurbsTopology &topology);

/** Return the next unused component ID, or zero when the ID space is exhausted. */
uint64_t BKE_nurbs_component_id_next(const ListBaseT<NurbsCurve> &curves,
                                     const NurbsTopology *topology);

/** Memory footprint of all topology records and owned arrays. */
std::size_t BKE_nurbs_topology_memory_size(const NurbsTopology &topology);

/** Validate supports, references, ordered loops, and incidence invariants. Edge/surface agreement
 * is sampled and UV loop closure and winding are measured within the data-block tolerances; these
 * checks do not certify a global geometric bound between samples. */
bool BKE_nurbs_topology_is_valid(const ListBaseT<NurbsCurve> &curves,
                                 const NurbsTopology &topology,
                                 std::string *r_error = nullptr);

/** Replace an edge by two fresh edge IDs and remap every incident loop use.
 * Failure leaves topology, ID allocation, and result unchanged. */
bool BKE_nurbs_topology_edge_split(const ListBaseT<NurbsCurve> &curves,
                                   NurbsTopology &topology,
                                   uint64_t &next_component_id,
                                   uint64_t edge_id,
                                   double parameter,
                                   NurbsEdgeSplitResult &r_result,
                                   std::string *r_error = nullptr);

}  // namespace blender
