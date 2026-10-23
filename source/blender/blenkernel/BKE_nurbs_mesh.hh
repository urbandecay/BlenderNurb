/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_tessellate.hh"

namespace blender {
struct Mesh;
}

namespace blender::bke::nurbs {

struct MeshDeleter {
  void operator()(Mesh *mesh) const;
};
using OwnedMesh = std::unique_ptr<Mesh, MeshDeleter>;

/**
 * Derived display data with its retained source state and explicit source provenance. Transferring
 * mesh ownership to a GeometrySet does not transfer the source or make the mesh writable NURBS
 * geometry. Keep this record with the evaluated display cache for later source-hit resolution.
 */
struct MeshEvaluation {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS Mesh Evaluation");

  ReadSnapshot source;
  SurfacePreview preview;
  OwnedMesh mesh;
  /** Measured double-to-float conversion deviation; not a surface approximation error bound. */
  double max_float_position_deviation = 0.0;
  /** Index-collapsed boundary segments omitted from the display mesh, retained in source data. */
  int skipped_repeated_point_segments = 0;
  /** Computed degenerate source frames; zero custom normals request Blender's display fallback. */
  int undefined_normal_corners = 0;
};

/** Object runtime owns this immutable result; failed evaluation retains its diagnostic. */
struct ObjectEvaluation {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS Object Evaluation");

  SurfacePreviewOptions options;
  std::optional<MeshEvaluation> result;
  std::string error;
};

/**
 * Build a derived float Mesh from an immutable NURBS source. Shares boundary samples by source
 * identity and retains per-triangle face/UV provenance in the accompanying preview. Rejects float
 * overflow; reports measured float quantization. Derived diagonals are not source topology.
 * Source identities, coefficients, tolerances and selection are never changed. Corner normals
 * come from source derivatives and face orientation, with one-sided knot limits toward each
 * triangle. Computed degenerate frames are counted and use Blender's display-normal fallback.
 * Normals use Blender's normal encoding; no geometric continuity or error certificate is implied.
 * Does not apply modifiers, create texture UVs or make a persistent Mesh ID.
 */
std::optional<MeshEvaluation> evaluate_mesh(const ReadSnapshot &source,
                                            const SurfacePreviewOptions &options,
                                            std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
