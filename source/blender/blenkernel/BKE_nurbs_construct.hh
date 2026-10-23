/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

struct EdgeExtrusionResult {
  OwnedGeometry geometry;
  /** Fresh cap identities, including shared curve resources and endpoints. */
  Map<int64_t, int64_t> source_to_cap;
  Map<int64_t, int64_t> vertex_to_rail;
  Map<int64_t, int64_t> edge_to_face;
  /** Conservative coefficient translation roundoff bound, not an evaluation error certificate. */
  double translation_error_bound = 0.0;
  /** Measured input endpoint discrepancy already covered by source vertex/edge tolerance. */
  double max_source_endpoint_deviation = 0.0;
};

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

}  // namespace blender::bke::nurbs
