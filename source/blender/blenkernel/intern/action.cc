/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_assert.h"
#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_session_uid.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_action.hh"
#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_asset.hh"
#include "BKE_fcurve.hh"
#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_preview_image.hh"

#include "RNA_access.hh"
#include "RNA_path.hh"

#include "BLO_read_write.hh"

#include "MEM_guardedalloc.h"

#include "ANIM_action.hh"
#include "ANIM_action_legacy.hh"
#include "ANIM_versioning.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"anim.action"};

/**************************** Action Datablock ******************************/
namespace bke {

static void action_init_data(ID *action_id)
{
  BLI_assert(GS(action_id->name) == ID_AC);
  bAction *action = reinterpret_cast<bAction *>(action_id);

  INIT_DEFAULT_STRUCT_AFTER(action, id);
}

/**
 * Only copy internal data of Action ID from source
 * to already allocated/initialized destination.
 * You probably never want to use that directly,
 * use #BKE_id_copy or #BKE_id_copy_ex for typical needs.
 *
 * WARNING! This function will not handle ID user count!
 *
 * \param flag: Copying options (see BKE_lib_id.hh's LIB_ID_COPY_... flags for more).
 */
static void action_copy_data(Main * /*bmain*/,
                             std::optional<Library *> /*owner_library*/,
                             ID *id_dst,
                             const ID *id_src,
                             const int flag)
{
  bAction *dna_action_dst = reinterpret_cast<bAction *>(id_dst);
  animrig::Action &action_dst = dna_action_dst->wrap();

  const bAction *dna_action_src = reinterpret_cast<const bAction *>(id_src);
  const animrig::Action &action_src = dna_action_src->wrap();

  bActionGroup *group_dst, *group_src;
  FCurve *fcurve_dst, *fcurve_src;

  /* Duplicate the lists of groups and markers. */
  BLI_duplicatelist(&action_dst.groups, &action_src.groups);
  BKE_copy_time_markers(action_dst.markers, action_src.markers, flag);

  /* Copy F-Curves, fixing up the links as we go. */
  action_dst.curves.clear_no_delete();

  for (fcurve_src = static_cast<FCurve *>(action_src.curves.first); fcurve_src;
       fcurve_src = fcurve_src->next)
  {
    /* Duplicate F-Curve. */

    /* XXX TODO: pass sub-data flag?
     * But surprisingly does not seem to be doing any ID reference-counting. */
    fcurve_dst = BKE_fcurve_copy(fcurve_src);

    BLI_addtail(&action_dst.curves, fcurve_dst);

    /* Fix group links (kind of bad list-in-list search, but this is the most reliable way). */
    for (group_dst = static_cast<bActionGroup *>(action_dst.groups.first),
        group_src = static_cast<bActionGroup *>(action_src.groups.first);
         group_dst && group_src;
         group_dst = group_dst->next, group_src = group_src->next)
    {
      if (fcurve_src->grp == group_src) {
        fcurve_dst->grp = group_dst;

        if (group_dst->channels.first == fcurve_src) {
          group_dst->channels.first = fcurve_dst;
        }
        if (group_dst->channels.last == fcurve_src) {
          group_dst->channels.last = fcurve_dst;
        }
        break;
      }
    }
  }

  /* Copy all simple properties. */
  action_dst.layer_array_num = action_src.layer_array_num;
  action_dst.layer_active_index = action_src.layer_active_index;
  action_dst.slot_array_num = action_src.slot_array_num;
  action_dst.last_slot_handle = action_src.last_slot_handle;

  /* Layers, and (recursively) Strips. */
  action_dst.layer_array = MEM_new_array_zeroed<ActionLayer *>(action_src.layer_array_num,
                                                               __func__);
  for (int i : action_src.layers().index_range()) {
    action_dst.layer_array[i] = action_src.layer(i)->duplicate_with_shallow_strip_copies(__func__);
  }

  /* Strip data. */
  action_dst.strip_keyframe_data_array = MEM_new_array_zeroed<ActionStripKeyframeData *>(
      action_src.strip_keyframe_data_array_num, __func__);
  for (int i : action_src.strip_keyframe_data().index_range()) {
    action_dst.strip_keyframe_data_array[i] = MEM_new<animrig::StripKeyframeData>(
        __func__, *action_src.strip_keyframe_data()[i]);
  }

  /* Slots. */
  action_dst.slot_array = MEM_new_array_zeroed<ActionSlot *>(action_src.slot_array_num, __func__);
  for (int i : action_src.slots().index_range()) {
    action_dst.slot_array[i] = MEM_new<animrig::Slot>(__func__, *action_src.slot(i));
  }

  if (flag & LIB_ID_COPY_NO_PREVIEW) {
    action_dst.preview = nullptr;
  }
  else {
    BKE_previewimg_id_copy(&action_dst.id, &action_src.id);
  }
}

/** Free (or release) any data used by this action (does not free the action itself). */
static void action_free_data(ID *id)
{
  animrig::Action &action = reinterpret_cast<bAction *>(id)->wrap();

  /* Free keyframe data. */
  for (animrig::StripKeyframeData *keyframe_data : action.strip_keyframe_data()) {
    MEM_delete(keyframe_data);
  }
  MEM_SAFE_DELETE(action.strip_keyframe_data_array);
  action.strip_keyframe_data_array_num = 0;

  /* Free layers. */
  for (animrig::Layer *layer : action.layers()) {
    MEM_delete(layer);
  }
  MEM_SAFE_DELETE(action.layer_array);
  action.layer_array_num = 0;

  /* Free slots. */
  for (animrig::Slot *slot : action.slots()) {
    MEM_delete(slot);
  }
  MEM_SAFE_DELETE(action.slot_array);
  action.slot_array_num = 0;

  /* Free legacy F-Curves & groups. */
  BKE_fcurves_free(&action.curves);
  action.groups.free_no_destruct();

  /* Free markers & preview. */
  action.markers.free_no_destruct();
  BKE_previewimg_id_free(&action.id);

  BLI_assert(action.is_empty());
}

static void action_foreach_id(ID *id, LibraryForeachIDData *data)
{
  animrig::Action &action = reinterpret_cast<bAction *>(id)->wrap();

  /* When this function is called without the IDWALK_READONLY flag, calls to
   * BKE_LIB_FOREACHID_PROCESS_... macros can change ID pointers. ID remapping is the main example
   * of such use.
   *
   * Those ID pointer changes are not guaranteed to be valid, though. For example, the remapping
   * can be used to replace one Mesh with another, but that neither means that the new Mesh is
   * animated with the same Action, nor that the old Mesh is no longer animated by that Action. In
   * other words, the best that can be done is to invalidate the cache.
   *
   * NOTE: early-returns by BKE_LIB_FOREACHID_PROCESS_... macros are forbidden in non-readonly
   * cases (see #IDWALK_RET_STOP_ITER documentation). */

  constexpr LibraryForeachIDCallbackFlag idwalk_flags = IDWALK_CB_NEVER_SELF | IDWALK_CB_LOOPBACK;

  /* Note that `bmain` can be `nullptr`. An example is in
   * `deg_eval_copy_on_write.cc`, function `deg_expand_eval_copy_datablock`. */
  Main *bmain = BKE_lib_query_foreachid_process_main_get(data);

  /* This function should not rebuild the slot user map, because that in turn loops over all IDs.
   * It is really up to the caller to ensure things are clean when the slot user pointers should be
   * reported.
   *
   * For things like ID remapping it's fine to skip the pointers when they're dirty. The next time
   * somebody tries to actually use them, they will be rebuilt anyway. */
  const bool slot_user_cache_is_known_clean = bmain && !bmain->is_action_slot_to_id_map_dirty;

  if (slot_user_cache_is_known_clean) {
    bool should_invalidate = false;
    for (animrig::Slot *slot : action.slots()) {
      for (ID *&slot_user : slot->runtime_users()) {
        ID *const old_pointer = slot_user;
        BKE_LIB_FOREACHID_PROCESS_ID(data, slot_user, idwalk_flags);
        /* If slot_user changed, the cache should be invalidated. Not all pointer changes are
         * semantically correct for our use. For example, when ID-remapping is used to replace
         * MECube with MESuzanne. If MECube is animated by some slot before the remap, it will
         * remain animated by that slot after the remap, even when all `object->data` pointers now
         * reference MESuzanne instead. */
        should_invalidate |= (slot_user != old_pointer);
      }
    }

    if (should_invalidate) {
      animrig::Slot::users_invalidate(*bmain);
    }

#ifndef NDEBUG
    const LibraryForeachIDFlag flag = BKE_lib_query_foreachid_process_flags_get(data);
    const bool is_readonly = flag & IDWALK_READONLY;
    if (is_readonly) {
      BLI_assert_msg(!should_invalidate,
                     "pointers were changed while IDWALK_READONLY flag was set");
    }
#endif
  }

  /* Note that, even though `BKE_fcurve_foreach_id()` exists, it is not called here. That function
   * is only relevant for drivers, but the F-Curves stored in an Action are always just animation
   * data, not drivers. */

  for (TimeMarker &marker : action.markers) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, marker.camera, IDWALK_CB_NOP);
  }
}

