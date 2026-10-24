/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdlib>

#include "DNA_layer_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_memory_utils.hh"

#include "BKE_collection.hh"
#include "BKE_curve.hh"
#include "BKE_geometry_set.hh"
#include "BKE_gtest_base.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_transform.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "BLO_writefile.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_nurbs.hh"

#include "nurbs_test_geometry.hh"

namespace blender::bke::nurbs::tests {

/** Optional, explicit fixture artifact for the Blender-app smoke/visual check. No public factory.
 */
static void write_preview_fixture_if_requested()
{
  const char *path = std::getenv("BLENDER_NURBS_PREVIEW_EXPORT");
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  Main *fixture_main = BKE_main_new();
  BLI_SCOPED_DEFER([&]() { BKE_main_free(fixture_main); });
  auto add_source = [&](const char *name, OwnedGeometry geometry) {
    Curve *curve = BKE_curve_add(fixture_main, name, OB_SURF);
    curve->nurbs_geometry = geometry.release();
    curve->resolu = curve->resolv = 8;
    return curve;
  };
  Curve *sheet = add_source("Native preview sheet", make_planar_sheet());
  ASSERT_NE(sheet->nurbs_geometry, nullptr);
  auto hole = make_planar_sheet();
  ASSERT_NE(hole, nullptr);
  const double2 parameters[] = {{0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.25}};
  add_planar_loop(
      *hole, *static_cast<NurbsTopologyFace *>(hole->faces.first), {parameters, 4}, true);
  add_source("Native preview hole", std::move(hole));
  Curve *circle = add_source("Native preview circle", make_arc_sheet(true));
  ASSERT_NE(circle->nurbs_geometry, nullptr);
  auto sheet_source = make_planar_sheet();
  const auto &source_edge = *static_cast<const NurbsTopologyEdge *>(sheet_source->edges.first);
  const int64_t edge_id = source_edge.id;
  auto line = copy_component_closure(*sheet_source, {{}, {&edge_id, 1}, {}});
  ASSERT_TRUE(line.has_value());
  add_source("Native preview line", std::move(line->geometry));
  auto arc_source = make_arc_sheet(false);
  const int64_t arc_edge = static_cast<const NurbsTopologyEdge *>(arc_source->edges.first)->id;
  auto arc = copy_component_closure(*arc_source, {{}, {&arc_edge, 1}, {}});
  ASSERT_TRUE(arc.has_value());
  auto &trim_edge = *static_cast<NurbsTopologyEdge *>(arc->geometry->edges.first);
  trim_edge.parameter_range[0] = 0.25;
  trim_edge.parameter_range[1] = 0.75;
  const auto &trim_curve = *static_cast<const NurbsCurveGeometry *>(arc->geometry->curves.first);
  int end = 0;
  for (NurbsTopologyVertex &vertex : arc->geometry->vertices) {
    const auto evaluated = evaluate_curve(trim_curve, trim_edge.parameter_range[end++]);
    ASSERT_TRUE(evaluated.has_value());
    std::copy_n(&evaluated->position.x, 3, vertex.position);
  }
  add_source("Native preview trimmed arc", std::move(arc->geometry));
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
  const BlendFileWriteParams params{};
  ASSERT_TRUE(BLO_write_file(fixture_main, path, 0, &params, &reports));
}

class NurbsObjectEvaluationTest : public BlenderGTestBase {
 protected:
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  ViewLayer *view_layer = nullptr;
  Object *object = nullptr;
  Curve *curve = nullptr;
  Depsgraph *graph = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    scene = BKE_scene_add(bmain, "NURBS object evaluation scene");
    view_layer = BKE_view_layer_default_view(scene);
    object = BKE_object_add_only_object(bmain, OB_SURF, "NURBS object evaluation");
    curve = BKE_curve_add(bmain, "NURBS authoritative source", OB_SURF);
    object->data = &curve->id;
    ASSERT_TRUE(BKE_collection_object_add(bmain, scene->master_collection, object));
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    view_layer->basact = BKE_view_layer_base_find(view_layer, object);
    ASSERT_NE(view_layer->basact, nullptr);

