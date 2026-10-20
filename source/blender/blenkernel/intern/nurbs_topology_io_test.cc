/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <memory>
#include <string>

#include "BKE_appdir.hh"
#include "BKE_blender.hh"
#include "BKE_blendfile.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_report.hh"

#include "BLI_fileops.hh"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_vector.hh"

#include "BLO_readfile.hh"

#include "testing/testing.h"

#include "nurbs_topology_test_utils.hh"

namespace blender::bke::tests {

class NurbsTopologyIOTest : public BlenderGTestBase {};

static void expect_control_vertex_equal(const NurbsControlVertex &a, const NurbsControlVertex &b)
{
  EXPECT_EQ(a.id, b.id);
  for (int axis = 0; axis < 3; axis++) {
    EXPECT_DOUBLE_EQ(a.position[axis], b.position[axis]);
  }
  EXPECT_DOUBLE_EQ(a.weight, b.weight);
  EXPECT_EQ(a.flag, b.flag);
}

static void expect_curve_equal(const NurbsCurve &a, const NurbsCurve &b)
{
  EXPECT_EQ(a.id, b.id);
  EXPECT_EQ(a.degree, b.degree);
  EXPECT_EQ(a.order, b.order);
  EXPECT_EQ(a.dimension, b.dimension);
  EXPECT_EQ(a.control_vertex_count, b.control_vertex_count);
  EXPECT_EQ(a.knot_count, b.knot_count);
  EXPECT_EQ(a.rational, b.rational);
  for (int i = 0; i < 2; i++) {
    EXPECT_DOUBLE_EQ(a.domain[i], b.domain[i]);
  }
  ASSERT_EQ(a.control_vertex_count, b.control_vertex_count);
  for (int i = 0; i < a.control_vertex_count; i++) {
    expect_control_vertex_equal(a.control_vertices[i], b.control_vertices[i]);
  }
  ASSERT_EQ(a.knot_count, b.knot_count);
  for (int i = 0; i < a.knot_count; i++) {
    EXPECT_DOUBLE_EQ(a.knots[i], b.knots[i]);
  }
}

static void expect_surface_equal(const NurbsSurface &a, const NurbsSurface &b)
{
  EXPECT_EQ(a.id, b.id);
  EXPECT_EQ(a.degree_u, b.degree_u);
  EXPECT_EQ(a.degree_v, b.degree_v);
  EXPECT_EQ(a.order_u, b.order_u);
  EXPECT_EQ(a.order_v, b.order_v);
  EXPECT_EQ(a.control_vertex_count_u, b.control_vertex_count_u);
  EXPECT_EQ(a.control_vertex_count_v, b.control_vertex_count_v);
  EXPECT_EQ(a.knot_count_u, b.knot_count_u);
  EXPECT_EQ(a.knot_count_v, b.knot_count_v);
  EXPECT_EQ(a.dimension, b.dimension);
  EXPECT_EQ(a.rational, b.rational);
  for (int i = 0; i < 2; i++) {
    EXPECT_DOUBLE_EQ(a.domain_u[i], b.domain_u[i]);
    EXPECT_DOUBLE_EQ(a.domain_v[i], b.domain_v[i]);
  }
  const int cv_count = a.control_vertex_count_u * a.control_vertex_count_v;
  ASSERT_EQ(cv_count, b.control_vertex_count_u * b.control_vertex_count_v);
  for (int i = 0; i < cv_count; i++) {
    expect_control_vertex_equal(a.control_vertices[i], b.control_vertices[i]);
  }
  ASSERT_EQ(a.knot_count_u, b.knot_count_u);
  for (int i = 0; i < a.knot_count_u; i++) {
    EXPECT_DOUBLE_EQ(a.knots_u[i], b.knots_u[i]);
  }
  ASSERT_EQ(a.knot_count_v, b.knot_count_v);
  for (int i = 0; i < a.knot_count_v; i++) {
    EXPECT_DOUBLE_EQ(a.knots_v[i], b.knots_v[i]);
  }
}

static void expect_topology_equal(const NurbsTopology &a, const NurbsTopology &b)
{
  EXPECT_DOUBLE_EQ(a.tolerance, b.tolerance);
  EXPECT_DOUBLE_EQ(a.parameter_tolerance, b.parameter_tolerance);
  EXPECT_EQ(a.active_component_id, b.active_component_id);

  ASSERT_EQ(BLI_listbase_count(&a.surfaces), BLI_listbase_count(&b.surfaces));
  auto surface_b = b.surfaces.begin();
  for (const NurbsSurface &surface_a : a.surfaces) {
    ASSERT_NE(surface_b, b.surfaces.end());
    expect_surface_equal(surface_a, *surface_b++);
  }

  ASSERT_EQ(BLI_listbase_count(&a.trim_curves), BLI_listbase_count(&b.trim_curves));
  auto trim_b = b.trim_curves.begin();
  for (const NurbsCurve &trim_a : a.trim_curves) {
    ASSERT_NE(trim_b, b.trim_curves.end());
    expect_curve_equal(trim_a, *trim_b++);
  }

  ASSERT_EQ(BLI_listbase_count(&a.vertices), BLI_listbase_count(&b.vertices));
  auto vertex_b = b.vertices.begin();
  for (const NurbsTopoVertex &vertex_a : a.vertices) {
    ASSERT_NE(vertex_b, b.vertices.end());
    const NurbsTopoVertex &vertex = *vertex_b++;
    EXPECT_EQ(vertex_a.id, vertex.id);
    for (int axis = 0; axis < 3; axis++) {
      EXPECT_DOUBLE_EQ(vertex_a.position[axis], vertex.position[axis]);
    }
    EXPECT_EQ(vertex_a.flag, vertex.flag);
  }

  ASSERT_EQ(BLI_listbase_count(&a.edges), BLI_listbase_count(&b.edges));
  auto edge_b = b.edges.begin();
  for (const NurbsTopoEdge &edge_a : a.edges) {
    ASSERT_NE(edge_b, b.edges.end());
    const NurbsTopoEdge &edge = *edge_b++;
    EXPECT_EQ(edge_a.id, edge.id);
    EXPECT_EQ(edge_a.vertex_ids[0], edge.vertex_ids[0]);
    EXPECT_EQ(edge_a.vertex_ids[1], edge.vertex_ids[1]);
    EXPECT_EQ(edge_a.curve_id, edge.curve_id);
    EXPECT_DOUBLE_EQ(edge_a.domain[0], edge.domain[0]);
    EXPECT_DOUBLE_EQ(edge_a.domain[1], edge.domain[1]);
    EXPECT_EQ(edge_a.flag, edge.flag);
  }

  ASSERT_EQ(BLI_listbase_count(&a.faces), BLI_listbase_count(&b.faces));
  auto face_b = b.faces.begin();
  for (const NurbsTopoFace &face_a : a.faces) {
    ASSERT_NE(face_b, b.faces.end());
    const NurbsTopoFace &face = *face_b++;
    EXPECT_EQ(face_a.id, face.id);
    EXPECT_EQ(face_a.surface_id, face.surface_id);
    EXPECT_EQ(face_a.loop_count, face.loop_count);
    EXPECT_EQ(face_a.flag, face.flag);
    EXPECT_EQ(face_a.reversed, face.reversed);
    ASSERT_EQ(face_a.loop_count, face.loop_count);
    for (int loop_index = 0; loop_index < face_a.loop_count; loop_index++) {
      const NurbsTrimLoop &loop_a = face_a.loops[loop_index];
      const NurbsTrimLoop &loop = face.loops[loop_index];
      EXPECT_EQ(loop_a.id, loop.id);
      EXPECT_EQ(loop_a.role, loop.role);
      EXPECT_EQ(loop_a.use_count, loop.use_count);
      ASSERT_EQ(loop_a.use_count, loop.use_count);
      for (int use_index = 0; use_index < loop_a.use_count; use_index++) {
        const NurbsEdgeUse &use_a = loop_a.uses[use_index];
        const NurbsEdgeUse &use = loop.uses[use_index];
        EXPECT_EQ(use_a.id, use.id);
        EXPECT_EQ(use_a.edge_id, use.edge_id);
        EXPECT_EQ(use_a.trim_curve_id, use.trim_curve_id);
        EXPECT_EQ(use_a.reversed, use.reversed);
      }
    }
  }

  ASSERT_EQ(BLI_listbase_count(&a.selection_history), BLI_listbase_count(&b.selection_history));
  auto selection_b = b.selection_history.begin();
  for (const NurbsSelection &selection_a : a.selection_history) {
    ASSERT_NE(selection_b, b.selection_history.end());
    const NurbsSelection &selection = *selection_b++;
    EXPECT_EQ(selection_a.component_id, selection.component_id);
    EXPECT_EQ(selection_a.type, selection.type);
  }
}

static void collect_geometry_ids(const NurbsGeometry &geometry, Vector<uint64_t> &r_ids)
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
  const NurbsTopology &topology = *geometry.topology;
  for (const NurbsSurface &surface : topology.surfaces) {
    r_ids.append(surface.id);
    for (int i = 0; i < surface.control_vertex_count_u * surface.control_vertex_count_v; i++) {
      r_ids.append(surface.control_vertices[i].id);
    }
  }
  for (const NurbsCurve &curve : topology.trim_curves) {
    r_ids.append(curve.id);
    for (int i = 0; i < curve.control_vertex_count; i++) {
      r_ids.append(curve.control_vertices[i].id);
    }
  }
  for (const NurbsTopoVertex &vertex : topology.vertices) {
    r_ids.append(vertex.id);
  }
  for (const NurbsTopoEdge &edge : topology.edges) {
    r_ids.append(edge.id);
  }
  for (const NurbsTopoFace &face : topology.faces) {
    r_ids.append(face.id);
    for (int i = 0; i < face.loop_count; i++) {
      r_ids.append(face.loops[i].id);
      for (int j = 0; j < face.loops[i].use_count; j++) {
        r_ids.append(face.loops[i].uses[j].id);
      }
    }
  }
}

