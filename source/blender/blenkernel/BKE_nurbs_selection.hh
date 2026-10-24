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
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_nurbs_types.h"

namespace blender::bke::nurbs {

/** Values are part of the persistent selection format. */
enum class ElementDomain { Vertex = 0, Edge = 1, Face = 2 };

/** Component modes may be combined, as in Blender mesh editing. Not a persistent DNA value. */
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
 * mode switching and gesture adapters require their own integration before public activation.
 */
std::optional<SelectionState> selection_flush(const NurbsGeometry &geometry,
                                              const SelectionState &selection,
                                              SelectionMode mode,
                                              std::string *r_error = nullptr);

enum class SelectionAction { Set, Add, Subtract, Invert, Intersect };

/**
 * Apply source vertex hits from any gesture, then flush in vertex mode. Click adapters may
 * explicitly nominate an active vertex; bulk gestures do not invent history. IDs are topology
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
