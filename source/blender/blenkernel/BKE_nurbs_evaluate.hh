/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

struct BoundarySample {
  double parameter;
  int point_index;
  /** Source curve evaluation, before using the shared topology vertex at an endpoint. */
  double3 curve_position;
};

struct EvaluatedBoundaryEdge {
  int64_t edge_id;
  /** Increasing canonical edge parameter, independently of any face's traversal. */
  Vector<BoundarySample> samples;
  double max_endpoint_deviation = 0.0;
  double max_weight_condition = 1.0;
};

struct EvaluatedBoundaryUse {
  int64_t face_id;
  int64_t loop_id;
  int64_t use_id;
  int edge_index;
  bool reversed;
  bool is_inner;
  /** Exact coefficient proof for a straight curve on a bilinear support, independent of sampled
   * floating-point evaluations. Measured discrepancies below remain reported, including when
   * this proof establishes identity at zero modeling tolerance. */
  bool exact_curve_support_identity = false;
  /** Sufficient bound from source polynomial coefficients over the whole edge. Includes the
   * diagonal of a bilinear patch, whose gap to its chord is bounded by its mixed coefficient.
   * General straight trims use a quadratic Bernstein difference with bounded arithmetic.
   * No sampled evaluation error or tolerance widening. Zero also proves exact identity. */
  std::optional<double> curve_support_deviation_bound;
  /** One UV per canonical edge sample, not reversed into loop traversal order. */
  Vector<double2> parameters;
  double max_curve_support_deviation = 0.0;
  double max_display_support_deviation = 0.0;
  double max_weight_condition = 1.0;
};

struct BoundaryEvaluation {
  /** Shared by source vertex identity and canonical edge sample, never by position equality. */
  Vector<double3> positions;
  /** Parallel to positions; zero for edge-interior samples. Includes isolated source vertices. */
  Vector<int64_t> vertex_ids;
  Vector<EvaluatedBoundaryEdge> edges;
  /** Source face/loop/use order, including holes and repeated seam uses. */
  Vector<EvaluatedBoundaryUse> uses;
};

struct BoundaryEvaluationOptions {
  /** Uniform subdivision of the union of an edge's 3D curve and incident p-curve knot spans. */
  int segments_per_span = 8;
  /** Limit output entries: source vertices + canonical edge samples + face-use UV samples. */
  int max_sample_entries = 1 << 20;
  /**
   * Explicit allowance added to declared source tolerances for sampled agreement checks.
   * Not an evaluation error bound, fitting tolerance or approximation certificate.
   */
  double evaluation_allowance = 0.0;
};

/** Sufficient whole-interval bound for a full straight polynomial curve and its linear p-curve
 * on a polynomial bilinear support. Zero proves exact geometric agreement independently of
 * rounded samples. A missing bound means the representation/proof is unsupported, not that the
 * geometry disagrees. Requires structurally validated geometry records. No mutation, fitting or
 * tolerance inference. Requires round-to-nearest/subnormal arithmetic. The sufficient bound can
 * be conservative; a nonzero bound is not a measurement or proof of a nonzero gap. Shared by
 * boundary evaluation and topology geometry reconciliation. */
std::optional<double> linear_curve_support_deviation_bound(const NurbsCurveGeometry &curve,
                                                           const NurbsCurveGeometry &pcurve,
                                                           const NurbsSurfaceGeometry &surface);

/**
 * Evaluate shared boundaries for a derived preview. Source vertices supply shared endpoint
 * positions; every incident use refers to the same edge samples through its own p-curve.
 * Measured curve/vertex/support discrepancies must fit declared tolerances plus the explicit
 * allowance and are reported in the result. A straight-curve/support coefficient bound
 * can establish agreement independently of rounded samples; its bound and the measured sample
 * discrepancies are retained. Endpoint/vertex checks remain required. No source mutation,
 * fitting or coordinate welding.
 *
 * Initial implementation requires positive weights and continuous curve/support parameterizations.
 * Rejects unsupported arithmetic, unrepresentable subdivisions and failed sampled agreement.
 * This is uniform preview sampling, not trim validation, picking, a regularity proof or a
 * geometric approximation bound. Does not triangulate faces or test UV loop closure/intersections.
 * The caller must keep the source alive and immutable during evaluation (e.g. a ReadSnapshot),
 * and retain its state token separately when using these source IDs for picking or later edits.
 * Results own their values and contain no borrowed source pointers.
 */
std::optional<BoundaryEvaluation> evaluate_boundaries(const NurbsGeometry &source,
                                                      const BoundaryEvaluationOptions &options,
                                                      std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
