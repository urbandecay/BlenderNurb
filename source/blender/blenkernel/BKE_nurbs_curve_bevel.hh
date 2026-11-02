/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BKE_nurbs_edit.hh"

namespace blender::bke::nurbs {

struct CurveControlBevelResult {
  EditSnapshot snapshot;
  BevelOffsetInfo offset_info;
};

/** Bevel selected interior control-polygon corners of complete clamped wire supports.
 * Preserve degree, unselected coefficients, end coefficients, topology and parameter domain.
 * Add local interior knots for the additional handles. Rounded profiles shape the control cage;
 * the resulting smooth NURBS curve is defined by that edited cage, not a fillet of its locus.
 * Incident surfaces need a compatible tensor-basis edit and are rejected atomically. */
std::optional<CurveControlBevelResult> bevel_curve_controls(const NurbsGeometry &source,
                                                            const SelectionState &selection,
                                                            const BevelOptions &options,
                                                            std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
