/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_curve_topology.hh"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"

namespace blender {

static int nurb_topology_vertex_point_indices_get(const Nurb &nurb, int r_indices[4])
{
  if (nurb.type != CU_NURBS || nurb.bp == nullptr) {
    return 0;
  }

  if (nurb.pntsu == 2 && nurb.pntsv == 1) {
    r_indices[0] = 0;
    r_indices[1] = 1;
    return 2;
  }

  if (nurb.pntsu == 2 && nurb.pntsv == 2 && nurb.topology_face_id > 0) {
    /* Nurb control points are stored with U varying fastest. The topology corner IDs run around
     * the face perimeter, so corner three maps to point two in the control-point array. */
    r_indices[0] = 0;
    r_indices[1] = 1;
    r_indices[2] = 3;
    r_indices[3] = 2;
    return 4;
  }

  return 0;
}

static int nurb_topology_vertex_id_from_point(const Nurb &nurb, const BPoint &point)
{
  int point_indices[4];
  const int corner_count = nurb_topology_vertex_point_indices_get(nurb, point_indices);
  for (int corner = 0; corner < corner_count; corner++) {
    if (&nurb.bp[point_indices[corner]] == &point) {
      return nurb.topology_vertex_ids[corner];
    }
  }
  return 0;
}

void BKE_curve_nurbs_topology_selection_sync(ListBaseT<Nurb> *nurbs)
{
  if (nurbs == nullptr) {
    return;
  }

  Map<int, bool> selected_vertex_ids;
  for (const Nurb &nurb : *nurbs) {
    int point_indices[4];
    const int corner_count = nurb_topology_vertex_point_indices_get(nurb, point_indices);
    for (int corner = 0; corner < corner_count; corner++) {
      const int vertex_id = nurb.topology_vertex_ids[corner];
      const BPoint &point = nurb.bp[point_indices[corner]];
      if (vertex_id > 0 && point.hide == 0 && (point.f1 & SELECT) != 0) {
        selected_vertex_ids.add_overwrite(vertex_id, true);
      }
    }
  }

  if (selected_vertex_ids.is_empty()) {
    return;
  }

  for (Nurb &nurb : *nurbs) {
    int point_indices[4];
    const int corner_count = nurb_topology_vertex_point_indices_get(nurb, point_indices);
    for (int corner = 0; corner < corner_count; corner++) {
      BPoint &point = nurb.bp[point_indices[corner]];
      if (point.hide == 0 && selected_vertex_ids.contains(nurb.topology_vertex_ids[corner])) {
        point.f1 |= SELECT;
      }
    }
  }
}

void BKE_curve_nurbs_topology_selection_set(ListBaseT<Nurb> *nurbs,
                                            const Nurb &source_nurb,
                                            const BPoint &source_point)
{
  if (nurbs == nullptr) {
    return;
  }

  const int vertex_id = nurb_topology_vertex_id_from_point(source_nurb, source_point);
  if (vertex_id == 0) {
    return;
  }

  const bool selected = (source_point.f1 & SELECT) != 0;
  for (Nurb &nurb : *nurbs) {
    int point_indices[4];
    const int corner_count = nurb_topology_vertex_point_indices_get(nurb, point_indices);
    for (int corner = 0; corner < corner_count; corner++) {
      if (nurb.topology_vertex_ids[corner] != vertex_id) {
        continue;
      }

      BPoint &point = nurb.bp[point_indices[corner]];
      if (point.hide != 0) {
        continue;
      }
      if (selected) {
        point.f1 |= SELECT;
      }
      else {
        point.f1 &= ~SELECT;
      }
    }
  }
}

void BKE_curve_nurbs_topology_position_sync(ListBaseT<Nurb> *nurbs)
{
  if (nurbs == nullptr) {
    return;
  }

  Map<int, const BPoint *> selected_vertex_points;
  for (const Nurb &nurb : *nurbs) {
    int point_indices[4];
    const int corner_count = nurb_topology_vertex_point_indices_get(nurb, point_indices);
    for (int corner = 0; corner < corner_count; corner++) {
      const int vertex_id = nurb.topology_vertex_ids[corner];
      const BPoint &point = nurb.bp[point_indices[corner]];
      if (vertex_id > 0 && point.hide == 0 && (point.f1 & SELECT) != 0) {
        selected_vertex_points.add_overwrite(vertex_id, &point);
      }
    }
  }

  if (selected_vertex_points.is_empty()) {
    return;
  }

  for (Nurb &nurb : *nurbs) {
    int point_indices[4];
    const int corner_count = nurb_topology_vertex_point_indices_get(nurb, point_indices);
    for (int corner = 0; corner < corner_count; corner++) {
      const int vertex_id = nurb.topology_vertex_ids[corner];
      BPoint &point = nurb.bp[point_indices[corner]];
      if (const BPoint *const *source = selected_vertex_points.lookup_ptr(vertex_id)) {
        /* Rational weights belong to each support's basis, not to shared topology vertices. */
        copy_v3_v3(point.vec, (*source)->vec);
      }
    }
  }
}

}  // namespace blender
