/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "DNA_nurbs_surface_types.h"

namespace blender {

/** Validate the complete tensor-product surface and both reduced knot vectors. */
bool BKE_nurbs_surface_is_valid(const NurbsSurface &surface);

/** Evaluate an exact surface point in object-local XYZ. */
bool BKE_nurbs_surface_evaluate(const NurbsSurface &surface,
                                double u,
                                double v,
                                double r_position[3]);

/** Deep-copy a surface, including its CV net and both knot vectors. */
NurbsSurface *BKE_nurbs_surface_copy(const NurbsSurface &surface);

/** Free a surface created by #BKE_nurbs_surface_copy. */
void BKE_nurbs_surface_free(NurbsSurface *surface);

}  // namespace blender
