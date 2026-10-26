/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

#include "BLI_listbase.h"
#include "BLI_math_rotation.h"
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_blender.hh"
#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_evaluate.hh"
#include "BKE_nurbs_legacy_edit.hh"
#include "BKE_nurbs_transform.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_undo_system.hh"
#include "BKE_wm_runtime.hh"
#include "BKE_workspace.hh"

#include "BLO_readfile.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_curve.hh"
#include "ED_nurbs.hh"
#include "ED_object.hh"
#include "ED_undo.hh"
#include "ED_util.hh"

#include "WM_api.hh"

#include "nurbs_test_geometry.hh"
#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

static OwnedGeometry make_line()
{
  Nurb source;
  source.type = CU_NURBS;
  source.pntsu = source.orderu = source.orderv = 2;
  source.pntsv = 1;
  source.flagu = CU_NURB_ENDPOINT;
  source.topology_vertex_ids[0] = 1;
  source.topology_vertex_ids[1] = 2;
  source.topology_edge_ids[0] = 3;
  BPoint points[2] = {};
  points[0].vec[3] = points[1].vec[3] = 1.0f;
  points[1].vec[0] = 1.0f;
  float knots[4] = {0, 0, 1, 1};
  source.bp = points;
  source.knotsu = knots;
  const ListBaseT<Nurb> nurbs = {&source, &source};
  return OwnedGeometry(geometry_from_tagged_legacy(nurbs));
}

static SelectionState make_selection()
{
  SelectionState selection;
  selection.vertices.add(1);
  selection.vertices.add(2);
  selection.edges.add(3);
  selection.active = ElementRef{ElementDomain::Vertex, 2};
  selection.history.append({ElementDomain::Vertex, 1});
  selection.history.append({ElementDomain::Edge, 3});
  selection.history.append({ElementDomain::Vertex, 2});
  return selection;
}

static const NurbsCurveGeometry &first_curve(const NurbsGeometry &geometry)
{
  return *static_cast<const NurbsCurveGeometry *>(geometry.curves.first);
}

static void translate_fixture(OwnedGeometry &geometry, const double offset)
{
  double4x4 matrix = double4x4::identity();
  matrix.location().x = offset;
  std::string error;
  auto result = transform_geometry(*geometry, matrix, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  geometry = std::move(result->geometry);
}

TEST(nurbs_edit_state, SourceAndSnapshotsOwnIndependentStorage)
{
  OwnedGeometry source = make_line();
  ASSERT_NE(source, nullptr);
  auto edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  EXPECT_NE(first_curve(edit->geometry()).points, first_curve(*source).points);
  EditSnapshot snapshot = edit->snapshot();
  EXPECT_NE(first_curve(*snapshot.geometry).points, first_curve(edit->geometry()).points);
  translate_fixture(snapshot.geometry, 4.0);
  snapshot.selection.vertices.remove(1);
  snapshot.selection.history.clear();
  EXPECT_EQ(first_curve(edit->geometry()).points[0].position[0], 0.0);
  EXPECT_TRUE(edit->selection().vertices.contains(1));
  EXPECT_EQ(edit->selection().history.size(), 3);
  source.reset();
  EXPECT_TRUE(geometry_validate_structure(edit->geometry()));
}

TEST(nurbs_edit_state, RestoreKeepsGeometrySelectionCounterTogetherAndRevisionMonotonic)
{
  OwnedGeometry source = make_line();
  ASSERT_NE(source, nullptr);
  auto edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  const EditSnapshot before = edit->snapshot();
  const int64_t old_counter = before.geometry->next_id;
  const uint64_t revision_before = edit->revision();
  EditSnapshot candidate = edit->snapshot();
  translate_fixture(candidate.geometry, 2.0);
  candidate.geometry->next_id += 10;
  candidate.selection.vertices.remove(1);
  candidate.selection.active = ElementRef{ElementDomain::Edge, 3};
  candidate.selection.history.clear();
  candidate.selection.history.append({ElementDomain::Edge, 3});
  std::string error;
  ASSERT_TRUE(edit->commit(std::move(candidate), revision_before, &error)) << error;
  const EditSnapshot after = edit->snapshot();
  EXPECT_EQ(edit->geometry().next_id, old_counter + 10);
  EXPECT_EQ(edit->revision(), revision_before + 1);
  ASSERT_TRUE(edit->restore(before, &error)) << error;
  EXPECT_EQ(edit->geometry().next_id, old_counter);
  EXPECT_EQ(first_curve(edit->geometry()).points[0].position[0], 0.0);
  EXPECT_TRUE(edit->selection().vertices.contains(1));
  ASSERT_TRUE(edit->selection().active.has_value());
  EXPECT_EQ(*edit->selection().active, (ElementRef{ElementDomain::Vertex, 2}));
  EXPECT_EQ(edit->selection().history.size(), 3);
  EXPECT_EQ(edit->revision(), revision_before + 2);
  ASSERT_TRUE(edit->restore(after, &error)) << error;
  EXPECT_EQ(edit->geometry().next_id, old_counter + 10);
  EXPECT_EQ(first_curve(edit->geometry()).points[0].position[0], 2.0);
  EXPECT_FALSE(edit->selection().vertices.contains(1));
  EXPECT_EQ(edit->selection().history.size(), 1);
  EXPECT_EQ(edit->revision(), revision_before + 3);
}

TEST(nurbs_edit_state, InvalidAndStaleCandidatesLeaveAllCurrentStateUnchanged)
{
  OwnedGeometry source = make_line();
  ASSERT_NE(source, nullptr);
  auto edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  const uint64_t revision = edit->revision();
  const int64_t counter = edit->geometry().next_id;
  std::string error;
  EditSnapshot invalid_geometry = edit->snapshot();
  static_cast<NurbsTopologyEdge *>(invalid_geometry.geometry->edges.first)->curve_id = 0;
  EXPECT_FALSE(edit->commit(std::move(invalid_geometry), revision, &error));
  EXPECT_FALSE(error.empty());
  EditSnapshot invalid_selection = edit->snapshot();
  invalid_selection.selection.vertices.add(99);
  EXPECT_FALSE(edit->commit(std::move(invalid_selection), revision, &error));
  EXPECT_FALSE(error.empty());
  EditSnapshot stale = edit->snapshot();
  translate_fixture(stale.geometry, 20.0);
  EXPECT_FALSE(edit->commit(std::move(stale), revision - 1, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(edit->commit({}, revision, &error));
  EXPECT_EQ(edit->revision(), revision);
  EXPECT_EQ(edit->geometry().next_id, counter);
  EXPECT_EQ(first_curve(edit->geometry()).points[0].position[0], 0.0);
  EXPECT_EQ(edit->selection().history.size(), 3);
  EXPECT_EQ(*edit->selection().active, (ElementRef{ElementDomain::Vertex, 2}));
}

TEST(nurbs_edit_state, ActiveAndHistoryMustReferenceSelectedSourceDomains)
{
  OwnedGeometry source = make_line();
  ASSERT_NE(source, nullptr);
  SelectionState selection = make_selection();
  selection.active = ElementRef{ElementDomain::Vertex, 3};
  EXPECT_EQ(EditData::create(*source, selection), nullptr);
  selection = make_selection();
  selection.history.append({ElementDomain::Vertex, 1});
  EXPECT_EQ(EditData::create(*source, selection), nullptr);
  selection = make_selection();
  selection.vertices.remove(1);
  EXPECT_EQ(EditData::create(*source, selection), nullptr);
  selection = make_selection();
  selection.history.clear();
  EXPECT_NE(EditData::create(*source, selection), nullptr);
}

TEST(nurbs_edit_state, InvalidRestoreDoesNotAdvanceRevisionOrReplaceSource)
{
  OwnedGeometry source = make_line();
  ASSERT_NE(source, nullptr);
  auto edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  const uint64_t revision = edit->revision();
  EditSnapshot snapshot = edit->snapshot();
  snapshot.selection.active = ElementRef{ElementDomain::Face, 100};
  std::string error;
  EXPECT_FALSE(edit->restore(snapshot, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(edit->revision(), revision);
  EXPECT_EQ(first_curve(edit->geometry()).points[0].position[0], 0.0);
}

TEST(nurbs_edit_state, StoredSelectionIsCanonicalOwnedAndPreservesHistory)
{
  SelectionState selection = make_selection();
  selection.faces.add(4);
  selection.active = ElementRef{ElementDomain::Face, 4};
  selection.history.append({ElementDomain::Face, 4});
  NurbsStoredSelection *storage = selection_to_storage(selection);
  BLI_SCOPED_DEFER([&]() { selection_storage_free(storage); });
  ASSERT_NE(storage, nullptr);
  ASSERT_EQ(storage->elements_num, 4);
  EXPECT_EQ(storage->elements[0].id, 1);
  EXPECT_EQ(storage->elements[1].id, 2);
  EXPECT_EQ(storage->elements[2].domain, int(ElementDomain::Edge));
  EXPECT_EQ(storage->elements[3].domain, int(ElementDomain::Face));
  const auto decoded = selection_from_storage(storage);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->vertices.size(), 2);
  EXPECT_TRUE(decoded->edges.contains(3));
  EXPECT_TRUE(decoded->faces.contains(4));
  EXPECT_EQ(decoded->active, selection.active);
  EXPECT_EQ(decoded->history.size(), 4);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(decoded->history[i], selection.history[i]);
  }
  NurbsStoredSelection *copy = selection_storage_copy(storage);
  BLI_SCOPED_DEFER([&]() { selection_storage_free(copy); });
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy->elements, storage->elements);
  EXPECT_NE(copy->history, storage->history);
  copy->elements[0].id = 99;
  EXPECT_EQ(storage->elements[0].id, 1);
  copy->elements_num++;
  std::string error;
  EXPECT_FALSE(selection_from_storage(copy, &error).has_value());
  EXPECT_FALSE(error.empty());
}

TEST(nurbs_edit_state, StoredSelectionRejectsInvalidReferencesDomainsAndDuplicates)
{
  OwnedGeometry source = make_line();
  ASSERT_NE(source, nullptr);
  source->stored_selection = selection_to_storage(make_selection());
  ASSERT_NE(source->stored_selection, nullptr);
  ASSERT_TRUE(geometry_validate_structure(*source));
  NurbsStoredSelection &storage = *source->stored_selection;
  std::string error;
  storage.elements[0].id = 99;
  EXPECT_FALSE(geometry_validate_structure(*source, &error));
  EXPECT_FALSE(error.empty());
  storage.elements[0].id = 1;
  storage.active.domain = int(ElementDomain::Face);
  EXPECT_FALSE(geometry_validate_structure(*source, &error));
  storage.active.domain = int(ElementDomain::Vertex);
  storage.elements[1].id = 1;
  EXPECT_FALSE(geometry_validate_structure(*source, &error));
  storage.elements[1].id = 2;
  storage.elements[1].domain = 99;
  EXPECT_FALSE(geometry_validate_structure(*source, &error));
  storage.elements[1].domain = int(ElementDomain::Vertex);
  storage.history[0].domain = int(ElementDomain::Face);
  EXPECT_FALSE(geometry_validate_structure(*source, &error));
  storage.history[0].domain = int(ElementDomain::Vertex);
  EXPECT_TRUE(geometry_validate_structure(*source, &error)) << error;
}

TEST(nurbs_edit_state, EditSnapshotsKeepOneSelectionAuthority)
{
  OwnedGeometry source = make_line();
  ASSERT_NE(source, nullptr);
  source->stored_selection = selection_to_storage(make_selection());
  ASSERT_NE(source->stored_selection, nullptr);
  auto edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  EXPECT_EQ(edit->geometry().stored_selection, nullptr);
  EXPECT_NE(source->stored_selection, nullptr);
  EditSnapshot candidate = edit->snapshot();
  candidate.geometry->stored_selection = selection_to_storage(make_selection());
  const uint64_t revision = edit->revision();
  std::string error;
  EXPECT_FALSE(edit->commit(std::move(candidate), revision, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(edit->revision(), revision);
  EXPECT_EQ(edit->selection().history.size(), 3);
  EXPECT_EQ(edit->geometry().stored_selection, nullptr);
}

TEST(nurbs_edit_state, ReadSnapshotsRetainGeometrySelectionAndRevisionAfterOwnerDestruction)
{
  OwnedGeometry source = make_line();
  auto edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot before = edit->read_snapshot();
  EXPECT_EQ(before, edit->read_snapshot());
  EditSnapshot candidate = before.copy_for_edit();
  EXPECT_NE(first_curve(*candidate.geometry).points, first_curve(before.geometry()).points);
  translate_fixture(candidate.geometry, 2.0);
  candidate.geometry->next_id += 10;
  candidate.selection.vertices.remove(1);
  candidate.selection.active = ElementRef{ElementDomain::Edge, 3};
  candidate.selection.history.clear();
  candidate.selection.history.append(*candidate.selection.active);
  ASSERT_TRUE(edit->commit(std::move(candidate), before));
  const ReadSnapshot after = edit->read_snapshot();
  EXPECT_NE(before, after);
  source.reset();
  edit.reset();
  EXPECT_EQ(before.revision(), 1);
  EXPECT_EQ(after.revision(), 2);
  EXPECT_EQ(first_curve(before.geometry()).points[0].position[0], 0.0);
  EXPECT_EQ(first_curve(after.geometry()).points[0].position[0], 2.0);
  EXPECT_EQ(after.geometry().next_id, before.geometry().next_id + 10);
  EXPECT_TRUE(before.selection().vertices.contains(1));
  EXPECT_FALSE(after.selection().vertices.contains(1));
  EXPECT_EQ(before.selection().history.size(), 3);
  EXPECT_EQ(after.selection().history.size(), 1);
  const auto point = evaluate_curve(first_curve(after.geometry()), 0.5);
  ASSERT_TRUE(point.has_value());
  EXPECT_EQ(point->position.x, 2.5);
}

TEST(nurbs_edit_state, ReadTokenRejectsARecreatedEditLifetimeWithTheSameRevision)
{
  OwnedGeometry source = make_line();
  auto edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot stale = edit->read_snapshot();
  edit.reset();
  edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot current = edit->read_snapshot();
  EXPECT_EQ(stale.revision(), current.revision());
  EXPECT_NE(stale, current);
  EditSnapshot candidate = stale.copy_for_edit();
  translate_fixture(candidate.geometry, 20.0);
  std::string error;
  EXPECT_FALSE(edit->commit(std::move(candidate), stale, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(edit->read_snapshot(), current);
  EXPECT_EQ(first_curve(edit->geometry()).points[0].position[0], 0.0);
  candidate = current.copy_for_edit();
  translate_fixture(candidate.geometry, 2.0);
  ASSERT_TRUE(edit->commit(std::move(candidate), current, &error)) << error;
  EXPECT_EQ(edit->revision(), 2);
}

TEST(nurbs_edit_state, ConcurrentReadSnapshotsKeepGeometryCounterSelectionAndRevisionCoherent)
{
  OwnedGeometry source = make_line();
  auto edit = EditData::create(*source, make_selection());
  ASSERT_NE(edit, nullptr);
  const int64_t initial_counter = source->next_id;
  std::atomic<bool> ready = false, finished = false;
  std::atomic<int> reads = 0;
  std::thread reader([&]() {
    ready.store(true, std::memory_order_release);
    do {
      const ReadSnapshot snapshot = edit->read_snapshot();
      const uint64_t step = snapshot.revision() - 1;
      EXPECT_EQ(first_curve(snapshot.geometry()).points[0].position[0], double(step));
      EXPECT_EQ(snapshot.geometry().next_id, initial_counter + int64_t(step) * 10);
      EXPECT_TRUE(snapshot.selection().active.has_value());
      if (snapshot.selection().active) {
        EXPECT_EQ(*snapshot.selection().active,
                  (ElementRef{ElementDomain::Vertex, step % 2 ? 1 : 2}));
      }
      EXPECT_EQ(snapshot.selection().history.size(), step == 0 ? 3 : 1);
      const auto point = evaluate_curve(first_curve(snapshot.geometry()), 0.5);
      EXPECT_TRUE(point.has_value());
      if (point) {
        EXPECT_EQ(point->position.x, double(step) + 0.5);
      }
      reads.fetch_add(1, std::memory_order_relaxed);
    } while (!finished.load(std::memory_order_acquire));
  });
  BLI_SCOPED_DEFER([&]() {
    finished.store(true, std::memory_order_release);
    if (reader.joinable()) {
      reader.join();
    }
  });
  while (!ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (int step = 1; step <= 32; step++) {
    const ReadSnapshot expected = edit->read_snapshot();
    EditSnapshot candidate = expected.copy_for_edit();
    translate_fixture(candidate.geometry, 1.0);
    candidate.geometry->next_id += 10;
    candidate.selection.active = ElementRef{ElementDomain::Vertex, step % 2 ? 1 : 2};
    candidate.selection.history.clear();
    candidate.selection.history.append(*candidate.selection.active);
    ASSERT_TRUE(edit->commit(std::move(candidate), expected));
  }
  finished.store(true, std::memory_order_release);
  reader.join();
  EXPECT_GT(reads.load(), 0);
  EXPECT_EQ(edit->revision(), 33);
}

class NurbsNativeEditTest : public BlenderGTestBase {};

TEST_F(NurbsNativeEditTest, ActiveVertexCenterReadsLiveSourceIdentityWithoutMutation)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Curve *curve = BKE_curve_add(bmain, "NURBS active center", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  curve->nurbs_geometry->stored_selection = selection_to_storage(make_selection());
  ASSERT_TRUE(curve_edit_begin(*curve));
  EditData &edit = *curve->editnurb->nurbs_edit;
  const auto original = edit.read_snapshot();
  std::string error;
  auto moved = translate_vertices(original.geometry(), {2}, {0, 0.125, 0.25}, 0, &error);
  ASSERT_TRUE(moved.has_value()) << error;
  ASSERT_TRUE(ed::nurbs::edit_commit(
      *bmain, *curve, {std::move(moved->geometry), original.selection()}, original, &error))
      << error;
  const auto live = edit.read_snapshot();
  float center[3] = {99, 98, 97};
  ASSERT_TRUE(ED_curve_active_center(curve, center));
  EXPECT_EQ(float3(center), float3(1, 0.125, 0.25));
  EXPECT_EQ(edit.read_snapshot(), live);
  {
    const auto persistent = TopologyIndex::build(*curve->nurbs_geometry);
    ASSERT_TRUE(persistent.has_value());
    EXPECT_EQ(persistent->vertex(2)->position[1], 0);
    EXPECT_EQ(persistent->vertex(2)->position[2], 0);
  }
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  bmain->is_memfile_undo_flush_needed = false;
  ASSERT_TRUE(ED_curve_active_center(curve, center));
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
  EXPECT_EQ(edit.read_snapshot(), live);

  auto no_active = live.copy_for_edit();
  no_active.selection.active.reset();
  no_active.selection.history.clear();
  ASSERT_TRUE(edit.commit(std::move(no_active), live));
  center[0] = 99;
  center[1] = 98;
  center[2] = 97;
  EXPECT_FALSE(ED_curve_active_center(curve, center));
  EXPECT_EQ(float3(center), float3(99, 98, 97));

  const auto before_large = edit.read_snapshot();
  auto large = translate_vertices(
      before_large.geometry(), {2}, {1e100, 0, 0}, std::numeric_limits<double>::max(), &error);
  ASSERT_TRUE(large.has_value()) << error;
  auto candidate = before_large.copy_for_edit();
  candidate.geometry = std::move(large->geometry);
  candidate.selection = original.selection();
  ASSERT_TRUE(edit.commit(std::move(candidate), before_large));
  const auto large_source = edit.read_snapshot();
  EXPECT_FALSE(ED_curve_active_center(curve, center));
  EXPECT_EQ(float3(center), float3(99, 98, 97));
  EXPECT_EQ(edit.read_snapshot(), large_source);
}

TEST_F(NurbsNativeEditTest, FillCommitsSharedSourceAndSelectionOnceAndContinuesExtrusion)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  for (const SelectionMode mode : {SelectionMode::Vertex, SelectionMode::Edge}) {
    Curve *curve = BKE_curve_add(bmain, "NURBS fill editing", OB_SURF);
    curve->nurbs_geometry = make_planar_sheet().release();
    ASSERT_NE(curve->nurbs_geometry, nullptr);
    const int64_t face = static_cast<NurbsTopologyFace *>(curve->nurbs_geometry->faces.first)->id;
    const auto wire = delete_components(
        *curve->nurbs_geometry, {{}, {}, {&face, 1}}, DeleteMode::FacesOnly);
    ASSERT_TRUE(wire.has_value());
    geometry_free(curve->nurbs_geometry);
    curve->nurbs_geometry = geometry_copy(wire->geometry.get());
    SelectionState selected;
    selected.mode = mode;
    for (const NurbsTopologyVertex &vertex : curve->nurbs_geometry->vertices) {
      selected.vertices.add(vertex.id);
    }
    for (const NurbsTopologyEdge &edge : curve->nurbs_geometry->edges) {
      selected.edges.add(edge.id);
    }
    curve->nurbs_geometry->stored_selection = selection_to_storage(selected);
    ASSERT_TRUE(curve_edit_begin(*curve));
    EditData &edit = *curve->editnurb->nurbs_edit;
    const auto before = edit.read_snapshot();
    std::string error;
    ASSERT_TRUE(ed::nurbs::fill(*bmain, *curve, &error)) << error;
    EXPECT_EQ(edit.revision(), before.revision() + 1);
    EXPECT_TRUE(before.geometry().faces.is_empty());
    EXPECT_EQ(edit.geometry().vertices.count(), 4);
    EXPECT_EQ(edit.geometry().edges.count(), 4);
    EXPECT_EQ(edit.geometry().faces.count(), 1);
    EXPECT_EQ(edit.selection().vertices, selected.vertices);
    EXPECT_EQ(edit.selection().edges, selected.edges);
    EXPECT_EQ(edit.selection().faces.size(), 1);
    EXPECT_EQ(edit.selection().mode, mode);
    EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
    EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
    const auto filled = edit.read_snapshot();
    EXPECT_FALSE(ed::nurbs::fill(*bmain, *curve, &error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(edit.read_snapshot(), filled);
    ASSERT_TRUE(ed::nurbs::extrude(*bmain, *curve, &error)) << error;
    EXPECT_EQ(edit.geometry().vertices.count(), 8);
    EXPECT_EQ(edit.geometry().edges.count(), 12);
    EXPECT_EQ(edit.geometry().faces.count(), 6);
    EXPECT_EQ(edit.selection().mode, mode);
    EXPECT_TRUE(curve->nurb.is_empty());
    EXPECT_TRUE(curve->editnurb->nurbs.is_empty());
  }
}

TEST_F(NurbsNativeEditTest, FailedFillLeavesSourceSelectionRevisionAndDirtyFlagsUnchanged)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Curve *curve = BKE_curve_add(bmain, "NURBS exhausted fill", OB_SURF);
  // Two independent vertices require a new curve and edge identity.
  OwnedGeometry wire(MEM_new<NurbsGeometry>(__func__));
  add_vertex(*wire, {0, 0, 0});
  add_vertex(*wire, {1, 0, 0});
  wire->next_id = std::numeric_limits<int64_t>::max();
  SelectionState selected;
  selected.vertices.add(1);
  selected.vertices.add(2);
  wire->stored_selection = selection_to_storage(selected);
  curve->nurbs_geometry = wire.release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  EditData &edit = *curve->editnurb->nurbs_edit;
  const auto before = edit.read_snapshot();
  curve->editnurb->needs_flush_to_id = false;
  bmain->is_memfile_undo_flush_needed = false;
  std::string error;
  EXPECT_FALSE(ed::nurbs::fill(*bmain, *curve, &error));
  EXPECT_NE(error.find("exhausted"), std::string::npos);
  EXPECT_EQ(edit.read_snapshot(), before);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
}

TEST_F(NurbsNativeEditTest, NormalEditorEntryExitAndReentryPreserveNativeSourceAndSelection)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Scene *scene = BKE_scene_add(bmain, "NURBS mode entry scene");
  Object *object = BKE_object_add_only_object(bmain, OB_SURF, "NURBS mode entry object");
  Curve *curve = BKE_curve_add(bmain, "NURBS mode entry source", OB_SURF);
  object->data = &curve->id;
  curve->nurbs_geometry = make_line().release();
  curve->nurbs_geometry->stored_selection = selection_to_storage(make_selection());
  const auto *persistent = curve->nurbs_geometry;
  ASSERT_TRUE(ed::object::editmode_enter_ex(bmain, scene, object, 0));
  ASSERT_NE(curve->editnurb, nullptr);
  ASSERT_NE(curve->editnurb->nurbs_edit, nullptr);
  EXPECT_TRUE(curve->editnurb->nurbs.is_empty());
  EXPECT_EQ(curve->nurbs_geometry, persistent);
  EXPECT_EQ(curve->editnurb->nurbs_edit->selection().active, make_selection().active);
  const auto original = curve->editnurb->nurbs_edit->read_snapshot();
  ASSERT_TRUE(ed::object::editmode_enter_ex(bmain, scene, object, 0));
  EXPECT_EQ(curve->editnurb->nurbs_edit->read_snapshot(), original);
  auto candidate = original.copy_for_edit();
  translate_fixture(candidate.geometry, 2);
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), original));
  ASSERT_TRUE(ed::object::editmode_exit_ex(bmain, scene, object, ed::object::EM_FREEDATA));
  EXPECT_EQ(curve->editnurb, nullptr);
  ASSERT_TRUE(ed::object::editmode_enter_ex(bmain, scene, object, 0));
  EXPECT_EQ(first_curve(curve->editnurb->nurbs_edit->geometry()).points[0].position[0], 2);
  EXPECT_EQ(first_curve(original.geometry()).points[0].position[0], 0);
  EXPECT_EQ(curve->editnurb->nurbs_edit->selection().active, make_selection().active);
  EXPECT_TRUE(curve->nurb.is_empty());
  EXPECT_EQ(BKE_object_shapekey_insert(bmain, object, "unsupported", false), nullptr);
  EXPECT_EQ(curve->key, nullptr);
}

TEST_F(NurbsNativeEditTest, FailedEditorEntryLeavesModesOwnerAndLegacySourceUnchanged)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Scene *scene = BKE_scene_add(bmain, "NURBS failed entry scene");
  Object *object = BKE_object_add_only_object(bmain, OB_SURF, "NURBS failed entry object");
  Curve *curve = BKE_curve_add(bmain, "NURBS failed entry source", OB_SURF);
  object->data = &curve->id;
  curve->nurbs_geometry = make_line().release();
  const auto *payload = curve->nurbs_geometry;
  auto *legacy = MEM_new<Nurb>(__func__);
  BLI_addtail(&curve->nurb, legacy);
  object->restore_mode = OB_MODE_SCULPT;
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  EXPECT_FALSE(
      ed::object::editmode_enter_ex(bmain, scene, object, ed::object::EM_NO_CONTEXT, &reports));
  EXPECT_EQ(object->mode, OB_MODE_OBJECT);
  EXPECT_EQ(object->restore_mode, OB_MODE_SCULPT);
  EXPECT_EQ(curve->editnurb, nullptr);
  EXPECT_EQ(curve->nurbs_geometry, payload);
  EXPECT_EQ(curve->nurb.first, legacy);
  EXPECT_TRUE(BKE_reports_contain(&reports, RPT_ERROR));
}

TEST_F(NurbsNativeEditTest, EditorVertexHitsRejectStaleSourcesAndKeepNoOpsClean)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS vertex hit source", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  const auto original = curve->editnurb->nurbs_edit->read_snapshot();
  const int64_t ids[] = {1, 2};
  std::string error;
  ASSERT_TRUE(ed::nurbs::select_vertices(
      *bmain, *curve, original, {ids, 2}, SelectionAction::Set, 2, &error))
      << error;
  const auto selected = curve->editnurb->nurbs_edit->read_snapshot();
  EXPECT_TRUE(selected.selection().edges.contains(3));
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  bmain->is_memfile_undo_flush_needed = false;
  EXPECT_FALSE(ed::nurbs::select_vertices(
      *bmain, *curve, selected, {}, SelectionAction::Add, std::nullopt, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
  EXPECT_FALSE(ed::nurbs::select_vertices(
      *bmain, *curve, original, {}, SelectionAction::Set, std::nullopt, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(curve->editnurb->nurbs_edit->read_snapshot(), selected);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
}

TEST_F(NurbsNativeEditTest, EditorLinkedSelectionPreservesHistoryAndKeepsNoOpsClean)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS linked selection source", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  std::string error;
  EXPECT_FALSE(ed::nurbs::select_linked(*bmain, *curve, &error));
  EXPECT_FALSE(error.empty());
  ASSERT_TRUE(curve_edit_begin(*curve));
  const auto empty = curve->editnurb->nurbs_edit->read_snapshot();
  EXPECT_FALSE(ed::nurbs::select_linked(*bmain, *curve, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(curve->editnurb->nurbs_edit->read_snapshot(), empty);
  const int64_t id = 2;
  ASSERT_TRUE(ed::nurbs::select_vertices(
      *bmain, *curve, empty, {&id, 1}, SelectionAction::Set, id, &error))
      << error;
  const auto clicked = curve->editnurb->nurbs_edit->read_snapshot();
  ASSERT_TRUE(ed::nurbs::select_linked(*bmain, *curve, &error)) << error;
  const auto linked = curve->editnurb->nurbs_edit->read_snapshot();
  EXPECT_EQ(linked.selection().vertices.size(), 2);
  EXPECT_EQ(linked.selection().edges.size(), 1);
  EXPECT_EQ(linked.selection().active, clicked.selection().active);
  EXPECT_EQ(linked.selection().history.as_span(), clicked.selection().history.as_span());
  EXPECT_EQ(linked.geometry().next_id, empty.geometry().next_id);
  EXPECT_EQ(first_curve(linked.geometry()).points[0].position[0],
            first_curve(empty.geometry()).points[0].position[0]);
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  bmain->is_memfile_undo_flush_needed = false;
  EXPECT_FALSE(ed::nurbs::select_linked(*bmain, *curve, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(curve->editnurb->nurbs_edit->read_snapshot(), linked);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
}

TEST_F(NurbsNativeEditTest, EditorDuplicateCommitsFreshIdentitiesAndMovesOnlyTheCopy)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS duplicate source", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  curve->nurbs_geometry->stored_selection = selection_to_storage(make_selection());
  ASSERT_TRUE(curve_edit_begin(*curve));
  const auto original = curve->editnurb->nurbs_edit->read_snapshot();
  std::string error;
  ASSERT_TRUE(ed::nurbs::duplicate(*bmain, *curve, &error)) << error;
  const auto copied = curve->editnurb->nurbs_edit->read_snapshot();
  EXPECT_NE(copied, original);
  EXPECT_EQ(copied.geometry().vertices.count(), 4);
  EXPECT_EQ(copied.geometry().edges.count(), 2);
  EXPECT_EQ(copied.selection().vertices.size(), 2);
  EXPECT_EQ(copied.selection().edges.size(), 1);
  EXPECT_FALSE(copied.selection().vertices.contains(1));
  EXPECT_FALSE(copied.selection().vertices.contains(2));
  EXPECT_FALSE(copied.selection().edges.contains(3));
  ASSERT_EQ(copied.selection().history.size(), 3);
  EXPECT_EQ(copied.selection().history[1].domain, ElementDomain::Edge);
  EXPECT_EQ(copied.selection().active, copied.selection().history.last());
  EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
  EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
  EXPECT_EQ(curve->nurbs_geometry->vertices.count(), 2);
  EXPECT_EQ(original.geometry().vertices.count(), 2);
  const Vector<int64_t> selected(copied.selection().vertices.begin(),
                                 copied.selection().vertices.end());
  auto moved = translate_vertices(copied.geometry(), selected, {0, 0, 0.125}, 0, &error);
  ASSERT_TRUE(moved.has_value()) << error;
  auto candidate = copied.copy_for_edit();
  candidate.geometry = std::move(moved->geometry);
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), copied, &error))
      << error;
  const auto current = curve->editnurb->nurbs_edit->read_snapshot();
  const auto index = TopologyIndex::build(current.geometry());
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->vertex(1)->position[2], 0);
  EXPECT_EQ(index->vertex(2)->position[2], 0);
  for (const int64_t id : selected) {
    EXPECT_EQ(index->vertex(id)->position[2], 0.125);
  }
  EXPECT_EQ(first_curve(original.geometry()).points[0].position[2], 0);
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  EXPECT_EQ(curve->nurbs_geometry->vertices.count(), 4);
  EXPECT_TRUE(curve->nurb.is_empty());
}

TEST_F(NurbsNativeEditTest, EditorDuplicateRejectsInvalidSelectionsAndExhaustionWithoutDirtying)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS duplicate rejection", OB_SURF);
  std::string error;
  EXPECT_FALSE(ed::nurbs::duplicate(*bmain, *curve, &error));
  EXPECT_FALSE(error.empty());
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  auto &edit = *curve->editnurb->nurbs_edit;
  const auto empty = edit.read_snapshot();
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  bmain->is_memfile_undo_flush_needed = false;
  EXPECT_FALSE(ed::nurbs::duplicate(*bmain, *curve, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(edit.read_snapshot(), empty);
  auto candidate = empty.copy_for_edit();
  candidate.selection.edges.add(3);
  ASSERT_TRUE(edit.commit(std::move(candidate), empty));
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  const auto inconsistent = edit.read_snapshot();
  EXPECT_FALSE(ed::nurbs::duplicate(*bmain, *curve, &error));
  EXPECT_NE(error.find("normalized component-mode"), std::string::npos);
  EXPECT_EQ(edit.read_snapshot(), inconsistent);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
  candidate = inconsistent.copy_for_edit();
  candidate.selection = make_selection();
  candidate.geometry->next_id = std::numeric_limits<int64_t>::max();
  ASSERT_TRUE(edit.commit(std::move(candidate), inconsistent));
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  const auto exhausted = edit.read_snapshot();
  EXPECT_FALSE(ed::nurbs::duplicate(*bmain, *curve, &error));
  EXPECT_NE(error.find("identity space"), std::string::npos);
  EXPECT_EQ(edit.read_snapshot(), exhausted);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
}

TEST_F(NurbsNativeEditTest, EditorExtrusionConnectsCapAndPrunesDeletionSelection)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS extrusion source", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  curve->nurbs_geometry->stored_selection = selection_to_storage(make_selection());
  ASSERT_TRUE(curve_edit_begin(*curve));
  auto &edit = *curve->editnurb->nurbs_edit;
  const auto before = edit.read_snapshot();
  std::string error;
  double endpoint_deviation = -1;
  ASSERT_TRUE(ed::nurbs::extrude(*bmain, *curve, &error, &endpoint_deviation)) << error;
  EXPECT_EQ(endpoint_deviation, 0);
  const auto swept = edit.read_snapshot();
  EXPECT_EQ(swept.geometry().vertices.count(), 4);
  EXPECT_EQ(swept.geometry().edges.count(), 4);
  EXPECT_EQ(swept.geometry().faces.count(), 1);
  EXPECT_EQ(swept.selection().vertices.size(), 2);
  EXPECT_EQ(swept.selection().edges.size(), 1);
  EXPECT_TRUE(swept.selection().faces.is_empty());
  EXPECT_EQ(swept.selection().history.size(), before.selection().history.size());
  EXPECT_EQ(swept.selection().active, swept.selection().history.last());
  const auto index = TopologyIndex::build(swept.geometry());
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->uses_for_edge(3).size(), 1);
  EXPECT_EQ(index->vertex(1)->position[2], 0);
  EXPECT_EQ(before.geometry().vertices.count(), 2);
  const Vector<int64_t> cap(swept.selection().vertices.begin(), swept.selection().vertices.end());
  auto moved = translate_vertices(swept.geometry(), cap, {0, 2, 0}, 0, &error);
  ASSERT_TRUE(moved.has_value()) << error;
  auto candidate = swept.copy_for_edit();
  candidate.geometry = std::move(moved->geometry);
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), swept, &error))
      << error;
  ASSERT_TRUE(ed::nurbs::extrude(*bmain, *curve, &error)) << error;
  EXPECT_EQ(edit.geometry().vertices.count(), 6);
  EXPECT_EQ(edit.geometry().edges.count(), 7);
  EXPECT_EQ(edit.geometry().faces.count(), 2);
  ASSERT_TRUE(ed::nurbs::delete_components(*bmain, *curve, DeleteMode::Vertices, &error)) << error;
  EXPECT_EQ(edit.geometry().vertices.count(), 4);
  EXPECT_EQ(edit.geometry().edges.count(), 4);
  EXPECT_EQ(edit.geometry().faces.count(), 1);
  EXPECT_TRUE(edit.selection().vertices.is_empty());
  EXPECT_TRUE(edit.selection().edges.is_empty());
  EXPECT_TRUE(edit.selection().faces.is_empty());
  EXPECT_TRUE(edit.selection().history.is_empty());
  EXPECT_FALSE(edit.selection().active.has_value());
  ASSERT_TRUE(geometry_validate_structure(edit.geometry(), &error)) << error;
  // Reusing the surviving boundary must work after deletion; no legacy identity reset.
  const auto remaining = edit.read_snapshot();
  ASSERT_TRUE(ed::nurbs::select_vertices(
      *bmain, *curve, remaining, cap, SelectionAction::Set, std::nullopt, &error))
      << error;
  ASSERT_TRUE(ed::nurbs::extrude(*bmain, *curve, &error)) << error;
  EXPECT_EQ(edit.geometry().vertices.count(), 6);
  EXPECT_EQ(edit.geometry().edges.count(), 7);
  EXPECT_EQ(edit.geometry().faces.count(), 2);
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  EXPECT_TRUE(curve->nurb.is_empty());
  EXPECT_EQ(curve->nurbs_geometry->faces.count(), 2);
}

