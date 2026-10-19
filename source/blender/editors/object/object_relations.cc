/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_constraint_types.h"
#include "DNA_lattice_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"
#include "DNA_scene_types.h"
#include "DNA_vfont_types.h"

#include "BLI_kdtree.hh"
#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"
#include "BLI_vector_set.hh"

#include "BLT_translation.hh"

#include "BKE_action.hh"
#include "BKE_anim_data.hh"
#include "BKE_armature.hh"
#include "BKE_collection.hh"
#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_displist.h"
#include "BKE_editmesh.hh"
#include "BKE_fcurve.hh"
#include "BKE_idtype.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_override.hh"
#include "BKE_lib_query.hh"
#include "BKE_lib_remap.hh"
#include "BKE_light_linking.h"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh_types.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "ED_armature.hh"
#include "ED_curve.hh"
#include "ED_mesh.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "ANIM_action.hh"
#include "ANIM_animdata.hh"

#include "MOD_nodes.hh"

#include "object_intern.hh"

namespace blender::ed::object {

/* ------------------------------------------------------------------- */
/** \name Clear Track Operator
 * \{ */

enum {
  CLEAR_TRACK = 1,
  CLEAR_TRACK_KEEP_TRANSFORM = 2,
};

static const EnumPropertyItem prop_clear_track_types[] = {
    {CLEAR_TRACK, "CLEAR", 0, "Clear Track", ""},
    {CLEAR_TRACK_KEEP_TRANSFORM,
     "CLEAR_KEEP_TRANSFORM",
     0,
     "Clear and Keep Transformation (Clear Track)",
     ""},
    {0, nullptr, 0, nullptr, nullptr},
};

/* NOTE: poll should check for editable scene. */
static wmOperatorStatus object_track_clear_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  const int type = RNA_enum_get(op->ptr, "type");

  if (CTX_data_edit_object(C)) {
    BKE_report(op->reports, RPT_ERROR, "Operation cannot be performed in edit mode");
    return OPERATOR_CANCELLED;
  }
  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    bConstraint *con, *pcon;

    /* remove track-object for old track */
    ob->track = nullptr;
    DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);

    /* also remove all tracking constraints */
    for (con = static_cast<bConstraint *>(ob->constraints.last); con; con = pcon) {
      pcon = con->prev;
      if (ELEM(con->type,
               CONSTRAINT_TYPE_TRACKTO,
               CONSTRAINT_TYPE_LOCKTRACK,
               CONSTRAINT_TYPE_DAMPTRACK))
      {
        BKE_constraint_remove_ex(&ob->constraints, ob, con);
      }
    }

    if (type == CLEAR_TRACK_KEEP_TRANSFORM) {
      BKE_object_apply_mat4(ob, ob->object_to_world().ptr(), true);
    }
  }
  CTX_DATA_END;

  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_track_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Track";
  ot->description = "Clear tracking constraint or flag from object";
  ot->idname = "OBJECT_OT_track_clear";

  /* API callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_track_clear_exec;

  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "type", prop_clear_track_types, 0, "Type", "");
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Make Track Operator
 * \{ */

enum {
  CREATE_TRACK_DAMPTRACK = 1,
  CREATE_TRACK_TRACKTO = 2,
  CREATE_TRACK_LOCKTRACK = 3,
};

static const EnumPropertyItem prop_make_track_types[] = {
    {CREATE_TRACK_DAMPTRACK, "DAMPTRACK", 0, "Damped Track Constraint", ""},
    {CREATE_TRACK_TRACKTO, "TRACKTO", 0, "Track to Constraint", ""},
    {CREATE_TRACK_LOCKTRACK, "LOCKTRACK", 0, "Lock Track Constraint", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus track_set_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *obact = context_active_object(C);

  const int type = RNA_enum_get(op->ptr, "type");

  switch (type) {
    case CREATE_TRACK_DAMPTRACK: {
      bConstraint *con;
      bDampTrackConstraint *data;

      CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
        if (ob != obact) {
          con = BKE_constraint_add_for_object(ob, "AutoTrack", CONSTRAINT_TYPE_DAMPTRACK);

          data = static_cast<bDampTrackConstraint *>(con->data);
          data->tar = obact;
          DEG_id_tag_update(&ob->id,
                            ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);

          /* Light, Camera and Speaker track differently by default */
          if (ELEM(ob->type, OB_LAMP, OB_CAMERA, OB_SPEAKER)) {
            data->trackflag = TRACK_nZ;
          }
        }
      }
      CTX_DATA_END;
      break;
    }
    case CREATE_TRACK_TRACKTO: {
      bConstraint *con;
      bTrackToConstraint *data;

      CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
        if (ob != obact) {
          con = BKE_constraint_add_for_object(ob, "AutoTrack", CONSTRAINT_TYPE_TRACKTO);

          data = static_cast<bTrackToConstraint *>(con->data);
          data->tar = obact;
          DEG_id_tag_update(&ob->id,
                            ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);

          /* Light, Camera and Speaker track differently by default */
          if (ELEM(ob->type, OB_LAMP, OB_CAMERA, OB_SPEAKER)) {
            data->reserved1 = TRACK_nZ;
            data->reserved2 = UP_Y;
          }
        }
      }
      CTX_DATA_END;
      break;
    }
    case CREATE_TRACK_LOCKTRACK: {
      bConstraint *con;
      bLockTrackConstraint *data;

      CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
        if (ob != obact) {
          con = BKE_constraint_add_for_object(ob, "AutoTrack", CONSTRAINT_TYPE_LOCKTRACK);

          data = static_cast<bLockTrackConstraint *>(con->data);
          data->tar = obact;
          DEG_id_tag_update(&ob->id,
                            ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);

          /* Light, Camera and Speaker track differently by default */
          if (ELEM(ob->type, OB_LAMP, OB_CAMERA, OB_SPEAKER)) {
            data->trackflag = TRACK_nZ;
            data->lockflag = LOCK_Y;
          }
        }
      }
      CTX_DATA_END;
      break;
    }
  }

  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_track_set(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Make Track";
  ot->description = "Make the object track another object, using various methods/constraints";
  ot->idname = "OBJECT_OT_track_set";

  /* API callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = track_set_exec;

  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna, "type", prop_make_track_types, 0, "Type", "");
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Link to Scene Operator
 * \{ */

#if 0
static void link_to_scene(Main * /*bmain*/, ushort /*nr*/)
{
  Scene *sce = (Scene *)BLI_findlink(&bmain->scene, G.curscreen->scenenr - 1);
  Base *base, *nbase;

  if (sce == nullptr) {
    return;
  }
  if (sce->id.lib) {
    return;
  }

  for (base = FIRSTBASE; base; base = base->next) {
    if (BASE_SELECTED(v3d, base)) {
      nbase = MEM_new_uninitialized<Base>("newbase");
      *nbase = *base;
      BLI_addhead(&(sce->base), nbase);
      id_us_plus((ID *)base->object);
    }
  }
}
#endif

static wmOperatorStatus make_links_scene_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene_to = static_cast<Scene *>(
      BLI_findlink(&bmain->scenes, RNA_enum_get(op->ptr, "scene")));

  if (scene_to == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not find scene");
    return OPERATOR_CANCELLED;
  }

  if (scene_to == CTX_data_scene(C)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot link objects into the same scene");
    return OPERATOR_CANCELLED;
  }

  if (!BKE_id_is_editable(bmain, &scene_to->id)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot link objects into a linked scene");
    return OPERATOR_CANCELLED;
  }

  Collection *collection_to = scene_to->master_collection;
  CTX_DATA_BEGIN (C, Base *, base, selected_bases) {
    BKE_collection_object_add(bmain, collection_to, base->object);
  }
  CTX_DATA_END;

  DEG_id_tag_update(&collection_to->id, ID_RECALC_HIERARCHY);

  DEG_relations_tag_update(bmain);

  /* redraw the 3D view because the object center points are colored differently */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, nullptr);

  /* one day multiple scenes will be visible, then we should have some update function for them
   */
  return OPERATOR_FINISHED;
}

