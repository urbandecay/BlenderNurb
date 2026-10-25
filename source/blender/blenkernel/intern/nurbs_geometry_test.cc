/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#if defined(__SSE2__)
#  include <xmmintrin.h>
#endif

#include "MEM_guardedalloc.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#ifdef WITH_GMP
#  include "BLI_math_mpq.hh"
#endif
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_vector.hh"

#include "BKE_appdir.hh"
#include "BKE_blender.hh"
#include "BKE_curve.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_evaluate.hh"
#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_transform.hh"
#include "BKE_report.hh"

#include "BLO_readfile.hh"
#include "BLO_undofile.hh"
#include "BLO_writefile.hh"

namespace blender::bke::nurbs::tests {

using GeometryPtr = std::unique_ptr<NurbsGeometry, decltype(&geometry_free)>;

template<typename T> static T *first(const ListBaseT<T> &list)
{
  return static_cast<T *>(list.first);
}

template<typename T> static T *last(const ListBaseT<T> &list)
{
  return static_cast<T *>(list.last);
}

static NurbsCurveGeometry *add_curve(NurbsGeometry &geometry,
                                     const NurbsTopologyVertex &a,
                                     const NurbsTopologyVertex &b,
                                     const bool parameter_space)
{
  NurbsCurveGeometry *curve = MEM_new<NurbsCurveGeometry>(__func__);
  curve->id = geometry.next_id++;
  curve->order = 2;
  curve->points_num = 2;
  curve->knots_num = 4;
  curve->dimension = parameter_space ? 2 : 3;
  curve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
  curve->knots = MEM_new_array_zeroed<double>(4, __func__);
  curve->knots[2] = curve->knots[3] = 1.0;
  for (int axis = 0; axis < 3; axis++) {
    const double scale = parameter_space && axis == 0 ? 0.5 : 1.0;
    curve->points[0].position[axis] = a.position[axis] * scale;
    curve->points[1].position[axis] = b.position[axis] * scale;
  }
  BLI_addtail(&geometry.curves, curve);
  return curve;
}

TEST(nurbs_geometry, LinearParameterCoordinatesPreserveExactIdentityAcrossDomainsAndSpans)
{
  OwnedGeometry geometry(MEM_new<NurbsGeometry>(__func__));
  NurbsTopologyVertex a, b;
  a.position[0] = 2;
  b.position[0] = 5;
  a.position[1] = b.position[1] = -3;
  auto *curve = add_curve(*geometry, a, b, false);
  curve->dimension = 2;
  curve->knots[0] = curve->knots[1] = 2;
  curve->knots[2] = curve->knots[3] = 5;
  for (const double weight : {1.0, 1.0e200, -7.0}) {
    curve->points[0].weight = curve->points[1].weight = weight;
    for (const double parameter : {2.0, std::nextafter(2.0, 5.0), 2.1, 2.7, 3.1, 4.9, 5.0}) {
      const auto result = evaluate_curve(*curve, parameter);
      ASSERT_TRUE(result.has_value());
      EXPECT_EQ(result->position.x, parameter);
      EXPECT_EQ(result->position.y, -3);
    }
  }
  curve->points[0].weight = 1;
  curve->points[1].weight = 2;
  EXPECT_EQ(evaluate_curve(*curve, 3.5)->position.x, 4);

  MEM_delete(curve->points);
  MEM_delete(curve->knots);
  curve->points_num = 3;
  curve->knots_num = 5;
  curve->points = MEM_new_array<NurbsControlPoint>(3, __func__);
  curve->knots = MEM_new_array_uninitialized<double>(5, __func__);
  const double knots[] = {2, 2, 3.5, 5, 5};
  std::copy_n(knots, 5, curve->knots);
  const double x[] = {2, 3.5, 5};
  for (int i = 0; i < 3; i++) {
    curve->points[i].position[0] = x[i];
  }
  for (const double parameter : {2.1, 3.1, 3.5, 4.8}) {
    const auto result = evaluate_curve(*curve, parameter);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->position.x, parameter);
  }
  a.position[0] = 0;
  b.position[0] = 2;
  const auto *pcurve = add_curve(*geometry, a, b, true);
  for (const double parameter : {1.0 / 13, 5.0 / 13, 12.0 / 13}) {
    const auto result = evaluate_curve(*pcurve, parameter);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->position.x, parameter);
    EXPECT_EQ(result->position.y, -3);
  }
}

static NurbsTopologyVertex *add_vertex(NurbsGeometry &geometry, const double x, const double y)
{
  NurbsTopologyVertex *vertex = MEM_new<NurbsTopologyVertex>(__func__);
  vertex->id = geometry.next_id++;
  vertex->position[0] = x;
  vertex->position[1] = y;
  BLI_addtail(&geometry.vertices, vertex);
  return vertex;
}

static NurbsTopologyLoop *add_loop(NurbsGeometry &geometry,
                                   NurbsTopologyFace &face,
                                   const Span<NurbsTopologyVertex *> vertices,
                                   const bool is_inner)
{
  NurbsTopologyLoop *loop = MEM_new<NurbsTopologyLoop>(__func__);
  loop->id = geometry.next_id++;
  loop->is_inner = is_inner;
  BLI_addtail(&face.loops, loop);
  for (const int i : vertices.index_range()) {
    const NurbsTopologyVertex &a = *vertices[i];
    const NurbsTopologyVertex &b = *vertices[(i + 1) % vertices.size()];
    NurbsTopologyEdge *edge = nullptr;
    bool reversed = false;
    for (NurbsTopologyEdge &existing : geometry.edges) {
      if (existing.vertex_ids[0] == a.id && existing.vertex_ids[1] == b.id) {
        edge = &existing;
        break;
      }
      if (existing.vertex_ids[0] == b.id && existing.vertex_ids[1] == a.id) {
        edge = &existing;
        reversed = true;
        break;
      }
    }
    if (edge == nullptr) {
      edge = MEM_new<NurbsTopologyEdge>(__func__);
      edge->id = geometry.next_id++;
      edge->vertex_ids[0] = a.id;
      edge->vertex_ids[1] = b.id;
      edge->curve_id = add_curve(geometry, a, b, false)->id;
      edge->parameter_range[1] = 1.0;
      BLI_addtail(&geometry.edges, edge);
    }
    NurbsTopologyEdgeUse *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
    use->id = geometry.next_id++;
    use->edge_id = edge->id;
    use->pcurve_id = reversed ? add_curve(geometry, b, a, true)->id :
                                add_curve(geometry, a, b, true)->id;
    use->reversed = reversed;
    BLI_addtail(&loop->edge_uses, use);
  }
  return loop;
}

/** Two adjacent faces on one support, sharing one edge, with an inner loop in the first face. */
static GeometryPtr make_fixture(const bool with_hole = true)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsSurfaceGeometry *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
  surface->id = geometry->next_id++;
  surface->order_u = surface->order_v = 2;
  surface->points_u = surface->points_v = 2;
  surface->points_num = 4;
  surface->knots_u_num = surface->knots_v_num = 4;
  surface->points = MEM_new_array<NurbsControlPoint>(4, __func__);
  for (int i = 0; i < 4; i++) {
    surface->points[i].position[0] = (i % 2) * 2.0;
    surface->points[i].position[1] = i / 2;
  }
  surface->knots_u = MEM_new_array_zeroed<double>(4, __func__);
  surface->knots_u[2] = surface->knots_u[3] = 1.0;
  surface->knots_v = MEM_dupalloc(surface->knots_u);
  BLI_addtail(&geometry->surfaces, surface);

  NurbsTopologyVertex *vertices[6];
  for (int i = 0; i < 6; i++) {
    vertices[i] = add_vertex(*geometry, i % 3, i / 3);
  }
  for (int i = 0; i < 2; i++) {
    NurbsTopologyFace *face = MEM_new<NurbsTopologyFace>(__func__);
    face->id = geometry->next_id++;
    face->surface_id = surface->id;
    BLI_addtail(&geometry->faces, face);
    std::array<NurbsTopologyVertex *, 4> corners = {
        vertices[i], vertices[i + 1], vertices[i + 4], vertices[i + 3]};
    add_loop(*geometry, *face, corners, false);
  }
  if (with_hole) {
    std::array<NurbsTopologyVertex *, 4> hole = {add_vertex(*geometry, 0.2, 0.2),
                                                 add_vertex(*geometry, 0.2, 0.4),
                                                 add_vertex(*geometry, 0.4, 0.4),
                                                 add_vertex(*geometry, 0.4, 0.2)};
    add_loop(*geometry, *first(geometry->faces), hole, true);
  }
  return geometry;
}

static NurbsCurveGeometry *add_rational_arc_payload(NurbsGeometry &geometry)
{
  NurbsCurveGeometry *curve = MEM_new<NurbsCurveGeometry>(__func__);
  curve->id = geometry.next_id++;
  curve->order = 3;
  curve->points_num = 3;
  curve->knots_num = 6;
  curve->points = MEM_new_array<NurbsControlPoint>(3, __func__);
  curve->points[0].position[0] = 1.0;
  curve->points[1].position[0] = 1.0;
  curve->points[1].position[1] = 1.0;
  curve->points[1].weight = std::sqrt(0.5);
  curve->points[2].position[1] = 1.0;
  curve->knots = MEM_new_array_uninitialized<double>(6, __func__);
  for (int i = 0; i < 6; i++) {
    curve->knots[i] = i < 3 ? 2.0 : 5.0;
  }
  BLI_addtail(&geometry.curves, curve);
  return curve;
}

static void reset_curve(NurbsCurveGeometry &curve,
                        const int points_num,
                        const int order,
                        const double start,
                        const double end)
{
  MEM_SAFE_DELETE(curve.points);
  MEM_SAFE_DELETE(curve.knots);
  curve.points_num = points_num;
  curve.order = order;
  curve.knots_num = points_num + order;
  curve.points = MEM_new_array<NurbsControlPoint>(points_num, __func__);
  curve.knots = MEM_new_array_uninitialized<double>(curve.knots_num, __func__);
  for (int i = 0; i < curve.knots_num; i++) {
    curve.knots[i] = i < order ? start : end;
  }
}

static NurbsSurfaceGeometry *add_arc_strip(NurbsGeometry &geometry, const bool reversed)
{
  NurbsSurfaceGeometry *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
  surface->id = geometry.next_id++;
  surface->points_u = surface->order_u = 3;
  surface->points_v = surface->order_v = 2;
  surface->points_num = 6;
  surface->knots_u_num = 6;
  surface->knots_v_num = 4;
  surface->points = MEM_new_array<NurbsControlPoint>(6, __func__);
  surface->knots_u = MEM_new_array_uninitialized<double>(6, __func__);
  surface->knots_v = MEM_new_array_uninitialized<double>(4, __func__);
  for (int i = 0; i < 6; i++) {
    surface->knots_u[i] = i < 3 ? (reversed ? -7.0 : 2.0) : (reversed ? 11.0 : 5.0);
  }
  for (int i = 0; i < 4; i++) {
    surface->knots_v[i] = i < 2 ? (reversed ? -4.0 : 0.0) : (reversed ? 6.0 : 1.0);
  }
  for (int v = 0; v < 2; v++) {
    const double radius = reversed ? 1.0 + v : 1.0;
    for (int u = 0; u < 3; u++) {
      const int arc_index = reversed ? 2 - u : u;
      NurbsControlPoint &point = surface->points[v * 3 + u];
      point.position[0] = arc_index < 2 ? radius : 0.0;
      point.position[1] = arc_index > 0 ? radius : 0.0;
      point.position[2] = reversed ? 1.0 + v : v;
      point.weight = arc_index == 1 ? std::sqrt(0.5) : 1.0;
    }
  }
  BLI_addtail(&geometry.surfaces, surface);
  return surface;
}

/** Cylinder and cone faces with different parameterizations and one shared rational arc. */
static GeometryPtr make_curved_fixture()
{
  GeometryPtr geometry = make_fixture(false);
  Map<int64_t, double2> original_positions;
  Map<int64_t, NurbsTopologyVertex *> vertices;
  Map<int64_t, NurbsCurveGeometry *> curves;
  Map<int64_t, NurbsTopologyEdge *> edges;
  for (NurbsCurveGeometry &curve : geometry->curves) {
    curves.add_new(curve.id, &curve);
  }
  for (NurbsTopologyVertex &vertex : geometry->vertices) {
    original_positions.add_new(vertex.id, double2(vertex.position[0], vertex.position[1]));
    vertices.add_new(vertex.id, &vertex);
    const double radius = std::max(1.0, vertex.position[0]);
    const double z = vertex.position[0];
    vertex.position[0] = radius * (1.0 - vertex.position[1]);
    vertex.position[1] *= radius;
    vertex.position[2] = z;
  }
  for (NurbsTopologyEdge &edge : geometry->edges) {
    edges.add_new(edge.id, &edge);
    const double2 a = original_positions.lookup(edge.vertex_ids[0]);
    const double2 b = original_positions.lookup(edge.vertex_ids[1]);
    const bool arc = a.x == b.x;
    NurbsCurveGeometry &curve = *curves.lookup(edge.curve_id);
    reset_curve(curve, arc ? 3 : 2, arc ? 3 : 2, 2.0, 5.0);
    edge.parameter_range[0] = 2.0;
    edge.parameter_range[1] = 5.0;
    if (arc) {
      const double radius = std::max(1.0, a.x);
      for (int i = 0; i < 3; i++) {
        const int arc_index = a.y < b.y ? i : 2 - i;
        curve.points[i].position[0] = arc_index < 2 ? radius : 0.0;
        curve.points[i].position[1] = arc_index > 0 ? radius : 0.0;
        curve.points[i].position[2] = a.x;
        curve.points[i].weight = arc_index == 1 ? std::sqrt(0.5) : 1.0;
      }
    }
    else {
      for (int i = 0; i < 2; i++) {
        const NurbsTopologyVertex &vertex = *vertices.lookup(edge.vertex_ids[i]);
        std::copy_n(vertex.position, 3, curve.points[i].position);
      }
    }
  }
  for (NurbsSurfaceGeometry &surface : geometry->surfaces) {
    MEM_SAFE_DELETE(surface.points);
    MEM_SAFE_DELETE(surface.knots_u);
    MEM_SAFE_DELETE(surface.knots_v);
  }
  BLI_freelistN(&geometry->surfaces);
  for (NurbsTopologyFace &face : geometry->faces) {
    const bool reversed = &face != first(geometry->faces);
    face.surface_id = add_arc_strip(*geometry, reversed)->id;
    face.orientation = reversed ? 1 : -1;
    for (NurbsTopologyLoop &loop : face.loops) {
      for (NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const NurbsTopologyEdge &edge = *edges.lookup(use.edge_id);
        NurbsCurveGeometry &pcurve = *curves.lookup(use.pcurve_id);
        reset_curve(pcurve, 2, 2, 2.0, 5.0);
        for (int i = 0; i < 2; i++) {
          const double2 source = original_positions.lookup(edge.vertex_ids[i]);
          pcurve.points[i].position[0] = reversed ? 11.0 - 18.0 * source.y : 2.0 + 3.0 * source.y;
          pcurve.points[i].position[1] = reversed ? -4.0 + 10.0 * (source.x - 1.0) : source.x;
        }
      }
    }
  }
  return geometry;
}

