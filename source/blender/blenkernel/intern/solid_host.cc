/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_solid_host.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_solid_types.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "bmesh.hh"

#include "BLO_read_write.hh"

namespace blender::bke::solid_host {
namespace {
struct Runtime {
  std::shared_ptr<const solid::Model> model;
  std::shared_ptr<const solid::DisplayMesh> display_samples;
  solid::Selection selection;
  std::string error;
};

Runtime &ensure(const SolidStorage &storage)
{
  auto &mutable_storage = const_cast<SolidStorage &>(storage);
  if (!mutable_storage.runtime) {
    auto *runtime = new Runtime;
    const auto decoded = storage.bytes_num > 0 ? solid::decode(storage.bytes,
                                                               size_t(storage.bytes_num),
                                                               &runtime->error) :
                                                 std::nullopt;
    if (decoded) {
      runtime->model = std::make_shared<const solid::Model>(std::move(decoded->model));
      runtime->selection = std::move(decoded->selection);
    }
    else if (runtime->error.empty()) {
      runtime->error = "Analytic source storage is missing or invalid";
    }
    mutable_storage.runtime = runtime;
  }
  return *static_cast<Runtime *>(mutable_storage.runtime);
}

int2 split_id(solid::Id id)
{
  return {int32_t(uint32_t(id)), int32_t(uint32_t(id >> 32))};
}
}  // namespace

SolidStorage *storage_create(solid::Model model, solid::Selection selection, std::string *error)
{
  if (!solid::validate(model, error))
    return nullptr;
  auto *storage = MEM_new<SolidStorage>(__func__);
  auto *runtime = new Runtime;
  runtime->model = std::make_shared<const solid::Model>(std::move(model));
  runtime->selection = solid::normalize_selection(*runtime->model, std::move(selection));
  storage->runtime = runtime;
  return storage;
}

SolidStorage *storage_copy(const SolidStorage *source)
{
  if (!source)
    return nullptr;
  auto *copy = MEM_new<SolidStorage>(__func__);
  copy->revision = source->revision;
  const Runtime &runtime = ensure(*source);
  if (!runtime.model) {
    copy->bytes_num = source->bytes_num;
    copy->bytes = source->bytes ? MEM_dupalloc(source->bytes) : nullptr;
  }
  copy->runtime = new Runtime(runtime);
  return copy;
}

void storage_free(SolidStorage *storage)
{
  if (!storage)
    return;
  delete static_cast<Runtime *>(storage->runtime);
  MEM_SAFE_DELETE(storage->bytes);
  MEM_delete(storage);
}

void storage_write(BlendWriter *writer, const SolidStorage &storage)
{
  const auto &runtime = ensure(storage);
  std::vector<uint8_t> encoded = runtime.model ? solid::encode(*runtime.model, runtime.selection) :
                                                 std::vector<uint8_t>();
  SolidStorage saved = storage;
  saved.runtime = nullptr;
  if (!encoded.empty()) {
    saved.bytes = encoded.data();
    saved.bytes_num = encoded.size();
  }
  BLO_write_generated_pointer_tag(writer, saved.bytes);
  writer->write_struct_at_address(&storage, &saved);
  writer->write_uint8_array(saved.bytes_num, saved.bytes);
}

void storage_read(BlendDataReader *reader, SolidStorage &storage)
{
  storage.runtime = nullptr;
  if (storage.bytes_num < 0 || storage.bytes_num > (128 << 20) ||
      !BLO_read_array(reader, &storage.bytes, storage.bytes_num))
  {
    storage.bytes = nullptr;
    storage.bytes_num = 0;
  }
  /* Preserve failure explicitly. A corrupt analytic payload must not become an editable mesh. */
  if (ensure(storage).model) {
    MEM_SAFE_DELETE(storage.bytes);
    storage.bytes_num = 0;
  }
}

std::optional<Snapshot> read(const Mesh &mesh, std::string *error)
{
  if (!mesh.solid_storage) {
    if (error)
      *error = "Object has no analytic solid source";
    return {};
  }
  const auto &runtime = ensure(*mesh.solid_storage);
  if (!runtime.model) {
    if (error)
      *error = runtime.error;
    return {};
  }
  return Snapshot{
      runtime.model, runtime.selection, mesh.solid_storage->revision, runtime.display_samples};
}

std::optional<Snapshot> read_edit(const Mesh &mesh, std::string *error)
{
  auto source = read(mesh, error);
  if (!source || !mesh.runtime->edit_mesh)
    return source;
  const BMesh &bm = *mesh.runtime->edit_mesh->bm;
  auto &selection = source->selection;
  const std::unordered_set<solid::Id> previous_edges(selection.edges.begin(), selection.edges.end());
  selection.mode = bm.selectmode & 4 ? 4 : bm.selectmode & 2 ? 2 : 1;
  auto read_domain =
      [&](const CustomData &data, int iter_type, const char *name, std::vector<solid::Id> &ids) {
        const int offset = CustomData_get_offset_named(&data, CD_PROP_INT32_2D, name);
        if (offset < 0) {
          ids.clear();
          return;
        }
        struct Counts {
          int total = 0, selected = 0;
        };
        std::unordered_map<solid::Id, Counts> counts;
        BMIter iter;
        BMElem *element;
        BM_ITER_MESH (element, &iter, const_cast<BMesh *>(&bm), iter_type) {
          const auto *value = static_cast<const int2 *>(BM_ELEM_CD_GET_VOID_P(element, offset));
          const solid::Id id = uint64_t(uint32_t(value->x)) | (uint64_t(uint32_t(value->y)) << 32);
          if (id) {
            auto &count = counts[id];
            count.total++;
            count.selected += BM_elem_flag_test(element, BM_ELEM_SELECT) != 0;
          }
        }
        std::vector<solid::Id> selected_ids;
        const std::unordered_set<solid::Id> previous(ids.begin(), ids.end());
        for (const auto &[id, count] : counts) {
          const bool was_selected = previous.contains(id);
          if ((was_selected && count.selected == count.total) ||
              (!was_selected && count.selected > 0))
            selected_ids.push_back(id);
        }
        ids = std::move(selected_ids);
        std::sort(ids.begin(), ids.end());
      };
  read_domain(bm.vdata, BM_VERTS_OF_MESH, ".solid_vertex_id", selection.vertices);
  read_domain(bm.edata, BM_EDGES_OF_MESH, ".solid_edge_id", selection.edges);
  read_domain(bm.pdata, BM_FACES_OF_MESH, ".solid_face_id", selection.faces);
  if (selection.mode == 2) {
    /* A periodic face's closing rail is source topology, but its two uses belong to the same
     * logical face and are suppressed in the sampled edge IDs. Preserve that implicit edge
     * when its endpoints are selected, so switching to edge mode does not drop the curved wall. */
    std::unordered_set<solid::Id> represented;
    const int offset = CustomData_get_offset_named(&bm.edata, CD_PROP_INT32_2D, ".solid_edge_id");
    if (offset >= 0) {
      BMIter iter;
      BMEdge *edge;
      BM_ITER_MESH (edge, &iter, const_cast<BMesh *>(&bm), BM_EDGES_OF_MESH) {
        const int2 &id = *static_cast<const int2 *>(BM_ELEM_CD_GET_VOID_P(edge, offset));
        represented.insert(uint64_t(uint32_t(id.x)) | (uint64_t(uint32_t(id.y)) << 32));
      }
      const std::unordered_set<solid::Id> vertices(selection.vertices.begin(), selection.vertices.end());
      for (const solid::Edge &edge : source->model->edges) {
        if (previous_edges.contains(edge.id) && !represented.contains(edge.id) && vertices.contains(edge.vertices[0]) &&
            vertices.contains(edge.vertices[1]))
        {
          selection.edges.push_back(edge.id);
        }
      }
    }
  }
  std::vector<solid::Id> ordered;
  std::unordered_set<solid::Id> ordered_ids;
  const std::unordered_set<solid::Id> selected_vertices(selection.vertices.begin(),
                                                       selection.vertices.end());
  for (const BMEditSelection *item = static_cast<const BMEditSelection *>(bm.selected.first); item;
       item = item->next)
  {
    const CustomData &domain = item->htype == BM_VERT ? bm.vdata :
                               item->htype == BM_EDGE ? bm.edata :
                                                        bm.pdata;
    const char *name = item->htype == BM_VERT ? ".solid_vertex_id" :
                       item->htype == BM_EDGE ? ".solid_edge_id" :
                                                ".solid_face_id";
    const int offset = CustomData_get_offset_named(&domain, CD_PROP_INT32_2D, name);
    if (offset < 0)
      continue;
    const int2 &value = *static_cast<const int2 *>(BM_ELEM_CD_GET_VOID_P(item->ele, offset));
    const solid::Id id = uint64_t(uint32_t(value.x)) | (uint64_t(uint32_t(value.y)) << 32);
    if (!id)
      continue;
    selection.active = id;
    if (item->htype == BM_VERT && selected_vertices.contains(id) && ordered_ids.insert(id).second)
      ordered.push_back(id);
  }
  for (solid::Id id : selection.vertices)
    if (ordered_ids.insert(id).second)
      ordered.push_back(id);
  selection.vertices = std::move(ordered);
  selection = solid::normalize_selection(*source->model, std::move(selection));
  return source;
}

void sync_edit_selection(Mesh &mesh)
{
  auto source = read_edit(mesh);
  if (!source || !mesh.runtime->edit_mesh)
    return;
  if (!publish(mesh, *source, source->revision, nullptr))
    return;
  BMesh &bm = *mesh.runtime->edit_mesh->bm;
  /* Deselecting the sampled display also clears BMesh selection history. Keep its ordered
   * elements so source-selection synchronization preserves the native active white highlight. */
  std::vector<BMElem *> history;
  for (const BMEditSelection *item = static_cast<const BMEditSelection *>(bm.selected.first); item;
       item = item->next)
  {
    history.push_back(item->ele);
  }
  BM_mesh_elem_hflag_disable_all(&bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  auto source_id = [](const BMElem *element, int offset) -> solid::Id {
    if (offset < 0)
      return 0;
    const auto &id = *static_cast<const int2 *>(BM_ELEM_CD_GET_VOID_P(element, offset));
    return uint64_t(uint32_t(id.x)) | (uint64_t(uint32_t(id.y)) << 32);
  };
  const int v_offset = CustomData_get_offset_named(
      &bm.vdata, CD_PROP_INT32_2D, ".solid_vertex_id");
  const int e_offset = CustomData_get_offset_named(&bm.edata, CD_PROP_INT32_2D, ".solid_edge_id");
  const int f_offset = CustomData_get_offset_named(&bm.pdata, CD_PROP_INT32_2D, ".solid_face_id");
  BMIter iter;
  BMVert *v;
  BMEdge *e;
  BMFace *f;
  const std::unordered_set<solid::Id> vertices(source->selection.vertices.begin(),
                                               source->selection.vertices.end());
  const std::unordered_set<solid::Id> edges(source->selection.edges.begin(),
                                            source->selection.edges.end());
  const std::unordered_set<solid::Id> faces(source->selection.faces.begin(),
                                            source->selection.faces.end());
  BM_ITER_MESH (v, &iter, &bm, BM_VERTS_OF_MESH)
    if (vertices.contains(source_id(reinterpret_cast<BMElem *>(v), v_offset)))
      BM_vert_select_set(&bm, v, true);
  BM_ITER_MESH (e, &iter, &bm, BM_EDGES_OF_MESH)
    if (edges.contains(source_id(reinterpret_cast<BMElem *>(e), e_offset)))
      BM_edge_select_set(&bm, e, true);
  BM_ITER_MESH (f, &iter, &bm, BM_FACES_OF_MESH)
    if (faces.contains(source_id(reinterpret_cast<BMElem *>(f), f_offset)))
      BM_face_select_set(&bm, f, true);
  BM_mesh_select_mode_flush(&bm);
  for (BMElem *element : history) {
    if (BM_elem_flag_test(element, BM_ELEM_SELECT)) {
      BM_select_history_store(&bm, element);
    }
  }
}

bool publish(Mesh &mesh, Snapshot candidate, uint64_t expected, std::string *error)
{
  if (!mesh.solid_storage || mesh.solid_storage->revision != expected || !candidate.model ||
      expected == std::numeric_limits<uint64_t>::max() ||
      (!(candidate.validation && candidate.validation->matches(candidate.model)) &&
       !solid::validate(*candidate.model, error)))
  {
    if (error && error->empty())
      *error = "Analytic source changed before the edit could be committed";
    return false;
  }
  auto &runtime = ensure(*mesh.solid_storage);
  if (candidate.model != runtime.model || candidate.display_samples)
    runtime.display_samples = candidate.display_samples;
  runtime.model = std::move(candidate.model);
  runtime.selection = solid::normalize_selection(*runtime.model, std::move(candidate.selection));
  runtime.error.clear();
  mesh.solid_storage->revision++;
  return true;
}

bool transform_mesh(Mesh &mesh, const float4x4 &matrix, std::string *error)
{
  if (mesh.runtime->edit_mesh) {
    if (error)
      *error =
          "Use component transforms in Edit Mode, or exit Edit Mode before transforming the "
          "datablock";
    return false;
  }
  auto source = read(mesh, error);
  if (!source)
    return false;
  solid::Selection all;
  for (const auto &v : source->model->vertices)
    all.vertices.push_back(v.id);
  solid::Affine affine;
  for (int i = 0; i < 3; ++i)
    affine.columns[i] = {matrix[i].x, matrix[i].y, matrix[i].z};
  affine.translation = {matrix.location().x, matrix.location().y, matrix.location().z};
  auto transformed = solid::transform_selected(*source->model, std::move(all), affine, error);
  if (!transformed)
    return false;
  Snapshot next{std::make_shared<const solid::Model>(std::move(transformed->model)),
                source->selection,
                source->revision};
  auto display = display_mesh(next, 64, error, display_uses_analytic_normals(mesh));
  if (!display || !publish(mesh, next, source->revision, error))
    return false;
  BKE_mesh_nomain_to_mesh(display.release(), &mesh, nullptr, false);
  return true;
}

bool display_is_flat(const Mesh &mesh)
{
  if (mesh.runtime->edit_mesh) {
    const BMesh &bm = *mesh.runtime->edit_mesh->bm;
    BMIter iter;
    BMFace *face;
    if (!bm.totface)
      return false;
    BM_ITER_MESH (face, &iter, const_cast<BMesh *>(&bm), BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(face, BM_ELEM_SMOOTH))
        return false;
    }
    return true;
  }
  const auto sharp = mesh.attributes().lookup<bool>("sharp_face", AttrDomain::Face);
  if (!mesh.faces_num || !sharp)
    return false;
  for (int i = 0; i < mesh.faces_num; ++i)
    if (!sharp.varray[i])
      return false;
  return true;
}

bool display_uses_analytic_normals(const Mesh &mesh)
{
  if (mesh.runtime->edit_mesh) {
    const BMesh &bm = *mesh.runtime->edit_mesh->bm;
    return !CustomData_has_layer_named(&bm.vdata, CD_PROP_BOOL, ".solid_blender_normals");
  }
  return !mesh.attributes().contains(".solid_blender_normals");
}

bool set_display_shading(Mesh &mesh, const bool smooth, std::string *error, const bool analytic)
{
  if (!analytic) {
    auto attributes = mesh.attributes_for_write();
    attributes.remove("custom_normal");
    if (!attributes.contains(".solid_blender_normals"))
      attributes.add<bool>(".solid_blender_normals", AttrDomain::Point, AttributeInitValue(true));
    mesh_smooth_set(mesh, smooth, true);
    return true;
  }
  std::shared_ptr<const solid::DisplayMesh> samples;
  if (smooth) {
    const auto source = read(mesh, error);
    if (!source)
      return false;
    samples = source->display_samples;
    if (!samples || !samples->has_analytic_normals) {
      auto generated = solid::tessellate(*source->model, 64, error);
      if (!generated)
        return false;
      samples = std::make_shared<const solid::DisplayMesh>(std::move(*generated));
      ensure(*mesh.solid_storage).display_samples = samples;
    }
    size_t corners = 0;
    for (const auto &polygon : samples->corner_normals)
      corners += polygon.size();
    if (corners != size_t(mesh.corners_num)) {
      if (error)
        *error = "Solid display topology does not match its analytic normals";
      return false;
    }
  }
  mesh.attributes_for_write().remove(".solid_blender_normals");
  auto normals = mesh.attributes_for_write().lookup_or_add_for_write_span<float3>(
      "custom_normal", AttrDomain::Corner);
  if (smooth) {
    int corner = 0;
    for (const auto &polygon : samples->corner_normals)
      for (const auto &n : polygon)
        normals.span[corner++] = float3(n.x, n.y, n.z);
  }
  else {
    const auto faces = mesh.faces();
    const auto face_normals = mesh.face_normals_true();
    for (const int i : faces.index_range())
      normals.span.slice(faces[i]).fill(face_normals[i]);
  }
  normals.finish();
  mesh_smooth_set(mesh, smooth, true);
  return true;
}

void MeshDeleter::operator()(Mesh *mesh) const
{
  if (mesh)
    BKE_id_free(nullptr, mesh);
}

OwnedMesh display_mesh(const Snapshot &source,
                       int circle_segments,
                       std::string *error,
                       const bool analytic_normals)
{
  if (!source.model)
    return {};
  const bool reuse_samples = source.display_samples && circle_segments == 64 &&
                             source.display_samples->has_analytic_normals == analytic_normals;
  const auto sampled = reuse_samples ? std::optional<solid::DisplayMesh>{} :
                                      solid::tessellate(
                                          *source.model, circle_segments, error, analytic_normals);
  const solid::DisplayMesh *display = reuse_samples ? source.display_samples.get() :
                                     sampled ? &*sampled : nullptr;
  if (!display)
    return {};
  int corners = 0;
  for (const auto &polygon : display->polygons)
    corners += int(polygon.size());
  OwnedMesh mesh(BKE_mesh_new_nomain(
      display->positions.size(), display->edges.size(), display->polygons.size(), corners));
  auto positions = mesh->vert_positions_for_write();
  for (const int i : positions.index_range()) {
    const auto &p = display->positions[i];
    const double limit = std::numeric_limits<float>::max();
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        std::abs(p.x) > limit || std::abs(p.y) > limit || std::abs(p.z) > limit)
    {
      if (error)
        *error = "Analytic display exceeds the viewport coordinate range";
      return {};
    }
    positions[i] = float3(p.x, p.y, p.z);
  }
  auto edges = mesh->edges_for_write();
  for (const int i : edges.index_range())
    edges[i] = int2(display->edges[i][0], display->edges[i][1]);
  auto offsets = mesh->face_offsets_for_write();
  auto corner_verts = mesh->corner_verts_for_write();
  int offset = 0;
  for (const int i : mesh->faces().index_range()) {
    offsets[i] = offset;
    for (int v : display->polygons[i])
      corner_verts[offset++] = v;
  }
  if (!offsets.is_empty())
    offsets.last() = offset;
  auto attributes = mesh->attributes_for_write();
  auto v_id = attributes.lookup_or_add_for_write_span<int2>(".solid_vertex_id", AttrDomain::Point);
  auto e_id = attributes.lookup_or_add_for_write_span<int2>(".solid_edge_id", AttrDomain::Edge);
  auto f_id = attributes.lookup_or_add_for_write_span<int2>(".solid_face_id", AttrDomain::Face);
  auto v_select = attributes.lookup_or_add_for_write_span<bool>(".select_vert", AttrDomain::Point);
  auto e_select = attributes.lookup_or_add_for_write_span<bool>(".select_edge", AttrDomain::Edge);
  auto f_select = attributes.lookup_or_add_for_write_span<bool>(".select_poly", AttrDomain::Face);
  auto face_dot = attributes.lookup_or_add_for_write_span<bool>(".solid_face_dot",
                                                                AttrDomain::Face);
  face_dot.span.fill(false);
  std::unordered_map<solid::Id, std::vector<int>> face_aliases;
  for (int i = 0; i < int(display->face_source.size()); ++i)
    face_aliases[display->face_source[i]].push_back(i);
  for (const auto &[id, aliases] : face_aliases)
    if (id && !aliases.empty())
      face_dot.span[aliases[aliases.size() / 2]] = true;

