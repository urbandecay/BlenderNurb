/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_curve.hh"
#include "BKE_nurbs_topology.hh"

#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "testing/testing.h"

#include "nurbs_topology_test_utils.hh"

namespace blender::bke::tests {

class NurbsTopologyTest : public BlenderGTestBase {};

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

static NurbsSurface *find_surface(NurbsTopology &topology, const uint64_t id)
{
  for (NurbsSurface &surface : topology.surfaces) {
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

static NurbsTopoFace *find_face(NurbsTopology &topology, const uint64_t id)
{
  for (NurbsTopoFace &face : topology.faces) {
    if (face.id == id) {
      return &face;
    }
  }
  return nullptr;
}

static NurbsEdgeUse *find_use(NurbsTopology &topology, const uint64_t id)
{
  for (NurbsTopoFace &face : topology.faces) {
    for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
      NurbsTrimLoop &loop = face.loops[loop_index];
      for (int use_index = 0; use_index < loop.use_count; use_index++) {
        if (loop.uses[use_index].id == id) {
          return &loop.uses[use_index];
        }
      }
    }
  }
  return nullptr;
}

static NurbsTrimLoop *find_loop(NurbsTopology &topology, const uint64_t id)
{
  for (NurbsTopoFace &face : topology.faces) {
    for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
      if (face.loops[loop_index].id == id) {
        return &face.loops[loop_index];
      }
    }
  }
  return nullptr;
}

static int edge_use_count(const NurbsTopology &topology, const uint64_t edge_id)
{
  int count = 0;
  for (const NurbsTopoFace &face : topology.faces) {
    for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
      const NurbsTrimLoop &loop = face.loops[loop_index];
      for (int use_index = 0; use_index < loop.use_count; use_index++) {
        count += loop.uses[use_index].edge_id == edge_id;
      }
    }
  }
  return count;
}

static void expect_positions_near(const double a[3], const double b[3], const double tolerance)
{
  for (int axis = 0; axis < 3; axis++) {
    EXPECT_NEAR(a[axis], b[axis], tolerance);
  }
}

static void collect_all_ids(const NurbsTopologyFixture &fixture, Vector<uint64_t> &r_ids)
{
  for (const NurbsCurve &curve : fixture.curves) {
    r_ids.append(curve.id);
    for (int i = 0; i < curve.control_vertex_count; i++) {
      r_ids.append(curve.control_vertices[i].id);
    }
  }
  for (const NurbsSurface &surface : fixture.topology->surfaces) {
    r_ids.append(surface.id);
    for (int i = 0; i < surface.control_vertex_count_u * surface.control_vertex_count_v; i++) {
      r_ids.append(surface.control_vertices[i].id);
    }
  }
  for (const NurbsCurve &curve : fixture.topology->trim_curves) {
    r_ids.append(curve.id);
    for (int i = 0; i < curve.control_vertex_count; i++) {
      r_ids.append(curve.control_vertices[i].id);
    }
  }
  for (const NurbsTopoVertex &vertex : fixture.topology->vertices) {
    r_ids.append(vertex.id);
  }
  for (const NurbsTopoEdge &edge : fixture.topology->edges) {
    r_ids.append(edge.id);
  }
  for (const NurbsTopoFace &face : fixture.topology->faces) {
    r_ids.append(face.id);
    for (int i = 0; i < face.loop_count; i++) {
      r_ids.append(face.loops[i].id);
      for (int j = 0; j < face.loops[i].use_count; j++) {
        r_ids.append(face.loops[i].uses[j].id);
      }
    }
  }
}

static void collect_all_ids(const NurbsGeometry &geometry, Vector<uint64_t> &r_ids)
{
  for (const NurbsCurve &curve : geometry.curves) {
    r_ids.append(curve.id);
    for (int i = 0; i < curve.control_vertex_count; i++) {
      r_ids.append(curve.control_vertices[i].id);
    }
  }
  if (geometry.topology == nullptr) {
    return;
  }
  for (const NurbsSurface &surface : geometry.topology->surfaces) {
    r_ids.append(surface.id);
    for (int i = 0; i < surface.control_vertex_count_u * surface.control_vertex_count_v; i++) {
      r_ids.append(surface.control_vertices[i].id);
    }
  }
  for (const NurbsCurve &curve : geometry.topology->trim_curves) {
    r_ids.append(curve.id);
    for (int i = 0; i < curve.control_vertex_count; i++) {
      r_ids.append(curve.control_vertices[i].id);
    }
  }
  for (const NurbsTopoVertex &vertex : geometry.topology->vertices) {
    r_ids.append(vertex.id);
  }
  for (const NurbsTopoEdge &edge : geometry.topology->edges) {
    r_ids.append(edge.id);
  }
  for (const NurbsTopoFace &face : geometry.topology->faces) {
    r_ids.append(face.id);
    for (int i = 0; i < face.loop_count; i++) {
      r_ids.append(face.loops[i].id);
      for (int j = 0; j < face.loops[i].use_count; j++) {
        r_ids.append(face.loops[i].uses[j].id);
      }
    }
  }
}

