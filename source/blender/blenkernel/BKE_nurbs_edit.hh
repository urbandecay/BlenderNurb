/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <memory>
#include <mutex>

#include "MEM_guardedalloc.h"

#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_selection.hh"
#include "BKE_nurbs_topology_edit.hh"

namespace blender {
struct Curve;
struct EditNurb;
}  // namespace blender

namespace blender::bke::nurbs {

struct EditSnapshot {
  OwnedGeometry geometry;
  SelectionState selection;
};

namespace detail {
struct EditStateStorage;
}

class EditData;
struct ObjectEvaluation;

/**
 * Immutable geometry/selection/revision read lease. Retains its data across commits and edit-owner
 * destruction. Equality means the same runtime source state, not persistent component identity.
 */
class ReadSnapshot {
 public:
  const NurbsGeometry &geometry() const;
  const SelectionState &selection() const;
  uint64_t revision() const;
  EditSnapshot copy_for_edit() const;

  friend bool operator==(const ReadSnapshot &a, const ReadSnapshot &b) = default;

 private:
  friend class EditData;
  explicit ReadSnapshot(std::shared_ptr<const detail::EditStateStorage> state);
  std::shared_ptr<const detail::EditStateStorage> state_;
};

struct ExtractedEditSelection {
  EditSnapshot remainder;
  EditSnapshot extracted;
  Map<int64_t, int64_t> source_to_copy;
};

/** Prepare Separate's two independent sources through the common topology extraction service.
 * Require a normalized component selection. Deselect the remainder, retain the extracted
 * selection's mode/active/history, and copy its entire dependency closure. No owner mutation,
 * fitting or geometry evaluation. An empty selection returns no result and no error.
 */
std::optional<ExtractedEditSelection> extract_edit_selection(const ReadSnapshot &source,
                                                             std::string *r_error = nullptr);

/** Validate and copy a live edit snapshot into independent persistent geometry with selection. */
OwnedGeometry geometry_from_edit_snapshot(const EditSnapshot &snapshot,
                                          std::string *r_error = nullptr);

/** Select a topology service's semantic result in the requested mode, remapping surviving
 * active/history identities and validating the complete candidate. No live source mutation.
 */
std::optional<EditSnapshot> edit_snapshot_from_topology_result(TopologyEditResult result,
                                                               const SelectionState &previous,
                                                               SelectionMode mode,
                                                               std::string *r_error = nullptr);

/**
 * Evaluated Curve's owned source handoff. A failed capture retains its diagnostic and no source;
 * clients must report it, not fall back to the older persistent payload or legacy spline lists.
 */
struct EvaluationSource {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS Evaluation Source");

  std::optional<ReadSnapshot> snapshot;
  std::string error;
  /** Completed preview of this exact immutable geometry, retained across the depsgraph handoff. */
  std::shared_ptr<const ObjectEvaluation> prepared_evaluation;
};

/**
 * Capture while the host Curve/edit container is alive and its ownership is serialized. Live
 * native Edit Mode is authoritative; object mode gets an independent source copy with stored
 * selection. Already evaluated sources retain their existing lease/diagnostic without touching
 * borrowed edit pointers. Returns null for a legacy source, an owned failed handoff for invalid
 * native source, or a retained source lease. No mutation, flushing or editor notification.
 */
std::unique_ptr<EvaluationSource> curve_eval_source_capture(const Curve &curve);

/** Approximate owned storage for native undo budgeting; excludes allocator overhead and caches. */
size_t snapshot_memory_size(const EditSnapshot &snapshot);

/**
 * Begin editing an existing native payload. Normal native creation/migration remain gated.
 * Requires no writable legacy Nurb source and no shape keys. Failure leaves owner and edit state
 * unchanged.
 */
bool curve_edit_begin(Curve &curve, std::string *r_error = nullptr);
bool curve_edit_flush_to_id(Curve &curve, std::string *r_error = nullptr);
/** Legacy curves pass through; native owners must have one supported, structurally valid source.
 */
bool curve_validate_source(const Curve &curve, std::string *r_error = nullptr);

/**
 * Owned editable source with atomic candidate commits. Geometry and selection snapshots are
 * independent; restoring an old ID counter does not restore an old runtime cache revision.
 * Commit checks structure and selection references; geometric validity is an operation-specific
 * gate and is not certified by this state container.
 * This class does not itself register an editor mode or implement native Blender undo.
 */
class EditData {
 public:
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS EditData");