    OwnedGeometry wire(MEM_new<NurbsGeometry>(__func__));
    const auto *a = add_vertex(*wire, {-1, -1, 0});
    const auto *b = add_vertex(*wire, {1, -1, 0});
    const auto *line = add_linear_curve(*wire, double3(a->position), double3(b->position), 3);
    const int64_t edge_id = add_edge(*wire, *a, *b, *line)->id;
    curve->nurbs_geometry = wire.release();
    ASSERT_TRUE(curve_edit_begin(*curve));
    object->mode = OB_MODE_EDIT;
    EditData &edit = *curve->editnurb->nurbs_edit;
    auto swept = extrude_edges(edit.geometry(), {edge_id}, {0, 2, 0}, 0);
    ASSERT_TRUE(swept.has_value());
    auto candidate = edit.snapshot();
    candidate.geometry = std::move(swept->geometry);
    ASSERT_TRUE(
        ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), edit.read_snapshot()));
    /* A different face count makes falling back to the persistent wire cache observable. */
    ASSERT_EQ(curve->nurbs_geometry->faces.count(), 0);
    ASSERT_EQ(edit.geometry().faces.count(), 1);
    curve->resolu = curve->resolv = 4;
  }

  void TearDown() override
  {
    if (graph != nullptr) {
      DEG_graph_free(graph);
    }
    BKE_main_free(bmain);
  }

  void build_graph()
  {
    graph = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_VIEWPORT);
    DEG_graph_build_from_view_layer(graph);
    DEG_graph_tag_on_visible_update(graph, false);
    this->refresh();
  }

  void refresh()
  {
    DEG_evaluate_on_refresh(graph);
    DEG_ids_clear_recalc(graph, false);
  }

  Object *evaluated() const
  {
    return DEG_get_evaluated(graph, object);
  }
};

TEST_F(NurbsObjectEvaluationTest, DependencyGraphDisplaysLiveSheetAndRetainsPreviousFrameSource)
{
  this->build_graph();
  Object *evaluated = this->evaluated();
  ASSERT_NE(evaluated, object);
  auto before = evaluated->runtime->nurbs_evaluation;
  ASSERT_NE(before, nullptr);
  ASSERT_TRUE(before->result.has_value()) << before->error;
  EXPECT_EQ(before->result->mesh, nullptr);
  ASSERT_NE(evaluated->runtime->geometry_set_eval, nullptr);
  const Mesh *mesh = evaluated->runtime->geometry_set_eval->get_mesh();
  ASSERT_NE(mesh, nullptr);
  EXPECT_GT(mesh->faces_num, 1);
  EXPECT_EQ(mesh->faces_num, before->result->preview.triangles.size());
  EXPECT_EQ(object->runtime->nurbs_evaluation, nullptr);
  EXPECT_EQ(curve->nurbs_geometry->faces.count(), 0);
  const ReadSnapshot first_state = before->result->source;

  EditData &edit = *curve->editnurb->nurbs_edit;
  auto candidate = edit.snapshot();
  double4x4 matrix = double4x4::identity();
  matrix.location().z = 3;
  auto moved = transform_geometry(*candidate.geometry, matrix, 0);
  ASSERT_TRUE(moved.has_value());
  candidate.geometry = std::move(moved->geometry);
  ASSERT_TRUE(ed::nurbs::edit_commit(*bmain, *curve, std::move(candidate), edit.read_snapshot()));
  this->refresh();
  evaluated = this->evaluated();
  const auto after = evaluated->runtime->nurbs_evaluation;
  ASSERT_NE(after, nullptr);
  ASSERT_TRUE(after->result.has_value()) << after->error;
  EXPECT_EQ(after->result->source, edit.read_snapshot());
  EXPECT_NE(after->result->source, first_state);
  EXPECT_DOUBLE_EQ(after->result->preview.position(0).z, 3);
  EXPECT_DOUBLE_EQ(before->result->preview.position(0).z, 0);
  EXPECT_EQ(first_state.geometry().faces.count(), 1);
  mesh = evaluated->runtime->geometry_set_eval->get_mesh();
  ASSERT_NE(mesh, nullptr);
  EXPECT_FLOAT_EQ(mesh->vert_positions()[0].z, 3);
  write_preview_fixture_if_requested();
}

