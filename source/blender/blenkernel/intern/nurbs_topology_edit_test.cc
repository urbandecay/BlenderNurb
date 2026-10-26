/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "DNA_object_types.h"

#include "BKE_blender.hh"
#include "BKE_curve.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_refine.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_nurbs_transform.hh"
#include "BKE_report.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "bmesh.hh"

#include "nurbs_test_geometry.hh"
#include "testing/testing.h"

namespace blender::bke::nurbs::tests {

class NurbsTopologyEditTest : public BlenderGTestBase {
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

static OwnedGeometry planar_grid(const int nx,
                                 const int ny,
                                 const double3 origin = {0, 0, 0},
                                 const double2 step = {1, 1})
{
  OwnedGeometry geometry(MEM_new<NurbsGeometry>(__func__));
  Vector<int64_t> vertices;
  Map<std::pair<int, int>, int64_t> edges;
  for (int y = 0; y <= ny; y++) {
    for (int x = 0; x <= nx; x++) {
      vertices.append(add_vertex(*geometry, origin + double3(x * step.x, y * step.y, 0))->id);
    }
  }
  auto edge = [&](const int a, const int b) {
    const auto key = std::minmax(a, b);
    if (const auto *id = edges.lookup_ptr({key.first, key.second})) {
      return *id;
    }
    const auto index = TopologyIndex::build(*geometry);
    const auto &va = *index->vertex(vertices[a]), &vb = *index->vertex(vertices[b]);
    const auto *curve = add_linear_curve(*geometry, double3(va.position), double3(vb.position), 3);
    const int64_t id = add_edge(*geometry, va, vb, *curve)->id;
    edges.add_new({key.first, key.second}, id);
    return id;
  };
  for (int y = 0; y < ny; y++) {
    for (int x = 0; x < nx; x++) {
      const int a = y * (nx + 1) + x, b = a + 1, d = a + nx + 1, c = d + 1;
      const int64_t boundary[] = {edge(a, b), edge(b, c), edge(c, d), edge(d, a)};
      auto filled = fill_components(*geometry, {{}, {boundary, 4}, {}});
      if (!filled) {
        return nullptr;
      }
      geometry = std::move(filled->geometry);
    }
  }
  return geometry;
}

static OwnedGeometry wire_junction()
{
  OwnedGeometry geometry(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*geometry, {-1, 0, 0});
  const auto *b = add_vertex(*geometry, {0, 0, 0});
  const auto *c = add_vertex(*geometry, {1, 0, 0});
  const auto *ab = add_linear_curve(*geometry, double3(a->position), double3(b->position), 3);
  const auto *bc = add_linear_curve(*geometry, double3(b->position), double3(c->position), 3);
  add_edge(*geometry, *a, *b, *ab);
  add_edge(*geometry, *b, *c, *bc);
  return geometry;
}

struct TopologySignature {
  using Point = std::array<double, 3>;
  std::vector<Point> vertices;
  std::vector<std::vector<Point>> edges, faces;
  void sort()
  {
    std::sort(vertices.begin(), vertices.end());
    for (auto *domain : {&edges, &faces}) {
      for (auto &item : *domain) {
        std::sort(item.begin(), item.end());
      }
      std::sort(domain->begin(), domain->end());
    }
  }
};

static TopologySignature signature(const NurbsGeometry &geometry)
{
  TopologySignature result;
  const auto index = TopologyIndex::build(geometry);
  auto point = [&](const int64_t id) {
    const auto &v = *index->vertex(id);
    return TopologySignature::Point{v.position[0], v.position[1], v.position[2]};
  };
  for (const auto &vertex : geometry.vertices) {
    result.vertices.push_back(point(vertex.id));
  }
  for (const auto &edge : geometry.edges) {
    result.edges.push_back({point(edge.vertex_ids[0]), point(edge.vertex_ids[1])});
  }
  for (const auto &face : geometry.faces) {
    std::vector<TopologySignature::Point> boundary;
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        boundary.push_back(point(index->edge(use.edge_id)->vertex_ids[use.reversed ? 1 : 0]));
      }
    }
    result.faces.push_back(std::move(boundary));
  }
  result.sort();
  return result;
}

static TopologySignature signature(const Mesh &mesh)
{
  TopologySignature result;
  const auto positions = mesh.vert_positions();
  auto point = [&](const int index) {
    return TopologySignature::Point{positions[index].x, positions[index].y, positions[index].z};
  };
  for (const int i : positions.index_range()) {
    result.vertices.push_back(point(i));
  }
  for (const int2 edge : mesh.edges()) {
    result.edges.push_back({point(edge.x), point(edge.y)});
  }
  const auto faces = mesh.faces();
  const auto corners = mesh.corner_verts();
  for (const int i : faces.index_range()) {
    std::vector<TopologySignature::Point> boundary;
    for (const int corner : faces[i]) {
      boundary.push_back(point(corners[corner]));
    }
    result.faces.push_back(std::move(boundary));
  }
  result.sort();
  return result;
}

static TopologySignature merge_bmesh_reference(const NurbsGeometry &source,
                                               const SelectionState &selection,
                                               const VertexMergeMode mode,
                                               const float3 target)
{
  const BMeshCreateParams parameters{.use_toolflags = true};
  BMesh *mesh = BM_mesh_create(&bm_mesh_allocsize_default, &parameters);
  BLI_SCOPED_DEFER([&]() { BM_mesh_free(mesh); });
  Map<int64_t, BMVert *> vertices;
  const auto index = TopologyIndex::build(source);
  for (const auto &vertex : source.vertices) {
    const float3 position(double3(vertex.position));
    auto *created = BM_vert_create(mesh, position, nullptr, BM_CREATE_NOP);
    vertices.add_new(vertex.id, created);
    BM_elem_flag_set(created, BM_ELEM_SELECT, selection.vertices.contains(vertex.id));
  }
  for (const auto &edge : source.edges) {
    auto *created = BM_edge_create(mesh,
                                   vertices.lookup(edge.vertex_ids[0]),
                                   vertices.lookup(edge.vertex_ids[1]),
                                   nullptr,
                                   BM_CREATE_NOP);
    BM_elem_flag_set(created, BM_ELEM_SELECT, selection.edges.contains(edge.id));
  }
  for (const auto &face : source.faces) {
    Vector<BMVert *> boundary;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      boundary.append(vertices.lookup(edge.vertex_ids[use.reversed ? 1 : 0]));
    }
    BM_face_create_verts(mesh, boundary.data(), boundary.size(), nullptr, BM_CREATE_NOP, false);
  }
  BMOperator op;
  if (mode == VertexMergeMode::Collapse) {
    BMO_op_initf(mesh, &op, BMO_FLAG_DEFAULTS, "collapse edges=%he uvs=%b", BM_ELEM_SELECT, false);
  }
  else {
    BMO_op_initf(mesh,
                 &op,
                 BMO_FLAG_DEFAULTS,
                 "pointmerge verts=%hv merge_co=%v",
                 BM_ELEM_SELECT,
                 &target.x);
  }
  BMO_op_exec(mesh, &op);
  BMO_op_finish(mesh, &op);
  TopologySignature result;
  auto point = [](const BMVert *vertex) {
    return TopologySignature::Point{vertex->co[0], vertex->co[1], vertex->co[2]};
  };
  BMIter iter;
  BMVert *vertex;
  BM_ITER_MESH (vertex, &iter, mesh, BM_VERTS_OF_MESH) {
    result.vertices.push_back(point(vertex));
  }
  BMEdge *edge;
  BM_ITER_MESH (edge, &iter, mesh, BM_EDGES_OF_MESH) {
    result.edges.push_back({point(edge->v1), point(edge->v2)});
  }
  BMFace *face;
  BM_ITER_MESH (face, &iter, mesh, BM_FACES_OF_MESH) {
    std::vector<TopologySignature::Point> boundary;
    BMIter loop_iter;
    BMLoop *loop;
    BM_ITER_ELEM (loop, &loop_iter, face, BM_LOOPS_OF_FACE) {
      boundary.push_back(point(loop->v));
    }
    result.faces.push_back(std::move(boundary));
  }
  result.sort();
  return result;
}

static void expect_signature_equal(const TopologySignature &a, const TopologySignature &b)
{
  EXPECT_EQ(a.vertices, b.vertices);
  EXPECT_EQ(a.edges, b.edges);
  EXPECT_EQ(a.faces, b.faces);
}

static void expect_signature_near_mesh(const TopologySignature &a, const TopologySignature &b)
{
  ASSERT_EQ(a.vertices.size(), b.vertices.size());
  for (int i = 0; i < a.vertices.size(); i++) {
    for (int axis = 0; axis < 3; axis++) {
      EXPECT_NEAR(a.vertices[i][axis], b.vertices[i][axis], 1e-6);
    }
  }
  const auto compare = [](const auto &first, const auto &second) {
    ASSERT_EQ(first.size(), second.size());
    for (int i = 0; i < first.size(); i++) {
      ASSERT_EQ(first[i].size(), second[i].size());
      for (int j = 0; j < first[i].size(); j++) {
        for (int axis = 0; axis < 3; axis++) {
          EXPECT_NEAR(first[i][j][axis], second[i][j][axis], 1e-6);
        }
      }
    }
  };
  compare(a.edges, b.edges);
  compare(a.faces, b.faces);
}

static void write_source(const std::string &name,
                         const NurbsGeometry &geometry,
                         const SelectionState &selection)
{
  const char *directory = std::getenv("BLENDER_NURBS_TOPOLOGY_OUTPUT");
  if (!directory) {
    return;
  }
  Main *bmain = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(bmain); });
  Curve *curve = BKE_curve_add(bmain, "Topology source", OB_SURF);
  auto state = EditData::create(geometry, selection);
  ASSERT_NE(state, nullptr);
  curve->nurbs_geometry = geometry_from_edit_snapshot(state->snapshot()).release();
  ASSERT_NE(curve->nurbs_geometry, nullptr);
  curve->resolu = curve->resolv = 8;
  const std::string path = std::string(directory) + "/" + name + ".blend";
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  const BlendFileWriteParams params{};
  EXPECT_TRUE(BLO_write_file(bmain, path.c_str(), 0, &params, &reports));
}

static void compare_mesh_file(const std::string &name,
                              const NurbsGeometry &geometry,
                              const double comparison_grid = 0,
                              const char *directory_override = nullptr)
{
  const char *directory = directory_override ? directory_override :
                                               std::getenv("BLENDER_NURBS_TOPOLOGY_OUTPUT");
  if (!directory) {
    return;
  }
  const std::string path = std::string(directory) + "/" + name + ".blend";
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
  ASSERT_NE(file, nullptr);
  BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
  ASSERT_EQ(file->main->meshes.count(), 1);
  auto native = signature(geometry);
  auto mesh = signature(*static_cast<Mesh *>(file->main->meshes.first));
  if (comparison_grid > 0) {
    // Mesh reference coordinates are float. This is a connectivity comparison,
    // independent of the double source-construction error assertions above.
    for (auto *result : {&native, &mesh}) {
      auto round = [&](TopologySignature::Point &point) {
        for (double &coordinate : point) {
          coordinate = std::round(coordinate / comparison_grid) * comparison_grid;
        }
      };
      for (auto &point : result->vertices) {
        round(point);
      }
      for (auto *domain : {&result->edges, &result->faces}) {
        for (auto &boundary : *domain) {
          for (auto &point : boundary) {
            round(point);
          }
        }
      }
      result->sort();
    }
  }
  expect_signature_equal(native, mesh);
}