static NurbsTopologyEdge *add_wire_edge(NurbsGeometry &geometry,
                                        const NurbsTopologyVertex &a,
                                        const NurbsTopologyVertex &b,
                                        NurbsCurveGeometry *curve = nullptr)
{
  if (curve == nullptr) {
    curve = add_curve(geometry, a, b, false);
  }
  auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
  edge->id = geometry.next_id++;
  edge->vertex_ids[0] = a.id;
  edge->vertex_ids[1] = b.id;
  edge->curve_id = curve->id;
  edge->parameter_range[0] = curve->knots[curve->order - 1];
  edge->parameter_range[1] = curve->knots[curve->points_num];
  BLI_addtail(&geometry.edges, edge);
  return edge;
}

static void check_constructed_boundaries(const NurbsGeometry &geometry,
                                         const Span<int64_t> face_ids)
{
  const auto index = TopologyIndex::build(geometry);
  ASSERT_TRUE(index.has_value());
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : geometry.curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : geometry.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (const int64_t face_id : face_ids) {
    const NurbsTopologyFace *face = index->face(face_id);
    ASSERT_NE(face, nullptr);
    const NurbsSurfaceGeometry &surface = *surfaces.lookup(face->surface_id);
    for (const NurbsTopologyLoop &loop : face->loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const NurbsTopologyEdge &edge = *index->edge(use.edge_id);
        const NurbsCurveGeometry &curve = *curves.lookup(edge.curve_id);
        const NurbsCurveGeometry &pcurve = *curves.lookup(use.pcurve_id);
        for (int i = 0; i <= 32; i++) {
          const double t = edge.parameter_range[0] +
                           (edge.parameter_range[1] - edge.parameter_range[0]) * i / 32.0;
          const auto uv = evaluate_curve(pcurve, t);
          const auto xyz = evaluate_curve(curve, t);
          ASSERT_TRUE(uv.has_value());
          ASSERT_TRUE(xyz.has_value());
          const auto point = evaluate_surface(surface, {uv->position.x, uv->position.y});
          ASSERT_TRUE(point.has_value());
          EXPECT_NEAR(math::distance(point->position, xyz->position), 0.0, 1.0e-13);
        }
      }
    }
  }
}

TEST(nurbs_geometry, RuledEdgeConstructionSharesOriginalEdgeAndOwnsCandidate)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*source, 0, 0);
  const auto *b = add_vertex(*source, 1, 0);
  const auto *edge = add_wire_edge(*source, *a, *b);
  const int64_t counter = source->next_id;
  std::string error;
  auto result = extrude_edges(*source, {edge->id}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->translation_error_bound, 0.0);
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(source->vertices.count(), 2);
  EXPECT_EQ(source->edges.count(), 1);
  EXPECT_TRUE(source->faces.is_empty());
  EXPECT_EQ(result->geometry->vertices.count(), 4);
  EXPECT_EQ(result->geometry->edges.count(), 4);
  EXPECT_EQ(result->geometry->faces.count(), 1);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->edge(edge->id)->vertex_ids[0], a->id);
  EXPECT_EQ(index->uses_for_edge(edge->id).size(), 1);
  const auto *top_edge = index->edge(result->source_to_cap.lookup(edge->id));
  ASSERT_NE(top_edge, nullptr);
  EXPECT_EQ(top_edge->vertex_ids[0], result->source_to_cap.lookup(a->id));
  EXPECT_EQ(top_edge->vertex_ids[1], result->source_to_cap.lookup(b->id));
  const int64_t face_id = result->edge_to_face.lookup(edge->id);
  check_constructed_boundaries(*result->geometry, {face_id});
  first(source->curves)->points[0].position[0] = 99;
  EXPECT_EQ(first(result->geometry->curves)->points[0].position[0], 0);
}

TEST(nurbs_geometry, RuledMultiEdgeConstructionSharesBranchRailsAndCanonicalizesInput)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *center = add_vertex(*source, 0, 0);
  const auto *a = add_vertex(*source, 1, 0);
  const auto *b = add_vertex(*source, 0, 1);
  const auto *c = add_vertex(*source, -1, 0);
  const auto *ea = add_wire_edge(*source, *center, *a);
  const auto *eb = add_wire_edge(*source, *center, *b);
  const auto *ec = add_wire_edge(*source, *center, *c);
  auto result = extrude_edges(*source, {ea->id, eb->id, ec->id}, {0, 0, 2}, 0);
  auto reordered = extrude_edges(*source, {ec->id, ea->id, ec->id, eb->id}, {0, 0, 2}, 0);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(reordered.has_value());
  EXPECT_EQ(result->geometry->vertices.count(), 8);
  EXPECT_EQ(result->geometry->edges.count(), 10);
  EXPECT_EQ(result->geometry->faces.count(), 3);
  EXPECT_EQ(result->geometry->next_id, reordered->geometry->next_id);
  for (const auto item : result->source_to_cap.items()) {
    EXPECT_EQ(item.value, reordered->source_to_cap.lookup(item.key));
  }
  for (const auto item : result->vertex_to_rail.items()) {
    EXPECT_EQ(item.value, reordered->vertex_to_rail.lookup(item.key));
  }
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->uses_for_edge(result->vertex_to_rail.lookup(center->id)).size(), 3);
  EXPECT_EQ(index->edges_for_vertex(center->id).size(), 4);
  Vector<int64_t> faces;
  for (const auto item : result->edge_to_face.items()) {
    EXPECT_EQ(item.value, reordered->edge_to_face.lookup(item.key));
    faces.append(item.value);
  }
  check_constructed_boundaries(*result->geometry, faces);
}

TEST(nurbs_geometry, RationalRuledConstructionPreservesBasisDomainAndCurvedBoundaryAgreement)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  NurbsCurveGeometry *arc = add_rational_arc_payload(*source);
  auto *a = add_vertex(*source, 1, 0);
  auto *b = add_vertex(*source, 0, 1);
  auto *edge = add_wire_edge(*source, *a, *b, arc);
  auto result = extrude_edges(*source, {edge->id}, {0, 0, 3}, 0);
  ASSERT_TRUE(result.has_value());
  const auto &surface = *first(result->geometry->surfaces);
  EXPECT_EQ(surface.order_u, arc->order);
  EXPECT_EQ(surface.order_v, 2);
  EXPECT_EQ(surface.points_u, arc->points_num);
  for (int i = 0; i < arc->knots_num; i++) {
    EXPECT_EQ(surface.knots_u[i], arc->knots[i]);
  }
  for (int i = 0; i < arc->points_num; i++) {
    EXPECT_EQ(surface.points[i].weight, arc->points[i].weight);
    EXPECT_EQ(surface.points[arc->points_num + i].weight, arc->points[i].weight);
  }
  for (int u = 0; u <= 32; u++) {
    const double t = 2.0 + 3.0 * u / 32.0;
    const auto curve_point = evaluate_curve(*arc, t);
    ASSERT_TRUE(curve_point.has_value());
    for (int v = 0; v <= 8; v++) {
      const auto point = evaluate_surface(surface, {t, v / 8.0});
      ASSERT_TRUE(point.has_value());
      EXPECT_NEAR(
          math::distance(point->position, curve_point->position + double3(0, 0, 3.0 * v / 8.0)),
          0,
          1.0e-14);
    }
  }
  check_constructed_boundaries(*result->geometry, {result->edge_to_face.lookup(edge->id)});
  edge->parameter_range[0] = 2.75;
  edge->parameter_range[1] = 4.25;
  const auto start = evaluate_curve(*arc, edge->parameter_range[0]);
  const auto end = evaluate_curve(*arc, edge->parameter_range[1]);
  ASSERT_TRUE(start.has_value());
  ASSERT_TRUE(end.has_value());
  std::copy_n(&start->position.x, 3, a->position);
  std::copy_n(&end->position.x, 3, b->position);
  auto trimmed = extrude_edges(*source, {edge->id}, {0, 0, 3}, 0);
  ASSERT_TRUE(trimmed.has_value());
  EXPECT_EQ(first(trimmed->geometry->surfaces)->knots_u[0], 2.0);
  EXPECT_EQ(first(trimmed->geometry->surfaces)->knots_u[5], 5.0);
  check_constructed_boundaries(*trimmed->geometry, {trimmed->edge_to_face.lookup(edge->id)});
}

TEST(nurbs_geometry, RuledConstructionSupportsRepeatedAndSingularEditingStates)
{
  GeometryPtr source = make_fixture(false);
  const int64_t edge_id = first(source->edges)->id;
  const int old_faces = source->faces.count();
  auto first_result = extrude_edges(*source, {edge_id}, {0, 0, 2}, 0);
  ASSERT_TRUE(first_result.has_value());
  const int64_t outgoing_edge = first_result->source_to_cap.lookup(edge_id);
  auto second_result = extrude_edges(*first_result->geometry, {outgoing_edge}, {0, 0, 2}, 0);
  ASSERT_TRUE(second_result.has_value());
  EXPECT_EQ(second_result->geometry->faces.count(), old_faces + 2);
  const auto index = TopologyIndex::build(*second_result->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->uses_for_edge(outgoing_edge).size(), 2);
  int oriented_uses = 0;
  for (const EdgeUseReference &use : index->uses_for_edge(outgoing_edge)) {
    oriented_uses += index->face(use.face_id)->orientation * (use.use->reversed ? -1 : 1);
  }
  EXPECT_EQ(oriented_uses, 0);
  check_constructed_boundaries(*second_result->geometry,
                               {second_result->edge_to_face.lookup(outgoing_edge)});
  auto singular = extrude_edges(*source, {edge_id}, {0, 0, 0}, 0);
  ASSERT_TRUE(singular.has_value());
  EXPECT_TRUE(geometry_validate_structure(*singular->geometry));
  check_constructed_boundaries(*singular->geometry, {singular->edge_to_face.lookup(edge_id)});
}

TEST(nurbs_geometry, RuledConstructionRejectsInvalidInputAndReportsAcceptedRoundoff)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  auto *a = add_vertex(*source, 1.0e20, 0);
  auto *b = add_vertex(*source, 1.0e20 + 32768, 0);
  auto *edge = add_wire_edge(*source, *a, *b);
  const int64_t counter = source->next_id;
  std::string error;
  EXPECT_FALSE(extrude_edges(*source, {}, {0, 0, 1}, 0, &error));
  EXPECT_FALSE(extrude_edges(*source, {999}, {0, 0, 1}, 0, &error));
  EXPECT_FALSE(extrude_edges(*source, {edge->id}, {0, 0, 1}, -1, &error));
  EXPECT_FALSE(extrude_edges(*source, {edge->id}, {0, 0, INFINITY}, 0, &error));
  EXPECT_FALSE(extrude_edges(*source, {edge->id}, {1, 0, 0}, 0, &error));
  EXPECT_FALSE(error.empty());
  auto accepted = extrude_edges(*source, {edge->id}, {1, 0, 0}, 2, &error);
  ASSERT_TRUE(accepted.has_value()) << error;
  EXPECT_GE(accepted->translation_error_bound, 1.0);
  EXPECT_LE(accepted->translation_error_bound, 2.0);
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(source->faces.count(), 0);
  first(source->curves)->points[0].weight = -1;
  EXPECT_FALSE(extrude_edges(*source, {edge->id}, {0, 0, 1}, 0, &error));
  first(source->curves)->points[0].weight = 1;
  a->position[1] = 1;
  EXPECT_FALSE(extrude_edges(*source, {edge->id}, {0, 0, 1}, 0, &error));
  a->tolerance = 1;
  auto tolerant_source = extrude_edges(*source, {edge->id}, {0, 0, 1}, 0, &error);
  ASSERT_TRUE(tolerant_source.has_value()) << error;
  EXPECT_EQ(tolerant_source->max_source_endpoint_deviation, 1.0);
  a->position[1] = 0;
  a->tolerance = 0;
  source->next_id = std::numeric_limits<int64_t>::max() - 5;
  EXPECT_FALSE(extrude_edges(*source, {edge->id}, {0, 0, 1}, 0, &error));
  EXPECT_EQ(source->next_id, std::numeric_limits<int64_t>::max() - 5);
  source->next_id = counter;
  const int rounding = std::fegetround();
  BLI_SCOPED_DEFER([&]() { std::fesetround(rounding); });
  ASSERT_EQ(std::fesetround(FE_DOWNWARD), 0);
  EXPECT_FALSE(extrude_edges(*source, {edge->id}, {0, 0, 1}, 0, &error));
  EXPECT_FALSE(error.empty());
}