TEST_F(NurbsNativeEditTest, EditorExtrusionReportsDeclaredSourceEndpointDeviation)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS endpoint deviation", OB_SURF);
  auto line = make_line();
  auto &vertex = *static_cast<NurbsTopologyVertex *>(line->vertices.first);
  vertex.position[0] = 0.125;
  vertex.tolerance = 0.125;
  line->stored_selection = selection_to_storage(make_selection());
  curve->nurbs_geometry = line.release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  const auto before = curve->editnurb->nurbs_edit->read_snapshot();
  std::string error;
  double deviation = 0;
  ASSERT_TRUE(ed::nurbs::extrude(*bmain, *curve, &error, &deviation)) << error;
  EXPECT_EQ(deviation, 0.125);
  const auto index = TopologyIndex::build(curve->editnurb->nurbs_edit->geometry());
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->vertex(1)->position[0], 0.125);
  EXPECT_EQ(index->vertex(1)->tolerance, 0.125);
  EXPECT_EQ(first_curve(curve->editnurb->nurbs_edit->geometry()).points[0].position[0], 0);
  EXPECT_EQ(first_curve(before.geometry()).points[0].position[0], 0);
}

TEST_F(NurbsNativeEditTest, EditorDeletionPreservesWireBoundariesAndCounterForOnlyFaces)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS face deletion", OB_SURF);
  auto wire = make_line();
  auto sheet = extrude_edges(*wire, {3}, {0, 2, 0}, 0);
  ASSERT_TRUE(sheet.has_value());
  curve->nurbs_geometry = sheet->geometry.release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  auto &edit = *curve->editnurb->nurbs_edit;
  Vector<int64_t> all;
  for (const NurbsTopologyVertex &vertex : edit.geometry().vertices) {
    all.append(vertex.id);
  }
  std::string error;
  ASSERT_TRUE(ed::nurbs::select_vertices(
      *bmain, *curve, edit.read_snapshot(), all, SelectionAction::Set, 2, &error))
      << error;
  const auto before = edit.read_snapshot();
  ASSERT_TRUE(ed::nurbs::delete_components(*bmain, *curve, DeleteMode::FacesOnly, &error))
      << error;
  EXPECT_EQ(edit.geometry().vertices.count(), 4);
  EXPECT_EQ(edit.geometry().edges.count(), 4);
  EXPECT_EQ(edit.geometry().faces.count(), 0);
  EXPECT_EQ(edit.geometry().next_id, before.geometry().next_id);
  EXPECT_EQ(edit.selection().vertices, before.selection().vertices);
  EXPECT_EQ(edit.selection().edges, before.selection().edges);
  EXPECT_EQ(edit.selection().active, before.selection().active);
  // No face remains: repeating Only Faces is a no-op and must not dirty the owner.
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  bmain->is_memfile_undo_flush_needed = false;
  const auto no_faces = edit.read_snapshot();
  EXPECT_FALSE(ed::nurbs::delete_components(*bmain, *curve, DeleteMode::FacesOnly, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(edit.read_snapshot(), no_faces);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
  ASSERT_TRUE(ed::nurbs::delete_components(*bmain, *curve, DeleteMode::Edges, &error)) << error;
  EXPECT_TRUE(edit.geometry().vertices.is_empty());
  EXPECT_TRUE(edit.geometry().edges.is_empty());
  EXPECT_TRUE(edit.selection().vertices.is_empty());
  EXPECT_FALSE(edit.selection().active.has_value());
  EXPECT_EQ(edit.geometry().next_id, before.geometry().next_id);
}

TEST_F(NurbsNativeEditTest, EditorExtrusionAndDeletionRejectInvalidRequestsWithoutDirtying)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS component rejection", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  auto &edit = *curve->editnurb->nurbs_edit;
  std::string error;
  for (const bool extrusion : {true, false}) {
    for (const bool inconsistent : {false, true}) {
      auto candidate = edit.snapshot();
      candidate.selection = {};
      if (inconsistent) {
        candidate.selection.edges.add(3);
      }
      ASSERT_TRUE(edit.commit(std::move(candidate), edit.read_snapshot()));
      ASSERT_TRUE(curve_edit_flush_to_id(*curve));
      bmain->is_memfile_undo_flush_needed = false;
      const auto before = edit.read_snapshot();
      const bool changed = extrusion ? ed::nurbs::extrude(*bmain, *curve, &error) :
                                       ed::nurbs::delete_components(
                                           *bmain, *curve, DeleteMode::Vertices, &error);
      EXPECT_FALSE(changed);
      EXPECT_EQ(error.empty(), !inconsistent);
      EXPECT_EQ(edit.read_snapshot(), before);
      EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
      EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
    }
  }
  auto candidate = edit.snapshot();
  candidate.selection = make_selection();
  candidate.geometry->next_id = std::numeric_limits<int64_t>::max();
  ASSERT_TRUE(edit.commit(std::move(candidate), edit.read_snapshot()));
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  bmain->is_memfile_undo_flush_needed = false;
  const auto exhausted = edit.read_snapshot();
  EXPECT_FALSE(ed::nurbs::extrude(*bmain, *curve, &error));
  EXPECT_NE(error.find("identity space"), std::string::npos);
  EXPECT_EQ(edit.read_snapshot(), exhausted);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
}

