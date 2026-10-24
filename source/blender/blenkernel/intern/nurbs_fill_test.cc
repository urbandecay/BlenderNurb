/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>

#include "BKE_gtest_base.hh"
#include "BKE_nurbs_evaluate.hh"

#include "bmesh.hh"

#include "nurbs_test_geometry.hh"
#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

class NurbsFillTest : public BlenderGTestBase {};

static OwnedGeometry make_fill_wire(const Span<double3> points, const bool closed)
{
  OwnedGeometry geometry(MEM_new<NurbsGeometry>(__func__));
  Vector<NurbsTopologyVertex *> vertices;
  for (const double3 &point : points) {
    vertices.append(add_vertex(*geometry, point));
  }
  for (int i = 0; i < points.size() - (closed ? 0 : 1); i++) {
    const int next = (i + 1) % points.size();
    const auto *curve = add_linear_curve(*geometry, points[i], points[next], 3);
    add_edge(*geometry, *vertices[i], *vertices[next], *curve);
  }
  return geometry;
}

static Vector<int64_t> fill_edges(const NurbsGeometry &geometry)
{
  Vector<int64_t> ids;
  for (const NurbsTopologyEdge &edge : geometry.edges) {
    ids.append(edge.id);
  }
  return ids;
}

static Vector<int64_t> fill_vertices(const NurbsGeometry &geometry)
{
  Vector<int64_t> ids;
  for (const NurbsTopologyVertex &vertex : geometry.vertices) {
    ids.append(vertex.id);
  }
  return ids;
}

static void expect_fill_preview(const NurbsGeometry &geometry)
{
  std::string error;
  ASSERT_TRUE(geometry_validate_structure(geometry, &error)) << error;
  const auto boundaries = evaluate_boundaries(geometry, preview_options().boundary, &error);
  ASSERT_TRUE(boundaries.has_value()) << error;
  for (const auto &use : boundaries->uses) {
    EXPECT_LE(use.max_curve_support_deviation, 1.0e-12);
  }
  const auto preview = tessellate_surfaces(geometry, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  EXPECT_FALSE(preview->triangles.is_empty());
}

TEST_F(NurbsFillTest, BoundaryTracingIsDeterministicAndDoesNotAllocateIdentities)
{
  const double3 points[] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
  auto source = make_fill_wire({points, ARRAY_SIZE(points)}, true);
  source->next_id = std::numeric_limits<int64_t>::max();
  const auto edges = fill_edges(*source);
  const int64_t shuffled[] = {edges[2], edges[0], edges[3], edges[1], edges[2]};
  const auto traced = trace_edge_chains(*source, {shuffled, 5});
  ASSERT_TRUE(traced.has_value());
  ASSERT_EQ(traced->size(), 1);
  EXPECT_TRUE(traced->first().closed);
  EXPECT_EQ(traced->first().edges.as_span(), edges.as_span());
  EXPECT_EQ(traced->first().vertices.first(), traced->first().vertices.last());
  const auto closure = component_closure(*source, {{}, {shuffled, 5}, {}});
  ASSERT_TRUE(closure.has_value());
  EXPECT_EQ(closure->vertices.size(), 4);
  EXPECT_EQ(closure->edges.size(), 4);
  EXPECT_EQ(closure->curves.size(), 4);
  EXPECT_EQ(source->next_id, std::numeric_limits<int64_t>::max());
  EXPECT_FALSE(copy_component_closure(*source, {{}, {shuffled, 5}, {}}).has_value());
}

TEST_F(NurbsFillTest, BoundaryTracingRejectsBranchesAndRetainsParallelAndClosedEdges)
{
  const double3 points[] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
  auto source = make_fill_wire({points, ARRAY_SIZE(points)}, false);
  auto index = TopologyIndex::build(*source);
  const auto vertices = fill_vertices(*source);
  const auto *extra = add_vertex(*source, {1, 1, 0});
  const auto *curve = add_linear_curve(*source, {1, 0, 0}, {1, 1, 0}, 3);
  add_edge(*source, *index->vertex(vertices[1]), *extra, *curve);
  std::string error;
  EXPECT_FALSE(trace_edge_chains(*source, fill_edges(*source), &error).has_value());
  EXPECT_NE(error.find("branch"), std::string::npos);
  const auto pair = make_fill_wire({points, 2}, false);
  const auto pair_index = TopologyIndex::build(*pair);
  const auto pair_vertices = fill_vertices(*pair);
  const auto *parallel_curve = add_linear_curve(*pair, points[0], points[1], 3);
  add_edge(*pair,
           *pair_index->vertex(pair_vertices[0]),
           *pair_index->vertex(pair_vertices[1]),
           *parallel_curve);
  const auto pair_trace = trace_edge_chains(*pair, fill_edges(*pair));
  ASSERT_TRUE(pair_trace.has_value());
  ASSERT_EQ(pair_trace->size(), 1);
  EXPECT_TRUE(pair_trace->first().closed);
  EXPECT_EQ(pair_trace->first().edges.size(), 2);
  EXPECT_FALSE(pair_trace->first().reversed[0]);
  EXPECT_TRUE(pair_trace->first().reversed[1]);
  const auto circle = make_arc_sheet(true);
  ASSERT_NE(circle, nullptr);
  const auto &edge = *static_cast<NurbsTopologyEdge *>(circle->edges.first);
  const auto traced = trace_edge_chains(*circle, {&edge.id, 1});
  ASSERT_TRUE(traced.has_value());
  EXPECT_TRUE(traced->first().closed);
  EXPECT_EQ(traced->first().vertices.size(), 2);
}

TEST_F(NurbsFillTest, ConnectTwoVerticesPreservesSharedIdentityAndIsIdempotent)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  add_vertex(*source, {1, 2, 3});
  add_vertex(*source, {4, 6, 8});
  const auto vertices = fill_vertices(*source);
  const int64_t next_id = source->next_id;
  std::string error;
  auto filled = fill_components(*source, {vertices, {}, {}}, &error);
  ASSERT_TRUE(filled.has_value()) << error;
  EXPECT_EQ(filled->geometry->vertices.count(), 2);
  EXPECT_EQ(filled->geometry->edges.count(), 1);
  EXPECT_EQ(filled->created_edges.size(), 1);
  EXPECT_TRUE(filled->created_faces.is_empty());
  EXPECT_EQ(source->next_id, next_id);
  EXPECT_TRUE(source->edges.is_empty());
  const auto index = TopologyIndex::build(*filled->geometry);
  const auto *edge = index->edge(filled->created_edges.first());
  EXPECT_EQ(edge->vertex_ids[0], vertices[0]);
  EXPECT_EQ(edge->vertex_ids[1], vertices[1]);
  auto again = fill_components(*filled->geometry, {vertices, {}, {}}, &error);
  ASSERT_TRUE(again.has_value()) << error;
  EXPECT_TRUE(again->created_edges.is_empty());
  EXPECT_EQ(again->geometry->next_id, filled->geometry->next_id);
}