TEST_F(NurbsTopologyTest, ExactQuarterCylinderFixtureAndSharedIncidence)
{
  NurbsTopologyFixture fixture;
  std::string error;
  ASSERT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;

  NurbsSurface *surface_a = find_surface(*fixture.topology, fixture.surface_a_id);
  NurbsSurface *surface_b = find_surface(*fixture.topology, fixture.surface_b_id);
  NurbsCurve *edge_curve = find_curve(fixture.curves, fixture.shared_edge_curve_id);
  ASSERT_NE(surface_a, nullptr);
  ASSERT_NE(surface_b, nullptr);
  ASSERT_NE(edge_curve, nullptr);
  ASSERT_TRUE(edge_curve->rational);
  EXPECT_EQ(edge_curve->degree, 2);
  EXPECT_EQ(edge_curve->order, 3);
  EXPECT_EQ(edge_curve->control_vertices[1].weight, 0.7071067811865475244);
  ASSERT_EQ(edge_use_count(*fixture.topology, fixture.shared_edge_id), 2);

  const NurbsTopoFace *face_a = find_face(*fixture.topology, fixture.face_a_id);
  const NurbsTopoFace *face_b = find_face(*fixture.topology, fixture.face_b_id);
  ASSERT_NE(face_a, nullptr);
  ASSERT_NE(face_b, nullptr);
  ASSERT_EQ(face_a->loop_count, 2);
  EXPECT_EQ(face_a->loops[0].role, 0);
  EXPECT_EQ(face_a->loops[1].role, 1);
  ASSERT_EQ(face_b->loop_count, 1);
  EXPECT_EQ(face_b->loops[0].role, 0);

  const double tolerance = fixture.topology->tolerance;
  for (int sample = 0; sample <= 32; sample++) {
    const double t = double(sample) / 32.0;
    double edge_position[3], position_a[3], position_b[3];
    ASSERT_TRUE(BKE_nurbs_curve_evaluate(*edge_curve, t, edge_position));
    ASSERT_TRUE(BKE_nurbs_surface_evaluate(*surface_a, t, 1.0, position_a));
    ASSERT_TRUE(BKE_nurbs_surface_evaluate(*surface_b, 2.0 + 3.0 * t, -3.0, position_b));
    expect_positions_near(edge_position, position_a, tolerance);
    expect_positions_near(edge_position, position_b, tolerance);
  }

  const NurbsTrimLoop &hole_loop = face_a->loops[1];
  for (int use_index = 0; use_index < hole_loop.use_count; use_index++) {
    const NurbsEdgeUse &use = hole_loop.uses[use_index];
    const NurbsTopoEdge *edge = find_edge(*fixture.topology, use.edge_id);
    ASSERT_NE(edge, nullptr);
    NurbsCurve *support = find_curve(fixture.curves, edge->curve_id);
    NurbsCurve *pcurve = find_trim(*fixture.topology, use.trim_curve_id);
    ASSERT_NE(support, nullptr);
    ASSERT_NE(pcurve, nullptr);
    EXPECT_EQ(pcurve->dimension, 2);
    EXPECT_EQ(pcurve->degree, 1);
    EXPECT_FALSE(pcurve->rational);
    ASSERT_EQ(pcurve->control_vertex_count, 2);
    ASSERT_EQ(pcurve->knot_count, 2);
    EXPECT_DOUBLE_EQ(pcurve->knots[0], 0.25);
    EXPECT_DOUBLE_EQ(pcurve->knots[1], 0.75);
    const bool circular_boundary = use_index == 1 || use_index == 3;
    EXPECT_EQ(support->degree, circular_boundary ? 2 : 1);
    EXPECT_EQ(bool(support->rational), circular_boundary);
    if (circular_boundary) {
      ASSERT_EQ(support->control_vertex_count, 3);
      ASSERT_EQ(support->knot_count, 4);
      const double circular_weight = std::sqrt(0.5);
      const double end_weight = 0.625 + 0.375 * circular_weight;
      const double middle_weight = 0.375 + 0.625 * circular_weight;
      EXPECT_NEAR(support->control_vertices[0].weight, end_weight, 1e-14);
      EXPECT_NEAR(support->control_vertices[1].weight, middle_weight, 1e-14);
      EXPECT_NEAR(support->control_vertices[2].weight, end_weight, 1e-14);
      EXPECT_DOUBLE_EQ(support->knots[0], 0.25);
      EXPECT_DOUBLE_EQ(support->knots[1], 0.25);
      EXPECT_DOUBLE_EQ(support->knots[2], 0.75);
      EXPECT_DOUBLE_EQ(support->knots[3], 0.75);
    }
    for (int sample = 0; sample <= 16; sample++) {
      const double t = edge->domain[0] +
                       (edge->domain[1] - edge->domain[0]) * (double(sample) / 16.0);
      double edge_position[3], uv[3], surface_position[3];
      ASSERT_TRUE(BKE_nurbs_curve_evaluate(*support, t, edge_position));
      ASSERT_TRUE(BKE_nurbs_curve_evaluate(*pcurve, t, uv));
      ASSERT_TRUE(BKE_nurbs_surface_evaluate(*surface_a, uv[0], uv[1], surface_position));
      expect_positions_near(edge_position, surface_position, tolerance);
    }
  }

  Vector<uint64_t> ids;
  collect_all_ids(fixture, ids);
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
}

