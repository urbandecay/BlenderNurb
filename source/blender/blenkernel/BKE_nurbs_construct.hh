/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_topology.hh"

namespace blender::bke::nurbs {

/** Exact positive rational quadratic circular arc, with spans of at most 90 degrees.
 * The start lies on the plane through center with the supplied normal. Sweep is signed,
 * nonzero and at most one revolution. Domain [0, 1]; no source identity is assigned.
 * Coefficients carry ordinary double-precision construction roundoff. */
OwnedCurve circular_arc(const double3 &center,
                        const double3 &start,
                        const double3 &normal,
                        double sweep,
                        std::string *r_error = nullptr);

/** Circular arc from start to end through the third point, including major arcs.
 * Reject coincident/collinear input instead of silently substituting a line. */
OwnedCurve circular_arc_through_points(const double3 &start,
                                       const double3 &through,
                                       const double3 &end,
                                       std::string *r_error = nullptr);

struct SurfaceConstruction {
  OwnedSurface surface;
  double geometry_error_bound = 0;
};

struct QuadChartResult {
  OwnedGeometry geometry;
  /** Whole-face deviation from the input support, including its measured boundary gap. */
  double geometry_error_bound = 0;
};

struct PlanarChartResult {
  OwnedGeometry geometry;
  /** Whole-support projection and boundary reconstruction bound; no fitted surface. */
  double geometry_error_bound = 0;
};

/** Reparameterize planar polynomial bilinear faces with affine plane charts. A trapezoid is
 * planar even though its original bilinear chart has a nonzero mixed coefficient. Certify
 * distance from the entire positive support to the chosen plane; reject actual curvature
 * outside max_error. Enclose spatial boundary control hulls in the new chart, copy spatial
 * curves and topology unchanged, and allocate private supports/p-curves. */
std::optional<PlanarChartResult> reparameterize_affine_planes(const NurbsGeometry &source,
                                                              Span<int64_t> faces,
                                                              double max_error,
                                                              std::string *r_error = nullptr);

/** Give convex straight-sided quads on polynomial bilinear supports complete unit tensor
 * charts. Bound support twist and input boundary discrepancy over the whole reparameterized
 * region. Preserve vertices, 3D curves, edges, face/use identities, selection and face senses;
 * allocate private supports and p-curves so other owners remain unchanged. No fitting or mesh
 * conversion. Support twist and boundary discrepancy must fit the explicit operation budget;
 * larger curvature or an invalid input boundary rejects atomically. */
std::optional<QuadChartResult> reparameterize_planar_quads(const NurbsGeometry &source,
                                                           Span<int64_t> faces,
                                                           double max_error,
                                                           std::string *r_error = nullptr);

struct TriangleBlendShape {
  /** Optional exact interior tensor control and weight on the collapsed top edge. */
  NurbsControlPoint interior;
  double collapsed_weight = 1;
};

/** Homogeneous Coons interpolation of three clamped positive rational quadratic boundaries.
 * The curves run A->B, A->C and B->C, have endpoint weights one and parameter domain [0, 1].
 * The top parameter boundary collapses to C. Copy boundary coefficients exactly and bound
 * interior coefficient construction roundoff. This interpolates positions (G0); it does not
 * certify tangent continuity or circular/spherical interior geometry. Reject nonpositive
 * interior weights rather than fitting or changing boundary geometry. An optional shape adds
 * a tensor interior/collapsed-boundary correction without changing any spatial boundary. */
std::optional<SurfaceConstruction> rational_coons_triangle(
    const NurbsCurveGeometry &ab,
    const NurbsCurveGeometry &ac,
    const NurbsCurveGeometry &bc,
    double max_error,
    std::string *r_error = nullptr,
    const TriangleBlendShape *shape = nullptr);

/** Affine image of a spherical octant, with a collapsed top parameter boundary. The three axes
 * locate its equator endpoints and pole relative to center. Positive rational quadratic tensor
 * construction; report coefficient/weight roundoff relative to the stored center and axes.
 * No fitting, identity allocation or source mutation. Singular axis frames remain valid editable
 * geometry; regularity and solid eligibility are separate contracts. */
std::optional<SurfaceConstruction> rational_octant_surface(const double3 &center,
                                                           const std::array<double3, 3> &axes,
                                                           double max_error,
                                                           std::string *r_error = nullptr);

/** Construct an octant and certify its full radial deviation from a sphere centered at center.
 * The error includes the Gram-matrix bound of the stored axes and coefficient/weight roundoff.
 * Axes are never snapped or fitted; reject when the requested budget cannot cover the result.
 * This allows nominally spherical constructions after source-precision affine arithmetic. */
std::optional<SurfaceConstruction> rational_spherical_octant_surface(
    const double3 &center,
    const std::array<double3, 3> &axes,
    double radius,
    double max_error,
    std::string *r_error = nullptr);

/** Join two compatible positive-rational curves by a degree-one ruled direction. The curves
 * must have identical bases. Their positive weights may differ; the ruled direction then has
 * a rational parameterization of each straight generator. Copy their coefficients exactly; the
 * returned support has no assigned source identity. Shared by boundary construction, bridge and
 * bevel services. */
OwnedSurface ruled_surface(const NurbsCurveGeometry &first,
                           const NurbsCurveGeometry &second,
                           std::string *r_error = nullptr);

/** Positive rational quadratic blend between compatible curves, with a common tangent-control
 * curve. Boundary coefficients are copied exactly. When the control curve lies in both source
 * tangent planes, the blend has their tangent planes along regular boundaries. A constant
 * cross weight defines a conic family; circularity/constant radius is not implied. */
std::optional<SurfaceConstruction> conic_curve_blend(const NurbsCurveGeometry &first,
                                                     const NurbsCurveGeometry &control,
                                                     const NurbsCurveGeometry &last,
                                                     double cross_weight,
                                                     double max_error,
                                                     std::string *r_error = nullptr);

struct PlanarCurveProjection {
  OwnedCurve curve;
  double geometry_error_bound = 0;
};

/** Construct the parameter curve of a positive-rational spatial curve on a clamped polynomial
 * bilinear support. Preserve its basis/weights. Bound the full curve/support deviation, including
 * bilinear twist, and snap parameter overshoots only when their mapped geometric error fits
 * max_error. Reject larger errors. No fitting, source mutation or identity allocation. */
std::optional<PlanarCurveProjection> project_curve_to_affine_surface(
    const NurbsCurveGeometry &curve,
    const NurbsSurfaceGeometry &surface,
    double max_error,
    std::string *r_error = nullptr);

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
  /** Conservative support projection/deviation bound, not a fitting/evaluation certificate. */
  double construction_error_bound = 0.0;
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

/** Also permit affine triangle charts within an explicit coefficient construction budget.
 * Retain the original shared 3D boundaries and represent their parameter curves on a bilinear
 * support. Only its fourth coefficient may incur arithmetic roundoff; record the conservative
 * bound in the result and new face/use tolerances. No boundary fitting or tolerance increase
 * beyond the caller's budget. The overload above requires zero construction roundoff.
 */
std::optional<FillResult> fill_components(const NurbsGeometry &source,
                                          const ComponentSelection &selection,
                                          double max_construction_error,
                                          std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