TEST_F(NurbsObjectEvaluationTest, OriginalObjectMeshExtractionUsesLiveSourceWithoutFlushingIt)
{
  const ReadSnapshot before = curve->editnurb->nurbs_edit->read_snapshot();
  OwnedMesh mesh(BKE_mesh_new_from_object(nullptr, object, false, false, false));
  ASSERT_NE(mesh, nullptr);
  EXPECT_GT(mesh->faces_num, 1);
  EXPECT_EQ(curve->editnurb->nurbs_edit->read_snapshot(), before);
  EXPECT_EQ(curve->nurbs_geometry->faces.count(), 0);
  EXPECT_EQ(object->runtime->nurbs_evaluation, nullptr);
  EXPECT_EQ(curve->nurbs_eval_source, nullptr);
}

TEST_F(NurbsObjectEvaluationTest,
       InvalidOwnerAndUnsupportedModifierDoNotDisplayStaleOrLegacySource)
{
  this->build_graph();
  ASSERT_TRUE(this->evaluated()->runtime->nurbs_evaluation->result.has_value());
  const auto good_frame = this->evaluated()->runtime->nurbs_evaluation;
  Nurb *foreign = MEM_new<Nurb>(__func__);
  BLI_addtail(&curve->editnurb->nurbs, foreign);
  DEG_id_tag_update_ex(bmain, &curve->id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  this->refresh();
  const auto invalid = this->evaluated()->runtime->nurbs_evaluation;
  ASSERT_NE(invalid, nullptr);
  EXPECT_FALSE(invalid->result.has_value());
  EXPECT_NE(invalid->error.find("legacy"), std::string::npos);
  EXPECT_EQ(this->evaluated()->runtime->geometry_set_eval->get_mesh(), nullptr);
  EXPECT_EQ(curve->editnurb->nurbs_edit->geometry().faces.count(), 1);
  EXPECT_EQ(curve->nurbs_geometry->faces.count(), 0);

  BLI_remlink(&curve->editnurb->nurbs, foreign);
  MEM_delete(foreign);
  curve->offset = 1.25f;
  DEG_id_tag_update_ex(bmain, &curve->id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  this->refresh();
  const auto offset = this->evaluated()->runtime->nurbs_evaluation;
  ASSERT_NE(offset, nullptr);
  EXPECT_FALSE(offset->result.has_value());
  EXPECT_NE(offset->error.find("offsets"), std::string::npos);
  EXPECT_EQ(this->evaluated()->runtime->geometry_set_eval->get_mesh(), nullptr);
  curve->offset = 1.0f;
  ModifierData *modifier = BKE_modifier_new(eModifierType_Subsurf);
  BLI_addtail(&object->modifiers, modifier);
  DEG_id_tag_update_ex(bmain, &curve->id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  DEG_id_tag_update_ex(bmain, &object->id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  DEG_graph_tag_relations_update(graph);
  DEG_graph_relations_update(graph);
  this->refresh();
  const auto unsupported = this->evaluated()->runtime->nurbs_evaluation;
  ASSERT_NE(unsupported, nullptr);
  EXPECT_FALSE(unsupported->result.has_value());
  EXPECT_NE(unsupported->error.find("modifiers"), std::string::npos);
  EXPECT_EQ(this->evaluated()->runtime->geometry_set_eval->get_mesh(), nullptr);
  EXPECT_EQ(good_frame->result->source.geometry().faces.count(), 1);
}

TEST_F(NurbsObjectEvaluationTest, RuntimeCopyAndCacheFreeDoNotDuplicateOrInvalidateRetainedSource)
{
  this->build_graph();
  Object *evaluated = this->evaluated();
  const auto retained = evaluated->runtime->nurbs_evaluation;
  ASSERT_NE(retained, nullptr);
  ASSERT_TRUE(retained->result.has_value()) << retained->error;
  Object *copy = id_cast<Object *>(
      BKE_id_copy_ex(nullptr, &evaluated->id, nullptr, LIB_ID_COPY_LOCALIZE));
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->runtime->nurbs_evaluation, nullptr);
  BKE_id_free(nullptr, copy);
  BKE_object_free_derived_caches(evaluated);
  EXPECT_EQ(evaluated->runtime->nurbs_evaluation, nullptr);
  EXPECT_EQ(evaluated->runtime->geometry_set_eval, nullptr);
  EXPECT_EQ(retained->result->source.geometry().faces.count(), 1);
  EXPECT_FALSE(retained->result->preview.triangles.is_empty());
}

}  // namespace blender::bke::nurbs::tests
