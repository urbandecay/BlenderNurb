/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "keyframes_general_intern.hh"

#include "BLI_string.h"

#include "BKE_fcurve.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"

#include "DNA_object_types.h"

#include "ED_keyframes_edit.hh"

#include <memory>

namespace blender {

using namespace blender::animrig;

namespace ed::animation::tests {

namespace {

/* std::unique_ptr for FCurve. */
struct fcurve_deleter {
  void operator()(FCurve *fcurve) const
  {
    BKE_fcurve_free(fcurve);
  }
};
using FCurvePtr = std::unique_ptr<FCurve, fcurve_deleter>;

/**
 * Create a "fake" F-Curve. It does not belong to any Action, and has no keys.
 * It just has its RNA path and array index set.
 */
FCurvePtr fake_fcurve(const char *rna_path, const int array_index)
{
  FCurve *fcurve = BKE_fcurve_create();

  if (rna_path) {
    fcurve->rna_path = BLI_strdup(rna_path);
  }
  fcurve->array_index = array_index;

  return FCurvePtr(fcurve);
}

/** Create a fake F-Curve and register the ID that it animates in the copy buffer. */
FCurvePtr fake_fcurve_in_buffer(const char *rna_path,
                                const int array_index,
                                const slot_handle_t slot_handle = Slot::unassigned,
                                ID *owner_id = nullptr)
{
  FCurvePtr fcurve_ptr = fake_fcurve(rna_path, array_index);

  if (owner_id) {
    keyframe_copy_buffer->slot_animated_ids.add_overwrite(slot_handle, owner_id);
  }
  return fcurve_ptr;
}

}  // namespace

/** Keyframe pasting tests for generic F-Curve matching. */
struct keyframes_paste : public testing::Test {
  static void SetUpTestSuite()
  {
    bke::gtest_setup();
    ANIM_fcurves_copybuf_reset();
  }

  static void TearDownTestSuite()
  {
    ANIM_fcurves_copybuf_free();
    bke::gtest_teardown();
  }
};

TEST_F(keyframes_paste, pastebuf_match_path_full)
{
  constexpr slot_handle_t unassigned = Slot::unassigned;

  { /* Null RNA paths only match when pasting to one target curve. */
    ANIM_fcurves_copybuf_reset();
    FCurvePtr fcurve_target = fake_fcurve(nullptr, 0);
    FCurvePtr fcurve_in_buffer = fake_fcurve_in_buffer(nullptr, 0);

    EXPECT_FALSE(pastebuf_match_path_full(
        nullptr, *fcurve_target, *fcurve_in_buffer, unassigned, false, false));
    EXPECT_TRUE(pastebuf_match_path_full(
        nullptr, *fcurve_target, *fcurve_in_buffer, unassigned, false, true));
  }

  { /* Multiple target curves require matching paths and array indices. */
    ANIM_fcurves_copybuf_reset();
    FCurvePtr target = fake_fcurve("location", 0);

    EXPECT_TRUE(pastebuf_match_path_full(nullptr,
                                         *target,
                                         *fake_fcurve_in_buffer("location", 0),
                                         unassigned,
                                         false,
                                         false));
    EXPECT_FALSE(pastebuf_match_path_full(nullptr,
                                          *target,
                                          *fake_fcurve_in_buffer("location", 1),
                                          unassigned,
                                          false,
                                          false));
    EXPECT_FALSE(pastebuf_match_path_full(nullptr,
                                          *target,
                                          *fake_fcurve_in_buffer("rotation_euler", 0),
                                          unassigned,
                                          false,
                                          false));

    /* A single source curve can match any array component, but its path still must match. */
    EXPECT_TRUE(pastebuf_match_path_full(nullptr,
                                         *target,
                                         *fake_fcurve_in_buffer("location", 2),
                                         unassigned,
                                         true,
                                         false));
    EXPECT_FALSE(pastebuf_match_path_full(nullptr,
                                          *target,
                                          *fake_fcurve_in_buffer("scale", 0),
                                          unassigned,
                                          true,
                                          false));
  }

  { /* A single target accepts any source path or array component. */
    ANIM_fcurves_copybuf_reset();
    FCurvePtr target = fake_fcurve("location", 0);

    EXPECT_TRUE(pastebuf_match_path_full(nullptr,
                                         *target,
                                         *fake_fcurve_in_buffer("rotation_euler", 2),
                                         unassigned,
                                         false,
                                         true));
  }
}

TEST_F(keyframes_paste, pastebuf_match_path_property)
{
  constexpr slot_handle_t unassigned = Slot::unassigned;
  Main *bmain = BKE_main_new();
  Object *object = BKE_object_add_only_object(bmain, OB_NO_DATA, "Object");

  { /* Property matching resolves the copied path against its owning ID. */
    ANIM_fcurves_copybuf_reset();
    FCurvePtr target = fake_fcurve("location", 0);
    FCurvePtr source = fake_fcurve_in_buffer("location", 0, unassigned, &object->id);

    EXPECT_TRUE(pastebuf_match_path_property(
        bmain, *target, *source, unassigned, false, false));

    FCurvePtr other_component = fake_fcurve_in_buffer("location", 1, unassigned, &object->id);
    EXPECT_FALSE(pastebuf_match_path_property(
        bmain, *target, *other_component, unassigned, false, false));

    FCurvePtr other_property = fake_fcurve_in_buffer("rotation_euler", 0, unassigned, &object->id);
    EXPECT_FALSE(pastebuf_match_path_property(
        bmain, *target, *other_property, unassigned, false, false));
  }

  { /* Unresolvable paths safely fail property matching. */
    ANIM_fcurves_copybuf_reset();
    FCurvePtr target = fake_fcurve("location", 0);
    Object *object_not_in_main = BKE_object_add_only_object(nullptr, OB_NO_DATA, "Detached");
    FCurvePtr source = fake_fcurve_in_buffer(
        "location", 0, unassigned, &object_not_in_main->id);

    EXPECT_FALSE(pastebuf_match_path_property(
        bmain, *target, *source, unassigned, false, false));

    BKE_id_free(nullptr, &object_not_in_main->id);
  }

  BKE_main_free(bmain);
}

TEST_F(keyframes_paste, pastebuf_match_index_only)
{
  constexpr slot_handle_t unassigned = Slot::unassigned;
  ANIM_fcurves_copybuf_reset();
  FCurvePtr target = fake_fcurve("some_prop", 1);

  EXPECT_TRUE(pastebuf_match_index_only(nullptr,
                                        *target,
                                        *fake_fcurve_in_buffer("location", 1),
                                        unassigned,
                                        false,
                                        false));
  EXPECT_FALSE(pastebuf_match_index_only(nullptr,
                                         *target,
                                         *fake_fcurve_in_buffer("location", 2),
                                         unassigned,
                                         false,
                                         false));
}

}  // namespace ed::animation::tests
}  // namespace blender