TEST(nurbs_geometry, ClosedRationalRuledConstructionSharesOneSeamRailWithDistinctPCurves)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  auto *circle = add_rational_arc_payload(*source);
  reset_curve(*circle, 9, 3, 0, 4);
  const double positions[9][2] = {
      {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}};
  const double knots[12] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  std::copy_n(knots, 12, circle->knots);
  for (int i = 0; i < 9; i++) {
    circle->points[i].position[0] = positions[i][0];
    circle->points[i].position[1] = positions[i][1];
    circle->points[i].weight = i % 2 ? std::sqrt(0.5) : 1.0;
  }
  const auto *vertex = add_vertex(*source, 1, 0);
  const auto *edge = add_wire_edge(*source, *vertex, *vertex, circle);
  auto result = extrude_edges(*source, {edge->id}, {0, 0, 3}, 0);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->geometry->vertices.count(), 2);
  EXPECT_EQ(result->geometry->edges.count(), 3);
  EXPECT_EQ(result->vertex_to_rail.size(), 1);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  const auto uses = index->uses_for_edge(result->vertex_to_rail.lookup(vertex->id));
  ASSERT_EQ(uses.size(), 2);
  EXPECT_NE(uses[0].use->pcurve_id, uses[1].use->pcurve_id);
  EXPECT_NE(uses[0].use->reversed, uses[1].use->reversed);
  check_constructed_boundaries(*result->geometry, {result->edge_to_face.lookup(edge->id)});
  const auto preview = evaluate_boundaries(*result->geometry, {2, 1000, 1.0e-12});
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->positions.size(), 17);
  EXPECT_EQ(preview->uses.size(), 4);
  const EvaluatedBoundaryUse *previous_seam = nullptr;
  for (const EvaluatedBoundaryUse &use : preview->uses) {
    const auto &sampled_edge = preview->edges[use.edge_index];
    if (sampled_edge.edge_id == result->vertex_to_rail.lookup(vertex->id)) {
      if (previous_seam != nullptr) {
        EXPECT_EQ(use.edge_index, previous_seam->edge_index);
        EXPECT_NE(use.parameters.first().x, previous_seam->parameters.first().x);
      }
      previous_seam = &use;
    }
    else {
      EXPECT_EQ(sampled_edge.samples.first().point_index, sampled_edge.samples.last().point_index);
    }
  }
  ASSERT_NE(previous_seam, nullptr);
  const auto &surface = *first(result->geometry->surfaces);
  for (int i = 0; i <= 128; i++) {
    const auto point = evaluate_surface(surface, {4.0 * i / 128.0, 0.5});
    ASSERT_TRUE(point.has_value());
    EXPECT_NEAR(point->position.x * point->position.x + point->position.y * point->position.y,
                1.0,
                1.0e-14);
    EXPECT_NEAR(point->position.z, 1.5, 1.0e-14);
  }
}

TEST(nurbs_geometry, BoundaryPreviewSharesSamplesAcrossFacesAndPreservesHoleProvenance)
{
  const GeometryPtr source = make_fixture();
  const auto result = evaluate_boundaries(*source, {2, 79, 1.0e-14});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->edges.size(), 11);
  EXPECT_EQ(result->uses.size(), 12);
  EXPECT_EQ(result->positions.size(), 21);
  const auto index = TopologyIndex::build(*source);
  int shared_uses = 0, inner_uses = 0;
  for (const EvaluatedBoundaryUse &use : result->uses) {
    const auto &edge = result->edges[use.edge_index];
    ASSERT_EQ(use.parameters.size(), edge.samples.size());
    EXPECT_EQ(index->face(use.face_id)->id, use.face_id);
    inner_uses += use.is_inner;
    shared_uses += index->uses_for_edge(edge.edge_id).size() == 2;
    for (const BoundarySample &sample : edge.samples) {
      const int64_t vertex_id = result->vertex_ids[sample.point_index];
      if (vertex_id != 0) {
        EXPECT_EQ(result->positions[sample.point_index],
                  double3(index->vertex(vertex_id)->position));
      }
    }
  }
  EXPECT_EQ(inner_uses, 4);
  EXPECT_EQ(shared_uses, 2);
  EXPECT_FALSE(evaluate_boundaries(*source, {2, 78, 1.0e-14}).has_value());
  /* Neither successful nor failed preview evaluation allocates source identities. */
  EXPECT_EQ(source->vertices.count(), 10);
  EXPECT_EQ(source->edges.count(), 11);
}

TEST(nurbs_geometry, BoundaryPreviewUsesCanonicalArcSamplesWithDifferentFaceParameterizations)
{
  const GeometryPtr source = make_curved_fixture();
  std::string error;
  const auto result = evaluate_boundaries(*source, {8, 1000, 1.0e-12}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  const EvaluatedBoundaryUse *previous = nullptr;
  for (const EvaluatedBoundaryUse &use : result->uses) {
    const EvaluatedBoundaryEdge &edge = result->edges[use.edge_index];
    const auto index = TopologyIndex::build(*source);
    if (index->uses_for_edge(edge.edge_id).size() != 2) {
      continue;
    }
    EXPECT_LT(use.max_curve_support_deviation, 1.0e-12);
    if (previous == nullptr) {
      previous = &use;
      for (const BoundarySample &sample : edge.samples) {
        const double3 point = result->positions[sample.point_index];
        EXPECT_NEAR(point.x * point.x + point.y * point.y, 1.0, 1.0e-14);
        EXPECT_DOUBLE_EQ(point.z, 1.0);
      }
    }
    else {
      EXPECT_EQ(previous->edge_index, use.edge_index);
      EXPECT_NE(previous->reversed, use.reversed);
      EXPECT_NE(previous->parameters.first(), use.parameters.first());
    }
  }
  ASSERT_NE(previous, nullptr);
}

TEST(nurbs_geometry, BoundaryPreviewRetainsCoincidentIdentityAndCollapsedNonManifoldStates)
{
  const GeometryPtr source = make_fixture(false);
  auto *face = MEM_new<NurbsTopologyFace>(__func__);
  face->id = source->next_id++;
  face->surface_id = first(source->surfaces)->id;
  BLI_addtail(&source->faces, face);
  std::array<NurbsTopologyVertex *, 4> vertices = {first(source->vertices),
                                                   first(source->vertices)->next,
                                                   first(source->vertices)->next->next->next->next,
                                                   first(source->vertices)->next->next->next};
  add_loop(*source, *face, vertices, false);
  auto *isolated = add_vertex(*source, 0, 0);
  auto *coincident = add_vertex(*source, 0, 0);
  const auto *wire = add_wire_edge(*source, *isolated, *coincident);
  const auto preview = evaluate_boundaries(*source, {1, 100, 0});
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->positions.size(), 8);
  EXPECT_EQ(preview->uses.size(), 12);
  const auto &sampled_wire = preview->edges.last();
  EXPECT_EQ(sampled_wire.edge_id, wire->id);
  EXPECT_NE(sampled_wire.samples.first().point_index, sampled_wire.samples.last().point_index);
  EXPECT_EQ(preview->positions[sampled_wire.samples.first().point_index],
            preview->positions[sampled_wire.samples.last().point_index]);
  /* Zero-offset extrusion has repeated positions but real, distinct source identities. */
  const auto swept = extrude_edges(*source, {first(source->edges)->id}, {0, 0, 0}, 0);
  ASSERT_TRUE(swept.has_value());
  EXPECT_TRUE(evaluate_boundaries(*swept->geometry, {2, 1000, 1.0e-14}).has_value());
}

TEST(nurbs_geometry, BoundaryPreviewReportsDisagreementWithoutChangingSourceOrTolerance)
{
  const GeometryPtr source = make_fixture(false);
  first(source->vertices)->position[2] = 0.125;
  std::string error;
  EXPECT_FALSE(evaluate_boundaries(*source, {2, 1000, 0}, &error).has_value());
  EXPECT_NE(error.find("endpoint"), std::string::npos);
  EXPECT_NE(error.find("0.125"), std::string::npos);
  first(source->vertices)->tolerance = 0.125;
  const auto result = evaluate_boundaries(*source, {2, 1000, 0}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_DOUBLE_EQ(result->edges.first().max_endpoint_deviation, 0.125);
  EXPECT_DOUBLE_EQ(result->uses.first().max_display_support_deviation, 0.125);
  EXPECT_DOUBLE_EQ(first(source->vertices)->position[2], 0.125);
  EXPECT_DOUBLE_EQ(first(source->vertices)->tolerance, 0.125);

  /* An explicit evaluation allowance accepts sampled disagreement without expanding source
   * tolerances or pretending to establish an evaluation error bound. */
  first(source->vertices)->position[2] = 0;
  auto *use = first(first(first(source->faces)->loops)->edge_uses);
  for (NurbsCurveGeometry &curve : source->curves) {
    if (curve.id == use->pcurve_id) {
      curve.points[0].position[1] = curve.points[1].position[1] = 0.0625;
    }
  }
  EXPECT_FALSE(evaluate_boundaries(*source, {2, 1000, 0}, &error).has_value());
  EXPECT_NE(error.find("edge/support"), std::string::npos);
  EXPECT_NE(error.find("0.0625"), std::string::npos);
  const auto allowed = evaluate_boundaries(*source, {2, 1000, 0.0625}, &error);
  ASSERT_TRUE(allowed.has_value()) << error;
  EXPECT_DOUBLE_EQ(allowed->uses.first().max_curve_support_deviation, 0.0625);
  EXPECT_DOUBLE_EQ(use->tolerance, 0);
}

TEST(nurbs_geometry, BoundaryPreviewUnitesPCurveKnotSpansWithoutCoordinateMerging)
{
  const GeometryPtr source = make_fixture(false);
  const auto *edge = first(source->edges);
  const auto *use = first(first(first(source->faces)->loops)->edge_uses);
  for (NurbsCurveGeometry &curve : source->curves) {
    if (curve.id != edge->curve_id && curve.id != use->pcurve_id) {
      continue;
    }
    const double3 a(curve.points[0].position), b(curve.points[1].position);
    const double knot = curve.id == edge->curve_id ? 0.75 : 0.25;
    reset_curve(curve, 3, 2, 0, 1);
    curve.knots[2] = knot;
    const double3 middle = a + (b - a) * knot;
    std::copy_n(&a.x, 3, curve.points[0].position);
    std::copy_n(&middle.x, 3, curve.points[1].position);
    std::copy_n(&b.x, 3, curve.points[2].position);
  }
  const auto result = evaluate_boundaries(*source, {1, 1000, 0});
  ASSERT_TRUE(result.has_value());
  const auto &samples = result->edges.first().samples;
  ASSERT_EQ(samples.size(), 4);
  EXPECT_DOUBLE_EQ(samples[0].parameter, 0);
  EXPECT_DOUBLE_EQ(samples[1].parameter, 0.25);
  EXPECT_DOUBLE_EQ(samples[2].parameter, 0.75);
  EXPECT_DOUBLE_EQ(samples[3].parameter, 1);
  EXPECT_EQ(result->uses.first().parameters.size(), 4);
}

TEST(nurbs_geometry, BoundaryPreviewRejectsUnsupportedSourceAndUnrepresentableSubdivisions)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  auto *a = add_vertex(*source, 0, 0);
  auto *b = add_vertex(*source, 1, 0);
  const auto *edge = add_wire_edge(*source, *a, *b);
  auto *curve = first(source->curves);
  std::string error;
  EXPECT_FALSE(evaluate_boundaries(*source, {0, 100, 0}, &error).has_value());
  EXPECT_FALSE(evaluate_boundaries(*source, {2, 100, -1}, &error).has_value());
  EXPECT_FALSE(
      evaluate_boundaries(*source, {2, 100, std::numeric_limits<double>::infinity()}, &error)
          .has_value());
  curve->points[0].weight = -1;
  EXPECT_FALSE(evaluate_boundaries(*source, {2, 100, 0}, &error).has_value());
  EXPECT_NE(error.find("positive weights"), std::string::npos);
  curve->points[0].weight = 1;
  reset_curve(*curve, 4, 2, 0, 1);
  const double discontinuous_knots[6] = {0, 0, 0.5, 0.5, 1, 1};
  std::copy_n(discontinuous_knots, 6, curve->knots);
  EXPECT_FALSE(evaluate_boundaries(*source, {2, 100, 0}, &error).has_value());
  EXPECT_NE(error.find("continuity"), std::string::npos);
  /* A full-multiplicity interior knot at a restricted edge's end needs left-limit evaluation,
   * which this preview builder does not yet provide. Do not bridge to the right limit. */
  for (int i = 0; i < curve->knots_num; i++) {
    curve->knots[i] *= 2;
  }
  EXPECT_FALSE(evaluate_boundaries(*source, {2, 100, 0}, &error).has_value());
  EXPECT_NE(error.find("continuity"), std::string::npos);
  /* Endpoint parameters are valid source doubles, but no middle double exists. */
  reset_curve(*curve, 2, 2, 1, std::nextafter(1.0, 2.0));
  curve->points[1].position[0] = 1;
  auto *mutable_edge = first(source->edges);
  mutable_edge->parameter_range[0] = curve->knots[1];
  mutable_edge->parameter_range[1] = curve->knots[2];
  EXPECT_EQ(mutable_edge->id, edge->id);
  EXPECT_TRUE(evaluate_boundaries(*source, {1, 100, 0}, &error).has_value()) << error;
  EXPECT_FALSE(evaluate_boundaries(*source, {2, 100, 0}, &error).has_value());
  EXPECT_NE(error.find("representable"), std::string::npos);
}

TEST(nurbs_geometry, BoundaryPreviewUsesRetainedLiveSourceAfterOwnerDestruction)
{
  const GeometryPtr source = make_fixture(false);
  auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot before = edit->read_snapshot();
  auto candidate = before.copy_for_edit();
  double4x4 matrix = double4x4::identity();
  matrix.location() = double3(4, 0, 0);
  auto moved = transform_geometry(*candidate.geometry, matrix, 0);
  ASSERT_TRUE(moved.has_value());
  candidate.geometry = std::move(moved->geometry);
  ASSERT_TRUE(edit->commit(std::move(candidate), before));
  const ReadSnapshot after = edit->read_snapshot();
  edit.reset();
  const auto old_preview = evaluate_boundaries(before.geometry(), {1, 100, 0});
  const auto new_preview = evaluate_boundaries(after.geometry(), {1, 100, 0});
  ASSERT_TRUE(old_preview.has_value());
  ASSERT_TRUE(new_preview.has_value());
  EXPECT_EQ(old_preview->vertex_ids, new_preview->vertex_ids);
  EXPECT_EQ(old_preview->positions.first(), double3(0, 0, 0));
  EXPECT_EQ(new_preview->positions.first(), double3(4, 0, 0));
  EXPECT_EQ(first(source->vertices)->position[0], 0);
}

