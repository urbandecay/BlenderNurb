/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "extrusion.hh"
#include "id_index.hh"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BLI_bit_vector.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_editmesh_cache.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_solid_host.hh"
#include "BKE_unit.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_mesh.hh"
#include "ED_numinput.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_solid.hh"
#include "ED_transform.hh"
#include "ED_util.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "curve_intern.hh"

namespace blender {
namespace ed::solid_editor {

constexpr float solid_bevel_mouse_pixel_margin = 5.0f;

bool edit_poll(bContext *C)
{
  const Object *object = CTX_data_edit_object(C);
  return object && object->type == OB_MESH && id_cast<const Mesh *>(object->data)->solid_storage;
}

/* A partial rigid drag is safe only when no edge, face or analytic support
 * straddles the selected and stationary portions of the source. */
static bool isolated_selection(const bke::solid_host::Snapshot &source)
{
  const auto &m = *source.model;
  const std::unordered_set<solid::Id> vertices(source.selection.vertices.begin(),
                                               source.selection.vertices.end());
  if (vertices.empty())
    return false;
  std::unordered_map<solid::Id, bool> edges, curves, surfaces;
  auto assign = [](auto &map, solid::Id id, bool moving) {
    const auto [it, inserted] = map.emplace(id, moving);
    return inserted || it->second == moving;
  };
  for (const auto &e : m.edges) {
    const bool moving = vertices.contains(e.vertices[0]);
    if (moving != vertices.contains(e.vertices[1]) || !assign(curves, e.curve, moving))
      return false;
    edges[e.id] = moving;
  }
  for (const auto &f : m.faces) {
    if (f.outer.empty())
      return false;
    const bool moving = edges.at(f.outer.front().edge);
    auto same = [&](const auto &loop) {
      return std::all_of(
          loop.begin(), loop.end(), [&](const auto &use) { return edges.at(use.edge) == moving; });
    };
    if (!same(f.outer) || !std::all_of(f.holes.begin(), f.holes.end(), same) ||
        !assign(surfaces, f.surface, moving))
      return false;
  }
  for (const auto &surface : m.surfaces) {
    if (!surfaces.contains(surface.id))
      continue;
    for (solid::Id curve : {surface.curve, surface.other_curve})
      if (curve && (!curves.contains(curve) || curves.at(curve) != surfaces.at(surface.id)))
        return false;
  }
  return true;
}

Session::Session(Object &object, bke::solid_host::Snapshot source)
    : object_(&object), source_(std::move(source)), revision_(source_.revision)
{
  rigid_selection_ = source_.selection.vertices.size() == source_.model->vertices.size() ||
                     isolated_selection(source_);
  if (!rigid_selection_ && source_.selection.mode == 4) {
    const auto &model = *source_.model;
    const solid::IdIndex<solid::Vertex> vertices(model.vertices);
    const solid::IdIndex<solid::Edge> edges(model.edges);
    const solid::IdIndex<solid::Curve> curves(model.curves);
    const solid::IdIndex<solid::Surface> surfaces(model.surfaces);
    const std::unordered_set<solid::Id> selected(source_.selection.vertices.begin(),
                                                source_.selection.vertices.end());
    std::unordered_map<solid::Id, int> curve_uses;
    for (const auto &edge : model.edges)
      ++curve_uses[edge.curve];
    for (const auto &edge : model.edges)
      if ((selected.contains(edge.vertices[0]) || selected.contains(edge.vertices[1])) &&
          (curves.at(edge.curve)->kind != solid::CurveKind::Line ||
           curve_uses.at(edge.curve) != 1))
        return;
    for (const auto &face : model.faces) {
      bool affected = false;
      for (const auto &use : face.outer)
        for (auto id : edges.at(use.edge)->vertices)
          affected |= selected.contains(id);
      for (const auto &hole : face.holes)
        for (const auto &use : hole)
          for (auto id : edges.at(use.edge)->vertices)
            affected |= selected.contains(id);
      if (!affected)
        continue;
      if (!face.holes.empty() || face.outer.size() < 3 || face.outer.size() > 4 ||
          surfaces.at(face.surface)->kind != solid::SurfaceKind::Plane) {
        planar_faces_.clear();
        return;
      }
      std::vector<PlanarPoint> points;
      for (const auto &use : face.outer) {
        const auto &edge = *edges.at(use.edge);
        if (curves.at(edge.curve)->kind != solid::CurveKind::Line) {
          planar_faces_.clear();
          return;
        }
        const auto id = edge.vertices[use.reversed ? 1 : 0];
        points.push_back({vertices.at(id)->position, selected.contains(id)});
      }
      planar_faces_.push_back(std::move(points));
    }
  }
}

Session::~Session()
{
  if (partial_cache_)
    BM_mesh_partial_destroy(partial_cache_);
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

/* Keep mesh transform's existing edit topology when the independently evaluated source
 * produces the same connectivity. Coordinates and analytic normals still come from the
 * current source, so repeated transforms never accumulate display-coordinate drift. */
static bool update_display_vertices(Mesh &mesh,
                                   const solid::DisplayMesh &display,
                                   bool apply,
                                   bool topology_validated = false)
{
  if (!mesh.runtime->edit_mesh)
    return false;
  BMesh *bm = mesh.runtime->edit_mesh->bm;
  if (bm->totvert != display.positions.size() || bm->totface != display.polygons.size())
    return false;
  const int normal_offset = CustomData_get_offset_named(
      &bm->ldata, CD_PROP_FLOAT3, "custom_normal");

  BM_mesh_elem_table_ensure(bm, BM_VERT | BM_FACE);
  BM_mesh_elem_index_ensure(bm, BM_VERT);
  if (!topology_validated) {
    for (const auto &p : display.positions) {
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
          std::abs(p.x) > FLT_MAX || std::abs(p.y) > FLT_MAX || std::abs(p.z) > FLT_MAX)
        return false;
    }
    for (size_t i = 0; i < display.polygons.size(); ++i) {
      BMFace *f = BM_face_at_index(bm, int(i));
      const auto &polygon = display.polygons[i];
      if (f->len != polygon.size() ||
          (normal_offset >= 0 && display.corner_normals[i].size() != polygon.size()))
        return false;
      BMLoop *loop = BM_FACE_FIRST_LOOP(f);
      for (int v : polygon) {
        if (BM_elem_index_get(loop->v) != v)
          return false;
        loop = loop->next;
      }
    }
  }
  if (!apply)
    return true;
  for (size_t i = 0; i < display.positions.size(); ++i) {
    const auto &p = display.positions[i];
    copy_v3_v3(BM_vert_at_index(bm, int(i))->co, float3(p.x, p.y, p.z));
  }
  for (size_t i = 0; normal_offset >= 0 && i < display.polygons.size(); ++i) {
    BMFace *face = BM_face_at_index(bm, int(i));
    float flat_normal[3];
    const bool flat = !BM_elem_flag_test(face, BM_ELEM_SMOOTH);
    if (flat)
      BM_face_calc_normal(face, flat_normal);
    BMLoop *loop = BM_FACE_FIRST_LOOP(face);
    for (const auto &n : display.corner_normals[i]) {
      copy_v3_v3(BM_ELEM_CD_GET_FLOAT_P(loop, normal_offset),
                 flat ? float3(flat_normal) : float3(n.x, n.y, n.z));
      loop = loop->next;
    }
  }
  return true;
}

static bool replace_display(bContext *C,
                            Object &object,
                            bke::solid_host::Snapshot candidate,
                            uint64_t &revision,
                            std::string *error,
                            bool reuse_topology = false,
                            BMPartialUpdate **partial_cache = nullptr,
                            std::vector<bool> *partial_mask = nullptr)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  candidate.selection = solid::normalize_selection(*candidate.model,
                                                   std::move(candidate.selection));
  const auto profile_start = std::chrono::steady_clock::now();
  if (!candidate.validation || !candidate.validation->matches(candidate.model))
    candidate.validation = solid::ValidatedModel::create(candidate.model, error);
  if (!candidate.validation)
    return false;
  const bool analytic = bke::solid_host::display_uses_analytic_normals(mesh);
  if (!candidate.display_samples || candidate.display_samples->has_analytic_normals != analytic) {
    auto samples = solid::tessellate(*candidate.validation, 64, error, analytic);
    if (!samples)
      return false;
    candidate.display_samples = std::make_shared<const solid::DisplayMesh>(std::move(*samples));
  }
  const bool reused = reuse_topology &&
                      update_display_vertices(mesh, *candidate.display_samples, false);
  auto display = reused ? bke::solid_host::OwnedMesh{} :
                          bke::solid_host::display_mesh(candidate, 64, error, analytic);
  const auto profile_display = std::chrono::steady_clock::now();
  if ((!reused && !display) || !bke::solid_host::publish(mesh, candidate, revision, error))
    return false;
  ++revision;
  const auto profile_publish = std::chrono::steady_clock::now();
  if (partial_cache && reused) {
    BMesh &bm = *mesh.runtime->edit_mesh->bm;
    BitVector<> mask(bm.totvert, false);
    std::vector<bool> changed(bm.totvert, false);
    int count = 0;
    for (int i = 0; i < bm.totvert; ++i) {
      const auto &p = candidate.display_samples->positions[i];
      const float3 next(p.x, p.y, p.z);
      if (next != float3(BM_vert_at_index(&bm, i)->co)) {
        mask[i].set();
        changed[i] = true;
        ++count;
      }
    }
    if (!*partial_cache || changed != *partial_mask) {
      if (*partial_cache)
        BM_mesh_partial_destroy(*partial_cache);
      BMPartialUpdate_Params params{};
      params.do_normals = true;
      params.do_tessellate = true;
      *partial_cache = BM_mesh_partial_create_from_verts(bm, params, mask, count);
      *partial_mask = std::move(changed);
    }
  }
  else if (partial_cache && *partial_cache) {
    BM_mesh_partial_destroy(*partial_cache);
    *partial_cache = nullptr;
    partial_mask->clear();
  }
  if (reused)
    update_display_vertices(mesh, *candidate.display_samples, true, true);
  if (!reused)
    BKE_mesh_nomain_to_mesh(display.release(), &mesh, &object, false);
  CTX_data_scene(C)->toolsettings->selectmode = candidate.selection.mode;
  if (!reused)
    EDBM_mesh_make(&object, candidate.selection.mode, false);
  const auto profile_bmesh = std::chrono::steady_clock::now();
  EDBMUpdate_Params update{};
  const bool partial = partial_cache && *partial_cache && reused;
  if (partial)
    BKE_editmesh_looptris_and_normals_calc_with_partial(mesh.runtime->edit_mesh.get(),
                                                        *partial_cache);
  update.calc_looptris = !partial;
  update.calc_normals = !partial;
  update.is_destructive = !reused;
  EDBM_update(&mesh, &update);
  if (std::getenv("BLENDER_SOLID_PROFILE")) {
    const auto now = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    fprintf(stderr,
            "SOLID_DISPLAY_PROFILE display_ms=%.3f publish_ms=%.3f rebuild_ms=%.3f update_ms=%.3f "
            "reused_topology=%d\n",
            ms(profile_start, profile_display),
            ms(profile_display, profile_publish),
            ms(profile_publish, profile_bmesh),
            ms(profile_bmesh, now),
            int(reused));
  }
  if (std::getenv("BLENDER_BEVEL_PROFILE")) {
    const solid::Model &model = *candidate.model;
    const solid::DisplayMesh &samples = *candidate.display_samples;
    const BMesh &bm = *mesh.runtime->edit_mesh->bm;
    fprintf(stderr,
            "SOLID_DISPLAY_COUNTS object=%s source_v=%zu source_e=%zu source_f=%zu "
            "sample_v=%zu sample_boundary_e=%zu sample_f=%zu mesh_v=%d mesh_e=%d mesh_f=%d "
            "edit_v=%d edit_e=%d edit_f=%d\n",
            object.id.name + 2,
            model.vertices.size(),
            model.edges.size(),
            model.faces.size(),
            samples.positions.size(),
            samples.edges.size(),
            samples.polygons.size(),
            mesh.verts_num,
            mesh.edges_num,
            mesh.faces_num,
            bm.totvert,
            bm.totedge,
            bm.totface);
  }
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, &mesh);
  return true;
}