static void write_channelbag(BlendWriter *writer, animrig::Channelbag &channelbag)
{
  writer->write_struct_cast<ActionChannelbag>(&channelbag);

  Span<bActionGroup *> groups = channelbag.channel_groups();
  writer->write_pointer_array(groups.size(), groups.data());
  for (const bActionGroup *group : groups) {
    writer->write_struct(group);
  }

  Span<FCurve *> fcurves = channelbag.fcurves();
  writer->write_pointer_array(fcurves.size(), fcurves.data());
  for (FCurve *fcurve : fcurves) {
    writer->write_struct(fcurve);
    BKE_fcurve_blend_write_data(writer, fcurve);
  }
}

static void write_strip_keyframe_data(BlendWriter *writer,
                                      animrig::StripKeyframeData &strip_keyframe_data)
{
  writer->write_struct_cast<ActionStripKeyframeData>(&strip_keyframe_data);

  auto channelbags = strip_keyframe_data.channelbags();
  writer->write_pointer_array(channelbags.size(), channelbags.data());

  for (animrig::Channelbag *channelbag : channelbags) {
    write_channelbag(writer, *channelbag);
  }
}

static void write_strip_keyframe_data_array(
    BlendWriter *writer, Span<animrig::StripKeyframeData *> strip_keyframe_data_array)
{
  writer->write_pointer_array(strip_keyframe_data_array.size(), strip_keyframe_data_array.data());

  for (animrig::StripKeyframeData *keyframe_data : strip_keyframe_data_array) {
    write_strip_keyframe_data(writer, *keyframe_data);
  }
}

