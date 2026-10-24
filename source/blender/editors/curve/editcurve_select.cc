/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_bitmap.h"
#include "BLI_ghash.h"
#include "BLI_heap_simple.h"
#include "BLI_kdtree.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_rand.h"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_curve_topology.hh"
#include "BKE_fcurve.hh"
#include "BKE_layer.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_report.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_curve.hh"
#include "ED_nurbs.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_view3d.hh"

#include "curve_intern.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "UI_resources.hh"

#include "DEG_depsgraph.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

bool select_beztriple(BezTriple *bezt, bool selstatus, eBezTriple_Flag flag, eVisible_Types hidden)
{
  if ((bezt->hide == 0) || (hidden == HIDDEN)) {
    if (selstatus) { /* selects */
      bezt->f1 |= flag;
      bezt->f2 |= flag;
      bezt->f3 |= flag;
      return true;
    }
    /* deselects */
    bezt->f1 &= ~flag;
    bezt->f2 &= ~flag;
    bezt->f3 &= ~flag;
    return true;
  }

  return false;
}

bool select_bpoint(BPoint *bp, bool selstatus, eBezTriple_Flag flag, bool hidden)
{
  if ((bp->hide == 0) || (hidden == 1)) {
    if (selstatus) {
      bp->f1 |= flag;
      return true;
    }
    bp->f1 &= ~flag;
    return true;
  }

  return false;
}

static bool swap_selection_beztriple(BezTriple *bezt)
{
  if (bezt->f2 & SELECT) {
    return select_beztriple(bezt, false, BEZT_FLAG_SELECT, VISIBLE);
  }
  return select_beztriple(bezt, true, BEZT_FLAG_SELECT, VISIBLE);
}

static bool swap_selection_bpoint(BPoint *bp)
{
  if (bp->f1 & SELECT) {
    return select_bpoint(bp, false, BEZT_FLAG_SELECT, VISIBLE);
  }
  return select_bpoint(bp, true, BEZT_FLAG_SELECT, VISIBLE);
}

bool ED_curve_nurb_select_check(const View3D *v3d, const Nurb *nu)
{
  if (nu->type == CU_BEZIER) {
    const BezTriple *bezt;
    int i;

    for (i = nu->pntsu, bezt = nu->bezt; i--; bezt++) {
      if (BEZT_ISSEL_ANY_HIDDENHANDLES(v3d, bezt)) {
        return true;
      }
    }
  }
  else {
    const BPoint *bp;
    int i;

    for (i = nu->pntsu * nu->pntsv, bp = nu->bp; i--; bp++) {
      if (bp->f1 & SELECT) {
        return true;
      }
    }
  }
  return false;
}

int ED_curve_nurb_select_count(const View3D *v3d, const Nurb *nu)
{
  int sel = 0;

  if (nu->type == CU_BEZIER) {
    const BezTriple *bezt;
    int i;

    for (i = nu->pntsu, bezt = nu->bezt; i--; bezt++) {
      if (BEZT_ISSEL_ANY_HIDDENHANDLES(v3d, bezt)) {
        sel++;
      }
    }
  }
  else {
    const BPoint *bp;
    int i;

    for (i = nu->pntsu * nu->pntsv, bp = nu->bp; i--; bp++) {
      if (bp->f1 & SELECT) {
        sel++;
      }
    }
  }

  return sel;
}

bool ED_curve_nurb_select_all(const Nurb *nu)
{
  bool changed = false;
  int i;
  if (nu->bezt) {
    BezTriple *bezt;
    for (i = nu->pntsu, bezt = nu->bezt; i--; bezt++) {
      if (bezt->hide == 0) {
        if (BEZT_ISSEL_ALL(bezt) == false) {
          BEZT_SEL_ALL(bezt);
          changed = true;
        }
      }
    }
  }
  else if (nu->bp) {
    BPoint *bp;
    for (i = nu->pntsu * nu->pntsv, bp = nu->bp; i--; bp++) {
      if (bp->hide == 0) {
        if ((bp->f1 & SELECT) == 0) {
          bp->f1 |= SELECT;
          changed = true;
        }
      }
    }
  }
  return changed;
}

bool ED_curve_select_all(EditNurb *editnurb)
{
  bool changed = false;
  for (Nurb &nu : editnurb->nurbs) {
    changed |= ED_curve_nurb_select_all(&nu);
  }
  return changed;
}

bool ED_curve_nurb_deselect_all(const Nurb *nu)
{
  bool changed = false;
  int i;
  if (nu->bezt) {
    BezTriple *bezt;
    for (i = nu->pntsu, bezt = nu->bezt; i--; bezt++) {
      if (BEZT_ISSEL_ANY(bezt)) {
        BEZT_DESEL_ALL(bezt);
        changed = true;
      }
    }
  }
  else if (nu->bp) {
    BPoint *bp;
    for (i = nu->pntsu * nu->pntsv, bp = nu->bp; i--; bp++) {
      if (bp->f1 & SELECT) {
        bp->f1 &= ~SELECT;
        changed = true;
      }
    }
  }
  return changed;
}

int ED_curve_select_count(const View3D *v3d, const EditNurb *editnurb)
{
  int sel = 0;

  for (Nurb &nu : editnurb->nurbs) {
    sel += ED_curve_nurb_select_count(v3d, &nu);
  }

  return sel;
}

bool ED_curve_select_check(const View3D *v3d, const EditNurb *editnurb)
{
  for (const Nurb &nu : editnurb->nurbs) {
    if (ED_curve_nurb_select_check(v3d, &nu)) {
      return true;
    }
  }

  return false;
}

bool ED_curve_deselect_all(EditNurb *editnurb)
{
  bool changed = false;
  for (Nurb &nu : editnurb->nurbs) {
    changed |= ED_curve_nurb_deselect_all(&nu);
  }
  return changed;
}

bool ED_curve_deselect_all_multi_ex(Span<Base *> bases)
{
  bool changed_multi = false;
  for (Base *base : bases) {
    Object *obedit = base->object;
    Curve *cu = id_cast<Curve *>(obedit->data);
    changed_multi |= ED_curve_deselect_all(cu->editnurb);
    DEG_id_tag_update(&cu->id, ID_RECALC_SELECT);
  }
  return changed_multi;
}

bool ED_curve_deselect_all_multi(bContext *C)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  Vector<Base *> bases = BKE_view_layer_array_from_bases_in_edit_mode_unique_data(
      *vc.bmain, vc.scene, vc.view_layer, vc.v3d);
  return ED_curve_deselect_all_multi_ex(bases);
}

bool ED_curve_select_swap(EditNurb *editnurb, bool hide_handles)
{
  BPoint *bp;
  BezTriple *bezt;
  int a;
  bool changed = false;

  /* This could be an argument to swap individual handle selection.
   * At the moment this is always used though. */
  bool swap_handles = false;

  /* When hiding handles, ignore handle selection. */
  if (hide_handles) {
    swap_handles = true;
  }

  for (Nurb &nu : editnurb->nurbs) {
    if (nu.type == CU_BEZIER) {
      bezt = nu.bezt;
      a = nu.pntsu;
      while (a--) {
        if (bezt->hide == 0) {
          if (swap_handles) {
            bezt->f2 ^= BEZT_FLAG_SELECT; /* always do the center point */
            if (!hide_handles) {
              bezt->f1 ^= BEZT_FLAG_SELECT;
              bezt->f3 ^= BEZT_FLAG_SELECT;
            }
          }
          else {
            BLI_assert(!hide_handles);
            if (BEZT_ISSEL_ANY(bezt)) {
              BEZT_DESEL_ALL(bezt);
            }
            else {
              BEZT_SEL_ALL(bezt);
            }
          }
          changed = true;
        }
        bezt++;
      }
    }
    else {
      bp = nu.bp;
      a = nu.pntsu * nu.pntsv;
      while (a--) {
        if (bp->hide == 0) {
          swap_selection_bpoint(bp);
          changed = true;
        }
        bp++;
      }
    }
  }
  return changed;
}

