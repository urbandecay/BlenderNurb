/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <limits>

#include "BLI_listbase.h"

#include "BKE_curve.hh"
#include "BKE_nurbs_edit.hh"

namespace blender::bke::nurbs {

namespace detail {
struct EditStateStorage {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS Edit State");

  EditSnapshot snapshot;
  uint64_t revision;

  EditStateStorage(EditSnapshot snapshot, const uint64_t revision)
      : snapshot(std::move(snapshot)), revision(revision)
  {
  }
};
}  // namespace detail

static bool source_validate(const NurbsGeometry &geometry,
                            const SelectionState &selection,
                            std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return false;
  };
  if (geometry.stored_selection != nullptr) {
    return fail("NURBS edit snapshots must keep selection in the live selection state");
  }
  const auto index = TopologyIndex::build(geometry, r_error);
  if (!index) {
    return false;
  }
  return selection_validate(
      selection,
      [&](const ElementRef element) {
        switch (element.domain) {
          case ElementDomain::Vertex:
            return index->vertex(element.id) != nullptr;
          case ElementDomain::Edge:
            return index->edge(element.id) != nullptr;
          case ElementDomain::Face:
            return index->face(element.id) != nullptr;
        }
        return false;
      },
      r_error);
}

static bool snapshot_validate(const EditSnapshot &snapshot, std::string *r_error)
{
  if (!snapshot.geometry) {
    if (r_error != nullptr) {
      *r_error = "NURBS edit snapshot has no source geometry";
    }
    return false;
  }
  return source_validate(*snapshot.geometry, snapshot.selection, r_error);
}

static EditSnapshot snapshot_copy(const EditSnapshot &snapshot)
{
  EditSnapshot copy;
  copy.geometry.reset(geometry_copy(snapshot.geometry.get()));
  copy.selection = snapshot.selection;
  return copy;
}

ReadSnapshot::ReadSnapshot(std::shared_ptr<const detail::EditStateStorage> state)
    : state_(std::move(state))
{
}

const NurbsGeometry &ReadSnapshot::geometry() const
{
  return *state_->snapshot.geometry;
}

const SelectionState &ReadSnapshot::selection() const
{
  return state_->snapshot.selection;
}

uint64_t ReadSnapshot::revision() const
{
  return state_->revision;
}

EditSnapshot ReadSnapshot::copy_for_edit() const
{
  return snapshot_copy(state_->snapshot);
}

EditData::EditData(EditSnapshot snapshot)
    : current_(new detail::EditStateStorage(std::move(snapshot), 1))
{
}

std::unique_ptr<EditData> EditData::create(const NurbsGeometry &geometry,
                                           const SelectionState &selection,
                                           std::string *r_error)
{
  if (!geometry_validate_structure(geometry, r_error)) {
    return nullptr;
  }
  EditSnapshot snapshot;
  snapshot.geometry.reset(geometry_copy(&geometry));
  selection_storage_free(snapshot.geometry->stored_selection);
  snapshot.geometry->stored_selection = nullptr;
  snapshot.selection = selection;
  if (!snapshot_validate(snapshot, r_error)) {
    return nullptr;
  }
  return std::unique_ptr<EditData>(new EditData(std::move(snapshot)));
}

const NurbsGeometry &EditData::geometry() const
{
  return *current_->snapshot.geometry;
}

const SelectionState &EditData::selection() const
{
  return current_->snapshot.selection;
}

uint64_t EditData::revision() const
{
  return read_snapshot().revision();
}

ReadSnapshot EditData::read_snapshot() const
{
  std::scoped_lock lock(mutex_);
  return ReadSnapshot(current_);
}

EditSnapshot EditData::snapshot() const
{
  return read_snapshot().copy_for_edit();
}

bool EditData::commit(EditSnapshot candidate,
                      const uint64_t expected_revision,
                      std::string *r_error)
{
  return commit_impl(std::move(candidate), expected_revision, nullptr, r_error);
}

bool EditData::commit(EditSnapshot candidate,
                      const ReadSnapshot &expected_state,
                      std::string *r_error)
{
  return commit_impl(
      std::move(candidate), expected_state.revision(), expected_state.state_.get(), r_error);
}

bool EditData::commit_impl(EditSnapshot candidate,
                           const uint64_t expected_revision,
                           const detail::EditStateStorage *expected_state,
                           std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto matches = [&]() {
    return expected_revision == current_->revision &&
           current_->revision != std::numeric_limits<uint64_t>::max() &&
           (expected_state == nullptr || expected_state == current_.get());
  };
  auto stale = [&]() {
    if (r_error != nullptr) {
      *r_error =
          "NURBS edit candidate has a stale source state, edit lifetime or exhausted revision";
    }
    return false;
  };
  {
    std::scoped_lock lock(mutex_);
    if (!matches()) {
      return stale();
    }
  }
  if (!snapshot_validate(candidate, r_error)) {
    return false;
  }
  std::shared_ptr<const detail::EditStateStorage> next(
      new detail::EditStateStorage(std::move(candidate), expected_revision + 1));
  {
    std::scoped_lock lock(mutex_);
    if (!matches()) {
      return stale();
    }
    current_.swap(next);
  }
  /* Release the previous state outside the lock; readers may still retain it. */
  return true;
}

bool EditData::restore(const EditSnapshot &snapshot, std::string *r_error)
{
  if (!snapshot_validate(snapshot, r_error)) {
    return false;
  }
  const ReadSnapshot expected = read_snapshot();
  return commit(snapshot_copy(snapshot), expected, r_error);
}