TEST_F(NurbsNativeEditTest, ViewportFixtureFilesPreserveSelectedSourceIDsAndGeometry)
{
  const char *directory = std::getenv("BLENDER_NURBS_EDIT_OUTPUT");
  const char *baseline_path = std::getenv("BLENDER_NURBS_PREVIEW_FIXTURE");
  if (directory == nullptr || baseline_path == nullptr) {
    GTEST_SKIP() << "Run after the native NURBS viewport test with its output directory";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *path) {
    return LoadedFile(BLO_read_from_file(path, BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto find_source = [](const BlendFileData &file) -> const Curve * {
    for (const Curve &curve : file.main->curves) {
      if (STREQ(curve.id.name + 2, "Native preview sheet")) {
        return &curve;
      }
    }
    return nullptr;
  };
  const auto baseline = load(baseline_path);
  ASSERT_NE(baseline, nullptr);
  const Curve *reference_curve = find_source(*baseline);
  ASSERT_NE(reference_curve, nullptr);
  ASSERT_NE(reference_curve->nurbs_geometry, nullptr);
  const NurbsGeometry &reference = *reference_curve->nurbs_geometry;
  const auto *selected_edge = static_cast<const NurbsTopologyEdge *>(reference.edges.first);
  ASSERT_NE(selected_edge, nullptr);
  for (const char *name : {"click.blend",
                           "box.blend",
                           "try-native-selection.blend",
                           "linked-click.blend",
                           "linked-hover.blend",
                           "linked-box.blend"})
  {
    SCOPED_TRACE(name);
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), directory, name);
    const auto loaded = load(path);
    ASSERT_NE(loaded, nullptr);
    const Curve *curve = find_source(*loaded);
    ASSERT_NE(curve, nullptr);
    ASSERT_NE(curve->nurbs_geometry, nullptr);
    EXPECT_EQ(curve->editnurb, nullptr);
    EXPECT_EQ(curve->nurbs_eval_source, nullptr);
    EXPECT_TRUE(curve->nurb.is_empty());
    const NurbsGeometry &geometry = *curve->nurbs_geometry;
    const auto index = TopologyIndex::build(geometry);
    ASSERT_TRUE(index.has_value());
    const auto reference_index = TopologyIndex::build(reference);
    ASSERT_TRUE(reference_index.has_value());
    EXPECT_EQ(geometry.next_id, reference.next_id);
    EXPECT_EQ(geometry.vertices.count(), reference.vertices.count());
    EXPECT_EQ(geometry.edges.count(), reference.edges.count());
    EXPECT_EQ(geometry.faces.count(), reference.faces.count());
    const auto selection = selection_from_storage(geometry.stored_selection);
    ASSERT_TRUE(selection.has_value());
    const bool linked = STRPREFIX(name, "linked-");
    EXPECT_EQ(selection->vertices.size(), linked ? 4 : 2);
    EXPECT_TRUE(selection->vertices.contains(selected_edge->vertex_ids[0]));
    EXPECT_TRUE(selection->vertices.contains(selected_edge->vertex_ids[1]));
    EXPECT_EQ(selection->edges.size(), linked ? 4 : 1);
    EXPECT_TRUE(selection->edges.contains(selected_edge->id));
    EXPECT_EQ(selection->faces.size(), linked ? 1 : 0);
    if (STREQ(name, "click.blend")) {
      EXPECT_EQ(selection->active,
                (ElementRef{ElementDomain::Vertex, selected_edge->vertex_ids[1]}));
      EXPECT_EQ(selection->history.size(), 2);
    }
    else if (STREQ(name, "linked-click.blend")) {
      EXPECT_EQ(selection->active,
                (ElementRef{ElementDomain::Vertex, selected_edge->vertex_ids[0]}));
      EXPECT_EQ(selection->history.size(), 1);
    }
    else {
      EXPECT_FALSE(selection->active.has_value());
      EXPECT_TRUE(selection->history.is_empty());
    }
    for (const NurbsTopologyVertex &vertex : reference.vertices) {
      const auto *saved = index->vertex(vertex.id);
      ASSERT_NE(saved, nullptr);
      EXPECT_EQ(double3(saved->position), double3(vertex.position));
      EXPECT_EQ(saved->tolerance, vertex.tolerance);
    }
    for (const NurbsTopologyEdge &edge : reference.edges) {
      const auto *saved = index->edge(edge.id);
      ASSERT_NE(saved, nullptr);
      EXPECT_EQ(saved->curve_id, edge.curve_id);
      EXPECT_EQ(saved->vertex_ids[0], edge.vertex_ids[0]);
      EXPECT_EQ(saved->vertex_ids[1], edge.vertex_ids[1]);
      EXPECT_EQ(saved->parameter_range[0], edge.parameter_range[0]);
      EXPECT_EQ(saved->parameter_range[1], edge.parameter_range[1]);
      EXPECT_EQ(index->uses_for_edge(edge.id).size(),
                reference_index->uses_for_edge(edge.id).size());
    }
    for (const NurbsTopologyFace &face : reference.faces) {
      const auto *saved = index->face(face.id);
      ASSERT_NE(saved, nullptr);
      EXPECT_EQ(saved->surface_id, face.surface_id);
      EXPECT_EQ(saved->orientation, face.orientation);
      EXPECT_EQ(saved->material_index, face.material_index);
      EXPECT_EQ(saved->loops.count(), face.loops.count());
    }
    Map<int64_t, const NurbsCurveGeometry *> curves;
    for (const NurbsCurveGeometry &resource : geometry.curves) {
      curves.add_new(resource.id, &resource);
    }
    EXPECT_EQ(geometry.curves.count(), reference.curves.count());
    for (const NurbsCurveGeometry &resource : reference.curves) {
      const auto *saved = curves.lookup_default(resource.id, nullptr);
      ASSERT_NE(saved, nullptr);
      EXPECT_EQ(saved->order, resource.order);
      EXPECT_EQ(saved->dimension, resource.dimension);
      ASSERT_EQ(saved->knots_num, resource.knots_num);
      EXPECT_TRUE(std::equal(saved->knots, saved->knots + saved->knots_num, resource.knots));
      ASSERT_EQ(saved->points_num, resource.points_num);
      for (int i = 0; i < resource.points_num; i++) {
        EXPECT_EQ(double3(saved->points[i].position), double3(resource.points[i].position));
        EXPECT_EQ(saved->points[i].weight, resource.points[i].weight);
      }
    }
    Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
    for (const NurbsSurfaceGeometry &resource : geometry.surfaces) {
      surfaces.add_new(resource.id, &resource);
    }
    EXPECT_EQ(geometry.surfaces.count(), reference.surfaces.count());
    for (const NurbsSurfaceGeometry &resource : reference.surfaces) {
      const auto *saved = surfaces.lookup_default(resource.id, nullptr);
      ASSERT_NE(saved, nullptr);
      EXPECT_EQ(saved->order_u, resource.order_u);
      EXPECT_EQ(saved->order_v, resource.order_v);
      EXPECT_EQ(saved->points_u, resource.points_u);
      EXPECT_EQ(saved->points_v, resource.points_v);
      ASSERT_EQ(saved->knots_u_num, resource.knots_u_num);
      ASSERT_EQ(saved->knots_v_num, resource.knots_v_num);
      EXPECT_TRUE(
          std::equal(saved->knots_u, saved->knots_u + saved->knots_u_num, resource.knots_u));
      EXPECT_TRUE(
          std::equal(saved->knots_v, saved->knots_v + saved->knots_v_num, resource.knots_v));
      ASSERT_EQ(saved->points_num, resource.points_num);
      for (int i = 0; i < resource.points_num; i++) {
        EXPECT_EQ(double3(saved->points[i].position), double3(resource.points[i].position));
        EXPECT_EQ(saved->points[i].weight, resource.points[i].weight);
      }
    }
  }
}

TEST_F(NurbsNativeEditTest, ViewportMovementFilesPreservePreciseSourceAndCancellation)
{
  const char *directory = std::getenv("BLENDER_NURBS_MOVE_OUTPUT");
  const char *baseline_path = std::getenv("BLENDER_NURBS_PREVIEW_FIXTURE");
  if (directory == nullptr || baseline_path == nullptr) {
    GTEST_SKIP() << "Run after the native NURBS movement viewport test";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *path) {
    return LoadedFile(BLO_read_from_file(path, BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto find_source = [](const BlendFileData &file) -> const NurbsGeometry * {
    for (const Curve &curve : file.main->curves) {
      if (STREQ(curve.id.name + 2, "Native preview sheet") && curve.nurb.is_empty() &&
          curve.editnurb == nullptr && curve.nurbs_eval_source == nullptr)
      {
        return curve.nurbs_geometry;
      }
    }
    return nullptr;
  };
  const auto baseline = load(baseline_path);
  ASSERT_NE(baseline, nullptr);
  const NurbsGeometry *reference = find_source(*baseline);
  ASSERT_NE(reference, nullptr);
  const int64_t selected =
      static_cast<const NurbsTopologyEdge *>(reference->edges.first)->vertex_ids[0];
  constexpr double delta = 0.12345678901234567;
  for (const char *name : {"move-click.blend",
                           "move-cancel.blend",
                           "move-undo.blend",
                           "move-redo.blend",
                           "move-f9.blend",
                           "move-box.blend",
                           "move-replay.blend",
                           "move-multi-axis.blend",
                           "move-reset.blend",
                           "move-unrepresentable.blend",
                           "move-unsupported-orientation.blend",
                           "move-negative.blend",
                           "move-inverse.blend",
                           "move-scaled-object.blend",
                           "move-unit.blend",
                           "move-f9-invalid.blend",
                           "move-f9-alias-edit.blend",
                           "move-sub-float.blend",
                           "move-f9-zero-alias.blend",
                           "move-f9-ui-edit.blend"})
  {
    SCOPED_TRACE(name);
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), directory, name);
    const auto loaded = load(path);
    ASSERT_NE(loaded, nullptr);
    const NurbsGeometry *geometry = find_source(*loaded);
    ASSERT_NE(geometry, nullptr);
    double3 offset(0, 0, delta);
    if (STREQ(name, "move-undo.blend") || STREQ(name, "move-reset.blend") ||
        STREQ(name, "move-unrepresentable.blend") || STREQ(name, "move-f9-invalid.blend") ||
        STREQ(name, "move-f9-zero-alias.blend") ||
        STREQ(name, "move-unsupported-orientation.blend"))
    {
      offset = double3(0);
    }
    else if (STREQ(name, "move-multi-axis.blend")) {
      offset = double3(delta, 0.23456789012345678, 0.34567890123456789);
    }
    else if (STREQ(name, "move-negative.blend")) {
      offset.z = -delta;
    }
    else if (STREQ(name, "move-inverse.blend")) {
      offset.z = 1 / delta;
    }
    else if (STREQ(name, "move-scaled-object.blend")) {
      offset.z = delta / 4;
    }
    else if (STREQ(name, "move-unit.blend")) {
      offset.z = 12.345678901234567 * 0.01;
    }
    else if (STREQ(name, "move-f9-alias-edit.blend")) {
      offset.z = double(float(delta));
    }
    else if (STREQ(name, "move-sub-float.blend")) {
      offset.z = 1e-200;
    }
    else if (STREQ(name, "move-f9-ui-edit.blend")) {
      offset.z = 0.12345679;
    }
    std::string error;
    const auto expected = translate_vertices(
        *reference, {selected}, offset, std::numeric_limits<double>::max(), &error);
    ASSERT_TRUE(expected.has_value()) << error;
    const auto index = TopologyIndex::build(*geometry, &error);
    ASSERT_TRUE(index.has_value()) << error;
    EXPECT_EQ(geometry->next_id, reference->next_id);
    EXPECT_EQ(geometry->vertices.count(), reference->vertices.count());
    EXPECT_EQ(geometry->edges.count(), reference->edges.count());
    EXPECT_EQ(geometry->faces.count(), reference->faces.count());
    EXPECT_EQ(index->vertex(selected)->position[2], offset.z);
    if (offset.z != double(float(offset.z))) {
      EXPECT_NE(index->vertex(selected)->position[2], double(float(offset.z)));
    }
    for (const NurbsTopologyVertex &vertex : expected->geometry->vertices) {
      const auto *saved = index->vertex(vertex.id);
      ASSERT_NE(saved, nullptr);
      EXPECT_EQ(double3(saved->position), double3(vertex.position));
      EXPECT_EQ(saved->tolerance, vertex.tolerance);
    }
    for (const NurbsTopologyEdge &edge : reference->edges) {
      const auto *saved = index->edge(edge.id);
      ASSERT_NE(saved, nullptr);
      EXPECT_EQ(saved->curve_id, edge.curve_id);
      EXPECT_EQ(saved->vertex_ids[0], edge.vertex_ids[0]);
      EXPECT_EQ(saved->vertex_ids[1], edge.vertex_ids[1]);
      EXPECT_EQ(saved->parameter_range[0], edge.parameter_range[0]);
      EXPECT_EQ(saved->parameter_range[1], edge.parameter_range[1]);
    }
    const auto selection = selection_from_storage(geometry->stored_selection);
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->vertices.size(), 1);
    EXPECT_TRUE(selection->vertices.contains(selected));
    EXPECT_TRUE(selection->edges.is_empty());
    EXPECT_TRUE(selection->faces.is_empty());
    const auto *curve = static_cast<const NurbsCurveGeometry *>(geometry->curves.first);
    ASSERT_EQ(geometry->curves.count(), expected->geometry->curves.count());
    for (const NurbsCurveGeometry &resource : expected->geometry->curves) {
      ASSERT_NE(curve, nullptr);
      EXPECT_EQ(curve->id, resource.id);
      EXPECT_EQ(curve->order, resource.order);
      EXPECT_EQ(curve->dimension, resource.dimension);
      ASSERT_EQ(curve->knots_num, resource.knots_num);
      EXPECT_TRUE(std::equal(curve->knots, curve->knots + curve->knots_num, resource.knots));
      ASSERT_EQ(curve->points_num, resource.points_num);
      for (int i = 0; i < curve->points_num; i++) {
        EXPECT_EQ(double3(curve->points[i].position), double3(resource.points[i].position));
        EXPECT_EQ(curve->points[i].weight, resource.points[i].weight);
      }
      curve = curve->next;
    }
    const auto *surface = static_cast<const NurbsSurfaceGeometry *>(geometry->surfaces.first);
    ASSERT_EQ(geometry->surfaces.count(), expected->geometry->surfaces.count());
    for (const NurbsSurfaceGeometry &resource : expected->geometry->surfaces) {
      ASSERT_NE(surface, nullptr);
      EXPECT_EQ(surface->id, resource.id);
      EXPECT_EQ(surface->order_u, resource.order_u);
      EXPECT_EQ(surface->order_v, resource.order_v);
      ASSERT_EQ(surface->points_num, resource.points_num);
      for (int i = 0; i < surface->points_num; i++) {
        EXPECT_EQ(double3(surface->points[i].position), double3(resource.points[i].position));
        EXPECT_EQ(surface->points[i].weight, resource.points[i].weight);
      }
      surface = surface->next;
    }
  }
}

TEST_F(NurbsNativeEditTest, ViewportAffineFilesMatchKernelAndActualMesh)
{
  const char *directory = std::getenv("BLENDER_NURBS_AFFINE_OUTPUT");
  const char *parity_directory = std::getenv("BLENDER_NURBS_AFFINE_PARITY_OUTPUT");
  if (!directory || !parity_directory) {
    GTEST_SKIP() << "Run after native Rotate/Scale viewport tests";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *root, const char *name) {
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), root, name);
    return LoadedFile(BLO_read_from_file(path, BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto source_of = [](const BlendFileData &file) -> const NurbsGeometry * {
    const NurbsGeometry *found = nullptr;
    for (const Curve &curve : file.main->curves) {
      if (curve.id.us > 0 && curve.nurbs_geometry) {
        EXPECT_TRUE(curve.nurb.is_empty());
        EXPECT_EQ(found, nullptr);
        found = curve.nurbs_geometry;
      }
    }
    return found;
  };
  const auto start_file = load(directory, "start.blend");
  ASSERT_NE(start_file, nullptr);
  const NurbsGeometry *start = source_of(*start_file);
  ASSERT_NE(start, nullptr);
  ASSERT_EQ(start->vertices.count(), 4);
  Vector<int64_t> all;
  for (const auto &vertex : start->vertices) {
    all.append(vertex.id);
  }
  auto affine = [&](const NurbsGeometry &source,
                    const Span<int64_t> vertices,
                    const double3x3 &linear,
                    const double3 &center) -> OwnedGeometry {
    double4x4 matrix = double4x4::identity();
    for (int column = 0; column < 3; column++) {
      for (int row = 0; row < 3; row++) {
        matrix[column][row] = linear[column][row];
      }
    }
    matrix.location() = center - linear * center;
    Vector<VertexTransform> transforms;
    for (const int64_t id : vertices) {
      transforms.append({id, matrix});
    }
    std::string error;
    auto result = transform_vertices(
        source, transforms, std::numeric_limits<double>::max(), &error);
    EXPECT_TRUE(result.has_value()) << error;
    return result ? std::move(result->geometry) : nullptr;
  };
  auto compare = [&](const char *name, const NurbsGeometry &expected) {
    SCOPED_TRACE(name);
    const auto file = load(directory, name);
    ASSERT_NE(file, nullptr);
    const NurbsGeometry *actual = source_of(*file);
    ASSERT_NE(actual, nullptr);
    expect_source_geometry_equal(*actual, expected);
  };
  double3x3 rz = double3x3::identity(), rx = rz, ry = rz;
  rz[0][0] = rz[1][1] = 0;
  rz[0][1] = 1;
  rz[1][0] = -1;
  rx[1][1] = rx[2][2] = 0;
  rx[1][2] = 1;
  rx[2][1] = -1;
  ry[0][0] = ry[2][2] = 0;
  ry[0][2] = -1;
  ry[2][0] = 1;
  double3x3 sx = double3x3::identity();
  sx[0][0] = 2;
  const double3 median(0, 1, 0);
  double3x3 collapse = double3x3::identity();
  collapse[0][0] = collapse[1][1] = collapse[2][2] = 0;
  const auto zero = affine(*start, all, collapse, median);
  ASSERT_NE(zero, nullptr);
  compare("scale-zero.blend", *zero);
  compare("scale-zero-undo.blend", *start);
  double3x3 reflection = double3x3::identity();
  reflection[0][0] = -1;
  const auto reflected = affine(*start, all, reflection, median);
  ASSERT_NE(reflected, nullptr);
  compare("scale-reflection.blend", *reflected);
  const auto rotated = affine(*start, all, rz, median);
  ASSERT_NE(rotated, nullptr);
  compare("rotate-90.blend", *rotated);
  compare("scale-undo.blend", *rotated);
  double3x3 sx3 = sx;
  sx3[0][0] = 3;
  const auto f9 = affine(*rotated, all, sx3, median);
  ASSERT_NE(f9, nullptr);
  compare("scale-f9-3.blend", *f9);
  const auto scaled = affine(*rotated, all, sx, median);
  ASSERT_NE(scaled, nullptr);
  for (const char *name : {"scale-2.blend", "scale-redo.blend", "cancel.blend"}) {
    compare(name, *scaled);
  }
  const auto gr = affine(*scaled, all, rz, median);
  ASSERT_NE(gr, nullptr);
  compare("switch-gr.blend", *gr);
  sx[0][0] = 0.5;
  const auto rs = affine(*gr, all, sx, median);
  ASSERT_NE(rs, nullptr);
  compare("switch-rs.blend", *rs);
  auto face = affine(*rs, all, rx, double3(0));
  ASSERT_NE(face, nullptr);
  double3x3 sz = double3x3::identity();
  sz[2][2] = 2;
  face = affine(*face, all, sz, double3(0));
  ASSERT_NE(face, nullptr);
  compare("face-rs.blend", *face);
  int64_t corner = 0;
  Vector<int64_t> edge;
  for (const auto &vertex : face->vertices) {
    const double3 position(vertex.position);
    if (position == double3(-0.5, 0, -2)) {
      corner = vertex.id;
    }
    if (position.x == 0.5) {
      edge.append(vertex.id);
    }
  }
  ASSERT_NE(corner, 0);
  ASSERT_EQ(edge.size(), 2);
  const auto vertex_rotate = affine(*face, {corner}, ry, double3(0));
  ASSERT_NE(vertex_rotate, nullptr);
  compare("vertex-rotate.blend", *vertex_rotate);
  compare("vertex-box-rotate.blend", *vertex_rotate);
  sx[0][0] = 2;
  const auto vertex_scale = affine(*vertex_rotate, {corner}, sx, double3(0));
  ASSERT_NE(vertex_scale, nullptr);
  compare("vertex-scale.blend", *vertex_scale);
  sz[2][2] = 0.5;
  const double3 edge_center(0.5, 0, 2);
  const auto edge_scale = affine(*vertex_rotate, edge, sz, edge_center);
  ASSERT_NE(edge_scale, nullptr);
  compare("edge-scale.blend", *edge_scale);
  float float_ry[3][3];
  const float axis[3] = {0, 1, 0};
  axis_angle_normalized_to_mat3(float_ry, axis, float(30 * M_PI / 180));
  const auto edge_rotate = affine(*edge_scale, edge, double3x3(float3x3(float_ry)), edge_center);
  ASSERT_NE(edge_rotate, nullptr);
  compare("edge-rotate.blend", *edge_rotate);
  compare("final.blend", *edge_rotate);
  const auto final_selection_file = load(directory, "final.blend");
  ASSERT_NE(final_selection_file, nullptr);
  const auto selected_final = selection_from_storage(
      source_of(*final_selection_file)->stored_selection);
  ASSERT_TRUE(selected_final.has_value());
  ASSERT_EQ(selected_final->edges.size(), 1);
  Vector<int64_t> selected_edges;
  for (const int64_t id : selected_final->edges) {
    selected_edges.append(id);
  }
  std::string extrusion_error;
  auto new_wall = extrude_components(
      *edge_rotate, {{}, selected_edges, {}}, {0, 0, 0}, 0, &extrusion_error);
  ASSERT_TRUE(new_wall.has_value()) << extrusion_error;
  Vector<int64_t> cap_vertices;
  for (const int64_t id : selected_final->vertices) {
    cap_vertices.append(new_wall->source_to_cap.lookup(id));
  }
  const auto continued = translate_vertices(*new_wall->geometry,
                                            cap_vertices,
                                            {0, 1, 0},
                                            std::numeric_limits<double>::max(),
                                            &extrusion_error);
  ASSERT_TRUE(continued.has_value()) << extrusion_error;
  compare("fresh-extruded.blend", *continued->geometry);
  sx[0][0] = 0.5;
  const auto continued_scale = affine(*continued->geometry, cap_vertices, sx, double3(0.5, 1, 2));
  ASSERT_NE(continued_scale, nullptr);
  compare("fresh-scaled.blend", *continued_scale);

  const auto parity_start_file = load(parity_directory, "start.blend");
  ASSERT_NE(parity_start_file, nullptr);
  const NurbsGeometry *before = source_of(*parity_start_file);
  ASSERT_NE(before, nullptr);
  char reference_path[FILE_MAX];
  BLI_path_join(reference_path, sizeof(reference_path), parity_directory, "mesh-reference.txt");
  std::ifstream reference(reference_path);
  ASSERT_TRUE(reference.good());
  std::string label;
  int mode, count, cases = 0;
  while (reference >> label >> mode >> count) {
    SCOPED_TRACE(label);
    ASSERT_EQ(count, 8);
    const std::string name = label + ".blend";
    const auto file = load(parity_directory, name.c_str());
    ASSERT_NE(file, nullptr);
    const auto *actual = source_of(*file);
    ASSERT_NE(actual, nullptr);
    const auto index = TopologyIndex::build(*actual);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(actual->next_id, before->next_id);
    EXPECT_EQ(actual->vertices.count(), before->vertices.count());
    EXPECT_EQ(actual->faces.count(), before->faces.count());
    Map<int64_t, double3> mesh_positions;
    for (int row = 0; row < count; row++) {
      double3 initial, expected;
      ASSERT_TRUE(bool(reference >> initial.x >> initial.y >> initial.z >> expected.x >>
                       expected.y >> expected.z));
      int64_t id = 0;
      for (const auto &vertex : before->vertices) {
        if (double3(vertex.position) == initial) {
          id = vertex.id;
        }
      }
      ASSERT_NE(id, 0);
      mesh_positions.add_new(id, expected);
      const auto *vertex = index->vertex(id);
      ASSERT_NE(vertex, nullptr);
      for (int axis = 0; axis < 3; axis++) {
        EXPECT_NEAR(vertex->position[axis], expected[axis], 2e-5);
      }
    }
    auto expected_point = [&](const double position[3]) -> double3 {
      for (const auto &vertex : before->vertices) {
        if (double3(vertex.position) == double3(position)) {
          return mesh_positions.lookup(vertex.id);
        }
      }
      ADD_FAILURE() << "Unexpected non-corner coefficient in linear parity fixture";
      return double3(0);
    };
    auto compare_points = [&](const NurbsControlPoint *actual,
                              const NurbsControlPoint *original,
                              const int points_num,
                              const bool parameter_space) {
      for (int i = 0; i < points_num; i++) {
        EXPECT_EQ(actual[i].weight, original[i].weight);
        const double3 expected = parameter_space ? double3(original[i].position) :
                                                   expected_point(original[i].position);
        for (int axis = 0; axis < 3; axis++) {
          if (parameter_space) {
            EXPECT_EQ(actual[i].position[axis], expected[axis]);
          }
          else {
            EXPECT_NEAR(actual[i].position[axis], expected[axis], 2e-5);
          }
        }
      }
    };
    expect_records_equal(actual->edges, before->edges, [](const auto &a, const auto &b) {
      EXPECT_EQ(a.curve_id, b.curve_id);
      EXPECT_EQ(a.vertex_ids[0], b.vertex_ids[0]);
      EXPECT_EQ(a.vertex_ids[1], b.vertex_ids[1]);
      EXPECT_EQ(std::memcmp(a.parameter_range, b.parameter_range, sizeof(a.parameter_range)), 0);
    });
    expect_records_equal(actual->curves, before->curves, [&](const auto &a, const auto &b) {
      EXPECT_EQ(a.order, b.order);
      EXPECT_EQ(a.dimension, b.dimension);
      ASSERT_EQ(a.points_num, b.points_num);
      ASSERT_EQ(a.knots_num, b.knots_num);
      EXPECT_EQ(std::memcmp(a.knots, b.knots, sizeof(double) * a.knots_num), 0);
      compare_points(a.points, b.points, a.points_num, a.dimension == 2);
    });
    expect_records_equal(actual->surfaces, before->surfaces, [&](const auto &a, const auto &b) {
      EXPECT_EQ(a.order_u, b.order_u);
      EXPECT_EQ(a.order_v, b.order_v);
      EXPECT_EQ(a.points_u, b.points_u);
      EXPECT_EQ(a.points_v, b.points_v);
      ASSERT_EQ(a.points_num, b.points_num);
      ASSERT_EQ(a.knots_u_num, b.knots_u_num);
      ASSERT_EQ(a.knots_v_num, b.knots_v_num);
      EXPECT_EQ(std::memcmp(a.knots_u, b.knots_u, sizeof(double) * a.knots_u_num), 0);
      EXPECT_EQ(std::memcmp(a.knots_v, b.knots_v, sizeof(double) * a.knots_v_num), 0);
      compare_points(a.points, b.points, a.points_num, false);
    });
    const auto selected = selection_from_storage(actual->stored_selection);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(int(selected->mode), 1 << (mode - 1));
    cases++;
  }
  EXPECT_EQ(cases, 6);
}

static Curve *make_tagged_edit_line(Main &bmain)
{
  Curve *curve = BKE_curve_add(&bmain, "Tagged component modes", OB_SURF);
  auto *line = MEM_new<Nurb>(__func__);
  line->type = CU_NURBS;
  line->flag = CU_SMOOTH;
  line->pntsu = line->orderu = line->orderv = 2;
  line->pntsv = 1;
  line->flagu = line->flagv = CU_NURB_ENDPOINT;
  line->resolu = curve->resolu;
  line->resolv = curve->resolv;
  line->topology_vertex_ids[0] = 1;
  line->topology_vertex_ids[1] = 2;
  line->topology_edge_ids[0] = 3;
  line->bp = MEM_new_array_zeroed<BPoint>(2, __func__);
  for (int i = 0; i < 2; i++) {
    line->bp[i].vec[0] = i == 0 ? -1 : 1;
    line->bp[i].vec[3] = 1;
    line->bp[i].radius = line->bp[i].weight = 1;
    line->bp[i].f1 = SELECT;
  }
  BKE_nurb_knot_calc_u(line);
  BLI_addtail(&curve->nurb, line);
  curve->editnurb = MEM_new<EditNurb>(__func__);
  BKE_nurbList_duplicate(&curve->editnurb->nurbs, &curve->nurb);
  curve->actnu = curve->actvert = 0;
  return curve;
}

TEST_F(NurbsNativeEditTest, TaggedModeEntryPreservesSourceAndInstallsOneAuthority)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = make_tagged_edit_line(*bmain);
  // Unequal rational weights must survive source entry, not just endpoint positions.
  Nurb &line = *static_cast<Nurb *>(curve->editnurb->nurbs.first);
  line.bp[0].vec[3] = 2;
  line.bp[1].vec[3] = 3;
  std::string error;
  OwnedGeometry original(geometry_from_tagged_legacy(curve->editnurb->nurbs, nullptr, &error));
  ASSERT_NE(original, nullptr) << error;
  const auto *old_source = curve->editnurb->nurbs.first;
  auto prepared = curve_edit_prepare_tagged(*curve, SelectionMode::Edge, &error);
  ASSERT_TRUE(prepared.has_value()) << error;
  EXPECT_EQ(curve->editnurb->nurbs.first, old_source);
  EXPECT_EQ(curve->nurbs_geometry, nullptr);
  expect_source_geometry_equal(prepared->edit->geometry(), *original);
  ASSERT_TRUE(curve_edit_commit_tagged(*curve, std::move(*prepared), &error)) << error;
  ASSERT_NE(curve->editnurb->nurbs_edit, nullptr);
  EXPECT_TRUE(curve->nurb.is_empty());
  EXPECT_TRUE(curve->editnurb->nurbs.is_empty());
  EXPECT_EQ(curve->editnurb->keyindex, nullptr);
  EXPECT_TRUE(curve_validate_source(*curve, &error)) << error;
  const auto &edit = *curve->editnurb->nurbs_edit;
  expect_source_geometry_equal(edit.geometry(), *original);
  EXPECT_EQ(edit.selection().mode, SelectionMode::Edge);
  EXPECT_TRUE(edit.selection().edges.contains(3));
  EXPECT_EQ(edit.selection().vertices.size(), 2);
  const auto stored = selection_from_storage(curve->nurbs_geometry->stored_selection);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->mode, SelectionMode::Edge);
}

