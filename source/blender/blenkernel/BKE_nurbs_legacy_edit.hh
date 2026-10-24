/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <optional>
#include <string>

#include "BLI_vector.hh"

#include "DNA_curve_types.h"

namespace blender::bke::nurbs {

enum class DeleteMode;

/**
 * Prepared compatibility edit for tagged loose points, two-point lines and two-by-two patches.
 * Owns new source
 * splines until transferred by the caller. Existing splines and selection are never modified
 * during preparation. IDs and region rules come from the native topology services; geometry
 * remains exact legacy NURBS coefficients, with no fit or evaluated-mesh conversion.
 */
struct LegacyTopologyEdit {
  ListBaseT<Nurb> added = {nullptr, nullptr};
  Vector<int> removed_faces;
  Vector<int> removed_wires;
  /** Standalone point source records removed or absorbed by a new edge. */
  Vector<int> removed_points;
  /** Redundant malformed patch aliases recovered through their existing declared edge IDs. */
  int recovered_patch_remnants = 0;

  LegacyTopologyEdit() = default;
  LegacyTopologyEdit(const LegacyTopologyEdit &) = delete;
  LegacyTopologyEdit &operator=(const LegacyTopologyEdit &) = delete;
  LegacyTopologyEdit(LegacyTopologyEdit &&other) noexcept;
  ~LegacyTopologyEdit();
};

/** Whether any visible selected point belongs to a topology-tagged source spline. */
bool legacy_topology_selection_check(const ListBaseT<Nurb> &source);

/**
 * Duplicate the selected component closure with fresh IDs, preserving sharing inside the copy.
 * Supports faces, edges and loose point copies. Mixed selected untagged geometry fails explicitly.
 * Source attributes and custom knot vectors on complete copied supports are retained.
 */
std::optional<LegacyTopologyEdit> legacy_topology_duplicate(const ListBaseT<Nurb> &source,
                                                            std::string *r_error = nullptr);

/**
 * Prepare a zero-offset region/edge extrusion for Blender's normal move macro. Generate walls
 * only on region boundaries and replace attached input faces using the shared region plan.
 * Selection is normalized by visible source vertex identity before classification. Supports the
 * existing tagged point/line/patch source domain; unsupported selection fails atomically.
 */
std::optional<LegacyTopologyEdit> legacy_topology_extrude(const ListBaseT<Nurb> &source,
                                                          std::string *r_error = nullptr);

/** Delete topology components and preserve surviving faces, wires and loose source points. */
std::optional<LegacyTopologyEdit> legacy_topology_delete(const ListBaseT<Nurb> &source,
                                                         DeleteMode mode,
                                                         std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
