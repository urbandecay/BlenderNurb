/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "BLI_listbase.h"
#include "BLI_math_vector.hh"

#include "BKE_curve.hh"
#include "BKE_nurbs_selection.hh"
#include "BKE_nurbs_topology_edit.hh"
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

static OwnedGeometry trimmed_ruled_fixture(Vector<int64_t> &cap_vertices,
                                           const bool nonclamped,
                                           const bool transpose,
                                           std::string &error)
{
  const auto full = make_arc_sheet(false);
  if (!full) {
    return nullptr;
  }
  const int64_t source_edge = first_transform_item(full->edges).id;
  auto wire = copy_component_closure(*full, {{}, {&source_edge, 1}, {}}, &error);
  if (!wire) {
    return nullptr;
  }
  auto &edge = first_transform_item(wire->geometry->edges);
  auto &curve = first_transform_item(wire->geometry->curves);
  if (nonclamped) {
    const double knots[6] = {-2, -1, 0, 1, 2, 3};
    std::copy_n(knots, 6, curve.knots);
  }
  edge.parameter_range[0] = 0.25;
  edge.parameter_range[1] = 0.75;
  int end = 0;
  for (NurbsTopologyVertex &vertex : wire->geometry->vertices) {
    const auto point = evaluate_curve(curve, edge.parameter_range[end++], &error);
    if (!point) {
      return nullptr;
    }
    std::copy_n(&point->position.x, 3, vertex.position);
  }
  auto sweep = extrude_edges(*wire->geometry, {edge.id}, {0, 0, 1}, 0, &error);
  if (!sweep) {
    return nullptr;
  }
  for (const NurbsTopologyVertex &vertex : wire->geometry->vertices) {
    cap_vertices.append(sweep->source_to_cap.lookup(vertex.id));
  }
  if (transpose) {
    auto &surface = first_transform_item(sweep->geometry->surfaces);
    auto *points = MEM_new_array<NurbsControlPoint>(surface.points_num, __func__);
    for (int v = 0; v < surface.points_v; v++) {
      for (int u = 0; u < surface.points_u; u++) {
        points[v + u * surface.points_v] = surface.points[u + v * surface.points_u];
      }
    }
    MEM_delete(surface.points);
    surface.points = points;
    std::swap(surface.points_u, surface.points_v);
    std::swap(surface.order_u, surface.order_v);
    std::swap(surface.knots_u, surface.knots_v);
    std::swap(surface.knots_u_num, surface.knots_v_num);
    for (NurbsCurveGeometry &pcurve : sweep->geometry->curves) {
      if (pcurve.dimension == 2) {
        for (int i = 0; i < pcurve.points_num; i++) {
          std::swap(pcurve.points[i].position[0], pcurve.points[i].position[1]);
        }
      }
    }
    for (NurbsTopologyFace &face : sweep->geometry->faces) {
      face.orientation = -face.orientation;
      for (NurbsTopologyLoop &loop : face.loops) {
        BLI_listbase_reverse(&loop.edge_uses);
        for (NurbsTopologyEdgeUse &use : loop.edge_uses) {
          use.reversed = !use.reversed;
        }
      }
    }
  }
  return std::move(sweep->geometry);
}

