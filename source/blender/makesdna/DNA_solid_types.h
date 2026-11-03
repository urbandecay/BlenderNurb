/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_sys_types.h"

namespace blender {

/** Versioned analytic-kernel source. The Mesh that owns this record is a display adapter;
 * its polygon coordinates never replace this source. Runtime keeps immutable decoded states
 * and is explicitly excluded from file writing. The byte stream defines its own endianness. */
struct SolidStorage {
  int64_t bytes_num = 0;
  uint64_t revision = 1;
  uint8_t *bytes = nullptr;
  void *runtime = nullptr;
};

}  // namespace blender
