/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_report.hh"
#include "BKE_solid_host.hh"
#include "BKE_unit.hh"

#include "DEG_depsgraph.hh"

#include "ED_mesh.hh"
#include "ED_numinput.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_solid.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "curve_intern.hh"

namespace blender {
namespace ed::solid_editor {

bool edit_poll(bContext *C)
{
  const Object *object = CTX_data_edit_object(C);
  return object && object->type == OB_MESH && id_cast<const Mesh *>(object->data)->solid_storage;
}

Session::Session(Object &object, bke::solid_host::Snapshot source)
    : object_(&object), source_(std::move(source)), revision_(source_.revision)
{
}

std::unique_ptr<Session> Session::begin(bContext *C, std::string *error)
{
  if (!edit_poll(C)) {
    if (error)
      *error = "Enter Edit Mode on an analytic solid sketch";
    return {};
  }
  Object &object = *CTX_data_edit_object(C);
  auto source = bke::solid_host::read_edit(*id_cast<Mesh *>(object.data), error);
  if (!source)
    return {};
  return std::unique_ptr<Session>(new Session(object, std::move(*source)));
}

static bool replace_display(bContext *C,
                            Object &object,
                            bke::solid_host::Snapshot candidate,
                            uint64_t &revision,
                            std::string *error)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  candidate.selection = solid::normalize_selection(*candidate.model,
                                                   std::move(candidate.selection));
  auto display = bke::solid_host::display_mesh(candidate, 64, error);
  if (!display || !bke::solid_host::publish(mesh, candidate, revision, error))
    return false;
  ++revision;
  BKE_mesh_nomain_to_mesh(display.release(), &mesh, &object, false);
  CTX_data_scene(C)->toolsettings->selectmode = candidate.selection.mode;
  EDBM_mesh_make(&object, candidate.selection.mode, false);
  EDBMUpdate_Params update{};
  update.calc_looptris = true;
  update.calc_normals = true;
  update.is_destructive = true;
  EDBM_update(&mesh, &update);
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, &mesh);
  return true;
}

bool Session::apply(bContext *C, solid::EditResult candidate, std::string *error)
{
  bke::solid_host::Snapshot next{std::make_shared<const solid::Model>(std::move(candidate.model)),
                                 std::move(candidate.selection),
                                 revision_};
  return replace_display(C, *object_, std::move(next), revision_, error);
}

bool Session::restore(bContext *C, std::string *error)
{
  return replace_display(C, *object_, source_, revision_, error);
}

}  // namespace ed::solid_editor