  static std::unique_ptr<EditData> create(const NurbsGeometry &geometry,
                                          const SelectionState &selection = {},
                                          std::string *r_error = nullptr);
  /** Adopt an independently owned candidate without copying its geometry again. */
  static std::unique_ptr<EditData> create(EditSnapshot snapshot, std::string *r_error = nullptr);

  /** Borrowed editor-thread access; invalidated by the next mutation or owner destruction. */
  const NurbsGeometry &geometry() const;
  const SelectionState &selection() const;
  uint64_t revision() const;

  /** Safe acquisition concurrent with commits, while this EditData object is alive. */
  ReadSnapshot read_snapshot() const;
  EditSnapshot snapshot() const;
  /** Numeric revisions require the same edit lifetime; prefer a read token for deferred work. */
  bool commit(EditSnapshot candidate, uint64_t expected_revision, std::string *r_error = nullptr);
  bool commit(EditSnapshot candidate,
              const ReadSnapshot &expected_state,
              std::string *r_error = nullptr);
  bool restore(const EditSnapshot &snapshot, std::string *r_error = nullptr);
  /** Commit an already evaluated immutable candidate atomically. Share its owned source and
   * retain the completed preview; the live edit revision still advances independently. */
  bool commit_evaluated(std::shared_ptr<const ObjectEvaluation> candidate,
                        const ReadSnapshot &expected_state,
                        std::string *r_error = nullptr);
  std::shared_ptr<const ObjectEvaluation> prepared_evaluation(const ReadSnapshot &source) const;

 private:
  explicit EditData(EditSnapshot snapshot);
  bool commit_impl(EditSnapshot candidate,
                   uint64_t expected_revision,
                   const detail::EditStateStorage *expected_state,
                   std::string *r_error);

  mutable std::mutex mutex_;
  std::shared_ptr<const detail::EditStateStorage> current_;
  std::shared_ptr<const ObjectEvaluation> prepared_evaluation_;
};

/** Prepared exact transition from the tagged prototype to the existing native edit pipeline.
 * Keep source stamps to reject edits made between preparation and commit. No owner is mutated
 * by preparation. Geometry/selection and persistent cache are installed together by commit.
 */
struct PreparedCurveSource {
  const Curve *owner = nullptr;
  const EditNurb *legacy_edit = nullptr;
  Vector<uint8_t> legacy_stamp;
  int active_spline = 0, active_point = 0;
  std::unique_ptr<EditData> edit;
  OwnedGeometry persistent;
};

std::optional<PreparedCurveSource> curve_edit_prepare_tagged(const Curve &curve,
                                                             SelectionMode mode,
                                                             std::string *r_error = nullptr);
bool curve_edit_commit_tagged(Curve &curve,
                              PreparedCurveSource candidate,
                              std::string *r_error = nullptr);

/** Restore the native side of an explicit tagged/native edit-undo transition. */
bool curve_edit_restore_native(Curve &curve,
                               const EditSnapshot &snapshot,
                               std::string *r_error = nullptr);
/** Release a native source for a validated tagged edit-undo snapshot. The caller owns restoring
 * the legacy edit list and key-index mapping through Blender's existing undo services.
 */
bool curve_edit_restore_tagged(Curve &curve,
                               const ListBaseT<Nurb> &snapshot,
                               std::string *r_error = nullptr);

}  // namespace blender::bke::nurbs
