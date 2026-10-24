/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_set.hh"

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

struct LinkedComponents {
  Vector<int64_t> vertices;
  Vector<int64_t> edges;
  Vector<int64_t> faces;
};

enum class LinkedConnectivity {
  /** Follow only shared endpoint/edge incidence; separate loops remain separate. */
  VertexGraph,
  /** Every boundary of an incident face belongs to the same surface component. */
  FaceBoundaries,
  /** Reject when the two rules would return different components. */
  RequireAgreement,
};

/**
 * Find the connected source component containing each seed using an explicit connectivity rule.
 * Face seeds always include all their boundaries. Coincident positions and shared geometry
 * resources do not connect topology. Returned faces have all their boundary edges in the result.
 * Includes loose vertices, wire edges and non-manifold incidence. Results follow source order,
 * independent of seed order or repetition. No geometry evaluation, mutation or ID allocation.
 */
std::optional<LinkedComponents> find_linked_components(const NurbsGeometry &source,
                                                       const ComponentSelection &seeds,
                                                       LinkedConnectivity connectivity,
                                                       std::string *r_error = nullptr);

struct EdgeChain {
  /** One more vertex than edge; the first and last identity agree for a closed chain. */
  Vector<int64_t> vertices;
  Vector<int64_t> edges;
  Vector<bool> reversed;
  bool closed = false;
};

/** Trace selected source incidence in deterministic source order. Repeated edge IDs have no
 * effect. Self loops and parallel curved edges retain their identities. Reject branches instead
 * of guessing a boundary pairing. Empty input returns no chains. No geometry or ID allocation.
 */
std::optional<Vector<EdgeChain>> trace_edge_chains(const NurbsGeometry &source,
                                                   Span<int64_t> edges,
                                                   std::string *r_error = nullptr);

enum class DeleteMode { Vertices, Edges, Faces, FacesOnly };

struct TopologyDeletionPlan {
  Set<int64_t> vertices;
  Set<int64_t> edges;
  Set<int64_t> faces;
};

/**
 * Plan component deletion using shared source incidence. Vertex deletion removes incident edges
 * and faces; edge deletion removes incident faces and newly loose edge endpoints. Face deletion
 * removes dependencies unused by surviving faces/edges; FacesOnly retains its boundary geometry.
 * Unrelated loose components survive. No support shrinking, fitting, mutation or ID allocation.
 * Selection order and repeated IDs have no effect. These are Blender/BMesh topology semantics,
 * independent of a NURBS source owner or a polygon/display representation.
 */
std::optional<TopologyDeletionPlan> plan_component_deletion(const NurbsGeometry &source,
                                                            const ComponentSelection &selection,
                                                            DeleteMode mode,
                                                            std::string *r_error = nullptr);

struct TopologyDeletionResult {
  OwnedGeometry geometry;
  TopologyDeletionPlan removed;
};

/**
 * Apply the shared deletion plan to a full independent source candidate. Keep remaining records,
 * coefficient resources, tolerances and the identity counter unchanged. Remove invalid stored
 * selection/history references without inferring a component mode. No fitting, support shrinking,
 * geometry-resource collection, owner mutation or notification occurs.
 */
std::optional<TopologyDeletionResult> delete_components(const NurbsGeometry &source,
                                                        const ComponentSelection &selection,
                                                        DeleteMode mode,
                                                        std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
