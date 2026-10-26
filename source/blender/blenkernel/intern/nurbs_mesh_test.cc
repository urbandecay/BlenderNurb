/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#ifdef WITH_GMP
#  include "BLI_math_mpq.hh"
#endif

#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_selection.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_nurbs_transform.hh"

#include "nurbs_test_geometry.hh"

namespace blender::bke::nurbs::tests {

class NurbsMeshEvaluationTest : public BlenderGTestBase {};

TEST_F(NurbsMeshEvaluationTest, RectangularSurfaceShowsAQuadGridAtDifferentResolutions)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  const auto snapshot = edit->read_snapshot();
  for (const int resolution : {1, 4, 7, 12, 13}) {
    SCOPED_TRACE(resolution);
    auto options = preview_options();
    options.boundary.segments_per_span = options.interior_subdivisions = resolution;
    std::string error;
    const auto result = evaluate_mesh(snapshot, options, &error);
    ASSERT_TRUE(result.has_value()) << error;
    const Mesh &mesh = *result->mesh;
    const auto &visible = mesh.runtime->subsurf_optimal_display_edges;
    ASSERT_EQ(visible.size(), mesh.edges_num);
    int wires = 0;
    for (const int i : mesh.edges().index_range()) {
      if (visible[i]) {
        const int2 edge = mesh.edges()[i];
        const float3 a = mesh.vert_positions()[edge.x], b = mesh.vert_positions()[edge.y];
        /* Source evaluation and the float display conversion can round the two ends of an
         * isoparametric segment differently, particularly around zero at nonbinary samples. */
        EXPECT_LE(std::min(std::abs(a.x - b.x), std::abs(a.y - b.y)), 1.0e-6f);
        wires++;
      }
    }
    EXPECT_EQ(wires, 2 * resolution * (resolution + 1));
    EXPECT_EQ(mesh.faces_num, 2 * resolution * resolution);
    EXPECT_EQ(mesh.edges_num - wires, resolution * resolution);
    EXPECT_EQ(result->source, snapshot);
    EXPECT_EQ(snapshot.geometry().faces.count(), 1);
    EXPECT_EQ(snapshot.geometry().edges.count(), 4);
    OwnedMesh copy(BKE_mesh_copy_for_eval(mesh));
    ASSERT_EQ(copy->runtime->subsurf_optimal_display_edges.size(), visible.size());
    for (const int i : visible.index_range()) {
      EXPECT_EQ(bool(copy->runtime->subsurf_optimal_display_edges[i]), bool(visible[i]));
    }
    copy->tag_edges_split();
    EXPECT_TRUE(copy->runtime->subsurf_optimal_display_edges.is_empty());
  }
}

TEST_F(NurbsMeshEvaluationTest, BilinearDiagonalGapUsesItsCoefficientBoundWithoutWideningTolerance)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto &face = *static_cast<NurbsTopologyFace *>(source->faces.first);
  for (auto &loop : face.loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&face.loops);
  const double2 corners[] = {{0, 0}, {1, 0}, {0, 1}};
  add_planar_loop(*source, face, {corners, 3}, false);
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
  surface.points[3].position[2] = 0.125;
  std::string error;
  // A real unsampled midpoint gap still fails; this is not an evaluator epsilon.
  EXPECT_FALSE(evaluate_boundaries(*source, {1, 1000, 0}, &error));
  EXPECT_NE(error.find("edge/support"), std::string::npos);
  face.tolerance = std::nextafter(0.03125, 0.0);
  EXPECT_FALSE(evaluate_boundaries(*source, {7, 1000, 0}, &error));
  face.tolerance = 0.03125;
  const auto result = evaluate_boundaries(*source, {7, 1000, 0}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  int diagonals = 0;
  for (const auto &use : result->uses) {
    ASSERT_TRUE(use.curve_support_deviation_bound.has_value());
    if (*use.curve_support_deviation_bound != 0) {
      EXPECT_EQ(*use.curve_support_deviation_bound, 0.03125);
      EXPECT_FALSE(use.exact_curve_support_identity);
      EXPECT_GT(use.max_curve_support_deviation, 0);
      EXPECT_LE(use.max_curve_support_deviation, *use.curve_support_deviation_bound);
      diagonals++;
    }
  }
  EXPECT_EQ(diagonals, 1);
  EXPECT_EQ(face.tolerance, 0.03125);
  EXPECT_EQ(surface.points[3].position[2], 0.125);
}

