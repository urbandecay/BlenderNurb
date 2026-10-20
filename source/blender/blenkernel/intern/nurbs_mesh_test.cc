/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_tessellate.hh"

#include "BLI_index_range.hh"

#include "testing/testing.h"

#include "nurbs_topology_test_utils.hh"

namespace blender::bke::tests {

class NurbsMeshTest : public BlenderGTestBase {};

TEST_F(NurbsMeshTest, QuarterCylinderHoleFaceIDsAndWire)
{
  NurbsTopologyFixture fixture;
  fixture.add_closed_circle_wire(3.0);
  NurbsGeometry geometry{};
  geometry.curves = fixture.curves;
  geometry.topology = fixture.topology;

  NurbsTessellation tessellation;
  ASSERT_TRUE(BKE_nurbs_topology_tessellate(geometry, 8, tessellation));

  Mesh *mesh = BKE_nurbs_mesh_create(geometry, 8);
  ASSERT_NE(mesh, nullptr);
  EXPECT_EQ(mesh->faces_num, tessellation.triangles.size());
  EXPECT_EQ(mesh->corners_num, tessellation.triangles.size() * 3);
  EXPECT_EQ(mesh->verts_num, tessellation.positions.size() + 65);
  EXPECT_TRUE(bke::mesh_is_valid(*mesh, false));

  const bke::AttributeAccessor attributes = mesh->attributes();
  const VArraySpan<int> low_ids = *attributes.lookup<int>(".nurbs_face_id_low",
                                                          bke::AttrDomain::Face);
  const VArraySpan<int> high_ids = *attributes.lookup<int>(".nurbs_face_id_high",
                                                           bke::AttrDomain::Face);
  ASSERT_EQ(low_ids.size(), tessellation.face_ids.size());
  ASSERT_EQ(high_ids.size(), tessellation.face_ids.size());
  for (const int face : IndexRange(mesh->faces_num)) {
    const uint64_t source_id = (uint64_t(uint32_t(high_ids[face])) << 32) |
                               uint32_t(low_ids[face]);
    EXPECT_EQ(source_id, tessellation.face_ids[face]);
    EXPECT_TRUE(source_id == fixture.face_a_id || source_id == fixture.face_b_id);
  }

  /* The circular support is a topology wire edge. It is emitted once as a 64-segment loose
   * polyline, rather than once as a topology edge and again as a geometry curve. */
  ASSERT_GE(mesh->edges_num, 64);
  const Span<int2> edges = mesh->edges();
  const int wire_vertex_start = int(tessellation.positions.size());
  for (const int segment : IndexRange(64)) {
    EXPECT_EQ(edges[segment][0], wire_vertex_start + segment);
    EXPECT_EQ(edges[segment][1], wire_vertex_start + segment + 1);
  }
  BKE_id_free(nullptr, mesh);
}

TEST_F(NurbsMeshTest, EmptyTopologyKeepsLooseCurves)
{
  NurbsTopologyFixture fixture;
  fixture.add_closed_circle_wire(3.0);
  NurbsTopology empty_topology{};
  NurbsGeometry geometry{};
  geometry.curves = fixture.curves;
  geometry.topology = &empty_topology;

  Mesh *mesh = BKE_nurbs_mesh_create(geometry, 8);
  ASSERT_NE(mesh, nullptr);
  EXPECT_EQ(mesh->faces_num, 0);
  EXPECT_EQ(mesh->corners_num, 0);
  EXPECT_GT(mesh->verts_num, 0);
  EXPECT_GT(mesh->edges_num, 0);
  EXPECT_TRUE(bke::mesh_is_valid(*mesh, false));
  BKE_id_free(nullptr, mesh);
}

}  // namespace blender::bke::tests
