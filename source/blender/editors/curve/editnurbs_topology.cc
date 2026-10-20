/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurve
 */

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "curve_intern.hh"

namespace blender {

namespace {

struct TopologySplitTransaction {
  NurbsGeometry *geometry;
  NurbsTopology *replacement;
  uint64_t next_component_id;
};

}  // namespace

static wmOperatorStatus nurbs_edge_split_exec(bContext *C, wmOperator *op)
{
  const double factor = RNA_float_get(op->ptr, "factor");
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));
  Vector<TopologySplitTransaction> transactions;
  const auto discard = [&]() {
    for (const TopologySplitTransaction &transaction : transactions) {
      BKE_nurbs_topology_free(transaction.replacement);
    }
  };

  for (Object *object : objects) {
    NurbsGeometry &geometry = *id_cast<NurbsGeometry *>(object->data);
    if (geometry.topology == nullptr) {
      continue;
    }
    Vector<uint64_t> selected_edges;
    for (const NurbsTopoEdge &edge : geometry.topology->edges) {
      if (edge.flag & NURBS_TOPO_SELECT) {
        selected_edges.append(edge.id);
      }
    }
    if (selected_edges.is_empty()) {
      continue;
    }
    transactions.append(
        {&geometry, BKE_nurbs_topology_copy(*geometry.topology), geometry.next_component_id});
    TopologySplitTransaction &transaction = transactions.last();
    for (const uint64_t edge_id : selected_edges) {
      double parameter = 0.0;
      for (const NurbsTopoEdge &edge : transaction.replacement->edges) {
        if (edge.id == edge_id) {
          parameter = edge.domain[0] + factor * (edge.domain[1] - edge.domain[0]);
          break;
        }
      }
      NurbsEdgeSplitResult result;
      std::string error;
      if (!BKE_nurbs_topology_edge_split(geometry.curves,
                                         *transaction.replacement,
                                         transaction.next_component_id,
                                         edge_id,
                                         parameter,
                                         result,
                                         &error))
      {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        discard();
        return OPERATOR_CANCELLED;
      }
    }
  }
  if (transactions.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "Select a NURBS topology edge to split");
    return OPERATOR_CANCELLED;
  }
  for (const TopologySplitTransaction &transaction : transactions) {
    NurbsGeometry &geometry = *transaction.geometry;
    BKE_nurbs_topology_free(geometry.topology);
    geometry.topology = transaction.replacement;
    geometry.next_component_id = transaction.next_component_id;
    BKE_nurbs_batch_cache_dirty_tag(&geometry, BKE_NURBS_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&geometry.id, ID_RECALC_GEOMETRY | ID_RECALC_SELECT);
  }
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, nullptr);
  return OPERATOR_FINISHED;
}

void NURBS_OT_edge_split(wmOperatorType *ot)
{
  ot->name = "Split NURBS Edges";
  ot->idname = "NURBS_OT_edge_split";
  ot->description = "Divide selected NURBS edges while preserving their exact curve supports";
  ot->exec = nurbs_edge_split_exec;
  ot->poll = ED_operator_editnurbs;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_float_factor(ot->srna,
                       "factor",
                       0.5f,
                       0.000001f,
                       0.999999f,
                       "Factor",
                       "Position within each selected edge parameter interval",
                       0.0f,
                       1.0f);
}

}  // namespace blender