TEST_F(NurbsMeshEvaluationTest, CollapsedBoxKeepsQuadBaseAndTriangularWallGrids)
{
  auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  const int64_t bottom = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  SelectionState initial;
  initial.mode = SelectionMode::Face;
  const auto selection = selection_apply(
      *box->geometry, initial, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(selection.has_value()) << error;
  const auto merged = merge_selected_vertices(
      *box->geometry, *selection, VertexMergeMode::Collapse, {}, 0, &error);
  ASSERT_TRUE(merged.has_value()) << error;
  const auto edit = EditData::create(*merged->geometry);
  ASSERT_NE(edit, nullptr);
  const auto snapshot = edit->read_snapshot();
  auto options = preview_options();
  options.boundary.segments_per_span = options.interior_subdivisions = 7;
  options.boundary.evaluation_allowance = 0;
  const auto result = evaluate_mesh(snapshot, options, &error);
  ASSERT_TRUE(result.has_value()) << error;
  for (const auto &use : result->preview.boundary.uses) {
    EXPECT_TRUE(use.exact_curve_support_identity);
  }
  for (const auto &face : snapshot.geometry().faces) {
    EXPECT_EQ(face.tolerance, 0);
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        EXPECT_EQ(use.tolerance, 0);
      }
    }
  }
  Set<int64_t> triangular_faces;
  int hidden_base_diagonals = 0;
  for (const auto &triangle : result->preview.triangles) {
    for (const int i : IndexRange(3)) {
      if (triangle.face_id == bottom) {
        hidden_base_diagonals += !triangle.display_edges[i];
      }
      else {
        EXPECT_TRUE(triangle.display_edges[i]);
        triangular_faces.add(triangle.face_id);
      }
    }
  }
  EXPECT_EQ(triangular_faces.size(), 4);
  EXPECT_EQ(hidden_base_diagonals, 2 * 7 * 7);
  const auto &visible = result->mesh->runtime->subsurf_optimal_display_edges;
  const auto corners = result->mesh->corner_edges();
  for (const int face : result->mesh_to_preview_triangle.index_range()) {
    const auto &triangle = result->preview.triangles[result->mesh_to_preview_triangle[face]];
    for (const int corner : IndexRange(3)) {
      EXPECT_EQ(bool(visible[corners[3 * face + corner]]), triangle.display_edges[corner]);
    }
  }
  EXPECT_EQ(snapshot.geometry().vertices.count(), 5);
  EXPECT_EQ(snapshot.geometry().edges.count(), 8);
  EXPECT_EQ(snapshot.geometry().faces.count(), 5);
  const auto deleted = delete_components(
      *merged->geometry, {merged->vertices, {}, {}}, DeleteMode::Vertices, &error);
  ASSERT_TRUE(deleted.has_value()) << error;
  EXPECT_EQ(deleted->geometry->faces.count(), 1);
  const auto remaining = EditData::create(*deleted->geometry);
  ASSERT_NE(remaining, nullptr);
  const auto remaining_mesh = evaluate_mesh(remaining->read_snapshot(), options, &error);
  ASSERT_TRUE(remaining_mesh.has_value()) << error;
  int hidden = 0;
  for (const int i : remaining_mesh->mesh->runtime->subsurf_optimal_display_edges.index_range()) {
    hidden += !remaining_mesh->mesh->runtime->subsurf_optimal_display_edges[i];
  }
  EXPECT_EQ(hidden, 7 * 7);
  /* A tiny real gap invalidates the identity proof and still fails at zero source tolerance. */
  OwnedGeometry broken(geometry_copy(&snapshot.geometry()));
  const auto index = TopologyIndex::build(*broken);
  ASSERT_TRUE(index.has_value());
  int64_t side_surface = 0;
  for (const auto &face : broken->faces) {
    if (face.id != bottom) {
      side_surface = face.surface_id;
      break;
    }
  }
  ASSERT_NE(side_surface, 0);
  for (auto &surface : broken->surfaces) {
    if (surface.id == side_surface) {
      surface.points[3].position[2] += std::ldexp(1.0, -40);
      break;
    }
  }
  EXPECT_FALSE(evaluate_boundaries(*broken, options.boundary, &error));
  EXPECT_NE(error.find("edge/support"), std::string::npos);
}