TEST(nurbs_geometry, CompleteAffineTransformPreservesIDsBasisSelectionAndParameterCurves)
{
  GeometryPtr source = make_fixture();
  SelectionState selection;
  selection.faces.add(first(source->faces)->id);
  selection.active = ElementRef{ElementDomain::Face, first(source->faces)->id};
  selection.history.append(*selection.active);
  source->stored_selection = selection_to_storage(selection);
  double4x4 matrix = double4x4::identity();
  matrix[0][0] = -2;
  matrix[1][1] = 4;
  matrix[2][2] = 0.5;
  matrix.location() = {8, -4, 2};
  auto result = transform_geometry(*source, matrix, 1.0e-12);
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->position_error_bound, 0.0);
  EXPECT_LE(result->position_error_bound, 1.0e-12);
  EXPECT_EQ(result->tolerance_scale_bound, 4.0);
  EXPECT_EQ(result->geometry->next_id, source->next_id);
  const auto source_index = TopologyIndex::build(*source);
  const auto result_index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(source_index.has_value());
  ASSERT_TRUE(result_index.has_value());
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    const auto *copy = result_index->vertex(vertex.id);
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(double3(copy->position),
              double3(-2 * vertex.position[0] + 8,
                      4 * vertex.position[1] - 4,
                      0.5 * vertex.position[2] + 2));
  }
  auto *copy = first(result->geometry->curves);
  for (const NurbsCurveGeometry &curve : source->curves) {
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->id, curve.id);
    EXPECT_EQ(copy->dimension, curve.dimension);
    EXPECT_EQ(copy->order, curve.order);
    for (int i = 0; i < curve.knots_num; i++) {
      EXPECT_EQ(copy->knots[i], curve.knots[i]);
    }
    for (int i = 0; i < curve.points_num; i++) {
      EXPECT_EQ(copy->points[i].weight, curve.points[i].weight);
      if (curve.dimension == 2) {
        EXPECT_EQ(double3(copy->points[i].position), double3(curve.points[i].position));
      }
    }
    copy = copy->next;
  }
  for (const NurbsTopologyFace &face : source->faces) {
    const auto *transformed = result_index->face(face.id);
    ASSERT_NE(transformed, nullptr);
    EXPECT_EQ(transformed->orientation, face.orientation);
    EXPECT_EQ(transformed->surface_id, face.surface_id);
  }
  const auto stored = selection_from_storage(result->geometry->stored_selection);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->active, selection.active);
  EXPECT_EQ(stored->history.size(), selection.history.size());
  EXPECT_EQ(first(source->vertices)->position[0], 0.0);
}

TEST(nurbs_geometry, RationalSourceTransformMatchesAffineEvaluationAndPreservesSharedBoundaries)
{
  GeometryPtr source = make_curved_fixture();
  double4x4 matrix = double4x4::identity();
  /* Exact quarter turn, non-uniform scaling, reflection and a translation. */
  matrix[0][0] = matrix[1][1] = 0;
  matrix[0][1] = 2;
  matrix[1][0] = 4;
  matrix.location() = {8, -4, 2};
  auto result = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(result.has_value());
  auto *transformed = first(result->geometry->surfaces);
  Vector<int64_t> faces;
  for (const NurbsTopologyFace &face : source->faces) {
    faces.append(face.id);
  }
  for (const NurbsSurfaceGeometry &surface : source->surfaces) {
    ASSERT_NE(transformed, nullptr);
    EXPECT_EQ(transformed->id, surface.id);
    for (int i = 0; i < surface.points_num; i++) {
      EXPECT_EQ(transformed->points[i].weight, surface.points[i].weight);
    }
    const double u0 = surface.knots_u[surface.order_u - 1];
    const double u1 = surface.knots_u[surface.points_u];
    const double v0 = surface.knots_v[surface.order_v - 1];
    const double v1 = surface.knots_v[surface.points_v];
    for (int u = 0; u <= 16; u++) {
      for (int v = 0; v <= 8; v++) {
        const double2 uv(u0 + (u1 - u0) * u / 16.0, v0 + (v1 - v0) * v / 8.0);
        const auto before = evaluate_surface(surface, uv);
        const auto after = evaluate_surface(*transformed, uv);
        ASSERT_TRUE(before.has_value());
        ASSERT_TRUE(after.has_value());
        const double3 expected(
            4 * before->position.y + 8, 2 * before->position.x - 4, before->position.z + 2);
        EXPECT_NEAR(math::distance(after->position, expected), 0, 1.0e-13);
      }
    }
    transformed = transformed->next;
  }
  check_constructed_boundaries(*result->geometry, faces);
}

TEST(nurbs_geometry, SourceTransformExpandsModelTolerancesAndAllowsSingularStates)
{
  GeometryPtr source = make_fixture(false);
  for (NurbsTopologyVertex &vertex : source->vertices) {
    vertex.tolerance = 0.125;
  }
  for (NurbsTopologyEdge &edge : source->edges) {
    edge.tolerance = 0.25;
  }
  for (NurbsTopologyFace &face : source->faces) {
    face.tolerance = 0.5;
    for (NurbsTopologyLoop &loop : face.loops) {
      for (NurbsTopologyEdgeUse &use : loop.edge_uses) {
        use.tolerance = 0.0625;
      }
    }
  }
  double4x4 matrix = double4x4::identity();
  matrix[0][0] = 2;
  auto scaled = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(scaled.has_value());
  EXPECT_EQ(scaled->tolerance_scale_bound, 2.0);
  EXPECT_EQ(first(scaled->geometry->vertices)->tolerance, 0.25);
  EXPECT_EQ(first(scaled->geometry->edges)->tolerance, 0.5);
  EXPECT_EQ(first(scaled->geometry->faces)->tolerance, 1.0);
  EXPECT_EQ(first(first(first(scaled->geometry->faces)->loops)->edge_uses)->tolerance, 0.125);
  matrix[0][0] = matrix[1][1] = matrix[2][2] = 0;
  matrix.location() = {2, 4, 8};
  auto collapsed = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(collapsed.has_value());
  EXPECT_EQ(collapsed->tolerance_scale_bound, 0.0);
  EXPECT_EQ(first(collapsed->geometry->vertices)->tolerance, 0.0);
  EXPECT_EQ(collapsed->geometry->vertices.count(), source->vertices.count());
  EXPECT_EQ(collapsed->geometry->edges.count(), source->edges.count());
  EXPECT_TRUE(geometry_validate_structure(*collapsed->geometry));
}

TEST(nurbs_geometry, RepeatedRotationsDoNotArtificiallyInflateModelTolerances)
{
  OwnedGeometry source(make_fixture(false).release());
  first(source->vertices)->tolerance = 1.0e-6;
  double4x4 matrix = double4x4::identity();
  const double s = std::sqrt(0.5);
  matrix[0][0] = matrix[1][1] = s;
  matrix[0][1] = s;
  matrix[1][0] = -s;
  for (int i = 0; i < 20; i++) {
    auto result = transform_geometry(*source, matrix, 1.0e-12);
    ASSERT_TRUE(result.has_value());
    EXPECT_GE(result->tolerance_scale_bound, 1.0);
    EXPECT_LE(result->tolerance_scale_bound, 1.0 + 1.0e-14);
    source = std::move(result->geometry);
  }
  EXPECT_GE(first(source->vertices)->tolerance, 1.0e-6);
  EXPECT_LE(first(source->vertices)->tolerance, 1.00001e-6);
}

TEST(nurbs_geometry, SourceTransformRejectsUnsupportedArithmeticAndBudgetsAtomically)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*source, 1.0e20, 0);
  const auto *b = add_vertex(*source, 1.0e20 + 32768, 0);
  add_wire_edge(*source, *a, *b);
  double4x4 matrix = double4x4::identity();
  matrix.location().x = 1.0;
  const int64_t counter = source->next_id;
  std::string error;
  EXPECT_FALSE(transform_geometry(*source, matrix, 0, &error));
  EXPECT_FALSE(error.empty());
  auto accepted = transform_geometry(*source, matrix, 2, &error);
  ASSERT_TRUE(accepted.has_value()) << error;
  EXPECT_GE(accepted->position_error_bound, 1.0);
  EXPECT_LE(accepted->position_error_bound, 2.0);
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(first(source->vertices)->position[0], 1.0e20);
  matrix[0][3] = 0.5;
  EXPECT_FALSE(transform_geometry(*source, matrix, 2, &error));
  matrix = double4x4::identity();
  matrix[0][0] = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(transform_geometry(*source, matrix, 2, &error));
  matrix = double4x4::identity();
  EXPECT_FALSE(transform_geometry(*source, matrix, -1, &error));
  first(source->curves)->points[0].weight = -1;
  EXPECT_FALSE(transform_geometry(*source, matrix, 0, &error));
  first(source->curves)->points[0].weight = 1;
  const int rounding = std::fegetround();
  BLI_SCOPED_DEFER([&]() { std::fesetround(rounding); });
  ASSERT_EQ(std::fesetround(FE_UPWARD), 0);
  EXPECT_FALSE(transform_geometry(*source, matrix, 0, &error));
}

TEST(nurbs_geometry, SourceTransformHandlesSubnormalProductsWithoutHidingUnderflow)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const double tiny = std::numeric_limits<double>::denorm_min();
  const auto *a = add_vertex(*source, tiny, 0);
  const auto *b = add_vertex(*source, 2 * tiny, 0);
  add_wire_edge(*source, *a, *b);
  double4x4 matrix = double4x4::identity();
  auto identity = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(first(identity->geometry->vertices)->position[0], tiny);
  matrix[0][0] = 2;
  auto exact_scale = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(exact_scale.has_value());
  EXPECT_EQ(first(exact_scale->geometry->vertices)->position[0], 2 * tiny);
  OwnedGeometry integer_source(MEM_new<NurbsGeometry>(__func__));
  const auto *c = add_vertex(*integer_source, 3 * tiny, 0);
  const auto *d = add_vertex(*integer_source, 5 * tiny, 0);
  add_wire_edge(*integer_source, *c, *d);
  matrix[0][0] = 3;
  auto exact_integer_scale = transform_geometry(*integer_source, matrix, 0);
  ASSERT_TRUE(exact_integer_scale.has_value());
  EXPECT_EQ(first(exact_integer_scale->geometry->vertices)->position[0], 9 * tiny);
  matrix[0][0] = 0.5;
  EXPECT_FALSE(transform_geometry(*source, matrix, 0));
  auto rounded = transform_geometry(*source, matrix, tiny);
  ASSERT_TRUE(rounded.has_value());
  EXPECT_EQ(rounded->position_error_bound, tiny);
  EXPECT_EQ(first(rounded->geometry->vertices)->position[0], 0.0);
  EXPECT_EQ(first(source->vertices)->position[0], tiny);
}

#if defined(__SSE2__)
TEST(nurbs_geometry, SourceTransformRejectsFlushToZeroAndDenormalsAsZeroModes)
{
  const unsigned int saved = _mm_getcsr();
  BLI_SCOPED_DEFER([&]() { _mm_setcsr(saved); });
  constexpr unsigned int flush_zero = 1u << 15;
  constexpr unsigned int denormals_zero = 1u << 6;
  constexpr unsigned int modes = flush_zero | denormals_zero;
  const unsigned int normal = saved & ~modes;
  _mm_setcsr(normal);
  GeometryPtr source = make_fixture(false);
  const double4x4 matrix = double4x4::identity();
  ASSERT_TRUE(transform_geometry(*source, matrix, 0));
  for (const unsigned int mode : {flush_zero, denormals_zero, modes}) {
    std::string error;
    _mm_setcsr(normal | mode);
    const auto result = transform_geometry(*source, matrix, 0, &error);
    const auto curve_point = evaluate_curve(*first(source->curves), 0.5);
    const auto surface_point = evaluate_surface(*first(source->surfaces), {0.5, 0.5});
    const auto boundary = evaluate_boundaries(*source, {2, 100, 0});
    const unsigned int remaining_modes = _mm_getcsr() & modes;
    _mm_setcsr(normal);
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(curve_point.has_value());
    EXPECT_FALSE(surface_point.has_value());
    EXPECT_FALSE(boundary.has_value());
    EXPECT_NE(error.find("subnormal"), std::string::npos);
    EXPECT_EQ(remaining_modes, mode);
  }
}
#endif

#ifdef WITH_GMP
TEST(nurbs_geometry, AffineRoundoffBoundCoversExactRationalReference)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*source, 0.1, 0.2);
  const auto *b = add_vertex(*source, 0.7, 0.9);
  add_wire_edge(*source, *a, *b);
  first(source->edges)->tolerance = 0.25;
  double4x4 matrix = double4x4::identity();
  matrix[0][1] = 0.4;
  matrix[1][0] = 0.3;
  matrix.location() = {0.1, 0.2, -0.3};
  EXPECT_FALSE(transform_geometry(*source, matrix, 0));
  auto result = transform_geometry(*source, matrix, 1.0e-12);
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->position_error_bound, 0.0);
  EXPECT_GE(result->tolerance_scale_bound, 1.0);
  EXPECT_LE(result->tolerance_scale_bound, 1.4 + 1.0e-14);
  const mpq_class bound(result->position_error_bound);
  auto check_point = [&](const double before[3], const double after[3]) {
    mpq_class l1_error(0);
    for (int row = 0; row < 3; row++) {
      mpq_class exact(matrix[3][row]);
      for (int column = 0; column < 3; column++) {
        exact += mpq_class(matrix[column][row]) * mpq_class(before[column]);
      }
      const mpq_class error = exact - mpq_class(after[row]);
      l1_error += error < 0 ? -error : error;
    }
    EXPECT_TRUE(l1_error <= bound);
  };
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    check_point(vertex.position, index->vertex(vertex.id)->position);
  }
  for (int i = 0; i < first(source->curves)->points_num; i++) {
    check_point(first(source->curves)->points[i].position,
                first(result->geometry->curves)->points[i].position);
  }
  EXPECT_GE(first(result->geometry->edges)->tolerance, 0.25 * result->tolerance_scale_bound);
  /* Exact rational norm witnesses avoid using the same floating arithmetic as the service. */
  const double directions[][3] = {{1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0.3, -0.7, 0.2}};
  for (const auto &direction : directions) {
    mpq_class source_squared(0), transformed_squared(0);
    for (int row = 0; row < 3; row++) {
      source_squared += mpq_class(direction[row]) * mpq_class(direction[row]);
      mpq_class transformed(0);
      for (int column = 0; column < 3; column++) {
        transformed += mpq_class(matrix[column][row]) * mpq_class(direction[column]);
      }
      transformed_squared += transformed * transformed;
    }
    const mpq_class scale(result->tolerance_scale_bound);
    EXPECT_TRUE(transformed_squared <= scale * scale * source_squared);
  }
}
#endif

