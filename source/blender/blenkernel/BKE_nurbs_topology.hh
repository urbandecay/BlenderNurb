/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_set.hh"

#include "BKE_nurbs_geometry.hh"

namespace blender::bke::nurbs {

enum class DeleteMode { Vertices, Edges, Faces, FacesOnly };

struct TopologyDeletionPlan {
  Set<int64_t> vertices;
  Set<int64_t> edges;
  Set<int64_t> faces;
};

/**
 * Plan component deletion using shared source incidence. Vertex deletion removes incident edges
 * and faces; edge deletion removes incident faces and newly loose edge endpoints. Face deletion
 * removes dependencies unused by surviving faces/edges; FacesOnly retains its boundary geometry.
 * Unrelated loose components survive. No support shrinking, fitting, mutation or ID allocation.
 * Selection order and repeated IDs have no effect. These are Blender/BMesh topology semantics,
 * independent of a NURBS source owner or a polygon/display representation.
 */
std::optional<TopologyDeletionPlan> plan_component_deletion(const NurbsGeometry &source,
                                                            const ComponentSelection &selection,
                                                            DeleteMode mode,
                                                            std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