static void write_strips(BlendWriter *writer, Span<animrig::Strip *> strips)
{
  writer->write_pointer_array(strips.size(), strips.data());

  for (animrig::Strip *strip : strips) {
    writer->write_struct_cast<ActionStrip>(strip);
  }
}

static void write_layers(BlendWriter *writer, Span<animrig::Layer *> layers)
{
  writer->write_pointer_array(layers.size(), layers.data());

  for (animrig::Layer *layer : layers) {
    writer->write_struct_cast<ActionLayer>(layer);
    write_strips(writer, layer->strips());
  }
}

static void write_slots(BlendWriter *writer, Span<animrig::Slot *> slots)
{
  writer->write_pointer_array(slots.size(), slots.data());
  for (animrig::Slot *slot : slots) {
    /* Make a shallow copy using the C type, so that no new runtime struct is
     * allocated for the copy. */
    ActionSlot shallow_copy = *slot;
    shallow_copy.runtime = nullptr;

    writer->write_struct_at_address(slot, &shallow_copy);
  }
}

/**
 * Create a listbase from a Span of channel groups.
 *
 * \note this does NOT transfer ownership of the pointers. The ListBaseT should
 * not be freed, but given to
 * `action_blend_write_clear_legacy_channel_groups_listbase()` below.
 *
 * \warning This code is modifying actual '`Main`' data in-place, which is
 * usually not acceptable (due to risks of unsafe concurrent accesses mainly).
 * The reasons why this is currently seen as 'reasonably safe' are:
 *   - Current blender code is _not_ expected to access the affected bActionGroup data
 *     (`prev`/`next` listbase pointers) in any way, as they are stored in an array.
 *   - The `action.groups` listbase modification is safe/valid, as this is a member of
 *     the Action ID, which is a shallow copy of the actual ID data from Main.
 */