TEST(nurbs_geometry, RationalArcEvaluationPreservesShapeAndDomain)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsCurveGeometry &arc = *add_rational_arc_payload(*geometry);
  const double weight = arc.points[1].weight;
  for (int i = 0; i <= 64; i++) {
    std::string error = "old error";
    const auto point = evaluate_curve(arc, 2.0 + 3.0 * i / 64.0, &error);
    ASSERT_TRUE(point.has_value()) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_NEAR(math::length_squared(point->position), 1.0, 1.0e-14);
    EXPECT_DOUBLE_EQ(point->weight_condition, 1.0);
  }
  const auto midpoint = evaluate_curve(arc, 3.5);
  ASSERT_TRUE(midpoint.has_value());
  EXPECT_NEAR(midpoint->position.x, std::sqrt(0.5), 1.0e-15);
  EXPECT_NEAR(midpoint->position.y, std::sqrt(0.5), 1.0e-15);
  EXPECT_EQ(arc.points[1].weight, weight);
  EXPECT_EQ(arc.knots[0], 2.0);
  EXPECT_EQ(arc.knots[5], 5.0);
  std::string error;
  EXPECT_FALSE(evaluate_curve(arc, 0.5, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(evaluate_curve(arc, std::numeric_limits<double>::quiet_NaN()).has_value());
}

TEST(nurbs_geometry, ExactClosedRationalCircleEvaluation)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsCurveGeometry &circle = *add_rational_arc_payload(*geometry);
  reset_curve(circle, 9, 3, 0.0, 4.0);
  const double xy[9][2] = {
      {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}};
  const double knots[12] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  std::copy_n(knots, 12, circle.knots);
  for (int i = 0; i < 9; i++) {
    std::copy_n(xy[i], 2, circle.points[i].position);
    circle.points[i].weight = i % 2 ? std::sqrt(0.5) : 1.0;
  }
  for (int i = 0; i <= 128; i++) {
    const auto point = evaluate_curve(circle, i / 32.0);
    ASSERT_TRUE(point.has_value());
    EXPECT_NEAR(math::length_squared(point->position), 1.0, 1.0e-14);
  }
  const auto start = evaluate_curve(circle, 0.0);
  const auto end = evaluate_curve(circle, 4.0);
  ASSERT_TRUE(start.has_value() && end.has_value());
  EXPECT_EQ(start->position, end->position);
}

TEST(nurbs_geometry, KnotLimitsAndUnclampedEndpoints)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsCurveGeometry &curve = *add_rational_arc_payload(*geometry);
  reset_curve(curve, 4, 2, 0.0, 2.0);
  const double split_knots[6] = {0, 0, 1, 1, 2, 2};
  std::copy_n(split_knots, 6, curve.knots);
  for (int i = 0; i < 4; i++) {
    curve.points[i].position[0] = i;
  }
  const auto right_limit = evaluate_curve(curve, 1.0);
  const auto split_end = evaluate_curve(curve, 2.0);
  ASSERT_TRUE(right_limit.has_value() && split_end.has_value());
  EXPECT_DOUBLE_EQ(right_limit->position.x, 2.0);
  EXPECT_DOUBLE_EQ(split_end->position.x, 3.0);
  reset_curve(curve, 3, 3, 0.0, 5.0);
  for (int i = 0; i < 6; i++) {
    curve.knots[i] = i;
  }
  for (int i = 0; i < 3; i++) {
    curve.points[i].position[0] = 2.0 * i;
  }
  const auto start = evaluate_curve(curve, 2.0);
  const auto end = evaluate_curve(curve, 3.0);
  ASSERT_TRUE(start.has_value() && end.has_value());
  EXPECT_DOUBLE_EQ(start->position.x, 1.0);
  EXPECT_DOUBLE_EQ(end->position.x, 3.0);
  EXPECT_FALSE(evaluate_curve(curve, 1.0).has_value());
}

TEST(nurbs_geometry, RationalScalingPolesAndLargeCoordinates)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsCurveGeometry &curve = *add_rational_arc_payload(*geometry);
  for (const double scale : {1.0e-300, 1.0e300}) {
    curve.points[0].weight = curve.points[2].weight = scale;
    curve.points[1].weight = scale * std::sqrt(0.5);
    const auto point = evaluate_curve(curve, 3.5);
    ASSERT_TRUE(point.has_value());
    EXPECT_NEAR(point->position.x, std::sqrt(0.5), 1.0e-15);
  }
  reset_curve(curve, 2, 2, 0.0, 1.0);
  curve.points[1].position[0] = 1.0;
  curve.points[1].weight = -1.0;
  std::string error;
  EXPECT_FALSE(evaluate_curve(curve, 0.5, &error).has_value());
  EXPECT_FALSE(error.empty());
  const auto negative = evaluate_curve(curve, 0.25);
  ASSERT_TRUE(negative.has_value());
  EXPECT_DOUBLE_EQ(negative->position.x, -0.5);
  EXPECT_DOUBLE_EQ(negative->weight_condition, 2.0);
  curve.points[0].position[0] = -1.0e308;
  curve.points[1].position[0] = 1.0e308;
  curve.points[0].weight = curve.points[1].weight = 1.0e308;
  const auto extreme = evaluate_curve(curve, 0.5);
  ASSERT_TRUE(extreme.has_value());
  EXPECT_DOUBLE_EQ(extreme->position.x, 0.0);
  curve.points[0].position[0] = 1.0e12 + 0.125;
  curve.points[1].position[0] = 1.0e12 + 0.375;
  const auto translated = evaluate_curve(curve, 0.5);
  ASSERT_TRUE(translated.has_value());
  EXPECT_DOUBLE_EQ(translated->position.x, 1.0e12 + 0.25);
  reset_curve(curve, 2, 2, 0.0, 1.0e-320);
  curve.points[1].position[0] = 1.0;
  const auto tiny_domain = evaluate_curve(curve, 5.0e-321);
  ASSERT_TRUE(tiny_domain.has_value());
  EXPECT_DOUBLE_EQ(tiny_domain->position.x, 0.5);
}

TEST(nurbs_geometry, RationalCubicMatchesIndependentBernsteinFormula)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsCurveGeometry &curve = *add_rational_arc_payload(*geometry);
  reset_curve(curve, 4, 4, -2.0, 7.0);
  const double coordinates[4][3] = {{0, 0, 0}, {1, 2, -1}, {3, -2, 1}, {4, 0, 2}};
  const double weights[4] = {1.0, 0.3, 2.0, 1.0};
  for (int i = 0; i < 4; i++) {
    std::copy_n(coordinates[i], 3, curve.points[i].position);
    curve.points[i].weight = weights[i];
  }
  for (int i = 0; i <= 32; i++) {
    const double t = i / 32.0;
    const double s = 1.0 - t;
    const double bernstein[4] = {s * s * s, 3.0 * s * s * t, 3.0 * s * t * t, t * t * t};
    double3 numerator(0.0);
    double denominator = 0.0;
    for (int j = 0; j < 4; j++) {
      numerator += double3(coordinates[j]) * (bernstein[j] * weights[j]);
      denominator += bernstein[j] * weights[j];
    }
    const auto point = evaluate_curve(curve, -2.0 + 9.0 * t);
    ASSERT_TRUE(point.has_value());
    EXPECT_NEAR(math::distance(point->position, numerator / denominator), 0.0, 1.0e-14);
  }
}

TEST(nurbs_geometry, TensorSurfaceEvaluationAndFailure)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsSurfaceGeometry &surface = *add_arc_strip(*geometry, false);
  for (int u = 0; u <= 16; u++) {
    for (int v = 0; v <= 8; v++) {
      const auto point = evaluate_surface(surface, {2.0 + 3.0 * u / 16.0, v / 8.0});
      ASSERT_TRUE(point.has_value());
      EXPECT_NEAR(point->position.x * point->position.x + point->position.y * point->position.y,
                  1.0,
                  1.0e-14);
      EXPECT_NEAR(point->position.z, v / 8.0, 1.0e-15);
    }
  }
  EXPECT_FALSE(evaluate_surface(surface, {3.5, 1.1}).has_value());
  surface.order_u = 4;
  EXPECT_FALSE(evaluate_surface(surface, {3.5, 0.5}).has_value());
}

TEST(nurbs_geometry, CurvedSharedEdgeMatchesBothSurfaceParameterizationsAtSamples)
{
  GeometryPtr geometry = make_curved_fixture();
  std::string error;
  ASSERT_TRUE(geometry_validate_structure(*geometry, &error)) << error;
  const auto index = TopologyIndex::build(*geometry);
  ASSERT_TRUE(index.has_value());
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : geometry->curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : geometry->surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  int shared_edges = 0;
  for (const NurbsTopologyEdge &edge : geometry->edges) {
    shared_edges += index->uses_for_edge(edge.id).size() == 2;
    for (int i = 0; i <= 32; i++) {
      const double t = 2.0 + 3.0 * i / 32.0;
      const auto source = evaluate_curve(*curves.lookup(edge.curve_id), t);
      ASSERT_TRUE(source.has_value());
      if (i == 0 || i == 32) {
        const NurbsTopologyVertex *vertex = index->vertex(edge.vertex_ids[i == 32]);
        EXPECT_NEAR(math::distance(source->position, double3(vertex->position)), 0.0, 1.0e-14);
      }
      for (const EdgeUseReference &ref : index->uses_for_edge(edge.id)) {
        const NurbsTopologyFace &face = *index->face(ref.face_id);
        const auto uv = evaluate_curve(*curves.lookup(ref.use->pcurve_id), t);
        ASSERT_TRUE(uv.has_value());
        const auto support = evaluate_surface(*surfaces.lookup(face.surface_id),
                                              {uv->position.x, uv->position.y});
        ASSERT_TRUE(support.has_value());
        EXPECT_NEAR(math::distance(source->position, support->position), 0.0, 1.0e-13);
      }
    }
  }
  EXPECT_EQ(shared_edges, 1);
}

static OwnedGeometry make_extrusion_sheet()
{
  OwnedGeometry wire(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*wire, -1, -1);
  const auto *b = add_vertex(*wire, 1, -1);
  const auto *edge = add_wire_edge(*wire, *a, *b);
  auto sheet = extrude_edges(*wire, {edge->id}, {0, 2, 0}, 0);
  return sheet ? std::move(sheet->geometry) : nullptr;
}

static void expect_closed_oriented_incidence(const NurbsGeometry &geometry)
{
  const auto index = TopologyIndex::build(geometry);
  ASSERT_TRUE(index.has_value());
  for (const NurbsTopologyEdge &edge : geometry.edges) {
    SCOPED_TRACE(edge.id);
    const auto uses = index->uses_for_edge(edge.id);
    ASSERT_EQ(uses.size(), 2);
    const auto sense = [&](const EdgeUseReference &use) {
      return index->face(use.face_id)->orientation * (use.use->reversed ? -1 : 1);
    };
    EXPECT_EQ(sense(uses[0]), -sense(uses[1]));
  }
}

