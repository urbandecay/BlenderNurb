/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "BKE_curve.hh"
#include "BKE_curve_topology.hh"
#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_legacy_edit.hh"
#include "BKE_nurbs_topology.hh"

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

TEST(nurbs_legacy_edit, DuplicateWireOwnsFreshIDsAndPreservesSourceAndKnots)
{
  LegacyFixture source;
  Nurb &wire = source.add_wire();
  const std::array<BPoint, 2> original = {wire.bp[0], wire.bp[1]};
  std::string error;
  auto edit = legacy_topology_duplicate(source.nurbs, &error);
  ASSERT_TRUE(edit.has_value()) << error;
  ASSERT_EQ(edit->added.count(), 1);
  const auto &copy = *static_cast<const Nurb *>(edit->added.first);
  EXPECT_NE(copy.topology_vertex_ids[0], wire.topology_vertex_ids[0]);
  EXPECT_NE(copy.topology_edge_ids[0], wire.topology_edge_ids[0]);
  EXPECT_EQ(std::memcmp(copy.bp, wire.bp, 2 * sizeof(BPoint)), 0);
  EXPECT_EQ(std::memcmp(copy.knotsu, wire.knotsu, 4 * sizeof(float)), 0);
  EXPECT_EQ(std::memcmp(original.data(), wire.bp, 2 * sizeof(BPoint)), 0);
  EXPECT_TRUE(edit->removed_faces.is_empty());
  EXPECT_TRUE(edit->removed_wires.is_empty());
}

TEST(nurbs_legacy_edit, DuplicateSharedFacesPreservesInternalSharingAndIndependentSource)
{
  LegacyFixture source;
  Nurb &a = source.add_patch(false);
  Nurb &b = source.add_patch(true);
  std::string error;
  auto edit = legacy_topology_duplicate(source.nurbs, &error);
  ASSERT_TRUE(edit.has_value()) << error;
  ASSERT_EQ(edit->added.count(), 2);
  const auto &copy_a = *static_cast<const Nurb *>(edit->added.first);
  const auto &copy_b = *static_cast<const Nurb *>(edit->added.last);
  EXPECT_EQ(copy_a.topology_edge_ids[1], copy_b.topology_edge_ids[3]);
  EXPECT_EQ(copy_a.topology_vertex_ids[1], copy_b.topology_vertex_ids[0]);
  EXPECT_NE(copy_a.topology_vertex_ids[1], a.topology_vertex_ids[1]);
  EXPECT_NE(copy_a.topology_face_id, a.topology_face_id);
  EXPECT_NE(copy_b.topology_face_id, b.topology_face_id);
  for (const auto &pair : {std::pair(&a, &copy_a), std::pair(&b, &copy_b)}) {
    EXPECT_EQ(std::memcmp(pair.first->bp, pair.second->bp, 4 * sizeof(BPoint)), 0);
    EXPECT_EQ(std::memcmp(pair.first->knotsu, pair.second->knotsu, 4 * sizeof(float)), 0);
    EXPECT_EQ(std::memcmp(pair.first->knotsv, pair.second->knotsv, 4 * sizeof(float)), 0);
    EXPECT_EQ(pair.first->mat_nr, pair.second->mat_nr);
  }
}