struct ScopedMain {
  Main *value = BKE_main_new();
  ~ScopedMain()
  {
    BKE_main_free(value);
  }
};

struct ScopedBlenderGlobals {
  ScopedBlenderGlobals()
  {
    BKE_blender_globals_init();
  }
  ~ScopedBlenderGlobals()
  {
    BKE_blender_globals_clear();
  }
};

struct ScopedReports {
  ReportList value = {};
  ScopedReports()
  {
    BKE_reports_init(&value, RPT_STORE);
  }
  ~ScopedReports()
  {
    BKE_reports_free(&value);
  }
};

struct ScopedTempDir {
  bool owns_session = false;

  ScopedTempDir()
  {
    if (BKE_tempdir_session()[0] == '\0') {
      BKE_tempdir_init(nullptr);
      owns_session = true;
    }
  }

  ~ScopedTempDir()
  {
    if (owns_session) {
      BKE_tempdir_session_purge();
    }
  }
};

struct ScopedFile {
  char path[FILE_MAX] = {};
  ~ScopedFile()
  {
    if (path[0] != '\0') {
      BLI_delete(path, false, false);
    }
  }
};

TEST_F(NurbsTopologyIOTest, BlendfileRoundTripRepairsStaleAllocatorAfterSharedEdgeSplit)
{
  NurbsTopologyFixture fixture;
  std::string error;
  ASSERT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;

  NurbsEdgeSplitResult split_result;
  uint64_t next_component_id = fixture.next_component_id;
  ASSERT_TRUE(BKE_nurbs_topology_edge_split(fixture.curves,
                                            *fixture.topology,
                                            next_component_id,
                                            fixture.shared_edge_id,
                                            0.5,
                                            split_result,
                                            &error))
      << error;
  const uint64_t selected_vertex_id =
      static_cast<const NurbsTopoVertex *>(fixture.topology->vertices.first)->id;
  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_FACE, fixture.face_a_id, true));
  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_VERTEX, selected_vertex_id, true));
  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_EDGE, split_result.child_edge_ids[0], true));
  ASSERT_TRUE(BKE_nurbs_topology_selection_set(
      *fixture.topology, NURBS_TOPO_SELECT_EDGE, split_result.child_edge_ids[1], true));
  ASSERT_TRUE(BKE_nurbs_topology_is_valid(fixture.curves, *fixture.topology, &error)) << error;

  ScopedMain source_main;
  NurbsGeometry *source = BKE_id_new<NurbsGeometry>(source_main.value, "Topology Roundtrip");
  BKE_nurbs_curve_list_copy(source->curves, fixture.curves);
  source->topology = BKE_nurbs_topology_copy(*fixture.topology);
  const uint64_t expected_next_component_id = BKE_nurbs_component_id_next(source->curves,
                                                                          source->topology);
  ASSERT_GT(expected_next_component_id, 1);
  source->next_component_id = 1;

  ScopedTempDir temp_dir;
  ScopedFile file;
  BLI_path_join(
      file.path, sizeof(file.path), BKE_tempdir_session(), "nurbs_topology_roundtrip.blend");
  ScopedReports reports;
  {
    bke::blendfile::PartialWriteContext write_context(*source_main.value);
    const auto operations = bke::blendfile::PartialWriteContext::MAKE_LOCAL |
                            bke::blendfile::PartialWriteContext::SET_FAKE_USER |
                            bke::blendfile::PartialWriteContext::CLEAR_DEPENDENCIES;
    ASSERT_NE(
        write_context.id_add(
            &source->id,
            {.operations = bke::blendfile::PartialWriteContext::IDAddOperations(operations)}),
        nullptr);
    ASSERT_TRUE(write_context.write(file.path, reports.value));
  }

  if (const char *artifact_path = BLI_getenv("BLENDER_NURBS_TEST_ARTIFACT")) {
    ASSERT_NE(artifact_path[0], '\0') << "BLENDER_NURBS_TEST_ARTIFACT must be an absolute path.";
    ASSERT_TRUE(BLI_path_is_abs_from_cwd(artifact_path))
        << "BLENDER_NURBS_TEST_ARTIFACT must be an absolute path.";
    ASSERT_EQ(BLI_copy(file.path, artifact_path), 0)
        << "Could not copy topology test blendfile to " << artifact_path;
  }

  BlendFileReadParams read_params{};
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports.value;
  ScopedBlenderGlobals blender_globals;
  std::unique_ptr<BlendFileData, decltype(&BLO_blendfiledata_free)> read_file(
      BKE_blendfile_read(file.path, &read_params, &read_reports), &BLO_blendfiledata_free);
  ASSERT_NE(read_file, nullptr);

  NurbsGeometry *restored = static_cast<NurbsGeometry *>(read_file->main->nurbs_geometries.first);
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->id.next, nullptr);
  ASSERT_NE(restored->topology, nullptr);
  EXPECT_EQ(restored->next_component_id, expected_next_component_id);
  EXPECT_EQ(restored->active_control_vertex_id, source->active_control_vertex_id);
  ASSERT_EQ(BLI_listbase_count(&source->curves), BLI_listbase_count(&restored->curves));
  auto restored_curve = restored->curves.begin();
  for (const NurbsCurve &source_curve : source->curves) {
    ASSERT_NE(restored_curve, restored->curves.end());
    expect_curve_equal(source_curve, *restored_curve++);
  }
  expect_topology_equal(*source->topology, *restored->topology);

  Vector<uint64_t> source_ids, restored_ids;
  collect_geometry_ids(*source, source_ids);
  collect_geometry_ids(*restored, restored_ids);
  std::sort(source_ids.begin(), source_ids.end());
  std::sort(restored_ids.begin(), restored_ids.end());
  ASSERT_EQ(source_ids.size(), restored_ids.size());
  EXPECT_TRUE(std::equal(source_ids.begin(), source_ids.end(), restored_ids.begin()));
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(restored->curves, *restored->topology, &error)) << error;

  NurbsEdgeSplitResult restored_split;
  uint64_t restored_next_component_id = restored->next_component_id;
  ASSERT_TRUE(BKE_nurbs_topology_edge_split(restored->curves,
                                            *restored->topology,
                                            restored_next_component_id,
                                            split_result.child_edge_ids[0],
                                            0.25,
                                            restored_split,
                                            &error))
      << error;
  EXPECT_GE(restored_split.vertex_id, expected_next_component_id);
  EXPECT_GT(restored_next_component_id, expected_next_component_id);
  EXPECT_TRUE(BKE_nurbs_topology_is_valid(restored->curves, *restored->topology, &error)) << error;
  restored_ids.clear();
  collect_geometry_ids(*restored, restored_ids);
  std::sort(restored_ids.begin(), restored_ids.end());
  EXPECT_EQ(std::adjacent_find(restored_ids.begin(), restored_ids.end()), restored_ids.end());
}

}  // namespace blender::bke::tests