TEST_F(NurbsFillTest, ClosedQuadReusesBoundaryAndCreatesExactBilinearSupport)
{
  const double3 points[] = {{0, 0, 0}, {2, 0, 0}, {3, 2, 0}, {0, 2, 0}};
  const auto source = make_fill_wire({points, ARRAY_SIZE(points)}, true);
  const auto edges = fill_edges(*source);
  std::string error;
  auto filled = fill_components(*source, {{}, edges, {}}, &error);
  ASSERT_TRUE(filled.has_value()) << error;
  EXPECT_TRUE(filled->created_edges.is_empty());
  ASSERT_EQ(filled->created_faces.size(), 1);
  EXPECT_EQ(filled->geometry->vertices.count(), 4);
  EXPECT_EQ(filled->geometry->edges.count(), 4);
  EXPECT_EQ(filled->geometry->faces.count(), 1);
  expect_fill_preview(*filled->geometry);
  const auto &surface = *static_cast<NurbsSurfaceGeometry *>(filled->geometry->surfaces.last);
  const auto sample = evaluate_surface(surface, {0.5, 0.5});
  ASSERT_TRUE(sample.has_value());
  EXPECT_EQ(sample->position, double3(1.25, 1, 0));
  auto again = fill_components(*filled->geometry, {{}, edges, {}}, &error);
  ASSERT_TRUE(again.has_value()) << error;
  EXPECT_TRUE(again->created_faces.is_empty());
  EXPECT_EQ(again->geometry->next_id, filled->geometry->next_id);
}

TEST_F(NurbsFillTest, NonplanarQuadBendsThroughFourCornersAndKeepsBoundaryParameterization)
{
  const double3 points[] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 2}, {0, 2, 0}};
  auto source = make_fill_wire({points, ARRAY_SIZE(points)}, true);
  auto *curve = static_cast<NurbsCurveGeometry *>(source->curves.first);
  curve->points[1].weight = 2;
  const auto edges = fill_edges(*source);
  std::string error;
  auto filled = fill_components(*source, {{}, edges, {}}, &error);
  ASSERT_TRUE(filled.has_value()) << error;
  const auto &surface = *static_cast<NurbsSurfaceGeometry *>(filled->geometry->surfaces.last);
  const double2 uv[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(evaluate_surface(surface, uv[i])->position, points[i]);
  }
  EXPECT_EQ(evaluate_surface(surface, {0.5, 0.5})->position, double3(1, 1, 0.5));
  EXPECT_EQ(curve->points[1].weight, 2);
  expect_fill_preview(*filled->geometry);
}

