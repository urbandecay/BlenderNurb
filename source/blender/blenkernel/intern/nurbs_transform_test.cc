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

#include "BKE_blender.hh"
#include "BKE_curve.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_refine.hh"
#include "BKE_nurbs_selection.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_nurbs_transform.hh"

#include "nurbs_test_geometry.hh"
#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

TEST(nurbs_transform, EveryCircleArcHasAnIndependentExactControlCage)
{
  for (const int count : {8, 10, 12, 13, 32}) {
    SCOPED_TRACE(count);
    OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
    auto support = circular_arc({0, 0, 0}, {2, 0, 0}, {0, 0, 1}, 2 * M_PI);
    ASSERT_TRUE(support);
    support->id = source->next_id++;
    auto *curve = support.release();
    BLI_addtail(&source->curves, curve);
    const double low = curve->knots[curve->order - 1], high = curve->knots[curve->points_num];
    Vector<NurbsTopologyVertex *> vertices;
    Vector<double> parameters;
    for (int i = 0; i <= count; i++) {
      const double angle = 2 * M_PI * i / count;
      const int quarter = std::min(i * 4 / count, 3);
      const double local = angle - quarter * (M_PI / 2);
      const double a = std::sin(local / 2), b = std::sin((M_PI / 2 - local) / 2);
      parameters.append(i == count ? high : std::lerp(low, high, (quarter + a / (a + b)) / 4));
    }
    for (int i = 0; i < count; i++) {
      const auto point = evaluate_curve(*curve, parameters[i]);
      ASSERT_TRUE(point);
      vertices.append(add_vertex(*source, point->position));
    }
    for (int i = 0; i < count; i++) {
      auto *edge = add_edge(*source, *vertices[i], *vertices[(i + 1) % count], *curve);
      edge->parameter_range[0] = parameters[i];
      edge->parameter_range[1] = parameters[i + 1];
      edge->tolerance = 1e-10;
    }
    std::string error;
    auto cages = curve_control_cages(*source, 1e-9, &error);
    ASSERT_TRUE(cages) << error;
    ASSERT_EQ(cages->size(), count);
    for (int i = 0; i < count; i++) {
      const auto &cage = (*cages)[i];
      EXPECT_EQ(cage.edges.size(), 1);
      EXPECT_FALSE(cage.uses_source_points);
      EXPECT_EQ(cage.curve->points_num, 3);
      EXPECT_TRUE(cage.reparameterized);
      const double theta = 2 * M_PI * (i + 0.5) / count;
      const double weight = std::cos(M_PI / count);
      EXPECT_NEAR(cage.curve->points[1].weight, weight, 1e-12);
      EXPECT_NEAR(cage.curve->points[1].position[0], 2 * std::cos(theta) / weight, 1e-9);
      EXPECT_NEAR(cage.curve->points[1].position[1], 2 * std::sin(theta) / weight, 1e-9);
      for (int sample = 0; sample <= 8; sample++) {
        const double t = std::lerp(cage.range.x, cage.range.y, sample / 8.0);
        const auto original = evaluate_curve(*curve, t);
        const auto restricted = evaluate_curve(*cage.curve, t);
        ASSERT_TRUE(original && restricted);
        EXPECT_NEAR(math::length(restricted->position), 2, 1e-9);
        EXPECT_NEAR(restricted->position.z, 0, 1e-9);
        if (sample == 0 || sample == 8) {
          EXPECT_LT(math::distance(original->position, restricted->position), 1e-9);
        }
      }
    }
    if (count == 10) {
      /* Keep edited geometry intact, even if it started life as a circle. */
      OwnedGeometry edited(geometry_copy(source.get()));
      auto &deformed = *static_cast<NurbsCurveGeometry *>(edited->curves.first);
      deformed.points[1].position[0] += 0.125;
      int i = 0;
      for (auto &vertex : edited->vertices) {
        const auto point = evaluate_curve(deformed, parameters[i++]);
        ASSERT_TRUE(point);
        std::copy_n(&point->position.x, 3, vertex.position);
      }
      auto edited_cages = curve_control_cages(*edited, 1e-9, &error);
      ASSERT_TRUE(edited_cages) << error;
      for (const auto &part : *edited_cages) {
        EXPECT_FALSE(part.reparameterized);
        const double t = (part.range.x + part.range.y) / 2;
        const auto old_point = evaluate_curve(deformed, t);
        const auto new_point = evaluate_curve(*part.curve, t);
        ASSERT_TRUE(old_point && new_point);
        EXPECT_LT(math::distance(old_point->position, new_point->position), 1e-9);
      }
      /* A surface's p-curves still use the original curve parameterization. */
      Vector<int64_t> edges;
      for (const auto &edge : source->edges) {
        edges.append(edge.id);
      }
      auto surface = extrude_edges(*source, edges, {0, 0, 2}, 1e-9, &error);
      ASSERT_TRUE(surface) << error;
      auto surface_cages = curve_control_cages(*surface->geometry, 1e-9, &error);
      ASSERT_TRUE(surface_cages) << error;
      for (const auto &part : *surface_cages) {
        EXPECT_FALSE(part.reparameterized);
      }
      auto compact = compact_circle_extrusion_controls(*surface->geometry, 1e-9, &error);
      ASSERT_TRUE(compact) << error;
      auto compact_cages = curve_control_cages(*compact, 1e-9, &error);
      ASSERT_TRUE(compact_cages) << error;
      for (const auto &part : *compact_cages) {
        EXPECT_EQ(part.curve->points_num, part.curve->order == 2 ? 2 : 3);
        if (part.curve->order == 3) {
          EXPECT_NEAR(part.curve->points[1].weight, std::cos(M_PI / count), 1e-12);
        }
      }
      BoundaryEvaluationOptions options;
      options.evaluation_allowance = 1e-9;
      EXPECT_TRUE(evaluate_boundaries(*compact, options, &error)) << error;
      for (const auto &face : compact->faces) {
        const NurbsSurfaceGeometry *patch = nullptr;
        for (const auto &support : compact->surfaces) {
          if (support.id == face.surface_id) {
            patch = &support;
          }
        }
        ASSERT_TRUE(patch);
        EXPECT_EQ(patch->points_u, 3);
        EXPECT_EQ(patch->points_v, 2);
        for (int u = 0; u <= 4; u++) {
          const double parameter = std::lerp(patch->knots_u[2], patch->knots_u[3], u / 4.0);
          const auto point = evaluate_surface(*patch, {parameter, 0.5});
          ASSERT_TRUE(point);
          EXPECT_NEAR(std::hypot(point->position.x, point->position.y), 2, 1e-9);
          EXPECT_NEAR(point->position.z, 1, 1e-9);
        }
      }
    }
    auto &cage = (*cages)[0];
    cage.curve->id = source->next_id++;
    const int64_t id = cage.curve->id;
    for (auto &edge : source->edges) {
      if (edge.id == cage.edges[0]) {
        edge.curve_id = id;
      }
    }
    BLI_addtail(&source->curves, cage.curve.release());
    auto matrix = double4x4::identity();
    matrix.location() = double3(0, 0, 1);
    const CurveControlPointTransform request{{id, 1}, matrix};
    auto moved = transform_curve_control_points(*source, {&request, 1}, 1e-9, &error);
    ASSERT_TRUE(moved) << error;
    for (const auto &after : moved->geometry->curves) {
      if (after.id == curve->id) {
        EXPECT_EQ(std::memcmp(
                      after.points, curve->points, sizeof(NurbsControlPoint) * curve->points_num),
                  0);
      }
    }
    EXPECT_TRUE(evaluate_boundaries(*moved->geometry, {}, &error)) << error;
  }
}

