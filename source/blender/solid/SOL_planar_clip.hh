/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_intersections.hh"

namespace blender::solid {

/** Intersect one bounded planar source face with one convex straight-boundary planar mask.
 * Source trims may be native Line/Conic curves, with holes and nonconvex boundaries.
 * A disconnected intersection returns several faces; disjoint regions return an empty Model.
 * The mask must have one simple convex loop and no holes. Face orientation does not alter
 * the mask's geometric interior. Source curves/supports remain native; inputs stay immutable.
 *
 * support_tolerance explicitly certifies the two planar supports (distance in model units,
 * angle as the sine of their normal angle). Zero requires exact arithmetic agreement.
 * It never joins vertices or projects, fits, or moves geometry. Unsupported or unresolved
 * configurations fail with an error rather than returning a partial trimmed region. */
std::optional<Model> boolean_clip_planar_face(const Model &source,
                                              Id source_face,
                                              const Model &mask,
                                              Id mask_face,
                                              IntersectionTolerance support_tolerance = {0, 0},
                                              std::string *error = nullptr);

}  // namespace blender::solid
