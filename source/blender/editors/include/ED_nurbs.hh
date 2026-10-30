/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 */

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "BKE_nurbs_selection.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"

namespace blender {
struct Curve;
struct Main;
struct Object;
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
enum class VertexMergeMode;
struct ElementRef;
struct TopologyEditResult;
struct ObjectEvaluation;
}  // namespace bke::nurbs
}  // namespace blender

namespace blender::ed::nurbs {

/** Read selected native topology endpoints through a retained source snapshot, transform in
 * double precision and convert once to viewport coordinates. Reject the complete query if a
 * coordinate cannot be represented by the viewport. These proxies never update source geometry.
 */
std::optional<Vector<float3>> view_selection_positions(const bke::nurbs::ReadSnapshot &source,
                                                       const float4x4 &transform,
                                                       std::string *r_error = nullptr);

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

/** Adopt an independently owned evaluated candidate and notify the owner. Interactive display
 * previews remain the caller's responsibility and must be discarded before confirmation. */
bool edit_commit_evaluated(Main &bmain,
                           Curve &curve,
                           std::shared_ptr<const bke::nurbs::ObjectEvaluation> candidate,
                           const bke::nurbs::ReadSnapshot &expected,
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

/** Disconnect selected source components through shared extraction and selection remapping.
 * Context form preflights all edit owners and exact tagged source entry before committing any
 * of them. Unrelated legacy Curve editing keeps its original operator route.
 */
bool split(Main &bmain, Curve &curve, std::string *r_error = nullptr);
bool split(bContext *C);

/** Move the selected closure to a new Blender object per unique edited source. Preflight exact
 * extraction, persistence and any tagged source entry for all owners before changing the scene.
 * Keep the original objects in Edit Mode and deselect their remaining components, as mesh
 * Separate.
 */
bool separate(bContext *C);

/** Native topology tools also accept exact tagged source entry, retaining legacy curve tools. */
bool topology_edit_poll(bContext *C);
bool rip(bContext *C, const int mval[2]);
bool merge(bContext *C, bke::nurbs::VertexMergeMode mode, double max_position_error);
bool merge_by_distance(bContext *C,
                       double distance,
                       bool use_centroid,
                       bool use_unselected,
                       double max_position_error);
bool dissolve_limited(bContext *C, double angle_limit);
bool subdivide(bContext *C, int cuts, double max_error);

/** Retained original source and optional exact tagged entry for preview-based topology tools.
 * Preview candidates never mutate the owner. A final commit validates the original source token
 * or tagged stamp, maps selection and uses the common edit lifecycle exactly once.
 */
class TopologyPreview {
 public:
  static std::unique_ptr<TopologyPreview> begin(bContext *C, std::string *r_error);
  ~TopologyPreview();
  const bke::nurbs::ReadSnapshot &source() const;
  Object &object() const;
  /** Replace the live native edit preview from the retained original. Cancel restores its exact
   * geometry/selection and dirty state; runtime revisions remain monotonic. Tagged sources can
   * use final commit directly, but must enter native component mode for a live preview. */
  bool update_preview(bContext *C, bke::nurbs::TopologyEditResult result, std::string *r_error);
  /** Install an evaluated candidate without discarding its immutable source or display work. */
  bool update_evaluated_preview(bContext *C,
                                std::shared_ptr<const bke::nurbs::ObjectEvaluation> candidate,
                                std::string *r_error);
  bool cancel_preview(bContext *C, std::string *r_error);
  bool commit(bContext *C,
              bke::nurbs::TopologyEditResult result,
              bke::nurbs::SelectionMode mode,
              std::string *r_error);
  bool commit_evaluated(bContext *C,
                        std::shared_ptr<const bke::nurbs::ObjectEvaluation> candidate,
                        double geometry_error_bound,
                        std::string *r_error);

 private:
  struct State;
  std::unique_ptr<State> state_;
  explicit TopologyPreview(std::unique_ptr<State> state);
};

std::optional<int64_t> topology_edge_under_cursor(bContext *C,
                                                  const TopologyPreview &preview,
                                                  const int mval[2],
                                                  std::string *r_error);
std::optional<std::array<float2, 2>> topology_edge_screen(bContext *C,
                                                          const TopologyPreview &preview,
                                                          int64_t edge_id,
                                                          std::string *r_error);

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