TEST(nurbs_transform, FourExtrudedCircleControlsAllowEvaluationRoundoffAndRejectRealGaps)
{
  OwnedGeometry wire(MEM_new<NurbsGeometry>(__func__));
  auto support = circular_arc({0, 0, 0}, {2, 0, 0}, {0, 0, 1}, 2 * M_PI);
  ASSERT_TRUE(support);
  support->id = wire->next_id++;
  auto *curve = support.release();
  BLI_addtail(&wire->curves, curve);
  Vector<double> parameters;
  Vector<NurbsTopologyVertex *> vertices;
  Vector<int64_t> edges;
  for (int i = 0; i < 10; i++) {
    const double angle = 2 * M_PI * i / 10;
    const int quarter = i * 4 / 10;
    const double local = angle - quarter * (M_PI / 2);
    const double a = std::sin(local / 2), b = std::sin((M_PI / 2 - local) / 2);
    parameters.append((quarter + a / (a + b)) / 4);
    const auto point = evaluate_curve(*curve, parameters.last());
    ASSERT_TRUE(point);
    vertices.append(add_vertex(*wire, point->position));
  }
  for (int i = 0; i < 10; i++) {
    auto *edge = add_edge(*wire, *vertices[i], *vertices[(i + 1) % 10], *curve);
    edge->parameter_range[0] = parameters[i];
    edge->parameter_range[1] = i == 9 ? 1 : parameters[i + 1];
    edges.append(edge->id);
  }
  std::string error;
  auto extrusion = extrude_edges(*wire, edges, {0, 0, 2}, 1e-9, &error);
  ASSERT_TRUE(extrusion) << error;
  auto matrix = double4x4::identity();
  matrix.location() = double3(0, 0, 1);
  Vector<CurveControlPointTransform> requests;
  for (const auto &line : extrusion->geometry->curves) {
    if (line.dimension != 3 || line.points_num != 2 ||
        line.points[0].position[2] == line.points[1].position[2])
    {
      continue;
    }
    const double3 a(line.points[0].position);
    for (const int station : {2, 7}) {
      const double3 b(vertices[station]->position);
      if (math::length(double2(a.x - b.x, a.y - b.y)) < 1e-12) {
        requests.append({{line.id, 0}, matrix});
        requests.append({{line.id, 1}, matrix});
      }
    }
  }
  ASSERT_EQ(requests.size(), 4);
  const OwnedGeometry before(geometry_copy(extrusion->geometry.get()));
  auto moved = transform_curve_control_points(*extrusion->geometry, requests, 1e-9, &error);
  ASSERT_TRUE(moved) << error;
  EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
  expect_source_geometry_equal(*extrusion->geometry, *before);

  /* A genuine support mismatch must still reject without changing the source. */
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(extrusion->geometry->surfaces.first);
  surface.points[0].position[0] += 0.01;
  const OwnedGeometry invalid(geometry_copy(extrusion->geometry.get()));
  EXPECT_FALSE(transform_curve_control_points(*extrusion->geometry, requests, 1e-9, &error));
  EXPECT_NE(error.find("exceed allowed agreement"), std::string::npos) << error;
  expect_source_geometry_equal(*extrusion->geometry, *invalid);
}

