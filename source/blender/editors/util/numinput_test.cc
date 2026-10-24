/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"

#include "DNA_scene_types.h"

#include "ED_numinput.hh"

#include "WM_types.hh"

#include "testing/testing.h"

namespace blender::tests {

TEST(numinput, DoubleDefaultsDoNotRoundThroughFloat)
{
  NumInput input;
  initNumInput(&input);
  input.flag |= NUM_USE_DOUBLE;
  input.idx_max = 2;
  double values[3] = {16777217.125, 0.12345678901234567, -0.0};
  EXPECT_FALSE(applyNumInput(&input, values));
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(input.val_org_double[i], values[i]);
  }
  EXPECT_NE(double(input.val_org[0]), values[0]);
  EXPECT_NE(double(input.val_org[1]), values[1]);
}

TEST(numinput, DoubleIncrementSurvivesAxisChangesAndReset)
{
  bContext *context = CTX_create();
  Scene scene{};
  scene.unit.scale_length = 1;
  CTX_data_scene_set(context, &scene);
  NumInput input;
  initNumInput(&input);
  input.flag |= NUM_USE_DOUBLE;
  input.idx_max = 2;
  const double original = 16777217.125;
  double values[3] = {original, 0, 0};
  EXPECT_FALSE(applyNumInput(&input, values));
  wmEvent event{};
  event.type = EVT_MODAL_MAP;
  event.val = NUM_MODAL_INCREMENT_UP;
  ASSERT_TRUE(handleNumInput(context, &input, &event));
  EXPECT_TRUE(applyNumInput(&input, values));
  EXPECT_EQ(values[0], original + 1);
  const std::string expression(input.str);
  event.type = EVT_TABKEY;
  event.val = KM_PRESS;
  ASSERT_TRUE(handleNumInput(context, &input, &event));
  EXPECT_EQ(input.idx, 1);
  EXPECT_EQ(input.str[0], '\0');
  event.type = EVT_MODAL_MAP;
  event.val = NUM_MODAL_INCREMENT_DOWN;
  ASSERT_TRUE(handleNumInput(context, &input, &event));
  EXPECT_TRUE(applyNumInput(&input, values));
  EXPECT_EQ(values[0], original + 1);
  EXPECT_EQ(values[1], -1);
  event.type = EVT_TABKEY;
  event.val = KM_PRESS;
  event.modifier = KM_CTRL;
  ASSERT_TRUE(handleNumInput(context, &input, &event));
  EXPECT_EQ(input.idx, 0);
  EXPECT_EQ(std::string(input.str), expression);
  EXPECT_TRUE(applyNumInput(&input, values));
  EXPECT_EQ(values[0], original + 1);
  event.type = EVT_BACKSPACEKEY;
  event.modifier = KM_SHIFT;
  ASSERT_TRUE(handleNumInput(context, &input, &event));
  EXPECT_TRUE(applyNumInput(&input, values));
  EXPECT_EQ(values[0], original);
  EXPECT_EQ(values[1], -1);
  CTX_free(context);
}

TEST(numinput, FloatDefaultsRetainTheirExistingChannel)
{
  NumInput input;
  initNumInput(&input);
  input.idx_max = 2;
  float values[3] = {1.25f, -2.5f, 0};
  EXPECT_FALSE(applyNumInput(&input, values));
  EXPECT_FALSE(hasNumInput(&input));
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(input.val_org[i], values[i]);
    EXPECT_EQ(input.val[i], values[i]);
  }
}

TEST(numinput, FullExpressionReturnReachesTheModalConfirmHandler)
{
  NumInput input;
  initNumInput(&input);
  input.flag |= NUM_USE_DOUBLE;
  wmEvent event{};
  event.type = EVT_EQUALKEY;
  event.val = KM_PRESS;
  event.utf8_buf[0] = '=';
  ASSERT_TRUE(handleNumInput(nullptr, &input, &event));
  ASSERT_TRUE(hasNumInput(&input));
  event.type = EVT_RETKEY;
  event.utf8_buf[0] = '\n';
  EXPECT_FALSE(handleNumInput(nullptr, &input, &event));
  EXPECT_EQ(input.str[0], '\0');
}

}  // namespace blender::tests