TEST_F(NurbsNativeEditTest, TaggedModeEntryAllowsEmptySelectionAndRejectsUnmappedAttributes)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = make_tagged_edit_line(*bmain);
  auto &line = *static_cast<Nurb *>(curve->editnurb->nurbs.first);
  line.bp[0].f1 = line.bp[1].f1 = 0;
  std::string error;
  auto prepared = curve_edit_prepare_tagged(*curve, SelectionMode::Face, &error);
  ASSERT_TRUE(prepared.has_value()) << error;
  EXPECT_TRUE(prepared->edit->selection().vertices.is_empty());
  EXPECT_EQ(prepared->edit->selection().mode, SelectionMode::Face);
  for (int i = 0; i < 3; i++) {
    line.bp[0].radius = i == 0 ? 2 : 1;
    line.bp[0].tilt = i == 1 ? 0.25f : 0;
    line.bp[0].hide = i == 2;
    EXPECT_FALSE(curve_edit_prepare_tagged(*curve, SelectionMode::Edge, &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(curve->nurbs_geometry, nullptr);
    EXPECT_EQ(curve->editnurb->nurbs_edit, nullptr);
  }
  line.bp[0].hide = 0;
  line.bp[0].radius = 1;
  line.bp[0].tilt = 0;
  curve->extrude = 1;
  EXPECT_FALSE(curve_edit_prepare_tagged(*curve, SelectionMode::Edge, &error).has_value());
  curve->extrude = 0;
  EXPECT_EQ(curve->editnurb->nurbs.count(), 1);
}

TEST_F(NurbsNativeEditTest, TaggedModeEntryRejectsStaleAndForeignCandidates)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = make_tagged_edit_line(*bmain);
  Curve *other = make_tagged_edit_line(*bmain);
  std::string error;
  auto prepared = curve_edit_prepare_tagged(*curve, SelectionMode::Edge, &error);
  ASSERT_TRUE(prepared.has_value()) << error;
  auto &line = *static_cast<Nurb *>(curve->editnurb->nurbs.first);
  line.bp[0].vec[0] = -2;
  EXPECT_FALSE(curve_edit_commit_tagged(*curve, std::move(*prepared), &error));
  EXPECT_EQ(line.bp[0].vec[0], -2);
  EXPECT_EQ(curve->nurbs_geometry, nullptr);
  prepared = curve_edit_prepare_tagged(*curve, SelectionMode::Edge, &error);
  ASSERT_TRUE(prepared.has_value()) << error;
  EXPECT_FALSE(curve_edit_commit_tagged(*other, std::move(*prepared), &error));
  EXPECT_EQ(other->nurbs_geometry, nullptr);
  EXPECT_EQ(curve->editnurb->nurbs_edit, nullptr);
}

TEST_F(NurbsNativeEditTest, TaggedNativeUndoTransitionsKeepSnapshotsAndEndpointsIndependent)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = make_tagged_edit_line(*bmain);
  ListBaseT<Nurb> legacy = {nullptr, nullptr};
  BKE_nurbList_duplicate(&legacy, &curve->editnurb->nurbs);
  BLI_SCOPED_DEFER([&]() { BKE_nurbList_free(&legacy); });
  std::string error;
  auto prepared = curve_edit_prepare_tagged(*curve, SelectionMode::Edge, &error);
  ASSERT_TRUE(prepared.has_value()) << error;
  ASSERT_TRUE(curve_edit_commit_tagged(*curve, std::move(*prepared), &error)) << error;
  const auto held = curve->editnurb->nurbs_edit->read_snapshot();
  const auto native = held.copy_for_edit();
  ASSERT_TRUE(curve_edit_restore_tagged(*curve, legacy, &error)) << error;
  BKE_nurbList_duplicate(&curve->editnurb->nurbs, &legacy);
  EXPECT_EQ(curve->nurbs_geometry, nullptr);
  EXPECT_EQ(curve->editnurb->nurbs_edit, nullptr);
  EXPECT_EQ(held.geometry().vertices.count(), 2);
  ASSERT_TRUE(curve_edit_restore_native(*curve, native, &error)) << error;
  EXPECT_TRUE(curve->nurb.is_empty());
  EXPECT_TRUE(curve->editnurb->nurbs.is_empty());
  EXPECT_EQ(curve->editnurb->nurbs_edit->selection().mode, SelectionMode::Edge);
  expect_source_geometry_equal(curve->editnurb->nurbs_edit->geometry(), held.geometry());
  EXPECT_FALSE(curve->editnurb->nurbs_edit->read_snapshot() == held);
}

TEST_F(NurbsNativeEditTest, NormalModeEntryFilesRetainCompleteSurfaceSourceAndSelection)
{
  const char *directory = std::getenv("BLENDER_NURBS_MODE_ENTRY_OUTPUT");
  if (!directory) {
    GTEST_SKIP() << "Run after ordinary Add component-mode viewport tests";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *name) {
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), directory, name);
    return LoadedFile(BLO_read_from_file(path, BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto only_curve = [](const BlendFileData &file) -> const Curve * {
    const Curve *found = nullptr;
    for (const Curve &curve : file.main->curves) {
      if (curve.id.us > 0) {
        if (found) {
          return nullptr;
        }
        found = &curve;
      }
    }
    return found;
  };
  const auto before_file = load("existing-before.blend");
  const auto after_file = load("existing-after.blend");
  ASSERT_NE(before_file, nullptr);
  ASSERT_NE(after_file, nullptr);
  const auto *before = only_curve(*before_file);
  const auto *after = only_curve(*after_file);
  ASSERT_NE(before, nullptr);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(before->nurbs_geometry, nullptr);
  EXPECT_EQ(before->nurb.count(), 6);
  EXPECT_TRUE(after->nurb.is_empty());
  ASSERT_NE(after->nurbs_geometry, nullptr);
  std::string error;
  OwnedGeometry expected(geometry_from_tagged_legacy(before->nurb, nullptr, &error));
  ASSERT_NE(expected, nullptr) << error;
  expect_source_geometry_equal(*after->nurbs_geometry, *expected);
  const auto selected = selection_from_storage(after->nurbs_geometry->stored_selection);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->mode, SelectionMode::Face);
  EXPECT_EQ(selected->vertices.size(), 8);
  EXPECT_EQ(selected->edges.size(), 12);
  EXPECT_EQ(selected->faces.size(), 6);
}