TEST(nurbs_vertex_transform, CompleteTrimmedRuledRowsTranslateInBothAxesAndNonclampedBases)
{
  for (const bool nonclamped : {false, true}) {
    for (const bool transpose : {false, true}) {
      SCOPED_TRACE(nonclamped);
      SCOPED_TRACE(transpose);
      Vector<int64_t> cap;
      std::string error;
      const auto source = trimmed_ruled_fixture(cap, nonclamped, transpose, error);
      ASSERT_NE(source, nullptr) << error;
      ASSERT_TRUE(geometry_validate_structure(*source, &error)) << error;
      const auto moved = translate_vertices(*source, cap, {0, 0, 0.125}, 0, &error);
      ASSERT_TRUE(moved.has_value()) << error;
      expect_bases_and_ids_unchanged(*source, *moved->geometry);
      const auto index = TopologyIndex::build(*moved->geometry);
      ASSERT_TRUE(index.has_value());
      for (const int64_t id : cap) {
        EXPECT_EQ(index->vertex(id)->position[2], 1.125);
      }
      const auto &before = first_transform_item(source->surfaces);
      const auto &after = first_transform_item(moved->geometry->surfaces);
      for (int i = 0; i < before.points_num; i++) {
        const bool selected_row = transpose ? i % before.points_u == 1 : i / before.points_u == 1;
        EXPECT_EQ(double3(after.points[i].position),
                  double3(before.points[i].position) + double3(0, 0, selected_row ? 0.125 : 0));
      }
      auto options = preview_options();
      options.boundary.evaluation_allowance = 1e-12;
      EXPECT_TRUE(tessellate_surfaces(*moved->geometry, options, &error)) << error;
    }
  }
}

TEST(nurbs_vertex_transform, TrimmedRuledRowsUseExactProjectiveProfilesAndRailComposition)
{
  for (const bool extreme : {false, true}) {
    Vector<int64_t> cap;
    std::string error;
    const auto source = trimmed_ruled_fixture(cap, false, false, error);
    ASSERT_NE(source, nullptr) << error;
    auto &surface = first_transform_item(source->surfaces);
    for (int i = 0; i < surface.points_num; i++) {
      const bool top = i >= surface.points_u;
      surface.points[i].weight = extreme ? std::ldexp(surface.points[i].weight, top ? 602 : 600) :
                                           surface.points[i].weight * (top ? 2 : 1);
    }
    Map<int64_t, NurbsCurveGeometry *> curves;
    for (NurbsCurveGeometry &curve : source->curves) {
      curves.add_new(curve.id, &curve);
      if (curve.dimension == 3 && curve.order == 2) {
        curve.points[0].weight = extreme ? std::ldexp(1.0, -600) : 1;
        curve.points[1].weight = extreme ? std::ldexp(1.0, -598) : 6;
      }
    }
    if (!extreme) {
      const auto index = TopologyIndex::build(*source);
      ASSERT_TRUE(index.has_value());
      for (NurbsTopologyLoop &loop : first_transform_item(source->faces).loops) {
        for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
          if (curves.lookup(index->edge(use.edge_id)->curve_id)->order == 2) {
            curves.lookup(use.pcurve_id)->points[1].weight = 3;
          }
        }
      }
    }
    const auto moved = translate_vertices(*source, cap, {0, 0, 0.125}, 0, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    expect_bases_and_ids_unchanged(*source, *moved->geometry);
    auto options = preview_options();
    options.boundary.evaluation_allowance = 1e-12;
    EXPECT_TRUE(tessellate_surfaces(*moved->geometry, options, &error)) << error;
  }
}

