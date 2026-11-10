/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"

#include <span>

namespace blender::solid {

/** Subtract contained, interior-disjoint planar regions from one bounded face.
 * Each region model contains exactly one planar face, with optional holes.
 * All trim curves must be native Line/Conic, and every boundary intersection
 * must already occur among the supplied vertices. Existing spans are split
 * jointly at those vertices; no new curve intersections are constructed.
 * Analytic interval bounds certify the resulting arrangement and reject
 * unmarked crossings, ambiguous tangencies, or unresolved near contacts.
 *
 * Tolerance is an explicit vertex sewing/support-contact policy. It never
 * changes analytic supports. Distinct conic parameterizations of a coincident
 * trim may require prior canonicalization. All supports must certify the same
 * plane. The result contains only the remaining open native face pieces,
 * or an empty Model for complete coverage. Sources remain unchanged. */
std::optional<Model> boolean_subtract_planar_regions(const Model &source,
                                                    Id face,
                                                    std::span<const Model> regions,
                                                    double tolerance = 0,
                                                    std::string *error = nullptr);

}  // namespace blender::solid