static void action_blend_write_make_legacy_channel_groups_listbase(
    ListBaseT<bActionGroup> &listbase, const Span<bActionGroup *> channel_groups)
{
  if (channel_groups.is_empty()) {
    listbase.clear_no_delete();
    return;
  }

  /* Set the fcurve listbase pointers.
   *
   * Note that the fcurves' own prev/next pointers are hooked up by
   * `action_blend_write_make_legacy_fcurves_listbase()`, so that they function
   * properly as a list. */
  for (bActionGroup *group : channel_groups) {
    Span<FCurve *> fcurves = group->wrap().fcurves();
    if (fcurves.is_empty()) {
      group->channels = {nullptr, nullptr};
    }
    else {
      group->channels = {fcurves.first(), fcurves.last()};
    }
  }

  /* Determine the prev/next pointers on the elements. */
  const int last_index = channel_groups.size() - 1;
  for (int index : channel_groups.index_range()) {
    channel_groups[index]->prev = (index > 0) ? channel_groups[index - 1] : nullptr;
    channel_groups[index]->next = (index < last_index) ? channel_groups[index + 1] : nullptr;
  }

  listbase.first = channel_groups[0];
  listbase.last = channel_groups[last_index];
}

static void action_blend_write_clear_legacy_channel_groups_listbase(
    ListBaseT<bActionGroup> &listbase)
{
  for (bActionGroup &group : listbase.items_mutable()) {
    group.prev = nullptr;
    group.next = nullptr;
    group.channels = {nullptr, nullptr};
  }

  listbase.clear_no_delete();
}

/**
 * Create a listbase from a Span of F-Curves.
 *
 * \note this does NOT transfer ownership of the pointers. The ListBaseT should not be freed,
 * but given to `action_blend_write_clear_legacy_fcurves_listbase()` below.
 *
 * \warning This code is modifying actual '`Main`' data in-place, which is
 * usually not acceptable (due to risks of unsafe concurrent accesses mainly).
 * The reasons why this is currently seen as 'reasonably safe' are:
 *   - Current blender code is _not_ expected to access the affected FCurve data
 *     (`prev`/`next` listbase pointers) in any way, as they are stored in an array.
 *   - The `action.curves` listbase modification is safe/valid, as this is a member of
 *     the Action ID, which is a shallow copy of the actual ID data from Main.
 */
static void action_blend_write_make_legacy_fcurves_listbase(ListBaseT<FCurve> &listbase,
                                                            const Span<FCurve *> fcurves)
{
  if (fcurves.is_empty()) {
    listbase.clear_no_delete();
    return;
  }

  /* Determine the prev/next pointers on the elements. */
  const int last_index = fcurves.size() - 1;
  for (int index : fcurves.index_range()) {
    fcurves[index]->prev = (index > 0) ? fcurves[index - 1] : nullptr;
    fcurves[index]->next = (index < last_index) ? fcurves[index + 1] : nullptr;
  }

  listbase.first = fcurves[0];
  listbase.last = fcurves[last_index];
}

