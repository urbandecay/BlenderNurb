/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_listBase.h"

namespace blender {

struct NurbsTopology;

namespace draw {
struct NurbsBatchCache;
}

enum eNurbsControlVertexFlag : char {
  NURBS_CV_SELECT = 1 << 0,
};

/** A Euclidean control vertex for an exact NURBS curve. */
struct NurbsControlVertex {
  uint64_t id = 0;
  double position[3] = {};
  double weight = 1.0;
  eNurbsControlVertexFlag flag = eNurbsControlVertexFlag(0);
  char _pad[7] = {};
};

/** Exact NURBS curve storage. Knot values use the reduced Rhino/openNURBS convention. */
struct NurbsCurve {
  struct NurbsCurve *next = nullptr, *prev = nullptr;

  uint64_t id = 0;
  int degree = 1;
  int order = 2;
  /** Native control vertices use object-local XYZ, including for planar curves. */
  int dimension = 3;
  int control_vertex_count = 0;
  int knot_count = 0;
  char rational = 0;
  char _pad[3] = {};

  double domain[2] = {0.0, 1.0};
  NurbsControlVertex *control_vertices = nullptr;
  double *knots = nullptr;
};

/** Native CAD NURBS data-block. Tessellated geometry is always derived from this data. */
struct NurbsGeometry {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_NU;
#endif

  ID id;
  struct AnimData *adt = nullptr;

  ListBaseT<NurbsCurve> curves = {nullptr, nullptr};
  uint64_t next_component_id = 1;
  /** Stable ID of the active control vertex, or zero when none is active. */
  uint64_t active_control_vertex_id = 0;

  draw::NurbsBatchCache *batch_cache = nullptr;
  NurbsTopology *topology = nullptr;
};

}  // namespace blender
