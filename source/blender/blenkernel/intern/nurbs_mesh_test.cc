/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <limits>

#ifdef WITH_GMP
#  include "BLI_math_mpq.hh"
#endif

#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_selection.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_nurbs_transform.hh"

#include "nurbs_test_geometry.hh"

namespace blender::bke::nurbs::tests {

class NurbsMeshEvaluationTest : public BlenderGTestBase {};

TEST_F(NurbsMeshEvaluationTest, TranslationPreviewTransportsOriginalDisplayAndOwnsItsMesh)
{
  for (const bool rational : {false, true}) {
    SCOPED_TRACE(rational);
    auto geometry = rational ? make_arc_sheet(true) : make_planar_sheet();
    auto original_edit = EditData::create(*geometry);
    ASSERT_NE(original_edit, nullptr);
    ObjectEvaluation original;
    original.options = preview_options();
    original.result = evaluate_mesh(original_edit->read_snapshot(), original.options);
    ASSERT_TRUE(original.result);
    ASSERT_EQ(original.result->undefined_normal_corners, 0);
    const float3 original_position = original.result->mesh->vert_positions()[0];
    /* The renderer normally owns this Mesh after object evaluation. */
    OwnedMesh renderer_mesh = std::move(original.result->mesh);
    ASSERT_NE(renderer_mesh, nullptr);
    for (const double3 offset : {double3(0.25, -0.5, 0.75), double3(0.5, -0.25, 1.25)}) {
      double4x4 matrix = double4x4::identity();
      matrix.location() = offset;
      auto moved = transform_geometry(*geometry, matrix, std::numeric_limits<double>::max());
      ASSERT_TRUE(moved);
      auto edit = EditData::create(*moved->geometry);
      ASSERT_NE(edit, nullptr);
      auto preview = translate_mesh_preview(
          edit->read_snapshot(), original, *renderer_mesh, offset);
      ASSERT_TRUE(preview);
      EXPECT_TRUE(preview->is_translation_preview);
      EXPECT_TRUE(preview->reused_tessellation);
      EXPECT_TRUE(preview->reused_normals);
      EXPECT_EQ(preview->source, edit->read_snapshot());
      EXPECT_EQ(preview->mesh_to_preview_triangle, original.result->mesh_to_preview_triangle);
      EXPECT_EQ(preview->corner_normals, original.result->corner_normals);
      const auto &wires = preview->mesh->runtime->subsurf_optimal_display_edges;
      const auto &original_wires = renderer_mesh->runtime->subsurf_optimal_display_edges;
      ASSERT_EQ(wires.size(), original_wires.size());
      for (const int i : wires.index_range()) {
        EXPECT_EQ(bool(wires[i]), bool(original_wires[i]));
      }
      for (const int i : IndexRange(preview->preview.points_num())) {
        EXPECT_EQ(preview->preview.position(i), original.result->preview.position(i) + offset);
        EXPECT_EQ(preview->mesh->vert_positions()[i], float3(preview->preview.position(i)));
      }
      ObjectEvaluation temporary;
      temporary.options = original.options;
      temporary.result = std::move(preview);
      auto handoff = evaluate_mesh(edit->read_snapshot(), original.options, nullptr, &temporary);
      ASSERT_TRUE(handoff);
      EXPECT_TRUE(handoff->is_translation_preview);
      EXPECT_TRUE(handoff->reused_evaluation);
      EXPECT_FALSE(
          translate_mesh_preview(edit->read_snapshot(), temporary, *renderer_mesh, offset));

      /* Confirmation clears the prepared evaluation and obtains a fresh source display. */
      const auto state = edit->read_snapshot();
      ASSERT_TRUE(edit->commit(state.copy_for_edit(), state));
      auto final = evaluate_mesh(edit->read_snapshot(), original.options, nullptr, &temporary);
      auto fresh = evaluate_mesh(edit->read_snapshot(), original.options);
      ASSERT_TRUE(final && fresh);
      EXPECT_FALSE(final->is_translation_preview);
      EXPECT_FALSE(final->reused_evaluation);
      for (const int i : IndexRange(final->preview.points_num())) {
        EXPECT_EQ(final->preview.position(i), fresh->preview.position(i));
      }
      temporary.result->mesh->vert_positions_for_write()[0].x = 42;
      EXPECT_EQ(renderer_mesh->vert_positions()[0], original_position);
    }
  }
}

TEST_F(NurbsMeshEvaluationTest, TranslationPreviewRejectsDeformationTrimAndDisplayChanges)
{
  auto geometry = make_planar_sheet();
  auto original_edit = EditData::create(*geometry);
  ObjectEvaluation original;
  original.options = preview_options();
  original.result = evaluate_mesh(original_edit->read_snapshot(), original.options);
  ASSERT_TRUE(original.result);
  const double3 offset(0.25, 0.5, 0.75);
  double4x4 matrix = double4x4::identity();
  matrix.location() = offset;
  auto moved = transform_geometry(*geometry, matrix, 0);
  ASSERT_TRUE(moved);
  auto check = [&](OwnedGeometry changed) {
    auto edit = EditData::create(*changed);
    ASSERT_NE(edit, nullptr);
    EXPECT_FALSE(
        translate_mesh_preview(edit->read_snapshot(), original, *original.result->mesh, offset));
  };
  {
    OwnedGeometry changed(geometry_copy(moved->geometry.get()));
    static_cast<NurbsTopologyFace *>(changed->faces.first)->material_index++;
    check(std::move(changed));
  }
  {
    OwnedGeometry changed(geometry_copy(moved->geometry.get()));
    static_cast<NurbsTopologyFace *>(changed->faces.first)->orientation *= -1;
    check(std::move(changed));
  }
  {
    OwnedGeometry changed(geometry_copy(moved->geometry.get()));
    static_cast<NurbsSurfaceGeometry *>(changed->surfaces.first)->points[0].position[2] += 1e-12;
    check(std::move(changed));
  }
  {
    OwnedGeometry changed(geometry_copy(moved->geometry.get()));
    for (auto &curve : changed->curves) {
      if (curve.dimension == 2) {
        curve.points[0].position[0] += 0.01;
        break;
      }
    }
    check(std::move(changed));
  }
  {
    const int64_t vertex = static_cast<NurbsTopologyVertex *>(geometry->vertices.first)->id;
    auto partial = translate_vertices(*geometry, {vertex}, offset, 1e-9);
    ASSERT_TRUE(partial);
    check(std::move(partial->geometry));
  }
  matrix.location().x = 1e12;
  auto distant = transform_geometry(*geometry, matrix, std::numeric_limits<double>::max());
  ASSERT_TRUE(distant);
  auto edit = EditData::create(*distant->geometry);
  ASSERT_NE(edit, nullptr);
  EXPECT_FALSE(translate_mesh_preview(
      edit->read_snapshot(), original, *original.result->mesh, matrix.location()));
  auto valid = EditData::create(*moved->geometry);
  ASSERT_NE(valid, nullptr);
  original.result->undefined_normal_corners = 1;
  EXPECT_FALSE(
      translate_mesh_preview(valid->read_snapshot(), original, *original.result->mesh, offset));
}

TEST_F(NurbsMeshEvaluationTest, PreparedEditSharesImmutableSourceAndReusesIndependentDisplayMesh)
{
  auto geometry = make_planar_sheet();
  ASSERT_NE(geometry, nullptr);
  auto edit = EditData::create(*geometry);
  const ReadSnapshot original = edit->read_snapshot();
  auto prepared_edit = EditData::create(original.copy_for_edit());
  ASSERT_NE(prepared_edit, nullptr);
  auto prepared = std::make_shared<ObjectEvaluation>();
  prepared->options = preview_options();
  prepared->result = evaluate_mesh(prepared_edit->read_snapshot(), prepared->options);
  ASSERT_TRUE(prepared->result);
  std::string error;
  ASSERT_TRUE(edit->commit_evaluated(prepared, original, &error)) << error;
  const ReadSnapshot accepted = edit->read_snapshot();
  EXPECT_EQ(accepted.revision(), original.revision() + 1);
  EXPECT_NE(accepted, prepared->result->source);
  EXPECT_EQ(&accepted.geometry(), &prepared->result->source.geometry());
  EXPECT_EQ(edit->prepared_evaluation(accepted), prepared);
  EXPECT_EQ(edit->prepared_evaluation(original), nullptr);

  auto reused = evaluate_mesh(accepted, prepared->options, &error, prepared.get());
  ASSERT_TRUE(reused) << error;
  EXPECT_TRUE(reused->reused_evaluation);
  EXPECT_EQ(reused->source, accepted);
  EXPECT_NE(reused->mesh.get(), prepared->result->mesh.get());
  const auto a = reused->mesh->corner_normals();
  const auto b = prepared->result->mesh->corner_normals();
  ASSERT_EQ(a.size(), b.size());
  for (const int i : a.index_range()) {
    EXPECT_EQ(a[i], b[i]);
  }
  const float3 first_position = prepared->result->mesh->vert_positions()[0];
  reused->mesh->vert_positions_for_write()[0].x += 1;
  EXPECT_EQ(prepared->result->mesh->vert_positions()[0], first_position);

  auto changed_options = prepared->options;
  changed_options.interior_subdivisions++;
  const auto changed = evaluate_mesh(accepted, changed_options, &error, prepared.get());
  ASSERT_TRUE(changed) << error;
  EXPECT_FALSE(changed->reused_evaluation);
  EXPECT_FALSE(changed->reused_tessellation);

  EXPECT_FALSE(edit->commit_evaluated(prepared, original, &error));
  EXPECT_EQ(edit->read_snapshot(), accepted);
  EXPECT_EQ(edit->prepared_evaluation(accepted), prepared);
  EXPECT_FALSE(edit->commit_evaluated(std::make_shared<ObjectEvaluation>(), accepted, &error));
  EXPECT_EQ(edit->read_snapshot(), accepted);
  ASSERT_TRUE(edit->commit(accepted.copy_for_edit(), accepted, &error)) << error;
  EXPECT_EQ(edit->prepared_evaluation(edit->read_snapshot()), nullptr);
  auto fresh = evaluate_mesh(edit->read_snapshot(), prepared->options, &error, prepared.get());
  ASSERT_TRUE(fresh) << error;
  EXPECT_FALSE(fresh->reused_evaluation);
  EXPECT_EQ(original.geometry().faces.count(), 1);
}

TEST_F(NurbsMeshEvaluationTest, RegularSourceNormalsUseFreeFloatStorageAndSurviveMeshCopies)
{
  auto geometry = make_arc_sheet(false);
  auto edit = EditData::create(*geometry);
  ASSERT_NE(edit, nullptr);
  auto evaluated = evaluate_mesh(edit->read_snapshot(), preview_options());
  ASSERT_TRUE(evaluated);
  ASSERT_EQ(evaluated->undefined_normal_corners, 0);
  const auto meta = evaluated->mesh->attributes().lookup_meta_data("custom_normal");
  ASSERT_TRUE(meta);
  EXPECT_EQ(meta->data_type, AttrType::Float3);
  EXPECT_EQ(meta->domain, AttrDomain::Corner);
  const auto normals = evaluated->mesh->corner_normals();
  ASSERT_EQ(normals.size(), evaluated->corner_normals.size());
  OwnedMesh copy(BKE_mesh_copy_for_eval(*evaluated->mesh));
  const auto copied_normals = copy->corner_normals();
  for (const int i : normals.index_range()) {
    EXPECT_EQ(normals[i], evaluated->corner_normals[i]);
    EXPECT_EQ(copied_normals[i], normals[i]);
  }
}

TEST_F(NurbsMeshEvaluationTest, UndefinedInteriorSourceFramesRetainAutomaticNormalFallback)
{
  auto geometry = make_planar_sheet();
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(geometry->surfaces.first);
  MEM_SAFE_DELETE(surface.points);
  MEM_SAFE_DELETE(surface.knots_u);
  surface.order_u = surface.points_u = 4;
  surface.points_num = 8;
  surface.knots_u_num = 8;
  surface.points = MEM_new_array<NurbsControlPoint>(8, __func__);
  surface.knots_u = MEM_new_array<double>(8, __func__);
  /* x=(2u-1)^3 has a stationary interior derivative at u=1/2, while its sampled faces
   * remain nondegenerate. The geometric limiting normal is still +Z. */
  for (int i = 0; i < 8; i++) {
    surface.knots_u[i] = i < 4 ? 0 : 1;
    surface.points[i].position[0] = i % 2 == 0 ? -1 : 1;
    surface.points[i].position[1] = i < 4 ? -1 : 1;
  }
  for (auto &curve : geometry->curves) {
    if (curve.dimension != 3 || curve.points[0].position[1] != curve.points[1].position[1]) {
      continue;
    }
    const double y = curve.points[0].position[1];
    const double first_x = curve.points[0].position[0];
    MEM_SAFE_DELETE(curve.points);
    MEM_SAFE_DELETE(curve.knots);
    curve.order = curve.points_num = 4;
    curve.knots_num = 8;
    curve.points = MEM_new_array<NurbsControlPoint>(4, __func__);
    curve.knots = MEM_new_array<double>(8, __func__);
    for (int i = 0; i < 8; i++) {
      curve.knots[i] = i < 4 ? 0 : 1;
    }
    for (int i = 0; i < 4; i++) {
      curve.points[i].position[0] = i % 2 == 0 ? first_x : -first_x;
      curve.points[i].position[1] = y;
    }
  }
  auto edit = EditData::create(*geometry);
  ASSERT_NE(edit, nullptr);
  auto options = preview_options();
  options.boundary.segments_per_span = options.interior_subdivisions = 4;
  std::string error;
  auto evaluated = evaluate_mesh(edit->read_snapshot(), options, &error);
  ASSERT_TRUE(evaluated) << error;
  EXPECT_GT(evaluated->undefined_normal_corners, 0);
  const auto meta = evaluated->mesh->attributes().lookup_meta_data("custom_normal");
  ASSERT_TRUE(meta);
  EXPECT_NE(meta->data_type, AttrType::Float3);
  for (const float3 normal : evaluated->mesh->corner_normals()) {
    EXPECT_NEAR(normal.x, 0, 1e-6);
    EXPECT_NEAR(normal.y, 0, 1e-6);
    EXPECT_NEAR(normal.z, 1, 1e-6);
  }
}

TEST_F(NurbsMeshEvaluationTest, CachedPreviewMatchesFreshGeometryAndInvalidatesOnChanges)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto original_edit = EditData::create(*source);
  ASSERT_NE(original_edit, nullptr);
  const auto options = preview_options();
  std::string error;
  ObjectEvaluation previous;
  previous.options = options;
  previous.result = evaluate_mesh(original_edit->read_snapshot(), options, &error);
  ASSERT_TRUE(previous.result) << error;

