/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "BKE_gtest_base.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_curve.hh"
#include "BKE_nurbs_surface.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_nurbs_topology_edit.hh"

#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"

#include "testing/testing.h"

#include "nurbs_topology_test_utils.hh"

namespace blender::bke::tests {

class NurbsTopologyEditTest : public BlenderGTestBase {};

static void copy_geometry(const NurbsTopologyFixture &fixture, NurbsGeometry &geometry)
{
  BKE_nurbs_curve_list_copy(geometry.curves, fixture.curves);
  geometry.topology = BKE_nurbs_topology_copy(*fixture.topology);
}

static void free_geometry(NurbsGeometry &geometry)
{
  BKE_nurbs_curve_list_free(geometry.curves);
  BKE_nurbs_topology_free(geometry.topology);
  geometry.topology = nullptr;
}

static NurbsSurface *find_surface(NurbsGeometry &geometry, const uint64_t id)
{
  for (NurbsSurface &surface : geometry.topology->surfaces) {
    if (surface.id == id) {
      return &surface;
    }
  }
  return nullptr;
}

static NurbsTopoEdge *find_edge(NurbsTopology &topology, const uint64_t id)
{
  for (NurbsTopoEdge &edge : topology.edges) {
    if (edge.id == id) {
      return &edge;
    }
  }
  return nullptr;
}

static NurbsCurve *find_curve(ListBaseT<NurbsCurve> &curves, const uint64_t id)
{
  for (NurbsCurve &curve : curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

static NurbsCurve *find_trim(NurbsTopology &topology, const uint64_t id)
{
  return find_curve(topology.trim_curves, id);
}

TEST_F(NurbsTopologyEditTest, TranslatesBothSidesAndReconcilesHoleBoundaries)
{
  NurbsTopologyFixture fixture;
  NurbsGeometry geometry = {};
  copy_geometry(fixture, geometry);
  double hole_trim_positions[4][2][3];
  double hole_trim_weights[4][2];
  double hole_trim_knots[4][2];
  double surface_weights[2][6];
  for (int surface_index = 0; surface_index < 2; surface_index++) {
    NurbsSurface *surface = find_surface(
        geometry, surface_index == 0 ? fixture.surface_a_id : fixture.surface_b_id);
    ASSERT_NE(surface, nullptr);
    for (int cv_index = 0; cv_index < 6; cv_index++) {
      surface_weights[surface_index][cv_index] = surface->control_vertices[cv_index].weight;
    }
  }
  for (int trim_index = 0; trim_index < 4; trim_index++) {
    NurbsCurve *trim = find_trim(*geometry.topology, fixture.hole_trim_ids[trim_index]);
    ASSERT_NE(trim, nullptr);
    for (int cv_index = 0; cv_index < 2; cv_index++) {
      std::copy_n(
          trim->control_vertices[cv_index].position, 3, hole_trim_positions[trim_index][cv_index]);
      hole_trim_weights[trim_index][cv_index] = trim->control_vertices[cv_index].weight;
    }
    std::copy_n(trim->knots, 2, hole_trim_knots[trim_index]);
  }
  const double3 delta(0.0, 0.0, 0.25);
  std::string error;
  ASSERT_TRUE(
      BKE_nurbs_topology_translate_boundary(geometry, fixture.shared_edge_curve_id, delta, &error))
      << error;
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(geometry.curves, *geometry.topology, &error)) << error;

  NurbsSurface *surface_a = find_surface(geometry, fixture.surface_a_id);
  NurbsSurface *surface_b = find_surface(geometry, fixture.surface_b_id);
  ASSERT_NE(surface_a, nullptr);
  ASSERT_NE(surface_b, nullptr);
  for (int u_index = 0; u_index < surface_a->control_vertex_count_u; u_index++) {
    EXPECT_NEAR(surface_a->control_vertices[u_index * 2].position[2], 0.0, 1e-12);
    EXPECT_NEAR(surface_a->control_vertices[u_index * 2 + 1].position[2], 1.25, 1e-12);
  }
  for (int u_index = 0; u_index < surface_b->control_vertex_count_u; u_index++) {
    EXPECT_NEAR(surface_b->control_vertices[u_index * 2].position[2], 1.25, 1e-12);
    EXPECT_NEAR(surface_b->control_vertices[u_index * 2 + 1].position[2], 2.0, 1e-12);
  }
  for (int cv_index = 0; cv_index < 6; cv_index++) {
    EXPECT_EQ(surface_a->control_vertices[cv_index].weight, surface_weights[0][cv_index]);
    EXPECT_EQ(surface_b->control_vertices[cv_index].weight, surface_weights[1][cv_index]);
  }
  for (int trim_index = 0; trim_index < 4; trim_index++) {
    NurbsCurve *trim = find_trim(*geometry.topology, fixture.hole_trim_ids[trim_index]);
    ASSERT_NE(trim, nullptr);
    for (int cv_index = 0; cv_index < 2; cv_index++) {
      for (int axis = 0; axis < 3; axis++) {
        EXPECT_EQ(trim->control_vertices[cv_index].position[axis],
                  hole_trim_positions[trim_index][cv_index][axis]);
      }
      EXPECT_EQ(trim->control_vertices[cv_index].weight, hole_trim_weights[trim_index][cv_index]);
    }
    EXPECT_EQ(trim->knots[0], hole_trim_knots[trim_index][0]);
    EXPECT_EQ(trim->knots[1], hole_trim_knots[trim_index][1]);
  }

  for (const uint64_t hole_edge_id : fixture.hole_edge_ids) {
    NurbsTopoEdge *edge = find_edge(*geometry.topology, hole_edge_id);
    ASSERT_NE(edge, nullptr);
    NurbsCurve *curve = find_curve(geometry.curves, edge->curve_id);
    ASSERT_NE(curve, nullptr);
    double start[3], end[3];
    ASSERT_TRUE(BKE_nurbs_curve_evaluate(*curve, curve->domain[0], start));
    ASSERT_TRUE(BKE_nurbs_curve_evaluate(*curve, curve->domain[1], end));
    if (hole_edge_id == fixture.hole_edge_ids[0] || hole_edge_id == fixture.hole_edge_ids[2]) {
      EXPECT_NEAR(start[2], 0.3125, 1e-10);
      EXPECT_NEAR(end[2], 0.9375, 1e-10);
    }
    else if (hole_edge_id == fixture.hole_edge_ids[1]) {
      EXPECT_NEAR(start[2], 0.9375, 1e-10);
      EXPECT_NEAR(end[2], 0.9375, 1e-10);
    }
    else {
      EXPECT_NEAR(start[2], 0.3125, 1e-10);
      EXPECT_NEAR(end[2], 0.3125, 1e-10);
    }
  }
  free_geometry(geometry);
}

TEST_F(NurbsTopologyEditTest, WorksAfterSharedEdgeTopologySplit)
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
  NurbsGeometry geometry = {};
  copy_geometry(fixture, geometry);
  ASSERT_TRUE(BKE_nurbs_topology_translate_boundary(
      geometry, fixture.shared_edge_curve_id, double3(0.0, 0.0, 0.25), &error))
      << error;
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(geometry.curves, *geometry.topology, &error)) << error;
  free_geometry(geometry);
}

