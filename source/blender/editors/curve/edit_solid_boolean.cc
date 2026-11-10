/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "ED_solid.hh"
#include "SOL_boolean.hh"
#include "SOL_boolean_replay.hh"
#include "curve_intern.hh"
#include "solid_cylinder_boolean.hh"

#include "GEO_mesh_boolean.hh"
#include "GEO_mesh_triangulate.hh"

#include "BKE_attribute_filter.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_report.hh"
#include "BKE_solid_host.hh"

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "bmesh.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace blender::ed::solid_editor {
namespace {

using solid::Id;
using FaceSet = std::unordered_set<Id>;
using OwnedMesh = bke::solid_host::OwnedMesh;

using TraceClock = std::chrono::steady_clock;

bool boolean_trace_enabled()
{
  static const bool enabled = [] {
    const char *value = std::getenv("BLENDER_SOLID_BOOLEAN_TRACE");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

double elapsed_milliseconds(const TraceClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(TraceClock::now() - start).count();
}

const char *operation_name(const solid::BooleanOperation operation)
{
  switch (operation) {
    case solid::BooleanOperation::Union:
      return "union";
    case solid::BooleanOperation::Subtract:
      return "subtract";
    case solid::BooleanOperation::Intersect:
      return "intersect";
  }
  return "unknown";
}

void trace_ids(const char *name, const std::span<const Id> ids)
{
  constexpr size_t limit = 32;
  std::fprintf(stderr, ",\"%s\":[", name);
  for (size_t i = 0; i < std::min(ids.size(), limit); ++i) {
    std::fprintf(stderr, "%s%llu", i ? "," : "", static_cast<unsigned long long>(ids[i]));
  }
  std::fprintf(stderr, "],\"%s_truncated\":%s", name, ids.size() > limit ? "true" : "false");
}

void trace_kernel_result(const solid::BooleanResult &result,
                         const solid::BooleanOperation operation,
                         const std::span<const Id> first,
                         const std::span<const Id> second)
{
  if (!boolean_trace_enabled()) {
    return;
  }
  std::fprintf(stderr,
               "{\"kind\":\"solid_boolean_kernel\",\"operation\":\"%s\",\"status\":\"%s\","
               "\"path\":\"%s\",\"first_face_count\":%zu,\"second_face_count\":%zu,"
               "\"result_face_count\":%zu",
               operation_name(operation),
               solid::boolean_status_name(result.status),
               solid::boolean_path_name(result.diagnostics.path),
               result.diagnostics.first_face_count,
               result.diagnostics.second_face_count,
               result.diagnostics.result_face_count);
  trace_ids("first_face_ids", first);
  trace_ids("second_face_ids", second);
  constexpr size_t timing_limit = 16;
  std::fprintf(stderr, ",\"timings_ms\":[");
  const size_t timing_count = std::min(result.diagnostics.timings.size(), timing_limit);
  for (size_t i = 0; i < timing_count; ++i) {
    const solid::BooleanTiming &timing = result.diagnostics.timings[i];
    std::fprintf(stderr,
                 "%s{\"stage\":\"%s\",\"milliseconds\":%.6f}",
                 i ? "," : "",
                 solid::boolean_stage_name(timing.stage),
                 timing.milliseconds);
  }
  std::fprintf(stderr,
               "],\"timings_truncated\":%s}\n",
               result.diagnostics.timings.size() > timing_limit ? "true" : "false");
}

void trace_operator_result(const char *path,
                           const char *status,
                           const double total_ms,
                           const double publication_ms,
                           const Id active_element,
                           const std::span<const Id> active_ids,
                           const std::span<const Id> cutter_ids,
                           const int mode,
                           const int solver,
                           const bool swap,
                           const bool invert,
                           const bool keep,
                           const bool intersect_each,
                           const bool merge,
                           const bool limited_dissolve,
                           const bool dissolve_per_iteration)
{
  if (!boolean_trace_enabled()) {
    return;
  }
  std::fprintf(stderr,
               "{\"kind\":\"solid_boolean_operator\",\"path\":\"%s\",\"status\":\"%s\","
               "\"operation_mode\":%d,\"solver\":%d,\"active_element_id\":%llu,"
               "\"swap_subtract\":%s,\"invert_cutter\":%s,\"keep_cutter\":%s,"
               "\"intersect_with_cutter\":%s,\"merge_results\":%s,\"limited_dissolve\":%s,"
               "\"dissolve_per_iteration\":%s,\"total_ms\":%.6f,\"publication_ms\":%.6f",
               path,
               status,
               mode,
               solver,
               static_cast<unsigned long long>(active_element),
               swap ? "true" : "false",
               invert ? "true" : "false",
               keep ? "true" : "false",
               intersect_each ? "true" : "false",
               merge ? "true" : "false",
               limited_dissolve ? "true" : "false",
               dissolve_per_iteration ? "true" : "false",
               total_ms,
               publication_ms);
  trace_ids("selected_face_ids", active_ids);
  trace_ids("cutter_face_ids", cutter_ids);
  std::fprintf(stderr, "}\n");
}

/** A separate opt-in capture saves the exact kernel request in native coordinates. Diagnostic
 * I/O never changes the Boolean outcome. Analytic editor-only shortcuts do not use this format. */
void capture_boolean_failure(const solid::Model &source,
                             const std::vector<Id> &first,
                             const std::vector<Id> &second,
                             const solid::BooleanOperation operation)
{
  const char *directory = std::getenv("BLENDER_SOLID_BOOLEAN_CAPTURE_DIR");
  if (!directory || directory[0] == '\0') {
    return;
  }
  static std::atomic<uint64_t> serial{0};
  const uint64_t capture_index = serial.fetch_add(1);
  constexpr uint64_t capture_limit = 16;
  if (capture_index >= capture_limit) {
    if (capture_index == capture_limit) {
      std::fprintf(stderr, "{\"kind\":\"solid_boolean_capture\",\"status\":\"limit_reached\"}\n");
    }
    return;
  }
  try {
    const auto bytes = solid::encode_boolean_replay({source, first, second, operation});
    if (bytes.empty()) {
      std::fprintf(stderr, "{\"kind\":\"solid_boolean_capture\",\"status\":\"unencodable\"}\n");
      return;
    }
    const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    const std::string filename = "solid-boolean-" + std::to_string(timestamp) + "-" +
                                 std::to_string(capture_index) + ".solidbool";
    const std::filesystem::path folder(directory);
    std::filesystem::create_directories(folder);
    std::ofstream file(folder / filename, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    file.close();
    std::fprintf(stderr,
                 "{\"kind\":\"solid_boolean_capture\",\"status\":\"written\","
                 "\"file\":\"%s\",\"bytes\":%zu}\n",
                 filename.c_str(),
                 bytes.size());
  }
  catch (const std::exception &) {
    std::fprintf(stderr, "{\"kind\":\"solid_boolean_capture\",\"status\":\"write_failed\"}\n");
  }
}

OwnedMesh triangulate_nonplanar(OwnedMesh mesh, bool float_tolerance = false);

struct Component {
  std::vector<Id> faces;
};

std::vector<Component> components_for_faces(const solid::Model &model)
{
  std::unordered_map<Id, std::vector<Id>> edge_faces;
  for (const solid::Face &face : model.faces) {
    for (const solid::Coedge &use : face.outer) {
      edge_faces[use.edge].push_back(face.id);
    }
    for (const auto &hole : face.holes) {
      for (const solid::Coedge &use : hole) {
        edge_faces[use.edge].push_back(face.id);
      }
    }
  }
  std::unordered_map<Id, std::vector<Id>> neighbors;
  for (const auto &[edge_id, uses] : edge_faces) {
    UNUSED_VARS(edge_id);
    if (uses.size() == 2) {
      neighbors[uses[0]].push_back(uses[1]);
      neighbors[uses[1]].push_back(uses[0]);
    }
  }
  std::vector<Component> result;
  FaceSet visited;
  for (const solid::Face &face : model.faces) {
    if (!visited.insert(face.id).second) {
      continue;
    }
    Component component;
    std::vector<Id> stack{face.id};
    while (!stack.empty()) {
      const Id current = stack.back();
      stack.pop_back();
      component.faces.push_back(current);
      for (const Id neighbor : neighbors[current]) {
        if (visited.insert(neighbor).second) {
          stack.push_back(neighbor);
        }
      }
    }
    result.push_back(std::move(component));
  }
  return result;
}

OwnedMesh mesh_from_display(const solid::DisplayMesh &display,
                            const FaceSet &source_faces,
                            std::string *error)
{
  if (display.face_source.size() != display.polygons.size()) {
    if (error) {
      *error = "Solid display samples are missing source-face identities";
    }
    return {};
  }
  std::vector<int> included;
  int corners_num = 0;
  for (const int i : IndexRange(display.polygons.size())) {
    if (source_faces.contains(display.face_source[i]) && display.polygons[i].size() >= 3) {
      included.push_back(i);
      corners_num += display.polygons[i].size();
    }
  }
  if (included.empty()) {
    if (error) {
      *error = "The selected Solid body has no display faces";
    }
    return {};
  }

  std::vector<int> vertex_map(display.positions.size(), -1), source_vertices;
  for (const int polygon_index : included) {
    for (const int vertex : display.polygons[polygon_index]) {
      if (vertex < 0 || vertex >= int(display.positions.size())) {
        if (error) {
          *error = "Solid display mesh has an invalid vertex index";
        }
        return {};
      }
      if (vertex_map[vertex] == -1) {
        vertex_map[vertex] = source_vertices.size();
        source_vertices.push_back(vertex);
      }
    }
  }

  OwnedMesh mesh(BKE_mesh_new_nomain(source_vertices.size(), 0, included.size(), corners_num));
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  for (const int i : IndexRange(source_vertices.size())) {
    const solid::Vec3 p = display.positions[source_vertices[i]];
    positions[i] = float3(float(p.x), float(p.y), float(p.z));
  }
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  int corner = 0;
  for (const int face_index : IndexRange(included.size())) {
    face_offsets[face_index] = corner;
    for (const int vertex : display.polygons[included[face_index]]) {
      corner_verts[corner++] = vertex_map[vertex];
    }
  }
  face_offsets.last() = corner;
  bke::mesh_calc_edges(*mesh, false, false);

  /* Keep planar polygons intact; curved display patches may need planar triangles. */
  return triangulate_nonplanar(std::move(mesh));
}

bool mesh_face_is_planar(const Mesh &mesh, const int face_index, const bool float_tolerance)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const OffsetIndices<int> faces = mesh.faces();
  const IndexRange loop = faces[face_index];
  if (loop.size() < 3) {
    return false;
  }
  const float3 origin = positions[corner_verts[loop.start()]];
  const solid::Vec3 origin_d{double(origin.x), double(origin.y), double(origin.z)};
  solid::Vec3 normal{};
  double scale = 1.0;
  for (const int i : loop.index_range()) {
    const float3 a = positions[corner_verts[loop.start() + i]];
    const float3 b = positions[corner_verts[loop.start() + (i + 1) % loop.size()]];
    const solid::Vec3 a_d{double(a.x), double(a.y), double(a.z)};
    const solid::Vec3 b_d{double(b.x), double(b.y), double(b.z)};
    normal = normal + solid::cross(a_d - origin_d, b_d - origin_d);
    scale = std::max(scale, double(math::distance(a, origin)));
  }
  const double normal_length = solid::length(normal);
  if (!(normal_length > 1e-12)) {
    return false;
  }
  normal = normal / normal_length;
  for (const int i : loop.index_range()) {
    const float3 point = positions[corner_verts[loop.start() + i]];
    const solid::Vec3 point_d{double(point.x), double(point.y), double(point.z)};
    const solid::Vec3 delta = point_d - origin_d;
    const double tolerance =
        (float_tolerance ? 8 * double(std::numeric_limits<float>::epsilon()) :
                           256 * std::numeric_limits<double>::epsilon()) *
        std::max({1.0, double(math::length(origin)), double(math::length(point)), scale});
    if (std::abs(solid::dot(delta, normal)) > tolerance) {
      return false;
    }
  }
  return true;
}

OwnedMesh triangulate_nonplanar(OwnedMesh mesh, const bool float_tolerance)
{
  if (!mesh || mesh->faces_num == 0) {
    return mesh;
  }
  /* Preserve polygonal planar regions. Only split individual faces which cannot be represented
   * by an analytic plane after the solver has rounded their coordinates to float. */
  IndexMaskMemory memory;
  const IndexMask nonplanar = IndexMask::from_predicate(
      mesh->faces().index_range(), memory, [&](const int face) {
        return !mesh_face_is_planar(*mesh, face, float_tolerance);
      });
  const std::optional<Mesh *> triangulated = geometry::mesh_triangulate(
      *mesh,
      nonplanar,
      geometry::TriangulateNGonMode::Beauty,
      geometry::TriangulateQuadMode::Beauty,
      bke::AttributeFilter::default_filter());
  if (triangulated) {
    mesh.reset(*triangulated);
  }
  return mesh;
}

OwnedMesh limited_dissolve(OwnedMesh mesh, std::string *error)
{
  if (!mesh || mesh->faces_num == 0) {
    return mesh;
  }
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh.get());
  BMeshCreateParams create_params{};
  create_params.use_toolflags = true;
  BMesh *bm = BM_mesh_create(&allocsize, &create_params);
  BMeshFromMeshParams from_params{};
  from_params.calc_face_normal = true;
  from_params.calc_vert_normal = true;
  BM_mesh_bm_from_me(bm, mesh.get(), &from_params);
  BM_mesh_elem_hflag_enable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  /* The dissolve heap compares against -cosf(angle). At 1e-4f that rounds to -1 and even
   * exactly coplanar edges fail its strict comparison. Keep the threshold representable. */
  const bool success = BMO_op_callf(
      bm,
      BMO_FLAG_DEFAULTS,
      "dissolve_limit edges=%he verts=%hv angle_limit=%f use_dissolve_boundaries=%b delimit=%i",
      BM_ELEM_SELECT,
      BM_ELEM_SELECT,
      1e-3f,
      false,
      0);
  if (!success) {
    BM_mesh_free(bm);
    if (error) {
      *error = "Limited dissolve failed on the Boolean result";
    }
    return {};
  }
  BMeshToMeshParams to_params{};
  to_params.calc_object_remap = false;
  Mesh *result = BKE_mesh_from_bmesh_nomain(bm, &to_params, mesh.get());
  BM_mesh_free(bm);
  OwnedMesh dissolved(result);
  return triangulate_nonplanar(std::move(dissolved), true);
}

OwnedMesh run_boolean(const Mesh &target,
                      const Mesh &cutter,
                      const geometry::boolean::Operation operation,
                      const geometry::boolean::Solver solver,
                      const bool dissolve,
                      std::string *error)
{
  geometry::boolean::BooleanOpParameters params;
  params.boolean_mode = operation;
  params.no_self_intersections = true;
  params.no_nested_components = false;
  params.watertight = true;
  geometry::boolean::BooleanError boolean_error;
  const std::array<const Mesh *, 2> meshes = {&target, &cutter};
  const std::array<float4x4, 2> transforms = {float4x4::identity(), float4x4::identity()};
  const std::array<Array<short>, 2> material_remaps = {};
  Mesh *result = geometry::boolean::mesh_boolean(
      meshes, transforms, material_remaps, params, solver, nullptr, &boolean_error);
  if (boolean_error.type != geometry::boolean::BooleanErrorType::NoError) {
    if (error) {
      switch (boolean_error.type) {
        case geometry::boolean::BooleanErrorType::NonManifold:
          *error = "Selected Boolean bodies are not closed manifold solids";
          break;
        case geometry::boolean::BooleanErrorType::ResultTooBig:
          *error = "Boolean result exceeds the selected solver's size limit";
          break;
        case geometry::boolean::BooleanErrorType::SolverNotAvailable:
          *error = "Selected Boolean solver is unavailable in this build";
          break;
        case geometry::boolean::BooleanErrorType::UnknownError:
          *error = "Boolean solver failed to produce a result";
          break;
        case geometry::boolean::BooleanErrorType::NoError:
          break;
      }
    }
    if (result) {
      OwnedMesh cleanup(result);
    }
    return {};
  }
  OwnedMesh output(result);
  if (!output || output->faces_num == 0) {
    return output;
  }
  if (dissolve) {
    output = limited_dissolve(std::move(output), error);
    if (!output) {
      return {};
    }
  }
  return dissolve ? std::move(output) : triangulate_nonplanar(std::move(output));
}

struct EdgeKey {
  int a, b;
  friend bool operator==(const EdgeKey &, const EdgeKey &) = default;
};

struct EdgeKeyHash {
  size_t operator()(const EdgeKey &key) const
  {
    return (uint64_t(uint32_t(key.a)) << 32) | uint32_t(key.b);
  }
};

Id next_id(solid::Model &model)
{
  return model.next_id++;
}

bool append_mesh_result(solid::Model &model,
                        const Mesh &mesh,
                        const solid::Model &source,
                        std::string *error)
{
  /* Mesh solvers round to float. Reconstruct points on the original analytic planes in double
   * precision before creating source polygons; float rounding must not split a flat source face.
   */
  std::vector<solid::Vec3> positions;
  positions.reserve(mesh.verts_num);
  for (const float3 &point : mesh.vert_positions()) {
    const solid::Vec3 original{double(point.x), double(point.y), double(point.z)};
    solid::Vec3 corrected = original;
    std::vector<solid::Vec3> basis;
    for (const solid::Surface &plane : source.surfaces) {
      if (plane.kind != solid::SurfaceKind::Plane) {
        continue;
      }
      solid::Vec3 n = solid::cross(plane.u, plane.v);
      n = n / solid::length(n);
      const double tolerance = 8 * double(std::numeric_limits<float>::epsilon()) *
                               std::max(
                                   {1.0, solid::length(original), solid::length(plane.origin)});
      if (std::abs(solid::dot(original - plane.origin, n)) > tolerance) {
        continue;
      }
      solid::Vec3 direction = n;
      for (const solid::Vec3 &axis : basis) {
        direction = direction - axis * solid::dot(n, axis);
      }
      const double size = solid::length(direction);
      if (size < 1e-8) {
        continue;
      }
      direction = direction / size;
      corrected = corrected +
                  direction * (solid::dot(plane.origin - corrected, n) / solid::dot(direction, n));
      basis.push_back(direction);
      if (basis.size() == 3) {
        break;
      }
    }
    positions.push_back(corrected);
  }
  const Span<int> corner_verts = mesh.corner_verts();
  const OffsetIndices<int> faces = mesh.faces();
  std::vector<Id> vertex_ids(positions.size(), 0);
  std::unordered_map<EdgeKey, Id, EdgeKeyHash> edge_ids;
  for (const int face_index : faces.index_range()) {
    const IndexRange loop = faces[face_index];
    if (loop.size() < 3) {
      continue;
    }
    std::vector<int> ring(loop.size());
    for (const int i : loop.index_range()) {
      ring[i] = corner_verts[loop.start() + i];
      if (ring[i] < 0 || ring[i] >= int(positions.size())) {
        if (error) {
          *error = "Boolean solver returned an invalid face vertex";
        }
        return false;
      }
    }
    const solid::Vec3 p0 = positions[ring[0]];
    const solid::Vec3 origin{double(p0.x), double(p0.y), double(p0.z)};
    solid::Vec3 normal{};
    for (int i = 0; i < int(ring.size()); ++i) {
      const solid::Vec3 a = positions[ring[i]], b = positions[ring[(i + 1) % ring.size()]];
      const solid::Vec3 a_d{double(a.x), double(a.y), double(a.z)};
      const solid::Vec3 b_d{double(b.x), double(b.y), double(b.z)};
      normal = normal + solid::cross(a_d - origin, b_d - origin);
    }
    const double normal_size = solid::length(normal);
    if (!(normal_size > 1e-12)) {
      if (error) {
        *error = "Boolean result contains a degenerate face";
      }
      return false;
    }
    normal = normal / normal_size;
    solid::Vec3 u{};
    for (int i = 1; i < int(ring.size()); ++i) {
      const solid::Vec3 p = positions[ring[i]];
      u = solid::Vec3{double(p.x), double(p.y), double(p.z)} - origin;
      if (solid::length(u) > 1e-12) {
        break;
      }
    }
    if (!(solid::length(u) > 1e-12)) {
      if (error) {
        *error = "Boolean result contains a zero-length face boundary";
      }
      return false;
    }
    solid::Surface surface;
    surface.id = next_id(model);
    surface.kind = solid::SurfaceKind::Plane;
    surface.origin = origin;
    surface.u = u;
    surface.v = solid::cross(normal, u);
    model.surfaces.push_back(surface);

    solid::Face face;
    face.id = next_id(model);
    face.surface = surface.id;
    for (int i = 0; i < int(ring.size()); ++i) {
      const int a = ring[i], b = ring[(i + 1) % ring.size()];
      for (const int index : {a, b}) {
        if (vertex_ids[index] == 0) {
          vertex_ids[index] = next_id(model);
          const solid::Vec3 p = positions[index];
          model.vertices.push_back({vertex_ids[index], {p.x, p.y, p.z}});
        }
      }
      const EdgeKey key{std::min(a, b), std::max(a, b)};
      auto edge_it = edge_ids.find(key);
      if (edge_it == edge_ids.end()) {
        const solid::Vec3 a_pos = positions[key.a], b_pos = positions[key.b];
        solid::Curve curve;
        curve.id = next_id(model);
        curve.kind = solid::CurveKind::Line;
        curve.origin = {double(a_pos.x), double(a_pos.y), double(a_pos.z)};
        curve.u = {double(b_pos.x) - double(a_pos.x),
                   double(b_pos.y) - double(a_pos.y),
                   double(b_pos.z) - double(a_pos.z)};
        model.curves.push_back(curve);
        const Id edge_id = next_id(model);
        model.edges.push_back({edge_id, curve.id, {vertex_ids[key.a], vertex_ids[key.b]}, 0, 1});
        edge_it = edge_ids.emplace(key, edge_id).first;
      }
      face.outer.push_back({edge_it->second, a > b});
    }
    model.faces.push_back(std::move(face));
  }
  return true;
}

bool append_source_components(solid::Model &destination,
                              const solid::Model &source,
                              const FaceSet &face_ids,
                              const FaceSet &consumed_faces)
{
  FaceSet used_edges, used_vertices, used_curves, used_surfaces;
  FaceSet consumed_edges, consumed_vertices, consumed_curves, consumed_surfaces;
  for (const solid::Face &face : source.faces) {
    auto add_loop = [&](const std::vector<solid::Coedge> &loop) {
      for (const solid::Coedge &use : loop) {
        (consumed_faces.contains(face.id) ? consumed_edges : used_edges).insert(use.edge);
      }
    };
    add_loop(face.outer);
    for (const auto &hole : face.holes) {
      add_loop(hole);
    }
    (consumed_faces.contains(face.id) ? consumed_surfaces : used_surfaces).insert(face.surface);
  }
  for (const solid::Edge &edge : source.edges) {
    if (consumed_edges.contains(edge.id)) {
      consumed_vertices.insert(edge.vertices.begin(), edge.vertices.end());
      consumed_curves.insert(edge.curve);
    }
    else {
      used_edges.insert(edge.id);
      used_vertices.insert(edge.vertices.begin(), edge.vertices.end());
      used_curves.insert(edge.curve);
    }
  }
  for (const solid::Surface &surface : source.surfaces) {
    if (consumed_surfaces.contains(surface.id)) {
      if (surface.curve) {
        consumed_curves.insert(surface.curve);
      }
      if (surface.other_curve) {
        consumed_curves.insert(surface.other_curve);
      }
    }
    else {
      used_surfaces.insert(surface.id);
      if (surface.curve) {
        used_curves.insert(surface.curve);
      }
      if (surface.other_curve) {
        used_curves.insert(surface.other_curve);
      }
    }
  }
  for (const solid::Vertex &vertex : source.vertices) {
    if (!consumed_vertices.contains(vertex.id)) {
      used_vertices.insert(vertex.id);
    }
  }
  for (const solid::Curve &curve : source.curves) {
    if (!consumed_curves.contains(curve.id)) {
      used_curves.insert(curve.id);
    }
  }

  std::unordered_map<Id, Id> remap;
  for (const solid::Vertex &vertex : source.vertices) {
    if (used_vertices.contains(vertex.id)) {
      const Id id = next_id(destination);
      remap[vertex.id] = id;
      destination.vertices.push_back({id, vertex.position});
    }
  }
  for (const solid::Curve &curve : source.curves) {
    if (used_curves.contains(curve.id)) {
      const Id id = next_id(destination);
      remap[curve.id] = id;
      solid::Curve copy = curve;
      copy.id = id;
      destination.curves.push_back(copy);
    }
  }
  for (const solid::Surface &surface : source.surfaces) {
    if (!used_surfaces.contains(surface.id)) {
      continue;
    }
    const Id id = next_id(destination);
    remap[surface.id] = id;
    solid::Surface copy = surface;
    copy.id = id;
    if (copy.curve) {
      copy.curve = remap.at(copy.curve);
    }
    if (copy.other_curve) {
      copy.other_curve = remap.at(copy.other_curve);
    }
    destination.surfaces.push_back(copy);
  }
  for (const solid::Edge &edge : source.edges) {
    if (used_edges.contains(edge.id)) {
      const Id id = next_id(destination);
      remap[edge.id] = id;
      solid::Edge copy = edge;
      copy.id = id;
      copy.curve = remap.at(copy.curve);
      copy.vertices = {remap.at(copy.vertices[0]), remap.at(copy.vertices[1])};
      destination.edges.push_back(copy);
    }
  }
  for (const solid::Face &face : source.faces) {
    if (face_ids.contains(face.id)) {
      remap[face.id] = next_id(destination);
    }
  }
  for (const solid::Face &face : source.faces) {
    if (!face_ids.contains(face.id)) {
      continue;
    }
    solid::Face copy = face;
    copy.id = remap.at(face.id);
    copy.surface = remap.at(copy.surface);
    copy.group = face.group && remap.contains(face.group) ? remap.at(face.group) : 0;
    auto remap_loop = [&](std::vector<solid::Coedge> &loop) {
      for (solid::Coedge &use : loop) {
        use.edge = remap.at(use.edge);
      }
    };
    remap_loop(copy.outer);
    for (auto &hole : copy.holes) {
      remap_loop(hole);
    }
    destination.faces.push_back(std::move(copy));
  }
  return true;
}

bool validate_result(const solid::Model &model, const FaceSet &boolean_faces, std::string *error)
{
  if (!solid::validate(model, error)) {
    return false;
  }
  for (const Component &component : components_for_faces(model)) {
    const bool is_boolean_result = std::any_of(
        component.faces.begin(), component.faces.end(), [&](const Id id) {
          return boolean_faces.contains(id);
        });
    if (is_boolean_result && !component.faces.empty() &&
        !solid::validate_body(model, component.faces.front(), error))
    {
      return false;
    }
  }
  return true;
}

std::optional<solid::EditResult> native_boolean(const solid::Model &model,
                                                const std::vector<Component> &components,
                                                const std::vector<int> &selected,
                                                const int cutter_index,
                                                const solid::Selection &source_selection,
                                                wmOperator *op,
                                                std::string *error)
{
  using Operation = solid::BooleanOperation;
  auto face_ids = [](const solid::Model &m) {
    FaceSet result;
    for (const solid::Face &face : m.faces)
      result.insert(face.id);
    return result;
  };
  auto body = [&](int index) {
    solid::Model result;
    FaceSet keep(components[index].faces.begin(), components[index].faces.end()), discard;
    for (const solid::Face &face : model.faces)
      if (!keep.contains(face.id))
        discard.insert(face.id);
    append_source_components(result, model, keep, discard);
    return result;
  };
  auto apply = [&](const solid::Model &a, const solid::Model &b, Operation operation) {
    solid::Model combined;
    append_source_components(combined, a, face_ids(a), {});
    std::vector<Id> a_faces, b_faces;
    for (const solid::Face &face : combined.faces)
      a_faces.push_back(face.id);
    const size_t first = combined.faces.size();
    append_source_components(combined, b, face_ids(b), {});
    for (size_t i = first; i < combined.faces.size(); ++i)
      b_faces.push_back(combined.faces[i].id);
    solid::BooleanResult boolean_result = solid::boolean_bodies_result(
        combined, a_faces, b_faces, operation);
    trace_kernel_result(boolean_result, operation, a_faces, b_faces);
    if (!boolean_result.succeeded() || !boolean_result.model) {
      capture_boolean_failure(combined, a_faces, b_faces, operation);
      if (error) {
        *error = boolean_result.message.empty() ? "Native Solid Boolean failed" :
                                                  boolean_result.message;
      }
      return std::optional<solid::Model>();
    }
    return std::move(boolean_result.model);
  };
  const int mode = RNA_enum_get(op->ptr, "operation_mode");
  const bool swap = RNA_boolean_get(op->ptr, "swap_subtract");
  const bool intersect_each = RNA_boolean_get(op->ptr, "intersect_with_cutter");
  const bool keep = RNA_boolean_get(op->ptr, "keep_cutter") && mode != 0 && !(mode == 1 && swap) &&
                    !(mode == 2 && !intersect_each);
  const solid::Model cutter = body(cutter_index);
  std::vector<solid::Model> outputs;
  if (mode == 0 || (mode == 1 && swap) || (mode == 2 && !intersect_each)) {
    solid::Model result = cutter;
    const Operation operation = mode == 0 ? Operation::Union :
                                mode == 2 || RNA_boolean_get(op->ptr, "invert_cutter") ?
                                            Operation::Intersect :
                                            Operation::Subtract;
    for (const int index : selected) {
      if (index == cutter_index)
        continue;
      auto next = apply(result, body(index), operation);
      if (!next)
        return {};
      result = std::move(*next);
    }
    outputs.push_back(std::move(result));
  }
  else {
    for (const int index : selected) {
      if (index == cutter_index)
        continue;
      const solid::Model target = body(index);
      const Operation operation = mode == 2 || (mode == 1 &&
                                                RNA_boolean_get(op->ptr, "invert_cutter")) ?
                                      Operation::Intersect :
                                      Operation::Subtract;
      auto result = apply(target, cutter, operation);
      if (!result)
        return {};
      outputs.push_back(std::move(*result));
      if (mode == 3) {
        auto inside = apply(target, cutter, Operation::Intersect);
        if (!inside)
          return {};
        outputs.push_back(std::move(*inside));
      }
    }
    if (mode != 3 && RNA_boolean_get(op->ptr, "merge_results") && outputs.size() > 1) {
      solid::Model merged = std::move(outputs.front());
      for (size_t i = 1; i < outputs.size(); ++i) {
        auto result = apply(merged, outputs[i], Operation::Union);
        if (!result)
          return {};
        merged = std::move(*result);
      }
      outputs.clear();
      outputs.push_back(std::move(merged));
    }
  }
  FaceSet preserve, discard;
  for (int index = 0; index < int(components.size()); ++index) {
    const bool consumed = std::find(selected.begin(), selected.end(), index) != selected.end();
    for (const Id id : components[index].faces)
      (consumed && !(keep && index == cutter_index) ? discard : preserve).insert(id);
  }
  solid::EditResult result;
  if (!append_source_components(result.model, model, preserve, discard))
    return {};
  const size_t first_vertex = result.model.vertices.size(), first_edge = result.model.edges.size(),
               first_face = result.model.faces.size();
  for (const solid::Model &output : outputs)
    if (!append_source_components(result.model, output, face_ids(output), {}))
      return {};
  FaceSet generated;
  for (size_t i = first_face; i < result.model.faces.size(); ++i)
    generated.insert(result.model.faces[i].id);
  if (!validate_result(result.model, generated, error))
    return {};
  result.selection.mode = source_selection.mode;
  for (size_t i = first_vertex; i < result.model.vertices.size(); ++i)
    result.selection.vertices.push_back(result.model.vertices[i].id);
  for (size_t i = first_edge; i < result.model.edges.size(); ++i)
    result.selection.edges.push_back(result.model.edges[i].id);
  for (size_t i = first_face; i < result.model.faces.size(); ++i)
    result.selection.faces.push_back(result.model.faces[i].id);
  const auto &active = result.selection.mode == 4 ? result.selection.faces :
                       result.selection.mode == 2 ? result.selection.edges :
                                                    result.selection.vertices;
  if (!active.empty())
    result.selection.active = active.front();
  return result;
}

geometry::boolean::Solver solver_from_rna(const int value)
{
  switch (value) {
    case 0:
      return geometry::boolean::Solver::Float;
    case 1:
      return geometry::boolean::Solver::MeshArr;
    case 2:
      return geometry::boolean::Solver::Manifold;
  }
  return geometry::boolean::Solver::MeshArr;
}

}  // namespace

static wmOperatorStatus solid_boolean_exec(bContext *C, wmOperator *op)
{
  const TraceClock::time_point operator_start = TraceClock::now();
  std::string error;
  auto session = Session::begin(C, &error);
  if (!session) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  const bke::solid_host::Snapshot &source = session->source();
  const solid::Model &model = *source.model;
  const FaceSet selected_faces(source.selection.faces.begin(), source.selection.faces.end());
  const FaceSet selected_edges(source.selection.edges.begin(), source.selection.edges.end());
  const FaceSet selected_vertices(source.selection.vertices.begin(),
                                  source.selection.vertices.end());
  const std::vector<Component> components = components_for_faces(model);

  std::vector<int> selected_components;
  FaceSet consumed_faces;
  int cutter_component = -1;
  for (const int i : IndexRange(components.size())) {
    const Component &component = components[i];
    FaceSet component_edges, component_vertices;
    for (Id face_id : component.faces) {
      const solid::Face &f = *solid::face(model, face_id);
      auto add_loop = [&](const std::vector<solid::Coedge> &loop) {
        for (const solid::Coedge &use : loop) {
          component_edges.insert(use.edge);
          const solid::Edge *edge = solid::edge(model, use.edge);
          component_vertices.insert(edge->vertices.begin(), edge->vertices.end());
        }
      };
      add_loop(f.outer);
      for (const auto &hole : f.holes)
        add_loop(hole);
    }
    const bool selected = std::any_of(component.faces.begin(),
                                      component.faces.end(),
                                      [&](Id id) { return selected_faces.contains(id); }) ||
                          std::any_of(component_edges.begin(),
                                      component_edges.end(),
                                      [&](Id id) { return selected_edges.contains(id); }) ||
                          std::any_of(component_vertices.begin(),
                                      component_vertices.end(),
                                      [&](Id id) { return selected_vertices.contains(id); });
    if (!selected) {
      continue;
    }
    /* Any selected element identifies the complete disconnected body as an operand. This follows
     * the active Face/Edge/Vertex selection used by the mesh Boolean workflow. */
    if (!solid::validate_body(model, component.faces.front(), &error)) {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
      return OPERATOR_CANCELLED;
    }
    selected_components.push_back(i);
    consumed_faces.insert(component.faces.begin(), component.faces.end());
    if (cutter_component < 0 &&
        (std::find(component.faces.begin(), component.faces.end(), source.selection.active) !=
             component.faces.end() ||
         component_edges.contains(source.selection.active) ||
         component_vertices.contains(source.selection.active)))
    {
      cutter_component = i;
    }
  }
  if (selected_components.size() < 2) {
    BKE_report(op->reports, RPT_ERROR, "Select faces from at least two disconnected Solid bodies");
    return OPERATOR_CANCELLED;
  }
  if (cutter_component < 0) {
    cutter_component = selected_components.front();
  }

  const int trace_mode = RNA_enum_get(op->ptr, "operation_mode");
  const int trace_solver = RNA_enum_get(op->ptr, "solver");
  const bool trace_swap = RNA_boolean_get(op->ptr, "swap_subtract");
  const bool trace_invert = RNA_boolean_get(op->ptr, "invert_cutter");
  const bool trace_keep = RNA_boolean_get(op->ptr, "keep_cutter");
  const bool trace_intersect_each = RNA_boolean_get(op->ptr, "intersect_with_cutter");
  const bool trace_merge = RNA_boolean_get(op->ptr, "merge_results");
  const bool trace_limited_dissolve = RNA_boolean_get(op->ptr, "limited_dissolve");
  const bool trace_dissolve_per_iteration = RNA_boolean_get(op->ptr, "dissolve_per_iteration");
  std::vector<Id> trace_selected_ids;
  if (boolean_trace_enabled()) {
    for (const int index : selected_components) {
      trace_selected_ids.insert(
          trace_selected_ids.end(), components[index].faces.begin(), components[index].faces.end());
    }
  }
  const std::span<const Id> cutter_ids(components[cutter_component].faces);
  auto emit_operator_trace = [&](const char *path,
                                 const char *status,
                                 const TraceClock::time_point start,
                                 const double publication_ms) {
    if (!boolean_trace_enabled()) {
      return;
    }
    trace_operator_result(path,
                          status,
                          elapsed_milliseconds(start),
                          publication_ms,
                          source.selection.active,
                          trace_selected_ids,
                          cutter_ids,
                          trace_mode,
                          trace_solver,
                          trace_swap,
                          trace_invert,
                          trace_keep,
                          trace_intersect_each,
                          trace_merge,
                          trace_limited_dissolve,
                          trace_dissolve_per_iteration);
  };

  const int boolean_solver = RNA_enum_get(op->ptr, "solver");
  const bool planar_native =
      boolean_solver == 3 &&
      std::all_of(consumed_faces.begin(), consumed_faces.end(), [&](const Id id) {
        const solid::Face *f = solid::face(model, id);
        if (solid::surface(model, f->surface)->kind != solid::SurfaceKind::Plane)
          return false;
        auto linear = [&](const auto &loop) {
          return std::all_of(loop.begin(), loop.end(), [&](const auto &use) {
            return solid::curve(model, solid::edge(model, use.edge)->curve)->kind ==
                   solid::CurveKind::Line;
          });
        };
        return linear(f->outer) && std::all_of(f->holes.begin(), f->holes.end(), linear);
      });
  if (planar_native) {
    auto result = native_boolean(
        model, components, selected_components, cutter_component, source.selection, op, &error);
    if (!result) {
      emit_operator_trace("native_planar", "failed", operator_start, 0.0);
      BKE_report(
          op->reports, RPT_ERROR, error.empty() ? "Native Solid Boolean failed" : error.c_str());
      return OPERATOR_CANCELLED;
    }
    const TraceClock::time_point publication_start = TraceClock::now();
    const bool published = session->apply(C, std::move(*result), &error);
    const double publication_ms = elapsed_milliseconds(publication_start);
    emit_operator_trace(
        "native_planar", published ? "success" : "failed", operator_start, publication_ms);
    if (!published) {
      BKE_report(
          op->reports, RPT_ERROR, error.empty() ? "Native Solid Boolean failed" : error.c_str());
      return OPERATOR_CANCELLED;
    }
    BKE_report(op->reports, RPT_INFO, "Native Solid Boolean complete");
    return OPERATOR_FINISHED;
  }

  /* Keep circular extrusion supports analytic instead of converting their sampled strips
   * into source planes. All outputs are validated before replacing the source geometry. */
  if (selected_components.size() == 2) {
    const int other = selected_components[0] == cutter_component ? selected_components[1] :
                                                                   selected_components[0];
    auto a = cylinder_boolean::recognize(model, components[other].faces);
    auto b = cylinder_boolean::recognize(model, components[cutter_component].faces);
    if (a && !b)
      b = cylinder_boolean::recognize_prism(model, components[cutter_component].faces, a->axis);
    else if (b && !a)
      a = cylinder_boolean::recognize_prism(model, components[other].faces, b->axis);
    if (a && b) {
      const int mode = RNA_enum_get(op->ptr, "operation_mode");
      const bool swap = RNA_boolean_get(op->ptr, "swap_subtract");
      const bool keep = RNA_boolean_get(op->ptr, "keep_cutter");
      if (mode == 1 && swap)
        std::swap(a, b);
      solid::Model result;
      FaceSet preserve;
      for (const solid::Face &f : model.faces) {
        const bool cutter = std::find(components[cutter_component].faces.begin(),
                                      components[cutter_component].faces.end(),
                                      f.id) != components[cutter_component].faces.end();
        if (!consumed_faces.contains(f.id) ||
            (keep && mode != 0 && !(mode == 1 && swap) &&
             !(mode == 2 && !RNA_boolean_get(op->ptr, "intersect_with_cutter")) && cutter))
          preserve.insert(f.id);
      }
      FaceSet discard;
      for (Id id : consumed_faces)
        if (!preserve.contains(id))
          discard.insert(id);
      if (!append_source_components(result, model, preserve, discard)) {
        emit_operator_trace("analytic_cylinder", "failed", operator_start, 0.0);
        BKE_report(op->reports, RPT_ERROR, "Could not preserve unselected Solid bodies");
        return OPERATOR_CANCELLED;
      }
      const size_t first_face = result.faces.size(), first_edge = result.edges.size(),
                   first_vert = result.vertices.size();
      int operation = mode;
      if (mode == 1 && RNA_boolean_get(op->ptr, "invert_cutter"))
        operation = 2;
      bool success;
      if (mode == 3) {
        success = cylinder_boolean::append(result, *a, *b, 1, &error) &&
                  cylinder_boolean::append(result, *a, *b, 2, &error);
      }
      else
        success = cylinder_boolean::append(result, *a, *b, operation, &error);
      FaceSet output_faces;
      for (size_t i = first_face; i < result.faces.size(); ++i)
        output_faces.insert(result.faces[i].id);
      if (!success || !validate_result(result, output_faces, &error)) {
        emit_operator_trace("analytic_cylinder", "failed", operator_start, 0.0);
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
      solid::Selection selection;
      selection.mode = source.selection.mode;
      for (size_t i = first_face; i < result.faces.size(); ++i)
        selection.faces.push_back(result.faces[i].id);
      for (size_t i = first_edge; i < result.edges.size(); ++i)
        selection.edges.push_back(result.edges[i].id);
      for (size_t i = first_vert; i < result.vertices.size(); ++i)
        selection.vertices.push_back(result.vertices[i].id);
      const auto &active = selection.mode == 4 ? selection.faces :
                           selection.mode == 2 ? selection.edges :
                                                 selection.vertices;
      if (!active.empty())
        selection.active = active.front();
      const TraceClock::time_point publication_start = TraceClock::now();
      const bool published = session->apply(C, {std::move(result), std::move(selection)}, &error);
      const double publication_ms = elapsed_milliseconds(publication_start);
      emit_operator_trace(
          "analytic_cylinder", published ? "success" : "failed", operator_start, publication_ms);
      if (!published) {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
      BKE_report(
          op->reports, RPT_INFO, "Solid Boolean complete; analytic cylinder surfaces preserved");
      return OPERATOR_FINISHED;
    }
  }

  if (boolean_solver == 3) {
    auto result = native_boolean(
        model, components, selected_components, cutter_component, source.selection, op, &error);
    if (!result) {
      emit_operator_trace("native_general", "failed", operator_start, 0.0);
      BKE_report(
          op->reports, RPT_ERROR, error.empty() ? "Native Solid Boolean failed" : error.c_str());
      return OPERATOR_CANCELLED;
    }
    const TraceClock::time_point publication_start = TraceClock::now();
    const bool published = session->apply(C, std::move(*result), &error);
    const double publication_ms = elapsed_milliseconds(publication_start);
    emit_operator_trace(
        "native_general", published ? "success" : "failed", operator_start, publication_ms);
    if (!published) {
      BKE_report(
          op->reports, RPT_ERROR, error.empty() ? "Native Solid Boolean failed" : error.c_str());
      return OPERATOR_CANCELLED;
    }
    BKE_report(op->reports, RPT_INFO, "Native Solid Boolean complete");
    return OPERATOR_FINISHED;
  }

  const solid::DisplayMesh *display = source.display_samples.get();
  std::optional<solid::DisplayMesh> generated;
  if (!display) {
    generated = solid::tessellate(model, 64, &error);
    if (!generated) {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
      return OPERATOR_CANCELLED;
    }
    display = &*generated;
  }
  std::vector<OwnedMesh> body_meshes(components.size());
  for (const int component_index : selected_components) {
    const FaceSet ids(components[component_index].faces.begin(),
                      components[component_index].faces.end());
    body_meshes[component_index] = mesh_from_display(*display, ids, &error);
    if (!body_meshes[component_index]) {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
      return OPERATOR_CANCELLED;
    }
  }

  const int mode = RNA_enum_get(op->ptr, "operation_mode");
  const geometry::boolean::Solver solver = solver_from_rna(RNA_enum_get(op->ptr, "solver"));
  const bool keep_cutter = RNA_boolean_get(op->ptr, "keep_cutter");
  const bool swap_subtract = RNA_boolean_get(op->ptr, "swap_subtract");
  const bool invert_cutter = RNA_boolean_get(op->ptr, "invert_cutter");
  const bool intersect_with_cutter = RNA_boolean_get(op->ptr, "intersect_with_cutter");
  const bool merge_results = RNA_boolean_get(op->ptr, "merge_results");
  const bool dissolve = RNA_boolean_get(op->ptr, "limited_dissolve");
  const bool dissolve_per_iteration = RNA_boolean_get(op->ptr, "dissolve_per_iteration");
  FaceSet curved_surfaces;
  for (const solid::Surface &surface : model.surfaces) {
    if (surface.kind != solid::SurfaceKind::Plane) {
      curved_surfaces.insert(surface.id);
    }
  }
  const bool has_curved_input = std::any_of(
      selected_components.begin(), selected_components.end(), [&](const int component_index) {
        return std::any_of(components[component_index].faces.begin(),
                           components[component_index].faces.end(),
                           [&](const Id face_id) {
                             const solid::Face *face = solid::face(model, face_id);
                             return face && curved_surfaces.contains(face->surface);
                           });
      });
  if (has_curved_input) {
    BKE_report(op->reports,
               RPT_ERROR,
               "This curved Boolean combination is not supported analytically yet; source "
               "geometry was kept");
    return OPERATOR_CANCELLED;
  }
  const bool use_limited_dissolve = dissolve && !has_curved_input;
  const bool cutter_is_consumed = (mode == 1 && swap_subtract) ||
                                  (mode == 2 && !intersect_with_cutter);
  const OwnedMesh &cutter = body_meshes[cutter_component];
  std::vector<int> targets;
  for (const int index : selected_components) {
    if (index != cutter_component) {
      targets.push_back(index);
    }
  }
  std::vector<OwnedMesh> outputs;
  auto apply = [&](const Mesh &target,
                   const Mesh &tool,
                   const geometry::boolean::Operation operation) {
    return run_boolean(
        target, tool, operation, solver, use_limited_dissolve && dissolve_per_iteration, &error);
  };
  auto add_output = [&](OwnedMesh mesh) {
    if (mesh && mesh->faces_num > 0) {
      outputs.push_back(std::move(mesh));
    }
  };
  auto union_outputs = [&]() -> bool {
    if (outputs.size() < 2) {
      return true;
    }
    OwnedMesh result = std::move(outputs.front());
    for (size_t i = 1; i < outputs.size(); ++i) {
      if (!result) {
        result = std::move(outputs[i]);
      }
      else if (outputs[i]) {
        result = apply(*result, *outputs[i], geometry::boolean::Operation::Union);
        if (!result && !error.empty()) {
          return false;
        }
      }
    }
    outputs.clear();
    add_output(std::move(result));
    return true;
  };

  if (mode == 0) {  // Union.
    OwnedMesh result(BKE_mesh_copy_for_eval(*cutter));
    for (const int target : targets) {
      result = apply(*result, *body_meshes[target], geometry::boolean::Operation::Union);
      if (!result && !error.empty()) {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
    }
    add_output(std::move(result));
  }
  else if (mode == 1) {  // Subtract.
    const auto operation = invert_cutter ? geometry::boolean::Operation::Intersect :
                                           geometry::boolean::Operation::Difference;
    if (swap_subtract) {
      OwnedMesh result(BKE_mesh_copy_for_eval(*cutter));
      for (const int target : targets) {
        result = apply(*result, *body_meshes[target], operation);
        if (!result && !error.empty()) {
          BKE_report(op->reports, RPT_ERROR, error.c_str());
          return OPERATOR_CANCELLED;
        }
      }
      add_output(std::move(result));
    }
    else {
      for (const int target : targets) {
        OwnedMesh result = apply(*body_meshes[target], *cutter, operation);
        if (!result && !error.empty()) {
          BKE_report(op->reports, RPT_ERROR, error.c_str());
          return OPERATOR_CANCELLED;
        }
        add_output(std::move(result));
      }
      if (merge_results && !union_outputs()) {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
    }
  }
  else if (mode == 2) {  // Intersect.
    if (intersect_with_cutter) {
      for (const int target : targets) {
        OwnedMesh result = apply(
            *body_meshes[target], *cutter, geometry::boolean::Operation::Intersect);
        if (!result && !error.empty()) {
          BKE_report(op->reports, RPT_ERROR, error.c_str());
          return OPERATOR_CANCELLED;
        }
        add_output(std::move(result));
      }
      if (merge_results && !union_outputs()) {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
    }
    else {
      OwnedMesh result(BKE_mesh_copy_for_eval(*cutter));
      for (const int target : targets) {
        if (!result) {
          break;
        }
        result = apply(*result, *body_meshes[target], geometry::boolean::Operation::Intersect);
        if (!result && !error.empty()) {
          BKE_report(op->reports, RPT_ERROR, error.c_str());
          return OPERATOR_CANCELLED;
        }
      }
      add_output(std::move(result));
    }
  }
  else {  // Split.
    for (const int target : targets) {
      OwnedMesh outside = apply(
          *body_meshes[target], *cutter, geometry::boolean::Operation::Difference);
      if (!outside && !error.empty()) {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
      add_output(std::move(outside));
      OwnedMesh inside = apply(
          *body_meshes[target], *cutter, geometry::boolean::Operation::Intersect);
      if (!inside && !error.empty()) {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
      add_output(std::move(inside));
    }
  }

  if (use_limited_dissolve && !dissolve_per_iteration) {
    for (OwnedMesh &output : outputs) {
      output = limited_dissolve(std::move(output), &error);
      if (!output && !error.empty()) {
        BKE_report(op->reports, RPT_ERROR, error.c_str());
        return OPERATOR_CANCELLED;
      }
    }
  }

  solid::Model result_model;
  FaceSet preserve;
  for (const int component_index : IndexRange(components.size())) {
    const Component &component = components[component_index];
    const bool selected = std::all_of(component.faces.begin(),
                                      component.faces.end(),
                                      [&](const Id id) { return consumed_faces.contains(id); });
    const bool is_cutter = component_index == cutter_component;
    if (!selected || (keep_cutter && mode != 0 && !cutter_is_consumed && is_cutter)) {
      preserve.insert(component.faces.begin(), component.faces.end());
    }
  }
  FaceSet discarded_faces;
  for (const Id face_id : consumed_faces) {
    if (!preserve.contains(face_id)) {
      discarded_faces.insert(face_id);
    }
  }
  if (!append_source_components(result_model, model, preserve, discarded_faces)) {
    BKE_report(op->reports, RPT_ERROR, "Could not preserve unselected Solid bodies");
    return OPERATOR_CANCELLED;
  }
  const size_t preserved_face_count = result_model.faces.size();
  const size_t preserved_edge_count = result_model.edges.size();
  const size_t preserved_vertex_count = result_model.vertices.size();
  if (dissolve) {
    /* The Limited Dissolve preference is handled in the mesh workflow when an output can remain
     * planar. A non-planar dissolve would no longer be a valid analytic plane face. */
  }
  for (const OwnedMesh &output : outputs) {
    if (output && output->faces_num > 0 &&
        !append_mesh_result(result_model, *output, model, &error))
    {
      BKE_report(op->reports, RPT_ERROR, error.c_str());
      return OPERATOR_CANCELLED;
    }
  }
  FaceSet boolean_faces;
  for (const size_t i :
       IndexRange::from_begin_end(preserved_face_count, result_model.faces.size()))
  {
    boolean_faces.insert(result_model.faces[i].id);
  }
  if (!validate_result(result_model, boolean_faces, &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  solid::Selection result_selection;
  result_selection.mode = source.selection.mode;
  for (size_t i = preserved_vertex_count; i < result_model.vertices.size(); ++i) {
    result_selection.vertices.push_back(result_model.vertices[i].id);
  }
  for (size_t i = preserved_edge_count; i < result_model.edges.size(); ++i) {
    result_selection.edges.push_back(result_model.edges[i].id);
  }
  for (size_t i = preserved_face_count; i < result_model.faces.size(); ++i) {
    result_selection.faces.push_back(result_model.faces[i].id);
  }
  const auto &active_ids = result_selection.mode == 4 ? result_selection.faces :
                           result_selection.mode == 2 ? result_selection.edges :
                                                        result_selection.vertices;
  if (!active_ids.empty()) {
    result_selection.active = active_ids.front();
  }
  if (!session->apply(C, {std::move(result_model), std::move(result_selection)}, &error)) {
    BKE_report(op->reports, RPT_ERROR, error.c_str());
    return OPERATOR_CANCELLED;
  }
  if (dissolve && has_curved_input) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Limited Dissolve was skipped because curved inputs are tessellated");
  }
  BKE_report(op->reports,
             RPT_INFO,
             has_curved_input ? "Solid Boolean complete; curved inputs were tessellated" :
                                "Solid Boolean complete");
  return OPERATOR_FINISHED;
}

static void solid_boolean_draw(bContext * /*C*/, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(op->ptr, "operation_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "solver", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "keep_cutter", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const int mode = RNA_enum_get(op->ptr, "operation_mode");
  if (mode == 1) {
    layout.prop(op->ptr, "swap_subtract", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(op->ptr, "invert_cutter", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(op->ptr, "merge_results", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (mode == 2) {
    layout.prop(op->ptr, "intersect_with_cutter", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (RNA_boolean_get(op->ptr, "intersect_with_cutter")) {
      layout.prop(op->ptr, "merge_results", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
  if (mode != 3) {
    layout.prop(op->ptr, "limited_dissolve", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (RNA_boolean_get(op->ptr, "limited_dissolve")) {
      layout.prop(op->ptr, "dissolve_per_iteration", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
  layout.separator();
  layout.label("Circular cylinders retain analytic surfaces", ICON_INFO);
}

}  // namespace blender::ed::solid_editor

namespace blender {

void SOLID_OT_boolean(wmOperatorType *ot)
{
  static const EnumPropertyItem operations[] = {
      {0, "UNION", 0, "Union", "Add the other selected bodies to the active body"},
      {1, "SUBTRACT", 0, "Subtract", "Subtract with the active body as cutter"},
      {2, "INTERSECT", 0, "Intersect", "Keep the common volume"},
      {3, "SPLIT", 0, "Split", "Keep inside and outside target pieces"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  static const EnumPropertyItem solvers[] = {
      {3, "SOLID", 0, "Solid", "Native Solid Boolean preserving analytic surfaces and trim curves"},
      {0, "FLOAT", 0, "Float", "Fast, with limitations on overlapping geometry"},
      {1, "EXACT", 0, "Exact", "More robust for overlaps, but slower"},
      {2, "MANIFOLD", 0, "Manifold", "Fast solver for closed manifold bodies"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Boolean Solid Bodies";
  ot->idname = "SOLID_OT_boolean";
  ot->description = "Combine, cut, intersect, or split selected disconnected Solid bodies";
  ot->poll = ed::solid_editor::edit_poll;
  ot->exec = ed::solid_editor::solid_boolean_exec;
  ot->ui = ed::solid_editor::solid_boolean_draw;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "operation_mode", operations, 0, "Operation", "Boolean operation");
  RNA_def_enum(ot->srna, "solver", solvers, 3, "Solver", "Boolean solver");
  RNA_def_boolean(
      ot->srna, "keep_cutter", false, "Keep Cutter", "Keep the active body as a separate result");
  RNA_def_boolean(ot->srna,
                  "swap_subtract",
                  false,
                  "Swap Cutter",
                  "Subtract the other selected bodies from the active body");
  RNA_def_boolean(ot->srna,
                  "invert_cutter",
                  false,
                  "Invert",
                  "For subtraction, keep the part inside the cutter instead");
  RNA_def_boolean(ot->srna,
                  "intersect_with_cutter",
                  false,
                  "Intersect with Cutter",
                  "Intersect each target with the active body separately");
  RNA_def_boolean(ot->srna,
                  "merge_results",
                  false,
                  "Merge Results",
                  "Union separate Boolean results together");
  RNA_def_boolean(ot->srna,
                  "limited_dissolve",
                  true,
                  "Limited Dissolve",
                  "Dissolve coplanar result edges where possible");
  RNA_def_boolean(ot->srna,
                  "dissolve_per_iteration",
                  true,
                  "Per Iteration",
                  "Dissolve after each Boolean instead of after all results");
}

}  // namespace blender
