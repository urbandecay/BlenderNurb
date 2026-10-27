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
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_refine.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_nurbs_transform.hh"
#include "BKE_report.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

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

static std::pair<OwnedGeometry, int64_t> bevel_fixture(const bool closed)
{
  auto sheet = make_planar_sheet();
  if (!sheet) {
    return {};
  }
  int64_t corner = 0;
  for (const auto &edge : sheet->edges) {
    const auto index = TopologyIndex::build(*sheet);
    if (index->vertex(edge.vertex_ids[0])->position[1] == 1 &&
        index->vertex(edge.vertex_ids[1])->position[1] == 1)
    {
      corner = edge.id;
    }
  }
  if (closed) {
    const int64_t face = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
    auto box = extrude_components(*sheet, {{}, {}, {&face, 1}}, {0, 0, 2}, 0);
    return {box ? std::move(box->geometry) : nullptr, corner};
  }
  auto bent = extrude_edges(*sheet, {&corner, 1}, {0, 0, 2}, 0);
  return {bent ? std::move(bent->geometry) : nullptr, corner};
}

static TopologySignature bevel_bmesh_reference(const NurbsGeometry &source,
                                               const Span<int64_t> selected,
                                               const double width,
                                               const bool clamp_overlap = false,
                                               const int segments = 1)
{
  const BMeshCreateParams parameters{.use_toolflags = true};
  BMesh *mesh = BM_mesh_create(&bm_mesh_allocsize_default, &parameters);
  BLI_SCOPED_DEFER([&]() { BM_mesh_free(mesh); });
  Map<int64_t, BMVert *> vertices;
  const auto index = TopologyIndex::build(source);
  for (const auto &vertex : source.vertices) {
    const float3 point(double3(vertex.position));
    vertices.add_new(vertex.id, BM_vert_create(mesh, point, nullptr, BM_CREATE_NOP));
  }
  for (const auto &edge : source.edges) {
    auto *created = BM_edge_create(mesh,
                                   vertices.lookup(edge.vertex_ids[0]),
                                   vertices.lookup(edge.vertex_ids[1]),
                                   nullptr,
                                   BM_CREATE_NOP);
    BM_elem_flag_set(created, BM_ELEM_SELECT, selected.contains(edge.id));
  }
  for (const auto &face : source.faces) {
    Vector<BMVert *> boundary;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      boundary.append(vertices.lookup(edge.vertex_ids[use.reversed ? 1 : 0]));
    }
    if (face.orientation < 0) {
      std::reverse(boundary.begin(), boundary.end());
    }
    BM_face_create_verts(mesh, boundary.data(), boundary.size(), nullptr, BM_CREATE_NOP, true);
  }
  BM_mesh_normals_update(mesh);
  BMOperator op;
  BMO_op_initf(mesh,
               &op,
               BMO_FLAG_DEFAULTS,
               "bevel geom=%he offset=%f segments=%i affect=%i offset_type=%i profile=%f "
               "clamp_overlap=%b loop_slide=%b",
               BM_ELEM_SELECT,
               width,
               segments,
               BEVEL_AFFECT_EDGES,
               BEVEL_AMT_OFFSET,
               0.5,
               clamp_overlap,
               true);
  BMO_op_exec(mesh, &op);
  BMO_op_finish(mesh, &op);
  TopologySignature result;
  auto point = [](const BMVert *v) {
    return TopologySignature::Point{v->co[0], v->co[1], v->co[2]};
  };
  BMIter iter;
  BMVert *v;
  BM_ITER_MESH (v, &iter, mesh, BM_VERTS_OF_MESH) {
    result.vertices.push_back(point(v));
  }
  BMEdge *e;
  BM_ITER_MESH (e, &iter, mesh, BM_EDGES_OF_MESH) {
    result.edges.push_back({point(e->v1), point(e->v2)});
  }
  BMFace *f;
  BM_ITER_MESH (f, &iter, mesh, BM_FACES_OF_MESH) {
    std::vector<TopologySignature::Point> boundary;
    BMIter loop_iter;
    BMLoop *loop;
    BM_ITER_ELEM (loop, &loop_iter, f, BM_LOOPS_OF_FACE) {
      boundary.push_back(point(loop->v));
    }
    result.faces.push_back(std::move(boundary));
  }
  result.sort();
  return result;
}

static void expect_signature_near_mesh(const TopologySignature &a, const TopologySignature &b);

TEST_F(NurbsTopologyEditTest, BevelSurvivesOrdinaryFloatingPointRotation)
{
  auto [source, corner] = bevel_fixture(false);
  ASSERT_NE(source, nullptr);
  const double angle = 0.371;
  double4x4 rotation = double4x4::identity();
  rotation[0][0] = std::cos(angle);
  rotation[0][1] = -std::sin(angle);
  rotation[1][0] = std::sin(angle);
  rotation[1][1] = std::cos(angle);
  rotation.location() = double3(0.137, -0.283, 0.091);
  std::string error;
  auto transformed = transform_geometry(*source, rotation, 1e-9, &error);
  ASSERT_TRUE(transformed) << error;
  double4x4 skew = double4x4::identity();
  skew[0][1] = 0.137;
  skew[0][2] = -0.219;
  skew[1][0] = 0.371;
  skew[1][2] = 0.053;
  skew[2][0] = -0.127;
  skew[2][1] = 0.241;
  skew.location() = double3(-0.173, 0.311, 0.097);
  auto transformed_again = transform_geometry(*transformed->geometry, skew, 1e-9, &error);
  ASSERT_TRUE(transformed_again) << error;
  for (const NurbsGeometry *geometry :
       {transformed->geometry.get(), transformed_again->geometry.get()})
  {
    SCOPED_TRACE(geometry == transformed_again->geometry.get() ? "repeated transform" :
                                                                 "rotation");
    for (const double width : {0.031, 0.1, 0.25, 0.4, 0.73}) {
      SCOPED_TRACE(width);
      auto result = bevel_edges(*geometry, {&corner, 1}, {.offset = width}, &error);
      ASSERT_TRUE(result) << error;
      if (geometry == transformed->geometry.get()) {
        expect_signature_near_mesh(signature(*result->geometry),
                                   bevel_bmesh_reference(*geometry, {&corner, 1}, width));
      }
      auto options = preview_options();
      options.boundary.evaluation_allowance = 0;
      EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
    }
  }
}

TEST_F(NurbsTopologyEditTest, StraightCornerChamferMatchesActualBMeshAndPreservesSupports)
{
  for (const bool closed : {false, true}) {
    auto [source, corner] = bevel_fixture(closed);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(corner, 0);
    const OwnedGeometry before(geometry_copy(source.get()));
    std::string error;
    auto result = bevel_edges(*source, {&corner, 1}, {.offset = 0.25, .max_error = 0}, &error);
    ASSERT_TRUE(result) << error;
    expect_signature_equal(signature(*result->geometry),
                           bevel_bmesh_reference(*source, {&corner, 1}, 0.25));
    EXPECT_EQ(result->geometry_error_bound, 0);
    EXPECT_EQ(result->faces.size(), 1);
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
    for (const auto &surface : source->surfaces) {
      const auto *retained = static_cast<const NurbsSurfaceGeometry *>(
          result->geometry->surfaces.first);
      while (retained && retained->id != surface.id) {
        retained = retained->next;
      }
      ASSERT_NE(retained, nullptr);
      EXPECT_EQ(std::memcmp(retained->points,
                            surface.points,
                            sizeof(NurbsControlPoint) * surface.points_num),
                0);
    }
    expect_source_geometry_equal(*source, *before);
  }
}

TEST_F(NurbsTopologyEditTest, RoundedBevelIsSharedTangentRationalGeometry)
{
  for (const bool closed : {false, true}) {
    for (const int segments : {2, 3, 8}) {
      auto [source, corner] = bevel_fixture(closed);
      std::string error;
      auto result = bevel_edges(
          *source, {&corner, 1}, {.offset = 0.25, .segments = segments}, &error);
      ASSERT_TRUE(result) << error;
      EXPECT_EQ(result->faces.size(), segments);
      EXPECT_EQ(result->geometry->faces.count(), source->faces.count() + segments);
      const auto index = TopologyIndex::build(*result->geometry);
      ASSERT_TRUE(index);
      for (const auto &edge : result->geometry->edges) {
        if (closed) {
          EXPECT_EQ(index->uses_for_edge(edge.id).size(), 2);
        }
      }
      std::optional<double3> previous_normal;
      for (const int64_t id : result->faces) {
        const auto *face = index->face(id);
        const NurbsSurfaceGeometry *surface = nullptr;
        for (const auto &candidate : result->geometry->surfaces) {
          if (candidate.id == face->surface_id) {
            surface = &candidate;
          }
        }
        ASSERT_NE(surface, nullptr);
        EXPECT_EQ(surface->order_u, 3);
        const auto start = evaluate_surface_differential(*surface, {surface->knots_u[2], 0.5});
        const auto finish = evaluate_surface_differential(*surface, {surface->knots_u[3], 0.5});
        ASSERT_TRUE(start && start->normal && finish && finish->normal);
        if (previous_normal) {
          EXPECT_NEAR(math::dot(*previous_normal, *start->normal), 1, 1e-14);
        }
        else {
          EXPECT_NEAR(std::abs(start->normal->z), 1, 1e-14);
        }
        previous_normal = *finish->normal;
        for (int step = 0; step <= 10; step++) {
          const double u = surface->knots_u[2] +
                           (surface->knots_u[3] - surface->knots_u[2]) * step / 10;
          const auto sample = evaluate_surface(*surface, {u, 0.5}, &error);
          ASSERT_TRUE(sample) << error;
          // Corner is y=1,z=0. The quarter-circle center is y=.75,z=.25.
          EXPECT_NEAR(
              std::hypot(sample->position.y - 0.75, sample->position.z - 0.25), 0.25, 1e-14);
        }
      }
      ASSERT_TRUE(previous_normal);
      EXPECT_NEAR(std::abs(previous_normal->y), 1, 1e-14);
      auto options = preview_options();
      options.boundary.evaluation_allowance = 0;
      EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
      auto extrusion = extrude_components(
          *result->geometry, {{}, {}, result->faces}, {0, 0, 0.5}, 1e-9, &error);
      ASSERT_TRUE(extrusion) << error;
      EXPECT_EQ(extrusion->edge_to_face.size(), 2 * segments + 2);
      EXPECT_TRUE(tessellate_surfaces(*extrusion->geometry, options, &error)) << error;
    }
  }
}