static void action_blend_write_clear_legacy_fcurves_listbase(ListBaseT<FCurve> &listbase)
{
  for (FCurve &fcurve : listbase.items_mutable()) {
    fcurve.prev = nullptr;
    fcurve.next = nullptr;
  }

  listbase.clear_no_delete();
}

static void action_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  animrig::Action &action = reinterpret_cast<bAction *>(id)->wrap();

  /* Create legacy data for Layered Actions: the F-Curves from the first Slot,
   * bottom layer, first Keyframe strip. */
  const bool do_write_forward_compat = !BLO_write_is_undo(writer) && action.slot_array_num > 0;
  if (do_write_forward_compat) {
    animrig::assert_baklava_phase_1_invariants(action);
    BLI_assert_msg(action.curves.is_empty(), "Layered Action should not have legacy data");
    BLI_assert_msg(action.groups.is_empty(), "Layered Action should not have legacy data");

    const animrig::Slot &first_slot = *action.slot(0);

    /* The forward-compat animation data we write is for IDs of the type that
     * the first slot is intended for. Therefore, the Action should have that
     * `idroot` when loaded in old versions of Blender.
     *
     * Note that if there is no slot, this code will never run and therefore the
     * action will be written with `idroot = 0`. Despite that, old
     * pre-slotted-action files are still guaranteed to round-trip losslessly,
     * because old actions (even when empty) are versioned to have one slot with
     * `idtype` set to whatever the old action's `idroot` was. In other words,
     * zero-slot actions can only be created via non-legacy features, and
     * therefore represent animation data that wasn't purely from old files
     * anyway. */
    action.idroot = first_slot.idtype;

    /* Note: channel group forward-compat data requires that fcurve
     * forward-compat legacy data is also written, and vice-versa. Both have
     * pointers to each other that won't resolve properly when loaded in older
     * Blender versions if only one is written. */
    animrig::Channelbag *bag = channelbag_for_action_slot(action, first_slot.handle);
    if (bag) {
      action_blend_write_make_legacy_fcurves_listbase(action.curves, bag->fcurves());
      action_blend_write_make_legacy_channel_groups_listbase(action.groups, bag->channel_groups());
    }
  }

  writer->write_id_struct(id_address, static_cast<const bAction *>(&action));
  BKE_id_blend_write(writer, &action.id);

  /* Write layered Action data. */
  write_strip_keyframe_data_array(writer, action.strip_keyframe_data());
  write_layers(writer, action.layers());
  write_slots(writer, action.slots());

  if (do_write_forward_compat) {
    /* Set the idroot back to 'unspecified', as it always should be for layered
     * Actions. */
    action.idroot = 0;

    /* The pointers to the first/last FCurve in the `action.curves` have already
     * been written as part of the Action struct data, so they can be cleared
     * here, such that the code writing legacy fcurves below does nothing (as
     * expected). And to leave the Action in a consistent state (it shouldn't
     * have F-Curves in both legacy and layered storage).
     *
     * Note that the FCurves themselves have been written as part of the layered
     * animation writing code called above. Writing them again as part of the
     * handling of the legacy `action.fcurves` ListBaseT would corrupt the
     * blend-file by generating two `BHead` `DATA` blocks with the same old
     * address for the same ID.
     */
    action_blend_write_clear_legacy_channel_groups_listbase(action.groups);
    action_blend_write_clear_legacy_fcurves_listbase(action.curves);
  }

  /* Write legacy F-Curves & Groups. */
  BKE_fcurve_blend_write_listbase(writer, &action.curves);
  for (bActionGroup &grp : action.groups) {
    writer->write_struct(&grp);
  }

  BKE_time_markers_blend_write(writer, action.markers);

  BKE_previewimg_blend_write(writer, action.preview);
}