TEST(nurbs_legacy_edit, TwoWireFillPreservesSharpCornersDomainsAndEditingTopology)
{
  LegacyFixture source;
  Nurb &a = source.add_wire(1);
  Nurb &b = source.add_wire(4);
  b.bp[0].vec[1] = b.bp[1].vec[1] = 2;
  const auto a_before = std::array<BPoint, 2>{a.bp[0], a.bp[1]};
  std::string error;
  auto filled = legacy_topology_fill(source.nurbs, &error);
  ASSERT_TRUE(filled.has_value()) << error;
  ASSERT_EQ(filled->added.count(), 1);
  EXPECT_TRUE(filled->removed_wires.contains(3));
  EXPECT_TRUE(filled->removed_wires.contains(6));
  EXPECT_EQ(source.nurbs.count(), 2);
  EXPECT_EQ(std::memcmp(a.bp, a_before.data(), sizeof(BPoint) * 2), 0);
  auto &patch = *static_cast<Nurb *>(filled->added.first);
  EXPECT_EQ(patch.pntsu, 2);
  EXPECT_EQ(patch.pntsv, 2);
  EXPECT_EQ(patch.orderu, 2);
  EXPECT_EQ(patch.orderv, 2);
  EXPECT_EQ(patch.flagu, CU_NURB_ENDPOINT);
  EXPECT_EQ(patch.flagv, CU_NURB_ENDPOINT);
  EXPECT_EQ(Span(patch.knotsu, 4), Span(a.knotsu, 4));
  EXPECT_EQ(Set<int>(Span(patch.topology_vertex_ids, 4)), Set<int>({1, 2, 4, 5}));
  ImportedGeometry geometry(geometry_from_tagged_legacy(filled->added, nullptr, &error),
                            geometry_free);
  ASSERT_NE(geometry, nullptr) << error;
  EXPECT_EQ(geometry->vertices.count(), 4);
  EXPECT_EQ(geometry->edges.count(), 4);
  EXPECT_EQ(geometry->faces.count(), 1);
  EXPECT_TRUE(legacy_topology_duplicate(filled->added, &error).has_value()) << error;
  auto swept = legacy_topology_extrude(filled->added, &error);
  ASSERT_TRUE(swept.has_value()) << error;
  EXPECT_EQ(swept->added.count(), 5);
  EXPECT_EQ(patch.pntsu, 2);
  EXPECT_EQ(patch.pntsv, 2);
  auto noop = legacy_topology_fill(filled->added, &error);
  ASSERT_TRUE(noop.has_value()) << error;
  EXPECT_TRUE(noop->added.is_empty());
}

TEST(nurbs_legacy_edit, FillRejectsUnrepresentableWeightsDomainsAndMalformedTagsAtomically)
{
  LegacyFixture source;
  Nurb &a = source.add_wire(1);
  Nurb &b = source.add_wire(4);
  b.bp[0].vec[1] = b.bp[1].vec[1] = 2;
  std::string error;
  b.bp[1].vec[3] = 2;
  EXPECT_FALSE(legacy_topology_fill(source.nurbs, &error).has_value());
  EXPECT_NE(error.find("weights"), std::string::npos);
  EXPECT_EQ(b.bp[1].vec[3], 2);
  b.bp[1].vec[3] = 1;
  b.knotsu[2] = b.knotsu[3] = 6;
  EXPECT_FALSE(legacy_topology_fill(source.nurbs, &error).has_value());
  EXPECT_NE(error.find("domains"), std::string::npos);
  EXPECT_EQ(b.knotsu[2], 6);
  b.knotsu[2] = b.knotsu[3] = 5;
  a.flagu |= CU_NURB_CYCLIC;
  EXPECT_FALSE(legacy_topology_fill(source.nurbs, &error).has_value());
  EXPECT_TRUE(a.flagu & CU_NURB_CYCLIC);
  EXPECT_EQ(source.nurbs.count(), 2);
  a.flagu &= ~CU_NURB_CYCLIC;
  a.bp[0].vec[0] = a.bp[0].vec[1] = 0;
  a.bp[1].vec[0] = 1;
  a.bp[1].vec[1] = 0;
  b.bp[0].vec[0] = 0;
  b.bp[0].vec[1] = 1;
  b.bp[1].vec[0] = b.bp[1].vec[1] = 0.25f;
  EXPECT_FALSE(legacy_topology_fill(source.nurbs, &error).has_value());
  EXPECT_NE(error.find("trimmed-face"), std::string::npos);
  EXPECT_EQ(source.nurbs.count(), 2);
}

TEST(nurbs_legacy_edit, RegionExtrusionMakesOnlyOuterWallsAndKeepsSourceUntouched)
{
  LegacyFixture source;
  source.add_patch(false);
  source.add_patch(true);
  std::string error;
  auto edit = legacy_topology_extrude(source.nurbs, &error);
  ASSERT_TRUE(edit.has_value()) << error;
  EXPECT_EQ(edit->added.count(), 8); /* Two caps, six boundary walls. */
  EXPECT_TRUE(edit->removed_faces.is_empty());
  EXPECT_EQ(source.nurbs.count(), 2);
  Set<int> original_vertices;
  for (const Nurb &nurb : source.nurbs) {
    for (int i = 0; i < 4; i++) {
      EXPECT_TRUE(nurb.bp[i].f1 & SELECT);
      original_vertices.add(nurb.topology_vertex_ids[i]);
    }
  }
  constexpr int corners[4] = {0, 1, 3, 2};
  int walls = 0;
  for (Nurb &nurb : edit->added) {
    if (nurb.bp[0].f1 & SELECT) {
      continue; /* Cap. */
    }
    for (int i = 0; i < 4; i++) {
      if (!original_vertices.contains(nurb.topology_vertex_ids[i])) {
        nurb.bp[corners[i]].vec[2] += 1;
      }
    }
    const float3 normal = math::cross(float3(nurb.bp[1].vec) - float3(nurb.bp[0].vec),
                                      float3(nurb.bp[2].vec) - float3(nurb.bp[0].vec));
    const float2 middle = (float2(nurb.bp[0].vec) + float2(nurb.bp[1].vec) +
                           float2(nurb.bp[2].vec) + float2(nurb.bp[3].vec)) *
                          0.25f;
    EXPECT_GT(math::dot(float2(normal.x, normal.y), middle - float2(1, 0.5f)), 0);
    walls++;
  }
  EXPECT_EQ(walls, 6);
}

