/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include <climits>

#include "DNA_nurbs_topology_types.h"
#include "DNA_nurbs_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DRW_select_buffer.hh"

#include "ED_curve.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "curve_intern.hh"

namespace blender {

static bool nurbs_control_vertex_is_selected(const NurbsControlVertex &control_vertex)
{
  return (control_vertex.flag & NURBS_CV_SELECT) != 0;
}

static bool nurbs_deselect_all(NurbsGeometry &geometry)
{
  bool changed = geometry.active_control_vertex_id != 0;
  geometry.active_control_vertex_id = 0;

  for (NurbsCurve &curve : geometry.curves) {
    if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
      continue;
    }
    for (int i = 0; i < curve.control_vertex_count; i++) {
      NurbsControlVertex &control_vertex = curve.control_vertices[i];
      if (nurbs_control_vertex_is_selected(control_vertex)) {
        control_vertex.flag = eNurbsControlVertexFlag(control_vertex.flag & ~NURBS_CV_SELECT);
        changed = true;
      }
    }
  }
  return changed;
}

static bool nurbs_any_control_vertex_selected(const NurbsGeometry &geometry)
{
  for (const NurbsCurve &curve : geometry.curves) {
    if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
      continue;
    }
    for (int i = 0; i < curve.control_vertex_count; i++) {
      if (nurbs_control_vertex_is_selected(curve.control_vertices[i])) {
        return true;
      }
    }
  }
  return false;
}

static bool nurbs_topology_any_selected(const NurbsTopology &topology, const int select_mode)
{
  if (select_mode & SCE_SELECT_VERTEX) {
    for (const NurbsTopoVertex &vertex : topology.vertices) {
      if (BKE_nurbs_topology_selection_is_selected(topology, NURBS_TOPO_SELECT_VERTEX, vertex.id))
      {
        return true;
      }
    }
  }
  if (select_mode & SCE_SELECT_EDGE) {
    for (const NurbsTopoEdge &edge : topology.edges) {
      if (BKE_nurbs_topology_selection_is_selected(topology, NURBS_TOPO_SELECT_EDGE, edge.id)) {
        return true;
      }
    }
  }
  if (select_mode & SCE_SELECT_FACE) {
    for (const NurbsTopoFace &face : topology.faces) {
      if (BKE_nurbs_topology_selection_is_selected(topology, NURBS_TOPO_SELECT_FACE, face.id)) {
        return true;
      }
    }
  }
  return false;
}

static bool nurbs_topology_select_all(NurbsTopology &topology,
                                      const int select_mode,
                                      const int action)
{
  bool changed = false;
  const auto apply = [&](const char type, const uint64_t id) {
    const bool was_selected = BKE_nurbs_topology_selection_is_selected(topology, type, id);
    const bool select = action == SEL_SELECT || (action == SEL_INVERT && !was_selected);
    const bool deselect = action == SEL_DESELECT || (action == SEL_INVERT && was_selected);
    if ((select && (action == SEL_SELECT || !was_selected)) || (deselect && was_selected)) {
      changed |= BKE_nurbs_topology_selection_set(topology, type, id, select, select);
    }
  };

  if (select_mode & SCE_SELECT_VERTEX) {
    for (const NurbsTopoVertex &vertex : topology.vertices) {
      apply(NURBS_TOPO_SELECT_VERTEX, vertex.id);
    }
  }
  if (select_mode & SCE_SELECT_EDGE) {
    for (const NurbsTopoEdge &edge : topology.edges) {
      apply(NURBS_TOPO_SELECT_EDGE, edge.id);
    }
  }
  if (select_mode & SCE_SELECT_FACE) {
    for (const NurbsTopoFace &face : topology.faces) {
      apply(NURBS_TOPO_SELECT_FACE, face.id);
    }
  }
  return changed;
}

static void nurbs_selection_tag_update(bContext *C, NurbsGeometry &geometry)
{
  BKE_nurbs_batch_cache_dirty_tag(&geometry, BKE_NURBS_BATCH_DIRTY_SELECT);
  DEG_id_tag_update(&geometry.id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, &geometry.id);
}

