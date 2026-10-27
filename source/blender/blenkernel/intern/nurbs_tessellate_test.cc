/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_boolean.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_tessellate.hh"
#include "BKE_nurbs_transform.hh"

#include "testing/testing.h"

#include "nurbs_test_geometry.hh"

namespace blender::bke::nurbs::tests {

TEST(nurbs_surface_preview, PlanarSheetRetainsSourceFaceAndBoundaryIdentity)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  std::string error;
  const int64_t counter = source->next_id;
  const auto preview = tessellate_surfaces(*source, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  EXPECT_NEAR(planar_area(*preview), 4, 1.0e-12);
  EXPECT_EQ(preview->boundary.edges.size(), 4);
  EXPECT_FALSE(preview->interior.is_empty());
  const auto *face = static_cast<const NurbsTopologyFace *>(source->faces.first);
  for (const auto &triangle : preview->triangles) {
    EXPECT_EQ(triangle.face_id, face->id);
    EXPECT_GT(orient2d(triangle.parameters[0], triangle.parameters[1], triangle.parameters[2]), 0);
  }
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(source->faces.count(), 1);
  EXPECT_EQ(source->edges.count(), 4);
}

TEST(nurbs_surface_preview, DisplayGridFollowsEachFaceChartIncludingReversedOrientation)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto *face = static_cast<NurbsTopologyFace *>(source->faces.first);
  for (const int orientation : {1, -1}) {
    face->orientation = orientation;
    std::string error;
    const auto preview = tessellate_surfaces(*source, preview_options(), &error);
    ASSERT_TRUE(preview.has_value()) << error;
    int hidden = 0;
    for (const auto &triangle : preview->triangles) {
      int sides = 0;
      for (const int i : IndexRange(3)) {
        sides += triangle.display_edges[i];
        hidden += !triangle.display_edges[i];
      }
      EXPECT_EQ(sides, 2);
    }
    EXPECT_EQ(hidden, 2 * 4 * 4);
  }
}

