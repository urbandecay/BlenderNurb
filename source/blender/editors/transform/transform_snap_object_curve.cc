/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <cmath>
#include <limits>

#include "DNA_curve_types.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"

#include "BKE_curve.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_object.hh"

#include "ED_transform_snap_object_context.hh"

#include "transform_snap_object.hh"

namespace blender::ed::transform {

eSnapMode snapCurve(SnapObjectContext *sctx, const Object *ob_eval, const float4x4 &obmat)
{
  bool has_snap = false;

  /* Only vertex snapping mode (eg control points and handles) supported for now. */
  if ((sctx->runtime.snap_to_flag & SCE_SNAP_TO_POINT) == 0) {
    return SCE_SNAP_TO_NONE;
  }

  Curve *cu = id_cast<Curve *>(ob_eval->data);

  if (const auto source = bke::nurbs::curve_eval_source_capture(*cu)) {
    if (!source->snapshot) {
      return SCE_SNAP_TO_NONE;
    }
    const auto &snapshot = *source->snapshot;
    SnapData nearest(sctx, obmat);
    nearest.clip_planes_enable(sctx, ob_eval, true);
    const bool skip_selected = BKE_object_is_in_editmode(ob_eval) &&
                               (sctx->runtime.params.snap_target_select &
                                SCE_SNAP_TARGET_NOT_SELECTED);
    const NurbsTopologyVertex *hit = nullptr;
    for (const NurbsTopologyVertex &vertex : snapshot.geometry().vertices) {
      if (skip_selected && snapshot.selection().vertices.contains(vertex.id)) {
        continue;
      }
      bool representable = true;
      for (const double value : vertex.position) {
        representable &= std::isfinite(value) &&
                         std::abs(value) <= std::numeric_limits<float>::max();
      }
      if (representable && nearest.snap_point(float3(double3(vertex.position)))) {
        hit = &vertex;
      }
    }
    if (hit == nullptr) {
      return SCE_SNAP_TO_NONE;
    }
    nearest.register_result(sctx, ob_eval, &cu->id);
    sctx->ret.point_location = math::transform_point(double4x4(obmat), double3(hit->position));
    sctx->ret.nurbs_vertex_id = hit->id;
    return SCE_SNAP_TO_POINT;
  }

  SnapData nearest2d(sctx, obmat);

  const bool use_obedit = BKE_object_is_in_editmode(ob_eval);

  if (use_obedit == false) {
    /* Test BoundBox. */
    std::optional<Bounds<float3>> bounds = BKE_curve_minmax(cu, true);
    if (bounds && !nearest2d.snap_boundbox(bounds->min, bounds->max)) {
      return SCE_SNAP_TO_NONE;
    }
  }

  nearest2d.clip_planes_enable(sctx, ob_eval, true);

  bool skip_selected = (sctx->runtime.params.snap_target_select & SCE_SNAP_TARGET_NOT_SELECTED) !=
                       0;

  for (Nurb &nu : use_obedit ? cu->editnurb->nurbs : cu->nurb) {
    if (nu.bezt) {
      for (int u : IndexRange(nu.pntsu)) {
        if (use_obedit) {
          if (nu.bezt[u].hide) {
            /* Skip hidden. */
            continue;
          }

          bool is_selected = (nu.bezt[u].f2 & SELECT) != 0;
          if (is_selected && skip_selected) {
            continue;
          }

          /* Don't snap if handle is selected (moving),
           * or if it is aligning to a moving handle. */
          bool is_selected_h1 = (nu.bezt[u].f1 & SELECT) != 0;
          bool is_selected_h2 = (nu.bezt[u].f3 & SELECT) != 0;
          bool is_autoalign_h1 = (nu.bezt[u].h1 & HD_ALIGN) != 0;
          bool is_autoalign_h2 = (nu.bezt[u].h2 & HD_ALIGN) != 0;
          if (!skip_selected || !(is_selected_h1 || (is_autoalign_h1 && is_selected_h2))) {
            has_snap |= nearest2d.snap_point(nu.bezt[u].vec[0]);
          }

          if (!skip_selected || !(is_selected_h2 || (is_autoalign_h2 && is_selected_h1))) {
            has_snap |= nearest2d.snap_point(nu.bezt[u].vec[2]);
          }
        }
        has_snap |= nearest2d.snap_point(nu.bezt[u].vec[1]);
      }
    }
    else if (nu.bp) {
      for (int u : IndexRange(nu.pntsu * nu.pntsv)) {
        if (use_obedit) {
          if (nu.bp[u].hide) {
            /* Skip hidden. */
            continue;
          }

          bool is_selected = (nu.bp[u].f1 & SELECT) != 0;
          if (is_selected && skip_selected) {
            continue;
          }
        }
        has_snap |= nearest2d.snap_point(nu.bp[u].vec);
      }
    }
  }
  if (has_snap) {
    nearest2d.register_result(sctx, ob_eval, &cu->id);
    return SCE_SNAP_TO_POINT;
  }
  return SCE_SNAP_TO_NONE;
}

}  // namespace blender::ed::transform