TEST_F(NurbsTopologyTest, SharedEdgeSplitPreservesSupportsAndMapsReversedUses)
{
  NurbsTopologyFixture fixture;
  std::string error;
  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_EDGE, fixture.shared_edge_id, true));
  ASSERT_EQ(fixture.topology->active_component_id, fixture.shared_edge_id);
  ASSERT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;

  NurbsSurface *surface_a = find_surface(*fixture.topology, fixture.surface_a_id);
  ASSERT_NE(surface_a, nullptr);
  std::array<NurbsControlVertex, 6> surface_cvs;
  std::copy(surface_a->control_vertices,
            surface_a->control_vertices + surface_cvs.size(),
            surface_cvs.begin());
  std::array<double, 4> surface_knots_u;
  std::array<double, 2> surface_knots_v;
  std::copy_n(surface_a->knots_u, surface_knots_u.size(), surface_knots_u.begin());
  std::copy_n(surface_a->knots_v, surface_knots_v.size(), surface_knots_v.begin());
  std::array<uint64_t, 4> hole_trim_ids;
  std::copy(
      std::begin(fixture.hole_trim_ids), std::end(fixture.hole_trim_ids), hole_trim_ids.begin());
  NurbsTrimLoop *hole = find_loop(*fixture.topology, fixture.hole_loop_id);
  ASSERT_NE(hole, nullptr);
  const std::array<NurbsEdgeUse, 4> hole_uses = {
      hole->uses[0], hole->uses[1], hole->uses[2], hole->uses[3]};
  std::array<NurbsTopoEdge, 4> hole_edges;
  std::array<std::array<NurbsControlVertex, 3>, 4> hole_support_cvs;
  std::array<std::array<double, 4>, 4> hole_support_knots;
  std::array<int, 4> hole_support_cv_counts;
  std::array<int, 4> hole_support_knot_counts;
  std::array<std::array<double, 2>, 4> hole_support_domains;
  for (int i = 0; i < 4; i++) {
    NurbsTopoEdge *edge = find_edge(*fixture.topology, fixture.hole_edge_ids[i]);
    ASSERT_NE(edge, nullptr);
    hole_edges[i] = *edge;
    NurbsCurve *support = find_curve(fixture.curves, edge->curve_id);
    ASSERT_NE(support, nullptr);
    ASSERT_LE(support->control_vertex_count, 3);
    ASSERT_LE(support->knot_count, 4);
    hole_support_cv_counts[i] = support->control_vertex_count;
    hole_support_knot_counts[i] = support->knot_count;
    hole_support_domains[i] = {support->domain[0], support->domain[1]};
    std::copy_n(
        support->control_vertices, support->control_vertex_count, hole_support_cvs[i].begin());
    std::copy_n(support->knots, support->knot_count, hole_support_knots[i].begin());
  }
  std::array<std::array<NurbsControlVertex, 2>, 4> hole_trim_cvs;
  std::array<std::array<double, 2>, 4> hole_trim_knots;
  for (int i = 0; i < 4; i++) {
    NurbsCurve *trim = find_trim(*fixture.topology, hole_trim_ids[i]);
    ASSERT_NE(trim, nullptr);
    ASSERT_EQ(trim->control_vertex_count, 2);
    std::copy_n(trim->control_vertices, 2, hole_trim_cvs[i].begin());
    std::copy_n(trim->knots, 2, hole_trim_knots[i].begin());
  }
  NurbsTopoFace *before_face_a = find_face(*fixture.topology, fixture.face_a_id);
  NurbsTopoFace *before_face_b = find_face(*fixture.topology, fixture.face_b_id);
  ASSERT_NE(before_face_a, nullptr);
  ASSERT_NE(before_face_b, nullptr);
  const uint64_t a_seam_use_id = before_face_a->loops[0].uses[2].id;
  const uint64_t b_seam_use_id = before_face_b->loops[0].uses[0].id;

  NurbsEdgeSplitResult result;
  uint64_t next_id = fixture.next_component_id;
  ASSERT_TRUE(BKE_nurbs_topology_edge_split(
      fixture.curves, *fixture.topology, next_id, fixture.shared_edge_id, 0.5, result, &error))
      << error;
  EXPECT_EQ(result.source_edge_id, fixture.shared_edge_id);
  EXPECT_NE(result.child_edge_ids[0], result.child_edge_ids[1]);
  EXPECT_NE(result.vertex_id, 0);
  EXPECT_GT(next_id, fixture.next_component_id);

  NurbsTopoEdge *left = find_edge(*fixture.topology, result.child_edge_ids[0]);
  NurbsTopoEdge *right = find_edge(*fixture.topology, result.child_edge_ids[1]);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_DOUBLE_EQ(left->domain[0], 0.0);
  EXPECT_DOUBLE_EQ(left->domain[1], 0.5);
  EXPECT_DOUBLE_EQ(right->domain[0], 0.5);
  EXPECT_DOUBLE_EQ(right->domain[1], 1.0);
  EXPECT_EQ(left->curve_id, fixture.shared_edge_curve_id);
  EXPECT_EQ(right->curve_id, fixture.shared_edge_curve_id);
  EXPECT_EQ(left->vertex_ids[1], result.vertex_id);
  EXPECT_EQ(right->vertex_ids[0], result.vertex_id);
  EXPECT_EQ(left->flag, NURBS_TOPO_SELECT);
  EXPECT_EQ(right->flag, NURBS_TOPO_SELECT);
  EXPECT_EQ(fixture.topology->active_component_id, result.child_edge_ids[1]);
  ASSERT_EQ(BLI_listbase_count(&fixture.topology->selection_history), 2);
  const NurbsSelection &left_history = *static_cast<const NurbsSelection *>(
      fixture.topology->selection_history.first);
  const NurbsSelection &right_history = *static_cast<const NurbsSelection *>(
      fixture.topology->selection_history.last);
  EXPECT_EQ(left_history.component_id, result.child_edge_ids[0]);
  EXPECT_EQ(left_history.type, NURBS_TOPO_SELECT_EDGE);
  EXPECT_EQ(right_history.component_id, result.child_edge_ids[1]);
  EXPECT_EQ(right_history.type, NURBS_TOPO_SELECT_EDGE);

  NurbsTopoVertex *split_vertex = nullptr;
  for (NurbsTopoVertex &vertex : fixture.topology->vertices) {
    if (vertex.id == result.vertex_id) {
      split_vertex = &vertex;
    }
  }
  ASSERT_NE(split_vertex, nullptr);
  EXPECT_EQ(split_vertex->flag, NURBS_TOPO_SELECT);
  double seam_midpoint[3];
  ASSERT_TRUE(BKE_nurbs_curve_evaluate(
      *find_curve(fixture.curves, fixture.shared_edge_curve_id), 0.5, seam_midpoint));
  expect_positions_near(split_vertex->position, seam_midpoint, fixture.topology->tolerance);

  ASSERT_EQ(result.edge_uses.size(), 2);
  const NurbsTopoFace *face_a = find_face(*fixture.topology, fixture.face_a_id);
  const NurbsTopoFace *face_b = find_face(*fixture.topology, fixture.face_b_id);
  ASSERT_NE(face_a, nullptr);
  ASSERT_NE(face_b, nullptr);
  const NurbsTrimLoop &a_outer = face_a->loops[0];
  const NurbsTrimLoop &b_outer = face_b->loops[0];
  EXPECT_EQ(a_outer.uses[2].edge_id, result.child_edge_ids[1]);
  EXPECT_EQ(a_outer.uses[3].edge_id, result.child_edge_ids[0]);
  EXPECT_TRUE(a_outer.uses[2].reversed);
  EXPECT_TRUE(a_outer.uses[3].reversed);
  EXPECT_EQ(b_outer.uses[0].edge_id, result.child_edge_ids[0]);
  EXPECT_EQ(b_outer.uses[1].edge_id, result.child_edge_ids[1]);
  EXPECT_FALSE(b_outer.uses[0].reversed);
  EXPECT_FALSE(b_outer.uses[1].reversed);
  for (const NurbsEdgeUseSplit &use_split : result.edge_uses) {
    const NurbsEdgeUse *old_use = nullptr;
    for (const NurbsEdgeUse &old : hole_uses) {
      if (old.id == use_split.source_id) {
        old_use = &old;
      }
    }
    EXPECT_EQ(old_use, nullptr);
    EXPECT_NE(find_use(*fixture.topology, use_split.child_ids[0]), nullptr);
    EXPECT_NE(find_use(*fixture.topology, use_split.child_ids[1]), nullptr);
  }
  const NurbsEdgeUseSplit *a_use_split = nullptr;
  const NurbsEdgeUseSplit *b_use_split = nullptr;
  for (const NurbsEdgeUseSplit &use_split : result.edge_uses) {
    if (use_split.source_id == a_seam_use_id) {
      a_use_split = &use_split;
    }
    if (use_split.source_id == b_seam_use_id) {
      b_use_split = &use_split;
    }
  }
  ASSERT_NE(a_use_split, nullptr);
  ASSERT_NE(b_use_split, nullptr);
  EXPECT_EQ(a_use_split->child_ids[0], a_outer.uses[2].id);
  EXPECT_EQ(a_use_split->child_ids[1], a_outer.uses[3].id);
  EXPECT_EQ(b_use_split->child_ids[0], b_outer.uses[0].id);
  EXPECT_EQ(b_use_split->child_ids[1], b_outer.uses[1].id);

  EXPECT_EQ(find_edge(*fixture.topology, fixture.shared_edge_id), nullptr);
  EXPECT_EQ(edge_use_count(*fixture.topology, result.child_edge_ids[0]), 2);
  EXPECT_EQ(edge_use_count(*fixture.topology, result.child_edge_ids[1]), 2);
  /* The split publishes a cloned topology and frees the old loop storage. Reacquire the
   * unaffected hole loop from the replacement before checking that it retained its identity. */
  hole = find_loop(*fixture.topology, fixture.hole_loop_id);
  ASSERT_NE(hole, nullptr);
  EXPECT_EQ(hole->id, fixture.hole_loop_id);
  EXPECT_EQ(hole->use_count, 4);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(hole->uses[i].id, hole_uses[i].id);
    EXPECT_EQ(hole->uses[i].edge_id, hole_uses[i].edge_id);
    EXPECT_EQ(hole->uses[i].trim_curve_id, hole_uses[i].trim_curve_id);
  }
  for (int i = 0; i < 6; i++) {
    EXPECT_EQ(surface_a->control_vertices[i].id, surface_cvs[i].id);
    for (int axis = 0; axis < 3; axis++) {
      EXPECT_DOUBLE_EQ(surface_a->control_vertices[i].position[axis],
                       surface_cvs[i].position[axis]);
    }
    EXPECT_DOUBLE_EQ(surface_a->control_vertices[i].weight, surface_cvs[i].weight);
  }
  for (int i = 0; i < int(surface_knots_u.size()); i++) {
    EXPECT_DOUBLE_EQ(surface_a->knots_u[i], surface_knots_u[i]);
  }
  for (int i = 0; i < int(surface_knots_v.size()); i++) {
    EXPECT_DOUBLE_EQ(surface_a->knots_v[i], surface_knots_v[i]);
  }
  for (int i = 0; i < 4; i++) {
    NurbsTopoEdge *edge = find_edge(*fixture.topology, fixture.hole_edge_ids[i]);
    ASSERT_NE(edge, nullptr);
    EXPECT_EQ(edge->id, hole_edges[i].id);
    EXPECT_EQ(edge->curve_id, hole_edges[i].curve_id);
    EXPECT_EQ(edge->vertex_ids[0], hole_edges[i].vertex_ids[0]);
    EXPECT_EQ(edge->vertex_ids[1], hole_edges[i].vertex_ids[1]);
    EXPECT_DOUBLE_EQ(edge->domain[0], hole_edges[i].domain[0]);
    EXPECT_DOUBLE_EQ(edge->domain[1], hole_edges[i].domain[1]);
    NurbsCurve *support = find_curve(fixture.curves, edge->curve_id);
    ASSERT_NE(support, nullptr);
    EXPECT_EQ(support->control_vertex_count, hole_support_cv_counts[i]);
    EXPECT_EQ(support->knot_count, hole_support_knot_counts[i]);
    EXPECT_DOUBLE_EQ(support->domain[0], hole_support_domains[i][0]);
    EXPECT_DOUBLE_EQ(support->domain[1], hole_support_domains[i][1]);
    for (int j = 0; j < hole_support_cv_counts[i]; j++) {
      EXPECT_EQ(support->control_vertices[j].id, hole_support_cvs[i][j].id);
      EXPECT_DOUBLE_EQ(support->control_vertices[j].weight, hole_support_cvs[i][j].weight);
      for (int axis = 0; axis < 3; axis++) {
        EXPECT_DOUBLE_EQ(support->control_vertices[j].position[axis],
                         hole_support_cvs[i][j].position[axis]);
      }
    }
    for (int j = 0; j < hole_support_knot_counts[i]; j++) {
      EXPECT_DOUBLE_EQ(support->knots[j], hole_support_knots[i][j]);
    }
    EXPECT_EQ(fixture.hole_trim_ids[i], hole_trim_ids[i]);
    NurbsCurve *trim = find_trim(*fixture.topology, hole_trim_ids[i]);
    ASSERT_NE(trim, nullptr);
    for (int j = 0; j < 2; j++) {
      EXPECT_EQ(trim->control_vertices[j].id, hole_trim_cvs[i][j].id);
      EXPECT_EQ(trim->control_vertices[j].position[0], hole_trim_cvs[i][j].position[0]);
      EXPECT_EQ(trim->control_vertices[j].position[1], hole_trim_cvs[i][j].position[1]);
      EXPECT_EQ(trim->control_vertices[j].position[2], hole_trim_cvs[i][j].position[2]);
      EXPECT_DOUBLE_EQ(trim->control_vertices[j].weight, hole_trim_cvs[i][j].weight);
      EXPECT_DOUBLE_EQ(trim->knots[j], hole_trim_knots[i][j]);
    }
  }
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;
}

