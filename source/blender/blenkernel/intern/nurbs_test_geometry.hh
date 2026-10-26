/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/* Native source fixtures shared by preview and display-mesh tests. */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.hh"

#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_tessellate.hh"

#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

template<typename T, typename Fn>
inline void expect_records_equal(const ListBaseT<T> &actual,
                                 const ListBaseT<T> &expected,
                                 Fn compare)
{
  ASSERT_EQ(actual.count(), expected.count());
  const T *saved = static_cast<const T *>(actual.first);
  for (const T &record : expected) {
    ASSERT_NE(saved, nullptr);
    SCOPED_TRACE(record.id);
    EXPECT_EQ(saved->id, record.id);
    compare(*saved, record);
    saved = saved->next;
  }
}

inline void expect_source_geometry_equal(const NurbsGeometry &actual,
                                         const NurbsGeometry &expected)
{
  EXPECT_EQ(actual.next_id, expected.next_id);
  expect_records_equal(actual.vertices, expected.vertices, [](const auto &a, const auto &b) {
    EXPECT_EQ(std::memcmp(a.position, b.position, sizeof(a.position)), 0);
    EXPECT_EQ(a.tolerance, b.tolerance);
  });
  expect_records_equal(actual.edges, expected.edges, [](const auto &a, const auto &b) {
    EXPECT_EQ(a.vertex_ids[0], b.vertex_ids[0]);
    EXPECT_EQ(a.vertex_ids[1], b.vertex_ids[1]);
    EXPECT_EQ(a.curve_id, b.curve_id);
    EXPECT_EQ(std::memcmp(a.parameter_range, b.parameter_range, sizeof(a.parameter_range)), 0);
    EXPECT_EQ(a.tolerance, b.tolerance);
  });
  expect_records_equal(actual.curves, expected.curves, [](const auto &a, const auto &b) {
    EXPECT_EQ(a.order, b.order);
    EXPECT_EQ(a.dimension, b.dimension);
    ASSERT_EQ(a.points_num, b.points_num);
    ASSERT_EQ(a.knots_num, b.knots_num);
    EXPECT_EQ(std::memcmp(a.points, b.points, sizeof(NurbsControlPoint) * b.points_num), 0);
    EXPECT_EQ(std::memcmp(a.knots, b.knots, sizeof(double) * b.knots_num), 0);
  });
  expect_records_equal(actual.surfaces, expected.surfaces, [](const auto &a, const auto &b) {
    EXPECT_EQ(a.order_u, b.order_u);
    EXPECT_EQ(a.order_v, b.order_v);
    EXPECT_EQ(a.points_u, b.points_u);
    EXPECT_EQ(a.points_v, b.points_v);
    ASSERT_EQ(a.points_num, b.points_num);
    ASSERT_EQ(a.knots_u_num, b.knots_u_num);
    ASSERT_EQ(a.knots_v_num, b.knots_v_num);
    EXPECT_EQ(std::memcmp(a.points, b.points, sizeof(NurbsControlPoint) * b.points_num), 0);
    EXPECT_EQ(std::memcmp(a.knots_u, b.knots_u, sizeof(double) * b.knots_u_num), 0);
    EXPECT_EQ(std::memcmp(a.knots_v, b.knots_v, sizeof(double) * b.knots_v_num), 0);
  });
  expect_records_equal(actual.faces, expected.faces, [](const auto &a, const auto &b) {
    EXPECT_EQ(a.surface_id, b.surface_id);
    EXPECT_EQ(a.tolerance, b.tolerance);
    EXPECT_EQ(a.orientation, b.orientation);
    EXPECT_EQ(a.material_index, b.material_index);
    expect_records_equal(a.loops, b.loops, [](const auto &a_loop, const auto &b_loop) {
      EXPECT_EQ(a_loop.is_inner, b_loop.is_inner);
      expect_records_equal(
          a_loop.edge_uses, b_loop.edge_uses, [](const auto &a_use, const auto &b_use) {
            EXPECT_EQ(a_use.edge_id, b_use.edge_id);
            EXPECT_EQ(a_use.pcurve_id, b_use.pcurve_id);
            EXPECT_EQ(a_use.tolerance, b_use.tolerance);
            EXPECT_EQ(a_use.reversed, b_use.reversed);
          });
    });
  });
}

inline NurbsTopologyVertex *add_vertex(NurbsGeometry &geometry, const double3 &position)
{
  auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
  vertex->id = geometry.next_id++;
  std::copy_n(&position.x, 3, vertex->position);
  BLI_addtail(&geometry.vertices, vertex);
  return vertex;
}

inline NurbsCurveGeometry *add_linear_curve(NurbsGeometry &geometry,
                                            const double3 &a,
                                            const double3 &b,
                                            const int dimension)
{
  auto *curve = MEM_new<NurbsCurveGeometry>(__func__);
  curve->id = geometry.next_id++;
  curve->dimension = dimension;
  curve->order = curve->points_num = 2;
  curve->knots_num = 4;
  curve->knots = MEM_new_array_zeroed<double>(4, __func__);
  curve->knots[2] = curve->knots[3] = 1;
  curve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
  std::copy_n(&a.x, 3, curve->points[0].position);
  std::copy_n(&b.x, 3, curve->points[1].position);
  BLI_addtail(&geometry.curves, curve);
  return curve;
}

