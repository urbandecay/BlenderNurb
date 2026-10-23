/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_mesh.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_transform.hh"

#include "nurbs_test_geometry.hh"

namespace blender::bke::nurbs::tests {

class NurbsMeshEvaluationTest : public BlenderGTestBase {};

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

TEST_F(NurbsMeshEvaluationTest, CollapsedEditingSurfaceRecordsUndefinedSourceNormals)
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
  EXPECT_GT(result->mesh->faces_num, 0);
  EXPECT_EQ(result->undefined_normal_corners, result->mesh->corners_num);
  EXPECT_EQ(edit->geometry().faces.count(), 1);
  EXPECT_EQ(edit->geometry().vertices.count(), 4);
}

}  // namespace blender::bke::nurbs::tests