bool Session::apply(bContext *C,
                    solid::EditResult candidate,
                    std::string *error,
                    bool reuse_topology,
                    const solid::Affine *affine,
                    std::shared_ptr<const solid::DisplayMesh> samples)
{
  bke::solid_host::Snapshot next{std::make_shared<const solid::Model>(std::move(candidate.model)),
                                 std::move(candidate.selection),
                                 revision_};
  next.display_samples = std::move(samples);
  if (reuse_topology && affine) {
    const auto &m = *affine;
    const auto &a = m.columns[0], &b = m.columns[1], &c = m.columns[2];
    const double det = solid::dot(a, solid::cross(b, c));
    const double orthogonal_error = std::abs(solid::dot(a, a) - 1) +
                                    std::abs(solid::dot(b, b) - 1) +
                                    std::abs(solid::dot(c, c) - 1) + std::abs(solid::dot(a, b)) +
                                    std::abs(solid::dot(a, c)) + std::abs(solid::dot(b, c));
    if (det > 0 && orthogonal_error < 1e-6) {
      /* Loaded files have no runtime samples. Keep the immutable starting
       * samples in this modal session too, otherwise every mouse movement
       * tessellates the entire source again. */
      const bool analytic = bke::solid_host::display_uses_analytic_normals(
          *id_cast<Mesh *>(object_->data));
      if (!source_.display_samples || source_.display_samples->has_analytic_normals != analytic) {
        auto samples = solid::tessellate(*source_.model, 64, error, analytic);
        if (!samples)
          return false;
        source_.display_samples = std::make_shared<const solid::DisplayMesh>(std::move(*samples));
      }
      auto samples = std::make_shared<solid::DisplayMesh>(*source_.display_samples);
      for (solid::Vec3 &p : samples->positions)
        p = a * p.x + b * p.y + c * p.z + m.translation;
      const solid::Vec3 na = solid::cross(b, c) / det, nb = solid::cross(c, a) / det,
                        nc = solid::cross(a, b) / det;
      for (auto &polygon : samples->corner_normals)
        for (solid::Vec3 &n : polygon) {
          n = na * n.x + nb * n.y + nc * n.z;
          n = n / solid::length(n);
        }
      next.display_samples = std::move(samples);
    }
  }
  const bool partial = affine && !reuse_topology;
  if (!partial && partial_cache_) {
    BM_mesh_partial_destroy(partial_cache_);
    partial_cache_ = nullptr;
    partial_mask_.clear();
  }
  return replace_display(C,
                         *object_,
                         std::move(next),
                         revision_,
                         error,
                         reuse_topology || partial,
                         partial ? &partial_cache_ : nullptr,
                         partial ? &partial_mask_ : nullptr);
}

