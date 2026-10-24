/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "DNA_curve_types.h"

namespace blender {

/**
 * Keep the selection of topology vertices consistent across NURBS patches that share an ID.
 * Currently supports loose points, two-point NURBS curves and bilinear patches created by topology
 * extrusion tools.
 */
void BKE_curve_nurbs_topology_selection_sync(ListBaseT<Nurb> *nurbs);

/** Apply the active control point's selection state to its matching topology vertex copies. */
void BKE_curve_nurbs_topology_selection_set(ListBaseT<Nurb> *nurbs,
                                            const Nurb &source_nurb,
                                            const BPoint &source_point);

/**
 * Copy the transformed position of selected topology vertices to all matching NURBS patch
 * corners. Preserve each support's rational weights.
 */
void BKE_curve_nurbs_topology_position_sync(ListBaseT<Nurb> *nurbs);

}  // namespace blender