/**
 * \param next: -1/1 for prev/next
 * \param cont: when true select continuously
 * \param selstatus: inverts behavior
 */
static void select_adjacent_cp(ListBaseT<Nurb> *editnurb,
                               short next,
                               const bool cont,
                               const bool selstatus)
{
  BezTriple *bezt;
  BPoint *bp;
  int a;
  bool lastsel = false;

  if (next == 0) {
    return;
  }

  for (Nurb &nu : *editnurb) {
    lastsel = false;
    if (nu.type == CU_BEZIER) {
      a = nu.pntsu;
      bezt = nu.bezt;
      if (next < 0) {
        bezt = &nu.bezt[a - 1];
      }
      while (a--) {
        if (a - abs(next) < 0) {
          break;
        }
        if ((lastsel == false) && (bezt->hide == 0) &&
            ((bezt->f2 & SELECT) || (selstatus == false)))
        {
          bezt += next;
          if (!(bezt->f2 & SELECT) || (selstatus == false)) {
            bool sel = select_beztriple(bezt, selstatus, BEZT_FLAG_SELECT, VISIBLE);
            if (sel && !cont) {
              lastsel = true;
            }
          }
        }
        else {
          bezt += next;
          lastsel = false;
        }
        /* move around in zigzag way so that we go through each */
        bezt -= (next - next / abs(next));
      }
    }
    else {
      a = nu.pntsu * nu.pntsv;
      bp = nu.bp;
      if (next < 0) {
        bp = &nu.bp[a - 1];
      }
      while (a--) {
        if (a - abs(next) < 0) {
          break;
        }
        if ((lastsel == false) && (bp->hide == 0) && ((bp->f1 & SELECT) || (selstatus == false))) {
          bp += next;
          if (!(bp->f1 & SELECT) || (selstatus == false)) {
            bool sel = select_bpoint(bp, selstatus, BEZT_FLAG_SELECT, VISIBLE);
            if (sel && !cont) {
              lastsel = true;
            }
          }
        }
        else {
          bp += next;
          lastsel = false;
        }
        /* move around in zigzag way so that we go through each */
        bp -= (next - next / abs(next));
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Start/End Operators
 * \{ */

/**
 * (De)selects first or last of visible part of each #Nurb depending on `selfirst`.
 *
 * \param selfirst: defines the end of which to select.
 * \param doswap: defines if selection state of each first/last control point is swapped.
 * \param selstatus: selection status in case `doswap` is false.
 */
static void selectend_nurb(Object *obedit, eEndPoint_Types selfirst, bool doswap, bool selstatus)
{
  ListBaseT<Nurb> *editnurb = object_editcurve_get(obedit);
  BPoint *bp;
  BezTriple *bezt;
  Curve *cu;
  int a;

  if (obedit == nullptr) {
    return;
  }

  cu = id_cast<Curve *>(obedit->data);
  cu->actvert = CU_ACT_NONE;

  for (Nurb &nu : *editnurb) {
    if (nu.type == CU_BEZIER) {
      a = nu.pntsu;

      /* which point? */
      if (selfirst == LAST) { /* select last */
        bezt = &nu.bezt[a - 1];
      }
      else { /* select first */
        bezt = nu.bezt;
      }

      while (a--) {
        bool sel;
        if (doswap) {
          sel = swap_selection_beztriple(bezt);
        }
        else {
          sel = select_beztriple(bezt, selstatus, BEZT_FLAG_SELECT, VISIBLE);
        }

        if (sel == true) {
          break;
        }
      }
    }
    else {
      a = nu.pntsu * nu.pntsv;

      /* which point? */
      if (selfirst == LAST) { /* select last */
        bp = &nu.bp[a - 1];
      }
      else { /* select first */
        bp = nu.bp;
      }

      while (a--) {
        if (bp->hide == 0) {
          bool sel;
          if (doswap) {
            sel = swap_selection_bpoint(bp);
          }
          else {
            sel = select_bpoint(bp, selstatus, BEZT_FLAG_SELECT, VISIBLE);
          }

          if (sel == true) {
            break;
          }
        }
      }
    }
  }
}

static wmOperatorStatus de_select_first_exec(bContext *C, wmOperator * /*op*/)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    selectend_nurb(obedit, FIRST, true, false);
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
    BKE_curve_nurb_vert_active_validate(id_cast<Curve *>(obedit->data));
  }
  return OPERATOR_FINISHED;
}

void CURVE_OT_de_select_first(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "(De)select First";
  ot->idname = "CURVE_OT_de_select_first";
  ot->description = "(De)select first of visible part of each NURBS";

  /* API callbacks. */
  ot->exec = de_select_first_exec;
  ot->poll = ED_operator_editcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus de_select_last_exec(bContext *C, wmOperator * /*op*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    selectend_nurb(obedit, LAST, true, false);
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
    BKE_curve_nurb_vert_active_validate(id_cast<Curve *>(obedit->data));
  }

  return OPERATOR_FINISHED;
}

void CURVE_OT_de_select_last(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "(De)select Last";
  ot->idname = "CURVE_OT_de_select_last";
  ot->description = "(De)select last of visible part of each NURBS";

  /* API callbacks. */
  ot->exec = de_select_last_exec;
  ot->poll = ED_operator_editcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select All Operator
 * \{ */

static wmOperatorStatus de_select_all_exec(bContext *C, wmOperator *op)
{
  int action = RNA_enum_get(op->ptr, "action");

  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));
  if (action == SEL_TOGGLE) {
    action = SEL_SELECT;
    for (Object *obedit : objects) {
      Curve *cu = id_cast<Curve *>(obedit->data);

      const auto *edit = cu->editnurb->nurbs_edit;
      const bool native_selected = edit != nullptr && (!edit->selection().vertices.is_empty() ||
                                                       !edit->selection().edges.is_empty() ||
                                                       !edit->selection().faces.is_empty());
      if (native_selected || ED_curve_select_check(v3d, cu->editnurb)) {
        action = SEL_DESELECT;
        break;
      }
    }
  }

  for (Object *obedit : objects) {
    Curve *cu = id_cast<Curve *>(obedit->data);
    bool changed = false;

    if (cu->editnurb->nurbs_edit != nullptr) {
      using bke::nurbs::SelectionAction;
      const auto expected = cu->editnurb->nurbs_edit->read_snapshot();
      using namespace bke::nurbs;
      Vector<ElementRef> hits;
      if (action != SEL_DESELECT) {
        if (flag_is_set(expected.selection().mode, SelectionMode::Vertex)) {
          for (const NurbsTopologyVertex &vertex : expected.geometry().vertices) {
            hits.append({ElementDomain::Vertex, vertex.id});
          }
        }
        if (flag_is_set(expected.selection().mode, SelectionMode::Edge)) {
          for (const NurbsTopologyEdge &edge : expected.geometry().edges) {
            hits.append({ElementDomain::Edge, edge.id});
          }
        }
        if (flag_is_set(expected.selection().mode, SelectionMode::Face)) {
          for (const NurbsTopologyFace &face : expected.geometry().faces) {
            hits.append({ElementDomain::Face, face.id});
          }
        }
      }
      std::string error;
      ed::nurbs::select_components(*bmain,
                                   *cu,
                                   expected,
                                   hits,
                                   action == SEL_INVERT ? SelectionAction::Invert :
                                   action == SEL_SELECT ? SelectionAction::Add :
                                                          SelectionAction::Set,
                                   std::nullopt,
                                   &error);
      if (!error.empty()) {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
      continue;
    }

    switch (action) {
      case SEL_SELECT:
        changed = ED_curve_select_all(cu->editnurb);
        break;
      case SEL_DESELECT:
        changed = ED_curve_deselect_all(cu->editnurb);
        break;
      case SEL_INVERT:
        changed = ED_curve_select_swap(
            cu->editnurb, (v3d && (v3d->overlay.handle_display == CURVE_HANDLE_NONE)));
        break;
    }

    if (changed) {
      DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
      BKE_curve_nurb_vert_active_validate(cu);
    }
  }

  return OPERATOR_FINISHED;
}

void CURVE_OT_select_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "(De)select All";
  ot->idname = "CURVE_OT_select_all";
  ot->description = "(De)select all editable points";

  /* API callbacks. */
  ot->exec = de_select_all_exec;
  ot->poll = ED_operator_editsurfcurve_or_nurbs;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_select_all(ot);
}

/** \} */

static bool native_select_mode_poll(bContext *C)
{
  if (!ED_operator_editsurfcurve_or_nurbs(C)) {
    return false;
  }
  const Curve &curve = *id_cast<Curve *>(CTX_data_edit_object(C)->data);
  return curve.editnurb != nullptr && curve.editnurb->nurbs_edit != nullptr;
}

static wmOperatorStatus select_mode_exec(bContext *C, wmOperator *op)
{
  bool changed = false;
  const bool success = ed::nurbs::select_mode(
      C,
      bke::nurbs::SelectionMode(RNA_enum_get(op->ptr, "type")),
      RNA_boolean_get(op->ptr, "use_extend"),
      &changed);
  return success && changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void CURVE_OT_select_mode(wmOperatorType *ot)
{
  static const EnumPropertyItem mode_items[] = {
      {1, "VERT", ICON_VERTEXSEL, "Vertex", "Select NURBS vertices"},
      {2, "EDGE", ICON_EDGESEL, "Edge", "Select shared NURBS edges"},
      {4, "FACE", ICON_FACESEL, "Face", "Select NURBS faces"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Select Mode";
  ot->idname = "CURVE_OT_select_mode";
  ot->description = "Change native NURBS component selection mode";
  ot->exec = select_mode_exec;
  ot->poll = native_select_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_enum(ot->srna, "type", mode_items, 1, "Type", "Component selection mode");
  RNA_def_boolean(
      ot->srna, "use_extend", false, "Extend", "Toggle this mode alongside the current modes");
}

/* -------------------------------------------------------------------- */
/** \name Select Linked Operator
 * \{ */

enum class CurveTopologyElementType : uint8_t { Face, Wire };

struct CurveTopologyElement {
  Nurb *nurb;
  CurveTopologyElementType type;
};

static bool curve_topology_face_is_supported(const Nurb &nurb)
{
  return nurb.type == CU_NURBS && nurb.pntsu == 2 && nurb.pntsv == 2 && nurb.bp != nullptr &&
         nurb.topology_face_id > 0;
}

static bool curve_topology_wire_is_supported(const Nurb &nurb)
{
  return nurb.type == CU_NURBS && nurb.pntsu == 2 && nurb.pntsv == 1 && nurb.bp != nullptr &&
         nurb.topology_edge_ids[0] > 0 && nurb.topology_vertex_ids[0] > 0 &&
         nurb.topology_vertex_ids[1] > 0;
}

static int curve_topology_element_point_indices(const CurveTopologyElement &element,
                                                int r_indices[4])
{
  if (element.type == CurveTopologyElementType::Wire) {
    r_indices[0] = 0;
    r_indices[1] = 1;
    return 2;
  }

  /* Nurb control points are stored with U varying fastest. The topology corner IDs run around
   * the face perimeter, so corner three maps to point two in the control-point array. */
  r_indices[0] = 0;
  r_indices[1] = 1;
  r_indices[2] = 3;
  r_indices[3] = 2;
  return 4;
}

static void curve_topology_element_edge_corners(const CurveTopologyElement &element,
                                                const int edge,
                                                int &r_corner_a,
                                                int &r_corner_b)
{
  if (element.type == CurveTopologyElementType::Wire) {
    r_corner_a = 0;
    r_corner_b = 1;
    return;
  }

  static constexpr int face_edge_corners[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
  r_corner_a = face_edge_corners[edge][0];
  r_corner_b = face_edge_corners[edge][1];
}

/** Select the visible topology vertex shell containing selected or picked vertices. */
static bool curve_topology_select_linked_component(ListBaseT<Nurb> *nurbs,
                                                   const BPoint *seed_point,
                                                   const bool select)
{
  if (nurbs == nullptr) {
    return false;
  }

  Vector<CurveTopologyElement> elements;
  Map<int, Vector<int>> adjacent_vertices_by_vertex;
  Map<int, Vector<BPoint *>> points_by_vertex;
  int seed_vertex_id = 0;
  for (Nurb &nurb : *nurbs) {
    const bool is_face = curve_topology_face_is_supported(nurb);
    if (!is_face && !curve_topology_wire_is_supported(nurb)) {
      continue;
    }

    const CurveTopologyElementType type = is_face ? CurveTopologyElementType::Face :
                                                    CurveTopologyElementType::Wire;
    const CurveTopologyElement element{&nurb, type};
    elements.append(element);

    int point_indices[4];
    const int point_count = curve_topology_element_point_indices(element, point_indices);
    for (int corner = 0; corner < point_count; corner++) {
      const int point_index = point_indices[corner];
      const int vertex_id = nurb.topology_vertex_ids[corner];
      if (vertex_id > 0 && nurb.bp[point_index].hide == 0) {
        points_by_vertex.lookup_or_add_default(vertex_id).append(&nurb.bp[point_index]);
        if (&nurb.bp[point_index] == seed_point) {
          seed_vertex_id = vertex_id;
        }
      }
    }

    const int edge_count = is_face ? 4 : 1;
    for (int edge = 0; edge < edge_count; edge++) {
      if (nurb.topology_edge_ids[edge] == 0) {
        continue;
      }
      int corner_a, corner_b;
      curve_topology_element_edge_corners(element, edge, corner_a, corner_b);
      const int point_a = point_indices[corner_a];
      const int point_b = point_indices[corner_b];
      const int vertex_a = nurb.topology_vertex_ids[corner_a];
      const int vertex_b = nurb.topology_vertex_ids[corner_b];
      if (vertex_a == 0 || vertex_b == 0 || nurb.bp[point_a].hide != 0 ||
          nurb.bp[point_b].hide != 0)
      {
        continue;
      }
      adjacent_vertices_by_vertex.lookup_or_add_default(vertex_a).append(vertex_b);
      adjacent_vertices_by_vertex.lookup_or_add_default(vertex_b).append(vertex_a);
    }
  }

  Map<int, bool> linked_vertex_ids;
  Vector<int> pending_vertex_ids;
  if (seed_point != nullptr) {
    if (seed_vertex_id == 0) {
      return false;
    }
    linked_vertex_ids.add(seed_vertex_id, true);
    pending_vertex_ids.append(seed_vertex_id);
  }
  else {
    for (const CurveTopologyElement &element : elements) {
      int point_indices[4];
      const int point_count = curve_topology_element_point_indices(element, point_indices);
      for (int corner = 0; corner < point_count; corner++) {
        const BPoint &point = element.nurb->bp[point_indices[corner]];
        const int vertex_id = element.nurb->topology_vertex_ids[corner];
        if (vertex_id > 0 && point.hide == 0 && (point.f1 & SELECT) != 0 &&
            !linked_vertex_ids.contains(vertex_id))
        {
          linked_vertex_ids.add(vertex_id, true);
          pending_vertex_ids.append(vertex_id);
        }
      }
    }
  }

  for (int pending_index = 0; pending_index < pending_vertex_ids.size(); pending_index++) {
    const int vertex_id = pending_vertex_ids[pending_index];
    const Vector<int> *const adjacent_vertices = adjacent_vertices_by_vertex.lookup_ptr(vertex_id);
    if (adjacent_vertices == nullptr) {
      continue;
    }
    for (const int adjacent_vertex : *adjacent_vertices) {
      if (!linked_vertex_ids.contains(adjacent_vertex)) {
        linked_vertex_ids.add(adjacent_vertex, true);
        pending_vertex_ids.append(adjacent_vertex);
      }
    }
  }

  bool changed = false;
  for (const int vertex_id : linked_vertex_ids.keys()) {
    const Vector<BPoint *> *const points = points_by_vertex.lookup_ptr(vertex_id);
    if (points == nullptr) {
      continue;
    }
    for (BPoint *point : *points) {
      const bool was_selected = (point->f1 & SELECT) != 0;
      if (was_selected == select) {
        continue;
      }
      if (select) {
        point->f1 |= SELECT;
      }
      else {
        point->f1 &= ~SELECT;
      }
      changed = true;
    }
  }
  return changed;
}

static wmOperatorStatus select_linked_exec(bContext *C, wmOperator * /*op*/)
{
  const Curve &active_curve = *id_cast<Curve *>(CTX_data_edit_object(C)->data);
  if (active_curve.editnurb->nurbs_edit != nullptr) {
    return ed::nurbs::select_linked(C) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
  }
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);

  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    Curve *cu = id_cast<Curve *>(obedit->data);
    EditNurb *editnurb = cu->editnurb;
    ListBaseT<Nurb> *nurbs = &editnurb->nurbs;
    bool changed = curve_topology_select_linked_component(nurbs, nullptr, true);

    for (Nurb &nu : *nurbs) {
      if (!curve_topology_face_is_supported(nu) && !curve_topology_wire_is_supported(nu) &&
          ED_curve_nurb_select_check(v3d, &nu))
      {
        changed |= ED_curve_nurb_select_all(&nu);
      }
    }

    if (changed) {
      DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
    }
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus select_linked_invoke(bContext *C,
                                             wmOperator *op,
                                             const wmEvent * /*event*/)
{
  return select_linked_exec(C, op);
}

void CURVE_OT_select_linked(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Linked All";
  ot->idname = "CURVE_OT_select_linked";
  ot->description = "Select all components linked to the current selection";

  /* API callbacks. */
  ot->exec = select_linked_exec;
  ot->invoke = select_linked_invoke;
  ot->poll = ED_operator_editsurfcurve_or_nurbs;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Linked Pick Operator
 * \{ */

static wmOperatorStatus select_linked_pick_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  const Curve &curve = *id_cast<Curve *>(CTX_data_edit_object(C)->data);
  if (curve.editnurb->nurbs_edit != nullptr) {
    return ed::nurbs::select_linked_pick(
               C, event->mval, ED_view3d_select_dist_px(), RNA_boolean_get(op->ptr, "deselect")) ?
               OPERATOR_FINISHED :
               OPERATOR_CANCELLED;
  }
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Nurb *nu;
  BezTriple *bezt;
  BPoint *bp;
  int a;
  const bool select = !RNA_boolean_get(op->ptr, "deselect");
  Base *basact = nullptr;

  view3d_operator_needs_gpu(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  copy_v2_v2_int(vc.mval, event->mval);

  if (!ED_curve_pick_vert(&vc, 1, &nu, &bezt, &bp, nullptr, &basact)) {
    return OPERATOR_CANCELLED;
  }

  Object *obedit = basact->object;
  bool changed = false;
  if (bp != nullptr && curve_topology_face_is_supported(*nu)) {
    Curve *cu = id_cast<Curve *>(obedit->data);
    changed = curve_topology_select_linked_component(&cu->editnurb->nurbs, bp, select);
  }
  else if (bp != nullptr && curve_topology_wire_is_supported(*nu)) {
    Curve *cu = id_cast<Curve *>(obedit->data);
    changed = curve_topology_select_linked_component(&cu->editnurb->nurbs, bp, select);
  }
  else if (bezt) {
    a = nu->pntsu;
    bezt = nu->bezt;
    while (a--) {
      changed |= select_beztriple(bezt, select, BEZT_FLAG_SELECT, VISIBLE);
      bezt++;
    }
  }
  else if (bp) {
    a = nu->pntsu * nu->pntsv;
    bp = nu->bp;
    while (a--) {
      changed |= select_bpoint(bp, select, BEZT_FLAG_SELECT, VISIBLE);
      bp++;
    }
  }

  if (changed) {
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  if (!select) {
    BKE_curve_nurb_vert_active_validate(id_cast<Curve *>(obedit->data));
  }

  return OPERATOR_FINISHED;
}

static bool select_linked_pick_poll(bContext *C)
{
  return ED_operator_editsurfcurve_or_nurbs(C) && ED_operator_region_view3d_active(C);
}

void CURVE_OT_select_linked_pick(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Linked";
  ot->idname = "CURVE_OT_select_linked_pick";
  ot->description = "Select all components linked to the component under the mouse";

  /* API callbacks. */
  ot->invoke = select_linked_pick_invoke;
  ot->poll = select_linked_pick_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna,
                  "deselect",
                  false,
                  "Deselect",
                  "Deselect linked components rather than selecting them");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Row Operator
 * \{ */

static wmOperatorStatus select_row_exec(bContext *C, wmOperator * /*op*/)
{
  Object *obedit = CTX_data_edit_object(C);
  Curve *cu = id_cast<Curve *>(obedit->data);
  ListBaseT<Nurb> *editnurb = object_editcurve_get(obedit);
  static BPoint *last = nullptr;
  static int direction = 0;
  Nurb *nu = nullptr;
  BPoint *bp = nullptr;
  int u = 0, v = 0, a, b;

  if (!BKE_curve_nurb_vert_active_get(cu, &nu, static_cast<void **>(static_cast<void *>(&bp)))) {
    return OPERATOR_CANCELLED;
  }

  if (last == bp) {
    direction = 1 - direction;
    BKE_nurbList_flag_set(editnurb, BEZT_FLAG_SELECT, false);
  }
  last = bp;

  u = cu->actvert % nu->pntsu;
  v = cu->actvert / nu->pntsu;
  bp = nu->bp;
  for (a = 0; a < nu->pntsv; a++) {
    for (b = 0; b < nu->pntsu; b++, bp++) {
      if (direction) {
        if (a == v) {
          select_bpoint(bp, true, BEZT_FLAG_SELECT, VISIBLE);
        }
      }
      else {
        if (b == u) {
          select_bpoint(bp, true, BEZT_FLAG_SELECT, VISIBLE);
        }
      }
    }
  }

  DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);

  return OPERATOR_FINISHED;
}

void CURVE_OT_select_row(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Control Point Row";
  ot->idname = "CURVE_OT_select_row";
  ot->description =
      "Select a row of control points including active one. "
      "Successive use on the same point switches between U/V directions";

  /* API callbacks. */
  ot->exec = select_row_exec;
  ot->poll = ED_operator_editsurf;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Next Operator
 * \{ */

static wmOperatorStatus select_next_exec(bContext *C, wmOperator * /*op*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {

    ListBaseT<Nurb> *editnurb = object_editcurve_get(obedit);
    select_adjacent_cp(editnurb, 1, false, SELECT);

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  return OPERATOR_FINISHED;
}

void CURVE_OT_select_next(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Next";
  ot->idname = "CURVE_OT_select_next";
  ot->description = "Select control points following already selected ones along the curves";

  /* API callbacks. */
  ot->exec = select_next_exec;
  ot->poll = ED_operator_editcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Previous Operator
 * \{ */

static wmOperatorStatus select_previous_exec(bContext *C, wmOperator * /*op*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {

    ListBaseT<Nurb> *editnurb = object_editcurve_get(obedit);
    select_adjacent_cp(editnurb, -1, false, SELECT);

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  return OPERATOR_FINISHED;
}

void CURVE_OT_select_previous(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Previous";
  ot->idname = "CURVE_OT_select_previous";
  ot->description = "Select control points preceding already selected ones along the curves";

  /* API callbacks. */
  ot->exec = select_previous_exec;
  ot->poll = ED_operator_editcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select More Operator
 * \{ */

static void curve_select_more(Object *obedit)
{
  ListBaseT<Nurb> *editnurb = object_editcurve_get(obedit);
  BPoint *bp, *tempbp;
  int a;
  short sel = 0;

  /* NOTE: NURBS surface is a special case because we mimic
   * the behavior of "select more" of mesh tools.
   * The algorithm is designed to work in planar cases so it
   * may not be optimal always (example: end of NURBS sphere). */
  if (obedit->type == OB_SURF) {
    for (Nurb &nu : *editnurb) {
      BLI_bitmap *selbpoints;
      a = nu.pntsu * nu.pntsv;
      bp = nu.bp;
      selbpoints = BLI_BITMAP_NEW(a + 1, "selectlist");
      while (a > 0) {
        if (!BLI_BITMAP_TEST(selbpoints, a) && (bp->hide == 0) && (bp->f1 & SELECT)) {
          /* upper control point */
          if (a % nu.pntsu != 0) {
            tempbp = bp - 1;
            if (!(tempbp->f1 & SELECT)) {
              select_bpoint(tempbp, true, BEZT_FLAG_SELECT, VISIBLE);
            }
          }

          /* left control point. select only if it is not selected already */
          if (a - nu.pntsu > 0) {
            sel = 0;
            tempbp = bp + nu.pntsu;
            if (!(tempbp->f1 & SELECT)) {
              sel = select_bpoint(tempbp, true, BEZT_FLAG_SELECT, VISIBLE);
            }
            /* make sure selected bpoint is discarded */
            if (sel == 1) {
              BLI_BITMAP_ENABLE(selbpoints, a - nu.pntsu);
            }
          }

          /* right control point */
          if (a + nu.pntsu <= nu.pntsu * nu.pntsv) {
            tempbp = bp - nu.pntsu;
            if (!(tempbp->f1 & SELECT)) {
              select_bpoint(tempbp, true, BEZT_FLAG_SELECT, VISIBLE);
            }
          }

          /* lower control point. skip next bp in case selection was made */
          if (a % nu.pntsu != 1) {
            sel = 0;
            tempbp = bp + 1;
            if (!(tempbp->f1 & SELECT)) {
              sel = select_bpoint(tempbp, true, BEZT_FLAG_SELECT, VISIBLE);
            }
            if (sel) {
              bp++;
              a--;
            }
          }
        }

        bp++;
        a--;
      }

      MEM_delete(selbpoints);
    }
  }
  else {
    select_adjacent_cp(editnurb, 1, false, SELECT);
    select_adjacent_cp(editnurb, -1, false, SELECT);
  }
}

static wmOperatorStatus curve_select_more_exec(bContext *C, wmOperator * /*op*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    curve_select_more(obedit);
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  return OPERATOR_FINISHED;
}

void CURVE_OT_select_more(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select More";
  ot->idname = "CURVE_OT_select_more";
  ot->description = "Select control points at the boundary of each selection region";

  /* API callbacks. */
  ot->exec = curve_select_more_exec;
  ot->poll = ED_operator_editsurfcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Less Operator
 * \{ */

/* basic method: deselect if control point doesn't have all neighbors selected */
static void curve_select_less(Object *obedit)
{
  ListBaseT<Nurb> *editnurb = object_editcurve_get(obedit);
  BPoint *bp;
  BezTriple *bezt;
  int a;
  int sel = 0;
  bool lastsel = false;

  if (obedit->type == OB_SURF) {
    for (Nurb &nu : *editnurb) {
      BLI_bitmap *selbpoints;
      a = nu.pntsu * nu.pntsv;
      bp = nu.bp;
      selbpoints = BLI_BITMAP_NEW(a, "selectlist");
      while (a--) {
        if ((bp->hide == 0) && (bp->f1 & SELECT)) {
          sel = 0;

          /* check if neighbors have been selected */
          /* edges of surface are an exception */
          if ((a + 1) % nu.pntsu == 0) {
            sel++;
          }
          else {
            bp--;
            if (BLI_BITMAP_TEST(selbpoints, a + 1) || ((bp->hide == 0) && (bp->f1 & SELECT))) {
              sel++;
            }
            bp++;
          }

          if ((a + 1) % nu.pntsu == 1) {
            sel++;
          }
          else {
            bp++;
            if ((bp->hide == 0) && (bp->f1 & SELECT)) {
              sel++;
            }
            bp--;
          }

          if (a + 1 > nu.pntsu * nu.pntsv - nu.pntsu) {
            sel++;
          }
          else {
            bp -= nu.pntsu;
            if (BLI_BITMAP_TEST(selbpoints, a + nu.pntsu) ||
                ((bp->hide == 0) && (bp->f1 & SELECT)))
            {
              sel++;
            }
            bp += nu.pntsu;
          }

          if (a < nu.pntsu) {
            sel++;
          }
          else {
            bp += nu.pntsu;
            if ((bp->hide == 0) && (bp->f1 & SELECT)) {
              sel++;
            }
            bp -= nu.pntsu;
          }

          if (sel != 4) {
            select_bpoint(bp, false, BEZT_FLAG_SELECT, VISIBLE);
            BLI_BITMAP_ENABLE(selbpoints, a);
          }
        }
        else {
          lastsel = false;
        }

        bp++;
      }

      MEM_delete(selbpoints);
    }
  }
  else {
    for (Nurb &nu : *editnurb) {
      lastsel = false;
      /* check what type of curve/nurb it is */
      if (nu.type == CU_BEZIER) {
        a = nu.pntsu;
        bezt = nu.bezt;
        while (a--) {
          if ((bezt->hide == 0) && (bezt->f2 & SELECT)) {
            sel = (lastsel == 1);

            /* check if neighbors have been selected */
            /* first and last are exceptions */
            if (a == nu.pntsu - 1) {
              sel++;
            }
            else {
              bezt--;
              if ((bezt->hide == 0) && (bezt->f2 & SELECT)) {
                sel++;
              }
              bezt++;
            }

            if (a == 0) {
              sel++;
            }
            else {
              bezt++;
              if ((bezt->hide == 0) && (bezt->f2 & SELECT)) {
                sel++;
              }
              bezt--;
            }

            if (sel != 2) {
              select_beztriple(bezt, false, BEZT_FLAG_SELECT, VISIBLE);
              lastsel = true;
            }
            else {
              lastsel = false;
            }
          }
          else {
            lastsel = false;
          }

          bezt++;
        }
      }
      else {
        a = nu.pntsu * nu.pntsv;
        bp = nu.bp;
        while (a--) {
          if ((lastsel == false) && (bp->hide == 0) && (bp->f1 & SELECT)) {
            sel = 0;

            /* first and last are exceptions */
            if (a == nu.pntsu * nu.pntsv - 1) {
              sel++;
            }
            else {
              bp--;
              if ((bp->hide == 0) && (bp->f1 & SELECT)) {
                sel++;
              }
              bp++;
            }

            if (a == 0) {
              sel++;
            }
            else {
              bp++;
              if ((bp->hide == 0) && (bp->f1 & SELECT)) {
                sel++;
              }
              bp--;
            }

            if (sel != 2) {
              select_bpoint(bp, false, BEZT_FLAG_SELECT, VISIBLE);
              lastsel = true;
            }
            else {
              lastsel = false;
            }
          }
          else {
            lastsel = false;
          }

          bp++;
        }
      }
    }
  }
}

static wmOperatorStatus curve_select_less_exec(bContext *C, wmOperator * /*op*/)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    curve_select_less(obedit);
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  return OPERATOR_FINISHED;
}

void CURVE_OT_select_less(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Less";
  ot->idname = "CURVE_OT_select_less";
  ot->description = "Deselect control points at the boundary of each selection region";

  /* API callbacks. */
  ot->exec = curve_select_less_exec;
  ot->poll = ED_operator_editsurfcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Random Operator
 * \{ */

static wmOperatorStatus curve_select_random_exec(bContext *C, wmOperator *op)
{
  const bool select = (RNA_enum_get(op->ptr, "action") == SEL_SELECT);
  const float randfac = RNA_float_get(op->ptr, "ratio");
  const int seed = WM_operator_properties_select_random_seed_increment_get(op);

  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (const int ob_index : objects.index_range()) {
    Object *obedit = objects[ob_index];
    ListBaseT<Nurb> *editnurb = object_editcurve_get(obedit);
    int seed_iter = seed;

    /* This gives a consistent result regardless of object order. */
    if (ob_index) {
      seed_iter += BLI_ghashutil_strhash_p(obedit->id.name);
    }

    int totvert = 0;
    for (Nurb &nu : *editnurb) {
      if (nu.type == CU_BEZIER) {
        int a = nu.pntsu;
        BezTriple *bezt = nu.bezt;
        while (a--) {
          if (!bezt->hide) {
            totvert++;
          }
          bezt++;
        }
      }
      else {
        int a = nu.pntsu * nu.pntsv;
        BPoint *bp = nu.bp;
        while (a--) {
          if (!bp->hide) {
            totvert++;
          }
          bp++;
        }
      }
    }

    BLI_bitmap *verts_selection_mask = BLI_BITMAP_NEW(totvert, __func__);
    const int count_select = totvert * randfac;
    for (int i = 0; i < count_select; i++) {
      BLI_BITMAP_SET(verts_selection_mask, i, true);
    }
    BLI_bitmap_randomize(verts_selection_mask, totvert, seed_iter);

    int bit_index = 0;
    for (Nurb &nu : *editnurb) {
      if (nu.type == CU_BEZIER) {
        int a = nu.pntsu;
        BezTriple *bezt = nu.bezt;

        while (a--) {
          if (!bezt->hide) {
            if (BLI_BITMAP_TEST(verts_selection_mask, bit_index)) {
              select_beztriple(bezt, select, BEZT_FLAG_SELECT, VISIBLE);
            }
            bit_index++;
          }
          bezt++;
        }
      }
      else {
        int a = nu.pntsu * nu.pntsv;
        BPoint *bp = nu.bp;

        while (a--) {
          if (!bp->hide) {
            if (BLI_BITMAP_TEST(verts_selection_mask, bit_index)) {
              select_bpoint(bp, select, BEZT_FLAG_SELECT, VISIBLE);
            }
            bit_index++;
          }
          bp++;
        }
      }
    }

    MEM_delete(verts_selection_mask);
    BKE_curve_nurb_vert_active_validate(id_cast<Curve *>(obedit->data));
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  return OPERATOR_FINISHED;
}

void CURVE_OT_select_random(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Random";
  ot->idname = "CURVE_OT_select_random";
  ot->description = "Randomly select some control points";

  /* API callbacks. */
  ot->exec = curve_select_random_exec;
  ot->poll = ED_operator_editsurfcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_select_random(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Every Nth Number of Point Operator
 * \{ */

static void select_nth_bezt(Nurb *nu, BezTriple *bezt, const CheckerIntervalParams *params)
{
  int a, start;

  start = bezt - nu->bezt;
  a = nu->pntsu;
  bezt = &nu->bezt[a - 1];

  while (a--) {
    const int depth = abs(start - a);
    if (!WM_operator_properties_checker_interval_test(params, depth)) {
      select_beztriple(bezt, false, BEZT_FLAG_SELECT, HIDDEN);
    }

    bezt--;
  }
}

static void select_nth_bp(Nurb *nu, BPoint *bp, const CheckerIntervalParams *params)
{
  int a, startrow, startpnt;
  int row, pnt;

  startrow = (bp - nu->bp) / nu->pntsu;
  startpnt = (bp - nu->bp) % nu->pntsu;

  a = nu->pntsu * nu->pntsv;
  bp = &nu->bp[a - 1];
  row = nu->pntsv - 1;
  pnt = nu->pntsu - 1;

  while (a--) {
    const int depth = abs(pnt - startpnt) + abs(row - startrow);
    if (!WM_operator_properties_checker_interval_test(params, depth)) {
      select_bpoint(bp, false, BEZT_FLAG_SELECT, HIDDEN);
    }

    pnt--;
    if (pnt < 0) {
      pnt = nu->pntsu - 1;
      row--;
    }

    bp--;
  }
}

static bool ed_curve_select_nth(Curve *cu, const CheckerIntervalParams *params)
{
  Nurb *nu = nullptr;
  void *vert = nullptr;

  if (!BKE_curve_nurb_vert_active_get(cu, &nu, &vert)) {
    return false;
  }

  if (nu->bezt) {
    select_nth_bezt(nu, static_cast<BezTriple *>(vert), params);
  }
  else {
    select_nth_bp(nu, static_cast<BPoint *>(vert), params);
  }

  return true;
}

static wmOperatorStatus select_nth_exec(bContext *C, wmOperator *op)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *obact = CTX_data_edit_object(C);
  View3D *v3d = CTX_wm_view3d(C);
  bool changed = false;

  CheckerIntervalParams op_params;
  WM_operator_properties_checker_interval_from_op(op, &op_params);

  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    Curve *cu = id_cast<Curve *>(obedit->data);

    if (!ED_curve_select_check(v3d, cu->editnurb)) {
      continue;
    }

    if (ed_curve_select_nth(id_cast<Curve *>(obedit->data), &op_params) == true) {
      changed = true;

      DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
    }
  }

  if (!changed) {
    if (obact->type == OB_SURF) {
      BKE_report(op->reports, RPT_ERROR, "Surface(s) have no active point");
    }
    else {
      BKE_report(op->reports, RPT_ERROR, "Curve(s) have no active point");
    }
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void CURVE_OT_select_nth(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Checker Deselect";
  ot->description = "Deselect every Nth point starting from the active one";
  ot->idname = "CURVE_OT_select_nth";

  /* API callbacks. */
  ot->exec = select_nth_exec;
  ot->poll = ED_operator_editsurfcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_checker_interval(ot, false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Similar Operator
 * \{ */

static const EnumPropertyItem curve_prop_similar_compare_types[] = {
    {SIM_CMP_EQ, "EQUAL", 0, "Equal", ""},
    {SIM_CMP_GT, "GREATER", 0, "Greater", ""},
    {SIM_CMP_LT, "LESS", 0, "Less", ""},

    {0, nullptr, 0, nullptr, nullptr},
};

enum {
  SIMCURHAND_TYPE = 0,
  SIMCURHAND_RADIUS,
  SIMCURHAND_WEIGHT,
  SIMCURHAND_DIRECTION,
};

static const EnumPropertyItem curve_prop_similar_types[] = {
    {SIMCURHAND_TYPE, "TYPE", 0, "Type", ""},
    {SIMCURHAND_RADIUS, "RADIUS", 0, "Radius", ""},
    {SIMCURHAND_WEIGHT, "WEIGHT", 0, "Weight", ""},
    {SIMCURHAND_DIRECTION, "DIRECTION", 0, "Direction", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static void nurb_bezt_direction_worldspace_get(Object *ob,
                                               Nurb *nu,
                                               BezTriple *bezt,
                                               float r_dir[3])
{
  float rsmat[3][3];
  BKE_nurb_bezt_calc_normal(nu, bezt, r_dir);
  copy_m3_m4(rsmat, ob->object_to_world().ptr());
  mul_m3_v3(rsmat, r_dir);
  normalize_v3(r_dir);
}

static void nurb_bpoint_direction_worldspace_get(Object *ob, Nurb *nu, BPoint *bp, float r_dir[3])
{
  float rsmat[3][3];
  BKE_nurb_bpoint_calc_normal(nu, bp, r_dir);
  copy_m3_m4(rsmat, ob->object_to_world().ptr());
  mul_m3_v3(rsmat, r_dir);
  normalize_v3(r_dir);
}

static void curve_nurb_selected_type_get(
    Object *ob, Nurb *nu, const int type, KDTree<float> *tree_1d, KDTree<float3> *tree_3d)
{
  float tree_entry[3] = {0.0f, 0.0f, 0.0f};

  if (nu->type == CU_BEZIER) {
    BezTriple *bezt;
    int i;
    int tree_index = 0;

    for (i = nu->pntsu, bezt = nu->bezt; i--; bezt++) {
      if ((!bezt->hide) && (bezt->f1 & SELECT)) {

        switch (type) {
          case SIMCURHAND_RADIUS: {
            float radius_ref = bezt->radius;
            tree_entry[0] = radius_ref;
            break;
          }
          case SIMCURHAND_WEIGHT: {
            float weight_ref = bezt->weight;
            tree_entry[0] = weight_ref;
            break;
          }
          case SIMCURHAND_DIRECTION: {
            nurb_bezt_direction_worldspace_get(ob, nu, bezt, tree_entry);
            break;
          }
        }
        if (tree_1d) {
          kdtree_insert<float>(tree_1d, tree_index++, tree_entry[0]);
        }
        else {
          kdtree_insert<float3>(tree_3d, tree_index++, tree_entry);
        }
      }
    }
  }
  else {
    BPoint *bp;
    int i;
    int tree_index = 0;

    for (i = nu->pntsu * nu->pntsv, bp = nu->bp; i--; bp++) {
      if (!bp->hide && bp->f1 & SELECT) {
        switch (type) {
          case SIMCURHAND_RADIUS: {
            float radius_ref = bp->radius;
            tree_entry[0] = radius_ref;
            break;
          }
          case SIMCURHAND_WEIGHT: {
            float weight_ref = bp->weight;
            tree_entry[0] = weight_ref;
            break;
          }
          case SIMCURHAND_DIRECTION: {
            nurb_bpoint_direction_worldspace_get(ob, nu, bp, tree_entry);
            break;
          }
        }
        if (tree_1d) {
          kdtree_insert<float>(tree_1d, tree_index++, tree_entry[0]);
        }
        else {
          kdtree_insert<float3>(tree_3d, tree_index++, tree_entry);
        }
      }
    }
  }
}

static bool curve_nurb_select_similar_type(Object *ob,
                                           Nurb *nu,
                                           const int type,
                                           const KDTree<float> *tree_1d,
                                           const KDTree<float3> *tree_3d,
                                           const float thresh,
                                           const int compare)
{
  const float thresh_cos = cosf(thresh * float(M_PI_2));
  bool changed = false;

  if (nu->type == CU_BEZIER) {
    BezTriple *bezt;
    int i;

    for (i = nu->pntsu, bezt = nu->bezt; i--; bezt++) {
      if (!bezt->hide) {
        bool select = false;

        switch (type) {
          case SIMCURHAND_RADIUS: {
            float radius_ref = bezt->radius;
            if (ED_select_similar_compare_float_tree(
                    tree_1d, radius_ref, thresh, eSimilarCmp(compare)))
            {
              select = true;
            }
            break;
          }
          case SIMCURHAND_WEIGHT: {
            float weight_ref = bezt->weight;
            if (ED_select_similar_compare_float_tree(
                    tree_1d, weight_ref, thresh, eSimilarCmp(compare)))
            {
              select = true;
            }
            break;
          }
          case SIMCURHAND_DIRECTION: {
            float dir[3];
            nurb_bezt_direction_worldspace_get(ob, nu, bezt, dir);
            KDTreeNearest<float3> nearest;
            if (kdtree_find_nearest<float3>(tree_3d, dir, &nearest) != -1) {
              float orient = angle_normalized_v3v3(dir, nearest.co);
              float delta = thresh_cos - fabsf(cosf(orient));
              if (ED_select_similar_compare_float(delta, thresh, eSimilarCmp(compare))) {
                select = true;
              }
            }
            break;
          }
        }

        if (select) {
          select_beztriple(bezt, true, BEZT_FLAG_SELECT, VISIBLE);
          changed = true;
        }
      }
    }
  }
  else {
    BPoint *bp;
    int i;

    for (i = nu->pntsu * nu->pntsv, bp = nu->bp; i--; bp++) {
      if (!bp->hide) {
        bool select = false;

        switch (type) {
          case SIMCURHAND_RADIUS: {
            float radius_ref = bp->radius;
            if (ED_select_similar_compare_float_tree(
                    tree_1d, radius_ref, thresh, eSimilarCmp(compare)))
            {
              select = true;
            }
            break;
          }
          case SIMCURHAND_WEIGHT: {
            float weight_ref = bp->weight;
            if (ED_select_similar_compare_float_tree(
                    tree_1d, weight_ref, thresh, eSimilarCmp(compare)))
            {
              select = true;
            }
            break;
          }
          case SIMCURHAND_DIRECTION: {
            float dir[3];
            nurb_bpoint_direction_worldspace_get(ob, nu, bp, dir);
            KDTreeNearest<float3> nearest;
            if (kdtree_find_nearest<float3>(tree_3d, dir, &nearest) != -1) {
              float orient = angle_normalized_v3v3(dir, nearest.co);
              float delta = fabsf(cosf(orient)) - thresh_cos;
              if (ED_select_similar_compare_float(delta, thresh, eSimilarCmp(compare))) {
                select = true;
              }
            }
            break;
          }
        }

        if (select) {
          select_bpoint(bp, true, BEZT_FLAG_SELECT, VISIBLE);
          changed = true;
        }
      }
    }
  }
  return changed;
}

static wmOperatorStatus curve_select_similar_exec(bContext *C, wmOperator *op)
{
  /* Get props. */
  const int optype = RNA_enum_get(op->ptr, "type");
  const float thresh = RNA_float_get(op->ptr, "threshold");
  const int compare = RNA_enum_get(op->ptr, "compare");

  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  int tot_nurbs_selected_all = 0;
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    Curve *cu = id_cast<Curve *>(obedit->data);
    tot_nurbs_selected_all += ED_curve_select_count(v3d, cu->editnurb);
  }

  if (tot_nurbs_selected_all == 0) {
    BKE_report(op->reports, RPT_ERROR, "No control point selected");
    return OPERATOR_CANCELLED;
  }

  KDTree<float> *tree_1d = nullptr;
  KDTree<float3> *tree_3d = nullptr;
  short type_ref = 0;

  switch (optype) {
    case SIMCURHAND_RADIUS:
    case SIMCURHAND_WEIGHT:
      tree_1d = kdtree_new<float>(tot_nurbs_selected_all);
      break;
    case SIMCURHAND_DIRECTION:
      tree_3d = kdtree_new<float3>(tot_nurbs_selected_all);
      break;
  }

  /* Get type of selected control points. */
  for (Object *obedit : objects) {
    Curve *cu = id_cast<Curve *>(obedit->data);
    EditNurb *editnurb = cu->editnurb;

    for (Nurb &nu : editnurb->nurbs) {
      if (!ED_curve_nurb_select_check(v3d, &nu)) {
        continue;
      }
      switch (optype) {
        case SIMCURHAND_TYPE: {
          type_ref |= nu.type;
          break;
        }
        case SIMCURHAND_RADIUS:
        case SIMCURHAND_WEIGHT:
        case SIMCURHAND_DIRECTION:
          curve_nurb_selected_type_get(obedit, &nu, optype, tree_1d, tree_3d);
          break;
      }
    }
  }

  if (tree_1d != nullptr) {
    kdtree_deduplicate<float>(tree_1d);
    kdtree_balance<float>(tree_1d);
  }
  if (tree_3d != nullptr) {
    kdtree_deduplicate<float3>(tree_3d);
    kdtree_balance<float3>(tree_3d);
  }

  /* Select control points with desired type. */
  for (Object *obedit : objects) {
    Curve *cu = id_cast<Curve *>(obedit->data);
    EditNurb *editnurb = cu->editnurb;
    bool changed = false;

    for (Nurb &nu : editnurb->nurbs) {
      switch (optype) {
        case SIMCURHAND_TYPE: {
          if (nu.type & type_ref) {
            changed |= ED_curve_nurb_select_all(&nu);
          }
          break;
        }
        case SIMCURHAND_RADIUS:
        case SIMCURHAND_WEIGHT:
        case SIMCURHAND_DIRECTION:
          changed = curve_nurb_select_similar_type(
              obedit, &nu, optype, tree_1d, tree_3d, thresh, compare);
          break;
      }
    }

    if (changed) {
      DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
    }
  }

  if (tree_1d != nullptr) {
    kdtree_free<float>(tree_1d);
  }
  if (tree_3d != nullptr) {
    kdtree_free<float3>(tree_3d);
  }
  return OPERATOR_FINISHED;
}

void CURVE_OT_select_similar(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Similar";
  ot->idname = "CURVE_OT_select_similar";
  ot->description = "Select similar curve points by property type";

  /* API callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = curve_select_similar_exec;
  ot->poll = ED_operator_editsurfcurve;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(
      ot->srna, "type", curve_prop_similar_types, SIMCURHAND_WEIGHT, "Type", "");
  RNA_def_enum(ot->srna, "compare", curve_prop_similar_compare_types, SIM_CMP_EQ, "Compare", "");
  RNA_def_float(ot->srna, "threshold", 0.1, 0.0, FLT_MAX, "Threshold", "", 0.0, 100.0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Shortest Path Operator
 * \{ */

static float curve_calc_dist_pair(const Nurb *nu, int a, int b)
{
  const float *a_fl, *b_fl;

  if (nu->type == CU_BEZIER) {
    a_fl = nu->bezt[a].vec[1];
    b_fl = nu->bezt[b].vec[1];
  }
  else {
    a_fl = nu->bp[a].vec;
    b_fl = nu->bp[b].vec;
  }

  return len_v3v3(a_fl, b_fl);
}

static float curve_calc_dist_span(Nurb *nu, int vert_src, int vert_dst)
{
  const int u = nu->pntsu;
  int i_prev, i;
  float dist = 0.0f;

  BLI_assert(nu->pntsv <= 1);

  i_prev = vert_src;
  i = (i_prev + 1) % u;

  while (true) {
    dist += curve_calc_dist_pair(nu, i_prev, i);

    if (i == vert_dst) {
      break;
    }
    i = (i + 1) % u;
  }
  return dist;
}

static void curve_select_shortest_path_curve(Nurb *nu, int vert_src, int vert_dst)
{
  const int u = nu->pntsu;
  int i;

  if (vert_src > vert_dst) {
    std::swap(vert_src, vert_dst);
  }

  if (nu->flagu & CU_NURB_CYCLIC) {
    if (curve_calc_dist_span(nu, vert_src, vert_dst) >
        curve_calc_dist_span(nu, vert_dst, vert_src))
    {
      std::swap(vert_src, vert_dst);
    }
  }

  i = vert_src;
  while (true) {
    if (nu->type & CU_BEZIER) {
      select_beztriple(&nu->bezt[i], true, BEZT_FLAG_SELECT, HIDDEN);
    }
    else {
      select_bpoint(&nu->bp[i], true, BEZT_FLAG_SELECT, HIDDEN);
    }

    if (i == vert_dst) {
      break;
    }
    i = (i + 1) % u;
  }
}

static void curve_select_shortest_path_surf(Nurb *nu, int vert_src, int vert_dst)
{
  int totu = nu->pntsu;
  int totv = nu->pntsv;
  int vert_num = totu * totv;

  /* custom data */
  struct PointAdj {
    int vert, vert_prev;
    float cost;
  } *data;

  /* init connectivity data */
  data = MEM_new_array_uninitialized<PointAdj>(vert_num, __func__);
  for (int i = 0; i < vert_num; i++) {
    data[i].vert = i;
    data[i].vert_prev = -1;
    data[i].cost = FLT_MAX;
  }

  /* init heap */
  HeapSimple *heap = BLI_heapsimple_new();

  int vert_curr = data[vert_src].vert;
  BLI_heapsimple_insert(heap, 0.0f, &data[vert_src].vert);
  data[vert_src].cost = 0.0f;
  data[vert_src].vert_prev = vert_src; /* nop */

  while (!BLI_heapsimple_is_empty(heap)) {
    vert_curr = *(static_cast<int *>(BLI_heapsimple_pop_min(heap)));
    if (vert_curr == vert_dst) {
      break;
    }

    int u, v;
    BKE_nurb_index_to_uv(nu, vert_curr, &u, &v);

    /* loop over 4 adjacent verts */
    for (int sign = -1; sign != 3; sign += 2) {
      for (int axis = 0; axis != 2; axis += 1) {
        int uv_other[2] = {u, v};
        int vert_other;

        uv_other[axis] += sign;

        vert_other = BKE_nurb_index_from_uv(nu, uv_other[0], uv_other[1]);
        if (vert_other != -1) {
          const float dist = data[vert_curr].cost +
                             curve_calc_dist_pair(nu, vert_curr, vert_other);

          if (data[vert_other].cost > dist) {
            data[vert_other].cost = dist;
            if (data[vert_other].vert_prev == -1) {
              BLI_heapsimple_insert(heap, data[vert_other].cost, &data[vert_other].vert);
            }
            data[vert_other].vert_prev = vert_curr;
          }
        }
      }
    }
  }

  BLI_heapsimple_free(heap, nullptr);

  if (vert_curr == vert_dst) {
    int i = 0;
    while (vert_curr != vert_src && i++ < vert_num) {
      if (nu->type == CU_BEZIER) {
        select_beztriple(&nu->bezt[vert_curr], true, BEZT_FLAG_SELECT, HIDDEN);
      }
      else {
        select_bpoint(&nu->bp[vert_curr], true, BEZT_FLAG_SELECT, HIDDEN);
      }
      vert_curr = data[vert_curr].vert_prev;
    }
  }

  MEM_delete(data);
}

static wmOperatorStatus edcu_shortest_path_pick_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Nurb *nu_dst;
  BezTriple *bezt_dst;
  BPoint *bp_dst;
  int vert_dst;
  void *vert_dst_p;
  Base *basact = nullptr;

  view3d_operator_needs_gpu(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  copy_v2_v2_int(vc.mval, event->mval);

  if (!ED_curve_pick_vert(&vc, 1, &nu_dst, &bezt_dst, &bp_dst, nullptr, &basact)) {
    return OPERATOR_PASS_THROUGH;
  }

  ED_view3d_viewcontext_init_object(&vc, basact->object);
  Object *obedit = basact->object;
  Curve *cu = id_cast<Curve *>(obedit->data);
  Nurb *nu_src = BKE_curve_nurb_active_get(cu);
  int vert_src = cu->actvert;

  if (vert_src == CU_ACT_NONE) {
    return OPERATOR_PASS_THROUGH;
  }

  if (nu_src != nu_dst) {
    BKE_report(op->reports, RPT_ERROR, "Control point belongs to another spline");
    return OPERATOR_CANCELLED;
  }

  vert_dst_p = bezt_dst ? static_cast<void *>(bezt_dst) : static_cast<void *>(bp_dst);
  vert_dst = BKE_curve_nurb_vert_index_get(nu_dst, vert_dst_p);
  if (vert_src == vert_dst) {
    return OPERATOR_CANCELLED;
  }

  if ((obedit->type == OB_SURF) && (nu_src->pntsv > 1)) {
    curve_select_shortest_path_surf(nu_src, vert_src, vert_dst);
  }
  else {
    curve_select_shortest_path_curve(nu_src, vert_src, vert_dst);
  }

  BKE_curve_nurb_vert_active_set(cu, nu_dst, vert_dst_p);

  BKE_view_layer_synced_ensure(*vc.bmain, vc.scene, vc.view_layer);
  if (BKE_view_layer_active_base_get(vc.view_layer) != basact) {
    ed::object::base_activate(C, basact);
  }

  DEG_id_tag_update(obedit->data, ID_RECALC_SELECT | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  return OPERATOR_FINISHED;
}

void CURVE_OT_shortest_path_pick(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Pick Shortest Path";
  ot->idname = "CURVE_OT_shortest_path_pick";
  ot->description = "Select shortest path between two selections";

  /* API callbacks. */
  ot->invoke = edcu_shortest_path_pick_invoke;
  ot->poll = ED_operator_editsurfcurve_region_view3d;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender
