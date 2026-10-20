/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <cstdint>
#include <string>

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {

struct NurbsGeometry;

/** Runtime display triangles derived from persistent NURBS topology. `face_ids` has one entry
 * per triangle. The tessellation is an approximation for display and does not alter source
 * geometry or certify a geometric error bound. */
struct NurbsTessellation {
  Vector<double3> positions;
  Vector<int3> triangles;
  Vector<uint64_t> face_ids;
};

/** Tessellate each trimmed face independently. Boundary curves are sampled uniformly in their
 * shared edge parameter, and a normalized UV grid adds interior samples for curved supports.
 * Resolution is bounded to keep derived display data predictable. Failure leaves the output
 * unchanged. */
bool BKE_nurbs_topology_tessellate(const NurbsGeometry &geometry,
                                   int resolution,
                                   NurbsTessellation &r_tessellation,
                                   std::string *r_error = nullptr);

}  // namespace blender
