/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edutil
 */

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_curve_types.h"

#include "BKE_collection.hh"
#include "BKE_global.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_remap.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_multires.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_packedFile.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"
#include "BKE_undo_system.hh"

#include "CLG_log.h"

#include "DEG_depsgraph.hh"

#include "ED_asset.hh"
#include "ED_image.hh"
#include "ED_mesh.hh"
#include "ED_object.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_screen_types.hh"
#include "ED_sculpt.hh"
#include "ED_space_api.hh"
#include "ED_util.hh"
#include "ED_view3d.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

/* ********* general editor util functions, not BKE stuff please! ********* */

void ED_editors_init_for_undo(Main *bmain)
{
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  for (wmWindow &win : wm->windows) {
    /* Stop animation from playing.
     * TODO: There might be a way to keep the animation from playing, but sad->scene and
     * sad->view_layer pointers are outdated and would need to be updated somehow. */
    bScreen *animscreen = ED_screen_animation_playing(wm);
    if (animscreen && animscreen->animtimer) {
      WM_event_timer_remove(wm, &win, animscreen->animtimer);
      animscreen->animtimer = nullptr;
    }

    /* UI Updates. */
    /* Flag local View3D's to check and exit if they are empty. */
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype == SPACE_VIEW3D) {
            View3D *v3d = reinterpret_cast<View3D *>(&sl);
            if (v3d->localvd) {
              v3d->localvd->runtime.flag |= V3D_RUNTIME_LOCAL_MAYBE_EMPTY;
            }
          }
        }
      }
    }
  }
}

void ED_editors_init(bContext *C)
{
  using namespace blender::ed;
  Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  wmWindowManager *wm = CTX_wm_manager(C);

  /* This is called during initialization, so we don't want to store any reports */
  ReportList *reports = CTX_wm_reports(C);
  int reports_flag_prev = reports->flag & ~RPT_STORE;

  std::swap(reports->flag, reports_flag_prev);

  /* Don't do undo pushes when calling an operator. */
  wm->op_undo_depth++;

  /* toggle on modes for objects that were saved with these enabled. for
   * e.g. linked objects we have to ensure that they are actually the
   * active object in this scene. */
  Object *obact = CTX_data_active_object(C);
  for (Object &ob : bmain->objects) {
    eObjectMode mode = ob.mode;
    if (mode == OB_MODE_OBJECT) {
      continue;
    }
    if (BKE_object_has_mode_data(&ob, mode)) {
      /* For multi-edit mode we may already have mode data. */
      continue;
    }

    /* Reset object to Object mode, so that code below can properly re-switch it to its
     * previous mode if possible, re-creating its mode data, etc. */
    ID *ob_data = ob.data;
    ob.mode = OB_MODE_OBJECT;
    DEG_id_tag_update(&ob.id, ID_RECALC_SYNC_TO_EVAL);

    /* Object mode is enforced if there is no active object, or if the active object's type is
     * different. */
    if (obact == nullptr || ob.type != obact->type) {
      continue;
    }
    /* Object mode is enforced for non-editable data (or their obdata). */
    if (!BKE_id_is_editable(bmain, &ob.id) ||
        (ob_data != nullptr && !BKE_id_is_editable(bmain, ob_data)))
    {
      continue;
    }

    /* Other edit/paint/etc. modes are only settable for objects visible in active scene currently.
     * Otherwise, they (and their obdata) may not be (fully) evaluated, which is mandatory for some
     * modes like Sculpt.
     * Ref. #98225. */
    if (!BKE_collection_has_object_recursive(scene->master_collection, &ob) ||
        !BKE_scene_has_object(*bmain, scene, &ob) || (ob.visibility_flag & OB_HIDE_VIEWPORT) != 0)
    {
      continue;
    }

    if (mode == OB_MODE_EDIT) {
      object::editmode_enter_ex(bmain, scene, &ob, 0);
    }
    else if (mode & OB_MODE_ALL_SCULPT) {
      if (obact == &ob) {
        if (mode == OB_MODE_SCULPT) {
          ed::sculpt_paint::object_sculpt_mode_enter(
              *bmain, *depsgraph, *scene, ob, true, reports);
        }
        else if (mode == OB_MODE_VERTEX_PAINT) {
          ED_object_vpaintmode_enter_ex(*bmain, *depsgraph, *scene, ob);
        }
        else {
          BLI_assert_unreachable();
        }
      }
      else {
        /* Create data for non-active objects which need it for
         * mode-switching but don't yet support multi-editing. */
        if (mode & OB_MODE_ALL_SCULPT) {
          ob.mode = mode;
          BKE_object_sculpt_data_create(&ob);
        }
      }
    }
    else {
      /* TODO(@ideasman42): avoid operator calls. */
      if (obact == &ob) {
        object::mode_set_ex(C, mode, true, reports);
      }
    }
  }

  /* image editor paint mode */
  if (scene) {
    ED_space_image_paint_update(bmain, wm, scene);
  }

  /* Enforce a full redraw for the first time areas/regions get drawn. Further region init/refresh
   * just triggers non-rebuild redraws (#RGN_DRAW_NO_REBUILD). Usually a full redraw would be
   * triggered by a `NC_WM | ND_FILEREAD` notifier, but if a startup script calls an operator that
   * redraws the window, notifiers are not handled before the operator runs. See #98461. */
  for (wmWindow &win : wm->windows) {
    const bScreen *screen = WM_window_get_active_screen(&win);

    ED_screen_areas_iter (&win, screen, area) {
      ED_area_tag_redraw(area);
    }
  }

  asset::list::storage_tag_main_data_dirty();

  std::swap(reports->flag, reports_flag_prev);
  wm->op_undo_depth--;
}

