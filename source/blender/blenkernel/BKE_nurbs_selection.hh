/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <optional>
#include <string>

#include "BLI_enum_flags.hh"
#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_nurbs_types.h"

namespace blender::bke::nurbs {

/** Values are part of the persistent selection format. */
enum class ElementDomain { Vertex = 0, Edge = 1, Face = 2 };

/** Persistent component-mode flags; combinations follow Blender mesh selection precedence. */
enum class SelectionMode : uint8_t { Vertex = 1, Edge = 2, Face = 4 };
ENUM_OPERATORS(SelectionMode)

struct ElementRef {
  ElementDomain domain;
  int64_t id;
  friend bool operator==(const ElementRef &a, const ElementRef &b) = default;
};

/** Topology selection. Fit-point constraints/handles are a separate source domain. */
struct SelectionState {
  Set<int64_t> vertices;
  Set<int64_t> edges;
  Set<int64_t> faces;
  std::optional<ElementRef> active;
  Vector<ElementRef> history;
  SelectionMode mode = SelectionMode::Vertex;
};

/** Check source references, active state and history; does not flush mesh-style V/E/F selection.
 */
bool selection_validate(const SelectionState &selection,
                        FunctionRef<bool(ElementRef)> exists,
                        std::string *r_error = nullptr);

/**
 * Normalize a bulk source selection using Blender's component-mode flush rules. Vertex mode
 * derives edges from their endpoints; vertex/edge modes derive faces from all boundary edges.
 * Edge/face-only modes first reconcile incident lower domains, with selected components winning
 * over adjacent unselected ones. Mixed modes give vertex, then edge, then face precedence.
 * Corresponds to BMesh mode flushing with the explicit Down flag, not mode conversion or a
 * selection gesture. All face loops participate, including holes and repeated seam uses.
 * Geometry and input selection remain unchanged; failures return no candidate. Retain surviving
 * history order and active identity, without inventing history for inferred selections.
 * No hidden/selectability state is stored yet: all source components participate. Visibility,
 * gesture adapters provide their own viewport visibility; mode conversion uses
 * selection_change_mode.
 */
std::optional<SelectionState> selection_flush(const NurbsGeometry &geometry,
                                              const SelectionState &selection,
                                              SelectionMode mode,
                                              std::string *r_error = nullptr);

enum class SelectionAction { Set, Add, Subtract, Invert, Intersect };

/** Change the source component mode, reconciling its closure and active/history identities. */
std::optional<SelectionState> selection_change_mode(const NurbsGeometry &geometry,
                                                    const SelectionState &selection,
                                                    SelectionMode mode,
                                                    std::string *r_error = nullptr);

/** Apply source V/E/F hits in the current mode. Geometry and inputs remain unchanged.
 * Lower-domain selection follows source incidence; inferred elements do not acquire history.
 */
std::optional<SelectionState> selection_apply(const NurbsGeometry &geometry,
                                              const SelectionState &selection,
                                              Span<ElementRef> hits,
                                              SelectionAction action,
                                              std::optional<ElementRef> active = std::nullopt,
                                              std::string *r_error = nullptr);

bool selection_contains(const SelectionState &selection, ElementRef element);

/** Unique selected topology endpoint positions, in source order and source precision.
 * Normalize V/E/F selection first so edge and face selections include their endpoint closure.
 * Empty selection returns an empty vector. No coefficient/fit-point/display samples, mutation,
 * owner transition or ID allocation. Failed source/selection validation returns no result.
 */
std::optional<Vector<double3>> selection_positions(const NurbsGeometry &geometry,
                                                   const SelectionState &selection,
                                                   std::string *r_error = nullptr);

/** Source representatives for UI centers: vertex position, curve interval midpoint, and
 * the mean of unique face-boundary vertices. A face center need not lie on its support or trim.
 * No source mutation, fitting, normal frame or geometric approximation certificate.
 */
std::optional<Vector<double3>> element_centers(const NurbsGeometry &geometry,
                                               Span<ElementRef> elements,
                                               std::string *r_error = nullptr);

enum class SelectionPivot { Median, Bounds, Active, Individual };

struct SelectionTransformGroup {
  Vector<int64_t> vertices;
  double3 center;
};

/**
 * Source-precision transform origins. Median/bounds use selected endpoints; Active uses the
 * existing source representative with median fallback. Individual groups follow mesh selection
 * precedence: connected selected edges in Vertex/Edge modes, or faces connected by vertices in
 * exclusive Face mode. Each group's center is the mean of its edge/face representatives;
 * remaining loose selected vertices use their own positions. No display geometry or ID allocation.
 */
std::optional<Vector<SelectionTransformGroup>> selection_transform_groups(
    const NurbsGeometry &geometry,
    const SelectionState &selection,
    SelectionPivot pivot,
    std::string *r_error = nullptr);

/**
 * Vertex-hit wrapper for the generic current-mode service; Vertex must be enabled. Click adapters
 * may explicitly nominate an active vertex; bulk gestures do not invent history. IDs are topology
 * vertices, never control coefficients, display indices or fit handles. Invalid input/hits fail
 * atomically. All source components participate until visibility storage is introduced.
 */
std::optional<SelectionState> selection_apply_vertices(
    const NurbsGeometry &geometry,
    const SelectionState &selection,
    Span<int64_t> hits,
    SelectionAction action,
    std::optional<int64_t> active_vertex = std::nullopt,
    std::string *r_error = nullptr);

/**
 * Remap a validated source selection to target component identities, then flush in the explicit
 * target mode. Unmapped elements are dropped. History follows its surviving identities; repeated
 * targets keep their last history occurrence. Invalid mapped domains/IDs fail without mutation.
 * No history is invented for inferred selection. Geometry/resource IDs not selected are ignored.
 */
std::optional<SelectionState> selection_remap(const NurbsGeometry &source,
                                              const NurbsGeometry &target,
                                              const SelectionState &selection,
                                              const Map<int64_t, int64_t> &id_map,
                                              SelectionMode mode,
                                              std::string *r_error = nullptr);

/**
 * Remap a selected copy's complete component closure, including dependencies not selected in
 * the input (for example a wire endpoint in Face mode). Match Blender's selection of all copied
 * output components. Preserve only existing active/history entries through the identity map.
 */
std::optional<SelectionState> selection_remap_closure(const NurbsGeometry &source,
                                                      const NurbsGeometry &target,
                                                      const SelectionState &selection,
                                                      const Map<int64_t, int64_t> &id_map,
                                                      SelectionMode mode,
                                                      std::string *r_error = nullptr);

/**
 * Drop deleted component identities from an already valid selection and its history. IDs are
 * unique across source domains. Preserve other flags without inferring a selection mode; the
 * caller can flush afterward. Resolve a removed active element from surviving history.
 */
SelectionState selection_remove_ids(const SelectionState &selection, const Set<int64_t> &ids);

/** Decode owned DNA arrays. Validate the resulting references against the source separately. */
std::optional<SelectionState> selection_from_storage(const NurbsStoredSelection *storage,
                                                     std::string *r_error = nullptr);
/**
 * Allocate deterministic DNA storage, including an empty state. Caller validates source
 * references before attaching it to an owner. Null means a storage-size failure.
 */
NurbsStoredSelection *selection_to_storage(const SelectionState &selection,
                                           std::string *r_error = nullptr);
NurbsStoredSelection *selection_storage_copy(const NurbsStoredSelection *storage);
void selection_storage_free(NurbsStoredSelection *storage);

}  // namespace blender::bke::nurbs