TEST(nurbs_legacy_edit, AttachedFaceRegionReplacesInputCap)
{
  LegacyFixture source;
  Nurb &a = source.add_patch(false);
  Nurb &b = source.add_patch(true);
  for (int i = 0; i < 4; i++) {
    b.bp[i].f1 = 0;
  }
  std::string error;
  auto edit = legacy_topology_extrude(source.nurbs, &error);
  ASSERT_TRUE(edit.has_value()) << error;
  ASSERT_EQ(edit->removed_faces.size(), 1);
  EXPECT_EQ(edit->removed_faces[0], a.topology_face_id);
  EXPECT_EQ(edit->added.count(), 5); /* One cap and four walls. */
  EXPECT_EQ(source.nurbs.count(), 2);
}

TEST(nurbs_legacy_edit, SharedEdgeSelectionUsesOneIdentityRegardlessOfSelectedCopies)
{
  LegacyFixture source;
  Nurb &a = source.add_patch(false);
  Nurb &b = source.add_patch(true);
  for (Nurb &nurb : source.nurbs) {
    for (int i = 0; i < 4; i++) {
      nurb.bp[i].f1 = 0;
    }
  }
  a.bp[1].f1 = a.bp[3].f1 = SELECT;
  auto single = legacy_topology_extrude(source.nurbs);
  ASSERT_TRUE(single.has_value());
  EXPECT_EQ(single->added.count(), 1);
  b.bp[0].f1 = b.bp[2].f1 = SELECT;
  auto both = legacy_topology_extrude(source.nurbs);
  ASSERT_TRUE(both.has_value());
  EXPECT_EQ(both->added.count(), 1);
  const auto &first = *static_cast<const Nurb *>(single->added.first);
  const auto &second = *static_cast<const Nurb *>(both->added.first);
  EXPECT_EQ(std::memcmp(first.topology_vertex_ids, second.topology_vertex_ids, 4 * sizeof(int)),
            0);
  EXPECT_EQ(std::memcmp(first.topology_edge_ids, second.topology_edge_ids, 4 * sizeof(int)), 0);
  EXPECT_EQ(std::memcmp(first.bp, second.bp, 4 * sizeof(BPoint)), 0);
}