TEST_F(NurbsTopologyTest, FailedSplitIsAtomicAndLeavesResultAndAllocatorUntouched)
{
  NurbsTopologyFixture fixture;
  NurbsEdgeSplitResult result;
  result.source_edge_id = 707;
  result.child_edge_ids[0] = 808;
  result.child_edge_ids[1] = 909;
  result.vertex_id = 1001;
  const uint64_t next_id_before = fixture.next_component_id;
  uint64_t next_id = next_id_before;
  const int edge_count_before = BLI_listbase_count(&fixture.topology->edges);
  const int vertex_count_before = BLI_listbase_count(&fixture.topology->vertices);
  std::string error;
  EXPECT_FALSE(BKE_nurbs_topology_edge_split(
      fixture.curves, *fixture.topology, next_id, fixture.shared_edge_id, 0.0, result, &error));
  EXPECT_EQ(next_id, next_id_before);
  EXPECT_EQ(result.source_edge_id, 707);
  EXPECT_EQ(result.child_edge_ids[0], 808);
  EXPECT_EQ(result.child_edge_ids[1], 909);
  EXPECT_EQ(result.vertex_id, 1001);
  EXPECT_EQ(BLI_listbase_count(&fixture.topology->edges), edge_count_before);
  EXPECT_EQ(BLI_listbase_count(&fixture.topology->vertices), vertex_count_before);
  EXPECT_EQ(find_edge(*fixture.topology, fixture.shared_edge_id)->id, fixture.shared_edge_id);
}

