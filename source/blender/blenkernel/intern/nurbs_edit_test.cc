/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <atomic>
#include <cstdlib>
#include <limits>
#include <thread>

#include "BLI_listbase.h"
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
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_evaluate.hh"
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
  for (const char *name : {"click.blend", "box.blend", "try-native-selection.blend"}) {
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
    EXPECT_EQ(selection->vertices.size(), 2);
    EXPECT_TRUE(selection->vertices.contains(selected_edge->vertex_ids[0]));
    EXPECT_TRUE(selection->vertices.contains(selected_edge->vertex_ids[1]));
    EXPECT_EQ(selection->edges.size(), 1);
    EXPECT_TRUE(selection->edges.contains(selected_edge->id));
    EXPECT_TRUE(selection->faces.is_empty());
    if (STREQ(name, "click.blend")) {
      EXPECT_EQ(selection->active,
                (ElementRef{ElementDomain::Vertex, selected_edge->vertex_ids[1]}));
      EXPECT_EQ(selection->history.size(), 2);
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

}  // namespace blender::bke::nurbs::tests
