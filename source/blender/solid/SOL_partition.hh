/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "SOL_boolean.hh"
#include <span>

namespace blender::solid {
struct BooleanCell {
  Model model;
  /** -1/+1 relative to each supplied plane, in input order. */
  std::vector<int> sides;
};

/** Jointly partition a closed body by all supplied planes. Later cuts refine
 * both sides of earlier cuts so shared interfaces have matching partitions.
 * Limits bound combinatorial growth explicitly; failure returns no partial cells. */
std::optional<std::vector<BooleanCell>> boolean_partition_body(const Model &source,
                                                               std::span<const Surface> planes,
                                                               double assembly_tolerance = 0,
                                                               size_t max_cells = 4096,
                                                               std::string *error = nullptr);
/** Plane-partition Boolean path with a convex closed planar operand.
 * Target may have supported curved surfaces and prior trims. Shared planar
 * interface regions reconstruct union and reversed subtraction boundaries. */
std::optional<Model> boolean_with_convex_planar_cutter(const Model &source,
                                                       std::span<const Id> target_faces,
                                                       std::span<const Id> cutter_faces,
                                                       BooleanOperation operation,
                                                       std::string *error = nullptr);
std::optional<Model> boolean_planar_minus_body(const Model &source,
                                               std::span<const Id> planar_faces,
                                               std::span<const Id> body_faces,
                                               std::string *error = nullptr);
}  // namespace blender::solid