static void read_channelbag(BlendDataReader *reader, animrig::Channelbag &channelbag)
{
  BLO_read_pointer_array_and_validate_size(
      reader, &channelbag.group_array, &channelbag.group_array_num);
  for (int i = 0; i < channelbag.group_array_num; i++) {
    BLO_read_struct(reader, bActionGroup, &channelbag.group_array[i]);
    channelbag.group_array[i]->channelbag = &channelbag;

    /* Clear the legacy channels #ListBase, since it will have been set for some
     * groups for forward compatibility.
     * See #action_blend_write_make_legacy_channel_groups_listbase. */
    channelbag.group_array[i]->channels = {nullptr, nullptr};
  }

  BLO_read_pointer_array_and_validate_size(
      reader, &channelbag.fcurve_array, &channelbag.fcurve_array_num);
  for (int i = 0; i < channelbag.fcurve_array_num; i++) {
    BLO_read_struct(reader, FCurve, &channelbag.fcurve_array[i]);
    FCurve *fcurve = channelbag.fcurve_array[i];

    /* Clear the prev/next pointers set by the forward compatibility code in
     * action_blend_write(). */
    fcurve->prev = nullptr;
    fcurve->next = nullptr;

    BKE_fcurve_blend_read_data(reader, fcurve);
  }
}

static void read_strip_keyframe_data(BlendDataReader *reader,
                                     animrig::StripKeyframeData &strip_keyframe_data)
{
  BLO_read_pointer_array_and_validate_size(
      reader, &strip_keyframe_data.channelbag_array, &strip_keyframe_data.channelbag_array_num);

  for (int i = 0; i < strip_keyframe_data.channelbag_array_num; i++) {
    BLO_read_struct(reader, ActionChannelbag, &strip_keyframe_data.channelbag_array[i]);
    ActionChannelbag *channelbag = strip_keyframe_data.channelbag_array[i];
    read_channelbag(reader, channelbag->wrap());
  }
}

static void read_strip_keyframe_data_array(BlendDataReader *reader, animrig::Action &action)
{
  BLO_read_pointer_array_and_validate_size(
      reader, &action.strip_keyframe_data_array, &action.strip_keyframe_data_array_num);

  for (int i = 0; i < action.strip_keyframe_data_array_num; i++) {
    BLO_read_struct(reader, ActionStripKeyframeData, &action.strip_keyframe_data_array[i]);
    ActionStripKeyframeData *keyframe_data = action.strip_keyframe_data_array[i];
    read_strip_keyframe_data(reader, keyframe_data->wrap());
  }
}

static void read_layers(BlendDataReader *reader, animrig::Action &action)
{
  BLO_read_pointer_array_and_validate_size(reader, &action.layer_array, &action.layer_array_num);

  for (int layer_idx = 0; layer_idx < action.layer_array_num; layer_idx++) {
    BLO_read_struct(reader, ActionLayer, &action.layer_array[layer_idx]);
    ActionLayer *layer = action.layer_array[layer_idx];

    BLO_read_pointer_array_and_validate_size(reader, &layer->strip_array, &layer->strip_array_num);
    for (int strip_idx = 0; strip_idx < layer->strip_array_num; strip_idx++) {
      BLO_read_struct(reader, ActionStrip, &layer->strip_array[strip_idx]);

      /* This if statement and the code in it is only for a transitional period
       * while we land #126559 and for a while after, to prevent crashes for
       * people that were already playing with slotted actions and have some
       * blend files written with them. This code can be removed after a while.
       * At the very least, if you're reading this and slotted actions are
       * already in an official release of Blender then this code is no longer
       * relevant and can be deleted. */
      if (layer->strip_array[strip_idx] == nullptr) {
        layer->strip_array[strip_idx] = &animrig::Strip::create(action,
                                                                animrig::Strip::Type::Keyframe);
      }
    }
  }
}

