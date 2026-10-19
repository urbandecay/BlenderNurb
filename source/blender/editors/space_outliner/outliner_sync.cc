/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_layer_types.h"
#include "DNA_outliner_types.h"
#include "DNA_screen_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"
#include "DNA_workspace_types.h"

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"

#include "DEG_depsgraph.hh"

#include "ED_object.hh"
#include "ED_outliner.hh"

#include "SEQ_select.hh"

#include "WM_api.hh"
#include "WM_types.hh"


#include "tree/tree_element_seq.hh"

#include "outliner_intern.hh"

namespace blender {

void ED_outliner_select_sync_from_object_tag(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wm->outliner_sync_select_dirty |= WM_OUTLINER_SYNC_SELECT_FROM_OBJECT;
}

void ED_outliner_select_sync_from_sequence_tag(const bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wm->outliner_sync_select_dirty |= WM_OUTLINER_SYNC_SELECT_FROM_SEQUENCE;
}

void ED_outliner_select_sync_from_all_tag(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wm->outliner_sync_select_dirty |= WM_OUTLINER_SYNC_SELECT_FROM_ALL;
}

bool ED_outliner_select_sync_is_dirty(const bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  return wm->outliner_sync_select_dirty & WM_OUTLINER_SYNC_SELECT_FROM_ALL;
}

void ED_outliner_select_sync_flag_outliners(const bContext *C)
{
  Main *bmain = CTX_data_main(C);
  wmWindowManager *wm = CTX_wm_manager(C);

  for (bScreen *screen = static_cast<bScreen *>(bmain->screens.first); screen;
       screen = static_cast<bScreen *>(screen->id.next))
  {
    for (ScrArea &area : screen->areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype == SPACE_OUTLINER) {
          SpaceOutliner *space_outliner = reinterpret_cast<SpaceOutliner *>(&sl);

          space_outliner->sync_select_dirty |= wm->outliner_sync_select_dirty;
        }
      }
    }
  }

  /* Clear global sync flag */
  wm->outliner_sync_select_dirty = eWM_OutlinerSyncSelectDirty{};
}

namespace ed::outliner {

/**
 * Outliner sync select dirty flags are not enough to determine which types to sync,
 * outliner display mode also needs to be considered. This stores the types of data
 * to sync to increase code clarity.
 */
struct SyncSelectTypes {
  bool object;
  bool seq_strip;
};

/**
 * Set which types of data to sync when syncing selection from the outliner based on object
 * interaction mode and outliner display mode
 */
static void outliner_sync_select_from_outliner_set_types(SpaceOutliner *space_outliner,
                                                         SyncSelectTypes *sync_types)
{
  const bool sequence_view = space_outliner->outlinevis == SO_SEQUENCE;

  sync_types->object = !sequence_view;
  sync_types->seq_strip = sequence_view;
}

/**
 * Current dirty flags and outliner display mode determine which type of syncing should occur.
 * This is to ensure sync flag data is not lost on sync in the wrong display mode.
 * Returns true if a sync is needed.
 */
static bool outliner_sync_select_to_outliner_set_types(SpaceOutliner *space_outliner,
                                                       SyncSelectTypes *sync_types)
{
  const bool sequence_view = space_outliner->outlinevis == SO_SEQUENCE;

  sync_types->object = !sequence_view &&
                       (space_outliner->sync_select_dirty & WM_OUTLINER_SYNC_SELECT_FROM_OBJECT);
  sync_types->seq_strip = sequence_view && (space_outliner->sync_select_dirty &
                                            WM_OUTLINER_SYNC_SELECT_FROM_SEQUENCE);

  return sync_types->object || sync_types->seq_strip;
}

/**
 * Stores items selected from a sync from the outliner. Prevents syncing the selection
 * state of the last instance of an object linked in multiple collections.
 */
struct SelectedItems {
  Set<Base *> objects;
};

static void outliner_select_sync_to_object(ViewLayer *view_layer,
                                           TreeElement *te,
                                           TreeStoreElem *tselem,
                                           Set<Base *> &selected_objects)
{
  Object *ob = id_cast<Object *>(tselem->id);
  Base *base = (te->directdata) ? static_cast<Base *>(te->directdata) :
                                  BKE_view_layer_base_find(view_layer, ob);

  if (base && (base->flag & BASE_SELECTABLE)) {
    if (tselem->flag & TSE_SELECTED) {
      object::base_select(base, object::BA_SELECT);
      selected_objects.add(base);
    }
    else if (!selected_objects.contains(base)) {
      object::base_select(base, object::BA_DESELECT);
    }
  }
}

static void outliner_select_sync_to_strip(WorkSpace *workspace, const TreeElement *te)
{
  Scene *sequencer_scene = workspace->sequencer_scene;
  if (!sequencer_scene) {
    return;
  }
  const TreeStoreElem *tselem = TREESTORE(te);

  const TreeElementStrip *te_strip = tree_element_cast<TreeElementStrip>(te);
  Strip *strip = &te_strip->get_strip();

  if (tselem->flag & TSE_ACTIVE) {
    seq::select_active_set(sequencer_scene, strip);
  }

  if (tselem->flag & TSE_SELECTED) {
    strip->flag |= SEQ_SELECT;
  }
  else {
    strip->flag &= ~SEQ_SELECT;
  }
}

/** Sync select and active flags from outliner to active view layer and sequencer. */
static void outliner_sync_selection_from_outliner(WorkSpace *workspace,
                                                  ViewLayer *view_layer,
                                                  ListBaseT<TreeElement> *tree,
                                                  const SyncSelectTypes *sync_types,
                                                  SelectedItems *selected_items)
{

  for (TreeElement &te : *tree) {
    TreeStoreElem *tselem = TREESTORE(&te);

    if ((tselem->type == TSE_SOME_ID) && (te.idcode == ID_OB)) {
      if (sync_types->object) {
        outliner_select_sync_to_object(view_layer, &te, tselem, selected_items->objects);
      }
    }
    else if (tselem->type == TSE_STRIP) {
      if (sync_types->seq_strip) {
        outliner_select_sync_to_strip(workspace, &te);
      }
    }

    outliner_sync_selection_from_outliner(
        workspace, view_layer, &te.subtree, sync_types, selected_items);
  }
}

}  // namespace ed::outliner