TEST_F(NurbsNativeEditTest, ViewportFillFilesPreserveAuthoritativeSourceAndBoundaryIdentities)
{
  const char *directory = std::getenv("BLENDER_NURBS_FILL_OUTPUT");
  const char *baseline_path = std::getenv("BLENDER_NURBS_PREVIEW_FIXTURE");
  if (directory == nullptr || baseline_path == nullptr) {
    GTEST_SKIP() << "Run after the native Fill viewport test";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *path) {
    return LoadedFile(BLO_read_from_file(path, BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto find = [](const BlendFileData &file) -> const NurbsGeometry * {
    for (const Curve &curve : file.main->curves) {
      if (STREQ(curve.id.name + 2, "Native preview sheet") && curve.nurb.is_empty() &&
          curve.editnurb == nullptr && curve.nurbs_eval_source == nullptr)
      {
        return curve.nurbs_geometry;
      }
    }
    return nullptr;
  };
  const auto baseline = load(baseline_path);
  ASSERT_NE(baseline, nullptr);
  const auto *reference = find(*baseline);
  ASSERT_NE(reference, nullptr);
  for (const char *name : {"fill-planar-edge.blend",
                           "fill-planar-extrude.blend",
                           "fill-bent-edge.blend",
                           "fill-bent-extrude.blend",
                           "fill-connect.blend",
                           "fill-connect-extrude.blend",
                           "fill-triangle.blend",
                           "fill-triangle-extrude.blend",
                           "fill-merge.blend",
                           "fill-merge-extrude.blend"})
  {
    SCOPED_TRACE(name);
    const bool bent = STRPREFIX(name, "fill-bent");
    const bool connect = STRPREFIX(name, "fill-connect");
    const bool triangle = STRPREFIX(name, "fill-triangle");
    const bool merge = STRPREFIX(name, "fill-merge");
    const bool extrude = std::strstr(name, "extrude") != nullptr;
    OwnedGeometry expected(geometry_copy(reference));
    std::string error;
    SelectionState expected_selection;
    auto sweep = [&](const ComponentSelection &components, const double3 offset) {
      auto result = extrude_components(*expected, components, {0, 0, 0}, 0, &error);
      if (!result) {
        return false;
      }
      expected_selection = {};
      const auto closure = component_closure(*expected, components, &error);
      if (!closure) {
        return false;
      }
      for (const int64_t id : closure->vertices) {
        expected_selection.vertices.add(result->source_to_cap.lookup(id));
      }
      for (const int64_t id : closure->edges) {
        expected_selection.edges.add(result->source_to_cap.lookup(id));
      }
      for (const int64_t id : closure->faces) {
        expected_selection.faces.add(result->source_to_cap.lookup(id));
      }
      Vector<int64_t> cap;
      for (const NurbsTopologyVertex &vertex : expected->vertices) {
        if (const int64_t *id = result->source_to_cap.lookup_ptr(vertex.id)) {
          cap.append(*id);
        }
      }
      auto moved = translate_vertices(*result->geometry, cap, offset, 0, &error);
      if (!moved) {
        return false;
      }
      expected = std::move(moved->geometry);
      return true;
    };
    Vector<int64_t> corner_ids;
    for (const NurbsTopologyVertex &vertex : expected->vertices) {
      if ((vertex.position[0] == 1 && vertex.position[1] == 1) ||
          (connect && vertex.position[0] == -1 && vertex.position[1] == -1))
      {
        corner_ids.append(vertex.id);
      }
    }
    if (bent) {
      auto moved = translate_vertices(*expected, corner_ids, {0, 0, 1}, 0, &error);
      ASSERT_TRUE(moved.has_value()) << error;
      expected = std::move(moved->geometry);
    }
    if (merge) {
      const auto index = TopologyIndex::build(*expected);
      int64_t right = 0;
      for (const auto &edge : expected->edges) {
        if (index->vertex(edge.vertex_ids[0])->position[0] == 1 &&
            index->vertex(edge.vertex_ids[1])->position[0] == 1)
        {
          right = edge.id;
        }
      }
      ASSERT_NE(right, 0);
      ASSERT_TRUE(sweep({{}, {&right, 1}, {}}, {2, 0, 0})) << error;
    }
    else if (connect || triangle) {
      auto deleted = delete_components(
          *expected, {corner_ids, {}, {}}, DeleteMode::Vertices, &error);
      ASSERT_TRUE(deleted.has_value()) << error;
      expected = std::move(deleted->geometry);
    }
    else {
      const int64_t face = static_cast<NurbsTopologyFace *>(expected->faces.first)->id;
      auto deleted = delete_components(
          *expected, {{}, {}, {&face, 1}}, DeleteMode::FacesOnly, &error);
      ASSERT_TRUE(deleted.has_value()) << error;
      expected = std::move(deleted->geometry);
    }
    Vector<int64_t> vertices, edges, faces;
    for (const auto &vertex : expected->vertices) {
      vertices.append(vertex.id);
    }
    for (const auto &edge : expected->edges) {
      edges.append(edge.id);
    }
    for (const auto &face : expected->faces) {
      faces.append(face.id);
    }
    auto filled = fill_components(*expected, {vertices, edges, faces}, &error);
    ASSERT_TRUE(filled.has_value()) << error;
    expected = std::move(filled->geometry);
    expected_selection = {};
    for (const auto &vertex : expected->vertices) {
      expected_selection.vertices.add(vertex.id);
    }
    for (const auto &edge : expected->edges) {
      expected_selection.edges.add(edge.id);
    }
    for (const auto &face : expected->faces) {
      expected_selection.faces.add(face.id);
    }
    if (extrude) {
      vertices.clear();
      edges.clear();
      faces.clear();
      for (const auto &vertex : expected->vertices) {
        vertices.append(vertex.id);
      }
      for (const auto &edge : expected->edges) {
        edges.append(edge.id);
      }
      for (const auto &face : expected->faces) {
        faces.append(face.id);
      }
      ASSERT_TRUE(sweep({vertices, edges, faces}, {0, 0, 1})) << error;
    }
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), directory, name);
    const auto loaded = load(path);
    ASSERT_NE(loaded, nullptr);
    const auto *actual = find(*loaded);
    ASSERT_NE(actual, nullptr);
    expect_source_geometry_equal(*actual, *expected);
    const auto selected = selection_from_storage(actual->stored_selection);
    ASSERT_TRUE(selected.has_value());
    const auto normalized = selection_flush(*actual, *selected, selected->mode, &error);
    ASSERT_TRUE(normalized.has_value()) << error;
    EXPECT_EQ(selected->vertices, expected_selection.vertices);
    EXPECT_EQ(selected->vertices, normalized->vertices);
    EXPECT_EQ(selected->edges, expected_selection.edges);
    EXPECT_EQ(selected->edges, normalized->edges);
    EXPECT_EQ(selected->faces, expected_selection.faces);
    EXPECT_EQ(selected->faces, normalized->faces);
    EXPECT_EQ(selected->mode,
              connect || triangle ? SelectionMode::Vertex :
              merge || extrude    ? SelectionMode::Face :
                                    SelectionMode::Edge);
    if (merge) {
      ASSERT_TRUE(selected->active.has_value());
      EXPECT_EQ(selected->active->domain, ElementDomain::Face);
    }
  }
}

TEST_F(NurbsNativeEditTest, ViewportDuplicationFilesPreserveCopiedSourceGeometryAndSelection)
{
  const char *directory = std::getenv("BLENDER_NURBS_DUPLICATE_OUTPUT");
  const char *baseline_path = std::getenv("BLENDER_NURBS_PREVIEW_FIXTURE");
  if (directory == nullptr || baseline_path == nullptr) {
    GTEST_SKIP() << "Run after the native NURBS duplication viewport test";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *path) {
    return LoadedFile(BLO_read_from_file(path, BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto find = [](const BlendFileData &file, const char *name) -> const NurbsGeometry * {
    for (const Curve &curve : file.main->curves) {
      if (STREQ(curve.id.name + 2, name) && curve.nurb.is_empty() && curve.editnurb == nullptr &&
          curve.nurbs_eval_source == nullptr)
      {
        return curve.nurbs_geometry;
      }
    }
    return nullptr;
  };
  const auto baseline = load(baseline_path);
  ASSERT_NE(baseline, nullptr);
  for (const char *name : {"duplicate-click.blend",
                           "duplicate-undo.blend",
                           "duplicate-redo.blend",
                           "duplicate-cancel.blend",
                           "duplicate-box.blend",
                           "duplicate-point.blend",
                           "duplicate-edge.blend",
                           "duplicate-circle.blend",
                           "duplicate-hole.blend",
                           "duplicate-python.blend",
                           "try-native-duplicate.blend"})
  {
    SCOPED_TRACE(name);
    const bool circle = STREQ(name, "duplicate-circle.blend");
    const bool hole = STREQ(name, "duplicate-hole.blend") ||
                      STREQ(name, "try-native-duplicate.blend");
    const char *source_name = circle ? "Native preview circle" :
                              hole   ? "Native preview hole" :
                                       "Native preview sheet";
    const NurbsGeometry *reference = find(*baseline, source_name);
    ASSERT_NE(reference, nullptr);
    auto state = EditData::create(*reference);
    ASSERT_NE(state, nullptr);
    auto expected = state->snapshot();
    const auto &edge = *static_cast<const NurbsTopologyEdge *>(reference->edges.first);
    const int64_t a = edge.vertex_ids[0], b = edge.vertex_ids[1];
    const bool point = STREQ(name, "duplicate-point.blend");
    const bool wire = STREQ(name, "duplicate-edge.blend");
    for (const NurbsTopologyVertex &vertex : reference->vertices) {
      if ((!point && !wire) || vertex.id == a || (wire && vertex.id == b)) {
        expected.selection.vertices.add(vertex.id);
      }
    }
    if (!circle && !hole && !STREQ(name, "duplicate-box.blend")) {
      expected.selection.history.append({ElementDomain::Vertex, a});
      if (wire) {
        expected.selection.history.append({ElementDomain::Vertex, b});
      }
      expected.selection.active = expected.selection.history.last();
    }
    auto normalized = selection_flush(
        *expected.geometry, expected.selection, SelectionMode::Vertex);
    ASSERT_TRUE(normalized.has_value());
    expected.selection = std::move(*normalized);
    std::string error;
    auto duplicate = [&]() -> bool {
      const Vector<int64_t> vertices(expected.selection.vertices.begin(),
                                     expected.selection.vertices.end());
      const Vector<int64_t> edges(expected.selection.edges.begin(),
                                  expected.selection.edges.end());
      const Vector<int64_t> faces(expected.selection.faces.begin(),
                                  expected.selection.faces.end());
      auto copied = duplicate_components(*expected.geometry, {vertices, edges, faces}, &error);
      if (!copied) {
        return false;
      }
      auto selected = selection_remap(*expected.geometry,
                                      *copied->geometry,
                                      expected.selection,
                                      copied->source_to_copy,
                                      SelectionMode::Vertex,
                                      &error);
      if (!selected) {
        return false;
      }
      expected = {std::move(copied->geometry), std::move(*selected)};
      return true;
    };
    if (!STREQ(name, "duplicate-undo.blend")) {
      ASSERT_TRUE(duplicate()) << error;
      const double3 offset = circle || hole ? double3(3, 0, 0) :
                             point || wire  ? double3(0, 0, 0.125) :
                                              double3(0, 0, 0.12345678901234567);
      const Vector<int64_t> vertices(expected.selection.vertices.begin(),
                                     expected.selection.vertices.end());
      auto moved = translate_vertices(
          *expected.geometry, vertices, offset, std::numeric_limits<double>::max(), &error);
      ASSERT_TRUE(moved.has_value()) << error;
      expected.geometry = std::move(moved->geometry);
      if (STREQ(name, "duplicate-cancel.blend")) {
        ASSERT_TRUE(duplicate()) << error;
      }
    }
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), directory, name);
    const auto loaded = load(path);
    ASSERT_NE(loaded, nullptr);
    const NurbsGeometry *actual = find(*loaded, source_name);
    ASSERT_NE(actual, nullptr);
    ASSERT_TRUE(geometry_validate_structure(*actual, &error)) << error;
    expect_source_geometry_equal(*actual, *expected.geometry);
    const auto selected = selection_from_storage(actual->stored_selection);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->vertices, expected.selection.vertices);
    EXPECT_EQ(selected->edges, expected.selection.edges);
    EXPECT_EQ(selected->faces, expected.selection.faces);
    EXPECT_EQ(selected->active, expected.selection.active);
    EXPECT_EQ(selected->history.as_span(), expected.selection.history.as_span());
  }
}

TEST_F(NurbsNativeEditTest, ViewportExtrusionAndDeletionFilesPreserveAuthoritativeSource)
{
  const char *directory = std::getenv("BLENDER_NURBS_EXTRUSION_OUTPUT");
  const char *baseline_path = std::getenv("BLENDER_NURBS_EXTRUSION_FIXTURE");
  if (directory == nullptr || baseline_path == nullptr) {
    GTEST_SKIP() << "Run after the native NURBS extrusion viewport test";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *path) {
    return LoadedFile(BLO_read_from_file(path, BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto find = [](const BlendFileData &file, const char *name) -> const NurbsGeometry * {
    for (const Curve &curve : file.main->curves) {
      if (STREQ(curve.id.name + 2, name) && curve.nurb.is_empty() && curve.editnurb == nullptr &&
          curve.nurbs_eval_source == nullptr)
      {
        return curve.nurbs_geometry;
      }
    }
    return nullptr;
  };
  const auto baseline = load(baseline_path);
  ASSERT_NE(baseline, nullptr);
  for (const char *name : {"extrude-click.blend",
                           "extrude-box.blend",
                           "extrude-linked.blend",
                           "extrude-repeat.blend",
                           "extrude-delete.blend",
                           "extrude-delete-continue.blend",
                           "try-native-extrude-delete.blend",
                           "duplicate-then-extrude.blend",
                           "extrude-cancel.blend",
                           "extrude-cancel-then-move.blend",
                           "extrude-undo.blend",
                           "extrude-redo.blend",
                           "extrude-line.blend",
                           "extrude-line-repeat.blend",
                           "extrude-two-face-region.blend",
                           "extrude-trimmed-arc.blend",
                           "extrude-trimmed-arc-repeat.blend",
                           "extrude-circle.blend",
                           "extrude-hole.blend",
                           "delete-only-face.blend",
                           "delete-edges.blend",
                           "delete-faces.blend"})
  {
    SCOPED_TRACE(name);
    const bool multi_face = STREQ(name, "extrude-two-face-region.blend");
    const bool line = multi_face || STREQ(name, "extrude-line.blend") ||
                      STREQ(name, "extrude-line-repeat.blend");
    const bool arc = STREQ(name, "extrude-trimmed-arc.blend") ||
                     STREQ(name, "extrude-trimmed-arc-repeat.blend");
    const bool circle = STREQ(name, "extrude-circle.blend");
    const bool hole = STREQ(name, "extrude-hole.blend");
    const char *source_name = line   ? "Native preview line" :
                              arc    ? "Native preview trimmed arc" :
                              circle ? "Native preview circle" :
                              hole   ? "Native preview hole" :
                                       "Native preview sheet";
    const NurbsGeometry *reference = find(*baseline, source_name);
    ASSERT_NE(reference, nullptr);
    EditSnapshot expected{OwnedGeometry(geometry_copy(reference)), {}};
    Vector<int64_t> all;
    for (const NurbsTopologyVertex &vertex : reference->vertices) {
      all.append(vertex.id);
    }
    std::string error;
    auto select = [&](const Span<int64_t> vertices) -> bool {
      auto result = selection_apply_vertices(*expected.geometry,
                                             expected.selection,
                                             vertices,
                                             SelectionAction::Set,
                                             std::nullopt,
                                             &error);
      if (!result) {
        return false;
      }
      expected.selection = std::move(*result);
      return true;
    };
    ASSERT_TRUE(select(all)) << error;
    const bool clicked = STREQ(name, "extrude-click.blend");
    const bool linked = STREQ(name, "extrude-linked.blend") ||
                        STREQ(name, "extrude-repeat.blend") ||
                        STREQ(name, "extrude-delete.blend") ||
                        STREQ(name, "extrude-delete-continue.blend") ||
                        STREQ(name, "try-native-extrude-delete.blend");
    if (clicked || linked) {
      for (const int64_t id : all) {
        expected.selection.history.append({ElementDomain::Vertex, id});
        if (linked) {
          break;
        }
      }
      expected.selection.active = expected.selection.history.last();
    }
    auto move = [&](const double3 &offset) -> bool {
      if (offset == double3(0)) {
        return true;
      }
      const Vector<int64_t> vertices(expected.selection.vertices.begin(),
                                     expected.selection.vertices.end());
      auto moved = translate_vertices(
          *expected.geometry, vertices, offset, std::numeric_limits<double>::max(), &error);
      if (!moved) {
        return false;
      }
      expected.geometry = std::move(moved->geometry);
      return true;
    };
    auto construct = [&](const bool duplicate, const double3 &offset) -> bool {
      const Vector<int64_t> vertices(expected.selection.vertices.begin(),
                                     expected.selection.vertices.end());
      const Vector<int64_t> edges(expected.selection.edges.begin(),
                                  expected.selection.edges.end());
      const Vector<int64_t> faces(expected.selection.faces.begin(),
                                  expected.selection.faces.end());
      const ComponentSelection components{vertices, edges, faces};
      OwnedGeometry geometry;
      Map<int64_t, int64_t> ids;
      if (duplicate) {
        auto result = duplicate_components(*expected.geometry, components, &error);
        if (!result) {
          return false;
        }
        geometry = std::move(result->geometry);
        ids = std::move(result->source_to_copy);
      }
      else {
        auto result = extrude_components(*expected.geometry, components, {0, 0, 0}, 0, &error);
        if (!result) {
          return false;
        }
        geometry = std::move(result->geometry);
        ids = std::move(result->source_to_cap);
      }
      auto selected = selection_remap(
          *expected.geometry, *geometry, expected.selection, ids, SelectionMode::Vertex, &error);
      if (!selected) {
        return false;
      }
      expected = {std::move(geometry), std::move(*selected)};
      return move(offset);
    };
    auto remove = [&](const DeleteMode mode) -> bool {
      const Vector<int64_t> vertices(expected.selection.vertices.begin(),
                                     expected.selection.vertices.end());
      const Vector<int64_t> edges(expected.selection.edges.begin(),
                                  expected.selection.edges.end());
      const Vector<int64_t> faces(expected.selection.faces.begin(),
                                  expected.selection.faces.end());
      auto deleted = delete_components(*expected.geometry, {vertices, edges, faces}, mode, &error);
      if (!deleted) {
        return false;
      }
      Set<int64_t> removed = deleted->removed.vertices;
      for (const int64_t id : deleted->removed.edges) {
        removed.add(id);
      }
      for (const int64_t id : deleted->removed.faces) {
        removed.add(id);
      }
      auto selected = selection_flush(*deleted->geometry,
                                      selection_remove_ids(expected.selection, removed),
                                      SelectionMode::Vertex,
                                      &error);
      if (!selected) {
        return false;
      }
      expected = {std::move(deleted->geometry), std::move(*selected)};
      return true;
    };
    if (STREQ(name, "delete-only-face.blend") || STREQ(name, "delete-edges.blend")) {
      ASSERT_TRUE(remove(DeleteMode::FacesOnly)) << error;
      if (STREQ(name, "delete-edges.blend")) {
        ASSERT_TRUE(remove(DeleteMode::Edges)) << error;
      }
    }
    else if (STREQ(name, "delete-faces.blend")) {
      ASSERT_TRUE(remove(DeleteMode::Faces)) << error;
    }
    else if (!STREQ(name, "extrude-undo.blend")) {
      if (STREQ(name, "duplicate-then-extrude.blend")) {
        ASSERT_TRUE(construct(true, {0, 0, 0.5})) << error;
      }
      const bool zero = STREQ(name, "extrude-cancel.blend") || STREQ(name, "extrude-redo.blend") ||
                        STREQ(name, "extrude-cancel-then-move.blend");
      ASSERT_TRUE(construct(false,
                            zero ? double3(0) :
                            line ? double3(0, 2, 0) :
                                   double3(0, 0, 1)))
          << error;
      if (STREQ(name, "extrude-cancel-then-move.blend")) {
        ASSERT_TRUE(move({0, 0, 1})) << error;
      }
      const bool repeat = STREQ(name, "extrude-repeat.blend") ||
                          STREQ(name, "extrude-delete.blend") ||
                          STREQ(name, "extrude-delete-continue.blend") ||
                          STREQ(name, "try-native-extrude-delete.blend");
      if (repeat || multi_face || STREQ(name, "extrude-line-repeat.blend") ||
          STREQ(name, "extrude-trimmed-arc-repeat.blend"))
      {
        const Vector<int64_t> rim(expected.selection.vertices.begin(),
                                  expected.selection.vertices.end());
        ASSERT_TRUE(construct(false, line ? double3(0, 1, 0) : double3(0, 0, 1))) << error;
        if (multi_face) {
          Vector<int64_t> region;
          for (const NurbsTopologyVertex &vertex : expected.geometry->vertices) {
            region.append(vertex.id);
          }
          ASSERT_TRUE(select(region)) << error;
          ASSERT_TRUE(construct(false, {0, 0, 1})) << error;
        }
        if (repeat && !STREQ(name, "extrude-repeat.blend")) {
          ASSERT_TRUE(remove(DeleteMode::Vertices)) << error;
          if (!STREQ(name, "extrude-delete.blend")) {
            ASSERT_TRUE(select(rim)) << error;
            ASSERT_TRUE(construct(false, {0, 0, 1})) << error;
          }
        }
      }
    }
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), directory, name);
    const auto loaded = load(path);
    ASSERT_NE(loaded, nullptr);
    const NurbsGeometry *actual = find(*loaded, source_name);
    ASSERT_NE(actual, nullptr);
    ASSERT_TRUE(geometry_validate_structure(*actual, &error)) << error;
    expect_source_geometry_equal(*actual, *expected.geometry);
    const auto selected = selection_from_storage(actual->stored_selection);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->vertices, expected.selection.vertices);
    EXPECT_EQ(selected->edges, expected.selection.edges);
    EXPECT_EQ(selected->faces, expected.selection.faces);
    EXPECT_EQ(selected->active, expected.selection.active);
    EXPECT_EQ(selected->history.as_span(), expected.selection.history.as_span());
  }
}

TEST_F(NurbsNativeEditTest, ViewportComponentModesPreserveSourceAndPersistentSelection)
{
  const char *directory = std::getenv("BLENDER_NURBS_COMPONENT_OUTPUT");
  const char *baseline_path = std::getenv("BLENDER_NURBS_PREVIEW_FIXTURE");
  if (directory == nullptr || baseline_path == nullptr) {
    GTEST_SKIP() << "Run after the native component-mode viewport test";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *path) {
    return LoadedFile(BLO_read_from_file(path, BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto find = [](const BlendFileData &file, const char *name) -> const NurbsGeometry * {
    for (const Curve &curve : file.main->curves) {
      if (STREQ(curve.id.name + 2, name) && curve.nurb.is_empty() && curve.editnurb == nullptr &&
          curve.nurbs_eval_source == nullptr)
      {
        return curve.nurbs_geometry;
      }
    }
    return nullptr;
  };
  const auto baseline = load(baseline_path);
  ASSERT_NE(baseline, nullptr);
  for (const char *name : {"edge-click.blend",
                           "edge-box.blend",
                           "edge-extrude.blend",
                           "edge-box-extrude.blend",
                           "face-click.blend",
                           "face-box.blend",
                           "face-linked.blend",
                           "face-extrude.blend",
                           "face-box-extrude.blend",
                           "face-linked-extrude.blend",
                           "face-extrude-repeat.blend",
                           "face-duplicate.blend",
                           "try-native-component-modes.blend",
                           "face-hole.blend",
                           "face-hole-extrude.blend",
                           "face-opposite-region.blend"})
  {
    SCOPED_TRACE(name);
    const bool opposite = STREQ(name, "face-opposite-region.blend");
    const bool edge = STRPREFIX(name, "edge-");
    const bool hole = STRPREFIX(name, "face-hole");
    const char *source_name = hole ? "Native preview hole" : "Native preview sheet";
    const NurbsGeometry *reference = find(*baseline, source_name);
    ASSERT_NE(reference, nullptr);
    EditSnapshot expected{OwnedGeometry(geometry_copy(reference)), {}};
    expected.selection.mode = opposite ? SelectionMode::Vertex :
                              edge     ? SelectionMode::Edge :
                                         SelectionMode::Face;
    const ElementRef hit =
        edge ? ElementRef{ElementDomain::Edge,
                          static_cast<const NurbsTopologyEdge *>(reference->edges.first)->id} :
               ElementRef{ElementDomain::Face,
                          static_cast<const NurbsTopologyFace *>(reference->faces.first)->id};
    const bool explicit_history = !STREQ(name, "edge-box.blend") &&
                                  !STREQ(name, "edge-box-extrude.blend") &&
                                  !STREQ(name, "face-box.blend") &&
                                  !STREQ(name, "face-box-extrude.blend") &&
                                  !STREQ(name, "face-linked.blend") &&
                                  !STREQ(name, "face-linked-extrude.blend") &&
                                  !STREQ(name, "try-native-component-modes.blend");
    std::string error;
    if (opposite) {
      Vector<int64_t> vertices;
      for (const NurbsTopologyVertex &vertex : reference->vertices) {
        vertices.append(vertex.id);
      }
      auto selected = selection_apply_vertices(
          *expected.geometry, expected.selection, vertices, SelectionAction::Set);
      ASSERT_TRUE(selected.has_value());
      expected.selection = std::move(*selected);
    }
    else {
      auto selected = selection_apply(*expected.geometry,
                                      expected.selection,
                                      {&hit, 1},
                                      SelectionAction::Set,
                                      explicit_history ? std::optional(hit) : std::nullopt,
                                      &error);
      ASSERT_TRUE(selected.has_value()) << error;
      expected.selection = std::move(*selected);
    }
    auto construct = [&](const bool duplicate, const double3 &offset) -> bool {
      const Vector<int64_t> vertices(expected.selection.vertices.begin(),
                                     expected.selection.vertices.end());
      const Vector<int64_t> edges(expected.selection.edges.begin(),
                                  expected.selection.edges.end());
      const Vector<int64_t> faces(expected.selection.faces.begin(),
                                  expected.selection.faces.end());
      OwnedGeometry geometry;
      Map<int64_t, int64_t> ids;
      if (duplicate) {
        auto result = duplicate_components(*expected.geometry, {vertices, edges, faces}, &error);
        if (!result) {
          return false;
        }
        geometry = std::move(result->geometry);
        ids = std::move(result->source_to_copy);
      }
      else {
        auto result = extrude_components(
            *expected.geometry, {vertices, edges, faces}, {0, 0, 0}, 0, &error);
        if (!result) {
          return false;
        }
        geometry = std::move(result->geometry);
        ids = std::move(result->source_to_cap);
      }
      auto selection = selection_remap(
          *expected.geometry, *geometry, expected.selection, ids, expected.selection.mode, &error);
      if (!selection) {
        return false;
      }
      const Vector<int64_t> cap(selection->vertices.begin(), selection->vertices.end());
      auto moved = translate_vertices(
          *geometry, cap, offset, std::numeric_limits<double>::max(), &error);
      if (!moved) {
        return false;
      }
      expected = {std::move(moved->geometry), std::move(*selection)};
      return true;
    };
    const bool repeated = STREQ(name, "face-extrude-repeat.blend") ||
                          STREQ(name, "face-duplicate.blend");
    if (opposite) {
      ASSERT_TRUE(construct(false, {0, 0, 1})) << error;
      auto faces = selection_change_mode(
          *expected.geometry, expected.selection, SelectionMode::Face, &error);
      ASSERT_TRUE(faces.has_value()) << error;
      auto selected = selection_apply(
          *expected.geometry, *faces, {&hit, 1}, SelectionAction::Add, hit, &error);
      ASSERT_TRUE(selected.has_value()) << error;
      expected.selection = std::move(*selected);
      ASSERT_TRUE(construct(false, {0, 0, 1})) << error;
    }
    else if (strstr(name, "extrude") != nullptr || repeated ||
             STREQ(name, "try-native-component-modes.blend"))
    {
      ASSERT_TRUE(construct(false, {0, 0, 1})) << error;
    }
    if (repeated) {
      ASSERT_TRUE(construct(false, {0, 0, 1})) << error;
    }
    if (STREQ(name, "face-duplicate.blend")) {
      auto vertices = selection_change_mode(
          *expected.geometry, expected.selection, SelectionMode::Vertex, &error);
      ASSERT_TRUE(vertices.has_value()) << error;
      auto faces = selection_change_mode(
          *expected.geometry, *vertices, SelectionMode::Face, &error);
      ASSERT_TRUE(faces.has_value()) << error;
      expected.selection = std::move(*faces);
      ASSERT_TRUE(construct(true, {3, 0, 0})) << error;
    }
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), directory, name);
    const auto loaded = load(path);
    ASSERT_NE(loaded, nullptr);
    const NurbsGeometry *actual = find(*loaded, source_name);
    ASSERT_NE(actual, nullptr);
    ASSERT_TRUE(geometry_validate_structure(*actual, &error)) << error;
    expect_source_geometry_equal(*actual, *expected.geometry);
    const auto selection = selection_from_storage(actual->stored_selection, &error);
    ASSERT_TRUE(selection.has_value()) << error;
    EXPECT_EQ(selection->mode, expected.selection.mode);
    EXPECT_EQ(selection->vertices, expected.selection.vertices);
    EXPECT_EQ(selection->edges, expected.selection.edges);
    EXPECT_EQ(selection->faces, expected.selection.faces);
    EXPECT_EQ(selection->active, expected.selection.active);
    EXPECT_EQ(selection->history.as_span(), expected.selection.history.as_span());
  }
}

TEST_F(NurbsNativeEditTest, EvaluationCaptureCopiesObjectSourceAndStoredSelection)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS evaluated object source", OB_SURF);
  EXPECT_EQ(curve_eval_source_capture(*curve), nullptr);
  curve->nurbs_geometry = make_line().release();
  curve->nurbs_geometry->stored_selection = selection_to_storage(make_selection());
  const auto captured = curve_eval_source_capture(*curve);
  ASSERT_NE(captured, nullptr);
  ASSERT_TRUE(captured->snapshot.has_value()) << captured->error;
  EXPECT_TRUE(captured->error.empty());
  EXPECT_EQ(captured->snapshot->geometry().stored_selection, nullptr);
  EXPECT_EQ(captured->snapshot->selection().history.size(), 3);
  EXPECT_EQ(captured->snapshot->selection().active, make_selection().active);
  EXPECT_NE(first_curve(captured->snapshot->geometry()).points,
            first_curve(*curve->nurbs_geometry).points);
  curve->nurbs_geometry->next_id += 10;
  EXPECT_NE(captured->snapshot->geometry().next_id, curve->nurbs_geometry->next_id);
}

TEST_F(NurbsNativeEditTest, EvaluatedIDCopiesRetainLiveSourceAfterOriginalEditAndOwnerFree)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() {
    if (bmain != nullptr) {
      BKE_main_free(bmain);
    }
  });
  Curve *curve = BKE_curve_add(bmain, "NURBS evaluated live source", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  EditData &edit = *curve->editnurb->nurbs_edit;
  auto candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 4);
  candidate.selection = make_selection();
  ASSERT_TRUE(edit.commit(std::move(candidate), edit.revision()));
  const ReadSnapshot expected = edit.read_snapshot();
  const auto captured = curve_eval_source_capture(*curve);
  ASSERT_NE(captured, nullptr);
  ASSERT_TRUE(captured->snapshot.has_value()) << captured->error;
  EXPECT_EQ(*captured->snapshot, expected);
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 0);

  Curve *evaluated = id_cast<Curve *>(
      BKE_id_copy_ex(nullptr, &curve->id, nullptr, LIB_ID_COPY_LOCALIZE));
  BLI_SCOPED_DEFER([&]() { BKE_id_free(nullptr, evaluated); });
  evaluated->nurbs_eval_source = new EvaluationSource(*captured);
  Curve *copied = id_cast<Curve *>(
      BKE_id_copy_ex(nullptr, &evaluated->id, nullptr, LIB_ID_COPY_LOCALIZE));
  BLI_SCOPED_DEFER([&]() { BKE_id_free(nullptr, copied); });
  ASSERT_NE(copied->nurbs_eval_source, nullptr);
  EXPECT_NE(copied->nurbs_eval_source, evaluated->nurbs_eval_source);
  EXPECT_EQ(*copied->nurbs_eval_source->snapshot, expected);
  /* A copied evaluated lease cannot become another writable owner. */
  EXPECT_FALSE(curve_edit_begin(*copied));
  EXPECT_FALSE(curve_edit_flush_to_id(*copied));

  candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 2);
  ASSERT_TRUE(edit.commit(std::move(candidate), edit.revision()));
  BKE_main_free(bmain);
  bmain = nullptr;
  EXPECT_EQ(first_curve(copied->nurbs_eval_source->snapshot->geometry()).points[0].position[0], 4);
  const auto preview = evaluate_boundaries(copied->nurbs_eval_source->snapshot->geometry(),
                                           {2, 100, 0});
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->positions.first(), double3(4, 0, 0));
  const auto retained = curve_eval_source_capture(*copied);
  ASSERT_TRUE(retained->snapshot.has_value());
  EXPECT_EQ(*retained->snapshot, expected);
}