TEST_F(NurbsMeshEvaluationTest, SplittingABoundaryEdgeDoesNotTurnARectangularGridIntoTriangles)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t edge = static_cast<const NurbsTopologyEdge *>(source->edges.first)->id;
  TopologyCutPlan plan;
  plan.edges.append({edge, {0.5}});
  std::string error;
  const auto split = partition_components(*source, plan, 0, &error);
  ASSERT_TRUE(split.has_value()) << error;
  ASSERT_EQ(split->edit.geometry->faces.count(), 1);
  ASSERT_EQ(split->edit.geometry->edges.count(), 5);
  const auto edit = EditData::create(*split->edit.geometry);
  ASSERT_NE(edit, nullptr);
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  const Mesh &mesh = *result->mesh;
  const auto &visible = mesh.runtime->subsurf_optimal_display_edges;
  int wires = 0;
  for (const int i : mesh.edges().index_range()) {
    if (visible[i]) {
      const int2 wire = mesh.edges()[i];
      const float3 a = mesh.vert_positions()[wire.x], b = mesh.vert_positions()[wire.y];
      EXPECT_TRUE(a.x == b.x || a.y == b.y);
      wires++;
    }
  }
  /* 4x4 grid, with four extra sample intervals on the split source boundary. */
  EXPECT_EQ(wires, 2 * 4 * 5 + 4);
}

TEST_F(NurbsMeshEvaluationTest, SheetMeshRetainsSourceFaceProvenanceAndIndependentGeometry)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  static_cast<NurbsTopologyFace *>(source->faces.first)->material_index = 2;
  const auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot snapshot = edit->read_snapshot();
  std::string error;
  auto result = evaluate_mesh(snapshot, preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  ASSERT_NE(result->mesh, nullptr);
  EXPECT_EQ(result->source, snapshot);
  EXPECT_EQ(result->mesh->verts_num, result->preview.points_num());
  EXPECT_EQ(result->mesh->faces_num, result->preview.triangles.size());
  EXPECT_GT(result->mesh->faces_num, 1);
  EXPECT_EQ(result->mesh->corners_num, 3 * result->mesh->faces_num);
  const auto materials = result->mesh->attributes().lookup<int>("material_index",
                                                                AttrDomain::Face);
  const auto sharp = result->mesh->attributes().lookup<bool>("sharp_face", AttrDomain::Face);
  ASSERT_TRUE(materials);
  ASSERT_TRUE(sharp);
  for (const int i : IndexRange(result->mesh->faces_num)) {
    EXPECT_EQ(materials.varray[i], 2);
    EXPECT_FALSE(sharp.varray[i]);
  }
  EXPECT_DOUBLE_EQ(result->max_float_position_deviation, 0);
  EXPECT_EQ(result->undefined_normal_corners, 0);
  for (const float3 normal : result->mesh->corner_normals()) {
    EXPECT_NEAR(normal.x, 0, 1.0e-5);
    EXPECT_NEAR(normal.y, 0, 1.0e-5);
    EXPECT_NEAR(normal.z, 1, 1.0e-5);
  }
  result->mesh->vert_positions_for_write()[0].x = 42;
  EXPECT_EQ(
      static_cast<const NurbsTopologyVertex *>(snapshot.geometry().vertices.first)->position[0],
      -1);
  EXPECT_EQ(snapshot.geometry().faces.count(), 1);
  EXPECT_EQ(snapshot.geometry().edges.count(), 4);
}