void ED_editors_exit(Main *bmain, bool do_undo_system)
{
  using namespace blender::ed;
  if (!bmain) {
    return;
  }

  /* Frees all edit-mode undo-steps. */
  if (do_undo_system && G_MAIN->wm.first) {
    wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
    /* normally we don't check for null undo stack,
     * do here since it may run in different context. */
    if (wm->runtime->undo_stack) {
      BKE_undosys_stack_destroy(wm->runtime->undo_stack);
      wm->runtime->undo_stack = nullptr;
    }
  }

  /* On undo, tag for update so the depsgraph doesn't use stale edit-mode data,
   * this is possible when mixing edit-mode and memory-file undo.
   *
   * By convention, objects are not left in edit-mode - so this isn't often problem in practice,
   * since exiting edit-mode will tag the objects too.
   *
   * However there is no guarantee the active object _never_ changes while in edit-mode.
   * Python for example can do this, some callers to #object::base_activate
   * don't handle modes either (doing so isn't always practical).
   *
   * To reproduce the problem where stale data is used, see: #84920. */
  for (Object &ob : bmain->objects) {
    if (object::editmode_free_ex(bmain, &ob)) {
      if (do_undo_system == false) {
        DEG_id_tag_update(&ob.id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
      }
    }
  }

  /* global in meshtools... */
  ED_mesh_mirror_spatial_table_end(nullptr);
  ED_mesh_mirror_topo_table_end(nullptr);
}

static bool curve_source_validate(const Curve &curve, ReportList *reports)
{
  std::string error;
  if (bke::nurbs::curve_validate_source(curve, &error)) {
    return true;
  }
  static CLG_LogRef LOG = {"edit.flush"};
  CLOG_ERROR(&LOG, "Unable to flush '%s': %s", curve.id.name + 2, error.c_str());
  if (reports != nullptr) {
    BKE_reportf(reports, RPT_ERROR, "Unable to flush '%s': %s", curve.id.name + 2, error.c_str());
  }
  return false;
}

EditDataFlushResult ED_editors_flush_edits_for_object_with_result(
    Main *bmain, Object *ob, bool for_render, bool check_needs_flush, ReportList *reports)
{
  using namespace blender::ed;
  EditDataFlushResult result;
  if (ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF, OB_FONT) &&
      !curve_source_validate(*id_cast<Curve *>(ob->data), reports))
  {
    bmain->is_memfile_undo_flush_needed = true;
    result.success = false;
    return result;
  }
  if (ob->mode & OB_MODE_SCULPT) {
    /* Don't allow flushing while in the middle of a stroke (frees data in use).
     * Auto-save prevents this from happening but scripts
     * may cause a flush on saving: #53986. */
    if (ob->runtime->sculpt_session != nullptr && ob->runtime->sculpt_session->cache == nullptr) {
      if (check_needs_flush && !ob->runtime->sculpt_session->needs_flush_to_id) {
        return result;
      }
      ob->runtime->sculpt_session->needs_flush_to_id = false;

      /* flush multires changes (for sculpt) */
      multires_flush_sculpt_updates(ob);
      result.has_edits = true;

      if (for_render) {
        /* flush changes from dynamic topology sculpt */
        BKE_sculptsession_bm_to_me_for_render(ob);
      }
      else {
        /* Set reorder=false so that saving the file doesn't reorder
         * the BMesh's elements */
        BKE_sculptsession_bm_to_me(ob);
      }
    }
  }
  else if (ob->mode & OB_MODE_EDIT) {

    char *needs_flush_ptr = BKE_object_data_editmode_flush_ptr_get(ob->data);
    if (needs_flush_ptr != nullptr) {
      if (check_needs_flush && (*needs_flush_ptr == 0)) {
        return result;
      }
    }

    /* get editmode results */
    result.has_edits = true;
    result.success = object::editmode_load_ex(bmain, ob, reports) !=
                     object::EditModeDataResult::Failure;
    if (result.success && needs_flush_ptr != nullptr) {
      *needs_flush_ptr = 0;
    }
  }
  return result;
}

