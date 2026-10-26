/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <memory>
#include <optional>
#include <string>

#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_nurbs_types.h"

namespace blender {
struct BlendDataReader;
struct BlendWriter;
struct Nurb;
}  // namespace blender

namespace blender::bke::nurbs {

/** Null-safe ownership services for the experimental native payload. */
NurbsGeometry *geometry_copy(const NurbsGeometry *geometry);
void geometry_free(NurbsGeometry *geometry);

struct GeometryDeleter {
  void operator()(NurbsGeometry *geometry) const
  {
    geometry_free(geometry);
  }
};
using OwnedGeometry = std::unique_ptr<NurbsGeometry, GeometryDeleter>;

/** Standalone coefficient resources; release before transferring to an owning geometry. */
struct CurveDeleter {
  void operator()(NurbsCurveGeometry *curve) const;
};
using OwnedCurve = std::unique_ptr<NurbsCurveGeometry, CurveDeleter>;
struct SurfaceDeleter {
  void operator()(NurbsSurfaceGeometry *surface) const;
};
using OwnedSurface = std::unique_ptr<NurbsSurfaceGeometry, SurfaceDeleter>;

/** Source topology IDs, independent of selection gesture, coefficient storage or display data. */
struct ComponentSelection {
  Span<int64_t> vertices;
  Span<int64_t> edges;
  Span<int64_t> faces;
};

/** Read-only dependency closure, shared by copying and component construction. No ID allocation.
 */
struct ComponentClosure {
  Set<int64_t> vertices;
  Set<int64_t> edges;
  Set<int64_t> faces;
  Set<int64_t> curves;
  Set<int64_t> surfaces;
  uint64_t boundary_records = 0;
};

std::optional<ComponentClosure> component_closure(const NurbsGeometry &source,
                                                  const ComponentSelection &selection,
                                                  std::string *r_error = nullptr);

struct GeometryCopyResult {
  OwnedGeometry geometry;
  /** Includes geometry resources, topology components, loops and edge uses in the closure. */
  Map<int64_t, int64_t> source_to_copy;
};

/**
 * Copy selected components and their dependencies to an independent geometry fragment. Faces
 * include loops/edges/vertices/supports/p-curves; edges include endpoints and their 3D curves.
 * Incident faces are not included merely because an edge or vertex is selected. Preserve all
 * sharing inside the copy, while allocating fresh IDs starting at the source's next identity.
 * Selection order and repeated IDs do not affect the result. Source and its counter are unchanged.
 * The caller must commit the fragment and new counter together before reusing those identities.
 */
std::optional<GeometryCopyResult> copy_component_closure(const NurbsGeometry &source,
                                                         const ComponentSelection &selection,
                                                         std::string *r_error = nullptr);

/**
 * Duplicate a component closure inside a full independent source candidate. Reuse the exact
 * closure copy above, preserving sharing inside each copy but giving every copied record a fresh
 * identity. Original records, coefficients, trims and stored selection remain unchanged. Returns
 * the complete candidate and original-to-copy map; does not select or commit the new components.
 */
std::optional<GeometryCopyResult> duplicate_components(const NurbsGeometry &source,
                                                       const ComponentSelection &selection,
                                                       std::string *r_error = nullptr);

void geometry_blend_write(BlendWriter *writer, const NurbsGeometry &geometry);
void geometry_blend_read(BlendDataReader *reader, NurbsGeometry &geometry);

/** Transient correspondence to the unchanged legacy input; keys are not persistent handles. */
struct LegacyGeometryMap {
  Map<const Nurb *, int64_t> curves;
  Map<const Nurb *, int64_t> surfaces;
};

/**
 * Build a geometry-only candidate from tagged loose points, two-point lines and two-by-two
 * patches. Preserve explicit component IDs and source coefficients/knots; never infer identity
 * from coordinates. Unsupported or inconsistent input fails atomically, leaving the input and
 * output map unchanged. The caller owns the result. This does not activate a new owner, migrate
 * attributes/selection, or create fit-point editing constraints.
 */
NurbsGeometry *geometry_from_tagged_legacy(const ListBaseT<Nurb> &nurbs,
                                           LegacyGeometryMap *r_map = nullptr,
                                           std::string *r_error = nullptr);

/**
 * Validate storage and topological references without modifying the source. Does not certify
 * geometric edge/support agreement, rational denominator regularity, trim intersections,
 * continuity, orientation or solid validity.
 */
bool geometry_validate_structure(const NurbsGeometry &geometry, std::string *r_error = nullptr);

bool curve_validate_structure(const NurbsCurveGeometry &curve, std::string *r_error = nullptr);
bool surface_validate_structure(const NurbsSurfaceGeometry &surface,
                                std::string *r_error = nullptr);

struct PointEvaluation {
  double3 position;
  /**
   * Sum of absolute weighted basis terms divided by the absolute rational denominator.
   * One for non-negative weights; larger values indicate cancellation. This is a
   * conditioning diagnostic, not a geometric error bound.
   */
  double weight_condition;
};

/**
 * Evaluate the explicit rational source, without fitting, parameter clamping or source mutation.
 * Invalid storage, parameters outside the domain, denominator zeros and non-finite arithmetic
 * fail explicitly. Interior knots use their right limit; the domain end uses its left limit.
 * These scalar services validate storage on each call; batch evaluation can share that work later.
 * Round-to-nearest arithmetic and subnormal support are required; other modes fail explicitly.
 */
std::optional<PointEvaluation> evaluate_curve(const NurbsCurveGeometry &curve,
                                              double parameter,
                                              std::string *r_error = nullptr);
std::optional<PointEvaluation> evaluate_surface(const NurbsSurfaceGeometry &surface,
                                                double2 parameter,
                                                std::string *r_error = nullptr);

struct CurveBasis {
  int first_coefficient = 0;
  /** Rational basis values for the nonzero span; weights are already included. */
  Vector<double> values;
  double weight_condition = 1.0;
};

/** Evaluate the same rational basis used by the authoritative curve evaluator. No source edits.
 * The result is a numerical basis evaluation, not a certified point/fit error bound.
 */
std::optional<CurveBasis> evaluate_curve_basis(const NurbsCurveGeometry &curve,
                                               double parameter,
                                               std::string *r_error = nullptr);

enum class KnotSide { Right, Left };

struct SurfaceLimits {
  KnotSide u = KnotSide::Right;
  KnotSide v = KnotSide::Right;
};

struct SurfaceDifferential {
  PointEvaluation point;
  double3 tangent_u;
  double3 tangent_v;
  /** Absent for a computed degenerate tangent frame, not a proof of exact source singularity. */
  std::optional<double3> normal;
};

/**
 * Evaluate first rational derivatives and a unit support normal from the source basis. Knot
 * sides choose one-sided limits without perturbing the parameter; domain ends use the interior
 * limit. No face orientation, fitting, source mutation or derivative error certificate. Rejects
 * non-finite/unrepresentable intermediate arithmetic; the scalar evaluation's mode rules apply.
 */
std::optional<SurfaceDifferential> evaluate_surface_differential(
    const NurbsSurfaceGeometry &surface,
    double2 parameter,
    SurfaceLimits limits = {},
    std::string *r_error = nullptr);

/** Evaluate the interior limiting normal at an exactly collapsed, clamped natural boundary.
 * Uses the adjacent coefficient row and its rational derivative, without perturbing parameters.
 * Returns absent for unsupported or degenerate limits. No regularity or error certificate.
 */
std::optional<double3> evaluate_surface_pole_normal(const NurbsSurfaceGeometry &surface,
                                                    double2 parameter,
                                                    SurfaceLimits limits = {});

struct EdgeUseReference {
  int64_t face_id;
  int64_t loop_id;
  const NurbsTopologyEdgeUse *use;
};

/**
 * Derived incidence for all topology clients. References the source; rebuild after any source
 * mutation and discard before freeing it. No coordinates or tessellation participate in identity.
 */
class TopologyIndex {
 public:
  static std::optional<TopologyIndex> build(const NurbsGeometry &geometry,
                                            std::string *r_error = nullptr);

  const NurbsTopologyVertex *vertex(int64_t id) const;
  const NurbsTopologyEdge *edge(int64_t id) const;
  const NurbsTopologyFace *face(int64_t id) const;
  Span<int64_t> edges_for_vertex(int64_t id) const;
  Span<EdgeUseReference> uses_for_edge(int64_t id) const;

 private:
  Map<int64_t, const NurbsTopologyVertex *> vertices_;
  Map<int64_t, const NurbsTopologyEdge *> edges_;
  Map<int64_t, const NurbsTopologyFace *> faces_;
  Map<int64_t, Vector<int64_t>> vertex_edges_;
  Map<int64_t, Vector<EdgeUseReference>> edge_uses_;
};

}  // namespace blender::bke::nurbs
