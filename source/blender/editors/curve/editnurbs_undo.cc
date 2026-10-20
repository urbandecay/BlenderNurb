/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "DNA_nurbs_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_object.hh"
#include "BKE_undo_system.hh"

#include "DEG_depsgraph.hh"

#include "ED_curve.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

/** We only need this locally. */
static CLG_LogRef LOG = {"undo.nurbs"};

namespace {

struct UndoNurbsGeometry {
  ListBaseT<NurbsCurve> curves;
  NurbsTopology *topology;
  uint64_t next_component_id;
  uint64_t active_control_vertex_id;
  size_t undo_size;
};

static void nurbs_geometry_snapshot_create(UndoNurbsGeometry &snapshot,
                                           const NurbsGeometry &geometry)
{
  snapshot.next_component_id = geometry.next_component_id;
  snapshot.active_control_vertex_id = geometry.active_control_vertex_id;
  snapshot.undo_size = sizeof(UndoNurbsGeometry) +
                       BKE_nurbs_curve_list_memory_size(geometry.curves);
  BKE_nurbs_curve_list_copy(snapshot.curves, geometry.curves);
  if (geometry.topology) {
    snapshot.topology = BKE_nurbs_topology_copy(*geometry.topology);
    snapshot.undo_size += BKE_nurbs_topology_memory_size(*geometry.topology);
  }
}

static void nurbs_geometry_snapshot_restore(const UndoNurbsGeometry &snapshot,
                                            NurbsGeometry &geometry)
{
  BKE_nurbs_curve_list_copy(geometry.curves, snapshot.curves);
  BKE_nurbs_topology_free(geometry.topology);
  geometry.topology = snapshot.topology ? BKE_nurbs_topology_copy(*snapshot.topology) : nullptr;
  geometry.next_component_id = snapshot.next_component_id;
  geometry.active_control_vertex_id = snapshot.active_control_vertex_id;

  BKE_nurbs_batch_cache_dirty_tag(&geometry, BKE_NURBS_BATCH_DIRTY_ALL);
}

static void nurbs_geometry_snapshot_free(UndoNurbsGeometry &snapshot)
{
  BKE_nurbs_curve_list_free(snapshot.curves);
  BKE_nurbs_topology_free(snapshot.topology);
  snapshot.topology = nullptr;
}

}  // namespace

struct NurbsUndoStep_Elem {
  UndoRefID_Object obedit_ref;
  UndoNurbsGeometry data;
};

struct NurbsUndoStep {
  UndoStep step;
  /** See #ED_undo_object_editmode_validate_scene_from_windows for details. */
  UndoRefID_Scene scene_ref;
  NurbsUndoStep_Elem *elems;
  uint elems_len;
};

static bool nurbs_undosys_poll(bContext *C)
{
  return ED_operator_editnurbs(C);
}

static bool nurbs_undosys_step_encode(bContext *C, Main *bmain, UndoStep *us_p)
{
  NurbsUndoStep *us = reinterpret_cast<NurbsUndoStep *>(us_p);

  /* Use the whole view-layer edit set so undo can restore multi-object Edit Mode. */
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = ED_undo_editmode_objects_from_view_layer(*bmain, scene, view_layer);
  if (objects.is_empty()) {
    return false;
  }

  us->scene_ref.ptr = scene;
  us->elems = MEM_new_array_zeroed<NurbsUndoStep_Elem>(objects.size(), __func__);
  us->elems_len = objects.size();

  for (const int i : objects.index_range()) {
    Object *object = objects[i];
    NurbsGeometry &geometry = *id_cast<NurbsGeometry *>(object->data);
    NurbsUndoStep_Elem &elem = us->elems[i];

    elem.obedit_ref.ptr = object;
    nurbs_geometry_snapshot_create(elem.data, geometry);
    us->step.data_size += elem.data.undo_size;
  }

  /* The authoritative geometry is already in the ID, but make global undo capture this state
   * when crossing between Edit Mode and memfile undo steps. */
  bmain->is_memfile_undo_flush_needed = true;
  return true;
}

static void nurbs_undosys_step_decode(
    bContext *C, Main *bmain, UndoStep *us_p, const eUndoStepDir /*dir*/, bool /*is_final*/)
{
  NurbsUndoStep *us = reinterpret_cast<NurbsUndoStep *>(us_p);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  ED_undo_object_editmode_validate_scene_from_windows(
      CTX_wm_manager(C), us->scene_ref.ptr, &scene, &view_layer);
  ED_undo_object_editmode_restore_helper(
      scene, view_layer, &us->elems[0].obedit_ref.ptr, us->elems_len, sizeof(*us->elems));

  BLI_assert(BKE_object_is_in_editmode(us->elems[0].obedit_ref.ptr));

  for (uint i = 0; i < us->elems_len; i++) {
    NurbsUndoStep_Elem &elem = us->elems[i];
    Object *obedit = elem.obedit_ref.ptr;
    if (obedit->data == nullptr || GS(static_cast<ID *>(obedit->data)->name) != ID_NU) {
      CLOG_ERROR(&LOG,
                 "name='%s', failed to restore NURBS data for object '%s', undo state invalid",
                 us_p->name,
                 obedit->id.name);
      continue;
    }

    NurbsGeometry &geometry = *id_cast<NurbsGeometry *>(obedit->data);
    nurbs_geometry_snapshot_restore(elem.data, geometry);
    DEG_id_tag_update(&geometry.id, ID_RECALC_GEOMETRY | ID_RECALC_SELECT);
  }

  /* The first element is always active. */
  ED_undo_object_set_active_or_warn(
      *bmain, scene, view_layer, us->elems[0].obedit_ref.ptr, us_p->name, &LOG);

  bmain->is_memfile_undo_flush_needed = true;
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, nullptr);
}

static void nurbs_undosys_step_free(UndoStep *us_p)
{
  NurbsUndoStep *us = reinterpret_cast<NurbsUndoStep *>(us_p);
  for (uint i = 0; i < us->elems_len; i++) {
    nurbs_geometry_snapshot_free(us->elems[i].data);
  }
  MEM_delete(us->elems);
}

static void nurbs_undosys_foreach_ID_ref(UndoStep *us_p,
                                         UndoTypeForEachIDRefFn foreach_ID_ref_fn,
                                         void *user_data)
{
  NurbsUndoStep *us = reinterpret_cast<NurbsUndoStep *>(us_p);

  foreach_ID_ref_fn(user_data, reinterpret_cast<UndoRefID *>(&us->scene_ref));
  for (uint i = 0; i < us->elems_len; i++) {
    foreach_ID_ref_fn(user_data, reinterpret_cast<UndoRefID *>(&us->elems[i].obedit_ref));
  }
}

void ED_nurbs_undosys_type(UndoType *ut)
{
  ut->name = "Edit NURBS";
  ut->poll = nurbs_undosys_poll;
  ut->step_encode = nurbs_undosys_step_encode;
  ut->step_decode = nurbs_undosys_step_decode;
  ut->step_free = nurbs_undosys_step_free;
  ut->step_foreach_ID_ref = nurbs_undosys_foreach_ID_ref;
  ut->flags = UNDOTYPE_FLAG_NEED_CONTEXT_FOR_ENCODE;
  ut->step_size = sizeof(NurbsUndoStep);
}

}  // namespace blender