static void read_slots(BlendDataReader *reader, animrig::Action &action)
{
  BLO_read_pointer_array_and_validate_size(reader, &action.slot_array, &action.slot_array_num);

  for (int i = 0; i < action.slot_array_num; i++) {
    BLO_read_struct(reader, ActionSlot, &action.slot_array[i]);

    /* NOTE: this is endianness-sensitive. */
    /* In case of required endian switching, this code would have to undo the generic endian
     * switching, as the ID type values are not numerically the same between little and big endian
     * machines. Due to the way they are defined, they are always in the same byte order,
     * regardless of hardware/platform endianness. */

    action.slot_array[i]->wrap().blend_read_post();
  }
}

static void action_blend_read_data(BlendDataReader *reader, ID *id)
{
  animrig::Action &action = reinterpret_cast<bAction *>(id)->wrap();

  /* NOTE: this is endianness-sensitive. */
  /* In case of required endianness switching, this code would need to undo the generic endian
   * switching (careful, only the two least significant bytes of the int32 must be swapped back
   * here, since this value is actually an int16). */

  read_strip_keyframe_data_array(reader, action);
  read_layers(reader, action);
  read_slots(reader, action);

  if (animrig::versioning::action_is_layered(action)) {
    /* Clear the forward-compatible storage (see action_blend_write_data()). */
    action.curves.clear_no_delete();
    action.groups.clear_no_delete();

    /* Layered actions should always have `idroot == 0`, but when writing an
     * action to a blend file `idroot` is typically set otherwise for forward
     * compatibility reasons (see `action_blend_write()`). So we set it to zero
     * here to put it back as it should be. */
    action.idroot = 0;
  }
  else {
    /* Read legacy data. */
    BLO_read_struct_list(reader, FCurve, &action.curves);
    BLO_read_struct_list(reader, bActionGroup, &action.groups);

    BKE_fcurve_blend_read_data_listbase(reader, &action.curves);

    for (bActionGroup &agrp : action.groups) {
      BLO_read_struct(reader, FCurve, &agrp.channels.first);
      BLO_read_struct(reader, FCurve, &agrp.channels.last);
    }
  }

  BKE_time_markers_blend_read(reader, action.markers);

  /* End of reading legacy data. */

  BLO_read_struct(reader, PreviewImage, &action.preview);
  BKE_previewimg_blend_read(reader, action.preview);
}

static IDProperty *action_asset_type_property(const bAction *action)
{
  const bool is_single_frame = action && action->wrap().has_single_frame();
  return bke::idprop::create("is_single_frame", int(is_single_frame)).release();
}

static void action_asset_metadata_ensure(void *asset_ptr, AssetMetaData *asset_data)
{
  bAction *action = static_cast<bAction *>(asset_ptr);
  BLI_assert(GS(action->id.name) == ID_AC);

  IDProperty *action_type = action_asset_type_property(action);
  BKE_asset_metadata_idprop_ensure(asset_data, action_type);
}

static AssetTypeInfo AssetType_AC = {
    /*pre_save_fn*/ action_asset_metadata_ensure,
    /*on_mark_asset_fn*/ action_asset_metadata_ensure,
    /*on_clear_asset_fn*/ nullptr,
};

}  // namespace bke