TEST_F(NurbsTopologyTest, SelectionHistoryTracksActiveAndClearsComponentFlags)
{
  NurbsTopologyFixture fixture;
  const uint64_t vertex_id =
      static_cast<const NurbsTopoVertex *>(fixture.topology->vertices.first)->id;
  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_VERTEX, vertex_id, true));
  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_FACE, fixture.face_a_id, true));
  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_EDGE, fixture.shared_edge_id, true, false));
  EXPECT_EQ(fixture.topology->active_component_id, fixture.face_a_id);
  EXPECT_TRUE(BKE_nurbs_topology_selection_is_selected(
      *fixture.topology, NURBS_TOPO_SELECT_VERTEX, vertex_id));
  EXPECT_TRUE(BKE_nurbs_topology_selection_is_selected(
      *fixture.topology, NURBS_TOPO_SELECT_FACE, fixture.face_a_id));
  EXPECT_TRUE(BKE_nurbs_topology_selection_is_selected(
      *fixture.topology, NURBS_TOPO_SELECT_EDGE, fixture.shared_edge_id));
  ASSERT_EQ(BLI_listbase_count(&fixture.topology->selection_history), 3);

  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_FACE, fixture.face_a_id, false));
  EXPECT_EQ(fixture.topology->active_component_id, fixture.shared_edge_id);
  EXPECT_EQ(BLI_listbase_count(&fixture.topology->selection_history), 2);

  BKE_nurbs_topology_selection_clear(*fixture.topology);
  EXPECT_EQ(fixture.topology->active_component_id, 0);
  EXPECT_EQ(BLI_listbase_count(&fixture.topology->selection_history), 0);
  EXPECT_FALSE(BKE_nurbs_topology_selection_is_selected(
      *fixture.topology, NURBS_TOPO_SELECT_VERTEX, vertex_id));
  EXPECT_FALSE(BKE_nurbs_topology_selection_is_selected(
      *fixture.topology, NURBS_TOPO_SELECT_FACE, fixture.face_a_id));
  EXPECT_FALSE(BKE_nurbs_topology_selection_is_selected(
      *fixture.topology, NURBS_TOPO_SELECT_EDGE, fixture.shared_edge_id));
}

