/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Experimental NURBS source storage used by native kernel/editor adapters.
 */

#pragma once

#include "DNA_listBase.h"

#include "BLI_sys_types.h"

namespace blender {

/** Euclidean control coefficient and its rational basis weight; not an edit handle. */
struct NurbsControlPoint {
  double position[3] = {};
  double weight = 1.0;
};

/** A full knot vector, including end multiplicities, independent of display resolution. */
struct NurbsCurveGeometry {
  NurbsCurveGeometry *next = nullptr, *prev = nullptr;
  int64_t id = 0;
  NurbsControlPoint *points = nullptr;
  double *knots = nullptr;
  int points_num = 0;
  int knots_num = 0;
  int order = 0;
  /** Two for a p-curve, three for a model-space curve. */
  int dimension = 3;
};

/** Tensor-product support. Points are stored with U varying fastest. */
struct NurbsSurfaceGeometry {
  NurbsSurfaceGeometry *next = nullptr, *prev = nullptr;
  int64_t id = 0;
  NurbsControlPoint *points = nullptr;
  double *knots_u = nullptr, *knots_v = nullptr;
  void *_pad_ptr = nullptr;
  int points_num = 0;
  int points_u = 0, points_v = 0;
  int order_u = 0, order_v = 0;
  int knots_u_num = 0, knots_v_num = 0;
  int _pad = 0;
};

struct NurbsTopologyVertex {
  NurbsTopologyVertex *next = nullptr, *prev = nullptr;
  int64_t id = 0;
  double position[3] = {};
  double tolerance = 0.0;
};

/** One shared identity and one 3D curve, regardless of the number of incident face uses. */
struct NurbsTopologyEdge {
  NurbsTopologyEdge *next = nullptr, *prev = nullptr;
  int64_t id = 0;
  int64_t vertex_ids[2] = {};
  int64_t curve_id = 0;
  double parameter_range[2] = {};
  double tolerance = 0.0;
};

/**
 * One face's use of an edge. The p-curve uses the edge's parameter interval;
 * reversing a use changes loop traversal, not the stored curve parameterization.
 */
struct NurbsTopologyEdgeUse {
  NurbsTopologyEdgeUse *next = nullptr, *prev = nullptr;
  int64_t id = 0;
  int64_t edge_id = 0;
  int64_t pcurve_id = 0;
  double tolerance = 0.0;
  int reversed = 0;
  int _pad = 0;
};

struct NurbsTopologyLoop {
  NurbsTopologyLoop *next = nullptr, *prev = nullptr;
  int64_t id = 0;
  ListBaseT<NurbsTopologyEdgeUse> edge_uses = {nullptr, nullptr};
  /** Zero for an outer boundary, one for a hole. */
  int is_inner = 0;
  int _pad = 0;
};

struct NurbsTopologyFace {
  NurbsTopologyFace *next = nullptr, *prev = nullptr;
  int64_t id = 0;
  int64_t surface_id = 0;
  ListBaseT<NurbsTopologyLoop> loops = {nullptr, nullptr};
  double tolerance = 0.0;
  /** One or minus one relative to the support orientation. */
  int orientation = 1;
  int material_index = 0;
};

/** Domain values match the kernel's ElementDomain. Zero ID means no active element. */
struct NurbsSelectionElement {
  int64_t id = 0;
  /** Zero for a vertex, one for an edge, two for a face. */
  int domain = 0;
  int _pad = 0;
};

/** Persistent owner state. The live edit state keeps its own selection and clears this copy. */
struct NurbsStoredSelection {
  NurbsSelectionElement *elements = nullptr;
  NurbsSelectionElement *history = nullptr;
  NurbsSelectionElement active;
  int elements_num = 0;
  int history_num = 0;
};

/**
 * Owned source payload. All references are local IDs; lookup and incidence caches are runtime
 * data. Wires, sheets and non-manifold incidence are legal, independently of solid eligibility.
 */
struct NurbsGeometry {
  int64_t next_id = 1;
  ListBaseT<NurbsCurveGeometry> curves = {nullptr, nullptr};
  ListBaseT<NurbsSurfaceGeometry> surfaces = {nullptr, nullptr};
  ListBaseT<NurbsTopologyVertex> vertices = {nullptr, nullptr};
  ListBaseT<NurbsTopologyEdge> edges = {nullptr, nullptr};
  ListBaseT<NurbsTopologyFace> faces = {nullptr, nullptr};
  NurbsStoredSelection *stored_selection = nullptr;
  void *_pad_ptr = nullptr;
};

}  // namespace blender
