/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "SOL_geometry.hh"

namespace blender::solid {

enum class PointClassification { Outside, Inside, Boundary, Uncertain };

struct ClassificationTolerance {
  /** Absolute uncertainty band in model units. Nearby supports are never snapped together.
   * A point in this band which is not on the support returns Uncertain, not Boundary. */
  double distance = 1e-9;
  double angle = 1e-12;
};

/** Classify against the bounded native face, including its holes. Inside means on the
 * support and inside its trims; Boundary means on a trimming edge. Supports accepted:
 * Plane, affine Cylinder, and Extrusion of a Conic. Trims must be Line or Conic.
 * No tessellation or fitting is used. Invalid/unsupported input returns nullopt and an error. */
std::optional<PointClassification> classify_face_point(const Model &model,
                                                       const Face &face,
                                                       Vec3 point,
                                                       ClassificationTolerance tolerance = {},
                                                       std::string *error = nullptr);

/** Classify only the native trimming region. The caller must already have certified that
 * point lies on the face support (for example, a vertex produced by analytic intersection).
 * This skips the redundant support residual test without projecting or moving geometry. */
std::optional<PointClassification> classify_face_trim_point(const Model &model,
                                                            const Face &face,
                                                            Vec3 point,
                                                            ClassificationTolerance tolerance = {},
                                                            std::string *error = nullptr);

/** Ray parity against a closed collection of faces. Multiple closed shells use even/odd
 * material membership, including cavities. Each boundary edge must occur twice.
 * Ray contacts with trim edges, tangencies, and numerically ambiguous intersections discard
 * the entire ray before another direction is attempted. Exhausted retries return Uncertain.
 * Unsupported surface/trim families fail explicitly even if a ray would miss them. */
std::optional<PointClassification> classify_body_point(const Model &model,
                                                       const std::vector<Id> &faces,
                                                       Vec3 point,
                                                       ClassificationTolerance tolerance = {},
                                                       std::string *error = nullptr);

std::optional<PointClassification> classify_body_point(const Model &model,
                                                       Vec3 point,
                                                       ClassificationTolerance tolerance = {},
                                                       std::string *error = nullptr);

}  // namespace blender::solid