TEST_F(NurbsMeshEvaluationTest, FloatQuantizationIsReportedWithoutRoundingAuthoritativeDoubles)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 matrix = double4x4::identity();
  matrix.location().x = 1.0e12 + 0.125;
  auto translated = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(translated.has_value());
  const auto edit = EditData::create(*translated->geometry);
  ASSERT_NE(edit, nullptr);
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_GT(result->max_float_position_deviation, 4000);
  EXPECT_LT(result->max_float_position_deviation, 5000);
  const double original =
      static_cast<const NurbsTopologyVertex *>(edit->geometry().vertices.first)->position[0];
  EXPECT_DOUBLE_EQ(original, 1.0e12 - 0.875);
  EXPECT_NE(double(result->mesh->vert_positions()[0].x), original);
  EXPECT_DOUBLE_EQ(result->preview.position(0).x, original);
  EXPECT_EQ(result->mesh->faces_num, 0);
  EXPECT_EQ(result->skipped_degenerate_float_triangles, result->preview.triangles.size());
  EXPECT_TRUE(result->mesh_to_preview_triangle.is_empty());
  EXPECT_EQ(edit->revision(), 1);
}

TEST_F(NurbsMeshEvaluationTest, FloatOverflowFailsExplicitlyWithSourceAndSelectionUnchanged)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 matrix = double4x4::identity();
  matrix[0][0] = 1.0e40;
  auto scaled = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(scaled.has_value());
  const auto edit = EditData::create(*scaled->geometry);
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot snapshot = edit->read_snapshot();
  const int64_t counter = snapshot.geometry().next_id;
  std::string error;
  EXPECT_FALSE(evaluate_mesh(snapshot, preview_options(), &error).has_value());
  EXPECT_NE(error.find("float display"), std::string::npos);
  EXPECT_EQ(edit->read_snapshot(), snapshot);
  EXPECT_EQ(edit->geometry().next_id, counter);
  EXPECT_EQ(edit->geometry().faces.count(), 1);
}

TEST_F(NurbsMeshEvaluationTest, WireDisplayRetainsCoincidentVerticesWithDistinctSourceIDs)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*source, {-1, 0, 0});
  const auto *b = add_vertex(*source, {1, 0, 0});
  const auto *coincident = add_vertex(*source, {-1, 0, 0});
  const auto *curve = add_linear_curve(*source, double3(a->position), double3(b->position), 3);
  add_edge(*source, *a, *b, *curve);
  add_edge(*source, *coincident, *b, *curve);
  auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot snapshot = edit->read_snapshot();
  edit.reset();
  auto options = preview_options();
  options.boundary.segments_per_span = 1;
  std::string error;
  const auto result = evaluate_mesh(snapshot, options, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->mesh->verts_num, 3);
  EXPECT_EQ(result->mesh->edges_num, 2);
  EXPECT_EQ(result->mesh->faces_num, 0);
  EXPECT_EQ(result->mesh->vert_positions()[0], result->mesh->vert_positions()[2]);
  EXPECT_NE(result->preview.boundary.vertex_ids[0], result->preview.boundary.vertex_ids[2]);
  EXPECT_EQ(result->source, snapshot);
}

TEST_F(NurbsMeshEvaluationTest, RationalCylinderUsesRadialSourceNormalsAtDisplayCorners)
{
  const auto source = make_arc_sheet(true);
  ASSERT_NE(source, nullptr);
  const auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->undefined_normal_corners, 0);
  const auto normals = result->mesh->corner_normals();
  const auto positions = result->mesh->vert_positions();
  const auto corners = result->mesh->corner_verts();
  ASSERT_EQ(normals.size(), corners.size());
  for (const int i : corners.index_range()) {
    const float3 point = positions[corners[i]];
    const float3 radial = math::normalize(float3(point.x, point.y, 0));
    EXPECT_GT(math::dot(normals[i], radial), 0.9999f);
    EXPECT_LT(std::abs(normals[i].z), 1.0e-3f);
  }
  EXPECT_EQ(source->faces.count(), 1);
  EXPECT_EQ(source->vertices.count(), 2);
  const auto &visible = result->mesh->runtime->subsurf_optimal_display_edges;
  int hidden = 0;
  for (const int i : visible.index_range()) {
    hidden += !visible[i];
  }
  /* Four rational knot spans around the seam, each with four U and four V subdivisions. */
  EXPECT_EQ(hidden, 4 * 4 * 4);
  EXPECT_EQ(visible.size() - hidden, 16 * 5 + 16 * 4);
}

