/* SPDX-FileCopyrightText: 2009 Blender Authors, Joshua Leung. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "BLI_sys_types.h"

#include "DNA_curve_types.h"

#include "ED_anim_api.hh"
#include "ED_keyframes_keylist.hh"

namespace blender {

struct AnimData;
struct ChannelDrawList;
struct FCurve;
struct MaskLayer;
struct Object;
struct Scene;
struct View2D;
struct bAction;
struct bActionGroup;
struct bAnimContext;
struct bDopeSheet;

/**
 * Draw simple diamond-shape keyframe.
 *
 * The caller should set up vertex format, bind #GPU_SHADER_KEYFRAME_SHAPE,
 * `immBegin(GPU_PRIM_POINTS, n)`, then call this `n` times.
 */
struct KeyframeShaderBindings {
  uint pos_id;
  uint size_id;
  uint color_id;
  uint outline_color_id;
  uint flags_id;
};

void draw_keyframe_shape(float x,
                         float y,
                         float size,
                         bool sel,
                         eBezTriple_KeyframeType key_type,
                         eKeyframeShapeDrawOpts mode,
                         float alpha,
                         const KeyframeShaderBindings *sh_bindings,
                         short handle_type,
                         short extreme_type);

/* ******************************* Methods ****************************** */

/* Channel Drawing ------------------ */
/* F-Curve */
void ED_add_fcurve_channel(ChannelDrawList *channel_list,
                           bAnimListElem *ale,
                           FCurve *fcu,
                           float ypos,
                           float yscale_fac,
                           int saction_flag);
/* Action Group Summary */
void ED_add_action_group_channel(ChannelDrawList *channel_list,
                                 bAnimListElem *ale,
                                 bActionGroup *agrp,
                                 float ypos,
                                 float yscale_fac,
                                 int saction_flag);
/* Layered Action Summary. */
void ED_add_action_layered_channel(ChannelDrawList *channel_list,
                                   bAnimContext *ac,
                                   bAnimListElem *ale,
                                   bAction *action,
                                   const float ypos,
                                   const float yscale_fac,
                                   int saction_flag);
/* Action Slot summary. */
void ED_add_action_slot_channel(ChannelDrawList *channel_list,
                                bAnimContext *ac,
                                bAnimListElem *ale,
                                animrig::Action &action,
                                animrig::Slot &slot,
                                float ypos,
                                float yscale_fac,
                                int saction_flag);

/* Object Summary */
void ED_add_object_channel(ChannelDrawList *channel_list,
                           bDopeSheet *ads,
                           Object *ob,
                           float ypos,
                           float yscale_fac,
                           int saction_flag);
/* Scene Summary */
void ED_add_scene_channel(ChannelDrawList *channel_list,
                          bDopeSheet *ads,
                          Scene *sce,
                          float ypos,
                          float yscale_fac,
                          int saction_flag);
/* DopeSheet Summary */
void ED_add_summary_channel(ChannelDrawList *channel_list,
                            bAnimContext *ac,
                            float ypos,
                            float yscale_fac,
                            int saction_flag);

/* Mask Layer */
void ED_add_mask_layer_channel(ChannelDrawList *channel_list,
                               bDopeSheet *ads,
                               MaskLayer *masklay,
                               float ypos,
                               float yscale_fac,
                               int saction_flag);

ChannelDrawList *ED_channel_draw_list_create();

void ED_channel_list_flush(ChannelDrawList *channel_list, View2D *v2d);

void ED_channel_list_free(ChannelDrawList *channel_list);

}  // namespace blender