  auto compare = [&](const MeshEvaluation &cached, const MeshEvaluation &fresh) {
    ASSERT_EQ(cached.preview.points_num(), fresh.preview.points_num());
    ASSERT_EQ(cached.preview.triangles.size(), fresh.preview.triangles.size());
    for (const int i : IndexRange(cached.preview.points_num())) {
      EXPECT_EQ(cached.preview.position(i), fresh.preview.position(i));
    }
    for (const int i : cached.preview.triangles.index_range()) {
      const auto &a = cached.preview.triangles[i], &b = fresh.preview.triangles[i];
      EXPECT_EQ(a.face_id, b.face_id);
      EXPECT_EQ(a.points, b.points);
      EXPECT_EQ(a.display_edges, b.display_edges);
      for (const int j : IndexRange(3)) {
        EXPECT_EQ(a.parameters[j], b.parameters[j]);
      }
    }
    ASSERT_EQ(cached.mesh->verts_num, fresh.mesh->verts_num);
    ASSERT_EQ(cached.mesh->faces_num, fresh.mesh->faces_num);
    const auto a_normals = cached.mesh->corner_normals(), b_normals = fresh.mesh->corner_normals();
    ASSERT_EQ(a_normals.size(), b_normals.size());
    for (const int i : a_normals.index_range()) {
      EXPECT_EQ(a_normals[i], b_normals[i]);
    }
  };