TEST_F(NurbsTopologyEditTest, UnsupportedInputIsAtomic)
{
  NurbsTopologyFixture fixture;
  NurbsGeometry geometry = {};
  copy_geometry(fixture, geometry);
  NurbsSurface *surface = find_surface(geometry, fixture.surface_a_id);
  ASSERT_NE(surface, nullptr);
  const double original_seam_z = surface->control_vertices[1].position[2];
  NurbsTopology *original_topology = geometry.topology;
  NurbsCurve *original_curve_list = static_cast<NurbsCurve *>(geometry.curves.first);
  std::string error;

  EXPECT_FALSE(BKE_nurbs_topology_translate_boundary(
      geometry, std::numeric_limits<uint64_t>::max(), double3(0.0, 0.0, 1.0), &error));
  EXPECT_EQ(geometry.topology, original_topology);
  EXPECT_EQ(geometry.curves.first, original_curve_list);
  EXPECT_NEAR(find_surface(geometry, fixture.surface_a_id)->control_vertices[1].position[2],
              original_seam_z,
              1e-12);

  EXPECT_FALSE(BKE_nurbs_topology_translate_boundary(
      geometry,
      fixture.shared_edge_curve_id,
      double3(std::numeric_limits<double>::infinity(), 0.0, 0.0),
      &error));
  EXPECT_EQ(geometry.topology, original_topology);

  NurbsTopoEdge *hole_arc = find_edge(*geometry.topology, fixture.hole_edge_ids[1]);
  ASSERT_NE(hole_arc, nullptr);
  EXPECT_FALSE(BKE_nurbs_topology_translate_boundary(
      geometry, hole_arc->curve_id, double3(0.0, 0.0, 1.0), &error));
  EXPECT_EQ(geometry.topology, original_topology);
  EXPECT_EQ(geometry.curves.first, original_curve_list);
  EXPECT_NEAR(find_surface(geometry, fixture.surface_a_id)->control_vertices[1].position[2],
              original_seam_z,
              1e-12);
  free_geometry(geometry);
}

}  // namespace blender::bke::tests
