/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_boolean.hh"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace blender::solid {

/** Immutable inputs needed to replay a native Boolean request. This diagnostic format is
 * independent of the analytic source schema and is not part of the .blend file format. */
struct BooleanReplay {
  Model source;
  std::vector<Id> first, second;
  BooleanOperation operation = BooleanOperation::Union;
};

/** Versioned, checksummed, bounded serialization for a Boolean replay request. */
std::vector<uint8_t> encode_boolean_replay(const BooleanReplay &replay,
                                           std::string *error = nullptr);
std::optional<BooleanReplay> decode_boolean_replay(const uint8_t *data,
                                                   size_t size,
                                                   std::string *error = nullptr);

}  // namespace blender::solid
