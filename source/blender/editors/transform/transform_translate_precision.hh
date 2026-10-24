/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "BLI_math_matrix_types.hh"

namespace blender::ed::transform {

inline constexpr const char *TRANSLATION_PRECISE_PROPERTIES[3] = {
    "value_precise_x", "value_precise_y", "value_precise_z"};

/**
 * Retain a double translation across the float operator value/redo boundary. Source coordinates
 * never enter this record. The displayed value is float(space); changing one displayed component
 * replaces that component only. An unchanged orientation/value replays the original world delta
 * without an inverse-matrix round-trip. An orientation change recomputes from the precise space
 * value. Matrices originate in Blender's float orientation system; this does not add precision to
 * their construction or to mouse/snapping input.
 */
struct TranslationPrecision {
  double3 space;
  double3 world;
  float3x3 orientation;

  std::string serialize() const;
  static std::optional<TranslationPrecision> deserialize(std::string_view text);
  TranslationPrecision replay(const float3 &displayed, const float3x3 &new_orientation) const;
};

}  // namespace blender::ed::transform