TEST_F(NurbsTopologyEditTest, BevelFailuresLeaveSourceAndCounterUnchanged)
{
  auto [source, corner] = bevel_fixture(true);
  ASSERT_NE(source, nullptr);
  const OwnedGeometry before(geometry_copy(source.get()));
  std::string error;
  for (const double width : {-0.1, 2.0, double(INFINITY), double(NAN)}) {
    EXPECT_FALSE(
        bevel_edges(*source, {&corner, 1}, {.offset = width, .clamp_overlap = false}, &error));
    EXPECT_FALSE(error.empty());
    expect_source_geometry_equal(*source, *before);
  }
  const auto index = TopologyIndex::build(*source);
  int64_t neighbor = 0;
  for (const int64_t id : index->edges_for_vertex(index->edge(corner)->vertex_ids[0])) {
    if (id != corner) {
      neighbor = id;
    }
  }
  const int64_t joined[] = {corner, neighbor};
  EXPECT_TRUE(bevel_edges(*source, {joined, 2}, {.offset = 0.25}, &error)) << error;
  const int64_t missing = 0;
  EXPECT_FALSE(bevel_edges(*source, {&missing, 1}, {.offset = 0.25}, &error));
  expect_source_geometry_equal(*source, *before);
  source->next_id = std::numeric_limits<int64_t>::max();
  EXPECT_FALSE(bevel_edges(*source, {&corner, 1}, {.offset = 0.25}, &error));
  EXPECT_NE(error.find("exhausted"), std::string::npos);
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

TEST_F(NurbsTopologyEditTest, MultipleSeparateBevelsShareFacesAndMatchBMesh)
{
  auto [source, corner] = bevel_fixture(true);
  Vector<int64_t> selected{corner};
  const auto index = TopologyIndex::build(*source);
  for (const auto &edge : source->edges) {
    const double3 a(index->vertex(edge.vertex_ids[0])->position);
    const double3 b(index->vertex(edge.vertex_ids[1])->position);
    if (a.y == -1 && b.y == -1 && a.z == 0 && b.z == 0) {
      selected.append(edge.id);
    }
  }
  ASSERT_EQ(selected.size(), 2);
  std::string error;
  auto result = bevel_edges(*source, selected, {.offset = 0.25}, &error);
  ASSERT_TRUE(result) << error;
  expect_signature_equal(signature(*result->geometry),
                         bevel_bmesh_reference(*source, selected, 0.25));
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
  selected.append(corner);
  std::reverse(selected.begin(), selected.end());
  auto repeated = bevel_edges(*source, selected, {.offset = 0.25}, &error);
  ASSERT_TRUE(repeated) << error;
  expect_source_geometry_equal(*result->geometry, *repeated->geometry);
  EXPECT_FALSE(bevel_edges(*source, selected, {.offset = 1.25, .clamp_overlap = false}, &error));
}

TEST_F(NurbsTopologyEditTest, BevelClampUsesFirstSourceCollisionAndReportsAppliedWidth)
{
  for (const bool closed : {false, true}) {
    for (const bool whole_network : {false, true}) {
      if (!closed && whole_network) {
        continue;
      }
      auto [source, corner] = bevel_fixture(closed);
      ASSERT_NE(source, nullptr);
      const OwnedGeometry before(geometry_copy(source.get()));
      Vector<int64_t> selected{corner};
      if (whole_network) {
        selected.clear();
        for (const auto &edge : source->edges) {
          selected.append(edge.id);
        }
      }
      for (const int segments : {1, 3}) {
        SCOPED_TRACE(testing::Message()
                     << closed << ", network " << whole_network << ", segments " << segments);
        OwnedGeometry previous;
        for (const double requested : {4.0, 100.0, std::numeric_limits<double>::max()}) {
          std::string error;
          BevelOffsetInfo info;
          auto result = bevel_edges(
              *source, selected, {.offset = requested, .segments = segments}, &error, &info);
          ASSERT_TRUE(result) << error;
          EXPECT_TRUE(error.empty());
          /* This two-unit fixture reaches the opposite boundary at two units for one
           * edge; beveling the entire box collides halfway along each source edge. */
          EXPECT_NEAR(info.offset, whole_network ? 1.0 : 2.0, 1e-12);
          EXPECT_LT(info.offset, whole_network ? 1.0 : 2.0);
          EXPECT_EQ(info.offset, info.limit);
          EXPECT_LT(info.offset, requested);
          if (previous) {
            expect_source_geometry_equal(*result->geometry, *previous);
          }
          auto options = preview_options();
          options.boundary.evaluation_allowance = 0;
          ASSERT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
          const auto index = TopologyIndex::build(*result->geometry, &error);
          ASSERT_TRUE(index) << error;
          for (const auto &edge : result->geometry->edges) {
            const int64_t uses = index->uses_for_edge(edge.id).size();
            EXPECT_GE(uses, 1);
            EXPECT_LE(uses, 2);
            if (closed) {
              EXPECT_EQ(uses, 2);
            }
          }
          auto replay = bevel_edges(
              *source,
              selected,
              {.offset = info.offset, .segments = segments, .clamp_overlap = false},
              &error);
          ASSERT_TRUE(replay) << error;
          expect_source_geometry_equal(*result->geometry, *replay->geometry);
          previous = std::move(result->geometry);
          BevelOffsetInfo rejected_info{17, 19};
          EXPECT_FALSE(
              bevel_edges(*source,
                          selected,
                          {.offset = requested, .segments = segments, .clamp_overlap = false},
                          &error,
                          &rejected_info));
          EXPECT_FALSE(error.empty());
          EXPECT_EQ(rejected_info.offset, 17);
          EXPECT_EQ(rejected_info.limit, 19);
          expect_source_geometry_equal(*source, *before);
        }
        if (segments == 1) {
          const auto mesh = bevel_bmesh_reference(*source, selected, 100, true);
          const auto native = signature(*previous);
          ASSERT_EQ(native.vertices.size(), mesh.vertices.size());
          /* At the limiting event BMesh has coincident boundary vertices, whereas native
           * supports keep arithmetic clearance. Compare locations without assuming the
           * same lexicographic ordering of nearly coincident corners. */
          for (const auto &point : native.vertices) {
            double distance = INFINITY;
            for (const auto &other : mesh.vertices) {
              distance = std::min(distance,
                                  math::length(double3(point[0], point[1], point[2]) -
                                               double3(other[0], other[1], other[2])));
            }
            EXPECT_LT(distance, 1e-6);
          }
        }
      }
    }
  }
}

TEST_F(NurbsTopologyEditTest, BevelClampIsIndependentOfScaleRotationAndSelectionOrder)
{
  auto [source, corner] = bevel_fixture(false);
  ASSERT_NE(source, nullptr);
  for (const double scale : {0.03125, 1.0, 128.0}) {
    double4x4 matrix = double4x4::identity();
    matrix[0][0] = matrix[1][1] = scale * std::cos(0.371);
    matrix[0][1] = -scale * std::sin(0.371);
    matrix[1][0] = scale * std::sin(0.371);
    matrix[2][2] = scale;
    matrix.location() = scale * double3(0.137, -0.283, 0.091);
    std::string error;
    auto transformed = transform_geometry(*source, matrix, 1e-9, &error);
    ASSERT_TRUE(transformed) << error;
    for (const int segments : {1, 3}) {
      SCOPED_TRACE(testing::Message() << scale << ", segments " << segments);
      BevelOffsetInfo info;
      auto result = bevel_edges(*transformed->geometry,
                                {&corner, 1},
                                {.offset = scale * 100, .segments = segments},
                                &error,
                                &info);
      ASSERT_TRUE(result) << error;
      EXPECT_NEAR(info.offset, scale * 2, scale * 1e-12);
      auto options = preview_options();
      options.boundary.evaluation_allowance = 0;
      ASSERT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
    }
  }
  auto [box, edge] = bevel_fixture(true);
  ASSERT_NE(box, nullptr);
  Vector<int64_t> selected;
  for (const auto &item : box->edges) {
    selected.append(item.id);
  }
  std::string error;
  BevelOffsetInfo forward, reverse;
  auto first = bevel_edges(*box, selected, {.offset = 100}, &error, &forward);
  ASSERT_TRUE(first) << error;
  std::reverse(selected.begin(), selected.end());
  selected.append(edge);
  auto second = bevel_edges(*box, selected, {.offset = 100}, &error, &reverse);
  ASSERT_TRUE(second) << error;
  EXPECT_EQ(forward.offset, reverse.offset);
  EXPECT_EQ(forward.limit, reverse.limit);
  expect_source_geometry_equal(*first->geometry, *second->geometry);
}

TEST_F(NurbsTopologyEditTest, ClampedChamfersKeepSharedIncidenceForEveryCubeEdgeSubset)
{
  auto [source, corner] = bevel_fixture(true);
  ASSERT_NE(source, nullptr);
  Vector<int64_t> edges;
  for (const auto &edge : source->edges) {
    edges.append(edge.id);
  }
  const OwnedGeometry before(geometry_copy(source.get()));
  for (uint32_t mask = 1; mask < (1u << edges.size()); mask++) {
    SCOPED_TRACE(mask);
    Vector<int64_t> selected;
    for (const int i : edges.index_range()) {
      if (mask & (1u << i)) {
        selected.append(edges[i]);
      }
    }
    std::string error;
    BevelOffsetInfo info;
    auto result = bevel_edges(*source, selected, {.offset = 100}, &error, &info);
    ASSERT_TRUE(result) << error;
    EXPECT_GE(info.offset, 1.0 - 1e-12);
    EXPECT_LE(info.offset, 2.0);
    const auto index = TopologyIndex::build(*result->geometry, &error);
    ASSERT_TRUE(index) << error;
    for (const auto &edge : result->geometry->edges) {
      const auto uses = index->uses_for_edge(edge.id);
      ASSERT_EQ(uses.size(), 2);
      EXPECT_NE(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
    }
    expect_source_geometry_equal(*source, *before);
    if (HasFailure()) {
      return;
    }
  }
}

TEST_F(NurbsTopologyEditTest, RoundedCubeCornersSurviveRotationAndBlendRealSkew)
{
  auto [source, corner] = bevel_fixture(true);
  ASSERT_NE(source, nullptr);
  double4x4 rotation = double4x4::identity();
  rotation[0][0] = rotation[1][1] = std::cos(0.371);
  rotation[0][1] = -std::sin(0.371);
  rotation[1][0] = std::sin(0.371);
  rotation.location() = double3(0.137, -0.283, 0.091);
  std::string error;
  auto transformed = transform_geometry(*source, rotation, 1e-9, &error);
  ASSERT_TRUE(transformed) << error;
  Vector<int64_t> selected;
  for (const auto &edge : source->edges) {
    selected.append(edge.id);
  }
  for (const double width : {0.1, 0.25, 100.0}) {
    BevelOffsetInfo info;
    auto result = bevel_edges(
        *transformed->geometry, selected, {.offset = width, .segments = 3}, &error, &info);
    ASSERT_TRUE(result) << error;
    EXPECT_EQ(info.boundary_blend_corners, 0);
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    auto preview = tessellate_surfaces(*result->geometry, options, &error);
    ASSERT_TRUE(preview) << error;
    const auto edit = EditData::create(*result->geometry, {}, &error);
    ASSERT_TRUE(edit) << error;
    auto display = evaluate_mesh(edit->read_snapshot(), options, &error);
    ASSERT_TRUE(display) << error;
    EXPECT_EQ(display->undefined_normal_corners, 0);
    /* Independently undo the source's rotation and translation. Every point on the
     * rounded box must lie at the requested radius from its inner axis-aligned box. */
    for (int i = 0; i < preview->points_num(); i++) {
      const double3 p = preview->position(i) - rotation.location();
      const double3 local(std::cos(0.371) * p.x - std::sin(0.371) * p.y,
                          std::sin(0.371) * p.x + std::cos(0.371) * p.y,
                          p.z - 1);
      const double3 outside = math::max(math::abs(local) - double3(1 - info.offset), double3(0));
      EXPECT_NEAR(math::length(outside), info.offset, 1e-11);
    }
  }
  for (const double amount : {0.1, 0.35, -0.35}) {
    SCOPED_TRACE(amount);
    double4x4 shear = double4x4::identity();
    shear[0][1] = amount;
    auto skewed = transform_geometry(*source, shear, 1e-9, &error);
    ASSERT_TRUE(skewed) << error;
    const OwnedGeometry before(geometry_copy(skewed->geometry.get()));
    BevelOffsetInfo info;
    auto result = bevel_edges(
        *skewed->geometry, selected, {.offset = 0.25, .segments = 3}, &error, &info);
    ASSERT_TRUE(result) << error;
    EXPECT_EQ(info.boundary_blend_corners, 8);
    EXPECT_LE(result->geometry_error_bound, 1e-9);
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    ASSERT_TRUE(evaluate_boundaries(*result->geometry, options.boundary, &error)) << error;
    const auto edit = EditData::create(*result->geometry, {}, &error);
    ASSERT_TRUE(edit) << error;
    auto display = evaluate_mesh(edit->read_snapshot(), options, &error);
    ASSERT_TRUE(display) << error;
    EXPECT_EQ(display->undefined_normal_corners, 0);
    const auto index = TopologyIndex::build(*result->geometry);
    ASSERT_TRUE(index);
    for (const auto &edge : result->geometry->edges) {
      const auto uses = index->uses_for_edge(edge.id);
      ASSERT_EQ(uses.size(), 2);
      EXPECT_NE(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
    }
    expect_source_geometry_equal(*skewed->geometry, *before);
  }
}

TEST_F(NurbsTopologyEditTest, ConnectedChamfersMatchBMeshForEveryCubeEdgeSubset)
{
  auto [source, corner] = bevel_fixture(true);
  ASSERT_NE(source, nullptr);
  const OwnedGeometry before(geometry_copy(source.get()));
  Vector<int64_t> edges;
  for (const auto &edge : source->edges) {
    edges.append(edge.id);
  }
  ASSERT_EQ(edges.size(), 12);
  for (uint32_t mask = 1; mask < (1u << edges.size()); mask++) {
    SCOPED_TRACE(mask);
    Vector<int64_t> selected;
    for (const int i : edges.index_range()) {
      if (mask & (1u << i)) {
        selected.append(edges[i]);
      }
    }
    std::string error;
    auto result = bevel_edges(*source, selected, {.offset = 0.25, .max_error = 0}, &error);
    ASSERT_TRUE(result) << error;
    expect_signature_equal(signature(*result->geometry),
                           bevel_bmesh_reference(*source, selected, 0.25));
    if (HasFailure()) {
      return;
    }
    EXPECT_EQ(result->geometry_error_bound, 0);
    const auto index = TopologyIndex::build(*result->geometry);
    ASSERT_TRUE(index);
    for (const auto &edge : result->geometry->edges) {
      const auto uses = index->uses_for_edge(edge.id);
      ASSERT_EQ(uses.size(), 2);
      EXPECT_NE(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
    }
    expect_source_geometry_equal(*source, *before);
    if (HasFailure()) {
      return;
    }
  }
}

TEST_F(NurbsTopologyEditTest, RoundedCornerBlendApproachesTheRightAngleSphereContinuously)
{
  auto [box, corner] = bevel_fixture(true);
  Vector<int64_t> edges;
  for (const auto &edge : box->edges) {
    edges.append(edge.id);
  }
  for (const double amount : {1e-10, 1e-7, 0.001}) {
    SCOPED_TRACE(amount);
    double4x4 shear = double4x4::identity();
    shear[0][1] = amount;
    std::string error;
    auto source = transform_geometry(*box, shear, 1e-9, &error);
    ASSERT_TRUE(source) << error;
    BevelOffsetInfo info;
    auto result = bevel_edges(
        *source->geometry, edges, {.offset = 0.25, .segments = 3}, &error, &info);
    ASSERT_TRUE(result) << error;
    EXPECT_EQ(info.boundary_blend_corners, 8);
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    const auto display = tessellate_surfaces(*result->geometry, options, &error);
    ASSERT_TRUE(display) << error;
    for (int i = 0; i < display->points_num(); i++) {
      double3 p = display->position(i);
      p.y -= amount * p.x;
      p.z -= 1;
      const double3 outside = math::max(math::abs(p) - double3(0.75), double3(0));
      EXPECT_NEAR(math::length(outside), 0.25, 20 * amount);
    }
  }
}

TEST_F(NurbsTopologyEditTest, PlanarTaperedBoxBevelUsesAffineChartsWithoutChangingTheSource)
{
  for (const double taper : {0.5, 1.3, 2.0}) {
    SCOPED_TRACE(taper);
    auto [box, corner] = bevel_fixture(true);
    Vector<VertexTransform> transforms;
    double4x4 scale = double4x4::identity();
    scale[0][0] = taper;
    for (const auto &vertex : box->vertices) {
      if (vertex.position[2] == 2) {
        transforms.append({vertex.id, scale});
      }
    }
    std::string error;
    auto tapered = transform_vertices(*box, transforms, 1e-9, &error);
    ASSERT_TRUE(tapered) << error;
    const OwnedGeometry before(geometry_copy(tapered->geometry.get()));
    Vector<int64_t> edges;
    for (const auto &edge : tapered->geometry->edges) {
      edges.append(edge.id);
    }
    for (const int segments : {1, 3, 8}) {
      SCOPED_TRACE(segments);
      BevelOffsetInfo info;
      auto result = bevel_edges(
          *tapered->geometry, edges, {.offset = 0.2, .segments = segments}, &error, &info);
      ASSERT_TRUE(result) << error;
      EXPECT_EQ(info.planar_charts, 2);
      EXPECT_LE(info.chart_error_bound, 1e-9);
      EXPECT_LE(result->geometry_error_bound, 1e-9);
      const auto index = TopologyIndex::build(*result->geometry);
      ASSERT_TRUE(index);
      for (const auto &edge : result->geometry->edges) {
        const auto uses = index->uses_for_edge(edge.id);
        ASSERT_EQ(uses.size(), 2);
        EXPECT_NE(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                  index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
      }
      auto options = preview_options();
      options.boundary.evaluation_allowance = 0;
      EXPECT_TRUE(evaluate_boundaries(*result->geometry, options.boundary, &error)) << error;
      EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
      if (segments == 1) {
        expect_signature_near_mesh(signature(*result->geometry),
                                   bevel_bmesh_reference(*tapered->geometry, edges, 0.2));
      }
      expect_source_geometry_equal(*tapered->geometry, *before);
    }
    tapered->geometry->next_id = std::numeric_limits<int64_t>::max() - 1;
    const OwnedGeometry exhausted(geometry_copy(tapered->geometry.get()));
    EXPECT_FALSE(bevel_edges(*tapered->geometry, edges, {.offset = 0.2}, &error));
    EXPECT_NE(error.find("identity range"), std::string::npos) << error;
    expect_source_geometry_equal(*tapered->geometry, *exhausted);
  }
}

TEST_F(NurbsTopologyEditTest, PlanarChartDoesNotFlattenAGenuinelyCurvedFace)
{
  auto [box, corner] = bevel_fixture(true);
  const int64_t vertex = static_cast<const NurbsTopologyVertex *>(box->vertices.first)->id;
  std::string error;
  auto bent = translate_vertices(*box, {&vertex, 1}, {0.1, 0.2, 0.3}, 1e-9, &error);
  ASSERT_TRUE(bent) << error;
  const OwnedGeometry before(geometry_copy(bent->geometry.get()));
  Vector<int64_t> edges;
  for (const auto &edge : bent->geometry->edges) {
    edges.append(edge.id);
  }
  EXPECT_FALSE(bevel_edges(*bent->geometry, edges, {.offset = 0.2, .segments = 3}, &error));
  EXPECT_NE(error.find("not planar"), std::string::npos) << error;
  expect_source_geometry_equal(*bent->geometry, *before);
}

TEST_F(NurbsTopologyEditTest, CurvedRuledFacesBevelWithoutFlattening)
{
  for (const bool closed : {false, true}) {
    SCOPED_TRACE(closed);
    auto [source, corner] = bevel_fixture(closed);
    std::string error;
    Vector<int64_t> outer;
    for (const auto &vertex : source->vertices) {
      if (vertex.position[1] == -1 && vertex.position[0] == -1 && vertex.position[2] == 0) {
        outer.append(vertex.id);
      }
    }
    ASSERT_EQ(outer.size(), 1);
    auto first = translate_vertices(*source, outer, {0, 0, 0.4}, 1e-9, &error);
    ASSERT_TRUE(first) << error;
    outer.clear();
    for (const auto &vertex : first->geometry->vertices) {
      if (vertex.position[2] == 2 && vertex.position[0] == 1 && vertex.position[1] == 1) {
        outer.append(vertex.id);
      }
    }
    ASSERT_EQ(outer.size(), 1);
    auto bent = translate_vertices(*first->geometry, outer, {0, 0.5, 0}, 1e-9, &error);
    ASSERT_TRUE(bent) << error;
    const OwnedGeometry before(geometry_copy(bent->geometry.get()));
    for (const int segments : {1, 3, 8}) {
      BevelOffsetInfo info;
      auto result = bevel_edges(
          *bent->geometry, {&corner, 1}, {.offset = 0.25, .segments = segments}, &error, &info);
      ASSERT_TRUE(result) << error;
      EXPECT_EQ(info.curved_blend_strips, 1);
      EXPECT_EQ(result->geometry->faces.count(), (closed ? 6 : 2) + segments);
      EXPECT_LE(result->geometry_error_bound, 1e-9);
      EXPECT_TRUE(evaluate_boundaries(*result->geometry, {}, &error)) << error;
      EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
      /* Both retained faces use exact cropped original supports. Every old coefficient resource
       * remains available unchanged, rather than replacing the curved shape with a plane. */
      for (const auto &surface : bent->geometry->surfaces) {
        const NurbsSurfaceGeometry *saved = nullptr;
        for (const auto &candidate : result->geometry->surfaces) {
          if (candidate.id == surface.id) {
            saved = &candidate;
          }
        }
        ASSERT_NE(saved, nullptr);
        EXPECT_EQ(std::memcmp(saved->points,
                              surface.points,
                              sizeof(NurbsControlPoint) * surface.points_u * surface.points_v),
                  0);
      }
      expect_source_geometry_equal(*bent->geometry, *before);
    }
    EXPECT_FALSE(bevel_edges(
        *bent->geometry, {&corner, 1}, {.offset = 100, .clamp_overlap = false}, &error));
    expect_source_geometry_equal(*bent->geometry, *before);
    BevelOffsetInfo clamped;
    EXPECT_TRUE(bevel_edges(*bent->geometry, {&corner, 1}, {.offset = 100}, &error, &clamped))
        << error;
    EXPECT_LT(clamped.offset, 100);
  }
}

TEST_F(NurbsTopologyEditTest, RationalCurvedEdgeBevelKeepsSupportTangency)
{
  auto sheet = make_arc_sheet(false);
  const auto original_index = TopologyIndex::build(*sheet);
  int64_t selected = 0;
  for (const auto &edge : sheet->edges) {
    if (original_index->vertex(edge.vertex_ids[0])->position[2] == 2 &&
        original_index->vertex(edge.vertex_ids[1])->position[2] == 2)
    {
      selected = edge.id;
    }
  }
  ASSERT_NE(selected, 0);
  std::string error;
  auto joined = extrude_edges(*sheet, {&selected, 1}, {2, 1, 1}, 0, &error);
  ASSERT_TRUE(joined) << error;
  const OwnedGeometry before(geometry_copy(joined->geometry.get()));
  BevelOffsetInfo info;
  auto result = bevel_edges(
      *joined->geometry, {&selected, 1}, {.offset = 0.2, .segments = 3}, &error, &info);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(info.curved_blend_strips, 1);
  EXPECT_TRUE(evaluate_boundaries(*result->geometry, {}, &error)) << error;
  EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
  const auto index = TopologyIndex::build(*result->geometry);
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> supports;
  for (const auto &curve : result->geometry->curves) {
    curves.add_new(curve.id, &curve);
  }
  for (const auto &surface : result->geometry->surfaces) {
    supports.add_new(surface.id, &surface);
  }
  int rails = 0;
  for (const auto &edge : result->geometry->edges) {
    const auto uses = index->uses_for_edge(edge.id);
    if (uses.size() != 2 ||
        result->faces.contains(uses[0].face_id) == result->faces.contains(uses[1].face_id))
    {
      continue;
    }
    rails++;
    for (const double fraction : {0.0, 0.2, 0.5, 0.8, 1.0}) {
      const double t = edge.parameter_range[0] +
                       (edge.parameter_range[1] - edge.parameter_range[0]) * fraction;
      std::array<double3, 2> normals;
      for (int side = 0; side < 2; side++) {
        const auto uv = evaluate_curve(*curves.lookup(uses[side].use->pcurve_id), t, &error);
        ASSERT_TRUE(uv) << error;
        const auto differential = evaluate_surface_differential(
            *supports.lookup(index->face(uses[side].face_id)->surface_id),
            double2(uv->position),
            {},
            &error);
        ASSERT_TRUE(differential && differential->normal) << error;
        normals[side] = *differential->normal;
      }
      EXPECT_NEAR(std::abs(math::dot(normals[0], normals[1])), 1, 1e-12);
    }
  }
  EXPECT_EQ(rails, 2);
  expect_source_geometry_equal(*joined->geometry, *before);
  EXPECT_TRUE(bevel_edges(*joined->geometry, {&selected, 1}, {.offset = 0.2, .segments = 1}));
}

TEST_F(NurbsTopologyEditTest, RoundedTaperedBoxEdgeSubsetsKeepSharedIncidence)
{
  auto [box, corner] = bevel_fixture(true);
  Vector<VertexTransform> transforms;
  double4x4 scale = double4x4::identity();
  scale[0][0] = 1.3;
  for (const auto &vertex : box->vertices) {
    if (vertex.position[2] == 2) {
      transforms.append({vertex.id, scale});
    }
  }
  std::string error;
  auto source = transform_vertices(*box, transforms, 1e-9, &error);
  ASSERT_TRUE(source) << error;
  const OwnedGeometry before(geometry_copy(source->geometry.get()));
  Vector<int64_t> all;
  for (const auto &edge : source->geometry->edges) {
    all.append(edge.id);
  }
  for (uint32_t mask = 1; mask < (1u << all.size()); mask++) {
    SCOPED_TRACE(mask);
    Vector<int64_t> selected;
    for (const int i : all.index_range()) {
      if (mask & (1u << i)) {
        selected.append(all[i]);
      }
    }
    auto result = bevel_edges(*source->geometry, selected, {.offset = 0.2, .segments = 3}, &error);
    ASSERT_TRUE(result) << error;
    const auto index = TopologyIndex::build(*result->geometry);
    ASSERT_TRUE(index);
    for (const auto &edge : result->geometry->edges) {
      const auto uses = index->uses_for_edge(edge.id);
      ASSERT_EQ(uses.size(), 2);
      EXPECT_NE(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
    }
    EXPECT_TRUE(evaluate_boundaries(*result->geometry, {}, &error)) << error;
    expect_source_geometry_equal(*source->geometry, *before);
    if (HasFailure()) {
      return;
    }
  }
}

TEST_F(NurbsTopologyEditTest, BevelSelectionModesResolveTheSameOpenCorner)
{
  auto [source, corner] = bevel_fixture(false);
  const auto index = TopologyIndex::build(*source);
  const auto &edge = *index->edge(corner);
  std::string error;
  auto expected = bevel_edges(*source, {&corner, 1}, {.offset = 0.3}, &error);
  ASSERT_TRUE(expected) << error;
  for (const auto mode : {SelectionMode::Vertex, SelectionMode::Edge, SelectionMode::Face}) {
    SelectionState selected;
    selected.mode = mode;
    if (mode == SelectionMode::Vertex) {
      selected.vertices.add_multiple({edge.vertex_ids, 2});
    }
    else if (mode == SelectionMode::Edge) {
      selected.edges.add(corner);
    }
    else {
      selected.faces.add(index->uses_for_edge(corner)[0].face_id);
    }
    auto normalized = selection_flush(*source, selected, mode, &error);
    ASSERT_TRUE(normalized) << error;
    const Vector<int64_t> edges(normalized->edges.begin(), normalized->edges.end());
    auto result = bevel_edges(*source, edges, {.offset = 0.3}, &error);
    ASSERT_TRUE(result) << error;
    expect_source_geometry_equal(*result->geometry, *expected->geometry);
  }
  expect_signature_near_mesh(signature(*expected->geometry),
                             bevel_bmesh_reference(*source, {&corner, 1}, 0.3));
}

TEST_F(NurbsTopologyEditTest, RoundedConnectedBoxHasSharedSphereCornersAndEditableSegments)
{
  auto [source, corner] = bevel_fixture(true);
  ASSERT_NE(source, nullptr);
  const OwnedGeometry before(geometry_copy(source.get()));
  Vector<int64_t> edges;
  for (const auto &edge : source->edges) {
    edges.append(edge.id);
  }
  for (const int segments : {2, 3, 8, 13}) {
    SCOPED_TRACE(segments);
    std::string error;
    auto result = bevel_edges(*source, edges, {.offset = 0.25, .segments = segments}, &error);
    ASSERT_TRUE(result) << error;
    /* Compare against Blender's real bevel, including its even/odd center layout. */
    const auto reference = bevel_bmesh_reference(*source, edges, 0.25, false, segments);
    const auto nurbs_signature = signature(*result->geometry);
    double maximum_vertex_error = 0;
    for (const auto &point : nurbs_signature.vertices) {
      double nearest = INFINITY;
      for (const auto &target : reference.vertices) {
        nearest = std::min(nearest,
                           math::distance(double3(point[0], point[1], point[2]),
                                          double3(target[0], target[1], target[2])));
      }
      maximum_vertex_error = std::max(maximum_vertex_error, nearest);
    }
    std::cout << "NURBS_BEVEL_ALL_VERTEX_ERROR segments=" << segments
              << " maximum=" << maximum_vertex_error << "\n";
    EXPECT_LT(maximum_vertex_error, 1e-6);
    auto corner_points = [](const std::vector<TopologySignature::Point> &points) {
      std::vector<TopologySignature::Point> result;
      for (const auto &point : points) {
        const double3 outside(std::max(std::abs(point[0]) - 0.75, 0.0),
                              std::max(std::abs(point[1]) - 0.75, 0.0),
                              std::max(std::max(0.25 - point[2], point[2] - 1.75), 0.0));
        if (outside.x > 1e-6 && outside.y > 1e-6 && outside.z > 1e-6 &&
            std::abs(math::length(outside) - 0.25) < 1e-6)
        {
          result.push_back(point);
        }
      }
      return result;
    };
    const auto nurbs_corner_points = corner_points(nurbs_signature.vertices);
    const auto mesh_corner_points = corner_points(reference.vertices);
    ASSERT_EQ(nurbs_corner_points.size(), mesh_corner_points.size());
    std::vector<bool> matched_mesh_points(mesh_corner_points.size(), false);
    double maximum_corner_point_error = 0;
    for (int i = 0; i < nurbs_corner_points.size(); i++) {
      int nearest = -1;
      double nearest_distance = std::numeric_limits<double>::infinity();
      for (int j = 0; j < mesh_corner_points.size(); j++) {
        if (matched_mesh_points[j]) {
          continue;
        }
        const double distance = math::length(
            double3(
                nurbs_corner_points[i][0], nurbs_corner_points[i][1], nurbs_corner_points[i][2]) -
            double3(mesh_corner_points[j][0], mesh_corner_points[j][1], mesh_corner_points[j][2]));
        if (distance < nearest_distance) {
          nearest = j;
          nearest_distance = distance;
        }
      }
      ASSERT_NE(nearest, -1);
      matched_mesh_points[nearest] = true;
      maximum_corner_point_error = std::max(maximum_corner_point_error, nearest_distance);
    }
    EXPECT_LT(maximum_corner_point_error, 1e-6);
    EXPECT_EQ(result->geometry->vertices.count(), reference.vertices.size());
    EXPECT_EQ(result->geometry->edges.count(), reference.edges.size());
    EXPECT_EQ(result->geometry->faces.count(), reference.faces.size());
    int triangles = 0;
    for (const NurbsTopologyFace &face : result->geometry->faces) {
      ASSERT_TRUE(face.loops.is_single());
      const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
      const int sides = loop.edge_uses.count();
      EXPECT_TRUE(sides == 3 || sides == 4);
      triangles += sides == 3;
    }
    EXPECT_EQ(triangles, segments % 2 ? 8 : 0);
    const auto index = TopologyIndex::build(*result->geometry);
    ASSERT_TRUE(index);
    std::vector<int> degrees;
    for (const NurbsTopologyVertex &vertex : result->geometry->vertices) {
      degrees.push_back(index->edges_for_vertex(vertex.id).size());
    }
    std::vector<int> reference_degrees(reference.vertices.size(), 0);
    for (const auto &edge : reference.edges) {
      for (const auto &point : edge) {
        const auto found = std::lower_bound(
            reference.vertices.begin(), reference.vertices.end(), point);
        ASSERT_NE(found, reference.vertices.end());
        ASSERT_EQ(*found, point);
        reference_degrees[found - reference.vertices.begin()]++;
      }
    }
    std::sort(degrees.begin(), degrees.end());
    std::sort(reference_degrees.begin(), reference_degrees.end());
    EXPECT_EQ(degrees, reference_degrees);
    for (const auto &edge : result->geometry->edges) {
      const auto uses = index->uses_for_edge(edge.id);
      ASSERT_EQ(uses.size(), 2);
      EXPECT_NE(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
    }
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    auto preview = tessellate_surfaces(*result->geometry, options, &error);
    ASSERT_TRUE(preview) << error;
    for (int i = 0; i < preview->points_num(); i++) {
      const double3 p = preview->position(i) - double3(0, 0, 1);
      const double3 outside = math::max(math::abs(p) - double3(0.75), double3(0));
      EXPECT_NEAR(math::length(outside), 0.25, 1e-13);
    }
    const auto edit = EditData::create(*result->geometry, {}, &error);
    ASSERT_NE(edit, nullptr) << error;
    auto display = evaluate_mesh(edit->read_snapshot(), options, &error);
    ASSERT_TRUE(display) << error;
    EXPECT_EQ(display->undefined_normal_corners, 0);
    const auto positions = display->mesh->vert_positions();
    const auto corner_vertices = display->mesh->corner_verts();
    const auto normals = display->mesh->corner_normals();
    for (const int i : corner_vertices.index_range()) {
      const double3 point = double3(positions[corner_vertices[i]]) - double3(0, 0, 1);
      const double3 nearest = math::clamp(point, double3(-0.75), double3(0.75));
      const double3 expected = math::normalize(point - nearest);
      EXPECT_GT(math::dot(double3(normals[i]), expected), 0.9999);
    }
    expect_source_geometry_equal(*source, *before);
  }
}

TEST_F(NurbsTopologyEditTest, RoundedBevelKeepsFlatFacesAndStraightRails)
{
  auto [source, unused] = bevel_fixture(true);
  ASSERT_TRUE(source);
  Vector<int64_t> selected;
  for (const auto &edge : source->edges) {
    selected.append(edge.id);
  }
  for (const int segments : {2, 3, 8, 13}) {
    std::string error;
    auto result = bevel_edges(*source, selected, {.offset = 0.25, .segments = segments}, &error);
    ASSERT_TRUE(result) << error;
    const auto index = TopologyIndex::build(*result->geometry);
    Map<int64_t, const NurbsCurveGeometry *> curves;
    for (const auto &curve : result->geometry->curves) {
      curves.add_new(curve.id, &curve);
    }
    for (const auto &original : source->faces) {
      const auto *face = index->face(original.id);
      ASSERT_NE(face, nullptr);
      for (const auto &loop : face->loops) {
        for (const auto &use : loop.edge_uses) {
          const auto &edge = *index->edge(use.edge_id);
          const double3 a(index->vertex(edge.vertex_ids[0])->position);
          const double3 b(index->vertex(edge.vertex_ids[1])->position);
          const double3 direction = math::normalize(b - a);
          for (int sample = 0; sample <= 32; sample++) {
            const double t = std::lerp(
                edge.parameter_range[0], edge.parameter_range[1], sample / 32.0);
            const auto point = evaluate_curve(*curves.lookup(edge.curve_id), t, &error);
            ASSERT_TRUE(point) << error;
            EXPECT_LT(math::length(math::cross(point->position - a, direction)), 1e-11)
                << "Straight rail bowed at " << edge.id << ", " << t;
          }
        }
      }
    }
  }
}

TEST_F(NurbsTopologyEditTest, RoundedCornerCurvesAvoidPoleDetoursAndHaveAlignedJoins)
{
  auto [source, corner] = bevel_fixture(true);
  ASSERT_TRUE(source);
  Vector<int64_t> selected;
  for (const auto &edge : source->edges) {
    selected.append(edge.id);
  }
  for (const int segments : {2, 3, 8, 13}) {
    SCOPED_TRACE(segments);
    std::string error;
    auto result = bevel_edges(*source, selected, {.offset = 0.25, .segments = segments}, &error);
    ASSERT_TRUE(result) << error;
    const auto index = TopologyIndex::build(*result->geometry, &error);
    ASSERT_TRUE(index) << error;
    Map<int64_t, const NurbsCurveGeometry *> curves;
    for (const auto &curve : result->geometry->curves) {
      curves.add_new(curve.id, &curve);
    }
    int tested = 0;
    double maximum_cross_track = 0;
    for (const auto &edge : result->geometry->edges) {
      const double3 a(index->vertex(edge.vertex_ids[0])->position);
      const double3 b(index->vertex(edge.vertex_ids[1])->position);
      const double3 midpoint = (a + b) * 0.5;
      const double3 center(midpoint.x < 0 ? -0.75 : 0.75,
                           midpoint.y < 0 ? -0.75 : 0.75,
                           midpoint.z < 1 ? 0.25 : 1.75);
      const double3 da = math::abs(a - center), db = math::abs(b - center);
      const bool interior_a = da.x > 1e-6 && da.y > 1e-6 && da.z > 1e-6;
      const bool interior_b = db.x > 1e-6 && db.y > 1e-6 && db.z > 1e-6;
      if ((!interior_a && !interior_b) || std::abs(math::length(da) - 0.25) > 1e-6 ||
          std::abs(math::length(db) - 0.25) > 1e-6)
      {
        continue;
      }
      SCOPED_TRACE(edge.id);
      const auto &curve = *curves.lookup(edge.curve_id);
      const double3 chord = b - a;
      const double length = math::length(chord);
      const double3 arc_normal = math::normalize(math::cross(a - center, b - center));
      double previous_progress = -1e-7;
      for (int sample = 0; sample <= 32; sample++) {
        const double t = sample / 32.0;
        const double parameter = (1 - t) * edge.parameter_range[0] + t * edge.parameter_range[1];
        const auto evaluated = evaluate_curve_bounded(curve, parameter, 1e-9, &error);
        ASSERT_TRUE(evaluated) << error;
        const double progress = math::dot(evaluated->position - a, chord) / (length * length);
        EXPECT_GE(progress, previous_progress - 1e-7);
        EXPECT_GE(progress, -1e-7);
        EXPECT_LE(progress, 1 + 1e-7);
        const double3 chord_point = a + progress * chord;
        maximum_cross_track = std::max(
            maximum_cross_track,
            std::abs(math::dot(evaluated->position - center, arc_normal)) / length);
        EXPECT_LE(math::length(evaluated->position - chord_point), 0.25 * length + 1e-8);
        previous_progress = progress;
      }
      tested++;
    }
    EXPECT_GE(tested, 24);
    /* The old latitude/longitude cuts wandered sideways by up to 13% of their chord,
     * despite matching both endpoints and lying exactly on the same spherical support. */
    EXPECT_LT(maximum_cross_track, 0.06);
    int aligned_joins = 0;
    double maximum_curvature_jump = 0;
    for (const auto &vertex : result->geometry->vertices) {
      const double3 point(vertex.position);
      const double3 outside(std::max(std::abs(point.x) - 0.75, 0.0),
                            std::max(std::abs(point.y) - 0.75, 0.0),
                            std::max(std::max(0.25 - point.z, point.z - 1.75), 0.0));
      const auto incident = index->edges_for_vertex(vertex.id);
      const int outside_axes = int(outside.x > 1e-6) + int(outside.y > 1e-6) +
                               int(outside.z > 1e-6);
      if (incident.size() != 4 || outside_axes < 2 ||
          std::abs(math::length(outside) - 0.25) > 1e-6)
      {
        continue;
      }
      auto outward_tangent = [&](const int64_t edge_id) {
        const auto &edge = *index->edge(edge_id);
        auto restricted = restrict_curve(
            *curves.lookup(edge.curve_id), double2(edge.parameter_range), 1e-9, &error);
        EXPECT_TRUE(restricted) << error;
        if (!restricted) {
          return double3(0);
        }
        const auto &curve = *restricted->curve;
        const int endpoint = edge.vertex_ids[0] == vertex.id ? 0 : curve.points_num - 1;
        const int adjacent = endpoint == 0 ? 1 : curve.points_num - 2;
        return math::normalize(double3(curve.points[adjacent].position) -
                               double3(curve.points[endpoint].position));
      };
      auto curvature = [&](const int64_t edge_id) {
        const auto &edge = *index->edge(edge_id);
        const auto &curve = *curves.lookup(edge.curve_id);
        EXPECT_EQ(curve.points_num, curve.order);
        const bool end = edge.vertex_ids[1] == vertex.id;
        const int p = curve.order - 1;
        const auto &a = curve.points[end ? p : 0];
        const auto &b = curve.points[end ? p - 1 : 1];
        const auto &c = curve.points[end ? p - 2 : 2];
        const double3 position(a.position);
        const double w1 = (end ? p : -p) * (a.weight - b.weight);
        const double w2 = p * (p - 1) * (a.weight - 2 * b.weight + c.weight);
        const double3 h1 = (end ? p : -p) * (a.weight * position - b.weight * double3(b.position));
        const double3 h2 = p * (p - 1) *
                           (a.weight * position - 2 * b.weight * double3(b.position) +
                            c.weight * double3(c.position));
        const double3 first = (h1 - position * w1) / a.weight;
        const double3 second = (h2 - 2 * first * w1 - position * w2) / a.weight;
        const double speed_squared = math::length_squared(first);
        return (second - first * (math::dot(first, second) / speed_squared)) / speed_squared;
      };
      for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
          bool share_face = false;
          for (const auto &a : index->uses_for_edge(incident[i])) {
            for (const auto &b : index->uses_for_edge(incident[j])) {
              share_face |= a.face_id == b.face_id;
            }
          }
          if (!share_face) {
            EXPECT_LT(math::dot(outward_tangent(incident[i]), outward_tangent(incident[j])),
                      -0.999999);
            aligned_joins++;
            if (outside_axes == 3) {
              maximum_curvature_jump = std::max(
                  maximum_curvature_jump,
                  math::distance(curvature(incident[i]), curvature(incident[j])));
            }
          }
        }
      }
    }
    if (segments > 2) {
      EXPECT_GT(aligned_joins, 0);
    }
    std::cout << "NURBS_ROW_CURVATURE_JUMP segments=" << segments
              << " maximum=" << maximum_curvature_jump << "\n";
    EXPECT_LT(maximum_curvature_jump, 1e-5);
  }
}

TEST_F(NurbsTopologyEditTest, RoundedCubeChainsKeepClosedSharedIncidence)
{
  auto [source, corner] = bevel_fixture(true);
  ASSERT_NE(source, nullptr);
  const OwnedGeometry before(geometry_copy(source.get()));
  Vector<int64_t> all;
  for (const auto &edge : source->edges) {
    all.append(edge.id);
  }
  for (const uint32_t mask : {3u, 7u, 15u, 85u, 170u, 273u, 1365u, 2730u, 4095u}) {
    SCOPED_TRACE(mask);
    Vector<int64_t> edges;
    for (const int i : all.index_range()) {
      if (mask & (1u << i)) {
        edges.append(all[i]);
      }
    }
    std::string error;
    auto result = bevel_edges(*source, edges, {.offset = 0.25, .segments = 3}, &error);
    ASSERT_TRUE(result) << error;
    const auto index = TopologyIndex::build(*result->geometry);
    ASSERT_TRUE(index);
    for (const auto &edge : result->geometry->edges) {
      const auto uses = index->uses_for_edge(edge.id);
      ASSERT_EQ(uses.size(), 2);
      EXPECT_NE(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
    }
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, options, &error)) << error;
    expect_source_geometry_equal(*source, *before);
  }
}

TEST_F(NurbsTopologyEditTest, RoundedSkewedBoxEdgeSubsetsKeepSharedIncidence)
{
  auto [box, corner] = bevel_fixture(true);
  double4x4 shear = double4x4::identity();
  shear[0][1] = 0.21;
  shear[0][2] = 0.13;
  shear[1][2] = -0.17;
  std::string error;
  auto source = transform_geometry(*box, shear, 1e-9, &error);
  ASSERT_TRUE(source) << error;
  const OwnedGeometry before(geometry_copy(source->geometry.get()));
  Vector<int64_t> all;
  for (const auto &edge : source->geometry->edges) {
    all.append(edge.id);
  }
  for (uint32_t mask = 1; mask < (1u << all.size()); mask++) {
    SCOPED_TRACE(mask);
    Vector<int64_t> selected;
    for (const int i : all.index_range()) {
      if (mask & (1u << i)) {
        selected.append(all[i]);
      }
    }
    auto result = bevel_edges(*source->geometry, selected, {.offset = 0.2, .segments = 3}, &error);
    ASSERT_TRUE(result) << error;
    const auto index = TopologyIndex::build(*result->geometry, &error);
    ASSERT_TRUE(index) << error;
    for (const auto &edge : result->geometry->edges) {
      const auto uses = index->uses_for_edge(edge.id);
      ASSERT_EQ(uses.size(), 2);
      EXPECT_NE(index->face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1),
                index->face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1));
    }
    EXPECT_TRUE(evaluate_boundaries(*result->geometry, {}, &error)) << error;
    expect_source_geometry_equal(*source->geometry, *before);
    if (HasFailure()) {
      return;
    }
  }
}

TEST_F(NurbsTopologyEditTest, RationalOctantConstructionReportsErrorAndOnlyCertifiesItsPole)
{
  std::string error;
  auto result = rational_octant_surface(
      {0, 0, 0}, {double3(1, 0, 0), double3(0, 1, 0), double3(0, 0, 1)}, 1e-9, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_GT(result->geometry_error_bound, 0);
  EXPECT_LT(result->geometry_error_bound, 1e-14);
  EXPECT_TRUE(collapsed_surface_boundary_interval(*result->surface, {0, 1}, {1, 1}, {0, 0, 1}));
  EXPECT_FALSE(collapsed_surface_boundary_interval(*result->surface, {0, 0}, {1, 0}, {0, 0, 1}));
  EXPECT_FALSE(
      collapsed_surface_boundary_interval(*result->surface, {0, 0.5}, {1, 0.5}, {0, 0, 1}));
  EXPECT_FALSE(
      collapsed_surface_boundary_interval(*result->surface, {0, 1}, {1, 1}, {0, 0, 1.001}));
  for (int u = 0; u <= 16; u++) {
    const auto normal = evaluate_surface_pole_normal(*result->surface, {u / 16.0, 1});
    ASSERT_TRUE(normal);
    EXPECT_NEAR(math::distance(*normal, double3(0, 0, 1)), 0, 1e-14);
    EXPECT_FALSE(evaluate_surface_pole_normal(*result->surface, {u / 16.0, 0.5}));
    for (int v = 0; v <= 16; v++) {
      auto sample = evaluate_surface(*result->surface, {u / 16.0, v / 16.0}, &error);
      ASSERT_TRUE(sample) << error;
      EXPECT_NEAR(math::length(sample->position), 1, 1e-14);
    }
  }
  EXPECT_FALSE(rational_octant_surface(
      {0, 0, 0}, {double3(1, 0, 0), double3(0, 1, 0), double3(0, 0, 1)}, 0, &error));
}

TEST_F(NurbsTopologyEditTest, SphericalOctantBoundsRadialDeviationWithoutChangingAxes)
{
  std::string error;
  const std::array<double3, 3> axes = {double3(1, 0, 0), double3(0, 1.1, 0), double3(0, 0, 1)};
  EXPECT_FALSE(rational_spherical_octant_surface({}, axes, 1, 1e-9, &error));
  EXPECT_FALSE(error.empty());
  error.clear();
  auto result = rational_spherical_octant_surface({}, axes, 1, 0.25, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_GE(result->geometry_error_bound, 0.1);
  EXPECT_LT(result->geometry_error_bound, 0.25);
  auto endpoint = evaluate_surface(*result->surface, {1, 0}, &error);
  ASSERT_TRUE(endpoint) << error;
  EXPECT_EQ(endpoint->position, axes[1]);
  for (int u = 0; u <= 16; u++) {
    for (int v = 0; v <= 16; v++) {
      const auto point = evaluate_surface(*result->surface, {u / 16.0, v / 16.0}, &error);
      ASSERT_TRUE(point) << error;
      EXPECT_LE(std::abs(math::length(point->position) - 1), result->geometry_error_bound);
    }
  }
  for (const double radius : {0.0, -1.0, double(INFINITY), double(NAN)}) {
    EXPECT_FALSE(rational_spherical_octant_surface({}, axes, radius, 1, &error));
  }
}

TEST_F(NurbsTopologyEditTest, RationalCoonsTriangleInterpolatesUnchangedConics)
{
  auto quadratic = [](const double3 a, const double3 pole, const double3 b, const double w) {
    OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
    curve->dimension = 3;
    curve->order = curve->points_num = 3;
    curve->knots_num = 6;
    curve->points = MEM_new_array<NurbsControlPoint>(3, __func__);
    curve->knots = MEM_new_array_zeroed<double>(6, __func__);
    curve->knots[3] = curve->knots[4] = curve->knots[5] = 1;
    for (int i = 0; i < 3; i++) {
      const double3 p = i == 0 ? a : i == 1 ? pole : b;
      std::copy_n(&p.x, 3, curve->points[i].position);
    }
    curve->points[1].weight = w;
    return curve;
  };
  const double3 a(0.23, 0.31, 0.0), b(0.29, 0.0, 0.27), c(0.0, 0.33, 0.22);
  auto ab = quadratic(a, {0.21, 0, 0}, b, 0.67);
  auto ac = quadratic(a, {0, 0.24, 0}, c, 0.72);
  auto bc = quadratic(b, {0, 0, 0.25}, c, 0.81);
  std::string error;
  auto patch = rational_coons_triangle(*ab, *ac, *bc, 1e-12, &error);
  ASSERT_TRUE(patch) << error;
  const auto &support = *patch->surface;
  for (const auto &point : Span(support.points, support.points_num)) {
    EXPECT_GT(point.weight, 0);
  }
  for (int i = 0; i <= 32; i++) {
    const double t = double(i) / 32;
    for (int side = 0; side < 3; side++) {
      const double2 uv = side == 0 ? double2(t, 0) : side == 1 ? double2(0, t) : double2(1, t);
      const auto *curve = side == 0 ? ab.get() : side == 1 ? ac.get() : bc.get();
      const auto expected = evaluate_curve(*curve, t);
      const auto actual = evaluate_surface(support, uv);
      ASSERT_TRUE(expected && actual);
      EXPECT_LT(math::distance(expected->position, actual->position), 1e-14);
    }
    const auto pole = evaluate_surface(support, {t, 1});
    ASSERT_TRUE(pole);
    EXPECT_LT(math::distance(pole->position, c), 1e-14);
  }
  /* Compare with the independent homogeneous Boolean-sum formula throughout the domain. */
  auto homogeneous = [](const NurbsCurveGeometry &curve, const double t) {
    double4 h(0);
    const double basis[] = {(1 - t) * (1 - t), 2 * t * (1 - t), t * t};
    for (int i = 0; i < 3; i++) {
      const auto &p = curve.points[i];
      h += double4(p.position[0], p.position[1], p.position[2], 1) * (basis[i] * p.weight);
    }
    return h;
  };
  for (const double u : {0.13, 0.37, 0.71, 0.89}) {
    for (const double v : {0.17, 0.41, 0.63, 0.91}) {
      const double4 ha(a.x, a.y, a.z, 1), hb(b.x, b.y, b.z, 1), hc(c.x, c.y, c.z, 1);
      const double4 h = (1 - v) * homogeneous(*ab, u) + v * hc + (1 - u) * homogeneous(*ac, v) +
                        u * homogeneous(*bc, v) - ((1 - v) * ((1 - u) * ha + u * hb) + v * hc);
      const auto actual = evaluate_surface(support, {u, v});
      ASSERT_TRUE(actual);
      EXPECT_LT(math::distance(actual->position, double3(h.x, h.y, h.z) / h.w), 1e-14);
    }
  }
  EXPECT_FALSE(rational_coons_triangle(*ab, *ac, *bc, 0, &error));
  ac->points[0].position[0] += 1e-6;
  EXPECT_FALSE(rational_coons_triangle(*ab, *ac, *bc, 1e-9, &error));
  ac->points[0] = ab->points[0];
  ab->points[1].weight = ac->points[1].weight = bc->points[1].weight = 0.1;
  EXPECT_FALSE(rational_coons_triangle(*ab, *ac, *bc, 1e-9, &error));
}

TEST_F(NurbsTopologyEditTest, BevelPyramidSubsetsMatchBMeshOrReportUnsupportedCap)
{
  auto [box, corner] = bevel_fixture(true);
  ASSERT_NE(box, nullptr);
  SelectionState vertices;
  for (const auto &v : box->vertices) {
    if (v.position[2] == 2) {
      vertices.vertices.add(v.id);
    }
  }
  std::string error;
  auto selected = selection_flush(*box, vertices, SelectionMode::Vertex, &error);
  ASSERT_TRUE(selected) << error;
  auto pyramid = merge_selected_vertices(*box, *selected, VertexMergeMode::Center, {}, 0, &error);
  ASSERT_TRUE(pyramid) << error;
  ASSERT_EQ(pyramid->geometry->vertices.count(), 5);
  const OwnedGeometry original(geometry_copy(pyramid->geometry.get()));
  Vector<int> unsupported;
  Vector<int64_t> all;
  for (const auto &e : pyramid->geometry->edges) {
    all.append(e.id);
  }
  Vector<Vector<int64_t>> cases;
  for (uint32_t mask = 1; mask < (1u << all.size()); mask++) {
    Vector<int64_t> edges;
    for (const int i : all.index_range()) {
      if (mask & (1u << i)) {
        edges.append(all[i]);
      }
    }
    cases.append(std::move(edges));
  }
  for (const int case_index : cases.index_range()) {
    const auto &edges = cases[case_index];
    SCOPED_TRACE(case_index + 1);
    SCOPED_TRACE(edges.first());
    SCOPED_TRACE(edges.size());
    auto result = bevel_edges(*pyramid->geometry, edges, {.offset = 0.25}, &error);
    if (!result) {
      EXPECT_EQ(
          error,
          "Boundary needs a nonplanar or tolerance-managed construction; no fitting was applied");
      unsupported.append(case_index + 1);
      expect_source_geometry_equal(*pyramid->geometry, *original);
      continue;
    }
    const auto native = signature(*result->geometry);
    const auto reference = bevel_bmesh_reference(*pyramid->geometry, edges, 0.25);
    EXPECT_EQ(native.vertices.size(), reference.vertices.size());
    EXPECT_EQ(native.edges.size(), reference.edges.size());
    EXPECT_EQ(native.faces.size(), reference.faces.size());
    /* Compare coordinates for independent edges and the complete fan. Partial loop-slide
     * chains intentionally minimize a symmetric width objective; BMesh's chain traversal can
     * leave asymmetric widths when a chain returns to the same non-cycle anchor. */
    if (edges.size() == 1 || edges.size() == all.size()) {
      expect_signature_near_mesh(native, reference);
    }
    if (case_index + 1 == 49) {
      /* The selected front triangle is symmetric in X. Preserve that symmetry rather than
       * reproducing BMesh's traversal-dependent width asymmetry at its returned anchor. */
      for (const auto &point : native.vertices) {
        const double3 reflected(-point[0], point[1], point[2]);
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto &candidate : native.vertices) {
          nearest = std::min(
              nearest,
              math::distance(reflected, double3(candidate[0], candidate[1], candidate[2])));
        }
        EXPECT_LT(nearest, 1e-14);
      }
    }
    const auto index = TopologyIndex::build(*result->geometry);
    ASSERT_TRUE(index);
    for (const auto &edge : result->geometry->edges) {
      EXPECT_EQ(index->uses_for_edge(edge.id).size(), 2);
    }
    EXPECT_TRUE(tessellate_surfaces(*result->geometry, preview_options(), &error)) << error;
  }
  EXPECT_EQ(unsupported, (Vector<int>{60, 61, 83, 87, 163, 171, 204, 206}));
  expect_source_geometry_equal(*pyramid->geometry, *original);
}

TEST_F(NurbsTopologyEditTest, BevelObliqueAndReversedFacesPreserveSharedBoundaries)
{
  for (const bool closed : {false, true}) {
    for (const bool reversed : {false, true}) {
      auto [source, corner] = bevel_fixture(closed);
      auto shear = [](double position[3]) {
        position[0] += 0.5 * position[2];
        position[1] += 0.5 * position[2];
      };
      for (auto &vertex : source->vertices) {
        shear(vertex.position);
      }
      for (auto &curve : source->curves) {
        if (curve.dimension == 3) {
          for (int i = 0; i < curve.points_num; i++) {
            shear(curve.points[i].position);
          }
        }
      }
      for (auto &surface : source->surfaces) {
        for (int i = 0; i < surface.points_num; i++) {
          shear(surface.points[i].position);
        }
      }
      if (reversed) {
        for (auto &face : source->faces) {
          face.orientation *= -1;
        }
      }
      const OwnedGeometry before(geometry_copy(source.get()));
      std::string error;
      auto chamfer = bevel_edges(*source, {&corner, 1}, {.offset = 0.3}, &error);
      ASSERT_TRUE(chamfer) << error;
      expect_signature_near_mesh(signature(*chamfer->geometry),
                                 bevel_bmesh_reference(*source, {&corner, 1}, 0.3));
      auto rounded = bevel_edges(*source, {&corner, 1}, {.offset = 0.3, .segments = 3}, &error);
      ASSERT_TRUE(rounded) << error;
      auto options = preview_options();
      options.boundary.evaluation_allowance = 0;
      EXPECT_TRUE(tessellate_surfaces(*chamfer->geometry, options, &error)) << error;
      EXPECT_TRUE(tessellate_surfaces(*rounded->geometry, options, &error)) << error;
      expect_source_geometry_equal(*source, *before);
    }
  }
}

TEST_F(NurbsTopologyEditTest, PlanarCurveProjectionCertifiesRationalTrimsAndRejectsRealGaps)
{
  auto sheet = make_planar_sheet();
  auto arc = make_arc_sheet(false);
  ASSERT_NE(sheet, nullptr);
  ASSERT_NE(arc, nullptr);
  const auto &surface = *static_cast<NurbsSurfaceGeometry *>(sheet->surfaces.first);
  auto &curve = *static_cast<NurbsCurveGeometry *>(arc->curves.first);
  const OwnedGeometry before(geometry_copy(arc.get()));
  std::string error;
  auto projected = project_curve_to_affine_surface(curve, surface, 0, &error);
  ASSERT_TRUE(projected) << error;
  EXPECT_EQ(projected->geometry_error_bound, 0);
  EXPECT_EQ(projected->curve->points[1].weight, curve.points[1].weight);
  projected->curve->points[1].position[0] += 0.125;
  const auto gap = affine_curve_support_deviation_bound(curve, *projected->curve, surface);
  ASSERT_TRUE(gap);
  EXPECT_GE(*gap, 0.25);
  expect_source_geometry_equal(*arc, *before);
  curve.points[1].position[2] = 0.001;
  EXPECT_FALSE(project_curve_to_affine_surface(curve, surface, 1e-9, &error));
  auto other = OwnedCurve(MEM_dupalloc(&curve));
  other->points = MEM_dupalloc(curve.points);
  other->knots = MEM_dupalloc(curve.knots);
  other->points[1].weight = 0.8;
  auto ruled = ruled_surface(curve, *other, &error);
  ASSERT_TRUE(ruled) << error;
  for (const double u : {0.0, 0.17, 0.5, 0.83, 1.0}) {
    const auto first = evaluate_curve(curve, u), last = evaluate_curve(*other, u);
    ASSERT_TRUE(first && last);
    const auto a = evaluate_surface(*ruled, {u, 0}), b = evaluate_surface(*ruled, {u, 1});
    ASSERT_TRUE(a && b);
    EXPECT_LT(math::distance(a->position, first->position), 1e-14);
    EXPECT_LT(math::distance(b->position, last->position), 1e-14);
    for (const double v : {0.2, 0.5, 0.8}) {
      const auto point = evaluate_surface(*ruled, {u, v});
      ASSERT_TRUE(point);
      EXPECT_LT(math::length(math::cross(point->position - first->position,
                                         last->position - first->position)),
                1e-14);
    }
  }
}

TEST_F(NurbsTopologyEditTest, PlanarProjectionBoundsSmallBilinearSupportTwist)
{
  auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  auto *support = static_cast<NurbsSurfaceGeometry *>(sheet->surfaces.first);
  const auto index = TopologyIndex::build(*sheet);
  ASSERT_TRUE(index);
  const NurbsTopologyEdge *right_edge = nullptr;
  for (const auto &candidate : sheet->edges) {
    const auto a = double3(index->vertex(candidate.vertex_ids[0])->position);
    const auto b = double3(index->vertex(candidate.vertex_ids[1])->position);
    if (a.x == 1 && b.x == 1) {
      right_edge = &candidate;
    }
  }
  ASSERT_NE(right_edge, nullptr);
  const NurbsCurveGeometry *curve = nullptr;
  for (const auto &candidate : sheet->curves) {
    if (candidate.id == right_edge->curve_id) {
      curve = &candidate;
    }
  }
  ASSERT_NE(curve, nullptr);
  const OwnedGeometry before(geometry_copy(sheet.get()));
  support->points[3].position[2] = 1e-10;
  std::string error;
  auto projected = project_curve_to_affine_surface(*curve, *support, 1e-9, &error);
  ASSERT_TRUE(projected) << error;
  EXPECT_GT(projected->geometry_error_bound, 0);
  EXPECT_LE(projected->geometry_error_bound, 1e-9);
  support->points[3].position[2] = 1e-5;
  EXPECT_FALSE(project_curve_to_affine_surface(*curve, *support, 1e-9, &error));
  support->points[3].position[2] = 0;
  expect_source_geometry_equal(*sheet, *before);
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

TEST_F(NurbsTopologyEditTest, CanonicalTrimCoordinatesHaveExactBoundedEvaluation)
{
  OwnedGeometry geometry(MEM_new<NurbsGeometry>(__func__));
  auto *curve = add_linear_curve(*geometry, {-0.3, 2, 0}, {0.7, 2, 0}, 2);
  curve->knots[0] = curve->knots[1] = -0.3;
  curve->knots[2] = curve->knots[3] = 0.7;
  for (const double weight : {0.7, 1e300}) {
    curve->points[0].weight = curve->points[1].weight = weight;
    for (const double parameter : {-0.3, 0.0, 0.1, 0.7}) {
      const auto point = evaluate_curve_bounded(*curve, parameter, 0);
      ASSERT_TRUE(point);
      EXPECT_EQ(point->position, double3(parameter, 2, 0));
      EXPECT_EQ(point->error_bound, 0);
    }
    EXPECT_FALSE(evaluate_curve_bounded(*curve, -0.4, 0));
    EXPECT_FALSE(evaluate_curve_bounded(*curve, 0.8, 0));
    EXPECT_FALSE(evaluate_curve_bounded(*curve, NAN, 0));
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
  for (const double2 range : {double2(0.25, 1.75), double2(0, 2), double2(0.5, 2)}) {
    auto piece = restrict_curve(curve, range, 1e-10, &error);
    ASSERT_TRUE(piece.has_value()) << error;
    for (int i = 0; i <= 64; i++) {
      const double parameter = std::lerp(range.x, range.y, i / 64.0);
      const auto a = evaluate_curve_bounded(curve, parameter, 1e-10, &error);
      const auto b = evaluate_curve_bounded(*piece->curve, parameter, 1e-10, &error);
      ASSERT_TRUE(a.has_value()) << error;
      ASSERT_TRUE(b.has_value()) << error;
      const double3 delta = a->position - b->position;
      EXPECT_LE(std::hypot(delta.x, delta.y, delta.z),
                piece->geometry_error_bound + a->error_bound + b->error_bound);
    }
  }
  EXPECT_EQ(curve.points, points);
  EXPECT_EQ(curve.knots, knots);
  EXPECT_EQ(curve.points_num, 4);
  EXPECT_EQ(knots[0], -2);
}

TEST_F(NurbsTopologyEditTest, TensorRestrictionPreservesNonclampedAndRepeatedKnotSupports)
{
  NurbsSurfaceGeometry source;
  source.order_u = source.order_v = 3;
  source.points_u = 4;
  source.points_v = 5;
  source.points_num = 20;
  source.knots_u_num = 7;
  source.knots_v_num = 8;
  NurbsControlPoint points[20];
  double knots_u[] = {-2, -1, 0, 0.5, 1, 2, 3};
  double knots_v[] = {0, 0, 0, 0.5, 0.5, 1, 1, 1};
  source.points = points;
  source.knots_u = knots_u;
  source.knots_v = knots_v;
  for (int v = 0; v < source.points_v; v++) {
    for (int u = 0; u < source.points_u; u++) {
      auto &point = points[v * source.points_u + u];
      point.position[0] = 0.7 * u - 1.3;
      point.position[1] = 0.3 * v - 0.6;
      point.position[2] = 0.2 * ((u + v) % 3) - 0.1;
      point.weight = 0.7 + 0.1 * ((2 * u + v) % 5);
    }
  }
  const std::vector<NurbsControlPoint> saved(std::begin(points), std::end(points));
  auto reference_basis = [](const Span<double> knots,
                            const int order,
                            const int count,
                            const double parameter) {
    std::vector<long double> values(knots.size() - 1);
    for (int i = 0; i < int(values.size()); i++) {
      values[i] = parameter == knots[count] ? knots[i] < parameter && parameter <= knots[i + 1] :
                                              knots[i] <= parameter && parameter < knots[i + 1];
    }
    for (int degree = 1; degree < order; degree++) {
      for (int i = 0; i < int(values.size()) - degree; i++) {
        const long double left = static_cast<long double>(knots[i + degree]) - knots[i];
        const long double right = static_cast<long double>(knots[i + degree + 1]) - knots[i + 1];
        values[i] = (left == 0 ?
                         0 :
                         (parameter - static_cast<long double>(knots[i])) / left * values[i]) +
                    (right == 0 ? 0 :
                                  (static_cast<long double>(knots[i + degree + 1]) - parameter) /
                                      right * values[i + 1]);
      }
    }
    values.resize(count);
    return values;
  };
  std::string error;
  for (const auto &range : {std::array<double2, 2>{{{0.17, 0.23}, {0.83, 0.91}}},
                            std::array<double2, 2>{{{0, 0}, {1, 1}}},
                            std::array<double2, 2>{{{0.5, 0.5}, {0.83, 1}}}})
  {
    const auto piece = restrict_surface(source, range[0], range[1], 1e-9, &error);
    ASSERT_TRUE(piece) << error;
    EXPECT_EQ(piece->surface->order_u, source.order_u);
    EXPECT_EQ(piece->surface->order_v, source.order_v);
    for (int j = 0; j <= 8; j++) {
      const double v = std::lerp(range[0].y, range[1].y, j / 8.0);
      const auto bv = reference_basis({knots_v, 8}, source.order_v, source.points_v, v);
      const auto iso = surface_isocurve(*piece->surface, 1, v, 1e-9, &error);
      ASSERT_TRUE(iso) << error;
      for (int i = 0; i <= 8; i++) {
        const double u = std::lerp(range[0].x, range[1].x, i / 8.0);
        const auto bu = reference_basis({knots_u, 7}, source.order_u, source.points_u, u);
        std::array<long double, 3> numerator{};
        long double weight = 0;
        for (int y = 0; y < source.points_v; y++) {
          for (int x = 0; x < source.points_u; x++) {
            const auto &point = points[y * source.points_u + x];
            const long double basis = bu[x] * bv[y] * point.weight;
            weight += basis;
            for (const int axis : IndexRange(3)) {
              numerator[axis] += basis * point.position[axis];
            }
          }
        }
        ASSERT_GT(weight, 0);
        const auto actual = evaluate_curve_bounded(*iso->curve, u, 1e-9, &error);
        ASSERT_TRUE(actual) << error;
        long double deviation = 0;
        for (const int axis : IndexRange(3)) {
          deviation += std::abs(numerator[axis] / weight - actual->position[axis]);
        }
        EXPECT_LE(deviation,
                  piece->geometry_error_bound + iso->geometry_error_bound + actual->error_bound +
                      256 * std::numeric_limits<long double>::epsilon());
      }
    }
  }
  EXPECT_EQ(std::memcmp(points, saved.data(), sizeof(points)), 0);
  EXPECT_EQ(knots_u[0], -2);
  EXPECT_EQ(knots_v[3], 0.5);
  EXPECT_FALSE(restrict_surface(source, {-0.1, 0}, {1, 1}, 1e-9, &error));
  EXPECT_FALSE(restrict_surface(source, {0, 0.7}, {1, 0.3}, 1e-9, &error));
}

TEST_F(NurbsTopologyEditTest, SurfaceParameterLinesMatchIndependentRationalTensorEvaluation)
{
  std::string error;
  const std::array<double3, 3> axes = {double3(1, 0, 0), double3(0, 1, 0), double3(0, 0, 1)};
  auto patch = rational_octant_surface({0.137, -0.283, 0.091}, axes, 1e-9, &error);
  ASSERT_TRUE(patch) << error;
  const auto &surface = *patch->surface;
  std::array<NurbsControlPoint, 9> before;
  std::copy_n(surface.points, 9, before.begin());
  const double2 lines[][2] = {{{0, 0.5}, {0.5, 0.39}},
                              {{0.12, 0.31}, {0.81, 0.72}},
                              {{0.9, 0.8}, {0.2, 0.1}},
                              {{0.1, 0.8}, {0.8, 0.2}},
                              {{0.3, 0.1}, {0.3, 0.9}},
                              {{0.8, 0.6}, {0.2, 0.6}}};
  for (const auto &line : lines) {
    auto curve = surface_parameter_line(surface, line[0], line[1], 1e-9, &error);
    ASSERT_TRUE(curve) << error;
    EXPECT_LE(curve->geometry_error_bound, 1e-9);
    for (int sample = 0; sample <= 32; sample++) {
      const double parameter = sample / 32.0;
      const long double t = parameter;
      const long double u = (1 - t) * line[0].x + t * line[1].x;
      const long double v = (1 - t) * line[0].y + t * line[1].y;
      const long double bu[] = {(1 - u) * (1 - u), 2 * u * (1 - u), u * u};
      const long double bv[] = {(1 - v) * (1 - v), 2 * v * (1 - v), v * v};
      std::array<long double, 3> numerator{};
      long double denominator = 0;
      for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
          const NurbsControlPoint &point = surface.points[3 * j + i];
          const long double basis = bu[i] * bv[j] * point.weight;
          denominator += basis;
          for (int axis = 0; axis < 3; axis++) {
            numerator[axis] += basis * point.position[axis];
          }
        }
      }
      const auto evaluated = evaluate_curve_bounded(*curve->curve, parameter, 1e-9, &error);
      ASSERT_TRUE(evaluated) << error;
      long double gap = 0;
      for (int axis = 0; axis < 3; axis++) {
        gap += std::abs(numerator[axis] / denominator - evaluated->position[axis]);
      }
      EXPECT_LE(gap,
                curve->geometry_error_bound + evaluated->error_bound +
                    256 * std::numeric_limits<long double>::epsilon());
    }
  }
  EXPECT_FALSE(surface_parameter_line(surface, {0.5, 0.5}, {0.5, 0.5}, 1e-9, &error));
  EXPECT_FALSE(surface_parameter_line(surface, {-0.1, 0.5}, {0.5, 0.5}, 1e-9, &error));
  for (int i = 0; i < 9; i++) {
    EXPECT_EQ(double3(surface.points[i].position), double3(before[i].position));
    EXPECT_EQ(surface.points[i].weight, before[i].weight);
  }
}

TEST_F(NurbsTopologyEditTest, SurfaceParameterLinesCoverShiftedDomainsAndStrictFallback)
{
  std::string error;
  const std::array<double3, 3> axes = {double3(1, 0, 0), double3(0, 1, 0), double3(0, 0, 1)};
  auto patch = rational_octant_surface({0.137, -0.283, 0.091}, axes, 1e-9, &error);
  ASSERT_TRUE(patch) << error;
  NurbsSurfaceGeometry &surface = *patch->surface;
  for (int i = 0; i < 6; i++) {
    surface.knots_u[i] = i < 3 ? -2.0 : 3.0;
    surface.knots_v[i] = i < 3 ? 5.0 : 11.0;
  }
  const double2 start(-1.5, 10.0), end(2.6, 5.5);
  auto curve = surface_parameter_line(surface, start, end, 1e-9, &error);
  ASSERT_TRUE(curve) << error;
  for (int sample = 0; sample <= 32; sample++) {
    const long double t = static_cast<long double>(sample) / 32;
    const long double u = ((1 - t) * start.x + t * end.x + 2) / 5;
    const long double v = ((1 - t) * start.y + t * end.y - 5) / 6;
    const long double bu[] = {(1 - u) * (1 - u), 2 * u * (1 - u), u * u};
    const long double bv[] = {(1 - v) * (1 - v), 2 * v * (1 - v), v * v};
    std::array<long double, 3> numerator{};
    long double denominator = 0;
    for (int j = 0; j < 3; j++) {
      for (int i = 0; i < 3; i++) {
        const NurbsControlPoint &point = surface.points[3 * j + i];
        const long double basis = bu[i] * bv[j] * point.weight;
        denominator += basis;
        for (int axis = 0; axis < 3; axis++) {
          numerator[axis] += basis * point.position[axis];
        }
      }
    }
    ASSERT_GT(denominator, 0);
    const auto measured = evaluate_curve_bounded(*curve->curve, double(t), 1e-9, &error);
    ASSERT_TRUE(measured) << error;
    long double gap = 0;
    for (int axis = 0; axis < 3; axis++) {
      gap += std::abs(numerator[axis] / denominator - measured->position[axis]);
    }
    EXPECT_LE(gap,
              curve->geometry_error_bound + measured->error_bound +
                  256 * std::numeric_limits<long double>::epsilon());
  }
  for (int i = 0; i < surface.points_num; i++) {
    for (int axis = 0; axis < 3; axis++) {
      surface.points[i].position[axis] = 0;
    }
    surface.points[i].weight = 1;
  }
  auto exact = surface_parameter_line(surface, start, end, 0, &error);
  ASSERT_TRUE(exact) << error;
  EXPECT_EQ(exact->geometry_error_bound, 0);
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

TEST_F(NurbsTopologyEditTest, SavedBevelSourcesRestoreExactlyAndMatchMeshWorkflow)
{
  if (!std::getenv("BLENDER_NURBS_BEVEL_UI_READY")) {
    GTEST_SKIP() << "Run after the native bevel and mesh-reference viewport workflows";
  }
  const char *directory = std::getenv("BLENDER_NURBS_BEVEL_OUTPUT");
  ASSERT_NE(directory, nullptr);
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  BlendFileReadReport read_reports{};
  read_reports.reports = &reports;
  auto read_source = [&](const std::string &name) -> std::optional<EditSnapshot> {
    const std::string path = std::string(directory) + "/bevel-" + name + ".blend";
    auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
    if (!file) {
      ADD_FAILURE() << "Cannot read " << path;
      return std::nullopt;
    }
    BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
    if (file->main->curves.count() != 1 || file->main->objects.count() != 1) {
      ADD_FAILURE() << "Expected one native source in " << path;
      return std::nullopt;
    }
    const auto &curve = *static_cast<const Curve *>(file->main->curves.first);
    EXPECT_TRUE(curve.nurb.is_empty());
    if (!curve.nurbs_geometry) {
      ADD_FAILURE() << "Missing native NURBS source in " << path;
      return std::nullopt;
    }
    std::string error;
    EXPECT_TRUE(geometry_validate_structure(*curve.nurbs_geometry, &error)) << error;
    auto selection = selection_from_storage(curve.nurbs_geometry->stored_selection, &error);
    if (!selection) {
      ADD_FAILURE() << error;
      return std::nullopt;
    }
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    EXPECT_TRUE(tessellate_surfaces(*curve.nurbs_geometry, options, &error)) << error;
    return EditSnapshot{OwnedGeometry(geometry_copy(curve.nurbs_geometry)), std::move(*selection)};
  };
  auto expect_state_equal = [](const EditSnapshot &a, const EditSnapshot &b) {
    expect_source_geometry_equal(*a.geometry, *b.geometry);
    EXPECT_EQ(a.selection.vertices, b.selection.vertices);
    EXPECT_EQ(a.selection.edges, b.selection.edges);
    EXPECT_EQ(a.selection.faces, b.selection.faces);
    EXPECT_EQ(a.selection.active, b.selection.active);
    EXPECT_EQ(a.selection.history, b.selection.history);
    EXPECT_EQ(a.selection.mode, b.selection.mode);
  };
  for (const char *label : {"native-corner",
                            "native-box",
                            "native-corner-round",
                            "native-box-round",
                            "native-corner-vertex"})
  {
    SCOPED_TRACE(label);
    const bool closed = std::strstr(label, "box");
    const bool rounded = std::strstr(label, "round");
    const bool vertex_mode = std::strstr(label, "vertex");
    auto before = read_source(std::string(label) + "-before");
    auto cancelled = read_source(std::string(label) + "-cancelled");
    auto finished = read_source(std::string(label) + "-finished");
    auto restored = read_source(std::string(label) + "-restored");
    auto continued = read_source(std::string(label) + "-continued");
    ASSERT_TRUE(before && cancelled && finished && restored && continued);
    expect_state_equal(*before, *cancelled);
    expect_state_equal(*finished, *restored);
    EXPECT_EQ(before->selection.mode, vertex_mode ? SelectionMode::Vertex : SelectionMode::Edge);
    EXPECT_EQ(finished->selection.mode, before->selection.mode);
    ASSERT_EQ(before->selection.edges.size(), 1);
    const auto index = TopologyIndex::build(*before->geometry);
    ASSERT_TRUE(index);
    const auto &selected = *index->edge(*before->selection.edges.begin());
    for (const int64_t id : selected.vertex_ids) {
      const double3 point(index->vertex(id)->position);
      EXPECT_EQ(std::abs(point.x), 1);
      EXPECT_EQ(point.y, closed ? 0 : 2);
      EXPECT_EQ(point.z, closed ? 2 : 0);
    }
    std::string error;
    const Vector<int64_t> edges(before->selection.edges.begin(), before->selection.edges.end());
    auto result = bevel_edges(
        *before->geometry, edges, {.offset = 0.25, .segments = rounded ? 3 : 1}, &error);
    ASSERT_TRUE(result) << error;
    auto expected = edit_snapshot_from_topology_result(
        std::move(*result), before->selection, before->selection.mode, &error);
    ASSERT_TRUE(expected) << error;
    expect_state_equal(*finished, *expected);
    const auto finished_index = TopologyIndex::build(*finished->geometry);
    ASSERT_TRUE(finished_index);
    for (const auto &edge : finished->geometry->edges) {
      const int64_t uses = finished_index->uses_for_edge(edge.id).size();
      EXPECT_GT(uses, 0);
      EXPECT_LE(uses, 2);
      if (closed) {
        EXPECT_EQ(uses, 2);
      }
    }
    EXPECT_EQ(continued->selection.mode, SelectionMode::Face);
    EXPECT_GT(continued->geometry->faces.count(), finished->geometry->faces.count());
    if (!closed && !rounded) {
      compare_mesh_file("bevel-mesh-corner-finished", *finished->geometry, 1e-6, directory);
      compare_mesh_file("bevel-mesh-corner-continued", *continued->geometry, 1e-6, directory);
    }
  }
  for (const char *label : {"native-connected", "native-pyramid", "native-connected-round"}) {
    SCOPED_TRACE(label);
    const bool rounded = std::strstr(label, "round");
    auto before = read_source(std::string(label) + "-before");
    auto cancelled = read_source(std::string(label) + "-cancelled");
    auto finished = read_source(std::string(label) + "-finished");
    auto restored = read_source(std::string(label) + "-restored");
    ASSERT_TRUE(before && cancelled && finished && restored);
    expect_state_equal(*before, *cancelled);
    expect_state_equal(*finished, *restored);
    EXPECT_EQ(finished->selection.mode, SelectionMode::Edge);
    std::string error;
    const Vector<int64_t> edges(before->selection.edges.begin(), before->selection.edges.end());
    auto result = bevel_edges(
        *before->geometry, edges, {.offset = 0.25, .segments = rounded ? 3 : 1}, &error);
    ASSERT_TRUE(result) << error;
    auto expected = edit_snapshot_from_topology_result(
        std::move(*result), before->selection, before->selection.mode, &error);
    ASSERT_TRUE(expected) << error;
    expect_state_equal(*finished, *expected);
    if (!rounded) {
      compare_mesh_file(std::string("bevel-mesh-") +
                            (std::strstr(label, "pyramid") ? "pyramid" : "connected") +
                            "-finished",
                        *finished->geometry,
                        1e-6,
                        directory);
    }
  }
  if (std::getenv("BLENDER_NURBS_BEVEL_MOUSE_UI_READY")) {
    for (const char *label : {"native-corner-mouse-limits", "native-corner-round-mouse-limits"}) {
      SCOPED_TRACE(label);
      auto before = read_source(std::string(label) + "-before");
      auto valid = read_source(std::string(label) + "-last-valid");
      auto held = read_source(std::string(label) + "-held-invalid");
      auto cancelled = read_source(std::string(label) + "-held-cancelled");
      auto rejected = read_source(std::string(label) + "-rejected");
      ASSERT_TRUE(before && valid && held && cancelled && rejected);
      expect_state_equal(*valid, *held);
      expect_state_equal(*before, *cancelled);
      expect_state_equal(*before, *rejected);
    }
  }
}

TEST_F(NurbsTopologyEditTest, SavedBoundaryBlendBevelRestoresAndContinuesFromAuthoritativeSource)
{
  if (!std::getenv("BLENDER_NURBS_BEVEL_BLEND_UI_READY")) {
    GTEST_SKIP() << "Run after the skewed native bevel viewport workflows";
  }
  const char *directory = std::getenv("BLENDER_NURBS_BEVEL_BLEND_OUTPUT");
  ASSERT_NE(directory, nullptr);
  auto read = [&](const std::string &label, const char *stage) -> std::optional<EditSnapshot> {
    const std::string path = std::string(directory) + "/bevel-" + label + "-" + stage + ".blend";
    BlendFileReadReport reports{};
    auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &reports);
    if (!file) {
      ADD_FAILURE() << "Cannot read " << path;
      return std::nullopt;
    }
    BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
    if (file->main->curves.count() != 1) {
      ADD_FAILURE() << "Expected one authoritative NURBS source in " << path;
      return std::nullopt;
    }
    const auto &curve = *static_cast<const Curve *>(file->main->curves.first);
    EXPECT_TRUE(curve.nurb.is_empty());
    if (!curve.nurbs_geometry) {
      ADD_FAILURE() << "Missing native NURBS geometry in " << path;
      return std::nullopt;
    }
    std::string error;
    auto selection = selection_from_storage(curve.nurbs_geometry->stored_selection, &error);
    EXPECT_TRUE(selection) << error;
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    EXPECT_TRUE(evaluate_boundaries(*curve.nurbs_geometry, options.boundary, &error)) << error;
    EXPECT_TRUE(tessellate_surfaces(*curve.nurbs_geometry, options, &error)) << error;
    if (!selection) {
      return std::nullopt;
    }
    return EditSnapshot{OwnedGeometry(geometry_copy(curve.nurbs_geometry)), *selection};
  };
  auto same = [](const EditSnapshot &a, const EditSnapshot &b) {
    expect_source_geometry_equal(*a.geometry, *b.geometry);
    EXPECT_EQ(a.selection.vertices, b.selection.vertices);
    EXPECT_EQ(a.selection.edges, b.selection.edges);
    EXPECT_EQ(a.selection.faces, b.selection.faces);
    EXPECT_EQ(a.selection.history, b.selection.history);
    EXPECT_EQ(a.selection.active, b.selection.active);
    EXPECT_EQ(a.selection.mode, b.selection.mode);
  };
  Vector<const char *> labels = {"native-connected-round-skewed",
                                 "native-connected-round-skewed-face",
                                 "native-connected-round-clamped-skewed"};
  const char *blend_directory = directory;
  const char *planar_directory = std::getenv("BLENDER_NURBS_BEVEL_PLANAR_OUTPUT");
  if (std::getenv("BLENDER_NURBS_BEVEL_PLANAR_UI_READY")) {
    ASSERT_NE(planar_directory, nullptr);
    labels.extend({"native-connected-round-tapered",
                   "native-connected-round-tapered-face",
                   "native-connected-round-clamped-tapered"});
  }
  for (const char *label : labels) {
    directory = std::strstr(label, "tapered") ? planar_directory : blend_directory;
    SCOPED_TRACE(label);
    auto before = read(label, "before"), cancelled = read(label, "cancelled"),
         finished = read(label, "finished"), restored = read(label, "restored"),
         continued = read(label, "continued"), duplicated = read(label, "duplicated");
    ASSERT_TRUE(before && cancelled && finished && restored && continued && duplicated);
    same(*before, *cancelled);
    same(*finished, *restored);
    const Vector<int64_t> edges(before->selection.edges.begin(), before->selection.edges.end());
    std::string error;
    BevelOffsetInfo info;
    auto bevel = bevel_edges(
        *before->geometry,
        edges,
        {.offset = std::strstr(label, "clamped") ? 100.0 : 0.25, .segments = 3},
        &error,
        &info);
    ASSERT_TRUE(bevel) << error;
    EXPECT_GT(info.boundary_blend_corners, 0);
    if (std::strstr(label, "tapered")) {
      EXPECT_GT(info.planar_charts, 0);
      EXPECT_LE(info.chart_error_bound, 1e-9);
    }
    auto expected = edit_snapshot_from_topology_result(
        std::move(*bevel), before->selection, before->selection.mode, &error);
    ASSERT_TRUE(expected) << error;
    same(*finished, *expected);
    const Vector<int64_t> vertices(finished->selection.vertices.begin(),
                                   finished->selection.vertices.end());
    auto moved = translate_vertices(*finished->geometry, vertices, {0, 0, 0.15}, 1e-9, &error);
    ASSERT_TRUE(moved) << error;
    expect_source_geometry_equal(*continued->geometry, *moved->geometry);
    EXPECT_GT(duplicated->geometry->vertices.count(), finished->geometry->vertices.count());
    EXPECT_EQ(duplicated->selection.mode, finished->selection.mode);
  }
}

TEST_F(NurbsTopologyEditTest, SavedClampedBevelReplaysSourceAndSurvivesContinuedEditing)
{
  if (!std::getenv("BLENDER_NURBS_BEVEL_CLAMP_UI_READY")) {
    GTEST_SKIP() << "Run after the clamped native bevel viewport workflows";
  }
  const char *directory = std::getenv("BLENDER_NURBS_BEVEL_OUTPUT");
  ASSERT_NE(directory, nullptr);
  auto read_source = [&](const std::string &label,
                         const char *stage) -> std::optional<EditSnapshot> {
    const std::string path = std::string(directory) + "/bevel-" + label + "-" + stage + ".blend";
    ReportList reports;
    BKE_reports_init(&reports, RPT_STORE);
    BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
    BlendFileReadReport read_reports{};
    read_reports.reports = &reports;
    auto *file = BLO_read_from_file(path.c_str(), BLO_READ_SKIP_USERDEF, &read_reports);
    if (!file) {
      ADD_FAILURE() << "Cannot read " << path;
      return std::nullopt;
    }
    BLI_SCOPED_DEFER([&]() { BLO_blendfiledata_free(file); });
    if (file->main->curves.count() != 1) {
      ADD_FAILURE() << "Expected one native source in " << path;
      return std::nullopt;
    }
    const auto &curve = *static_cast<const Curve *>(file->main->curves.first);
    EXPECT_TRUE(curve.nurb.is_empty());
    if (!curve.nurbs_geometry) {
      ADD_FAILURE() << "Missing authoritative NURBS geometry in " << path;
      return std::nullopt;
    }
    std::string error;
    auto selection = selection_from_storage(curve.nurbs_geometry->stored_selection, &error);
    if (!selection) {
      ADD_FAILURE() << error;
      return std::nullopt;
    }
    auto options = preview_options();
    options.boundary.evaluation_allowance = 0;
    EXPECT_TRUE(tessellate_surfaces(*curve.nurbs_geometry, options, &error))
        << path << ": " << error;
    return EditSnapshot{OwnedGeometry(geometry_copy(curve.nurbs_geometry)), std::move(*selection)};
  };
  auto expect_state = [](const EditSnapshot &a, const EditSnapshot &b) {
    expect_source_geometry_equal(*a.geometry, *b.geometry);
    EXPECT_EQ(a.selection.vertices, b.selection.vertices);
    EXPECT_EQ(a.selection.edges, b.selection.edges);
    EXPECT_EQ(a.selection.faces, b.selection.faces);
    EXPECT_EQ(a.selection.active, b.selection.active);
    EXPECT_EQ(a.selection.history, b.selection.history);
    EXPECT_EQ(a.selection.mode, b.selection.mode);
  };
  for (const char *name : {"native-corner-mouse-clamped", "native-corner-round-mouse-clamped"}) {
    SCOPED_TRACE(name);
    auto before = read_source(name, "before");
    auto preview = read_source(name, "clamped");
    auto cancelled = read_source(name, "clamp-cancelled");
    auto confirmed = read_source(name, "clamp-confirmed");
    auto restored = read_source(name, "clamp-restored");
    auto continued = read_source(name, "clamp-continued");
    ASSERT_TRUE(before && preview && cancelled && confirmed && restored && continued);
    expect_state(*before, *cancelled);
    expect_state(*preview, *confirmed);
    expect_state(*confirmed, *restored);
    const Vector<int64_t> edges(before->selection.edges.begin(), before->selection.edges.end());
    std::string error;
    BevelOffsetInfo info;
    auto result = bevel_edges(*before->geometry,
                              edges,
                              {.offset = 100, .segments = std::strstr(name, "round") ? 3 : 1},
                              &error,
                              &info);
    ASSERT_TRUE(result) << error;
    EXPECT_LT(info.offset, 2);
    EXPECT_NEAR(info.offset, 2, 1e-12);
    auto replay = edit_snapshot_from_topology_result(
        std::move(*result), before->selection, before->selection.mode, &error);
    ASSERT_TRUE(replay) << error;
    expect_state(*confirmed, *replay);
    EXPECT_GT(continued->geometry->faces.count(), confirmed->geometry->faces.count());
    EXPECT_EQ(continued->selection.mode, SelectionMode::Face);
  }
}

}  // namespace blender::bke::nurbs::tests
