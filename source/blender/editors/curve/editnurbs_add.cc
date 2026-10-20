/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include "DNA_object_types.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_nurbs.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "ED_object.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "curve_intern.hh"

namespace blender {

static Object *nurbs_geometry_object_add(bContext *C,
                                         wmOperator *op,
                                         NurbsGeometry *geometry,
                                         const float size)
{
  bool enter_editmode;
  ushort local_view_bits;
  float loc[3], rot[3];
  WM_operator_view3d_unit_defaults(C, op);
  ed::object::add_generic_get_opts(
      C, op, 'Z', loc, rot, nullptr, &enter_editmode, &local_view_bits, nullptr);

  id_us_min(&geometry->id);
  Object *object = ed::object::add_type_with_obdata(
      C, OB_NURBS, nullptr, loc, rot, enter_editmode, local_view_bits, &geometry->id);
  if (size != 1.0f) {
    BKE_object_obdata_size_init(object, size);
  }
  DEG_id_tag_update(&geometry->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, object);
  return object;
}

static wmOperatorStatus nurbs_primitive_circle_add_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  const float radius = RNA_float_get(op->ptr, "radius");
  NurbsGeometry *geometry = BKE_nurbs_add_circle(bmain, "NurbsGeometry");
  nurbs_geometry_object_add(C, op, geometry, radius);
  return OPERATOR_FINISHED;
}

void NURBS_OT_primitive_circle_add(wmOperatorType *ot)
{
  ot->name = "Add NURBS Circle";
  ot->description = "Add an exact rational NURBS circle";
  ot->idname = "NURBS_OT_primitive_circle_add";
  ot->exec = nurbs_primitive_circle_add_exec;
  ot->poll = ED_operator_scene_editable;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ed::object::add_unit_props_radius(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus nurbs_primitive_line_add_exec(bContext *C, wmOperator *op)
{
  const double start[3] = {-1.0, 0.0, 0.0};
  const double end[3] = {1.0, 0.0, 0.0};
  NurbsGeometry *geometry = BKE_nurbs_add_line(
      CTX_data_main(C), "NurbsGeometry", start, end);
  if (geometry == nullptr) {
    return OPERATOR_CANCELLED;
  }
  nurbs_geometry_object_add(C, op, geometry, RNA_float_get(op->ptr, "radius"));
  return OPERATOR_FINISHED;
}

void NURBS_OT_primitive_line_add(wmOperatorType *ot)
{
  ot->name = "Add NURBS Line";
  ot->description = "Add an exact degree-one NURBS line";
  ot->idname = "NURBS_OT_primitive_line_add";
  ot->exec = nurbs_primitive_line_add_exec;
  ot->poll = ED_operator_scene_editable;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ed::object::add_unit_props_radius(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus nurbs_primitive_path_add_exec(bContext *C, wmOperator *op)
{
  NurbsGeometry *geometry = BKE_nurbs_add_path(CTX_data_main(C), "NurbsGeometry");
  nurbs_geometry_object_add(C, op, geometry, RNA_float_get(op->ptr, "radius"));
  return OPERATOR_FINISHED;
}

void NURBS_OT_primitive_path_add(wmOperatorType *ot)
{
  ot->name = "Add NURBS Path";
  ot->description = "Add a degree-four NURBS path with five control points";
  ot->idname = "NURBS_OT_primitive_path_add";
  ot->exec = nurbs_primitive_path_add_exec;
  ot->poll = ED_operator_scene_editable;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ed::object::add_unit_props_radius(ot);
  ed::object::add_generic_props(ot, true);
}

static wmOperatorStatus nurbs_primitive_arc_add_exec(bContext *C, wmOperator *op)
{
  const float radius = RNA_float_get(op->ptr, "radius");
  const double start_angle = RNA_float_get(op->ptr, "start_angle");
  const double sweep_angle = RNA_float_get(op->ptr, "sweep_angle");
  NurbsGeometry *geometry = BKE_nurbs_add_arc(
      CTX_data_main(C), "NurbsGeometry", radius, start_angle, sweep_angle);
  if (geometry == nullptr) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Arc radius must be positive and sweep must be non-zero and at most 360 degrees");
    return OPERATOR_CANCELLED;
  }
  nurbs_geometry_object_add(C, op, geometry, 1.0f);
  return OPERATOR_FINISHED;
}

void NURBS_OT_primitive_arc_add(wmOperatorType *ot)
{
  constexpr float two_pi = 6.28318530717958647692f;
  ot->name = "Add NURBS Arc";
  ot->description = "Add an exact rational circular NURBS arc";
  ot->idname = "NURBS_OT_primitive_arc_add";
  ot->exec = nurbs_primitive_arc_add_exec;
  ot->poll = ED_operator_scene_editable;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_float(ot->srna,
                                   "radius",
                                   1.0f,
                                   1e-6f,
                                   FLT_MAX,
                                   "Radius",
                                   "Radius of the exact circular arc",
                                   0.0f,
                                   100.0f);
  RNA_def_property_ui_range(prop, 1e-6f, 1000.0f, 1.0f, 3);
  prop = RNA_def_float(ot->srna,
                       "start_angle",
                       0.0f,
                       -two_pi,
                       two_pi,
                       "Start Angle",
                       "Angle where the arc begins",
                       -two_pi,
                       two_pi);
  RNA_def_property_subtype(prop, PROP_ANGLE);
  prop = RNA_def_float(ot->srna,
                       "sweep_angle",
                       DEG2RADF(90.0f),
                       -two_pi,
                       two_pi,
                       "Sweep Angle",
                       "Signed angle covered by the arc",
                       -two_pi,
                       two_pi);
  RNA_def_property_subtype(prop, PROP_ANGLE);
  ed::object::add_generic_props(ot, true);
}

}  // namespace blender
