/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "BKE_lib_id.hh"
#include "BKE_nurbs_debug.hh"
#include "BKE_nurbs_evaluate.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_eval.hh"
#include "BLI_math_vector.hh"
#include "BLI_time.h"
#include "DNA_mesh_types.h"
#include "bmesh.hh"
#include "bmesh_tools.hh"
#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs::detail {
namespace {

static double choose(const int n, const int k)
{
  double result = 1;
  for (int i = 1; i <= k; i++) {
    result *= double(n - i + 1) / i;
  }
  return result;
}

static Vector<double> basis(const int degree, const double t)
{
  Vector<double> result;
  for (int i = 0; i <= degree; i++) {
    result.append(choose(degree, i) * std::pow(t, i) * std::pow(1 - t, degree - i));
  }
  return result;
}

/* The small, common Bernstein least-squares matrices are factored once per degree. Boundary
 * coefficients are fixed and shared; only interior coefficients enter the solve. */
struct NormalCondition {
  Vector<double> row;
  double3 normal;
};

struct FitSystem {
  int degree;
  bool surface;
  Vector<int> unknown;
  Vector<double2> samples;
  Vector<Vector<double>> rows, inverse;
  bool valid = true;

  FitSystem(const int degree, const bool surface, const int endpoint_controls = 1)
      : degree(degree), surface(surface)
  {
    const int n = degree + 1;
    for (int v = surface ? 1 : 0; v < (surface ? degree : 1); v++) {
      for (int u = surface ? 1 : endpoint_controls;
           u <= degree - (surface ? 1 : endpoint_controls);
           u++)
      {
        unknown.append(v * n + u);
      }
    }
    const int resolution = degree == 3 ? 6 : surface ? 16 : 24;
    for (int j = 0; j <= (surface ? resolution : 0); j++) {
      const double v = surface ? double(j) / resolution : 0;
      const auto bv = basis(degree, v);
      for (int i = 0; i <= resolution; i++) {
        const double u = double(i) / resolution;
        const auto bu = basis(degree, u);
        samples.append({u, v});
        Vector<double> row;
        for (int y = 0; y < (surface ? n : 1); y++) {
          for (int x = 0; x < n; x++) {
            row.append(bu[x] * (surface ? bv[y] : 1));
          }
        }
        rows.append(std::move(row));
      }
    }
    const int m = unknown.size();
    Vector<Vector<double>> matrix(m);
    inverse.resize(m);
    for (int i = 0; i < m; i++) {
      matrix[i].resize(m, 0.0);
      inverse[i].resize(m, 0.0);
      inverse[i][i] = 1;
      for (int j = 0; j < m; j++) {
        for (const auto &row : rows) {
          matrix[i][j] += row[unknown[i]] * row[unknown[j]];
        }
      }
    }
    for (int column = 0; column < m; column++) {
      int pivot = column;
      for (int row = column + 1; row < m; row++) {
        if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) {
          pivot = row;
        }
      }
      const double divisor = matrix[pivot][column];
      if (!(std::abs(divisor) > 1e-18)) {
        valid = false;
        return;
      }
      std::swap(matrix[pivot], matrix[column]);
      std::swap(inverse[pivot], inverse[column]);
      for (int j = 0; j < m; j++) {
        matrix[column][j] /= divisor;
        inverse[column][j] /= divisor;
      }
      for (int row = 0; row < m; row++) {
        if (row == column) {
          continue;
        }
        const double factor = matrix[row][column];
        for (int j = 0; j < m; j++) {
          matrix[row][j] -= factor * matrix[column][j];
          inverse[row][j] -= factor * inverse[column][j];
        }
      }
    }
  }