namespace {

static double3 host_point(const solid::Vec3 &p)
{
  return {p.x, p.y, p.z};
}

static wmOperatorStatus sketch_add_exec(bContext *C, wmOperator *op)
{
  const int type = RNA_enum_get(op->ptr, "type");
  const double size = RNA_float_get(op->ptr, "size");
  solid::Model model;
  solid::Selection selection;
  std::string error;
  if (type == 3) {
    auto c = solid::circle({0, 0, 0}, {size, 0, 0}, {0, 0, 1}, &error);
    if (!c || !solid::append_curve(model, *c, 0, 2 * M_PI, 0, 0, true, &selection, &error)) {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
      return OPERATOR_CANCELLED;
    }
  }
  else if (type) {
    const std::vector<solid::Vec3> points = type == 1 ?
                                                std::vector<solid::Vec3>{{-size, 0, 0},
                                                                         {size, 0, 0}} :
                                                std::vector<solid::Vec3>{{-size, -size, 0},
                                                                         {size, -size, 0},
                                                                         {size, size, 0},
                                                                         {-size, size, 0},
                                                                         {-size, -size, 0}};
    solid::Id previous = 0, first = 0;
    for (size_t i = 1; i < points.size(); ++i) {
      auto c = solid::line(points[i - 1], points[i], &error);
      solid::Selection added;
      if (!c || !solid::append_curve(model,
                                     *c,
                                     0,
                                     1,
                                     previous,
                                     type == 2 && i + 1 == points.size() ? first : 0,
                                     false,
                                     &added,
                                     &error))
      {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
      if (!first)
        first = added.vertices.front();
      previous = added.vertices.back();
      selection.vertices.insert(
          selection.vertices.end(), added.vertices.begin(), added.vertices.end());
    }
  }
  selection = solid::normalize_selection(model, std::move(selection));
  auto *storage = bke::solid_host::storage_create(std::move(model), std::move(selection), &error);
  if (!storage) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  float location[3], rotation[3];
  ushort local_view_bits;
  bool enter_editmode;
  ed::object::add_generic_get_opts(
      C, op, 'Z', location, rotation, nullptr, &enter_editmode, &local_view_bits, nullptr);
  Object *object = ed::object::add_type(
      C, OB_MESH, "SolidSketch", location, rotation, false, local_view_bits);
  Mesh &mesh = *id_cast<Mesh *>(object->data);
  mesh.solid_storage = storage;
  auto display = bke::solid_host::display_mesh(*bke::solid_host::read(mesh), 64, &error);
  if (!display) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  BKE_mesh_nomain_to_mesh(display.release(), &mesh, object, false);
  CTX_data_scene(C)->toolsettings->selectmode = 1;
  if (!ed::object::editmode_enter_ex(CTX_data_main(C), CTX_data_scene(C), object, 0, op->reports))
    return OPERATOR_CANCELLED;
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, object);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
  if (CTX_wm_area(C) && CTX_wm_area(C)->spacetype == SPACE_VIEW3D) {
    WM_toolsystem_ref_set_by_id(C, type <= 1 ? "builtin.nurbs_draw_line" : "builtin.select_box");
  }
  return OPERATOR_FINISHED;
}

static bool solid_edit_poll(bContext *C)
{
  return ed::solid_editor::edit_poll(C);
}

static std::optional<solid::Profile> prepare(const ed::solid_editor::Session &session,
                                             std::string &error)
{
  return solid::prepare_profile(*session.source().model, session.source().selection, &error);
}

static wmOperatorStatus fill_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  auto profile = session ? prepare(*session, error) : std::nullopt;
  if (session && !profile) {
    const auto &source = session->source();
    solid::EditResult connected{*source.model, source.selection};
    std::vector<solid::Id> endpoints;
    if (source.selection.vertices.size() == 2 && source.selection.edges.empty())
      endpoints = source.selection.vertices;
    else if (source.selection.faces.empty()) {
      std::unordered_map<solid::Id, int> degree;
      for (solid::Id id : source.selection.edges)
        for (solid::Id v : solid::edge(*source.model, id)->vertices)
          degree[v]++;
      bool chain = true;
      for (auto [id, count] : degree) {
        if (count == 1)
          endpoints.push_back(id);
        else if (count != 2)
          chain = false;
      }
      if (!chain)
        endpoints.clear();
    }
    if (endpoints.size() == 2) {
      auto line = solid::line(solid::vertex(connected.model, endpoints[0])->position,
                              solid::vertex(connected.model, endpoints[1])->position,
                              &error);
      if (line && solid::append_curve(connected.model,
                                      *line,
                                      0,
                                      1,
                                      endpoints[0],
                                      endpoints[1],
                                      false,
                                      &connected.selection,
                                      &error))
      {
        if (source.selection.vertices.size() == 2) {
          if (!session->apply(C, std::move(connected), &error)) {
            BKE_report(op->reports, RPT_ERROR, error.c_str());
            return OPERATOR_CANCELLED;
          }
          return OPERATOR_FINISHED;
        }
        error.clear();
        profile = solid::prepare_profile(connected.model, connected.selection, &error);
      }
    }
  }
  solid::Selection selection;
  selection.mode = 4;
  if (profile)
    selection.faces.push_back(profile->face);
  if (!profile || !session->apply(C, {std::move(profile->model), std::move(selection)}, &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

static double distance_get(wmOperator *op)
{
  const std::string exact = RNA_string_get(op->ptr, "distance_exact");
  if (exact.empty()) {
    return RNA_float_get(op->ptr, "distance");
  }
  char *end;
  const double value = std::strtod(exact.c_str(), &end);
  if (end != exact.c_str() + exact.size()) {
    return NAN;
  }
  const float displayed = RNA_float_get(op->ptr, "distance");
  /* Keep double precision on replay, but honor a subsequent edit in the Adjust panel. */
  return float(value) == displayed ? value : double(displayed);
}

static wmOperatorStatus extrude_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto preview = ed::solid_editor::Session::begin(C, &error);
  auto profile = preview ? prepare(*preview, error) : std::nullopt;
  std::optional<solid::EditResult> result;
  float offset[3];
  RNA_float_get_array(op->ptr, "offset", offset);
  const bool vector_offset = !math::is_zero(float3(offset));
  if (preview && (vector_offset || !profile)) {
    error.clear();
    result = solid::extrude_elements(*preview->source().model,
                                     preview->source().selection,
                                     vector_offset ? solid::Vec3{offset[0], offset[1], offset[2]} :
                                                     solid::Vec3{0, 0, distance_get(op)},
                                     &error);
  }
  else if (profile)
    result = solid::extrude(*profile, distance_get(op), &error);
  if (!result || !preview->apply(C, std::move(*result), &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

struct ExtrudeModal {
  MEM_CXX_CLASS_ALLOC_FUNCS("Solid extrusion");
  std::unique_ptr<ed::solid_editor::Session> preview;
  solid::Profile profile;
  bool elements = false;
  bool bevel = false;
  bool vertices_only = false;
  int axis = -1;
  solid::Vec3 offset;
  float depth = 1;
  NumInput num{};
  float2 initial_mouse;
  float2 direction = {0, 1};
  double units_per_pixel = 0;
  double distance = 0;
  bool valid = false;
  std::string error;
};

static void cleanup(bContext *C, wmOperator *op)
{
  MEM_delete(static_cast<ExtrudeModal *>(op->customdata));
  op->customdata = nullptr;
  ED_workspace_status_text(C, nullptr);
  ED_region_tag_redraw(CTX_wm_region(C));
}

static void extrude_cancel(bContext *C, wmOperator *op)
{
  auto *data = static_cast<ExtrudeModal *>(op->customdata);
  if (data) {
    std::string error;
    if (!data->preview->restore(C, &error)) {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
    }
    cleanup(C, op);
  }
}

static void update(bContext *C, wmOperator *op)
{
  auto &data = *static_cast<ExtrudeModal *>(op->customdata);
  data.error.clear();
  const auto &source = data.preview->source();
  auto result = data.bevel    ? solid::chamfer(*source.model,
                                            source.selection,
                                            std::abs(data.distance),
                                            data.vertices_only,
                                            &data.error) :
                data.elements ? solid::extrude_elements(
                                    *source.model, source.selection, data.offset, &data.error) :
                                solid::extrude(data.profile, data.distance, &data.error);
  data.valid = result && data.preview->apply(C, std::move(*result), &data.error);
  if (!data.valid) {
    /* Do not leave a stale, apparently valid extrusion visible at a rejected distance. */
    std::string restore_error;
    if (!data.preview->restore(C, &restore_error)) {
      data.error = restore_error;
    }
  }
  char status[512];
  if (data.valid) {
    BLI_snprintf(status,
                 sizeof(status),
                 "%s: %.9g | X/Y/Z: Axis | Ctrl: Snap | "
                 "LMB/Enter: Confirm | RMB/Esc: Cancel",
                 data.bevel ? "Bevel Solid (1 segment)" : "Extrude Solid",
                 data.distance);
  }
  else {
    BLI_snprintf(status,
                 sizeof(status),
                 "Extrude Solid: %s | Move mouse or type distance | Esc: Cancel",
                 data.error.c_str());
  }
  ED_workspace_status_text(C, status);
  ED_region_tag_redraw(CTX_wm_region(C));
}

static wmOperatorStatus extrude_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!CTX_wm_region_view3d(C)) {
    return extrude_exec(C, op);
  }
  std::string error;
  auto preview = ed::solid_editor::Session::begin(C, &error);
  auto profile = preview ? prepare(*preview, error) : std::nullopt;
  if (!preview || (!profile && preview->source().selection.vertices.empty())) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  auto *data = MEM_new<ExtrudeModal>(__func__);
  data->preview = std::move(preview);
  data->elements = !profile;
  if (profile)
    data->profile = std::move(*profile);
  data->initial_mouse = float2(event->mval);
  const double4x4 transform(data->preview->object().object_to_world());
  const auto &source = data->preview->source();
  const auto *vertex = solid::vertex(*source.model, source.selection.vertices.front());
  const float3 center(math::transform_point(transform, host_point(vertex->position)));
  const solid::Vec3 normal = data->elements ? solid::Vec3{0, 0, 1} : data->profile.normal;
  const float3 direction(math::transform_direction(transform, host_point(normal)));
  data->depth = ED_view3d_calc_zfac(CTX_wm_region_view3d(C), center);
  const double scale = math::length(direction);
  data->units_per_pixel = ED_view3d_pixel_size(CTX_wm_region_view3d(C), center) / scale;
  float2 a, b;
  const float3 end = center + direction;
  if (ED_view3d_project_float_global(CTX_wm_region(C), center, a, V3D_PROJ_TEST_NOP) ==
          V3D_PROJ_RET_OK &&
      ED_view3d_project_float_global(CTX_wm_region(C), end, b, V3D_PROJ_TEST_NOP) ==
          V3D_PROJ_RET_OK &&
      math::distance(a, b) > 4)
  {
    data->direction = math::normalize(b - a);
    data->units_per_pixel = 1.0 / math::distance(a, b);
  }
  if (!std::isfinite(data->units_per_pixel) || data->units_per_pixel <= 0) {
    MEM_delete(data);
    BKE_report(
        op->reports, RPT_ERROR, "Extrude Solid requires a usable object and viewport scale");
    return OPERATOR_CANCELLED;
  }
  initNumInput(&data->num);
  data->num.flag |= NUM_USE_DOUBLE;
  data->num.idx_max = 0;
  data->num.unit_sys = CTX_data_scene(C)->unit.system;
  data->num.unit_type[0] = B_UNIT_LENGTH;
  data->num.val_inc[0] = 0.1f;
  op->customdata = data;
  ED_workspace_status_text(
      C, "Extrude Solid: Move mouse or type distance | LMB/Enter: Confirm | Esc: Cancel");
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus extrude_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &data = *static_cast<ExtrudeModal *>(op->customdata);
  if (event->val == KM_PRESS && ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE)) {
    extrude_cancel(C, op);
    return OPERATOR_CANCELLED;
  }
  if (event->val == KM_PRESS && ELEM(event->type, LEFTMOUSE, EVT_RETKEY, EVT_PADENTER)) {
    if (!data.valid) {
      return OPERATOR_RUNNING_MODAL;
    }
    if (data.bevel)
      RNA_float_set(op->ptr, "offset", std::abs(data.distance));
    else {
      RNA_float_set(op->ptr, "distance", float(data.distance));
      char exact[64];
      BLI_snprintf(exact, sizeof(exact), "%.17g", data.distance);
      RNA_string_set(op->ptr, "distance_exact", exact);
      if (data.elements) {
        const float offset[3] = {float(data.offset.x), float(data.offset.y), float(data.offset.z)};
        RNA_float_set_array(op->ptr, "offset", offset);
      }
    }
    cleanup(C, op);
    return OPERATOR_FINISHED;
  }
  if (data.elements && event->val == KM_PRESS && ELEM(event->type, EVT_XKEY, EVT_YKEY, EVT_ZKEY)) {
    data.axis = event->type == EVT_XKEY ? 0 : event->type == EVT_YKEY ? 1 : 2;
    const auto &source = data.preview->source();
    const auto p = solid::vertex(*source.model, source.selection.vertices.front())->position;
    const float3 center = math::transform_point(data.preview->object().object_to_world(),
                                                float3(p.x, p.y, p.z));
    float3 axis(0);
    axis[data.axis] = 1;
    const float3 world_axis = math::transform_direction(data.preview->object().object_to_world(),
                                                        axis);
    float2 a, b;
    data.direction = {0, 1};
    data.units_per_pixel = ED_view3d_pixel_size(CTX_wm_region_view3d(C), center) /
                           math::length(world_axis);
    if (ED_view3d_project_float_global(CTX_wm_region(C), center, a, V3D_PROJ_TEST_NOP) ==
            V3D_PROJ_RET_OK &&
        ED_view3d_project_float_global(
            CTX_wm_region(C), center + world_axis, b, V3D_PROJ_TEST_NOP) == V3D_PROJ_RET_OK &&
        math::distance(a, b) > 4)
    {
      data.direction = math::normalize(b - a);
      data.units_per_pixel = 1.0 / math::distance(a, b);
    }
    data.offset = {};
    data.offset[data.axis] = data.distance;
    update(C, op);
  }
  else if (event->val == KM_PRESS && handleNumInput(C, &data.num, event)) {
    applyNumInput(&data.num, &data.distance);
    if (data.elements) {
      data.offset = {};
      data.offset[data.axis < 0 ? 2 : data.axis] = data.distance;
    }
    update(C, op);
  }
  else if (event->type == MOUSEMOVE && !hasNumInput(&data.num)) {
    data.distance = math::dot(float2(event->mval) - data.initial_mouse, data.direction) *
                    data.units_per_pixel;
    if (event->modifier & KM_CTRL) {
      data.distance = std::round(data.distance * 10) / 10;
    }
    if (data.elements) {
      data.offset = {};
      if (data.axis >= 0)
        data.offset[data.axis] = data.distance;
      else {
        float3 delta;
        ED_view3d_win_to_delta(
            CTX_wm_region(C), float2(event->mval) - data.initial_mouse, data.depth, delta);
        const auto local = math::transform_direction(data.preview->object().world_to_object(),
                                                     delta);
        data.offset = {local.x, local.y, local.z};
        if (event->modifier & KM_CTRL)
          for (int i = 0; i < 3; ++i)
            data.offset[i] = std::round(data.offset[i] * 10) / 10;
        data.distance = solid::length(data.offset);
      }
    }
    update(C, op);
  }
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus merge_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  if (!session)
    return OPERATOR_CANCELLED;
  const auto local = math::transform_point(session->object().world_to_object(),
                                           float3(CTX_data_scene(C)->cursor.location));
  auto result = solid::merge_vertices(*session->source().model,
                                      session->source().selection,
                                      solid::MergeMode(RNA_enum_get(op->ptr, "type")),
                                      {local.x, local.y, local.z},
                                      RNA_float_get(op->ptr, "threshold"),
                                      &error);
  if (!result || !session->apply(C, std::move(*result), &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}
static wmOperatorStatus bevel_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  auto result = session ? solid::chamfer(*session->source().model,
                                         session->source().selection,
                                         RNA_float_get(op->ptr, "offset"),
                                         RNA_boolean_get(op->ptr, "vertices_only"),
                                         &error) :
                          std::nullopt;
  if (!result || !session->apply(C, std::move(*result), &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}
static wmOperatorStatus bevel_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!CTX_wm_region_view3d(C))
    return bevel_exec(C, op);
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  const bool vertices_only = RNA_boolean_get(op->ptr, "vertices_only");
  auto check = session ? solid::chamfer(*session->source().model,
                                        session->source().selection,
                                        RNA_float_get(op->ptr, "offset"),
                                        vertices_only,
                                        &error) :
                         std::nullopt;
  if (!check) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  auto *data = MEM_new<ExtrudeModal>(__func__);
  data->preview = std::move(session);
  data->bevel = true;
  data->vertices_only = vertices_only;
  data->initial_mouse = float2(event->mval);
  const auto &source = data->preview->source();
  const auto p = solid::vertex(*source.model, source.selection.vertices.front())->position;
  const float3 center = math::transform_point(data->preview->object().object_to_world(),
                                              float3(p.x, p.y, p.z));
  data->units_per_pixel = ED_view3d_pixel_size(CTX_wm_region_view3d(C), center);
  data->direction = {1, 0};
  initNumInput(&data->num);
  data->num.flag |= NUM_USE_DOUBLE;
  data->num.idx_max = 0;
  data->num.unit_sys = CTX_data_scene(C)->unit.system;
  data->num.unit_type[0] = B_UNIT_LENGTH;
  op->customdata = data;
  ED_workspace_status_text(
      C, "Bevel Solid (1 segment): Move mouse or type width | Enter: Confirm | Esc: Cancel");
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}
static wmOperatorStatus delete_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  auto result = session ? solid::delete_elements(*session->source().model,
                                                 session->source().selection,
                                                 solid::DeleteMode(RNA_enum_get(op->ptr, "type")),
                                                 &error) :
                          std::nullopt;
  if (!result || !session->apply(C, std::move(*result), &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

}  // namespace

void SOLID_OT_sketch_add(wmOperatorType *ot)
{
  ot->name = "Add Solid Sketch";
  ot->idname = "SOLID_OT_sketch_add";
  ot->description = "Start an exact sketch; draw a closed profile and press E to extrude a solid";
  ot->exec = sketch_add_exec;
  ot->poll = ED_operator_objectmode;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  static const EnumPropertyItem types[] = {
      {0, "EMPTY", 0, "Empty Sketch", "Start drawing an exact closed profile"},
      {1, "LINE", 0, "Line", "Start with an exact straight segment"},
      {2, "RECTANGLE", 0, "Rectangle", "Start with a closed rectangular profile"},
      {3, "CIRCLE", 0, "Circle", "Start with an exact circular profile"},
      {0, nullptr, 0, nullptr, nullptr}};
  RNA_def_enum(ot->srna, "type", types, 0, "Profile", "Initial sketch geometry");
  RNA_def_float_distance(ot->srna,
                         "size",
                         1,
                         1e-6f,
                         FLT_MAX,
                         "Size",
                         "Half-width of the line or rectangle, or radius of the circle",
                         0.01f,
                         100);
  ed::object::add_generic_props(ot, false);
}

void SOLID_OT_fill(wmOperatorType *ot)
{
  ot->name = "Fill Solid Sketch";
  ot->idname = "SOLID_OT_fill";
  ot->description = "Fill one closed planar sketch with an exact plane and its original curves";
  ot->exec = fill_exec;
  ot->poll = solid_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void SOLID_OT_extrude(wmOperatorType *ot)
{
  ot->name = "Extrude Solid";
  ot->idname = "SOLID_OT_extrude";
  ot->description =
      "Extrude vertices into edges, edges into surfaces, or a closed profile into a solid";
  ot->poll = solid_edit_poll;
  ot->exec = extrude_exec;
  ot->invoke = extrude_invoke;
  ot->modal = extrude_modal;
  ot->cancel = extrude_cancel;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
  RNA_def_float_distance(ot->srna,
                         "distance",
                         1,
                         -FLT_MAX,
                         FLT_MAX,
                         "Distance",
                         "Extrusion distance along the profile normal in object-local units",
                         -100,
                         100);
  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "distance_exact",
                                     nullptr,
                                     0,
                                     "Exact Distance",
                                     "Double precision modal distance for replay");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_float_vector(ot->srna,
                       "offset",
                       3,
                       nullptr,
                       -FLT_MAX,
                       FLT_MAX,
                       "Offset",
                       "Translation for vertex or edge extrusion in object-local coordinates",
                       -100,
                       100);
}

void SOLID_OT_merge(wmOperatorType *ot)
{
  ot->name = "Merge Solid Vertices";
  ot->idname = "SOLID_OT_merge";
  ot->description = "Merge or weld analytic source vertices and rebuild their straight boundaries";
  ot->poll = solid_edit_poll;
  ot->exec = merge_exec;
  ot->invoke = WM_menu_invoke;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  static const EnumPropertyItem types[] = {
      {0, "CENTER", 0, "At Center", "Merge at the average position"},
      {1, "FIRST", 0, "At First", "Merge at the first selected vertex"},
      {2, "LAST", 0, "At Last", "Merge at the active selected vertex"},
      {3, "CURSOR", 0, "At Cursor", "Merge at the 3D cursor"},
      {4, "DISTANCE", 0, "By Distance", "Weld selected vertices within the threshold"},
      {5, "COLLAPSE", 0, "Collapse", "Collapse each selected edge component"},
      {0, nullptr, 0, nullptr, nullptr}};
  ot->prop = RNA_def_enum(ot->srna, "type", types, 0, "Type", "Merge method");
  RNA_def_float_distance(
      ot->srna, "threshold", 1e-4f, 0, FLT_MAX, "Distance", "Weld threshold", 0, 1);
}
void SOLID_OT_bevel(wmOperatorType *ot)
{
  ot->name = "Bevel Solid";
  ot->idname = "SOLID_OT_bevel";
  ot->description = "Create exact one-segment bevels on convex planar solid edges or vertices";
  ot->poll = solid_edit_poll;
  ot->exec = bevel_exec;
  ot->invoke = bevel_invoke;
  ot->modal = extrude_modal;
  ot->cancel = extrude_cancel;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
  RNA_def_float_distance(ot->srna,
                         "offset",
                         0.1f,
                         1e-8f,
                         FLT_MAX,
                         "Width",
                         "Distance along adjoining faces",
                         .001f,
                         1);
  RNA_def_boolean(ot->srna,
                  "vertices_only",
                  false,
                  "Vertices Only",
                  "Bevel selected vertices instead of edges");
}
void SOLID_OT_delete(wmOperatorType *ot)
{
  ot->name = "Delete Solid Elements";
  ot->idname = "SOLID_OT_delete";
  ot->description = "Delete source vertices, edges or faces";
  ot->poll = solid_edit_poll;
  ot->exec = delete_exec;
  ot->invoke = WM_menu_invoke;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  static const EnumPropertyItem types[] = {
      {0, "VERT", 0, "Vertices", "Delete selected vertices and attached edges and faces"},
      {1, "EDGE", 0, "Edges", "Delete selected edges and attached faces"},
      {2, "FACE", 0, "Faces", "Delete selected faces and unused boundary elements"},
      {3, "ONLY_FACE", 0, "Only Faces", "Delete selected faces and retain their edges"},
      {0, nullptr, 0, nullptr, nullptr}};
  ot->prop = RNA_def_enum(ot->srna, "type", types, 0, "Type", "Elements to delete");
}

}  // namespace blender