enum {
  MAKE_LINKS_OBDATA = 1,
  MAKE_LINKS_MATERIALS = 2,
  MAKE_LINKS_ANIMDATA = 3,
  MAKE_LINKS_GROUP = 4,
  MAKE_LINKS_DUPLICOLLECTION = 5,
  MAKE_LINKS_MODIFIERS = 6,
  MAKE_LINKS_CONSTRAINTS = 7,
  MAKE_LINKS_FONTS = 8,
  MAKE_LINKS_LIGHT_LINKING = 9,
  MAKE_LINKS_SHADOW_LINKING = 10,
};

/* Matches has_geometry_visibility() from Python.
 * Indicates whether an object has shading/visibility settings. */
static bool has_geometry_visibility(const Object &object)
{
  if (ELEM(object.type,
           OB_MESH,
           OB_CURVES_LEGACY,
           OB_SURF,
           OB_FONT,
           OB_MBALL,
           OB_LAMP,
           OB_VOLUME,
           OB_POINTCLOUD,
           OB_CURVES))
  {
    return true;
  }

  if (object.transflag & OB_DUPLICOLLECTION && object.instance_collection) {
    return true;
  }

  return false;
}

/* Return true if make link data is allowed, false otherwise */
static bool allow_make_links_data(const int type, Object *ob_src, Object *ob_dst)
{
  switch (type) {
    case MAKE_LINKS_OBDATA:
      if (ob_src->type == ob_dst->type && ob_src->type != OB_EMPTY) {
        return true;
      }
      break;
    case MAKE_LINKS_MATERIALS:
      if (OB_TYPE_SUPPORT_MATERIAL(ob_src->type) && OB_TYPE_SUPPORT_MATERIAL(ob_dst->type))
      {
        return true;
      }
      break;
    case MAKE_LINKS_DUPLICOLLECTION:
      if (ob_dst->type == OB_EMPTY) {
        return true;
      }
      break;
    case MAKE_LINKS_ANIMDATA:
    case MAKE_LINKS_GROUP:
      return true;
    case MAKE_LINKS_MODIFIERS:
      if (!ELEM(OB_EMPTY, ob_src->type, ob_dst->type)) {
        return true;
      }
      break;
    case MAKE_LINKS_CONSTRAINTS:
      return true;
    case MAKE_LINKS_FONTS:
      if ((ob_src->data != ob_dst->data) && (ob_src->type == OB_FONT) && (ob_dst->type == OB_FONT))
      {
        return true;
      }
      break;
    case MAKE_LINKS_LIGHT_LINKING:
    case MAKE_LINKS_SHADOW_LINKING:
      return has_geometry_visibility(*ob_src) && has_geometry_visibility(*ob_dst);
  }
  return false;
}