TEST_F(NurbsTopologyTest, RejectsBadReferencesMisalignedPcurveAndDuplicateIds)
{
  NurbsTopologyFixture fixture;
  std::string error;
  ASSERT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;
  NurbsTopoFace *face_b = find_face(*fixture.topology, fixture.face_b_id);
  ASSERT_NE(face_b, nullptr);
  NurbsEdgeUse &b_seam_use = face_b->loops[0].uses[0];
  NurbsCurve *pcurve = find_trim(*fixture.topology, b_seam_use.trim_curve_id);
  ASSERT_NE(pcurve, nullptr);
  const double saved_u = pcurve->control_vertices[1].position[0];
  pcurve->control_vertices[1].position[0] += 0.125;
  EXPECT_FALSE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error));
  pcurve->control_vertices[1].position[0] = saved_u;
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;

  const uint64_t saved_trim_id = b_seam_use.trim_curve_id;
  b_seam_use.trim_curve_id = std::numeric_limits<uint64_t>::max();
  EXPECT_FALSE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error));
  b_seam_use.trim_curve_id = saved_trim_id;
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;

  NurbsTrimLoop *hole = find_loop(*fixture.topology, fixture.hole_loop_id);
  ASSERT_NE(hole, nullptr);
  hole->role = 0;
  EXPECT_FALSE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error));
  hole->role = 1;
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;

  NurbsTopoEdge *seam = find_edge(*fixture.topology, fixture.shared_edge_id);
  ASSERT_NE(seam, nullptr);
  const uint64_t saved_edge_id = seam->id;
  seam->id = static_cast<NurbsTopoEdge *>(fixture.topology->edges.first)->next->id;
  EXPECT_FALSE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error));
  seam->id = saved_edge_id;
}