TEST_F(NurbsMeshEvaluationTest, SourceNormalsPreserveG0FaceJoinWithoutAveragingItsTangents)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t original_face = static_cast<NurbsTopologyFace *>(source->faces.first)->id;
  const int64_t common_edge = static_cast<NurbsTopologyEdge *>(source->edges.first)->id;
  auto swept = extrude_edges(*source, {common_edge}, {0, 0, 2}, 0);
  ASSERT_TRUE(swept.has_value());
  source = std::move(swept->geometry);
  const auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->undefined_normal_corners, 0);
  const auto normals = result->mesh->corner_normals();
  for (const int i : result->preview.triangles.index_range()) {
    const bool horizontal = result->preview.triangles[i].face_id == original_face;
    for (const int j : IndexRange(3)) {
      const float3 normal = normals[i * 3 + j];
      if (horizontal) {
        EXPECT_NEAR(normal.z, 1, 1.0e-4f);
        EXPECT_NEAR(normal.y, 0, 1.0e-4f);
      }
      else {
        EXPECT_NEAR(std::abs(normal.y), 1, 1.0e-4f);
        EXPECT_NEAR(normal.z, 0, 1.0e-4f);
      }
    }
  }
  const auto index = TopologyIndex::build(*source);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->uses_for_edge(common_edge).size(), 2);
  EXPECT_EQ(source->faces.count(), 2);
  EXPECT_EQ(source->vertices.count(), 6);
}

TEST_F(NurbsMeshEvaluationTest, CollapsedEditingSurfaceRetainsPreviewWithoutDegenerateDisplayFaces)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 matrix = double4x4::identity();
  matrix[1][1] = 0;
  const auto collapsed = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(collapsed.has_value());
  const auto edit = EditData::create(*collapsed->geometry);
  ASSERT_NE(edit, nullptr);
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_FALSE(result->preview.triangles.is_empty());
  EXPECT_EQ(result->mesh->faces_num, 0);
  EXPECT_EQ(result->skipped_degenerate_float_triangles, result->preview.triangles.size());
  EXPECT_TRUE(result->mesh_to_preview_triangle.is_empty());
  EXPECT_GT(result->mesh->edges_num, 0);
  EXPECT_EQ(result->undefined_normal_corners, 0);
  EXPECT_EQ(edit->geometry().faces.count(), 1);
  EXPECT_EQ(edit->geometry().vertices.count(), 4);
}

