/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "BKE_gtest_base.hh"

#include "intern/builder/deg_builder_rna.h"

#include "testing/testing.h"

namespace blender::deg::tests {

class TestableRNANodeQuery : public RNANodeQuery {
 public:
  static bool contains(const char *prop_identifier, const char *rna_path_component)
  {
    return RNANodeQuery::contains(prop_identifier, rna_path_component);
  }
};

class DegBuilderRNATest : public bke::BlenderGTestBase {};

TEST_F(DegBuilderRNATest, contains)
{
  EXPECT_TRUE(TestableRNANodeQuery::contains("location", "location"));
  EXPECT_TRUE(TestableRNANodeQuery::contains("location.x", "location"));
  EXPECT_TRUE(TestableRNANodeQuery::contains("my_collection[\"blork\"].location", "location"));
  EXPECT_TRUE(TestableRNANodeQuery::contains("my_collection[\"blork\"].location.x", "location"));
  EXPECT_TRUE(TestableRNANodeQuery::contains("my_collection[\"blork\"].location[0]", "location"));

  EXPECT_FALSE(TestableRNANodeQuery::contains("", "location"));
  EXPECT_FALSE(TestableRNANodeQuery::contains("locatio", "location"));
  EXPECT_FALSE(TestableRNANodeQuery::contains("locationnn", "location"));
  EXPECT_FALSE(TestableRNANodeQuery::contains("test_location", "location"));
  EXPECT_FALSE(TestableRNANodeQuery::contains("location_test", "location"));
  EXPECT_FALSE(TestableRNANodeQuery::contains("test_location_test", "location"));
  EXPECT_FALSE(TestableRNANodeQuery::contains("my_collection[\"location\"].scale", "location"));
  EXPECT_FALSE(TestableRNANodeQuery::contains("my_collection[\"location\"].scale[0]", "location"));
}

}  // namespace blender::deg::tests