TEST_F(NurbsNativeEditTest, EvaluationCaptureRetainsFailureInsteadOfUsingOlderOrLegacySource)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS failed evaluated source", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  Nurb *foreign = MEM_new<Nurb>(__func__);
  BLI_addtail(&curve->editnurb->nurbs, foreign);
  const auto failed = curve_eval_source_capture(*curve);
  ASSERT_NE(failed, nullptr);
  EXPECT_FALSE(failed->snapshot.has_value());
  EXPECT_FALSE(failed->error.empty());
  EXPECT_NE(curve->nurbs_geometry, nullptr);
  EXPECT_EQ(curve->editnurb->nurbs.first, foreign);
  Curve evaluated;
  evaluated.nurbs_eval_source = new EvaluationSource(*failed);
  BLI_SCOPED_DEFER([&]() { delete evaluated.nurbs_eval_source; });
  const auto retained = curve_eval_source_capture(evaluated);
  EXPECT_FALSE(retained->snapshot.has_value());
  EXPECT_EQ(retained->error, failed->error);
}

TEST_F(NurbsNativeEditTest, DependencyGraphCapturesAndRefreshesLiveNativeSource)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Scene *scene = BKE_scene_add(bmain, "NURBS source handoff scene");
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  Curve *curve = BKE_curve_add(bmain, "NURBS source handoff", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  EditData &edit = *curve->editnurb->nurbs_edit;
  auto candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 2);
  candidate.selection = make_selection();
  ASSERT_TRUE(edit.commit(std::move(candidate), edit.revision()));

  Depsgraph *graph = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_VIEWPORT);
  BLI_SCOPED_DEFER([&]() { DEG_graph_free(graph); });
  ID *ids[] = {&curve->id};
  DEG_graph_build_from_ids(graph, {ids, 1});
  DEG_graph_tag_on_visible_update(graph, false);
  auto refresh = [&]() {
    DEG_evaluate_on_refresh(graph);
    /* Match scene_graph_update_tagged's completion so a previous explicit copy tag does not
     * force all later updates through full ID recopy. */
    DEG_ids_clear_recalc(graph, false);
  };
  refresh();
  Curve *evaluated = DEG_get_evaluated(graph, curve);
  ASSERT_NE(evaluated, curve);
  ASSERT_NE(evaluated->nurbs_eval_source, nullptr);
  ASSERT_TRUE(evaluated->nurbs_eval_source->snapshot.has_value())
      << evaluated->nurbs_eval_source->error;
  EXPECT_EQ(*evaluated->nurbs_eval_source->snapshot, edit.read_snapshot());
  const ReadSnapshot first_evaluation = *evaluated->nurbs_eval_source->snapshot;

  candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 3);
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), edit.read_snapshot()));
  refresh();
  evaluated = DEG_get_evaluated(graph, curve);
  ASSERT_NE(evaluated->nurbs_eval_source, nullptr);
  ASSERT_TRUE(evaluated->nurbs_eval_source->snapshot.has_value());
  EXPECT_EQ(*evaluated->nurbs_eval_source->snapshot, edit.read_snapshot());
  EXPECT_NE(*evaluated->nurbs_eval_source->snapshot, first_evaluation);
  EXPECT_EQ(first_curve(evaluated->nurbs_eval_source->snapshot->geometry()).points[0].position[0],
            5);
  EXPECT_EQ(first_curve(first_evaluation.geometry()).points[0].position[0], 2);
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 0);

  /* Without an explicit copy-on-eval tag, edit-mode refresh must still capture the new state.
   * A harmless evaluated-only sentinel distinguishes the fast path from a full ID recopy. */
  const int sentinel_resolution = curve->resolu + 1;
  evaluated->resolu = sentinel_resolution;
  candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 1);
  ASSERT_TRUE(edit.commit(std::move(candidate), edit.revision()));
  DEG_id_tag_update_ex(bmain, &curve->id, ID_RECALC_GEOMETRY | ID_RECALC_SELECT);
  refresh();
  evaluated = DEG_get_evaluated(graph, curve);
  ASSERT_NE(evaluated->nurbs_eval_source, nullptr);
  ASSERT_TRUE(evaluated->nurbs_eval_source->snapshot.has_value());
  EXPECT_EQ(evaluated->resolu, sentinel_resolution);
  EXPECT_EQ(*evaluated->nurbs_eval_source->snapshot, edit.read_snapshot());
  EXPECT_EQ(first_curve(evaluated->nurbs_eval_source->snapshot->geometry()).points[0].position[0],
            6);

  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  BKE_curve_editNurb_free(curve);
  DEG_id_tag_update_ex(bmain, &curve->id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  refresh();
  evaluated = DEG_get_evaluated(graph, curve);
  ASSERT_NE(evaluated->nurbs_eval_source, nullptr);
  ASSERT_TRUE(evaluated->nurbs_eval_source->snapshot.has_value());
  EXPECT_EQ(first_curve(evaluated->nurbs_eval_source->snapshot->geometry()).points[0].position[0],
            6);
  EXPECT_EQ(evaluated->nurbs_eval_source->snapshot->selection().history.size(), 3);
  EXPECT_EQ(evaluated->editnurb, nullptr);

  /* Removing the native payload is fixture cleanup, not an editing operation or migration. */
  geometry_free(curve->nurbs_geometry);
  curve->nurbs_geometry = nullptr;
  DEG_id_tag_update_ex(bmain, &curve->id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  refresh();
  evaluated = DEG_get_evaluated(graph, curve);
  EXPECT_EQ(evaluated->nurbs_eval_source, nullptr);
  EXPECT_EQ(first_curve(first_evaluation.geometry()).points[0].position[0], 2);
}

TEST_F(NurbsNativeEditTest, EditOwnerFlushAndFreeKeepIndependentSource)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Curve *curve = BKE_curve_add(bmain, "NURBS edit owner", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_NE(curve->nurbs_geometry, nullptr);
  std::string error;
  ASSERT_TRUE(curve_edit_begin(*curve, &error)) << error;
  EditData &edit = *curve->editnurb->nurbs_edit;
  EXPECT_NE(first_curve(edit.geometry()).points, first_curve(*curve->nurbs_geometry).points);
  EditSnapshot candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 2.0);
  candidate.geometry->next_id += 10;
  candidate.selection = make_selection();
  ASSERT_TRUE(edit.commit(std::move(candidate), edit.revision(), &error)) << error;
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 0.0);
  ASSERT_TRUE(curve_edit_flush_to_id(*curve, &error)) << error;
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 2.0);
  EXPECT_EQ(curve->nurbs_geometry->next_id, edit.geometry().next_id);
  EXPECT_NE(first_curve(edit.geometry()).points, first_curve(*curve->nurbs_geometry).points);
  EXPECT_EQ(edit.selection().history.size(), 3);
  EXPECT_FALSE(curve_edit_begin(*curve, &error));
  BKE_curve_editNurb_free(curve);
  EXPECT_EQ(curve->editnurb, nullptr);
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 2.0);
  ASSERT_NE(curve->nurbs_geometry->stored_selection, nullptr);
  ASSERT_TRUE(curve_edit_begin(*curve, &error)) << error;
  const EditData &reentered = *curve->editnurb->nurbs_edit;
  EXPECT_EQ(reentered.geometry().stored_selection, nullptr);
  EXPECT_EQ(reentered.selection().history.size(), 3);
  EXPECT_TRUE(reentered.selection().vertices.contains(1));
  EXPECT_TRUE(reentered.selection().edges.contains(3));
  EXPECT_EQ(*reentered.selection().active, (ElementRef{ElementDomain::Vertex, 2}));
  EXPECT_EQ(reentered.geometry().next_id, curve->nurbs_geometry->next_id);
  BKE_curve_editNurb_free(curve);
}

TEST_F(NurbsNativeEditTest, EntryAndFlushRejectConflictingLegacySourceAtomically)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS conflicting owner", OB_SURF);
  std::string error;
  EXPECT_FALSE(curve_edit_begin(*curve, &error));
  EXPECT_EQ(curve->editnurb, nullptr);
  curve->nurbs_geometry = make_line().release();
  Nurb *legacy = MEM_new<Nurb>(__func__);
  BLI_addtail(&curve->nurb, legacy);
  EXPECT_FALSE(curve_edit_begin(*curve, &error));
  EXPECT_EQ(curve->editnurb, nullptr);
  BLI_remlink(&curve->nurb, legacy);
  ASSERT_TRUE(curve_edit_begin(*curve, &error)) << error;
  BLI_addtail(&curve->editnurb->nurbs, legacy);
  NurbsGeometry *stored = curve->nurbs_geometry;
  EXPECT_FALSE(curve_edit_flush_to_id(*curve, &error));
  EXPECT_EQ(curve->nurbs_geometry, stored);
  EXPECT_EQ(curve->editnurb->nurbs.first, legacy);
  EXPECT_EQ(first_curve(*stored).points[0].position[0], 0.0);
}

TEST_F(NurbsNativeEditTest, FlushFailurePreservesLiveSourceDirtyFlagsAndEditMode)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Scene *scene = BKE_scene_add(bmain, "NURBS flush scene");
  Object *object = BKE_object_add_only_object(bmain, OB_SURF, "NURBS flush object");
  Curve *curve = BKE_curve_add(bmain, "NURBS flush source", OB_SURF);
  object->data = &curve->id;
  curve->nurbs_geometry = make_line().release();
  EXPECT_FALSE(ED_curve_editnurb_load(bmain, nullptr));
  EXPECT_FALSE(ED_curve_editnurb_load(bmain, object));
  ASSERT_TRUE(curve_edit_begin(*curve));
  object->mode = OB_MODE_EDIT;
  EditData *edit = curve->editnurb->nurbs_edit;
  EditSnapshot candidate = edit->snapshot();
  translate_fixture(candidate.geometry, 4.0);
  candidate.geometry->next_id += 10;
  candidate.selection = make_selection();
  ASSERT_TRUE(edit->commit(std::move(candidate), edit->revision()));
  const uint64_t revision = edit->revision();
  const int64_t counter = edit->geometry().next_id;
  NurbsGeometry *stored = curve->nurbs_geometry;
  curve->editnurb->needs_flush_to_id = true;
  bmain->is_memfile_undo_flush_needed = true;
  Nurb *foreign = MEM_new<Nurb>(__func__);
  BLI_addtail(&curve->editnurb->nurbs, foreign);
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });

  EXPECT_FALSE(ED_curve_editnurb_load(bmain, object, &reports));
  EXPECT_EQ(ed::object::editmode_load_ex(bmain, object, &reports),
            ed::object::EditModeDataResult::Failure);
  EXPECT_FALSE(
      ED_editors_flush_edits_for_object_with_result(bmain, object, false, true, &reports).success);
  EXPECT_FALSE(ED_editors_flush_edits_with_result(bmain, false, true, &reports).success);
  wmWindowManager wm{};
  const auto user_flags = U.flag;
  BLI_SCOPED_DEFER([&]() { U.flag = user_flags; });
  U.flag &= ~USER_AUTOSAVE;
  EXPECT_FALSE(WM_autosave_write(&wm, bmain, &reports));
  EXPECT_TRUE(wm.autosave_scheduled);
  EXPECT_FALSE(
      ed::object::editmode_exit_ex(bmain, scene, object, ed::object::EM_FREEDATA, &reports));
  EXPECT_FALSE(reports.list.is_empty());
  EXPECT_TRUE(object->mode & OB_MODE_EDIT);
  ASSERT_NE(curve->editnurb, nullptr);
  EXPECT_EQ(curve->editnurb->nurbs_edit, edit);
  EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
  EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
  EXPECT_EQ(curve->nurbs_geometry, stored);
  EXPECT_EQ(first_curve(*stored).points[0].position[0], 0.0);
  EXPECT_EQ(first_curve(edit->geometry()).points[0].position[0], 4.0);
  EXPECT_EQ(edit->revision(), revision);
  EXPECT_EQ(edit->geometry().next_id, counter);
  EXPECT_EQ(edit->selection().history.size(), 3);

  BLI_remlink(&curve->editnurb->nurbs, foreign);
  MEM_delete(foreign);
  const EditDataFlushResult flushed = ED_editors_flush_edits_with_result(
      bmain, false, true, &reports);
  EXPECT_TRUE(flushed.success);
  EXPECT_TRUE(flushed.has_edits);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 4.0);
  ASSERT_NE(curve->nurbs_geometry->stored_selection, nullptr);
  ASSERT_TRUE(
      ed::object::editmode_exit_ex(bmain, scene, object, ed::object::EM_FREEDATA, &reports));
  EXPECT_EQ(object->mode & OB_MODE_EDIT, 0);
  EXPECT_EQ(curve->editnurb, nullptr);
  ASSERT_TRUE(curve_edit_begin(*curve));
  EXPECT_EQ(curve->editnurb->nurbs_edit->selection().history.size(), 3);
  EXPECT_EQ(curve->editnurb->nurbs_edit->geometry().next_id, counter);
}

TEST_F(NurbsNativeEditTest, MultiObjectExitValidatesSourcesBeforeFreeingAnyEditData)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Scene *scene = BKE_scene_add(bmain, "NURBS multi-object exit scene");
  Object *objects[2];
  Curve *curves[2];
  EditData *edits[2];
  for (int i = 0; i < 2; i++) {
    objects[i] = BKE_object_add_only_object(bmain, OB_SURF, "NURBS exit object");
    curves[i] = BKE_curve_add(bmain, "NURBS exit source", OB_SURF);
    objects[i]->data = &curves[i]->id;
    curves[i]->nurbs_geometry = make_line().release();
    ASSERT_TRUE(curve_edit_begin(*curves[i]));
    objects[i]->mode = OB_MODE_EDIT;
    edits[i] = curves[i]->editnurb->nurbs_edit;
    ASSERT_TRUE(BKE_collection_object_add(bmain, scene->master_collection, objects[i]));
  }
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  view_layer->basact = BKE_view_layer_base_find(view_layer, objects[0]);
  ASSERT_NE(view_layer->basact, nullptr);
  Nurb *foreign = MEM_new<Nurb>(__func__);
  BLI_addtail(&curves[1]->editnurb->nurbs, foreign);
  EXPECT_FALSE(
      ed::object::editmode_exit_multi_ex(bmain, scene, view_layer, ed::object::EM_FREEDATA));
  for (int i = 0; i < 2; i++) {
    EXPECT_TRUE(objects[i]->mode & OB_MODE_EDIT);
    ASSERT_NE(curves[i]->editnurb, nullptr);
    EXPECT_EQ(curves[i]->editnurb->nurbs_edit, edits[i]);
  }
  BLI_remlink(&curves[1]->editnurb->nurbs, foreign);
  MEM_delete(foreign);
  EXPECT_TRUE(
      ed::object::editmode_exit_multi_ex(bmain, scene, view_layer, ed::object::EM_FREEDATA));
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(objects[i]->mode & OB_MODE_EDIT, 0);
    EXPECT_EQ(curves[i]->editnurb, nullptr);
  }
}

TEST_F(NurbsNativeEditTest, GlobalFlushChecksNativeOwnersWithoutObjectUsers)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS unreferenced source", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  auto *edge = static_cast<NurbsTopologyEdge *>(curve->nurbs_geometry->edges.first);
  const int64_t vertex = edge->vertex_ids[0];
  edge->vertex_ids[0] = 999;
  EXPECT_FALSE(ED_editors_flush_edits_with_result(bmain, false, false).success);
  EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
  edge->vertex_ids[0] = vertex;
  const EditDataFlushResult flushed = ED_editors_flush_edits_with_result(bmain, false, false);
  EXPECT_TRUE(flushed.success);
  EXPECT_FALSE(flushed.has_edits);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
}

TEST_F(NurbsNativeEditTest, EditorCommitTagsOwnerAndQueuesNotificationAfterKernelCommit)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Curve *curve = BKE_curve_add(bmain, "NURBS editor commit source", OB_SURF);
  Object *object = BKE_object_add_only_object(bmain, OB_SURF, "NURBS editor commit object");
  object->data = &curve->id;
  object->mode = OB_MODE_EDIT;
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  wmWindowManager *wm = static_cast<wmWindowManager *>(
      BKE_libblock_alloc(bmain, ID_WM, "NURBS commit window manager", 0));
  wm->runtime = MEM_new<WindowManagerRuntime>(__func__);
  ASSERT_TRUE(wm->runtime->notifier_queue.is_empty());
  curve->editnurb->needs_flush_to_id = false;
  bmain->is_memfile_undo_flush_needed = false;
  curve->id.recalc = 0;
  curve->id.recalc_after_undo_push = 0;
  EditData &edit = *curve->editnurb->nurbs_edit;
  const uint64_t revision = edit.revision();
  EditSnapshot candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 2.0);
  candidate.selection = make_selection();
  candidate.geometry->next_id += 10;
  std::string error;
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), revision, &error))
      << error;
  EXPECT_EQ(edit.revision(), revision + 1);
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 2.0);
  EXPECT_EQ(edit.selection().history.size(), 3);
  EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
  EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
  const unsigned int flags = ID_RECALC_GEOMETRY | ID_RECALC_SELECT | ID_RECALC_SYNC_TO_EVAL;
  EXPECT_EQ(curve->id.recalc & flags, flags);
  EXPECT_EQ(curve->id.recalc_after_undo_push & flags, flags);
  ASSERT_EQ(wm->runtime->notifier_queue.count(), 1);
  const wmNotifier &note = *static_cast<const wmNotifier *>(wm->runtime->notifier_queue.first);
  EXPECT_EQ(note.reference, &curve->id);
  EXPECT_EQ(note.category, NC_GEOM);
  EXPECT_EQ(note.data, ND_DATA);
  EXPECT_EQ(wm->runtime->undo_stack, nullptr);
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 0.0);
  ASSERT_TRUE(ED_editors_flush_edits_with_result(bmain, false, true, nullptr).success);
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 2.0);
  EXPECT_EQ(curve->nurbs_geometry->next_id, edit.geometry().next_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
}

TEST_F(NurbsNativeEditTest, RejectedEditorCommitDoesNotNotifyOrMarkDirty)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Curve *curve = BKE_curve_add(bmain, "NURBS rejected commit", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  EditSnapshot no_edit;
  no_edit.geometry = make_line();
  EXPECT_FALSE(ed::nurbs::edit_commit(*bmain, *curve, std::move(no_edit), 1));
  ASSERT_TRUE(curve_edit_begin(*curve));
  wmWindowManager *wm = static_cast<wmWindowManager *>(
      BKE_libblock_alloc(bmain, ID_WM, "NURBS rejected commit manager", 0));
  wm->runtime = MEM_new<WindowManagerRuntime>(__func__);
  curve->editnurb->needs_flush_to_id = false;
  bmain->is_memfile_undo_flush_needed = false;
  curve->id.recalc = 0;
  curve->id.recalc_after_undo_push = 0;
  EditData &edit = *curve->editnurb->nurbs_edit;
  const uint64_t revision = edit.revision();
  const int64_t counter = edit.geometry().next_id;
  std::string error;
  EXPECT_FALSE(ed::nurbs::edit_commit(*bmain, *curve, edit.snapshot(), revision - 1, &error));
  EXPECT_FALSE(error.empty());
  EditSnapshot invalid = edit.snapshot();
  static_cast<NurbsTopologyEdge *>(invalid.geometry->edges.first)->vertex_ids[0] = 999;
  EXPECT_FALSE(ed::nurbs::edit_commit(*bmain, *curve, std::move(invalid), revision, &error));
  EXPECT_FALSE(error.empty());
  invalid = edit.snapshot();
  invalid.selection.vertices.add(999);
  EXPECT_FALSE(ed::nurbs::edit_commit(*bmain, *curve, std::move(invalid), revision, &error));
  EXPECT_FALSE(error.empty());
  Nurb *foreign = MEM_new<Nurb>(__func__);
  BLI_addtail(&curve->editnurb->nurbs, foreign);
  EXPECT_FALSE(ed::nurbs::edit_commit(*bmain, *curve, edit.snapshot(), revision, &error));
  EXPECT_FALSE(error.empty());
  BLI_remlink(&curve->editnurb->nurbs, foreign);
  MEM_delete(foreign);
  EXPECT_EQ(edit.revision(), revision);
  EXPECT_EQ(edit.geometry().next_id, counter);
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 0.0);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
  EXPECT_EQ(curve->id.recalc, 0);
  EXPECT_EQ(curve->id.recalc_after_undo_push, 0);
  EXPECT_TRUE(wm->runtime->notifier_queue.is_empty());
}

TEST_F(NurbsNativeEditTest, SelectionOnlyEditorCommitUsesTheSameOwnerNotificationPath)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS selection commit", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  curve->editnurb->needs_flush_to_id = false;
  bmain->is_memfile_undo_flush_needed = false;
  EditData &edit = *curve->editnurb->nurbs_edit;
  EditSnapshot candidate = edit.snapshot();
  candidate.selection = make_selection();
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), edit.revision()));
  EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
  EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 0.0);
  EXPECT_EQ(edit.selection().history.size(), 3);
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  BKE_curve_editNurb_free(curve);
  ASSERT_TRUE(curve_edit_begin(*curve));
  EXPECT_EQ(curve->editnurb->nurbs_edit->selection().history.size(), 3);
}

