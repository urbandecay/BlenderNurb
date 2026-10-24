/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_topology.hh"

namespace blender::bke::nurbs {

struct FaceRegionExtrusionPlan {
  /** Unique source identities, in source order. */
  Vector<int64_t> faces;
  Vector<int64_t> boundary_edges;
  Vector<int64_t> interior_edges;
  Vector<int64_t> interior_vertices;
  /** Match Blender's region rule: replace input faces when they border an unselected face. */
  bool remove_original_faces = false;
};

/**
 * Classify a face region using shared edge uses, independently of geometry or selection gesture.
 * A single selected use is a wall boundary; two selected uses are internal, including a seam.
 * Reject more than two selected uses per edge rather than guess a non-manifold sweep.
 * Holes contribute their own boundary walls. This is topology planning, not support construction
 * or a geometric/solid validity certificate. Input order and repeated IDs have no effect.
 */
std::optional<FaceRegionExtrusionPlan> plan_face_region_extrusion(const NurbsGeometry &source,
                                                                  Span<int64_t> face_ids,
                                                                  std::string *r_error = nullptr);

struct ExtrusionResult {
  OwnedGeometry geometry;
  /** Fresh cap identities, including shared curve resources and endpoints. */
  Map<int64_t, int64_t> source_to_cap;
  Map<int64_t, int64_t> vertex_to_rail;
  Map<int64_t, int64_t> edge_to_face;
  /** Original components removed when replacing a face region. */
  TopologyDeletionPlan removed;
  /** Conservative coefficient translation roundoff bound, not an evaluation error certificate. */
  double translation_error_bound = 0.0;
  /** Measured input endpoint discrepancy already covered by source vertex/edge tolerance. */
  double max_source_endpoint_deviation = 0.0;
};

/** Existing edge-only callers share the same construction result. */
using EdgeExtrusionResult = ExtrusionResult;

/**
 * Extrude a source component closure using one shared cap and one rail per boundary vertex.
 * Selected faces contribute walls only at region boundaries (including holes, excluding seams).
 * Independent selected wires/vertices are swept in the same candidate and share cap identities.
 * Match the region plan's original-face replacement rule; retained originals reverse face sense
 * while copied cap parameterizations stay unchanged. Walls follow declared oriented source uses;
 * no trim-winding or solid-validity certificate is implied. Deleted stored selection references
 * are pruned, but selection of the new cap and owner commits remain the caller's responsibility.
 * The arithmetic, positive-weight, endpoint agreement and explicit error-budget requirements of
 * extrude_edges apply. Input order/repetition have no effect. No fitting or coordinate welding.
 */
std::optional<ExtrusionResult> extrude_components(const NurbsGeometry &source,
                                                  const ComponentSelection &selection,
                                                  const double3 &offset,
                                                  double max_translation_error,
                                                  std::string *r_error = nullptr);

/**
 * Construct a full candidate by translating selected edges and connecting them with exact ruled
 * supports. Original edges remain shared. One copied vertex and one rail are made per source
 * vertex, including across multiple incident edges. Repeated/input-order IDs have no effect.
 * Zero displacement is allowed as a singular editable intermediate.
 * New faces oppose the oriented source boundary when its incident uses agree. Wires and
 * conflicting/non-manifold orientations use the sweep's parameter orientation; no outside of
 * a solid is inferred.
 *
 * Requires positive rational weights. Existing endpoint/edge tolerance must cover measured source
 * endpoint agreement. Requires IEEE round-to-nearest arithmetic with subnormal support; a
 * different floating-point environment is rejected, not changed. The caller supplies an explicit
 * absolute roundoff budget; zero requires exact coefficient additions. The reported bound covers
 * translation of positive-weight source coefficients, not pre-existing geometric error, evaluation
 * error, trim validity or solidness. No fitting, coordinate welding, source mutation, selection
 * change or editor notification occurs.
 */
std::optional<EdgeExtrusionResult> extrude_edges(const NurbsGeometry &source,
                                                 Span<int64_t> edge_ids,
                                                 const double3 &offset,
                                                 double max_translation_error,
                                                 std::string *r_error = nullptr);

struct FillResult {
  OwnedGeometry geometry;
  Vector<int64_t> created_edges;
  Vector<int64_t> created_faces;
  TopologyDeletionPlan removed;
};

/** Connect two selected vertices, fill one simple boundary, or merge one planar face region.
 * Reuse source vertices/edges; report created identities and removed region interiors. An existing
 * edge/face is an unchanged full candidate. Four straight sides use an interpolating bilinear
 * NURBS support, including nonplanar corners. Other supported boundaries use an exact planar
 * chart; unavailable exact correspondence rejects rather than fitting or flattening. Initial
 * curved-plane support requires full clamped positive piecewise-Bezier curves and a simple
 * convex control polygon. No arbitrary trim validity, continuity or solid certificate is implied.
 * Source, selection and its identity counter remain unchanged. No display geometry is used.
 */
std::optional<FillResult> fill_components(const NurbsGeometry &source,
                                          const ComponentSelection &selection,
                                          std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
