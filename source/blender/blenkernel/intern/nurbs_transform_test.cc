/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

#include "BLI_listbase.h"
#include "BLI_math_vector.hh"

#include "BKE_curve.hh"
#include "BKE_nurbs_transform.hh"

#include "nurbs_test_geometry.hh"
#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

template<typename T> static T &first_transform_item(const ListBaseT<T> &list)
{
  return *static_cast<T *>(list.first);
}

static OwnedGeometry weighted_patch()
{
  Nurb source{};
  source.type = CU_NURBS;
  source.pntsu = source.pntsv = source.orderu = source.orderv = 2;
  source.flagu = source.flagv = CU_NURB_ENDPOINT;
  source.topology_face_id = 20;
  const int vertices[4] = {1, 2, 3, 4}, edges[4] = {10, 11, 12, 13};
  std::copy_n(vertices, 4, source.topology_vertex_ids);
  std::copy_n(edges, 4, source.topology_edge_ids);
  BPoint points[4] = {};
  const float weights[4] = {1, 2, 3, 5};
  for (int i = 0; i < 4; i++) {
    points[i].vec[0] = i % 2;
    points[i].vec[1] = i / 2;
    points[i].vec[3] = weights[i];
  }
  source.bp = points;
  float u[4] = {2, 2, 5, 5}, v[4] = {-1, -1, 3, 3};
  source.knotsu = u;
  source.knotsv = v;
  ListBaseT<Nurb> list{nullptr, nullptr};
  BLI_addtail(&list, &source);
  return OwnedGeometry(geometry_from_tagged_legacy(list));
}

static void expect_bases_and_ids_unchanged(const NurbsGeometry &before, const NurbsGeometry &after)
{
  EXPECT_EQ(before.next_id, after.next_id);
  EXPECT_EQ(before.vertices.count(), after.vertices.count());
  EXPECT_EQ(before.edges.count(), after.edges.count());
  EXPECT_EQ(before.faces.count(), after.faces.count());
  const auto index = TopologyIndex::build(after);
  ASSERT_TRUE(index.has_value());
  for (const NurbsTopologyVertex &vertex : before.vertices) {
    EXPECT_NE(index->vertex(vertex.id), nullptr);
  }
  for (const NurbsTopologyEdge &edge : before.edges) {
    const auto *copy = index->edge(edge.id);
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->curve_id, edge.curve_id);
    EXPECT_EQ(copy->vertex_ids[0], edge.vertex_ids[0]);
    EXPECT_EQ(copy->vertex_ids[1], edge.vertex_ids[1]);
    EXPECT_EQ(copy->parameter_range[0], edge.parameter_range[0]);
    EXPECT_EQ(copy->parameter_range[1], edge.parameter_range[1]);
  }
  const NurbsCurveGeometry *curve = static_cast<const NurbsCurveGeometry *>(after.curves.first);
  for (const NurbsCurveGeometry &original : before.curves) {
    ASSERT_NE(curve, nullptr);
    EXPECT_EQ(original.id, curve->id);
    EXPECT_EQ(original.order, curve->order);
    EXPECT_EQ(original.dimension, curve->dimension);
    EXPECT_EQ(original.points_num, curve->points_num);
    EXPECT_EQ(std::memcmp(original.knots, curve->knots, sizeof(double) * original.knots_num), 0);
    for (int i = 0; i < original.points_num; i++) {
      EXPECT_EQ(original.points[i].weight, curve->points[i].weight);
      if (original.dimension == 2) {
        EXPECT_EQ(std::memcmp(
                      original.points[i].position, curve->points[i].position, 3 * sizeof(double)),
                  0);
      }
    }
    curve = curve->next;
  }
  EXPECT_EQ(curve, nullptr);
  const NurbsSurfaceGeometry *surface = static_cast<const NurbsSurfaceGeometry *>(
      after.surfaces.first);
  for (const NurbsSurfaceGeometry &original : before.surfaces) {
    ASSERT_NE(surface, nullptr);
    EXPECT_EQ(original.id, surface->id);
    EXPECT_EQ(original.order_u, surface->order_u);
    EXPECT_EQ(original.order_v, surface->order_v);
    EXPECT_EQ(original.points_num, surface->points_num);
    EXPECT_EQ(
        std::memcmp(original.knots_u, surface->knots_u, sizeof(double) * original.knots_u_num), 0);
    EXPECT_EQ(
        std::memcmp(original.knots_v, surface->knots_v, sizeof(double) * original.knots_v_num), 0);
    for (int i = 0; i < original.points_num; i++) {
      EXPECT_EQ(original.points[i].weight, surface->points[i].weight);
    }
    surface = surface->next;
  }
  EXPECT_EQ(surface, nullptr);
}