bool Session::apply(bContext *C,
                    bke::solid_host::Snapshot candidate,
                    std::string *error,
                    bool reuse_topology)
{
  const bool partial_reuse = reuse_topology;
  if (!partial_reuse && partial_cache_) {
    BM_mesh_partial_destroy(partial_cache_);
    partial_cache_ = nullptr;
    partial_mask_.clear();
  }
  candidate.revision = revision_;
  return replace_display(C,
                         *object_,
                         std::move(candidate),
                         revision_,
                         error,
                         reuse_topology,
                         partial_reuse ? &partial_cache_ : nullptr,
                         partial_reuse ? &partial_mask_ : nullptr);
}

/* Complete rigid components and convex straight planar face translations can update the
 * visible BMesh exactly. Source construction and validation are deferred to confirmation. */
bool Session::can_preview_affine(const solid::Affine &m) const
{
  if (revision_ != source_.revision || (!rigid_selection_ && planar_faces_.empty()))
    return false;
  const auto &a = m.columns[0], &b = m.columns[1], &c = m.columns[2];
  if (!rigid_selection_) {
    if (a != solid::Vec3{1, 0, 0} || b != solid::Vec3{0, 1, 0} ||
        c != solid::Vec3{0, 0, 1})
      return false;
    for (const auto &face : planar_faces_) {
      auto point = [&](size_t i) {
        return face[i].position + (face[i].moving ? m.translation : solid::Vec3{});
      };
      solid::Vec3 normal, original;
      double scale = 1;
      for (size_t i = 1; i + 1 < face.size(); ++i) {
        normal = normal + solid::cross(point(i) - point(0), point(i + 1) - point(0));
        original = original + solid::cross(face[i].position - face[0].position,
                                           face[i + 1].position - face[0].position);
        scale = std::max(scale, solid::length(point(i)));
      }
      const double length = solid::length(normal);
      if (!(length > 0) || !std::isfinite(length) || solid::dot(normal, original) <= 0)
        return false;
      for (size_t i = 0; i < face.size(); ++i)
        if (std::abs(solid::dot(point(i) - point(0), normal) / length) >
            1024 * std::numeric_limits<double>::epsilon() * scale)
          return false;
      /* Existing triangle indices remain valid for convex planar triangles/quads. */
      for (size_t i = 0; i < face.size(); ++i)
        if (!(solid::dot(solid::cross(point((i + 1) % face.size()) - point(i),
                                     point((i + 2) % face.size()) - point((i + 1) % face.size())),
                         normal) > 0))
          return false;
    }
  }
  const double error = std::abs(solid::dot(a, a) - 1) + std::abs(solid::dot(b, b) - 1) +
                       std::abs(solid::dot(c, c) - 1) + std::abs(solid::dot(a, b)) +
                       std::abs(solid::dot(a, c)) + std::abs(solid::dot(b, c));
  return solid::dot(a, solid::cross(b, c)) > 0 && error < 1e-6 && std::isfinite(m.translation.x) &&
         std::isfinite(m.translation.y) && std::isfinite(m.translation.z);
}

static void affine_display_update(bContext *C, Mesh &mesh)
{
  /* Reuse evaluated geometry only when it directly shares this BMesh and
   * no modifier, dependent geometry or render engine needs geometry evaluation. */
  Object *object = CTX_data_edit_object(C);
  const View3D *view = CTX_wm_view3d(C);
  Object *evaluated_object = nullptr;
  Mesh *evaluated = nullptr;
  bool eligible = view && ELEM(view->shading.type, OB_WIRE, OB_SOLID) && object &&
                  object->modifiers.first == nullptr;
  if (eligible) {
    const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    evaluated_object = DEG_get_evaluated(depsgraph, object);
    evaluated = evaluated_object ? id_cast<Mesh *>(evaluated_object->data) : nullptr;
    eligible = evaluated && evaluated != &mesh &&
               evaluated->runtime->wrapper_type == ME_WRAPPER_TYPE_BMESH &&
               evaluated->runtime->edit_mesh &&
               evaluated->runtime->edit_mesh->bm == mesh.runtime->edit_mesh->bm &&
               (!evaluated->runtime->edit_data ||
                evaluated->runtime->edit_data->vert_positions.is_empty());
    if (eligible)
      DEG_foreach_dependent_ID(depsgraph, &mesh.id, [&](ID *id) {
        if (id != &mesh.id && id != &object->id && GS(id->name) != ID_SCE)
          eligible = false;
      });
  }
  if (eligible) {
    if (mesh.runtime->edit_mesh->looptris.is_empty())
      BKE_editmesh_looptris_calc(mesh.runtime->edit_mesh.get());
    EDBM_mesh_normals_update(mesh.runtime->edit_mesh.get());
    mesh.tag_positions_changed();
    evaluated->tag_positions_changed();
    BKE_mesh_wrapper_tag_positions_changed(evaluated);
    const auto bounds = evaluated->bounds_min_max();
    object->runtime->bounds_eval = bounds;
    evaluated_object->runtime->bounds_eval = bounds;
    const auto dirty = BKE_MESH_BATCH_DIRTY_RIGID_TRANSFORM;
    BKE_mesh_batch_cache_dirty_tag(&mesh, dirty);
    BKE_mesh_batch_cache_dirty_tag(evaluated, dirty);
    /* Workbench may reuse its completed shaded image until the depsgraph changes.
     * A transform evaluation refreshes that image without changing the object matrix
     * or re-evaluating independent geometry. A redraw notifier alone is insufficient. */
    DEG_id_tag_update(&object->id, ID_RECALC_TRANSFORM);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh);
    if (std::getenv("BLENDER_SOLID_PROFILE"))
      fprintf(stderr, "SOLID_GPU_REUSE\n");
    return;
  }
  EDBMUpdate_Params update{};
  update.calc_looptris = true;
  update.calc_normals = true;
  update.is_destructive = false;
  EDBM_update(&mesh, &update);
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh);
}

