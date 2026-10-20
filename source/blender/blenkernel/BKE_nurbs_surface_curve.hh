/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "DNA_nurbs_surface_types.h"

namespace blender {

/** Extract an exact isoparametric curve from a NURBS surface.
 *
 * When \a varying_u is true, \a fixed_parameter is a V parameter and the output varies in U.
 * Otherwise it is a U parameter and the output varies in V. The output must be an empty curve
 * distinct from the surface storage. On failure it is left unchanged. */
bool BKE_nurbs_surface_isocurve(const NurbsSurface &surface,
                                bool varying_u,
                                double fixed_parameter,
                                NurbsCurve &r_curve);

}  // namespace blender