  Vector<int64_t> vertices;
  for (const auto &vertex : source->vertices) {
    vertices.append(vertex.id);
  }
  /* Exercise both a rigid drag and a one-corner deformation of the same UV chart. */
  for (const bool whole_sheet : {true, false}) {
    SCOPED_TRACE(whole_sheet);
    const Span<int64_t> moving = whole_sheet ? vertices.as_span() :
                                               vertices.as_span().take_front(1);
    auto moved = translate_vertices(
        *source, moving, {0.123456789, 0.234567891, 0.345678912}, 1e-9, &error);
    ASSERT_TRUE(moved) << error;
    auto edit = EditData::create(*moved->geometry);
    ASSERT_NE(edit, nullptr);
    const auto snapshot = edit->read_snapshot();
    const auto cached = evaluate_mesh(snapshot, options, &error, &previous);
    ASSERT_TRUE(cached) << error;
    EXPECT_TRUE(cached->reused_tessellation);
    const auto fresh = evaluate_mesh(snapshot, options, &error);
    ASSERT_TRUE(fresh) << error;
    compare(*cached, *fresh);
  }

  auto denser = options;
  denser.interior_subdivisions++;
  const auto changed_resolution = evaluate_mesh(
      original_edit->read_snapshot(), denser, &error, &previous);
  ASSERT_TRUE(changed_resolution) << error;
  EXPECT_FALSE(changed_resolution->reused_tessellation);
  const auto fresh_resolution = evaluate_mesh(original_edit->read_snapshot(), denser, &error);
  ASSERT_TRUE(fresh_resolution) << error;
  compare(*changed_resolution, *fresh_resolution);

  OwnedGeometry reversed(geometry_copy(source.get()));
  static_cast<NurbsTopologyFace *>(reversed->faces.first)->orientation *= -1;
  auto reversed_edit = EditData::create(*reversed);
  ASSERT_NE(reversed_edit, nullptr);
  const auto reversed_cache = evaluate_mesh(
      reversed_edit->read_snapshot(), options, &error, &previous);
  ASSERT_TRUE(reversed_cache) << error;
  EXPECT_FALSE(reversed_cache->reused_tessellation);
  const auto reversed_fresh = evaluate_mesh(reversed_edit->read_snapshot(), options, &error);
  ASSERT_TRUE(reversed_fresh) << error;
  compare(*reversed_cache, *reversed_fresh);
}

