// Adapted from BOPDS_PaveBlock::Update in Open CASCADE Technology.
// Created by: Peter KURNEV
// Copyright (c) 1999-2014 OPEN CASCADE SAS
// Modifications Copyright (c) 2026 Blender Authors
//
// This library is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published
// by the Free Software Foundation, with the Open CASCADE exception.
// See LICENSE_LGPL_21.txt and OCCT_LGPL_EXCEPTION.txt in this directory.
// SPDX-License-Identifier: LGPL-2.1-only WITH OCCT-exception-1.0

#include "boolean_pave.hh"

#include <algorithm>
#include <cmath>

namespace blender::solid {

std::optional<std::vector<BooleanPaveBlock>> boolean_pave_blocks(
    const Id original_edge,
    const BooleanPave first,
    const BooleanPave last,
    const std::span<const BooleanPave> intersections,
    std::string *error)
{
  auto fail = [&](const char *message) -> std::optional<std::vector<BooleanPaveBlock>> {
    if (error) {
      *error = message;
    }
    return {};
  };
  if (!original_edge || !first.vertex || !last.vertex ||
      !std::isfinite(first.parameter) || !std::isfinite(last.parameter) ||
      first.parameter == last.parameter)
  {
    return fail("Boolean edge partition requires valid endpoints and a nonempty range");
  }
  const double low = std::min(first.parameter, last.parameter);
  const double high = std::max(first.parameter, last.parameter);
  // OCCT's Update collects endpoints and extra paves, sorts them by parameter,
  // then emits adjacent pairs retaining OriginalEdge. Solid owns the vertices
  // and curves, so no OCCT handles, shape pool or allocator are needed here.
  std::vector<BooleanPave> paves{first, last};
  for (const BooleanPave pave : intersections) {
    if (!pave.vertex || !std::isfinite(pave.parameter) || pave.parameter < low ||
        pave.parameter > high)
    {
      return fail("Boolean intersection parameter lies outside its source edge");
    }
    paves.push_back(pave);
  }
  std::sort(paves.begin(), paves.end(), [](const BooleanPave a, const BooleanPave b) {
    return a.parameter < b.parameter;
  });
  std::vector<BooleanPaveBlock> blocks;
  BooleanPave previous = paves.front();
  for (size_t i = 1; i < paves.size(); ++i) {
    const BooleanPave current = paves[i];
    if (current.parameter == previous.parameter) {
      if (current.vertex != previous.vertex) {
        return fail("Coincident Boolean intersection vertices must be reconciled before splitting");
      }
      continue;
    }
    blocks.push_back({original_edge, previous, current});
    previous = current;
  }
  if (first.parameter > last.parameter) {
    std::reverse(blocks.begin(), blocks.end());
    for (BooleanPaveBlock &block : blocks) {
      std::swap(block.first, block.last);
    }
  }
  return blocks;
}

}  // namespace blender::solid
