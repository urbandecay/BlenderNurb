/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_curve_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"

#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_action.hh"
#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_tracking.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "ED_anim_api.hh"
#include "ED_curves.hh"
#include "ED_keyframing.hh"
#include "ED_nurbs.hh"
#include "ED_object.hh"
#include "ED_pointcloud.hh"
#include "ED_screen.hh"
#include "ED_transverts.hh"

#include "ANIM_action.hh"
#include "ANIM_keyframing.hh"
#include "ANIM_keyingsets.hh"

#include "view3d_intern.hh"

namespace blender {

static bool snap_curs_to_sel_ex(bContext *C, const int pivot_point, float r_cursor[3]);
static bool snap_calc_active_center(bContext *C, const bool select_only, float r_center[3]);

/* -------------------------------------------------------------------- */
/** \name Snap Selection to Grid Operator
 * \{ */

/** Snaps every individual object center to its nearest point on the grid. */
static wmOperatorStatus snap_sel_to_grid_exec(bContext *C, wmOperator *op)
{
  using namespace blender::ed;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewLayer *view_layer_eval = DEG_get_evaluated_view_layer(depsgraph);
  Object *obact = CTX_data_active_object(C);
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  View3D *v3d = CTX_wm_view3d(C);
  TransVertStore tvs = {nullptr};
  TransVert *tv;
  float gridf, imat[3][3], bmat[3][3], vec[3];
  int a;

  gridf = ED_view3d_grid_view_scale(scene, v3d, region, nullptr);

  if (OBEDIT_FROM_OBACT(obact)) {
    const Main *bmain = CTX_data_main(C);
    ViewLayer *view_layer = CTX_data_view_layer(C);
    Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
        *bmain, scene, view_layer, CTX_wm_view3d(C));
    for (Object *obedit : objects) {
      if (obedit->type == OB_MESH) {
        BMEditMesh *em = BKE_editmesh_from_object(obedit);

        if (em->bm->totvertsel == 0) {
          continue;
        }
      }

      if (ed::object::shape_key_report_if_locked(obedit, op->reports)) {
        continue;
      }

      if (ED_transverts_check_obedit(obedit)) {
        const Object *obedit_eval = DEG_get_evaluated(depsgraph, obedit);
        ED_transverts_create_from_obedit(&tvs, obedit_eval, 0);
      }

      if (tvs.transverts_tot != 0) {
        copy_m3_m4(bmat, obedit->object_to_world().ptr());
        invert_m3_m3(imat, bmat);

        tv = tvs.transverts;
        for (a = 0; a < tvs.transverts_tot; a++, tv++) {
          copy_v3_v3(vec, tv->loc);
          mul_m3_v3(bmat, vec);
          add_v3_v3(vec, obedit->object_to_world().location());
          vec[0] = gridf * floorf(0.5f + vec[0] / gridf);
          vec[1] = gridf * floorf(0.5f + vec[1] / gridf);
          vec[2] = gridf * floorf(0.5f + vec[2] / gridf);
          sub_v3_v3(vec, obedit->object_to_world().location());

          mul_m3_v3(imat, vec);
          copy_v3_v3(tv->loc, vec);
        }
        ED_transverts_update_obedit(&tvs, obedit);
      }
      ED_transverts_free(&tvs);
    }
  }
  else {
    /* Object mode. */
    Main *bmain = CTX_data_main(C);

    KeyingSet *ks = animrig::get_keyingset_for_autokeying(scene, ANIM_KS_LOCATION_ID);

    const bool use_transform_data_origin = (scene->toolsettings->transform_flag &
                                            SCE_XFORM_DATA_ORIGIN);
    object::XFormObjectData_Container *xds = nullptr;

    /* Build object array. */
    Vector<Object *> objects_eval;
    Vector<Object *> objects_orig;
    {
      FOREACH_SELECTED_EDITABLE_OBJECT_BEGIN (view_layer_eval, v3d, ob_eval) {
        objects_eval.append(ob_eval);
        objects_orig.append(DEG_get_original(ob_eval));
      }
      FOREACH_SELECTED_EDITABLE_OBJECT_END;
    }

    if (use_transform_data_origin) {
      BKE_scene_graph_evaluated_ensure(depsgraph, bmain);
      xds = object::data_xform_container_create();
    }

    if (animrig::is_autokey_on(scene)) {
      ANIM_deselect_keys_in_animation_editors(C);
    }

    for (Object *ob_eval : objects_eval) {
      Object *ob = DEG_get_original(ob_eval);
      vec[0] = -ob_eval->object_to_world().location()[0] +
               gridf * floorf(0.5f + ob_eval->object_to_world().location()[0] / gridf);
      vec[1] = -ob_eval->object_to_world().location()[1] +
               gridf * floorf(0.5f + ob_eval->object_to_world().location()[1] / gridf);
      vec[2] = -ob_eval->object_to_world().location()[2] +
               gridf * floorf(0.5f + ob_eval->object_to_world().location()[2] / gridf);

      const float3 loc_final = float3(ob_eval->loc) + float3(vec);
      BKE_object_protected_location_set(ob, loc_final);

      /* auto-keyframing */
      animrig::autokeyframe_object(C, scene, ob, ks);

      if (use_transform_data_origin) {
        object::data_xform_container_item_ensure(xds, ob);
      }

      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
    }

    if (use_transform_data_origin) {
      object::data_xform_container_update_all(xds, bmain, depsgraph);
      object::data_xform_container_destroy(xds);
    }
  }

  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);

  return OPERATOR_FINISHED;
}