bool Session::preview_affine(bContext *C, const solid::Affine &m, std::string *error)
{
  Mesh &mesh = *id_cast<Mesh *>(object_->data);
  if (!can_preview_affine(m) || !mesh.runtime->edit_mesh)
    return false;
  const auto start = std::chrono::steady_clock::now();
  BMesh *bm = mesh.runtime->edit_mesh->bm;
  const int normal_offset = CustomData_get_offset_named(
      &bm->ldata, CD_PROP_FLOAT3, "custom_normal");

  BM_mesh_elem_table_ensure(bm, BM_VERT | BM_FACE);
  if (display_positions_.empty()) {
    if (source_.selection.vertices.size() != source_.model->vertices.size()) {
      BM_mesh_elem_index_ensure(bm, BM_VERT);
      display_position_mask_.resize(bm->totvert, false);
      const std::unordered_set<solid::Id> selected_vertices(source_.selection.vertices.begin(),
                                                            source_.selection.vertices.end());
      const std::unordered_set<solid::Id> selected_edges(source_.selection.edges.begin(),
                                                         source_.selection.edges.end());
      const std::unordered_set<solid::Id> selected_faces(source_.selection.faces.begin(),
                                                         source_.selection.faces.end());
      auto offset = [](CustomData &data, const char *name) {
        return CustomData_get_offset_named(&data, CD_PROP_INT32_2D, name);
      };
      auto id = [](const auto *element, int offset) -> solid::Id {
        if (offset < 0)
          return 0;
        const auto &value = *static_cast<const int2 *>(BM_ELEM_CD_GET_VOID_P(element, offset));
        return uint64_t(uint32_t(value.x)) | (uint64_t(uint32_t(value.y)) << 32);
      };
      const int vo = offset(bm->vdata, ".solid_vertex_id"),
                eo = offset(bm->edata, ".solid_edge_id"), fo = offset(bm->pdata, ".solid_face_id");
      for (int i = 0; i < bm->totvert; ++i)
        display_position_mask_[i] = selected_vertices.contains(id(BM_vert_at_index(bm, i), vo));
      BMIter iter;
      BMEdge *e;
      BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH)
        if (selected_edges.contains(id(e, eo))) {
          display_position_mask_[BM_elem_index_get(e->v1)] = true;
          display_position_mask_[BM_elem_index_get(e->v2)] = true;
        }
      for (int i = 0; i < bm->totface; ++i) {
        BMFace *f = BM_face_at_index(bm, i);
        const bool moving = selected_faces.contains(id(f, fo));
        BMLoop *loop = BM_FACE_FIRST_LOOP(f);
        do {
          display_normal_mask_.push_back(moving);
          if (moving)
            display_position_mask_[BM_elem_index_get(loop->v)] = true;
        } while ((loop = loop->next) != BM_FACE_FIRST_LOOP(f));
      }
    }
    for (int i = 0; i < bm->totvert; ++i) {
      const float *p = BM_vert_at_index(bm, i)->co;
      display_positions_.push_back({p[0], p[1], p[2]});
    }
    for (int i = 0; normal_offset >= 0 && i < bm->totface; ++i) {
      BMFace *f = BM_face_at_index(bm, i);
      BMLoop *loop = BM_FACE_FIRST_LOOP(f);
      do {
        const float *n = BM_ELEM_CD_GET_FLOAT_P(loop, normal_offset);
        display_normals_.push_back({n[0], n[1], n[2]});
      } while ((loop = loop->next) != BM_FACE_FIRST_LOOP(f));
    }
  }
  if (display_positions_.size() != bm->totvert ||
      (normal_offset >= 0 && display_normals_.size() != bm->totloop))
    return false;
  const auto &a = m.columns[0], &b = m.columns[1], &c = m.columns[2];
  auto position = [&](solid::Vec3 p) { return a * p.x + b * p.y + c * p.z + m.translation; };
  /* Reject overflow before changing any displayed coordinate. */
  for (size_t i = 0; i < display_positions_.size(); ++i) {
    if (!display_position_mask_.empty() && !display_position_mask_[i])
      continue;
    const auto p = position(display_positions_[i]);
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        std::abs(p.x) > FLT_MAX || std::abs(p.y) > FLT_MAX || std::abs(p.z) > FLT_MAX)
    {
      if (error)
        *error = "Solid transform exceeds display coordinate range";
      return false;
    }
  }
  for (int i = 0; i < bm->totvert; ++i) {
    if (!display_position_mask_.empty() && !display_position_mask_[i])
      continue;
    const auto p = position(display_positions_[i]);
    copy_v3_v3(BM_vert_at_index(bm, i)->co, float3(p.x, p.y, p.z));
  }
  size_t index = 0;
  for (int i = 0; normal_offset >= 0 && i < bm->totface; ++i) {
    BMFace *f = BM_face_at_index(bm, i);
    bool deformed = false;
    if (!rigid_selection_) {
      BMLoop *l = BM_FACE_FIRST_LOOP(f);
      do {
        deformed |= display_position_mask_[BM_elem_index_get(l->v)];
      } while ((l = l->next) != BM_FACE_FIRST_LOOP(f));
      if (deformed)
        BM_face_calc_normal(f, f->no);
    }
    BMLoop *loop = BM_FACE_FIRST_LOOP(f);
    do {
      const bool moving = display_normal_mask_.empty() || display_normal_mask_[index];
      const auto original = display_normals_[index++];
      if (deformed) {
        copy_v3_v3(BM_ELEM_CD_GET_FLOAT_P(loop, normal_offset), f->no);
        continue;
      }
      if (!moving)
        continue;
      auto n = a * original.x + b * original.y + c * original.z;
      const double size = solid::length(n);
      if (size > 0)
        n = n / size;
      copy_v3_v3(BM_ELEM_CD_GET_FLOAT_P(loop, normal_offset), float3(n.x, n.y, n.z));
    } while ((loop = loop->next) != BM_FACE_FIRST_LOOP(f));
  }
  display_matrix_ = m;
  display_preview_ = true;
  display_restored_ = false;
  affine_display_update(C, mesh);
  if (std::getenv("BLENDER_SOLID_PROFILE"))
    fprintf(stderr,
            "%s update_ms=%.3f\n",
            rigid_selection_ ? "SOLID_AFFINE_DRAG" : "SOLID_PLANAR_DRAG",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
  return true;
}

