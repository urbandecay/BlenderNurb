/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.hh"

#include "BKE_curve.hh"
#include "BKE_nurbs_geometry.hh"

#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

using ImportedGeometry = std::unique_ptr<NurbsGeometry, decltype(&geometry_free)>;

struct LegacyFixture {
  ListBaseT<Nurb> nurbs = {nullptr, nullptr};

  ~LegacyFixture()
  {
    BKE_nurbList_free(&nurbs);
  }

  Nurb &add(const bool patch)
  {
    Nurb *nurb = MEM_new<Nurb>(__func__);
    nurb->type = CU_NURBS;
    nurb->pntsu = nurb->orderu = nurb->orderv = 2;
    nurb->pntsv = patch ? 2 : 1;
    nurb->flagu = nurb->flagv = CU_NURB_ENDPOINT;
    nurb->bp = MEM_new_array_zeroed<BPoint>(2 * nurb->pntsv, __func__);
    nurb->knotsu = MEM_new_array_uninitialized<float>(4, __func__);
    nurb->knotsu[0] = nurb->knotsu[1] = -2.0f;
    nurb->knotsu[2] = nurb->knotsu[3] = 5.0f;
    if (patch) {
      nurb->knotsv = MEM_dupalloc(nurb->knotsu);
    }
    for (int i = 0; i < 2 * nurb->pntsv; i++) {
      nurb->bp[i].vec[0] = i % 2;
      nurb->bp[i].vec[1] = i / 2;
      nurb->bp[i].vec[3] = 1.0f;
      nurb->bp[i].radius = 1.0f;
      nurb->bp[i].tilt = 0.2f;
      nurb->bp[i].weight = 0.8f;
      nurb->bp[i].f1 = SELECT;
    }
    BLI_addtail(&nurbs, nurb);
    return *nurb;
  }

  Nurb &add_wire(const int first_id = 1)
  {
    Nurb &nurb = add(false);
    nurb.topology_vertex_ids[0] = first_id;
    nurb.topology_vertex_ids[1] = first_id + 1;
    nurb.topology_edge_ids[0] = first_id + 2;
    return nurb;
  }

  Nurb &add_patch(const bool second)
  {
    Nurb &nurb = add(true);
    const int vertex_ids[2][4] = {{1, 2, 5, 4}, {2, 3, 6, 5}};
    const int edge_ids[2][4] = {{10, 20, 30, 40}, {50, 60, 70, 20}};
    const float weights[2][4] = {{1, 2, 3, 5}, {7, 13, 11, 17}};
    std::copy_n(vertex_ids[second], 4, nurb.topology_vertex_ids);
    std::copy_n(edge_ids[second], 4, nurb.topology_edge_ids);
    nurb.topology_face_id = 100 + int(second);
    nurb.mat_nr = second ? 2 : 1;
    for (int i = 0; i < 4; i++) {
      nurb.bp[i].vec[0] += float(second);
      nurb.bp[i].vec[2] = 0.2f * nurb.bp[i].vec[0] * nurb.bp[i].vec[1];
      nurb.bp[i].vec[3] = weights[second][i];
    }
    nurb.knotsu[0] = nurb.knotsu[1] = second ? -7.0f : 2.0f;
    nurb.knotsu[2] = nurb.knotsu[3] = second ? 11.0f : 5.0f;
    nurb.knotsv[0] = nurb.knotsv[1] = second ? -2.0f : -4.0f;
    nurb.knotsv[2] = nurb.knotsv[3] = second ? 7.0f : 6.0f;
    return nurb;
  }
};

static double3 reference_surface(const Nurb &nurb, const double s, const double t)
{
  const double basis[4] = {(1.0 - s) * (1.0 - t), s * (1.0 - t), (1.0 - s) * t, s * t};
  double3 numerator(0.0);
  double denominator = 0.0;
  for (int i = 0; i < 4; i++) {
    const double weight = basis[i] * double(nurb.bp[i].vec[3]);
    numerator += double3(nurb.bp[i].vec[0], nurb.bp[i].vec[1], nurb.bp[i].vec[2]) * weight;
    denominator += weight;
  }
  return numerator / denominator;
}

