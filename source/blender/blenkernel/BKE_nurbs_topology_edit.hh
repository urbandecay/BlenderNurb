/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <cstdint>
#include <string>

#include "BLI_math_vector_types.hh"

namespace blender {

struct NurbsGeometry;

/** Translate one edge support that is used as a complete clamped surface boundary on every
 * incident face, then reconcile the exact surface isocurves of the affected faces' edges. The
 * target boundary trims must be linear UV curves. Other affected boundary uses, including
 * isoparametric sub-intervals around holes, must also have linear UV trims. The operation stages a
 * complete geometry copy and leaves the input unchanged on failure. */
bool BKE_nurbs_topology_translate_boundary(NurbsGeometry &geometry,
                                           uint64_t support_curve_id,
                                           const double3 &delta,
                                           std::string *r_error = nullptr);

}  // namespace blender