void ED_outliner_select_sync_from_outliner(bContext *C, SpaceOutliner *space_outliner)
{
  using namespace blender::ed::outliner;

  /* Don't sync if not checked or in certain outliner display modes */
  if (!(space_outliner->flag & SO_SYNC_SELECT) || ELEM(space_outliner->outlinevis,
                                                       SO_LIBRARIES,
                                                       SO_OVERRIDES_LIBRARY,
                                                       SO_DATA_API,
                                                       SO_ID_ORPHANS))
  {
    return;
  }

  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  SyncSelectTypes sync_types;
  outliner_sync_select_from_outliner_set_types(space_outliner, &sync_types);

  /* To store elements that have been selected to prevent linked object sync errors */
  SelectedItems selected_items;
  outliner_sync_selection_from_outliner(CTX_wm_workspace(C),
                                        view_layer,
                                        &space_outliner->runtime->tree,
                                        &sync_types,
                                        &selected_items);

  /* Tag for updates and clear dirty flag to prevent a sync to the outliner on draw. */
  if (sync_types.object) {
    space_outliner->sync_select_dirty &= ~WM_OUTLINER_SYNC_SELECT_FROM_OBJECT;
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  }
  if (sync_types.seq_strip) {
    space_outliner->sync_select_dirty &= ~WM_OUTLINER_SYNC_SELECT_FROM_SEQUENCE;
    WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER | NA_SELECTED, CTX_data_sequencer_scene(C));
  }
}