TEST_F(NurbsMeshEvaluationTest, BevelWidthRefreshRebuildsOnlyChangedTrimsAndMatchesFreshPreview)
{
  auto sheet = make_planar_sheet();
  const int64_t face = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  auto box = extrude_components(*sheet, {{}, {}, {&face, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box) << error;
  Vector<int64_t> edges;
  for (const auto &edge : box->geometry->edges) {
    edges.append(edge.id);
  }
  ObjectEvaluation previous;
  previous.options = preview_options();
  for (const double width : {0.2, 0.25, 0.3}) {
    auto bevel = bevel_edges(*box->geometry, edges, {.offset = width, .segments = 13}, &error);
    ASSERT_TRUE(bevel) << error;
    auto edit = EditData::create(*bevel->geometry);
    ASSERT_NE(edit, nullptr);
    auto cached = evaluate_mesh(edit->read_snapshot(), previous.options, &error, &previous);
    ASSERT_TRUE(cached) << error;
    auto fresh = evaluate_mesh(edit->read_snapshot(), previous.options, &error);
    ASSERT_TRUE(fresh) << error;
    if (previous.result) {
      EXPECT_EQ(cached->preview.reused_faces, bevel->geometry->faces.count() - 6);
      EXPECT_FALSE(cached->reused_tessellation);
    }
    ASSERT_EQ(cached->preview.points_num(), fresh->preview.points_num());
    for (const int i : IndexRange(cached->preview.points_num())) {
      EXPECT_EQ(cached->preview.position(i), fresh->preview.position(i));
    }
    ASSERT_EQ(cached->preview.triangles.size(), fresh->preview.triangles.size());
    for (const int i : cached->preview.triangles.index_range()) {
      const auto &a = cached->preview.triangles[i], &b = fresh->preview.triangles[i];
      EXPECT_EQ(a.face_id, b.face_id);
      EXPECT_EQ(a.points, b.points);
      EXPECT_EQ(a.display_edges, b.display_edges);
      for (const int j : IndexRange(3)) {
        EXPECT_EQ(a.parameters[j], b.parameters[j]);
      }
    }
    EXPECT_EQ(cached->preview.skipped_repeated_point_triangles,
              fresh->preview.skipped_repeated_point_triangles);
    EXPECT_EQ(cached->preview.max_interior_weight_condition,
              fresh->preview.max_interior_weight_condition);
    const auto a = cached->mesh->corner_normals(), b = fresh->mesh->corner_normals();
    ASSERT_EQ(a.size(), b.size());
    for (const int i : a.index_range()) {
      EXPECT_EQ(a[i], b[i]);
    }
    previous.result = std::move(cached);
  }
}

TEST_F(NurbsMeshEvaluationTest, TranslationNormalsIgnoreUnusedParentsButRejectCoefficientChanges)
{
  auto source = make_planar_sheet();
  const auto &surface = *static_cast<const NurbsSurfaceGeometry *>(source->surfaces.first);
  auto *unused = MEM_dupalloc(&surface);
  unused->id = source->next_id++;
  unused->points = MEM_dupalloc(surface.points);
  unused->knots_u = MEM_dupalloc(surface.knots_u);
  unused->knots_v = MEM_dupalloc(surface.knots_v);
  BLI_addtail(&source->surfaces, unused);
  auto original_edit = EditData::create(*source);
  ASSERT_NE(original_edit, nullptr);
  ObjectEvaluation previous;
  previous.options = preview_options();
  previous.result = evaluate_mesh(original_edit->read_snapshot(), previous.options);
  ASSERT_TRUE(previous.result);
  double4x4 matrix = double4x4::identity();
  matrix.location() = double3(0.125, 0.25, 0.5);
  auto moved = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(moved);
  for (auto &support : moved->geometry->surfaces) {
    if (support.id == unused->id) {
      std::copy_n(unused->points, unused->points_num, support.points);
    }
  }
  auto current_edit = EditData::create(*moved->geometry);
  ASSERT_NE(current_edit, nullptr);
  auto evaluated = evaluate_mesh(
      current_edit->read_snapshot(), previous.options, nullptr, &previous);
  ASSERT_TRUE(evaluated);
  EXPECT_TRUE(evaluated->reused_tessellation);
  EXPECT_TRUE(evaluated->reused_normals);
  const auto a = evaluated->mesh->corner_normals();
  const auto b = previous.result->mesh->corner_normals();
  ASSERT_EQ(a.size(), b.size());
  for (const int i : a.index_range()) {
    EXPECT_EQ(a[i], b[i]);
  }
  auto changed = current_edit->read_snapshot().copy_for_edit();
  auto &changed_support = *static_cast<NurbsSurfaceGeometry *>(changed.geometry->surfaces.first);
  changed_support.points[0].position[2] = std::nextafter(changed_support.points[0].position[2],
                                                         INFINITY);
  static_cast<NurbsTopologyFace *>(changed.geometry->faces.first)->tolerance = 1e-8;
  auto changed_edit = EditData::create(std::move(changed));
  ASSERT_NE(changed_edit, nullptr);
  std::string error;
  auto not_translation = evaluate_mesh(
      changed_edit->read_snapshot(), previous.options, &error, &previous);
  ASSERT_TRUE(not_translation) << error;
  EXPECT_TRUE(not_translation->reused_tessellation);
  EXPECT_FALSE(not_translation->reused_normals);
}

TEST_F(NurbsMeshEvaluationTest, CachedRoundedBoxMatchesFreshPreviewAfterTranslation)
{
  auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  const int64_t face = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  auto box = extrude_components(*sheet, {{}, {}, {&face, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box) << error;
  Vector<int64_t> edges;
  for (const auto &edge : box->geometry->edges) {
    edges.append(edge.id);
  }
  auto bevel = bevel_edges(*box->geometry, edges, {.offset = 0.2, .segments = 3}, &error);
  ASSERT_TRUE(bevel) << error;
  auto original_edit = EditData::create(*bevel->geometry);
  ASSERT_NE(original_edit, nullptr);
  const auto options = preview_options();
  ObjectEvaluation previous;
  previous.options = options;
  previous.result = evaluate_mesh(original_edit->read_snapshot(), options, &error);
  ASSERT_TRUE(previous.result) << error;
  Vector<int64_t> vertices;
  for (const auto &vertex : bevel->geometry->vertices) {
    vertices.append(vertex.id);
  }
  auto moved = translate_vertices(*bevel->geometry, vertices, {0.125, 0.25, 0.5}, 1e-9, &error);
  ASSERT_TRUE(moved) << error;
  auto edit = EditData::create(*moved->geometry);
  ASSERT_NE(edit, nullptr);
  const auto cached = evaluate_mesh(edit->read_snapshot(), options, &error, &previous);
  ASSERT_TRUE(cached) << error;
  EXPECT_TRUE(cached->reused_tessellation);
  const auto fresh = evaluate_mesh(edit->read_snapshot(), options, &error);
  ASSERT_TRUE(fresh) << error;
  ASSERT_EQ(cached->preview.points_num(), fresh->preview.points_num());
  ASSERT_EQ(cached->preview.triangles.size(), fresh->preview.triangles.size());
  for (const int i : IndexRange(cached->preview.points_num())) {
    EXPECT_EQ(cached->preview.position(i), fresh->preview.position(i));
  }
  for (const int i : cached->preview.triangles.index_range()) {
    EXPECT_EQ(cached->preview.triangles[i].points, fresh->preview.triangles[i].points);
    EXPECT_EQ(cached->preview.triangles[i].face_id, fresh->preview.triangles[i].face_id);
    EXPECT_EQ(cached->preview.triangles[i].display_edges,
              fresh->preview.triangles[i].display_edges);
  }
  const auto cached_normals = cached->mesh->corner_normals();
  const auto fresh_normals = fresh->mesh->corner_normals();
  ASSERT_EQ(cached_normals.size(), fresh_normals.size());
  for (const int i : cached_normals.index_range()) {
    EXPECT_EQ(cached_normals[i], fresh_normals[i]);
  }
}

TEST_F(NurbsMeshEvaluationTest, RectangularSurfaceShowsAQuadGridAtDifferentResolutions)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  const auto snapshot = edit->read_snapshot();
  for (const int resolution : {1, 4, 7, 12, 13}) {
    SCOPED_TRACE(resolution);
    auto options = preview_options();
    options.boundary.segments_per_span = options.interior_subdivisions = resolution;
    std::string error;
    const auto result = evaluate_mesh(snapshot, options, &error);
    ASSERT_TRUE(result.has_value()) << error;
    const Mesh &mesh = *result->mesh;
    const auto &visible = mesh.runtime->subsurf_optimal_display_edges;
    ASSERT_EQ(visible.size(), mesh.edges_num);
    int wires = 0;
    for (const int i : mesh.edges().index_range()) {
      if (visible[i]) {
        const int2 edge = mesh.edges()[i];
        const float3 a = mesh.vert_positions()[edge.x], b = mesh.vert_positions()[edge.y];
        /* Source evaluation and the float display conversion can round the two ends of an
         * isoparametric segment differently, particularly around zero at nonbinary samples. */
        EXPECT_LE(std::min(std::abs(a.x - b.x), std::abs(a.y - b.y)), 1.0e-6f);
        wires++;
      }
    }
    EXPECT_EQ(wires, 2 * resolution * (resolution + 1));
    EXPECT_EQ(mesh.faces_num, 2 * resolution * resolution);
    EXPECT_EQ(mesh.edges_num - wires, resolution * resolution);
    EXPECT_EQ(result->source, snapshot);
    EXPECT_EQ(snapshot.geometry().faces.count(), 1);
    EXPECT_EQ(snapshot.geometry().edges.count(), 4);
    OwnedMesh copy(BKE_mesh_copy_for_eval(mesh));
    ASSERT_EQ(copy->runtime->subsurf_optimal_display_edges.size(), visible.size());
    for (const int i : visible.index_range()) {
      EXPECT_EQ(bool(copy->runtime->subsurf_optimal_display_edges[i]), bool(visible[i]));
    }
    copy->tag_edges_split();
    EXPECT_TRUE(copy->runtime->subsurf_optimal_display_edges.is_empty());
  }
}

TEST_F(NurbsMeshEvaluationTest, BilinearDiagonalGapUsesItsCoefficientBoundWithoutWideningTolerance)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto &face = *static_cast<NurbsTopologyFace *>(source->faces.first);
  for (auto &loop : face.loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&face.loops);
  const double2 corners[] = {{0, 0}, {1, 0}, {0, 1}};
  add_planar_loop(*source, face, {corners, 3}, false);
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
  surface.points[3].position[2] = 0.125;
  std::string error;
  // A real unsampled midpoint gap still fails; this is not an evaluator epsilon.
  EXPECT_FALSE(evaluate_boundaries(*source, {1, 1000, 0}, &error));
  EXPECT_NE(error.find("edge/support"), std::string::npos);
  face.tolerance = std::nextafter(0.03125, 0.0);
  EXPECT_FALSE(evaluate_boundaries(*source, {7, 1000, 0}, &error));
  face.tolerance = 0.03125;
  const auto result = evaluate_boundaries(*source, {7, 1000, 0}, &error);
  ASSERT_TRUE(result.has_value()) << error;
  int diagonals = 0;
  for (const auto &use : result->uses) {
    ASSERT_TRUE(use.curve_support_deviation_bound.has_value());
    if (*use.curve_support_deviation_bound != 0) {
      EXPECT_EQ(*use.curve_support_deviation_bound, 0.03125);
      EXPECT_FALSE(use.exact_curve_support_identity);
      EXPECT_GT(use.max_curve_support_deviation, 0);
      EXPECT_LE(use.max_curve_support_deviation, *use.curve_support_deviation_bound);
      diagonals++;
    }
  }
  EXPECT_EQ(diagonals, 1);
  EXPECT_EQ(face.tolerance, 0.03125);
  EXPECT_EQ(surface.points[3].position[2], 0.125);
}

TEST_F(NurbsMeshEvaluationTest, CollapsedBoxKeepsQuadBaseAndTriangularWallGrids)
{
  auto sheet = make_planar_sheet();
  ASSERT_NE(sheet, nullptr);
  const int64_t bottom = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  SelectionState initial;
  initial.mode = SelectionMode::Face;
  const auto selection = selection_apply(
      *box->geometry, initial, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(selection.has_value()) << error;
  const auto merged = merge_selected_vertices(
      *box->geometry, *selection, VertexMergeMode::Collapse, {}, 0, &error);
  ASSERT_TRUE(merged.has_value()) << error;
  const auto edit = EditData::create(*merged->geometry);
  ASSERT_NE(edit, nullptr);
  const auto snapshot = edit->read_snapshot();
  auto options = preview_options();
  options.boundary.segments_per_span = options.interior_subdivisions = 7;
  options.boundary.evaluation_allowance = 0;
  const auto result = evaluate_mesh(snapshot, options, &error);
  ASSERT_TRUE(result.has_value()) << error;
  for (const auto &use : result->preview.boundary.uses) {
    EXPECT_TRUE(use.exact_curve_support_identity);
  }
  for (const auto &face : snapshot.geometry().faces) {
    EXPECT_EQ(face.tolerance, 0);
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        EXPECT_EQ(use.tolerance, 0);
      }
    }
  }
  Set<int64_t> triangular_faces;
  int hidden_base_diagonals = 0;
  for (const auto &triangle : result->preview.triangles) {
    for (const int i : IndexRange(3)) {
      if (triangle.face_id == bottom) {
        hidden_base_diagonals += !triangle.display_edges[i];
      }
      else {
        EXPECT_TRUE(triangle.display_edges[i]);
        triangular_faces.add(triangle.face_id);
      }
    }
  }
  EXPECT_EQ(triangular_faces.size(), 4);
  EXPECT_EQ(hidden_base_diagonals, 2 * 7 * 7);
  const auto &visible = result->mesh->runtime->subsurf_optimal_display_edges;
  const auto corners = result->mesh->corner_edges();
  for (const int face : result->mesh_to_preview_triangle.index_range()) {
    const auto &triangle = result->preview.triangles[result->mesh_to_preview_triangle[face]];
    for (const int corner : IndexRange(3)) {
      EXPECT_EQ(bool(visible[corners[3 * face + corner]]), triangle.display_edges[corner]);
    }
  }
  EXPECT_EQ(snapshot.geometry().vertices.count(), 5);
  EXPECT_EQ(snapshot.geometry().edges.count(), 8);
  EXPECT_EQ(snapshot.geometry().faces.count(), 5);
  const auto deleted = delete_components(
      *merged->geometry, {merged->vertices, {}, {}}, DeleteMode::Vertices, &error);
  ASSERT_TRUE(deleted.has_value()) << error;
  EXPECT_EQ(deleted->geometry->faces.count(), 1);
  const auto remaining = EditData::create(*deleted->geometry);
  ASSERT_NE(remaining, nullptr);
  const auto remaining_mesh = evaluate_mesh(remaining->read_snapshot(), options, &error);
  ASSERT_TRUE(remaining_mesh.has_value()) << error;
  int hidden = 0;
  for (const int i : remaining_mesh->mesh->runtime->subsurf_optimal_display_edges.index_range()) {
    hidden += !remaining_mesh->mesh->runtime->subsurf_optimal_display_edges[i];
  }
  EXPECT_EQ(hidden, 7 * 7);
  /* A tiny real gap invalidates the identity proof and still fails at zero source tolerance. */
  OwnedGeometry broken(geometry_copy(&snapshot.geometry()));
  const auto index = TopologyIndex::build(*broken);
  ASSERT_TRUE(index.has_value());
  int64_t side_surface = 0;
  for (const auto &face : broken->faces) {
    if (face.id != bottom) {
      side_surface = face.surface_id;
      break;
    }
  }
  ASSERT_NE(side_surface, 0);
  for (auto &surface : broken->surfaces) {
    if (surface.id == side_surface) {
      surface.points[3].position[2] += std::ldexp(1.0, -40);
      break;
    }
  }
  EXPECT_FALSE(evaluate_boundaries(*broken, options.boundary, &error));
  EXPECT_NE(error.find("edge/support"), std::string::npos);
}

TEST_F(NurbsMeshEvaluationTest, SplittingABoundaryEdgeDoesNotTurnARectangularGridIntoTriangles)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t edge = static_cast<const NurbsTopologyEdge *>(source->edges.first)->id;
  TopologyCutPlan plan;
  plan.edges.append({edge, {0.5}});
  std::string error;
  const auto split = partition_components(*source, plan, 0, &error);
  ASSERT_TRUE(split.has_value()) << error;
  ASSERT_EQ(split->edit.geometry->faces.count(), 1);
  ASSERT_EQ(split->edit.geometry->edges.count(), 5);
  const auto edit = EditData::create(*split->edit.geometry);
  ASSERT_NE(edit, nullptr);
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  const Mesh &mesh = *result->mesh;
  const auto &visible = mesh.runtime->subsurf_optimal_display_edges;
  int wires = 0;
  for (const int i : mesh.edges().index_range()) {
    if (visible[i]) {
      const int2 wire = mesh.edges()[i];
      const float3 a = mesh.vert_positions()[wire.x], b = mesh.vert_positions()[wire.y];
      EXPECT_TRUE(a.x == b.x || a.y == b.y);
      wires++;
    }
  }
  /* 4x4 grid, with four extra sample intervals on the split source boundary. */
  EXPECT_EQ(wires, 2 * 4 * 5 + 4);
}