TEST(nurbs_surface_preview, NonBinarySamplingKeepsIdentityParameterAgreementWithZeroAllowance)
{
  for (const bool unit_coordinates : {false, true}) {
    auto source = make_planar_sheet();
    ASSERT_NE(source, nullptr);
    if (unit_coordinates) {
      double4x4 matrix = double4x4::identity();
      matrix[0][0] = matrix[1][1] = 0.5;
      matrix.location() = double3(0.5, 0.5, 0);
      auto transformed = transform_geometry(*source, matrix, 0);
      ASSERT_TRUE(transformed.has_value());
      source = std::move(transformed->geometry);
    }
    auto options = preview_options();
    options.boundary.segments_per_span = 13;
    options.boundary.evaluation_allowance = 0;
    options.interior_subdivisions = 13;
    std::string error;
    const auto result = tessellate_surfaces(*source, options, &error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(result->triangles.size(), 2 * 13 * 13);
    for (const auto &use : result->boundary.uses) {
      EXPECT_EQ(use.max_curve_support_deviation, 0);
      EXPECT_EQ(use.max_display_support_deviation, 0);
    }
  }
}

TEST(nurbs_surface_preview, RectangularGridsKeepEveryBoundaryIntervalAndPeriodicSeam)
{
  for (const int variant : IndexRange(3)) {
    const auto source = variant == 0 ? make_planar_sheet() : make_arc_sheet(variant == 2);
    ASSERT_TRUE(source);
    const auto original = OwnedGeometry(geometry_copy(source.get()));
    auto options = preview_options();
    options.boundary.segments_per_span = options.interior_subdivisions = 7;
    std::string error;
    const auto preview = tessellate_surfaces(*source, options, &error);
    ASSERT_TRUE(preview) << error;
    for (const auto &use : preview->boundary.uses) {
      const auto &samples = preview->boundary.edges[use.edge_index].samples;
      for (int i = 1; i < samples.size(); i++) {
        const int a = samples[i - 1].point_index, b = samples[i].point_index;
        const double2 uv_a = use.parameters[i - 1], uv_b = use.parameters[i];
        int matches = 0;
        for (const auto &triangle : preview->triangles) {
          if (triangle.face_id != use.face_id) {
            continue;
          }
          for (const int j : IndexRange(3)) {
            const int k = (j + 1) % 3;
            matches += (triangle.points[j] == a && triangle.points[k] == b &&
                        triangle.parameters[j] == uv_a && triangle.parameters[k] == uv_b) ||
                       (triangle.points[j] == b && triangle.points[k] == a &&
                        triangle.parameters[j] == uv_b && triangle.parameters[k] == uv_a);
          }
        }
        EXPECT_EQ(matches, 1) << variant << ": " << use.use_id << "/" << i;
      }
    }
    expect_source_geometry_equal(*source, *original);
  }
}

TEST(nurbs_surface_preview, HoleRemainsUnfilledAndFaceOrientationIsRespected)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto *face = static_cast<NurbsTopologyFace *>(source->faces.first);
  const double2 hole[] = {{0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.25}};
  add_planar_loop(*source, *face, {hole, 4}, true);
  std::string error;
  const auto preview = tessellate_surfaces(*source, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  EXPECT_NEAR(planar_area(*preview), 3, 1.0e-12);
  for (const auto &triangle : preview->triangles) {
    for (const bool visible : triangle.display_edges) {
      EXPECT_TRUE(visible);
    }
    const double3 center = (preview->position(triangle.points[0]) +
                            preview->position(triangle.points[1]) +
                            preview->position(triangle.points[2])) /
                           3;
    EXPECT_FALSE(center.x > -0.5 && center.x < 0.5 && center.y > -0.5 && center.y < 0.5);
  }
  face->orientation = -1;
  const auto reversed = tessellate_surfaces(*source, preview_options(), &error);
  ASSERT_TRUE(reversed.has_value()) << error;
  EXPECT_NEAR(planar_area(*reversed), -3, 1.0e-12);
}

TEST(nurbs_surface_preview, RationalSheetSamplesSourceAndPreservesWeights)
{
  const auto source = make_arc_sheet(false);
  ASSERT_NE(source, nullptr);
  const int64_t counter = source->next_id;
  const auto *curve = static_cast<const NurbsCurveGeometry *>(source->curves.first);
  const double middle_weight = curve->points[1].weight;
  std::string error;
  const auto preview = tessellate_surfaces(*source, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  EXPECT_FALSE(preview->interior.is_empty());
  for (const int i : IndexRange(preview->points_num())) {
    const double3 position = preview->position(i);
    EXPECT_NEAR(position.x * position.x + position.y * position.y, 1, 1.0e-13);
    EXPECT_GE(position.z, 0);
    EXPECT_LE(position.z, 2);
  }
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(curve->order, 3);
  EXPECT_EQ(curve->points[1].weight, middle_weight);
  EXPECT_EQ(source->surfaces.count(), 1);
}

TEST(nurbs_surface_preview, ConcaveFaceDoesNotFillItsCutoutOrWeldCoincidentWireVertices)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto *face = static_cast<NurbsTopologyFace *>(source->faces.first);
  auto *old_loop = static_cast<NurbsTopologyLoop *>(face->loops.first);
  BLI_remlink(&face->loops, old_loop);
  BLI_freelistN(&old_loop->edge_uses);
  MEM_delete(old_loop);
  /* The old boundary remains as independent wire geometry. This is fixture construction,
   * not a face-editing operation, and the new outline's coincident vertices have fresh IDs. */
  const double2 outline[] = {{0, 0}, {1, 0}, {1, 0.5}, {0.5, 0.5}, {0.5, 1}, {0, 1}};
  add_planar_loop(*source, *face, {outline, 6}, false);
  const int64_t counter = source->next_id;
  std::string error;
  const auto preview = tessellate_surfaces(*source, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  EXPECT_NEAR(planar_area(*preview), 3, 1.0e-12);
  for (const auto &triangle : preview->triangles) {
    const double2 center = (triangle.parameters[0] + triangle.parameters[1] +
                            triangle.parameters[2]) /
                           3;
    EXPECT_FALSE(center.x > 0.5 && center.y > 0.5);
  }
  EXPECT_EQ(preview->boundary.edges.size(), 10);
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(source->faces.count(), 1);
  EXPECT_EQ(source->vertices.count(), 10);
}

TEST(nurbs_surface_preview, ClosedRationalSeamSharesModelPointsWithDistinctCornerUVs)
{
  const auto source = make_arc_sheet(true);
  ASSERT_NE(source, nullptr);
  std::string error;
  const auto preview = tessellate_surfaces(*source, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  /* Four U knot spans each receive four subdivisions, rather than four over the whole circle. */
  EXPECT_EQ(preview->interior.size(), 15 * 3);
  const auto index = TopologyIndex::build(*source);
  bool found_seam = false;
  for (const auto &edge : preview->boundary.edges) {
    if (index->uses_for_edge(edge.edge_id).size() != 2) {
      continue;
    }
    found_seam = true;
    for (const auto &sample : edge.samples) {
      bool low = false, high = false;
      for (const auto &triangle : preview->triangles) {
        for (const int i : IndexRange(3)) {
          if (triangle.points[i] == sample.point_index) {
            low |= triangle.parameters[i].x == 0;
            high |= triangle.parameters[i].x == 4;
          }
        }
      }
      EXPECT_TRUE(low);
      EXPECT_TRUE(high);
    }
  }
  EXPECT_TRUE(found_seam);
  EXPECT_EQ(source->vertices.count(), 2);
  EXPECT_EQ(source->edges.count(), 3);
  EXPECT_EQ(source->faces.count(), 1);
}

TEST(nurbs_surface_preview, AdjacentFacesShareCanonicalBoundaryPointsAtDifferentResolutions)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const auto *face = static_cast<const NurbsTopologyFace *>(source->faces.first);
  const auto *loop = static_cast<const NurbsTopologyLoop *>(face->loops.first);
  const auto *use = static_cast<const NurbsTopologyEdgeUse *>(loop->edge_uses.first);
  /* Constructor loop order: original, end rail, cap, start rail. */
  const int64_t common_edge = use->next->next->edge_id;
  auto extended = extrude_edges(*source, {common_edge}, {0, 2, 0}, 0);
  ASSERT_TRUE(extended.has_value());
  source = std::move(extended->geometry);
  for (const int resolution : {1, 4}) {
    auto options = preview_options();
    options.boundary.segments_per_span = resolution;
    options.interior_subdivisions = resolution;
    std::string error;
    const auto preview = tessellate_surfaces(*source, options, &error);
    ASSERT_TRUE(preview.has_value()) << error;
    EXPECT_NEAR(planar_area(*preview), 8, 1.0e-12);
    for (const auto &edge : preview->boundary.edges) {
      if (edge.edge_id != common_edge) {
        continue;
      }
      for (const auto &sample : edge.samples) {
        Set<int64_t> incident_faces;
        for (const auto &triangle : preview->triangles) {
          if (ELEM(sample.point_index, triangle.points[0], triangle.points[1], triangle.points[2]))
          {
            incident_faces.add(triangle.face_id);
          }
        }
        EXPECT_EQ(incident_faces.size(), 2);
      }
    }
  }
  EXPECT_EQ(source->faces.count(), 2);
  EXPECT_EQ(source->edges.count(), 7);
  EXPECT_EQ(source->vertices.count(), 6);
}

TEST(nurbs_surface_preview, RejectsNestedExteriorAndIntersectingSampledHoles)
{
  const double2 outer_hole[] = {{0.125, 0.125}, {0.125, 0.875}, {0.875, 0.875}, {0.875, 0.125}};
  const double2 inner_hole[] = {{0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.25}};
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto *face = static_cast<NurbsTopologyFace *>(source->faces.first);
  add_planar_loop(*source, *face, {outer_hole, 4}, true);
  add_planar_loop(*source, *face, {inner_hole, 4}, true);
  std::string error;
  EXPECT_FALSE(tessellate_surfaces(*source, preview_options(), &error).has_value());
  EXPECT_NE(error.find("nested"), std::string::npos);

  source = make_planar_sheet();
  face = static_cast<NurbsTopologyFace *>(source->faces.first);
  static_cast<NurbsTopologyLoop *>(face->loops.first)->is_inner = 1;
  add_planar_loop(*source, *face, {inner_hole, 4}, false);
  EXPECT_FALSE(tessellate_surfaces(*source, preview_options(), &error).has_value());
  EXPECT_NE(error.find("exterior"), std::string::npos);

  source = make_planar_sheet();
  face = static_cast<NurbsTopologyFace *>(source->faces.first);
  const double2 crossing_hole[] = {{0.25, 0.25}, {0.75, 0.75}, {0.25, 0.75}, {0.75, 0.25}};
  add_planar_loop(*source, *face, {crossing_hole, 4}, true);
  EXPECT_FALSE(tessellate_surfaces(*source, preview_options(), &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(geometry_validate_structure(*source));
}

TEST(nurbs_surface_preview, RejectsTriangulatorMergingAndOutputBudgetExhaustionWithoutSourceEdits)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto options = preview_options();
  /* Distinct source vertices at the same UV are an actual CDT merge. A large epsilon alone
   * does not force merging when all constraints already occur in the initial triangulation. */
  const double2 repeated_corner[] = {
      {0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.75}, {0.75, 0.25}};
  add_planar_loop(
      *source, *static_cast<NurbsTopologyFace *>(source->faces.first), {repeated_corner, 5}, true);
  const int64_t repeated_counter = source->next_id;
  std::string error;
  EXPECT_FALSE(tessellate_surfaces(*source, options, &error).has_value());
  EXPECT_NE(error.find("merged"), std::string::npos);
  EXPECT_EQ(source->next_id, repeated_counter);
  EXPECT_EQ(source->vertices.count(), 9);
  source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t counter = source->next_id;
  options = preview_options();
  options.max_points = 1;
  EXPECT_FALSE(tessellate_surfaces(*source, options, &error).has_value());
  options = preview_options();
  options.max_triangles = 0;
  EXPECT_FALSE(tessellate_surfaces(*source, options, &error).has_value());
  options = preview_options();
  options.normalized_merge_epsilon = 0;
  EXPECT_FALSE(tessellate_surfaces(*source, options, &error).has_value());
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(source->vertices.count(), 4);
  EXPECT_EQ(source->faces.count(), 1);
  EXPECT_TRUE(geometry_validate_structure(*source));
}

TEST(nurbs_surface_preview, CollapsedBoundaryClosesItsChartButGeneralUVGapsStillFail)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 collapse = double4x4::identity();
  collapse[0][0] = collapse[1][1] = collapse[2][2] = 0;
  auto collapsed = transform_geometry(*source, collapse, 0);
  ASSERT_TRUE(collapsed.has_value());
  std::string error;
  const auto preview = tessellate_surfaces(*collapsed->geometry, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  EXPECT_FALSE(preview->triangles.is_empty());
  for (const int i : IndexRange(preview->points_num())) {
    EXPECT_EQ(preview->position(i), double3(0));
  }
  auto *face = static_cast<NurbsTopologyFace *>(collapsed->geometry->faces.first);
  auto *loop = static_cast<NurbsTopologyLoop *>(face->loops.first);
  const auto *use = static_cast<NurbsTopologyEdgeUse *>(loop->edge_uses.first);
  for (NurbsCurveGeometry &curve : collapsed->geometry->curves) {
    if (curve.id == use->pcurve_id) {
      curve.points[0].position[0] = 0.125;
    }
  }
  /* This gap lies entirely on a certified collapsed natural boundary. */
  const auto pole_preview = tessellate_surfaces(*collapsed->geometry, preview_options(), &error);
  ASSERT_TRUE(pole_preview) << error;
  OwnedGeometry pole_source(geometry_copy(collapsed->geometry.get()));
  bool reused = false;
  const auto refreshed = refresh_surface_preview(*collapsed->geometry,
                                                 preview_options(),
                                                 *pole_source,
                                                 preview_options(),
                                                 *pole_preview,
                                                 reused,
                                                 &error);
  ASSERT_TRUE(refreshed) << error;
  EXPECT_TRUE(reused);
  ASSERT_EQ(refreshed->points_num(), pole_preview->points_num());
  ASSERT_EQ(refreshed->triangles.size(), pole_preview->triangles.size());
  for (const int i : IndexRange(refreshed->points_num())) {
    EXPECT_EQ(refreshed->position(i), pole_preview->position(i));
  }
  for (NurbsCurveGeometry &curve : collapsed->geometry->curves) {
    if (curve.id == use->pcurve_id) {
      curve.points[0].position[1] = 0.125;
    }
  }
  /* A gap into the chart interior is not a natural boundary pole. */
  EXPECT_FALSE(refresh_surface_preview(*collapsed->geometry,
                                       preview_options(),
                                       *pole_source,
                                       preview_options(),
                                       *pole_preview,
                                       reused,
                                       &error));
  EXPECT_FALSE(reused);
  EXPECT_FALSE(tessellate_surfaces(*collapsed->geometry, preview_options(), &error).has_value());
  EXPECT_NE(error.find("UV"), std::string::npos);
}

TEST(nurbs_surface_preview, FaceDisplayCentersStayInsideTrimAndKeepPlanarCenters)
{
  auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  std::string error;
  auto preview = tessellate_surfaces(*sheet, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  const int64_t face_id = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  const auto planar = face_display_centers(*sheet, *preview);
  ASSERT_TRUE(planar.contains(face_id));
  EXPECT_EQ(planar.lookup(face_id), double3(0));
  const double2 hole[] = {{0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.25}};
  add_planar_loop(*sheet, *static_cast<NurbsTopologyFace *>(sheet->faces.first), {hole, 4}, true);
  preview = tessellate_surfaces(*sheet, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
  const auto holed = face_display_centers(*sheet, *preview);
  ASSERT_TRUE(holed.contains(face_id));
  const double3 anchor = holed.lookup(face_id);
  EXPECT_EQ(anchor.z, 0);
  EXPECT_LE(std::abs(anchor.x), 1);
  EXPECT_LE(std::abs(anchor.y), 1);
  EXPECT_TRUE(std::abs(anchor.x) >= 0.5 || std::abs(anchor.y) >= 0.5);
}

}  // namespace blender::bke::nurbs::tests