inline NurbsTopologyEdge *add_edge(NurbsGeometry &geometry,
                                   const NurbsTopologyVertex &a,
                                   const NurbsTopologyVertex &b,
                                   const NurbsCurveGeometry &curve)
{
  auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
  edge->id = geometry.next_id++;
  edge->curve_id = curve.id;
  edge->vertex_ids[0] = a.id;
  edge->vertex_ids[1] = b.id;
  edge->parameter_range[0] = curve.knots[curve.order - 1];
  edge->parameter_range[1] = curve.knots[curve.points_num];
  BLI_addtail(&geometry.edges, edge);
  return edge;
}

inline OwnedGeometry make_planar_sheet()
{
  OwnedGeometry wire(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*wire, {-1, -1, 0});
  const auto *b = add_vertex(*wire, {1, -1, 0});
  const auto *curve = add_linear_curve(*wire, double3(a->position), double3(b->position), 3);
  const auto *edge = add_edge(*wire, *a, *b, *curve);
  auto result = extrude_edges(*wire, {edge->id}, {0, 2, 0}, 0);
  return result ? std::move(result->geometry) : nullptr;
}

/** The planar fixture's support maps UV to (2u-1, 2v-1, 0). */
inline NurbsTopologyLoop *add_planar_loop(NurbsGeometry &geometry,
                                          NurbsTopologyFace &face,
                                          const Span<double2> parameters,
                                          const bool is_inner)
{
  auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
  loop->id = geometry.next_id++;
  loop->is_inner = is_inner;
  BLI_addtail(&face.loops, loop);
  Vector<NurbsTopologyVertex *> vertices;
  for (const double2 parameter : parameters) {
    vertices.append(add_vertex(geometry, {2 * parameter.x - 1, 2 * parameter.y - 1, 0}));
  }
  for (const int i : parameters.index_range()) {
    const int next = (i + 1) % parameters.size();
    const auto *curve = add_linear_curve(
        geometry, double3(vertices[i]->position), double3(vertices[next]->position), 3);
    const auto *edge = add_edge(geometry, *vertices[i], *vertices[next], *curve);
    const auto *pcurve = add_linear_curve(geometry,
                                          {parameters[i].x, parameters[i].y, 0},
                                          {parameters[next].x, parameters[next].y, 0},
                                          2);
    auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
    use->id = geometry.next_id++;
    use->edge_id = edge->id;
    use->pcurve_id = pcurve->id;
    BLI_addtail(&loop->edge_uses, use);
  }
  return loop;
}

inline OwnedGeometry make_arc_sheet(const bool closed)
{
  OwnedGeometry wire(MEM_new<NurbsGeometry>(__func__));
  auto *curve = MEM_new<NurbsCurveGeometry>(__func__);
  curve->id = wire->next_id++;
  curve->order = 3;
  curve->points_num = closed ? 9 : 3;
  curve->knots_num = curve->points_num + curve->order;
  curve->knots = MEM_new_array_uninitialized<double>(curve->knots_num, __func__);
  curve->points = MEM_new_array<NurbsControlPoint>(curve->points_num, __func__);
  const double xy[9][2] = {
      {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}};
  const double circle_knots[12] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  for (int i = 0; i < curve->knots_num; i++) {
    curve->knots[i] = closed ? circle_knots[i] : (i < 3 ? 0 : 1);
  }
  for (int i = 0; i < curve->points_num; i++) {
    std::copy_n(xy[i], 2, curve->points[i].position);
    curve->points[i].weight = i % 2 ? std::sqrt(0.5) : 1;
  }
  BLI_addtail(&wire->curves, curve);
  const auto *a = add_vertex(*wire, {1, 0, 0});
  const auto *b = closed ? a : add_vertex(*wire, {0, 1, 0});
  const auto *edge = add_edge(*wire, *a, *b, *curve);
  auto result = extrude_edges(*wire, {edge->id}, {0, 0, 2}, 0);
  return result ? std::move(result->geometry) : nullptr;
}

inline SurfacePreviewOptions preview_options()
{
  SurfacePreviewOptions options;
  options.boundary.segments_per_span = 4;
  options.boundary.evaluation_allowance = 1.0e-12;
  options.interior_subdivisions = 4;
  return options;
}

inline double planar_area(const SurfacePreview &preview)
{
  double result = 0;
  for (const auto &triangle : preview.triangles) {
    const double3 a = preview.position(triangle.points[0]);
    const double3 b = preview.position(triangle.points[1]);
    const double3 c = preview.position(triangle.points[2]);
    result += math::cross(b - a, c - a).z * 0.5;
  }
  return result;
}

}  // namespace blender::bke::nurbs::tests