TEST_F(NurbsMeshEvaluationTest, PyramidDisplayNormalsMatchMeshFacesAfterMovingItsApex)
{
  const auto sheet = make_planar_sheet();
  const int64_t bottom = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  SelectionState initial;
  initial.mode = SelectionMode::Face;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  const auto selected = selection_apply(
      *box->geometry, initial, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(selected.has_value()) << error;
  const auto pyramid = merge_selected_vertices(
      *box->geometry, *selected, VertexMergeMode::Center, {}, 0, &error);
  ASSERT_TRUE(pyramid.has_value()) << error;
  int skipped = 0;
  // Blender's modal transform adapter supplies float matrices, including typed decimal input.
  for (const double offset : {0.0, 0.5, double(0.3f)}) {
    const auto moved = translate_vertices(
        *pyramid->geometry, pyramid->vertices, {offset, 0, 0}, 1e-9, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    const auto before = OwnedGeometry(geometry_copy(moved->geometry.get()));
    const auto edit = EditData::create(*moved->geometry);
    ASSERT_NE(edit, nullptr);
    for (const int resolution : {1, 4, 7, 11, 23, 32}) {
      SCOPED_TRACE(offset);
      SCOPED_TRACE(resolution);
      auto options = preview_options();
      options.boundary.segments_per_span = options.interior_subdivisions = resolution;
      options.boundary.evaluation_allowance = 0;
      const auto result = evaluate_mesh(edit->read_snapshot(), options, &error);
      ASSERT_TRUE(result.has_value()) << error;
      const auto &mesh = *result->mesh;
      EXPECT_EQ(mesh.faces_num, result->mesh_to_preview_triangle.size());
      EXPECT_EQ(mesh.faces_num + result->skipped_degenerate_float_triangles,
                result->preview.triangles.size());
      EXPECT_EQ(result->undefined_normal_corners, 0);
      const auto normals = mesh.corner_normals();
      const auto face_normals = mesh.face_normals();
      for (const int i : result->mesh_to_preview_triangle.index_range()) {
        SCOPED_TRACE(i);
        const int original = result->mesh_to_preview_triangle[i];
        EXPECT_TRUE(i == 0 || original > result->mesh_to_preview_triangle[i - 1]);
        const auto &triangle = result->preview.triangles[original];
        for (const int j : IndexRange(3)) {
          EXPECT_EQ(mesh.corner_verts()[i * 3 + j], triangle.points[j]);
          // This planar body's corner shading must agree with Mesh's geometric face normals.
          EXPECT_GT(math::dot(normals[i * 3 + j], face_normals[i]), 0.9999f);
          EXPECT_NEAR(math::length(normals[i * 3 + j]), 1, 1e-4f);
        }
      }
      skipped += result->skipped_degenerate_float_triangles;
      expect_source_geometry_equal(edit->geometry(), *before);
    }
  }
  EXPECT_GT(skipped, 0) << "The regression must exercise triangles lost in float conversion";
}

TEST_F(NurbsMeshEvaluationTest, ThinRepresentableDisplayFacesAreNotDiscardedByAnAreaEpsilon)
{
  const auto source = make_planar_sheet();
  auto matrix = double4x4::identity();
  matrix[1][1] = 1e-20;
  const auto thin = transform_geometry(*source, matrix, 1e-30);
  ASSERT_TRUE(thin.has_value());
  const auto edit = EditData::create(*thin->geometry);
  ASSERT_NE(edit, nullptr);
  auto options = preview_options();
  options.boundary.segments_per_span = options.interior_subdivisions = 1;
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), options, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->mesh->faces_num, 2);
  EXPECT_EQ(result->skipped_degenerate_float_triangles, 0);
}