  /* Selection membership is queried per display element; avoid rescanning a large
   * face/edge selection for each sampled polygon and boundary. */
  const std::unordered_set<solid::Id> selected_vertices(source.selection.vertices.begin(),
                                                      source.selection.vertices.end());
  const std::unordered_set<solid::Id> selected_edges(source.selection.edges.begin(),
                                                   source.selection.edges.end());
  const std::unordered_set<solid::Id> selected_faces(source.selection.faces.begin(),
                                                   source.selection.faces.end());
  for (const int i : v_id.span.index_range()) {
    v_id.span[i] = split_id(display->vertex_source[i]);
    v_select.span[i] = display->vertex_source[i] &&
                       selected_vertices.contains(display->vertex_source[i]);
  }
  for (const int i : e_id.span.index_range()) {
    e_id.span[i] = split_id(display->edge_source[i]);
    e_select.span[i] = display->edge_source[i] && selected_edges.contains(display->edge_source[i]);
  }
  for (const int i : f_id.span.index_range()) {
    f_id.span[i] = split_id(display->face_source[i]);
    f_select.span[i] = display->face_source[i] && selected_faces.contains(display->face_source[i]);
  }
  /* Sampling vertices inherit source edge/face display selection, but retain ID zero:
   * they can never become independent source points when selection is read back. */
  for (size_t i = 0; i < display->edges.size(); ++i) {
    if (display->edge_source[i] && selected_edges.contains(display->edge_source[i])) {
      for (int v : display->edges[i])
        v_select.span[v] = true;
    }
  }
  for (size_t i = 0; i < display->polygons.size(); ++i) {
    if (display->face_source[i] && selected_faces.contains(display->face_source[i])) {
      for (int v : display->polygons[i])
        v_select.span[v] = true;
    }
  }
  v_id.finish();
  e_id.finish();
  f_id.finish();
  v_select.finish();
  e_select.finish();
  f_select.finish();
  face_dot.finish();
  mesh_calc_edges(*mesh, true, false);
  if (!analytic_normals) {
    attributes.add<bool>(".solid_blender_normals", AttrDomain::Point, AttributeInitValue(true));
    return mesh;
  }
  Vector<float3> normals;
  normals.reserve(corners);
  for (const auto &polygon : display->corner_normals) {
    for (const auto &n : polygon)
      normals.append(float3(n.x, n.y, n.z));
  }
  if (normals.size() == corners && corners) {
    /* The analytic evaluator already supplies normalized corner normals. Store those
     * vectors directly, as the native NURBS display does. Encoding them in legacy fan
     * spaces recalculates mesh normals and sharp edges on every source update. */
    auto custom_normals = attributes.lookup_or_add_for_write_span<float3>("custom_normal",
                                                                          AttrDomain::Corner);
    custom_normals.span.copy_from(normals);
    custom_normals.finish();
  }
  return mesh;
}

}  // namespace blender::bke::solid_host