TEST(nurbs_vertex_transform, TrimmedEndpointAndOneBitRailMismatchAreNotFitted)
{
  Vector<int64_t> cap;
  std::string error;
  const auto source = trimmed_ruled_fixture(cap, false, false, error);
  ASSERT_NE(source, nullptr) << error;
  EXPECT_FALSE(translate_vertices(*source, cap.as_span().take_front(1), {0, 0, 0.125}, 0, &error));
  EXPECT_FALSE(error.empty());
  auto *rail = static_cast<NurbsCurveGeometry *>(source->curves.first);
  while (rail && (rail->dimension != 3 || rail->order != 2)) {
    rail = rail->next;
  }
  ASSERT_NE(rail, nullptr);
  rail->points[1].weight = std::nextafter(1.0, 2.0);
  const double stored = rail->points[1].weight;
  EXPECT_FALSE(translate_vertices(*source, cap, {0, 0, 0.125}, 0, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(rail->points[1].weight, stored);
  EXPECT_EQ(first_transform_item(source->surfaces).points[3].position[2], 1);
}

TEST(nurbs_vertex_transform, NonproportionalTrimmedRowsRejectWithoutChangingOwnership)
{
  Vector<int64_t> cap;
  std::string error;
  const auto source = trimmed_ruled_fixture(cap, false, false, error);
  ASSERT_NE(source, nullptr) << error;
  auto &surface = first_transform_item(source->surfaces);
  const int64_t counter = source->next_id;
  surface.points[surface.points_u + 1].weight = std::nextafter(
      surface.points[surface.points_u + 1].weight, 1.0);
  /* Keep each horizontal curve exactly matched to its own support row. The rejected
   * correspondence is the non-constant ratio between the two rows' weight profiles. */
  Map<int64_t, NurbsCurveGeometry *> curves;
  Map<int64_t, NurbsTopologyVertex *> vertices;
  for (NurbsCurveGeometry &curve : source->curves) {
    curves.add_new(curve.id, &curve);
  }
  for (NurbsTopologyVertex &vertex : source->vertices) {
    vertices.add_new(vertex.id, &vertex);
  }
  for (NurbsTopologyEdge &edge : source->edges) {
    auto &curve = *curves.lookup(edge.curve_id);
    if (curve.order == 3 && curve.points[0].position[2] == 1) {
      for (int i = 0; i < curve.points_num; i++) {
        curve.points[i].weight = surface.points[surface.points_u + i].weight;
      }
      for (int end = 0; end < 2; end++) {
        const auto point = evaluate_curve(curve, edge.parameter_range[end], &error);
        ASSERT_TRUE(point.has_value()) << error;
        std::copy_n(&point->position.x, 3, vertices.lookup(edge.vertex_ids[end])->position);
      }
    }
  }
  for (NurbsTopologyEdge &edge : source->edges) {
    auto &curve = *curves.lookup(edge.curve_id);
    if (curve.order == 2) {
      for (int end = 0; end < 2; end++) {
        std::copy_n(
            vertices.lookup(edge.vertex_ids[end])->position, 3, curve.points[end].position);
      }
    }
  }
  EXPECT_FALSE(translate_vertices(*source, cap, {0, 0, 0.125}, 0, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(source->surfaces.count(), 1);
}

TEST(nurbs_vertex_transform, TrimmedRowSupportOwnersMustRequestTheSameTranslation)
{
  Vector<int64_t> cap;
  std::string error;
  const auto source = trimmed_ruled_fixture(cap, false, false, error);
  ASSERT_NE(source, nullptr) << error;
  const auto &face = first_transform_item(source->faces);
  const int64_t face_id = face.id;
  auto copy = copy_component_closure(*source, {{}, {}, {&face_id, 1}}, &error);
  ASSERT_TRUE(copy.has_value()) << error;
  first_transform_item(copy->geometry->faces).surface_id = face.surface_id;
  source->next_id = copy->geometry->next_id;
  BLI_movelisttolist(&source->curves, &copy->geometry->curves);
  BLI_movelisttolist(&source->surfaces, &copy->geometry->surfaces);
  BLI_movelisttolist(&source->vertices, &copy->geometry->vertices);
  BLI_movelisttolist(&source->edges, &copy->geometry->edges);
  BLI_movelisttolist(&source->faces, &copy->geometry->faces);
  EXPECT_FALSE(translate_vertices(*source, cap, {0, 0, 0.125}, 0, &error));
  EXPECT_NE(error.find("conflicting"), std::string::npos);
  Vector<int64_t> both = cap;
  for (const int64_t id : cap) {
    both.append(copy->source_to_copy.lookup(id));
  }
  const auto moved = translate_vertices(*source, both, {0, 0, 0.125}, 0, &error);
  ASSERT_TRUE(moved.has_value()) << error;
  const auto &surface = first_transform_item(moved->geometry->surfaces);
  EXPECT_EQ(surface.points[3].position[2], 1.125);
  EXPECT_EQ(surface.points[4].position[2], 1.125);
  EXPECT_EQ(surface.points[5].position[2], 1.125);
  const auto &unused = *static_cast<const NurbsSurfaceGeometry *>(moved->geometry->surfaces.last);
  EXPECT_EQ(unused.points[3].position[2], 1);
}

TEST(nurbs_vertex_transform, RuledBoundaryGapBeyondDeclaredToleranceRejects)
{
  Vector<int64_t> cap;
  std::string error;
  const auto source = trimmed_ruled_fixture(cap, false, false, error);
  ASSERT_NE(source, nullptr) << error;
  auto &surface = first_transform_item(source->surfaces);
  constexpr double shift = 1.0 / 1024;
  const double tolerance = shift + 1e-12;
  for (int i = surface.points_u; i < surface.points_num; i++) {
    surface.points[i].position[0] += shift;
  }
  for (NurbsCurveGeometry &curve : source->curves) {
    if (curve.dimension != 3) {
      continue;
    }
    if (curve.order == 3 && curve.points[0].position[2] == 1) {
      for (int i = 0; i < curve.points_num; i++) {
        curve.points[i].position[0] += shift;
      }
    }
    else if (curve.order == 2) {
      curve.points[1].position[0] -= shift;
    }
  }
  for (NurbsTopologyVertex &vertex : source->vertices) {
    vertex.tolerance = tolerance;
  }
  for (NurbsTopologyEdge &edge : source->edges) {
    edge.tolerance = tolerance;
  }
  for (NurbsTopologyFace &face : source->faces) {
    face.tolerance = tolerance;
    for (NurbsTopologyLoop &loop : face.loops) {
      for (NurbsTopologyEdgeUse &use : loop.edge_uses) {
        use.tolerance = tolerance;
      }
    }
  }
  EXPECT_FALSE(translate_vertices(*source, cap, {0, 0, 0.125}, 0, &error));
  EXPECT_NE(error.find("support tolerance"), std::string::npos);
  EXPECT_EQ(surface.points[3].position[2], 1);
}

TEST(nurbs_vertex_transform, AffineWeightedCornersKeepSharedGeometryAndRationalBasis)
{
  const auto source = weighted_patch();
  ASSERT_NE(source, nullptr);
  const auto *unowned = add_linear_curve(*source, {10, 0, 0}, {11, 0, 0}, 3);
  const int64_t unowned_id = unowned->id;
  double4x4 matrix = double4x4::identity();
  matrix[0][0] = -1;
  matrix[1][1] = 2;
  matrix[2][2] = 0.5;
  matrix.location() = double3(2, 1, 3);
  std::string error;
  const auto result = transform_vertices(
      *source, {{1, matrix}, {2, matrix}, {1, matrix}}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  expect_bases_and_ids_unchanged(*source, *result->geometry);
  EXPECT_EQ(result->position_error_bound, 0);
  EXPECT_EQ(result->tolerance_scale_bound, 2);
  const auto &before = first_transform_item(source->surfaces);
  const auto &after = first_transform_item(result->geometry->surfaces);
  for (int i = 0; i < 4; i++) {
    const double3 expected = i < 2 ? double3(2 - before.points[i].position[0], 1, 3) :
                                     double3(before.points[i].position);
    EXPECT_EQ(double3(after.points[i].position), expected);
  }
  auto options = preview_options();
  options.boundary.evaluation_allowance = 1e-12;
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
  EXPECT_EQ(before.points[0].position[2], 0);
  for (const auto &curve : result->geometry->curves) {
    if (curve.id == unowned_id) {
      EXPECT_EQ(double3(curve.points[0].position), double3(10, 0, 0));
      EXPECT_EQ(double3(curve.points[1].position), double3(11, 0, 0));
    }
  }
  matrix = double4x4::identity();
  matrix[0][0] = 0;
  const auto flattened = transform_vertices(
      *source, {{1, matrix}, {2, matrix}, {3, matrix}, {4, matrix}}, 0, &error);
  ASSERT_TRUE(flattened.has_value()) << error;
  EXPECT_TRUE(geometry_validate_structure(*flattened->geometry, &error)) << error;
  expect_bases_and_ids_unchanged(*source, *flattened->geometry);
}

TEST(nurbs_vertex_transform, AffineRuledArcAndCircleRowsRetainExactRationalConstruction)
{
  for (const bool closed : {false, true}) {
    const auto source = make_arc_sheet(closed);
    ASSERT_NE(source, nullptr);
    double4x4 matrix = double4x4::identity();
    matrix[0][0] = matrix[1][1] = 0;
    matrix[0][1] = 2;
    matrix[1][0] = -2;
    matrix.location() = double3(1, -1, 0);
    Vector<VertexTransform> transforms;
    for (const NurbsTopologyVertex &vertex : source->vertices) {
      if (vertex.position[2] == 2) {
        transforms.append({vertex.id, matrix});
      }
    }
    std::string error;
    const auto result = transform_vertices(*source, transforms, 0, &error);
    ASSERT_TRUE(result.has_value()) << error;
    expect_bases_and_ids_unchanged(*source, *result->geometry);
    const auto &before = first_transform_item(source->surfaces);
    const auto &after = first_transform_item(result->geometry->surfaces);
    for (int i = 0; i < before.points_num; i++) {
      const double3 point(before.points[i].position);
      EXPECT_EQ(double3(after.points[i].position),
                i < before.points_u ? point : double3(1 - 2 * point.y, -1 + 2 * point.x, point.z));
    }
    auto options = preview_options();
    options.boundary.evaluation_allowance = 1e-12;
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
  }
}

TEST(nurbs_vertex_transform, DifferentAffineOriginsApplyOnceAndRejectConflictingRequests)
{
  const auto source = weighted_patch();
  double4x4 a = double4x4::identity(), b = a;
  a.location().z = 1;
  b.location().z = -2;
  std::string error;
  const auto result = transform_vertices(*source, {{1, a}, {2, b}}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  expect_bases_and_ids_unchanged(*source, *result->geometry);
  const auto index = TopologyIndex::build(*result->geometry);
  EXPECT_EQ(index->vertex(1)->position[2], 1);
  EXPECT_EQ(index->vertex(2)->position[2], -2);
  EXPECT_EQ(first_transform_item(result->geometry->surfaces).points[0].position[2], 1);
  EXPECT_EQ(first_transform_item(result->geometry->surfaces).points[1].position[2], -2);
  EXPECT_FALSE(transform_vertices(*source, {{1, a}, {1, b}}, 0, &error));
  EXPECT_NE(error.find("conflicting"), std::string::npos);
  EXPECT_EQ(first_transform_item(source->surfaces).points[0].position[2], 0);
}

TEST(nurbs_vertex_transform, AffineFailuresRetainSourceAndNoopAllowsCurvedSelection)
{
  const auto source = make_arc_sheet(false);
  const int64_t id = first_transform_item(source->vertices).id;
  const auto identity = double4x4::identity();
  std::string error;
  EXPECT_TRUE(transform_vertices(*source, {{id, identity}}, 0, &error)) << error;
  double4x4 matrix = identity;
  matrix[0][0] = 2;
  EXPECT_FALSE(transform_vertices(*source, {{id, matrix}}, 0, &error));
  EXPECT_NE(error.find("fit-point"), std::string::npos);
  matrix[0][3] = 1;
  EXPECT_FALSE(transform_vertices(*source, {{id, matrix}}, 0, &error));
  matrix = identity;
  matrix.location().x = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(transform_vertices(*source, {{id, matrix}}, 0, &error));
  const auto patch = weighted_patch();
  matrix = identity;
  matrix.location().x = 0.1;
  EXPECT_FALSE(transform_vertices(*patch, {{2, matrix}}, 0, &error));
  EXPECT_NE(error.find("roundoff"), std::string::npos);
  const auto result = transform_vertices(*patch, {{2, matrix}}, 1e-14, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_GT(result->position_error_bound, 0);
  EXPECT_EQ(first_transform_item(patch->surfaces).points[1].position[0], 1);
}

TEST(nurbs_vertex_transform, AffineTrimmedRuledRowsUseTheSameCorrespondenceProof)
{
  Vector<int64_t> cap;
  std::string error;
  const auto source = trimmed_ruled_fixture(cap, false, false, error);
  ASSERT_NE(source, nullptr) << error;
  double4x4 matrix = double4x4::identity();
  matrix[0][0] = -1;
  matrix[1][1] = 2;
  Vector<VertexTransform> transforms;
  for (const int64_t id : cap) {
    transforms.append({id, matrix});
  }
  const auto result = transform_vertices(*source, transforms, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  expect_bases_and_ids_unchanged(*source, *result->geometry);
  auto options = preview_options();
  options.boundary.evaluation_allowance = 1e-12;
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
}

TEST(nurbs_vertex_transform, SharedAffineVertexAndRegionUpdateEachIncidentResourceOnce)
{
  const auto sheet = make_planar_sheet();
  const auto &edge = first_transform_item(sheet->edges);
  auto source = extrude_edges(*sheet, {edge.id}, {0, 0, 2}, 0);
  ASSERT_TRUE(source.has_value());
  double4x4 matrix = double4x4::identity();
  matrix[0][0] = 2;
  matrix[1][1] = 3;
  matrix[2][2] = 4;
  matrix.location().z = 1;
  for (const bool whole : {false, true}) {
    Vector<VertexTransform> transforms;
    Set<int64_t> selected;
    for (const auto &vertex : source->geometry->vertices) {
      if (whole || vertex.id == edge.vertex_ids[0]) {
        transforms.append({vertex.id, matrix});
        selected.add(vertex.id);
      }
    }
    std::string error;
    const auto result = transform_vertices(*source->geometry, transforms, 0, &error);
    ASSERT_TRUE(result.has_value()) << error;
    expect_bases_and_ids_unchanged(*source->geometry, *result->geometry);
    const auto index = TopologyIndex::build(*result->geometry);
    ASSERT_TRUE(index.has_value());
    for (const auto &vertex : source->geometry->vertices) {
      const double3 p(vertex.position);
      EXPECT_EQ(double3(index->vertex(vertex.id)->position),
                selected.contains(vertex.id) ? double3(2 * p.x, 3 * p.y, 4 * p.z + 1) : p);
    }
    Map<int64_t, const NurbsCurveGeometry *> curves;
    for (const auto &curve : result->geometry->curves) {
      curves.add_new(curve.id, &curve);
    }
    for (const auto &item : result->geometry->edges) {
      for (int end = 0; end < 2; end++) {
        const auto point = evaluate_curve(
            *curves.lookup(item.curve_id), item.parameter_range[end], &error);
        ASSERT_TRUE(point.has_value()) << error;
        EXPECT_EQ(point->position, double3(index->vertex(item.vertex_ids[end])->position));
      }
    }
    auto options = preview_options();
    options.boundary.evaluation_allowance = 1e-12;
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
  }
}

TEST(nurbs_vertex_transform, MergedPyramidCornersRemainEditable)
{
  const auto sheet = make_planar_sheet();
  const int64_t bottom = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  SelectionState selection;
  selection.mode = SelectionMode::Face;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  const auto selected = selection_apply(
      *box->geometry, selection, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(selected.has_value()) << error;
  const auto pyramid = merge_selected_vertices(
      *box->geometry, *selected, VertexMergeMode::Center, {}, 0, &error);
  ASSERT_TRUE(pyramid.has_value()) << error;
  for (const auto &vertex : pyramid->geometry->vertices) {
    SCOPED_TRACE(vertex.id);
    const double3 offset(0.25, -0.125, 0.5);
    const auto moved = translate_vertices(
        *pyramid->geometry, {&vertex.id, 1}, offset, 1e-9, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    expect_bases_and_ids_unchanged(*pyramid->geometry, *moved->geometry);
    const auto index = TopologyIndex::build(*moved->geometry);
    EXPECT_EQ(double3(index->vertex(vertex.id)->position), double3(vertex.position) + offset);
    SurfacePreviewOptions options;
    options.boundary.segments_per_span = options.interior_subdivisions = 7;
    EXPECT_TRUE(tessellate_surfaces(*moved->geometry, options, &error)) << error;
    auto rotation = double4x4::identity();
    rotation[0][0] = rotation[1][1] = 0;
    rotation[0][1] = 1;
    rotation[1][0] = -1;
    auto scale = double4x4::identity();
    scale[0][0] = 1.25;
    scale[1][1] = 0.5;
    scale[2][2] = 2;
    auto singular = double4x4::identity();
    singular[2][2] = 0;
    for (const double4x4 &matrix : {rotation, scale, singular}) {
      const VertexTransform request{vertex.id, matrix};
      const auto transformed = transform_vertices(*moved->geometry, {&request, 1}, 1e-9, &error);
      ASSERT_TRUE(transformed.has_value()) << error;
      expect_bases_and_ids_unchanged(*moved->geometry, *transformed->geometry);
      const double3 p = double3(index->vertex(vertex.id)->position);
      const double3 target = double3(matrix * double4(p, 1));
      const auto output = TopologyIndex::build(*transformed->geometry);
      for (const auto &old : moved->geometry->vertices) {
        EXPECT_EQ(double3(output->vertex(old.id)->position),
                  old.id == vertex.id ? target : double3(old.position));
      }
      EXPECT_TRUE(tessellate_surfaces(*transformed->geometry, options, &error)) << error;
      Vector<int64_t> base_vertices;
      for (const auto &old : pyramid->geometry->vertices) {
        if (old.position[2] == 0) {
          base_vertices.append(old.id);
        }
      }
      const double3 center = (double3(output->vertex(base_vertices[0])->position) +
                              double3(output->vertex(base_vertices[1])->position)) *
                             0.5;
      const VertexMergeGroup edge_merge{
          {base_vertices[0], base_vertices[1]}, base_vertices[0], center};
      const auto merged_again = merge_vertex_groups(
          *transformed->geometry, {&edge_merge, 1}, 1e-9, &error);
      ASSERT_TRUE(merged_again.has_value()) << error;
      EXPECT_TRUE(tessellate_surfaces(*merged_again->geometry, options, &error)) << error;
      SelectionState all;
      all.mode = SelectionMode::Vertex;
      for (const auto &item : transformed->geometry->vertices) {
        all.vertices.add(item.id);
      }
      const auto normalized = selection_flush(*transformed->geometry, all, all.mode, &error);
      ASSERT_TRUE(normalized.has_value()) << error;
      const auto collapsed = merge_selected_vertices(
          *transformed->geometry, *normalized, VertexMergeMode::Center, {}, 1e-9, &error);
      ASSERT_TRUE(collapsed.has_value()) << error;
      EXPECT_EQ(collapsed->geometry->vertices.count(), 1);
      EXPECT_TRUE(collapsed->geometry->faces.is_empty());
    }
  }
}

static OwnedGeometry trimmed_triangle(const Span<double2> corners)
{
  auto geometry = make_planar_sheet();
  auto &face = first_transform_item(geometry->faces);
  for (auto &loop : face.loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&face.loops);
  add_planar_loop(*geometry, face, corners, false);
  return geometry;
}

TEST(nurbs_vertex_transform, TriangleDisplacementsUseTrimParametersAndIndividualMatrices)
{
  const double2 cases[][3] = {{{0, 0}, {1, 0}, {0, 1}},
                              {{0.125, 0.25}, {0.875, 0.25}, {0.25, 0.75}},
                              {{0.1, 0.2}, {0.9, 0.3}, {0.2, 0.8}}};
  for (const auto &corners : cases) {
    const auto source = trimmed_triangle({corners, 3});
    const auto index = TopologyIndex::build(*source);
    const auto &face = first_transform_item(source->faces);
    const auto &loop = first_transform_item(face.loops);
    Vector<VertexTransform> transforms;
    int i = 0;
    for (const auto &use : loop.edge_uses) {
      const int64_t id = index->edge(use.edge_id)->vertex_ids[0];
      auto matrix = double4x4::identity();
      i++;
      matrix.location() = double3(0.125 * i, 0, 0.25 * i);
      transforms.append({id, matrix});
    }
    const auto before = OwnedGeometry(geometry_copy(source.get()));
    std::string error;
    const auto result = transform_vertices(*source, transforms, 1e-9, &error);
    ASSERT_TRUE(result.has_value()) << error;
    expect_source_geometry_equal(*source, *before);
    expect_bases_and_ids_unchanged(*source, *result->geometry);
    const auto output = TopologyIndex::build(*result->geometry);
    for (const auto &request : transforms) {
      EXPECT_EQ(double3(output->vertex(request.vertex_id)->position),
                double3(index->vertex(request.vertex_id)->position) + request.matrix.location());
    }
    SurfacePreviewOptions options;
    options.boundary.segments_per_span = options.interior_subdivisions = 7;
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
    if (result->position_error_bound > 0) {
      EXPECT_FALSE(transform_vertices(*source, transforms, 0, &error));
      expect_source_geometry_equal(*source, *before);
    }
  }
}

TEST(nurbs_vertex_transform, InvalidTriangleFieldsRejectAtomically)
{
  const double2 corners[] = {{0, 0}, {1, 0}, {0, 1}};
  const double2 degenerate[] = {{0, 0}, {1, 0}, {0, 0}};
  for (int failure = 0; failure < 5; failure++) {
    auto source = trimmed_triangle({failure == 4 ? degenerate : corners, 3});
    auto &face = first_transform_item(source->faces);
    auto &loop = first_transform_item(face.loops);
    auto &use = first_transform_item(loop.edge_uses);
    const auto index = TopologyIndex::build(*source);
    const int64_t id = index->edge(use.edge_id)->vertex_ids[0];
    if (failure == 0) {
      first_transform_item(source->surfaces).points[3].weight = 2;
    }
    else if (failure != 4) {
      for (auto &curve : source->curves) {
        if (curve.id != use.pcurve_id) {
          continue;
        }
        if (failure == 1) {
          curve.points[0].weight = 2;
        }
        else {
          // A conflicting endpoint association or an out-of-domain trim.
          curve.points[0].position[0] = failure == 2 ? 1 : -1;
        }
      }
    }
    const auto before = OwnedGeometry(geometry_copy(source.get()));
    std::string error;
    EXPECT_FALSE(translate_vertices(*source, {&id, 1}, {0, 0, 0.25}, 1e-9, &error));
    EXPECT_FALSE(error.empty());
    expect_source_geometry_equal(*source, *before);
  }
}

TEST(nurbs_vertex_transform, SharedTriangleSupportsRequireIdenticalOwnerDeformation)
{
  const double2 corners[] = {{0, 0}, {1, 0}, {0, 1}};
  auto source = trimmed_triangle({corners, 3});
  const auto &original = first_transform_item(source->faces);
  const auto index = TopologyIndex::build(*source);
  const auto &use = first_transform_item(first_transform_item(original.loops).edge_uses);
  const int64_t id = index->edge(use.edge_id)->vertex_ids[0];
  auto *other = MEM_new<NurbsTopologyFace>(__func__);
  other->id = source->next_id++;
  other->surface_id = original.surface_id;
  other->orientation = original.orientation;
  BLI_addtail(&source->faces, other);
  add_planar_loop(*source, *other, {corners, 3}, false);
  const auto before = OwnedGeometry(geometry_copy(source.get()));
  std::string error;
  EXPECT_FALSE(translate_vertices(*source, {&id, 1}, {0, 0, 0.25}, 1e-9, &error));
  EXPECT_NE(error.find("owners"), std::string::npos) << error;
  expect_source_geometry_equal(*source, *before);
}

}  // namespace blender::bke::nurbs::tests
