/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Extraction of Mesh data into VBO to feed to GPU.
 */

#include "DNA_meshdata_types.h"

#include "ED_uvedit.hh"

#include "extract_mesh.hh"

#include "draw_cache_impl.hh"

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Edit Flag Utils
 * \{ */

void mesh_render_data_face_flag(const MeshRenderData &mr,
                                const BMFace *efa,
                                const BMUVOffsets &offsets,
                                EditLoopData &eattr)
{
  const bool solid_face_alias_active =
      (mr.solid_active_face_id.x != 0 || mr.solid_active_face_id.y != 0) &&
      mr.solid_face_id_offset >= 0 &&
      *static_cast<const int2 *>(BM_ELEM_CD_GET_VOID_P(efa, mr.solid_face_id_offset)) ==
          mr.solid_active_face_id;
  if (efa == mr.efa_act || solid_face_alias_active) {
    eattr.v_flag |= VFLAG_FACE_ACTIVE;
  }
  if (BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
    eattr.v_flag |= VFLAG_FACE_SELECTED;
  }

  if (efa == mr.efa_act_uv) {
    eattr.v_flag |= VFLAG_FACE_UV_ACTIVE;
  }
  if ((offsets.uv != -1) && (!BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) &&
      uvedit_face_select_test_ex(mr.toolsettings, mr.bm, efa))
  {
    eattr.v_flag |= VFLAG_FACE_UV_SELECT;
  }

#ifdef WITH_FREESTYLE
  if (mr.freestyle_face_ofs != -1) {
    if (BM_ELEM_CD_GET_BOOL(efa, mr.freestyle_face_ofs)) {
      eattr.v_flag |= VFLAG_FACE_FREESTYLE;
    }
  }
#endif
}

void mesh_render_data_loop_flag(const MeshRenderData &mr,
                                const BMLoop *l,
                                const BMUVOffsets &offsets,
                                EditLoopData &eattr)
{
  if (offsets.uv == -1) {
    return;
  }
  if (BM_ELEM_CD_GET_BOOL(l, offsets.pin)) {
    eattr.v_flag |= VFLAG_VERT_UV_PINNED;
  }
  if (uvedit_uv_select_test_ex(mr.toolsettings, mr.bm, l, offsets)) {
    eattr.v_flag |= VFLAG_VERT_UV_SELECT;
  }
}

void mesh_render_data_loop_edge_flag(const MeshRenderData &mr,
                                     const BMLoop *l,
                                     const BMUVOffsets &offsets,
                                     EditLoopData &eattr)
{
  if (offsets.uv == -1) {
    return;
  }
  if (uvedit_edge_select_test_ex(mr.toolsettings, mr.bm, l, offsets)) {
    eattr.v_flag |= VFLAG_EDGE_UV_SELECT;
    eattr.v_flag |= VFLAG_VERT_UV_SELECT;
  }
}

/** \} */

}  // namespace blender::draw
