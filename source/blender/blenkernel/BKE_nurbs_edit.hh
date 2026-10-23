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

namespace blender {
struct Curve;
}

namespace blender::bke::nurbs {

struct EditSnapshot {
  OwnedGeometry geometry;
  SelectionState selection;
};

namespace detail {
struct EditStateStorage;
}

class EditData;

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

/**
 * Evaluated Curve's owned source handoff. A failed capture retains its diagnostic and no source;
 * clients must report it, not fall back to the older persistent payload or legacy spline lists.
 */
struct EvaluationSource {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS Evaluation Source");

  std::optional<ReadSnapshot> snapshot;
  std::string error;
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
 * Internal owner experiment, not a production mode-entry path. Requires an existing payload,
 * no writable legacy Nurb source and no shape keys. Failure leaves owner and edit state unchanged.
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

 private:
  explicit EditData(EditSnapshot snapshot);
  bool commit_impl(EditSnapshot candidate,
                   uint64_t expected_revision,
                   const detail::EditStateStorage *expected_state,
                   std::string *r_error);

  mutable std::mutex mutex_;
  std::shared_ptr<const detail::EditStateStorage> current_;
};

}  // namespace blender::bke::nurbs
