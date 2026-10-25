/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cstring>
#include <limits>

#include "BLI_memory_utils.hh"

#include "BKE_gtest_base.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_topology.hh"

#include "bmesh.hh"

#include "nurbs_test_geometry.hh"
#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

class NurbsSelectionTest : public BlenderGTestBase {};

template<typename T> static T &first_item(const ListBaseT<T> &list)
{
  return *static_cast<T *>(list.first);
}

/** Behavior reference only: no BMesh is used by the native selection service. */
struct MeshReference {
  std::unique_ptr<BMesh, decltype(&BM_mesh_free)> mesh{nullptr, BM_mesh_free};
  Map<int64_t, BMVert *> vertices;
  Map<int64_t, BMEdge *> edges;
  Map<int64_t, BMFace *> faces;

  explicit MeshReference(const NurbsGeometry &source)
  {
    const BMeshCreateParams params{.use_toolflags = true};
    mesh.reset(BM_mesh_create(&bm_mesh_allocsize_default, &params));
    for (const NurbsTopologyVertex &vertex : source.vertices) {
      const float3 position(double3(vertex.position));
      vertices.add_new(vertex.id, BM_vert_create(mesh.get(), position, nullptr, BM_CREATE_NOP));
    }
    for (const NurbsTopologyEdge &edge : source.edges) {
      edges.add_new(edge.id,
                    BM_edge_create(mesh.get(),
                                   vertices.lookup(edge.vertex_ids[0]),
                                   vertices.lookup(edge.vertex_ids[1]),
                                   nullptr,
                                   BM_CREATE_NOP));
    }
    const auto index = TopologyIndex::build(source);
    for (const NurbsTopologyFace &face : source.faces) {
      BLI_assert(face.loops.is_single());
      Vector<BMVert *> face_vertices;
      Vector<BMEdge *> face_edges;
      for (const NurbsTopologyEdgeUse &use : first_item(face.loops).edge_uses) {
        const auto &edge = *index->edge(use.edge_id);
        face_vertices.append(vertices.lookup(edge.vertex_ids[use.reversed ? 1 : 0]));
        face_edges.append(edges.lookup(edge.id));
      }
      BMFace *created = BM_face_create(mesh.get(),
                                       face_vertices.data(),
                                       face_edges.data(),
                                       face_vertices.size(),
                                       nullptr,
                                       BM_CREATE_NOP);
      if (face.orientation < 0) {
        BM_face_normal_flip(mesh.get(), created);
      }
      faces.add_new(face.id, created);
    }
  }

  SelectionState flush(const SelectionState &input, const SelectionMode mode)
  {
    for (const auto item : vertices.items()) {
      BM_elem_flag_set(item.value, BM_ELEM_SELECT, input.vertices.contains(item.key));
    }
    for (const auto item : edges.items()) {
      BM_elem_flag_set(item.value, BM_ELEM_SELECT, input.edges.contains(item.key));
    }
    for (const auto item : faces.items()) {
      BM_elem_flag_set(item.value, BM_ELEM_SELECT, input.faces.contains(item.key));
    }
    mesh->totvertsel = input.vertices.size();
    mesh->totedgesel = input.edges.size();
    mesh->totfacesel = input.faces.size();
    /* The default BMesh flush is upward only. Down explicitly reconciles bulk flag changes. */
    BM_mesh_select_mode_flush_ex(
        mesh.get(), short(mode), BMSelectFlushFlag_Default | BMSelectFlushFlag::Down);
    SelectionState result;
    for (const auto item : vertices.items()) {
      if (BM_elem_flag_test(item.value, BM_ELEM_SELECT)) {
        result.vertices.add(item.key);
      }
    }
    for (const auto item : edges.items()) {
      if (BM_elem_flag_test(item.value, BM_ELEM_SELECT)) {
        result.edges.add(item.key);
      }
    }
    for (const auto item : faces.items()) {
      if (BM_elem_flag_test(item.value, BM_ELEM_SELECT)) {
        result.faces.add(item.key);
      }
    }
    return result;
  }
};

static SelectionState mask_selection(const NurbsGeometry &source, const uint64_t mask)
{
  SelectionState result;
  int bit = 0;
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    if (mask & (uint64_t(1) << bit++)) {
      result.vertices.add(vertex.id);
    }
  }
  for (const NurbsTopologyEdge &edge : source.edges) {
    if (mask & (uint64_t(1) << bit++)) {
      result.edges.add(edge.id);
    }
  }
  for (const NurbsTopologyFace &face : source.faces) {
    if (mask & (uint64_t(1) << bit++)) {
      result.faces.add(face.id);
    }
  }
  return result;
}

static void expect_selection_equal(const SelectionState &a, const SelectionState &b)
{
  EXPECT_EQ(a.vertices, b.vertices);
  EXPECT_EQ(a.edges, b.edges);
  EXPECT_EQ(a.faces, b.faces);
}

static void compare_modes(const NurbsGeometry &source,
                          MeshReference &reference,
                          const SelectionState &selection)
{
  for (int mode = 1; mode <= 7; mode++) {
    SCOPED_TRACE(mode);
    std::string error;
    const auto result = selection_flush(source, selection, SelectionMode(mode), &error);
    ASSERT_TRUE(result.has_value()) << error;
    expect_selection_equal(*result, reference.flush(selection, SelectionMode(mode)));
    const auto repeated = selection_flush(source, *result, SelectionMode(mode));
    ASSERT_TRUE(repeated.has_value());
    expect_selection_equal(*result, *repeated);
  }
}

TEST_F(NurbsSelectionTest, EveryQuadSelectionMatchesBMeshInAllSevenModeCombinations)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  MeshReference reference(*source);
  for (uint64_t mask = 0; mask < 512; mask++) {
    SCOPED_TRACE(mask);
    compare_modes(*source, reference, mask_selection(*source, mask));
  }
}

TEST_F(NurbsSelectionTest, SharedFaceIncidenceAndIsolatedVertexMatchBMesh)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto adjacent = extrude_edges(*sheet, {first_item(sheet->edges).id}, {0, 0, 2}, 0);
  ASSERT_TRUE(adjacent.has_value());
  add_vertex(*adjacent->geometry, {10, 10, 10});
  MeshReference reference(*adjacent->geometry);
  uint64_t state = 17;
  for (int i = 0; i < 256; i++) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    SCOPED_TRACE(state);
    compare_modes(*adjacent->geometry, reference, mask_selection(*adjacent->geometry, state));
  }
  compare_modes(*adjacent->geometry, reference, mask_selection(*adjacent->geometry, 0));
  compare_modes(*adjacent->geometry, reference, mask_selection(*adjacent->geometry, UINT64_MAX));
}

TEST_F(NurbsSelectionTest, HoleBoundariesParticipateWithoutSelectingSupportCoefficients)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto &face = first_item(source->faces);
  const double2 hole[] = {{0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}};
  const auto *inner = add_planar_loop(*source, face, {hole, 4}, true);
  const auto index = TopologyIndex::build(*source);
  ASSERT_TRUE(index.has_value());
  SelectionState selected_face;
  selected_face.faces.add(face.id);
  const auto down = selection_flush(*source, selected_face, SelectionMode::Face);
  ASSERT_TRUE(down.has_value());
  EXPECT_EQ(down->vertices.size(), 8);
  EXPECT_EQ(down->edges.size(), 8);
  EXPECT_EQ(down->faces.size(), 1);
  SelectionState outer_vertices = *down;
  for (const NurbsTopologyEdgeUse &use : inner->edge_uses) {
    const auto *edge = index->edge(use.edge_id);
    outer_vertices.vertices.remove(edge->vertex_ids[0]);
    outer_vertices.vertices.remove(edge->vertex_ids[1]);
  }
  const auto up = selection_flush(*source, outer_vertices, SelectionMode::Vertex);
  ASSERT_TRUE(up.has_value());
  EXPECT_EQ(up->vertices.size(), 4);
  EXPECT_EQ(up->edges.size(), 4);
  EXPECT_TRUE(up->faces.is_empty());
  EXPECT_EQ(first_item(source->surfaces).points_num, 4);
}

TEST_F(NurbsSelectionTest, ClosedRationalSeamUsesSharedSourceComponentsOnce)
{
  const auto source = make_arc_sheet(true);
  ASSERT_NE(source, nullptr);
  SelectionState selection;
  selection.faces.add(first_item(source->faces).id);
  const auto result = selection_flush(*source, selection, SelectionMode::Face);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->vertices.size(), 2);
  EXPECT_EQ(result->edges.size(), 3);
  EXPECT_EQ(result->faces.size(), 1);
  const auto from_vertices = selection_flush(*source, *result, SelectionMode::Vertex);
  ASSERT_TRUE(from_vertices.has_value());
  expect_selection_equal(*result, *from_vertices);
  EXPECT_EQ(first_item(source->surfaces).points_num, 18);
}

TEST_F(NurbsSelectionTest, NonManifoldIncidenceDoesNotSelectUnrequestedFaces)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  const int64_t shared = first_item(sheet->edges).id;
  auto second = extrude_edges(*sheet, {shared}, {0, 0, 2}, 0);
  ASSERT_TRUE(second.has_value());
  auto third = extrude_edges(*second->geometry, {shared}, {0, 0, -2}, 0);
  ASSERT_TRUE(third.has_value());
  const auto index = TopologyIndex::build(*third->geometry);
  ASSERT_TRUE(index.has_value());
  ASSERT_EQ(index->uses_for_edge(shared).size(), 3);
  SelectionState selected;
  selected.faces.add(static_cast<const NurbsTopologyFace *>(third->geometry->faces.last)->id);
  const auto result = selection_flush(*third->geometry, selected, SelectionMode::Face);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->faces.size(), 1);
  EXPECT_EQ(result->edges.size(), 4);
  EXPECT_EQ(result->vertices.size(), 4);
  EXPECT_TRUE(result->edges.contains(shared));
}

