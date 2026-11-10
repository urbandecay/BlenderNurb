/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"

namespace blender::solid {

/** Remove artificial partitions between connected Cylinder faces with exactly
 * identical affine frames and orientations in an already closed native model.
 * A simple contractible boundary becomes one face. A full-wrap band bounded by
 * two opposite, complete conic cycles retains one existing cancelled generator
 * chain as its genuine periodic seam, used in both directions in the outer wire.
 * No geometry is sampled, fitted, snapped or invented.
 *
 * Components with holes, ambiguous boundary cycles, incompatible orientations,
 * or no certified generator seam are conservatively retained. Different frames
 * are not recognized as equivalent. Seam-chain joins require exactly identical
 * Line supports and contiguous native parameters; each complete rim must use
 * one Conic support with a contiguous parameter chain at its seam endpoint.
 * This reconstructs already partitioned cells;
 * it does not compute overlapping-face intersections.
 *
 * The final coplanar unifier also joins safe global valence-two Line/Conic edges,
 * allowing an intact original cylinder to recover its compact four-use wall.
 * That existing sewing utility remaps IDs; native supports and parameter ranges
 * are preserved or analytically joined. Source is immutable, including on failure. */
std::optional<Model> boolean_unify_cylindrical_faces(const Model &source,
                                                    std::string *error = nullptr);

}  // namespace blender::solid
