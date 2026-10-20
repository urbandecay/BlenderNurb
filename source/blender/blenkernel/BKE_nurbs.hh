/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <cstddef>

#include "BLI_math_matrix_types.hh"

#include "DNA_nurbs_types.h"

namespace blender {

struct Main;
struct Object;

/** Create a NURBS geometry data-block with a default cubic curve. */
NurbsGeometry *BKE_nurbs_add(Main *bmain, const char *name);

/** Create a degree-four NURBS path with five collinear control vertices. */
NurbsGeometry *BKE_nurbs_add_path(Main *bmain, const char *name);

/** Create a NURBS geometry data-block containing an exact rational circle. */
NurbsGeometry *BKE_nurbs_add_circle(Main *bmain, const char *name);

/** Create a degree-one NURBS line from two object-local endpoints. */
NurbsGeometry *BKE_nurbs_add_line(Main *bmain,
                                 const char *name,
                                 const double start[3],
                                 const double end[3]);

/** Create an exact rational circular arc in the object-local XY plane.
 *
 * The sweep must be non-zero and no greater than one full turn. The arc is split into quadratic
 * spans of at most 90 degrees. A full turn produces a closed curve with a duplicated endpoint.
 */
NurbsGeometry *BKE_nurbs_add_arc(Main *bmain,
                                 const char *name,
                                 double radius,
                                 double start_angle,
                                 double sweep_angle);

/** Deep-copy an exact NURBS curve, including its owned control-vertex and knot arrays. */
NurbsCurve *BKE_nurbs_curve_copy(const NurbsCurve &curve);

/** Free a curve created by #BKE_nurbs_curve_copy or a NURBS geometry factory. */
void BKE_nurbs_curve_free(NurbsCurve *curve);

/** Deep-copy a curve list. The destination is replaced, and may not alias the source. */
void BKE_nurbs_curve_list_copy(ListBaseT<NurbsCurve> &destination,
                               const ListBaseT<NurbsCurve> &source);

/** Free all curves in a list. */
void BKE_nurbs_curve_list_free(ListBaseT<NurbsCurve> &curves);

/** Memory footprint of a curve list, including its curve records and owned arrays. */
std::size_t BKE_nurbs_curve_list_memory_size(const ListBaseT<NurbsCurve> &curves);

/** Evaluate a curve within its stored parameter domain. */
bool BKE_nurbs_curve_evaluate(const NurbsCurve &curve, double parameter, double r_position[3]);

/** Validate exact object-local XYZ curve data and its reduced knot domain. */
bool BKE_nurbs_curve_is_valid(const NurbsCurve &curve);

/** Apply an affine object transform to exact CVs, preserving weights and parameterization. */
void BKE_nurbs_transform(NurbsGeometry &geometry, const float4x4 &matrix);

/** Evaluate NURBS control data to a temporary display mesh without modifying the authoritative ID.
 */
void BKE_nurbs_data_update(Object *object);

enum {
  BKE_NURBS_BATCH_DIRTY_ALL = 0,
  BKE_NURBS_BATCH_DIRTY_SELECT,
};

void BKE_nurbs_batch_cache_dirty_tag(NurbsGeometry *geometry, int mode);
void BKE_nurbs_batch_cache_free(NurbsGeometry *geometry);

extern void (*BKE_nurbs_batch_cache_dirty_tag_cb)(NurbsGeometry *geometry, int mode);
extern void (*BKE_nurbs_batch_cache_free_cb)(NurbsGeometry *geometry);

}  // namespace blender