TEST_F(NurbsSelectionTest, HistoryAndActiveRetainSurvivingOrderWithoutInferredEntries)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  SelectionState selection;
  const int64_t vertex = first_item(source->vertices).id, edge = first_item(source->edges).id;
  selection.vertices.add(vertex);
  selection.edges.add(edge);
  selection.history.append({ElementDomain::Vertex, vertex});
  selection.history.append({ElementDomain::Edge, edge});
  selection.active = ElementRef{ElementDomain::Edge, edge};
  const auto result = selection_flush(*source, selection, SelectionMode::Vertex);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->history.size(), 1);
  EXPECT_EQ(result->history[0], selection.history[0]);
  EXPECT_FALSE(result->active.has_value());
  const auto down = selection_flush(*source, selection, SelectionMode::Edge);
  ASSERT_TRUE(down.has_value());
  EXPECT_EQ(down->history.size(), 2);
  EXPECT_EQ(down->active, selection.active);
  EXPECT_EQ(down->vertices.size(), 2);
}

TEST_F(NurbsSelectionTest, SharedClosureDoesNotDependOnFaceStorageOrder)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto adjacent = extrude_edges(*sheet, {first_item(sheet->edges).id}, {0, 0, 2}, 0);
  ASSERT_TRUE(adjacent.has_value());
  auto &source = *adjacent->geometry;
  SelectionState selection;
  selection.faces.add(first_item(source.faces).id);
  const auto before = selection_flush(source, selection, SelectionMode::Face);
  ASSERT_TRUE(before.has_value());
  auto *face = &first_item(source.faces);
  BLI_remlink(&source.faces, face);
  BLI_addtail(&source.faces, face);
  const auto after = selection_flush(source, selection, SelectionMode::Face);
  ASSERT_TRUE(after.has_value());
  expect_selection_equal(*before, *after);
}

TEST_F(NurbsSelectionTest, InvalidSourceReferencesModesAndHistoryFailWithoutMutation)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t counter = source->next_id;
  SelectionState selection;
  selection.vertices.add(counter + 10);
  std::string error;
  EXPECT_FALSE(selection_flush(*source, selection, SelectionMode::Vertex, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(selection.vertices.contains(counter + 10));
  selection.vertices.clear();
  selection.vertices.add(first_item(source->vertices).id);
  EXPECT_FALSE(selection_flush(*source, selection, SelectionMode(0)));
  EXPECT_FALSE(selection_flush(*source, selection, SelectionMode(8)));
  selection.history.append({ElementDomain::Edge, first_item(source->edges).id});
  EXPECT_FALSE(selection_flush(*source, selection, SelectionMode::Vertex));
  selection.history.clear();
  first_item(source->edges).vertex_ids[0] = counter + 20;
  EXPECT_FALSE(selection_flush(*source, selection, SelectionMode::Vertex));
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(selection.vertices.size(), 1);
}

TEST_F(NurbsSelectionTest, NormalizedSelectionUsesExistingAtomicCommitAndRetainedSnapshots)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  const auto original = edit->read_snapshot();
  SelectionState selection;
  selection.faces.add(first_item(source->faces).id);
  const auto normalized = selection_flush(original.geometry(), selection, SelectionMode::Face);
  ASSERT_TRUE(normalized.has_value());
  auto candidate = original.copy_for_edit();
  candidate.selection = *normalized;
  ASSERT_TRUE(edit->commit(std::move(candidate), original));
  EXPECT_EQ(edit->selection().faces.size(), 1);
  EXPECT_EQ(edit->selection().edges.size(), 4);
  EXPECT_EQ(edit->selection().vertices.size(), 4);
  EXPECT_TRUE(original.selection().faces.is_empty());
  EXPECT_TRUE(original.selection().edges.is_empty());
  EXPECT_EQ(edit->geometry().next_id, original.geometry().next_id);
  ASSERT_TRUE(edit->restore(original.copy_for_edit()));
  EXPECT_TRUE(edit->selection().vertices.is_empty());
}

TEST_F(NurbsSelectionTest, BulkAndIncrementalVertexHitsSelectTheSameSourceComponents)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const auto &edge = first_item(source->edges);
  const int64_t a = edge.vertex_ids[0], b = edge.vertex_ids[1];
  const int64_t bulk_ids[] = {b, a, b, a};
  const auto bulk = selection_apply_vertices(*source, {}, {bulk_ids, 4}, SelectionAction::Set);
  const auto click_a = selection_apply_vertices(*source, {}, {&a, 1}, SelectionAction::Set, a);
  ASSERT_TRUE(bulk.has_value() && click_a.has_value());
  const auto click_b = selection_apply_vertices(
      *source, *click_a, {&b, 1}, SelectionAction::Add, b);
  ASSERT_TRUE(click_b.has_value());
  expect_selection_equal(*bulk, *click_b);
  EXPECT_EQ(bulk->vertices.size(), 2);
  EXPECT_EQ(bulk->edges.size(), 1);
  EXPECT_TRUE(bulk->edges.contains(edge.id));
  EXPECT_TRUE(bulk->history.is_empty());
  ASSERT_EQ(click_b->history.size(), 2);
  EXPECT_EQ(click_b->active, (ElementRef{ElementDomain::Vertex, b}));
}

TEST_F(NurbsSelectionTest, VertexHitSubtractIntersectAndInvertPruneHistoryAndInferredFaces)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  Vector<int64_t> ids;
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    ids.append(vertex.id);
  }
  const int64_t a = ids.first(), b = ids[1];
  const auto all = selection_apply_vertices(*source, {}, ids, SelectionAction::Set, a);
  ASSERT_TRUE(all.has_value());
  ASSERT_EQ(all->faces.size(), 1);
  const auto sub = selection_apply_vertices(*source, *all, {&a, 1}, SelectionAction::Subtract);
  ASSERT_TRUE(sub.has_value());
  EXPECT_EQ(sub->vertices.size(), 3);
  EXPECT_TRUE(sub->faces.is_empty());
  EXPECT_TRUE(sub->history.is_empty());
  EXPECT_FALSE(sub->active.has_value());
  const auto intersection = selection_apply_vertices(
      *source, *all, {&b, 1}, SelectionAction::Intersect);
  ASSERT_TRUE(intersection.has_value());
  EXPECT_EQ(intersection->vertices.size(), 1);
  EXPECT_TRUE(intersection->edges.is_empty());
  const int64_t repeated[] = {a, a, a};
  const auto inverted = selection_apply_vertices(
      *source, *all, {repeated, 3}, SelectionAction::Invert);
  ASSERT_TRUE(inverted.has_value());
  expect_selection_equal(*sub, *inverted);
  EXPECT_EQ(all->vertices.size(), 4);
  EXPECT_EQ(all->history.size(), 1);
}

TEST_F(NurbsSelectionTest, VertexHitFailureLeavesInputAndSourceUntouched)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t counter = source->next_id;
  const int64_t id = first_item(source->vertices).id;
  const int64_t unknown = counter + 10;
  const auto input = selection_apply_vertices(*source, {}, {&id, 1}, SelectionAction::Set, id);
  ASSERT_TRUE(input.has_value());
  std::string error;
  EXPECT_FALSE(selection_apply_vertices(
      *source, *input, {&unknown, 1}, SelectionAction::Add, std::nullopt, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(selection_apply_vertices(*source, *input, {}, SelectionAction::Set, unknown));
  EXPECT_FALSE(selection_apply_vertices(*source, *input, {}, SelectionAction(99)));
  EXPECT_EQ(input->vertices.size(), 1);
  EXPECT_EQ(input->history.size(), 1);
  EXPECT_EQ(input->active, (ElementRef{ElementDomain::Vertex, id}));
  EXPECT_EQ(source->next_id, counter);
}

TEST_F(NurbsSelectionTest, RemovingActiveVertexRestoresTheLastSurvivingHistoryElement)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const auto &edge = first_item(source->edges);
  const int64_t a = edge.vertex_ids[0], b = edge.vertex_ids[1];
  const auto first = selection_apply_vertices(*source, {}, {&a, 1}, SelectionAction::Set, a);
  ASSERT_TRUE(first.has_value());
  const auto both = selection_apply_vertices(*source, *first, {&b, 1}, SelectionAction::Add, b);
  ASSERT_TRUE(both.has_value());
  for (const bool explicit_click : {false, true}) {
    const auto removed = selection_apply_vertices(*source,
                                                  *both,
                                                  {&b, 1},
                                                  SelectionAction::Subtract,
                                                  explicit_click ? std::optional(b) :
                                                                   std::nullopt);
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->active, (ElementRef{ElementDomain::Vertex, a}));
    ASSERT_EQ(removed->history.size(), 1);
    EXPECT_EQ(removed->history.first(), (ElementRef{ElementDomain::Vertex, a}));
    EXPECT_TRUE(removed->edges.is_empty());
  }
}