TEST(nurbs_transform, ControlPointDeformsRationalArcWithoutChangingBasisOrOtherCoefficients)
{
  OwnedGeometry geometry(MEM_new<NurbsGeometry>(__func__));
  auto support = circular_arc({0, 0, 0}, {2, 0, 0}, {0, 0, 1}, M_PI / 2);
  ASSERT_TRUE(support);
  support->id = geometry->next_id++;
  auto *curve = support.release();
  BLI_addtail(&geometry->curves, curve);
  auto *a = add_vertex(*geometry, double3(curve->points[0].position));
  auto *b = add_vertex(*geometry, double3(curve->points[curve->points_num - 1].position));
  add_edge(*geometry, *a, *b, *curve);
  auto matrix = double4x4::identity();
  matrix.location() = double3(0, 0, 1);
  const CurveControlPointTransform request{{curve->id, 1}, matrix};
  std::string error;
  auto result = transform_curve_control_points(*geometry, {&request, 1}, 1e-10, &error);
  ASSERT_TRUE(result) << error;
  const auto &after = *static_cast<NurbsCurveGeometry *>(result->geometry->curves.first);
  EXPECT_EQ(after.order, curve->order);
  EXPECT_EQ(after.points_num, curve->points_num);
  EXPECT_EQ(std::memcmp(after.knots, curve->knots, sizeof(double) * curve->knots_num), 0);
  for (int i = 0; i < curve->points_num; i++) {
    EXPECT_EQ(after.points[i].weight, curve->points[i].weight);
    EXPECT_EQ(double3(after.points[i].position),
              double3(curve->points[i].position) + (i == 1 ? double3(0, 0, 1) : double3(0)));
  }
  const auto sample = evaluate_curve(after, 0.5);
  ASSERT_TRUE(sample);
  EXPECT_NEAR(sample->position.z, std::sqrt(2.0) - 1, 1e-14);
  EXPECT_EQ(
      double3(static_cast<NurbsTopologyVertex *>(result->geometry->vertices.first)->position),
      double3(a->position));
  EXPECT_TRUE(evaluate_boundaries(*result->geometry, {}, &error)) << error;
}

TEST(nurbs_transform, ControlPointEndpointKeepsAdjacentWireConnected)
{
  OwnedGeometry geometry(MEM_new<NurbsGeometry>(__func__));
  auto support = circular_arc({0, 0, 0}, {2, 0, 0}, {0, 0, 1}, M_PI / 2);
  support->id = geometry->next_id++;
  auto *curve = support.release();
  BLI_addtail(&geometry->curves, curve);
  auto *a = add_vertex(*geometry, double3(curve->points[0].position));
  auto *b = add_vertex(*geometry, double3(curve->points[curve->points_num - 1].position));
  auto *c = add_vertex(*geometry, {4, 0, 0});
  add_edge(*geometry, *a, *b, *curve);
  const auto *line = add_linear_curve(*geometry, double3(a->position), double3(c->position), 3);
  add_edge(*geometry, *a, *c, *line);
  auto matrix = double4x4::identity();
  matrix.location() = double3(0, 0, 1);
  const CurveControlPointTransform request{{curve->id, 0}, matrix};
  std::string error;
  auto result = transform_curve_control_points(*geometry, {&request, 1}, 1e-10, &error);
  ASSERT_TRUE(result) << error;
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index);
  EXPECT_EQ(double3(index->vertex(a->id)->position), double3(2, 0, 1));
  EXPECT_EQ(double3(index->vertex(c->id)->position), double3(c->position));
  EXPECT_TRUE(evaluate_boundaries(*result->geometry, {}, &error)) << error;
}

TEST(nurbs_transform, InvalidControlPointTransformDoesNotModifySource)
{
  auto source = make_planar_sheet();
  const OwnedGeometry before(geometry_copy(source.get()));
  const CurveControlPointTransform request{{123456, 3}, double4x4::identity()};
  EXPECT_FALSE(transform_curve_control_points(*source, {&request, 1}, 1e-10));
  expect_source_geometry_equal(*source, *before);
}

TEST(nurbs_transform, BoundaryControlPointDeformsIncidentRationalSurface)
{
  auto source = make_arc_sheet(false);
  ASSERT_TRUE(source);
  const auto &curve = *static_cast<const NurbsCurveGeometry *>(source->curves.first);
  auto matrix = double4x4::identity();
  matrix.location() = double3(0.25, 0, 0.5);
  const CurveControlPointTransform request{{curve.id, 1}, matrix};
  const OwnedGeometry before(geometry_copy(source.get()));
  std::string error;
  auto result = transform_curve_control_points(*source, {&request, 1}, 1e-9, &error);
  ASSERT_TRUE(result) << error;
  const auto &old_surface = *static_cast<const NurbsSurfaceGeometry *>(source->surfaces.first);
  const auto &surface = *static_cast<const NurbsSurfaceGeometry *>(
      result->geometry->surfaces.first);
  EXPECT_EQ(surface.points_num, old_surface.points_num);
  int moved = 0;
  for (int i = 0; i < surface.points_num; i++) {
    EXPECT_EQ(surface.points[i].weight, old_surface.points[i].weight);
    moved += double3(surface.points[i].position) != double3(old_surface.points[i].position);
  }
  EXPECT_EQ(moved, 1);
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
  expect_source_geometry_equal(*source, *before);
}

