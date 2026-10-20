/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_nurbs_surface_types.h"

namespace blender {

enum eNurbsTopologyFlag : char {
  NURBS_TOPO_SELECT = 1 << 0,
};

enum eNurbsTopologySelectType : char {
  NURBS_TOPO_SELECT_VERTEX = 1 << 0,
  NURBS_TOPO_SELECT_EDGE = 1 << 1,
  NURBS_TOPO_SELECT_FACE = 1 << 2,
};

/** Ordered persistent selection history entry, referring to a topology component ID. */
struct NurbsSelection {
  struct NurbsSelection *next = nullptr, *prev = nullptr;
  uint64_t component_id = 0;
  char type = 0;
  char _pad[7] = {};
};

/** Topological vertex, distinct from geometric support control vertices. */
struct NurbsTopoVertex {
  struct NurbsTopoVertex *next = nullptr, *prev = nullptr;
  uint64_t id = 0;
  double position[3] = {};
  char flag = 0;
  char _pad[7] = {};
};

/** Topological edge referring to a shared 3D curve support by ID. */
struct NurbsTopoEdge {
  struct NurbsTopoEdge *next = nullptr, *prev = nullptr;
  uint64_t id = 0;
  uint64_t vertex_ids[2] = {};
  uint64_t curve_id = 0;
  double domain[2] = {0.0, 1.0};
  char flag = 0;
  char _pad[7] = {};
};

/** Oriented reference to an edge and an exact UV trim curve support. */
struct NurbsEdgeUse {
  uint64_t id = 0;
  uint64_t edge_id = 0;
  uint64_t trim_curve_id = 0;
  char reversed = 0;
  char _pad[7] = {};
};

/** Ordered boundary loop of a face; role is 0 for outer and 1 for hole. */
struct NurbsTrimLoop {
  uint64_t id = 0;
  int use_count = 0;
  char role = 0;
  char _pad[3] = {};
  NurbsEdgeUse *uses = nullptr;
  void *_pad1 = nullptr;
};

/** Face support and its oriented outer/hole trim loops. */
struct NurbsTopoFace {
  struct NurbsTopoFace *next = nullptr, *prev = nullptr;
  uint64_t id = 0;
  uint64_t surface_id = 0;
  int loop_count = 0;
  char flag = 0;
  char reversed = 0;
  char _pad[2] = {};
  NurbsTrimLoop *loops = nullptr;
  void *_pad1 = nullptr;
};

/** Persistent support and incidence data owned by a NURBS data-block. */
struct NurbsTopology {
  ListBaseT<NurbsSurface> surfaces = {nullptr, nullptr};
  ListBaseT<NurbsCurve> trim_curves = {nullptr, nullptr};
  ListBaseT<NurbsTopoVertex> vertices = {nullptr, nullptr};
  ListBaseT<NurbsTopoEdge> edges = {nullptr, nullptr};
  ListBaseT<NurbsTopoFace> faces = {nullptr, nullptr};
  ListBaseT<NurbsSelection> selection_history = {nullptr, nullptr};
  uint64_t active_component_id = 0;
  double tolerance = 1e-9;
  /** Dimensionless UV closure and winding tolerance, relative to surface domain widths. */
  double parameter_tolerance = 1e-12;
};

}  // namespace blender