void VIEW3D_OT_snap_selected_to_grid(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Selection to Grid";
  ot->description = "Snap selected item(s) to their nearest grid division";
  ot->idname = "VIEW3D_OT_snap_selected_to_grid";

  /* API callbacks. */
  ot->exec = snap_sel_to_grid_exec;
  ot->poll = ED_operator_region_view3d_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Selection to Location (Utility)
 * \{ */

/**
 * Snaps the selection as a whole (use_offset=true) or each selected object to the given location.
 *
 * \param target_loc_global: a location in global space to snap to
 * (eg. 3D cursor or active object).
 * \param target_orientation_global: a 3d cursor which will provides rotation.
 * When non-null objects are rotated to match the rotation of the 3d cursor,
 * otherwise, keep their original rotation.
 * Note that a more generic orientation parameter could be supported in future, but for now,
 * only the 3d cursor is used.
 * \param use_offset: if the selected objects should maintain their relative offsets
 * and be snapped by the selection pivot point (median, active),
 * or if every object origin should be snapped to the given location.
 */
static bool snap_selected_to_location_rotation(bContext *C,
                                               wmOperator *op,
                                               const float3 &target_loc_global,
                                               const View3DCursor *target_orientation_global,
                                               const bool use_offset,
                                               const int pivot_point,
                                               const bool use_toolsettings)
{
  using namespace blender::ed;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Scene *scene = CTX_data_scene(C);
  Object *obedit = CTX_data_edit_object(C);
  View3D *v3d = CTX_wm_view3d(C);
  TransVertStore tvs = {nullptr};
  TransVert *tv;
  float imat[3][3], bmat[3][3];
  float center_global[3];
  float offset_global[3];
  int a;

  /* Some use of this needs to transform into local-space. */
  const bool use_rotation = target_orientation_global != nullptr;
  const float3x3 target_rot_global = target_orientation_global ?
                                         target_orientation_global->matrix<float3x3>() :
                                         float3x3::zero();

  if (use_offset) {
    if ((pivot_point == V3D_AROUND_ACTIVE) && snap_calc_active_center(C, true, center_global)) {
      /* pass */
    }
    else {
      snap_curs_to_sel_ex(C, pivot_point, center_global);
    }
    sub_v3_v3v3(offset_global, target_loc_global, center_global);
  }

  if (obedit) {
    float3 target_loc_local;
    const Main *bmain = CTX_data_main(C);
    ViewLayer *view_layer = CTX_data_view_layer(C);
    Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
        *bmain, scene, view_layer, v3d);
    for (const int ob_index : objects.index_range()) {
      obedit = objects[ob_index];

      if (obedit->type == OB_MESH) {
        BMEditMesh *em = BKE_editmesh_from_object(obedit);

        if (em->bm->totvertsel == 0) {
          continue;
        }
      }

      if (ed::object::shape_key_report_if_locked(obedit, op->reports)) {
        continue;
      }

      if (ED_transverts_check_obedit(obedit)) {
        const Object *ob_eval = DEG_get_evaluated(depsgraph, obedit);
        ED_transverts_create_from_obedit(&tvs, ob_eval, 0);
      }

      if (tvs.transverts_tot != 0) {
        copy_m3_m4(bmat, obedit->object_to_world().ptr());
        invert_m3_m3(imat, bmat);

        /* Get the `target_loc_global` in object space. */
        sub_v3_v3v3(target_loc_local, target_loc_global, obedit->object_to_world().location());
        mul_m3_v3(imat, target_loc_local);

        if (use_offset) {
          float3 offset_local;

          mul_v3_m3v3(offset_local, imat, offset_global);

          tv = tvs.transverts;
          for (a = 0; a < tvs.transverts_tot; a++, tv++) {
            add_v3_v3(tv->loc, offset_local);
          }
        }
        else {
          tv = tvs.transverts;
          for (a = 0; a < tvs.transverts_tot; a++, tv++) {
            copy_v3_v3(tv->loc, target_loc_local);
          }
        }
        ED_transverts_update_obedit(&tvs, obedit);
      }
      ED_transverts_free(&tvs);
    }
  }
  else {
    KeyingSet *ks = animrig::get_keyingset_for_autokeying(scene, ANIM_KS_LOCATION_ID);
    Main *bmain = CTX_data_main(C);
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    BKE_scene_graph_evaluated_ensure(depsgraph, bmain);

    /* Build object array. */
    ViewLayer *view_layer = CTX_data_view_layer(C);
    Vector<Object *> objects;
    {
      FOREACH_SELECTED_EDITABLE_OBJECT_BEGIN (view_layer, v3d, ob) {
        objects.append(ob);
      }
      FOREACH_SELECTED_EDITABLE_OBJECT_END;
    }

    const bool use_transform_data_origin = use_toolsettings &&
                                           (scene->toolsettings->transform_flag &
                                            SCE_XFORM_DATA_ORIGIN);
    object::XFormObjectData_Container *xds = nullptr;

    if (use_transform_data_origin) {
      xds = object::data_xform_container_create();

      /* Initialize the transform data in a separate loop because the depsgraph
       * may be evaluated while setting the locations. */
      for (Object *ob : objects) {
        object::data_xform_container_item_ensure(xds, ob);
      }
    }

    if (animrig::is_autokey_on(scene)) {
      ANIM_deselect_keys_in_animation_editors(C);
    }

    for (Object *ob : objects) {
      float3 target_loc_local;

      if (use_offset) {
        add_v3_v3v3(target_loc_local, ob->object_to_world().location(), offset_global);

        if (use_rotation) {
          sub_v3_v3(target_loc_local, target_loc_global);
          mul_m3_v3(target_rot_global.ptr(), target_loc_local);
          add_v3_v3(target_loc_local, target_loc_global);
        }
      }
      else {
        copy_v3_v3(target_loc_local, target_loc_global);
      }
      sub_v3_v3(target_loc_local, ob->object_to_world().location());

      float3x3 target_rot = target_rot_global;
      if (use_toolsettings) {
        const float3 loc_final = float3(ob->loc) + target_loc_local;
        BKE_object_protected_location_set(ob, loc_final);

        /* auto-keyframing */
        animrig::autokeyframe_object(C, scene, ob, ks);
      }
      else {
        add_v3_v3(ob->loc, target_loc_local);
      }

      if (use_rotation) {
        const bool assign_rotation_directly = (ob->rotmode ==
                                               target_orientation_global->rotation_mode);

        if (ob->rotmode == ROT_MODE_QUAT) {
          float quat[4];
          if (assign_rotation_directly) {
            copy_v4_v4(quat, target_orientation_global->rotation_quaternion);
          }
          else {
            mat3_normalized_to_quat(quat, target_rot.ptr());
          }
          if (use_toolsettings) {
            BKE_object_protected_rotation_quaternion_set(ob, quat);
          }
          else {
            copy_v4_v4(ob->quat, quat);
          }
        }
        else if (ob->rotmode == ROT_MODE_AXISANGLE) {
          float rot_axis[3];
          float rot_angle;
          if (assign_rotation_directly) {
            copy_v3_v3(rot_axis, target_orientation_global->rotation_axis);
            rot_angle = target_orientation_global->rotation_angle;
          }
          else {
            mat3_to_axis_angle(rot_axis, &rot_angle, target_rot.ptr());
          }
          if (use_toolsettings) {
            BKE_object_protected_rotation_axisangle_set(ob, rot_axis, rot_angle);
          }
          else {
            copy_v3_v3(ob->rotAxis, rot_axis);
            ob->rotAngle = rot_angle;
          }
        }
        else {
          float rot_euler[3];
          if (assign_rotation_directly) {
            copy_v3_v3(rot_euler, target_orientation_global->rotation_euler);
          }
          else {
            mat3_to_eulO(rot_euler, ob->rotmode, target_rot.ptr());
          }
          if (use_toolsettings) {
            BKE_object_protected_rotation_euler_set(ob, rot_euler);
          }
          else {
            copy_v3_v3(ob->rot, rot_euler);
          }
        }

        /* auto-keyframing */
        animrig::autokeyframe_object(C, scene, ob, ks);
      }

      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
    }

    if (use_transform_data_origin) {
      object::data_xform_container_update_all(xds, bmain, depsgraph);
      object::data_xform_container_destroy(xds);
    }
  }

  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);

  return true;
}