bool ED_editors_flush_edits_for_object_ex(Main *bmain,
                                          Object *ob,
                                          bool for_render,
                                          bool check_needs_flush)
{
  return ED_editors_flush_edits_for_object_with_result(bmain, ob, for_render, check_needs_flush)
      .has_edits;
}

bool ED_editors_flush_edits_for_object(Main *bmain, Object *ob)
{
  return ED_editors_flush_edits_for_object_ex(bmain, ob, false, false);
}

EditDataFlushResult ED_editors_flush_edits_with_result(Main *bmain,
                                                       bool for_render,
                                                       bool check_needs_flush,
                                                       ReportList *reports)
{
  EditDataFlushResult result;
  /* Check native sources before flushing any objects, including owners without an Object user. */
  for (const Curve &curve : bmain->curves) {
    if (!curve_source_validate(curve, reports)) {
      bmain->is_memfile_undo_flush_needed = true;
      result.success = false;
      return result;
    }
  }

  /* A context cannot identify all objects with edit data (e.g. multiple sculpt objects). */
  for (Object &ob : bmain->objects) {
    const EditDataFlushResult object_result = ED_editors_flush_edits_for_object_with_result(
        bmain, &ob, for_render, check_needs_flush, reports);
    result.has_edits |= object_result.has_edits;
    result.success &= object_result.success;
  }
  bmain->is_memfile_undo_flush_needed = !result.success;
  return result;
}

bool ED_editors_flush_edits_ex(Main *bmain, bool for_render, bool check_needs_flush)
{
  return ED_editors_flush_edits_with_result(bmain, for_render, check_needs_flush).has_edits;
}

bool ED_editors_flush_edits(Main *bmain)
{
  return ED_editors_flush_edits_ex(bmain, false, false);
}

/* ***** XXX: functions are using old blender names, cleanup later ***** */

void apply_keyb_grid(
    bool shift, bool ctrl, float *val, float fac1, float fac2, float fac3, int invert)
{
  /* fac1 is for 'nothing', fac2 for CTRL, fac3 for SHIFT */
  if (invert) {
    ctrl = !ctrl;
  }

  if (ctrl && shift) {
    if (fac3 != 0.0f) {
      *val = fac3 * floorf(*val / fac3 + 0.5f);
    }
  }
  else if (ctrl) {
    if (fac2 != 0.0f) {
      *val = fac2 * floorf(*val / fac2 + 0.5f);
    }
  }
  else {
    if (fac1 != 0.0f) {
      *val = fac1 * floorf(*val / fac1 + 0.5f);
    }
  }
}