TEST(nurbs_transform, BoundaryEndpointKeepsSurfaceAndNeighborEdgesConnected)
{
  auto source = make_arc_sheet(false);
  ASSERT_TRUE(source);
  const auto &curve = *static_cast<const NurbsCurveGeometry *>(source->curves.first);
  auto matrix = double4x4::identity();
  matrix.location() = double3(0.25, 0, 0.5);
  const CurveControlPointTransform request{{curve.id, 0}, matrix};
  std::string error;
  auto result = transform_curve_control_points(*source, {&request, 1}, 1e-9, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
}

class NurbsControlPointTest : public BlenderGTestBase {
 protected:
  void SetUp() override
  {
    BlenderGTestBase::SetUp();
    BKE_blender_globals_main_replace(BKE_main_new());
  }
  void TearDown() override
  {
    BKE_blender_globals_clear();
    BlenderGTestBase::TearDown();
  }
};

TEST_F(NurbsControlPointTest, SubdivisionBevelBoundaryHandlesEditAllIncidentPatches)
{
  auto sheet = make_planar_sheet();
  const int64_t face = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  auto box = extrude_components(*sheet, {{}, {}, {&face, 1}}, {0, 0, 2}, 0);
  ASSERT_TRUE(box);
  Vector<int64_t> edges;
  for (const auto &edge : box->geometry->edges) {
    edges.append(edge.id);
  }
  std::string error;
  for (const auto shape : {BevelSurfaceShape::Subdivision}) {
    SCOPED_TRACE(int(shape));
    auto bevel = bevel_edges(
        *box->geometry, edges, {.offset = 0.25, .segments = 2, .surface_shape = shape}, &error);
    ASSERT_TRUE(bevel) << error;
    auto matrix = double4x4::identity();
    matrix.location() = double3(0.03, 0.02, 0.04);
    int tested = 0;
    Set<int64_t> supports;
    for (const auto &edge : bevel->geometry->edges) {
      const NurbsCurveGeometry *curve = nullptr;
      for (const auto &support : bevel->geometry->curves) {
        if (support.id == edge.curve_id) {
          curve = &support;
        }
      }
      if (!curve || supports.contains(curve->id)) {
        continue;
      }
      supports.add(curve->id);
      for (int i = 0; i < curve->points_num; i++) {
        SCOPED_TRACE(::testing::Message() << "order " << curve->order << " point " << i);
        const CurveControlPointTransform request{{curve->id, i}, matrix};
        auto moved = transform_curve_control_points(*bevel->geometry, {&request, 1}, 1e-9, &error);
        ASSERT_TRUE(moved) << error;
        EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
        tested++;
      }
    }
    EXPECT_GE(tested, 3);
  }
}

TEST_F(NurbsControlPointTest, BeveledBoxVerticesMoveWithTheControlCageHidden)
{
  auto sheet = make_planar_sheet();
  const int64_t face = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  auto box = extrude_components(*sheet, {{}, {}, {&face, 1}}, {0, 0, 2}, 0);
  ASSERT_TRUE(box);
  Vector<int64_t> edges;
  for (const auto &edge : box->geometry->edges) {
    edges.append(edge.id);
  }
  std::string error;
  for (const auto shape : {BevelSurfaceShape::Circular, BevelSurfaceShape::Subdivision}) {
    SCOPED_TRACE(int(shape));
    auto bevel = bevel_edges(*box->geometry,
                             edges,
                             {.offset = 0.25, .segments = 1, .surface_shape = shape},
                             &error);
    ASSERT_TRUE(bevel) << error;
    const OwnedGeometry before(geometry_copy(bevel->geometry.get()));
    for (const auto &vertex : bevel->geometry->vertices) {
      SCOPED_TRACE(vertex.id);
      const double3 offset(0.03, 0.02, 0.04);
      auto moved = translate_vertices(*bevel->geometry, {vertex.id}, offset, 1e-9, &error);
      ASSERT_TRUE(moved) << error;
      const auto index = TopologyIndex::build(*moved->geometry);
      ASSERT_TRUE(index);
      EXPECT_LT(math::distance(double3(index->vertex(vertex.id)->position),
                               double3(vertex.position) + offset), 1e-9);
      for (const auto &other : bevel->geometry->vertices) {
        if (other.id != vertex.id) {
          EXPECT_LT(math::distance(double3(index->vertex(other.id)->position),
                                   double3(other.position)), 1e-9);
        }
      }
      EXPECT_TRUE(evaluate_boundaries(*moved->geometry, {}, &error)) << error;
      EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
    }
    Vector<int64_t> top;
    double max_z = -std::numeric_limits<double>::infinity();
    for (const auto &vertex : bevel->geometry->vertices) {
      max_z = std::max(max_z, vertex.position[2]);
    }
    for (const auto &vertex : bevel->geometry->vertices) {
      if (std::abs(vertex.position[2] - max_z) < 1e-9) {
        top.append(vertex.id);
      }
    }
    ASSERT_FALSE(top.is_empty());
    auto moved = translate_vertices(*bevel->geometry, top, {0, 0, 0.5}, 1e-9, &error);
    ASSERT_TRUE(moved) << error;
    EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
    expect_source_geometry_equal(*bevel->geometry, *before);
  }
}

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

TEST(nurbs_vertex_transform, PartialCurvedEndpointDeformsLikeItsControlPoint)
{
  const auto source = make_arc_sheet(false);
  ASSERT_NE(source, nullptr);
  const int64_t id = first_transform_item(source->vertices).id;
  const auto &surface = first_transform_item(source->surfaces);
  const double3 point(surface.points[0].position);
  std::string error;
  auto moved = translate_vertices(*source, {id}, {1, 0, 0}, 1e-9, &error);
  ASSERT_TRUE(moved) << error;
  auto matrix = double4x4::identity();
  matrix.location() = double3(1, 0, 0);
  const auto &curve = first_transform_item(source->curves);
  const CurveControlPointTransform request{{curve.id, 0}, matrix};
  const auto control = transform_curve_control_points(*source, {&request, 1}, 1e-9, &error);
  ASSERT_TRUE(control) << error;
  const auto moved_index = TopologyIndex::build(*moved->geometry);
  const auto control_index = TopologyIndex::build(*control->geometry);
  ASSERT_TRUE(moved_index && control_index);
  for (const auto &vertex : source->vertices) {
    EXPECT_EQ(double3(moved_index->vertex(vertex.id)->position),
              double3(control_index->vertex(vertex.id)->position));
  }
  const auto &moved_surface = first_transform_item(moved->geometry->surfaces);
  const auto &control_surface = first_transform_item(control->geometry->surfaces);
  ASSERT_EQ(moved_surface.points_num, control_surface.points_num);
  for (int i = 0; i < moved_surface.points_num; i++) {
    EXPECT_EQ(double3(moved_surface.points[i].position),
              double3(control_surface.points[i].position));
  }
  EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
  EXPECT_EQ(double3(surface.points[0].position), point);
  auto noop = translate_vertices(*source, {id}, {0, 0, 0}, 0, &error);
  ASSERT_TRUE(noop.has_value()) << error;
  expect_bases_and_ids_unchanged(*source, *noop->geometry);
  EXPECT_EQ(double3(first_transform_item(noop->geometry->surfaces).points[0].position), point);
}

TEST(nurbs_vertex_transform, MovingASharedArcStationKeepsItsOtherEndpointsFixed)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  auto arc = circular_arc({0, 0, 0}, {2, 0, 0}, {0, 0, 1}, M_PI / 2);
  ASSERT_TRUE(arc);
  arc->id = source->next_id++;
  auto *curve = arc.release();
  BLI_addtail(&source->curves, curve);
  const double low = curve->knots[curve->order - 1];
  const double high = curve->knots[curve->points_num];
  const double middle = (low + high) / 2;
  auto *a = add_vertex(*source, evaluate_curve(*curve, low)->position);
  auto *b = add_vertex(*source, evaluate_curve(*curve, middle)->position);
  auto *c = add_vertex(*source, evaluate_curve(*curve, high)->position);
  auto *ab = add_edge(*source, *a, *b, *curve);
  auto *bc = add_edge(*source, *b, *c, *curve);
  ab->parameter_range[1] = middle;
  bc->parameter_range[0] = middle;
  const OwnedGeometry before(geometry_copy(source.get()));
  std::string error;
  auto result = translate_vertices(*source, {b->id}, {0, 0, 1}, 1e-9, &error);
  ASSERT_TRUE(result) << error;
  auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index);
  EXPECT_LT(math::distance(double3(index->vertex(b->id)->position),
                           double3(b->position) + double3(0, 0, 1)), 1e-9);
  EXPECT_EQ(double3(index->vertex(a->id)->position), double3(a->position));
  EXPECT_EQ(double3(index->vertex(c->id)->position), double3(c->position));
  EXPECT_NE(index->edge(ab->id)->curve_id, index->edge(bc->id)->curve_id);
  EXPECT_TRUE(evaluate_boundaries(*result->geometry, {}, &error)) << error;
  expect_source_geometry_equal(*source, *before);
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
  EXPECT_TRUE(transform_vertices(*source, {{id, matrix}}, 1e-9, &error)) << error;
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

