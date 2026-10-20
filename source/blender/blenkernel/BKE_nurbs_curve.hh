/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "DNA_nurbs_types.h"

namespace blender {

/** Split an exact NURBS curve at an interior parameter using homogeneous knot insertion.
 * The outputs must be distinct, empty records. On failure they are left unchanged. */
bool BKE_nurbs_curve_split(const NurbsCurve &source,
                           double parameter,
                           NurbsCurve &r_left,
                           NurbsCurve &r_right);

}  // namespace blender
