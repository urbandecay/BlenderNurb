/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include <cstdlib>

#include "BLI_bounds.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"

#include "DNA_anim_types.h"
#include "DNA_scene_types.h"

#include "BKE_camera.h"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "GPU_batch.hh"
#include "GPU_vertex_buffer.hh"

#include "ED_anim_api.hh"
#include "ED_keyframes_keylist.hh"

#include "ANIM_action.hh"
#include "ANIM_action_legacy.hh"
#include "ANIM_animdata.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"anim.motion_paths"};

/* Motion path needing to be baked (mpt). */
struct MPathTarget {
  bMotionPath *mpath; /* Motion path in question. */

  AnimKeylist *keylist; /* Temp, to know where the keyframes are. */

  /* Original (Source Objects) */
  Object *ob; /* Source Object */
};

/* ........ */

Depsgraph *animviz_depsgraph_build(Main *bmain,
                                   Scene *scene,
                                   ViewLayer *view_layer,
                                   const Span<MPathTarget *> targets)
{
  /* Allocate dependency graph. */
  Depsgraph *depsgraph = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_VIEWPORT);

  /* Make a flat array of IDs for the DEG API. */
  Array<ID *> ids(targets.size());
  int current_id_index = 0;
  for (const MPathTarget *mpt : targets) {
    ids[current_id_index++] = &mpt->ob->id;
  }

  /* Build graph from all requested IDs. */
  DEG_graph_build_from_ids(depsgraph, ids);

  return depsgraph;
}

void animviz_build_motionpath_targets(Object *ob, Vector<MPathTarget *> &r_targets)
{
  MPathTarget *mpt;

  /* Object itself first. */
  if ((ob->avs.recalc & ANIMVIZ_RECALC_PATHS) && (ob->mpath)) {
    /* New target for object. */
    mpt = MEM_new_zeroed<MPathTarget>("MPathTarget Ob");
    mpt->mpath = ob->mpath;
    mpt->ob = ob;

    r_targets.append(mpt);
  }

}

void animviz_free_motionpath_targets(Vector<MPathTarget *> &targets)
{
  for (MPathTarget *mpt : targets) {
    MEM_delete(mpt);
  }
  targets.clear_and_shrink();
}

/* ........ */

/* Perform baking for the targets on the current frame. */
static void motionpaths_calc_bake_targets(const Span<MPathTarget *> targets,
                                          const int cframe,
                                          Depsgraph *depsgraph,
                                          Object *camera)
{
  /* For each target, check if it can be baked on the current frame. */
  for (const MPathTarget *mpt : targets) {
    bMotionPath *mpath = mpt->mpath;

    /* Current frame must be within the range the cache works for.
     * - is inclusive of the first frame, but not the last otherwise we get buffer overruns.
     */
    if ((cframe < mpath->start_frame) || (cframe >= mpath->end_frame)) {
      continue;
    }

    /* Get the relevant cache vert to write to. */
    bMotionPathVert *mpv = mpath->points + (cframe - mpath->start_frame);

    Object *ob_eval = DEG_get_evaluated(depsgraph, mpt->ob);

    /* World-space object location. */
    copy_v3_v3(mpv->co, ob_eval->object_to_world().location());

    if (mpath->flag & MOTIONPATH_FLAG_BAKE_CAMERA && camera) {
      Object *cam_eval = DEG_get_evaluated(depsgraph, camera);
      /* Aka projection matrix. */
      float4x4 window_matrix;
      Scene *scene = DEG_get_input_scene(depsgraph);
      BKE_camera_multiview_window_matrix(&scene->r, cam_eval, nullptr, window_matrix.ptr());
      /* World to Object is the view matrix. */
      float4x4 perspective_matrix = window_matrix * cam_eval->world_to_object();
      const float4 co_clip_space = perspective_matrix *
                                   float4(mpv->co[0], mpv->co[1], mpv->co[2], 1.0);
      /* Storing the verts in NDC space which contains lens effects like sensor offset. See
       * `overlay_motion_path.hh/motion_path_sync`. Negative w values are behind the camera, thus
       * can't be correctly projected into the scene. Using abs(w) is consistent with
       * `project_point` in shader code. */
      const float3 co_ndc_space = float3(co_clip_space) /
                                  math::max(math::abs(co_clip_space.w), 0.0001f);
      copy_v3_v3(mpv->co, co_ndc_space);
    }

    float mframe = float(cframe);

    /* Tag if it's a keyframe. */
    if (ED_keylist_find_exact(mpt->keylist, mframe)) {
      mpv->flag |= MOTIONPATH_VERT_KEY;
    }
    else {
      mpv->flag &= ~MOTIONPATH_VERT_KEY;
    }

    /* Incremental update on evaluated object if possible, for fast updating
     * while dragging in transform. */
    bMotionPath *mpath_eval = ob_eval->mpath;

    if (mpath_eval && mpath_eval->length == mpath->length) {
      bMotionPathVert *mpv_eval = mpath_eval->points + (cframe - mpath_eval->start_frame);
      *mpv_eval = *mpv;

      GPU_VERTBUF_DISCARD_SAFE(mpath_eval->points_vbo);
      GPU_BATCH_DISCARD_SAFE(mpath_eval->batch_line);
      GPU_BATCH_DISCARD_SAFE(mpath_eval->batch_points);
    }
  }
}

