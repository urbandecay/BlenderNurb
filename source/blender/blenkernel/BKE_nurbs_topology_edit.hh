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

enum class BevelSurfaceShape { Circular, Subdivision };

struct BevelOptions {
  /** Perpendicular face width. Ruled curved blends use the surface frame at the edge midpoint;
   * the corresponding natural parameter offset can have varying world width along a curve. */
  double offset = 0;
  /** One makes a chamfer; larger counts partition a smooth rational rounded strip. */
  int segments = 1;
  double max_error = 1e-9;
  /** Limit the whole selected network together before its first boundary collision. */
  bool clamp_overlap = true;
  /** Subdivision changes the surface shape, including the retained cage faces. Circular keeps
   * analytic rounding; Subdivision fits shared native patches to the bevel cage's limit. */
  BevelSurfaceShape surface_shape = BevelSurfaceShape::Circular;
};

struct BevelOffsetInfo {
  /** Actual requested or clamped offset, in object-local source units. */
  double offset = 0;
  /** Conservative upper limit with positive clearance for construction roundoff. */
  double limit = 0;
  /** Junctions using common conic profiles or affine octants. Shared boundaries are exact (G0),
   * but tangent continuity and a constant-radius interior are not certified. */
  int boundary_blend_corners = 0;
  /** Flat supports whose non-affine parameterization was replaced by a private plane chart. */
  int planar_charts = 0;
  double chart_error_bound = 0;
  /** Source-native conic strips on ruled supports; regular rail joins share tangent planes.
   * Width is measured in the perpendicular surface frame at the source edge midpoint.
   * A varying source frame does not imply a constant-radius fillet. */
  int curved_blend_strips = 0;
  /** Maximum sampled holdout deviation of a subdivision fit, not a certified global bound. */
  double subdivision_deviation = 0;
};

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

/** Bevel straight manifold edges between planar faces. Plan shared offsets and endpoint fans,
 * normalize flat non-affine charts, repair boundaries, and construct chamfer/rational strips.
 * Convex adjacent face boundaries and regular manifold fans are required; chamfers support higher
 * valence and planar miter reconstruction. Rounded corners currently require valence three. Fully
 * selected corners use mapped rational octants and mesh-guided complete curve rows. Right-angle
 * corners certify a spherical radius; other angles use affine supports, recorded in
 * BevelOffsetInfo. Width limiting uses source station motions, before constructing surfaces, and
 * preserves their relative offsets across the selected network. A single natural shared boundary
 * between compatible positive-rational ruled supports also admits exact isocurve rails, restricted
 * original supports and conic strips with regular G1 rail joins. Planar end caps are trimmed;
 * nonplanar caps need an intersection solver. Curved widths use the midpoint frame; general
 * circularity and curved self-collision are not certified. Subdivision surface shape fits native
 * polynomial patches to the Catmull-Clark limit of a closed triangular/quadrilateral bevel cage.
 * It also changes retained faces. Shared edges are authoritative; sampled target deviation is
 * reported separately from coefficient roundoff. Circular shape uses no fitting. Neither path
 * mutates source or edits a display mesh, performs general curved-support intersection, or
 * silently enlarges the construction tolerance.
 */
std::optional<TopologyEditResult> bevel_edges(const NurbsGeometry &source,
                                              Span<int64_t> edges,
                                              const BevelOptions &options,
                                              std::string *r_error = nullptr,
                                              BevelOffsetInfo *r_offset_info = nullptr);

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