TEST_F(NurbsFillTest, OpenChainClosesAndThreeLooseVerticesMakePlanarTrim)
{
  const double3 points[] = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
  auto source = make_fill_wire({points, ARRAY_SIZE(points)}, false);
  const auto edges = fill_edges(*source);
  auto filled = fill_components(*source, {{}, edges, {}});
  ASSERT_TRUE(filled.has_value());
  EXPECT_EQ(filled->created_edges.size(), 1);
  EXPECT_EQ(filled->geometry->edges.count(), 3);
  expect_fill_preview(*filled->geometry);
  OwnedGeometry loose(MEM_new<NurbsGeometry>(__func__));
  for (const auto &point : points) {
    add_vertex(*loose, point);
  }
  const auto vertices = fill_vertices(*loose);
  filled = fill_components(*loose, {vertices, {}, {}});
  ASSERT_TRUE(filled.has_value());
  EXPECT_EQ(filled->created_edges.size(), 3);
  expect_fill_preview(*filled->geometry);
}

TEST_F(NurbsFillTest, PlanarConcaveBoundaryUsesTrimWithoutFlatteningOrTriangulatingSource)
{
  const double3 points[] = {{0, 0, 0}, {3, 0, 0}, {3, 3, 0}, {1, 1, 0}, {0, 3, 0}};
  const auto source = make_fill_wire({points, ARRAY_SIZE(points)}, true);
  const auto edges = fill_edges(*source);
  std::string error;
  auto filled = fill_components(*source, {{}, edges, {}}, &error);
  ASSERT_TRUE(filled.has_value()) << error;
  EXPECT_EQ(filled->geometry->faces.count(), 1);
  EXPECT_EQ(filled->geometry->edges.count(), 5);
  expect_fill_preview(*filled->geometry);
}

TEST_F(NurbsFillTest, ExactRationalCircleCapRetainsCurveKnotsAndWeights)
{
  const auto cylinder = make_arc_sheet(true);
  ASSERT_NE(cylinder, nullptr);
  const auto &edge = *static_cast<NurbsTopologyEdge *>(cylinder->edges.first);
  std::string error;
  auto filled = fill_components(*cylinder, {{}, {&edge.id, 1}, {}}, &error);
  ASSERT_TRUE(filled.has_value()) << error;
  EXPECT_EQ(filled->geometry->vertices.count(), cylinder->vertices.count());
  EXPECT_EQ(filled->geometry->edges.count(), cylinder->edges.count());
  EXPECT_EQ(filled->geometry->faces.count(), 2);
  expect_fill_preview(*filled->geometry);
  const auto index = TopologyIndex::build(*filled->geometry);
  EXPECT_EQ(index->uses_for_edge(edge.id).size(), 2);
  const auto &original = *static_cast<NurbsCurveGeometry *>(cylinder->curves.first);
  const auto &copied = *static_cast<NurbsCurveGeometry *>(filled->geometry->curves.first);
  EXPECT_EQ(Span(copied.knots, copied.knots_num), Span(original.knots, original.knots_num));
  for (int i = 0; i < original.points_num; i++) {
    EXPECT_EQ(copied.points[i].weight, original.points[i].weight);
    EXPECT_EQ(double3(copied.points[i].position), double3(original.points[i].position));
  }
}