TEST_F(NurbsMeshEvaluationTest, SheetMeshRetainsSourceFaceProvenanceAndIndependentGeometry)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  static_cast<NurbsTopologyFace *>(source->faces.first)->material_index = 2;
  const auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot snapshot = edit->read_snapshot();
  std::string error;
  auto result = evaluate_mesh(snapshot, preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  ASSERT_NE(result->mesh, nullptr);
  EXPECT_EQ(result->source, snapshot);
  EXPECT_EQ(result->mesh->verts_num, result->preview.points_num());
  EXPECT_EQ(result->mesh->faces_num, result->preview.triangles.size());
  EXPECT_GT(result->mesh->faces_num, 1);
  EXPECT_EQ(result->mesh->corners_num, 3 * result->mesh->faces_num);
  const auto materials = result->mesh->attributes().lookup<int>("material_index",
                                                                AttrDomain::Face);
  const auto sharp = result->mesh->attributes().lookup<bool>("sharp_face", AttrDomain::Face);
  ASSERT_TRUE(materials);
  ASSERT_TRUE(sharp);
  for (const int i : IndexRange(result->mesh->faces_num)) {
    EXPECT_EQ(materials.varray[i], 2);
    EXPECT_FALSE(sharp.varray[i]);
  }
  EXPECT_DOUBLE_EQ(result->max_float_position_deviation, 0);
  EXPECT_EQ(result->undefined_normal_corners, 0);
  for (const float3 normal : result->mesh->corner_normals()) {
    EXPECT_NEAR(normal.x, 0, 1.0e-5);
    EXPECT_NEAR(normal.y, 0, 1.0e-5);
    EXPECT_NEAR(normal.z, 1, 1.0e-5);
  }
  result->mesh->vert_positions_for_write()[0].x = 42;
  EXPECT_EQ(
      static_cast<const NurbsTopologyVertex *>(snapshot.geometry().vertices.first)->position[0],
      -1);
  EXPECT_EQ(snapshot.geometry().faces.count(), 1);
  EXPECT_EQ(snapshot.geometry().edges.count(), 4);
}

TEST_F(NurbsMeshEvaluationTest, FloatQuantizationIsReportedWithoutRoundingAuthoritativeDoubles)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 matrix = double4x4::identity();
  matrix.location().x = 1.0e12 + 0.125;
  auto translated = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(translated.has_value());
  const auto edit = EditData::create(*translated->geometry);
  ASSERT_NE(edit, nullptr);
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_GT(result->max_float_position_deviation, 4000);
  EXPECT_LT(result->max_float_position_deviation, 5000);
  const double original =
      static_cast<const NurbsTopologyVertex *>(edit->geometry().vertices.first)->position[0];
  EXPECT_DOUBLE_EQ(original, 1.0e12 - 0.875);
  EXPECT_NE(double(result->mesh->vert_positions()[0].x), original);
  EXPECT_DOUBLE_EQ(result->preview.position(0).x, original);
  EXPECT_EQ(result->mesh->faces_num, 0);
  EXPECT_EQ(result->skipped_degenerate_float_triangles, result->preview.triangles.size());
  EXPECT_TRUE(result->mesh_to_preview_triangle.is_empty());
  EXPECT_EQ(edit->revision(), 1);
}