/* Returns the combined range of all `MPathTarget` start and end frames. */
static Bounds<int> motionpath_get_global_framerange(const Span<MPathTarget *> targets)
{
  Bounds<int> frame_range = {INT_MAX, INT_MIN};
  for (const MPathTarget *mpt : targets) {
    frame_range.min = min_ii(frame_range.min, mpt->mpath->start_frame);
    frame_range.max = max_ii(frame_range.max, mpt->mpath->end_frame);
  }
  return frame_range;
}

static Bounds<int> motionpath_calculate_update_range(MPathTarget *mpt,
                                                     const int current_frame)
{
  /* If the current frame is outside of the configured motion path range we ignore update of this
   * motion path by using invalid frame range where start frame is above the end frame. */
  if (current_frame < mpt->mpath->start_frame || current_frame > mpt->mpath->end_frame) {
    return {INT_MAX, INT_MIN};
  }

  /* Object paths have no narrower update interval, so recalculate their full configured range. */
  return {mpt->mpath->start_frame, mpt->mpath->end_frame};
}

static void motionpath_free_free_tree_data(MutableSpan<MPathTarget *> targets)
{
  for (MPathTarget *mpt : targets) {
    ED_keylist_free(mpt->keylist);
  }
}

void animviz_motionpath_compute_range(Object *ob, Scene *scene)
{
  bAnimVizSettings *avs = &ob->avs;

  if (avs->path_range == MOTIONPATH_RANGE_MANUAL) {
    /* Don't touch manually-determined ranges. */
    return;
  }

  const bool has_action = ob->adt && ob->adt->action;
  if (avs->path_range == MOTIONPATH_RANGE_SCENE || !has_action ||
      !animrig::legacy::assigned_action_has_keyframes(ob->adt))
  {
    /* Default to the scene (preview) range if there is no animation data to
     * find selected keys in. */
    avs->path_sf = scene->playback_start();
    avs->path_ef = scene->playback_end();
    return;
  }

  AnimKeylist *keylist = ED_keylist_create();
  for (FCurve *fcu : animrig::fcurves_for_assigned_action(ob->adt)) {
    fcurve_to_keylist(ob->adt, fcu, keylist, 0, {-FLT_MAX, FLT_MAX}, true);
  }

  Bounds<float> frame_range;
  switch (avs->path_range) {
    case MOTIONPATH_RANGE_KEYS_SELECTED:
      if (ED_keylist_selected_keys_frame_range(keylist, &frame_range)) {
        break;
      }
      ATTR_FALLTHROUGH; /* Fall through if there were no selected keys found. */
    case MOTIONPATH_RANGE_KEYS_ALL:
      ED_keylist_all_keys_frame_range(keylist, &frame_range);
      break;
    case MOTIONPATH_RANGE_MANUAL:
    case MOTIONPATH_RANGE_SCENE:
      BLI_assert_msg(false, "This should not happen, function should have exited earlier.");
  };

  avs->path_sf = frame_range.min;
  avs->path_ef = frame_range.max;

  ED_keylist_free(keylist);
}

