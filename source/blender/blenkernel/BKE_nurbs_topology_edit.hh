/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

struct SelectionState;

/** An independent topology candidate and the source identities selected by the operation.
 * source_to_result maps retained/replaced active/history identities; zero means removed.
 * Output components name semantic results, not control coefficients or tessellation elements.
 */
struct TopologyEditResult {
  OwnedGeometry geometry;
  Map<int64_t, int64_t> source_to_result;
  Vector<int64_t> vertices, edges, faces;
  double geometry_error_bound = 0.0;
};

/** Disconnect a manifold face fan on the chosen side of a vertex/edge selection, or one wire
 * branch at a single selected junction. side_face/side_edge are source incidence hints supplied
 * by the editor's cursor query. Extend open interior path ends through the opposite vertex fan,
 * following mesh Rip's local fan split. Faces and uncut neighbors retain their identities.
 * Preserve all source coefficients, knots, trims and tolerances; isolate affected curve/support
 * ownership so later movement cannot deform the opposite side. No movement, fitting, owner
 * mutation or selection commit. Reject selected faces, disconnected vertices, branched paths,
 * non-manifold or ambiguous seam fans before mutation.
 */
std::optional<TopologyEditResult> rip_components(const NurbsGeometry &source,
                                                 const ComponentSelection &selection,
                                                 int64_t side_face,
                                                 int64_t side_edge,
                                                 std::string *r_error = nullptr);

struct VertexMergeGroup {
  Vector<int64_t> vertices;
  /** Must be a member. Retain this topology identity and move the group to target. */
  int64_t survivor = 0;
  double3 target;
};

/** Weld disjoint explicit vertex groups, planning quotient boundaries before deformation.
 * Remove only proven constant collapsed edges; closed rational curves remain real edges.
 * Coalesce equivalent edge representations and repair collapsed straight face boundaries with
 * planar/affine source construction within the explicit roundoff budget. Simple straight-sided
 * polynomial degree-one patches are rebuilt together from surviving cycles: affine triangles,
 * bilinear quads or exactly planar polygons. Preserve effective boundary orientation by incidence,
 * and coalesce equal newly reconstructed cycles. Other geometry uses the source deformation
 * contract. Preserve unaffected source geometry and remaining topology IDs.
 * Reject unsupported deformation, collapsed curved trims, incompatible parameterizations or
 * exceeded construction roundoff atomically. No proximity inference or hidden surface fitting.
 */
std::optional<TopologyEditResult> merge_vertex_groups(const NurbsGeometry &source,
                                                      Span<VertexMergeGroup> groups,
                                                      double max_position_error,
                                                      std::string *r_error = nullptr);

enum class VertexMergeMode { Center, Cursor, First, Last, Collapse };

/** Mesh merge mode planning over canonical source selection; cursor_target is object-local.
 * Center averages selected source vertices. Collapse groups connected selected edges and
 * averages their endpoint incidences, matching BMesh at junctions. First/Last require selected
 * vertex history. No control-net selection.
 */
std::optional<TopologyEditResult> merge_selected_vertices(const NurbsGeometry &source,
                                                          const SelectionState &selection,
                                                          VertexMergeMode mode,
                                                          const double3 &cursor_target,
                                                          double max_position_error,
                                                          std::string *r_error = nullptr);

/** Mesh-style duplicate clustering over source-precision vertex positions, followed by the
 * common topology weld. Unselected targets are anchors when requested. Centroid mode moves each
 * complete cluster to its mean; otherwise retain the stable most-central existing vertex.
 * The distance is object-local and distinct from the construction roundoff budget.
 */
std::optional<TopologyEditResult> merge_by_distance(const NurbsGeometry &source,
                                                    const SelectionState &selection,
                                                    double distance,
                                                    bool use_centroid,
                                                    bool use_unselected,
                                                    double max_position_error,
                                                    std::string *r_error = nullptr);

struct EdgePartitionRequest {
  int64_t edge_id;
  Vector<double> parameters;
};
struct FaceIsoCut {
  /** Fixed support coordinate, U=0 or V=1. */
  int axis;
  double parameter;
  /** Exact source-edge parameters at the low/high end of the varying coordinate. */
  std::array<double, 2> boundary_parameters;
};
struct FacePartitionRequest {
  int64_t face_id;
  Vector<FaceIsoCut> cuts;
};
struct TopologyCutPlan {
  Vector<EdgePartitionRequest> edges;
  Vector<FacePartitionRequest> faces;
  double transport_error_bound = 0;
};
struct TopologyCutResult {
  TopologyEditResult edit;
  Map<int64_t, Vector<int64_t>> edge_descendants;
  Map<int64_t, Vector<int64_t>> face_descendants;
};

/** Follow opposite sides of rectangular source faces, stopping at non-quads/boundaries.
 * Logical ring identities, not position welding, share intersections across supports. factor
 * shifts uniform cut parameters by one cut spacing and must stay strictly inside (-1,1).
 * Reject non-manifold or inconsistent closed-ring transport atomically.
 */
std::optional<TopologyCutPlan> plan_loop_cut(const NurbsGeometry &source,
                                             int64_t seed_edge,
                                             int cuts,
                                             double factor,
                                             double max_error,
                                             std::string *r_error = nullptr);

/** Split shared edges and every incident use, then partition rectangular faces in their own
 * NURBS parameter space. Crop supports/curves by shape-preserving refinement so the new faces
 * own editable subpatches. Preserve one original edge/face ID per partition and return all
 * descendants. New boundary identity is shared across faces, including periodic seam uses.
 * No mesh source, fitting, coordinate welding or live owner mutation.
 */
std::optional<TopologyCutResult> partition_components(const NurbsGeometry &source,
                                                      const TopologyCutPlan &plan,
                                                      double max_error,
                                                      std::string *r_error = nullptr);

/** Mesh-style uniform subdivision for wires and rectangular full/opposite-side selections.
 * Unsupported corner/trim patterns reject the complete candidate before source mutation.
 */
std::optional<TopologyEditResult> subdivide_components(const NurbsGeometry &source,
                                                       const ComponentSelection &selection,
                                                       int cuts,
                                                       double max_error,
                                                       std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