static wmOperatorStatus nurbs_select_all_exec(bContext *C, wmOperator *op)
{
  int action = RNA_enum_get(op->ptr, "action");
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  if (action == SEL_TOGGLE) {
    action = SEL_SELECT;
    for (Object *object : objects) {
      const NurbsGeometry &geometry = *id_cast<NurbsGeometry *>(object->data);
      const bool any_selected = geometry.topology != nullptr ?
                                    nurbs_topology_any_selected(*geometry.topology,
                                                                scene->toolsettings->selectmode) :
                                    nurbs_any_control_vertex_selected(geometry);
      if (any_selected) {
        action = SEL_DESELECT;
        break;
      }
    }
  }

  for (Object *object : objects) {
    NurbsGeometry &geometry = *id_cast<NurbsGeometry *>(object->data);
    bool changed = false;
    if (geometry.topology != nullptr) {
      changed = nurbs_topology_select_all(
          *geometry.topology, scene->toolsettings->selectmode, action);
    }
    else {
      switch (action) {
        case SEL_SELECT:
          for (NurbsCurve &curve : geometry.curves) {
            if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
              continue;
            }
            for (int i = 0; i < curve.control_vertex_count; i++) {
              NurbsControlVertex &control_vertex = curve.control_vertices[i];
              if (!nurbs_control_vertex_is_selected(control_vertex)) {
                control_vertex.flag = eNurbsControlVertexFlag(control_vertex.flag |
                                                              NURBS_CV_SELECT);
                changed = true;
              }
              if (geometry.active_control_vertex_id == 0) {
                geometry.active_control_vertex_id = control_vertex.id;
                changed = true;
              }
            }
          }
          break;
        case SEL_DESELECT:
          changed = nurbs_deselect_all(geometry);
          break;
        case SEL_INVERT:
          for (NurbsCurve &curve : geometry.curves) {
            if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
              continue;
            }
            for (int i = 0; i < curve.control_vertex_count; i++) {
              NurbsControlVertex &control_vertex = curve.control_vertices[i];
              control_vertex.flag = eNurbsControlVertexFlag(control_vertex.flag ^ NURBS_CV_SELECT);
              changed = true;
            }
          }
          if (geometry.active_control_vertex_id != 0) {
            bool active_is_selected = false;
            for (const NurbsCurve &curve : geometry.curves) {
              if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
                continue;
              }
              for (int i = 0; i < curve.control_vertex_count; i++) {
                const NurbsControlVertex &control_vertex = curve.control_vertices[i];
                if (control_vertex.id == geometry.active_control_vertex_id) {
                  active_is_selected = nurbs_control_vertex_is_selected(control_vertex);
                  break;
                }
              }
              if (active_is_selected) {
                break;
              }
            }
            if (!active_is_selected) {
              geometry.active_control_vertex_id = 0;
            }
          }
          break;
        default:
          BLI_assert_unreachable();
      }
    }

    if (changed) {
      nurbs_selection_tag_update(C, geometry);
    }
  }
  return OPERATOR_FINISHED;
}

void NURBS_OT_select_all(wmOperatorType *ot)
{
  ot->name = "Select All";
  ot->idname = "NURBS_OT_select_all";
  ot->description = "Select or deselect all NURBS control points or topology components";
  ot->exec = nurbs_select_all_exec;
  ot->poll = ED_operator_editnurbs;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_select_all(ot);
}

static wmOperatorStatus nurbs_select_mode_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ToolSettings *tool_settings = scene->toolsettings;
  const char select_mode = char(RNA_enum_get(op->ptr, "type"));
  if (!ELEM(select_mode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE)) {
    return OPERATOR_CANCELLED;
  }
  if (tool_settings->selectmode == select_mode) {
    return OPERATOR_CANCELLED;
  }
  tool_settings->selectmode = select_mode;
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
  return OPERATOR_FINISHED;
}