size_t snapshot_memory_size(const EditSnapshot &snapshot)
{
  size_t bytes = sizeof(EditSnapshot) + snapshot.selection.vertices.size_in_bytes() +
                 snapshot.selection.edges.size_in_bytes() +
                 snapshot.selection.faces.size_in_bytes() +
                 size_t(snapshot.selection.history.capacity()) * sizeof(ElementRef);
  if (!snapshot.geometry) {
    return bytes;
  }
  const NurbsGeometry &geometry = *snapshot.geometry;
  bytes += sizeof(NurbsGeometry);
  for (const NurbsCurveGeometry &curve : geometry.curves) {
    bytes += sizeof(curve) + size_t(curve.points_num) * sizeof(NurbsControlPoint) +
             size_t(curve.knots_num) * sizeof(double);
  }
  for (const NurbsSurfaceGeometry &surface : geometry.surfaces) {
    bytes += sizeof(surface) + size_t(surface.points_num) * sizeof(NurbsControlPoint) +
             (size_t(surface.knots_u_num) + size_t(surface.knots_v_num)) * sizeof(double);
  }
  bytes += size_t(geometry.vertices.count()) * sizeof(NurbsTopologyVertex) +
           size_t(geometry.edges.count()) * sizeof(NurbsTopologyEdge);
  for (const NurbsTopologyFace &face : geometry.faces) {
    bytes += sizeof(face);
    for (const NurbsTopologyLoop &loop : face.loops) {
      bytes += sizeof(loop) + size_t(loop.edge_uses.count()) * sizeof(NurbsTopologyEdgeUse);
    }
  }
  return bytes;
}

bool curve_validate_source(const Curve &curve, std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return false;
  };
  if (curve.nurbs_eval_source != nullptr) {
    return fail("An evaluated NURBS source lease is not a writable Curve owner");
  }
  const EditData *edit = curve.editnurb == nullptr ? nullptr : curve.editnurb->nurbs_edit;
  if (curve.nurbs_geometry == nullptr) {
    return edit == nullptr ? true : fail("NURBS edit source has no persistent owner payload");
  }
  if (!ELEM(curve.ob_type, OB_CURVES_LEGACY, OB_SURF) || !curve.nurb.is_empty() || curve.key ||
      (curve.editnurb != nullptr && !curve.editnurb->nurbs.is_empty()))
  {
    return fail(
        "NURBS source conflicts with legacy splines, shape keys or an unsupported owner type");
  }
  if (curve.editnurb != nullptr && edit == nullptr) {
    return fail("NURBS source has incompatible legacy edit data");
  }
  /* A live edit source is authoritative. Its older ID cache may be replaced during flush. */
  if (edit != nullptr) {
    const ReadSnapshot snapshot = edit->read_snapshot();
    return source_validate(snapshot.geometry(), snapshot.selection(), r_error);
  }
  return geometry_validate_structure(*curve.nurbs_geometry, r_error);
}

std::unique_ptr<EvaluationSource> curve_eval_source_capture(const Curve &curve)
{
  if (curve.nurbs_eval_source != nullptr) {
    return std::make_unique<EvaluationSource>(*curve.nurbs_eval_source);
  }
  const EditData *edit = curve.editnurb == nullptr ? nullptr : curve.editnurb->nurbs_edit;
  if (curve.nurbs_geometry == nullptr && edit == nullptr) {
    return nullptr;
  }
  auto result = std::make_unique<EvaluationSource>();
  if (!curve_validate_source(curve, &result->error)) {
    return result;
  }
  if (edit != nullptr) {
    result->snapshot = edit->read_snapshot();
  }
  else {
    const auto selection = selection_from_storage(curve.nurbs_geometry->stored_selection,
                                                  &result->error);
    if (!selection) {
      return result;
    }
    const auto state = EditData::create(*curve.nurbs_geometry, *selection, &result->error);
    if (!state) {
      return result;
    }
    result->snapshot = state->read_snapshot();
  }
  return result;
}

bool curve_edit_begin(Curve &curve, std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return false;
  };
  if (curve.nurbs_geometry == nullptr) {
    return fail("Curve has no NURBS source payload");
  }
  if (!curve_validate_source(curve, r_error)) {
    return false;
  }
  if (curve.editnurb != nullptr) {
    return fail("Curve already owns edit data");
  }
  const auto selection = selection_from_storage(curve.nurbs_geometry->stored_selection, r_error);
  if (!selection) {
    return false;
  }
  std::unique_ptr<EditData> edit = EditData::create(*curve.nurbs_geometry, *selection, r_error);
  if (!edit) {
    return false;
  }
  EditNurb *editnurb = MEM_new<EditNurb>(__func__);
  editnurb->nurbs_edit = edit.release();
  curve.editnurb = editnurb;
  return true;
}

bool curve_edit_flush_to_id(Curve &curve, std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return false;
  };
  if (!curve_validate_source(curve, r_error)) {
    return false;
  }
  if (curve.editnurb == nullptr || curve.editnurb->nurbs_edit == nullptr) {
    return fail("Curve has no NURBS edit source");
  }
  const NurbsGeometry &source = curve.editnurb->nurbs_edit->geometry();
  if (!geometry_validate_structure(source, r_error)) {
    return false;
  }
  OwnedGeometry copy(geometry_copy(&source));
  copy->stored_selection = selection_to_storage(curve.editnurb->nurbs_edit->selection(), r_error);
  if (copy->stored_selection == nullptr || !geometry_validate_structure(*copy, r_error)) {
    return false;
  }
  geometry_free(curve.nurbs_geometry);
  curve.nurbs_geometry = copy.release();
  curve.editnurb->needs_flush_to_id = 0;
  return true;
}

}  // namespace blender::bke::nurbs