static OwnedGeometry trimmed_polygon(const Span<double2> corners)
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

TEST(nurbs_vertex_transform, RectangularTrimFieldsPreserveInteriorAndRationalBoundaryProfiles)
{
  const double2 ranges[][2] = {
      {{0, 0}, {1, 0.75}}, {{0.125, 0.25}, {0.875, 0.75}}, {{0.1, 0.2}, {0.9, 0.8}}};
  for (int test = 0; test < 3; test++) {
    SCOPED_TRACE(test);
    const double2 low = ranges[test][0], high = ranges[test][1];
    const double2 corners[] = {low, {high.x, low.y}, high, {low.x, high.y}};
    auto source = trimmed_polygon({corners, 4});
    auto &face = first_transform_item(source->faces);
    // The independently constructed nonbinary fixture has its own declared construction budget.
    face.tolerance = 1e-12;
    auto &surface = first_transform_item(source->surfaces);
    if (test == 1) {
      surface.points[3].position[2] = 0.5;
    }
    Map<int64_t, NurbsCurveGeometry *> curves;
    Map<int64_t, NurbsTopologyVertex *> vertices;
    for (auto &curve : source->curves) {
      curves.add_new(curve.id, &curve);
    }
    for (auto &vertex : source->vertices) {
      vertices.add_new(vertex.id, &vertex);
    }
    const auto index = TopologyIndex::build(*source);
    ASSERT_TRUE(index);
    Vector<VertexTransform> transforms;
    Vector<double3> displacements;
    for (const auto &use : first_transform_item(face.loops).edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      auto &curve = *curves.lookup(edge.curve_id);
      auto &pcurve = *curves.lookup(use.pcurve_id);
      for (int end = 0; end < 2; end++) {
        const double2 uv(pcurve.points[end].position);
        const double z = test == 1 ? 0.5 * uv.x * uv.y : 0;
        vertices.lookup(edge.vertex_ids[end])->position[2] = z;
        curve.points[end].position[2] = z;
        curve.points[end].weight = pcurve.points[end].weight = end + 2;
        if (test == 2) {
          pcurve.points[end].position[0] = 2 + 3 * uv.x;
          pcurve.points[end].position[1] = -3 + 4 * uv.y;
        }
      }
      const int i = transforms.size();
      auto matrix = double4x4::identity();
      matrix.location() = double3(0.125 * i, -0.0625 * i, 0.25 * (i + 1));
      displacements.append(matrix.location());
      transforms.append({edge.vertex_ids[0], matrix});
    }
    if (test == 2) {
      surface.knots_u[0] = surface.knots_u[1] = 2;
      surface.knots_u[2] = surface.knots_u[3] = 5;
      surface.knots_v[0] = surface.knots_v[1] = -3;
      surface.knots_v[2] = surface.knots_v[3] = 1;
    }
    const auto before = OwnedGeometry(geometry_copy(source.get()));
    std::string error;
    const auto result = transform_vertices(*source, transforms, 1e-9, &error);
    ASSERT_TRUE(result) << error;
    expect_source_geometry_equal(*source, *before);
    expect_bases_and_ids_unchanged(*source, *result->geometry);
    const auto &output = first_transform_item(result->geometry->surfaces);
    for (int y = 0; y <= 4; y++) {
      for (int x = 0; x <= 4; x++) {
        const double s = x / 4.0, t = y / 4.0;
        double2 uv = low + double2(s, t) * (high - low);
        if (test == 2) {
          uv = double2(2 + 3 * uv.x, -3 + 4 * uv.y);
        }
        const auto original = evaluate_surface(surface, uv, &error);
        const auto edited = evaluate_surface(output, uv, &error);
        ASSERT_TRUE(original && edited) << error;
        const double3 delta = (1 - s) * (1 - t) * displacements[0] +
                              s * (1 - t) * displacements[1] + s * t * displacements[2] +
                              (1 - s) * t * displacements[3];
        for (int axis = 0; axis < 3; axis++) {
          EXPECT_NEAR(edited->position[axis], original->position[axis] + delta[axis], 1e-12);
        }
      }
    }
    SurfacePreviewOptions options;
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
    EXPECT_LE(result->boundary_error_bound, 1e-9);
    if (result->position_error_bound > 0 || result->boundary_error_bound > 0) {
      EXPECT_FALSE(transform_vertices(*source, transforms, 0, &error));
      expect_source_geometry_equal(*source, *before);
    }
  }
}

