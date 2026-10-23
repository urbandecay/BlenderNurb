/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_gtest_base.hh"
#include "BKE_nurbs_edit.hh"

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
    const BMeshCreateParams params{};
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
      faces.add_new(face.id,
                    BM_face_create(mesh.get(),
                                   face_vertices.data(),
                                   face_edges.data(),
                                   face_vertices.size(),
                                   nullptr,
                                   BM_CREATE_NOP));
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

}  // namespace blender::bke::nurbs::tests
