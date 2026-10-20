/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

namespace blender {

struct Mesh;
struct NurbsGeometry;

/** Create a derived display mesh from exact NURBS topology and curves. */
Mesh *BKE_nurbs_mesh_create(const NurbsGeometry &geometry, int resolution);

}  // namespace blender