TEST_F(NurbsSelectionTest, FaceRegionPlanMatchesActualBMeshExtrusionCounts)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto adjacent = extrude_edges(*sheet, {first_item(sheet->edges).id}, {0, 0, 2}, 0);
  ASSERT_TRUE(adjacent.has_value());
  const auto &source = *adjacent->geometry;
  for (int mask = 1; mask < 4; mask++) {
    SCOPED_TRACE(mask);
    Vector<int64_t> selected;
    int bit = 0;
    for (const NurbsTopologyFace &face : source.faces) {
      if (mask & (1 << bit++)) {
        selected.append(face.id);
      }
    }
    std::string error;
    const auto plan = plan_face_region_extrusion(source, selected, &error);
    ASSERT_TRUE(plan.has_value()) << error;
    SelectionState face_selection;
    face_selection.faces.add_multiple(selected);
    const auto closure = selection_flush(source, face_selection, SelectionMode::Face);
    ASSERT_TRUE(closure.has_value());
    MeshReference reference(source);
    reference.flush(face_selection, SelectionMode::Face);
    BMOperator operation;
    BMO_op_init(reference.mesh.get(), &operation, BMO_FLAG_DEFAULTS, "extrude_face_region");
    BMO_slot_buffer_from_enabled_hflag(reference.mesh.get(),
                                       &operation,
                                       operation.slots_in,
                                       "geom",
                                       BM_VERT | BM_EDGE | BM_FACE,
                                       BM_ELEM_SELECT);
    BMO_op_exec(reference.mesh.get(), &operation);
    const int removed_faces = plan->remove_original_faces ? plan->faces.size() : 0;
    const int removed_edges = plan->remove_original_faces ? plan->interior_edges.size() : 0;
    const int removed_vertices = plan->remove_original_faces ? plan->interior_vertices.size() : 0;
    EXPECT_EQ(reference.mesh->totface,
              source.faces.count() + selected.size() + plan->boundary_edges.size() -
                  removed_faces);
    Set<int64_t> boundary_vertices;
    const auto index = TopologyIndex::build(source);
    for (const int64_t id : plan->boundary_edges) {
      const auto &edge = *index->edge(id);
      boundary_vertices.add_multiple(Span(edge.vertex_ids, 2));
    }
    EXPECT_EQ(reference.mesh->totedge,
              source.edges.count() + closure->edges.size() + boundary_vertices.size() -
                  removed_edges);
    EXPECT_EQ(reference.mesh->totvert,
              source.vertices.count() + closure->vertices.size() - removed_vertices);
    BMO_op_finish(reference.mesh.get(), &operation);
  }
}

TEST_F(NurbsSelectionTest, RegionPlanIncludesHoleWallsAndExcludesPeriodicSeam)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto &face = first_item(sheet->faces);
  const double2 hole[] = {{0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}};
  add_planar_loop(*sheet, face, {hole, 4}, true);
  const auto plan = plan_face_region_extrusion(*sheet, {face.id, face.id});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->faces.size(), 1);
  EXPECT_EQ(plan->boundary_edges.size(), 8);
  EXPECT_FALSE(plan->remove_original_faces);
  const auto cylinder = make_arc_sheet(true);
  ASSERT_NE(cylinder, nullptr);
  const auto periodic = plan_face_region_extrusion(*cylinder, {first_item(cylinder->faces).id});
  ASSERT_TRUE(periodic.has_value());
  EXPECT_EQ(periodic->boundary_edges.size(), 2);
  EXPECT_EQ(periodic->interior_edges.size(), 1);
}

