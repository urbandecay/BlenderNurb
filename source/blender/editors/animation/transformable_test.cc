/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "BLI_math_base.h"

#include "BKE_idtype.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"

#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "ED_anim_transformable.hh"

#include "CLG_log.h"
#include "testing/testing.h"

namespace blender::ed::tests {

class TransformableTest : public testing::Test {
 public:
  Main *bmain;
  Object *object;

  static void SetUpTestSuite()
  {
    /* BKE_id_free() hits a code path that uses CLOG, which crashes if not initialized properly. */
    CLG_init();
    BKE_idtype_init();
    RNA_init();
  }

  static void TearDownTestSuite()
  {
    CLG_exit();
    RNA_exit();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();

    object = BKE_object_add_only_object(bmain, OB_NO_DATA, "Object");
    object->rotmode = ROT_MODE_QUAT;
    object->quat[0] = 1.0f;
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

TEST_F(TransformableTest, transformable_get_values)
{
  AnimTransformable transformable(*object);
  EXPECT_TRUE(transformable.rna_path().is_empty());

  Array<float> location = transformable.get_property(AnimTransformable::PropertyType::LOCATION);
  Array<float> expected = {0, 0, 0};
  EXPECT_EQ(expected, location);

  object->loc[0] = 1;

  expected = {0, 0, 0};
  /* The returned values are a copy, changing the underlying data does not modify the array. */
  EXPECT_EQ(expected, location);

  location = transformable.get_property(AnimTransformable::PropertyType::LOCATION);
  expected = {1, 0, 0};
  EXPECT_EQ(expected, location);

  Array<float> rotation_values = transformable.get_property(
      AnimTransformable::PropertyType::ROTATION);
  EXPECT_EQ(object->rotmode, ROT_MODE_QUAT);
  EXPECT_EQ(transformable.get_rotation_mode(), object->rotmode);
  EXPECT_EQ(rotation_values.size(), 4);
}

TEST_F(TransformableTest, transformable_rotation)
{
  AnimTransformable transformable(*object);
  Rotation rotation = transformable.get_rotation();
  /* The rotation is always returned in the mode of the Transformable. */
  EXPECT_EQ(rotation.mode, transformable.get_rotation_mode());
  EXPECT_EQ(rotation.mode, ROT_MODE_QUAT);
  Array<float> expected = {1, 0, 0, 0};
  EXPECT_EQ(expected, rotation.values);

  object->rotmode = ROT_MODE_XYZ;
  object->rot[0] = 3.14;
  transformable.set_rotation(rotation);
  /* Even though the rotation is a quaternion, setting it to an object with xyz euler still works.
   * The rotation is converted to the correct mode of the Transformable. */
  expected = {0, 0, 0};
  EXPECT_NEAR_SPAN(expected.as_span(), Span<float>(object->rot, 3), 0.001);
}

TEST_F(TransformableTest, transformable_blend_to)
{
  AnimTransformable transformable(*object);
  transformable.blend_property_to(
      AnimTransformable::PropertyType::LOCATION, {1, 0, 0}, 0.0f, AXIS_MUTABLE_ALL);
  Array<float> expected = {0, 0, 0};
  /* A blend factor of 0 keeps the current values. */
  EXPECT_NEAR_SPAN(expected.as_span(),
                   transformable.get_property(AnimTransformable::PropertyType::LOCATION).as_span(),
                   0.001);

  transformable.blend_property_to(
      AnimTransformable::PropertyType::LOCATION, {1, 0, 0}, 0.1f, AXIS_MUTABLE_ALL);
  expected = {0.1f, 0, 0};
  /* Blending linearly to 1. */
  EXPECT_NEAR_SPAN(expected.as_span(),
                   transformable.get_property(AnimTransformable::PropertyType::LOCATION).as_span(),
                   0.001);

  transformable.blend_property_to(
      AnimTransformable::PropertyType::LOCATION, {1, 0, 0}, 1.0f, AXIS_MUTABLE_ALL);
  expected = {1.0f, 0, 0};
  /* Blending linearly to 1. */
  EXPECT_NEAR_SPAN(expected.as_span(),
                   transformable.get_property(AnimTransformable::PropertyType::LOCATION).as_span(),
                   0.001);
}

TEST_F(TransformableTest, transformable_blend_rotation_to)
{
  AnimTransformable transformable(*object);
  /* There is a special function for rotations that does spherical interpolation for
   * quaternions. */
  EXPECT_EQ(object->rotmode, ROT_MODE_QUAT);
  /* A 90 degree rotation on X. */
  Rotation rot_90_x = {{0.707107f, 0.707107f, 0, 0}, ROT_MODE_QUAT};
  transformable.blend_rotation_to(rot_90_x, 0.5f, AXIS_MUTABLE_ALL);
  Rotation current_rotation = transformable.get_rotation();
  EXPECT_NEAR(current_rotation.values[0], 0.92387f, 0.001);
  EXPECT_NEAR(current_rotation.values[1], 0.38268f, 0.001);

  /* Checking that the result is different from linear interpolation. */
  EXPECT_NE(current_rotation.values[0], interpf(0.707107f, 1.0f, 0.5f));
  EXPECT_NE(current_rotation.values[1], interpf(0.707107f, 0.0f, 0.5f));

  transformable.set_rotation(identity_rotation(ROT_MODE_QUAT));
  /* Using the generic blend function assumes that the given values are in the rotation mode that
   * the object is currently in. As long as that is the case it will work as expected. */
  transformable.blend_property_to(
      AnimTransformable::PropertyType::ROTATION, rot_90_x.values, 0.5f, AXIS_MUTABLE_ALL);
  EXPECT_NEAR(current_rotation.values[0], 0.92387f, 0.001);
  EXPECT_NEAR(current_rotation.values[1], 0.38268f, 0.001);
}

TEST_F(TransformableTest, transformable_axis_constraints)
{
  /* It is possible to only set and blend selected axes. */
  AnimTransformable transformable(*object);

  transformable.set_property(AnimTransformable::PropertyType::LOCATION, {1, 1, 1}, AXIS_MUTABLE_X);
  Array<float> expected = {1, 0, 0};
  EXPECT_NEAR_SPAN(expected.as_span(),
                   transformable.get_property(AnimTransformable::PropertyType::LOCATION).as_span(),
                   0.001);

  transformable.set_property(AnimTransformable::PropertyType::LOCATION,
                             {2, 2, 2},
                             AxisMutable(AXIS_MUTABLE_X | AXIS_MUTABLE_Y));
  expected = {2, 2, 0};
  EXPECT_NEAR_SPAN(expected.as_span(),
                   transformable.get_property(AnimTransformable::PropertyType::LOCATION).as_span(),
                   0.001);

  transformable.blend_property_to(AnimTransformable::PropertyType::LOCATION,
                                  {3, 3, 3},
                                  1.0f,
                                  AxisMutable(AXIS_MUTABLE_Y | AXIS_MUTABLE_Z));
  expected = {2, 3, 3};
  EXPECT_NEAR_SPAN(expected.as_span(),
                   transformable.get_property(AnimTransformable::PropertyType::LOCATION).as_span(),
                   0.001);
}

}  // namespace blender::ed::tests
