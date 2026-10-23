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

/** Apply source vertex hits through the kernel and shared owner commit. False also means no-op. */
bool select_vertices(Main &bmain,
                     Curve &curve,
                     const bke::nurbs::ReadSnapshot &expected,
                     Span<int64_t> hits,
                     bke::nurbs::SelectionAction action,
                     std::optional<int64_t> active_vertex = std::nullopt,
                     std::string *r_error = nullptr);

/** Initial topology-vertex adapters require X-Ray; surface occlusion picking is a later gate. */
bool select_pick(bContext *C, const int mval[2], int distance, const SelectPick_Params &params);
bool select_box(const ViewContext &vc, const rcti &rect, eSelectOp operation);

}  // namespace blender::ed::nurbs
