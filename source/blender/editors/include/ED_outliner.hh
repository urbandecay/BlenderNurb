/* SPDX-FileCopyrightText: 2015 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "DNA_listBase.h"

namespace blender {

struct Base;
struct SpaceOutliner;
struct bContext;
struct ID;
struct PointerRNA;

bool ED_outliner_collections_editor_poll(bContext *C);

/**
 * Populates the `objects` #ListBaseT with all the outliner selected objects
 * We store it as `(Object *)LinkData->data`.
 *
 * \param objects: Expected to be empty.
 */
void ED_outliner_selected_objects_get(const bContext *C, ListBaseT<LinkData> *objects);

/**
 * Get base of object under cursor. Used for eyedropper tool.
 */
Base *ED_outliner_give_base_under_cursor(bContext *C, const int mval[2]);

/**
 * Functions for tagging outliner selection syncing is dirty from operators.
 */
void ED_outliner_select_sync_from_object_tag(bContext *C);
void ED_outliner_select_sync_from_sequence_tag(const bContext *C);
void ED_outliner_select_sync_from_all_tag(bContext *C);

bool ED_outliner_select_sync_is_dirty(const bContext *C);

/**
 * Set clean outliner and mark other outliners for syncing.
 */
void ED_outliner_select_sync_from_outliner(bContext *C, SpaceOutliner *space_outliner);

/**
 * Copy sync select dirty flag from window manager to all outliners to be synced lazily on draw.
 */
void ED_outliner_select_sync_flag_outliners(const bContext *C);

int ED_outliner_icon_from_id(const ID &id);

bool ED_outliner_support_searching(const SpaceOutliner *space_outliner);

}  // namespace blender
