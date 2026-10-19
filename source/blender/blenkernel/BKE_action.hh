/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 * \brief Blender kernel action functionality.
 */

#include "BLI_compiler_attrs.h"
#include "BLI_sys_types.h"

namespace blender {

struct AnimationEvalContext;
struct Main;
struct Object;
struct bAction;
struct bActionGroup;

namespace animrig {

/** Handle type used to identify a slot within an Action. */
using slot_handle_t = int32_t;

}  // namespace animrig

/* Action Lib Stuff ----------------- */

/* Allocate a new bAction with the given name */
bAction *BKE_action_add(Main *bmain, const char name[]);

/* Action Groups API ----------------- */

/**
 * Sync colors used for action group with theme settings.
 * This has to be called when the color theme index on the group changes so the actual color can be
 * copied from the theme.
 *
 * This updates the group's stored color from the user theme.
 */
void action_group_colors_sync(bActionGroup *grp);

/* Assorted Evaluation ----------------- */

/**
 * Calculate the effects of an Action at the given frame on an object. This is used by the Action
 * Constraint.
 */
void what_does_obaction(Object *ob,
                        Object *workob,
                        bAction *act,
                        int32_t action_slot_handle,
                        char groupname[],
                        const AnimationEvalContext *anim_eval_context) ATTR_NONNULL(1, 2);

}  // namespace blender