TEST(nurbs_geometry, RegionConstructionSharesWallsAndContinuesAfterExtrusionAndVertexDeletion)
{
  const auto sheet = make_extrusion_sheet();
  ASSERT_NE(sheet, nullptr);
  const int64_t face_id = first(sheet->faces)->id;
  const int64_t counter = sheet->next_id;
  std::string error;
  auto first_sweep = extrude_components(*sheet, {{}, {}, {&face_id, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(first_sweep.has_value()) << error;
  EXPECT_EQ(first_sweep->geometry->vertices.count(), 8);
  EXPECT_EQ(first_sweep->geometry->edges.count(), 12);
  EXPECT_EQ(first_sweep->geometry->faces.count(), 6);
  EXPECT_EQ(first_sweep->vertex_to_rail.size(), 4);
  EXPECT_EQ(first_sweep->edge_to_face.size(), 4);
  const int64_t cap = first_sweep->source_to_cap.lookup(face_id);
  const auto index = TopologyIndex::build(*first_sweep->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->face(face_id)->orientation, -first(sheet->faces)->orientation);
  EXPECT_EQ(index->face(cap)->orientation, first(sheet->faces)->orientation);
  EXPECT_EQ(first(sheet->faces)->orientation, 1);
  EXPECT_EQ(sheet->next_id, counter);
  expect_closed_oriented_incidence(*first_sweep->geometry);
  Vector<int64_t> first_faces;
  for (const NurbsTopologyFace &face : first_sweep->geometry->faces) {
    first_faces.append(face.id);
  }
  check_constructed_boundaries(*first_sweep->geometry, first_faces);
  auto second = extrude_components(
      *first_sweep->geometry, {{}, {}, {&cap, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(second.has_value()) << error;
  EXPECT_EQ(second->geometry->vertices.count(), 12);
  EXPECT_EQ(second->geometry->edges.count(), 20);
  EXPECT_EQ(second->geometry->faces.count(), 10);
  EXPECT_TRUE(second->removed.faces.contains(cap));
  const auto second_index = TopologyIndex::build(*second->geometry);
  ASSERT_TRUE(second_index.has_value());
  EXPECT_EQ(second_index->face(cap), nullptr);
  expect_closed_oriented_incidence(*second->geometry);
  Vector<int64_t> top_vertices, opening_edges;
  for (const NurbsTopologyVertex &vertex : first_sweep->geometry->vertices) {
    if (const int64_t *mapped = second->source_to_cap.lookup_ptr(vertex.id)) {
      top_vertices.append(*mapped);
    }
  }
  for (const NurbsTopologyEdge &edge : first_sweep->geometry->edges) {
    if (second->source_to_cap.contains(edge.id)) {
      opening_edges.append(edge.id);
    }
  }
  ASSERT_EQ(top_vertices.size(), 4);
  auto deleted = delete_components(
      *second->geometry, {top_vertices, {}, {}}, DeleteMode::Vertices, &error);
  ASSERT_TRUE(deleted.has_value()) << error;
  EXPECT_EQ(deleted->geometry->vertices.count(), 8);
  EXPECT_EQ(deleted->geometry->edges.count(), 12);
  EXPECT_EQ(deleted->geometry->faces.count(), 5);
  auto continued = extrude_components(
      *deleted->geometry, {{}, opening_edges, {}}, {0, 0, 1}, 0, &error);
  ASSERT_TRUE(continued.has_value()) << error;
  EXPECT_EQ(continued->geometry->vertices.count(), 12);
  EXPECT_EQ(continued->geometry->edges.count(), 20);
  EXPECT_EQ(continued->geometry->faces.count(), 9);
}

TEST(nurbs_geometry, DuplicateFaceCanBeSnappedToAdjacentCornerAndStillExtruded)
{
  const auto source = make_extrusion_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t source_face = first(source->faces)->id;
  std::string error;

  const auto duplicate = duplicate_components(*source, {{}, {}, {&source_face, 1}}, &error);
  ASSERT_TRUE(duplicate.has_value()) << error;
  const int64_t duplicate_face = duplicate->source_to_copy.lookup(source_face);

  Vector<int64_t> duplicate_vertices;
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    duplicate_vertices.append(duplicate->source_to_copy.lookup(vertex.id));
  }
  /* Moving by the face width makes its left corners coincide with the original right corners,
   * matching a vertex snap. Snapping moves geometry; it does not implicitly weld topology. */
  const auto snapped = translate_vertices(
      *duplicate->geometry, duplicate_vertices, {2, 0, 0}, 0, &error);
  ASSERT_TRUE(snapped.has_value()) << error;

  const auto result = extrude_components(
      *snapped->geometry, {{}, {}, {&duplicate_face, 1}}, {0, 0, 1}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->geometry->faces.count(), 7);
  EXPECT_EQ(result->geometry->vertices.count(), 12);
}

TEST(nurbs_geometry, MultiFaceRegionCreatesOnlyOuterWallsAndKeepsSharedCapResources)
{
  const auto source = make_fixture(false);
  const int64_t faces[] = {first(source->faces)->id, last(source->faces)->id};
  const int64_t reversed[] = {faces[1], faces[0], faces[1]};
  auto result = extrude_components(*source, {{}, {}, {faces, 2}}, {0, 0, 2}, 0);
  auto repeated = extrude_components(*source, {{}, {}, {reversed, 3}}, {0, 0, 2}, 0);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(repeated.has_value());
  EXPECT_EQ(result->geometry->vertices.count(), 12);
  EXPECT_EQ(result->geometry->edges.count(), 20);
  EXPECT_EQ(result->geometry->faces.count(), 10);
  EXPECT_EQ(result->geometry->surfaces.count(), 8);
  EXPECT_EQ(result->vertex_to_rail.size(), 6);
  EXPECT_EQ(result->edge_to_face.size(), 6);
  const auto source_index = TopologyIndex::build(*source);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(source_index.has_value());
  ASSERT_TRUE(index.has_value());
  for (const NurbsTopologyEdge &edge : source->edges) {
    if (source_index->uses_for_edge(edge.id).size() == 2) {
      EXPECT_FALSE(result->edge_to_face.contains(edge.id));
      EXPECT_EQ(index->uses_for_edge(result->source_to_cap.lookup(edge.id)).size(), 2);
    }
  }
  EXPECT_EQ(index->face(result->source_to_cap.lookup(faces[0]))->surface_id,
            index->face(result->source_to_cap.lookup(faces[1]))->surface_id);
  EXPECT_EQ(result->geometry->next_id, repeated->geometry->next_id);
  for (const auto item : result->source_to_cap.items()) {
    EXPECT_EQ(item.value, repeated->source_to_cap.lookup(item.key));
  }
  expect_closed_oriented_incidence(*result->geometry);
}

TEST(nurbs_geometry, ReplacingFiveCubeFacesPrunesOnlyUnusedOriginalInteriorComponents)
{
  const auto sheet = make_extrusion_sheet();
  ASSERT_NE(sheet, nullptr);
  const int64_t floor = first(sheet->faces)->id;
  const auto cube = extrude_components(*sheet, {{}, {}, {&floor, 1}}, {0, 0, 2}, 0);
  ASSERT_TRUE(cube.has_value());
  Vector<int64_t> selected;
  for (const NurbsTopologyFace &face : cube->geometry->faces) {
    if (face.id != floor) {
      selected.append(face.id);
    }
  }
  std::string error;
  const auto result = extrude_components(
      *cube->geometry, {{}, {}, selected}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->geometry->vertices.count(), 12);
  EXPECT_EQ(result->geometry->edges.count(), 20);
  EXPECT_EQ(result->geometry->faces.count(), 10);
  EXPECT_EQ(result->removed.faces.size(), 5);
  EXPECT_EQ(result->removed.edges.size(), 8);
  EXPECT_EQ(result->removed.vertices.size(), 4);
  EXPECT_EQ(result->vertex_to_rail.size(), 4);
  EXPECT_EQ(result->edge_to_face.size(), 4);
  EXPECT_FALSE(result->removed.faces.contains(floor));
  expect_closed_oriented_incidence(*result->geometry);
}

TEST(nurbs_geometry, RegionHoleUsesRealBoundariesWithoutInteriorWalls)
{
  const auto source = make_fixture();
  const int64_t faces[] = {first(source->faces)->id, last(source->faces)->id};
  std::string error;
  const auto result = extrude_components(*source, {{}, {}, {faces, 2}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->geometry->vertices.count(), 20);
  EXPECT_EQ(result->geometry->edges.count(), 32);
  EXPECT_EQ(result->geometry->faces.count(), 14);
  EXPECT_EQ(result->edge_to_face.size(), 10);
  EXPECT_EQ(result->vertex_to_rail.size(), 10);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->face(result->source_to_cap.lookup(faces[0]))->loops.count(), 2);
  expect_closed_oriented_incidence(*result->geometry);
  /* The source faces share one interior edge, which must not become a wall. */
  const auto original = TopologyIndex::build(*source);
  ASSERT_TRUE(original.has_value());
  for (const NurbsTopologyEdge &edge : source->edges) {
    if (original->uses_for_edge(edge.id).size() == 2) {
      EXPECT_FALSE(result->edge_to_face.contains(edge.id));
    }
  }
}

TEST(nurbs_geometry, CurvedRegionPreservesCapBasisAndParameterCurves)
{
  const auto source = make_curved_fixture();
  const int64_t faces[] = {first(source->faces)->id, last(source->faces)->id};
  const double3 delta(1, 2, 3);
  std::string error;
  const auto result = extrude_components(
      *source, {{}, {}, {faces, 2}}, delta, std::numeric_limits<double>::max(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : result->geometry->curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : result->geometry->surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (const NurbsCurveGeometry &curve : source->curves) {
    const auto &cap = *curves.lookup(result->source_to_cap.lookup(curve.id));
    EXPECT_EQ(cap.order, curve.order);
    EXPECT_EQ(std::memcmp(cap.knots, curve.knots, sizeof(double) * curve.knots_num), 0);
    for (int i = 0; i < curve.points_num; i++) {
      EXPECT_EQ(cap.points[i].weight, curve.points[i].weight);
      EXPECT_EQ(double3(cap.points[i].position),
                double3(curve.points[i].position) + (curve.dimension == 3 ? delta : double3(0)));
    }
  }
  for (const NurbsSurfaceGeometry &surface : source->surfaces) {
    const auto &cap = *surfaces.lookup(result->source_to_cap.lookup(surface.id));
    EXPECT_EQ(cap.order_u, surface.order_u);
    EXPECT_EQ(cap.order_v, surface.order_v);
    EXPECT_EQ(std::memcmp(cap.knots_u, surface.knots_u, sizeof(double) * surface.knots_u_num), 0);
    EXPECT_EQ(std::memcmp(cap.knots_v, surface.knots_v, sizeof(double) * surface.knots_v_num), 0);
    for (int i = 0; i < surface.points_num; i++) {
      EXPECT_EQ(cap.points[i].weight, surface.points[i].weight);
      EXPECT_EQ(double3(cap.points[i].position), double3(surface.points[i].position) + delta);
    }
  }
  Vector<int64_t> created;
  for (const auto item : result->edge_to_face.items()) {
    created.append(item.value);
  }
  check_constructed_boundaries(*result->geometry, created);
}

TEST(nurbs_geometry, MixedRegionWireAndPointExtrusionSharesOneCandidateAndSupportsZeroSweep)
{
  auto source = make_extrusion_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t face_id = first(source->faces)->id;
  const auto *a = add_vertex(*source, 10, 0);
  const auto *b = add_vertex(*source, 11, 0);
  const auto *edge = add_wire_edge(*source, *a, *b);
  const auto *point = add_vertex(*source, 20, 0);
  std::string error;
  const auto result = extrude_components(
      *source, {{&point->id, 1}, {&edge->id, 1}, {&face_id, 1}}, {0, 0, 0}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->geometry->vertices.count(), 14);
  EXPECT_EQ(result->geometry->edges.count(), 17);
  EXPECT_EQ(result->geometry->faces.count(), 7);
  EXPECT_EQ(result->vertex_to_rail.size(), 7);
  EXPECT_EQ(result->edge_to_face.size(), 5);
  EXPECT_EQ(result->translation_error_bound, 0);
  const auto index = TopologyIndex::build(*result->geometry);
  ASSERT_TRUE(index.has_value());
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    const auto *copy = index->vertex(result->source_to_cap.lookup(vertex.id));
    ASSERT_NE(copy, nullptr);
    EXPECT_NE(copy->id, vertex.id);
    EXPECT_EQ(std::memcmp(copy->position, vertex.position, sizeof(vertex.position)), 0);
  }
}

TEST(nurbs_geometry, ExtrusionPreservesUntouchedSignedZeroAndRejectsUnsupportedCapWeights)
{
  auto source = make_extrusion_sheet();
  ASSERT_NE(source, nullptr);
  auto *vertex = first(source->vertices);
  vertex->position[2] = -0.0;
  const int64_t face_id = first(source->faces)->id;
  const int64_t counter = source->next_id;
  const auto zero = extrude_components(*source, {{}, {}, {&face_id, 1}}, {0, 0, 0}, 0);
  ASSERT_TRUE(zero.has_value());
  const auto index = TopologyIndex::build(*zero->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_TRUE(std::signbit(index->vertex(zero->source_to_cap.lookup(vertex->id))->position[2]));
  const auto shifted = extrude_components(*source, {{}, {}, {&face_id, 1}}, {2, 0, 0}, 0);
  ASSERT_TRUE(shifted.has_value());
  const auto shifted_index = TopologyIndex::build(*shifted->geometry);
  ASSERT_TRUE(shifted_index.has_value());
  EXPECT_TRUE(
      std::signbit(shifted_index->vertex(shifted->source_to_cap.lookup(vertex->id))->position[2]));
  first(source->surfaces)->points[0].weight = -1;
  std::string error;
  EXPECT_FALSE(extrude_components(*source, {{}, {}, {&face_id, 1}}, {0, 0, 1}, 0, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(source->faces.count(), 1);
  EXPECT_EQ(first(source->surfaces)->points[0].weight, -1);
}

TEST(nurbs_geometry, ComponentCopyPreservesSharedClosureAndHolesWithFreshIDs)
{
  GeometryPtr source = make_fixture();
  const int64_t next_id = source->next_id;
  const std::array<int64_t, 2> faces = {first(source->faces)->id, last(source->faces)->id};
  ComponentSelection selection;
  selection.faces = faces;
  std::string error;
  auto copy = copy_component_closure(*source, selection, &error);
  ASSERT_TRUE(copy.has_value()) << error;
  EXPECT_EQ(copy->geometry->vertices.count(), 10);
  EXPECT_EQ(copy->geometry->edges.count(), 11);
  EXPECT_EQ(copy->geometry->faces.count(), 2);
  EXPECT_EQ(copy->geometry->surfaces.count(), 1);
  const auto index = TopologyIndex::build(*copy->geometry);
  ASSERT_TRUE(index.has_value());
  const int64_t shared_edge = first(first(first(source->faces)->loops)->edge_uses)->next->edge_id;
  EXPECT_EQ(index->uses_for_edge(copy->source_to_copy.lookup(shared_edge)).size(), 2);
  EXPECT_EQ(index->face(copy->source_to_copy.lookup(faces[0]))->loops.count(), 2);
  for (const auto item : copy->source_to_copy.items()) {
    EXPECT_GE(item.value, next_id);
    EXPECT_NE(item.key, item.value);
  }
  EXPECT_EQ(copy->geometry->next_id, next_id + copy->source_to_copy.size());
  EXPECT_EQ(source->next_id, next_id);
  EXPECT_EQ(source->faces.count(), 2);
  EXPECT_EQ(first(copy->geometry->faces)->surface_id, last(copy->geometry->faces)->surface_id);
  first(copy->geometry->surfaces)->points[0].position[0] = 123.0;
  EXPECT_EQ(first(source->surfaces)->points[0].position[0], 0.0);
}

TEST(nurbs_geometry, ComponentCopyIsIndependentOfSelectionOrderAndRepeatedIDs)
{
  GeometryPtr source = make_fixture();
  const int64_t a = first(source->faces)->id, b = last(source->faces)->id;
  const std::array<int64_t, 2> ordered = {a, b};
  const std::array<int64_t, 4> repeated = {b, a, b, a};
  ComponentSelection selection_a, selection_b;
  selection_a.faces = ordered;
  selection_b.faces = repeated;
  const auto copy_a = copy_component_closure(*source, selection_a);
  const auto copy_b = copy_component_closure(*source, selection_b);
  ASSERT_TRUE(copy_a.has_value() && copy_b.has_value());
  EXPECT_EQ(copy_a->source_to_copy.size(), copy_b->source_to_copy.size());
  EXPECT_EQ(copy_a->geometry->next_id, copy_b->geometry->next_id);
  for (const auto item : copy_a->source_to_copy.items()) {
    EXPECT_EQ(copy_b->source_to_copy.lookup(item.key), item.value);
  }
}

TEST(nurbs_geometry, ComponentCopyIncludesDependenciesWithoutIncidentFaces)
{
  GeometryPtr source = make_fixture();
  NurbsTopologyVertex *isolated = add_vertex(*source, 4.0, 5.0);
  const NurbsTopologyEdge &edge = *first(source->edges);
  const std::array<int64_t, 3> vertices = {isolated->id, edge.vertex_ids[0], isolated->id};
  const std::array<int64_t, 2> edges = {edge.id, edge.id};
  ComponentSelection selection;
  selection.vertices = vertices;
  selection.edges = edges;
  const auto copy = copy_component_closure(*source, selection);
  ASSERT_TRUE(copy.has_value());
  EXPECT_EQ(copy->geometry->vertices.count(), 3);
  EXPECT_EQ(copy->geometry->edges.count(), 1);
  EXPECT_EQ(copy->geometry->curves.count(), 1);
  EXPECT_EQ(copy->geometry->faces.count(), 0);
  EXPECT_EQ(copy->geometry->surfaces.count(), 0);
  EXPECT_EQ(first(copy->geometry->curves)->dimension, 3);
  EXPECT_FALSE(copy->source_to_copy.contains(first(source->faces)->id));
}

TEST(nurbs_geometry, ComponentCopyRejectsInvalidSelectionAndIdentityOverflowAtomically)
{
  GeometryPtr source = make_fixture();
  const int64_t next_id = source->next_id;
  const std::array<int64_t, 1> wrong_domain = {first(source->edges)->id};
  ComponentSelection selection;
  selection.faces = wrong_domain;
  std::string error;
  EXPECT_FALSE(copy_component_closure(*source, selection, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source->next_id, next_id);
  const std::array<int64_t, 1> faces = {first(source->faces)->id};
  selection.faces = faces;
  source->next_id = std::numeric_limits<int64_t>::max();
  EXPECT_FALSE(copy_component_closure(*source, selection, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source->next_id, std::numeric_limits<int64_t>::max());
  EXPECT_EQ(source->faces.count(), 2);
  const auto empty = copy_component_closure(*source, {});
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->source_to_copy.is_empty());
  EXPECT_EQ(empty->geometry->next_id, source->next_id);
  EXPECT_TRUE(empty->geometry->vertices.is_empty());
  EXPECT_TRUE(empty->geometry->faces.is_empty());
}

TEST(nurbs_geometry, ComponentCopyRetainsRationalCoefficientsAndParameterMappingsExactly)
{
  GeometryPtr source = make_curved_fixture();
  const std::array<int64_t, 2> faces = {first(source->faces)->id, last(source->faces)->id};
  ComponentSelection selection;
  selection.faces = faces;
  const auto copy = copy_component_closure(*source, selection);
  ASSERT_TRUE(copy.has_value());
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : copy->geometry->curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : copy->geometry->surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (const NurbsCurveGeometry &curve : source->curves) {
    const NurbsCurveGeometry &mapped = *curves.lookup(copy->source_to_copy.lookup(curve.id));
    EXPECT_NE(mapped.points, curve.points);
    EXPECT_NE(mapped.knots, curve.knots);
    EXPECT_EQ(mapped.order, curve.order);
    EXPECT_EQ(mapped.dimension, curve.dimension);
    EXPECT_EQ(
        std::memcmp(mapped.points, curve.points, sizeof(NurbsControlPoint) * curve.points_num), 0);
    EXPECT_EQ(std::memcmp(mapped.knots, curve.knots, sizeof(double) * curve.knots_num), 0);
  }
  for (const NurbsSurfaceGeometry &surface : source->surfaces) {
    const NurbsSurfaceGeometry &mapped = *surfaces.lookup(copy->source_to_copy.lookup(surface.id));
    EXPECT_NE(mapped.points, surface.points);
    EXPECT_EQ(
        std::memcmp(mapped.points, surface.points, sizeof(NurbsControlPoint) * surface.points_num),
        0);
    EXPECT_EQ(std::memcmp(mapped.knots_u, surface.knots_u, sizeof(double) * surface.knots_u_num),
              0);
    EXPECT_EQ(std::memcmp(mapped.knots_v, surface.knots_v, sizeof(double) * surface.knots_v_num),
              0);
  }
}

TEST(nurbs_geometry, DuplicateCandidatePreservesSharedClosureAndOriginalIdentity)
{
  const GeometryPtr source = make_fixture();
  const int64_t counter = source->next_id;
  const int64_t faces[] = {first(source->faces)->id, last(source->faces)->id};
  std::string error;
  const auto duplicate = duplicate_components(*source, {{}, {}, {faces, 2}}, &error);
  ASSERT_TRUE(duplicate.has_value()) << error;
  const auto &geometry = *duplicate->geometry;
  EXPECT_EQ(geometry.vertices.count(), 20);
  EXPECT_EQ(geometry.edges.count(), 22);
  EXPECT_EQ(geometry.faces.count(), 4);
  EXPECT_EQ(geometry.surfaces.count(), 2);
  const auto index = TopologyIndex::build(geometry);
  const auto original = TopologyIndex::build(*source);
  ASSERT_TRUE(index.has_value());
  ASSERT_TRUE(original.has_value());
  for (const NurbsTopologyVertex &vertex : source->vertices) {
    const auto *unchanged = index->vertex(vertex.id);
    const auto *copied = index->vertex(duplicate->source_to_copy.lookup(vertex.id));
    ASSERT_NE(unchanged, nullptr);
    ASSERT_NE(copied, nullptr);
    EXPECT_NE(unchanged, &vertex);
    EXPECT_NE(unchanged, copied);
    EXPECT_EQ(std::memcmp(unchanged->position, vertex.position, sizeof(vertex.position)), 0);
    EXPECT_EQ(std::memcmp(copied->position, vertex.position, sizeof(vertex.position)), 0);
    EXPECT_EQ(copied->tolerance, vertex.tolerance);
  }
  for (const NurbsTopologyEdge &edge : source->edges) {
    const auto *copied = index->edge(duplicate->source_to_copy.lookup(edge.id));
    ASSERT_NE(copied, nullptr);
    EXPECT_EQ(copied->vertex_ids[0], duplicate->source_to_copy.lookup(edge.vertex_ids[0]));
    EXPECT_EQ(copied->vertex_ids[1], duplicate->source_to_copy.lookup(edge.vertex_ids[1]));
    EXPECT_EQ(index->uses_for_edge(copied->id).size(), original->uses_for_edge(edge.id).size());
    EXPECT_EQ(index->uses_for_edge(edge.id).size(), original->uses_for_edge(edge.id).size());
  }
  EXPECT_EQ(index->face(duplicate->source_to_copy.lookup(faces[0]))->loops.count(), 2);
  EXPECT_EQ(source->next_id, counter);
  EXPECT_EQ(geometry.next_id, counter + duplicate->source_to_copy.size());
}

TEST(nurbs_geometry, DuplicateRetainsOriginalAndCopiedRationalResourcesBitForBit)
{
  const GeometryPtr source = make_curved_fixture();
  const int64_t faces[] = {first(source->faces)->id, last(source->faces)->id};
  const auto duplicate = duplicate_components(*source, {{}, {}, {faces, 2}});
  ASSERT_TRUE(duplicate.has_value());
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : duplicate->geometry->curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : duplicate->geometry->surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (const NurbsCurveGeometry &curve : source->curves) {
    for (const int64_t id : {curve.id, duplicate->source_to_copy.lookup(curve.id)}) {
      const auto &mapped = *curves.lookup(id);
      EXPECT_NE(mapped.points, curve.points);
      EXPECT_EQ(mapped.order, curve.order);
      EXPECT_EQ(mapped.dimension, curve.dimension);
      EXPECT_EQ(
          std::memcmp(mapped.points, curve.points, sizeof(NurbsControlPoint) * curve.points_num),
          0);
      EXPECT_EQ(std::memcmp(mapped.knots, curve.knots, sizeof(double) * curve.knots_num), 0);
    }
  }
  for (const NurbsSurfaceGeometry &surface : source->surfaces) {
    for (const int64_t id : {surface.id, duplicate->source_to_copy.lookup(surface.id)}) {
      const auto &mapped = *surfaces.lookup(id);
      EXPECT_NE(mapped.points, surface.points);
      EXPECT_EQ(std::memcmp(
                    mapped.points, surface.points, sizeof(NurbsControlPoint) * surface.points_num),
                0);
      EXPECT_EQ(std::memcmp(mapped.knots_u, surface.knots_u, sizeof(double) * surface.knots_u_num),
                0);
      EXPECT_EQ(std::memcmp(mapped.knots_v, surface.knots_v, sizeof(double) * surface.knots_v_num),
                0);
    }
  }
}

TEST(nurbs_geometry, DuplicateEmptyAndRejectedRequestsLeaveSourceAndCounterUnchanged)
{
  const GeometryPtr source = make_fixture();
  const int64_t counter = source->next_id;
  const auto empty = duplicate_components(*source, {});
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->source_to_copy.is_empty());
  EXPECT_EQ(empty->geometry->next_id, counter);
  EXPECT_EQ(empty->geometry->vertices.count(), source->vertices.count());
  EXPECT_EQ(empty->geometry->faces.count(), source->faces.count());
  std::string error;
  EXPECT_FALSE(duplicate_components(*source, {{&counter, 1}, {}, {}}, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source->next_id, counter);
  source->next_id = std::numeric_limits<int64_t>::max();
  const int64_t id = first(source->faces)->id;
  EXPECT_FALSE(duplicate_components(*source, {{}, {}, {&id, 1}}, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source->next_id, std::numeric_limits<int64_t>::max());
  EXPECT_EQ(source->faces.count(), 2);
}

TEST(nurbs_geometry, SharedEdgeAndHoleStructure)
{
  GeometryPtr geometry = make_fixture();
  std::string error;
  EXPECT_TRUE(geometry_validate_structure(*geometry, &error)) << error;
  EXPECT_EQ(geometry->vertices.count(), 10);
  EXPECT_EQ(geometry->edges.count(), 11);
  EXPECT_EQ(geometry->faces.count(), 2);
  EXPECT_EQ(geometry->surfaces.count(), 1);
  EXPECT_EQ(first(geometry->faces)->loops.count(), 2);
  const NurbsTopologyEdgeUse &a = *first(first(first(geometry->faces)->loops)->edge_uses)->next;
  const NurbsTopologyEdgeUse &b = *last(first(last(geometry->faces)->loops)->edge_uses);
  EXPECT_EQ(a.edge_id, b.edge_id);
  EXPECT_NE(a.id, b.id);
  EXPECT_NE(a.pcurve_id, b.pcurve_id);
  EXPECT_NE(a.reversed, b.reversed);
}

TEST(nurbs_geometry, CopyOwnsNestedGeometryAndReferences)
{
  GeometryPtr source = make_fixture();
  GeometryPtr copy(geometry_copy(source.get()), geometry_free);
  EXPECT_TRUE(geometry_validate_structure(*copy));
  EXPECT_NE(first(source->curves)->points, first(copy->curves)->points);
  EXPECT_NE(first(source->curves)->knots, first(copy->curves)->knots);
  EXPECT_NE(first(source->surfaces)->points, first(copy->surfaces)->points);
  EXPECT_NE(first(source->surfaces)->knots_u, first(copy->surfaces)->knots_u);
  EXPECT_NE(first(source->surfaces)->knots_v, first(copy->surfaces)->knots_v);
  EXPECT_NE(last(first(source->faces)->loops), last(first(copy->faces)->loops));
  EXPECT_NE(first(last(first(source->faces)->loops)->edge_uses),
            first(last(first(copy->faces)->loops)->edge_uses));
  EXPECT_EQ(first(last(first(source->faces)->loops)->edge_uses)->pcurve_id,
            first(last(first(copy->faces)->loops)->edge_uses)->pcurve_id);
  first(copy->surfaces)->points[0].position[0] = 1.0e12 + 0.125;
  EXPECT_EQ(first(source->surfaces)->points[0].position[0], 0.0);
  source.reset();
  EXPECT_TRUE(geometry_validate_structure(*copy));
  EXPECT_EQ(first(copy->surfaces)->points[0].position[0], 1.0e12 + 0.125);
}

TEST(nurbs_geometry, RejectsInvalidReferencesWithoutMutation)
{
  GeometryPtr geometry = make_fixture();
  NurbsTopologyEdgeUse &use = *first(first(first(geometry->faces)->loops)->edge_uses);
  const int64_t original_id = use.edge_id;
  use.edge_id = geometry->next_id + 1;
  const int64_t next_id = geometry->next_id;
  std::string error;
  EXPECT_FALSE(geometry_validate_structure(*geometry, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(use.edge_id, next_id + 1);
  EXPECT_EQ(geometry->next_id, next_id);
  use.edge_id = original_id;
  EXPECT_TRUE(geometry_validate_structure(*geometry, &error));
  EXPECT_TRUE(error.empty());
  use.reversed = !use.reversed;
  EXPECT_FALSE(geometry_validate_structure(*geometry));
}

TEST(nurbs_geometry, RejectsDuplicateIdentityAndInvalidSplineData)
{
  GeometryPtr geometry = make_fixture();
  NurbsCurveGeometry &curve = *first(geometry->curves);
  const int64_t id = curve.id;
  curve.id = first(geometry->vertices)->id;
  EXPECT_FALSE(geometry_validate_structure(*geometry));
  curve.id = id;
  curve.points[0].weight = 0.0;
  curve.points[1].weight = 0.0;
  EXPECT_FALSE(geometry_validate_structure(*geometry));
  curve.points[0].weight = 1.0;
  curve.points[1].weight = 1.0;
  curve.knots[1] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(geometry_validate_structure(*geometry));
  curve.knots[1] = 2.0;
  EXPECT_FALSE(geometry_validate_structure(*geometry));
  curve.knots[1] = 0.0;
  first(geometry->surfaces)->points_u = std::numeric_limits<int>::max();
  EXPECT_FALSE(geometry_validate_structure(*geometry));
}

TEST(nurbs_geometry, CoincidentVerticesRemainDistinctAndWireIsLegal)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsTopologyVertex *a = add_vertex(*geometry, 0.0, 0.0);
  NurbsTopologyVertex *b = add_vertex(*geometry, 0.0, 0.0);
  NurbsTopologyEdge *edge = MEM_new<NurbsTopologyEdge>(__func__);
  edge->id = geometry->next_id++;
  edge->vertex_ids[0] = a->id;
  edge->vertex_ids[1] = b->id;
  edge->curve_id = add_curve(*geometry, *a, *b, false)->id;
  edge->parameter_range[1] = 1.0;
  BLI_addtail(&geometry->edges, edge);
  EXPECT_TRUE(geometry_validate_structure(*geometry));
  EXPECT_NE(a->id, b->id);
  EXPECT_EQ(geometry->vertices.count(), 2);
  /* This is a structural check. A zero-length edge is not certified as usable geometry. */
}

TEST(nurbs_geometry, NonManifoldIncidenceIsNotRejectedAsANonSolid)
{
  GeometryPtr geometry = make_fixture();
  NurbsTopologyFace *face = MEM_new<NurbsTopologyFace>(__func__);
  face->id = geometry->next_id++;
  face->surface_id = first(geometry->surfaces)->id;
  BLI_addtail(&geometry->faces, face);
  std::array<NurbsTopologyVertex *, 4> vertices = {
      first(geometry->vertices),
      first(geometry->vertices)->next,
      first(geometry->vertices)->next->next->next->next,
      first(geometry->vertices)->next->next->next};
  add_loop(*geometry, *face, vertices, false);
  EXPECT_TRUE(geometry_validate_structure(*geometry));
  EXPECT_EQ(geometry->edges.count(), 11);
  const auto index = TopologyIndex::build(*geometry);
  ASSERT_TRUE(index.has_value());
  const int64_t shared_edge_id =
      first(first(first(geometry->faces)->loops)->edge_uses)->next->edge_id;
  EXPECT_EQ(index->uses_for_edge(shared_edge_id).size(), 3);
  EXPECT_EQ(index->edges_for_vertex(vertices[1]->id).size(), 3);
  EXPECT_EQ(index->edge(shared_edge_id)->id, shared_edge_id);
  EXPECT_EQ(index->vertex(vertices[1]->id), vertices[1]);
  EXPECT_EQ(index->face(face->id), face);
  EXPECT_EQ(index->vertex(0), nullptr);
  EXPECT_TRUE(index->uses_for_edge(0).is_empty());
  EXPECT_TRUE(index->edges_for_vertex(0).is_empty());
  first(geometry->edges)->vertex_ids[0] = 0;
  EXPECT_FALSE(TopologyIndex::build(*geometry).has_value());
}

TEST(nurbs_geometry, VertexIncidenceHasNoPatchCornerLimit)
{
  GeometryPtr geometry(MEM_new<NurbsGeometry>(__func__), geometry_free);
  NurbsTopologyVertex *center = add_vertex(*geometry, 0.0, 0.0);
  for (int i = 0; i < 64; i++) {
    NurbsTopologyVertex *end = add_vertex(*geometry, i + 1.0, 1.0);
    NurbsTopologyEdge *edge = MEM_new<NurbsTopologyEdge>(__func__);
    edge->id = geometry->next_id++;
    edge->vertex_ids[0] = center->id;
    edge->vertex_ids[1] = end->id;
    edge->curve_id = add_curve(*geometry, *center, *end, false)->id;
    edge->parameter_range[1] = 1.0;
    BLI_addtail(&geometry->edges, edge);
  }
  const auto index = TopologyIndex::build(*geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->edges_for_vertex(center->id).size(), 64);
  for (const int64_t edge_id : index->edges_for_vertex(center->id)) {
    EXPECT_EQ(index->edge(edge_id)->vertex_ids[0], center->id);
    EXPECT_TRUE(index->uses_for_edge(edge_id).is_empty());
  }
}

class NurbsGeometryOwnerTest : public BlenderGTestBase {};

static SelectionState fixture_selection(const NurbsGeometry &geometry)
{
  SelectionState selection;
  const int64_t vertex = first(geometry.vertices)->id;
  const int64_t edge = first(geometry.edges)->id;
  const int64_t face = first(geometry.faces)->id;
  selection.vertices.add(vertex);
  selection.edges.add(edge);
  selection.faces.add(face);
  selection.active = ElementRef{ElementDomain::Face, face};
  selection.history.append({ElementDomain::Vertex, vertex});
  selection.history.append({ElementDomain::Edge, edge});
  selection.history.append({ElementDomain::Face, face});
  return selection;
}

TEST_F(NurbsGeometryOwnerTest, NativeIDDeepCopy)
{
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "NURBS storage fixture", OB_SURF);
  curve->nurbs_geometry = make_fixture().release();
  curve->nurbs_geometry->stored_selection = selection_to_storage(
      fixture_selection(*curve->nurbs_geometry));
  Curve *copy = reinterpret_cast<Curve *>(BKE_id_copy(bmain, &curve->id));
  ASSERT_NE(copy->nurbs_geometry, nullptr);
  EXPECT_NE(copy->nurbs_geometry, curve->nurbs_geometry);
  EXPECT_NE(first(copy->nurbs_geometry->surfaces)->points,
            first(curve->nurbs_geometry->surfaces)->points);
  EXPECT_TRUE(geometry_validate_structure(*copy->nurbs_geometry));
  ASSERT_NE(copy->nurbs_geometry->stored_selection, nullptr);
  EXPECT_NE(copy->nurbs_geometry->stored_selection, curve->nurbs_geometry->stored_selection);
  EXPECT_NE(copy->nurbs_geometry->stored_selection->elements,
            curve->nurbs_geometry->stored_selection->elements);
  EXPECT_NE(copy->nurbs_geometry->stored_selection->history,
            curve->nurbs_geometry->stored_selection->history);
}

TEST_F(NurbsGeometryOwnerTest, OlderCurveAndSurfaceFilesKeepLegacySourceWithoutPayload)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  struct Fixture {
    const char *path;
    const char *name;
    int points_u, points_v;
    std::array<float, 4> first_point;
  };
  const Fixture fixtures[] = {
      {"render/mesh/texture_space_nurbs.blend",
       "Surface",
       8,
       2,
       {0, -0.8160001635551453f, 0.3f, 1}},
      {"alembic/single-curve.blend", "CurveData", 4, 1, {-1, -1, 0, 1}},
  };
  for (const Fixture &fixture : fixtures) {
    const std::string path = blender::tests::flags_test_asset_dir() + SEP_STR + fixture.path;
    SCOPED_TRACE(path);
    ASSERT_TRUE(BLI_exists(path.c_str()));
    EXPECT_LT(BLO_version_from_file(path.c_str()), 500);
    ReportList reports;
    BKE_reports_init(&reports, RPT_STORE);
    BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
    /* Kernel setup does not register UI space types. Load the actual old Curve ID through
     * Blender's data-only library reader, leaving old window/workspace versioning to app tests. */
    TempLibraryContext *loaded = BLO_library_temp_load_id(
        bmain, path.c_str(), ID_CU_LEGACY, fixture.name, &reports);
    BLI_SCOPED_DEFER([&]() {
      if (loaded != nullptr) {
        BLO_library_temp_free(loaded);
      }
    });
    ASSERT_NE(loaded, nullptr);
    ASSERT_NE(loaded->temp_id, nullptr);
    ASSERT_EQ(loaded->bmain_base->curves.count(), 1);
    const Curve &curve = *first(loaded->bmain_base->curves);
    EXPECT_EQ(loaded->temp_id, &curve.id);
    EXPECT_EQ(curve.nurbs_geometry, nullptr);
    EXPECT_EQ(curve.nurbs_eval_source, nullptr);
    EXPECT_EQ(curve.editnurb, nullptr);
    ASSERT_EQ(curve.nurb.count(), 1);
    const Nurb &nurb = *first(curve.nurb);
    EXPECT_EQ(nurb.type, CU_NURBS);
    EXPECT_EQ(nurb.pntsu, fixture.points_u);
    EXPECT_EQ(nurb.pntsv, fixture.points_v);
    EXPECT_EQ(nurb.orderu, 4);
    ASSERT_NE(nurb.bp, nullptr);
    ASSERT_NE(nurb.knotsu, nullptr);
    for (int axis = 0; axis < 4; axis++) {
      EXPECT_EQ(nurb.bp[0].vec[axis], fixture.first_point[axis]);
    }
    EXPECT_EQ(nurb.topology_face_id, 0);
    for (int i = 0; i < 4; i++) {
      EXPECT_EQ(nurb.topology_vertex_ids[i], 0);
      EXPECT_EQ(nurb.topology_edge_ids[i], 0);
    }
  }
}

TEST_F(NurbsGeometryOwnerTest, NativeFileAndMemfilePersistence)
{
  Main *bmain = BKE_main_new();
  BKE_blender_globals_main_replace(bmain);
  BLI_SCOPED_DEFER([]() { BKE_blender_globals_clear(); });
  Curve *curve = BKE_curve_add(bmain, "NURBS storage fixture", OB_SURF);
  GeometryPtr base = make_fixture();
  const int64_t swept_edge = first(base->edges)->id;
  const int64_t original_face = first(base->faces)->id;
  const int64_t original_neighbor = first(base->faces)->next->id;
  auto swept = extrude_edges(*base, {swept_edge}, {0, 0, 2}, 0);
  ASSERT_TRUE(swept.has_value());
  const int64_t cap_edge = swept->source_to_cap.lookup(swept_edge);
  const int64_t new_face = swept->edge_to_face.lookup(swept_edge);
  curve->nurbs_geometry = swept->geometry.release();
  const SelectionState expected_selection = fixture_selection(*curve->nurbs_geometry);
  curve->nurbs_geometry->stored_selection = selection_to_storage(expected_selection);
  NurbsCurveGeometry *arc = add_rational_arc_payload(*curve->nurbs_geometry);
  const int64_t arc_id = arc->id;
  /* Preserve a rational arc at coordinates that cannot survive a float round trip. */
  for (int i = 0; i < arc->points_num; i++) {
    arc->points[i].position[0] += 1.0e12 + 0.125;
  }
  const int64_t expected_next_id = curve->nurbs_geometry->next_id;
  /* Synthetic runtime state exercises exclusion from both native file and memfile writes.
   * Production Main owners do not acquire evaluated leases. The writer uses a temporary ID. */
  curve->nurbs_eval_source = curve_eval_source_capture(*curve).release();
  ASSERT_NE(curve->nurbs_eval_source, nullptr);
  ASSERT_TRUE(curve->nurbs_eval_source->snapshot.has_value());
  const EvaluationSource *runtime_source = curve->nurbs_eval_source;

  BKE_tempdir_init(nullptr);
  BLI_SCOPED_DEFER([]() { BKE_tempdir_session_purge(); });
  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), "nurbs_storage.blend");
  BLI_SCOPED_DEFER([&]() { BLI_delete(filepath, false, false); });
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  const BlendFileWriteParams params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &params, &reports));
  EXPECT_EQ(curve->nurbs_eval_source, runtime_source);
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  BlendFileData *loaded = BLO_read_from_file(filepath, BLO_READ_SKIP_USERDEF, &read_reports);
  BLI_SCOPED_DEFER([&]() {
    if (loaded != nullptr) {
      BLO_blendfiledata_free(loaded);
    }
  });
  ASSERT_NE(loaded, nullptr);

  auto check_loaded = [&](const BlendFileData &data) {
    Curve *saved_curve = first(data.main->curves);
    ASSERT_NE(saved_curve, nullptr);
    EXPECT_EQ(saved_curve->nurbs_eval_source, nullptr);
    const NurbsGeometry *geometry = saved_curve->nurbs_geometry;
    ASSERT_NE(geometry, nullptr);
    EXPECT_TRUE(geometry_validate_structure(*geometry));
    EXPECT_EQ(geometry->next_id, expected_next_id);
    EXPECT_EQ(geometry->faces.count(), 3);
    const auto index = TopologyIndex::build(*geometry);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(index->uses_for_edge(swept_edge).size(), 2);
    EXPECT_EQ(index->uses_for_edge(cap_edge).size(), 1);
    check_constructed_boundaries(*geometry, {new_face});
    const NurbsCurveGeometry &arc = *last(geometry->curves);
    EXPECT_EQ(arc.id, arc_id);
    EXPECT_EQ(arc.order, 3);
    EXPECT_EQ(arc.points_num, 3);
    EXPECT_EQ(arc.knots_num, 6);
    EXPECT_EQ(arc.points[0].position[0], 1.0e12 + 1.125);
    EXPECT_EQ(arc.points[1].position[0], 1.0e12 + 1.125);
    EXPECT_EQ(arc.points[2].position[0], 1.0e12 + 0.125);
    EXPECT_EQ(arc.points[1].weight, std::sqrt(0.5));
    for (int i = 0; i < 6; i++) {
      EXPECT_EQ(arc.knots[i], i < 3 ? 2.0 : 5.0);
    }
    EXPECT_EQ(first(geometry->faces)->loops.count(), 2);
    const NurbsTopologyEdgeUse &a =
        *first(first(index->face(original_face)->loops)->edge_uses)->next;
    const NurbsTopologyEdgeUse &b = *last(first(index->face(original_neighbor)->loops)->edge_uses);
    EXPECT_EQ(a.edge_id, b.edge_id);
    EXPECT_NE(a.pcurve_id, b.pcurve_id);
    ASSERT_NE(geometry->stored_selection, nullptr);
    std::string error;
    ASSERT_TRUE(curve_edit_begin(*saved_curve, &error)) << error;
    const EditData &edit = *saved_curve->editnurb->nurbs_edit;
    EXPECT_EQ(edit.geometry().stored_selection, nullptr);
    EXPECT_EQ(edit.selection().vertices.size(), 1);
    EXPECT_EQ(edit.selection().edges.size(), 1);
    EXPECT_EQ(edit.selection().faces.size(), 1);
    EXPECT_TRUE(edit.selection().vertices.contains(first(geometry->vertices)->id));
    EXPECT_TRUE(edit.selection().edges.contains(first(geometry->edges)->id));
    EXPECT_TRUE(edit.selection().faces.contains(first(geometry->faces)->id));
    EXPECT_EQ(edit.selection().active, expected_selection.active);
    ASSERT_EQ(edit.selection().history.size(), 3);
    for (int i = 0; i < 3; i++) {
      EXPECT_EQ(edit.selection().history[i], expected_selection.history[i]);
    }
    BKE_curve_editNurb_free(saved_curve);
  };
  check_loaded(*loaded);
  BLO_blendfiledata_free(loaded);
  loaded = nullptr;

  MemFile memfile{};
  BLI_SCOPED_DEFER([&]() { BLO_memfile_free(&memfile); });
  ASSERT_TRUE(BLO_write_file_mem(bmain, nullptr, &memfile, 0));
  EXPECT_EQ(curve->nurbs_eval_source, runtime_source);
  Vector<char> bytes;
  for (const MemFileChunk &chunk : memfile.chunks) {
    bytes.extend(Span(chunk.buf, chunk.size));
  }
  loaded = BLO_read_from_memory(bytes.data(), int(bytes.size()), BLO_READ_SKIP_USERDEF, &reports);
  ASSERT_NE(loaded, nullptr);
  check_loaded(*loaded);
}

}  // namespace blender::bke::nurbs::tests
