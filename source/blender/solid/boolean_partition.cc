/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_clip.hh"
#include "SOL_partition.hh"

#include <cmath>

namespace blender::solid {
std::optional<std::vector<BooleanCell>> boolean_partition_body(
    const Model &source,
    const std::span<const Surface> planes,
    const double assembly_tolerance,
    const size_t max_cells,
    std::string *error)
{
  auto fail = [&](const std::string &message) -> std::optional<std::vector<BooleanCell>> {
    if (error)
      *error = message;
    return {};
  };
  if (!max_cells || !std::isfinite(assembly_tolerance) || assembly_tolerance < 0)
    return fail("Native cell partition needs a positive cell budget and valid joining tolerance");
  std::string message;
  if (!validate(source, &message))
    return fail(message);
  if (source.faces.empty())
    return fail("Native cell partition needs a closed body");
  for (const Face &f : source.faces)
    if (!validate_body(source, f.id, &message))
      return fail(message);
  std::vector<BooleanCell> cells{{source, {}}};
  for (const Surface &plane : planes) {
    std::vector<BooleanCell> next;
    for (const BooleanCell &cell : cells) {
      auto split = boolean_split_body_by_plane(cell.model, plane, assembly_tolerance, &message);
      if (!split)
        return fail(message);
      auto append = [&](Model model, int side) {
        if (model.faces.empty())
          return;
        auto sides = cell.sides;
        sides.push_back(side);
        next.push_back({std::move(model), std::move(sides)});
      };
      append(std::move(split->negative), -1);
      append(std::move(split->positive), 1);
      if (next.size() > max_cells)
        return fail("Native cell partition exceeded its explicit cell budget");
    }
    cells = std::move(next);
  }
  return cells;
}
}  // namespace blender::solid
