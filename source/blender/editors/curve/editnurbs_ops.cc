/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include <cfloat>

#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_nurbs_edit.hh"

#include "DNA_curve_types.h"
#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "ED_nurbs.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "curve_intern.hh"

namespace blender {

static wmOperatorStatus nurbs_rip_invoke(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  return ed::nurbs::rip(C, event->mval) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void CURVE_OT_rip(wmOperatorType *ot)
{
  ot->name = "Rip NURBS";
  ot->idname = "CURVE_OT_rip";
  ot->description = "Disconnect a selected NURBS vertex or edge path on the cursor side";
  ot->invoke = nurbs_rip_invoke;
  ot->poll = ed::nurbs::topology_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;
}

static const EnumPropertyItem nurbs_merge_items[] = {
    {int(bke::nurbs::VertexMergeMode::Center),
     "CENTER",
     0,
     "At Center",
     "Merge at the selected vertices' mean position"},
    {int(bke::nurbs::VertexMergeMode::Cursor), "CURSOR", 0, "At Cursor", "Merge at the 3D cursor"},
    {int(bke::nurbs::VertexMergeMode::Collapse),
     "COLLAPSE",
     0,
     "Collapse",
     "Merge each connected selected edge group"},
    {int(bke::nurbs::VertexMergeMode::First),
     "FIRST",
     0,
     "At First",
     "Merge at the first selected vertex"},
    {int(bke::nurbs::VertexMergeMode::Last),
     "LAST",
     0,
     "At Last",
     "Merge at the last selected vertex"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem *nurbs_merge_items_get(bContext *C,
                                                     PointerRNA *,
                                                     PropertyRNA *,
                                                     bool *r_free)
{
  if (!C) {
    return nurbs_merge_items;
  }
  EnumPropertyItem *items = nullptr;
  int count = 0;
  for (const auto mode : {bke::nurbs::VertexMergeMode::Center,
                          bke::nurbs::VertexMergeMode::Cursor,
                          bke::nurbs::VertexMergeMode::Collapse})
  {
    RNA_enum_items_add_value(&items, &count, nurbs_merge_items, int(mode));
  }
  const Object *object = CTX_data_edit_object(C);
  const Curve *curve = object ? id_cast<const Curve *>(object->data) : nullptr;
  if (curve && curve->editnurb && curve->editnurb->nurbs_edit) {
    const auto source = curve->editnurb->nurbs_edit->read_snapshot();
    const auto &history = source.selection().history;
    if (!history.is_empty()) {
      if (history.first().domain == bke::nurbs::ElementDomain::Vertex) {
        RNA_enum_items_add_value(
            &items, &count, nurbs_merge_items, int(bke::nurbs::VertexMergeMode::First));
      }
      if (history.last().domain == bke::nurbs::ElementDomain::Vertex) {
        RNA_enum_items_add_value(
            &items, &count, nurbs_merge_items, int(bke::nurbs::VertexMergeMode::Last));
      }
    }
  }
  RNA_enum_item_end(&items, &count);
  *r_free = true;
  return items;
}

static wmOperatorStatus nurbs_merge_exec(bContext *C, wmOperator *op)
{
  return ed::nurbs::merge(C,
                          bke::nurbs::VertexMergeMode(RNA_enum_get(op->ptr, "type")),
                          RNA_float_get(op->ptr, "max_error")) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

void CURVE_OT_merge(wmOperatorType *ot)
{
  ot->name = "Merge NURBS";
  ot->idname = "CURVE_OT_merge";
  ot->description = "Merge selected source vertices and repair NURBS topology";
  ot->exec = nurbs_merge_exec;
  ot->invoke = WM_menu_invoke;
  ot->poll = ed::nurbs::topology_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          nurbs_merge_items,
                          int(bke::nurbs::VertexMergeMode::Center),
                          "Type",
                          "Merge method");
  RNA_def_enum_funcs(ot->prop, nurbs_merge_items_get);
  WM_operatortype_props_advanced_begin(ot);
  RNA_def_float(ot->srna,
                "max_error",
                1.0e-9f,
                0,
                FLT_MAX,
                "Maximum Error",
                "Maximum allowed source construction roundoff, in object units",
                0,
                0.001f);
}

static wmOperatorStatus nurbs_dissolve_limited_exec(bContext *C, wmOperator *op)
{
  return ed::nurbs::dissolve_limited(C, RNA_float_get(op->ptr, "angle_limit")) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

void CURVE_OT_dissolve_limited(wmOperatorType *ot)
{
  ot->name = "Limited Dissolve";
  ot->idname = "CURVE_OT_dissolve_limited";
  ot->description = "Dissolve redundant NURBS edges and vertices while preserving the surface shape";
  ot->exec = nurbs_dissolve_limited_exec;
  ot->poll = ed::nurbs::topology_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_float_rotation(ot->srna,
                                              "angle_limit",
                                              0,
                                              nullptr,
                                              0.0f,
                                              DEG2RADF(90.0f),
                                              "Max Angle",
                                              "Maximum angle between adjacent face normals",
                                              0.0f,
                                              DEG2RADF(90.0f));
  RNA_def_property_float_default(prop, DEG2RADF(5.0f));
}

static wmOperatorStatus nurbs_remove_doubles_exec(bContext *C, wmOperator *op)
{
  return ed::nurbs::merge_by_distance(C,
                                      RNA_float_get(op->ptr, "threshold"),
                                      RNA_boolean_get(op->ptr, "use_centroid"),
                                      RNA_boolean_get(op->ptr, "use_unselected"),
                                      RNA_float_get(op->ptr, "max_error")) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

void CURVE_OT_remove_doubles(wmOperatorType *ot)
{
  ot->name = "Merge by Distance";
  ot->idname = "CURVE_OT_remove_doubles";
  ot->description = "Merge native NURBS vertices based on their proximity";
  ot->exec = nurbs_remove_doubles_exec;
  ot->poll = ed::nurbs::topology_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_float_distance(ot->srna,
                         "threshold",
                         1e-4f,
                         1e-6f,
                         50.0f,
                         "Merge Distance",
                         "Maximum distance between elements to merge",
                         1e-5f,
                         10.0f);
  RNA_def_boolean(ot->srna,
                  "use_centroid",
                  true,
                  "Centroid Merge",
                  "Move vertices to the centroid of the duplicate cluster, otherwise retain "
                  "the vertex closest to its centroid");
  RNA_def_boolean(ot->srna,
                  "use_unselected",
                  false,
                  "Unselected",
                  "Merge selected to other unselected vertices");
  WM_operatortype_props_advanced_begin(ot);
  RNA_def_float(ot->srna,
                "max_error",
                1e-9f,
                0,
                FLT_MAX,
                "Maximum Error",
                "Maximum allowed source construction roundoff, in object units",
                0,
                0.001f);
}

}  // namespace blender
