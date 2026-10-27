/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cstring>
#include <limits>

#include "BLI_listbase.h"

#include "BKE_curve.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_legacy_edit.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_topology.hh"

namespace blender::bke::nurbs {

namespace detail {
struct EditStateStorage {
  MEM_CXX_CLASS_ALLOC_FUNCS("NURBS Edit State");

  std::shared_ptr<const EditSnapshot> snapshot;
  uint64_t revision;

  EditStateStorage(EditSnapshot snapshot, const uint64_t revision)
      : snapshot(std::make_shared<EditSnapshot>(std::move(snapshot))), revision(revision)
  {
  }
  EditStateStorage(std::shared_ptr<const EditSnapshot> snapshot, const uint64_t revision)
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

std::optional<ExtractedEditSelection> extract_edit_selection(const ReadSnapshot &source,
                                                             std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  const SelectionState &selection = source.selection();
  if (selection.vertices.is_empty() && selection.edges.is_empty() && selection.faces.is_empty()) {
    return std::nullopt;
  }
  const auto normalized = selection_flush(source.geometry(), selection, selection.mode, r_error);
  if (!normalized) {
    return std::nullopt;
  }
  if (normalized->vertices != selection.vertices || normalized->edges != selection.edges ||
      normalized->faces != selection.faces)
  {
    if (r_error) {
      *r_error = "NURBS extraction requires a normalized component-mode selection";
    }
    return std::nullopt;
  }
  const Vector<int64_t> vertices(selection.vertices.begin(), selection.vertices.end());
  const Vector<int64_t> edges(selection.edges.begin(), selection.edges.end());
  const Vector<int64_t> faces(selection.faces.begin(), selection.faces.end());
  auto extraction = extract_components(source.geometry(), {vertices, edges, faces}, r_error);
  if (!extraction) {
    return std::nullopt;
  }
  auto extracted_selection = selection_remap_closure(source.geometry(),
                                                     *extraction->extracted.geometry,
                                                     selection,
                                                     extraction->extracted.source_to_copy,
                                                     selection.mode,
                                                     r_error);
  if (!extracted_selection) {
    return std::nullopt;
  }
  ExtractedEditSelection result;
  result.remainder = {std::move(extraction->remainder), {}};
  result.remainder.selection.mode = selection.mode;
  result.extracted = {std::move(extraction->extracted.geometry), std::move(*extracted_selection)};
  result.source_to_copy = std::move(extraction->extracted.source_to_copy);
  if (!snapshot_validate(result.remainder, r_error) ||
      !snapshot_validate(result.extracted, r_error))
  {
    return std::nullopt;
  }
  return result;
}

OwnedGeometry geometry_from_edit_snapshot(const EditSnapshot &snapshot, std::string *r_error)
{
  if (!snapshot_validate(snapshot, r_error)) {
    return nullptr;
  }
  OwnedGeometry geometry(geometry_copy(snapshot.geometry.get()));
  geometry->stored_selection = selection_to_storage(snapshot.selection, r_error);
  if (!geometry->stored_selection || !geometry_validate_structure(*geometry, r_error)) {
    return nullptr;
  }
  return geometry;
}

std::optional<EditSnapshot> edit_snapshot_from_topology_result(TopologyEditResult result,
                                                               const SelectionState &previous,
                                                               const SelectionMode mode,
                                                               std::string *r_error)
{
  if (!result.geometry) {
    if (r_error) {
      *r_error = "NURBS topology operation returned no source geometry";
    }
    return std::nullopt;
  }
  SelectionState selected;
  selected.mode = mode;
  if (flag_is_set(mode, SelectionMode::Vertex)) {
    selected.vertices.add_multiple(result.vertices);
  }
  if (flag_is_set(mode, SelectionMode::Edge)) {
    selected.edges.add_multiple(result.edges);
  }
  if (flag_is_set(mode, SelectionMode::Face)) {
    selected.faces.add_multiple(result.faces);
  }
  auto normalized = selection_flush(*result.geometry, selected, mode, r_error);
  if (!normalized) {
    return std::nullopt;
  }
  auto mapped = [&](ElementRef element) -> std::optional<ElementRef> {
    element.id = result.source_to_result.lookup_default(element.id, element.id);
    if (element.id && flag_is_set(mode, SelectionMode(1 << int(element.domain))) &&
        selection_contains(*normalized, element))
    {
      return element;
    }
    return std::nullopt;
  };
  for (const ElementRef element : previous.history) {
    if (auto replacement = mapped(element)) {
      if (!normalized->history.contains(*replacement)) {
        normalized->history.append(*replacement);
      }
    }
  }
  if (previous.active) {
    normalized->active = mapped(*previous.active);
  }
  if (!normalized->active) {
    if (flag_is_set(mode, SelectionMode::Face) && !result.faces.is_empty()) {
      normalized->active = ElementRef{ElementDomain::Face, result.faces.last()};
    }
    else if (flag_is_set(mode, SelectionMode::Edge) && !result.edges.is_empty()) {
      normalized->active = ElementRef{ElementDomain::Edge, result.edges.last()};
    }
    else if (flag_is_set(mode, SelectionMode::Vertex) && !result.vertices.is_empty()) {
      normalized->active = ElementRef{ElementDomain::Vertex, result.vertices.last()};
    }
  }
  if (normalized->active && !normalized->history.contains(*normalized->active)) {
    normalized->history.append(*normalized->active);
  }
  EditSnapshot snapshot{std::move(result.geometry), std::move(*normalized)};
  if (!snapshot_validate(snapshot, r_error)) {
    return std::nullopt;
  }
  return snapshot;
}

ReadSnapshot::ReadSnapshot(std::shared_ptr<const detail::EditStateStorage> state)
    : state_(std::move(state))
{
}

const NurbsGeometry &ReadSnapshot::geometry() const
{
  return *state_->snapshot->geometry;
}

const SelectionState &ReadSnapshot::selection() const
{
  return state_->snapshot->selection;
}

uint64_t ReadSnapshot::revision() const
{
  return state_->revision;
}

EditSnapshot ReadSnapshot::copy_for_edit() const
{
  return snapshot_copy(*state_->snapshot);
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

std::unique_ptr<EditData> EditData::create(EditSnapshot snapshot, std::string *r_error)
{
  if (!snapshot_validate(snapshot, r_error)) {
    return nullptr;
  }
  return std::unique_ptr<EditData>(new EditData(std::move(snapshot)));
}

const NurbsGeometry &EditData::geometry() const
{
  return *current_->snapshot->geometry;
}

const SelectionState &EditData::selection() const
{
  return current_->snapshot->selection;
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
  std::shared_ptr<const ObjectEvaluation> previous_evaluation;
  {
    std::scoped_lock lock(mutex_);
    if (!matches()) {
      return stale();
    }
    current_.swap(next);
    previous_evaluation.swap(prepared_evaluation_);
  }
  /* Release the previous state outside the lock; readers may still retain it. */
  return true;
}

bool EditData::commit_evaluated(std::shared_ptr<const ObjectEvaluation> candidate,
                                const ReadSnapshot &expected_state,
                                std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!candidate || !candidate->error.empty() || !candidate->result || !candidate->result->mesh) {
    if (r_error) {
      *r_error = "NURBS prepared edit requires a successful owned display evaluation";
    }
    return false;
  }
  const ReadSnapshot &source = candidate->result->source;
  /* ReadSnapshot can only retain a state accepted by create/commit. Geometry and selection are
   * immutable for that state's lifetime, so validating this same snapshot again is redundant. */
  std::scoped_lock lock(mutex_);
  if (current_ != expected_state.state_ ||
      current_->revision == std::numeric_limits<uint64_t>::max())
  {
    if (r_error) {
      *r_error = "NURBS evaluated candidate has a stale source state or exhausted revision";
    }
    return false;
  }
  current_ = std::make_shared<detail::EditStateStorage>(source.state_->snapshot,
                                                        current_->revision + 1);
  prepared_evaluation_.swap(candidate);
  return true;
}

std::shared_ptr<const ObjectEvaluation> EditData::prepared_evaluation(
    const ReadSnapshot &source) const
{
  std::scoped_lock lock(mutex_);
  return prepared_evaluation_ &&
                 &prepared_evaluation_->result->source.geometry() == &source.geometry() ?
             prepared_evaluation_ :
             nullptr;
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
  /* A live edit source is authoritative and only publishes validated immutable snapshots.
   * Keep the owner checks above, but do not rebuild a topology index merely to revalidate a
   * snapshot which has not changed. Its older persistent cache may be replaced during flush. */
  if (edit != nullptr) {
    return true;
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
    result->prepared_evaluation = edit->prepared_evaluation(*result->snapshot);
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

namespace {

static bool tagged_owner_check(const Curve &curve, std::string *r_error)
{
  if (curve.nurbs_eval_source != nullptr || curve.key != nullptr || curve.adt != nullptr ||
      !ELEM(curve.ob_type, OB_SURF, OB_CURVES_LEGACY) || curve.bevobj || curve.taperobj ||
      curve.offset != 1.0f || curve.extrude != 0 || curve.bevel_radius != 0)
  {
    if (r_error) {
      *r_error =
          "Component modes cannot yet preserve this source's shape keys, animation, "
          "offsets or bevels";
    }
    return false;
  }
  return true;
}

static std::optional<Vector<uint8_t>> tagged_source_stamp(const Curve &curve)
{
  Vector<uint8_t> bytes;
  auto append = [&](const void *data, const size_t size) {
    const int64_t start = bytes.size();
    bytes.resize(start + size);
    if (size) {
      std::memcpy(bytes.data() + start, data, size);
    }
  };
  append(&curve.id.session_uid, sizeof(curve.id.session_uid));
  append(&curve.ob_type, sizeof(curve.ob_type));
  append(&curve.flag, sizeof(curve.flag));
  for (const Nurb &nurb : curve.editnurb->nurbs) {
    const bool point = nurb.pntsu == 1 && nurb.pntsv == 1;
    if (!nurb.bp ||
        (!point && (nurb.pntsu != 2 || nurb.orderu != 2 || !ELEM(nurb.pntsv, 1, 2) ||
                    !nurb.knotsu || (nurb.pntsv == 2 && (nurb.orderv != 2 || !nurb.knotsv)))))
    {
      return std::nullopt;
    }
    append(&nurb, sizeof(nurb));
    append(nurb.bp, sizeof(BPoint) * nurb.pntsu * nurb.pntsv);
    if (!point) {
      append(nurb.knotsu, sizeof(float) * 4);
      if (nurb.pntsv == 2) {
        append(nurb.knotsv, sizeof(float) * 4);
      }
    }
  }
  return bytes;
}

}  // namespace

std::optional<PreparedCurveSource> curve_edit_prepare_tagged(const Curve &curve,
                                                             const SelectionMode mode,
                                                             std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!tagged_owner_check(curve, r_error)) {
    return std::nullopt;
  }
  if (curve.nurbs_geometry || !curve.editnurb || curve.editnurb->nurbs_edit) {
    if (r_error) {
      *r_error = "Source is not an editable tagged NURBS prototype";
    }
    return std::nullopt;
  }
  for (const Nurb &nurb : curve.editnurb->nurbs) {
    if ((nurb.resolu && nurb.resolu != curve.resolu) ||
        (nurb.resolv && nurb.resolv != curve.resolv))
    {
      if (r_error) {
        *r_error = "Component modes cannot yet preserve per-spline preview resolutions";
      }
      return std::nullopt;
    }
  }
  auto snapshot = legacy_topology_import_edit(curve.editnurb->nurbs, r_error);
  if (!snapshot) {
    return std::nullopt;
  }
  if (curve.actnu >= 0 && curve.actvert >= 0) {
    if (const auto *active = static_cast<const Nurb *>(
            BLI_findlink(&curve.editnurb->nurbs, curve.actnu)))
    {
      if (curve.actvert < active->pntsu * active->pntsv) {
        constexpr int point_to_corner[4] = {0, 1, 3, 2};
        const int corner = active->pntsv == 2 ? point_to_corner[curve.actvert] : curve.actvert;
        const int64_t id = active->topology_vertex_ids[corner];
        if (snapshot->selection.vertices.contains(id)) {
          snapshot->selection.active = ElementRef{ElementDomain::Vertex, id};
          snapshot->selection.history.append(*snapshot->selection.active);
        }
      }
    }
  }
  auto selected = selection_change_mode(*snapshot->geometry, snapshot->selection, mode, r_error);
  if (!selected) {
    return std::nullopt;
  }
  snapshot->selection = std::move(*selected);
  auto stamp = tagged_source_stamp(curve);
  if (!stamp) {
    return std::nullopt;
  }
  PreparedCurveSource result;
  result.owner = &curve;
  result.legacy_edit = curve.editnurb;
  result.legacy_stamp = std::move(*stamp);
  result.active_spline = curve.actnu;
  result.active_point = curve.actvert;
  result.persistent = geometry_from_edit_snapshot(*snapshot, r_error);
  result.edit = EditData::create(*snapshot->geometry, snapshot->selection, r_error);
  if (!result.persistent || !result.edit) {
    return std::nullopt;
  }
  SurfacePreviewOptions options;
  options.boundary.segments_per_span = std::max(curve.resolu, curve.resolv);
  options.interior_subdivisions = std::max(curve.resolu, curve.resolv);
  if (!evaluate_mesh(result.edit->read_snapshot(), options, r_error)) {
    return std::nullopt;
  }
  return result;
}

bool curve_edit_commit_tagged(Curve &curve, PreparedCurveSource candidate, std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!tagged_owner_check(curve, r_error)) {
    return false;
  }
  if (candidate.owner != &curve || candidate.legacy_edit != curve.editnurb || !curve.editnurb ||
      curve.editnurb->nurbs_edit || curve.nurbs_geometry ||
      candidate.active_spline != curve.actnu || candidate.active_point != curve.actvert ||
      !candidate.edit || !candidate.persistent)
  {
    if (r_error) {
      *r_error = "NURBS source changed since component-mode entry was prepared";
    }
    return false;
  }
  const auto stamp = tagged_source_stamp(curve);
  if (!stamp || stamp->as_span() != candidate.legacy_stamp.as_span()) {
    if (r_error) {
      *r_error = "NURBS source changed since component-mode entry was prepared";
    }
    return false;
  }
  BKE_curve_editNurb_keyIndex_free(&curve.editnurb->keyindex);
  BKE_nurbList_free(&curve.editnurb->nurbs);
  BKE_nurbList_free(&curve.nurb);
  curve.nurbs_geometry = candidate.persistent.release();
  curve.editnurb->nurbs_edit = candidate.edit.release();
  curve.actnu = curve.actvert = CU_ACT_NONE;
  return true;
}

bool curve_edit_restore_native(Curve &curve, const EditSnapshot &snapshot, std::string *r_error)
{
  if (!snapshot.geometry || !curve.editnurb || curve.nurbs_eval_source || curve.key ||
      !legacy_topology_source_check(curve.editnurb->nurbs))
  {
    if (r_error) {
      *r_error = "Native undo transition requires the original tagged NURBS owner";
    }
    return false;
  }
  auto edit = EditData::create(*snapshot.geometry, snapshot.selection, r_error);
  auto persistent = geometry_from_edit_snapshot(snapshot, r_error);
  if (!edit || !persistent) {
    return false;
  }
  BKE_curve_editNurb_keyIndex_free(&curve.editnurb->keyindex);
  BKE_nurbList_free(&curve.editnurb->nurbs);
  BKE_nurbList_free(&curve.nurb);
  geometry_free(curve.nurbs_geometry);
  curve.nurbs_geometry = persistent.release();
  curve.editnurb->nurbs_edit = edit.release();
  return true;
}

bool curve_edit_restore_tagged(Curve &curve, const ListBaseT<Nurb> &snapshot, std::string *r_error)
{
  if (!curve.editnurb || !curve.editnurb->nurbs_edit || curve.nurbs_eval_source || curve.key ||
      !legacy_topology_source_check(snapshot))
  {
    if (r_error) {
      *r_error = "Legacy undo transition requires a tagged NURBS snapshot";
    }
    return false;
  }
  // Import validates the geometry/IDs, while the undo list retains all original attributes.
  OwnedGeometry checked(geometry_from_tagged_legacy(snapshot, nullptr, r_error));
  if (!checked) {
    return false;
  }
  ListBaseT<Nurb> persistent = {nullptr, nullptr};
  BKE_nurbList_duplicate(&persistent, &snapshot);
  MEM_delete(curve.editnurb->nurbs_edit);
  curve.editnurb->nurbs_edit = nullptr;
  geometry_free(curve.nurbs_geometry);
  curve.nurbs_geometry = nullptr;
  BKE_nurbList_free(&curve.nurb);
  curve.nurb = persistent;
  return true;
}

}  // namespace blender::bke::nurbs