TEST_F(NurbsNativeEditTest, RuledConstructionCommitsAndRestoresSourceIdentityWithSelection)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS ruled edit source", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  EditData &edit = *curve->editnurb->nurbs_edit;
  EditSnapshot before = edit.snapshot();
  const int64_t original_counter = before.geometry->next_id;
  auto construction = extrude_edges(edit.geometry(), {3}, {0, 0, 2}, 0);
  ASSERT_TRUE(construction.has_value());
  const int64_t cap_edge = construction->source_to_cap.lookup(3);
  const int64_t face_id = construction->edge_to_face.lookup(3);
  EditSnapshot candidate;
  candidate.geometry = std::move(construction->geometry);
  candidate.selection.edges.add(cap_edge);
  candidate.selection.active = ElementRef{ElementDomain::Edge, cap_edge};
  candidate.selection.history.append(*candidate.selection.active);
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), edit.revision()));
  EditSnapshot after = edit.snapshot();
  const int64_t constructed_counter = edit.geometry().next_id;
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  EXPECT_EQ(curve->nurbs_geometry->faces.count(), 1);
  EXPECT_EQ(edit.geometry().vertices.count(), 4);
  EXPECT_EQ(edit.geometry().edges.count(), 4);
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(before), edit.revision()));
  EXPECT_TRUE(edit.geometry().faces.is_empty());
  EXPECT_EQ(edit.geometry().next_id, original_counter);
  EXPECT_TRUE(edit.selection().edges.is_empty());
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(after), edit.revision()));
  ASSERT_TRUE(curve_edit_flush_to_id(*curve));
  BKE_curve_editNurb_free(curve);
  ASSERT_TRUE(curve_edit_begin(*curve));
  const EditData &reentered = *curve->editnurb->nurbs_edit;
  EXPECT_EQ(reentered.geometry().next_id, constructed_counter);
  const auto index = TopologyIndex::build(reentered.geometry());
  ASSERT_TRUE(index.has_value());
  EXPECT_NE(index->edge(3), nullptr);
  EXPECT_NE(index->edge(cap_edge), nullptr);
  EXPECT_NE(index->face(face_id), nullptr);
  EXPECT_EQ(index->uses_for_edge(3).size(), 1);
  EXPECT_EQ(reentered.selection().edges.size(), 1);
  EXPECT_TRUE(reentered.selection().edges.contains(cap_edge));
  ASSERT_TRUE(reentered.selection().active.has_value());
  EXPECT_EQ(*reentered.selection().active, (ElementRef{ElementDomain::Edge, cap_edge}));
}

TEST_F(NurbsNativeEditTest, EditorReadTokenRejectsFreedAndReenteredSourcesWithoutDirtyingOwner)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS token owner", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(curve_edit_begin(*curve));
  const ReadSnapshot stale = curve->editnurb->nurbs_edit->read_snapshot();
  BKE_curve_editNurb_free(curve);
  ASSERT_TRUE(curve_edit_begin(*curve));
  EditData &edit = *curve->editnurb->nurbs_edit;
  EXPECT_EQ(edit.revision(), stale.revision());
  EXPECT_NE(edit.read_snapshot(), stale);
  curve->editnurb->needs_flush_to_id = false;
  bmain->is_memfile_undo_flush_needed = false;
  curve->id.recalc = 0;
  EditSnapshot candidate = stale.copy_for_edit();
  translate_fixture(candidate.geometry, 20.0);
  std::string error;
  EXPECT_FALSE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), stale, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
  EXPECT_EQ(curve->id.recalc, 0);
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 0.0);
  const ReadSnapshot current = edit.read_snapshot();
  candidate = current.copy_for_edit();
  translate_fixture(candidate.geometry, 2.0);
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), current, &error))
      << error;
  EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
  EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 2.0);
}

/* Exercise the native Curve undo callbacks directly. The generic undo stack,
 * memfile transitions and UI shortcuts are separate integration checks. */
TEST_F(NurbsNativeEditTest, NativeCurveUndoCallbacksRestoreSourceSelectionAndCounter)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Scene *scene = BKE_scene_add(bmain, "NURBS undo scene");
  Object *object = BKE_object_add_only_object(bmain, OB_SURF, "NURBS undo object");
  Curve *curve = BKE_curve_add(bmain, "NURBS undo source", OB_SURF);
  object->data = &curve->id;
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(BKE_collection_object_add(bmain, scene->master_collection, object));
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  view_layer->basact = BKE_view_layer_base_find(view_layer, object);
  ASSERT_NE(view_layer->basact, nullptr);
  object->mode = OB_MODE_EDIT;
  std::string error;
  ASSERT_TRUE(curve_edit_begin(*curve, &error)) << error;
  EditData &edit = *curve->editnurb->nurbs_edit;
  EditSnapshot initial = edit.snapshot();
  initial.selection = make_selection();
  ASSERT_TRUE(edit.commit(std::move(initial), edit.revision(), &error)) << error;
  const int64_t counter_before = edit.geometry().next_id;
  bContext *context = CTX_create();
  BLI_SCOPED_DEFER([&]() { CTX_free(context); });
  CTX_data_main_set(context, bmain);
  CTX_data_scene_set(context, scene);
  UndoType type{};
  ED_curve_undosys_type(&type);
  ASSERT_TRUE(type.poll(context));
  UndoStep *before = static_cast<UndoStep *>(MEM_new_zeroed(type.step_size, __func__));
  before->type = &type;
  BLI_SCOPED_DEFER([&]() {
    type.step_free(before);
    MEM_delete(before);
  });
  ASSERT_TRUE(type.step_encode(context, bmain, before));
  EXPECT_GT(before->data_size, sizeof(EditSnapshot));
  EditSnapshot candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 4.0);
  candidate.geometry->next_id += 10;
  candidate.selection.vertices.remove(1);
  candidate.selection.active = ElementRef{ElementDomain::Edge, 3};
  candidate.selection.history.clear();
  candidate.selection.history.append({ElementDomain::Edge, 3});
  ASSERT_TRUE(edit.commit(std::move(candidate), edit.revision(), &error)) << error;
  UndoStep *after = static_cast<UndoStep *>(MEM_new_zeroed(type.step_size, __func__));
  after->type = &type;
  BLI_SCOPED_DEFER([&]() {
    type.step_free(after);
    MEM_delete(after);
  });
  ASSERT_TRUE(type.step_encode(context, bmain, after));
  const uint64_t revision_before_undo = edit.revision();
  type.step_decode(context, bmain, before, STEP_UNDO, true);
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 0.0);
  EXPECT_EQ(edit.geometry().next_id, counter_before);
  EXPECT_EQ(edit.selection().history.size(), 3);
  EXPECT_TRUE(edit.selection().vertices.contains(1));
  EXPECT_EQ(*edit.selection().active, (ElementRef{ElementDomain::Vertex, 2}));
  EXPECT_EQ(edit.revision(), revision_before_undo + 1);
  type.step_decode(context, bmain, after, STEP_REDO, true);
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 4.0);
  EXPECT_EQ(edit.geometry().next_id, counter_before + 10);
  EXPECT_EQ(edit.selection().history.size(), 1);
  EXPECT_FALSE(edit.selection().vertices.contains(1));
  EXPECT_EQ(*edit.selection().active, (ElementRef{ElementDomain::Edge, 3}));
  EXPECT_EQ(edit.revision(), revision_before_undo + 2);
  EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 0.0);
  ED_curve_editnurb_load(bmain, object);
  EXPECT_EQ(first_curve(*curve->nurbs_geometry).points[0].position[0], 4.0);
  EXPECT_EQ(curve->nurbs_geometry->next_id, counter_before + 10);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  ED_curve_editnurb_make(object);
  EXPECT_EQ(curve->editnurb->nurbs_edit, &edit);
  EXPECT_EQ(edit.selection().history.size(), 1);
}

TEST_F(NurbsNativeEditTest, GenericUndoStackResolvesIDsAndRestoresAdjacentEditSteps)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Scene *scene = BKE_scene_add(bmain, "NURBS undo stack scene");
  Object *object = BKE_object_add_only_object(bmain, OB_SURF, "NURBS undo stack object");
  Curve *curve = BKE_curve_add(bmain, "NURBS undo stack source", OB_SURF);
  object->data = &curve->id;
  curve->nurbs_geometry = make_line().release();
  ASSERT_TRUE(BKE_collection_object_add(bmain, scene->master_collection, object));
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  view_layer->basact = BKE_view_layer_base_find(view_layer, object);
  ASSERT_NE(view_layer->basact, nullptr);
  object->mode = OB_MODE_EDIT;
  std::string error;
  ASSERT_TRUE(curve_edit_begin(*curve, &error)) << error;
  EditData &edit = *curve->editnurb->nurbs_edit;
  EditSnapshot initial = edit.snapshot();
  initial.selection = make_selection();
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(initial), edit.revision(), &error))
      << error;
  const int64_t counter_before = edit.geometry().next_id;

  wmWindowManager *wm = static_cast<wmWindowManager *>(
      BKE_libblock_alloc(bmain, ID_WM, "NURBS test window manager", 0));
  wm->runtime = MEM_new<WindowManagerRuntime>(__func__);
  bContext *context = CTX_create();
  BLI_SCOPED_DEFER([&]() { CTX_free(context); });
  CTX_data_main_set(context, bmain);
  CTX_data_scene_set(context, scene);
  CTX_wm_manager_set(context, wm);
  ASSERT_EQ(BKE_UNDOSYS_TYPE_MEMFILE, nullptr);
  ED_undosys_type_init();
  wm->runtime->undo_stack = BKE_undosys_stack_create();
  BLI_SCOPED_DEFER([&]() {
    BKE_undosys_stack_destroy(wm->runtime->undo_stack);
    wm->runtime->undo_stack = nullptr;
    ED_undosys_type_free();
    /* Type shutdown is normally final application shutdown. This fixture must
     * also leave the exported type pointers clean for subsequent tests. */
    BKE_UNDOSYS_TYPE_IMAGE = nullptr;
    BKE_UNDOSYS_TYPE_MEMFILE = nullptr;
    BKE_UNDOSYS_TYPE_PAINTCURVE = nullptr;
    BKE_UNDOSYS_TYPE_PARTICLE = nullptr;
    BKE_UNDOSYS_TYPE_SCULPT = nullptr;
    BKE_UNDOSYS_TYPE_TEXT = nullptr;
  });
  UndoStack *stack = wm->runtime->undo_stack;
  /* Encode a real memfile root; do not fake Main's memfile-written state. */
  EXPECT_FALSE(bmain->is_memfile_undo_written);
  Nurb *foreign = MEM_new<Nurb>(__func__);
  BLI_addtail(&curve->editnurb->nurbs, foreign);
  curve->editnurb->needs_flush_to_id = true;
  bmain->is_memfile_undo_flush_needed = true;
  EXPECT_FALSE(BKE_undosys_step_push_with_type(
                   stack, context, "Rejected memfile root", BKE_UNDOSYS_TYPE_MEMFILE) &
               UNDO_PUSH_RET_SUCCESS);
  EXPECT_EQ(stack->step_active, nullptr);
  EXPECT_TRUE(stack->steps.is_empty());
  EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
  EXPECT_FALSE(BKE_undosys_step_push(stack, context, "Rejected edit without root") &
               UNDO_PUSH_RET_SUCCESS);
  EXPECT_EQ(stack->step_active, nullptr);
  EXPECT_TRUE(stack->steps.is_empty());
  EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_written);
  BLI_remlink(&curve->editnurb->nurbs, foreign);
  MEM_delete(foreign);
  BKE_undosys_stack_init_from_main(stack, bmain);
  ASSERT_NE(stack->step_active, nullptr);
  ASSERT_EQ(stack->step_active->type, BKE_UNDOSYS_TYPE_MEMFILE);
  EXPECT_TRUE(bmain->is_memfile_undo_written);
  ASSERT_TRUE(BKE_undosys_step_push(stack, context, "NURBS before") & UNDO_PUSH_RET_SUCCESS);
  const UndoType *edit_type = stack->step_active->type;
  ASSERT_NE(edit_type, BKE_UNDOSYS_TYPE_MEMFILE);
  EXPECT_STREQ(edit_type->name, "Edit Curve");

  EditSnapshot candidate = edit.snapshot();
  translate_fixture(candidate.geometry, 4.0);
  candidate.geometry->next_id += 10;
  candidate.selection.vertices.remove(1);
  candidate.selection.active = ElementRef{ElementDomain::Edge, 3};
  candidate.selection.history.clear();
  candidate.selection.history.append({ElementDomain::Edge, 3});
  ASSERT_TRUE(
      ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), edit.revision(), &error))
      << error;
  ASSERT_TRUE(BKE_undosys_step_push(stack, context, "NURBS after") & UNDO_PUSH_RET_SUCCESS);
  EXPECT_EQ(stack->step_active->type, edit_type);
  const uint64_t revision_before_undo = edit.revision();
  ASSERT_TRUE(BKE_undosys_step_undo(stack, context));
  EXPECT_STREQ(stack->step_active->name, "NURBS before");
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 0.0);
  EXPECT_EQ(edit.geometry().next_id, counter_before);
  EXPECT_TRUE(edit.selection().vertices.contains(1));
  EXPECT_EQ(edit.selection().history.size(), 3);
  EXPECT_EQ(*edit.selection().active, (ElementRef{ElementDomain::Vertex, 2}));
  EXPECT_EQ(edit.revision(), revision_before_undo + 1);
  EXPECT_TRUE(BKE_undosys_stack_has_redo(stack));
  ASSERT_TRUE(BKE_undosys_step_redo(stack, context));
  EXPECT_STREQ(stack->step_active->name, "NURBS after");
  EXPECT_EQ(first_curve(edit.geometry()).points[0].position[0], 4.0);
  EXPECT_EQ(edit.geometry().next_id, counter_before + 10);
  EXPECT_FALSE(edit.selection().vertices.contains(1));
  EXPECT_EQ(edit.selection().history.size(), 1);
  EXPECT_EQ(*edit.selection().active, (ElementRef{ElementDomain::Edge, 3}));
  EXPECT_EQ(edit.revision(), revision_before_undo + 2);
  EXPECT_FALSE(BKE_undosys_stack_has_redo(stack));
  EXPECT_EQ(CTX_data_main(context), bmain);
  /* This checks adjacent Curve edit steps and ID resolution. Decoding across
   * a different memfile generation, mode changes and UI events are separate gates. */
}

TEST_F(NurbsNativeEditTest, UndoCrossesEditModeAndMemfileGenerations)
{
  const auto user_ui_flags = U.uiflag;
  BLI_SCOPED_DEFER([&]() { U.uiflag = user_ui_flags; });
  U.uiflag |= USER_GLOBALUNDO;
  for (const bool full_barrier : {false, true}) {
    SCOPED_TRACE(full_barrier ? "full memfile barrier" : "reusable memfile IDs");
    Main *bmain = BKE_main_new();
    BKE_blender_globals_main_replace(bmain);
    BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
    Scene *scene = BKE_scene_add(bmain, "NURBS memfile scene");
    Object *object = BKE_object_add_only_object(bmain, OB_SURF, "NURBS memfile object");
    Curve *curve = BKE_curve_add(bmain, "NURBS memfile source", OB_SURF);
    object->data = &curve->id;
    OwnedGeometry line = make_line();
    auto construction = extrude_edges(*line, {3}, {0, 0, 2}, 0);
    ASSERT_TRUE(construction.has_value());
    const int64_t cap_edge = construction->source_to_cap.lookup(3);
    const int64_t face_id = construction->edge_to_face.lookup(3);
    curve->nurbs_geometry = construction->geometry.release();
    ASSERT_TRUE(BKE_collection_object_add(bmain, scene->master_collection, object));
    ViewLayer *view_layer = BKE_view_layer_default_view(scene);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    view_layer->basact = BKE_view_layer_base_find(view_layer, object);
    ASSERT_NE(view_layer->basact, nullptr);
    object->mode = OB_MODE_EDIT;
    ASSERT_TRUE(curve_edit_begin(*curve));
    EditData &edit = *curve->editnurb->nurbs_edit;
    EditSnapshot initial = edit.snapshot();
    initial.selection = make_selection();
    ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(initial), edit.revision()));
    const int64_t initial_counter = edit.geometry().next_id;

    wmWindowManager *wm = static_cast<wmWindowManager *>(
        BKE_libblock_alloc(bmain, ID_WM, "NURBS memfile manager", 0));
    wm->runtime = MEM_new<WindowManagerRuntime>(__func__);
    WorkSpace *workspace = BKE_workspace_add(bmain, "NURBS memfile workspace");
    bScreen *screen = static_cast<bScreen *>(
        BKE_libblock_alloc(bmain, ID_SCR, "NURBS memfile screen", 0));
    WorkSpaceLayout *layout = BKE_workspace_layout_add(bmain, *workspace, *screen, "NURBS layout");
    wmWindow *window = MEM_new<wmWindow>(__func__);
    window->winid = 1;
    window->active = true;
    window->runtime = MEM_new<WindowRuntime>(__func__);
    window->stereo3d_format = MEM_new<Stereo3dFormat>(__func__);
    window->workspace_hook = BKE_workspace_instance_hook_create(bmain, window->winid);
    BKE_workspace_active_set(window->workspace_hook, workspace);
    BKE_workspace_active_layout_set(window->workspace_hook, window->winid, workspace, layout);
    window->scene = scene;
    STRNCPY(window->view_layer_name, view_layer->name);
    BLI_addtail(&wm->windows, window);
    bContext *context = CTX_create();
    BLI_SCOPED_DEFER([&]() { CTX_free(context); });
    CTX_data_main_set(context, bmain);
    CTX_wm_manager_set(context, wm);
    CTX_wm_window_set(context, window);
    ASSERT_EQ(CTX_data_scene(context), scene);
    ASSERT_EQ(CTX_data_view_layer(context), view_layer);
    ASSERT_EQ(BKE_UNDOSYS_TYPE_MEMFILE, nullptr);
    ED_undosys_type_init();
    wm->runtime->undo_stack = BKE_undosys_stack_create();
    BLI_SCOPED_DEFER([&]() {
      wmWindowManager *current_wm = CTX_wm_manager(context);
      BKE_undosys_stack_destroy(current_wm->runtime->undo_stack);
      current_wm->runtime->undo_stack = nullptr;
      ED_undosys_type_free();
      BKE_UNDOSYS_TYPE_IMAGE = nullptr;
      BKE_UNDOSYS_TYPE_MEMFILE = nullptr;
      BKE_UNDOSYS_TYPE_PAINTCURVE = nullptr;
      BKE_UNDOSYS_TYPE_PARTICLE = nullptr;
      BKE_UNDOSYS_TYPE_SCULPT = nullptr;
      BKE_UNDOSYS_TYPE_TEXT = nullptr;
    });
    UndoStack *stack = wm->runtime->undo_stack;
    BKE_undosys_stack_init_from_main(stack, bmain);
    ASSERT_NE(stack->step_active, nullptr);
    ASSERT_TRUE(BKE_undosys_step_push(stack, context, "NURBS edit before") &
                UNDO_PUSH_RET_SUCCESS);
    EditSnapshot candidate = edit.snapshot();
    translate_fixture(candidate.geometry, 4.0);
    candidate.geometry->next_id += 10;
    candidate.selection.vertices.remove(1);
    candidate.selection.active = ElementRef{ElementDomain::Edge, 3};
    candidate.selection.history.clear();
    candidate.selection.history.append({ElementDomain::Edge, 3});
    ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), edit.revision()));
    ASSERT_TRUE(BKE_undosys_step_push(stack, context, "NURBS edit after") & UNDO_PUSH_RET_SUCCESS);
    ASSERT_TRUE(ed::object::editmode_exit_ex(bmain, scene, object, ed::object::EM_FREEDATA));
    ASSERT_TRUE(BKE_undosys_step_push_with_type(
                    stack, context, "NURBS object mode", BKE_UNDOSYS_TYPE_MEMFILE) &
                UNDO_PUSH_RET_SUCCESS);
    object->loc[0] = 7.0f;
    DEG_id_tag_update_ex(bmain, &object->id, ID_RECALC_TRANSFORM);
    bmain->use_memfile_full_barrier = full_barrier;
    ASSERT_TRUE(BKE_undosys_step_push_with_type(
                    stack, context, "NURBS object moved", BKE_UNDOSYS_TYPE_MEMFILE) &
                UNDO_PUSH_RET_SUCCESS);

    auto check_state = [&](const bool in_edit_mode, const bool after, const float object_x) {
      /* Memfile decode may replace Main and its IDs. Never inspect old pointers. */
      Main *current_main = CTX_data_main(context);
      Object *current_object = id_cast<Object *>(
          BKE_libblock_find_name(current_main, ID_OB, "NURBS memfile object"));
      ASSERT_NE(current_object, nullptr);
      Curve *current_curve = id_cast<Curve *>(current_object->data);
      ASSERT_NE(current_curve->nurbs_geometry, nullptr);
      EXPECT_EQ(current_object->loc[0], object_x);
      EXPECT_EQ(bool(current_object->mode & OB_MODE_EDIT), in_edit_mode);
      const NurbsGeometry *geometry;
      SelectionState selection;
      if (in_edit_mode) {
        ASSERT_NE(current_curve->editnurb, nullptr);
        ASSERT_NE(current_curve->editnurb->nurbs_edit, nullptr);
        geometry = &current_curve->editnurb->nurbs_edit->geometry();
        selection = current_curve->editnurb->nurbs_edit->selection();
      }
      else {
        EXPECT_EQ(current_curve->editnurb, nullptr);
        geometry = current_curve->nurbs_geometry;
        const auto stored = selection_from_storage(geometry->stored_selection);
        ASSERT_TRUE(stored.has_value());
        selection = *stored;
      }
      EXPECT_TRUE(geometry_validate_structure(*geometry));
      EXPECT_EQ(geometry->faces.count(), 1);
      EXPECT_EQ(geometry->vertices.count(), 4);
      EXPECT_EQ(geometry->edges.count(), 4);
      const auto index = TopologyIndex::build(*geometry);
      ASSERT_TRUE(index.has_value());
      EXPECT_NE(index->face(face_id), nullptr);
      EXPECT_EQ(index->uses_for_edge(3).size(), 1);
      EXPECT_EQ(index->uses_for_edge(cap_edge).size(), 1);
      EXPECT_EQ(first_curve(*geometry).points[0].position[0], after ? 4.0 : 0.0);
      EXPECT_EQ(geometry->next_id, initial_counter + (after ? 10 : 0));
      EXPECT_EQ(selection.vertices.contains(1), !after);
      EXPECT_EQ(selection.history.size(), after ? 1 : 3);
      ASSERT_TRUE(selection.active.has_value());
      EXPECT_EQ(*selection.active,
                after ? (ElementRef{ElementDomain::Edge, 3}) :
                        (ElementRef{ElementDomain::Vertex, 2}));
    };
    for (int cycle = 0; cycle < 2; cycle++) {
      SCOPED_TRACE(cycle);
      check_state(false, true, 7.0f);
      ASSERT_TRUE(BKE_undosys_step_undo(stack, context));
      EXPECT_STREQ(stack->step_active->name, "NURBS object mode");
      check_state(false, true, 0.0f);
      ASSERT_TRUE(BKE_undosys_step_undo(stack, context));
      EXPECT_STREQ(stack->step_active->name, "NURBS edit after");
      check_state(true, true, 0.0f);
      ASSERT_TRUE(BKE_undosys_step_undo(stack, context));
      EXPECT_STREQ(stack->step_active->name, "NURBS edit before");
      check_state(true, false, 0.0f);
      ASSERT_TRUE(BKE_undosys_step_redo(stack, context));
      check_state(true, true, 0.0f);
      ASSERT_TRUE(BKE_undosys_step_redo(stack, context));
      check_state(false, true, 0.0f);
      ASSERT_TRUE(BKE_undosys_step_redo(stack, context));
      check_state(false, true, 7.0f);
    }
  }
}