TEST_F(NurbsMeshEvaluationTest, FloatOverflowFailsExplicitlyWithSourceAndSelectionUnchanged)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 matrix = double4x4::identity();
  matrix[0][0] = 1.0e40;
  auto scaled = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(scaled.has_value());
  const auto edit = EditData::create(*scaled->geometry);
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot snapshot = edit->read_snapshot();
  const int64_t counter = snapshot.geometry().next_id;
  std::string error;
  EXPECT_FALSE(evaluate_mesh(snapshot, preview_options(), &error).has_value());
  EXPECT_NE(error.find("float display"), std::string::npos);
  EXPECT_EQ(edit->read_snapshot(), snapshot);
  EXPECT_EQ(edit->geometry().next_id, counter);
  EXPECT_EQ(edit->geometry().faces.count(), 1);
}

TEST_F(NurbsMeshEvaluationTest, WireDisplayRetainsCoincidentVerticesWithDistinctSourceIDs)
{
  OwnedGeometry source(MEM_new<NurbsGeometry>(__func__));
  const auto *a = add_vertex(*source, {-1, 0, 0});
  const auto *b = add_vertex(*source, {1, 0, 0});
  const auto *coincident = add_vertex(*source, {-1, 0, 0});
  const auto *curve = add_linear_curve(*source, double3(a->position), double3(b->position), 3);
  add_edge(*source, *a, *b, *curve);
  add_edge(*source, *coincident, *b, *curve);
  auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  const ReadSnapshot snapshot = edit->read_snapshot();
  edit.reset();
  auto options = preview_options();
  options.boundary.segments_per_span = 1;
  std::string error;
  const auto result = evaluate_mesh(snapshot, options, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->mesh->verts_num, 3);
  EXPECT_EQ(result->mesh->edges_num, 2);
  EXPECT_EQ(result->mesh->faces_num, 0);
  EXPECT_EQ(result->mesh->vert_positions()[0], result->mesh->vert_positions()[2]);
  EXPECT_NE(result->preview.boundary.vertex_ids[0], result->preview.boundary.vertex_ids[2]);
  EXPECT_EQ(result->source, snapshot);
}

TEST_F(NurbsMeshEvaluationTest, SharedNormalCachePreservesBothSidesOfAnInternalKnot)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
  MEM_delete(surface.points);
  MEM_delete(surface.knots_u);
  MEM_delete(surface.knots_v);
  surface.points_u = surface.points_v = 3;
  surface.points_num = 9;
  surface.knots_u_num = surface.knots_v_num = 5;
  surface.points = MEM_new_array<NurbsControlPoint>(9, __func__);
  surface.knots_u = MEM_new_array_uninitialized<double>(5, __func__);
  surface.knots_v = MEM_new_array_uninitialized<double>(5, __func__);
  const double knots[] = {0, 0, 0.5, 1, 1};
  std::copy_n(knots, 5, surface.knots_u);
  std::copy_n(knots, 5, surface.knots_v);
  for (const int v : IndexRange(3)) {
    for (const int u : IndexRange(3)) {
      const double3 point(-1.0 + u, -1.0 + v, u == 1 && v == 1 ? 0.5 : 0.0);
      std::copy_n(&point.x, 3, surface.points[v * 3 + u].position);
    }
  }
  auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  auto options = preview_options();
  options.boundary.segments_per_span = options.interior_subdivisions = 8;
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), options, &error);
  ASSERT_TRUE(result) << error;
  const Span<float3> normals = result->mesh->corner_normals();
  std::optional<float3> left, right;
  for (const int i : result->mesh_to_preview_triangle.index_range()) {
    const auto &triangle = result->preview.triangles[result->mesh_to_preview_triangle[i]];
    const double center_u = (triangle.parameters[0].x + triangle.parameters[1].x +
                             triangle.parameters[2].x) /
                            3;
    for (const int j : IndexRange(3)) {
      if (triangle.parameters[j] == double2(0.5, 0.25)) {
        (center_u < 0.5 ? left : right) = normals[3 * i + j];
      }
    }
  }
  ASSERT_TRUE(left && right);
  const auto expected_left = evaluate_surface_differential(
      surface, {0.5, 0.25}, {KnotSide::Left, KnotSide::Right});
  const auto expected_right = evaluate_surface_differential(
      surface, {0.5, 0.25}, {KnotSide::Right, KnotSide::Right});
  ASSERT_TRUE(expected_left && expected_left->normal && expected_right && expected_right->normal);
  const float orientation = float(
      static_cast<const NurbsTopologyFace *>(source->faces.first)->orientation);
  EXPECT_GT(math::dot(*left, float3(*expected_left->normal) * orientation), 0.999f);
  EXPECT_GT(math::dot(*right, float3(*expected_right->normal) * orientation), 0.999f);
  EXPECT_LT(math::dot(*left, *right), 0.95f);
}

TEST_F(NurbsMeshEvaluationTest, RationalCylinderUsesRadialSourceNormalsAtDisplayCorners)
{
  const auto source = make_arc_sheet(true);
  ASSERT_NE(source, nullptr);
  const auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->undefined_normal_corners, 0);
  const auto normals = result->mesh->corner_normals();
  const auto positions = result->mesh->vert_positions();
  const auto corners = result->mesh->corner_verts();
  ASSERT_EQ(normals.size(), corners.size());
  for (const int i : corners.index_range()) {
    const float3 point = positions[corners[i]];
    const float3 radial = math::normalize(float3(point.x, point.y, 0));
    EXPECT_GT(math::dot(normals[i], radial), 0.9999f);
    EXPECT_LT(std::abs(normals[i].z), 1.0e-3f);
  }
  EXPECT_EQ(source->faces.count(), 1);
  EXPECT_EQ(source->vertices.count(), 2);
  const auto &visible = result->mesh->runtime->subsurf_optimal_display_edges;
  int hidden = 0;
  for (const int i : visible.index_range()) {
    hidden += !visible[i];
  }
  /* Four rational knot spans around the seam, each with four U and four V subdivisions. */
  EXPECT_EQ(hidden, 4 * 4 * 4);
  EXPECT_EQ(visible.size() - hidden, 16 * 5 + 16 * 4);
}

TEST_F(NurbsMeshEvaluationTest, SourceNormalsPreserveG0FaceJoinWithoutAveragingItsTangents)
{
  auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  const int64_t original_face = static_cast<NurbsTopologyFace *>(source->faces.first)->id;
  const int64_t common_edge = static_cast<NurbsTopologyEdge *>(source->edges.first)->id;
  auto swept = extrude_edges(*source, {common_edge}, {0, 0, 2}, 0);
  ASSERT_TRUE(swept.has_value());
  source = std::move(swept->geometry);
  const auto edit = EditData::create(*source);
  ASSERT_NE(edit, nullptr);
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->undefined_normal_corners, 0);
  const auto normals = result->mesh->corner_normals();
  for (const int i : result->preview.triangles.index_range()) {
    const bool horizontal = result->preview.triangles[i].face_id == original_face;
    for (const int j : IndexRange(3)) {
      const float3 normal = normals[i * 3 + j];
      if (horizontal) {
        EXPECT_NEAR(normal.z, 1, 1.0e-4f);
        EXPECT_NEAR(normal.y, 0, 1.0e-4f);
      }
      else {
        EXPECT_NEAR(std::abs(normal.y), 1, 1.0e-4f);
        EXPECT_NEAR(normal.z, 0, 1.0e-4f);
      }
    }
  }
  const auto index = TopologyIndex::build(*source);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->uses_for_edge(common_edge).size(), 2);
  EXPECT_EQ(source->faces.count(), 2);
  EXPECT_EQ(source->vertices.count(), 6);
}

