/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>

#include "transform_translate_precision.hh"

#include "testing/testing.h"

namespace blender::ed::transform::tests {

TEST(translation_precision, UnchangedReplayRetainsDeltaWithoutAnInverseRoundTrip)
{
  const TranslationPrecision source{{0.12345678901234567, 16777217.125, -0.0},
                                    {0.12345678901234568, 16777217.125, -0.0},
                                    float3x3::identity()};
  const auto stored = TranslationPrecision::deserialize(source.serialize());
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->space, source.space);
  EXPECT_EQ(stored->world, source.world);
  const auto replay = stored->replay(float3(source.space), source.orientation);
  EXPECT_EQ(replay.world, source.world);
  EXPECT_EQ(replay.space, source.space);
}

TEST(translation_precision, EditingOneRedoComponentRetainsOtherPreciseComponents)
{
  const TranslationPrecision source{{0.12345678901234567, 16777217.125, 3},
                                    {0.12345678901234567, 16777217.125, 3},
                                    float3x3::identity()};
  float3 displayed(source.space);
  displayed.z = 4;
  const auto replay = source.replay(displayed, source.orientation);
  EXPECT_EQ(replay.space.x, source.space.x);
  EXPECT_EQ(replay.space.y, source.space.y);
  EXPECT_EQ(replay.world.z, 4);
  EXPECT_NE(replay.space.y, double(displayed.y));
}

TEST(translation_precision, OrientationChangeUsesPreciseSpaceValue)
{
  const TranslationPrecision source{{0.12345678901234567, 16777217.125, 3},
                                    {0.12345678901234567, 16777217.125, 3},
                                    float3x3::identity()};
  float3x3 orientation = float3x3::identity();
  orientation[0] = float3(0, 1, 0);
  orientation[1] = float3(-1, 0, 0);
  const auto replay = source.replay(float3(source.space), orientation);
  EXPECT_EQ(replay.world, double3(-source.space.y, source.space.x, source.space.z));
}

TEST(translation_precision, InvalidOrNonFiniteReplayIsRejected)
{
  const TranslationPrecision source{double3(1), double3(1), float3x3::identity()};
  const std::string stored = source.serialize();
  EXPECT_FALSE(TranslationPrecision::deserialize(""));
  EXPECT_FALSE(TranslationPrecision::deserialize("v2"));
  EXPECT_FALSE(TranslationPrecision::deserialize(stored + " extra"));
  EXPECT_FALSE(TranslationPrecision::deserialize("v1 nan " + stored.substr(3)));
  EXPECT_FALSE(TranslationPrecision::deserialize("v1 1e309 " + stored.substr(3)));
  EXPECT_FALSE(TranslationPrecision::deserialize(stored.substr(0, stored.size() - 2)));
}

}  // namespace blender::ed::transform::tests