  template<typename Evaluate> bool fit(Vector<double3> &controls, Evaluate &&evaluate) const
  {
    if (!valid) {
      return false;
    }
    for (const int i : unknown) {
      controls[i] = double3(0);
    }
    Vector<double3> rhs(unknown.size(), double3(0));
    for (const int i : samples.index_range()) {
      double3 residual = evaluate(samples[i]);
      for (const int j : controls.index_range()) {
        residual -= controls[j] * rows[i][j];
      }
      for (const int j : unknown.index_range()) {
        rhs[j] += residual * rows[i][unknown[j]];
      }
    }
    for (const int i : unknown.index_range()) {
      double3 point(0);
      for (const int j : unknown.index_range()) {
        point += rhs[j] * inverse[i][j];
      }
      if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
        return false;
      }
      controls[unknown[i]] = point;
    }
    return true;
  }
  template<typename Evaluate>
  bool fit_normals(Vector<double3> &controls,
                   Evaluate &&evaluate,
                   const Span<NormalCondition> conditions) const
  {
    if (!fit(controls, evaluate)) {
      return false;
    }
    const int count = conditions.size();
    Vector<Vector<double>> projected(count), matrix(count);
    Vector<double> rhs(count);
    for (int i = 0; i < count; i++) {
      projected[i].resize(unknown.size(), 0.0);
      matrix[i].resize(count, 0.0);
      const auto &condition = conditions[i];
      double3 derivative(0);
      for (const int j : controls.index_range()) {
        derivative += controls[j] * condition.row[j];
      }
      rhs[i] = -math::dot(condition.normal, derivative);
      for (const int j : unknown.index_range()) {
        for (const int k : unknown.index_range()) {
          projected[i][j] += inverse[j][k] * condition.row[unknown[k]];
        }
      }
    }
    /* Woodbury reduction: the position fit has the same cached Bernstein matrix in
     * X/Y/Z. Only the small normal-constraint system depends on this corner's frame. */
    constexpr double weight = 10;
    for (int i = 0; i < count; i++) {
      for (int j = 0; j <= i; j++) {
        double value = 0;
        for (const int k : unknown.index_range()) {
          value += projected[i][k] * conditions[j].row[unknown[k]];
        }
        value *= math::dot(conditions[i].normal, conditions[j].normal);
        if (i == j) {
          value += 1 / (weight * weight);
        }
        matrix[i][j] = matrix[j][i] = value;
      }
    }
    for (int i = 0; i < count; i++) {
      for (int j = 0; j <= i; j++) {
        double value = matrix[i][j];
        for (int k = 0; k < j; k++) {
          value -= matrix[i][k] * matrix[j][k];
        }
        if (i == j) {
          if (!(value > 0) || !std::isfinite(value)) {
            return false;
          }
          matrix[i][j] = std::sqrt(value);
        }
        else {
          matrix[i][j] = value / matrix[j][j];
        }
      }
    }
    for (int i = 0; i < count; i++) {
      for (int j = 0; j < i; j++) {
        rhs[i] -= matrix[i][j] * rhs[j];
      }
      rhs[i] /= matrix[i][i];
    }
    for (int i = count - 1; i >= 0; i--) {
      for (int j = i + 1; j < count; j++) {
        rhs[i] -= matrix[j][i] * rhs[j];
      }
      rhs[i] /= matrix[i][i];
    }
    for (const int i : unknown.index_range()) {
      for (int j = 0; j < count; j++) {
        controls[unknown[i]] += conditions[j].normal * (projected[j][i] * rhs[j]);
      }
      const auto point = controls[unknown[i]];
      if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
        return false;
      }
    }
    return true;
  }
};

struct FaceData {
  int64_t id;
  BMFace *bm_face;
  int mesh_index;
  Vector<int64_t> vertices, edges;
};
struct EdgeData {
  int degree;
  Vector<double3> controls;
  std::array<double3, 2> directions;
};

static double2 side_uv(const int size, const int side, const double t)
{
  if (side == 0) {
    return {t, 0};
  }
  if (side == 1) {
    return {1, t};
  }
  if (size == 3 || side == 3) {
    return {0, 1 - t};
  }
  return {1 - t, 1};
}

static Vector<double3> elevate(const Span<double3> controls, const int degree)
{
  const int old = controls.size() - 1;
  Vector<double3> result(degree + 1, double3(0));
  for (int i = 0; i <= degree; i++) {
    for (int j = std::max(0, i - degree + old); j <= std::min(old, i); j++) {
      result[i] += controls[j] *
                   (choose(old, j) * choose(degree - old, i - j) / choose(degree, i));
    }
  }
  return result;
}

static OwnedCurve polynomial_curve(const Span<double3> controls, const int dimension)
{
  OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
  const int n = controls.size();
  curve->dimension = dimension;
  curve->order = curve->points_num = n;
  curve->knots_num = 2 * n;
  curve->points = MEM_new_array<NurbsControlPoint>(n, __func__);
  curve->knots = MEM_new_array<double>(2 * n, __func__);
  for (int i = 0; i < n; i++) {
    std::copy_n(&controls[i].x, 3, curve->points[i].position);
    curve->knots[i] = 0;
    curve->knots[n + i] = 1;
  }
  return curve;
}

}  // namespace