bool Session::commit_affine(bContext *C, std::string *error)
{
  if (!display_preview_)
    return true;
  const auto start = std::chrono::steady_clock::now();
  auto result = solid::transform_selected(
      *source_.model, source_.selection, display_matrix_, error);
  if (!result)
    return false;
  if (!rigid_selection_) {
    /* Reconcile the display with the checked source once, on confirmation. */
    if (!apply(C, std::move(*result), error))
      return false;
    display_preview_ = false;
    if (std::getenv("BLENDER_SOLID_PROFILE"))
      fprintf(stderr,
              "SOLID_PLANAR_COMMIT update_ms=%.3f\n",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count());
    return true;
  }
  bke::solid_host::Snapshot next{std::make_shared<const solid::Model>(std::move(result->model)),
                                 std::move(result->selection),
                                 revision_};
  Mesh &mesh = *id_cast<Mesh *>(object_->data);
  if (!bke::solid_host::publish(mesh, std::move(next), revision_, error))
    return false;
  ++revision_;
  display_preview_ = false;
  /* The same BMesh already occupies the confirmed position. Do not replace it. */
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh);
  return true;
}

bool Session::restore(bContext *C, std::string *error)
{
  if (partial_cache_) {
    BM_mesh_partial_destroy(partial_cache_);
    partial_cache_ = nullptr;
    partial_mask_.clear();
  }
  if (display_restored_ && revision_ == source_.revision)
    return true;
  if (display_preview_) {
    solid::Affine identity;
    identity.columns = {solid::Vec3{1, 0, 0}, solid::Vec3{0, 1, 0}, solid::Vec3{0, 0, 1}};
    if (!preview_affine(C, identity, error))
      return false;
    /* Preserve the original floats exactly, including custom normals. */
    Mesh &mesh = *id_cast<Mesh *>(object_->data);
    BMesh *bm = mesh.runtime->edit_mesh->bm;
    const int offset = CustomData_get_offset_named(&bm->ldata, CD_PROP_FLOAT3, "custom_normal");
    size_t index = 0;
    for (int i = 0; offset >= 0 && i < bm->totface; ++i) {
      BMFace *f = BM_face_at_index(bm, i);
      BMLoop *loop = BM_FACE_FIRST_LOOP(f);
      do {
        const auto n = display_normals_[index++];
        copy_v3_v3(BM_ELEM_CD_GET_FLOAT_P(loop, offset), float3(n.x, n.y, n.z));
      } while ((loop = loop->next) != BM_FACE_FIRST_LOOP(f));
    }
    display_preview_ = false;
    display_restored_ = true;
    return true;
  }
  return replace_display(C, *object_, source_, revision_, error);
}

}  // namespace ed::solid_editor

namespace {

static double3 host_point(const solid::Vec3 &p)
{
  return {p.x, p.y, p.z};
}

static std::optional<solid::EditResult> bevel_result(const solid::Model &model,
                                                     solid::Selection selection,
                                                     double width,
                                                     bool vertices_only,
                                                     std::string *error,
                                                     int subdivisions,
                                                     solid::CornerTopology corner_topology,
                                                     std::string *warning = nullptr)
{
  if (subdivisions > 0) {
    return solid::fillet(
        model,
        std::move(selection),
        width,
        subdivisions,
        error,
        vertices_only,
        warning,
        corner_topology);
  }
  return solid::chamfer(model, std::move(selection), width, vertices_only, error);
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
                                             std::string &error,
                                             const bool allow_boundary_cap = false)
{
  return solid::prepare_profile(
      *session.source().model, session.source().selection, &error, allow_boundary_cap);
}

static solid::Selection selection_for_face(const solid::Model &model,
                                            const solid::Id face_id,
                                            const int mode)
{
  solid::Selection selection;
  selection.mode = mode & 4 ? 4 : mode & 2 ? 2 : 1;
  const solid::Face *face = solid::face(model, face_id);
  if (!face)
    return selection;
  if (selection.mode == 4) {
    selection.faces.push_back(face_id);
    selection.active = face_id;
    return selection;
  }
  auto select_boundary = [&](const std::vector<solid::Coedge> &loop) {
    for (const solid::Coedge &use : loop) {
      const solid::Edge *edge = solid::edge(model, use.edge);
      if (!edge)
        continue;
      if (selection.mode == 2) {
        selection.edges.push_back(edge->id);
      }
      else {
        selection.vertices.push_back(edge->vertices[0]);
        selection.vertices.push_back(edge->vertices[1]);
      }
    }
  };
  select_boundary(face->outer);
  for (const auto &hole : face->holes)
    select_boundary(hole);
  if (selection.mode == 2 && !selection.edges.empty())
    selection.active = selection.edges.back();
  else if (selection.mode == 1 && !selection.vertices.empty())
    selection.active = selection.vertices.back();
  return selection;
}

static wmOperatorStatus fill_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  auto profile = session ? prepare(*session, error, true) : std::nullopt;
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
        profile = solid::prepare_profile(connected.model, connected.selection, &error, true);
      }
    }
  }
  solid::Selection selection;
  selection.mode = session->source().selection.mode;
  if (profile)
    selection = selection_for_face(profile->model, profile->face, selection.mode);
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

static void finish_sketch_extrusion(bContext *C)
{
  if (bToolRef *tool = WM_toolsystem_ref_from_context(C)) {
    if (STRPREFIX(tool->idname, "builtin.nurbs_draw_"))
      WM_toolsystem_ref_set_by_id(C, "builtin.select_box");
  }
}