TEST_F(NurbsSelectionTest, RegionPlanRejectsUnknownIDsAndAmbiguousSelectedNonManifoldUses)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  const int64_t shared = first_item(sheet->edges).id;
  auto second = extrude_edges(*sheet, {shared}, {0, 0, 2}, 0);
  ASSERT_TRUE(second.has_value());
  auto third = extrude_edges(*second->geometry, {shared}, {0, 0, -2}, 0);
  ASSERT_TRUE(third.has_value());
  Vector<int64_t> faces;
  for (const NurbsTopologyFace &face : third->geometry->faces) {
    faces.append(face.id);
  }
  const int64_t counter = third->geometry->next_id;
  std::string error;
  EXPECT_FALSE(plan_face_region_extrusion(*third->geometry, {}, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(plan_face_region_extrusion(*third->geometry, {counter}, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(plan_face_region_extrusion(*third->geometry, faces, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(third->geometry->next_id, counter);
  EXPECT_EQ(third->geometry->faces.count(), 3);
}

TEST_F(NurbsSelectionTest, ComponentDeletionPlansMatchActualBMeshDeletion)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto adjacent = extrude_edges(*sheet, {first_item(sheet->edges).id}, {0, 0, 2}, 0);
  ASSERT_TRUE(adjacent.has_value());
  for (const NurbsGeometry *source : {sheet.get(), adjacent->geometry.get()}) {
    for (const DeleteMode mode :
         {DeleteMode::Vertices, DeleteMode::Edges, DeleteMode::Faces, DeleteMode::FacesOnly})
    {
      Vector<int64_t> domain;
      if (mode == DeleteMode::Vertices) {
        for (const NurbsTopologyVertex &vertex : source->vertices) {
          domain.append(vertex.id);
        }
      }
      else if (mode == DeleteMode::Edges) {
        for (const NurbsTopologyEdge &edge : source->edges) {
          domain.append(edge.id);
        }
      }
      else {
        for (const NurbsTopologyFace &face : source->faces) {
          domain.append(face.id);
        }
      }
      for (uint64_t mask = 1; mask < (uint64_t(1) << domain.size()); mask++) {
        SCOPED_TRACE(mask);
        SCOPED_TRACE(int(mode));
        Vector<int64_t> ids;
        for (const int i : domain.index_range()) {
          if (mask & (uint64_t(1) << i)) {
            ids.append(domain[i]);
          }
        }
        ComponentSelection selection;
        const int context = mode == DeleteMode::Vertices ? DEL_VERTS :
                            mode == DeleteMode::Edges    ? DEL_EDGES :
                            mode == DeleteMode::Faces    ? DEL_FACES :
                                                           DEL_ONLYFACES;
        MeshReference reference(*source);
        if (mode == DeleteMode::Vertices) {
          selection.vertices = ids;
          for (const int64_t id : ids) {
            BM_elem_flag_enable(reference.vertices.lookup(id), BM_ELEM_TAG);
          }
        }
        else if (mode == DeleteMode::Edges) {
          selection.edges = ids;
          for (const int64_t id : ids) {
            BM_elem_flag_enable(reference.edges.lookup(id), BM_ELEM_TAG);
          }
        }
        else {
          selection.faces = ids;
          for (const int64_t id : ids) {
            BM_elem_flag_enable(reference.faces.lookup(id), BM_ELEM_TAG);
          }
        }
        std::string error;
        const auto plan = plan_component_deletion(*source, selection, mode, &error);
        ASSERT_TRUE(plan.has_value()) << error;
        const auto deleted = delete_components(*source, selection, mode, &error);
        ASSERT_TRUE(deleted.has_value()) << error;
        EXPECT_EQ(deleted->removed.vertices, plan->vertices);
        EXPECT_EQ(deleted->removed.edges, plan->edges);
        EXPECT_EQ(deleted->removed.faces, plan->faces);
        const auto index = TopologyIndex::build(*deleted->geometry, &error);
        ASSERT_TRUE(index.has_value()) << error;
        for (const NurbsTopologyVertex &vertex : source->vertices) {
          EXPECT_EQ(index->vertex(vertex.id) == nullptr, plan->vertices.contains(vertex.id));
        }
        for (const NurbsTopologyEdge &edge : source->edges) {
          EXPECT_EQ(index->edge(edge.id) == nullptr, plan->edges.contains(edge.id));
        }
        for (const NurbsTopologyFace &face : source->faces) {
          EXPECT_EQ(index->face(face.id) == nullptr, plan->faces.contains(face.id));
        }
        EXPECT_EQ(deleted->geometry->next_id, source->next_id);
        EXPECT_EQ(deleted->geometry->curves.count(), source->curves.count());
        EXPECT_EQ(deleted->geometry->surfaces.count(), source->surfaces.count());
        BMOperator operation;
        BMO_op_init(reference.mesh.get(), &operation, BMO_FLAG_DEFAULTS, "delete");
        BMO_slot_int_set(operation.slots_in, "context", context);
        BMO_slot_buffer_from_enabled_hflag(reference.mesh.get(),
                                           &operation,
                                           operation.slots_in,
                                           "geom",
                                           BM_VERT | BM_EDGE | BM_FACE,
                                           BM_ELEM_TAG);
        BMO_op_exec(reference.mesh.get(), &operation);
        EXPECT_EQ(reference.mesh->totvert, source->vertices.count() - plan->vertices.size());
        EXPECT_EQ(reference.mesh->totedge, source->edges.count() - plan->edges.size());
        EXPECT_EQ(reference.mesh->totface, source->faces.count() - plan->faces.size());
        EXPECT_EQ(reference.mesh->totvert, deleted->geometry->vertices.count());
        EXPECT_EQ(reference.mesh->totedge, deleted->geometry->edges.count());
        EXPECT_EQ(reference.mesh->totface, deleted->geometry->faces.count());
        BMO_op_finish(reference.mesh.get(), &operation);
      }
    }
  }
}

TEST_F(NurbsSelectionTest, LinkedComponentsMatchBMeshVertexShellIncludingNonManifoldAndWire)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  const int64_t shared = first_item(sheet->edges).id;
  auto second = extrude_edges(*sheet, {shared}, {0, 0, 2}, 0);
  ASSERT_TRUE(second.has_value());
  auto third = extrude_edges(*second->geometry, {shared}, {0, 0, -2}, 0);
  ASSERT_TRUE(third.has_value());
  auto &source = *third->geometry;
  const auto *a = add_vertex(source, {10, 0, 0});
  const auto *b = add_vertex(source, {11, 0, 0});
  const auto *c = add_vertex(source, {12, 1, 0});
  const auto *d = add_vertex(source, {12, -1, 0});
  for (const auto *other : {a, c, d}) {
    add_edge(source,
             *b,
             *other,
             *add_linear_curve(source, double3(b->position), double3(other->position), 3));
  }
  add_vertex(source, {20, 0, 0});
  MeshReference reference(source);
  for (const NurbsTopologyVertex &vertex : source.vertices) {
    SCOPED_TRACE(vertex.id);
    std::string error;
    const auto linked = find_linked_components(
        source, {{&vertex.id, 1}, {}, {}}, LinkedConnectivity::RequireAgreement, &error);
    ASSERT_TRUE(linked.has_value()) << error;
    reference.flush({}, SelectionMode::Vertex);
    BMVert *start = reference.vertices.lookup(vertex.id);
    BM_vert_select_set(reference.mesh.get(), start, true);
    BMWalker walker;
    BMW_init(&walker,
             reference.mesh.get(),
             BMW_VERT_SHELL,
             BMW_MASK_NOP,
             BMW_MASK_NOP,
             BMW_MASK_NOP,
             BMW_FLAG_TEST_HIDDEN,
             BMW_NIL_LAY,
             BMW_DELIMIT_NONE);
    BMEdge *edge;
    BMW_ITER (edge, &walker, start) {
      BM_edge_select_set(reference.mesh.get(), edge, true);
    }
    BMW_end(&walker);
    SelectionState walked;
    for (const auto item : reference.vertices.items()) {
      if (BM_elem_flag_test(item.value, BM_ELEM_SELECT)) {
        walked.vertices.add(item.key);
      }
    }
    const auto mesh_selection = reference.flush(walked, SelectionMode::Vertex);
    SelectionState native;
    native.vertices.add_multiple(linked->vertices);
    native.edges.add_multiple(linked->edges);
    native.faces.add_multiple(linked->faces);
    expect_selection_equal(native, mesh_selection);
  }
}

TEST_F(NurbsSelectionTest, LinkedComponentsUseIdentityRatherThanCoincidenceOrSharedSupports)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t face_id = first_item(source->faces).id;
  auto copy = copy_component_closure(*source, {{}, {}, {&face_id, 1}});
  ASSERT_TRUE(copy.has_value());
  const int64_t copied_face = copy->source_to_copy.lookup(face_id);
  const int64_t original_vertex = first_item(source->vertices).id;
  const int64_t copied_vertex = copy->source_to_copy.lookup(original_vertex);
  first_item(copy->geometry->faces).surface_id = first_item(source->faces).surface_id;
  for (NurbsTopologyEdge &edge : copy->geometry->edges) {
    for (const NurbsTopologyEdge &original : source->edges) {
      if (copy->source_to_copy.lookup(original.id) == edge.id) {
        edge.curve_id = original.curve_id;
      }
    }
  }
  source->next_id = copy->geometry->next_id;
  BLI_movelisttolist(&source->vertices, &copy->geometry->vertices);
  BLI_movelisttolist(&source->edges, &copy->geometry->edges);
  BLI_movelisttolist(&source->faces, &copy->geometry->faces);
  BLI_movelisttolist(&source->curves, &copy->geometry->curves);
  BLI_movelisttolist(&source->surfaces, &copy->geometry->surfaces);
  const int64_t counter = source->next_id;
  const auto original = find_linked_components(
      *source, {{&original_vertex, 1}, {}, {}}, LinkedConnectivity::RequireAgreement);
  const auto duplicate = find_linked_components(
      *source, {{&copied_vertex, 1}, {}, {}}, LinkedConnectivity::RequireAgreement);
  ASSERT_TRUE(original.has_value());
  ASSERT_TRUE(duplicate.has_value());
  EXPECT_EQ(original->vertices.size(), 4);
  EXPECT_EQ(duplicate->vertices.size(), 4);
  EXPECT_EQ(original->faces.as_span(), Span<int64_t>(&face_id, 1));
  EXPECT_EQ(duplicate->faces.as_span(), Span<int64_t>(&copied_face, 1));
  const int64_t unordered[] = {copied_vertex, original_vertex, copied_vertex};
  const int64_t ordered[] = {original_vertex, copied_vertex};
  const auto both = find_linked_components(
      *source, {{unordered, 3}, {}, {}}, LinkedConnectivity::RequireAgreement);
  const auto again = find_linked_components(
      *source, {{ordered, 2}, {}, {}}, LinkedConnectivity::RequireAgreement);
  ASSERT_TRUE(both.has_value());
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(both->vertices.as_span(), again->vertices.as_span());
  EXPECT_EQ(both->edges.as_span(), again->edges.as_span());
  EXPECT_EQ(both->faces.as_span(), again->faces.as_span());
  EXPECT_EQ(both->vertices.size(), 8);
  EXPECT_EQ(source->next_id, counter);
}

TEST_F(NurbsSelectionTest, LinkedComponentsJoinAllFaceLoopsAndHandleClosedRationalSeams)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto &face = first_item(sheet->faces);
  const double2 hole[] = {{0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}};
  add_planar_loop(*sheet, face, {hole, 4}, true);
  const auto circle = make_arc_sheet(true);
  ASSERT_NE(circle, nullptr);
  for (const NurbsGeometry *source : {sheet.get(), circle.get()}) {
    for (const NurbsTopologyVertex &vertex : source->vertices) {
      const auto linked = find_linked_components(
          *source, {{&vertex.id, 1}, {}, {}}, LinkedConnectivity::FaceBoundaries);
      ASSERT_TRUE(linked.has_value());
      EXPECT_EQ(linked->vertices.size(), source->vertices.count());
      EXPECT_EQ(linked->edges.size(), source->edges.count());
      EXPECT_EQ(linked->faces.size(), 1);
    }
    for (const NurbsTopologyEdge &edge : source->edges) {
      const auto linked = find_linked_components(
          *source, {{}, {&edge.id, 1}, {}}, LinkedConnectivity::FaceBoundaries);
      ASSERT_TRUE(linked.has_value());
      EXPECT_EQ(linked->vertices.size(), source->vertices.count());
      EXPECT_EQ(linked->edges.size(), source->edges.count());
      EXPECT_EQ(linked->faces.size(), 1);
    }
    const int64_t id = first_item(source->faces).id;
    const auto linked = find_linked_components(
        *source, {{}, {}, {&id, 1}}, LinkedConnectivity::FaceBoundaries);
    ASSERT_TRUE(linked.has_value());
    EXPECT_EQ(linked->vertices.size(), source->vertices.count());
    EXPECT_EQ(linked->edges.size(), source->edges.count());
  }
}

TEST_F(NurbsSelectionTest, LinkedSelectionPreservesClickHistoryAndMatchesBulkVertexSelection)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t id = first_item(source->vertices).id;
  const auto click = selection_apply_vertices(*source, {}, {&id, 1}, SelectionAction::Set, id);
  ASSERT_TRUE(click.has_value());
  const auto linked = find_linked_components(
      *source, {{&id, 1}, {}, {}}, LinkedConnectivity::RequireAgreement);
  ASSERT_TRUE(linked.has_value());
  const auto selected = selection_apply_vertices(
      *source, *click, linked->vertices, SelectionAction::Add);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->active, click->active);
  EXPECT_EQ(selected->history.as_span(), click->history.as_span());
  Vector<int64_t> all;
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    all.append(vertex.id);
  }
  const auto bulk = selection_apply_vertices(*source, {}, all, SelectionAction::Set);
  ASSERT_TRUE(bulk.has_value());
  expect_selection_equal(*selected, *bulk);
  const auto hover = selection_apply_vertices(*source, {}, linked->vertices, SelectionAction::Add);
  ASSERT_TRUE(hover.has_value());
  EXPECT_FALSE(hover->active.has_value());
  EXPECT_TRUE(hover->history.is_empty());
  const auto deselected = selection_apply_vertices(
      *source, *selected, linked->vertices, SelectionAction::Subtract);
  ASSERT_TRUE(deselected.has_value());
  EXPECT_TRUE(deselected->vertices.is_empty());
  EXPECT_TRUE(deselected->edges.is_empty());
  EXPECT_TRUE(deselected->faces.is_empty());
  EXPECT_TRUE(deselected->history.is_empty());
  EXPECT_FALSE(deselected->active.has_value());
}