TEST(nurbs_vertex_transform, TrimmedCornerCannotHealAnExistingOutOfToleranceBoundary)
{
  const double2 corners[] = {{0, 0}, {1, 0}, {1, 0.75}, {0, 0.75}};
  auto source = trimmed_polygon({corners, 4});
  const auto &face = first_transform_item(source->faces);
  const auto &use = first_transform_item(first_transform_item(face.loops).edge_uses);
  const auto index = TopologyIndex::build(*source);
  const int64_t vertex = index->edge(use.edge_id)->vertex_ids[0];
  for (auto &item : source->vertices) {
    if (item.id == vertex) {
      item.position[2] = 0.001;
    }
  }
  for (const int64_t id : index->edges_for_vertex(vertex)) {
    const auto &edge = *index->edge(id);
    for (auto &curve : source->curves) {
      if (curve.id == edge.curve_id) {
        curve.points[edge.vertex_ids[0] == vertex ? 0 : 1].position[2] = 0.001;
      }
    }
  }
  const auto before = OwnedGeometry(geometry_copy(source.get()));
  std::string error;
  EXPECT_FALSE(translate_vertices(*source, {&vertex, 1}, {0, 0, 0.25}, 1e-9, &error));
  EXPECT_NE(error.find("input boundary tolerance"), std::string::npos) << error;
  expect_source_geometry_equal(*source, *before);
}

TEST(nurbs_vertex_transform, TrimmedQuadsRechartSkewLayoutsAndRejectConflictingFields)
{
  const double2 rectangle[] = {{0.125, 0.25}, {0.875, 0.25}, {0.875, 0.75}, {0.125, 0.75}};
  const double2 skew[] = {{0.125, 0.25}, {0.875, 0.375}, {0.875, 0.75}, {0.125, 0.75}};
  for (const bool shared : {false, true}) {
    auto source = trimmed_polygon({shared ? rectangle : skew, 4});
    const auto &face = first_transform_item(source->faces);
    const auto &use = first_transform_item(first_transform_item(face.loops).edge_uses);
    const auto index = TopologyIndex::build(*source);
    const int64_t vertex = index->edge(use.edge_id)->vertex_ids[0];
    if (shared) {
      auto *other = MEM_new<NurbsTopologyFace>(__func__);
      other->id = source->next_id++;
      other->surface_id = face.surface_id;
      other->orientation = face.orientation;
      BLI_addtail(&source->faces, other);
      add_planar_loop(*source, *other, {rectangle, 4}, false);
    }
    const auto before = OwnedGeometry(geometry_copy(source.get()));
    std::string error;
    auto moved = translate_vertices(*source, {&vertex, 1}, {0, 0, 0.25}, 1e-9, &error);
    if (shared) {
      EXPECT_FALSE(moved);
      EXPECT_NE(error.find("owners"), std::string::npos) << error;
    }
    else {
      ASSERT_TRUE(moved) << error;
      EXPECT_EQ(moved->reparameterized_faces, 1);
      EXPECT_LE(moved->chart_error_bound, 1e-9);
      const auto after = TopologyIndex::build(*moved->geometry);
      ASSERT_TRUE(after);
      EXPECT_EQ(double3(after->vertex(vertex)->position),
                double3(index->vertex(vertex)->position) + double3(0, 0, 0.25));
      BoundaryEvaluationOptions options;
      options.evaluation_allowance = 0;
      EXPECT_TRUE(evaluate_boundaries(*moved->geometry, options, &error)) << error;
      EXPECT_TRUE(tessellate_surfaces(*moved->geometry, {}, &error)) << error;
    }
    expect_source_geometry_equal(*source, *before);
  }
}

