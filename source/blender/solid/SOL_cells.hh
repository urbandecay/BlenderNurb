/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"

#include <span>

namespace blender::solid {

/** Assemble the boundary of already jointly partitioned, closed native cells.
 * Full common faces cancel only when their native edge cycles (including holes)
 * agree and their physical boundary senses are opposite. Each input must be
 * closed; intersections and partial face overlaps must already be partitioned.
 * This is boundary reconstruction, not a general overlapping-solid Boolean.
 *
 * Tolerance is the explicit vertex sewing policy; zero requires identical
 * positions. Analytic supports and native curve trims are retained. Different
 * nonplanar parameterizations of a common support are conservatively rejected.
 * Coplanar exterior faces remain separate. Empty input produces an empty model.
 * The sources remain unchanged, including on failure. */
std::optional<Model> boolean_assemble_cells(std::span<const Model> cells,
                                          double tolerance = 0,
                                          std::string *error = nullptr);

}  // namespace blender::solid
