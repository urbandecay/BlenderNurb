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

}  // namespace blender::bke::nurbs::tests