TEST(nurbs_vertex_transform, QuadChartsPreserveOtherOwnersAndBothLoopWindings)
{
  const double2 corners[] = {{0.125, 0.25}, {0.875, 0.375}, {0.875, 0.75}, {0.125, 0.75}};
  for (const bool reversed : {false, true}) {
    SCOPED_TRACE(reversed);
    std::array<double2, 4> ordered;
    std::copy_n(corners, 4, ordered.begin());
    if (reversed) {
      std::reverse(ordered.begin(), ordered.end());
    }
    auto source = trimmed_polygon(ordered);
    auto &face = first_transform_item(source->faces);
    const int64_t face_id = face.id, original_surface = face.surface_id;
    auto *other = MEM_new<NurbsTopologyFace>(__func__);
    other->id = source->next_id++;
    other->surface_id = original_surface;
    other->orientation = face.orientation;
    BLI_addtail(&source->faces, other);
    add_planar_loop(*source, *other, ordered, false);
    const OwnedGeometry before(geometry_copy(source.get()));
    std::string error;
    auto result = reparameterize_planar_quads(*source, {&face_id, 1}, 1e-9, &error);
    ASSERT_TRUE(result) << error;
    const auto index = TopologyIndex::build(*result->geometry);
    ASSERT_TRUE(index);
    EXPECT_NE(index->face(face_id)->surface_id, original_surface);
    EXPECT_EQ(index->face(other->id)->surface_id, original_surface);
    EXPECT_EQ(index->face(face_id)->orientation, face.orientation);
    BoundaryEvaluationOptions options;
    options.evaluation_allowance = 0;
    EXPECT_TRUE(evaluate_boundaries(*result->geometry, options, &error)) << error;
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, {}, &error)) << error;
    expect_source_geometry_equal(*source, *before);
    /* A large source-support twist is not concealed by a new straight-sided chart. */
    first_transform_item(source->surfaces).points[3].position[2] = 0.5;
    face.tolerance = 1;
    EXPECT_FALSE(reparameterize_planar_quads(*source, {&face_id, 1}, 1e-9, &error));
    EXPECT_NE(error.find("whole-face error budget"), std::string::npos) << error;
  }
}