TEST(nurbs_vertex_transform, WeightedTensorCornerMovementPreservesBasisAndOtherVertices)
{
  const auto source = weighted_patch();
  ASSERT_NE(source, nullptr);
  std::string error;
  const auto result = translate_vertices(*source, {1, 1}, {0, 0, 1}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->position_error_bound, 0);
  expect_bases_and_ids_unchanged(*source, *result->geometry);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->vertex(1)->position[2], 1);
  EXPECT_EQ(first_transform_item(source->surfaces).points[0].position[2], 0);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(first_transform_item(result->geometry->surfaces).points[i].position[2],
              i == 0 ? 1 : 0);
  }
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    if (vertex.id != 1) {
      EXPECT_EQ(
          std::memcmp(vertex.position, index->vertex(vertex.id)->position, 3 * sizeof(double)), 0);
    }
  }
  auto options = preview_options();
  options.boundary.evaluation_allowance = 1e-12;
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error).has_value()) << error;
}

TEST(nurbs_vertex_transform, SharedVertexMovesEveryIncidentSupportOnce)
{
  const auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  const auto &edge = first_transform_item(sheet->edges);
  auto adjacent = extrude_edges(*sheet, {edge.id}, {0, 0, 2}, 0);
  ASSERT_TRUE(adjacent.has_value());
  const int64_t id = edge.vertex_ids[0];
  std::string error;
  auto result = translate_vertices(*adjacent->geometry, {id}, {0, 1, 0}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  expect_bases_and_ids_unchanged(*adjacent->geometry, *result->geometry);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->vertex(id)->position[1], 0);
  auto options = preview_options();
  options.boundary.evaluation_allowance = 0;
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error).has_value()) << error;
}

TEST(nurbs_vertex_transform, CompleteRuledArcBoundaryMovesWithoutChangingRationalProfile)
{
  for (const bool closed : {false, true}) {
    const auto source = make_arc_sheet(closed);
    ASSERT_NE(source, nullptr);
    Vector<int64_t> selected;
    for (const NurbsTopologyVertex &vertex : source->vertices) {
      if (vertex.position[2] == 2) {
        selected.append(vertex.id);
      }
    }
    std::string error;
    auto result = translate_vertices(*source, selected, {1, 0, 1}, 0, &error);
    ASSERT_TRUE(result.has_value()) << error;
    expect_bases_and_ids_unchanged(*source, *result->geometry);
    const auto &original = first_transform_item(source->surfaces);
    const auto &moved = first_transform_item(result->geometry->surfaces);
    for (int i = 0; i < original.points_num; i++) {
      const double3 offset = i < original.points_u ? double3(0) : double3(1, 0, 1);
      EXPECT_EQ(double3(moved.points[i].position), double3(original.points[i].position) + offset);
    }
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    options.boundary.segments_per_span = 13;
    options.interior_subdivisions = 13;
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error).has_value()) << error;
  }
}