TEST_F(NurbsTopologyTest, CloneOwnsIndependentSupportsAndSeparateCurvesRemainSeparate)
{
  NurbsTopologyFixture fixture;
  std::string error;
  ASSERT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;

  ListBaseT<NurbsCurve> copied_curves = {};
  BKE_nurbs_curve_list_copy(copied_curves, fixture.curves);
  NurbsTopology *copy = BKE_nurbs_topology_copy(*fixture.topology);
  ASSERT_NE(copy, nullptr);
  NurbsSurface &source_surface = *find_surface(*fixture.topology, fixture.surface_a_id);
  NurbsSurface *copied_surface = find_surface(*copy, fixture.surface_a_id);
  ASSERT_NE(copied_surface, nullptr);
  EXPECT_NE(copied_surface->control_vertices, source_surface.control_vertices);
  EXPECT_NE(copied_surface->knots_u, source_surface.knots_u);
  NurbsCurve *source_trim = find_trim(*fixture.topology, fixture.hole_trim_ids[0]);
  NurbsCurve *copied_trim = find_trim(*copy, fixture.hole_trim_ids[0]);
  ASSERT_NE(source_trim, nullptr);
  ASSERT_NE(copied_trim, nullptr);
  EXPECT_NE(copied_trim->control_vertices, source_trim->control_vertices);
  EXPECT_NE(copied_trim->knots, source_trim->knots);
  copied_surface->control_vertices[0].position[0] += 10.0;
  copied_trim->control_vertices[0].position[0] += 10.0;
  EXPECT_DOUBLE_EQ(source_surface.control_vertices[0].position[0], 1.0);
  EXPECT_DOUBLE_EQ(source_trim->control_vertices[0].position[0], 0.25);
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error));

  const uint64_t lower_arc_id =
      find_edge(*fixture.topology, static_cast<NurbsTopoEdge *>(fixture.topology->edges.first)->id)
          ->curve_id;
  const uint64_t upper_arc_id = find_edge(
                                    *fixture.topology,
                                    static_cast<NurbsTopoEdge *>(fixture.topology->edges.last)->id)
                                    ->curve_id;
  EXPECT_NE(lower_arc_id, upper_arc_id);
  BKE_nurbs_curve_list_free(copied_curves);
  BKE_nurbs_topology_free(copy);
}

TEST_F(NurbsTopologyTest, ClosedEdgeWireAndDisjointCurvesKeepIndependentIdentity)
{
  NurbsTopologyFixture fixture;
  const uint64_t first_edge_id = fixture.add_closed_circle_wire(4.0);
  const uint64_t second_edge_id = fixture.add_closed_circle_wire(8.0);
  NurbsTopoEdge *first_edge = find_edge(*fixture.topology, first_edge_id);
  NurbsTopoEdge *second_edge = find_edge(*fixture.topology, second_edge_id);
  ASSERT_NE(first_edge, nullptr);
  ASSERT_NE(second_edge, nullptr);
  EXPECT_EQ(first_edge->vertex_ids[0], first_edge->vertex_ids[1]);
  EXPECT_EQ(second_edge->vertex_ids[0], second_edge->vertex_ids[1]);
  EXPECT_NE(first_edge->curve_id, second_edge->curve_id);
  EXPECT_NE(first_edge->id, second_edge->id);

  std::string error;
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;
}

TEST_F(NurbsTopologyTest, NativeIDCopyOwnsIndependentTopologyAndPreservesComponentIDs)
{
  NurbsTopologyFixture fixture;
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_id_new<NurbsGeometry>(bmain, "Topology Fixture");
  BKE_nurbs_curve_list_copy(geometry->curves, fixture.curves);
  geometry->topology = BKE_nurbs_topology_copy(*fixture.topology);
  geometry->next_component_id = fixture.next_component_id;

  std::string error;
  ASSERT_TRUE(BKE_nurbs_topology_is_valid(geometry->curves, *geometry->topology, &error)) << error;
  Vector<uint64_t> expected_ids, source_ids;
  collect_all_ids(fixture, expected_ids);
  collect_all_ids(*geometry, source_ids);
  std::sort(expected_ids.begin(), expected_ids.end());
  std::sort(source_ids.begin(), source_ids.end());
  ASSERT_EQ(source_ids.size(), expected_ids.size());
  EXPECT_TRUE(std::equal(source_ids.begin(), source_ids.end(), expected_ids.begin()));

  NurbsGeometry *copy = id_cast<NurbsGeometry *>(BKE_id_copy(bmain, &geometry->id));
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->topology, nullptr);
  ASSERT_NE(copy->topology, geometry->topology);
  EXPECT_NE(copy->curves.first, geometry->curves.first);
  EXPECT_NE(copy->topology->surfaces.first, geometry->topology->surfaces.first);
  EXPECT_NE(copy->topology->trim_curves.first, geometry->topology->trim_curves.first);
  EXPECT_EQ(copy->next_component_id, geometry->next_component_id);

  Vector<uint64_t> copy_ids;
  collect_all_ids(*copy, copy_ids);
  std::sort(copy_ids.begin(), copy_ids.end());
  ASSERT_EQ(copy_ids.size(), source_ids.size());
  EXPECT_TRUE(std::equal(copy_ids.begin(), copy_ids.end(), source_ids.begin()));
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(copy->curves, *copy->topology, &error)) << error;

  NurbsSurface *source_surface = find_surface(*geometry->topology, fixture.surface_a_id);
  NurbsSurface *copied_surface = find_surface(*copy->topology, fixture.surface_a_id);
  ASSERT_NE(source_surface, nullptr);
  ASSERT_NE(copied_surface, nullptr);
  EXPECT_NE(copied_surface->control_vertices, source_surface->control_vertices);
  copied_surface->control_vertices[0].position[0] += 12.0;
  EXPECT_DOUBLE_EQ(source_surface->control_vertices[0].position[0], 1.0);
  EXPECT_DOUBLE_EQ(copied_surface->control_vertices[0].position[0], 13.0);
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(geometry->curves, *geometry->topology, &error)) << error;

  BKE_id_free(bmain, &copy->id);
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(geometry->curves, *geometry->topology, &error)) << error;
  BKE_main_free(bmain);
}