TEST(nurbs_vertex_transform, TriangleDisplacementsUseTrimParametersAndIndividualMatrices)
{
  const double2 cases[][3] = {{{0, 0}, {1, 0}, {0, 1}},
                              {{0.125, 0.25}, {0.875, 0.25}, {0.25, 0.75}},
                              {{0.1, 0.2}, {0.9, 0.3}, {0.2, 0.8}}};
  for (const auto &corners : cases) {
    const auto source = trimmed_polygon({corners, 3});
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
    auto source = trimmed_polygon({failure == 4 ? degenerate : corners, 3});
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
  auto source = trimmed_polygon({corners, 3});
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

TEST(nurbs_vertex_transform, BeveledCornerStripRemainsEditableThroughTrimmedNeighbors)
{
  auto sheet = make_planar_sheet();
  const auto sheet_index = TopologyIndex::build(*sheet);
  ASSERT_TRUE(sheet_index);
  int64_t shared_edge = 0;
  for (const auto &edge : sheet->edges) {
    if (sheet_index->vertex(edge.vertex_ids[0])->position[1] == 1 &&
        sheet_index->vertex(edge.vertex_ids[1])->position[1] == 1)
    {
      shared_edge = edge.id;
    }
  }
  ASSERT_NE(shared_edge, 0);
  std::string error;
  auto corner = extrude_edges(*sheet, {&shared_edge, 1}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(corner) << error;
  auto beveled = bevel_edges(*corner->geometry, {&shared_edge, 1}, {.offset = 0.25}, &error);
  ASSERT_TRUE(beveled) << error;
  ASSERT_EQ(beveled->faces.size(), 1);
  const auto index = TopologyIndex::build(*beveled->geometry);
  ASSERT_TRUE(index);
  Set<int64_t> selected;
  for (const auto &loop : index->face(beveled->faces[0])->loops) {
    for (const auto &use : loop.edge_uses) {
      for (const int64_t id : index->edge(use.edge_id)->vertex_ids) {
        selected.add(id);
      }
    }
  }
  const Vector<int64_t> vertices(selected.begin(), selected.end());
  const auto before = OwnedGeometry(geometry_copy(beveled->geometry.get()));
  auto moved = translate_vertices(*beveled->geometry, vertices, {0.125, 0, 0.25}, 1e-9, &error);
  ASSERT_TRUE(moved) << error;
  expect_source_geometry_equal(*beveled->geometry, *before);
  expect_bases_and_ids_unchanged(*beveled->geometry, *moved->geometry);
  SurfacePreviewOptions options;
  EXPECT_TRUE(tessellate_surfaces(*moved->geometry, options, &error)) << error;

  // Y and P retain the trimmed source support. Each detached original face must still bend
  // when a single topology corner moves, independently of the former neighboring faces.
  for (const auto &face : beveled->geometry->faces) {
    SCOPED_TRACE(face.id);
    const int64_t face_id = face.id;
    auto split = split_components(*beveled->geometry, {{}, {}, {&face_id, 1}}, &error);
    ASSERT_TRUE(split) << error;
    const auto detached_index = TopologyIndex::build(*split->geometry);
    ASSERT_TRUE(detached_index);
    const auto *detached_face = detached_index->face(split->source_to_copy.lookup(face_id));
    ASSERT_NE(detached_face, nullptr);
    const auto &loop = first_transform_item(detached_face->loops);
    for (const auto &use : loop.edge_uses) {
      const int64_t vertex = detached_index->edge(use.edge_id)->vertex_ids[0];
      SCOPED_TRACE(vertex);
      auto deformed = translate_vertices(
          *split->geometry, {&vertex, 1}, {0, 0, 0.375}, 1e-9, &error);
      ASSERT_TRUE(deformed) << error;
      expect_bases_and_ids_unchanged(*split->geometry, *deformed->geometry);
      const auto output_index = TopologyIndex::build(*deformed->geometry);
      ASSERT_TRUE(output_index);
      for (const auto &original : split->geometry->vertices) {
        const double3 expected = double3(original.position) +
                                 (original.id == vertex ? double3(0, 0, 0.375) : double3(0));
        EXPECT_EQ(double3(output_index->vertex(original.id)->position), expected);
      }
      auto *output_surface = static_cast<const NurbsSurfaceGeometry *>(
          deformed->geometry->surfaces.first);
      for (const auto &original : split->geometry->surfaces) {
        ASSERT_NE(output_surface, nullptr);
        if (original.id != detached_face->surface_id) {
          EXPECT_EQ(std::memcmp(original.points,
                                output_surface->points,
                                sizeof(NurbsControlPoint) * original.points_num),
                    0);
        }
        output_surface = output_surface->next;
      }
      EXPECT_TRUE(tessellate_surfaces(*deformed->geometry, options, &error)) << error;
    }
  }
}

TEST(nurbs_vertex_transform, WholeSelectionTranslationKeepsUnusedSupportsAndMatchesGeneralPath)
{
  auto source = weighted_patch();
  ASSERT_TRUE(source);
  auto *unused_curve = MEM_dupalloc(&first_transform_item(source->curves));
  unused_curve->id = source->next_id++;
  unused_curve->points = MEM_dupalloc(unused_curve->points);
  unused_curve->knots = MEM_dupalloc(unused_curve->knots);
  BLI_addtail(&source->curves, unused_curve);
  auto *unused_surface = MEM_dupalloc(&first_transform_item(source->surfaces));
  unused_surface->id = source->next_id++;
  unused_surface->points = MEM_dupalloc(unused_surface->points);
  unused_surface->knots_u = MEM_dupalloc(unused_surface->knots_u);
  unused_surface->knots_v = MEM_dupalloc(unused_surface->knots_v);
  BLI_addtail(&source->surfaces, unused_surface);
  const auto before = OwnedGeometry(geometry_copy(source.get()));
  Vector<int64_t> ids;
  for (const auto &vertex : source->vertices) {
    ids.append(vertex.id);
  }
  ids.append(ids.first());
  for (const double3 offset : {double3(0), double3(0.1, -0.2, 0.3), double3(4, 2, 1)}) {
    auto matrix = double4x4::identity();
    matrix.location() = offset;
    Vector<VertexTransform> requests;
    for (const int64_t id : ids) {
      requests.append({id, matrix});
    }
    for (const double budget : {0.0, 1e-9}) {
      auto expected = transform_vertices(*source, requests, budget);
      auto actual = translate_vertices(*source, ids, offset, budget);
      ASSERT_EQ(bool(actual), bool(expected));
      if (actual) {
        expect_source_geometry_equal(*actual->geometry, *expected->geometry);
        EXPECT_EQ(actual->position_error_bound, expected->position_error_bound);
        EXPECT_EQ(actual->boundary_error_bound, expected->boundary_error_bound);
      }
    }
  }
  ids[0] = source->next_id + 100;
  EXPECT_FALSE(translate_vertices(*source, ids, {1, 0, 0}, 0));
  expect_source_geometry_equal(*source, *before);

  /* Retain the general component-edit gate for an inconsistent input edge endpoint. */
  auto invalid = OwnedGeometry(geometry_copy(source.get()));
  first_transform_item(invalid->vertices).position[0] += 10;
  ids.clear();
  for (const auto &vertex : invalid->vertices) {
    ids.append(vertex.id);
  }
  std::string error;
  EXPECT_FALSE(translate_vertices(*invalid, ids, {1, 0, 0}, 1e-9, &error));
  EXPECT_NE(error.find("endpoint"), std::string::npos) << error;
}

TEST(nurbs_vertex_transform, WholeSelectionRotationMatchesGeneralPath)
{
  auto source = weighted_patch();
  ASSERT_TRUE(source);
  const auto *unused_curve = add_linear_curve(*source, {10, 0, 0}, {11, 0, 0}, 3);
  const int64_t unused_curve_id = unused_curve->id;
  auto matrix = double4x4::identity();
  matrix[0][0] = 0;
  matrix[1][1] = 0;
  matrix[0][1] = 1;
  matrix[1][0] = -1;
  const double3 pivot(0.25, -0.5, 0.75);
  matrix.location() = pivot - double3(matrix[0][0] * pivot.x + matrix[1][0] * pivot.y,
                                      matrix[0][1] * pivot.x + matrix[1][1] * pivot.y,
                                      pivot.z);
  Vector<VertexTransform> requests;
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    requests.append({vertex.id, matrix});
  }

  std::string error;
  const auto expected = transform_vertices(*source, requests, 0, &error);
  ASSERT_TRUE(expected) << error;
  const auto actual = transform_all_vertices(*source, matrix, 0, &error);
  ASSERT_TRUE(actual) << error;
  expect_source_geometry_equal(*actual->geometry, *expected->geometry);
  EXPECT_EQ(actual->position_error_bound, expected->position_error_bound);
  EXPECT_EQ(actual->boundary_error_bound, expected->boundary_error_bound);
  for (const NurbsCurveGeometry &curve : actual->geometry->curves) {
    if (curve.id == unused_curve_id) {
      EXPECT_EQ(double3(curve.points[0].position), double3(10, 0, 0));
      EXPECT_EQ(double3(curve.points[1].position), double3(11, 0, 0));
    }
  }
}

}  // namespace blender::bke::nurbs::tests