bool ED_view3d_snap_selected_to_location(bContext *C,
                                         wmOperator *op,
                                         const float target_loc_global[3],
                                         const int pivot_point)
{
  /* These could be passed as arguments if needed. */
  /* Always use pivot point. */
  const bool use_offset = true;
  /* Disable object protected flags & auto-keyframing,
   * so this can be used as a low level function. */
  const bool use_toolsettings = false;
  return snap_selected_to_location_rotation(
      C, op, target_loc_global, nullptr, use_offset, pivot_point, use_toolsettings);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Selection to Cursor Operator
 * \{ */

static wmOperatorStatus snap_selected_to_cursor_exec(bContext *C, wmOperator *op)
{
  const bool use_offset = RNA_boolean_get(op->ptr, "use_offset");
  const bool use_rotation = RNA_boolean_get(op->ptr, "use_rotation");

  Scene *scene = CTX_data_scene(C);

  const float *target_loc_global = scene->cursor.location;
  const View3DCursor *snap_orientation = use_rotation ? &scene->cursor : nullptr;
  const int pivot_point = scene->toolsettings->transform_pivot_point;

  if (snap_selected_to_location_rotation(
          C, op, target_loc_global, snap_orientation, use_offset, pivot_point, true))
  {
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void VIEW3D_OT_snap_selected_to_cursor(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Selection to Cursor";
  ot->description = "Snap selected item(s) to the 3D cursor";
  ot->idname = "VIEW3D_OT_snap_selected_to_cursor";

  /* API callbacks. */
  ot->exec = snap_selected_to_cursor_exec;
  ot->poll = ED_operator_view3d_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* rna */
  RNA_def_boolean(ot->srna,
                  "use_offset",
                  true,
                  "Offset",
                  "If the selection should be snapped as a whole or by each object center");
  RNA_def_boolean(ot->srna,
                  "use_rotation",
                  false,
                  "Rotation",
                  "If the selection should be rotated to match the cursor");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Selection to Active Operator
 * \{ */

/** Snaps each selected object to the location of the active selected object. */
static wmOperatorStatus snap_selected_to_active_exec(bContext *C, wmOperator *op)
{
  float target_loc_global[3];

  if (snap_calc_active_center(C, false, target_loc_global) == false) {
    BKE_report(op->reports, RPT_ERROR, "No active element found!");
    return OPERATOR_CANCELLED;
  }

  if (!snap_selected_to_location_rotation(C, op, target_loc_global, nullptr, false, -1, true)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_snap_selected_to_active(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Selection to Active";
  ot->description = "Snap selected item(s) to the active item";
  ot->idname = "VIEW3D_OT_snap_selected_to_active";

  /* API callbacks. */
  ot->exec = snap_selected_to_active_exec;
  ot->poll = ED_operator_view3d_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Cursor to Grid Operator
 * \{ */

/** Snaps the 3D cursor location to its nearest point on the grid. */
static wmOperatorStatus snap_curs_to_grid_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  View3D *v3d = CTX_wm_view3d(C);
  float gridf, *curs;

  gridf = ED_view3d_grid_view_scale(scene, v3d, region, nullptr);
  curs = scene->cursor.location;

  curs[0] = gridf * floorf(0.5f + curs[0] / gridf);
  curs[1] = gridf * floorf(0.5f + curs[1] / gridf);
  curs[2] = gridf * floorf(0.5f + curs[2] / gridf);

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr); /* hrm */
  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);

  return OPERATOR_FINISHED;
}

void VIEW3D_OT_snap_cursor_to_grid(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Cursor to Grid";
  ot->description = "Snap 3D cursor to the nearest grid division";
  ot->idname = "VIEW3D_OT_snap_cursor_to_grid";

  /* API callbacks. */
  ot->exec = snap_curs_to_grid_exec;
  ot->poll = ED_operator_region_view3d_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Cursor to Selection Operator
 * \{ */

/**
 * Returns the center position of a tracking marker visible on the viewport
 * (useful to snap to).
 */
static void bundle_midpoint(Scene *scene, Object *ob, float r_vec[3])
{
  MovieClip *clip = BKE_object_movieclip_get(scene, ob, false);
  bool ok = false;
  float min[3], max[3], mat[4][4], pos[3], cammat[4][4];

  if (!clip) {
    return;
  }

  MovieTracking *tracking = &clip->tracking;

  copy_m4_m4(cammat, ob->object_to_world().ptr());

  BKE_tracking_get_camera_object_matrix(ob, mat);

  INIT_MINMAX(min, max);

  for (MovieTrackingObject &tracking_object : tracking->objects) {
    float obmat[4][4];

    if (tracking_object.flag & TRACKING_OBJECT_CAMERA) {
      copy_m4_m4(obmat, mat);
    }
    else {
      float imat[4][4];

      BKE_tracking_camera_get_reconstructed_interpolate(
          tracking, &tracking_object, scene->r.cfra, imat);
      invert_m4(imat);

      mul_m4_m4m4(obmat, cammat, imat);
    }

    for (const MovieTrackingTrack &track : tracking_object.tracks) {
      if ((track.flag & TRACK_HAS_BUNDLE) && TRACK_SELECTED(&track)) {
        ok = true;
        mul_v3_m4v3(pos, obmat, track.bundle_pos);
        minmax_v3v3_v3(min, max, pos);
      }
    }
  }

  if (ok) {
    mid_v3_v3v3(r_vec, min, max);
  }
}

/** Snaps the 3D cursor location to the median point of the selection. */
static bool snap_curs_to_sel_ex(bContext *C, const int pivot_point, float r_cursor[3])
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewLayer *view_layer_eval = DEG_get_evaluated_view_layer(depsgraph);
  Object *obedit = CTX_data_edit_object(C);
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  View3D *v3d = CTX_wm_view3d(C);
  TransVertStore tvs = {nullptr};
  TransVert *tv;
  float bmat[3][3], vec[3], min[3], max[3], centroid[3];
  int count = 0;

  INIT_MINMAX(min, max);
  zero_v3(centroid);

  if (obedit) {
    ViewLayer *view_layer = CTX_data_view_layer(C);
    Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
        *bmain, scene, view_layer, CTX_wm_view3d(C));
    for (const int ob_index : objects.index_range()) {
      obedit = objects[ob_index];

      /* We can do that quick check for meshes only... */
      if (obedit->type == OB_MESH) {
        BMEditMesh *em = BKE_editmesh_from_object(obedit);

        if (em->bm->totvertsel == 0) {
          continue;
        }
      }

      if (ED_transverts_check_obedit(obedit)) {
        const Object *obedit_eval = DEG_get_evaluated(depsgraph, obedit);
        ED_transverts_create_from_obedit(&tvs, obedit_eval, TM_SKIP_HANDLES);
      }

      count += tvs.transverts_tot;
      if (tvs.transverts_tot != 0) {
        Object *obedit_eval = DEG_get_evaluated(depsgraph, obedit);
        copy_m3_m4(bmat, obedit_eval->object_to_world().ptr());

        tv = tvs.transverts;
        for (int i = 0; i < tvs.transverts_tot; i++, tv++) {
          copy_v3_v3(vec, tv->loc);
          mul_m3_v3(bmat, vec);
          add_v3_v3(vec, obedit_eval->object_to_world().location());
          add_v3_v3(centroid, vec);
          minmax_v3v3_v3(min, max, vec);
        }
      }
      ED_transverts_free(&tvs);
    }
  }
  else {
    FOREACH_SELECTED_OBJECT_BEGIN (view_layer_eval, v3d, ob_eval) {
      copy_v3_v3(vec, ob_eval->object_to_world().location());

      /* special case for camera -- snap to bundles */
      if (ob_eval->type == OB_CAMERA) {
        /* snap to bundles should happen only when bundles are visible */
        if (v3d->flag2 & V3D_SHOW_RECONSTRUCTION) {
          bundle_midpoint(scene, DEG_get_original(ob_eval), vec);
        }
      }

      add_v3_v3(centroid, vec);
      minmax_v3v3_v3(min, max, vec);
      count++;
    }
    FOREACH_SELECTED_OBJECT_END;
  }

  if (count == 0) {
    return false;
  }

  if (pivot_point == V3D_AROUND_CENTER_BOUNDS) {
    mid_v3_v3v3(r_cursor, min, max);
  }
  else {
    mul_v3_fl(centroid, 1.0f / float(count));
    copy_v3_v3(r_cursor, centroid);
  }
  return true;
}

static wmOperatorStatus snap_curs_to_sel_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  const int pivot_point = scene->toolsettings->transform_pivot_point;
  if (snap_curs_to_sel_ex(C, pivot_point, scene->cursor.location)) {
    WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
    DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void VIEW3D_OT_snap_cursor_to_selected(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Cursor to Selected";
  ot->description = "Snap 3D cursor to the middle of the selected item(s)";
  ot->idname = "VIEW3D_OT_snap_cursor_to_selected";

  /* API callbacks. */
  ot->exec = snap_curs_to_sel_exec;
  ot->poll = ED_operator_view3d_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Cursor to Active Operator
 * \{ */

/**
 * Calculates the center position of the active object in global space.
 *
 * NOTE: this could be exported to be a generic function.
 * see: #calculateCenterActive
 */
static bool snap_calc_active_center(bContext *C, const bool select_only, float r_center[3])
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  return ed::object::calc_active_center(ob, select_only, r_center);
}

static wmOperatorStatus snap_curs_to_active_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);

  if (snap_calc_active_center(C, false, scene->cursor.location)) {
    WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
    DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void VIEW3D_OT_snap_cursor_to_active(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Cursor to Active";
  ot->description = "Snap 3D cursor to the active item";
  ot->idname = "VIEW3D_OT_snap_cursor_to_active";

  /* API callbacks. */
  ot->exec = snap_curs_to_active_exec;
  ot->poll = ED_operator_view3d_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Cursor to Center Operator
 * \{ */

/** Snaps the 3D cursor location to the origin and clears cursor rotation. */
static wmOperatorStatus snap_curs_to_center_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);

  scene->cursor.set_matrix(float4x4::identity(), false);

  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_snap_cursor_to_center(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Cursor to World Origin";
  ot->description = "Snap 3D cursor to the world origin";
  ot->idname = "VIEW3D_OT_snap_cursor_to_center";

  /* API callbacks. */
  ot->exec = snap_curs_to_center_exec;
  ot->poll = ED_operator_view3d_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Min/Max Object Vertices Utility
 * \{ */

static std::optional<Bounds<float3>> bounds_min_max_with_transform(const float4x4 &transform,
                                                                   const Span<float3> positions,
                                                                   const IndexMask &mask)
{
  if (mask.is_empty()) {
    return std::nullopt;
  }
  return threading::parallel_reduce(
      mask.index_range(),
      1024,
      Bounds<float3>(math::transform_point(transform, positions[mask.first()])),
      [&](const IndexRange range, Bounds<float3> init) {
        mask.slice(range).foreach_index_optimized<int>([&](const int i) {
          math::min_max(math::transform_point(transform, positions[i]), init.min, init.max);
        });
        return init;
      },
      [](const Bounds<float3> &a, const Bounds<float3> &b) { return bounds::merge(a, b); });
}

bool ED_view3d_minmax_verts(const Scene * /*scene*/,
                            Object *obedit,
                            float r_min[3],
                            float r_max[3])
{
  using namespace blender::ed;
  TransVertStore tvs = {nullptr};
  TransVert *tv;
  float centroid[3], vec[3], bmat[3][3];

  if (ELEM(obedit->type, OB_CURVES_LEGACY, OB_SURF)) {
    const Curve &curve = *id_cast<const Curve *>(obedit->data);
    if (const auto source = bke::nurbs::curve_eval_source_capture(curve)) {
      if (!source->snapshot) {
        return false;
      }
      const auto positions = nurbs::view_selection_positions(*source->snapshot,
                                                             obedit->object_to_world());
      if (!positions || positions->is_empty()) {
        return false;
      }
      for (const float3 &position : *positions) {
        minmax_v3v3_v3(r_min, r_max, position);
      }
      return true;
    }
  }

  if (obedit->type == OB_POINTCLOUD) {
    const Object &ob_orig = *DEG_get_original(obedit);
    const PointCloud &pointcloud = *id_cast<const PointCloud *>(ob_orig.data);

    IndexMaskMemory memory;
    const IndexMask mask = pointcloud::retrieve_selected_points(pointcloud, memory);

    const std::optional<Bounds<float3>> bounds = bounds_min_max_with_transform(
        obedit->object_to_world(), pointcloud.positions(), mask);

    if (bounds) {
      minmax_v3v3_v3(r_min, r_max, bounds->min);
      minmax_v3v3_v3(r_min, r_max, bounds->max);
      return true;
    }
    return false;
  }
  if (obedit->type == OB_CURVES) {
    const Object &ob_orig = *DEG_get_original(obedit);
    const Curves &curves_id = *id_cast<const Curves *>(ob_orig.data);
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();

    IndexMaskMemory memory;
    const IndexMask mask = curves::retrieve_selected_points(curves, memory);

    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(obedit, ob_orig);

    const std::optional<Bounds<float3>> bounds = bounds_min_max_with_transform(
        obedit->object_to_world(), deformation.positions, mask);

    if (bounds) {
      minmax_v3v3_v3(r_min, r_max, bounds->min);
      minmax_v3v3_v3(r_min, r_max, bounds->max);
      return true;
    }
    return false;
  }
  if (ED_transverts_check_obedit(obedit)) {
    ED_transverts_create_from_obedit(&tvs, obedit, TM_CALC_MAPLOC);
  }

  if (tvs.transverts_tot == 0) {
    return false;
  }

  copy_m3_m4(bmat, obedit->object_to_world().ptr());

  tv = tvs.transverts;
  for (int a = 0; a < tvs.transverts_tot; a++, tv++) {
    copy_v3_v3(vec, (tv->flag & TX_VERT_USE_MAPLOC) ? tv->maploc : tv->loc);
    mul_m3_v3(bmat, vec);
    add_v3_v3(vec, obedit->object_to_world().location());
    add_v3_v3(centroid, vec);
    minmax_v3v3_v3(r_min, r_max, vec);
  }

  ED_transverts_free(&tvs);

  return true;
}

/** \} */

}  // namespace blender
