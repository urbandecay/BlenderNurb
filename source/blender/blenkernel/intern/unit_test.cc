/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstring>

#include "BLI_expr_pylike_eval.h"
#include "BLI_memory_utils.hh"
#include "BLI_string.h"

#include "DNA_scene_types.h"

#include "BKE_unit.hh"

#include "testing/testing.h"

namespace blender::tests {

static double converted_expression(const char *input,
                                   const int type,
                                   const UnitConversionPrecision precision)
{
  char expression[1024];
  STRNCPY(expression, input);
  EXPECT_TRUE(BKE_unit_replace_string(
      expression, sizeof(expression), input, 1, USER_UNIT_METRIC, type, precision));
  ExprPyLike_Parsed *parsed = BLI_expr_pylike_parse(expression, nullptr, 0);
  BLI_SCOPED_DEFER([&]() { BLI_expr_pylike_free(parsed); });
  EXPECT_TRUE(BLI_expr_pylike_is_valid(parsed)) << expression;
  double result = 0;
  EXPECT_EQ(BLI_expr_pylike_eval(parsed, nullptr, 0, &result), EXPR_PYLIKE_SUCCESS);
  return result;
}

TEST(unit_precision, DoubleInputUsesDeclaredLengthScales)
{
  constexpr double value = 12.345678901234567;
  const char *expressions[] = {"12.345678901234567cm",
                               "12.345678901234567mm",
                               "12.345678901234567ft",
                               "12.345678901234567in"};
  const double factors[] = {0.01, 0.001, 0.3048, 0.0254};
  for (int i = 0; i < 4; i++) {
    SCOPED_TRACE(expressions[i]);
    const double precise = converted_expression(
        expressions[i], B_UNIT_LENGTH, UnitConversionPrecision::Double);
    EXPECT_EQ(precise, value * factors[i]);
    EXPECT_NE(
        precise,
        converted_expression(expressions[i], B_UNIT_LENGTH, UnitConversionPrecision::Legacy));
  }
}

TEST(unit_precision, BaseAndPreferredUnitsRequireAnExplicitPrecisionChoice)
{
  EXPECT_EQ(BKE_unit_base_scalar(USER_UNIT_IMPERIAL, B_UNIT_LENGTH), double(0.3048f));
  EXPECT_EQ(
      BKE_unit_base_scalar(USER_UNIT_IMPERIAL, B_UNIT_LENGTH, UnitConversionPrecision::Double),
      0.3048);
  UnitSettings settings{};
  settings.system = USER_UNIT_IMPERIAL;
  settings.length_unit = USER_UNIT_ADAPTIVE;
  EXPECT_EQ(BKE_unit_apply_preferred_unit(settings, B_UNIT_LENGTH, 1), double(0.3048f));
  EXPECT_EQ(
      BKE_unit_apply_preferred_unit(settings, B_UNIT_LENGTH, 1, UnitConversionPrecision::Double),
      0.3048);
  char first[256] = "12.345678901234567cm", second[256];
  STRNCPY(second, first);
  EXPECT_EQ(BKE_unit_replace_string(first, sizeof(first), "", 1, USER_UNIT_METRIC, B_UNIT_LENGTH),
            BKE_unit_replace_string(second,
                                    sizeof(second),
                                    "",
                                    1,
                                    USER_UNIT_METRIC,
                                    B_UNIT_LENGTH,
                                    UnitConversionPrecision::Legacy));
  EXPECT_STREQ(first, second);
}

TEST(unit_precision, AreaAndVolumeKeepLegacyCatalogueProducts)
{
  EXPECT_EQ(converted_expression("1cm2", B_UNIT_AREA, UnitConversionPrecision::Double),
            0.01 * 0.01);
  EXPECT_EQ(converted_expression("1cm3", B_UNIT_VOLUME, UnitConversionPrecision::Double),
            0.01 * 0.01 * 0.01);
  for (const int type : {B_UNIT_AREA, B_UNIT_VOLUME}) {
    const void *collection;
    int length;
    BKE_unit_system_get(USER_UNIT_METRIC, type, &collection, &length);
    bool found = false;
    for (int i = 0; i < length; i++) {
      if (STREQ(BKE_unit_name_get(collection, i),
                type == B_UNIT_AREA ? "square centimeter" : "cubic centimeter"))
      {
        const float area = 0.01f * 0.01f;
        const float legacy = type == B_UNIT_AREA ? area : area * 0.01f;
        EXPECT_EQ(BKE_unit_scalar_get(collection, i), double(legacy));
        found = true;
      }
    }
    EXPECT_TRUE(found);
  }
}

}  // namespace blender::tests