TEST_F(NurbsTopologyTest, AffineTransformMovesTopologySupportsAndLeavesUVSupportsExact)
{
  NurbsTopologyFixture fixture;
  Main *bmain = BKE_main_new();
  NurbsGeometry *geometry = BKE_id_new<NurbsGeometry>(bmain, "Topology Transform Fixture");
  BKE_nurbs_curve_list_copy(geometry->curves, fixture.curves);
  geometry->topology = BKE_nurbs_topology_copy(*fixture.topology);
  geometry->next_component_id = fixture.next_component_id;

  NurbsCurve *shared_curve = find_curve(geometry->curves, fixture.shared_edge_curve_id);
  NurbsSurface *surface = find_surface(*geometry->topology, fixture.surface_a_id);
  NurbsTopoEdge *shared_edge = find_edge(*geometry->topology, fixture.shared_edge_id);
  ASSERT_NE(shared_edge, nullptr);
  NurbsTopoVertex *seam_vertex = nullptr;
  for (NurbsTopoVertex &vertex : geometry->topology->vertices) {
    if (vertex.id == shared_edge->vertex_ids[1]) {
      seam_vertex = &vertex;
      break;
    }
  }
  ASSERT_NE(shared_curve, nullptr);
  ASSERT_NE(surface, nullptr);
  ASSERT_NE(seam_vertex, nullptr);
  const std::array<double, 3> curve_point = {shared_curve->control_vertices[0].position[0],
                                             shared_curve->control_vertices[0].position[1],
                                             shared_curve->control_vertices[0].position[2]};
  const std::array<double, 3> surface_point = {surface->control_vertices[0].position[0],
                                               surface->control_vertices[0].position[1],
                                               surface->control_vertices[0].position[2]};
  const std::array<double, 3> vertex_point = {
      seam_vertex->position[0], seam_vertex->position[1], seam_vertex->position[2]};

  Vector<double> trim_values_before;
  for (const NurbsCurve &trim : geometry->topology->trim_curves) {
    for (int i = 0; i < trim.control_vertex_count; i++) {
      for (double coordinate : trim.control_vertices[i].position) {
        trim_values_before.append(coordinate);
      }
    }
    for (int i = 0; i < trim.knot_count; i++) {
      trim_values_before.append(trim.knots[i]);
    }
  }

  float4x4 matrix = float4x4::identity();
  matrix[0][0] = 2.0f;
  matrix[0][1] = 0.25f;
  matrix[1][1] = 3.0f;
  matrix[2][2] = 0.5f;
  matrix.location() = float3(5.0f, -2.0f, 7.0f);
  const double4x4 double_matrix(matrix);
  const auto expected_transform = [&](const std::array<double, 3> &point) {
    return math::transform_point(double_matrix, double3(point.data()));
  };
  const double3 expected_curve = expected_transform(curve_point);
  const double3 expected_surface = expected_transform(surface_point);
  const double3 expected_vertex = expected_transform(vertex_point);

  BKE_nurbs_transform(*geometry, matrix);

  for (int axis = 0; axis < 3; axis++) {
    EXPECT_NEAR(shared_curve->control_vertices[0].position[axis], expected_curve[axis], 1e-6);
    EXPECT_NEAR(surface->control_vertices[0].position[axis], expected_surface[axis], 1e-6);
    EXPECT_NEAR(seam_vertex->position[axis], expected_vertex[axis], 1e-6);
  }
  Vector<double> trim_values_after;
  for (const NurbsCurve &trim : geometry->topology->trim_curves) {
    for (int i = 0; i < trim.control_vertex_count; i++) {
      for (double coordinate : trim.control_vertices[i].position) {
        trim_values_after.append(coordinate);
      }
    }
    for (int i = 0; i < trim.knot_count; i++) {
      trim_values_after.append(trim.knots[i]);
    }
  }
  ASSERT_EQ(trim_values_after.size(), trim_values_before.size());
  for (int i = 0; i < trim_values_before.size(); i++) {
    EXPECT_DOUBLE_EQ(trim_values_after[i], trim_values_before[i]);
  }

  std::string error;
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(geometry->curves, *geometry->topology, &error)) << error;
  BKE_main_free(bmain);
}

}  // namespace blender::bke::tests