TEST_F(NurbsMeshEvaluationTest,
       DecimalPyramidMovesKeepCertifiedBoundariesWithoutAnEvaluationAllowance)
{
  bool sampled_roundoff_exceeds_tolerance = false;
  const auto sheet = make_planar_sheet();
  const int64_t bottom = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  SelectionState initial;
  initial.mode = SelectionMode::Face;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  const auto selected = selection_apply(
      *box->geometry, initial, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(selected.has_value()) << error;
  const auto pyramid = merge_selected_vertices(
      *box->geometry, *selected, VertexMergeMode::Center, {}, 0, &error);
  ASSERT_TRUE(pyramid.has_value()) << error;
  for (const double offset : {0.3, 0.1, 1.0 / 3.0, -0.3, std::nextafter(0.3, 1.0)}) {
    SCOPED_TRACE(offset);
    const auto moved = translate_vertices(
        *pyramid->geometry, pyramid->vertices, {offset, 0, 0}, 1e-9, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    const auto edit = EditData::create(*moved->geometry);
    ASSERT_NE(edit, nullptr);
    const auto before = OwnedGeometry(geometry_copy(moved->geometry.get()));
    const auto index = TopologyIndex::build(edit->geometry());
    ASSERT_TRUE(index.has_value());
    Map<int64_t, double> tolerances;
    for (const auto &face : edit->geometry().faces) {
      for (const auto &loop : face.loops) {
        for (const auto &use : loop.edge_uses) {
          tolerances.add_new(
              use.id,
              std::max({face.tolerance, use.tolerance, index->edge(use.edge_id)->tolerance}));
        }
      }
    }
    for (const int resolution : {1, 7, 11, 23}) {
      SCOPED_TRACE(resolution);
      SurfacePreviewOptions options;
      options.boundary.segments_per_span = options.interior_subdivisions = resolution;
      ASSERT_EQ(options.boundary.evaluation_allowance, 0);
      const auto result = evaluate_mesh(edit->read_snapshot(), options, &error);
      ASSERT_TRUE(result.has_value()) << error;
      for (const auto &use : result->preview.boundary.uses) {
        ASSERT_TRUE(use.curve_support_deviation_bound.has_value());
        const double tolerance = tolerances.lookup(use.use_id);
        EXPECT_LE(*use.curve_support_deviation_bound, tolerance);
        sampled_roundoff_exceeds_tolerance |= use.max_curve_support_deviation > tolerance;
      }
      expect_source_geometry_equal(edit->geometry(), *before);
    }
  }
  EXPECT_TRUE(sampled_roundoff_exceeds_tolerance)
      << "The regression must retain a measured discrepancy exceeding its certified source gap";
}

#ifdef WITH_GMP
TEST_F(NurbsMeshEvaluationTest, PolynomialBoundaryBoundCoversExactRationalWitnesses)
{
  struct Case {
    double origin, size, low, high;
  };
  const Case cases[] = {{0, 1, -0.3, 1.7},
                        {1e12, 1, -0.3, 1.7},
                        {0, 1e-200, -0.3, 1.7},
                        {0, 1e-320, -0.3, 1.7},
                        {0, 1, -1e-300, 1e-300},
                        {0, 1e200, -1e200, 1e200}};
  for (const auto &item : cases) {
    SCOPED_TRACE(item.origin);
    SCOPED_TRACE(item.size);
    SCOPED_TRACE(item.low);
    auto source = make_planar_sheet();
    auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
    for (int i = 0; i < 4; i++) {
      surface.knots_u[i] = surface.knots_v[i] = i < 2 ? item.low : item.high;
      for (int axis = 0; axis < 3; axis++) {
        surface.points[i].position[axis] = item.origin +
                                           item.size * (surface.points[i].position[axis] +
                                                        0.1 * (i + axis));
      }
    }
    surface.points[3].position[2] += item.size * 0.125;
    const double3 uv[] = {
        {std::lerp(item.low, item.high, 0.1), std::lerp(item.low, item.high, 0.2), 0},
        {std::lerp(item.low, item.high, 0.8), std::lerp(item.low, item.high, 0.7), 0}};
    std::string error;
    const auto a = evaluate_surface(surface, double2(uv[0]), &error);
    const auto b = evaluate_surface(surface, double2(uv[1]), &error);
    ASSERT_TRUE(a.has_value()) << error;
    ASSERT_TRUE(b.has_value()) << error;
    const auto *curve = add_linear_curve(*source, a->position, b->position, 3);
    const auto *pcurve = add_linear_curve(*source, uv[0], uv[1], 2);
    const auto bound = linear_curve_support_deviation_bound(*curve, *pcurve, surface);
    ASSERT_TRUE(bound.has_value());
    EXPECT_GT(*bound, 0);
    const mpq_class exact_bound(*bound);
    for (int sample = 0; sample <= 100; sample++) {
      const mpq_class t(sample, 100);
      mpq_class parameters[2];
      for (int axis = 0; axis < 2; axis++) {
        const mpq_class coordinate = (1 - t) * mpq_class(uv[0][axis]) + t * mpq_class(uv[1][axis]);
        parameters[axis] = (coordinate - mpq_class(item.low)) /
                           (mpq_class(item.high) - mpq_class(item.low));
      }
      mpq_class difference_l1(0);
      for (int axis = 0; axis < 3; axis++) {
        mpq_class point(0);
        for (int corner = 0; corner < 4; corner++) {
          const mpq_class u = corner & 1 ? parameters[0] : 1 - parameters[0];
          const mpq_class v = corner & 2 ? parameters[1] : 1 - parameters[1];
          point += u * v * mpq_class(surface.points[corner].position[axis]);
        }
        const mpq_class difference = point -
                                     ((1 - t) * mpq_class(curve->points[0].position[axis]) +
                                      t * mpq_class(curve->points[1].position[axis]));
        difference_l1 += difference < 0 ? -difference : difference;
      }
      EXPECT_TRUE(difference_l1 <= exact_bound) << sample;
    }
  }
}
#endif

TEST_F(NurbsMeshEvaluationTest, InteriorTrimGapIsNotHiddenByThePolynomialCertificate)
{
  auto source = make_planar_sheet();
  auto &face = *static_cast<NurbsTopologyFace *>(source->faces.first);
  for (auto &loop : face.loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&face.loops);
  const double2 corners[] = {{0.125, 0.25}, {0.875, 0.25}, {0.25, 0.75}};
  add_planar_loop(*source, face, {corners, 3}, false);
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
  surface.points[3].position[2] = 0.125;
  const auto before = OwnedGeometry(geometry_copy(source.get()));
  std::string error;
  EXPECT_FALSE(evaluate_boundaries(*source, {7, 1000, 0}, &error));
  EXPECT_NE(error.find("edge/support"), std::string::npos);
  expect_source_geometry_equal(*source, *before);
  const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
  const auto &use = *static_cast<const NurbsTopologyEdgeUse *>(loop.edge_uses.first);
  Map<int64_t, const NurbsCurveGeometry *> curves;
  for (const auto &curve : source->curves) {
    curves.add_new(curve.id, &curve);
  }
  const auto index = TopologyIndex::build(*source);
  const auto &curve = *curves.lookup(index->edge(use.edge_id)->curve_id);
  const auto &pcurve = *curves.lookup(use.pcurve_id);
  const auto bound = linear_curve_support_deviation_bound(curve, pcurve, surface);
  ASSERT_TRUE(bound.has_value());
  EXPECT_GT(*bound, 0);
  // Rational support weights and incompatible curve parameterizations need their own proofs.
  surface.points[3].weight = 2;
  EXPECT_FALSE(linear_curve_support_deviation_bound(curve, pcurve, surface));
  surface.points[3].weight = 1;
  for (auto &item : source->curves) {
    if (item.id == pcurve.id) {
      item.knots[2] = item.knots[3] = 2;
    }
  }
  EXPECT_FALSE(linear_curve_support_deviation_bound(curve, pcurve, surface));
}

TEST_F(NurbsMeshEvaluationTest, UnrepresentableBoundaryCertificateDoesNotClaimAgreement)
{
  auto source = make_planar_sheet();
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
  for (int i = 0; i < 4; i++) {
    surface.knots_u[i] = surface.knots_v[i] = i < 2 ? -1e308 : 1e308;
  }
  const auto *curve = add_linear_curve(*source, {-1, -1, 0}, {1, 1, 0}, 3);
  const auto *pcurve = add_linear_curve(*source, {-5e307, -5e307, 0}, {5e307, 5e307, 0}, 2);
  EXPECT_FALSE(linear_curve_support_deviation_bound(*curve, *pcurve, surface));
}

TEST_F(NurbsMeshEvaluationTest, PolynomialCertificateRequiresParameterRolesAndContainedTrims)
{
  auto source = make_planar_sheet();
  const auto &surface = *static_cast<const NurbsSurfaceGeometry *>(source->surfaces.first);
  auto *curve = add_linear_curve(*source, {-0.5, -0.5, 0}, {0.5, 0.5, 0}, 3);
  auto *pcurve = add_linear_curve(*source, {0.25, 0.25, 0}, {0.75, 0.75, 0}, 2);
  ASSERT_EQ(linear_curve_support_deviation_bound(*curve, *pcurve, surface), 0.0);
  curve->dimension = 2;
  EXPECT_FALSE(linear_curve_support_deviation_bound(*curve, *pcurve, surface));
  curve->dimension = 3;
  pcurve->dimension = 3;
  EXPECT_FALSE(linear_curve_support_deviation_bound(*curve, *pcurve, surface));
  pcurve->dimension = 2;
  pcurve->points[1].position[0] = 1.25;
  EXPECT_FALSE(linear_curve_support_deviation_bound(*curve, *pcurve, surface));
}

}  // namespace blender::bke::nurbs::tests
