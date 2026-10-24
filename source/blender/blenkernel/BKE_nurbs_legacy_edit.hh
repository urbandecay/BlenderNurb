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

/**
 * Prepared compatibility edit for the tagged two-point/two-by-two prototype. Owns new source
 * splines until transferred by the caller. Existing splines and selection are never modified
 * during preparation. IDs and region rules come from the native topology services; geometry
 * remains exact legacy NURBS coefficients, with no fit or evaluated-mesh conversion.
 */
struct LegacyTopologyEdit {
  ListBaseT<Nurb> added = {nullptr, nullptr};
  Vector<int> removed_faces;
  Vector<int> removed_wires;

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
 * Supports faces and edges; isolated point copies and mixed selected untagged geometry fail
 * explicitly. Source attributes and custom knot vectors on complete copied supports are retained.
 */
std::optional<LegacyTopologyEdit> legacy_topology_duplicate(const ListBaseT<Nurb> &source,
                                                            std::string *r_error = nullptr);

/**
 * Prepare a zero-offset region/edge extrusion for Blender's normal move macro. Generate walls
 * only on region boundaries and replace attached input faces using the shared region plan.
 * Selection is normalized by visible source vertex identity before classification. Supports the
 * existing tagged two-point/two-by-two source domain; unsupported selection fails atomically.
 */
std::optional<LegacyTopologyEdit> legacy_topology_extrude(const ListBaseT<Nurb> &source,
                                                          std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