TEST(nurbs_legacy_import, WirePreservesIDsCoefficientsAndDomain)
{
  LegacyFixture source;
  Nurb &wire = source.add_wire();
  wire.bp[0].vec[0] = 0.1f;
  wire.bp[1].vec[3] = 3.0f;
  const std::array<BPoint, 2> original = {wire.bp[0], wire.bp[1]};
  LegacyGeometryMap map;
  std::string error;
  ImportedGeometry geometry(geometry_from_tagged_legacy(source.nurbs, &map, &error),
                            geometry_free);
  ASSERT_NE(geometry, nullptr) << error;
  const auto index = TopologyIndex::build(*geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(geometry->vertices.count(), 2);
  EXPECT_EQ(geometry->edges.count(), 1);
  EXPECT_EQ(geometry->faces.count(), 0);
  EXPECT_NE(index->vertex(1), nullptr);
  EXPECT_NE(index->vertex(2), nullptr);
  const NurbsTopologyEdge *edge = index->edge(3);
  ASSERT_NE(edge, nullptr);
  const NurbsCurveGeometry &curve = *static_cast<NurbsCurveGeometry *>(geometry->curves.first);
  EXPECT_EQ(edge->curve_id, curve.id);
  EXPECT_EQ(map.curves.lookup(&wire), curve.id);
  EXPECT_GT(curve.id, 3);
  EXPECT_EQ(edge->parameter_range[0], -2.0);
  EXPECT_EQ(edge->parameter_range[1], 5.0);
  EXPECT_EQ(curve.points[0].position[0], double(0.1f));
  EXPECT_EQ(curve.points[1].weight, 3.0);
  for (int i = 0; i <= 16; i++) {
    const double t = i / 16.0;
    const auto point = evaluate_curve(curve, -2.0 + 7.0 * t);
    ASSERT_TRUE(point.has_value());
    const double expected = ((1.0 - t) * double(0.1f) + 3.0 * t) / (1.0 - t + 3.0 * t);
    EXPECT_NEAR(point->position.x, expected, 1.0e-15);
  }
  EXPECT_EQ(std::memcmp(original.data(), wire.bp, sizeof(BPoint) * 2), 0);
}

TEST(nurbs_legacy_import, SharedEdgeUsesExactProjectiveParameterChanges)
{
  LegacyFixture source;
  Nurb &a = source.add_patch(false);
  Nurb &b = source.add_patch(true);
  LegacyGeometryMap map;
  std::string error;
  ImportedGeometry geometry(geometry_from_tagged_legacy(source.nurbs, &map, &error),
                            geometry_free);
  ASSERT_NE(geometry, nullptr) << error;
  const auto index = TopologyIndex::build(*geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(geometry->vertices.count(), 6);
  EXPECT_EQ(geometry->edges.count(), 7);
  EXPECT_EQ(geometry->faces.count(), 2);
  const auto shared = index->uses_for_edge(20);
  ASSERT_EQ(shared.size(), 2);
  EXPECT_NE(shared[0].use->reversed, shared[1].use->reversed);
  EXPECT_NE(shared[0].use->pcurve_id, shared[1].use->pcurve_id);
  EXPECT_EQ(index->face(100)->material_index, 1);
  EXPECT_EQ(index->face(101)->material_index, 2);
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const NurbsCurveGeometry &curve : geometry->curves) {
    curves.add_new(curve.id, &curve);
    EXPECT_GT(curve.id, 101);
  }
  for (const NurbsSurfaceGeometry &surface : geometry->surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  const NurbsCurveGeometry &b_pcurve = *curves.lookup(shared[1].use->pcurve_id);
  EXPECT_EQ(b_pcurve.points[0].weight, 22.0);
  EXPECT_EQ(b_pcurve.points[1].weight, 35.0);
  for (const Nurb *nurb : {&a, &b}) {
    const NurbsSurfaceGeometry &surface = *surfaces.lookup(map.surfaces.lookup(nurb));
    for (int i = 0; i < 4; i++) {
      EXPECT_EQ(surface.points[i].weight, double(nurb->bp[i].vec[3]));
      EXPECT_EQ(surface.knots_u[i], double(nurb->knotsu[i]));
      EXPECT_EQ(surface.knots_v[i], double(nurb->knotsv[i]));
    }
    for (int i = 0; i <= 8; i++) {
      for (int j = 0; j <= 8; j++) {
        const double s = i / 8.0, t = j / 8.0;
        const auto point = evaluate_surface(
            surface,
            {surface.knots_u[1] + s * (surface.knots_u[2] - surface.knots_u[1]),
             surface.knots_v[1] + t * (surface.knots_v[2] - surface.knots_v[1])});
        ASSERT_TRUE(point.has_value());
        EXPECT_NEAR(math::distance(point->position, reference_surface(*nurb, s, t)), 0.0, 1.0e-14);
      }
    }
  }
  for (const NurbsTopologyEdge &edge : geometry->edges) {
    for (int i = 0; i <= 32; i++) {
      const double parameter = edge.parameter_range[0] +
                               (edge.parameter_range[1] - edge.parameter_range[0]) * i / 32.0;
      const auto point = evaluate_curve(*curves.lookup(edge.curve_id), parameter);
      ASSERT_TRUE(point.has_value());
      for (const EdgeUseReference &ref : index->uses_for_edge(edge.id)) {
        const auto uv = evaluate_curve(*curves.lookup(ref.use->pcurve_id), parameter);
        ASSERT_TRUE(uv.has_value());
        const NurbsTopologyFace &face = *index->face(ref.face_id);
        const auto support = evaluate_surface(*surfaces.lookup(face.surface_id),
                                              {uv->position.x, uv->position.y});
        ASSERT_TRUE(support.has_value());
        EXPECT_NEAR(math::distance(point->position, support->position), 0.0, 1.0e-13);
      }
    }
  }
}

TEST(nurbs_legacy_import, CoincidentSourcesDoNotAcquireSharedIdentity)
{
  LegacyFixture source;
  source.add_wire(1);
  source.add_wire(4);
  ImportedGeometry geometry(geometry_from_tagged_legacy(source.nurbs), geometry_free);
  ASSERT_NE(geometry, nullptr);
  EXPECT_EQ(geometry->vertices.count(), 4);
  EXPECT_EQ(geometry->edges.count(), 2);
  const auto index = TopologyIndex::build(*geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->edges_for_vertex(1).size(), 1);
  EXPECT_EQ(index->edges_for_vertex(4).size(), 1);
  EXPECT_NE(index->edges_for_vertex(1)[0], index->edges_for_vertex(4)[0]);
}

TEST(nurbs_legacy_import, ReversedBoundaryAndNegativeWeightsPreserveCorrespondence)
{
  LegacyFixture source;
  source.add_patch(false);
  Nurb &b = source.add_patch(true);
  std::swap(b.bp[0], b.bp[2]);
  std::swap(b.bp[1], b.bp[3]);
  const int vertices[4] = {5, 6, 3, 2};
  const int edges[4] = {70, 60, 50, 20};
  std::copy_n(vertices, 4, b.topology_vertex_ids);
  std::copy_n(edges, 4, b.topology_edge_ids);
  for (int i = 0; i < 4; i++) {
    b.bp[i].vec[3] *= -1.0f;
  }
  LegacyGeometryMap map;
  std::string error;
  ImportedGeometry geometry(geometry_from_tagged_legacy(source.nurbs, &map, &error),
                            geometry_free);
  ASSERT_NE(geometry, nullptr) << error;
  const auto index = TopologyIndex::build(*geometry);
  ASSERT_TRUE(index.has_value());
  const auto shared = index->uses_for_edge(20);
  ASSERT_EQ(shared.size(), 2);
  Map<int64_t, const NurbsCurveGeometry *> curves;
  for (const NurbsCurveGeometry &curve : geometry->curves) {
    curves.add_new(curve.id, &curve);
  }
  const NurbsSurfaceGeometry *surface = nullptr;
  for (const NurbsSurfaceGeometry &candidate : geometry->surfaces) {
    if (candidate.id == map.surfaces.lookup(&b)) {
      surface = &candidate;
    }
  }
  ASSERT_NE(surface, nullptr);
  const NurbsCurveGeometry &pcurve = *curves.lookup(shared[1].use->pcurve_id);
  EXPECT_EQ(pcurve.points[0].position[1], 7.0);
  EXPECT_EQ(pcurve.points[1].position[1], -2.0);
  EXPECT_EQ(pcurve.points[0].weight, -22.0);
  EXPECT_EQ(pcurve.points[1].weight, -35.0);
  const NurbsCurveGeometry &edge_curve = *curves.lookup(index->edge(20)->curve_id);
  for (int i = 0; i <= 32; i++) {
    const double parameter = -4.0 + 10.0 * i / 32.0;
    const auto edge = evaluate_curve(edge_curve, parameter);
    const auto uv = evaluate_curve(pcurve, parameter);
    ASSERT_TRUE(edge.has_value() && uv.has_value());
    const auto support = evaluate_surface(*surface, {uv->position.x, uv->position.y});
    ASSERT_TRUE(support.has_value());
    EXPECT_NEAR(math::distance(edge->position, support->position), 0.0, 1.0e-13);
  }
}

TEST(nurbs_legacy_import, ConflictingIdentityFailsWithoutMutatingInputOrMap)
{
  LegacyFixture source;
  source.add_patch(false);
  Nurb &b = source.add_patch(true);
  b.bp[0].vec[0] = 1.25f;
  const std::array<BPoint, 4> original = {b.bp[0], b.bp[1], b.bp[2], b.bp[3]};
  LegacyGeometryMap map;
  map.curves.add_new(nullptr, 123);
  std::string error;
  ImportedGeometry geometry(geometry_from_tagged_legacy(source.nurbs, &map, &error),
                            geometry_free);
  EXPECT_EQ(geometry, nullptr);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(map.curves.size(), 1);
  EXPECT_EQ(map.curves.lookup(nullptr), 123);
  EXPECT_TRUE(map.surfaces.is_empty());
  EXPECT_EQ(std::memcmp(original.data(), b.bp, sizeof(BPoint) * 4), 0);
}

TEST(nurbs_legacy_import, InvalidAndUnsupportedSourcesAreRejectedAtomically)
{
  LegacyFixture source;
  Nurb &a = source.add_patch(false);
  Nurb &b = source.add_patch(true);
  auto rejected = [&]() {
    std::string error;
    ImportedGeometry geometry(geometry_from_tagged_legacy(source.nurbs, nullptr, &error),
                              geometry_free);
    EXPECT_EQ(geometry, nullptr);
    EXPECT_FALSE(error.empty());
  };
  b.topology_face_id = a.topology_face_id;
  rejected();
  b.topology_face_id = 101;
  b.topology_edge_ids[0] = 20;
  rejected();
  b.topology_edge_ids[0] = 50;
  b.topology_edge_ids[0] = b.topology_vertex_ids[0];
  rejected();
  b.topology_edge_ids[0] = 0;
  rejected();
  b.topology_edge_ids[0] = 50;
  b.bp[0].vec[3] = -7.0f;
  rejected();
  b.bp[0].vec[3] = 7.0f;
  b.knotsu[1] = std::numeric_limits<float>::quiet_NaN();
  rejected();
  b.knotsu[1] = -7.0f;
  b.orderu = 3;
  rejected();
  b.orderu = 2;
  b.flagu = CU_NURB_CYCLIC;
  rejected();
}

TEST(nurbs_legacy_import, DuplicateWireIdentityDoesNotSilentlyMerge)
{
  LegacyFixture source;
  Nurb &wire = source.add_wire();
  Nurb *copy = BKE_nurb_copy(&wire, wire.pntsu, wire.pntsv);
  BLI_addtail(&source.nurbs, copy);
  std::string error;
  ImportedGeometry geometry(geometry_from_tagged_legacy(source.nurbs, nullptr, &error),
                            geometry_free);
  EXPECT_EQ(geometry, nullptr);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source.nurbs.count(), 2);
  EXPECT_EQ(wire.topology_edge_ids[0], copy->topology_edge_ids[0]);
}

}  // namespace blender::bke::nurbs::tests
