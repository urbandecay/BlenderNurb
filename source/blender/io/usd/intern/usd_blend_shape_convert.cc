/* SPDX-FileCopyrightText: 2023 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_blend_shape_convert.hh"

#include "usd_blend_shape_utils.hh"

#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShape.h>

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_key.hh"
#include "BKE_lib_id.hh"
#include "BKE_report.hh"

#include "BLI_math_vector.h"
#include "BLI_span.hh"

#include "CLG_log.h"

namespace blender::io::usd {

static CLG_LogRef LOG = {"io.usd"};

void import_blendshapes(Main *bmain,
                        Object *mesh_obj,
                        const pxr::UsdPrim &prim,
                        ReportList *reports)
{
  if (!(mesh_obj && mesh_obj->data && mesh_obj->type == OB_MESH && prim)) {
    return;
  }

  if (prim.IsInstanceProxy()) {
    /* Attempting to create a blend-shape binding API for
     * instance proxies generates USD errors. */
    return;
  }

  pxr::UsdSkelBindingAPI binding_api(prim);

  /* Get the blend shape targets, which are the USD paths to the
   * blend shape primitives. */

  if (!binding_api.GetBlendShapeTargetsRel().HasAuthoredTargets()) {
    /* No targets. */
    return;
  }

  pxr::SdfPathVector targets;
  if (!binding_api.GetBlendShapeTargetsRel().GetTargets(&targets)) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Couldn't get blendshape targets for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  if (targets.empty()) {
    return;
  }

  if (!binding_api.GetBlendShapesAttr().HasAuthoredValue()) {
    return;
  }

  /* Get the blend shape name tokens. */
  pxr::VtTokenArray usd_blendshapes;
  if (!binding_api.GetBlendShapesAttr().Get(&usd_blendshapes)) {
    return;
  }

  if (usd_blendshapes.empty()) {
    return;
  }

  /* Sanity check. */
  if (targets.size() != usd_blendshapes.size()) {
    BKE_reportf(
        reports,
        RPT_WARNING,
        "%s: Number of blendshapes does not match number of blendshape targets for prim %s",
        __func__,
        prim.GetPath().GetAsString().c_str());
    return;
  }

  pxr::UsdStageRefPtr stage = prim.GetStage();

  if (!stage) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Couldn't get stage for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  Mesh *mesh = id_cast<Mesh *>(mesh_obj->data);

  /* Insert key to source mesh. */
  Key *key = BKE_key_add(bmain, id_cast<ID *>(mesh));
  key->type = KEY_RELATIVE;

  mesh->key = key;

  /* Insert basis key. */
  KeyBlock *kb = BKE_keyblock_add(key, "Basis");
  BKE_keyblock_convert_from_mesh(mesh, key, kb);

  Span<pxr::TfToken> blendshapes = Span(usd_blendshapes.cdata(), usd_blendshapes.size());

  for (int i = 0; i < targets.size(); ++i) {
    /* Get USD path to blend shape. */
    const pxr::SdfPath &path = targets[i];
    pxr::UsdSkelBlendShape blendshape(stage->GetPrimAtPath(path));

    if (!blendshape) {
      continue;
    }

    /* Get the blend shape offsets. */
    if (!blendshape.GetOffsetsAttr().HasAuthoredValue()) {
      /* Blend shape has no authored offsets. */
      continue;
    }

    pxr::VtVec3fArray usd_offsets;
    if (!blendshape.GetOffsetsAttr().Get(&usd_offsets)) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "%s: Couldn't get offsets for blend shape %s",
                  __func__,
                  path.GetAsString().c_str());
      continue;
    }

    if (usd_offsets.empty()) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "%s: No offsets for blend shape %s",
                  __func__,
                  path.GetAsString().c_str());
      continue;
    }

    /* Add the key block. */
    kb = BKE_keyblock_add(key, blendshapes[i].GetString().c_str());
    BKE_keyblock_convert_from_mesh(mesh, key, kb);
    if (!kb->data) {
      /* Nothing to do. This can happen if the mesh has no vertices. */
      continue;
    }

    /* if authored, point indices are indices into the original mesh
     * that correspond to the values in the offsets array. */
    pxr::VtArray<int> point_indices;
    if (blendshape.GetPointIndicesAttr().HasAuthoredValue()) {
      blendshape.GetPointIndicesAttr().Get(&point_indices);
    }

    float *fp = static_cast<float *>(kb->data);
    Span<pxr::GfVec3f> offsets = Span(usd_offsets.cdata(), usd_offsets.size());

    if (point_indices.empty()) {
      /* Iterate over all key block elements and add the corresponding
       * offset to the key block point. */
      for (int a = 0; a < kb->totelem; ++a, fp += 3) {
        if (a >= offsets.size()) {
          BKE_reportf(
              reports,
              RPT_WARNING,
              "%s: Number of offsets greater than number of mesh vertices for blend shape %s",
              __func__,
              path.GetAsString().c_str());
          break;
        }
        add_v3_v3(fp, offsets[a].data());
      }
    }
    else {
      /* Iterate over the point indices and add the offset to the corresponding
       * key block point. */
      int a = 0;
      for (const int point : point_indices.AsConst()) {
        if (point < 0 || point >= kb->totelem) {
          CLOG_WARN(&LOG,
                    "Out of bounds point index %d for blendshape %s",
                    point,
                    path.GetAsString().c_str());
          ++a;
          continue;
        }
        if (a >= offsets.size()) {
          BKE_reportf(
              reports,
              RPT_WARNING,
              "%s: Number of offsets greater than number of mesh vertices for blend shape %s",
              __func__,
              path.GetAsString().c_str());
          break;
        }
        add_v3_v3(&fp[3 * point], offsets[a].data());
        ++a;
      }
    }
  }
}

}  // namespace blender::io::usd