TEST_F(NurbsTopologyEditTest, RipFansMatchActualMeshAndKeepIndependentGeometry)
{
  for (const char *label : {"VERT", "EDGE", "WIRE", "GRID_VERT", "GRID_EDGE"}) {
    SCOPED_TRACE(label);
    const bool grid = STRPREFIX(label, "GRID");
    const bool wire = STREQ(label, "WIRE");
    const bool edge_mode = STREQ(label, "EDGE") || STREQ(label, "GRID_EDGE");
    auto geometry = wire ? wire_junction() :
                    grid ? planar_grid(2, 2) :
                           planar_grid(1, 2, {-1, 0, 0}, {2, 2});
    ASSERT_NE(geometry, nullptr);
    Vector<int64_t> vertices, faces;
    for (const auto &v : geometry->vertices) {
      vertices.append(v.id);
    }
    for (const auto &f : geometry->faces) {
      faces.append(f.id);
    }
    auto edge_between = [&](const int a, const int b) {
      for (const auto &edge : geometry->edges) {
        if ((edge.vertex_ids[0] == vertices[a] && edge.vertex_ids[1] == vertices[b]) ||
            (edge.vertex_ids[1] == vertices[a] && edge.vertex_ids[0] == vertices[b]))
        {
          return edge.id;
        }
      }
      return int64_t(0);
    };
    const int selected_vertex = wire ? 1 : grid ? 4 : 2;
    const int64_t seed_edge = wire                      ? edge_between(1, 2) :
                              STREQ(label, "GRID_VERT") ? edge_between(4, 7) :
                              grid                      ? edge_between(1, 4) :
                                                          edge_between(2, 3);
    const int64_t seed_face = wire ? 0 : faces[STREQ(label, "GRID_VERT") ? 3 : 1];
    SelectionState initial;
    initial.mode = edge_mode ? SelectionMode::Edge : SelectionMode::Vertex;
    const ElementRef hit{edge_mode ? ElementDomain::Edge : ElementDomain::Vertex,
                         edge_mode ? seed_edge : vertices[selected_vertex]};
    auto selected = selection_apply(*geometry, initial, {&hit, 1}, SelectionAction::Set, hit);
    ASSERT_TRUE(selected.has_value());
    write_source(std::string("rip-source-") + label, *geometry, *selected);
    Vector<int64_t> selected_vertices(selected->vertices.begin(), selected->vertices.end());
    Vector<int64_t> selected_edges(selected->edges.begin(), selected->edges.end());
    std::string error;
    auto rip = rip_components(
        *geometry, {selected_vertices, selected_edges, {}}, seed_face, seed_edge, &error);
    ASSERT_TRUE(rip.has_value()) << error;
    const int original_vertices = geometry->vertices.count();
    EXPECT_EQ(rip->geometry->vertices.count(), original_vertices + (edge_mode ? 2 : 1));
    EXPECT_EQ(rip->geometry->faces.count(), geometry->faces.count());
    EXPECT_EQ(rip->geometry->edges.count(), wire ? 2 : grid ? 14 : 8);
    const auto index = TopologyIndex::build(*rip->geometry);
    ASSERT_TRUE(index.has_value());
    for (const auto &face : geometry->faces) {
      EXPECT_NE(index->face(face.id), nullptr);
    }
    const Vector<int64_t> moving = rip->vertices;
    auto candidate = edit_snapshot_from_topology_result(
        std::move(*rip), *selected, initial.mode, &error);
    ASSERT_TRUE(candidate.has_value()) << error;
    auto moved = translate_vertices(*candidate->geometry, moving, {0, 0, 1}, 0, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    compare_mesh_file(std::string("rip-mesh-") + label, *moved->geometry);
    EXPECT_EQ(geometry->vertices.count(), original_vertices);
    for (const auto &v : geometry->vertices) {
      EXPECT_EQ(v.position[2], 0);
    }
  }
}

TEST_F(NurbsTopologyEditTest, RipRejectsBoundaryFaceAndExhaustedIdentityWithoutMutation)
{
  auto geometry = planar_grid(1, 1);
  ASSERT_NE(geometry, nullptr);
  const auto &vertex = *static_cast<NurbsTopologyVertex *>(geometry->vertices.first);
  const auto &edge = *static_cast<NurbsTopologyEdge *>(geometry->edges.first);
  const auto &face = *static_cast<NurbsTopologyFace *>(geometry->faces.first);
  std::string error;
  EXPECT_FALSE(rip_components(*geometry, {{&vertex.id, 1}, {}, {}}, face.id, edge.id, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(rip_components(*geometry, {{}, {}, {&face.id, 1}}, face.id, edge.id, &error));
  EXPECT_NE(error.find("faces"), std::string::npos);
  auto wire = wire_junction();
  auto &junction = *static_cast<NurbsTopologyVertex *>(wire->vertices.first)->next;
  const int64_t branch = static_cast<NurbsTopologyEdge *>(wire->edges.last)->id;
  wire->next_id = std::numeric_limits<int64_t>::max();
  EXPECT_FALSE(rip_components(*wire, {{&junction.id, 1}, {}, {}}, 0, branch, &error));
  EXPECT_NE(error.find("exhausted"), std::string::npos);
  EXPECT_EQ(wire->vertices.count(), 3);
  EXPECT_EQ(wire->edges.count(), 2);
}

TEST_F(NurbsTopologyEditTest, SavedRipSourcesMatchActualMeshAndKeepNativeSelection)
{
  if (!std::getenv("BLENDER_NURBS_TOPOLOGY_UI_READY")) {
    GTEST_SKIP() << "Run after the native Rip viewport workflow";
  }
  const char *directory = std::getenv("BLENDER_NURBS_TOPOLOGY_OUTPUT");
  ASSERT_NE(directory, nullptr);
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  for (const char *label : {"VERT", "EDGE", "WIRE", "GRID_VERT", "GRID_EDGE"}) {
    SCOPED_TRACE(label);
    const std::string path = std::string(directory) + "/rip-native-" + label + ".blend";
    auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
    ASSERT_NE(file, nullptr);
    BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
    ASSERT_EQ(file->main->objects.count(), 1);
    const auto &object = *static_cast<Object *>(file->main->objects.first);
    const auto &curve = *id_cast<const Curve *>(object.data);
    ASSERT_TRUE(curve.nurb.is_empty());
    ASSERT_NE(curve.nurbs_geometry, nullptr);
    std::string error;
    EXPECT_TRUE(geometry_validate_structure(*curve.nurbs_geometry, &error)) << error;
    const auto selected = selection_from_storage(curve.nurbs_geometry->stored_selection, &error);
    ASSERT_TRUE(selected.has_value()) << error;
    EXPECT_EQ(selected->vertices.size(),
              STREQ(label, "EDGE") || STREQ(label, "GRID_EDGE") ? 2 : 1);
    ASSERT_TRUE(selected->active.has_value());
    EXPECT_TRUE(selection_contains(*selected, *selected->active));
    compare_mesh_file(std::string("rip-mesh-") + label, *curve.nurbs_geometry);
  }
}

TEST_F(NurbsTopologyEditTest, MergeModesRepairCollapsedBoundariesAndMatchMesh)
{
  for (const char *label : {"CENTER", "CURSOR", "FIRST", "LAST", "COLLAPSE", "ALL", "OPPOSITE"}) {
    SCOPED_TRACE(label);
    const bool collapse = STREQ(label, "COLLAPSE");
    auto geometry = planar_grid(collapse ? 2 : 1, 1, {-1, 0, 0}, {2, 2});
    ASSERT_NE(geometry, nullptr);
    Vector<int64_t> vertices;
    for (const auto &vertex : geometry->vertices) {
      vertices.append(vertex.id);
    }
    VertexMergeMode mode = VertexMergeMode::Center;
    if (STREQ(label, "CURSOR")) {
      mode = VertexMergeMode::Cursor;
    }
    if (STREQ(label, "FIRST")) {
      mode = VertexMergeMode::First;
    }
    if (STREQ(label, "LAST")) {
      mode = VertexMergeMode::Last;
    }
    if (collapse) {
      mode = VertexMergeMode::Collapse;
    }
    Vector<int> hits = STREQ(label, "ALL")      ? Vector<int>{0, 1, 2, 3} :
                       STREQ(label, "OPPOSITE") ? Vector<int>{0, 3} :
                       collapse                 ? Vector<int>{0, 1, 2} :
                       ELEM(mode, VertexMergeMode::First, VertexMergeMode::Last) ?
                                  Vector<int>{1, 0} :
                                  Vector<int>{0, 1};
    SelectionState selected;
    for (const int i : hits) {
      const ElementRef hit{ElementDomain::Vertex, vertices[i]};
      const auto next = selection_apply(*geometry, selected, {&hit, 1}, SelectionAction::Add, hit);
      ASSERT_TRUE(next.has_value());
      selected = std::move(*next);
    }
    write_source(std::string("merge-source-") + label, *geometry, selected);
    std::string error;
    auto merged = merge_selected_vertices(*geometry, selected, mode, {0, 0, 1}, 1.0e-9, &error);
    ASSERT_TRUE(merged.has_value()) << error;
    EXPECT_EQ(merged->geometry->vertices.count(), STREQ(label, "ALL") ? 1 : collapse ? 4 : 3);
    EXPECT_EQ(merged->geometry->edges.count(),
              STREQ(label, "ALL")      ? 0 :
              STREQ(label, "OPPOSITE") ? 2 :
              collapse                 ? 5 :
                                         3);
    EXPECT_EQ(merged->geometry->faces.count(),
              (STREQ(label, "ALL") || STREQ(label, "OPPOSITE")) ? 0 :
              collapse                                          ? 2 :
                                                                  1);
    ASSERT_EQ(merged->vertices.size(), 1);
    const auto index = TopologyIndex::build(*merged->geometry);
    ASSERT_TRUE(index.has_value());
    const auto &survivor = *index->vertex(merged->vertices[0]);
    if (mode == VertexMergeMode::Cursor) {
      EXPECT_EQ(double3(survivor.position), double3(0, 0, 1));
    }
    if (mode == VertexMergeMode::First) {
      EXPECT_EQ(double3(survivor.position), double3(1, 0, 0));
    }
    if (mode == VertexMergeMode::Last) {
      EXPECT_EQ(double3(survivor.position), double3(-1, 0, 0));
    }
    const auto preview = tessellate_surfaces(*merged->geometry, preview_options(), &error);
    ASSERT_TRUE(preview.has_value()) << error;
    if (std::getenv("BLENDER_NURBS_MERGE_REFERENCE_READY")) {
      compare_mesh_file(std::string("merge-mesh-") + label, *merged->geometry);
    }
    const int64_t output = merged->vertices[0];
    auto candidate = edit_snapshot_from_topology_result(
        std::move(*merged), selected, selected.mode, &error);
    ASSERT_TRUE(candidate.has_value()) << error;
    EXPECT_EQ(candidate->selection.vertices.size(), 1);
    EXPECT_EQ(candidate->selection.history.size(), 1);
    EXPECT_EQ(candidate->selection.active, (ElementRef{ElementDomain::Vertex, output}));
    EXPECT_EQ(geometry->vertices.count(), collapse ? 6 : 4);
  }
}

TEST_F(NurbsTopologyEditTest, MergeCoincidentRationalSeamsPreservesCirclesAndRealSharedIdentity)
{
  auto source = make_arc_sheet(true);
  ASSERT_NE(source, nullptr);
  const int64_t face_id = static_cast<NurbsTopologyFace *>(source->faces.first)->id;
  auto copy = duplicate_components(*source, {{}, {}, {&face_id, 1}});
  ASSERT_TRUE(copy.has_value());
  Vector<VertexMergeGroup> groups;
  for (const auto &vertex : source->vertices) {
    groups.append({{vertex.id, copy->source_to_copy.lookup(vertex.id)},
                   vertex.id,
                   double3(vertex.position)});
  }
  std::string error;
  auto result = merge_vertex_groups(*copy->geometry, groups, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->geometry->vertices.count(), 2);
  EXPECT_EQ(result->geometry->edges.count(), 3);
  EXPECT_EQ(result->geometry->faces.count(), 1);
  EXPECT_EQ(result->geometry_error_bound, 0);
  for (const auto &curve : source->curves) {
    const auto *original = static_cast<const NurbsCurveGeometry *>(result->geometry->curves.first);
    while (original && original->id != curve.id) {
      original = original->next;
    }
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(
        std::memcmp(original->points, curve.points, sizeof(NurbsControlPoint) * curve.points_num),
        0);
    EXPECT_EQ(std::memcmp(original->knots, curve.knots, sizeof(double) * curve.knots_num), 0);
  }
  const auto preview = tessellate_surfaces(*result->geometry, preview_options(), &error);
  ASSERT_TRUE(preview.has_value()) << error;
}

TEST_F(NurbsTopologyEditTest, MergeUsesComponentModesAndRejectsUnsupportedCurvesAtomically)
{
  auto geometry = planar_grid(1, 1);
  ASSERT_NE(geometry, nullptr);
  for (const SelectionMode mode :
       {SelectionMode::Vertex, SelectionMode::Edge, SelectionMode::Face})
  {
    SelectionState initial;
    initial.mode = mode;
    const ElementRef hit =
        mode == SelectionMode::Face ?
            ElementRef{ElementDomain::Face,
                       static_cast<NurbsTopologyFace *>(geometry->faces.first)->id} :
            ElementRef{ElementDomain::Edge,
                       static_cast<NurbsTopologyEdge *>(geometry->edges.first)->id};
    std::optional<SelectionState> selected;
    if (mode == SelectionMode::Vertex) {
      initial.vertices.add_multiple(
          Span(static_cast<NurbsTopologyEdge *>(geometry->edges.first)->vertex_ids, 2));
      selected = selection_flush(*geometry, initial, mode);
    }
    else {
      selected = selection_apply(*geometry, initial, {&hit, 1}, SelectionAction::Set, hit);
    }
    ASSERT_TRUE(selected.has_value());
    std::string error;
    auto result = merge_selected_vertices(
        *geometry, *selected, VertexMergeMode::Center, {0, 0, 0}, 1e-9, &error);
    ASSERT_TRUE(result.has_value()) << error;
    auto candidate = edit_snapshot_from_topology_result(
        std::move(*result), *selected, mode, &error);
    ASSERT_TRUE(candidate.has_value()) << error;
    EXPECT_EQ(candidate->selection.mode, mode);
    EXPECT_EQ(candidate->selection.vertices.size(), mode == SelectionMode::Vertex ? 1 : 0);
    EXPECT_TRUE(candidate->selection.edges.is_empty());
    EXPECT_TRUE(candidate->selection.faces.is_empty());
  }
  auto arc = make_arc_sheet(false);
  ASSERT_NE(arc, nullptr);
  Vector<int64_t> vertices;
  for (const auto &vertex : arc->vertices) {
    vertices.append(vertex.id);
  }
  const VertexMergeGroup group{{vertices[0], vertices[1]}, vertices[0], {0.5, 0.5, 0}};
  const auto before = signature(*arc);
  std::string error;
  EXPECT_FALSE(merge_vertex_groups(*arc, {&group, 1}, 1e-9, &error));
  EXPECT_FALSE(error.empty());
  expect_signature_equal(signature(*arc), before);
  const VertexMergeGroup invalid{{99999}, 99999, {0, 0, 0}};
  EXPECT_FALSE(merge_vertex_groups(*geometry, {&invalid, 1}, 1e-9, &error));
}

TEST_F(NurbsTopologyEditTest, SavedMergeSourcesMatchActualMeshAndKeepNativeSelection)
{
  if (!std::getenv("BLENDER_NURBS_MERGE_UI_READY")) {
    GTEST_SKIP() << "Run after the native Merge viewport workflow";
  }
  const char *directory = std::getenv("BLENDER_NURBS_TOPOLOGY_OUTPUT");
  ASSERT_NE(directory, nullptr);
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  for (const char *label : {"CENTER", "CURSOR", "FIRST", "LAST", "COLLAPSE", "ALL", "OPPOSITE"}) {
    SCOPED_TRACE(label);
    const std::string path = std::string(directory) + "/merge-native-" + label + ".blend";
    auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
    ASSERT_NE(file, nullptr);
    BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
    ASSERT_EQ(file->main->objects.count(), 1);
    const auto &object = *static_cast<Object *>(file->main->objects.first);
    const auto &curve = *id_cast<const Curve *>(object.data);
    ASSERT_TRUE(curve.nurb.is_empty());
    ASSERT_NE(curve.nurbs_geometry, nullptr);
    std::string error;
    EXPECT_TRUE(geometry_validate_structure(*curve.nurbs_geometry, &error)) << error;
    const auto selected = selection_from_storage(curve.nurbs_geometry->stored_selection, &error);
    ASSERT_TRUE(selected.has_value()) << error;
    EXPECT_EQ(selected->vertices.size(), 1);
    EXPECT_EQ(selected->mode, SelectionMode::Vertex);
    ASSERT_TRUE(selected->active.has_value());
    EXPECT_TRUE(selection_contains(*selected, *selected->active));
    compare_mesh_file(std::string("merge-mesh-") + label, *curve.nurbs_geometry);
  }
}

TEST_F(NurbsTopologyEditTest, ConsecutiveCenterAndCollapseDropDoomedTriangleBeforeDeformation)
{
  auto source = planar_grid(1, 1, {-1, 0, 0}, {2, 2});
  ASSERT_NE(source, nullptr);
  Vector<int64_t> vertices;
  for (const auto &vertex : source->vertices) {
    vertices.append(vertex.id);
  }
  SelectionState first;
  first.vertices.add(vertices[1]);
  first.vertices.add(vertices[3]);
  first = *selection_flush(*source, first, first.mode);
  std::string error;
  auto center = merge_selected_vertices(*source, first, VertexMergeMode::Center, {}, 1e-9, &error);
  ASSERT_TRUE(center.has_value()) << error;
  EXPECT_EQ(center->geometry->faces.count(), 1);
  EXPECT_EQ(center->geometry->vertices.count(), 3);
  SelectionState second;
  second.vertices.add(vertices[0]);
  second.vertices.add(vertices[2]);
  second = *selection_flush(*center->geometry, second, second.mode);
  auto collapsed = merge_selected_vertices(
      *center->geometry, second, VertexMergeMode::Collapse, {}, 1e-9, &error);
  ASSERT_TRUE(collapsed.has_value()) << error;
  EXPECT_EQ(collapsed->geometry->vertices.count(), 2);
  EXPECT_EQ(collapsed->geometry->edges.count(), 1);
  EXPECT_EQ(collapsed->geometry->faces.count(), 0);
  EXPECT_EQ(source->vertices.count(), 4);
  EXPECT_EQ(source->faces.count(), 1);
}

TEST_F(NurbsTopologyEditTest, MergeExtrudedBoxTopIntoOneSharedApex)
{
  auto sheet = planar_grid(1, 1, {-1, -1, 0}, {2, 2});
  ASSERT_NE(sheet, nullptr);
  const int64_t bottom = static_cast<NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  SelectionState initial;
  initial.mode = SelectionMode::Face;
  const auto top_selected = selection_apply(
      *box->geometry, initial, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(top_selected.has_value()) << error;
  for (const VertexMergeMode merge_mode : {VertexMergeMode::Center, VertexMergeMode::Collapse}) {
    for (const SelectionMode mode :
         {SelectionMode::Vertex, SelectionMode::Edge, SelectionMode::Face})
    {
      SCOPED_TRACE(int(merge_mode));
      SCOPED_TRACE(int(mode));
      const auto selected = selection_change_mode(*box->geometry, *top_selected, mode, &error);
      ASSERT_TRUE(selected.has_value()) << error;
      auto result = merge_selected_vertices(*box->geometry, *selected, merge_mode, {}, 0, &error);
      ASSERT_TRUE(result.has_value()) << error;
      EXPECT_EQ(result->geometry->vertices.count(), 5);
      EXPECT_EQ(result->geometry->edges.count(), 8);
      EXPECT_EQ(result->geometry->faces.count(), 5);
      EXPECT_EQ(result->geometry_error_bound, 0);
      expect_signature_equal(
          signature(*result->geometry),
          merge_bmesh_reference(*box->geometry, *selected, merge_mode, {0, 0, 2}));
      ASSERT_EQ(result->vertices.size(), 1);
      const auto index = TopologyIndex::build(*result->geometry, &error);
      ASSERT_TRUE(index.has_value()) << error;
      EXPECT_EQ(double3(index->vertex(result->vertices[0])->position), double3(0, 0, 2));
      for (const auto &edge : result->geometry->edges) {
        const auto uses = index->uses_for_edge(edge.id);
        ASSERT_EQ(uses.size(), 2);
        EXPECT_EQ(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                  -index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
      }
      EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
      EXPECT_EQ(box->geometry->vertices.count(), 8);
      EXPECT_EQ(box->geometry->faces.count(), 6);
    }
  }
}

TEST_F(NurbsTopologyEditTest, SuccessiveBoxEdgeAndTopMergesMatchBMesh)
{
  auto sheet = planar_grid(1, 1, {-1, -1, 0}, {2, 2});
  const int64_t bottom = static_cast<NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  const auto index = TopologyIndex::build(*box->geometry);
  for (const auto &first_use :
       static_cast<const NurbsTopologyLoop *>(index->face(top.id)->loops.first)->edge_uses)
  {
    const int64_t first_edge = first_use.edge_id;
    SCOPED_TRACE(first_edge);
    const auto &source_edge = *index->edge(first_edge);
    const double3 first_target = (double3(index->vertex(source_edge.vertex_ids[0])->position) +
                                  double3(index->vertex(source_edge.vertex_ids[1])->position)) *
                                 0.5;
    SelectionState initial;
    initial.mode = SelectionMode::Edge;
    const ElementRef edge{ElementDomain::Edge, first_edge};
    const auto first_selection = selection_apply(
        *box->geometry, initial, {&edge, 1}, SelectionAction::Set, edge, &error);
    ASSERT_TRUE(first_selection.has_value()) << error;
    for (const VertexMergeMode first_mode : {VertexMergeMode::Center, VertexMergeMode::Collapse}) {
      const auto first = merge_selected_vertices(
          *box->geometry, *first_selection, first_mode, {}, 1e-9, &error);
      ASSERT_TRUE(first.has_value()) << error;
      expect_signature_equal(
          signature(*first->geometry),
          merge_bmesh_reference(
              *box->geometry, *first_selection, first_mode, float3(first_target)));
      SelectionState face_mode;
      face_mode.mode = SelectionMode::Face;
      const auto face_selection = selection_apply(
          *first->geometry, face_mode, {&top, 1}, SelectionAction::Set, top, &error);
      ASSERT_TRUE(face_selection.has_value()) << error;
      const auto first_index = TopologyIndex::build(*first->geometry);
      double3 second_target(0);
      for (const int64_t id : face_selection->vertices) {
        second_target += double3(first_index->vertex(id)->position);
      }
      second_target /= double(face_selection->vertices.size());
      for (const SelectionMode mode :
           {SelectionMode::Vertex, SelectionMode::Edge, SelectionMode::Face})
      {
        const auto selected = selection_change_mode(
            *first->geometry, *face_selection, mode, &error);
        ASSERT_TRUE(selected.has_value()) << error;
        for (const VertexMergeMode second_mode :
             {VertexMergeMode::Center, VertexMergeMode::Collapse})
        {
          SCOPED_TRACE(int(first_mode));
          SCOPED_TRACE(int(mode));
          SCOPED_TRACE(int(second_mode));
          const auto second = merge_selected_vertices(
              *first->geometry, *selected, second_mode, {}, 1e-9, &error);
          ASSERT_TRUE(second.has_value()) << error;
          EXPECT_EQ(second->geometry->vertices.count(), 5);
          EXPECT_EQ(second->geometry->edges.count(), 8);
          EXPECT_EQ(second->geometry->faces.count(), 5);
          // BMesh stores float positions; source targets and error budgets are checked separately.
          expect_signature_near_mesh(
              signature(*second->geometry),
              merge_bmesh_reference(
                  *first->geometry, *selected, second_mode, float3(second_target)));
          const auto result_index = TopologyIndex::build(*second->geometry);
          EXPECT_EQ(double3(result_index->vertex(second->vertices[0])->position), second_target);
          for (const auto &edge : second->geometry->edges) {
            const auto uses = result_index->uses_for_edge(edge.id);
            ASSERT_EQ(uses.size(), 2);
            EXPECT_EQ(result_index->face(uses[0].face_id)->orientation *
                          (uses[0].use->reversed ? -1 : 1),
                      -result_index->face(uses[1].face_id)->orientation *
                          (uses[1].use->reversed ? -1 : 1));
          }
          SurfacePreviewOptions strict_preview;
          strict_preview.boundary.segments_per_span = strict_preview.interior_subdivisions = 7;
          const auto preview = tessellate_surfaces(*second->geometry, strict_preview, &error);
          ASSERT_TRUE(preview.has_value()) << error;
        }
      }
    }
  }
}

TEST_F(NurbsTopologyEditTest, RepeatedEdgeMergesReachFacesWiresAndOneVertexLikeBMesh)
{
  for (const VertexMergeMode mode : {VertexMergeMode::Center, VertexMergeMode::Collapse}) {
    auto sheet = planar_grid(1, 1, {-1, -1, 0}, {2, 2});
    const int64_t bottom = static_cast<NurbsTopologyFace *>(sheet->faces.first)->id;
    std::string error;
    auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
    ASSERT_TRUE(box.has_value()) << error;
    OwnedGeometry geometry = std::move(box->geometry);
    while (!geometry->edges.is_empty()) {
      const int old_count = geometry->vertices.count();
      SCOPED_TRACE(int(mode));
      SCOPED_TRACE(old_count);
      const auto index = TopologyIndex::build(*geometry);
      const auto &edge = *static_cast<const NurbsTopologyEdge *>(geometry->edges.first);
      const double3 target = (double3(index->vertex(edge.vertex_ids[0])->position) +
                              double3(index->vertex(edge.vertex_ids[1])->position)) *
                             0.5;
      SelectionState initial;
      initial.mode = SelectionMode::Edge;
      const ElementRef ref{ElementDomain::Edge, edge.id};
      const auto selected = selection_apply(
          *geometry, initial, {&ref, 1}, SelectionAction::Set, ref, &error);
      ASSERT_TRUE(selected.has_value()) << error;
      const auto reference = merge_bmesh_reference(*geometry, *selected, mode, float3(target));
      auto merged = merge_selected_vertices(*geometry, *selected, mode, {}, 1e-9, &error);
      ASSERT_TRUE(merged.has_value()) << error;
      EXPECT_EQ(merged->geometry->vertices.count(), old_count - 1);
      expect_signature_near_mesh(signature(*merged->geometry), reference);
      EXPECT_EQ(merged->geometry_error_bound, 0);
      EXPECT_TRUE(tessellate_surfaces(*merged->geometry, preview_options(), &error)) << error;
      geometry = std::move(merged->geometry);
    }
    EXPECT_EQ(geometry->vertices.count(), 1);
    EXPECT_TRUE(geometry->faces.is_empty());
  }
}

TEST_F(NurbsTopologyEditTest, MergePreservesUnmovedPatchesAndWarpedTriangleInteriors)
{
  auto source = planar_grid(3, 1);
  ASSERT_NE(source, nullptr);
  const auto index = TopologyIndex::build(*source);
  const auto &edge = *static_cast<const NurbsTopologyEdge *>(source->edges.first);
  const auto &unaffected = *static_cast<const NurbsTopologyFace *>(source->faces.last);
  const auto &loop = *static_cast<const NurbsTopologyLoop *>(unaffected.loops.first);
  const auto &last_use = *static_cast<const NurbsTopologyEdgeUse *>(loop.edge_uses.first);
  const auto &last_edge = *index->edge(last_use.edge_id);
  const int64_t unmoved = last_edge.vertex_ids[1];
  const VertexMergeGroup groups[] = {
      {{edge.vertex_ids[0], edge.vertex_ids[1]}, edge.vertex_ids[0], {0.5, 0, 0}},
      {{unmoved}, unmoved, double3(index->vertex(unmoved)->position)}};
  std::string error;
  const auto merged = merge_vertex_groups(*source, {groups, 2}, 0, &error);
  ASSERT_TRUE(merged.has_value()) << error;
  const auto output = TopologyIndex::build(*merged->geometry);
  ASSERT_TRUE(output.has_value());
  const auto &kept = *output->face(unaffected.id);
  EXPECT_EQ(kept.surface_id, unaffected.surface_id);
  const auto &kept_loop = *static_cast<const NurbsTopologyLoop *>(kept.loops.first);
  EXPECT_EQ(kept_loop.id, loop.id);
  for (const auto &use : loop.edge_uses) {
    bool found = false;
    for (const auto &kept_use : kept_loop.edge_uses) {
      if (kept_use.id == use.id) {
        EXPECT_EQ(kept_use.edge_id, use.edge_id);
        EXPECT_EQ(kept_use.pcurve_id, use.pcurve_id);
        found = true;
      }
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(output->edge(use.edge_id)->curve_id, index->edge(use.edge_id)->curve_id);
  }
  auto warped = make_planar_sheet();
  auto &face = *static_cast<NurbsTopologyFace *>(warped->faces.first);
  for (auto &old_loop : face.loops) {
    BLI_freelistN(&old_loop.edge_uses);
  }
  BLI_freelistN(&face.loops);
  const double2 corners[] = {{0, 0}, {1, 0}, {0, 1}};
  const auto *triangle = add_planar_loop(*warped, face, {corners, 3}, false);
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(warped->surfaces.first);
  surface.points[3].position[2] = 1;
  face.tolerance = 1;
  ASSERT_TRUE(evaluate_boundaries(*warped, {7, 1000, 0}, &error)) << error;
  const auto warped_index = TopologyIndex::build(*warped);
  const auto &triangle_use = *static_cast<const NurbsTopologyEdgeUse *>(triangle->edge_uses.first);
  const int64_t corner = warped_index->edge(triangle_use.edge_id)->vertex_ids[0];
  const VertexMergeGroup move{{corner}, corner, {-0.75, -1, 0}};
  const int64_t next_id = warped->next_id;
  const auto deformed = merge_vertex_groups(*warped, {&move, 1}, 1e-9, &error);
  ASSERT_TRUE(deformed.has_value()) << error;
  const auto &deformed_surface = *static_cast<const NurbsSurfaceGeometry *>(
      deformed->geometry->surfaces.first);
  EXPECT_EQ(deformed_surface.id, surface.id);
  for (int axis = 0; axis < 3; axis++) {
    EXPECT_EQ(
        deformed_surface.points[0].position[axis] - deformed_surface.points[1].position[axis] -
            deformed_surface.points[2].position[axis] + deformed_surface.points[3].position[axis],
        surface.points[0].position[axis] - surface.points[1].position[axis] -
            surface.points[2].position[axis] + surface.points[3].position[axis]);
  }
  EXPECT_TRUE(evaluate_boundaries(*deformed->geometry, {7, 1000, 0}, &error)) << error;
  EXPECT_EQ(surface.points[3].position[2], 1);
  EXPECT_EQ(warped->next_id, next_id);
  EXPECT_EQ(double3(warped_index->vertex(corner)->position), double3(-1, -1, 0));
}

TEST_F(NurbsTopologyEditTest, MergeShearedBoxTopRepairsSlantedTriangleSupports)
{
  auto sheet = planar_grid(1, 1, {-1, -1, 0}, {2, 2});
  const int64_t bottom = static_cast<NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  auto shear = double4x4::identity();
  shear[2][0] = 0.25;
  shear[0][2] = 0.5;
  auto source = transform_geometry(*box->geometry, shear, 0, &error);
  ASSERT_TRUE(source.has_value()) << error;
  SelectionState selection;
  selection.mode = SelectionMode::Face;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  const auto selected = selection_apply(
      *source->geometry, selection, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(selected.has_value()) << error;
  for (const VertexMergeMode mode : {VertexMergeMode::Center, VertexMergeMode::Collapse}) {
    const auto result = merge_selected_vertices(
        *source->geometry, *selected, mode, {}, 1e-9, &error);
    ASSERT_TRUE(result.has_value()) << error;
    expect_signature_equal(signature(*result->geometry),
                           merge_bmesh_reference(*source->geometry, *selected, mode, {0.5, 0, 2}));
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
  }
}

TEST_F(NurbsTopologyEditTest, CollapseRetainsSeparateSelectedEdgeIslandsLikeBMesh)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  Vector<NurbsTopologyVertex *> vertices;
  for (const double x : {0, 2, 4, 6}) {
    vertices.append(add_vertex(*source, {x, 0, 0}));
  }
  for (const int i : {0, 2}) {
    const auto *curve = add_linear_curve(
        *source, double3(vertices[i]->position), double3(vertices[i + 1]->position), 3);
    add_edge(*source, *vertices[i], *vertices[i + 1], *curve);
  }
  SelectionState selection;
  for (const auto *vertex : vertices) {
    selection.vertices.add(vertex->id);
  }
  selection = *selection_flush(*source, selection, selection.mode);
  for (const VertexMergeMode mode : {VertexMergeMode::Center, VertexMergeMode::Collapse}) {
    std::string error;
    const auto result = merge_selected_vertices(*source, selection, mode, {}, 0, &error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(result->vertices.size(), mode == VertexMergeMode::Center ? 1 : 2);
    expect_signature_equal(signature(*result->geometry),
                           merge_bmesh_reference(*source, selection, mode, {3, 0, 0}));
  }
}

TEST_F(NurbsTopologyEditTest, CollapseAveragesEdgeIncidencesAtJunctionsLikeBMesh)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *center = add_vertex(*source, {0, 0, 0});
  for (const double3 position : {double3(4, 0, 0), double3(0, 2, 0), double3(0, -2, 0)}) {
    const auto *end = add_vertex(*source, position);
    const auto *curve = add_linear_curve(*source, double3(center->position), position, 3);
    add_edge(*source, *center, *end, *curve);
  }
  SelectionState initial;
  initial.mode = SelectionMode::Vertex;
  for (const auto &vertex : source->vertices) {
    initial.vertices.add(vertex.id);
  }
  const auto selected = selection_flush(*source, initial, initial.mode);
  ASSERT_TRUE(selected.has_value());
  std::string error;
  for (const VertexMergeMode mode : {VertexMergeMode::Center, VertexMergeMode::Collapse}) {
    const auto result = merge_selected_vertices(*source, *selected, mode, {}, 1e-9, &error);
    ASSERT_TRUE(result.has_value()) << error;
    const double3 expected(mode == VertexMergeMode::Center ? 1.0 : 2.0 / 3, 0, 0);
    EXPECT_EQ(
        double3(
            static_cast<const NurbsTopologyVertex *>(result->geometry->vertices.first)->position),
        expected);
    expect_signature_near_mesh(signature(*result->geometry),
                               merge_bmesh_reference(*source, *selected, mode, float3(expected)));
  }
}

TEST_F(NurbsTopologyEditTest, MergeCenterRemainsFiniteWhenTheVertexSumOverflows)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  SelectionState selection;
  selection.vertices.add(add_vertex(*source, {1e308, 0, 0})->id);
  selection.vertices.add(add_vertex(*source, {1e308, 0, 0})->id);
  std::string error;
  const auto result = merge_selected_vertices(
      *source, selection, VertexMergeMode::Center, {}, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  ASSERT_EQ(result->geometry->vertices.count(), 1);
  EXPECT_EQ(static_cast<NurbsTopologyVertex *>(result->geometry->vertices.first)->position[0],
            1e308);
}

TEST_F(NurbsTopologyEditTest, MergeApexConstructionReportsAndHonorsItsRoundoffBudget)
{
  auto sheet = planar_grid(1, 1, {-1, -1, 0}, {2, 2});
  const int64_t bottom = static_cast<NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  VertexMergeGroup group;
  for (const auto &vertex : box->geometry->vertices) {
    if (vertex.position[2] == 2) {
      group.vertices.append(vertex.id);
    }
  }
  group.survivor = group.vertices.first();
  group.target = {0x1p-60, 0, 2};
  EXPECT_FALSE(merge_vertex_groups(*box->geometry, {&group, 1}, 0, &error));
  const auto result = merge_vertex_groups(*box->geometry, {&group, 1}, 1e-12, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_GT(result->geometry_error_bound, 0);
  EXPECT_LE(result->geometry_error_bound, 1e-12);
  EXPECT_EQ(result->geometry->vertices.count(), 5);
  EXPECT_EQ(result->geometry->edges.count(), 8);
  EXPECT_EQ(result->geometry->faces.count(), 5);
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
  EXPECT_EQ(box->geometry->vertices.count(), 8);
  EXPECT_EQ(box->geometry->faces.count(), 6);
}

TEST_F(NurbsTopologyEditTest, SavedCubeTopMergesMatchActualBMeshAndRetainNurbsSource)
{
  const char *directory = std::getenv("BLENDER_NURBS_APEX_OUTPUT");
  if (!directory) {
    GTEST_SKIP() << "Run after the cube top merge viewport workflows";
  }
  for (const char *label : {"CENTER-VERT-False-True-CAP",
                            "COLLAPSE-VERT-False-True-CAP",
                            "CENTER-FACE-False-True-CAP",
                            "CENTER-EDGE-False-True-CAP",
                            "CENTER-VERT-True-True-CAP",
                            "CENTER-VERT-False-False-CAP",
                            "CENTER-VERT-False-True-BOX",
                            "CENTER-VERT-False-True-CLICK"})
  {
    SCOPED_TRACE(label);
    ReportList reports;
    BKE_reports_init(&reports, RPT_STORE);
    BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
    BlendFileReadReport read_reports{};
    read_reports.reports = &reports;
    const std::string path = std::string(directory) + "/apex-" + label + ".blend";
    auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
    ASSERT_NE(file, nullptr);
    BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
    ASSERT_EQ(file->main->objects.count(), 1);
    const Curve &curve = *id_cast<Curve *>(static_cast<Object *>(file->main->objects.first)->data);
    EXPECT_TRUE(curve.nurb.is_empty());
    ASSERT_NE(curve.nurbs_geometry, nullptr);
    std::string error;
    EXPECT_TRUE(geometry_validate_structure(*curve.nurbs_geometry, &error)) << error;
    const double y = STREQ(label, "CENTER-VERT-False-False-CAP") ? 1 : 0;
    auto sheet = planar_grid(1, 1, {-1, y - 1, 0}, {2, 2});
    const int64_t bottom = static_cast<NurbsTopologyFace *>(sheet->faces.first)->id;
    const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
    ASSERT_TRUE(box.has_value()) << error;
    SelectionState initial;
    initial.mode = SelectionMode::Face;
    const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
    const auto selected = selection_apply(
        *box->geometry, initial, {&top, 1}, SelectionAction::Set, top, &error);
    ASSERT_TRUE(selected.has_value()) << error;
    const VertexMergeMode mode = STREQ(label, "COLLAPSE-VERT-False-True-CAP") ?
                                     VertexMergeMode::Collapse :
                                     VertexMergeMode::Center;
    expect_signature_equal(
        signature(*curve.nurbs_geometry),
        merge_bmesh_reference(*box->geometry, *selected, mode, {0, float(y), 2}));
    EXPECT_TRUE(tessellate_surfaces(*curve.nurbs_geometry, preview_options(), &error)) << error;
    const auto stored = selection_from_storage(curve.nurbs_geometry->stored_selection, &error);
    ASSERT_TRUE(stored.has_value()) << error;
    EXPECT_EQ(stored->mode,
              STREQ(label, "CENTER-FACE-False-True-CAP") ? SelectionMode::Face :
              STREQ(label, "CENTER-EDGE-False-True-CAP") ? SelectionMode::Edge :
                                                           SelectionMode::Vertex);
  }
}

TEST_F(NurbsTopologyEditTest, SavedLayeredMergesMatchBMeshAndContinueFromPersistentSource)
{
  const char *directory = std::getenv("BLENDER_NURBS_LAYERED_OUTPUT");
  if (!directory) {
    GTEST_SKIP() << "Run after the layered merge viewport workflows";
  }
  for (const char *label : {"CENTER-CENTER-VERT",
                            "COLLAPSE-COLLAPSE-VERT",
                            "CENTER-COLLAPSE-EDGE",
                            "CENTER-CENTER-FACE"})
  {
    SCOPED_TRACE(label);
    OwnedGeometry before;
    SelectionState before_selection;
    for (const char *stage : {"before-second", "after-second", "continued"}) {
      SCOPED_TRACE(stage);
      ReportList reports;
      BKE_reports_init(&reports, RPT_STORE);
      BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
      BlendFileReadReport read_reports{};
      read_reports.reports = &reports;
      const std::string path = std::string(directory) + "/layered-" + label + "-" + stage +
                               ".blend";
      auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
      ASSERT_NE(file, nullptr);
      BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
      ASSERT_EQ(file->main->objects.count(), 1);
      const Curve &curve = *id_cast<Curve *>(
          static_cast<Object *>(file->main->objects.first)->data);
      ASSERT_TRUE(curve.nurb.is_empty());
      ASSERT_NE(curve.nurbs_geometry, nullptr);
      const auto &geometry = *curve.nurbs_geometry;
      std::string error;
      ASSERT_TRUE(geometry_validate_structure(geometry, &error)) << error;
      SurfacePreviewOptions options;
      options.boundary.segments_per_span = options.interior_subdivisions = 7;
      ASSERT_TRUE(tessellate_surfaces(geometry, options, &error)) << error;
      const auto selected = selection_from_storage(geometry.stored_selection, &error);
      ASSERT_TRUE(selected.has_value()) << error;
      const SelectionMode expected = STREQ(stage, "continued")            ? SelectionMode::Vertex :
                                     STREQ(label, "CENTER-CENTER-FACE")   ? SelectionMode::Face :
                                     STREQ(label, "CENTER-COLLAPSE-EDGE") ? SelectionMode::Edge :
                                                                            SelectionMode::Vertex;
      EXPECT_EQ(selected->mode, expected);
      if (STREQ(stage, "before-second")) {
        EXPECT_EQ(geometry.vertices.count(), 7);
        EXPECT_EQ(selected->vertices.size(), 3);
        before.reset(geometry_copy(&geometry));
        before_selection = *selected;
      }
      else if (STREQ(stage, "after-second")) {
        EXPECT_EQ(geometry.vertices.count(), 5);
        EXPECT_EQ(geometry.edges.count(), 8);
        EXPECT_EQ(geometry.faces.count(), 5);
        const VertexMergeMode mode = STREQ(label, "CENTER-CENTER-VERT") ||
                                             STREQ(label, "CENTER-CENTER-FACE") ?
                                         VertexMergeMode::Center :
                                         VertexMergeMode::Collapse;
        expect_signature_near_mesh(
            signature(geometry),
            merge_bmesh_reference(*before, before_selection, mode, {0, 1.0f / 3, 2}));
        const auto resumed = merge_selected_vertices(
            *before, before_selection, mode, {}, 1e-9, &error);
        ASSERT_TRUE(resumed.has_value()) << error;
        expect_signature_equal(signature(geometry), signature(*resumed->geometry));
      }
      else {
        EXPECT_EQ(geometry.vertices.count(), 6);
        EXPECT_EQ(geometry.edges.count(), 9);
        EXPECT_EQ(geometry.faces.count(), 5);
      }
    }
  }
}

TEST_F(NurbsTopologyEditTest, DistanceClusteringMatchesActualBMeshAndPreservesSourcePrecision)
{
  const double3 coordinates[] = {
      {0, 0, 0}, {0.125, 0, 0}, {0.25, 0, 0}, {0.625, 0, 0}, {0.75, 0, 0}, {2, 0, 0}};
  for (const bool centroid : {false, true}) {
    for (const bool use_unselected : {false, true}) {
      for (const bool partial_selection : {false, true}) {
        SCOPED_TRACE(centroid);
        SCOPED_TRACE(use_unselected);
        SCOPED_TRACE(partial_selection);
        OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
        const BMeshCreateParams parameters{.use_toolflags = true};
        BMesh *mesh = BM_mesh_create(&bm_mesh_allocsize_default, &parameters);
        BLI_SCOPED_DEFER([&]() { BM_mesh_free(mesh); });
        SelectionState selected;
        for (int i = 0; i < 6; i++) {
          const auto *vertex = add_vertex(*source, coordinates[i]);
          const float3 position(coordinates[i]);
          BMVert *reference = BM_vert_create(mesh, position, nullptr, BM_CREATE_NOP);
          if (!partial_selection || i % 2 == 1) {
            selected.vertices.add(vertex->id);
            BM_elem_flag_enable(reference, BM_ELEM_SELECT);
          }
        }
        BMOperator find, weld;
        if (use_unselected) {
          BMO_op_initf(mesh,
                       &find,
                       BMO_FLAG_DEFAULTS,
                       "find_doubles verts=%av keep_verts=%Hv dist=%f",
                       BM_ELEM_SELECT,
                       0.1875);
        }
        else {
          BMO_op_initf(mesh,
                       &find,
                       BMO_FLAG_DEFAULTS,
                       "find_doubles verts=%hv dist=%f",
                       BM_ELEM_SELECT,
                       0.1875);
        }
        BMO_op_exec(mesh, &find);
        BMO_op_initf(mesh, &weld, BMO_FLAG_DEFAULTS, "weld_verts use_centroid=%b", centroid);
        BMO_slot_copy(&find, slots_out, "targetmap.out", &weld, slots_in, "targetmap");
        BMO_op_exec(mesh, &weld);
        BMO_op_finish(mesh, &find);
        BMO_op_finish(mesh, &weld);
        std::string error;
        auto actual = merge_by_distance(
            *source, selected, 0.1875, centroid, use_unselected, 1e-9, &error);
        ASSERT_TRUE(actual.has_value()) << error;
        std::vector<TopologySignature::Point> expected, found;
        BMIter iter;
        BMVert *vertex;
        BM_ITER_MESH (vertex, &iter, mesh, BM_VERTS_OF_MESH) {
          expected.push_back({vertex->co[0], vertex->co[1], vertex->co[2]});
        }
        for (const auto &point : actual->geometry->vertices) {
          found.push_back({point.position[0], point.position[1], point.position[2]});
        }
        std::sort(expected.begin(), expected.end());
        std::sort(found.begin(), found.end());
        EXPECT_EQ(found, expected);
        int selected_count = 0;
        BM_ITER_MESH (vertex, &iter, mesh, BM_VERTS_OF_MESH) {
          selected_count += BM_elem_flag_test(vertex, BM_ELEM_SELECT) ? 1 : 0;
        }
        EXPECT_EQ(actual->vertices.size(), selected_count);
        EXPECT_EQ(source->vertices.count(), 6);
      }
    }
  }
  OwnedGeometry precise(MEM_new<NurbsGeometry>(__func__));
  SelectionState selected;
  selected.vertices.add(add_vertex(*precise, {1, 0, 0})->id);
  selected.vertices.add(add_vertex(*precise, {1 + 0x1p-40, 0, 0})->id);
  std::string error;
  auto untouched = merge_by_distance(*precise, selected, 0, true, false, 1e-9, &error);
  ASSERT_TRUE(untouched.has_value()) << error;
  EXPECT_EQ(untouched->geometry->vertices.count(), 2);
  EXPECT_EQ(untouched->geometry->next_id, precise->next_id);
  EXPECT_FALSE(merge_by_distance(*precise, selected, NAN, true, false, 1e-9, &error));
}

TEST_F(NurbsTopologyEditTest, DistanceWeldingSharesFaceIdentityAndPreservesRationalCircles)
{
  for (const bool rational : {false, true}) {
    for (const double displacement :
         rational ? Vector<double>{0.0} : Vector<double>{0.0, 0.0625, 0.1})
    {
      for (const bool centroid : {false, true}) {
        SCOPED_TRACE(rational);
        SCOPED_TRACE(displacement);
        SCOPED_TRACE(centroid);
        auto source = rational ? make_arc_sheet(true) : planar_grid(1, 1);
        ASSERT_NE(source, nullptr);
        const int64_t face = static_cast<NurbsTopologyFace *>(source->faces.first)->id;
        auto duplicate = duplicate_components(*source, {{}, {}, {&face, 1}});
        ASSERT_TRUE(duplicate.has_value());
        Vector<int64_t> moving;
        for (const auto &vertex : source->vertices) {
          moving.append(duplicate->source_to_copy.lookup(vertex.id));
        }
        std::string error;
        const double3 offset(displacement, 0, 0);
        auto moved = translate_vertices(*duplicate->geometry, moving, offset, 1e-9, &error);
        ASSERT_TRUE(moved.has_value()) << error;
        SelectionState selected;
        for (const auto &vertex : moved->geometry->vertices) {
          selected.vertices.add(vertex.id);
        }
        selected = *selection_flush(*moved->geometry, selected, selected.mode);
        auto result = merge_by_distance(
            *moved->geometry, selected, 0.125, centroid, false, 1e-9, &error);
        ASSERT_TRUE(result.has_value()) << error;
        EXPECT_EQ(result->geometry->vertices.count(), source->vertices.count());
        EXPECT_EQ(result->geometry->edges.count(), source->edges.count());
        EXPECT_EQ(result->geometry->faces.count(), 1);
        EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
        if (rational) {
          const auto original = TopologyIndex::build(*source);
          const auto welded = TopologyIndex::build(*result->geometry);
          for (const auto &edge : source->edges) {
            ASSERT_NE(welded->edge(edge.id), nullptr);
            if (edge.vertex_ids[0] == edge.vertex_ids[1]) {
              EXPECT_EQ(welded->edge(edge.id)->vertex_ids[0],
                        welded->edge(edge.id)->vertex_ids[1]);
              EXPECT_EQ(welded->uses_for_edge(edge.id).size(),
                        original->uses_for_edge(edge.id).size());
            }
          }
        }
      }
    }
  }
}

TEST_F(NurbsTopologyEditTest, CoincidentWeldingRetainsDifferentCurvedInteriors)
{
  auto source = planar_grid(1, 1);
  const int64_t face = static_cast<const NurbsTopologyFace *>(source->faces.first)->id;
  auto duplicate = duplicate_components(*source, {{}, {}, {&face, 1}});
  ASSERT_TRUE(duplicate.has_value());
  const int64_t copied_surface = duplicate->source_to_copy.lookup(
      static_cast<const NurbsTopologyFace *>(source->faces.first)->surface_id);
  for (auto &surface : duplicate->geometry->surfaces) {
    if (surface.id != copied_surface) {
      continue;
    }
    MEM_delete(surface.points);
    MEM_delete(surface.knots_u);
    MEM_delete(surface.knots_v);
    surface.order_u = surface.order_v = surface.points_u = surface.points_v = 3;
    surface.points_num = 9;
    surface.knots_u_num = surface.knots_v_num = 6;
    surface.points = MEM_new_array<NurbsControlPoint>(9, __func__);
    surface.knots_u = MEM_new_array_zeroed<double>(6, __func__);
    surface.knots_v = MEM_new_array_zeroed<double>(6, __func__);
    for (int i = 3; i < 6; i++) {
      surface.knots_u[i] = surface.knots_v[i] = 1;
    }
    for (int v = 0; v < 3; v++) {
      for (int u = 0; u < 3; u++) {
        const double3 point(u * 0.5, v * 0.5, u == 1 && v == 1 ? 0.25 : 0);
        std::copy_n(&point.x, 3, surface.points[v * 3 + u].position);
      }
    }
  }
  SelectionState initial;
  for (const auto &vertex : duplicate->geometry->vertices) {
    initial.vertices.add(vertex.id);
  }
  const auto selected = selection_flush(*duplicate->geometry, initial, initial.mode);
  ASSERT_TRUE(selected.has_value());
  std::string error;
  const auto welded = merge_by_distance(
      *duplicate->geometry, *selected, 0, false, false, 0, &error);
  ASSERT_TRUE(welded.has_value()) << error;
  EXPECT_EQ(welded->geometry->vertices.count(), 4);
  EXPECT_EQ(welded->geometry->edges.count(), 4);
  EXPECT_EQ(welded->geometry->faces.count(), 2);
  bool curved = false;
  for (const auto &surface : welded->geometry->surfaces) {
    if (surface.id == copied_surface) {
      EXPECT_EQ(surface.order_u, 3);
      EXPECT_EQ(surface.points[4].position[2], 0.25);
      curved = true;
    }
  }
  EXPECT_TRUE(curved);
  EXPECT_TRUE(tessellate_surfaces(*welded->geometry, preview_options(), &error)) << error;
}

TEST_F(NurbsTopologyEditTest, SavedMixedWorkflowsMatchMeshAndKeepSharedSourceIncidence)
{
  const char *directory = std::getenv("BLENDER_NURBS_COMPOSITION_OUTPUT");
  if (!directory) {
    GTEST_SKIP() << "Run nurbs_native_composition.py before checking its saved source states";
  }
  for (const int cuts : {1, 2}) {
    OwnedGeometry before_rejected, before_reload, before_no_weld;
    SelectionState selected_before_rejected, selected_before_reload;
    for (const char *stage : {"initial",
                              "delete-face",
                              "fill",
                              "duplicate-sheet",
                              "weld-sheet",
                              "loop-cut",
                              "subdivide-sheet",
                              "rejected-cut",
                              "extrude-region",
                              "delete-cap",
                              "duplicate-body",
                              "no-weld",
                              "subdivide-body",
                              "reload",
                              "merge-all",
                              "extrude-point",
                              "subdivide-wire",
                              "duplicate-wire",
                              "delete-edges"})
    {
      SCOPED_TRACE(cuts);
      SCOPED_TRACE(stage);
      const std::string suffix = std::to_string(cuts) + "-" + stage;
      const std::string path = std::string(directory) + "/composition-native-" + suffix + ".blend";
      ReportList reports;
      BKE_reports_init(&reports, RPT_STORE);
      BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
      BlendFileReadReport read_reports{};
      read_reports.reports = &reports;
      auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
      ASSERT_NE(file, nullptr);
      BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
      ASSERT_EQ(file->main->objects.count(), 1);
      const auto &curve = *id_cast<Curve *>(
          static_cast<Object *>(file->main->objects.first)->data);
      ASSERT_TRUE(curve.nurb.is_empty());
      ASSERT_NE(curve.nurbs_geometry, nullptr);
      const auto &geometry = *curve.nurbs_geometry;
      std::string error;
      const auto index = TopologyIndex::build(geometry, &error);
      ASSERT_TRUE(index.has_value()) << error;
      const auto selected = selection_from_storage(geometry.stored_selection, &error);
      ASSERT_TRUE(selected.has_value()) << error;
      const auto edit = EditData::create(geometry, *selected, &error);
      ASSERT_NE(edit, nullptr) << error;
      if (STREQ(stage, "subdivide-sheet")) {
        before_rejected.reset(geometry_copy(&geometry));
        selected_before_rejected = *selected;
      }
      if (STREQ(stage, "duplicate-body")) {
        before_no_weld.reset(geometry_copy(&geometry));
      }
      if (STREQ(stage, "no-weld")) {
        ASSERT_NE(before_no_weld, nullptr);
        expect_source_geometry_equal(geometry, *before_no_weld);
      }
      if (STREQ(stage, "subdivide-body")) {
        before_reload.reset(geometry_copy(&geometry));
        selected_before_reload = *selected;
      }
      if (STREQ(stage, "rejected-cut") || STREQ(stage, "reload")) {
        const bool rejected = STREQ(stage, "rejected-cut");
        const auto &previous = rejected ? before_rejected : before_reload;
        const auto &previous_selection = rejected ? selected_before_rejected :
                                                    selected_before_reload;
        ASSERT_NE(previous, nullptr);
        expect_source_geometry_equal(geometry, *previous);
        EXPECT_EQ(selected->vertices, previous_selection.vertices);
        EXPECT_EQ(selected->edges, previous_selection.edges);
        EXPECT_EQ(selected->faces, previous_selection.faces);
        EXPECT_EQ(selected->history, previous_selection.history);
        EXPECT_EQ(selected->active, previous_selection.active);
        EXPECT_EQ(selected->mode, previous_selection.mode);
      }
      SurfacePreviewOptions options;
      options.boundary.segments_per_span = options.interior_subdivisions = 7;
      EXPECT_TRUE(tessellate_surfaces(geometry, options, &error)) << error;
      compare_mesh_file("composition-mesh-" + suffix, geometry, 1e-6, directory);
      Set<int64_t> visited;
      int components = 0;
      for (const auto &vertex : geometry.vertices) {
        if (visited.contains(vertex.id)) {
          continue;
        }
        const auto linked = find_linked_components(
            geometry, {{&vertex.id, 1}, {}, {}}, LinkedConnectivity::VertexGraph, &error);
        ASSERT_TRUE(linked.has_value()) << error;
        visited.add_multiple(linked->vertices);
        components++;
      }
      const bool two = STREQ(stage, "duplicate-sheet") || STREQ(stage, "duplicate-body") ||
                       STREQ(stage, "no-weld") || STREQ(stage, "subdivide-body") ||
                       STREQ(stage, "reload") || STREQ(stage, "duplicate-wire");
      EXPECT_EQ(components, STREQ(stage, "delete-edges") ? 0 : two ? 2 : 1);
      for (const auto &edge : geometry.edges) {
        const auto uses = index->uses_for_edge(edge.id);
        EXPECT_LE(uses.size(), 2);
        if (uses.size() == 2) {
          EXPECT_EQ(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                    -index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
        }
      }
      for (const auto &vertex : geometry.vertices) {
        EXPECT_LE(vertex.tolerance, 1e-9);
      }
      for (const auto &face : geometry.faces) {
        EXPECT_LE(face.tolerance, 1e-9);
      }
    }
  }
}

TEST_F(NurbsTopologyEditTest, SavedPyramidMovementMatchesMeshAndPreservesTrimIdentities)
{
  const char *directory = std::getenv("BLENDER_NURBS_TRIANGLE_OUTPUT");
  if (!directory) {
    GTEST_SKIP() << "Run the pyramid_movement UI workflows before checking their source states";
  }
  for (const char *label : {"native", "box"}) {
    OwnedGeometry initial, apex, extruded;
    for (const char *stage : {"initial",
                              "apex-move",
                              "apex-restored",
                              "base-move",
                              "edge-transform",
                              "face-move",
                              "extrude",
                              "reload",
                              "fresh-move"})
    {
      SCOPED_TRACE(label);
      SCOPED_TRACE(stage);
      ReportList reports;
      BKE_reports_init(&reports, RPT_STORE);
      BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
      BlendFileReadReport read_reports{};
      read_reports.reports = &reports;
      const std::string path = std::string(directory) + "/triangle-" + label + "-" + stage +
                               ".blend";
      auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
      ASSERT_NE(file, nullptr);
      BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
      ASSERT_EQ(file->main->objects.count(), 1);
      const auto &curve = *id_cast<Curve *>(
          static_cast<Object *>(file->main->objects.first)->data);
      ASSERT_TRUE(curve.nurb.is_empty());
      ASSERT_NE(curve.nurbs_geometry, nullptr);
      const auto &geometry = *curve.nurbs_geometry;
      std::string error;
      const auto index = TopologyIndex::build(geometry, &error);
      ASSERT_TRUE(index.has_value()) << error;
      SurfacePreviewOptions options;
      options.boundary.segments_per_span = options.interior_subdivisions = 7;
      EXPECT_TRUE(tessellate_surfaces(geometry, options, &error)) << error;
      compare_mesh_file(std::string("triangle-mesh-") + stage, geometry, 1e-6, directory);
      if (STREQ(stage, "initial")) {
        initial.reset(geometry_copy(&geometry));
      }
      if (STREQ(stage, "apex-move")) {
        apex.reset(geometry_copy(&geometry));
      }
      if (STREQ(stage, "apex-restored")) {
        ASSERT_NE(apex, nullptr);
        expect_source_geometry_equal(geometry, *apex);
      }
      if (!STREQ(stage, "extrude") && !STREQ(stage, "reload") && !STREQ(stage, "fresh-move")) {
        ASSERT_NE(initial, nullptr);
        EXPECT_EQ(geometry.next_id, initial->next_id);
        for (const auto &face : initial->faces) {
          const auto *after = index->face(face.id);
          ASSERT_NE(after, nullptr);
          EXPECT_EQ(after->surface_id, face.surface_id);
          EXPECT_EQ(after->orientation, face.orientation);
          const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
          const auto &kept = *static_cast<const NurbsTopologyLoop *>(after->loops.first);
          EXPECT_EQ(loop.id, kept.id);
          const auto *use = static_cast<const NurbsTopologyEdgeUse *>(kept.edge_uses.first);
          for (const auto &old : loop.edge_uses) {
            ASSERT_NE(use, nullptr);
            EXPECT_EQ(use->id, old.id);
            EXPECT_EQ(use->edge_id, old.edge_id);
            EXPECT_EQ(use->pcurve_id, old.pcurve_id);
            EXPECT_EQ(use->reversed, old.reversed);
            use = use->next;
          }
        }
      }
      if (STREQ(stage, "extrude")) {
        extruded.reset(geometry_copy(&geometry));
      }
      if (STREQ(stage, "reload")) {
        ASSERT_NE(extruded, nullptr);
        expect_source_geometry_equal(geometry, *extruded);
      }
      if (STREQ(stage, "fresh-move")) {
        ASSERT_NE(extruded, nullptr);
        const auto selected = selection_from_storage(extruded->stored_selection, &error);
        ASSERT_TRUE(selected.has_value()) << error;
        Vector<int64_t> ids;
        for (const int64_t id : selected->vertices) {
          ids.append(id);
        }
        const auto resumed = translate_vertices(*extruded, ids, {0.125, 0, 0}, 1e-9, &error);
        ASSERT_TRUE(resumed.has_value()) << error;
        expect_source_geometry_equal(geometry, *resumed->geometry);
      }
    }
  }
}

TEST_F(NurbsTopologyEditTest, RefinementPreservesRationalArcsAndExactRuledCircleRows)
{
  auto geometry = make_arc_sheet(true);
  ASSERT_NE(geometry, nullptr);
  const auto &surface = *static_cast<NurbsSurfaceGeometry *>(geometry->surfaces.first);
  const auto &circle = *static_cast<NurbsCurveGeometry *>(geometry->curves.first);
  std::string error;
  auto row = surface_isocurve(surface, 1, 0.5, 0, &error);
  ASSERT_TRUE(row.has_value()) << error;
  EXPECT_EQ(row->geometry_error_bound, 0);
  ASSERT_EQ(row->curve->points_num, circle.points_num);
  ASSERT_EQ(row->curve->knots_num, circle.knots_num);
  EXPECT_EQ(std::memcmp(row->curve->knots, circle.knots, sizeof(double) * circle.knots_num), 0);
  for (int i = 0; i < circle.points_num; i++) {
    EXPECT_EQ(row->curve->points[i].weight, circle.points[i].weight);
    EXPECT_EQ(row->curve->points[i].position[0], circle.points[i].position[0]);
    EXPECT_EQ(row->curve->points[i].position[1], circle.points[i].position[1]);
    EXPECT_EQ(row->curve->points[i].position[2], 1);
  }
  auto arbitrary_row = surface_isocurve(surface, 1, 0.3, 1e-10, &error);
  ASSERT_TRUE(arbitrary_row.has_value()) << error;
  for (int i = 0; i < circle.points_num; i++) {
    EXPECT_EQ(arbitrary_row->curve->points[i].weight, circle.points[i].weight);
    EXPECT_EQ(arbitrary_row->curve->points[i].position[0], circle.points[i].position[0]);
    EXPECT_EQ(arbitrary_row->curve->points[i].position[1], circle.points[i].position[1]);
    EXPECT_EQ(arbitrary_row->curve->points[i].position[2],
              arbitrary_row->curve->points[0].position[2]);
    EXPECT_LE(std::abs(arbitrary_row->curve->points[i].position[2] - 0.6),
              arbitrary_row->geometry_error_bound);
  }
  auto top = restrict_surface(surface, {0, 0.5}, {4, 1}, 0, &error);
  ASSERT_TRUE(top.has_value()) << error;
  EXPECT_EQ(top->geometry_error_bound, 0);
  EXPECT_EQ(top->surface->knots_v[top->surface->order_v - 1], 0.5);
  EXPECT_EQ(top->surface->knots_v[top->surface->points_v], 1);
  for (int i = 0; i < top->surface->points_u; i++) {
    EXPECT_EQ(top->surface->points[i].position[2], 1);
    EXPECT_EQ(top->surface->points[top->surface->points_u + i].position[2], 2);
    EXPECT_EQ(top->surface->points[i].weight, surface.points[i].weight);
  }
  for (const double2 range : {double2(0, 1), double2(0.25, 0.75), double2(1, 3)}) {
    auto piece = restrict_curve(circle, range, 1e-10, &error);
    ASSERT_TRUE(piece.has_value()) << error;
    EXPECT_EQ(piece->curve->order, circle.order);
    EXPECT_EQ(piece->curve->knots[piece->curve->order - 1], range.x);
    EXPECT_EQ(piece->curve->knots[piece->curve->points_num], range.y);
    for (int i = 0; i <= 64; i++) {
      const double parameter = range.x + (range.y - range.x) * (i / 64.0);
      auto a = evaluate_curve_bounded(circle, parameter, 1e-10, &error);
      auto b = evaluate_curve_bounded(*piece->curve, parameter, 1e-10, &error);
      ASSERT_TRUE(a.has_value()) << error;
      ASSERT_TRUE(b.has_value()) << error;
      const double3 delta = a->position - b->position;
      EXPECT_LE(std::hypot(delta.x, delta.y, delta.z),
                piece->geometry_error_bound + a->error_bound + b->error_bound);
    }
  }
  EXPECT_EQ(surface.knots_v[1], 0);
  EXPECT_EQ(surface.points[0].position[2], 0);
}

TEST_F(NurbsTopologyEditTest, RefinementClampsNonuniformNonclampedCurvesWithoutChangingTheSource)
{
  NurbsCurveGeometry curve;
  curve.order = 3;
  curve.points_num = 4;
  curve.knots_num = 7;
  NurbsControlPoint points[4];
  for (int i = 0; i < 4; i++) {
    points[i].position[0] = i;
    points[i].position[1] = (i & 1) ? 1 : 0;
    points[i].weight = i + 1;
  }
  double knots[7] = {-2, -1, 0, 1, 2, 3, 4};
  curve.points = points;
  curve.knots = knots;
  std::string error;
  auto piece = restrict_curve(curve, {0.25, 1.75}, 1e-10, &error);
  ASSERT_TRUE(piece.has_value()) << error;
  for (int i = 0; i <= 64; i++) {
    const double parameter = 0.25 + 1.5 * (i / 64.0);
    const auto a = evaluate_curve_bounded(curve, parameter, 1e-10, &error);
    const auto b = evaluate_curve_bounded(*piece->curve, parameter, 1e-10, &error);
    ASSERT_TRUE(a.has_value()) << error;
    ASSERT_TRUE(b.has_value()) << error;
    const double3 delta = a->position - b->position;
    EXPECT_LE(std::hypot(delta.x, delta.y, delta.z),
              piece->geometry_error_bound + a->error_bound + b->error_bound);
  }
  EXPECT_EQ(curve.points, points);
  EXPECT_EQ(curve.knots, knots);
  EXPECT_EQ(curve.points_num, 4);
  EXPECT_EQ(knots[0], -2);
}

TEST_F(NurbsTopologyEditTest,
       BoundedEvaluationCoversIndependentRationalBezierAndRejectsInsufficientBudget)
{
  auto sheet = make_arc_sheet(false);
  ASSERT_NE(sheet, nullptr);
  const auto &curve = *static_cast<NurbsCurveGeometry *>(sheet->curves.first);
  std::string error;
  for (int i = 0; i <= 64; i++) {
    const double parameter = i / 64.0;
    const auto point = evaluate_curve_bounded(curve, parameter, 1e-10, &error);
    ASSERT_TRUE(point.has_value()) << error;
    const long double t = parameter, a = (1 - t) * (1 - t), b = 2 * t * (1 - t), c = t * t;
    const long double w = curve.points[1].weight;
    const long double denominator = a + b * w + c;
    const long double expected_x = (a + b * w) / denominator;
    const long double expected_y = (b * w + c) / denominator;
    const long double measured =
        std::abs(static_cast<long double>(point->position.x) - expected_x) +
        std::abs(static_cast<long double>(point->position.y) - expected_y);
    EXPECT_LE(measured,
              static_cast<long double>(point->error_bound) +
                  8 * std::numeric_limits<long double>::epsilon());
  }
  EXPECT_FALSE(evaluate_curve_bounded(curve, 0.5, 0, &error));
  EXPECT_NE(error.find("budget"), std::string::npos);
  EXPECT_FALSE(restrict_curve(curve, {-1, 1}, 1e-10, &error));
  EXPECT_FALSE(restrict_curve(curve, {0.5, 0.5}, 1e-10, &error));
  EXPECT_FALSE(restrict_curve(curve, {0, 1}, -1, &error));
  const auto &surface = *static_cast<NurbsSurfaceGeometry *>(sheet->surfaces.first);
  EXPECT_FALSE(surface_isocurve(surface, 7, 0.5, 1e-10, &error));
}

TEST_F(NurbsTopologyEditTest, LoopCutsCreateSharedEditableSubpatchesAndPreserveTheSurface)
{
  auto geometry = planar_grid(1, 1);
  ASSERT_NE(geometry, nullptr);
  const auto &seed = *static_cast<NurbsTopologyEdge *>(geometry->edges.first);
  const int64_t counter = geometry->next_id;
  std::string error;
  SelectionState initial;
  initial.mode = SelectionMode::Face;
  const ElementRef face_ref{ElementDomain::Face,
                            static_cast<NurbsTopologyFace *>(geometry->faces.first)->id};
  auto selected = selection_apply(
      *geometry, initial, {&face_ref, 1}, SelectionAction::Set, face_ref);
  ASSERT_TRUE(selected.has_value());
  write_source(std::string("cut-api-source-") + std::to_string(seed.id), *geometry, *selected);
  for (const double factor : {0.0, 0.25}) {
    auto plan = plan_loop_cut(*geometry, seed.id, 1, factor, 0, &error);
    ASSERT_TRUE(plan.has_value()) << error;
    auto cut = partition_components(*geometry, *plan, 0, &error);
    ASSERT_TRUE(cut.has_value()) << error;
    EXPECT_EQ(cut->edit.geometry->vertices.count(), 6);
    EXPECT_EQ(cut->edit.geometry->edges.count(), 7);
    EXPECT_EQ(cut->edit.geometry->faces.count(), 2);
    EXPECT_EQ(cut->edit.geometry_error_bound, 0);
    ASSERT_EQ(cut->edit.edges.size(), 1);
    const auto index = TopologyIndex::build(*cut->edit.geometry);
    ASSERT_TRUE(index.has_value());
    const auto uses = index->uses_for_edge(cut->edit.edges[0]);
    ASSERT_EQ(uses.size(), 2);
    EXPECT_NE(uses[0].face_id, uses[1].face_id);
    ASSERT_EQ(cut->edge_descendants.lookup(seed.id).size(), 2);
    EXPECT_EQ(cut->edge_descendants.lookup(seed.id)[0], seed.id);
    const int64_t source_face = static_cast<NurbsTopologyFace *>(geometry->faces.first)->id;
    EXPECT_EQ(cut->face_descendants.lookup(source_face).size(), 2);
    EXPECT_EQ(cut->face_descendants.lookup(source_face)[0], source_face);
    const auto preview = tessellate_surfaces(*cut->edit.geometry, preview_options(), &error);
    ASSERT_TRUE(preview.has_value()) << error;
    for (const auto &surface : cut->edit.geometry->surfaces) {
      if (surface.id == static_cast<NurbsSurfaceGeometry *>(geometry->surfaces.first)->id) {
        continue;
      }
      const double2 uv(
          (surface.knots_u[surface.order_u - 1] + surface.knots_u[surface.points_u]) * 0.5,
          (surface.knots_v[surface.order_v - 1] + surface.knots_v[surface.points_v]) * 0.5);
      const auto before = evaluate_surface(
          *static_cast<NurbsSurfaceGeometry *>(geometry->surfaces.first), uv, &error);
      const auto after = evaluate_surface(surface, uv, &error);
      ASSERT_TRUE(before.has_value());
      ASSERT_TRUE(after.has_value());
      EXPECT_EQ(before->position, after->position);
    }
    // Newly cut endpoints are normal topology handles, and their incident subpatches agree.
    const Vector<int64_t> moving = cut->edit.vertices;
    auto moved = translate_vertices(*cut->edit.geometry, moving, {0, 0, 0.25}, 0, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
  }
  EXPECT_EQ(geometry->next_id, counter);
  EXPECT_EQ(geometry->vertices.count(), 4);
  EXPECT_EQ(geometry->faces.count(), 1);
}

TEST_F(NurbsTopologyEditTest, SubdivisionUsesRealSharedGridTopologyAndMovesItsCenter)
{
  auto geometry = planar_grid(1, 1);
  ASSERT_NE(geometry, nullptr);
  const int64_t face = static_cast<NurbsTopologyFace *>(geometry->faces.first)->id;
  std::string error;
  auto result = subdivide_components(*geometry, {{}, {}, {&face, 1}}, 1, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->geometry->vertices.count(), 9);
  EXPECT_EQ(result->geometry->edges.count(), 12);
  EXPECT_EQ(result->geometry->faces.count(), 4);
  EXPECT_EQ(result->vertices.size(), 9);
  EXPECT_EQ(result->edges.size(), 12);
  EXPECT_EQ(result->faces.size(), 4);
  int64_t center = 0;
  for (const auto &vertex : result->geometry->vertices) {
    if (double3(vertex.position) == double3(0.5, 0.5, 0)) {
      center = vertex.id;
    }
  }
  ASSERT_NE(center, 0);
  auto moved = translate_vertices(*result->geometry, {&center, 1}, {0, 0, 0.25}, 0, &error);
  ASSERT_TRUE(moved.has_value()) << error;
  ASSERT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
  const auto index = TopologyIndex::build(*moved->geometry);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->edges_for_vertex(center).size(), 4);
  EXPECT_EQ(index->vertex(center)->position[2], 0.25);
  for (const auto &vertex : geometry->vertices) {
    EXPECT_EQ(vertex.position[2], 0);
  }
  auto two = subdivide_components(*geometry, {{}, {}, {&face, 1}}, 2, 1e-9, &error);
  ASSERT_TRUE(two.has_value()) << error;
  EXPECT_EQ(two->geometry->vertices.count(), 16);
  EXPECT_EQ(two->geometry->edges.count(), 24);
  EXPECT_EQ(two->geometry->faces.count(), 9);
  EXPECT_LE(two->geometry_error_bound, 1e-9);
  EXPECT_TRUE(tessellate_surfaces(*two->geometry, preview_options(), &error)) << error;
}

TEST_F(NurbsTopologyEditTest, CutsPreserveExactRationalCylinderCirclesAndPeriodicSharedUses)
{
  auto geometry = make_arc_sheet(true);
  ASSERT_NE(geometry, nullptr);
  const auto source_index = TopologyIndex::build(*geometry);
  int64_t seam = 0;
  for (const auto &edge : geometry->edges) {
    if (source_index->uses_for_edge(edge.id).size() == 2) {
      seam = edge.id;
    }
  }
  ASSERT_NE(seam, 0);
  std::string error;
  auto plan = plan_loop_cut(*geometry, seam, 1, 0, 0, &error);
  ASSERT_TRUE(plan.has_value()) << error;
  auto result = partition_components(*geometry, *plan, 0, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->edit.geometry->vertices.count(), 3);
  EXPECT_EQ(result->edit.geometry->edges.count(), 5);
  EXPECT_EQ(result->edit.geometry->faces.count(), 2);
  EXPECT_EQ(result->edit.geometry_error_bound, 0);
  ASSERT_EQ(result->edit.edges.size(), 1);
  const auto index = TopologyIndex::build(*result->edit.geometry);
  ASSERT_TRUE(index.has_value());
  const auto &ring = *index->edge(result->edit.edges[0]);
  EXPECT_EQ(ring.vertex_ids[0], ring.vertex_ids[1]);
  EXPECT_EQ(index->uses_for_edge(ring.id).size(), 2);
  const auto *curve = static_cast<NurbsCurveGeometry *>(result->edit.geometry->curves.first);
  while (curve && curve->id != ring.curve_id) {
    curve = curve->next;
  }
  ASSERT_NE(curve, nullptr);
  EXPECT_EQ(curve->order, 3);
  EXPECT_EQ(curve->points_num, 9);
  const auto &source_curve = *static_cast<NurbsCurveGeometry *>(geometry->curves.first);
  for (int i = 0; i < 9; i++) {
    EXPECT_EQ(curve->points[i].weight, source_curve.points[i].weight);
    EXPECT_EQ(curve->points[i].position[0], source_curve.points[i].position[0]);
    EXPECT_EQ(curve->points[i].position[1], source_curve.points[i].position[1]);
    EXPECT_EQ(curve->points[i].position[2], 1);
  }
  EXPECT_TRUE(tessellate_surfaces(*result->edit.geometry, preview_options(), &error)) << error;
  auto moved = translate_vertices(
      *result->edit.geometry, result->edit.vertices, {0, 0, 0.25}, 0, &error);
  ASSERT_TRUE(moved.has_value()) << error;
  EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
  const int64_t face = static_cast<NurbsTopologyFace *>(geometry->faces.first)->id;
  auto subdivided = subdivide_components(*geometry, {{}, {}, {&face, 1}}, 1, 0, &error);
  ASSERT_TRUE(subdivided.has_value()) << error;
  EXPECT_TRUE(tessellate_surfaces(*subdivided->geometry, preview_options(), &error)) << error;
}

TEST_F(NurbsTopologyEditTest, CutsRejectUnsupportedSelectionsAndIdentityExhaustionAtomically)
{
  auto geometry = planar_grid(1, 1);
  ASSERT_NE(geometry, nullptr);
  const auto &first = *static_cast<NurbsTopologyEdge *>(geometry->edges.first);
  std::string error;
  EXPECT_FALSE(plan_loop_cut(*geometry, first.id, 0, 0, 0, &error));
  EXPECT_FALSE(plan_loop_cut(*geometry, first.id, 1, 1, 0, &error));
  EXPECT_FALSE(plan_loop_cut(*geometry, 99999, 1, 0, 0, &error));
  const int64_t adjacent[] = {first.id, first.next->id};
  EXPECT_FALSE(subdivide_components(*geometry, {{}, {adjacent, 2}, {}}, 1, 1e-9, &error));
  EXPECT_NE(error.find("Adjacent"), std::string::npos);
  TopologyCutPlan invalid;
  invalid.edges.append({first.id, {NAN}});
  EXPECT_FALSE(partition_components(*geometry, invalid, 1e-9, &error));
  auto plan = plan_loop_cut(*geometry, first.id, 1, 0, 0, &error);
  ASSERT_TRUE(plan.has_value());
  geometry->next_id = std::numeric_limits<int64_t>::max();
  EXPECT_FALSE(partition_components(*geometry, *plan, 0, &error));
  EXPECT_EQ(geometry->vertices.count(), 4);
  EXPECT_EQ(geometry->edges.count(), 4);
  EXPECT_EQ(geometry->faces.count(), 1);
}

TEST_F(NurbsTopologyEditTest, CutsCrossSharedFaceStripsAndCloseAroundAnExtrudedBox)
{
  std::string error;
  auto strip = planar_grid(1, 2, {-1, 0, 0}, {2, 2});
  ASSERT_NE(strip, nullptr);
  const int64_t seed = static_cast<NurbsTopologyEdge *>(strip->edges.first)->id;
  for (const int count : {1, 2}) {
    auto plan = plan_loop_cut(*strip, seed, count, 0, 1e-9, &error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->faces.size(), 2);
    auto result = partition_components(*strip, *plan, 1e-9, &error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(result->edit.geometry->vertices.count(), 3 * (count + 2));
    EXPECT_EQ(result->edit.geometry->edges.count(), 7 + 5 * count);
    EXPECT_EQ(result->edit.geometry->faces.count(), 2 * (count + 1));
    const auto index = TopologyIndex::build(*result->edit.geometry);
    ASSERT_TRUE(index.has_value());
    for (const int64_t id : result->edit.edges) {
      EXPECT_EQ(index->uses_for_edge(id).size(), 2);
    }
    EXPECT_TRUE(tessellate_surfaces(*result->edit.geometry, preview_options(), &error)) << error;
  }
  auto sheet = planar_grid(1, 1);
  const int64_t bottom = static_cast<NurbsTopologyFace *>(sheet->faces.first)->id;
  auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 1}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  const auto box_index = TopologyIndex::build(*box->geometry);
  int64_t upright = 0;
  for (const auto &edge : box->geometry->edges) {
    if (box_index->vertex(edge.vertex_ids[0])->position[2] !=
        box_index->vertex(edge.vertex_ids[1])->position[2])
    {
      upright = edge.id;
      break;
    }
  }
  ASSERT_NE(upright, 0);
  auto plan = plan_loop_cut(*box->geometry, upright, 1, 0, 0, &error);
  ASSERT_TRUE(plan.has_value()) << error;
  EXPECT_EQ(plan->faces.size(), 4);
  auto cut = partition_components(*box->geometry, *plan, 0, &error);
  ASSERT_TRUE(cut.has_value()) << error;
  EXPECT_EQ(cut->edit.geometry->vertices.count(), 12);
  EXPECT_EQ(cut->edit.geometry->edges.count(), 20);
  EXPECT_EQ(cut->edit.geometry->faces.count(), 10);
  const auto index = TopologyIndex::build(*cut->edit.geometry);
  ASSERT_TRUE(index.has_value());
  for (const auto &edge : cut->edit.geometry->edges) {
    EXPECT_EQ(index->uses_for_edge(edge.id).size(), 2);
  }
  EXPECT_TRUE(tessellate_surfaces(*cut->edit.geometry, preview_options(), &error)) << error;
  auto moved = translate_vertices(
      *cut->edit.geometry, cut->edit.vertices, {0, 0, 0.125}, 0, &error);
  ASSERT_TRUE(moved.has_value()) << error;
  EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
  const int64_t top = box->source_to_cap.lookup(bottom);
  auto subdivided = subdivide_components(*box->geometry, {{}, {}, {&top, 1}}, 1, 0, &error);
  ASSERT_TRUE(subdivided.has_value()) << error;
  EXPECT_EQ(subdivided->geometry->vertices.count(), 13);
  EXPECT_EQ(subdivided->geometry->edges.count(), 20);
  EXPECT_EQ(subdivided->geometry->faces.count(), 9);
  const auto subdivided_index = TopologyIndex::build(*subdivided->geometry);
  for (const auto &edge : subdivided->geometry->edges) {
    EXPECT_EQ(subdivided_index->uses_for_edge(edge.id).size(), 2);
  }
  EXPECT_TRUE(tessellate_surfaces(*subdivided->geometry, preview_options(), &error)) << error;
}

TEST_F(NurbsTopologyEditTest, SavedCutsMatchActualMeshAndRemainAuthoritativeNurbs)
{
  if (!std::getenv("BLENDER_NURBS_CUT_UI_READY")) {
    GTEST_SKIP() << "Run after the native cut and mesh-reference viewport workflows";
  }
  const char *directory = std::getenv("BLENDER_NURBS_TOPOLOGY_OUTPUT");
  ASSERT_NE(directory, nullptr);
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  for (const char *label : {"CENTER", "SLIDE", "MULTI", "TAGGED", "SUBDIVIDE"}) {
    SCOPED_TRACE(label);
    const bool subdivide = STREQ(label, "SUBDIVIDE");
    const std::string path = std::string(directory) +
                             (subdivide ? "/subdivide-native-FULL.blend" :
                                          std::string("/loopcut-native-") + label + ".blend");
    auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
    ASSERT_NE(file, nullptr);
    BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
    ASSERT_EQ(file->main->objects.count(), 1);
    const auto &object = *static_cast<Object *>(file->main->objects.first);
    const auto &curve = *id_cast<const Curve *>(object.data);
    ASSERT_TRUE(curve.nurb.is_empty());
    ASSERT_NE(curve.nurbs_geometry, nullptr);
    std::string error;
    EXPECT_TRUE(geometry_validate_structure(*curve.nurbs_geometry, &error)) << error;
    auto selection = selection_from_storage(curve.nurbs_geometry->stored_selection, &error);
    ASSERT_TRUE(selection.has_value()) << error;
    auto edit = EditData::create(*curve.nurbs_geometry, *selection, &error);
    ASSERT_NE(edit, nullptr) << error;
    const SelectionMode expected_mode = (STREQ(label, "MULTI") || STREQ(label, "TAGGED")) ?
                                            SelectionMode::Vertex :
                                        subdivide            ? SelectionMode::Face :
                                        STREQ(label, "FACE") ? SelectionMode::Face :
                                                               SelectionMode::Edge;
    EXPECT_EQ(selection->mode, expected_mode);
    switch (selection->mode) {
      case SelectionMode::Vertex:
        EXPECT_FALSE(selection->vertices.is_empty());
        break;
      case SelectionMode::Edge:
        EXPECT_FALSE(selection->edges.is_empty());
        break;
      case SelectionMode::Face:
        EXPECT_FALSE(selection->faces.is_empty());
        break;
    }
    Set<int64_t> moving_set;
    for (const int64_t id : selection->vertices) {
      moving_set.add(id);
    }
    const auto topology = TopologyIndex::build(edit->geometry());
    ASSERT_TRUE(topology.has_value());
    for (const int64_t id : selection->edges) {
      for (const int64_t vertex : topology->edge(id)->vertex_ids) {
        moving_set.add(vertex);
      }
    }
    for (const int64_t id : selection->faces) {
      for (const auto &loop : topology->face(id)->loops) {
        for (const auto &use : loop.edge_uses) {
          for (const int64_t vertex : topology->edge(use.edge_id)->vertex_ids) {
            moving_set.add(vertex);
          }
        }
      }
    }
    const Vector<int64_t> moving(moving_set.begin(), moving_set.end());
    auto moved = translate_vertices(edit->geometry(), moving, {0, 0, 0.25}, 1e-9, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    EXPECT_TRUE(tessellate_surfaces(*moved->geometry, preview_options(), &error)) << error;
    compare_mesh_file(std::string("cut-mesh-") + label, *curve.nurbs_geometry, 1e-6);
  }
}

}  // namespace blender::bke::nurbs::tests
