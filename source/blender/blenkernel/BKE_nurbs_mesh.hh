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
struct Curve;
struct Mesh;
}  // namespace blender

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
  /** Display face index -> original double preview triangle. The preview itself is retained,
   * including collapsed editing intermediates and triangles lost to float quantization. */
  Vector<int> mesh_to_preview_triangle;
  /** Source-derived display normals for each mesh corner, retained for rigid translations. */
  Vector<float3> corner_normals;
  /** Exactly collinear float triangles omitted before building Mesh corner-normal spaces. */
  int skipped_degenerate_float_triangles = 0;
  /** Measured double-to-float conversion deviation; not a surface approximation error bound. */
  double max_float_position_deviation = 0.0;
  /** Index-collapsed boundary segments omitted from the display mesh, retained in source data. */
  int skipped_repeated_point_segments = 0;
  /** Computed degenerate source frames; zero custom normals request Blender's display fallback. */
  int undefined_normal_corners = 0;
  /** The UV triangulation came from a matching prior preview; positions were re-evaluated
   * unless this is an explicitly tagged interactive translation preview. */
  bool reused_tessellation = false;
  /** Source derivatives were unchanged by a uniform support translation. */
  bool reused_normals = false;
  /** A completed preview of the identical immutable geometry was copied for this evaluation. */
  bool reused_evaluation = false;
  /** Temporary component translation of an existing display, rebuilt from the source on
   * confirmation. Its positions are transported from the drag's original preview, never from
   * a previous translated frame. Source coefficients and tolerances remain authoritative. */
  bool is_translation_preview = false;
};

/** Object runtime owns this immutable result; failed evaluation retains its diagnostic. */
struct ObjectEvaluation {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS Object Evaluation");

  SurfacePreviewOptions options;
  std::optional<MeshEvaluation> result;
  std::string error;
};

/** Shared native/compatibility preview density. A legacy two-by-two patch has 2 * resolution
 * samples, including its endpoints, so use 2 * resolution - 1 intervals per span. Preserve that
 * grid density when entering native editing. The current uniform preview uses the larger U/V
 * setting; render overrides are applied independently before choosing that setting. */
SurfacePreviewOptions curve_preview_options(const Curve &curve, bool for_render = false);

/**
 * Build a derived float Mesh from an immutable NURBS source. Shares boundary samples by source
 * identity and retains per-triangle face/UV provenance in the accompanying preview. Exactly
 * collinear float triangles are counted and omitted from the Mesh, with an explicit map back to
 * the unchanged double preview. No area epsilon or coordinate welding is used. Rejects float
 * overflow; reports measured float quantization. Derived diagonals are not source topology.
 * The mesh renderer's optimal-display mask keeps U/V wires on rectangular charts and triangle
 * wires on other trims, independently for each source face. Boundary and loose source edges
 * remain visible. Source identities, coefficients, tolerances and selection are never changed.
 * Corner normals come from source derivatives and face orientation, with one-sided knot limits
 * toward each triangle. Computed degenerate frames are counted and use Blender's display-normal
 * fallback. Regular frames use Blender's floating-point free corner normals; undefined frames
 * retain its encoded automatic fallback. No continuity or derivative error certificate is implied.
 * A previous successful object evaluation can provide a reusable UV triangulation; it
 * must retain the source and options that generated it. Does not apply modifiers, create texture
 * UVs or make a persistent Mesh ID.
 */
std::optional<MeshEvaluation> evaluate_mesh(const ReadSnapshot &source,
                                            const SurfacePreviewOptions &options,
                                            std::string *r_error = nullptr,
                                            const ObjectEvaluation *previous = nullptr);

/** Transport an unchanged display during a translation of whole independent components. Each
 * active surface and edge curve must be either unchanged or translated uniformly by the requested
 * amount; shared display samples must agree on their displacement. Partial control-net
 * deformations fall back to ordinary evaluation. The caller must use the same preview settings as
 * previous.
 * The independent display_mesh must correspond to previous (its mesh may have been transferred
 * to the renderer). Reject changes in float triangle visibility, undefined source normals and
 * overflow; callers use ordinary evaluation in those cases. Rounded sample transport is only
 * an interactive preview, not a fresh source evaluation or geometric-error certificate. The
 * editor must discard it on confirmation and cancellation. */
std::optional<MeshEvaluation> translate_mesh_preview(const ReadSnapshot &source,
                                                     const ObjectEvaluation &previous,
                                                     const Mesh &display_mesh,
                                                     const double3 &translation);

}  // namespace blender::bke::nurbs
