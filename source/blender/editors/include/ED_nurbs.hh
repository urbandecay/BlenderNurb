/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 */

#include <cstdint>
#include <optional>
#include <string>

#include "BKE_nurbs_selection.hh"
#include "BLI_span.hh"

namespace blender {
struct Curve;
struct Main;
struct bContext;
struct rcti;
struct ViewContext;
struct SelectPick_Params;
enum eSelectOp : int8_t;
namespace bke::nurbs {
struct EditSnapshot;
class ReadSnapshot;
enum class SelectionAction;
enum class DeleteMode;
enum class SelectionMode : uint8_t;
struct ElementRef;
}  // namespace bke::nurbs
}  // namespace blender

namespace blender::ed::nurbs {

/**
 * Commit a kernel candidate and notify its Blender owner. The Curve must belong to bmain and
 * already own native edit data. Rejection leaves source, dirty flags and notifications unchanged.
 *
 * Geometry/selection validation and atomic replacement live in the kernel. This adapter marks
 * edit/save and dependency-graph state and queues a redraw only after that replacement succeeds.
 * Invalidation is deliberately conservative, including for selection-only candidates. Native
 * operator/modal undo remains the caller's responsibility; this does not push an undo step.
 */
bool edit_commit(Main &bmain,
                 Curve &curve,
                 bke::nurbs::EditSnapshot candidate,
                 uint64_t expected_revision,
                 std::string *r_error = nullptr);

/** Retained-state token also rejects results from a different edit lifetime. */
bool edit_commit(Main &bmain,
                 Curve &curve,
                 bke::nurbs::EditSnapshot candidate,
                 const bke::nurbs::ReadSnapshot &expected_state,
                 std::string *r_error = nullptr);

/** Apply V/E/F hits in the stored source mode, retaining the common commit contract. */
bool select_components(Main &bmain,
                       Curve &curve,
                       const bke::nurbs::ReadSnapshot &expected,
                       Span<bke::nurbs::ElementRef> hits,
                       bke::nurbs::SelectionAction action,
                       std::optional<bke::nurbs::ElementRef> active,
                       std::string *r_error = nullptr);

/** Change all native edit owners to the same source mode. Returns success; r_changed reports
 * whether any owner changed, including a valid no-op when the last enabled mode is toggled.
 */
bool select_mode(bContext *C,
                 bke::nurbs::SelectionMode mode,
                 bool extend,
                 bool *r_changed = nullptr);

/** Apply source vertex hits through the kernel and shared owner commit. False also means no-op. */
bool select_vertices(Main &bmain,
                     Curve &curve,
                     const bke::nurbs::ReadSnapshot &expected,
                     Span<int64_t> hits,
                     bke::nurbs::SelectionAction action,
                     std::optional<int64_t> active_vertex = std::nullopt,
                     std::string *r_error = nullptr);

/** Source-provenance component picking; ordinary selection checks viewport scene depth. */
bool select_pick(bContext *C, const int mval[2], int distance, const SelectPick_Params &params);
bool select_box(const ViewContext &vc, const rcti &rect, eSelectOp operation);

/** Grow selection through source incidence, preserving active identity/history. */
bool select_linked(Main &bmain, Curve &curve, std::string *r_error = nullptr);
/** Preflight all native edit owners before applying linked selection. No X-Ray requirement. */
bool select_linked(bContext *C);
bool select_linked_pick(bContext *C, const int mval[2], int distance, bool deselect);

/** Exact native component duplication, remapped selection/history and one owner commit. */
bool duplicate(Main &bmain, Curve &curve, std::string *r_error = nullptr);
/** Match the current native Move adapter's single edit-owner scope before changing topology. */
bool duplicate(bContext *C);

/** Straight region sweep; the existing E macro moves the selected cap after this zero sweep.
 * Report measured pre-existing endpoint disagreement separately from translation roundoff.
 */
bool extrude(Main &bmain,
             Curve &curve,
             std::string *r_error = nullptr,
             double *r_source_endpoint_deviation = nullptr);
bool extrude(bContext *C);

/** Connect or fill source components using the shared construction service. Context form
 * preflights all edit owners before committing any of them.
 */
bool fill(Main &bmain, Curve &curve, std::string *r_error = nullptr);
bool fill(bContext *C);

/** Shared component deletion and selection pruning. Context form preflights all edit owners. */
bool delete_components(Main &bmain,
                       Curve &curve,
                       bke::nurbs::DeleteMode mode,
                       std::string *r_error = nullptr);
bool delete_components(bContext *C, bke::nurbs::DeleteMode mode);

}  // namespace blender::ed::nurbs
