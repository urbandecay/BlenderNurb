/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_nurbs_types.h"

namespace blender {

/** Exact tensor-product NURBS surface. V control vertices vary fastest. */
struct NurbsSurface {
  struct NurbsSurface *next = nullptr, *prev = nullptr;

  uint64_t id = 0;
  int degree_u = 1;
  int degree_v = 1;
  int order_u = 2;
  int order_v = 2;
  int control_vertex_count_u = 0;
  int control_vertex_count_v = 0;
  int knot_count_u = 0;
  int knot_count_v = 0;
  int dimension = 3;
  char rational = 0;
  char _pad[3] = {};

  double domain_u[2] = {0.0, 1.0};
  double domain_v[2] = {0.0, 1.0};
  NurbsControlVertex *control_vertices = nullptr;
  double *knots_u = nullptr;
  double *knots_v = nullptr;
  void *_pad1 = nullptr;
};

}  // namespace blender