TEST_F(NurbsNativeEditTest, SeparateSelectionPreparesIndependentSourcesAcrossComponentModes)
{
  for (const SelectionMode mode :
       {SelectionMode::Vertex, SelectionMode::Edge, SelectionMode::Face})
  {
    SCOPED_TRACE(int(mode));
    auto sheet = make_planar_sheet();
    ASSERT_NE(sheet, nullptr);
    auto adjacent = extrude_edges(
        *sheet, {static_cast<NurbsTopologyEdge *>(sheet->edges.first)->id}, {0, 0, 2}, 0);
    ASSERT_TRUE(adjacent.has_value());
    const auto &geometry = *adjacent->geometry;
    const ElementRef hit =
        mode == SelectionMode::Face ?
            ElementRef{ElementDomain::Face,
                       static_cast<const NurbsTopologyFace *>(geometry.faces.first)->id} :
        mode == SelectionMode::Edge ?
            ElementRef{ElementDomain::Edge,
                       static_cast<const NurbsTopologyEdge *>(geometry.edges.first)->id} :
            ElementRef{ElementDomain::Vertex,
                       static_cast<const NurbsTopologyVertex *>(geometry.vertices.first)->id};
    SelectionState input;
    input.mode = mode;
    const auto selection = selection_apply(geometry, input, {&hit, 1}, SelectionAction::Set, hit);
    ASSERT_TRUE(selection.has_value());
    auto edit = EditData::create(geometry, *selection);
    ASSERT_NE(edit, nullptr);
    const auto before = edit->read_snapshot();
    std::string error;
    auto extracted = extract_edit_selection(before, &error);
    ASSERT_TRUE(extracted.has_value()) << error;
    EXPECT_EQ(edit->read_snapshot(), before);
    EXPECT_EQ(before.geometry().vertices.count(), 6);
    EXPECT_EQ(before.geometry().edges.count(), 7);
    EXPECT_EQ(before.geometry().faces.count(), 2);
    EXPECT_TRUE(extracted->remainder.selection.vertices.is_empty());
    EXPECT_TRUE(extracted->remainder.selection.edges.is_empty());
    EXPECT_TRUE(extracted->remainder.selection.faces.is_empty());
    EXPECT_FALSE(extracted->remainder.selection.active.has_value());
    EXPECT_TRUE(extracted->remainder.selection.history.is_empty());
    EXPECT_EQ(extracted->remainder.selection.mode, mode);
    EXPECT_EQ(extracted->extracted.selection.mode, mode);
    EXPECT_EQ(extracted->extracted.selection.active,
              (ElementRef{hit.domain, extracted->source_to_copy.lookup(hit.id)}));
    ASSERT_EQ(extracted->extracted.selection.history.size(), 1);
    EXPECT_EQ(extracted->extracted.selection.history[0], *extracted->extracted.selection.active);
    EXPECT_EQ(extracted->remainder.geometry->faces.count(), mode == SelectionMode::Face ? 1 : 2);
    EXPECT_EQ(extracted->extracted.geometry->vertices.count(),
              mode == SelectionMode::Face ? 4 :
              mode == SelectionMode::Edge ? 2 :
                                            1);
    EXPECT_EQ(extracted->extracted.geometry->edges.count(),
              mode == SelectionMode::Face ? 4 :
              mode == SelectionMode::Edge ? 1 :
                                            0);
    EXPECT_EQ(extracted->extracted.geometry->faces.count(), mode == SelectionMode::Face ? 1 : 0);
    auto persistent = geometry_from_edit_snapshot(extracted->extracted, &error);
    ASSERT_NE(persistent, nullptr) << error;
    const auto stored = selection_from_storage(persistent->stored_selection);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->active, extracted->extracted.selection.active);
    EXPECT_EQ(stored->mode, mode);
    const auto reopened = EditData::create(*persistent, *stored, &error);
    ASSERT_NE(reopened, nullptr) << error;
    expect_source_geometry_equal(reopened->geometry(), *extracted->extracted.geometry);
    ASSERT_TRUE(edit->commit(std::move(extracted->remainder), before, &error)) << error;
    const auto remainder = edit->read_snapshot();
    const Vector<int64_t> moving(stored->vertices.begin(), stored->vertices.end());
    auto moved = translate_vertices(reopened->geometry(), moving, {0, 0, 3}, 0, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    EXPECT_EQ(edit->read_snapshot(), remainder);
    EXPECT_EQ(before.geometry().faces.count(), 2);
  }
}

TEST_F(NurbsNativeEditTest, SeparatePreservesRationalSeamAndTrimHoleSourcesExactly)
{
  for (const bool circle : {false, true}) {
    auto geometry = circle ? make_arc_sheet(true) : make_planar_sheet();
    ASSERT_NE(geometry, nullptr);
    if (!circle) {
      const double2 hole[] = {{0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.25}};
      add_planar_loop(
          *geometry, *static_cast<NurbsTopologyFace *>(geometry->faces.first), {hole, 4}, true);
    }
    SelectionState input;
    input.mode = SelectionMode::Face;
    const ElementRef hit{ElementDomain::Face,
                         static_cast<const NurbsTopologyFace *>(geometry->faces.first)->id};
    auto selection = selection_apply(*geometry, input, {&hit, 1}, SelectionAction::Set, hit);
    ASSERT_TRUE(selection.has_value());
    auto edit = EditData::create(*geometry, *selection);
    ASSERT_NE(edit, nullptr);
    const auto before = edit->read_snapshot();
    std::string error;
    auto result = extract_edit_selection(before, &error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(result->remainder.geometry->vertices.count(), 0);
    EXPECT_EQ(result->remainder.geometry->edges.count(), 0);
    EXPECT_EQ(result->remainder.geometry->faces.count(), 0);
    const auto index = TopologyIndex::build(*result->extracted.geometry);
    ASSERT_TRUE(index.has_value());
    for (const NurbsCurveGeometry &curve : geometry->curves) {
      const auto *copy = static_cast<const NurbsCurveGeometry *>(
          result->extracted.geometry->curves.first);
      while (copy && copy->id != result->source_to_copy.lookup(curve.id)) {
        copy = copy->next;
      }
      ASSERT_NE(copy, nullptr);
      EXPECT_EQ(copy->order, curve.order);
      ASSERT_EQ(copy->points_num, curve.points_num);
      ASSERT_EQ(copy->knots_num, curve.knots_num);
      EXPECT_NE(copy->points, curve.points);
      EXPECT_NE(copy->knots, curve.knots);
      EXPECT_EQ(
          std::memcmp(copy->points, curve.points, sizeof(NurbsControlPoint) * curve.points_num),
          0);
      EXPECT_EQ(std::memcmp(copy->knots, curve.knots, sizeof(double) * curve.knots_num), 0);
    }
    const auto &surface = *static_cast<const NurbsSurfaceGeometry *>(geometry->surfaces.first);
    const auto *copy = static_cast<const NurbsSurfaceGeometry *>(
        result->extracted.geometry->surfaces.first);
    ASSERT_NE(copy, nullptr);
    ASSERT_EQ(copy->points_num, surface.points_num);
    EXPECT_NE(copy->points, surface.points);
    EXPECT_EQ(
        std::memcmp(copy->points, surface.points, sizeof(NurbsControlPoint) * surface.points_num),
        0);
    const auto &face = *static_cast<const NurbsTopologyFace *>(
        result->extracted.geometry->faces.first);
    const auto &original_face = *static_cast<const NurbsTopologyFace *>(geometry->faces.first);
    EXPECT_EQ(face.loops.count(), original_face.loops.count());
    EXPECT_EQ(face.orientation, original_face.orientation);
    EXPECT_EQ(face.material_index, original_face.material_index);
    if (circle) {
      EXPECT_EQ(result->extracted.geometry->vertices.count(), 2);
      EXPECT_EQ(result->extracted.geometry->edges.count(), 3);
      EXPECT_EQ(static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses.count(), 4);
    }
    EXPECT_EQ(edit->read_snapshot(), before);
  }
}

TEST_F(NurbsNativeEditTest, SeparateRejectsInvalidOrExhaustedSelectionWithoutSourceChanges)
{
  auto line = make_line();
  auto empty = EditData::create(*line);
  ASSERT_NE(empty, nullptr);
  const auto before = empty->read_snapshot();
  std::string error = "stale diagnostic";
  EXPECT_FALSE(extract_edit_selection(before, &error).has_value());
  EXPECT_TRUE(error.empty());
  auto invalid = before.copy_for_edit();
  invalid.selection.vertices.add(99999);
  EXPECT_EQ(geometry_from_edit_snapshot(invalid, &error), nullptr);
  EXPECT_FALSE(error.empty());
  line->next_id = std::numeric_limits<int64_t>::max();
  auto exhausted = EditData::create(*line, make_selection());
  ASSERT_NE(exhausted, nullptr);
  const auto exhausted_before = exhausted->read_snapshot();
  EXPECT_FALSE(extract_edit_selection(exhausted_before, &error).has_value());
  EXPECT_NE(error.find("exhausted"), std::string::npos);
  EXPECT_EQ(exhausted->read_snapshot(), exhausted_before);
  EXPECT_EQ(empty->read_snapshot(), before);
}

TEST_F(NurbsNativeEditTest, ViewportSeparateSourcesMatchExactExtractionAndMesh)
{
  const char *directory = std::getenv("BLENDER_NURBS_SEPARATE_OUTPUT");
  if (!directory) {
    GTEST_SKIP() << "Run after the NURBS Separate viewport workflow";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const std::string &label, const char *suffix) {
    const std::string path = std::string(directory) + "/separate-" + label + "-" + suffix +
                             ".blend";
    return LoadedFile(BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  using Point = std::array<double, 3>;
  struct Signature {
    std::vector<Point> vertices;
    std::vector<std::vector<Point>> edges, faces;
    void sort()
    {
      std::sort(vertices.begin(), vertices.end());
      for (auto *domain : {&edges, &faces}) {
        for (auto &element : *domain) {
          std::sort(element.begin(), element.end());
        }
        std::sort(domain->begin(), domain->end());
      }
    }
  };
  auto native_signature = [](const NurbsGeometry &geometry) {
    Signature signature;
    const auto index = TopologyIndex::build(geometry);
    auto point = [&](const int64_t id) {
      const auto &v = *index->vertex(id);
      return Point{v.position[0], v.position[1], v.position[2]};
    };
    for (const auto &v : geometry.vertices) {
      signature.vertices.push_back(point(v.id));
    }
    for (const auto &e : geometry.edges) {
      signature.edges.push_back({point(e.vertex_ids[0]), point(e.vertex_ids[1])});
    }
    for (const auto &face : geometry.faces) {
      std::vector<Point> boundary;
      for (const auto &loop : face.loops) {
        for (const auto &use : loop.edge_uses) {
          const auto &edge = *index->edge(use.edge_id);
          boundary.push_back(point(edge.vertex_ids[use.reversed ? 1 : 0]));
        }
      }
      signature.faces.push_back(std::move(boundary));
    }
    signature.sort();
    return signature;
  };
  auto mesh_signature = [](const Mesh &mesh) {
    Signature signature;
    const auto positions = mesh.vert_positions();
    auto point = [&](const int v) {
      return Point{positions[v].x, positions[v].y, positions[v].z};
    };
    for (const int v : positions.index_range()) {
      signature.vertices.push_back(point(v));
    }
    for (const int2 edge : mesh.edges()) {
      signature.edges.push_back({point(edge.x), point(edge.y)});
    }
    const auto corners = mesh.corner_verts();
    const auto faces = mesh.faces();
    for (const int face_index : faces.index_range()) {
      const IndexRange face = faces[face_index];
      std::vector<Point> boundary;
      for (const int corner : face) {
        boundary.push_back(point(corners[corner]));
      }
      signature.faces.push_back(std::move(boundary));
    }
    signature.sort();
    return signature;
  };
  for (const char *label : {"VERT", "EDGE", "FACE", "BOX", "REGION", "TAGGED"}) {
    SCOPED_TRACE(label);
    const auto before = load(label, "before");
    const auto after = load(label, "after");
    const auto mesh_file = load(std::string("MESH-") + label, "after");
    ASSERT_NE(before, nullptr);
    ASSERT_NE(after, nullptr);
    ASSERT_NE(mesh_file, nullptr);
    ASSERT_EQ(before->main->objects.count(), 1);
    ASSERT_EQ(after->main->objects.count(), 2);
    const auto &original = *static_cast<const Object *>(before->main->objects.first);
    const auto &curve = *id_cast<const Curve *>(original.data);
    std::string error;
    std::unique_ptr<EditData> edit;
    if (curve.nurbs_geometry) {
      auto selection = selection_from_storage(curve.nurbs_geometry->stored_selection, &error);
      ASSERT_TRUE(selection.has_value()) << error;
      edit = EditData::create(*curve.nurbs_geometry, *selection, &error);
    }
    else {
      auto source = legacy_topology_import_edit(curve.nurb, &error);
      ASSERT_TRUE(source.has_value()) << error;
      edit = EditData::create(*source->geometry, source->selection, &error);
    }
    ASSERT_NE(edit, nullptr) << error;
    const auto expected = extract_edit_selection(edit->read_snapshot(), &error);
    ASSERT_TRUE(expected.has_value()) << error;
    for (const Object &object : after->main->objects) {
      const bool remainder = STREQ(object.id.name, original.id.name);
      const auto &expected_source = remainder ? expected->remainder : expected->extracted;
      const auto &actual_curve = *id_cast<const Curve *>(object.data);
      ASSERT_TRUE(actual_curve.nurb.is_empty());
      ASSERT_NE(actual_curve.nurbs_geometry, nullptr);
      expect_source_geometry_equal(*actual_curve.nurbs_geometry, *expected_source.geometry);
      const auto selection = selection_from_storage(actual_curve.nurbs_geometry->stored_selection);
      ASSERT_TRUE(selection.has_value());
      EXPECT_EQ(selection->mode, expected_source.selection.mode);
      EXPECT_EQ(selection->vertices, expected_source.selection.vertices);
      EXPECT_EQ(selection->edges, expected_source.selection.edges);
      EXPECT_EQ(selection->faces, expected_source.selection.faces);
      EXPECT_EQ(selection->active, expected_source.selection.active);
      EXPECT_EQ(selection->history.as_span(), expected_source.selection.history.as_span());
      const Object *reference = nullptr;
      for (const Object &candidate : mesh_file->main->objects) {
        if (STREQ(candidate.id.name,
                  remainder ? "OBSeparate mesh reference" : "OBSeparate mesh reference.001"))
        {
          reference = &candidate;
        }
      }
      ASSERT_NE(reference, nullptr);
      const Signature native = native_signature(*actual_curve.nurbs_geometry);
      const Signature mesh = mesh_signature(*id_cast<const Mesh *>(reference->data));
      EXPECT_EQ(native.vertices, mesh.vertices);
      EXPECT_EQ(native.edges, mesh.edges);
      EXPECT_EQ(native.faces, mesh.faces);
    }
  }
}

TEST_F(NurbsNativeEditTest, SplitCommitsExactSourceAndRemapsSelectionAcrossComponentModes)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  for (const SelectionMode mode :
       {SelectionMode::Vertex, SelectionMode::Edge, SelectionMode::Face})
  {
    const auto sheet = make_planar_sheet();
    ASSERT_NE(sheet, nullptr);
    auto adjacent = extrude_edges(
        *sheet, {static_cast<NurbsTopologyEdge *>(sheet->edges.first)->id}, {0, 0, 2}, 0);
    ASSERT_TRUE(adjacent.has_value());
    Curve *curve = BKE_curve_add(bmain, "NURBS split editing", OB_SURF);
    curve->nurbs_geometry = adjacent->geometry.release();
    const auto &geometry = *curve->nurbs_geometry;
    const ElementRef hit =
        mode == SelectionMode::Face ?
            ElementRef{ElementDomain::Face,
                       static_cast<NurbsTopologyFace *>(geometry.faces.first)->id} :
        mode == SelectionMode::Edge ?
            ElementRef{ElementDomain::Edge,
                       static_cast<NurbsTopologyEdge *>(geometry.edges.first)->id} :
            ElementRef{ElementDomain::Vertex,
                       static_cast<NurbsTopologyVertex *>(geometry.vertices.first)->id};
    SelectionState input;
    input.mode = mode;
    const auto selection = selection_apply(geometry, input, {&hit, 1}, SelectionAction::Set, hit);
    ASSERT_TRUE(selection.has_value());
    curve->nurbs_geometry->stored_selection = selection_to_storage(*selection);
    ASSERT_TRUE(curve_edit_begin(*curve));
    auto &edit = *curve->editnurb->nurbs_edit;
    const auto before = edit.read_snapshot();
    const Vector<int64_t> vertices(selection->vertices.begin(), selection->vertices.end());
    const Vector<int64_t> edges(selection->edges.begin(), selection->edges.end());
    const Vector<int64_t> faces(selection->faces.begin(), selection->faces.end());
    const auto expected = split_components(before.geometry(), {vertices, edges, faces});
    ASSERT_TRUE(expected.has_value());
    std::string error;
    ASSERT_TRUE(ed::nurbs::split(*bmain, *curve, &error)) << error;
    EXPECT_EQ(edit.revision(), before.revision() + 1);
    expect_source_geometry_equal(edit.geometry(), *expected->geometry);
    EXPECT_EQ(edit.selection().mode, mode);
    ASSERT_TRUE(edit.selection().active.has_value());
    EXPECT_EQ(edit.selection().active->id, expected->source_to_copy.lookup(hit.id));
    ASSERT_EQ(edit.selection().history.size(), selection->history.size());
    for (const int i : selection->history.index_range()) {
      EXPECT_EQ(edit.selection().history[i].id,
                expected->source_to_copy.lookup(selection->history[i].id));
    }
    EXPECT_TRUE(curve->editnurb->needs_flush_to_id);
    EXPECT_TRUE(bmain->is_memfile_undo_flush_needed);
    EXPECT_EQ(before.geometry().vertices.count(), 6);
    EXPECT_EQ(before.geometry().faces.count(), 2);
    EXPECT_TRUE(curve->editnurb->nurbs.is_empty());

    // The detached selection can move through the existing source transform service.
    const Vector<int64_t> detached(edit.selection().vertices.begin(),
                                   edit.selection().vertices.end());
    const auto moved = translate_vertices(edit.geometry(), detached, {0, 0, 0.5}, 0, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    const auto original_index = TopologyIndex::build(before.geometry());
    for (const NurbsTopologyVertex &vertex : moved->geometry->vertices) {
      if (const auto *original = original_index->vertex(vertex.id)) {
        EXPECT_EQ(double3(vertex.position), double3(original->position));
      }
    }
    EXPECT_TRUE(curve_edit_flush_to_id(*curve, &error)) << error;
    EXPECT_TRUE(geometry_validate_structure(*curve->nurbs_geometry, &error)) << error;
  }
}

TEST_F(NurbsNativeEditTest, FailedSplitLeavesSourceSelectionRevisionAndDirtyFlagsUnchanged)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Curve *curve = BKE_curve_add(bmain, "NURBS exhausted split", OB_SURF);
  curve->nurbs_geometry = make_line().release();
  curve->nurbs_geometry->next_id = std::numeric_limits<int64_t>::max();
  curve->nurbs_geometry->stored_selection = selection_to_storage(make_selection());
  ASSERT_TRUE(curve_edit_begin(*curve));
  const auto before = curve->editnurb->nurbs_edit->read_snapshot();
  curve->editnurb->needs_flush_to_id = false;
  bmain->is_memfile_undo_flush_needed = false;
  std::string error;
  EXPECT_FALSE(ed::nurbs::split(*bmain, *curve, &error));
  EXPECT_NE(error.find("exhausted"), std::string::npos);
  EXPECT_EQ(curve->editnurb->nurbs_edit->read_snapshot(), before);
  EXPECT_FALSE(curve->editnurb->needs_flush_to_id);
  EXPECT_FALSE(bmain->is_memfile_undo_flush_needed);
}

TEST_F(NurbsNativeEditTest, ViewportSplitFilesPreserveSourceGeometryAndSelection)
{
  const char *directory = std::getenv("BLENDER_NURBS_SPLIT_OUTPUT");
  if (directory == nullptr) {
    GTEST_SKIP() << "Run after the NURBS split viewport workflow";
  }
  BKE_blender_globals_main_replace(BKE_main_new());
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  using LoadedFile = std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)>;
  auto load = [&](const char *label, const char *suffix) {
    const std::string path = std::string(directory) + "/split-" + label + "-" + suffix + ".blend";
    return LoadedFile(BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports),
                      BLO_blendfiledata_free);
  };
  auto find = [](const BlendFileData &file) -> const NurbsGeometry * {
    for (const Curve &curve : file.main->curves) {
      if (curve.nurbs_geometry && curve.nurb.is_empty()) {
        return curve.nurbs_geometry;
      }
    }
    return nullptr;
  };
  for (const char *label : {"VERT", "EDGE", "FACE", "BOX", "REGION"}) {
    SCOPED_TRACE(label);
    const auto before_file = load(label, "before");
    const auto after_file = load(label, "after");
    ASSERT_NE(before_file, nullptr);
    ASSERT_NE(after_file, nullptr);
    const auto *before = find(*before_file);
    const auto *after = find(*after_file);
    ASSERT_NE(before, nullptr);
    ASSERT_NE(after, nullptr);
    const auto selection = selection_from_storage(before->stored_selection);
    ASSERT_TRUE(selection.has_value());
    const Vector<int64_t> vertices(selection->vertices.begin(), selection->vertices.end());
    const Vector<int64_t> edges(selection->edges.begin(), selection->edges.end());
    const Vector<int64_t> faces(selection->faces.begin(), selection->faces.end());
    std::string error;
    const auto split = split_components(*before, {vertices, edges, faces}, &error);
    ASSERT_TRUE(split.has_value()) << error;
    const auto selected = selection_remap_closure(
        *before, *split->geometry, *selection, split->source_to_copy, selection->mode, &error);
    ASSERT_TRUE(selected.has_value()) << error;
    const Vector<int64_t> detached(selected->vertices.begin(), selected->vertices.end());
    const auto moved = translate_vertices(*split->geometry, detached, {0, 0, 0.5}, 0, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    expect_source_geometry_equal(*after, *moved->geometry);
    const auto saved_selection = selection_from_storage(after->stored_selection);
    ASSERT_TRUE(saved_selection.has_value());
    EXPECT_EQ(saved_selection->vertices, selected->vertices);
    EXPECT_EQ(saved_selection->edges, selected->edges);
    EXPECT_EQ(saved_selection->faces, selected->faces);
    EXPECT_EQ(saved_selection->active, selected->active);
    EXPECT_EQ(saved_selection->history.as_span(), selected->history.as_span());
  }
}

}  // namespace blender::bke::nurbs::tests