static wmOperatorStatus make_links_data_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  Main *bmain = CTX_data_main(C);
  const int type = RNA_enum_get(op->ptr, "type");
  Object *ob_src;
  ID *obdata_id;
  int a;

  /* collection */
  LinkNode *ob_collections = nullptr;
  bool is_cycle = false;
  bool is_lib = false;

  ob_src = context_active_object(C);

  /* avoid searching all collections in source object each time */
  if (type == MAKE_LINKS_GROUP) {
    ob_collections = BKE_object_groups(bmain, scene, ob_src);
  }

  CTX_DATA_BEGIN (C, Base *, base_dst, selected_editable_bases) {
    Object *ob_dst = base_dst->object;

    if (ob_src != ob_dst) {
      if (allow_make_links_data(type, ob_src, ob_dst)) {
        obdata_id = ob_dst->data;

        switch (type) {
          case MAKE_LINKS_OBDATA: /* obdata */
            id_us_min(obdata_id);

            obdata_id = ob_src->data;
            id_us_plus(obdata_id);
            ob_dst->data = obdata_id;

            BKE_sculptsession_free_pbvh(*ob_dst);
            /* if amount of material indices changed: */
            BKE_object_materials_sync_length(bmain, ob_dst, ob_dst->data);

            if (ob_dst->type == OB_ARMATURE) {
              BKE_pose_rebuild(bmain, ob_dst, id_cast<bArmature *>(ob_dst->data), true);
            }
            DEG_id_tag_update(&ob_dst->id, ID_RECALC_GEOMETRY);
            break;
          case MAKE_LINKS_MATERIALS:
            /* new approach, using functions from kernel */
            for (a = 0; a < ob_src->totcol; a++) {
              Material *ma = BKE_object_material_get(ob_src, a + 1);
              /* also works with `ma == nullptr` */
              BKE_object_material_assign(bmain, ob_dst, ma, a + 1, BKE_MAT_ASSIGN_USERPREF);
            }
            DEG_id_tag_update(&ob_dst->id, ID_RECALC_GEOMETRY);
            break;
          case MAKE_LINKS_ANIMDATA:
            BKE_animdata_copy_id(bmain, id_cast<ID *>(ob_dst), id_cast<ID *>(ob_src), 0);
            if (ob_dst->data && ob_src->data) {
              if (BKE_id_is_editable(bmain, obdata_id)) {
                BKE_animdata_copy_id(bmain, ob_dst->data, ob_src->data, 0);
              }
              else {
                is_lib = true;
              }
            }
            DEG_id_tag_update(&ob_dst->id,
                              ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
            break;
          case MAKE_LINKS_GROUP: {
            LinkNode *collection_node;

            /* first clear collections */
            BKE_object_groups_clear(bmain, scene, ob_dst);

            /* now add in the collections from the link nodes */
            for (collection_node = ob_collections; collection_node;
                 collection_node = collection_node->next)
            {
              if (ob_dst->instance_collection != collection_node->link) {
                BKE_collection_object_add(
                    bmain, static_cast<Collection *>(collection_node->link), ob_dst);
              }
              else {
                is_cycle = true;
              }
            }
            break;
          }
          case MAKE_LINKS_DUPLICOLLECTION:
            ob_dst->instance_collection = ob_src->instance_collection;
            if (ob_dst->instance_collection) {
              id_us_plus(&ob_dst->instance_collection->id);
              ob_dst->transflag |= OB_DUPLICOLLECTION;
            }
            DEG_id_tag_update(&ob_dst->id, ID_RECALC_SYNC_TO_EVAL);
            break;
          case MAKE_LINKS_MODIFIERS:
            BKE_object_link_modifiers(ob_dst, ob_src);
            DEG_id_tag_update(&ob_dst->id,
                              ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
            break;
          case MAKE_LINKS_CONSTRAINTS:
            BKE_constraints_free(&ob_dst->constraints);
            BKE_constraints_copy(&ob_dst->constraints, &ob_src->constraints, true);
            DEG_id_tag_update(&ob_dst->id, ID_RECALC_GEOMETRY | ID_RECALC_TRANSFORM);
            WM_event_add_notifier(C, NC_OBJECT | ND_CONSTRAINT, nullptr);
            break;
          case MAKE_LINKS_FONTS: {
            Curve *cu_src = id_cast<Curve *>(ob_src->data);
            Curve *cu_dst = id_cast<Curve *>(ob_dst->data);

            if (!BKE_id_is_editable(bmain, obdata_id)) {
              is_lib = true;
              break;
            }

#define CURVE_VFONT_SET(vfont_member) \
  { \
    if (cu_dst->vfont_member) { \
      id_us_min(&cu_dst->vfont_member->id); \
    } \
    cu_dst->vfont_member = cu_src->vfont_member; \
    id_us_plus((ID *)cu_dst->vfont_member); \
  } \
  ((void)0)

            CURVE_VFONT_SET(vfont);
            CURVE_VFONT_SET(vfontb);
            CURVE_VFONT_SET(vfonti);
            CURVE_VFONT_SET(vfontbi);

#undef CURVE_VFONT_SET

            DEG_id_tag_update(&ob_dst->id,
                              ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
            break;
          }
          case MAKE_LINKS_LIGHT_LINKING:
            BKE_light_linking_copy_receiver_collection(bmain, *ob_dst, *ob_src);
            break;
          case MAKE_LINKS_SHADOW_LINKING:
            BKE_light_linking_copy_blocker_collection(bmain, *ob_dst, *ob_src);
            break;
        }
      }
    }
  }
  CTX_DATA_END;

  if (type == MAKE_LINKS_GROUP) {
    if (ob_collections) {
      BLI_linklist_free(ob_collections, nullptr);
    }

    if (is_cycle) {
      BKE_report(op->reports, RPT_WARNING, "Skipped some collections because of cycle detected");
    }
  }

  if (is_lib) {
    BKE_report(op->reports, RPT_WARNING, "Skipped editing library object data");
  }

  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  WM_event_add_notifier(C, NC_ANIMATION | ND_NLA_ACTCHANGE, CTX_wm_view3d(C));
  WM_event_add_notifier(C, NC_OBJECT, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_make_links_scene(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Link Objects to Scene";
  ot->description = "Link selection to another scene";
  ot->idname = "OBJECT_OT_make_links_scene";

  /* API callbacks. */
  ot->invoke = WM_enum_search_invoke;
  ot->exec = make_links_scene_exec;
  /* better not run the poll check */

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(ot->srna, "scene", rna_enum_dummy_NULL_items, 0, "Scene", "");
  RNA_def_enum_funcs(prop, RNA_scene_local_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  ot->prop = prop;
}

void OBJECT_OT_make_links_data(wmOperatorType *ot)
{
  static const EnumPropertyItem make_links_items[] = {
      {MAKE_LINKS_OBDATA, "OBDATA", 0, "Link Object Data", "Replace assigned Object Data"},
      {MAKE_LINKS_MATERIALS, "MATERIAL", 0, "Link Materials", "Replace assigned Materials"},
      {MAKE_LINKS_ANIMDATA,
       "ANIMATION",
       0,
       "Link Animation Data",
       "Replace assigned Animation Data"},
      {MAKE_LINKS_GROUP, "GROUPS", 0, "Link Collections", "Replace assigned Collections"},
      {MAKE_LINKS_DUPLICOLLECTION,
       "DUPLICOLLECTION",
       0,
       "Link Instance Collection",
       "Replace assigned Collection Instance"},
      {MAKE_LINKS_FONTS, "FONTS", 0, "Link Fonts to Text", "Replace Text object Fonts"},
      RNA_ENUM_ITEM_SEPR,
      {MAKE_LINKS_MODIFIERS, "MODIFIERS", 0, "Copy Modifiers", "Replace Modifiers"},
      {MAKE_LINKS_CONSTRAINTS, "CONSTRAINTS", 0, "Copy Constraints", "Replace Constraints"},
      {MAKE_LINKS_LIGHT_LINKING,
       "LIGHT_LINKING",
       0,
       "Copy Light Linking",
       "Replace assigned Light Linking collection"},
      {MAKE_LINKS_SHADOW_LINKING,
       "SHADOW_LINKING",
       0,
       "Copy Shadow Linking",
       "Replace assigned Shadow Linking collection"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Link/Transfer Data";
  ot->description = "Transfer data from active object to selected objects";
  ot->idname = "OBJECT_OT_make_links_data";

  /* API callbacks. */
  ot->exec = make_links_data_exec;
  /* The object must not be in edit-mode because multiple objects in edit-mode
   * sharing data is an invalid state which can cause crashes. See: #160956. */
  ot->poll = ED_operator_object_active_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna, "type", make_links_items, 0, "Type", "");
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Make Single User Operator
 * \{ */

static bool single_data_needs_duplication(ID *id)
{
  /* NOTE: When dealing with linked data, we always make a local copy of it.
   * While in theory we could rather make it local when it only has one user, this is difficult
   * in practice with current code of this function. */
  return (id != nullptr && (id->us > 1 || ID_IS_LINKED(id)));
}

static void libblock_relink_collection(Main *bmain,
                                       Collection *collection,
                                       const bool do_collection)
{
  if (do_collection) {
    BKE_libblock_relink_to_newid(bmain, &collection->id, 0);
  }

  for (CollectionObject *cob = static_cast<CollectionObject *>(collection->gobject.first);
       cob != nullptr;
       cob = cob->next)
  {
    BKE_libblock_relink_to_newid(bmain, &cob->ob->id, 0);
  }

  for (CollectionChild &child : collection->children) {
    libblock_relink_collection(bmain, child.collection, true);
  }
}

static Collection *single_object_users_collection(Main *bmain,
                                                  Scene *scene,
                                                  Collection *collection,
                                                  const int flag,
                                                  const bool copy_collections,
                                                  const bool is_master_collection)
{
  /* Generate new copies for objects in given collection and all its children,    * and
   * optionally also copy collections themselves. */
  if (copy_collections && !is_master_collection) {
    Collection *collection_new = id_cast<Collection *>(BKE_id_copy_ex(
        bmain, &collection->id, nullptr, LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
    id_us_min(&collection_new->id);
    collection = id_cast<Collection *>(ID_NEW_SET(collection, collection_new));
  }

  /* We do not remap to new objects here, this is done in separate step. */
  for (CollectionObject &cob : collection->gobject) {
    Object *ob = cob.ob;
    /* an object may be in more than one collection */
    if ((ob->id.newid == nullptr) && ((ob->flag & flag) == flag)) {
      if (!ID_IS_LINKED(ob) && BKE_object_scenes_users_get(bmain, ob) > 1) {
        ID_NEW_SET(
            ob,
            BKE_id_copy_ex(bmain, &ob->id, nullptr, LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
        id_us_min(ob->id.newid);
      }
    }
  }

  /* Since master collection has already be duplicated as part of scene copy,    * we do not
   * duplicate it here. However, this means its children need to be re-added manually here,    *
   * otherwise their parent lists are empty (which will lead to crashes, see #63101). */
  CollectionChild *child_next, *child = static_cast<CollectionChild *>(collection->children.first);
  CollectionChild *orig_child_last = static_cast<CollectionChild *>(collection->children.last);
  for (; child != nullptr; child = child_next) {
    child_next = child->next;
    Collection *collection_child_new = single_object_users_collection(
        bmain, scene, child->collection, flag, copy_collections, false);

    if (is_master_collection && copy_collections && child->collection != collection_child_new) {
      /* We do not want a collection sync here, our collections are in a complete uninitialized
       * state currently. With current code, that would lead to a memory leak - because of
       * reasons. It would be a useless loss of computing anyway, since caller has to fully
       * refresh view-layers/collections caching at the end. */
      BKE_collection_child_add_no_sync(bmain, collection, collection_child_new);
      BLI_remlink(&collection->children, child);
      MEM_delete(child);
      if (child == orig_child_last) {
        break;
      }
    }
  }

  return collection;
}

/* Warning, sets ID->newid pointers of objects and collections, but does not clear them. */
static void single_object_users(
    Main *bmain, Scene *scene, View3D *v3d, const int flag, const bool copy_collections)
{
  /* duplicate all the objects of the scene (and matching collections, if required). */
  Collection *master_collection = scene->master_collection;
  single_object_users_collection(bmain, scene, master_collection, flag, copy_collections, true);

  /* Will also handle the master collection. */
  BKE_libblock_relink_to_newid(bmain, &scene->id, 0);

  /* Collection and object pointers in collections */
  libblock_relink_collection(bmain, scene->master_collection, false);

  /* We also have to handle runtime things in UI. */
  if (v3d) {
    ID_NEW_REMAP(v3d->camera);
  }

  /* Making single user may affect other scenes if they share
   * with current one some collections in their ViewLayer. */
  BKE_main_collection_sync_remap(bmain);
}

void object_single_user_make(Main *bmain, Scene *scene, Object *ob)
{
  FOREACH_SCENE_OBJECT_BEGIN (scene, ob_iter) {
    ob_iter->flag &= ~OB_DONE;
  }
  FOREACH_SCENE_OBJECT_END;

  /* tag only the one object */
  ob->flag |= OB_DONE;

  single_object_users(bmain, scene, nullptr, OB_DONE, false);
  BKE_main_id_newptr_and_tag_clear(bmain);
}

static void single_obdata_users(
    Main *bmain, Scene *scene, ViewLayer *view_layer, View3D *v3d, const int flag)
{
  ID *id;

  FOREACH_OBJECT_FLAG_BEGIN (bmain, scene, view_layer, v3d, flag, ob) {
    if (BKE_id_is_editable(bmain, &ob->id)) {
      id = ob->data;
      if (single_data_needs_duplication(id)) {
        DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);

        switch (ob->type) {
          case OB_EMPTY:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_LAMP:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_CAMERA: {
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            Camera *cam = id_cast<Camera *>(ob->data);
            ID_NEW_REMAP(cam->dof.focus_object);
            break;
          }
          case OB_MESH:
            /* Needed to remap texcomesh below. */
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_MBALL:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_CURVES_LEGACY:
          case OB_SURF:
          case OB_FONT: {
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            Curve *cu = id_cast<Curve *>(ob->data);
            ID_NEW_REMAP(cu->bevobj);
            ID_NEW_REMAP(cu->taperobj);
            break;
          }
          case OB_LATTICE:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_ARMATURE:
            DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            BKE_pose_rebuild(bmain, ob, id_cast<bArmature *>(ob->data), true);
            break;
          case OB_SPEAKER:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_LIGHTPROBE:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_CURVES:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_POINTCLOUD:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          case OB_VOLUME:
            ob->data = ID_NEW_SET(ob->data,
                                  BKE_id_copy_ex(bmain,
                                                 static_cast<const ID *>(ob->data),
                                                 nullptr,
                                                 LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
            break;
          default:
            printf("ERROR %s: cannot copy %s\n", __func__, id->name);
            BLI_assert_msg(0, "This should never happen.");

            /* We need to end the FOREACH_OBJECT_FLAG_BEGIN iterator to prevent memory leak. */
            BKE_scene_objects_iterator_end(&iter_macro);
            return;
        }

        id_us_min(id);
      }
    }
  }
  FOREACH_OBJECT_FLAG_END;

  Mesh *mesh = static_cast<Mesh *>(bmain->meshes.first);
  while (mesh) {
    ID_NEW_REMAP(mesh->texcomesh);
    mesh = static_cast<Mesh *>(mesh->id.next);
  }
}

void single_obdata_user_make(Main *bmain, Scene *scene, Object *ob)
{
  FOREACH_SCENE_OBJECT_BEGIN (scene, ob_iter) {
    ob_iter->flag &= ~OB_DONE;
  }
  FOREACH_SCENE_OBJECT_END;

  /* Tag only the one object. */
  ob->flag |= OB_DONE;

  single_obdata_users(bmain, scene, nullptr, nullptr, OB_DONE);
}

static void single_object_action_users(
    Main *bmain, Scene *scene, ViewLayer *view_layer, View3D *v3d, const int flag)
{
  FOREACH_OBJECT_FLAG_BEGIN (bmain, scene, view_layer, v3d, flag, ob) {
    if (BKE_id_is_editable(bmain, &ob->id)) {
      AnimData *adt = BKE_animdata_from_id(&ob->id);
      if (adt == nullptr) {
        continue;
      }

      ID *id_act = id_cast<ID *>(adt->action);
      if (single_data_needs_duplication(id_act)) {
        DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
        BKE_animdata_duplicate_id_action(bmain, &ob->id, USER_DUP_ACT | USER_DUP_LINKED_ID);
      }
    }
  }
  FOREACH_OBJECT_FLAG_END;
}

static void single_objectdata_action_users(
    Main *bmain, Scene *scene, ViewLayer *view_layer, View3D *v3d, const int flag)
{
  FOREACH_OBJECT_FLAG_BEGIN (bmain, scene, view_layer, v3d, flag, ob) {
    if (BKE_id_is_editable(bmain, &ob->id) && ob->data != nullptr) {
      ID *id_obdata = ob->data;
      AnimData *adt = BKE_animdata_from_id(id_obdata);
      if (adt == nullptr) {
        continue;
      }

      ID *id_act = id_cast<ID *>(adt->action);
      if (single_data_needs_duplication(id_act)) {
        DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
        BKE_animdata_duplicate_id_action(bmain, id_obdata, USER_DUP_ACT | USER_DUP_LINKED_ID);
      }
    }
  }
  FOREACH_OBJECT_FLAG_END;
}

static void single_mat_users(
    Main *bmain, Scene *scene, ViewLayer *view_layer, View3D *v3d, const int flag)
{
  Material *ma, *man;
  int a;

  FOREACH_OBJECT_FLAG_BEGIN (bmain, scene, view_layer, v3d, flag, ob) {
    if (BKE_id_is_editable(bmain, &ob->id)) {
      const bool is_obdata_editable = ob->data ? BKE_id_is_editable(bmain, ob->data) : false;
      for (a = 1; a <= ob->totcol; a++) {
        if (!ob->matbits[a - 1] && !is_obdata_editable) {
          /* Cannot edit material usage of obdata if it's not editable (e.g. local object using a
           * linked mesh). See also #161211. */
          continue;
        }
        ma = BKE_object_material_get(ob, short(a));
        if (single_data_needs_duplication(&ma->id)) {
          man = id_cast<Material *>(
              BKE_id_copy_ex(bmain, &ma->id, nullptr, LIB_ID_COPY_DEFAULT | LIB_ID_COPY_ACTIONS));
          man->id.us = 0;
          BKE_object_material_assign(bmain, ob, man, short(a), BKE_MAT_ASSIGN_EXISTING);
        }
      }
    }
  }
  FOREACH_OBJECT_FLAG_END;
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Make Local Operator
 * \{ */

enum {
  MAKE_LOCAL_SELECT_OB = 1,
  MAKE_LOCAL_SELECT_OBDATA = 2,
  MAKE_LOCAL_SELECT_OBDATA_MATERIAL = 3,
  MAKE_LOCAL_ALL = 4,
};

static int tag_localizable_looper(LibraryIDLinkCallbackData *cb_data)
{
  ID **id_pointer = cb_data->id_pointer;
  if (*id_pointer) {
    (*id_pointer)->tag &= ~ID_TAG_DOIT;
  }

  return IDWALK_RET_NOP;
}

static void tag_localizable_objects(bContext *C, const int mode)
{
  Main *bmain = CTX_data_main(C);

  BKE_main_id_tag_all(bmain, ID_TAG_DOIT, false);

  /* Set ID_TAG_DOIT flag for all selected objects, so next we can check whether
   * object is gonna to become local or not.
   */
  CTX_DATA_BEGIN (C, Object *, object, selected_objects) {
    object->id.tag |= ID_TAG_DOIT;

    /* If obdata is also going to become local, mark it as such too. */
    if (mode == MAKE_LOCAL_SELECT_OBDATA && object->data) {
      ID *data_id = object->data;
      data_id->tag |= ID_TAG_DOIT;
    }
  }
  CTX_DATA_END;

  /* Also forbid making objects local if other library objects are using
   * them for modifiers or constraints.
   *
   * FIXME This is ignoring all other linked ID types potentially using the selected tagged
   * objects! Probably works fine in most 'usual' cases though.
   */
  for (Object *object = static_cast<Object *>(bmain->objects.first); object;
       object = static_cast<Object *>(object->id.next))
  {
    if ((object->id.tag & ID_TAG_DOIT) == 0 && ID_IS_LINKED(object)) {
      BKE_library_foreach_ID_link(
          nullptr, &object->id, tag_localizable_looper, nullptr, IDWALK_READONLY);
    }
    if (object->data) {
      ID *data_id = object->data;
      if ((data_id->tag & ID_TAG_DOIT) == 0 && ID_IS_LINKED(data_id)) {
        BKE_library_foreach_ID_link(
            nullptr, data_id, tag_localizable_looper, nullptr, IDWALK_READONLY);
      }
    }
  }

  /* TODO(sergey): Drivers targets? */
}

/**
 * Instance indirectly referenced zero user objects,  * otherwise they're lost on reload, see
 * #40595.
 */
static bool make_local_all__instance_indirect_unused(Main *bmain,
                                                     const Scene *scene,
                                                     ViewLayer *view_layer,
                                                     Collection *collection)
{
  Object *ob;
  bool changed = false;

  for (ob = static_cast<Object *>(bmain->objects.first); ob;
       ob = static_cast<Object *>(ob->id.next))
  {
    if (ID_IS_LINKED(ob) && (ob->id.us == 0)) {
      Base *base;

      id_us_plus(&ob->id);

      BKE_collection_object_add(bmain, collection, ob);
      BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
      base = BKE_view_layer_base_find(view_layer, ob);
      base_select(base, BA_SELECT);
      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);

      changed = true;
    }
  }

  return changed;
}

static void make_local_animdata_tag_strips(ListBaseT<NlaStrip> *strips)
{
  for (NlaStrip &strip : *strips) {
    if (strip.act) {
      strip.act->id.tag &= ~ID_TAG_PRE_EXISTING;
    }

    make_local_animdata_tag_strips(&strip.strips);
  }
}

/* Tag all actions used by given animdata to be made local. */
static void make_local_animdata_tag(AnimData *adt)
{
  if (adt) {
    /* Actions - Active and Temp */
    if (adt->action) {
      adt->action->id.tag &= ~ID_TAG_PRE_EXISTING;
    }
    if (adt->tmpact) {
      adt->tmpact->id.tag &= ~ID_TAG_PRE_EXISTING;
    }

    /* Drivers */
    /* TODO: need to handle the ID-targets too? */

    /* NLA Data */
    for (NlaTrack &nlt : adt->nla_tracks) {
      make_local_animdata_tag_strips(&nlt.strips);
    }
  }
}

static void make_local_material_tag(Material *ma)
{
  if (ma) {
    ma->id.tag &= ~ID_TAG_PRE_EXISTING;
    make_local_animdata_tag(BKE_animdata_from_id(&ma->id));

    /* About node-trees: root one is made local together with material,
     * others we keep linked (for now). */
  }
}

static wmOperatorStatus make_local_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Material *ma, ***matarar;
  const int mode = RNA_enum_get(op->ptr, "type");
  int a;

  /* NOTE: we (ab)use ID_TAG_PRE_EXISTING to cherry pick which ID to make local... */
  if (mode == MAKE_LOCAL_ALL) {
    const Scene *scene = CTX_data_scene(C);
    ViewLayer *view_layer = CTX_data_view_layer(C);
    Collection *collection = CTX_data_collection(C);

    BKE_main_id_tag_all(bmain, ID_TAG_PRE_EXISTING, false);

    /* De-select so the user can differentiate newly instanced from existing objects. */
    BKE_view_layer_base_deselect_all(*bmain, scene, view_layer);

    if (make_local_all__instance_indirect_unused(bmain, scene, view_layer, collection)) {
      BKE_report(op->reports,
                 RPT_INFO,
                 "Orphan library objects added to the current scene to avoid loss");
    }
  }
  else {
    BKE_main_id_tag_all(bmain, ID_TAG_PRE_EXISTING, true);
    tag_localizable_objects(C, mode);

    CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
      if ((ob->id.tag & ID_TAG_DOIT) == 0) {
        continue;
      }

      ob->id.tag &= ~ID_TAG_PRE_EXISTING;
      make_local_animdata_tag(BKE_animdata_from_id(&ob->id));
      for (ParticleSystem &psys : ob->particlesystem) {
        psys.part->id.tag &= ~ID_TAG_PRE_EXISTING;
      }

      if (mode == MAKE_LOCAL_SELECT_OBDATA_MATERIAL) {
        for (a = 0; a < ob->totcol; a++) {
          ma = ob->mat[a];
          if (ma) {
            make_local_material_tag(ma);
          }
        }

        matarar = BKE_object_material_array_p(ob);
        if (matarar) {
          for (a = 0; a < ob->totcol; a++) {
            ma = (*matarar)[a];
            if (ma) {
              make_local_material_tag(ma);
            }
          }
        }
      }

      if (ELEM(mode, MAKE_LOCAL_SELECT_OBDATA, MAKE_LOCAL_SELECT_OBDATA_MATERIAL) &&
          ob->data != nullptr)
      {
        ID *ob_data = ob->data;
        ob_data->tag &= ~ID_TAG_PRE_EXISTING;
        make_local_animdata_tag(BKE_animdata_from_id(ob_data));
      }
    }
    CTX_DATA_END;
  }

  BKE_library_make_local(
      bmain, nullptr, nullptr, true, false, true); /* nullptr is all libraries. */

  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

void OBJECT_OT_make_local(wmOperatorType *ot)
{
  static const EnumPropertyItem type_items[] = {
      {MAKE_LOCAL_SELECT_OB, "SELECT_OBJECT", 0, "Selected Objects", ""},
      {MAKE_LOCAL_SELECT_OBDATA, "SELECT_OBDATA", 0, "Selected Objects and Data", ""},
      {MAKE_LOCAL_SELECT_OBDATA_MATERIAL,
       "SELECT_OBDATA_MATERIAL",
       0,
       "Selected Objects, Data and Materials",
       ""},
      {MAKE_LOCAL_ALL, "ALL", 0, "All", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Make Local";
  ot->description = "Make library linked data-blocks local to this file";
  ot->idname = "OBJECT_OT_make_local";

  /* API callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = make_local_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna, "type", type_items, 0, "Type", "");
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Make Library Override Operator
 * \{ */

static bool make_override_library_object_overridable_check(Main *bmain, Object *object)
{
  /* An object is actually overridable only if it is in at least one local collection.
   * Unfortunately 'direct link' flag is not enough here. */
  for (Collection &collection : bmain->collections) {
    if (!ID_IS_LINKED(&collection) && BKE_collection_has_object(&collection, object)) {
      return true;
    }
  }
  for (Scene &scene : bmain->scenes) {
    if (!ID_IS_LINKED(&scene) && BKE_collection_has_object(scene.master_collection, object)) {
      return true;
    }
  }
  return false;
}

static wmOperatorStatus make_override_library_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *obact = CTX_data_active_object(C);
  ID *id_root = nullptr;
  bool is_override_instancing_object = false;

  bool user_overrides_from_selected_objects = false;

  if (!ID_IS_LINKED(obact) && obact->instance_collection != nullptr &&
      ID_IS_LINKED(obact->instance_collection))
  {
    if (!ID_IS_OVERRIDABLE_LIBRARY(obact->instance_collection)) {
      BKE_reportf(op->reports,
                  RPT_ERROR_INVALID_INPUT,
                  "Collection '%s' (instantiated by the active object) is not overridable",
                  obact->instance_collection->id.name + 2);
      return OPERATOR_CANCELLED;
    }

    id_root = &obact->instance_collection->id;
    is_override_instancing_object = true;
    user_overrides_from_selected_objects = false;
  }
  else if (!make_override_library_object_overridable_check(bmain, obact)) {
    const int i = RNA_property_int_get(op->ptr, op->type->prop);
    const uint collection_session_uid = *(reinterpret_cast<const uint *>(&i));
    if (collection_session_uid == MAIN_ID_SESSION_UID_UNSET) {
      BKE_reportf(op->reports,
                  RPT_ERROR_INVALID_INPUT,
                  "Could not find an overridable root hierarchy for object '%s'",
                  obact->id.name + 2);
      return OPERATOR_CANCELLED;
    }
    Collection *collection = static_cast<Collection *>(
        BLI_listbase_bytes_find(&bmain->collections,
                                &collection_session_uid,
                                sizeof(collection_session_uid),
                                offsetof(ID, session_uid)));
    id_root = &collection->id;
    user_overrides_from_selected_objects = true;
  }
  /* Else, poll func ensures us that ID_IS_LINKED(obact) is true, or that it is already an
   * existing liboverride. */
  else {
    BLI_assert(ID_IS_LINKED(obact) || ID_IS_OVERRIDE_LIBRARY_REAL(obact));
    id_root = &obact->id;
    user_overrides_from_selected_objects = true;
  }

  /* Make already existing selected liboverrides editable. */
  bool is_active_override = false;
  FOREACH_SELECTED_OBJECT_BEGIN (view_layer, CTX_wm_view3d(C), ob_iter) {
    if (ID_IS_OVERRIDE_LIBRARY_REAL(ob_iter) && !ID_IS_LINKED(ob_iter)) {
      ob_iter->id.override_library->flag &= ~LIBOVERRIDE_FLAG_SYSTEM_DEFINED;
      is_active_override = is_active_override || (&ob_iter->id == id_root);
      DEG_id_tag_update(&ob_iter->id, ID_RECALC_SYNC_TO_EVAL);
    }
  }
  FOREACH_SELECTED_OBJECT_END;
  /* If the active object is a liboverride, there is no point going further, since in the weird
   * case where some other selected objects would be linked ones, there is no way to properly
   * create overrides for them currently.
   *
   * Could be added later if really needed, but would rather avoid that extra complexity here. */
  if (is_active_override) {
    return OPERATOR_FINISHED;
  }

  /** Currently there is no 'all editable' option from the 3DView. */
  const bool do_fully_editable = false;

  std::unique_ptr<Set<uint32_t>> user_overrides_objects_uids =
      do_fully_editable ? nullptr : std::make_unique<Set<uint32_t>>();

  if (do_fully_editable) {
    /* Pass. */
  }
  else if (user_overrides_from_selected_objects) {
    /* Only selected objects can be 'user overrides'. */
    FOREACH_SELECTED_OBJECT_BEGIN (view_layer, CTX_wm_view3d(C), ob_iter) {
      user_overrides_objects_uids->add(ob_iter->id.session_uid);
    }
    FOREACH_SELECTED_OBJECT_END;
  }
  else {
    /* Only armatures inside the root collection (and their children) can be 'user overrides'. */
    FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN ((Collection *)id_root, ob_iter) {
      if (ob_iter->type == OB_ARMATURE) {
        user_overrides_objects_uids->add(ob_iter->id.session_uid);
      }
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  }

  BKE_main_id_tag_all(bmain, ID_TAG_DOIT, false);

  /* For the time being, replace selected linked objects by their overrides in all collections.
   * While this may not be the absolute best behavior in all cases, in most common one this
   * should match the expected result. */
  if (user_overrides_objects_uids != nullptr) {
    for (Collection &coll_iter : bmain->collections) {
      if (ID_IS_LINKED(&coll_iter)) {
        continue;
      }
      for (CollectionObject &coll_ob_iter : coll_iter.gobject) {
        if (user_overrides_objects_uids->contains(coll_ob_iter.ob->id.session_uid)) {
          /* Tag for remapping when creating overrides. */
          coll_iter.id.tag |= ID_TAG_DOIT;
          break;
        }
      }
    }
    /* Also tag the Scene itself for remapping when creating overrides (includes the scene's master
     * collection too). */
    scene->id.tag |= ID_TAG_DOIT;
  }

  ID *id_root_override;
  const bool success = BKE_lib_override_library_create(bmain,
                                                       scene,
                                                       view_layer,
                                                       nullptr,
                                                       id_root,
                                                       id_root,
                                                       &obact->id,
                                                       &id_root_override,
                                                       do_fully_editable);

  if (!do_fully_editable) {
    /* Define liboverrides from selected/validated objects as user defined. */
    ID *id_hierarchy_root_override = id_root_override->override_library->hierarchy_root;
    ID *id_iter;
    FOREACH_MAIN_ID_BEGIN (bmain, id_iter) {
      if (ID_IS_LINKED(id_iter) || !ID_IS_OVERRIDE_LIBRARY_REAL(id_iter) ||
          id_iter->override_library->hierarchy_root != id_hierarchy_root_override)
      {
        continue;
      }
      if (user_overrides_objects_uids->contains(id_iter->override_library->reference->session_uid))
      {
        id_iter->override_library->flag &= ~LIBOVERRIDE_FLAG_SYSTEM_DEFINED;
      }
    }
    FOREACH_MAIN_ID_END;
  }

  if (success) {
    if (is_override_instancing_object) {
      /* Remove the instance empty from this scene, the items now have an overridden collection
       * instead. */
      base_free_and_unlink(bmain, scene, obact);
    }
    else {
      /* Remove the found root ID from the view layer. */
      switch (GS(id_root->name)) {
        case ID_GR: {
          Collection *collection_root = id_cast<Collection *>(id_root);
          for (CollectionParent &collection_parent :
               collection_root->runtime->parents.items_mutable())
          {
            if (ID_IS_LINKED(collection_parent.collection) ||
                !BKE_view_layer_has_collection(view_layer, collection_parent.collection))
            {
              continue;
            }
            BKE_collection_child_remove(bmain, collection_parent.collection, collection_root);
          }
          break;
        }
        case ID_OB: {
          /* TODO: Not sure how well we can handle this case, when we don't have the collections
           * as reference containers... */
          break;
        }
        default:
          break;
      }
    }
  }

  DEG_id_tag_update(&CTX_data_scene(C)->id, ID_RECALC_BASE_FLAGS | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  WM_event_add_notifier(C, NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  return success ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

/* Set the object to override. */
static wmOperatorStatus make_override_library_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent * /*event*/)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *obact = context_active_object(C);

  /* Sanity checks. */
  if (!scene || ID_IS_LINKED(scene) || !obact) {
    return OPERATOR_CANCELLED;
  }

  if ((!ID_IS_LINKED(obact) && obact->instance_collection != nullptr &&
       ID_IS_OVERRIDABLE_LIBRARY(obact->instance_collection)) ||
      make_override_library_object_overridable_check(bmain, obact))
  {
    return make_override_library_exec(C, op);
  }

  if (!ID_IS_LINKED(obact)) {
    if (ID_IS_OVERRIDE_LIBRARY_REAL(obact)) {
      return make_override_library_exec(C, op);
    }
    BKE_report(op->reports, RPT_ERROR, "Cannot make library override from a local object");
    return OPERATOR_CANCELLED;
  }

  VectorSet<Collection *> potential_root_collections;
  for (Collection &collection : bmain->collections) {
    /* Only check for linked collections from the same library, in the current view-layer. */
    if (!ID_IS_LINKED(&collection.id) || collection.id.lib != obact->id.lib ||
        !BKE_view_layer_has_collection(view_layer, &collection))
    {
      continue;
    }
    if (!BKE_collection_has_object_recursive(&collection, obact)) {
      continue;
    }
    if (potential_root_collections.is_empty()) {
      potential_root_collections.add_new(&collection);
    }
    else {
      bool has_parents_in_potential_roots = false;
      bool is_potential_root = false;
      for (auto *collection_root_iter : potential_root_collections) {
        if (BKE_collection_has_collection(collection_root_iter, &collection)) {
          BLI_assert_msg(!BKE_collection_has_collection(&collection, collection_root_iter),
                         "Invalid loop in collection hierarchy");
          /* Current potential root is already 'better' (higher up in the collection hierarchy)
           * than current collection, nothing else to do. */
          has_parents_in_potential_roots = true;
        }
        else if (BKE_collection_has_collection(&collection, collection_root_iter)) {
          BLI_assert_msg(!BKE_collection_has_collection(collection_root_iter, &collection),
                         "Invalid loop in collection hierarchy");
          /* Current potential root is in the current collection's hierarchy, so the later is a
           * better candidate as root collection. */
          is_potential_root = true;
          potential_root_collections.remove(collection_root_iter);
        }
        else {
          /* Current potential root is not found in current collection's hierarchy, so the later
           * is a potential candidate as root collection. */
          is_potential_root = true;
        }
      }
      /* Only add the current collection as potential root if it is not a descendant of any
       * already known potential root collections. */
      if (is_potential_root && !has_parents_in_potential_roots) {
        potential_root_collections.add_new(&collection);
      }
    }
  }

  if (potential_root_collections.is_empty()) {
    RNA_property_int_set(op->ptr, op->type->prop, MAIN_ID_SESSION_UID_UNSET);
    return make_override_library_exec(C, op);
  }
  if (potential_root_collections.size() == 1) {
    Collection *collection_root = potential_root_collections.pop();
    RNA_property_int_set(
        op->ptr, op->type->prop, *(reinterpret_cast<int *>(&collection_root->id.session_uid)));
    return make_override_library_exec(C, op);
  }

  BKE_reportf(op->reports,
              RPT_ERROR,
              "Too many potential root collections (%d) for the override hierarchy, "
              "please use the Outliner instead",
              int(potential_root_collections.size()));
  return OPERATOR_CANCELLED;
}

static bool make_override_library_poll(bContext *C)
{
  Base *base_act = CTX_data_active_base(C);
  /* If the active object is not selected, do nothing (operators rely on selection too, they will
   * misbehave if the active object is not also selected, see e.g. #120701). */
  if ((base_act == nullptr) || ((base_act->flag & BASE_SELECTED) == 0)) {
    return false;
  }

  /* Object must be directly linked to be overridable. */
  Object *obact = base_act->object;
  return (
      ED_operator_objectmode(C) && obact != nullptr &&
      (ID_IS_LINKED(obact) || ID_IS_OVERRIDE_LIBRARY(obact) ||
       (obact->instance_collection != nullptr &&
        ID_IS_OVERRIDABLE_LIBRARY(obact->instance_collection) && !ID_IS_OVERRIDE_LIBRARY(obact))));
}

void OBJECT_OT_make_override_library(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Make Library Override";
  ot->description =
      "Create a local override of the selected linked objects, and their hierarchy of "
      "dependencies";
  ot->idname = "OBJECT_OT_make_override_library";

  /* API callbacks. */
  ot->invoke = make_override_library_invoke;
  ot->exec = make_override_library_exec;
  ot->poll = make_override_library_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  PropertyRNA *prop;
  prop = RNA_def_int(ot->srna,
                     "collection",
                     MAIN_ID_SESSION_UID_UNSET,
                     INT_MIN,
                     INT_MAX,
                     "Override Collection",
                     "Session UID of the directly linked collection containing the selected "
                     "object, to make an override from",
                     INT_MIN,
                     INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  ot->prop = prop;
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Reset Library Override Operator
 * \{ */

static bool reset_clear_override_library_poll(bContext *C)
{
  Base *base_act = CTX_data_active_base(C);
  /* If the active object is not selected, do nothing (operators rely on selection too, they will
   * misbehave if the active object is not also selected, see e.g. #120701). */
  if ((base_act == nullptr) || ((base_act->flag & BASE_SELECTED) == 0)) {
    return false;
  }

  /* Object must be local and an override. */
  Object *obact = base_act->object;
  return (ED_operator_objectmode(C) && obact != nullptr && !ID_IS_LINKED(obact) &&
          ID_IS_OVERRIDE_LIBRARY(obact));
}

static wmOperatorStatus reset_override_library_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);

  /* Reset all selected liboverrides. */
  FOREACH_SELECTED_OBJECT_BEGIN (CTX_data_view_layer(C), CTX_wm_view3d(C), ob_iter) {
    if (ID_IS_OVERRIDE_LIBRARY_REAL(ob_iter) && !ID_IS_LINKED(ob_iter)) {
      BKE_lib_override_library_id_reset(bmain, &ob_iter->id, false);
    }
  }
  FOREACH_SELECTED_OBJECT_END;

  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  WM_event_add_notifier(C, NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_reset_override_library(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reset Library Override";
  ot->description = "Reset the selected local overrides to their linked references values";
  ot->idname = "OBJECT_OT_reset_override_library";

  /* API callbacks. */
  ot->exec = reset_override_library_exec;
  ot->poll = reset_clear_override_library_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Clear Library Override Operator
 * \{ */

static wmOperatorStatus clear_override_library_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Scene *scene = CTX_data_scene(C);
  LinkNode *todo_objects = nullptr, *todo_object_iter;

  /* Make already existing selected liboverrides editable. */
  FOREACH_SELECTED_OBJECT_BEGIN (view_layer, CTX_wm_view3d(C), ob_iter) {
    if (ID_IS_LINKED(ob_iter)) {
      continue;
    }
    BLI_linklist_prepend_alloca(&todo_objects, ob_iter);
  }
  FOREACH_SELECTED_OBJECT_END;

  for (todo_object_iter = todo_objects; todo_object_iter != nullptr;
       todo_object_iter = todo_object_iter->next)
  {
    Object *ob_iter = static_cast<Object *>(todo_object_iter->link);
    if (BKE_lib_override_library_is_hierarchy_leaf(bmain, &ob_iter->id)) {
      bool do_remap_active = false;
      BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
      if (BKE_view_layer_active_object_get(view_layer) == ob_iter) {
        do_remap_active = true;
      }
      BKE_libblock_remap(bmain,
                         &ob_iter->id,
                         ob_iter->id.override_library->reference,
                         ID_REMAP_SKIP_INDIRECT_USAGE);
      if (do_remap_active) {
        BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
        Object *ref_object = id_cast<Object *>(ob_iter->id.override_library->reference);
        Base *basact = BKE_view_layer_base_find(view_layer, ref_object);
        if (basact != nullptr) {
          view_layer->basact = basact;
        }
        DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
      }
      BKE_id_delete(bmain, &ob_iter->id);
    }
    else {
      BKE_lib_override_library_id_reset(bmain, &ob_iter->id, true);
    }
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  WM_event_add_notifier(C, NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_clear_override_library(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Library Override";
  ot->description =
      "Delete the selected local overrides and relink their usages to the linked data-blocks if "
      "possible, else reset them and mark them as non editable";
  ot->idname = "OBJECT_OT_clear_override_library";

  /* API callbacks. */
  ot->exec = clear_override_library_exec;
  ot->poll = reset_clear_override_library_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Make Single User Operator
 * \{ */

enum {
  MAKE_SINGLE_USER_ALL = 1,
  MAKE_SINGLE_USER_SELECTED = 2,
};

static wmOperatorStatus make_single_user_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C); /* ok if this is nullptr */
  const int flag = (RNA_enum_get(op->ptr, "type") == MAKE_SINGLE_USER_SELECTED) ? SELECT : 0;
  const bool copy_collections = false;
  bool update_deps = false;

  if (RNA_boolean_get(op->ptr, "object")) {
    if (flag == SELECT) {
      BKE_view_layer_selected_objects_tag(*bmain, scene, view_layer, OB_DONE);
      single_object_users(bmain, scene, v3d, OB_DONE, copy_collections);
    }
    else {
      single_object_users(bmain, scene, v3d, 0, copy_collections);
    }

    /* needed since object relationships may have changed */
    update_deps = true;
  }

  if (RNA_boolean_get(op->ptr, "obdata")) {
    single_obdata_users(bmain, scene, view_layer, v3d, flag);

    /* Needed since some IDs were remapped? (incl. mesh->texcomesh, see #73797). */
    update_deps = true;
  }

  if (RNA_boolean_get(op->ptr, "material")) {
    single_mat_users(bmain, scene, view_layer, v3d, flag);
  }

  if (RNA_boolean_get(op->ptr, "animation")) {
    single_object_action_users(bmain, scene, view_layer, v3d, flag);
  }

  if (RNA_boolean_get(op->ptr, "obdata_animation")) {
    single_objectdata_action_users(bmain, scene, view_layer, v3d, flag);
  }

  BKE_main_id_newptr_and_tag_clear(bmain);

  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (update_deps) {
    DEG_relations_tag_update(bmain);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus make_single_user_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return WM_operator_props_popup_confirm_ex(
      C, op, event, IFACE_("Make Selected Objects Single-User"), IFACE_("Make Single"));
}

void OBJECT_OT_make_single_user(wmOperatorType *ot)
{
  static const EnumPropertyItem type_items[] = {
      {MAKE_SINGLE_USER_SELECTED, "SELECTED_OBJECTS", 0, "Selected Objects", ""},
      {MAKE_SINGLE_USER_ALL, "ALL", 0, "All", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Make Single User";
  ot->description = "Make linked data local to each object";
  ot->idname = "OBJECT_OT_make_single_user";

  /* Note that the invoke callback is only used from operator search,    * otherwise this does
   * nothing by default. */

  /* API callbacks. */
  ot->invoke = make_single_user_invoke;
  ot->exec = make_single_user_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna, "type", type_items, MAKE_SINGLE_USER_SELECTED, "Type", "");

  RNA_def_boolean(ot->srna, "object", false, "Object", "Make single user objects");
  RNA_def_boolean(ot->srna, "obdata", false, "Object Data", "Make single user object data");
  RNA_def_boolean(
      ot->srna, "material", false, "Materials", "Make materials local to each data-block");
  RNA_def_boolean(ot->srna,
                  "animation",
                  false,
                  "Object Animation",
                  "Make object animation data local to each object");
  RNA_def_boolean(ot->srna,
                  "obdata_animation",
                  false,
                  "Object Data Animation",
                  "Make object data (mesh, curve etc.) animation data local to each object");
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Drop Named Material on Object Operator
 * \{ */

std::string drop_named_material_tooltip(bContext *C, StringRef name, const int mval[2])
{
  int mat_slot = 0;
  Object *ob = ED_view3d_give_material_slot_under_cursor(C, mval, &mat_slot);
  if (ob == nullptr) {
    return {};
  }
  mat_slot = max_ii(mat_slot, 1);

  Material *prev_mat = BKE_object_material_get(ob, mat_slot);

  if (prev_mat) {
    return fmt::format(fmt::runtime(TIP_("Drop {} on {} (slot {}, replacing {})")),
                       name,
                       ob->id.name + 2,
                       mat_slot,
                       prev_mat->id.name + 2);
  }
  return fmt::format(
      fmt::runtime(TIP_("Drop {} on {} (slot {})")), name, ob->id.name + 2, mat_slot);
}

static wmOperatorStatus drop_named_material_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  Main *bmain = CTX_data_main(C);
  int mat_slot = 0;
  Object *ob = ED_view3d_give_material_slot_under_cursor(C, event->mval, &mat_slot);
  mat_slot = max_ii(mat_slot, 1);

  Material *ma = id_cast<Material *>(
      WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_MA));

  if (ob == nullptr || ma == nullptr) {
    return OPERATOR_CANCELLED;
  }

  int assign_type = BKE_MAT_ASSIGN_USERPREF;
  /* When trying to assign to non-editable object data, assign to the object instead. */
  if (BKE_id_is_editable(bmain, &ob->id) && ob->data && !BKE_id_is_editable(bmain, ob->data)) {
    assign_type = BKE_MAT_ASSIGN_OBJECT;
  }

  BKE_object_material_assign(CTX_data_main(C), ob, ma, mat_slot, assign_type);

  DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);

  WM_event_add_notifier(C, NC_OBJECT | ND_OB_SHADING, ob);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING_LINKS, ma);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_drop_named_material(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Drop Named Material on Object";
  ot->idname = "OBJECT_OT_drop_named_material";

  /* API callbacks. */
  ot->invoke = drop_named_material_invoke;
  ot->poll = ED_operator_objectmode_with_view3d_poll_msg;

  /* flags */
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;

  /* properties */
  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Drop Geometry Nodes on Object Operator
 * \{ */

std::string drop_geometry_nodes_tooltip(bContext *C, PointerRNA *properties, const int mval[2])
{
  const Object *ob = ED_view3d_give_object_under_cursor(C, mval);
  if (ob == nullptr) {
    return {};
  }

  const uint32_t session_uid = RNA_int_get(properties, "session_uid");
  const ID *id = BKE_libblock_find_session_uid(CTX_data_main(C), ID_NT, session_uid);
  if (!id) {
    return {};
  }

  return fmt::format(fmt::runtime(TIP_("Add modifier with node group \"{}\" on object \"{}\"")),
                     id->name,
                     ob->id.name);
}

static bool check_geometry_node_group_sockets(wmOperator *op, const bNodeTree *tree)
{
  tree->ensure_interface_cache();
  if (!tree->interface_outputs().is_empty()) {
    const bNodeTreeInterfaceSocket *first_output = tree->interface_outputs()[0];
    if (!first_output) {
      BKE_report(op->reports, RPT_ERROR, "The node group must have a geometry output socket");
      return false;
    }
    const bke::bNodeSocketType *typeinfo = first_output->socket_typeinfo();
    const eNodeSocketDatatype type = typeinfo ? typeinfo->type : SOCK_CUSTOM;
    if (type != SOCK_GEOMETRY) {
      BKE_report(op->reports, RPT_ERROR, "The first output must be a geometry socket");
      return false;
    }
  }
  return true;
}

static wmOperatorStatus drop_geometry_nodes_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  Object *ob = ED_view3d_give_object_under_cursor(C, event->mval);
  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);

  const uint32_t uid = RNA_int_get(op->ptr, "session_uid");
  bNodeTree *node_tree = id_cast<bNodeTree *>(BKE_libblock_find_session_uid(bmain, ID_NT, uid));
  if (!node_tree) {
    return OPERATOR_CANCELLED;
  }
  if (node_tree->type != NTREE_GEOMETRY) {
    BKE_report(op->reports, RPT_ERROR, "Node group must be a geometry node tree");
    return OPERATOR_CANCELLED;
  }

  if (!check_geometry_node_group_sockets(op, node_tree)) {
    return OPERATOR_CANCELLED;
  }

  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(
      modifier_add(op->reports, bmain, scene, ob, node_tree->id.name + 2, eModifierType_Nodes));
  if (!nmd) {
    BKE_report(op->reports, RPT_ERROR, "Could not add geometry nodes modifier");
    return OPERATOR_CANCELLED;
  }

  if (!RNA_boolean_get(op->ptr, "show_datablock_in_modifier")) {
    nmd->flag |= NODES_MODIFIER_HIDE_DATABLOCK_SELECTOR;
  }
  SET_FLAG_FROM_TEST(nmd->flag,
                     node_tree->geometry_node_asset_traits &&
                         (node_tree->geometry_node_asset_traits->flag &
                          GEO_NODE_ASSET_HIDE_MODIFIER_MANAGE_PANEL),
                     NODES_MODIFIER_HIDE_MANAGE_PANEL);

  nmd->node_group = node_tree;
  id_us_plus(&node_tree->id);
  MOD_nodes_update_interface(*bmain, ob, nmd);

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_drop_geometry_nodes(wmOperatorType *ot)
{
  ot->name = "Drop Geometry Node Group on Object";
  ot->idname = "OBJECT_OT_drop_geometry_nodes";

  ot->invoke = drop_geometry_nodes_invoke;
  ot->poll = ED_operator_view3d_active;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "session_uid",
                                  0,
                                  INT32_MIN,
                                  INT32_MAX,
                                  "Session UID",
                                  "Session UID of the geometry node group being dropped",
                                  INT32_MIN,
                                  INT32_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_boolean(ot->srna,
                  "show_datablock_in_modifier",
                  true,
                  "Show the data-block selector in the modifier",
                  "");
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Unlink Object Operator
 * \{ */

static wmOperatorStatus object_unlink_data_exec(bContext *C, wmOperator *op)
{
  ID *id;
  PropertyPointerRNA pprop;

  ui::context_active_but_prop_get_templateID(C, &pprop.ptr, &pprop.prop);

  if (pprop.prop == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Incorrect context for running object data unlink");
    return OPERATOR_CANCELLED;
  }

  id = pprop.ptr.owner_id;

  if (GS(id->name) == ID_OB) {
    Object *ob = id_cast<Object *>(id);
    if (ob->data) {
      ID *id_data = ob->data;

      if (GS(id_data->name) == ID_IM) {
        id_us_min(id_data);
        ob->data = nullptr;
      }
      else {
        BKE_report(op->reports, RPT_ERROR, "Cannot unlink this object data");
        return OPERATOR_CANCELLED;
      }
    }
  }

  RNA_property_update(C, &pprop.ptr, pprop.prop);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_unlink_data(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Unlink";
  ot->idname = "OBJECT_OT_unlink_data";

  /* API callbacks. */
  ot->exec = object_unlink_data_exec;

  /* flags */
  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

}  // namespace blender::ed::object