IDTypeInfo IDType_ID_AC = {
    .id_code = bAction::id_type,
    .id_filter = FILTER_ID_AC,

    /* This value will be set dynamically in `BKE_idtype_init()` to only include
     * animatable ID types (see `animrig::Slot::users()`). */
    .dependencies_id_types = FILTER_ID_ALL,

    .main_listbase_index = INDEX_ID_AC,
    .struct_size = sizeof(bAction),
    .name = "Action",
    .name_plural = N_("actions"),
    .translation_context = BLT_I18NCONTEXT_ID_ACTION,
    .flags = IDTYPE_FLAGS_NO_ANIMDATA,
    .asset_type_info = &bke::AssetType_AC,

    .init_data = bke::action_init_data,
    .copy_data = bke::action_copy_data,
    .free_data = bke::action_free_data,
    .make_local = nullptr,
    .foreach_id = bke::action_foreach_id,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = nullptr,
    .owner_pointer_get = nullptr,

    .blend_write = bke::action_blend_write,
    .blend_read_data = bke::action_blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

/* ***************** Library data level operations on action ************** */

bAction *BKE_action_add(Main *bmain, const char name[])
{
  bAction *act;

  act = BKE_id_new<bAction>(bmain, name);

  return act;
}

/* .................................. */

/* *************** Action Groups *************** */

void action_group_colors_sync(bActionGroup *grp)
{
  /* Only do color copying if using a custom color (i.e. not default color). */
  if (!grp->customCol) {
    return;
  }
  if (grp->customCol > 0) {
    /* Copy theme colors on-to group's custom color in case user tries to edit color. */
    const bTheme *btheme = static_cast<const bTheme *>(U.themes.first);
    const ThemeWireColor *col_set = &btheme->color_sets[(grp->customCol - 1)];

    memcpy(&grp->cs, col_set, sizeof(ThemeWireColor));
  }
  else {
    /* Init custom color with a generic/placeholder color set if
     * no previous theme color was used that we can just keep using.
     */
    if (grp->cs.solid[0] == 0) {
      /* define for setting colors in theme below */
      rgba_uchar_args_set(grp->cs.solid, 0xff, 0x00, 0x00, 255);
      rgba_uchar_args_set(grp->cs.select, 0x81, 0xe6, 0x14, 255);
      rgba_uchar_args_set(grp->cs.active, 0x18, 0xb6, 0xe0, 255);
    }
  }
}

void what_does_obaction(Object *ob,
                        Object *workob,
                        bAction *act,
                        const int32_t action_slot_handle,
                        char groupname[],
                        const AnimationEvalContext *anim_eval_context)
{
  using namespace blender::animrig;
  BLI_assert(act);

  bActionGroup *agrp = nullptr;
  if (groupname && groupname[0]) {
    /* Find the named channel group. */
    Action &action = act->wrap();
    Channelbag *cbag = channelbag_for_action_slot(action, action_slot_handle);
    agrp = cbag ? cbag->channel_group_find(groupname) : nullptr;
  }

  /* clear workob */
  bke::ObjectRuntime workob_runtime;
  BKE_object_workob_clear(workob);
  workob->runtime = &workob_runtime;

  /* init workob */
  copy_m4_m4(workob->runtime->object_to_world.ptr(), ob->object_to_world().ptr());
  copy_m4_m4(workob->constinv, ob->constinv);

  workob->rotmode = ob->rotmode;

  workob->trackflag = ob->trackflag;
  workob->upflag = ob->upflag;

  workob->constraints.first = ob->constraints.first;
  workob->constraints.last = ob->constraints.last;

  /* we don't use real object name, otherwise RNA screws with the real thing */
  STRNCPY_UTF8(workob->id.name, "OB<ConstrWorkOb>");

  /* If we're given a group to use, it's likely to be more efficient
   * (though a bit more dangerous). */
  if (agrp) {
    /* specifically evaluate this group only */

    /* get RNA-pointer for the workob's ID */
    PointerRNA id_ptr = RNA_id_pointer_create(&workob->id);

    /* execute action for this group only */
    animsys_evaluate_action_group(&id_ptr, act, agrp, anim_eval_context);
  }
  else {
    AnimData adt = {nullptr};

    /* init animdata, and attach to workob */
    workob->adt = &adt;

    adt.action = act;
    adt.slot_handle = action_slot_handle;

    /* execute effects of Action on to workob (or its PoseChannels) */
    BKE_animsys_evaluate_animdata(&workob->id, &adt, anim_eval_context, ADT_RECALC_ANIM, false);

    /* Ensure stack memory set here isn't accessed later, relates to !118847. */
    workob->adt = nullptr;
  }
  /* Ensure stack memory set here isn't accessed later, see !118847. */
  workob->runtime = nullptr;
}

}  // namespace blender
