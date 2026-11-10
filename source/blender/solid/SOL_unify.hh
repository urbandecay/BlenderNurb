/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"

namespace blender::solid {

/** Conservative arithmetic certificate; false includes unresolved equivalence. */
bool boolean_same_planar_support(const Surface &first, const Surface &second);

/** Unify adjacent coplanar faces of an already closed native boundary.
 * Opposite internal coedges cancel, and the remaining Line/Conic boundaries
 * rebuild outer loops and holes. Native supports and parameterizations remain
 * analytic; no tessellation, fitting, positional snapping, or angular snapping
 * is used. Exactly collinear redundant boundary edges may also be joined.
 *
 * Only faces with certified identical planes are joined. Nonplanar faces are
 * retained. Unsupported merged trims and ambiguous boundary junctions fail
 * explicitly. This does not intersect overlapping faces or partition edges.
 * The source remains unchanged, including on failure. */
std::optional<Model> boolean_unify_coplanar_faces(const Model &source,
                                                  std::string *error = nullptr);

}  // namespace blender::solid