TEST_F(NurbsSelectionTest, LinkedComponentsRejectUnknownDomainsAndLeaveEmptySeedsEmpty)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t counter = source->next_id;
  std::string error = "old error";
  const auto empty = find_linked_components(
      *source, {}, LinkedConnectivity::RequireAgreement, &error);
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->vertices.is_empty());
  EXPECT_TRUE(empty->edges.is_empty());
  EXPECT_TRUE(empty->faces.is_empty());
  EXPECT_TRUE(error.empty());
  const int64_t vertex_id = first_item(source->vertices).id;
  for (const ComponentSelection seeds : {ComponentSelection{{&counter, 1}, {}, {}},
                                         ComponentSelection{{}, {&vertex_id, 1}, {}},
                                         ComponentSelection{{}, {}, {&vertex_id, 1}}})
  {
    EXPECT_FALSE(
        find_linked_components(*source, seeds, LinkedConnectivity::RequireAgreement, &error));
    EXPECT_FALSE(error.empty());
  }
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(first_item(source->vertices).position[0], -1);
}

TEST_F(NurbsSelectionTest, LinkedConnectivityRequiresAnExplicitRuleWhenHoleLoopsDiffer)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto &face = first_item(source->faces);
  const double2 hole[] = {{0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}};
  add_planar_loop(*source, face, {hole, 4}, true);
  const int64_t outer = first_item(source->vertices).id;
  const int64_t inner = static_cast<NurbsTopologyVertex *>(source->vertices.last)->id;
  const int64_t counter = source->next_id;
  for (const int64_t id : {outer, inner}) {
    const ComponentSelection seeds{{&id, 1}, {}, {}};
    const auto graph = find_linked_components(*source, seeds, LinkedConnectivity::VertexGraph);
    const auto surface = find_linked_components(
        *source, seeds, LinkedConnectivity::FaceBoundaries);
    ASSERT_TRUE(graph.has_value());
    ASSERT_TRUE(surface.has_value());
    EXPECT_EQ(graph->vertices.size(), 4);
    EXPECT_EQ(graph->edges.size(), 4);
    EXPECT_TRUE(graph->faces.is_empty());
    EXPECT_EQ(surface->vertices.size(), 8);
    EXPECT_EQ(surface->edges.size(), 8);
    EXPECT_EQ(surface->faces.size(), 1);
    std::string error;
    EXPECT_FALSE(
        find_linked_components(*source, seeds, LinkedConnectivity::RequireAgreement, &error));
    EXPECT_FALSE(error.empty());
  }
  const int64_t both[] = {outer, inner};
  const auto agreement = find_linked_components(
      *source, {{both, 2}, {}, {}}, LinkedConnectivity::RequireAgreement);
  ASSERT_TRUE(agreement.has_value());
  EXPECT_EQ(agreement->vertices.size(), 8);
  EXPECT_EQ(agreement->faces.size(), 1);
  // A face seed explicitly includes every boundary, so its result is unambiguous.
  const auto face_seed = find_linked_components(
      *source, {{}, {}, {&face.id, 1}}, LinkedConnectivity::RequireAgreement);
  ASSERT_TRUE(face_seed.has_value());
  EXPECT_EQ(face_seed->vertices.as_span(), agreement->vertices.as_span());
  const auto *loose = add_vertex(*source, {20, 0, 0});
  const auto unrelated = find_linked_components(
      *source, {{&loose->id, 1}, {}, {}}, LinkedConnectivity::RequireAgreement);
  ASSERT_TRUE(unrelated.has_value());
  EXPECT_EQ(unrelated->vertices.size(), 1);
  EXPECT_EQ(source->next_id, counter + 1);
  EXPECT_FALSE(find_linked_components(*source, {}, LinkedConnectivity(99)));
}

TEST_F(NurbsSelectionTest, DuplicateSelectionAndHistoryMatchActualBMeshDuplication)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto adjacent = extrude_edges(*sheet, {first_item(sheet->edges).id}, {0, 0, 2}, 0);
  ASSERT_TRUE(adjacent.has_value());
  auto &source = *adjacent->geometry;
  add_vertex(source, {10, 0, 0});
  for (const uint64_t mask : {1, 3, 5, 15, 127}) {
    SCOPED_TRACE(mask);
    auto input = selection_flush(source, mask_selection(source, mask), SelectionMode::Vertex);
    ASSERT_TRUE(input.has_value());
    for (const NurbsTopologyVertex &vertex : source.vertices) {
      if (input->vertices.contains(vertex.id)) {
        input->history.append({ElementDomain::Vertex, vertex.id});
        input->active = input->history.last();
      }
    }
    const Vector<int64_t> vertices(input->vertices.begin(), input->vertices.end());
    const Vector<int64_t> edges(input->edges.begin(), input->edges.end());
    const Vector<int64_t> faces(input->faces.begin(), input->faces.end());
    const auto duplicate = duplicate_components(source, {vertices, edges, faces});
    ASSERT_TRUE(duplicate.has_value());
    std::string error;
    const auto selection = selection_remap(source,
                                           *duplicate->geometry,
                                           *input,
                                           duplicate->source_to_copy,
                                           SelectionMode::Vertex,
                                           &error);
    ASSERT_TRUE(selection.has_value()) << error;
    MeshReference reference(source);
    reference.flush(*input, SelectionMode::Vertex);
    for (const ElementRef item : input->history) {
      BM_select_history_store(reference.mesh.get(), reference.vertices.lookup(item.id));
    }
    BMOperator operation;
    BMO_op_init(reference.mesh.get(), &operation, BMO_FLAG_DEFAULTS, "duplicate");
    BMO_slot_bool_set(operation.slots_in, "use_select_history", true);
    BMO_slot_bool_set(operation.slots_in, "use_edge_flip_from_face", true);
    BMO_slot_buffer_from_enabled_hflag(reference.mesh.get(),
                                       &operation,
                                       operation.slots_in,
                                       "geom",
                                       BM_VERT | BM_EDGE | BM_FACE,
                                       BM_ELEM_SELECT);
    BMO_op_exec(reference.mesh.get(), &operation);
    BM_SELECT_HISTORY_BACKUP(reference.mesh.get());
    BM_mesh_elem_hflag_disable_all(reference.mesh.get(), BM_ALL_NOLOOP, BM_ELEM_SELECT, false);
    BMO_slot_buffer_hflag_enable(reference.mesh.get(),
                                 operation.slots_out,
                                 "geom.out",
                                 BM_ALL_NOLOOP,
                                 BM_ELEM_SELECT,
                                 true);
    BM_SELECT_HISTORY_RESTORE(reference.mesh.get());
    EXPECT_EQ(reference.mesh->totvert, duplicate->geometry->vertices.count());
    EXPECT_EQ(reference.mesh->totedge, duplicate->geometry->edges.count());
    EXPECT_EQ(reference.mesh->totface, duplicate->geometry->faces.count());
    EXPECT_EQ(reference.mesh->totvertsel, selection->vertices.size());
    EXPECT_EQ(reference.mesh->totedgesel, selection->edges.size());
    EXPECT_EQ(reference.mesh->totfacesel, selection->faces.size());
    BMEditSelection *history = static_cast<BMEditSelection *>(reference.mesh->selected.first);
    ASSERT_EQ(selection->history.size(), input->history.size());
    for (const int i : input->history.index_range()) {
      ASSERT_NE(history, nullptr);
      EXPECT_EQ(history->ele,
                BMO_slot_map_elem_get(BMO_slot_get(operation.slots_out, "vert_map.out"),
                                      reference.vertices.lookup(input->history[i].id)));
      EXPECT_EQ(selection->history[i].id, duplicate->source_to_copy.lookup(input->history[i].id));
      history = history->next;
    }
    EXPECT_EQ(history, nullptr);
    EXPECT_EQ(selection->active->id, duplicate->source_to_copy.lookup(input->active->id));
    for (const int64_t id : input->vertices) {
      EXPECT_FALSE(selection->vertices.contains(id));
    }
    BMO_op_finish(reference.mesh.get(), &operation);
  }
}

TEST_F(NurbsSelectionTest, SelectionRemappingFlushesAllSevenModesWithoutInventingHistory)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  SelectionState face;
  face.faces.add(first_item(source->faces).id);
  const auto input = selection_flush(*source, face, SelectionMode::Face);
  ASSERT_TRUE(input.has_value());
  const int64_t face_id = first_item(source->faces).id;
  const auto duplicate = duplicate_components(*source, {{}, {}, {&face_id, 1}});
  ASSERT_TRUE(duplicate.has_value());
  for (int mode = 1; mode <= 7; mode++) {
    const auto result = selection_remap(
        *source, *duplicate->geometry, *input, duplicate->source_to_copy, SelectionMode(mode));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->vertices.size(), 4);
    EXPECT_EQ(result->edges.size(), 4);
    EXPECT_EQ(result->faces.size(), 1);
    EXPECT_TRUE(result->faces.contains(duplicate->source_to_copy.lookup(face_id)));
    EXPECT_TRUE(result->history.is_empty());
    EXPECT_FALSE(result->active.has_value());
  }
}