TEST(nurbs_vertex_transform, PartialCurvedEndpointRejectsWithoutChangingSource)
{
  const auto source = make_arc_sheet(false);
  ASSERT_NE(source, nullptr);
  const int64_t id = first_transform_item(source->vertices).id;
  const auto &surface = first_transform_item(source->surfaces);
  const double3 point(surface.points[0].position);
  std::string error;
  EXPECT_FALSE(translate_vertices(*source, {id}, {1, 0, 0}, 0, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(double3(surface.points[0].position), point);
  auto noop = translate_vertices(*source, {id}, {0, 0, 0}, 0, &error);
  ASSERT_TRUE(noop.has_value()) << error;
  expect_bases_and_ids_unchanged(*source, *noop->geometry);
  EXPECT_EQ(double3(first_transform_item(noop->geometry->surfaces).points[0].position), point);
}

TEST(nurbs_vertex_transform, IndependentOwnersOfOneCurveRejectConflictingRequests)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*source, {0, 0, 0});
  const auto *b = add_vertex(*source, {1, 0, 0});
  const auto *c = add_vertex(*source, {0, 0, 0});
  const auto *d = add_vertex(*source, {1, 0, 0});
  const auto *curve = add_linear_curve(*source, {0, 0, 0}, {1, 0, 0}, 3);
  add_edge(*source, *a, *b, *curve);
  add_edge(*source, *c, *d, *curve);
  std::string error;
  EXPECT_FALSE(translate_vertices(*source, {a->id, b->id}, {0, 1, 0}, 0, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(curve->points[0].position[1], 0);
}

TEST(nurbs_vertex_transform, InconsistentRationalBoundaryMappingIsNotFitted)
{
  const auto source = weighted_patch();
  ASSERT_NE(source, nullptr);
  auto &face = first_transform_item(source->faces);
  const auto &use = first_transform_item(first_transform_item(face.loops).edge_uses);
  for (NurbsCurveGeometry &curve : source->curves) {
    if (curve.id == use.pcurve_id) {
      curve.points[0].weight *= 2;
    }
  }
  std::string error;
  EXPECT_FALSE(translate_vertices(*source, {1}, {0, 0, 1}, 0, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(first_transform_item(source->surfaces).points[0].position[2], 0);
}

TEST(nurbs_vertex_transform, BoundaryWeightAgreementDoesNotMultiplyOrDivideExtremeWeights)
{
  for (const int exponent : {-1000, 1000}) {
    const auto source = weighted_patch();
    ASSERT_NE(source, nullptr);
    const double scale = std::ldexp(1.0, exponent);
    for (NurbsCurveGeometry &curve : source->curves) {
      for (int i = 0; i < curve.points_num; i++) {
        curve.points[i].weight *= scale;
      }
    }
    for (NurbsSurfaceGeometry &surface : source->surfaces) {
      for (int i = 0; i < surface.points_num; i++) {
        surface.points[i].weight *= scale;
      }
    }
    std::string error;
    auto result = translate_vertices(*source, {1}, {0, 0, 1}, 0, &error);
    ASSERT_TRUE(result.has_value()) << exponent << ": " << error;
    expect_bases_and_ids_unchanged(*source, *result->geometry);
    EXPECT_EQ(result->position_error_bound, 0);
  }
}

TEST(nurbs_vertex_transform, OneBitRationalMappingDifferenceIsNotTreatedAsExact)
{
  const auto source = weighted_patch();
  ASSERT_NE(source, nullptr);
  const auto &face = first_transform_item(source->faces);
  const auto &use = first_transform_item(first_transform_item(face.loops).edge_uses);
  for (NurbsCurveGeometry &curve : source->curves) {
    if (curve.id == use.pcurve_id) {
      curve.points[0].weight = std::nextafter(curve.points[0].weight,
                                              std::numeric_limits<double>::infinity());
    }
  }
  std::string error;
  EXPECT_FALSE(translate_vertices(*source, {1}, {0, 0, 1}, 0, &error).has_value());
  EXPECT_FALSE(error.empty());
}

TEST(nurbs_vertex_transform, RoundoffBudgetIsExplicitAndUntouchedAxesKeepTheirBits)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*source, {1e15, -0.0, 0});
  const auto *b = add_vertex(*source, {1e15 + 1, -0.0, 0});
  auto *curve = add_linear_curve(*source, double3(a->position), double3(b->position), 3);
  add_edge(*source, *a, *b, *curve);
  std::string error;
  EXPECT_FALSE(translate_vertices(*source, {a->id}, {0.1, 0, 0}, 0, &error).has_value());
  auto result = translate_vertices(*source, {a->id}, {0.1, 0, 0}, 0.1, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_GT(result->position_error_bound, 0);
  EXPECT_LE(result->position_error_bound, 0.1);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->vertex(a->id)->position[0], 1e15 + 0.1);
  EXPECT_EQ(std::bit_cast<uint64_t>(index->vertex(a->id)->position[1]),
            std::bit_cast<uint64_t>(a->position[1]));
  EXPECT_EQ(index->vertex(b->id)->position[0], b->position[0]);
  EXPECT_EQ(a->position[0], 1e15);
  curve->points[1].weight = -1;
  EXPECT_FALSE(translate_vertices(*source, {a->id}, {0, 1, 0}, 0, &error).has_value());
  EXPECT_FALSE(error.empty());
}

TEST(nurbs_vertex_transform, LooseVertexMovesWithoutTouchingUnownedGeometry)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *vertex = add_vertex(*source, {1, 2, 3});
  const auto *orphan = add_linear_curve(*source, {5, 6, 7}, {8, 9, 10}, 3);
  auto result = translate_vertices(*source, {vertex->id}, {1, 0, 0}, 0);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(first_transform_item(result->geometry->vertices).position[0], 2);
  EXPECT_EQ(std::memcmp(first_transform_item(result->geometry->curves).points,
                        orphan->points,
                        2 * sizeof(NurbsControlPoint)),
            0);
  std::string error;
  EXPECT_FALSE(translate_vertices(*source, {source->next_id}, {1, 0, 0}, 0, &error).has_value());
  EXPECT_FALSE(error.empty());
}

}  // namespace blender::bke::nurbs::tests
