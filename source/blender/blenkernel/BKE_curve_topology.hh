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
 * Currently supports two-point NURBS curves and bilinear NURBS patches created by the topology
 * extrusion tools.
 */
void BKE_curve_nurbs_topology_selection_sync(ListBaseT<Nurb> *nurbs);

/** Apply the active control point's selection state to its matching topology vertex copies. */
void BKE_curve_nurbs_topology_selection_set(ListBaseT<Nurb> *nurbs,
                                           const Nurb &source_nurb,
                                           const BPoint &source_point);

/**
 * Copy the transformed position and weight of selected topology vertices to all matching NURBS
 * patch corners.
 */
void BKE_curve_nurbs_topology_position_sync(ListBaseT<Nurb> *nurbs);

}  // namespace blender