void unpack_menu(bContext *C,
                 const char *opname,
                 const char *id_name,
                 const char *abs_name,
                 const char *folder,
                 PackedFile *pf)
{
  Main *bmain = CTX_data_main(C);
  char line[FILE_MAX + 100];
  wmOperatorType *ot = WM_operatortype_find(opname, true);
  const char *blendfile_path = BKE_main_blendfile_path(bmain);

  ui::PopupMenu *pup = ui::popup_menu_begin(C, IFACE_("Unpack File"), ICON_NONE);
  ui::Layout &layout = *popup_menu_layout(pup);

  PointerRNA props_ptr = layout.op(
      ot, IFACE_("Remove Pack"), ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
  RNA_enum_set(&props_ptr, "method", PF_REMOVE);
  RNA_string_set(&props_ptr, "id", id_name);

  if (blendfile_path[0] != '\0') {
    char local_name[FILE_MAXDIR + FILE_MAX], fi[FILE_MAX];

    BLI_path_split_file_part(abs_name, fi, sizeof(fi));
    BLI_path_join(local_name, sizeof(local_name), "//", folder, fi);
    if (!STREQ(abs_name, local_name)) {
      switch (BKE_packedfile_compare_to_file(blendfile_path, local_name, pf)) {
        case PF_CMP_NOFILE:
          SNPRINTF_UTF8(line, IFACE_("Create %s"), local_name);
          props_ptr = layout.op(ot, line, ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
          RNA_enum_set(&props_ptr, "method", PF_WRITE_LOCAL);
          RNA_string_set(&props_ptr, "id", id_name);

          break;
        case PF_CMP_EQUAL:
          SNPRINTF_UTF8(line, IFACE_("Use %s (identical)"), local_name);
          props_ptr = layout.op(ot, line, ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
          RNA_enum_set(&props_ptr, "method", PF_USE_LOCAL);
          RNA_string_set(&props_ptr, "id", id_name);

          break;
        case PF_CMP_DIFFERS:
          SNPRINTF_UTF8(line, IFACE_("Use %s (differs)"), local_name);
          props_ptr = layout.op(ot, line, ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
          RNA_enum_set(&props_ptr, "method", PF_USE_LOCAL);
          RNA_string_set(&props_ptr, "id", id_name);

          SNPRINTF_UTF8(line, IFACE_("Overwrite %s"), local_name);
          props_ptr = layout.op(ot, line, ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
          RNA_enum_set(&props_ptr, "method", PF_WRITE_LOCAL);
          RNA_string_set(&props_ptr, "id", id_name);
          break;
      }
    }
  }

  switch (BKE_packedfile_compare_to_file(blendfile_path, abs_name, pf)) {
    case PF_CMP_NOFILE:
      SNPRINTF_UTF8(line, IFACE_("Create %s"), abs_name);
      props_ptr = layout.op(ot, line, ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
      RNA_enum_set(&props_ptr, "method", PF_WRITE_ORIGINAL);
      RNA_string_set(&props_ptr, "id", id_name);
      break;
    case PF_CMP_EQUAL:
      SNPRINTF_UTF8(line, IFACE_("Use %s (identical)"), abs_name);
      props_ptr = layout.op(ot, line, ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
      RNA_enum_set(&props_ptr, "method", PF_USE_ORIGINAL);
      RNA_string_set(&props_ptr, "id", id_name);
      break;
    case PF_CMP_DIFFERS:
      SNPRINTF_UTF8(line, IFACE_("Use %s (differs)"), abs_name);
      props_ptr = layout.op(ot, line, ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
      RNA_enum_set(&props_ptr, "method", PF_USE_ORIGINAL);
      RNA_string_set(&props_ptr, "id", id_name);

      SNPRINTF_UTF8(line, IFACE_("Overwrite %s"), abs_name);
      props_ptr = layout.op(ot, line, ICON_NONE, wm::OpCallContext::ExecDefault, UI_ITEM_NONE);
      RNA_enum_set(&props_ptr, "method", PF_WRITE_ORIGINAL);
      RNA_string_set(&props_ptr, "id", id_name);
      break;
  }

  popup_menu_end(C, pup);
}

void ED_spacedata_id_remap(ScrArea *area, SpaceLink *sl, const bke::id::IDRemapper &mappings)
{
  SpaceType *st = BKE_spacetype_from_id(sl->spacetype);
  if (st && st->id_remap) {
    st->id_remap(area, sl, mappings);
  }
}

void ED_spacedata_id_remap_single(ScrArea *area, SpaceLink *sl, ID *old_id, ID *new_id)
{
  SpaceType *st = BKE_spacetype_from_id(sl->spacetype);

  if (st && st->id_remap) {
    bke::id::IDRemapper mappings;
    mappings.add(old_id, new_id);
    st->id_remap(area, sl, mappings);
  }
}

}  // namespace blender