static void build_keylist_for_target(MPathTarget &target, AnimKeylist &keylist)
{
  for (FCurve *fcu : animrig::fcurves_for_assigned_action(target.ob->adt)) {
    /* When only updating a subset of the motion path we could pass a range here to improve
     * performance. */
    fcurve_to_keylist(target.ob->adt, fcu, &keylist, 0, {-FLT_MAX, FLT_MAX}, true);
  }
}

void animviz_calc_motionpaths(Depsgraph *depsgraph,
                              Scene *scene,
                              MutableSpan<MPathTarget *> targets,
                              eAnimvizCalcRange range)
{
  BLI_assert_msg(!DEG_is_active(depsgraph),
                 "Motion path calculation should always happen with a minimal depsgraph.");

  if (targets.is_empty()) {
    return;
  }

  /* The frame range to calculate. Inclusive/Exclusive. */
  Bounds<int> frame_range = {INT_MAX, INT_MIN};
  switch (range) {
    case ANIMVIZ_CALC_RANGE_CHANGED:
      /* Nothing to do here, will be handled later when iterating through the targets. */
      break;
    case ANIMVIZ_CALC_RANGE_FULL:
      frame_range = motionpath_get_global_framerange(targets);
      if (frame_range.is_empty()) {
        return;
      }
      break;
  }

  for (MPathTarget *mpt : targets) {

    /* Build list of all keyframes in the active action for the object. */
    mpt->keylist = ED_keylist_create();

    if (mpt->ob->adt && mpt->ob->adt->action) {
      build_keylist_for_target(*mpt, *mpt->keylist);
    }
    ED_keylist_prepare_for_direct_access(mpt->keylist);

    if (range == ANIMVIZ_CALC_RANGE_CHANGED) {
      const Bounds<int> target_bounds = motionpath_calculate_update_range(mpt, scene->r.cfra);
      if (!target_bounds.is_empty()) {
        frame_range.min = min_ii(frame_range.min, target_bounds.min);
        frame_range.max = max_ii(frame_range.max, target_bounds.max);
      }
    }
  }

  if (frame_range.is_empty()) {
    motionpath_free_free_tree_data(targets);
    return;
  }

  /* Calculate path over requested range. */
  CLOG_INFO(&LOG,
            "Calculating MotionPaths between frames %d - %d (%d frames)",
            frame_range.min,
            frame_range.max,
            frame_range.max - frame_range.min + 1);

  for (int frame = frame_range.min; frame < frame_range.max; frame++) {
    /* Update relevant data for new frame. */
    DEG_evaluate_on_framechange(depsgraph, frame);

    /* Perform baking for targets. */
    motionpaths_calc_bake_targets(targets, frame, depsgraph, scene->camera);
  }

  /* Clear recalc flags from targets. */
  for (MPathTarget *mpt : targets) {
    bMotionPath *mpath = mpt->mpath;

    /* Clear the flag requesting recalculation of targets. */
    mpt->ob->avs.recalc &= ~ANIMVIZ_RECALC_PATHS;

    /* Clean temp data. */
    ED_keylist_free(mpt->keylist);

    /* Free previous batches to force update. */
    GPU_VERTBUF_DISCARD_SAFE(mpath->points_vbo);
    GPU_BATCH_DISCARD_SAFE(mpath->batch_line);
    GPU_BATCH_DISCARD_SAFE(mpath->batch_points);
  }
}

}  // namespace blender
