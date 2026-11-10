/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"

#include <span>

namespace blender::solid {

/** An intersection vertex at a parameter on the original, unmodified curve.
 * One vertex can occur at different parameters (notably a periodic seam). */
struct BooleanPave {
  Id vertex = 0;
  double parameter = 0;
};

struct BooleanPaveBlock {
  Id original_edge = 0;
  BooleanPave first, last;
};

/** Partition an original edge at known intersection vertices. Shared vertices
 * must already have been reconciled by the intersection stage. Preserve native
 * parameter ranges, including descending ranges and periodic seam occurrences.
 * Error is transactional: no partial collection of intervals is returned. */
std::optional<std::vector<BooleanPaveBlock>> boolean_pave_blocks(
    Id original_edge,
    BooleanPave first,
    BooleanPave last,
    std::span<const BooleanPave> intersections,
    std::string *error = nullptr);

}  // namespace blender::solid