TEST_F(NurbsSelectionTest, SelectionRemappingHandlesPartialAndMergedMapsAndRejectsWrongDomains)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const auto &edge = first_item(source->edges);
  const int64_t a = edge.vertex_ids[0], b = edge.vertex_ids[1];
  const auto first = selection_apply_vertices(*source, {}, {&a, 1}, SelectionAction::Set, a);
  ASSERT_TRUE(first.has_value());
  const auto both = selection_apply_vertices(*source, *first, {&b, 1}, SelectionAction::Add, b);
  ASSERT_TRUE(both.has_value());
  Map<int64_t, int64_t> merged;
  merged.add(a, b);
  merged.add(b, b);
  const auto result = selection_remap(*source, *source, *both, merged, SelectionMode::Vertex);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->vertices.size(), 1);
  EXPECT_EQ(result->active, (ElementRef{ElementDomain::Vertex, b}));
  ASSERT_EQ(result->history.size(), 1);
  EXPECT_EQ(result->history.first(), (ElementRef{ElementDomain::Vertex, b}));
  Map<int64_t, int64_t> partial;
  partial.add(a, a);
  const auto surviving = selection_remap(*source, *source, *both, partial, SelectionMode::Vertex);
  ASSERT_TRUE(surviving.has_value());
  EXPECT_EQ(surviving->active, (ElementRef{ElementDomain::Vertex, a}));
  EXPECT_EQ(surviving->history.size(), 1);
  partial.add(b, edge.id);
  std::string error;
  EXPECT_FALSE(selection_remap(*source, *source, *both, partial, SelectionMode::Vertex, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(both->vertices.size(), 2);
  EXPECT_EQ(both->history.size(), 2);
  SelectionState invalid = *both;
  invalid.vertices.add(source->next_id);
  EXPECT_FALSE(selection_remap(*source, *source, invalid, {}, SelectionMode::Vertex, &error));
  EXPECT_FALSE(error.empty());
  const auto empty = selection_remap(*source, *source, *both, {}, SelectionMode::Vertex);
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->vertices.is_empty());
  EXPECT_TRUE(empty->history.is_empty());
  EXPECT_FALSE(empty->active.has_value());
}

TEST_F(NurbsSelectionTest, DeletionCandidatesPruneStoredSelectionAndPreserveRationalResources)
{
  const auto sheet = make_planar_sheet();
  const auto circle = make_arc_sheet(true);
  ASSERT_NE(sheet, nullptr);
  ASSERT_NE(circle, nullptr);
  for (NurbsGeometry *source : {sheet.get(), circle.get()}) {
    SelectionState input;
    const int64_t face_id = first_item(source->faces).id;
    const int64_t vertex_id = first_item(source->vertices).id;
    input.faces.add(face_id);
    input.history.append({ElementDomain::Face, face_id});
    auto selected = selection_flush(*source, input, SelectionMode::Face);
    ASSERT_TRUE(selected.has_value());
    selected->history.prepend({ElementDomain::Vertex, vertex_id});
    selected->active = ElementRef{ElementDomain::Face, face_id};
    source->stored_selection = selection_to_storage(*selected);
    const int64_t counter = source->next_id;
    std::string error;
    const auto deleted = delete_components(
        *source, {{}, {}, {&face_id, 1}}, DeleteMode::FacesOnly, &error);
    ASSERT_TRUE(deleted.has_value()) << error;
    EXPECT_TRUE(deleted->geometry->faces.is_empty());
    EXPECT_EQ(deleted->geometry->vertices.count(), source->vertices.count());
    EXPECT_EQ(deleted->geometry->edges.count(), source->edges.count());
    EXPECT_EQ(deleted->geometry->next_id, counter);
    const auto surviving = selection_from_storage(deleted->geometry->stored_selection);
    ASSERT_TRUE(surviving.has_value());
    EXPECT_EQ(surviving->vertices, selected->vertices);
    EXPECT_EQ(surviving->edges, selected->edges);
    EXPECT_TRUE(surviving->faces.is_empty());
    EXPECT_EQ(surviving->active, (ElementRef{ElementDomain::Vertex, vertex_id}));
    ASSERT_EQ(surviving->history.size(), 1);
    EXPECT_EQ(surviving->history.first(), (ElementRef{ElementDomain::Vertex, vertex_id}));
    const auto unchanged = selection_from_storage(source->stored_selection);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->active, selected->active);
    EXPECT_EQ(unchanged->history.size(), 2);
    EXPECT_EQ(source->faces.count(), 1);
    const auto *saved = static_cast<const NurbsCurveGeometry *>(deleted->geometry->curves.first);
    for (const NurbsCurveGeometry &curve : source->curves) {
      ASSERT_NE(saved, nullptr);
      EXPECT_EQ(saved->id, curve.id);
      EXPECT_NE(saved->points, curve.points);
      ASSERT_EQ(saved->points_num, curve.points_num);
      ASSERT_EQ(saved->knots_num, curve.knots_num);
      EXPECT_EQ(
          std::memcmp(saved->points, curve.points, sizeof(NurbsControlPoint) * curve.points_num),
          0);
      EXPECT_EQ(std::memcmp(saved->knots, curve.knots, sizeof(double) * curve.knots_num), 0);
      saved = saved->next;
    }
  }
}