TEST_F(NurbsMeshEvaluationTest, CollapsedEditingSurfaceRetainsPreviewWithoutDegenerateDisplayFaces)
{
  const auto source = make_planar_sheet();
  ASSERT_NE(source, nullptr);
  double4x4 matrix = double4x4::identity();
  matrix[1][1] = 0;
  const auto collapsed = transform_geometry(*source, matrix, 0);
  ASSERT_TRUE(collapsed.has_value());
  const auto edit = EditData::create(*collapsed->geometry);
  ASSERT_NE(edit, nullptr);
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), preview_options(), &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_FALSE(result->preview.triangles.is_empty());
  EXPECT_EQ(result->mesh->faces_num, 0);
  EXPECT_EQ(result->skipped_degenerate_float_triangles, result->preview.triangles.size());
  EXPECT_TRUE(result->mesh_to_preview_triangle.is_empty());
  EXPECT_GT(result->mesh->edges_num, 0);
  EXPECT_EQ(result->undefined_normal_corners, 0);
  EXPECT_EQ(edit->geometry().faces.count(), 1);
  EXPECT_EQ(edit->geometry().vertices.count(), 4);
}

TEST_F(NurbsMeshEvaluationTest, PyramidDisplayNormalsMatchMeshFacesAfterMovingItsApex)
{
  const auto sheet = make_planar_sheet();
  const int64_t bottom = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  SelectionState initial;
  initial.mode = SelectionMode::Face;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  const auto selected = selection_apply(
      *box->geometry, initial, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(selected.has_value()) << error;
  const auto pyramid = merge_selected_vertices(
      *box->geometry, *selected, VertexMergeMode::Center, {}, 0, &error);
  ASSERT_TRUE(pyramid.has_value()) << error;
  int skipped = 0;
  // Blender's modal transform adapter supplies float matrices, including typed decimal input.
  for (const double offset : {0.0, 0.5, double(0.3f)}) {
    const auto moved = translate_vertices(
        *pyramid->geometry, pyramid->vertices, {offset, 0, 0}, 1e-9, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    const auto before = OwnedGeometry(geometry_copy(moved->geometry.get()));
    const auto edit = EditData::create(*moved->geometry);
    ASSERT_NE(edit, nullptr);
    for (const int resolution : {1, 4, 7, 11, 23, 32}) {
      SCOPED_TRACE(offset);
      SCOPED_TRACE(resolution);
      auto options = preview_options();
      options.boundary.segments_per_span = options.interior_subdivisions = resolution;
      options.boundary.evaluation_allowance = 0;
      const auto result = evaluate_mesh(edit->read_snapshot(), options, &error);
      ASSERT_TRUE(result.has_value()) << error;
      const auto &mesh = *result->mesh;
      EXPECT_EQ(mesh.faces_num, result->mesh_to_preview_triangle.size());
      EXPECT_EQ(mesh.faces_num + result->skipped_degenerate_float_triangles,
                result->preview.triangles.size());
      EXPECT_EQ(result->undefined_normal_corners, 0);
      const auto normals = mesh.corner_normals();
      const auto face_normals = mesh.face_normals();
      for (const int i : result->mesh_to_preview_triangle.index_range()) {
        SCOPED_TRACE(i);
        const int original = result->mesh_to_preview_triangle[i];
        EXPECT_TRUE(i == 0 || original > result->mesh_to_preview_triangle[i - 1]);
        const auto &triangle = result->preview.triangles[original];
        for (const int j : IndexRange(3)) {
          EXPECT_EQ(mesh.corner_verts()[i * 3 + j], triangle.points[j]);
          // This planar body's corner shading must agree with Mesh's geometric face normals.
          EXPECT_GT(math::dot(normals[i * 3 + j], face_normals[i]), 0.9999f);
          EXPECT_NEAR(math::length(normals[i * 3 + j]), 1, 1e-4f);
        }
      }
      skipped += result->skipped_degenerate_float_triangles;
      expect_source_geometry_equal(edit->geometry(), *before);
    }
  }
  EXPECT_GT(skipped, 0) << "The regression must exercise triangles lost in float conversion";
}

TEST_F(NurbsMeshEvaluationTest, ThinRepresentableDisplayFacesAreNotDiscardedByAnAreaEpsilon)
{
  const auto source = make_planar_sheet();
  auto matrix = double4x4::identity();
  matrix[1][1] = 1e-20;
  const auto thin = transform_geometry(*source, matrix, 1e-30);
  ASSERT_TRUE(thin.has_value());
  const auto edit = EditData::create(*thin->geometry);
  ASSERT_NE(edit, nullptr);
  auto options = preview_options();
  options.boundary.segments_per_span = options.interior_subdivisions = 1;
  std::string error;
  const auto result = evaluate_mesh(edit->read_snapshot(), options, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->mesh->faces_num, 2);
  EXPECT_EQ(result->skipped_degenerate_float_triangles, 0);
}

TEST_F(NurbsMeshEvaluationTest,
       DecimalPyramidMovesKeepCertifiedBoundariesWithoutAnEvaluationAllowance)
{
  bool sampled_roundoff_exceeds_tolerance = false;
  const auto sheet = make_planar_sheet();
  const int64_t bottom = static_cast<const NurbsTopologyFace *>(sheet->faces.first)->id;
  std::string error;
  const auto box = extrude_components(*sheet, {{}, {}, {&bottom, 1}}, {0, 0, 2}, 0, &error);
  ASSERT_TRUE(box.has_value()) << error;
  SelectionState initial;
  initial.mode = SelectionMode::Face;
  const ElementRef top{ElementDomain::Face, box->source_to_cap.lookup(bottom)};
  const auto selected = selection_apply(
      *box->geometry, initial, {&top, 1}, SelectionAction::Set, top, &error);
  ASSERT_TRUE(selected.has_value()) << error;
  const auto pyramid = merge_selected_vertices(
      *box->geometry, *selected, VertexMergeMode::Center, {}, 0, &error);
  ASSERT_TRUE(pyramid.has_value()) << error;
  for (const double offset : {0.3, 0.1, 1.0 / 3.0, -0.3, std::nextafter(0.3, 1.0)}) {
    SCOPED_TRACE(offset);
    const auto moved = translate_vertices(
        *pyramid->geometry, pyramid->vertices, {offset, 0, 0}, 1e-9, &error);
    ASSERT_TRUE(moved.has_value()) << error;
    const auto edit = EditData::create(*moved->geometry);
    ASSERT_NE(edit, nullptr);
    const auto before = OwnedGeometry(geometry_copy(moved->geometry.get()));
    const auto index = TopologyIndex::build(edit->geometry());
    ASSERT_TRUE(index.has_value());
    Map<int64_t, double> tolerances;
    for (const auto &face : edit->geometry().faces) {
      for (const auto &loop : face.loops) {
        for (const auto &use : loop.edge_uses) {
          tolerances.add_new(
              use.id,
              std::max({face.tolerance, use.tolerance, index->edge(use.edge_id)->tolerance}));
        }
      }
    }
    for (const int resolution : {1, 7, 11, 23}) {
      SCOPED_TRACE(resolution);
      SurfacePreviewOptions options;
      options.boundary.segments_per_span = options.interior_subdivisions = resolution;
      ASSERT_EQ(options.boundary.evaluation_allowance, 0);
      const auto result = evaluate_mesh(edit->read_snapshot(), options, &error);
      ASSERT_TRUE(result.has_value()) << error;
      for (const auto &use : result->preview.boundary.uses) {
        ASSERT_TRUE(use.curve_support_deviation_bound.has_value());
        const double tolerance = tolerances.lookup(use.use_id);
        EXPECT_LE(*use.curve_support_deviation_bound, tolerance);
        sampled_roundoff_exceeds_tolerance |= use.max_curve_support_deviation > tolerance;
      }
      expect_source_geometry_equal(edit->geometry(), *before);
    }
  }
  EXPECT_TRUE(sampled_roundoff_exceeds_tolerance)
      << "The regression must retain a measured discrepancy exceeding its certified source gap";
}

#ifdef WITH_GMP
TEST_F(NurbsMeshEvaluationTest, PolynomialBoundaryBoundCoversExactRationalWitnesses)
{
  struct Case {
    double origin, size, low, high;
  };
  const Case cases[] = {{0, 1, -0.3, 1.7},
                        {1e12, 1, -0.3, 1.7},
                        {0, 1e-200, -0.3, 1.7},
                        {0, 1e-320, -0.3, 1.7},
                        {0, 1, -1e-300, 1e-300},
                        {0, 1e200, -1e200, 1e200}};
  for (const auto &item : cases) {
    SCOPED_TRACE(item.origin);
    SCOPED_TRACE(item.size);
    SCOPED_TRACE(item.low);
    auto source = make_planar_sheet();
    auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
    for (int i = 0; i < 4; i++) {
      surface.knots_u[i] = surface.knots_v[i] = i < 2 ? item.low : item.high;
      for (int axis = 0; axis < 3; axis++) {
        surface.points[i].position[axis] = item.origin +
                                           item.size * (surface.points[i].position[axis] +
                                                        0.1 * (i + axis));
      }
    }
    surface.points[3].position[2] += item.size * 0.125;
    const double3 uv[] = {
        {std::lerp(item.low, item.high, 0.1), std::lerp(item.low, item.high, 0.2), 0},
        {std::lerp(item.low, item.high, 0.8), std::lerp(item.low, item.high, 0.7), 0}};
    std::string error;
    const auto a = evaluate_surface(surface, double2(uv[0]), &error);
    const auto b = evaluate_surface(surface, double2(uv[1]), &error);
    ASSERT_TRUE(a.has_value()) << error;
    ASSERT_TRUE(b.has_value()) << error;
    const auto *curve = add_linear_curve(*source, a->position, b->position, 3);
    const auto *pcurve = add_linear_curve(*source, uv[0], uv[1], 2);
    const auto bound = linear_curve_support_deviation_bound(*curve, *pcurve, surface);
    ASSERT_TRUE(bound.has_value());
    EXPECT_GT(*bound, 0);
    const mpq_class exact_bound(*bound);
    for (int sample = 0; sample <= 100; sample++) {
      const mpq_class t(sample, 100);
      mpq_class parameters[2];
      for (int axis = 0; axis < 2; axis++) {
        const mpq_class coordinate = (1 - t) * mpq_class(uv[0][axis]) + t * mpq_class(uv[1][axis]);
        parameters[axis] = (coordinate - mpq_class(item.low)) /
                           (mpq_class(item.high) - mpq_class(item.low));
      }
      mpq_class difference_l1(0);
      for (int axis = 0; axis < 3; axis++) {
        mpq_class point(0);
        for (int corner = 0; corner < 4; corner++) {
          const mpq_class u = corner & 1 ? parameters[0] : 1 - parameters[0];
          const mpq_class v = corner & 2 ? parameters[1] : 1 - parameters[1];
          point += u * v * mpq_class(surface.points[corner].position[axis]);
        }
        const mpq_class difference = point -
                                     ((1 - t) * mpq_class(curve->points[0].position[axis]) +
                                      t * mpq_class(curve->points[1].position[axis]));
        difference_l1 += difference < 0 ? -difference : difference;
      }
      EXPECT_TRUE(difference_l1 <= exact_bound) << sample;
    }
  }
}
#endif

TEST_F(NurbsMeshEvaluationTest, InteriorTrimGapIsNotHiddenByThePolynomialCertificate)
{
  auto source = make_planar_sheet();
  auto &face = *static_cast<NurbsTopologyFace *>(source->faces.first);
  for (auto &loop : face.loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&face.loops);
  const double2 corners[] = {{0.125, 0.25}, {0.875, 0.25}, {0.25, 0.75}};
  add_planar_loop(*source, face, {corners, 3}, false);
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
  surface.points[3].position[2] = 0.125;
  const auto before = OwnedGeometry(geometry_copy(source.get()));
  std::string error;
  EXPECT_FALSE(evaluate_boundaries(*source, {7, 1000, 0}, &error));
  EXPECT_NE(error.find("edge/support"), std::string::npos);
  expect_source_geometry_equal(*source, *before);
  const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
  const auto &use = *static_cast<const NurbsTopologyEdgeUse *>(loop.edge_uses.first);
  Map<int64_t, const NurbsCurveGeometry *> curves;
  for (const auto &curve : source->curves) {
    curves.add_new(curve.id, &curve);
  }
  const auto index = TopologyIndex::build(*source);
  const auto &curve = *curves.lookup(index->edge(use.edge_id)->curve_id);
  const auto &pcurve = *curves.lookup(use.pcurve_id);
  const auto bound = linear_curve_support_deviation_bound(curve, pcurve, surface);
  ASSERT_TRUE(bound.has_value());
  EXPECT_GT(*bound, 0);
  // Rational support weights and incompatible curve parameterizations need their own proofs.
  surface.points[3].weight = 2;
  EXPECT_FALSE(linear_curve_support_deviation_bound(curve, pcurve, surface));
  surface.points[3].weight = 1;
  for (auto &item : source->curves) {
    if (item.id == pcurve.id) {
      item.knots[2] = item.knots[3] = 2;
    }
  }
  EXPECT_FALSE(linear_curve_support_deviation_bound(curve, pcurve, surface));
}

TEST_F(NurbsMeshEvaluationTest, UnrepresentableBoundaryCertificateDoesNotClaimAgreement)
{
  auto source = make_planar_sheet();
  auto &surface = *static_cast<NurbsSurfaceGeometry *>(source->surfaces.first);
  for (int i = 0; i < 4; i++) {
    surface.knots_u[i] = surface.knots_v[i] = i < 2 ? -1e308 : 1e308;
  }
  const auto *curve = add_linear_curve(*source, {-1, -1, 0}, {1, 1, 0}, 3);
  const auto *pcurve = add_linear_curve(*source, {-5e307, -5e307, 0}, {5e307, 5e307, 0}, 2);
  EXPECT_FALSE(linear_curve_support_deviation_bound(*curve, *pcurve, surface));
}

TEST_F(NurbsMeshEvaluationTest, PolynomialCertificateRequiresParameterRolesAndContainedTrims)
{
  auto source = make_planar_sheet();
  const auto &surface = *static_cast<const NurbsSurfaceGeometry *>(source->surfaces.first);
  auto *curve = add_linear_curve(*source, {-0.5, -0.5, 0}, {0.5, 0.5, 0}, 3);
  auto *pcurve = add_linear_curve(*source, {0.25, 0.25, 0}, {0.75, 0.75, 0}, 2);
  ASSERT_EQ(linear_curve_support_deviation_bound(*curve, *pcurve, surface), 0.0);
  curve->dimension = 2;
  EXPECT_FALSE(linear_curve_support_deviation_bound(*curve, *pcurve, surface));
  curve->dimension = 3;
  pcurve->dimension = 3;
  EXPECT_FALSE(linear_curve_support_deviation_bound(*curve, *pcurve, surface));
  pcurve->dimension = 2;
  pcurve->points[1].position[0] = 1.25;
  EXPECT_FALSE(linear_curve_support_deviation_bound(*curve, *pcurve, surface));
}

}  // namespace blender::bke::nurbs::tests