TEST(nurbs_legacy_edit, LooseVertexDuplicationAndInvalidIdentityAreAtomic)
{
  LegacyFixture source;
  Nurb &wire = source.add_wire();
  wire.bp[1].f1 = 0;
  const std::array<BPoint, 2> original = {wire.bp[0], wire.bp[1]};
  std::string error;
  auto copy = legacy_topology_duplicate(source.nurbs, &error);
  ASSERT_TRUE(copy.has_value()) << error;
  ASSERT_EQ(copy->added.count(), 1);
  const auto &point = *static_cast<const Nurb *>(copy->added.first);
  EXPECT_EQ(point.pntsu, 1);
  EXPECT_EQ(point.pntsv, 1);
  EXPECT_EQ(point.topology_edge_ids[0], 0);
  EXPECT_NE(point.topology_vertex_ids[0], wire.topology_vertex_ids[0]);
  EXPECT_EQ(std::memcmp(original.data(), wire.bp, 2 * sizeof(BPoint)), 0);
  wire.topology_vertex_ids[0] = 0;
  EXPECT_FALSE(legacy_topology_extrude(source.nurbs, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(std::memcmp(original.data(), wire.bp, 2 * sizeof(BPoint)), 0);
  EXPECT_EQ(source.nurbs.count(), 1);
}

TEST(nurbs_legacy_edit, SharedCornerPositionSyncPreservesEachSupportsRationalWeights)
{
  LegacyFixture source;
  Nurb &a = source.add_patch(false);
  Nurb &b = source.add_patch(true);
  for (int i = 0; i < 4; i++) {
    b.bp[i].f1 = 0;
  }
  const std::array<float, 4> weights = {
      b.bp[0].vec[3], b.bp[1].vec[3], b.bp[2].vec[3], b.bp[3].vec[3]};
  a.bp[1].vec[2] = 4;
  BKE_curve_nurbs_topology_position_sync(&source.nurbs);
  EXPECT_EQ(b.bp[0].vec[2], 4);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(b.bp[i].vec[3], weights[i]);
  }
}

TEST(nurbs_legacy_edit, VertexDeletionKeepsSurvivingEdgeAsOneValidWire)
{
  LegacyFixture source;
  Nurb &face = source.add_patch(false);
  face.bp[0].f1 = face.bp[1].f1 = 0;
  std::string error;
  auto edit = legacy_topology_delete(source.nurbs, DeleteMode::Vertices, &error);
  ASSERT_TRUE(edit.has_value()) << error;
  ASSERT_EQ(edit->removed_faces.size(), 1);
  EXPECT_EQ(edit->removed_faces[0], face.topology_face_id);
  ASSERT_EQ(edit->added.count(), 1);
  const auto &wire = *static_cast<const Nurb *>(edit->added.first);
  EXPECT_EQ(wire.pntsv, 1);
  EXPECT_EQ(wire.topology_face_id, 0);
  EXPECT_EQ(wire.topology_edge_ids[0], face.topology_edge_ids[0]);
  EXPECT_EQ(wire.topology_edge_ids[1], 0);
  EXPECT_EQ(wire.topology_vertex_ids[2], 0);
  OwnedGeometry geometry(geometry_from_tagged_legacy(edit->added, nullptr, &error));
  ASSERT_NE(geometry, nullptr) << error;
  EXPECT_EQ(geometry->vertices.count(), 2);
  EXPECT_EQ(geometry->edges.count(), 1);
  EXPECT_TRUE(geometry->faces.is_empty());
  EXPECT_EQ(source.nurbs.count(), 1);
  EXPECT_EQ(face.pntsv, 2);
}

TEST(nurbs_legacy_edit, VertexDeletionPreservesDisconnectedLoosePoints)
{
  LegacyFixture source;
  Nurb &face = source.add_patch(false);
  face.bp[1].f1 = face.bp[2].f1 = 0;
  std::string error;
  auto edit = legacy_topology_delete(source.nurbs, DeleteMode::Vertices, &error);
  ASSERT_TRUE(edit.has_value()) << error;
  ASSERT_EQ(edit->added.count(), 2);
  for (const Nurb &point : edit->added) {
    EXPECT_EQ(point.pntsu, 1);
    EXPECT_EQ(point.pntsv, 1);
    EXPECT_EQ(point.topology_edge_ids[0], 0);
    EXPECT_EQ(point.topology_face_id, 0);
  }
  OwnedGeometry geometry(geometry_from_tagged_legacy(edit->added, nullptr, &error));
  ASSERT_NE(geometry, nullptr) << error;
  EXPECT_EQ(geometry->vertices.count(), 2);
  EXPECT_TRUE(geometry->edges.is_empty());
  EXPECT_TRUE(geometry->faces.is_empty());
  auto *point = static_cast<Nurb *>(edit->added.first);
  point->bp[0].f1 |= SELECT;
  auto extend = legacy_topology_extrude(edit->added, &error);
  ASSERT_TRUE(extend.has_value()) << error;
  EXPECT_EQ(extend->added.count(), 1);
  ASSERT_EQ(extend->removed_points.size(), 1);
  EXPECT_EQ(extend->removed_points[0], point->topology_vertex_ids[0]);
  point->bp[0].vec[3] = 0;
  EXPECT_FALSE(legacy_topology_extrude(edit->added, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(point->bp[0].vec[3], 0);
}

TEST(nurbs_legacy_edit, FacesOnlyDeletionRetainsTheSharedBoundarySource)
{
  LegacyFixture source;
  Nurb &face = source.add_patch(false);
  std::string error;
  auto edit = legacy_topology_delete(source.nurbs, DeleteMode::FacesOnly, &error);
  ASSERT_TRUE(edit.has_value()) << error;
  ASSERT_EQ(edit->removed_faces.size(), 1);
  EXPECT_EQ(edit->removed_faces[0], face.topology_face_id);
  EXPECT_EQ(edit->added.count(), 4);
  OwnedGeometry geometry(geometry_from_tagged_legacy(edit->added, nullptr, &error));
  ASSERT_NE(geometry, nullptr) << error;
  EXPECT_EQ(geometry->vertices.count(), 4);
  EXPECT_EQ(geometry->edges.count(), 4);
  EXPECT_TRUE(geometry->faces.is_empty());
}

static Nurb &add_collapsed_patch_alias(LegacyFixture &source, const Nurb &face)
{
  Nurb &alias = source.add_wire();
  alias.topology_face_id = 200;
  const int vertices[4] = {face.topology_vertex_ids[0], face.topology_vertex_ids[1], 501, 502};
  const int edges[4] = {face.topology_edge_ids[0], 503, 504, 505};
  std::copy_n(vertices, 4, alias.topology_vertex_ids);
  std::copy_n(edges, 4, alias.topology_edge_ids);
  alias.bp[0] = face.bp[0];
  alias.bp[1] = face.bp[1];
  std::copy_n(face.knotsu, 4, alias.knotsu);
  alias.mat_nr = face.mat_nr;
  alias.flag = face.flag;
  return alias;
}

TEST(nurbs_legacy_edit, KnownCollapsedPatchAliasRecoversOnlyItsDeclaredExistingEdge)
{
  LegacyFixture source;
  Nurb &face = source.add_patch(false);
  Nurb &alias = add_collapsed_patch_alias(source, face);
  std::string error;
  /* Keep the low-level importer strict; recovery belongs to the checked compatibility adapter. */
  OwnedGeometry invalid(geometry_from_tagged_legacy(source.nurbs, nullptr, &error));
  EXPECT_EQ(invalid, nullptr);
  auto edit = legacy_topology_extrude(source.nurbs, &error);
  ASSERT_TRUE(edit.has_value()) << error;
  EXPECT_EQ(edit->recovered_patch_remnants, 1);
  EXPECT_TRUE(edit->removed_faces.contains(alias.topology_face_id));
  EXPECT_EQ(edit->added.count(), 5);
  const auto &cap = *static_cast<const Nurb *>(edit->added.first);
  EXPECT_GT(cap.topology_face_id, 505);
  EXPECT_EQ(source.nurbs.count(), 2);
  EXPECT_EQ(alias.pntsv, 1);
  EXPECT_EQ(alias.topology_face_id, 200);
}

TEST(nurbs_legacy_edit, CoincidenceWithoutDeclaredIdentityDoesNotRecoverMalformedPatch)
{
  LegacyFixture source;
  Nurb &face = source.add_patch(false);
  Nurb &alias = add_collapsed_patch_alias(source, face);
  alias.topology_edge_ids[0] = 999;
  std::string error;
  EXPECT_FALSE(legacy_topology_extrude(source.nurbs, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source.nurbs.count(), 2);
  EXPECT_EQ(alias.topology_face_id, 200);
  EXPECT_EQ(alias.topology_edge_ids[0], 999);
  alias.topology_edge_ids[0] = face.topology_edge_ids[0];
  alias.topology_vertex_ids[0] = 1234;
  EXPECT_FALSE(legacy_topology_extrude(source.nurbs, &error).has_value());
  EXPECT_FALSE(error.empty());
  alias.topology_vertex_ids[0] = face.topology_vertex_ids[0];
  /* Periodic source geometry must not be discarded as a collapsed open patch alias. */
  alias.topology_edge_ids[0] = face.topology_edge_ids[0];
  alias.flagu |= CU_NURB_CYCLIC;
  MEM_delete(alias.knotsu);
  alias.knotsu = MEM_new_array_uninitialized<float>(5, __func__);
  std::copy_n(face.knotsu, 4, alias.knotsu);
  alias.knotsu[4] = face.knotsu[3] + 1;
  EXPECT_FALSE(legacy_topology_extrude(source.nurbs, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source.nurbs.count(), 2);
}

TEST(nurbs_legacy_edit, AmbiguousCollapsedPatchDoesNotGuessACanonicalIdentity)
{
  LegacyFixture source;
  Nurb &face = source.add_patch(false);
  for (int i = 0; i < 4; i++) {
    std::fill_n(face.bp[i].vec, 3, 0);
    face.bp[i].vec[3] = 1;
  }
  Nurb &alias = add_collapsed_patch_alias(source, face);
  alias.topology_edge_ids[1] = face.topology_edge_ids[1];
  alias.topology_vertex_ids[2] = face.topology_vertex_ids[2];
  std::string error;
  EXPECT_FALSE(legacy_topology_extrude(source.nurbs, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(source.nurbs.count(), 2);
}

}  // namespace blender::bke::nurbs::tests
