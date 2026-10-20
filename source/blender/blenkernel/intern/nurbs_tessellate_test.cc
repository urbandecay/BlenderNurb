/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>
#include <string>

#include "BKE_gtest_base.hh"
#include "BKE_nurbs_tessellate.hh"
#include "BKE_nurbs_topology.hh"

#include "BLI_math_vector_types.hh"

#include "testing/testing.h"

#include "nurbs_topology_test_utils.hh"

namespace blender::bke::tests {

class NurbsTessellateTest : public BlenderGTestBase {};

static NurbsGeometry geometry_for_fixture(const NurbsTopologyFixture &fixture)
{
  NurbsGeometry geometry = {};
  geometry.curves = fixture.curves;
  geometry.topology = fixture.topology;
  return geometry;
}

static double parameter_from_quarter_circle_angle(const NurbsSurface &surface, const double angle)
{
  double low = 0.0;
  double high = 1.0;
  for (int iteration = 0; iteration < 48; iteration++) {
    const double middle = 0.5 * (low + high);
    double position[3];
    BKE_nurbs_surface_evaluate(surface, middle, surface.domain_v[0], position);
    if (std::atan2(position[1], position[0]) < angle) {
      low = middle;
    }
    else {
      high = middle;
    }
  }
  return 0.5 * (low + high);
}

static bool triangle_centroid_inside_hole(const NurbsTessellation &tessellation,
                                          const NurbsSurface &surface,
                                          const int3 &triangle)
{
  const double3 &a = tessellation.positions[triangle.x];
  const double3 &b = tessellation.positions[triangle.y];
  const double3 &c = tessellation.positions[triangle.z];
  const double x = (a.x + b.x + c.x) / 3.0;
  const double y = (a.y + b.y + c.y) / 3.0;
  const double z = (a.z + b.z + c.z) / 3.0;
  const double u = parameter_from_quarter_circle_angle(surface, std::atan2(y, x));
  /* Stay away from the sampled boundary by a small margin. Triangles near the trim can have
   * planar centroids slightly off the exact curved surface, which is expected for display data. */
  return u > 0.30 && u < 0.70 && z > 0.30 && z < 0.70;
}

TEST_F(NurbsTessellateTest, CurvedTrimmedFacesAndHole)
{
  NurbsTopologyFixture fixture;
  NurbsGeometry geometry = geometry_for_fixture(fixture);
  NurbsTessellation tessellation;
  std::string error;
  ASSERT_TRUE(BKE_nurbs_topology_tessellate(geometry, 12, tessellation, &error)) << error;
  ASSERT_FALSE(tessellation.triangles.is_empty());
  ASSERT_EQ(tessellation.triangles.size(), tessellation.face_ids.size());

  bool found_face_a = false;
  bool found_face_b = false;
  for (const uint64_t face_id : tessellation.face_ids) {
    found_face_a |= face_id == fixture.face_a_id;
    found_face_b |= face_id == fixture.face_b_id;
  }
  EXPECT_TRUE(found_face_a);
  EXPECT_TRUE(found_face_b);

  for (const double3 &position : tessellation.positions) {
    EXPECT_NEAR(position.x * position.x + position.y * position.y, 1.0, 1e-9);
    EXPECT_GE(position.z, -1e-9);
    EXPECT_LE(position.z, 2.0 + 1e-9);
  }

  const NurbsSurface &surface_a = *static_cast<const NurbsSurface *>(
      fixture.topology->surfaces.first);
  for (int i = 0; i < tessellation.triangles.size(); i++) {
    if (tessellation.face_ids[i] == fixture.face_a_id) {
      EXPECT_FALSE(
          triangle_centroid_inside_hole(tessellation, surface_a, tessellation.triangles[i]));
    }
  }
}

TEST_F(NurbsTessellateTest, SharedEdgeSplitRetainsFaceAndHoleTessellation)
{
  NurbsTopologyFixture fixture;
  NurbsEdgeSplitResult split;
  std::string error;
  ASSERT_TRUE(BKE_nurbs_topology_edge_split(fixture.curves,
                                            *fixture.topology,
                                            fixture.next_component_id,
                                            fixture.shared_edge_id,
                                            0.5,
                                            split,
                                            &error))
      << error;

  NurbsGeometry geometry = geometry_for_fixture(fixture);
  NurbsTessellation tessellation;
  ASSERT_TRUE(BKE_nurbs_topology_tessellate(geometry, 10, tessellation, &error)) << error;
  bool found_face_a = false;
  bool found_face_b = false;
  const NurbsSurface &surface_a = *static_cast<const NurbsSurface *>(
      fixture.topology->surfaces.first);
  for (int i = 0; i < tessellation.triangles.size(); i++) {
    found_face_a |= tessellation.face_ids[i] == fixture.face_a_id;
    found_face_b |= tessellation.face_ids[i] == fixture.face_b_id;
    if (tessellation.face_ids[i] == fixture.face_a_id) {
      EXPECT_FALSE(
          triangle_centroid_inside_hole(tessellation, surface_a, tessellation.triangles[i]));
    }
  }
  EXPECT_TRUE(found_face_a);
  EXPECT_TRUE(found_face_b);
}

TEST_F(NurbsTessellateTest, FailureDoesNotReplaceExistingOutput)
{
  NurbsTopologyFixture fixture;
  NurbsGeometry geometry = geometry_for_fixture(fixture);
  NurbsTessellation tessellation;
  tessellation.positions.append(double3(7.0, 8.0, 9.0));
  tessellation.triangles.append(int3(1, 2, 3));
  tessellation.face_ids.append(42);
  std::string error;
  EXPECT_FALSE(BKE_nurbs_topology_tessellate(geometry, -1, tessellation, &error));
  EXPECT_FALSE(error.empty());
  ASSERT_EQ(tessellation.positions.size(), 1);
  ASSERT_EQ(tessellation.triangles.size(), 1);
  ASSERT_EQ(tessellation.face_ids.size(), 1);
  EXPECT_EQ(tessellation.positions[0], double3(7.0, 8.0, 9.0));
  EXPECT_EQ(tessellation.triangles[0], int3(1, 2, 3));
  EXPECT_EQ(tessellation.face_ids[0], 42);
}

}  // namespace blender::bke::tests