TEST_F(NurbsSelectionTest, DeletionRejectionsAndEmptyRequestsPreserveIdentityAndSelection)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  source->next_id = std::numeric_limits<int64_t>::max();
  SelectionState selected;
  const int64_t id = first_item(source->vertices).id;
  selected.vertices.add(id);
  selected.history.append({ElementDomain::Vertex, id});
  source->stored_selection = selection_to_storage(selected);
  const auto empty = delete_components(*source, {}, DeleteMode::Vertices);
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->geometry->next_id, source->next_id);
  EXPECT_EQ(empty->geometry->vertices.count(), source->vertices.count());
  const auto kept = selection_from_storage(empty->geometry->stored_selection);
  ASSERT_TRUE(kept.has_value());
  EXPECT_FALSE(kept->active.has_value());
  EXPECT_EQ(kept->history.as_span(), selected.history.as_span());
  std::string error;
  EXPECT_FALSE(delete_components(*source, {{}, {}, {&id, 1}}, DeleteMode::Faces, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(delete_components(*source, {}, DeleteMode(99), &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source->vertices.count(), 4);
  EXPECT_EQ(source->next_id, std::numeric_limits<int64_t>::max());
  const auto deleted = delete_components(
      *source, {{&id, 1}, {}, {}}, DeleteMode::Vertices, &error);
  ASSERT_TRUE(deleted.has_value()) << error;
  EXPECT_EQ(deleted->geometry->next_id, source->next_id);
  const auto pruned = selection_from_storage(deleted->geometry->stored_selection);
  ASSERT_TRUE(pruned.has_value());
  EXPECT_TRUE(pruned->vertices.is_empty());
  EXPECT_TRUE(pruned->history.is_empty());
  EXPECT_FALSE(pruned->active.has_value());
}

TEST_F(NurbsSelectionTest, RegionConstructionCountsAndOrientedNormalsMatchBMeshAcrossFaceSubsets)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto adjacent = extrude_edges(*sheet, {first_item(sheet->edges).id}, {0, -2, 0}, 0);
  ASSERT_TRUE(adjacent.has_value());
  const int64_t face_id = first_item(sheet->faces).id;
  auto cube = extrude_components(*sheet, {{}, {}, {&face_id, 1}}, {0, 0, 2}, 0);
  ASSERT_TRUE(cube.has_value());
  for (const NurbsGeometry *source : {sheet.get(), adjacent->geometry.get(), cube->geometry.get()})
  {
    Vector<int64_t> all_faces;
    for (const NurbsTopologyFace &face : source->faces) {
      all_faces.append(face.id);
    }
    for (uint64_t mask = 1; mask < (uint64_t(1) << all_faces.size()); mask++) {
      SCOPED_TRACE(all_faces.size());
      SCOPED_TRACE(mask);
      SelectionState input;
      for (const int i : all_faces.index_range()) {
        if (mask & (uint64_t(1) << i)) {
          input.faces.add(all_faces[i]);
        }
      }
      const auto selected = selection_flush(*source, input, SelectionMode::Face);
      ASSERT_TRUE(selected.has_value());
      const Vector<int64_t> vertices(selected->vertices.begin(), selected->vertices.end());
      const Vector<int64_t> edges(selected->edges.begin(), selected->edges.end());
      const Vector<int64_t> faces(selected->faces.begin(), selected->faces.end());
      std::string error;
      const auto result = extrude_components(
          *source, {vertices, edges, faces}, {0, 0, 2}, 0, &error);
      ASSERT_TRUE(result.has_value()) << error;
      MeshReference reference(*source);
      reference.flush(*selected, SelectionMode::Face);
      BMesh *bm = reference.mesh.get();
      BMOperator operation;
      BMO_op_init(bm, &operation, BMO_FLAG_DEFAULTS, "extrude_face_region");
      BMO_slot_buffer_from_enabled_hflag(
          bm, &operation, operation.slots_in, "geom", BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT);
      BMO_op_exec(bm, &operation);
      BMOIter iterator;
      BMVert *vertex;
      BMO_ITER (vertex, &iterator, operation.slots_out, "geom.out", BM_VERT) {
        vertex->co[2] += 2;
      }
      BM_mesh_normals_update(bm);
      EXPECT_EQ(bm->totvert, result->geometry->vertices.count());
      EXPECT_EQ(bm->totedge, result->geometry->edges.count());
      EXPECT_EQ(bm->totface, result->geometry->faces.count());
      const auto index = TopologyIndex::build(*result->geometry);
      ASSERT_TRUE(index.has_value());
      Map<int64_t, const NurbsSurfaceGeometry *> supports;
      for (const NurbsSurfaceGeometry &surface : result->geometry->surfaces) {
        supports.add_new(surface.id, &surface);
      }
      auto compare_normal = [&](const NurbsTopologyFace &face, const BMFace &mesh_face) {
        const auto &surface = *supports.lookup(face.surface_id);
        const double2 uv(
            (surface.knots_u[surface.order_u - 1] + surface.knots_u[surface.points_u]) * 0.5,
            (surface.knots_v[surface.order_v - 1] + surface.knots_v[surface.points_v]) * 0.5);
        const auto frame = evaluate_surface_differential(surface, uv);
        ASSERT_TRUE(frame.has_value());
        for (int axis = 0; axis < 3; axis++) {
          EXPECT_NEAR(frame->normal ? (*frame->normal)[axis] * face.orientation : 0,
                      mesh_face.no[axis],
                      1e-6);
        }
      };
      Set<BMFace *> original_faces;
      for (const NurbsTopologyFace &face : source->faces) {
        if (!result->removed.faces.contains(face.id)) {
          BMFace *mesh_face = reference.faces.lookup(face.id);
          original_faces.add(mesh_face);
          compare_normal(*index->face(face.id), *mesh_face);
        }
      }
      for (const auto wall : result->edge_to_face.items()) {
        BMIter faces_iterator;
        BMFace *candidate, *wall_face = nullptr;
        BM_ITER_ELEM (
            candidate, &faces_iterator, reference.edges.lookup(wall.key), BM_FACES_OF_EDGE)
        {
          if (!original_faces.contains(candidate)) {
            ASSERT_EQ(wall_face, nullptr);
            wall_face = candidate;
          }
        }
        ASSERT_NE(wall_face, nullptr);
        compare_normal(*index->face(wall.value), *wall_face);
      }
      BMO_op_finish(bm, &operation);
    }
  }
}

TEST_F(NurbsSelectionTest, PeriodicRegionConstructionSharesSeamAndClosedBoundaryRails)
{
  const auto source = make_arc_sheet(true);
  ASSERT_NE(source, nullptr);
  const int64_t id = first_item(source->faces).id;
  std::string error;
  const auto result = extrude_components(*source, {{}, {}, {&id, 1}}, {3, 0, 0}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->geometry->vertices.count(), 4);
  EXPECT_EQ(result->geometry->edges.count(), 8);
  EXPECT_EQ(result->geometry->faces.count(), 4);
  EXPECT_EQ(result->edge_to_face.size(), 2);
  EXPECT_EQ(result->vertex_to_rail.size(), 2);
  const auto original = TopologyIndex::build(*source);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(original.has_value());
  ASSERT_TRUE(index.has_value());
  for (const NurbsTopologyEdge &edge : source->edges) {
    if (original->uses_for_edge(edge.id).size() == 2) {
      EXPECT_FALSE(result->edge_to_face.contains(edge.id));
      EXPECT_EQ(index->uses_for_edge(result->source_to_cap.lookup(edge.id)).size(), 2);
    }
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  for (const NurbsCurveGeometry &curve : result->geometry->curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsCurveGeometry &curve : source->curves) {
    const auto &cap = *curves.lookup(result->source_to_cap.lookup(curve.id));
    ASSERT_EQ(cap.points_num, curve.points_num);
    ASSERT_EQ(cap.knots_num, curve.knots_num);
    EXPECT_EQ(std::memcmp(cap.knots, curve.knots, sizeof(double) * curve.knots_num), 0);
    for (int i = 0; i < curve.points_num; i++) {
      EXPECT_EQ(cap.points[i].weight, curve.points[i].weight);
      EXPECT_EQ(double3(cap.points[i].position),
                double3(curve.points[i].position) +
                    (curve.dimension == 3 ? double3(3, 0, 0) : double3(0)));
    }
  }
}

TEST_F(NurbsSelectionTest, ComponentModeStorageRoundTripAndOlderFileDefault)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  SelectionState selection;
  selection.mode = SelectionMode::Edge;
  const int64_t id = first_item(source->edges).id;
  const ElementRef hit{ElementDomain::Edge, id};
  std::string error;
  auto selected = selection_apply(
      *source, selection, {&hit, 1}, SelectionAction::Set, hit, &error);
  ASSERT_TRUE(selected.has_value()) << error;
  auto *storage = selection_to_storage(*selected);
  ASSERT_NE(storage, nullptr);
  BLI_SCOPED_DEFER([&]() { selection_storage_free(storage); });
  EXPECT_EQ(storage->mode, int(SelectionMode::Edge));
  const auto restored = selection_from_storage(storage, &error);
  ASSERT_TRUE(restored.has_value()) << error;
  EXPECT_EQ(restored->mode, SelectionMode::Edge);
  EXPECT_EQ(restored->active, hit);
  expect_selection_equal(*restored, *selected);
  storage->mode = 0;
  EXPECT_EQ(selection_from_storage(storage)->mode, SelectionMode::Vertex);
  storage->mode = 8;
  EXPECT_FALSE(selection_from_storage(storage, &error));
  selection.mode = SelectionMode(8);
  EXPECT_EQ(selection_to_storage(selection, &error), nullptr);
  EXPECT_FALSE(error.empty());
}

TEST_F(NurbsSelectionTest, ComponentGesturesMatchBMeshAcrossAllModesAndActions)
{
  auto sheet = make_planar_sheet();
  auto adjacent = extrude_edges(*sheet, {first_item(sheet->edges).id}, {0, 0, 2}, 0);
  ASSERT_TRUE(adjacent.has_value());
  int comparisons = 0;
  for (const NurbsGeometry *source : {sheet.get(), adjacent->geometry.get()}) {
    MeshReference reference(*source);
    for (int mode = 1; mode <= 7; mode++) {
      SCOPED_TRACE(mode);
      Vector<ElementRef> elements;
      for (const NurbsTopologyVertex &vertex : source->vertices) {
        if (mode & 1) {
          elements.append({ElementDomain::Vertex, vertex.id});
        }
      }
      for (const NurbsTopologyEdge &edge : source->edges) {
        if (mode & 2) {
          elements.append({ElementDomain::Edge, edge.id});
        }
      }
      for (const NurbsTopologyFace &face : source->faces) {
        if (mode & 4) {
          elements.append({ElementDomain::Face, face.id});
        }
      }
      for (const uint64_t mask :
           {uint64_t(0), uint64_t(1), uint64_t(17), uint64_t(85), ~uint64_t(0)})
      {
        const auto initial = selection_flush(
            *source, mask_selection(*source, mask), SelectionMode(mode));
        ASSERT_TRUE(initial.has_value());
        for (const SelectionAction action : {SelectionAction::Set,
                                             SelectionAction::Add,
                                             SelectionAction::Subtract,
                                             SelectionAction::Invert,
                                             SelectionAction::Intersect})
        {
          SCOPED_TRACE(int(action));
          for (const int hit_index : IndexRange(elements.size() + 2)) {
            Vector<ElementRef> hits;
            if (hit_index < elements.size()) {
              hits.append(elements[hit_index]);
              hits.append(elements[hit_index]);
            }
            else if (hit_index == elements.size()) {
              hits = elements;
            }
            std::string error;
            const auto actual = selection_apply(
                *source, *initial, hits, action, std::nullopt, &error);
            ASSERT_TRUE(actual.has_value()) << error;
            reference.flush(action == SelectionAction::Set ? SelectionState{} : *initial,
                            SelectionMode(mode));
            reference.mesh->selectmode = mode;
            for (const ElementRef element : elements) {
              const bool hit = std::any_of(
                  hits.begin(), hits.end(), [&](const auto other) { return other == element; });
              if (!hit && action != SelectionAction::Intersect) {
                continue;
              }
              const bool selected = selection_contains(*initial, element);
              const bool value = action == SelectionAction::Intersect ? selected && hit :
                                 action == SelectionAction::Subtract  ? false :
                                 action == SelectionAction::Invert    ? !selected :
                                                                        true;
              switch (element.domain) {
                case ElementDomain::Vertex:
                  BM_vert_select_set(
                      reference.mesh.get(), reference.vertices.lookup(element.id), value);
                  break;
                case ElementDomain::Edge:
                  BM_edge_select_set(
                      reference.mesh.get(), reference.edges.lookup(element.id), value);
                  break;
                case ElementDomain::Face:
                  BM_face_select_set(
                      reference.mesh.get(), reference.faces.lookup(element.id), value);
                  break;
              }
            }
            SelectionState flags;
            for (const auto item : reference.vertices.items()) {
              if (BM_elem_flag_test(item.value, BM_ELEM_SELECT)) {
                flags.vertices.add(item.key);
              }
            }
            for (const auto item : reference.edges.items()) {
              if (BM_elem_flag_test(item.value, BM_ELEM_SELECT)) {
                flags.edges.add(item.key);
              }
            }
            for (const auto item : reference.faces.items()) {
              if (BM_elem_flag_test(item.value, BM_ELEM_SELECT)) {
                flags.faces.add(item.key);
              }
            }
            expect_selection_equal(*actual, reference.flush(flags, SelectionMode(mode)));
            EXPECT_EQ(actual->mode, SelectionMode(mode));
            comparisons++;
          }
        }
      }
    }
  }
  EXPECT_GT(comparisons, 2000);
}

TEST_F(NurbsSelectionTest, ModeChangesKeepOnlyTheSelectedComponentClosureAndHistory)
{
  auto source = make_planar_sheet();
  const auto *loose = add_vertex(*source, {5, 5, 5});
  SelectionState selected;
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    selected.vertices.add(vertex.id);
  }
  selected.active = ElementRef{ElementDomain::Vertex, loose->id};
  selected.history.append(*selected.active);
  auto edges = selection_change_mode(*source, selected, SelectionMode::Edge);
  ASSERT_TRUE(edges.has_value());
  EXPECT_EQ(edges->vertices.size(), 4);
  EXPECT_EQ(edges->edges.size(), 4);
  EXPECT_EQ(edges->faces.size(), 1);
  EXPECT_FALSE(edges->vertices.contains(loose->id));
  EXPECT_TRUE(edges->history.is_empty());
  EXPECT_FALSE(edges->active.has_value());
  const auto faces = selection_change_mode(*source, *edges, SelectionMode::Face);
  ASSERT_TRUE(faces.has_value());
  EXPECT_EQ(faces->mode, SelectionMode::Face);
  expect_selection_equal(*faces, *edges);
  const auto vertices = selection_change_mode(*source, *faces, SelectionMode::Vertex);
  ASSERT_TRUE(vertices.has_value());
  EXPECT_EQ(vertices->vertices.size(), 4);
  EXPECT_FALSE(selection_change_mode(*source, *vertices, SelectionMode(0)));
}