namespace ed::outliner {

static void outliner_select_sync_from_object(const Main &bmain,
                                             const Scene *scene,
                                             ViewLayer *view_layer,
                                             Object *obact,
                                             TreeElement *te,
                                             TreeStoreElem *tselem)
{
  Object *ob = id_cast<Object *>(tselem->id);
  BKE_view_layer_synced_ensure(bmain, scene, view_layer);
  Base *base = (te->directdata) ? static_cast<Base *>(te->directdata) :
                                  BKE_view_layer_base_find(view_layer, ob);
  const bool is_selected = (base != nullptr) && ((base->flag & BASE_SELECTED) != 0);

  if (base && (ob == obact)) {
    tselem->flag |= TSE_ACTIVE;
  }
  else {
    tselem->flag &= ~TSE_ACTIVE;
  }

  if (is_selected) {
    tselem->flag |= TSE_SELECTED;
  }
  else {
    tselem->flag &= ~TSE_SELECTED;
  }
}

static void outliner_select_sync_from_strip(Strip *strip_active, const TreeElement *te)
{
  TreeStoreElem *tselem = TREESTORE(te);

  const TreeElementStrip *te_strip = tree_element_cast<TreeElementStrip>(te);
  const Strip *strip = &te_strip->get_strip();

  if (strip == strip_active) {
    tselem->flag |= TSE_ACTIVE;
  }
  else {
    tselem->flag &= ~TSE_ACTIVE;
  }

  if (strip->flag & SEQ_SELECT) {
    tselem->flag |= TSE_SELECTED;
  }
  else {
    tselem->flag &= ~TSE_SELECTED;
  }
}

/**
 * Contains active object and strip for syncing to prevent getting active data
 * repeatedly throughout syncing to the outliner.
 */
struct SyncSelectActiveData {
  Object *object;
  Strip *strip;
};

/** Sync select and active flags from active view layer and sequences to the outliner. */
static void outliner_sync_selection_to_outliner(const Main &bmain,
                                                const Scene *scene,
                                                ViewLayer *view_layer,
                                                SpaceOutliner *space_outliner,
                                                ListBaseT<TreeElement> *tree,
                                                SyncSelectActiveData *active_data,
                                                const SyncSelectTypes *sync_types,
                                                bool *r_any_new_active)
{
  for (TreeElement &te : *tree) {
    TreeStoreElem *tselem = TREESTORE(&te);
    const bool is_active_old = (tselem->flag & TSE_ACTIVE) && (tselem->flag & TSE_SELECTED);

    if ((tselem->type == TSE_SOME_ID) && te.idcode == ID_OB) {
      if (sync_types->object) {
        outliner_select_sync_from_object(
            bmain, scene, view_layer, active_data->object, &te, tselem);
      }
    }
    else if (tselem->type == TSE_STRIP) {
      if (sync_types->seq_strip) {
        outliner_select_sync_from_strip(active_data->strip, &te);
      }
    }
    else {
      tselem->flag &= ~(TSE_SELECTED | TSE_ACTIVE);
    }
    const bool is_active_new = (tselem->flag & TSE_ACTIVE) && (tselem->flag & TSE_SELECTED);
    *r_any_new_active |= is_active_new && !is_active_old;
    /* Sync subtree elements */
    outliner_sync_selection_to_outliner(bmain,
                                        scene,
                                        view_layer,
                                        space_outliner,
                                        &te.subtree,
                                        active_data,
                                        sync_types,
                                        r_any_new_active);
  }
}

/* Get active data from context */
static void get_sync_select_active_data(const bContext *C, SyncSelectActiveData *active_data)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Scene *sequencer_scene = CTX_data_sequencer_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  active_data->object = BKE_view_layer_active_object_get(view_layer);
  active_data->strip = sequencer_scene ? seq::select_active_get(sequencer_scene) : nullptr;
}

bool outliner_sync_selection(const bContext *C,
                             const TreeViewContext &tvc,
                             SpaceOutliner *space_outliner)
{
  /* Set which types of data to sync from sync dirty flag and outliner display mode */
  SyncSelectTypes sync_types;
  const bool sync_required = outliner_sync_select_to_outliner_set_types(space_outliner,
                                                                         &sync_types);
  bool r_any_new_active = false;

  if (sync_required) {
    /* Store active object and strip */
    SyncSelectActiveData active_data;
    get_sync_select_active_data(C, &active_data);

    outliner_sync_selection_to_outliner(*tvc.bmain,
                                        tvc.scene,
                                        tvc.view_layer,
                                        space_outliner,
                                        &space_outliner->runtime->tree,
                                        &active_data,
                                        &sync_types,
                                        &r_any_new_active);

    /* Keep any un-synced data in the dirty flag. */
    if (sync_types.object) {
      space_outliner->sync_select_dirty &= ~WM_OUTLINER_SYNC_SELECT_FROM_OBJECT;
    }
    if (sync_types.seq_strip) {
      space_outliner->sync_select_dirty &= ~WM_OUTLINER_SYNC_SELECT_FROM_SEQUENCE;
    }
  }

  return r_any_new_active;
}

}  // namespace ed::outliner
}  // namespace blender