void NURBS_OT_select_mode(wmOperatorType *ot)
{
  static const EnumPropertyItem select_mode_items[] = {
      {SCE_SELECT_VERTEX, "VERTEX", 0, "Vertex", "Select topological vertices"},
      {SCE_SELECT_EDGE, "EDGE", 0, "Edge", "Select topological edges"},
      {SCE_SELECT_FACE, "FACE", 0, "Face", "Select topological faces"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Select Mode";
  ot->idname = "NURBS_OT_select_mode";
  ot->description = "Set the active NURBS topology selection mode";
  ot->exec = nurbs_select_mode_exec;
  ot->poll = ED_operator_editnurbs;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->prop = RNA_def_enum(ot->srna, "type", select_mode_items, SCE_SELECT_VERTEX, "Type", "");
}

static NurbsControlVertex *nurbs_control_vertex_at_index(NurbsGeometry &geometry,
                                                         uint control_vertex_index)
{
  for (NurbsCurve &curve : geometry.curves) {
    if (curve.control_vertices == nullptr || curve.control_vertex_count <= 0) {
      continue;
    }
    if (control_vertex_index < uint(curve.control_vertex_count)) {
      return &curve.control_vertices[control_vertex_index];
    }
    control_vertex_index -= uint(curve.control_vertex_count);
  }
  return nullptr;
}

static uint64_t nurbs_topology_component_at_index(NurbsTopology &topology,
                                                  const char type,
                                                  uint component_index)
{
  if (type == NURBS_TOPO_SELECT_VERTEX) {
    for (NurbsTopoVertex &vertex : topology.vertices) {
      if (component_index-- == 0) {
        return vertex.id;
      }
    }
  }
  else if (type == NURBS_TOPO_SELECT_EDGE) {
    for (NurbsTopoEdge &edge : topology.edges) {
      if (component_index-- == 0) {
        return edge.id;
      }
    }
  }
  else if (type == NURBS_TOPO_SELECT_FACE) {
    for (NurbsTopoFace &face : topology.faces) {
      if (component_index-- == 0) {
        return face.id;
      }
    }
  }
  return 0;
}

static char nurbs_topology_type_from_element_type(const char element_type)
{
  switch (element_type) {
    case SCE_SELECT_VERTEX:
      return NURBS_TOPO_SELECT_VERTEX;
    case SCE_SELECT_EDGE:
      return NURBS_TOPO_SELECT_EDGE;
    case SCE_SELECT_FACE:
      return NURBS_TOPO_SELECT_FACE;
    default:
      return 0;
  }
}

bool ED_nurbs_select_pick(bContext *C, const int mval[2], const SelectPick_Params &params)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  View3D *v3d = CTX_wm_view3d(C);
  ARegion *region = CTX_wm_region(C);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Base *> bases = BKE_view_layer_array_from_bases_in_edit_mode(
      *bmain, scene, view_layer, v3d);

  const char select_mode = scene->toolsettings->selectmode;
  DRW_select_buffer_context_create(depsgraph, bases, select_mode);
  uint distance = uint(ED_view3d_backbuf_sample_size_clamp(region, ED_view3d_select_dist_px()));
  const uint select_id = DRW_select_buffer_find_nearest_to_point(
      depsgraph, region, v3d, mval, 1, UINT_MAX, &distance);

  uint component_index = 0;
  uint base_index = 0;
  char element_type = 0;
  bool found = select_id != 0 &&
               DRW_select_buffer_elem_get(select_id, component_index, base_index, element_type) &&
               base_index < bases.size();

  NurbsGeometry *target_geometry = nullptr;
  NurbsControlVertex *target_control_vertex = nullptr;
  uint64_t target_component_id = 0;
  char target_component_type = 0;
  Base *target_base = nullptr;
  if (found) {
    target_base = bases[base_index];
    target_geometry = id_cast<NurbsGeometry *>(target_base->object->data);
    if (target_geometry->topology != nullptr) {
      target_component_type = nurbs_topology_type_from_element_type(element_type);
      target_component_id = target_component_type != 0 ?
                                nurbs_topology_component_at_index(*target_geometry->topology,
                                                                  target_component_type,
                                                                  component_index) :
                                0;
      found = target_component_id != 0;
    }
    else {
      target_control_vertex = element_type == SCE_SELECT_VERTEX ?
                                  nurbs_control_vertex_at_index(*target_geometry,
                                                                component_index) :
                                  nullptr;
      found = target_control_vertex != nullptr;
    }
  }

  if (params.sel_op == SEL_OP_SET && found && params.select_passthrough) {
    const bool target_is_selected = target_geometry->topology != nullptr ?
                                        BKE_nurbs_topology_selection_is_selected(
                                            *target_geometry->topology,
                                            target_component_type,
                                            target_component_id) :
                                        nurbs_control_vertex_is_selected(*target_control_vertex);
    if (target_is_selected) {
      return false;
    }
  }

  bool changed = false;
  if (params.sel_op == SEL_OP_SET && (found || params.deselect_all)) {
    for (Base *base : bases) {
      NurbsGeometry &geometry = *id_cast<NurbsGeometry *>(base->object->data);
      const bool deselected = geometry.topology != nullptr ?
                                  [&]() {
                                    bool any_selected = false;
                                    for (const NurbsTopoVertex &vertex :
                                         geometry.topology->vertices) {
                                      any_selected |= BKE_nurbs_topology_selection_is_selected(
                                          *geometry.topology, NURBS_TOPO_SELECT_VERTEX, vertex.id);
                                    }
                                    for (const NurbsTopoEdge &edge : geometry.topology->edges) {
                                      any_selected |= BKE_nurbs_topology_selection_is_selected(
                                          *geometry.topology, NURBS_TOPO_SELECT_EDGE, edge.id);
                                    }
                                    for (const NurbsTopoFace &face : geometry.topology->faces) {
                                      any_selected |= BKE_nurbs_topology_selection_is_selected(
                                          *geometry.topology, NURBS_TOPO_SELECT_FACE, face.id);
                                    }
                                    if (any_selected) {
                                      BKE_nurbs_topology_selection_clear(*geometry.topology);
                                    }
                                    return any_selected;
                                  }() :
                                  nurbs_deselect_all(geometry);
      if (deselected) {
        nurbs_selection_tag_update(C, geometry);
        changed = true;
      }
    }
  }

  if (found) {
    NurbsGeometry &geometry = *target_geometry;
    if (geometry.topology != nullptr) {
      const bool was_selected = BKE_nurbs_topology_selection_is_selected(
          *geometry.topology, target_component_type, target_component_id);
      bool select = was_selected;
      switch (params.sel_op) {
        case SEL_OP_SET:
        case SEL_OP_ADD:
          select = true;
          break;
        case SEL_OP_SUB:
          select = false;
          break;
        case SEL_OP_XOR:
          select = !was_selected;
          break;
        case SEL_OP_AND:
          BLI_assert_unreachable();
          break;
      }
      const bool selection_changed = BKE_nurbs_topology_selection_set(
          *geometry.topology, target_component_type, target_component_id, select, select);
      changed |= selection_changed;
      BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
      if (BKE_view_layer_active_base_get(view_layer) != target_base) {
        ed::object::base_activate(C, target_base);
      }
      if (selection_changed) {
        nurbs_selection_tag_update(C, geometry);
      }
      return changed || found;
    }
    NurbsControlVertex &control_vertex = *target_control_vertex;
    const bool was_selected = nurbs_control_vertex_is_selected(control_vertex);
    bool select = was_selected;
    switch (params.sel_op) {
      case SEL_OP_SET:
      case SEL_OP_ADD:
        select = true;
        break;
      case SEL_OP_SUB:
        select = false;
        break;
      case SEL_OP_XOR:
        select = !was_selected;
        break;
      case SEL_OP_AND:
        BLI_assert_unreachable();
        break;
    }

    if (select) {
      control_vertex.flag = eNurbsControlVertexFlag(control_vertex.flag | NURBS_CV_SELECT);
      geometry.active_control_vertex_id = control_vertex.id;
    }
    else {
      control_vertex.flag = eNurbsControlVertexFlag(control_vertex.flag & ~NURBS_CV_SELECT);
      if (geometry.active_control_vertex_id == control_vertex.id) {
        geometry.active_control_vertex_id = 0;
      }
    }
    changed = changed || was_selected != select;
    if (target_geometry->active_control_vertex_id == control_vertex.id && was_selected != select) {
      changed = true;
    }

    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    if (BKE_view_layer_active_base_get(view_layer) != target_base) {
      ed::object::base_activate(C, target_base);
    }
    nurbs_selection_tag_update(C, geometry);
  }

  return changed || found;
}

}  // namespace blender