std::optional<SubdivisionBevelFit> subdivision_bevel_surfaces(const NurbsGeometry &source,
                                                              const double max_error,
                                                              std::string *r_error)
{
  const double start = BLI_time_now_seconds();
  auto fail = [&](const char *message) -> std::optional<SubdivisionBevelFit> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  const BMeshCreateParams parameters{.use_toolflags = false};
  BMesh *bm = BM_mesh_create(&bm_mesh_allocsize_default, &parameters);
  BLI_SCOPED_DEFER([&]() { BM_mesh_free(bm); });
  Map<int64_t, BMVert *> vertices;
  Map<std::pair<int64_t, int64_t>, int64_t> edge_ids;
  for (const auto &vertex : source.vertices) {
    const float3 position(double3(vertex.position));
    vertices.add_new(vertex.id, BM_vert_create(bm, position, nullptr, BM_CREATE_NOP));
  }
  auto key = [](const int64_t a, const int64_t b) {
    return std::pair<int64_t, int64_t>{std::min(a, b), std::max(a, b)};
  };
  for (const auto &edge : source.edges) {
    if (index->uses_for_edge(edge.id).size() != 2 ||
        !edge_ids.add(key(edge.vertex_ids[0], edge.vertex_ids[1]), edge.id))
    {
      return fail("Subdivision bevel currently needs a closed, unambiguous cage");
    }
    BM_edge_create(bm,
                   vertices.lookup(edge.vertex_ids[0]),
                   vertices.lookup(edge.vertex_ids[1]),
                   nullptr,
                   BM_CREATE_NOP);
  }
  Vector<FaceData> faces;
  for (const auto &face : source.faces) {
    if (!face.loops.is_single()) {
      return fail("Subdivision bevel needs one boundary per cage face");
    }
    FaceData data;
    data.id = face.id;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      data.vertices.append(edge.vertex_ids[use.reversed ? 1 : 0]);
    }
    if (face.orientation < 0) {
      std::reverse(data.vertices.begin(), data.vertices.end());
    }
    if (data.vertices.size() != 3 && data.vertices.size() != 4) {
      return fail("Subdivision bevel supports triangular and quadrilateral cage faces");
    }
    Vector<BMVert *> boundary;
    for (const int i : data.vertices.index_range()) {
      boundary.append(vertices.lookup(data.vertices[i]));
      data.edges.append(
          edge_ids.lookup(key(data.vertices[i], data.vertices[(i + 1) % data.vertices.size()])));
    }
    data.bm_face = BM_face_create_verts(
        bm, boundary.data(), boundary.size(), nullptr, BM_CREATE_NOP, true);
    if (!data.bm_face) {
      return fail("Subdivision bevel could not construct its reference cage");
    }
    faces.append(std::move(data));
  }
  Mesh *coarse = BKE_id_new_nomain<Mesh>(nullptr);
  BLI_SCOPED_DEFER([&]() { BKE_id_free(nullptr, coarse); });
  BM_mesh_bm_to_me_for_eval(*bm, *coarse, nullptr);
  subdiv::Settings settings{};
  settings.is_adaptive = true;
  settings.level = 4;
  settings.vtx_boundary_interpolation = subdiv::SUBDIV_VTX_BOUNDARY_EDGE_ONLY;
  settings.fvar_linear_interpolation =
      subdiv::SUBDIV_FVAR_LINEAR_INTERPOLATION_CORNERS_AND_JUNCTIONS;
  subdiv::Subdiv *smooth = subdiv::new_from_mesh(&settings, coarse);
  if (!smooth) {
    return fail("Subdivision bevel could not initialize its limit surface");
  }
  BLI_SCOPED_DEFER([&]() { subdiv::free(smooth); });
  if (!subdiv::eval_begin_from_mesh(smooth, coarse, subdiv::SUBDIV_EVALUATOR_TYPE_CPU)) {
    return fail("Subdivision bevel could not evaluate its limit surface");
  }
  const auto ptex = subdiv::face_ptex_offset_get(smooth);
  for (auto &face : faces) {
    face.mesh_index = BM_elem_index_get(face.bm_face);
  }
  auto locate = [&](const FaceData &face, const double2 uv) {
    int patch = ptex[face.mesh_index];
    double2 coordinate = uv;
    if (face.vertices.size() == 3) {
      const double bary[] = {(1 - uv.x) * (1 - uv.y), uv.x * (1 - uv.y), uv.y};
      int sector = 0;
      for (int i = 1; i < 3; i++) {
        if (bary[i] > bary[sector]) {
          sector = i;
        }
      }
      const double p = bary[(sector + 1) % 3], q = bary[(sector + 2) % 3];
      const double d = 2 * (p - q), b = 3 + d;
      const double s = 12 * p / (b + std::sqrt(std::max(0.0, b * b - 24 * p)));
      coordinate = math::clamp(double2(s, s - d), double2(0), double2(1));
      patch += sector;
    }
    return std::pair<int, double2>{patch, coordinate};
  };
  auto sample = [&](const FaceData &face, const double2 uv) {
    const auto location = locate(face, uv);
    return double3(subdiv::eval_limit_point(
        smooth, location.first, float(location.second.x), float(location.second.y)));
  };
  auto normal = [&](const FaceData &face, const double2 uv) {
    const auto location = locate(face, uv);
    float3 position, normal;
    subdiv::eval_limit_point_and_normal(smooth,
                                        location.first,
                                        float(location.second.x),
                                        float(location.second.y),
                                        position,
                                        normal);
    return double3(normal);
  };
  Map<int64_t, double3> limit_vertices;
  Map<int64_t, double3> vertex_normals;
  for (const auto &face : faces) {
    for (const int i : face.vertices.index_range()) {
      const double2 uv = side_uv(face.vertices.size(), i, 0);
      limit_vertices.lookup_or_add(face.vertices[i], sample(face, uv));
      vertex_normals.lookup_or_add(face.vertices[i], double3(0)) += normal(face, uv);
    }
  }
  for (auto item : vertex_normals.items()) {
    item.value = math::normalize(item.value);
  }
  FitSystem edge3(3, false), edge7(7, false), edge7_tangents(7, false, 2), face3(3, true),
      face7(7, true);
  Map<int64_t, EdgeData> curves;
  Map<int64_t, std::pair<const FaceData *, int>> reference_edges;
  for (const auto &face : faces) {
    for (const int i : face.edges.index_range()) {
      const int64_t id = face.edges[i];
      if (curves.contains(id)) {
        continue;
      }
      const auto &edge = *index->edge(id);
      bool regular = index->edges_for_vertex(edge.vertex_ids[0]).size() == 4 &&
                     index->edges_for_vertex(edge.vertex_ids[1]).size() == 4;
      for (const auto &use : index->uses_for_edge(id)) {
        regular &= static_cast<const NurbsTopologyLoop *>(index->face(use.face_id)->loops.first)
                       ->edge_uses.count() == 4;
      }
      EdgeData data;
      data.degree = regular ? 3 : 7;
      data.controls.resize(data.degree + 1, double3(0));
      data.controls.first() = limit_vertices.lookup(edge.vertex_ids[0]);
      data.controls.last() = limit_vertices.lookup(edge.vertex_ids[1]);
      const bool reverse = edge.vertex_ids[0] != face.vertices[i];
      const FitSystem &fit = regular ? edge3 : edge7;
      if (!fit.fit(data.controls, [&](const double2 parameter) {
            return sample(
                face, side_uv(face.vertices.size(), i, reverse ? 1 - parameter.x : parameter.x));
          }))
      {
        return fail("Subdivision bevel edge fitting is ill-conditioned");
      }
      reference_edges.add_new(id, {&face, i});
      curves.add_new(id, std::move(data));
    }
  }
  for (auto item : curves.items()) {
    const auto &edge = *index->edge(item.key);
    const auto reference = reference_edges.lookup(edge.id);
    const bool reverse = edge.vertex_ids[0] != reference.first->vertices[reference.second];
    auto point = [&](const double parameter) {
      return sample(*reference.first,
                    side_uv(reference.first->vertices.size(),
                            reference.second,
                            reverse ? 1 - parameter : parameter));
    };
    constexpr double h = 1.0 / 16;
    for (const int end : {0, 1}) {
      auto at = [&](const int i) { return point(end ? 1 - i * h : i * h); };
      item.value.directions[end] = math::normalize(-11 * at(0) + 18 * at(1) - 9 * at(2) +
                                                   2 * at(3));
    }
  }
  Map<std::pair<int64_t, int64_t>, double3> vertex_axes;
  for (const auto &vertex : source.vertices) {
    const auto incident = index->edges_for_vertex(vertex.id);
    const double3 n = vertex_normals.lookup(vertex.id);
    for (const int64_t id : incident) {
      if (vertex_axes.contains({vertex.id, id})) {
        continue;
      }
      const auto &edge = *index->edge(id);
      double3 axis = curves.lookup(id).directions[edge.vertex_ids[0] == vertex.id ? 0 : 1];
      int64_t opposite = 0;
      if (incident.size() == 4) {
        for (const int64_t candidate : incident) {
          if (candidate == id) {
            continue;
          }
          bool common_face = false;
          for (const auto &a : index->uses_for_edge(id)) {
            for (const auto &b : index->uses_for_edge(candidate)) {
              common_face |= a.face_id == b.face_id;
            }
          }
          if (!common_face) {
            opposite = candidate;
            const auto &other = *index->edge(candidate);
            axis -= curves.lookup(candidate).directions[other.vertex_ids[0] == vertex.id ? 0 : 1];
            break;
          }
        }
      }
      axis = math::normalize(axis - n * math::dot(axis, n));
      if (math::is_zero(axis)) {
        return fail("Subdivision bevel cannot resolve a tangent at mesh precision");
      }
      vertex_axes.add_new({vertex.id, id}, axis);
      if (opposite) {
        vertex_axes.add_new({vertex.id, opposite}, -axis);
      }
    }
  }
  for (auto item : curves.items()) {
    const auto &edge = *index->edge(item.key);
    auto &data = item.value;
    for (const int end : {0, 1}) {
      const int adjacent = end ? data.degree - 1 : 1;
      const double3 endpoint = data.controls[end ? data.degree : 0];
      const double3 axis = vertex_axes.lookup({edge.vertex_ids[end], edge.id});
      /* Keep the polynomial chart regular even at an extraordinary limit vertex.
       * This speed floor moves a control by at most 1e-10 of its edge chord. */
      const double minimum = 1e-10 * math::distance(data.controls.first(), data.controls.last());
      const double length = std::max(minimum, math::dot(data.controls[adjacent] - endpoint, axis));
      data.controls[adjacent] = endpoint + axis * length;
    }
    if (data.degree == 7) {
      const auto reference = reference_edges.lookup(edge.id);
      const bool reverse = edge.vertex_ids[0] != reference.first->vertices[reference.second];
      if (!edge7_tangents.fit(data.controls, [&](const double2 parameter) {
            return sample(*reference.first,
                          side_uv(reference.first->vertices.size(),
                                  reference.second,
                                  reverse ? 1 - parameter.x : parameter.x));
          }))
      {
        return fail("Subdivision bevel tangent-constrained edge fit is ill-conditioned");
      }
    }
  }
  double maximum_curve_deviation = 0;
  for (const auto &item : curves.items()) {
    const auto &edge = *index->edge(item.key);
    const auto &curve = item.value;
    const auto reference = reference_edges.lookup(edge.id);
    const bool reverse = edge.vertex_ids[0] != reference.first->vertices[reference.second];
    for (int i = 0; i < 16; i++) {
      const double parameter = (i + 0.37) / 16;
      const auto b = basis(curve.degree, parameter);
      double3 position(0);
      for (const int j : curve.controls.index_range()) {
        position += curve.controls[j] * b[j];
      }
      const double3 target = sample(*reference.first,
                                    side_uv(reference.first->vertices.size(),
                                            reference.second,
                                            reverse ? 1 - parameter : parameter));
      maximum_curve_deviation = std::max(maximum_curve_deviation,
                                         math::distance(position, target));
    }
  }
  TopologyBuilder builder(source);
  double extent = 1;
  for (auto &vertex : builder.geometry->vertices) {
    const double3 position = limit_vertices.lookup(vertex.id);
    std::copy_n(&position.x, 3, vertex.position);
    extent = std::max(extent, math::length(position));
  }
  const double roundoff = 512 * std::numeric_limits<double>::epsilon() * extent;
  if (roundoff > max_error) {
    return fail("Subdivision bevel coefficient construction exceeds its arithmetic budget");
  }
  auto install_curve = [&](OwnedCurve curve) {
    curve->id = builder.id();
    auto *created = curve.release();
    BLI_addtail(&builder.geometry->curves, created);
    builder.curves.add_new(created->id, created);
    return created->id;
  };
  for (auto &edge : builder.geometry->edges) {
    edge.curve_id = install_curve(polynomial_curve(curves.lookup(edge.id).controls, 3));
    edge.parameter_range[0] = 0;
    edge.parameter_range[1] = 1;
    edge.tolerance = std::max(edge.tolerance, roundoff);
  }
  double maximum_deviation = maximum_curve_deviation;
  int high_degree_faces = 0;
  for (const auto &data : faces) {
    int degree = data.vertices.size() == 3 ? 7 : 3;
    for (const int64_t id : data.edges) {
      degree = std::max(degree, curves.lookup(id).degree);
    }
    high_degree_faces += degree > 3;
    const int n = degree + 1;
    Vector<double3> controls(n * n, double3(0));
    for (const int side : data.edges.index_range()) {
      const auto &edge = *index->edge(data.edges[side]);
      auto boundary = elevate(curves.lookup(edge.id).controls, degree);
      if (edge.vertex_ids[0] != data.vertices[side]) {
        std::reverse(boundary.begin(), boundary.end());
      }
      for (int i = 0; i < n; i++) {
        const int slot = side == 0                              ? i :
                         side == 1                              ? i * n + degree :
                         side == 2 && data.vertices.size() == 4 ? degree * n + degree - i :
                                                                  (degree - i) * n;
        controls[slot] = boundary[i];
      }
    }
    if (data.vertices.size() == 3) {
      for (int i = 0; i < n; i++) {
        controls[degree * n + i] = limit_vertices.lookup(data.vertices[2]);
      }
    }
    /* A triangular Catmull-Clark face consists of three PTex charts. Their piecewise
     * bilinear parameterization is not a smooth rectangular chart. Use the shared boundary
     * curves to define a smooth planar Coons chart, then invert the limit surface in that
     * plane. Fitting the piecewise chart directly introduces artificial interior waviness. */
    auto surface_sample = [&](const double2 uv) {
      if (data.vertices.size() != 3) {
        return sample(data, uv);
      }
      auto boundary_point = [&](const int side, const double t) {
        const auto &edge = *index->edge(data.edges[side]);
        const auto &curve = curves.lookup(edge.id);
        const double parameter = edge.vertex_ids[0] == data.vertices[side] ? t : 1 - t;
        const auto b = basis(curve.degree, parameter);
        double3 position(0);
        for (const int i : curve.controls.index_range()) {
          position += curve.controls[i] * b[i];
        }
        return position;
      };
      const double3 a = limit_vertices.lookup(data.vertices[0]);
      const double3 b = limit_vertices.lookup(data.vertices[1]);
      const double3 c = limit_vertices.lookup(data.vertices[2]);
      const double3 desired = (1 - uv.y) * boundary_point(0, uv.x) +
                              (1 - uv.x) * boundary_point(2, 1 - uv.y) +
                              uv.x * boundary_point(1, uv.y) -
                              (1 - uv.y) * ((1 - uv.x) * a + uv.x * b);
      const double3 x = math::normalize(b - a);
      const double3 y = math::normalize(math::cross(math::cross(b - a, c - a), x));
      const auto initial = locate(data, uv);
      double minimum_error = INFINITY;
      double3 best = sample(data, uv);
      const double tolerance = 1e-7 * std::max(1.0, math::length(b - a));
      for (int attempt = 0; attempt < 3; attempt++) {
        const int sector = (initial.first - ptex[data.mesh_index] + attempt) % 3;
        const int patch = ptex[data.mesh_index] + sector;
        double2 coordinate = attempt == 0 ? initial.second : double2(0.5);
        for (int iteration = 0; iteration < 12; iteration++) {
          const double3 position(
              subdiv::eval_limit_point(smooth, patch, coordinate.x, coordinate.y));
          const double3 delta = position - desired;
          const double2 residual(math::dot(delta, x), math::dot(delta, y));
          const double error = math::length(residual);
          if (error < minimum_error) {
            minimum_error = error;
            best = position;
          }
          if (error < tolerance) {
            return position;
          }
          float3 unused, du, dv;
          subdiv::eval_limit_point_and_derivatives(
              smooth, patch, coordinate.x, coordinate.y, unused, du, dv);
          const double aa = math::dot(double3(du), x), ab = math::dot(double3(dv), x);
          const double ba = math::dot(double3(du), y), bb = math::dot(double3(dv), y);
          const double determinant = aa * bb - ab * ba;
          if (std::abs(determinant) < 1e-20) {
            break;
          }
          const double2 step((bb * residual.x - ab * residual.y) / determinant,
                             (aa * residual.y - ba * residual.x) / determinant);
          const double2 next = math::clamp(coordinate - step, double2(0), double2(1));
          if (math::distance(next, coordinate) < 1e-10) {
            break;
          }
          coordinate = next;
        }
      }
      return best;
    };
    const FitSystem &fit = degree == 3 ? face3 : face7;
    Vector<NormalCondition> conditions;
    if (degree > 3) {
      for (const int side : data.edges.index_range()) {
        const auto &edge = *index->edge(data.edges[side]);
        const auto &curve = curves.lookup(edge.id);
        const auto reference = reference_edges.lookup(edge.id);
        for (int j = 1; j < 16; j++) {
          const double t = j / 16.0;
          const double canonical = edge.vertex_ids[0] == data.vertices[side] ? t : 1 - t;
          const double reference_t = edge.vertex_ids[0] ==
                                             reference.first->vertices[reference.second] ?
                                         canonical :
                                         1 - canonical;
          double3 target_normal = normal(
              *reference.first,
              side_uv(reference.first->vertices.size(), reference.second, reference_t));
          const auto derivative_basis = basis(curve.degree - 1, canonical);
          double3 tangent(0);
          for (int i = 0; i < curve.degree; i++) {
            tangent += (curve.controls[i + 1] - curve.controls[i]) *
                       (curve.degree * derivative_basis[i]);
          }
          target_normal -= tangent *
                           (math::dot(target_normal, tangent) / math::length_squared(tangent));
          target_normal = math::normalize(target_normal);
          const double2 uv = side_uv(data.vertices.size(), side, t);
          const auto bu = basis(degree, uv.x), bv = basis(degree, uv.y);
          const auto low_u = basis(degree - 1, uv.x), low_v = basis(degree - 1, uv.y);
          NormalCondition condition;
          condition.normal = target_normal;
          for (int v = 0; v < n; v++) {
            for (int u = 0; u < n; u++) {
              const double du = degree * ((u ? low_u[u - 1] : 0) - (u < degree ? low_u[u] : 0));
              const double dv = degree * ((v ? low_v[v - 1] : 0) - (v < degree ? low_v[v] : 0));
              condition.row.append(
                  side == 0 || (side == 2 && data.vertices.size() == 4) ? bu[u] * dv : du * bv[v]);
            }
          }
          conditions.append(std::move(condition));
        }
      }
    }
    const auto evaluate = [&](const double2 uv) { return surface_sample(uv); };
    const bool accepted = conditions.is_empty() ? fit.fit(controls, evaluate) :
                                                  fit.fit_normals(controls, evaluate, conditions);
    if (!accepted) {
      return fail("Subdivision bevel surface fitting is ill-conditioned");
    }
    /* Holdout locations differ from fitting locations. This is a sampled comparison, not a
     * certified Hausdorff bound. */
    for (int j = 0; j < 7; j++) {
      for (int i = 0; i < 7; i++) {
        const double2 uv((i + 0.37) / 7, (j + 0.61) / 7);
        const auto bu = basis(degree, uv.x), bv = basis(degree, uv.y);
        double3 position(0);
        for (int v = 0; v < n; v++) {
          for (int u = 0; u < n; u++) {
            position += controls[v * n + u] * (bu[u] * bv[v]);
          }
        }
        maximum_deviation = std::max(maximum_deviation,
                                     math::distance(position, surface_sample(uv)));
      }
    }
    auto *surface = MEM_new<NurbsSurfaceGeometry>(__func__);
    surface->id = builder.id();
    surface->order_u = surface->order_v = surface->points_u = surface->points_v = n;
    surface->points_num = n * n;
    surface->knots_u_num = surface->knots_v_num = 2 * n;
    surface->points = MEM_new_array<NurbsControlPoint>(n * n, __func__);
    surface->knots_u = MEM_new_array<double>(2 * n, __func__);
    surface->knots_v = MEM_new_array<double>(2 * n, __func__);
    for (int i = 0; i < n * n; i++) {
      std::copy_n(&controls[i].x, 3, surface->points[i].position);
    }
    for (int i = 0; i < 2 * n; i++) {
      surface->knots_u[i] = surface->knots_v[i] = i < n ? 0 : 1;
    }
    BLI_addtail(&builder.geometry->surfaces, surface);
    builder.surfaces.add_new(surface->id, surface);
    auto &face = *builder.faces.lookup(data.id);
    face.surface_id = surface->id;
    face.orientation = 1;
    face.tolerance = std::max(face.tolerance, roundoff);
    auto &loop = *static_cast<NurbsTopologyLoop *>(face.loops.first);
    BLI_freelistN(&loop.edge_uses);
    for (const int side : data.edges.index_range()) {
      const auto &edge = *builder.edges.lookup(data.edges[side]);
      double2 a = side_uv(data.vertices.size(), side, 0),
              b = side_uv(data.vertices.size(), side, 1);
      if (edge.vertex_ids[0] != data.vertices[side]) {
        std::swap(a, b);
      }
      const double3 points[] = {{a.x, a.y, 0}, {b.x, b.y, 0}};
      auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
      use->id = builder.id();
      use->edge_id = edge.id;
      use->reversed = edge.vertex_ids[0] != data.vertices[side];
      use->pcurve_id = install_curve(polynomial_curve({points, 2}, 2));
      use->tolerance = std::max(face.tolerance, roundoff);
      BLI_addtail(&loop.edge_uses, use);
    }
  }
  /* The temporary analytic cage and source parents are not part of this fitted
   * representation. Retain only resources actually referenced by the resulting topology. */
  Set<int64_t> live_curves, live_surfaces;
  for (const auto &edge : builder.geometry->edges) {
    live_curves.add(edge.curve_id);
  }
  for (const auto &face : builder.geometry->faces) {
    live_surfaces.add(face.surface_id);
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        live_curves.add(use.pcurve_id);
      }
    }
  }
  OwnedGeometry discarded(MEM_new<NurbsGeometry>(__func__));
  auto *curve = static_cast<NurbsCurveGeometry *>(builder.geometry->curves.first);
  while (curve) {
    auto *next = curve->next;
    if (!live_curves.contains(curve->id)) {
      builder.curves.remove(curve->id);
      BLI_remlink(&builder.geometry->curves, curve);
      BLI_addtail(&discarded->curves, curve);
    }
    curve = next;
  }
  auto *surface = static_cast<NurbsSurfaceGeometry *>(builder.geometry->surfaces.first);
  while (surface) {
    auto *next = surface->next;
    if (!live_surfaces.contains(surface->id)) {
      builder.surfaces.remove(surface->id);
      BLI_remlink(&builder.geometry->surfaces, surface);
      BLI_addtail(&discarded->surfaces, surface);
    }
    surface = next;
  }
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  if (std::getenv("BLENDER_NURBS_SUBDIVISION_FIT_DIAGNOSTICS")) {
    const auto fitted_index = TopologyIndex::build(*builder.geometry, r_error);
    double maximum_angle = 0;
    int undefined = 0;
    for (const auto &edge : builder.geometry->edges) {
      const auto uses = fitted_index->uses_for_edge(edge.id);
      for (int j = 1; j < 16; j++) {
        double3 normals[2];
        bool accepted = true;
        for (int side = 0; side < 2; side++) {
          const auto &use = *uses[side].use;
          const auto &surface = *builder.surfaces.lookup(
              fitted_index->face(uses[side].face_id)->surface_id);
          const auto uv = evaluate_curve(*builder.curves.lookup(use.pcurve_id), j / 16.0, r_error);
          const auto value = evaluate_surface_differential(
              surface, double2(uv->position), {}, r_error);
          if (!value || !value->normal) {
            accepted = false;
            undefined++;
            break;
          }
          normals[side] = *value->normal;
        }
        if (accepted) {
          const double angle = std::acos(
                                   std::clamp(math::dot(normals[0], normals[1]), -1.0, 1.0)) *
                               180 / M_PI;
          maximum_angle = std::max(maximum_angle, angle);
        }
      }
    }
    NURBS_DEBUG_LOG("Subdivision seam diagnostic: maximum angle %.9g degrees; undefined %d",
                    maximum_angle,
                    undefined);
  }
  NURBS_DEBUG_LOG(
      "Subdivision native fit: %.3f ms; %d faces, %d degree-seven faces; maximum holdout "
      "deviation %.9g",
      (BLI_time_now_seconds() - start) * 1000,
      faces.size(),
      high_degree_faces,
      maximum_deviation);
  return SubdivisionBevelFit{std::move(builder.geometry), maximum_deviation, roundoff};
}

}  // namespace blender::bke::nurbs::detail
