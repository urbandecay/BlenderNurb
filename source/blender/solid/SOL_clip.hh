/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"

namespace blender::solid {

struct BooleanBodyPlaneSplit {
  Model negative, positive;
};

/** Split a closed native body by an infinite plane. The negative/positive bodies include
 * analytic planar caps, with inner loops for cavities. An empty side is an empty Model.
 * Source geometry stays immutable, including on failure. Multiple closed shells are accepted.
 *
 * Supported surfaces: Plane, affine Cylinder, and conic Extrusion. Trims: Line/Conic.
 * assembly_tolerance is an explicit vertex-joining policy; zero preserves distinct positions.
 * No points are projected, no topology is inferred from tessellation, and unsupported or
 * unresolved configurations fail with an error rather than returning a partial solid. */
std::optional<BooleanBodyPlaneSplit> boolean_split_body_by_plane(const Model &source,
                                                                 const Surface &plane,
                                                                 double assembly_tolerance = 0,
                                                                 std::string *error = nullptr);

}  // namespace blender::solid
