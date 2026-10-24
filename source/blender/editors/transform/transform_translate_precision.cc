/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <charconv>
#include <cmath>
#include <limits>

#include "transform_translate_precision.hh"

namespace blender::ed::transform {

std::string TranslationPrecision::serialize() const
{
  std::string result = "v1";
  auto append = [&](const double value) {
    char buffer[64];
    const auto converted = std::to_chars(buffer,
                                         buffer + sizeof(buffer),
                                         value,
                                         std::chars_format::general,
                                         std::numeric_limits<double>::max_digits10);
    BLI_assert(converted.ec == std::errc());
    result += ' ';
    result.append(buffer, converted.ptr);
  };
  for (int i = 0; i < 3; i++) {
    append(space[i]);
  }
  for (int i = 0; i < 3; i++) {
    append(world[i]);
  }
  for (int column = 0; column < 3; column++) {
    for (int row = 0; row < 3; row++) {
      append(orientation[column][row]);
    }
  }
  return result;
}

std::optional<TranslationPrecision> TranslationPrecision::deserialize(const std::string_view text)
{
  if (!text.starts_with("v1 ")) {
    return std::nullopt;
  }
  const char *cursor = text.data() + 3, *end = text.data() + text.size();
  auto read = [&](double &value) {
    const auto parsed = std::from_chars(cursor, end, value);
    if (parsed.ec != std::errc() || !std::isfinite(value)) {
      return false;
    }
    cursor = parsed.ptr;
    if (cursor != end && *cursor++ != ' ') {
      return false;
    }
    return true;
  };
  TranslationPrecision result;
  for (int i = 0; i < 3; i++) {
    if (!read(result.space[i]) || std::abs(result.space[i]) > std::numeric_limits<float>::max()) {
      return std::nullopt;
    }
  }
  for (int i = 0; i < 3; i++) {
    if (!read(result.world[i])) {
      return std::nullopt;
    }
  }
  for (int column = 0; column < 3; column++) {
    for (int row = 0; row < 3; row++) {
      double value;
      if (!read(value) || double(float(value)) != value) {
        return std::nullopt;
      }
      result.orientation[column][row] = float(value);
    }
  }
  if (cursor != end) {
    return std::nullopt;
  }
  return result;
}

TranslationPrecision TranslationPrecision::replay(const float3 &displayed,
                                                  const float3x3 &new_orientation) const
{
  TranslationPrecision result = *this;
  bool unchanged = orientation == new_orientation;
  for (int i = 0; i < 3; i++) {
    if (displayed[i] != float(space[i])) {
      result.space[i] = displayed[i];
      unchanged = false;
    }
  }
  result.orientation = new_orientation;
  if (!unchanged) {
    result.world = double3x3(new_orientation) * result.space;
  }
  return result;
}

}  // namespace blender::ed::transform