static wmOperatorStatus extrude_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto preview = ed::solid_editor::Session::begin(C, &error);
  auto profile = preview ? prepare(*preview, error) : std::nullopt;
  const int selection_mode = preview ? preview->source().selection.mode : 1;
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
  if (result && profile && !vector_offset)
    result->selection = selection_for_face(
        result->model, result->selection.active, selection_mode);
  if (!result || !preview->apply(C, std::move(*result), &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  finish_sketch_extrusion(C);
  return OPERATOR_FINISHED;
}

struct ExtrudeModal {
  MEM_CXX_CLASS_ALLOC_FUNCS("Solid extrusion");
  std::unique_ptr<ed::solid_editor::Session> preview;
  ARegion *region = nullptr;
  void *draw_handle_pixel = nullptr;
  std::optional<solid::PreparedExtrusion> profile;
  bool elements = false;
  bool bevel = false;
  bool vertices_only = false;
  int selection_mode = 1;
  int subdivisions = 0;
  solid::CornerTopology corner_topology = solid::CornerTopology::Quads;
  int axis = -1;
  solid::Vec3 offset;
  float depth = 1;
  NumInput num{};
  float2 initial_mouse;
  float2 direction = {0, 1};
  double units_per_pixel = 0;
  float bevel_center[2] = {};
  double bevel_start_length = 0;
  double bevel_start_offset = 0;
  double bevel_mouse_margin = 0;
  double bevel_units_per_pixel = 0;
  double bevel_last_valid_distance = 0;
  bool bevel_precision = false;
  bool bevel_snap = false;
  bool bevel_has_valid_preview = false;
  bool bevel_limit_reached = false;
  double distance = 0;
  bool valid = false;
  std::string error;
};

static void cleanup(bContext *C, wmOperator *op)
{
  auto *data = static_cast<ExtrudeModal *>(op->customdata);
  if (data && data->draw_handle_pixel && data->region) {
    ED_region_draw_cb_exit(data->region->runtime->type, data->draw_handle_pixel);
  }
  MEM_delete(data);
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
  const bool profile_bevel = data.bevel && std::getenv("BLENDER_BEVEL_PROFILE");
  const auto profile_start = profile_bevel ? std::chrono::steady_clock::now() :
                                             std::chrono::steady_clock::time_point{};
  std::optional<solid::PreparedExtrusion::Preview> extrusion;
  if (!data.bevel && !data.elements) {
    const Mesh &mesh = *id_cast<Mesh *>(data.preview->object().data);
    extrusion = data.profile->preview(
        data.distance, bke::solid_host::display_uses_analytic_normals(mesh), &data.error);
  }
  std::optional<solid::EditResult> result;
  if (data.bevel && data.distance > 0) {
    result = bevel_result(*source.model,
                          source.selection,
                          std::abs(data.distance),
                          data.vertices_only,
                          &data.error,
                          data.subdivisions,
                          data.corner_topology);
  }
  else if (data.elements) {
    result = solid::extrude_elements(*source.model, source.selection, data.offset, &data.error);
  }
  const auto kernel_end = profile_bevel ? std::chrono::steady_clock::now() :
                                          std::chrono::steady_clock::time_point{};
  const size_t result_vertices = profile_bevel && result ? result->model.vertices.size() : 0;
  const size_t result_edges = profile_bevel && result ? result->model.edges.size() : 0;
  const size_t result_faces = profile_bevel && result ? result->model.faces.size() : 0;
  /* Extrusion distances and bevel widths usually preserve connectivity. The adapter checks it and
   * rebuilds when segment counts or the generated boundary change. */
  if (extrusion) {
    bke::solid_host::Snapshot candidate{std::move(extrusion->model),
                                        std::move(extrusion->selection),
                                        source.revision,
                                        std::move(extrusion->samples),
                                        std::move(extrusion->validation)};
    candidate.selection = selection_for_face(
        *candidate.model, candidate.selection.active, data.selection_mode);
    data.valid = data.preview->apply(C, std::move(candidate), &data.error, true);
  }
  else if (data.bevel && data.distance <= 0) {
    data.valid = data.preview->restore(C, &data.error);
    data.bevel_has_valid_preview = false;
    data.bevel_limit_reached = false;
  }
  else {
    const bool candidate_valid = result &&
                                 data.preview->apply(C, std::move(*result), &data.error, true);
    if (data.bevel && candidate_valid) {
      data.valid = true;
      data.bevel_has_valid_preview = true;
      data.bevel_last_valid_distance = data.distance;
      data.bevel_limit_reached = false;
    }
    else if (data.bevel && data.bevel_has_valid_preview) {
      /* Keep the last published bevel visible and confirmable when the requested width
       * exceeds the solid's geometric limit. Moving back to a valid width resumes updates. */
      data.valid = true;
      data.distance = data.bevel_last_valid_distance;
      data.bevel_limit_reached = true;
    }
    else {
      data.valid = candidate_valid;
    }
  }
  if (profile_bevel) {
    const auto profile_end = std::chrono::steady_clock::now();
    const double kernel_ms = std::chrono::duration<double, std::milli>(kernel_end - profile_start)
                                 .count();
    const double apply_ms = std::chrono::duration<double, std::milli>(profile_end - kernel_end)
                                .count();
    fprintf(stderr,
            "SOLID_BEVEL_UPDATE kernel_ms=%.3f apply_ms=%.3f total_ms=%.3f valid=%d "
            "width=%.9g segments=%d source_v=%zu source_e=%zu source_f=%zu "
            "result_v=%zu result_e=%zu result_f=%zu\n",
            kernel_ms,
            apply_ms,
            kernel_ms + apply_ms,
            data.valid,
            data.distance,
            data.subdivisions,
            source.model->vertices.size(),
            source.model->edges.size(),
            source.model->faces.size(),
            result_vertices,
            result_edges,
            result_faces);
  }
  if (!data.valid && (!data.bevel || !data.bevel_has_valid_preview)) {
    /* Do not leave a stale, apparently valid extrusion visible at a rejected distance. */
    std::string restore_error;
    if (!data.preview->restore(C, &restore_error)) {
      data.error = restore_error;
    }
  }
  char status[512];
  if (data.valid && data.bevel && data.bevel_limit_reached) {
    BLI_snprintf(
        status,
        sizeof(status),
        "Bevel / Fillet Solid: %.9g (last valid width) | Shift: Precision | Ctrl: Snap | "
        "Segments: %d (Wheel) | Enter: Keep | Esc: Cancel",
        data.distance,
        data.subdivisions);
  }
  else if (data.valid && data.bevel) {
    BLI_snprintf(
        status,
        sizeof(status),
        "Bevel / Fillet Solid: %.9g | Shift: Precision | Ctrl: Snap | "
        "Segments: %d (Wheel) | LMB/Enter: Confirm | Esc: Cancel",
        data.distance,
        data.subdivisions);
  }
  else if (data.valid) {
    BLI_snprintf(status,
                 sizeof(status),
                 "%s: %.9g | X/Y/Z: Axis | Ctrl: Snap | "
                 "LMB/Enter: Confirm | RMB/Esc: Cancel",
                 data.bevel ? "Bevel / Fillet Solid" : "Extrude Solid",
                 data.distance);
  }
  else {
    BLI_snprintf(status,
                 sizeof(status),
                 "%s: %s | Move mouse or type distance | Esc: Cancel",
                 data.bevel ? "Bevel / Fillet Solid" : "Extrude Solid",
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
  data->selection_mode = data->preview->source().selection.mode;
  if (profile) {
    data->profile = solid::PreparedExtrusion::create(std::move(*profile), &error);
    if (!data->profile) {
      MEM_delete(data);
      BKE_report(op->reports, RPT_ERROR, error.c_str());
      return OPERATOR_CANCELLED;
    }
  }
  data->initial_mouse = float2(event->mval);
  const double4x4 transform(data->preview->object().object_to_world());
  const auto &source = data->preview->source();
  const auto *vertex = solid::vertex(*source.model, source.selection.vertices.front());
  const float3 center(math::transform_point(transform, host_point(vertex->position)));
  const solid::Vec3 normal = data->elements ? solid::Vec3{0, 0, 1} : data->profile->normal();
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
  if (data.bevel && event->val == KM_PRESS && ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
    data.subdivisions = std::clamp(
        data.subdivisions + (event->type == WHEELUPMOUSE ? 1 : -1), 0, 32);
    RNA_int_set(op->ptr, "segments", data.subdivisions);
    update(C, op);
    return OPERATOR_RUNNING_MODAL;
  }
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
    const bool extruded = !data.bevel;
    cleanup(C, op);
    if (extruded)
      finish_sketch_extrusion(C);
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
    if (data.bevel) {
      const double length = math::distance(float2(event->mval), float2(data.bevel_center));
      const bool precision = event->modifier & KM_SHIFT;
      const bool snap = event->modifier & KM_CTRL;
      if (precision != data.bevel_precision || snap != data.bevel_snap) {
        /* Rebase when a modifier changes so the preview does not jump. */
        data.bevel_start_offset = data.distance;
        data.bevel_start_length = length;
        data.bevel_mouse_margin = 0;
        data.bevel_precision = precision;
        data.bevel_snap = snap;
      }
      data.distance = std::max(
          0.0,
          data.bevel_start_offset +
              (length - data.bevel_start_length - data.bevel_mouse_margin) *
                  data.bevel_units_per_pixel * (data.bevel_precision ? 0.1 : 1.0));
      if (data.bevel_snap) {
        const double snap_step = data.bevel_precision ? 0.01 : 0.1;
        data.distance = std::round(data.distance / snap_step) * snap_step;
      }
      data.distance = std::min(data.distance, 1e6);
    }
    else {
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
    }
    update(C, op);
  }
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus duplicate_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  auto result = session ? solid::duplicate_elements(
                              *session->source().model, session->source().selection, &error) :
                          std::nullopt;
  if (!result || !session->apply(C, std::move(*result), &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

static wmOperatorStatus translate_delta_exec(bContext *C, wmOperator *op)
{
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  if (!session) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  float offset[3];
  RNA_float_get_array(op->ptr, "offset", offset);
  solid::Affine transform;
  transform.translation = {offset[0], offset[1], offset[2]};
  auto result = solid::transform_selected(
      *session->source().model, session->source().selection, transform, &error);
  if (!result || !session->apply(C, std::move(*result), &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
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
  const bool profile_bevel = std::getenv("BLENDER_BEVEL_PROFILE");
  const auto profile_start = profile_bevel ? std::chrono::steady_clock::now() :
                                             std::chrono::steady_clock::time_point{};
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  const auto session_end = profile_bevel ? std::chrono::steady_clock::now() :
                                           std::chrono::steady_clock::time_point{};
  const bool vertices_only = session && (RNA_boolean_get(op->ptr, "vertices_only") ||
                                         session->source().selection.mode == 1);
  const auto corner_topology = solid::CornerTopology(
      RNA_enum_get(op->ptr, "corner_topology"));
  std::string warning;
  const auto kernel_start = profile_bevel ? std::chrono::steady_clock::now() :
                                            std::chrono::steady_clock::time_point{};
  auto result = session ? bevel_result(*session->source().model,
                                       session->source().selection,
                                       RNA_float_get(op->ptr, "offset"),
                                       vertices_only,
                                       &error,
                                       RNA_int_get(op->ptr, "segments"),
                                       corner_topology,
                                       &warning) :
                          std::nullopt;
  const auto kernel_end = profile_bevel ? std::chrono::steady_clock::now() :
                                          std::chrono::steady_clock::time_point{};
  const size_t result_vertices = profile_bevel && result ? result->model.vertices.size() : 0;
  const size_t result_edges = profile_bevel && result ? result->model.edges.size() : 0;
  const size_t result_faces = profile_bevel && result ? result->model.faces.size() : 0;
  const size_t result_surfaces = profile_bevel && result ? result->model.surfaces.size() : 0;
  const bool valid = result && session->apply(C, std::move(*result), &error);
  if (profile_bevel) {
    const auto profile_end = std::chrono::steady_clock::now();
    const double session_ms = std::chrono::duration<double, std::milli>(session_end - profile_start)
                                  .count();
    const double kernel_ms = std::chrono::duration<double, std::milli>(kernel_end - kernel_start)
                                 .count();
    const double apply_ms = std::chrono::duration<double, std::milli>(profile_end - kernel_end)
                                .count();
    fprintf(stderr,
            "SOLID_BEVEL_EXEC session_ms=%.3f kernel_ms=%.3f apply_ms=%.3f total_ms=%.3f "
            "valid=%d width=%.9g segments=%d source_s=%zu result_v=%zu result_e=%zu "
            "result_f=%zu result_s=%zu\n",
            session_ms,
            kernel_ms,
            apply_ms,
            std::chrono::duration<double, std::milli>(profile_end - profile_start).count(),
            valid,
            RNA_float_get(op->ptr, "offset"),
            RNA_int_get(op->ptr, "segments"),
            session ? session->source().model->surfaces.size() : 0,
            result_vertices,
            result_edges,
            result_faces,
            result_surfaces);
  }
  if (!valid) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  if (!warning.empty())
    BKE_report(op->reports, RPT_WARNING, warning.c_str());
  return OPERATOR_FINISHED;
}
static wmOperatorStatus bevel_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!CTX_wm_region_view3d(C))
    return bevel_exec(C, op);
  const bool profile_bevel = std::getenv("BLENDER_BEVEL_PROFILE");
  const auto invoke_start = std::chrono::steady_clock::now();
  std::string error;
  auto session = ed::solid_editor::Session::begin(C, &error);
  const auto session_end = std::chrono::steady_clock::now();
  const bool vertices_only = session && (RNA_boolean_get(op->ptr, "vertices_only") ||
                                         session->source().selection.mode == 1);
  const auto corner_topology = solid::CornerTopology(
      RNA_enum_get(op->ptr, "corner_topology"));
  std::string warning;
  const auto kernel_start = std::chrono::steady_clock::now();
  auto check = session ? bevel_result(*session->source().model,
                                      session->source().selection,
                                      RNA_float_get(op->ptr, "offset"),
                                      vertices_only,
                                      &error,
                                      RNA_int_get(op->ptr, "segments"),
                                      corner_topology,
                                      &warning) :
                         std::nullopt;
  const auto kernel_end = std::chrono::steady_clock::now();
  if (profile_bevel) {
    const double session_ms = std::chrono::duration<double, std::milli>(session_end - invoke_start)
                                  .count();
    const double preflight_ms = std::chrono::duration<double, std::milli>(kernel_end - kernel_start)
                                    .count();
    fprintf(stderr,
            "SOLID_BEVEL_INVOKE session_ms=%.3f preflight_ms=%.3f total_ms=%.3f valid=%d "
            "width=%.9g segments=%d\n",
            session_ms,
            preflight_ms,
            std::chrono::duration<double, std::milli>(kernel_end - invoke_start).count(),
            bool(check),
            RNA_float_get(op->ptr, "offset"),
            RNA_int_get(op->ptr, "segments"));
  }
  if (!check) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  if (!warning.empty())
    BKE_report(op->reports, RPT_WARNING, warning.c_str());
  auto *data = MEM_new<ExtrudeModal>(__func__);
  data->preview = std::move(session);
  data->bevel = true;
  data->vertices_only = vertices_only;
  data->corner_topology = corner_topology;
  data->subdivisions = RNA_int_get(op->ptr, "segments");
  data->region = CTX_wm_region(C);
  float center3d[3];
  if (!ed::transform::calculateTransformCenter(
          C, V3D_AROUND_CENTER_MEDIAN, center3d, data->bevel_center))
  {
    MEM_delete(data);
    BKE_report(op->reports, RPT_ERROR, "Could not find the selected geometry center for bevel");
    return OPERATOR_CANCELLED;
  }
  data->bevel_start_length = math::distance(float2(event->mval), float2(data->bevel_center));
  data->bevel_mouse_margin = ed::solid_editor::solid_bevel_mouse_pixel_margin;
  const float object_scale = mat4_to_scale(data->preview->object().object_to_world().ptr());
  data->bevel_units_per_pixel = ED_view3d_pixel_size(
                                    CTX_wm_region_view3d(C), center3d) /
                                object_scale;
  if (!std::isfinite(data->bevel_units_per_pixel) || data->bevel_units_per_pixel <= 0) {
    MEM_delete(data);
    BKE_report(
        op->reports, RPT_ERROR, "Bevel Solid requires a usable object and viewport scale");
    return OPERATOR_CANCELLED;
  }
  initNumInput(&data->num);
  data->num.flag |= NUM_USE_DOUBLE;
  data->num.idx_max = 0;
  data->num.unit_sys = CTX_data_scene(C)->unit.system;
  data->num.unit_type[0] = B_UNIT_LENGTH;
  op->customdata = data;
  data->draw_handle_pixel = ED_region_draw_cb_activate(data->region->runtime->type,
                                                       ED_region_draw_mouse_line_cb,
                                                       data->bevel_center,
                                                       REGION_DRAW_POST_PIXEL);
  ED_workspace_status_text(C,
                           "Bevel / Fillet Solid: Move away from selection center to set width | "
                           "Shift: Precision | Ctrl: Snap | Wheel: Segments | "
                           "Enter: Confirm | Esc: Cancel");
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
  ot->description = "Fill a closed planar sketch or cap an open planar face boundary";
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

void SOLID_OT_duplicate(wmOperatorType *ot)
{
  ot->name = "Duplicate Solid Elements";
  ot->idname = "SOLID_OT_duplicate";
  ot->description =
      "Duplicate selected source vertices, edges and faces with independent geometry";
  ot->poll = solid_edit_poll;
  ot->exec = duplicate_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void SOLID_OT_translate_delta(wmOperatorType *ot)
{
  ot->name = "Translate Solid Source by Delta";
  ot->idname = "SOLID_OT_translate_delta";
  ot->description =
      "Apply one incremental offset to selected analytic geometry during a modal tool";
  ot->poll = solid_edit_poll;
  ot->exec = translate_delta_exec;
  ot->flag = OPTYPE_INTERNAL;
  RNA_def_float_vector(ot->srna,
                       "offset",
                       3,
                       nullptr,
                       -FLT_MAX,
                       FLT_MAX,
                       "Offset",
                       "Incremental offset in object-local coordinates",
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
  ot->description =
      "Chamfer planar solid edges or round a shared network with analytic conic profiles";
  ot->poll = solid_edit_poll;
  ot->exec = bevel_exec;
  ot->invoke = bevel_invoke;
  ot->modal = extrude_modal;
  ot->cancel = extrude_cancel;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_GRAB_CURSOR_XY | OPTYPE_BLOCKING;
  RNA_def_float_distance(
      ot->srna,
      "offset",
      0.1f,
      1e-8f,
      FLT_MAX,
      "Width",
      "Width along adjacent faces in object-local units, matching the native bevel workflow",
      .001f,
      1);
  RNA_def_boolean(ot->srna,
                  "vertices_only",
                  false,
                  "Vertices Only",
                  "Bevel selected vertices instead of edges");
  RNA_def_int(ot->srna,
              "segments",
              0,
              0,
              32,
              "Segments",
              "0: flat chamfer; 1: exact curved fillet; higher values split the curved surfaces",
              0,
              8);
  static const EnumPropertyItem corner_topologies[] = {
      {0, "QUADS", 0, "Quads", "Use a quad-dominant layout for rounded corner patches"},
      {1, "TRIANGLES", 0, "Triangles", "Split rounded corner patches into triangles"},
      {0, nullptr, 0, nullptr, nullptr}};
  RNA_def_enum(ot->srna,
               "corner_topology",
               corner_topologies,
               0,
               "Corner Topology",
               "Odd segment counts require one triangle to keep the patch boundary matched");
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