TEST_F(NurbsSelectionTest, ComponentHitsRejectWrongDomainsAndRetainExplicitHistory)
{
  const auto source = make_planar_sheet();
  SelectionState selection;
  selection.mode = SelectionMode::Face;
  const ElementRef face{ElementDomain::Face, first_item(source->faces).id};
  const ElementRef edge{ElementDomain::Edge, first_item(source->edges).id};
  std::string error;
  EXPECT_FALSE(
      selection_apply(*source, selection, {&edge, 1}, SelectionAction::Set, edge, &error));
  EXPECT_FALSE(error.empty());
  const auto selected = selection_apply(
      *source, selection, {&face, 1}, SelectionAction::Set, face, &error);
  ASSERT_TRUE(selected.has_value()) << error;
  EXPECT_EQ(selected->vertices.size(), 4);
  EXPECT_EQ(selected->edges.size(), 4);
  EXPECT_EQ(selected->faces.size(), 1);
  ASSERT_EQ(selected->history.size(), 1);
  EXPECT_EQ(selected->history.first(), face);
  EXPECT_EQ(selected->active, face);
  const auto deselected = selection_apply(
      *source, *selected, {&face, 1}, SelectionAction::Subtract);
  ASSERT_TRUE(deselected.has_value());
  EXPECT_TRUE(deselected->vertices.is_empty());
  EXPECT_TRUE(deselected->edges.is_empty());
  EXPECT_TRUE(deselected->faces.is_empty());
  EXPECT_TRUE(deselected->history.is_empty());
  EXPECT_FALSE(deselected->active.has_value());
}

TEST_F(NurbsSelectionTest, SourceCentersCoverVerticesEdgesFacesAndCurvedParameterMidpoints)
{
  const auto sheet = make_planar_sheet();
  const ElementRef elements[] = {
      {ElementDomain::Vertex, first_item(sheet->vertices).id},
      {ElementDomain::Edge, first_item(sheet->edges).id},
      {ElementDomain::Face, first_item(sheet->faces).id},
  };
  std::string error;
  const auto centers = element_centers(*sheet, {elements, 3}, &error);
  ASSERT_TRUE(centers.has_value()) << error;
  EXPECT_EQ((*centers)[0], double3(-1, -1, 0));
  EXPECT_EQ((*centers)[1], double3(0, -1, 0));
  EXPECT_EQ((*centers)[2], double3(0, 0, 0));
  const auto arc = make_arc_sheet(false);
  const auto &edge = first_item(arc->edges);
  const auto &curve = first_item(arc->curves);
  const ElementRef curved{ElementDomain::Edge, edge.id};
  const auto actual = element_centers(*arc, {&curved, 1}, &error);
  ASSERT_TRUE(actual.has_value()) << error;
  const auto expected = evaluate_curve(curve, 0.5, &error);
  ASSERT_TRUE(expected.has_value()) << error;
  EXPECT_EQ(actual->first(), expected->position);
  EXPECT_NE(actual->first(), double3(0.5, 0.5, 0));
  const ElementRef unknown{ElementDomain::Face, sheet->next_id};
  EXPECT_FALSE(element_centers(*sheet, {&unknown, 1}, &error));
  EXPECT_FALSE(error.empty());
}

TEST_F(NurbsSelectionTest, TransformPivotsRetainDoubleSourceAndActiveCenters)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*source, {0, 0, 0});
  const auto *b = add_vertex(*source, {2 + 0x1p-40, 0, 0});
  const auto *c = add_vertex(*source, {6, 2, 0});
  add_edge(
      *source, *a, *b, *add_linear_curve(*source, double3(a->position), double3(b->position), 3));
  add_edge(
      *source, *b, *c, *add_linear_curve(*source, double3(b->position), double3(c->position), 3));
  SelectionState selection;
  selection.vertices.add_multiple({a->id, b->id, c->id});
  selection.active = ElementRef{ElementDomain::Vertex, b->id};
  std::string error;
  const auto active = selection_transform_groups(
      *source, selection, SelectionPivot::Active, &error);
  ASSERT_TRUE(active.has_value()) << error;
  ASSERT_EQ(active->size(), 1);
  EXPECT_EQ((*active)[0].center.x, 2 + 0x1p-40);
  const auto bounds = selection_transform_groups(
      *source, selection, SelectionPivot::Bounds, &error);
  ASSERT_TRUE(bounds.has_value()) << error;
  EXPECT_EQ((*bounds)[0].center, double3(3, 1, 0));
  const auto individual = selection_transform_groups(
      *source, selection, SelectionPivot::Individual, &error);
  ASSERT_TRUE(individual.has_value()) << error;
  ASSERT_EQ(individual->size(), 1);
  EXPECT_NEAR((*individual)[0].center.x, 2.5 + 0x1p-41, 1e-15);
  EXPECT_EQ((*individual)[0].center.y, 0.5);
  const auto median = selection_transform_groups(
      *source, selection, SelectionPivot::Median, &error);
  ASSERT_TRUE(median.has_value()) << error;
  EXPECT_NEAR((*median)[0].center.x, (8 + 0x1p-40) / 3, 1e-15);
  EXPECT_EQ((*median)[0].center.y, 2.0 / 3);
}

TEST_F(NurbsSelectionTest, IndividualTransformGroupsMatchActualBMeshInEveryMode)
{
  const auto sheet = make_planar_sheet();
  const int64_t edge = first_item(sheet->edges).id;
  auto source = extrude_edges(*sheet, {edge}, {0, 0, 2}, 0);
  ASSERT_TRUE(source.has_value());
  add_vertex(*source->geometry, {10, 3, 2});
  for (int bits = 1; bits <= 7; bits++) {
    SelectionState selection;
    selection.mode = SelectionMode(bits);
    for (const auto &vertex : source->geometry->vertices) {
      selection.vertices.add(vertex.id);
    }
    for (const auto &item : source->geometry->edges) {
      selection.edges.add(item.id);
    }
    for (const auto &face : source->geometry->faces) {
      selection.faces.add(face.id);
    }
    MeshReference reference(*source->geometry);
    reference.flush(selection, selection.mode);
    BMesh *bm = reference.mesh.get();
    BM_mesh_elem_table_ensure(bm, BM_EDGE | BM_FACE);
    int (*ranges)[2] = nullptr;
    const bool edges = bits & 3;
    int *elements = MEM_new_array_uninitialized<int>(edges ? bm->totedgesel : bm->totfacesel,
                                                     __func__);
    const int count =
        edges ? BM_mesh_calc_edge_groups(bm, elements, &ranges, nullptr, nullptr, BM_ELEM_SELECT) :
                BM_mesh_calc_face_groups(
                    bm, elements, &ranges, nullptr, nullptr, nullptr, BM_ELEM_SELECT, BM_VERT);
    Map<BMVert *, float3> expected;
    for (int group = 0; group < count; group++) {
      float3 center(0);
      Set<BMVert *> vertices;
      for (int i = 0; i < ranges[group][1]; i++) {
        const int slot = elements[ranges[group][0] + i];
        BMEditSelection element{};
        element.htype = edges ? BM_EDGE : BM_FACE;
        element.ele = edges ? reinterpret_cast<BMElem *>(bm->etable[slot]) :
                              reinterpret_cast<BMElem *>(bm->ftable[slot]);
        float3 point;
        BM_editselection_center(&element, point);
        center += point;
        BMIter iter;
        BMVert *vertex;
        BM_ITER_ELEM (vertex, &iter, element.ele, edges ? BM_VERTS_OF_EDGE : BM_VERTS_OF_FACE) {
          vertices.add(vertex);
        }
      }
      center /= ranges[group][1];
      for (BMVert *vertex : vertices) {
        expected.add(vertex, center);
      }
    }
    MEM_SAFE_DELETE(elements);
    MEM_SAFE_DELETE(ranges);
    std::string error;
    const auto groups = selection_transform_groups(
        *source->geometry, selection, SelectionPivot::Individual, &error);
    ASSERT_TRUE(groups.has_value()) << error;
    for (const auto &group : *groups) {
      for (const int64_t id : group.vertices) {
        BMVert *vertex = reference.vertices.lookup(id);
        const float3 center = expected.lookup_default(vertex, float3(vertex->co));
        for (int axis = 0; axis < 3; axis++) {
          EXPECT_NEAR(group.center[axis], center[axis], 1e-6) << bits << " vertex " << id;
        }
      }
    }
  }
}

}  // namespace blender::bke::nurbs::tests
