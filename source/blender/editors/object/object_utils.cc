/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 */

#include <cstdlib>
#include <cstring>

#include "DNA_armature_types.h"
#include "DNA_curve_types.h"
#include "DNA_lattice_types.h"
#include "DNA_meta_types.h"
#include "DNA_object_types.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_editmesh.hh"
#include "BKE_lattice.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_curve.hh"
#include "ED_object.hh" /* own include */

#include "WM_api.hh"

#include "MEM_guardedalloc.h"

namespace blender::ed::object {

/* -------------------------------------------------------------------- */
/** \name Material Functions
 * \{ */

bool material_active_index_set(Object *ob, const int index)
{
  if (ob->totcol > 0) {
    const short actcol_test = std::clamp(index + 1, 1, ob->totcol);
    if (ob->actcol != actcol_test) {
      ob->actcol = actcol_test;
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING_LINKS, nullptr);
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Active Element Center
 * \{ */

bool calc_active_center_for_editmode(Object *obedit, const bool select_only, float r_center[3])
{
  switch (obedit->type) {
    case OB_MESH: {
      BMEditMesh *em = BKE_editmesh_from_object(obedit);
      BMEditSelection ese;

      if (BM_select_history_active_get(em->bm, &ese)) {
        BM_editselection_center(&ese, r_center);
        return true;
      }
      break;
    }
    case OB_ARMATURE: {
      bArmature *arm = id_cast<bArmature *>(obedit->data);
      EditBone *ebo = arm->act_edbone;

      if (ebo && (!select_only || (ebo->flag & (BONE_SELECTED | BONE_ROOTSEL)))) {
        copy_v3_v3(r_center, ebo->head);
        return true;
      }

      break;
    }
    case OB_CURVES_LEGACY:
    case OB_SURF: {
      Curve *cu = id_cast<Curve *>(obedit->data);

      if (ED_curve_active_center(cu, r_center)) {
        return true;
      }
      break;
    }
    case OB_MBALL: {
      MetaBall *mb = id_cast<MetaBall *>(obedit->data);
      MetaElem *ml_act = mb->lastelem;

      if (ml_act && (!select_only || (ml_act->flag & SELECT))) {
        copy_v3_v3(r_center, &ml_act->x);
        return true;
      }
      break;
    }
    case OB_LATTICE: {
      BPoint *actbp = BKE_lattice_active_point_get(id_cast<Lattice *>(obedit->data));

      if (actbp) {
        copy_v3_v3(r_center, actbp->vec);
        return true;
      }
      break;
    }
    default:
      break;
  }

  return false;
}

bool calc_active_center_for_posemode(Object *ob, const bool select_only, float r_center[3])
{
  bPoseChannel *pchan = BKE_pose_channel_active_if_bonecoll_visible(ob);
  if (pchan && (!select_only || (pchan->flag & POSE_SELECTED))) {
    const bArmature *arm = id_cast<bArmature *>(ob->data);
    BKE_pose_channel_transform_location(arm, pchan, r_center);
    return true;
  }
  return false;
}

bool calc_active_center(Object *ob, const bool select_only, float r_center[3])
{
  if (ob->mode & OB_MODE_EDIT) {
    if (calc_active_center_for_editmode(ob, select_only, r_center)) {
      mul_m4_v3(ob->object_to_world().ptr(), r_center);
      return true;
    }
    return false;
  }
  if (ob->mode & OB_MODE_POSE) {
    if (calc_active_center_for_posemode(ob, select_only, r_center)) {
      mul_m4_v3(ob->object_to_world().ptr(), r_center);
      return true;
    }
    return false;
  }
  if (!select_only || (ob->base_flag & BASE_SELECTED)) {
    copy_v3_v3(r_center, ob->object_to_world().location());
    return true;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Data Transform Container
 *
 * Use to implement 'Affect Only Origins' feature.
 *
 * \{ */

struct XFormObjectData_Extra {
  Object *ob = nullptr;
  float obmat_orig[4][4] = {};
  std::unique_ptr<XFormObjectData> xod;
};

struct XFormObjectData_Container {
  Map<ID *, std::unique_ptr<XFormObjectData_Extra>> obdata_in_obmode_map;
};

void data_xform_container_item_ensure(XFormObjectData_Container *xds, Object *ob)
{
  xds->obdata_in_obmode_map.lookup_or_add_cb(ob->data, [&]() {
    auto xf = std::make_unique<XFormObjectData_Extra>();
    copy_m4_m4(xf->obmat_orig, ob->object_to_world().ptr());
    xf->ob = ob;
    /* Result may be nullptr, that's OK. */
    xf->xod = data_xform_create(ob->data);
    return xf;
  });
}

void data_xform_container_update_all(XFormObjectData_Container *xds,
                                     Main *bmain,
                                     Depsgraph *depsgraph)
{
  if (xds->obdata_in_obmode_map.is_empty()) {
    return;
  }
  BKE_scene_graph_evaluated_ensure(depsgraph, bmain);

  for (const auto &item : xds->obdata_in_obmode_map.items()) {
    ID *id = item.key;
    XFormObjectData_Extra *xf = item.value.get();

    if (!xf->xod) {
      continue;
    }

    Object *ob_eval = DEG_get_evaluated(depsgraph, xf->ob);
    float4x4 imat, dmat;
    invert_m4_m4(imat.ptr(), xf->obmat_orig);
    mul_m4_m4m4(dmat.ptr(), imat.ptr(), ob_eval->object_to_world().ptr());
    invert_m4(dmat.ptr());

    data_xform_by_mat4(*xf->xod, dmat);
    if (xf->ob->type == OB_ARMATURE) {
      /* TODO: none of the current flags properly update armatures, needs investigation. */
      DEG_id_tag_update(id, 0);
    }
    else {
      DEG_id_tag_update(id, ID_RECALC_GEOMETRY);
    }
  }
}

XFormObjectData_Container *data_xform_container_create()
{
  return MEM_new<XFormObjectData_Container>(__func__);
}

void data_xform_container_destroy(XFormObjectData_Container *xds)
{
  MEM_delete(xds);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Object Array
 *
 * Low level object transform function, transforming objects by `matrix`.
 * Simple alternative to full transform logic.
 * \{ */

void object_xform_array_m4(Object **objects, uint objects_len, const float matrix[4][4])
{
  /* Detect translation only matrix, prevent rotation/scale channels from being touched at all. */
  bool is_translation_only;
  {
    float test_m4_a[4][4], test_m4_b[4][4];
    unit_m4(test_m4_a);
    copy_m4_m4(test_m4_b, matrix);
    zero_v3(test_m4_b[3]);
    is_translation_only = equals_m4m4(test_m4_a, test_m4_b);
  }

  if (is_translation_only) {
    for (uint i = 0; i < objects_len; i++) {
      Object *ob = objects[i];
      add_v3_v3(ob->loc, matrix[3]);
      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
    }
  }
  else {
    for (uint i = 0; i < objects_len; i++) {
      float m4[4][4];
      Object *ob = objects[i];
      BKE_object_to_mat4(ob, m4);
      mul_m4_m4m4(m4, matrix, m4);
      BKE_object_apply_mat4(ob, m4, true);
      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
    }
  }
}

/** \} */

}  // namespace blender::ed::object