TEST_F(NurbsFillTest, FillThenExtrudeSharesCapAndWallsAndCanFillDeletedTopAgain)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t face = static_cast<NurbsTopologyFace *>(source->faces.first)->id;
  std::string error;
  auto cube = extrude_components(*source, {{}, {}, {&face, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(cube.has_value()) << error;
  const int64_t top = cube->source_to_cap.lookup(face);
  const auto index = TopologyIndex::build(*cube->geometry);
  Vector<int64_t> top_edges;
  for (const NurbsTopologyEdgeUse &use :
       static_cast<NurbsTopologyLoop *>(index->face(top)->loops.first)->edge_uses)
  {
    top_edges.append(use.edge_id);
  }
  auto opened = delete_components(*cube->geometry, {{}, {}, {&top, 1}}, DeleteMode::FacesOnly);
  ASSERT_TRUE(opened.has_value());
  auto filled = fill_components(*opened->geometry, {{}, top_edges, {}}, &error);
  ASSERT_TRUE(filled.has_value()) << error;
  EXPECT_EQ(filled->geometry->vertices.count(), 8);
  EXPECT_EQ(filled->geometry->edges.count(), 12);
  EXPECT_EQ(filled->geometry->faces.count(), 6);
  const auto filled_index = TopologyIndex::build(*filled->geometry);
  for (const auto &edge : filled->geometry->edges) {
    const auto uses = filled_index->uses_for_edge(edge.id);
    ASSERT_EQ(uses.size(), 2);
    EXPECT_EQ(filled_index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
              -filled_index->face(uses[1].face_id)->orientation *
                  (uses[1].use->reversed ? -1 : 1));
  }
  auto continued = extrude_components(
      *filled->geometry, {{}, {}, filled->created_faces}, {0, 0, 1}, 0, &error);
  ASSERT_TRUE(continued.has_value()) << error;
  EXPECT_EQ(continued->geometry->vertices.count(), 12);
  EXPECT_EQ(continued->geometry->edges.count(), 20);
  EXPECT_EQ(continued->geometry->faces.count(), 10);
  expect_fill_preview(*continued->geometry);
}

/** BMesh is a test-only behavioral reference; source construction does not use it. */
TEST_F(NurbsFillTest, PlanarFillTopologyMatchesMeshContextualCreate)
{
  const double3 quad[] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
  const double3 triangle[] = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
  const double3 concave[] = {{0, 0, 0}, {3, 0, 0}, {3, 3, 0}, {1, 1, 0}, {0, 3, 0}};
  for (const Span<double3> points :
       {Span<double3>(quad, 4), Span<double3>(triangle, 3), Span<double3>(concave, 5)})
  {
    for (const bool closed : {false, true}) {
      const auto source = make_fill_wire(points, closed);
      const BMeshCreateParams params{.use_toolflags = true};
      std::unique_ptr<BMesh, decltype(&BM_mesh_free)> mesh(
          BM_mesh_create(&bm_mesh_allocsize_default, &params), BM_mesh_free);
      Vector<BMVert *> vertices;
      for (const auto &point : points) {
        const float3 co(point);
        vertices.append(BM_vert_create(mesh.get(), co, nullptr, BM_CREATE_NOP));
      }
      for (int i = 0; i < points.size() - (closed ? 0 : 1); i++) {
        BM_edge_create(
            mesh.get(), vertices[i], vertices[(i + 1) % points.size()], nullptr, BM_CREATE_NOP);
      }
      BMOperator op;
      BMO_op_initf(mesh.get(),
                   &op,
                   BMO_FLAG_DEFAULTS,
                   "contextual_create geom=%avef mat_nr=%i use_smooth=%b",
                   0,
                   false);
      BMO_op_exec(mesh.get(), &op);
      BMO_op_finish(mesh.get(), &op);
      std::string error;
      const auto filled = fill_components(*source, {{}, fill_edges(*source), {}}, &error);
      ASSERT_TRUE(filled.has_value()) << error;
      EXPECT_EQ(filled->geometry->vertices.count(), mesh->totvert);
      EXPECT_EQ(filled->geometry->edges.count(), mesh->totedge);
      EXPECT_EQ(filled->geometry->faces.count(), mesh->totface);
    }
  }
}

TEST_F(NurbsFillTest, PlanarFaceMergeRemovesInteriorEdgeAndPreservesBoundaryIdentities)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  const auto index = TopologyIndex::build(*sheet);
  int64_t side = 0;
  for (const NurbsTopologyEdge &edge : sheet->edges) {
    if (index->vertex(edge.vertex_ids[0])->position[0] == 1 &&
        index->vertex(edge.vertex_ids[1])->position[0] == 1)
    {
      side = edge.id;
    }
  }
  ASSERT_NE(side, 0);
  std::string error;
  auto adjacent = extrude_edges(*sheet, {&side, 1}, {2, 0, 0}, 0, &error);
  ASSERT_TRUE(adjacent.has_value()) << error;
  Vector<int64_t> faces;
  for (const NurbsTopologyFace &face : adjacent->geometry->faces) {
    faces.append(face.id);
  }
  ASSERT_EQ(faces.size(), 2);
  const auto region = plan_face_region_extrusion(*adjacent->geometry, faces);
  ASSERT_TRUE(region.has_value());
  const auto filled = fill_components(*adjacent->geometry, {{}, {}, faces}, &error);
  ASSERT_TRUE(filled.has_value()) << error;
  EXPECT_EQ(filled->geometry->vertices.count(), 6);
  EXPECT_EQ(filled->geometry->edges.count(), 6);
  EXPECT_EQ(filled->geometry->faces.count(), 1);
  EXPECT_TRUE(filled->removed.edges.contains(side));
  EXPECT_EQ(filled->removed.faces.size(), 2);
  const auto merged_index = TopologyIndex::build(*filled->geometry);
  for (const int64_t id : region->boundary_edges) {
    EXPECT_NE(merged_index->edge(id), nullptr);
  }
  expect_fill_preview(*filled->geometry);
  const auto swept = extrude_components(
      *filled->geometry, {{}, {}, filled->created_faces}, {0, 0, 1}, 0, &error);
  ASSERT_TRUE(swept.has_value()) << error;
  EXPECT_EQ(swept->geometry->faces.count(), 8);
}

TEST_F(NurbsFillTest, FillRejectsIdentityExhaustionAndUnsupportedCurveDomainsAtomically)
{
  const double3 points[] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
  auto source = make_fill_wire({points, 4}, true);
  const auto edges = fill_edges(*source);
  const int64_t next_id = source->next_id;
  source->next_id = std::numeric_limits<int64_t>::max();
  std::string error;
  EXPECT_FALSE(fill_components(*source, {{}, edges, {}}, &error).has_value());
  EXPECT_NE(error.find("exhausted"), std::string::npos);
  EXPECT_TRUE(source->faces.is_empty());
  EXPECT_EQ(source->next_id, std::numeric_limits<int64_t>::max());
  source->next_id = next_id;
  auto *curve = static_cast<NurbsCurveGeometry *>(source->curves.first);
  curve->points[0].weight = -1;
  EXPECT_FALSE(fill_components(*source, {{}, edges, {}}, &error).has_value());
  EXPECT_NE(error.find("positive"), std::string::npos);
  curve->points[0].weight = 1;
  auto *edge = static_cast<NurbsTopologyEdge *>(source->edges.first);
  edge->parameter_range[0] = 0.25;
  EXPECT_FALSE(fill_components(*source, {{}, edges, {}}, &error).has_value());
  EXPECT_NE(error.find("full clamped"), std::string::npos);
  EXPECT_EQ(source->next_id, next_id);
  EXPECT_TRUE(source->faces.is_empty());
}

TEST_F(NurbsFillTest, MultipleBoundariesAreTracedSeparatelyAndNeverJoinedByPosition)
{
  const double3 points[] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
  auto source = make_fill_wire({points, 4}, true);
  const auto copy = duplicate_components(*source, {{}, fill_edges(*source), {}});
  ASSERT_TRUE(copy.has_value());
  const auto edges = fill_edges(*copy->geometry);
  const auto traced = trace_edge_chains(*copy->geometry, edges);
  ASSERT_TRUE(traced.has_value());
  ASSERT_EQ(traced->size(), 2);
  EXPECT_TRUE((*traced)[0].closed);
  EXPECT_TRUE((*traced)[1].closed);
  std::string error;
  EXPECT_FALSE(fill_components(*copy->geometry, {{}, edges, {}}, &error).has_value());
  EXPECT_NE(error.find("one boundary"), std::string::npos);
  EXPECT_EQ(copy->geometry->vertices.count(), 8);
}

TEST_F(NurbsFillTest, UnsupportedBoundariesFailWithoutSourceOrCounterMutation)
{
  const double3 crossing[] = {{0, 0, 0}, {2, 2, 0}, {0, 2, 0}, {2, 0, 0}};
  const double3 nonplanar[] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 1}, {1, 3, 0}, {0, 2, 0}};
  for (const Span<double3> points : {Span<double3>(crossing, 4), Span<double3>(nonplanar, 5)}) {
    const auto source = make_fill_wire(points, true);
    const int64_t next_id = source->next_id;
    std::string error;
    EXPECT_FALSE(fill_components(*source, {{}, fill_edges(*source), {}}, &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(source->next_id, next_id);
    EXPECT_TRUE(source->faces.is_empty());
  }
  const auto source = make_fill_wire({crossing, 4}, false);
  source->next_id = std::numeric_limits<int64_t>::max();
  std::string error;
  EXPECT_FALSE(fill_components(*source, {{}, {&source->next_id, 1}, {}}, &error).has_value());
  EXPECT_NE(error.find("identity"), std::string::npos);
}

}  // namespace blender::bke::nurbs::tests
