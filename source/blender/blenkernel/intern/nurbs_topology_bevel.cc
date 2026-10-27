/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <array>
#include <cmath>

#include "BLI_math_boolean.hh"
#include "BLI_math_vector.hh"
#include "BLI_memory_utils.hh"
#include "BLI_time.h"

#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_debug.hh"
#include "BKE_nurbs_evaluate.hh"
#include "BKE_nurbs_refine.hh"
#include "BKE_nurbs_topology_edit.hh"

#include "eigen_capi.h"

#include "nurbs_math.hh"
#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs {

namespace {

using namespace detail;
using BoundedVector = std::array<BoundedScalar, 3>;

static BoundedVector difference(const double3 a, const double3 b)
{
  BoundedVector result;
  for (int i = 0; i < 3; i++) {
    result[i] = bounded_subtract({a[i]}, {b[i]});
  }
  return result;
}

static BoundedScalar dot(const BoundedVector &a, const BoundedVector &b)
{
  BoundedScalar result;
  for (int i = 0; i < 3; i++) {
    result = bounded_add(result, bounded_multiply(a[i], b[i]));
  }
  return result;
}

static BoundedVector cross(const BoundedVector &a, const BoundedVector &b)
{
  BoundedVector result;
  for (int i = 0; i < 3; i++) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    result[i] = bounded_subtract(bounded_multiply(a[j], b[k]), bounded_multiply(a[k], b[j]));
  }
  return result;
}

/** IEEE square root followed by a residual enclosure; the divisor is a lower bound for
 * sqrt(exact input) + stored root. This also accounts for rounded/subnormal products. */
static BoundedScalar root(const BoundedScalar input)
{
  if (!input.valid() || input.value < input.error) {
    return {0, INFINITY};
  }
  if (input.value == 0) {
    return {0, 0};
  }
  const double value = std::sqrt(input.value);
  const auto residual = bounded_subtract(bounded_multiply({value}, {value}), input);
  const auto error = bounded_divide({add_upper(std::abs(residual.value), residual.error)},
                                    {value});
  return error.valid() ? BoundedScalar{value, add_upper(std::abs(error.value), error.error)} :
                         BoundedScalar{0, INFINITY};
}

static double distance_upper(const double3 a, const double3 b)
{
  double result = 0;
  for (const auto &coordinate : difference(a, b)) {
    result = add_upper(result, add_upper(std::abs(coordinate.value), coordinate.error));
  }
  return result;
}

static bool full_line(const NurbsCurveGeometry &curve, const NurbsTopologyEdge &edge)
{
  return curve.dimension == 3 && curve.order == 2 && curve.points_num == 2 &&
         curve.points[0].weight > 0 && curve.points[0].weight == curve.points[1].weight &&
         curve.knots[0] == edge.parameter_range[0] && curve.knots[1] == edge.parameter_range[0] &&
         curve.knots[2] == edge.parameter_range[1] && curve.knots[3] == edge.parameter_range[1];
}

static NurbsCurveGeometry *install_curve(TopologyBuilder &builder, OwnedCurve curve)
{
  curve->id = builder.id();
  auto *result = curve.release();
  BLI_addtail(&builder.geometry->curves, result);
  builder.curves.add_new(result->id, result);
  return result;
}

static OwnedCurve line(const double3 a,
                       const double3 b,
                       const double2 range = {0, 1},
                       const int dimension = 3)
{
  OwnedCurve result(MEM_new<NurbsCurveGeometry>(__func__));
  result->dimension = dimension;
  result->order = result->points_num = 2;
  result->knots_num = 4;
  result->points = MEM_new_array<NurbsControlPoint>(2, __func__);
  result->knots = MEM_new_array<double>(4, __func__);
  for (int i = 0; i < 2; i++) {
    std::copy_n((i ? &b.x : &a.x), 3, result->points[i].position);
    result->knots[2 * i] = result->knots[2 * i + 1] = range[i];
  }
  return result;
}

static NurbsTopologyVertex *vertex(TopologyBuilder &builder,
                                   const double3 position,
                                   const double tolerance)
{
  auto *result = MEM_new<NurbsTopologyVertex>(__func__);
  result->id = builder.id();
  std::copy_n(&position.x, 3, result->position);
  result->tolerance = tolerance;
  BLI_addtail(&builder.geometry->vertices, result);
  builder.vertices.add_new(result->id, result);
  return result;
}

static NurbsTopologyEdge *edge(TopologyBuilder &builder,
                               const int64_t a,
                               const int64_t b,
                               OwnedCurve curve,
                               const double tolerance)
{
  const auto *installed = install_curve(builder, std::move(curve));
  auto *result = MEM_new<NurbsTopologyEdge>(__func__);
  result->id = builder.id();
  result->curve_id = installed->id;
  result->vertex_ids[0] = a;
  result->vertex_ids[1] = b;
  result->parameter_range[0] = installed->knots[installed->order - 1];
  result->parameter_range[1] = installed->knots[installed->points_num];
  result->tolerance = tolerance;
  BLI_addtail(&builder.geometry->edges, result);
  builder.edges.add_new(result->id, result);
  return result;
}

struct PlanarBevelCorner {
  int64_t vertex, incoming, outgoing;
  std::array<BoundedScalar, 2> heights{};
};

struct BevelWidthLink {
  int first, second;
  BoundedScalar first_height, second_height;
};

struct BevelStationMotion {
  int64_t vertex;
  /** Displacement per unit offset. Planning these before geometry avoids width-dependent
   * collision failures and keeps the selected network's proportions when limiting it. */
  BoundedVector direction;
};

struct BevelOffsetLimit {
  double offset = std::numeric_limits<double>::infinity();

  /** First zero of available - offset * closing, approached with positive clearance. */
  bool add(const BoundedScalar available, const BoundedScalar closing, const double clearance = 0)
  {
    if (!available.valid() || !closing.valid() || !std::isfinite(clearance)) {
      return false;
    }
    const double speed = add_upper(closing.value, closing.error);
    if (speed <= 0) {
      return true;
    }
    const auto gap = bounded_subtract(available, {clearance});
    const auto limit = bounded_divide(gap, {speed});
    if (!limit.valid() || limit.value <= limit.error) {
      return false;
    }
    offset = std::min(offset, std::nextafter(limit.value - limit.error, 0.0));
    return offset > 0;
  }
};

/** Source-coordinate rounding clearance over the entire limited sweep. Use a bound on
 * magnitudes, rather than the residual at one particular width (which can happen to be zero).
 * This is an arithmetic enclosure, not a modeling or fitting tolerance. */
static double station_roundoff(const double origin, const BoundedScalar motion, const double width)
{
  const double magnitude = add_upper(
      std::abs(origin), multiply_upper(add_upper(std::abs(motion.value), motion.error), width));
  return add_upper(multiply_upper(4 * std::numeric_limits<double>::epsilon(), magnitude),
                   4 * std::numeric_limits<double>::denorm_min());
}

/** BMesh's loop-slide width objective: strong agreement at opposite ends of an edge and a
 * 0.2-weight preference for the requested width. A shared on-edge station is a hard ratio of
 * its incident face offsets. This chooses widths, not fitted source geometry. Certify the
 * solution using a bounded normal-equation residual and the positive diagonal contributed by
 * the width preferences; fail instead of accepting an unbounded numerical solve.
 */
static bool balance_bevel_widths(const Span<std::array<int, 2>> pairs,
                                 const Span<BevelWidthLink> links,
                                 const double offset,
                                 Vector<BoundedScalar> &widths,
                                 std::string *r_error)
{
  auto fail = [&](const char *message) {
    if (r_error) {
      *r_error = message;
    }
    return false;
  };
  Vector<int> groups(widths.size(), -1);
  Vector<BoundedScalar> scales(widths.size(), BoundedScalar{1});
  int columns = 0;
  bool needs_solve = false;
  for (const auto &link : links) {
    const auto ratio = bounded_divide(link.second_height, link.first_height);
    if (!ratio.valid() || ratio.value <= ratio.error || groups[link.first] != -1 ||
        groups[link.second] != -1)
    {
      return fail("NURBS bevel has an ambiguous or ill-conditioned offset chain");
    }
    groups[link.first] = groups[link.second] = columns++;
    scales[link.second] = ratio;
    needs_solve |= ratio.value != 1;
  }
  if (!needs_solve) {
    return true;
  }
  for (int &group : groups) {
    if (group == -1) {
      group = columns++;
    }
  }
  if (pairs.size() > std::numeric_limits<int>::max() / 3) {
    return fail("NURBS bevel width balancing exceeds the solver index range");
  }
  struct Equation {
    std::array<int, 2> columns{};
    std::array<BoundedScalar, 2> coefficients{};
    BoundedScalar rhs;
    int terms = 0;
  };
  Vector<Equation> equations;
  Vector<BoundedScalar> preferences(columns);
  for (const auto &pair : pairs) {
    equations.append({{groups[pair[0]], groups[pair[1]]},
                      {scales[pair[0]], bounded_multiply({-1}, scales[pair[1]])},
                      {},
                      2});
    for (const int index : pair) {
      const auto coefficient = bounded_multiply({0.2}, scales[index]);
      equations.append(
          {{groups[index], 0}, {coefficient, {}}, bounded_multiply({0.2}, {offset}), 1});
      preferences[groups[index]] = bounded_add(preferences[groups[index]],
                                               bounded_multiply(coefficient, coefficient));
    }
  }
  LinearSolver *solver = EIG_linear_least_squares_solver_new(equations.size(), columns, 1);
  BLI_SCOPED_DEFER([&]() { EIG_linear_solver_delete(solver); });
  for (const int row : equations.index_range()) {
    const auto &equation = equations[row];
    for (int term = 0; term < equation.terms; term++) {
      EIG_linear_solver_matrix_add(
          solver, row, equation.columns[term], equation.coefficients[term].value);
    }
    EIG_linear_solver_right_hand_side_add(solver, 0, row, equation.rhs.value);
  }
  if (!EIG_linear_solver_solve(solver)) {
    return fail("NURBS bevel width balancing could not solve its offset chain");
  }
  Vector<double> solution(columns);
  for (int column = 0; column < columns; column++) {
    solution[column] = EIG_linear_solver_variable_get(solver, 0, column);
    if (!std::isfinite(solution[column])) {
      return fail("NURBS bevel width balancing returned a non-finite solution");
    }
  }
  Vector<BoundedScalar> gradient(columns);
  for (const auto &equation : equations) {
    BoundedScalar residual = bounded_multiply({-1}, equation.rhs);
    for (int term = 0; term < equation.terms; term++) {
      residual = bounded_add(
          residual,
          bounded_multiply(equation.coefficients[term], {solution[equation.columns[term]]}));
    }
    for (int term = 0; term < equation.terms; term++) {
      const int column = equation.columns[term];
      gradient[column] = bounded_add(gradient[column],
                                     bounded_multiply(equation.coefficients[term], residual));
    }
  }
  double norm = 0, lower = std::numeric_limits<double>::infinity();
  for (int column = 0; column < columns; column++) {
    if (!gradient[column].valid() || !preferences[column].valid()) {
      return fail("NURBS bevel width balancing has no finite residual bound");
    }
    norm = add_upper(norm, add_upper(std::abs(gradient[column].value), gradient[column].error));
    lower = std::min(lower,
                     std::nextafter(preferences[column].value - preferences[column].error, 0.0));
  }
  if (!(lower > 0)) {
    return fail("NURBS bevel width balancing cannot certify a regular offset system");
  }
  const auto quotient = bounded_divide({norm}, {lower});
  const double error = add_upper(std::abs(quotient.value), quotient.error);
  for (const int i : widths.index_range()) {
    widths[i] = bounded_multiply(scales[i], {solution[groups[i]], error});
    if (!widths[i].valid() || widths[i].value <= widths[i].error) {
      return fail("NURBS bevel width balancing cannot certify positive widths");
    }
  }
  return true;
}

struct RoundBevelCorner {
  OwnedSurface support;
  /** Equator start, equator end, pole. */
  std::array<int64_t, 3> stations;
  /** Boundary profile edges in support order: start-end, end-pole, pole-start. */
  std::array<int64_t, 3> boundary_edges{};
  int64_t face_id = 0;
};

/** Port BMesh's three-sided M_ADJ corner indexing and quad assembly. Even segment counts
 * share one interior vertex; odd counts have a center triangle. Only the subdivision layout
 * is ported: all points and edge curves are constructed on the authoritative NURBS support.
 * Private copies of that support and exact UV trims retain the original rounded surface.
 */
static bool partition_rounded_corner_patches(TopologyCutResult &cut,
                                             const Map<int64_t, RoundBevelCorner> &corners,
                                             const int segments,
                                             const double max_error,
                                             std::string *r_error)
{
  auto fail = [&](const char *message) {
    if (r_error) {
      *r_error = message;
    }
    return false;
  };
  if (corners.is_empty() || segments <= 1) {
    return true;
  }
  if (segments > 100) {
    return fail("Rounded NURBS corner partition exceeds the segment budget");
  }
  struct EdgeChain {
    Vector<int64_t> edges, vertices;
    Vector<double2> parameters;
  };
  struct CornerNode {
    int64_t vertex = 0;
    double2 uv{0};
    double3 direction{0};
  };
  struct CornerEdge {
    int64_t edge, pcurve;
  };
  TopologyBuilder builder(*cut.edit.geometry);
  const int half = segments / 2, odd = segments % 2;
  auto key = [](const int64_t a, const int64_t b) {
    return std::pair<int64_t, int64_t>{std::min(a, b), std::max(a, b)};
  };
  /* Same equivalences as bmesh_bevel.cc::mesh_vert_canon. j is the inward ring,
   * k the position along the outgoing boundary profile, and i the corner sector. */
  auto canonical = [&](const int i, const int j, const int k) -> std::array<int, 3> {
    if (!odd && j == half && k == half) {
      return {0, j, k};
    }
    if (j <= half - 1 + odd && k <= half) {
      return {i, j, k};
    }
    if (k <= half) {
      return {(i + 2) % 3, k, segments - j};
    }
    return {(i + 1) % 3, segments - k, j};
  };
  const double weight = std::sqrt(0.5);
  auto reference_direction = [&](const double2 uv) {
    auto circle = [&](const double t) {
      const double a = (1 - t) * (1 - t), b = 2 * weight * t * (1 - t), c = t * t;
      return double2((a + b) / (a + b + c), (b + c) / (a + b + c));
    };
    const double2 longitude = circle(uv.x), latitude = circle(uv.y);
    const double3 p(longitude.x * latitude.x, longitude.y * latitude.x, latitude.y);
    return p / (p.x + p.y + p.z);
  };
  auto reference_uv = [&](const double3 p) {
    const double xy = std::hypot(p.x, p.y), xyz = std::hypot(p.x, p.y, p.z);
    return double2(p.y / (weight * (xy + p.x) + (1 - weight) * p.y),
                   p.z / (weight * (xyz + xy) + (1 - weight) * p.z));
  };

  for (const auto &item : corners.items()) {
    const RoundBevelCorner &corner = item.value;
    NurbsTopologyFace *face = builder.faces.lookup(corner.face_id);
    const NurbsTopologyFace face_template = *face;
    const int64_t support_id = face->surface_id;
    const NurbsSurfaceGeometry &support = *builder.surfaces.lookup(support_id);
    Map<int64_t, int64_t> pcurves;
    for (const NurbsTopologyLoop &loop : face->loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        pcurves.add_overwrite(use.edge_id, use.pcurve_id);
      }
    }
    auto edge_chain = [&](const int64_t original,
                          const int64_t start,
                          const int64_t end) -> std::optional<EdgeChain> {
      const auto *pieces = cut.edge_descendants.lookup_ptr(original);
      if (!pieces || pieces->size() != segments) {
        return std::nullopt;
      }
      EdgeChain chain;
      Vector<bool> used(segments, false);
      chain.vertices.append(start);
      int64_t current = start;
      for (int step = 0; step < segments; step++) {
        int chosen = -1;
        for (const int i : pieces->index_range()) {
          const auto &edge = *builder.edges.lookup((*pieces)[i]);
          if (!used[i] && ELEM(current, edge.vertex_ids[0], edge.vertex_ids[1])) {
            chosen = i;
            break;
          }
        }
        if (chosen < 0) {
          return std::nullopt;
        }
        used[chosen] = true;
        const auto &edge = *builder.edges.lookup((*pieces)[chosen]);
        const bool reverse = edge.vertex_ids[1] == current;
        const int64_t *pcurve_id = pcurves.lookup_ptr(edge.id);
        if (!pcurve_id) {
          return std::nullopt;
        }
        const auto &pcurve = *builder.curves.lookup(*pcurve_id);
        const auto a = evaluate_curve_bounded(
            pcurve, edge.parameter_range[reverse ? 1 : 0], max_error, r_error);
        const auto b = evaluate_curve_bounded(
            pcurve, edge.parameter_range[reverse ? 0 : 1], max_error, r_error);
        if (!a || !b) {
          return std::nullopt;
        }
        const double2 uv_a(a->position), uv_b(b->position);
        if (chain.parameters.is_empty()) {
          chain.parameters.append(uv_a);
        }
        else if (chain.parameters.last() != uv_a) {
          return std::nullopt;
        }
        chain.parameters.append(uv_b);
        current = edge.vertex_ids[reverse ? 0 : 1];
        chain.edges.append(edge.id);
        chain.vertices.append(current);
        cut.edit.geometry_error_bound = std::max(cut.edit.geometry_error_bound,
                                                 std::max(a->error_bound, b->error_bound));
      }
      return current == end ? std::optional<EdgeChain>(std::move(chain)) : std::nullopt;
    };
    std::array<EdgeChain, 3> boundaries;
    for (int i = 0; i < 3; i++) {
      auto chain = edge_chain(
          corner.boundary_edges[i], corner.stations[i], corner.stations[(i + 1) % 3]);
      if (!chain) {
        return fail("Rounded NURBS corner lost its shared strip boundary subdivisions");
      }
      boundaries[i] = std::move(*chain);
    }

    std::array<Vector<CornerNode>, 3> sectors;
    for (auto &sector : sectors) {
      sector.resize((half + 1) * (half + 1));
    }
    auto node = [&](const int i, const int j, const int k) -> CornerNode & {
      const auto index = canonical(i, j, k);
      return sectors[index[0]][index[1] * (half + 1) + index[2]];
    };
    Map<std::pair<int64_t, int64_t>, CornerEdge> corner_edges;
    for (int i = 0; i < 3; i++) {
      const EdgeChain &chain = boundaries[i];
      for (int k = 0; k <= segments; k++) {
        CornerNode &point = node(i, 0, k);
        if (point.vertex && point.vertex != chain.vertices[k]) {
          return fail("Rounded NURBS corner has inconsistent mesh-style boundary identities");
        }
        point.vertex = chain.vertices[k];
        point.uv = chain.parameters[k];
        point.direction = reference_direction(point.uv);
      }
      for (const int k : chain.edges.index_range()) {
        const int64_t edge = chain.edges[k];
        corner_edges.add_new(key(chain.vertices[k], chain.vertices[k + 1]),
                             {edge, pcurves.lookup(edge)});
      }
    }

    /* Blend the two actual boundary profiles toward the shared reference centroid. The
     * symmetric three-sector chart places subdivisions without inheriting the support's pole.
     * These UVs choose cut positions only; exact source evaluations supply all 3D geometry. */
    for (int i = 0; i < 3; i++) {
      for (int j = 1; j <= half; j++) {
        for (int k = 1; k <= half; k++) {
          if (!odd && j == half && !(k == half && i == 0)) {
            continue;
          }
          CornerNode &point = node(i, j, k);
          if (point.vertex) {
            continue;
          }
          const double s = 2.0 * k / segments, t = 2.0 * j / segments;
          double3 axis(0);
          axis[i] = 1;
          point.direction = (1 - t) * node(i, 0, k).direction + (1 - s) * node(i, j, 0).direction -
                            (1 - s) * (1 - t) * axis + s * t * double3(1.0 / 3.0);
          point.uv = reference_uv(point.direction);
          auto iso = surface_isocurve(support, 0, point.uv.x, max_error, r_error);
          if (!iso) {
            return false;
          }
          const auto evaluated = evaluate_curve_bounded(
              *iso->curve, point.uv.y, max_error, r_error);
          if (!evaluated) {
            return false;
          }
          const double error = add_upper(iso->geometry_error_bound, evaluated->error_bound);
          auto *created = vertex(
              builder, evaluated->position, add_upper(face_template.tolerance, error));
          point.vertex = created->id;
          cut.edit.vertices.append(created->id);
          cut.edit.geometry_error_bound = std::max(cut.edit.geometry_error_bound, error);
        }
      }
    }

    Vector<Vector<CornerNode *>> polygons;
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < half; j++) {
        for (int k = 0; k < half + odd; k++) {
          polygons.append(
              {&node(i, j, k), &node(i, j, k + 1), &node(i, j + 1, k + 1), &node(i, j + 1, k)});
        }
      }
    }
    if (odd) {
      polygons.append({&node(0, half, half), &node(1, half, half), &node(2, half, half)});
    }
    auto get_edge = [&](const CornerNode &a, const CornerNode &b) -> std::optional<CornerEdge> {
      const auto pair = key(a.vertex, b.vertex);
      if (const auto *existing = corner_edges.lookup_ptr(pair)) {
        return *existing;
      }
      if (!a.vertex || !b.vertex || a.vertex == b.vertex ||
          ELEM(corner.stations[2], a.vertex, b.vertex))
      {
        return std::nullopt;
      }
      auto curve = surface_parameter_line(support, a.uv, b.uv, max_error, r_error);
      if (!curve) {
        return std::nullopt;
      }
      const auto *created = edge(builder,
                                 a.vertex,
                                 b.vertex,
                                 std::move(curve->curve),
                                 add_upper(face_template.tolerance, curve->geometry_error_bound));
      const auto *pcurve = install_curve(
          builder, line({a.uv.x, a.uv.y, 0}, {b.uv.x, b.uv.y, 0}, {0, 1}, 2));
      CornerEdge result{created->id, pcurve->id};
      corner_edges.add_new(pair, result);
      cut.edit.edges.append(created->id);
      cut.edit.geometry_error_bound = std::max(cut.edit.geometry_error_bound,
                                               curve->geometry_error_bound);
      return result;
    };

    for (auto &loop : face->loops) {
      BLI_freelistN(&loop.edge_uses);
    }
    BLI_freelistN(&face->loops);
    BLI_remlink(&builder.geometry->faces, face);
    MEM_delete(face);
    Vector<int64_t> descendants;
    for (const auto &polygon : polygons) {
      auto *child = MEM_new<NurbsTopologyFace>(__func__, face_template);
      child->id = descendants.is_empty() ? corner.face_id : builder.id();
      child->surface_id = builder.copy_surface(support_id)->id;
      child->loops.clear_no_delete();
      BLI_addtail(&builder.geometry->faces, child);
      builder.faces.add_overwrite(child->id, child);
      descendants.append(child->id);
      cut.edit.faces.append(child->id);
      auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
      loop->id = builder.id();
      BLI_addtail(&child->loops, loop);
      for (const int k : polygon.index_range()) {
        const CornerNode &a = *polygon[k], &b = *polygon[(k + 1) % polygon.size()];
        const auto boundary = get_edge(a, b);
        if (!boundary) {
          if (r_error && r_error->empty()) {
            *r_error = "Rounded NURBS mesh-style corner could not construct a shared edge";
          }
          return false;
        }
        auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
        use->id = builder.id();
        use->edge_id = boundary->edge;
        use->pcurve_id = boundary->pcurve;
        use->reversed = builder.edges.lookup(boundary->edge)->vertex_ids[0] != a.vertex;
        use->tolerance = add_upper(face_template.tolerance, cut.edit.geometry_error_bound);
        BLI_addtail(&loop->edge_uses, use);
      }
    }
    cut.face_descendants.add_overwrite(corner.face_id, std::move(descendants));
  }
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry, r_error) ||
      !std::isfinite(cut.edit.geometry_error_bound) || cut.edit.geometry_error_bound > max_error)
  {
    return fail("Rounded NURBS mesh-style corner exceeds its construction budget");
  }
  cut.edit.geometry = std::move(builder.geometry);
  return true;
}

/** Offset face corners first, then share stations through their original endpoint/edge fan.
 * This is the planar counterpart of BMesh's boundary/offset-meet phase. It deliberately uses
 * source incidence, rather than beveling each selected edge independently and sewing results.
 */
static std::optional<TopologyEditResult> bevel_planar_network(const NurbsGeometry &source,
                                                              const TopologyIndex &index,
                                                              const Set<int64_t> &selected,
                                                              BevelOptions options,
                                                              std::string *r_error,
                                                              BevelOffsetInfo *r_offset_info)
{
  auto fail = [&](const char *message) -> std::optional<TopologyEditResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  const bool rounded = options.segments > 1;
  Map<int64_t, int> selected_count;
  Set<int64_t> affected;
  for (const auto &item : source.edges) {
    if (selected.contains(item.id)) {
      for (const int64_t v : item.vertex_ids) {
        affected.add(v);
        selected_count.lookup_or_add(v, 0)++;
      }
    }
  }
  for (const int64_t v : affected) {
    if (index.edges_for_vertex(v).size() < 3) {
      return fail("Connected NURBS bevel requires regular endpoint fans");
    }
    if (rounded && index.edges_for_vertex(v).size() != 3) {
      return fail("Rounded NURBS bevel needs a corner construction for this higher-valence fan");
    }
    for (const int64_t id : index.edges_for_vertex(v)) {
      if (index.uses_for_edge(id).size() > 2) {
        return fail("Connected NURBS bevel cannot join a non-manifold endpoint fan");
      }
    }
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const auto &item : source.curves) {
    curves.add_new(item.id, &item);
  }
  for (const auto &item : source.surfaces) {
    surfaces.add_new(item.id, &item);
  }
  uint64_t required = uint64_t(source.edges.count()) * 16 + uint64_t(affected.size()) * 128;
  for (const auto &face : source.faces) {
    for (const auto &loop : face.loops) {
      required += uint64_t(loop.edge_uses.count()) * 16;
    }
  }
  if (required >= uint64_t(std::numeric_limits<int64_t>::max() - source.next_id)) {
    return fail("NURBS bevel source identity range is exhausted");
  }
  TopologyBuilder builder(source);
  TopologyEditResult result;
  Map<std::pair<int64_t, int64_t>, std::array<int, 2>> width_slots;
  Vector<BoundedScalar> widths;
  Vector<std::array<int, 2>> width_pairs;
  for (const auto &edge : source.edges) {
    if (!selected.contains(edge.id)) {
      continue;
    }
    const auto uses = index.uses_for_edge(edge.id);
    if (uses.size() != 2 || uses[0].face_id == uses[1].face_id) {
      return fail("NURBS bevel requires two distinct incident faces per selected edge");
    }
    for (const auto &use : uses) {
      if (widths.size() > std::numeric_limits<int>::max() - 2) {
        return fail("NURBS bevel width storage exceeds the solver index range");
      }
      const std::array<int, 2> slots{int(widths.size()), int(widths.size() + 1)};
      width_slots.add_new({edge.id, use.face_id}, slots);
      width_pairs.append(slots);
      widths.append({1});
      widths.append({1});
    }
  }
  auto width_slot = [&](const int64_t edge, const int64_t face, const int64_t vertex) {
    return width_slots.lookup({edge, face})[index.edge(edge)->vertex_ids[0] == vertex ? 0 : 1];
  };
  Map<std::pair<int64_t, int64_t>, Vector<std::pair<int, BoundedScalar>>> ray_widths;
  Map<int64_t, Vector<PlanarBevelCorner>> face_corners;
  Map<std::pair<int64_t, int64_t>, int64_t> face_stations, edge_stations;
  for (const auto &face : source.faces) {
    bool changed = false;
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        const auto &boundary = *index.edge(use.edge_id);
        changed |= affected.contains(boundary.vertex_ids[0]) ||
                   affected.contains(boundary.vertex_ids[1]);
      }
    }
    if (!changed) {
      continue;
    }
    if (!face.loops.is_single() ||
        static_cast<const NurbsTopologyLoop *>(face.loops.first)->is_inner)
    {
      return fail("Connected NURBS bevel requires simple planar face boundaries");
    }
    Vector<const NurbsTopologyEdgeUse *> uses;
    bool offset_face = false;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      uses.append(&use);
      offset_face |= selected.contains(use.edge_id);
    }
    if (uses.size() < 3) {
      return fail("Connected NURBS bevel requires nondegenerate face boundaries");
    }
    Vector<PlanarBevelCorner> corners;
    Vector<double3> positions;
    for (const int i : uses.index_range()) {
      const auto &boundary = *index.edge(uses[i]->edge_id);
      const auto &curve = *curves.lookup(boundary.curve_id);
      if (!full_line(curve, boundary) ||
          double3(curve.points[0].position) !=
              double3(index.vertex(boundary.vertex_ids[0])->position) ||
          double3(curve.points[1].position) !=
              double3(index.vertex(boundary.vertex_ids[1])->position))
      {
        return fail("Connected NURBS bevel requires full straight source boundaries");
      }
      const int64_t v = boundary.vertex_ids[uses[i]->reversed ? 1 : 0];
      corners.append({v, uses[(i + uses.size() - 1) % uses.size()]->edge_id, boundary.id});
      positions.append(double3(index.vertex(v)->position));
    }
    double3 normal(0);
    for (int i = 1; i + 1 < positions.size() && normal == double3(0); i++) {
      normal = math::cross(positions[i] - positions[0], positions[i + 1] - positions[0]);
    }
    if (normal == double3(0) || !std::isfinite(math::length_squared(normal))) {
      return fail("Connected NURBS bevel has no regular planar face frame");
    }
    const int omitted = math::dominant_axis(normal);
    const int x = (omitted + 1) % 3, y = (omitted + 2) % 3;
    int turn = 0;
    for (const int i : positions.index_range()) {
      const auto a = positions[i], b = positions[(i + 1) % positions.size()],
                 c = positions[(i + 2) % positions.size()];
      const int current = orient2d({a[x], a[y]}, {b[x], b[y]}, {c[x], c[y]});
      if (offset_face && current && turn && current != turn) {
        return fail("Connected NURBS bevel currently requires convex planar face boundaries");
      }
      if (current) {
        turn = current;
      }
    }
    if (!project_curve_to_affine_surface(*curves.lookup(index.edge(uses[0]->edge_id)->curve_id),
                                         *surfaces.lookup(face.surface_id),
                                         options.max_error,
                                         r_error))
    {
      return std::nullopt;
    }
    for (const int i : corners.index_range()) {
      auto &corner = corners[i];
      const bool incoming = selected.contains(corner.incoming);
      const bool outgoing = selected.contains(corner.outgoing);
      if (!incoming && !outgoing) {
        continue;
      }
      const double3 origin = positions[i];
      const auto a = difference(positions[(i + positions.size() - 1) % positions.size()], origin);
      const auto b = difference(positions[(i + 1) % positions.size()], origin);
      for (int side = 0; side < 2; side++) {
        if (!(side == 0 ? outgoing : incoming)) {
          continue;
        }
        const auto &along = side == 0 ? a : b;
        const auto &axis = side == 0 ? b : a;
        const auto fraction = bounded_divide(dot(along, axis), dot(axis, axis));
        BoundedVector perpendicular;
        for (int axis_index = 0; axis_index < 3; axis_index++) {
          perpendicular[axis_index] = bounded_subtract(
              along[axis_index], bounded_multiply(axis[axis_index], fraction));
        }
        corner.heights[side] = root(dot(perpendicular, perpendicular));
        if (!corner.heights[side].valid() ||
            corner.heights[side].value <= corner.heights[side].error)
        {
          return fail("NURBS bevel requires a regular face corner");
        }
      }
      int64_t station = 0;
      const int64_t unselected = incoming && outgoing ? 0 :
                                 incoming             ? corner.outgoing :
                                                        corner.incoming;
      if (unselected) {
        station = edge_stations.lookup_default({corner.vertex, unselected}, 0);
        ray_widths.lookup_or_add_default({corner.vertex, unselected})
            .append(
                {width_slot(incoming ? corner.incoming : corner.outgoing, face.id, corner.vertex),
                 corner.heights[incoming ? 1 : 0]});
      }
      if (!station) {
        station = vertex(builder, origin, 0)->id;
        result.vertices.append(station);
        if (unselected) {
          edge_stations.add_new({corner.vertex, unselected}, station);
        }
      }
      face_stations.add_new({corner.vertex, face.id}, station);
    }
    face_corners.add_new(face.id, std::move(corners));
  }
  Vector<BevelWidthLink> width_links;
  for (const auto &items : ray_widths.values()) {
    if (items.size() > 2) {
      return fail("NURBS bevel has a non-manifold width constraint");
    }
    if (items.size() == 2) {
      width_links.append({items[0].first, items[1].first, items[0].second, items[1].second});
    }
  }
  if (!balance_bevel_widths(width_pairs, width_links, 1, widths, r_error)) {
    return std::nullopt;
  }
  Map<int64_t, BevelStationMotion> motions;
  BevelOffsetLimit limit;
  for (const auto &face : source.faces) {
    const auto *corners = face_corners.lookup_ptr(face.id);
    if (!corners) {
      continue;
    }
    for (const auto &corner : *corners) {
      const auto *id = face_stations.lookup_ptr({corner.vertex, face.id});
      if (!id) {
        continue;
      }
      const double3 origin(index.vertex(corner.vertex)->position);
      const auto &incoming = *index.edge(corner.incoming);
      const auto &outgoing = *index.edge(corner.outgoing);
      const int64_t a_id = incoming.vertex_ids[incoming.vertex_ids[0] == corner.vertex ? 1 : 0];
      const int64_t b_id = outgoing.vertex_ids[outgoing.vertex_ids[0] == corner.vertex ? 1 : 0];
      const auto a = difference(double3(index.vertex(a_id)->position), origin);
      const auto b = difference(double3(index.vertex(b_id)->position), origin);
      const auto first = selected.contains(corner.outgoing) ?
                             bounded_divide(
                                 widths[width_slot(corner.outgoing, face.id, corner.vertex)],
                                 corner.heights[0]) :
                             BoundedScalar{};
      const auto second = selected.contains(corner.incoming) ?
                              bounded_divide(
                                  widths[width_slot(corner.incoming, face.id, corner.vertex)],
                                  corner.heights[1]) :
                              BoundedScalar{};
      for (const auto factor : {first, second}) {
        if (!factor.valid() || factor.value < factor.error || !limit.add({1}, factor)) {
          return fail("NURBS bevel cannot bound its corner motion");
        }
      }
      BoundedVector direction;
      for (int axis = 0; axis < 3; axis++) {
        direction[axis] = bounded_add(bounded_multiply(a[axis], first),
                                      bounded_multiply(b[axis], second));
      }
      if (auto *shared = motions.lookup_ptr(*id)) {
        /* Like BMesh's offset_on_edge_between, average the two incidence-defined meets.
         * This shares topology intentionally; it never infers identity from proximity. */
        for (int axis = 0; axis < 3; axis++) {
          shared->direction[axis] = bounded_multiply(
              bounded_add(shared->direction[axis], direction[axis]), {0.5});
        }
      }
      else {
        motions.add_new(*id, {corner.vertex, direction});
      }
    }
  }
  /* Plan terminal stations before limiting, including higher-valence chamfer fans. */
  for (const auto &original : source.edges) {
    if (selected.contains(original.id)) {
      continue;
    }
    for (int end = 0; end < 2; end++) {
      const int64_t v = original.vertex_ids[end];
      if (!affected.contains(v) || edge_stations.contains({v, original.id})) {
        continue;
      }
      const double3 origin(index.vertex(v)->position);
      const auto along = difference(double3(index.vertex(original.vertex_ids[1 - end])->position),
                                    origin);
      const auto rate = bounded_divide({1}, root(dot(along, along)));
      if (!limit.add({1}, rate)) {
        return fail("NURBS bevel cannot bound its terminal motion");
      }
      BoundedVector direction;
      for (int axis = 0; axis < 3; axis++) {
        direction[axis] = bounded_multiply(along[axis], rate);
      }
      const int64_t station = vertex(builder, origin, 0)->id;
      result.vertices.append(station);
      edge_stations.add_new({v, original.id}, station);
      motions.add_new(station, {v, direction});
    }
  }
  /* As in BMesh's geometry_collide_offset, limit the entire network at its first edge
   * collapse or slide collision. The coefficients come from the native NURBS source plan;
   * no polygon copy or display mesh participates in geometry construction. */
  auto limit_edge = [&](const NurbsTopologyEdge &original, const int64_t a, const int64_t b) {
    const double3 start(index.vertex(original.vertex_ids[0])->position);
    const double3 end(index.vertex(original.vertex_ids[1])->position);
    const auto direction = difference(end, start);
    BoundedVector closing;
    double clearance = 0;
    for (int axis = 0; axis < 3; axis++) {
      const auto *am = motions.lookup_ptr(a), *bm = motions.lookup_ptr(b);
      const auto av = am ? am->direction[axis] : BoundedScalar{};
      const auto bv = bm ? bm->direction[axis] : BoundedScalar{};
      closing[axis] = bounded_subtract(av, bv);
      clearance = add_upper(
          clearance,
          multiply_upper(add_upper(std::abs(direction[axis].value), direction[axis].error),
                         add_upper(station_roundoff(start[axis], av, limit.offset),
                                   station_roundoff(end[axis], bv, limit.offset))));
    }
    return limit.add(dot(direction, direction), dot(closing, direction), clearance);
  };
  for (const auto &original : source.edges) {
    if (selected.contains(original.id)) {
      for (const auto &use : index.uses_for_edge(original.id)) {
        if (!limit_edge(original,
                        face_stations.lookup({original.vertex_ids[0], use.face_id}),
                        face_stations.lookup({original.vertex_ids[1], use.face_id})))
        {
          return fail("NURBS bevel cannot certify its rail collision limit");
        }
      }
    }
    else if (affected.contains(original.vertex_ids[0]) ||
             affected.contains(original.vertex_ids[1]))
    {
      if (!limit_edge(original,
                      edge_stations.lookup_default({original.vertex_ids[0], original.id}, 0),
                      edge_stations.lookup_default({original.vertex_ids[1], original.id}, 0)))
      {
        return fail("NURBS bevel cannot certify its boundary collision limit");
      }
    }
  }
  if (!std::isfinite(limit.offset) || !(limit.offset > 0)) {
    return fail("NURBS bevel has no positive finite collision-free width");
  }
  if (options.offset > limit.offset) {
    if (!options.clamp_overlap) {
      return fail(
          "NURBS bevel width reaches neighboring geometry; enable Clamp Overlap or reduce width");
    }
    options.offset = limit.offset;
  }
  for (const auto &item : motions.items()) {
    auto &station = *builder.vertices.lookup(item.key);
    const auto &motion = item.value;
    const auto &origin = *index.vertex(motion.vertex);
    double error = 0;
    for (int axis = 0; axis < 3; axis++) {
      const auto coordinate = bounded_add(
          {origin.position[axis]}, bounded_multiply(motion.direction[axis], {options.offset}));
      if (!coordinate.valid()) {
        return fail("NURBS bevel station is not representable");
      }
      station.position[axis] = coordinate.value;
      error = add_upper(error, coordinate.error);
    }
    station.tolerance = error;
    result.geometry_error_bound = std::max(result.geometry_error_bound, error);
    if (result.geometry_error_bound > options.max_error) {
      return fail("NURBS bevel station exceeds the source construction error budget");
    }
  }
  Map<int64_t, RoundBevelCorner> round_corners;
  int boundary_blend_corners = 0;
  const double station_error = result.geometry_error_bound;
  auto profile_weight = [&](const NurbsTopologyEdge &original,
                            const int64_t v) -> std::optional<BoundedScalar> {
    const auto uses = index.uses_for_edge(original.id);
    const double3 origin(index.vertex(v)->position);
    const auto direction = difference(
        double3(index.vertex(original.vertex_ids[original.vertex_ids[0] == v ? 1 : 0])->position),
        origin);
    std::array<BoundedVector, 2> inward;
    for (int side = 0; side < 2; side++) {
      int64_t neighbor = 0;
      for (const auto &corner : face_corners.lookup(uses[side].face_id)) {
        if (corner.vertex == v) {
          neighbor = corner.incoming == original.id ? corner.outgoing : corner.incoming;
        }
      }
      if (!neighbor) {
        return std::nullopt;
      }
      const auto &item = *index.edge(neighbor);
      const int64_t far = item.vertex_ids[item.vertex_ids[0] == v ? 1 : 0];
      const auto along = difference(double3(index.vertex(far)->position), origin);
      const auto fraction = bounded_divide(dot(along, direction), dot(direction, direction));
      BoundedVector perpendicular;
      for (int axis = 0; axis < 3; axis++) {
        perpendicular[axis] = bounded_subtract(along[axis],
                                               bounded_multiply(direction[axis], fraction));
      }
      const auto height = root(dot(perpendicular, perpendicular));
      for (int axis = 0; axis < 3; axis++) {
        inward[side][axis] = bounded_divide(perpendicular[axis], height);
      }
    }
    const auto weight = root(
        bounded_multiply({0.5}, bounded_subtract({1}, dot(inward[0], inward[1]))));
    if (!weight.valid() || weight.value <= weight.error ||
        add_upper(weight.value, weight.error) >= 1)
    {
      return std::nullopt;
    }
    return weight;
  };
  Map<int64_t, BoundedScalar> joint_weights;
  if (rounded) {
    /* A two-edge junction has one shared end profile. Plan it once from the fan,
     * rather than constructing two different curves and trying to merge them by
     * endpoints afterward. Unequal incident rounds use their homogeneous conic
     * weight mean; the strips interpolate that prescribed common profile. */
    for (const auto &original : source.vertices) {
      if (selected_count.lookup_default(original.id, 0) != 2) {
        continue;
      }
      std::array<BoundedScalar, 2> weights;
      int count = 0;
      for (const int64_t id : index.edges_for_vertex(original.id)) {
        if (!selected.contains(id)) {
          continue;
        }
        const auto weight = profile_weight(*index.edge(id), original.id);
        if (!weight) {
          return fail("Rounded NURBS junction has an uncertifiable conic angle");
        }
        weights[count++] = *weight;
      }
      joint_weights.add_new(original.id,
                            bounded_multiply(bounded_add(weights[0], weights[1]), {0.5}));
      if (std::abs(weights[0].value - weights[1].value) >
          add_upper(weights[0].error, weights[1].error))
      {
        boundary_blend_corners++;
      }
    }
    for (const auto &original : source.vertices) {
      if (selected_count.lookup_default(original.id, 0) != 3) {
        continue;
      }
      std::array<BoundedVector, 3> directions;
      int direction = 0;
      for (const int64_t id : index.edges_for_vertex(original.id)) {
        const auto &item = *index.edge(id);
        const int64_t far = item.vertex_ids[item.vertex_ids[0] == original.id ? 1 : 0];
        const auto &endpoint = *index.vertex(far);
        auto &along = directions[direction++];
        along = difference(double3(endpoint.position), double3(original.position));
        for (auto &coordinate : along) {
          coordinate.error = add_upper(coordinate.error,
                                       add_upper(endpoint.tolerance, original.tolerance));
        }
      }
      bool orthogonal = true;
      for (int i = 0; i < 3; i++) {
        const auto perpendicular = dot(directions[i], directions[(i + 1) % 3]);
        orthogonal &= perpendicular.valid() &&
                      std::abs(perpendicular.value) <= perpendicular.error;
      }
      RoundBevelCorner corner;
      std::array<int64_t, 3> face_ids{};
      int station = 0;
      for (const auto &face : source.faces) {
        if (const auto *id = face_stations.lookup_ptr({original.id, face.id})) {
          if (station >= 3) {
            return fail("Rounded NURBS corner has an ambiguous incident face fan");
          }
          face_ids[station] = face.id;
          corner.stations[station++] = *id;
        }
      }
      if (station != 3) {
        return fail("Rounded NURBS corner requires three incident face stations");
      }
      if (!orthogonal) {
        /* As in a fillet kernel's analytic-then-boundary-patch strategy, retain the
         * spherical construction when valid and interpolate the actual shared conics
         * otherwise. This is a Coons corner blend, not a claimed spherical fillet. */
        std::array<OwnedCurve, 3> profiles;
        std::array<BoundedScalar, 3> profile_weights;
        double profile_error = station_error;
        const int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
        for (int side = 0; side < 3; side++) {
          const int a = pairs[side][0], b = pairs[side][1];
          const NurbsTopologyEdge *source_edge = nullptr;
          for (const int64_t edge_id : index.edges_for_vertex(original.id)) {
            const auto uses = index.uses_for_edge(edge_id);
            if ((uses[0].face_id == face_ids[a] && uses[1].face_id == face_ids[b]) ||
                (uses[1].face_id == face_ids[a] && uses[0].face_id == face_ids[b]))
            {
              source_edge = index.edge(edge_id);
              break;
            }
          }
          if (!source_edge) {
            return fail("Rounded NURBS corner has no unique shared source edge");
          }
          const auto weight = profile_weight(*source_edge, original.id);
          if (!weight) {
            return fail("Rounded NURBS corner has an uncertifiable conic angle");
          }
          profile_weights[side] = *weight;
          const double3 pa(builder.vertices.lookup(corner.stations[a])->position);
          const double3 pb(builder.vertices.lookup(corner.stations[b])->position);
          const double3 origin(original.position);
          const auto along = difference(
              double3(
                  index
                      .vertex(source_edge
                                  ->vertex_ids[source_edge->vertex_ids[0] == original.id ? 1 : 0])
                      ->position),
              origin);
          const auto mean = bounded_multiply(
              bounded_add(dot(difference(pa, origin), along), dot(difference(pb, origin), along)),
              {0.5});
          const auto parameter = bounded_divide(mean, dot(along, along));
          auto &curve = profiles[side];
          curve.reset(MEM_new<NurbsCurveGeometry>(__func__));
          curve->dimension = 3;
          curve->order = curve->points_num = 3;
          curve->knots_num = 6;
          curve->points = MEM_new_array<NurbsControlPoint>(3, __func__);
          curve->knots = MEM_new_array_zeroed<double>(6, __func__);
          curve->knots[3] = curve->knots[4] = curve->knots[5] = 1;
          std::copy_n(&pa.x, 3, curve->points[0].position);
          std::copy_n(&pb.x, 3, curve->points[2].position);
          double error = station_error;
          for (int axis = 0; axis < 3; axis++) {
            const auto coordinate = bounded_add({origin[axis]},
                                                bounded_multiply(along[axis], parameter));
            if (!coordinate.valid()) {
              return fail("Rounded NURBS corner conic pole is not representable");
            }
            curve->points[1].position[axis] = coordinate.value;
            error = add_upper(error, coordinate.error);
          }
          curve->points[1].weight = weight->value;
          const double extent = std::max(distance_upper(double3(curve->points[1].position), pa),
                                         distance_upper(double3(curve->points[1].position), pb));
          const auto perturbation = bounded_divide(
              bounded_multiply({2 * extent}, {weight->error}),
              bounded_subtract({weight->value}, {weight->error}));
          error = add_upper(error, add_upper(std::abs(perturbation.value), perturbation.error));
          profile_error = std::max(profile_error, error);
        }
        /* Correct the interior tensor coefficient and collapsed-edge weight so this
         * family meets the exact spherical octant continuously at a right angle.
         * The interior control remains at the original source vertex. */
        const auto interior_weight = bounded_multiply(
            profile_weights[0],
            bounded_multiply(bounded_add(profile_weights[1], profile_weights[2]), {0.5}));
        TriangleBlendShape shape;
        std::copy_n(original.position, 3, shape.interior.position);
        shape.interior.weight = interior_weight.value;
        shape.collapsed_weight = profile_weights[0].value;
        double minimum_weight = interior_weight.value - interior_weight.error;
        double extent = 0;
        for (int side = 0; side < 3; side++) {
          minimum_weight = std::min(minimum_weight,
                                    profile_weights[side].value - profile_weights[side].error);
          for (const auto &point : Span(profiles[side]->points, 3)) {
            extent = std::max(extent,
                              distance_upper(double3(original.position), double3(point.position)));
          }
        }
        const auto shape_error = bounded_divide(
            bounded_multiply({2 * extent},
                             {add_upper(interior_weight.error, profile_weights[0].error)}),
            {minimum_weight});
        if (!(minimum_weight > 0) || !shape_error.valid()) {
          return fail("Rounded NURBS corner cannot certify positive blend coefficients");
        }
        profile_error = add_upper(profile_error,
                                  add_upper(std::abs(shape_error.value), shape_error.error));
        auto constructed = rational_coons_triangle(
            *profiles[0], *profiles[1], *profiles[2], options.max_error, r_error, &shape);
        if (!constructed) {
          return std::nullopt;
        }
        result.geometry_error_bound = std::max(
            result.geometry_error_bound,
            add_upper(profile_error, constructed->geometry_error_bound));
        if (result.geometry_error_bound > options.max_error) {
          return fail("Rounded NURBS corner blend exceeds the source construction error budget");
        }
        corner.support = std::move(constructed->surface);
        round_corners.add_new(original.id, std::move(corner));
        boundary_blend_corners++;
        continue;
      }
      const double input_error = station_error;
      BoundedVector center_bound;
      double3 center;
      std::array<double3, 3> axes;
      double transport_error = 0;
      for (int axis = 0; axis < 3; axis++) {
        BoundedScalar sum{-original.position[axis]};
        for (const int64_t id : corner.stations) {
          sum = bounded_add(sum, {builder.vertices.lookup(id)->position[axis], input_error});
        }
        center_bound[axis] = bounded_multiply(sum, {0.5});
        if (!center_bound[axis].valid()) {
          return fail("Rounded NURBS corner center is not representable");
        }
        center[axis] = center_bound[axis].value;
        transport_error = add_upper(transport_error, center_bound[axis].error);
      }
      for (int i = 0; i < 3; i++) {
        for (int axis = 0; axis < 3; axis++) {
          const auto value = bounded_subtract(
              {builder.vertices.lookup(corner.stations[i])->position[axis], input_error},
              center_bound[axis]);
          if (!value.valid()) {
            return fail("Rounded NURBS corner frame is not representable");
          }
          axes[i][axis] = value.value;
          transport_error = add_upper(transport_error, value.error);
        }
      }
      auto constructed = rational_spherical_octant_surface(
          center, axes, options.offset, options.max_error, r_error);
      if (!constructed) {
        return std::nullopt;
      }
      double endpoint_error = 0;
      for (const auto &[slot, endpoint] : {std::pair{0, 0}, {2, 1}, {6, 2}, {7, 2}, {8, 2}}) {
        auto &point = constructed->surface->points[slot];
        const auto &position = builder.vertices.lookup(corner.stations[endpoint])->position;
        endpoint_error = std::max(endpoint_error,
                                  distance_upper(double3(point.position), double3(position)));
        std::copy_n(position, 3, point.position);
      }
      result.geometry_error_bound = std::max(
          result.geometry_error_bound,
          add_upper(transport_error,
                    add_upper(constructed->geometry_error_bound, endpoint_error)));
      if (result.geometry_error_bound > options.max_error) {
        return fail("Rounded NURBS corner exceeds the source construction error budget");
      }
      corner.support = std::move(constructed->surface);
      round_corners.add_new(original.id, std::move(corner));
    }
  }
  Map<std::pair<int64_t, int64_t>, int64_t> edges_by_vertices;
  Vector<VertexMergeGroup> miter_groups;
  double miter_error = 0;
  if (!rounded) {
    for (const auto &original : source.vertices) {
      if (selected_count.lookup_default(original.id, 0) < 2) {
        continue;
      }
      Map<int64_t, Vector<int64_t>> adjacency;
      for (const auto &face : source.faces) {
        if (const auto *corners = face_corners.lookup_ptr(face.id)) {
          for (const auto &corner : *corners) {
            if (corner.vertex == original.id && !selected.contains(corner.incoming) &&
                !selected.contains(corner.outgoing))
            {
              adjacency.lookup_or_add_default(corner.incoming).append(corner.outgoing);
              adjacency.lookup_or_add_default(corner.outgoing).append(corner.incoming);
            }
          }
        }
      }
      Set<int64_t> visited;
      for (const int64_t start : index.edges_for_vertex(original.id)) {
        if (selected.contains(start) || !visited.add(start)) {
          continue;
        }
        Vector<int64_t> component{start};
        for (int i = 0; i < component.size(); i++) {
          if (const auto *neighbors = adjacency.lookup_ptr(component[i])) {
            for (const int64_t next : *neighbors) {
              if (visited.add(next)) {
                component.append(next);
              }
            }
          }
        }
        if (component.size() < 2) {
          continue;
        }
        Vector<std::pair<int64_t, int64_t>> sides;
        Vector<int64_t> between, adjacent;
        for (const auto &face : source.faces) {
          if (const auto *corners = face_corners.lookup_ptr(face.id)) {
            for (const auto &corner : *corners) {
              if (corner.vertex != original.id) {
                continue;
              }
              const bool incoming = component.contains(corner.incoming);
              const bool outgoing = component.contains(corner.outgoing);
              if (incoming && outgoing) {
                between.append(face.id);
              }
              else if (incoming && selected.contains(corner.outgoing)) {
                sides.append(
                    {corner.outgoing, edge_stations.lookup({original.id, corner.incoming})});
                adjacent.append(face.id);
              }
              else if (outgoing && selected.contains(corner.incoming)) {
                sides.append(
                    {corner.incoming, edge_stations.lookup({original.id, corner.outgoing})});
                adjacent.append(face.id);
              }
            }
          }
        }
        if (sides.size() != 2) {
          return fail("NURBS bevel cannot resolve this open or branching miter fan");
        }
        std::array<BoundedVector, 2> directions, points;
        for (int i = 0; i < 2; i++) {
          const auto &edge = *index.edge(sides[i].first);
          const int64_t far = edge.vertex_ids[edge.vertex_ids[0] == original.id ? 1 : 0];
          directions[i] = difference(double3(index.vertex(far)->position),
                                     double3(original.position));
          const auto &point = *builder.vertices.lookup(sides[i].second);
          for (int axis = 0; axis < 3; axis++) {
            points[i][axis] = {point.position[axis], point.tolerance};
          }
        }
        BoundedVector delta;
        for (int axis = 0; axis < 3; axis++) {
          delta[axis] = bounded_subtract(points[1][axis], points[0][axis]);
        }
        const auto a = dot(directions[0], directions[0]);
        const auto b = dot(directions[0], directions[1]);
        const auto c = dot(directions[1], directions[1]);
        const auto d = dot(directions[0], delta), e = dot(directions[1], delta);
        const auto determinant = bounded_subtract(bounded_multiply(a, c), bounded_multiply(b, b));
        const BoundedScalar parameters[] = {
            bounded_divide(bounded_subtract(bounded_multiply(c, d), bounded_multiply(b, e)),
                           determinant),
            bounded_divide(bounded_subtract(bounded_multiply(b, d), bounded_multiply(a, e)),
                           determinant)};
        BoundedVector meet;
        for (int axis = 0; axis < 3; axis++) {
          meet[axis] = bounded_multiply(
              bounded_add(bounded_add(points[0][axis],
                                      bounded_multiply(directions[0][axis], parameters[0])),
                          bounded_add(points[1][axis],
                                      bounded_multiply(directions[1][axis], parameters[1]))),
              {0.5});
        }
        between.extend(adjacent);
        bool found = false;
        double3 target;
        double error = 0;
        for (const int64_t face_id : between) {
          const auto &support = *surfaces.lookup(index.face(face_id)->surface_id);
          const double3 origin(support.points[0].position);
          const auto normal = cross(difference(double3(support.points[1].position), origin),
                                    difference(double3(support.points[2].position), origin));
          BoundedVector displacement;
          for (int axis = 0; axis < 3; axis++) {
            displacement[axis] = bounded_subtract(meet[axis], {origin[axis]});
          }
          const auto amount = bounded_divide(dot(displacement, normal), dot(normal, normal));
          double3 point, plane_normal;
          double point_error = 0;
          bool valid = true;
          for (int axis = 0; axis < 3; axis++) {
            const auto coordinate = bounded_subtract(meet[axis],
                                                     bounded_multiply(normal[axis], amount));
            valid &= coordinate.valid();
            point[axis] = coordinate.value;
            plane_normal[axis] = normal[axis].value;
            point_error = add_upper(point_error, coordinate.error);
          }
          if (!valid) {
            continue;
          }
          const int omitted = math::dominant_axis(plane_normal);
          const int x = (omitted + 1) % 3, y = (omitted + 2) % 3;
          for (const auto &corner : face_corners.lookup(face_id)) {
            if (corner.vertex != original.id) {
              continue;
            }
            const auto &first = *index.edge(corner.incoming), &last = *index.edge(corner.outgoing);
            const double3 p(
                index.vertex(first.vertex_ids[first.vertex_ids[0] == original.id ? 1 : 0])
                    ->position);
            const double3 q(
                index.vertex(last.vertex_ids[last.vertex_ids[0] == original.id ? 1 : 0])
                    ->position);
            const double2 v(original.position[x], original.position[y]);
            const int turn = orient2d(v, {p[x], p[y]}, {q[x], q[y]});
            if (turn && turn * orient2d(v, {p[x], p[y]}, {point[x], point[y]}) >= 0 &&
                turn * orient2d(v, {point[x], point[y]}, {q[x], q[y]}) >= 0)
            {
              target = point;
              error = point_error;
              found = true;
            }
          }
          if (found) {
            break;
          }
        }
        if (!found || error > options.max_error) {
          return fail(
              "NURBS bevel cannot certify this miter intersection within its error budget");
        }
        VertexMergeGroup group;
        for (const int64_t edge : component) {
          group.vertices.append(edge_stations.lookup({original.id, edge}));
        }
        group.survivor = group.vertices.first();
        group.target = target;
        miter_error = std::max(miter_error, error);
        miter_groups.append(std::move(group));
      }
    }
  }
  auto pair_key = [](const int64_t a, const int64_t b) {
    return std::pair<int64_t, int64_t>{std::min(a, b), std::max(a, b)};
  };
  auto new_edge = [&](const int64_t a, const int64_t b) {
    const auto key = pair_key(a, b);
    if (const auto *id = edges_by_vertices.lookup_ptr(key)) {
      return *id;
    }
    const int64_t id = edge(builder,
                            a,
                            b,
                            line(double3(builder.vertices.lookup(a)->position),
                                 double3(builder.vertices.lookup(b)->position)),
                            result.geometry_error_bound)
                           ->id;
    edges_by_vertices.add_new(key, id);
    result.edges.append(id);
    return id;
  };
  auto new_profile = [&](const int64_t a,
                         const int64_t b,
                         const int64_t source_vertex,
                         BoundedScalar weight) -> int64_t {
    if (!rounded) {
      return new_edge(a, b);
    }
    auto profile_fail = [&](const char *message) {
      if (r_error) {
        *r_error = message;
      }
      return int64_t(0);
    };
    OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
    curve->dimension = 3;
    curve->order = curve->points_num = 3;
    curve->knots_num = 6;
    curve->knots = MEM_new_array_zeroed<double>(6, __func__);
    curve->knots[3] = curve->knots[4] = curve->knots[5] = 1;
    curve->points = MEM_new_array<NurbsControlPoint>(3, __func__);
    double error = station_error;
    if (const auto *corner = round_corners.lookup_ptr(source_vertex)) {
      int first = -1, last = -1;
      for (int i = 0; i < 3; i++) {
        if (corner->stations[i] == a) {
          first = i;
        }
        if (corner->stations[i] == b) {
          last = i;
        }
      }
      if (first < 0 || last < 0 || first == last) {
        return profile_fail("Rounded NURBS corner has an invalid boundary station");
      }
      const int indices[3][3] = {{0, 1, 2}, {0, 3, 6}, {2, 5, 8}};
      const int which = std::min(first, last) == 0 ? std::max(first, last) - 1 : 2;
      for (int i = 0; i < 3; i++) {
        curve->points[i] = corner->support->points[indices[which][first < last ? i : 2 - i]];
      }
      error = result.geometry_error_bound;
    }
    else {
      if (const auto *joint = joint_weights.lookup_ptr(source_vertex)) {
        weight = *joint;
      }
      std::copy_n(builder.vertices.lookup(a)->position, 3, curve->points[0].position);
      std::copy_n(index.vertex(source_vertex)->position, 3, curve->points[1].position);
      std::copy_n(builder.vertices.lookup(b)->position, 3, curve->points[2].position);
      curve->points[1].weight = weight.value;
      const double3 origin(index.vertex(source_vertex)->position);
      const double extent = std::max(distance_upper(origin, double3(curve->points[0].position)),
                                     distance_upper(origin, double3(curve->points[2].position)));
      const auto perturbation = bounded_divide(bounded_multiply({2 * extent}, {weight.error}),
                                               bounded_subtract({weight.value}, {weight.error}));
      error = add_upper(error, add_upper(std::abs(perturbation.value), perturbation.error));
    }
    if (!std::isfinite(error) || error > options.max_error) {
      return profile_fail("Rounded NURBS profile exceeds the source construction error budget");
    }
    const auto key = pair_key(a, b);
    if (const auto *id = edges_by_vertices.lookup_ptr(key)) {
      auto &existing_edge = *builder.edges.lookup(*id);
      const auto &existing = *builder.curves.lookup(existing_edge.curve_id);
      if (existing.order != 3 || existing.points_num != 3) {
        return profile_fail("Rounded NURBS fan has incompatible shared profiles");
      }
      double discrepancy = 0;
      const bool reverse = existing_edge.vertex_ids[0] != a;
      for (int i = 0; i < 3; i++) {
        const auto &point = existing.points[reverse ? 2 - i : i];
        if (point.weight != curve->points[i].weight) {
          return profile_fail(
              "Rounded NURBS miter requires an intersection between unequal profiles");
        }
        discrepancy = std::max(
            discrepancy,
            distance_upper(double3(point.position), double3(curve->points[i].position)));
      }
      error = add_upper(error, discrepancy);
      if (error > options.max_error) {
        return profile_fail("Rounded NURBS miter exceeds the shared profile error budget");
      }
      existing_edge.tolerance = std::max(existing_edge.tolerance, error);
      result.geometry_error_bound = std::max(result.geometry_error_bound, error);
      return *id;
    }
    result.geometry_error_bound = std::max(result.geometry_error_bound, error);
    const int64_t id = edge(builder, a, b, std::move(curve), error)->id;
    edges_by_vertices.add_new(key, id);
    result.edges.append(id);
    return id;
  };
  Set<int64_t> shortened;
  for (const auto &original : source.edges) {
    if (selected.contains(original.id) ||
        (!affected.contains(original.vertex_ids[0]) && !affected.contains(original.vertex_ids[1])))
    {
      continue;
    }
    auto &changed = *builder.edges.lookup(original.id);
    for (int end = 0; end < 2; end++) {
      changed.vertex_ids[end] = edge_stations.lookup_default(
          {original.vertex_ids[end], original.id}, original.vertex_ids[end]);
      if (affected.contains(original.vertex_ids[end]) &&
          changed.vertex_ids[end] == original.vertex_ids[end])
      {
        return fail("Connected NURBS bevel found an incomplete endpoint fan");
      }
    }
    const double3 a(builder.vertices.lookup(changed.vertex_ids[0])->position);
    const double3 b(builder.vertices.lookup(changed.vertex_ids[1])->position);
    const auto remaining = dot(
        difference(b, a),
        difference(double3(index.vertex(original.vertex_ids[1])->position),
                   double3(index.vertex(original.vertex_ids[0])->position)));
    if (!remaining.valid() || remaining.value <= remaining.error) {
      return fail("Selected bevel widths overlap on a neighboring edge");
    }
    changed.curve_id = install_curve(
                           builder,
                           line(a, b, {original.parameter_range[0], original.parameter_range[1]}))
                           ->id;
    changed.tolerance = std::max(changed.tolerance, result.geometry_error_bound);
    shortened.add(changed.id);
    edges_by_vertices.add_new(pair_key(changed.vertex_ids[0], changed.vertex_ids[1]), changed.id);
  }
  Map<std::pair<int64_t, int64_t>, int64_t> rails, profiles;
  Map<int64_t, Vector<int64_t>> corner_profiles;
  Vector<std::pair<int64_t, int64_t>> strip_seeds;
  for (const auto &original : source.edges) {
    if (!selected.contains(original.id)) {
      continue;
    }
    const auto uses = index.uses_for_edge(original.id);
    if (uses.size() != 2 || uses[0].face_id == uses[1].face_id) {
      return fail("Connected NURBS bevel requires two distinct faces per selected edge");
    }
    const int orientation = index.face(uses[0].face_id)->orientation *
                            (uses[0].use->reversed ? -1 : 1);
    if (orientation == index.face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1))
    {
      return fail("Connected NURBS bevel requires consistent adjacent face orientations");
    }
    int64_t stations[2][2];
    BoundedScalar weight{1};
    if (rounded) {
      const int64_t v = original.vertex_ids[0];
      const double3 origin(index.vertex(v)->position);
      const auto direction = difference(double3(index.vertex(original.vertex_ids[1])->position),
                                        origin);
      std::array<BoundedVector, 2> inward;
      for (int side = 0; side < 2; side++) {
        int64_t neighbor = 0;
        for (const auto &corner : face_corners.lookup(uses[side].face_id)) {
          if (corner.vertex == v) {
            neighbor = corner.incoming == original.id ? corner.outgoing : corner.incoming;
          }
        }
        if (!neighbor) {
          return fail("Rounded NURBS bevel could not find an incident face direction");
        }
        const auto &item = *index.edge(neighbor);
        const int64_t far = item.vertex_ids[item.vertex_ids[0] == v ? 1 : 0];
        const auto along = difference(double3(index.vertex(far)->position), origin);
        const auto fraction = bounded_divide(dot(along, direction), dot(direction, direction));
        BoundedVector perpendicular;
        for (int axis = 0; axis < 3; axis++) {
          perpendicular[axis] = bounded_subtract(along[axis],
                                                 bounded_multiply(direction[axis], fraction));
        }
        const auto height = root(dot(perpendicular, perpendicular));
        for (int axis = 0; axis < 3; axis++) {
          inward[side][axis] = bounded_divide(perpendicular[axis], height);
        }
      }
      weight = root(bounded_multiply({0.5}, bounded_subtract({1}, dot(inward[0], inward[1]))));
      if (!weight.valid() || weight.value <= weight.error ||
          add_upper(weight.value, weight.error) >= 1)
      {
        return fail("Rounded NURBS bevel requires a regular non-flat corner angle");
      }
    }
    for (int side = 0; side < 2; side++) {
      for (int end = 0; end < 2; end++) {
        stations[side][end] = face_stations.lookup({original.vertex_ids[end], uses[side].face_id});
      }
      const auto remaining = dot(
          difference(double3(builder.vertices.lookup(stations[side][1])->position),
                     double3(builder.vertices.lookup(stations[side][0])->position)),
          difference(double3(index.vertex(original.vertex_ids[1])->position),
                     double3(index.vertex(original.vertex_ids[0])->position)));
      if (!remaining.valid() || remaining.value <= remaining.error) {
        return fail("Selected bevel widths overlap along a face boundary");
      }
      rails.add_new({original.id, uses[side].face_id},
                    new_edge(stations[side][0], stations[side][1]));
    }
    for (int end = 0; end < 2; end++) {
      const int64_t id = new_profile(
          stations[0][end], stations[1][end], original.vertex_ids[end], weight);
      if (!id) {
        return std::nullopt;
      }
      profiles.add_new({original.id, original.vertex_ids[end]}, id);
      auto &at_vertex = corner_profiles.lookup_or_add_default(original.vertex_ids[end]);
      if (!at_vertex.contains(id)) {
        at_vertex.append(id);
      }
    }
    auto oriented_profile = [&](const int end) {
      const auto &boundary = *builder.edges.lookup(
          profiles.lookup({original.id, original.vertex_ids[end]}));
      const auto &source_curve = *builder.curves.lookup(boundary.curve_id);
      OwnedCurve curve(MEM_dupalloc(&source_curve));
      curve->points = MEM_dupalloc(source_curve.points);
      curve->knots = MEM_dupalloc(source_curve.knots);
      if (boundary.vertex_ids[0] != stations[0][end]) {
        std::reverse(curve->points, curve->points + curve->points_num);
      }
      return curve;
    };
    const auto first_profile = oriented_profile(0), second_profile = oriented_profile(1);
    auto support = ruled_surface(*first_profile, *second_profile, r_error);
    if (!support) {
      return std::nullopt;
    }
    support->id = builder.id();
    auto *surface = support.release();
    BLI_addtail(&builder.geometry->surfaces, surface);
    builder.surfaces.add_new(surface->id, surface);
    auto *face = MEM_new<NurbsTopologyFace>(__func__);
    face->id = builder.id();
    face->surface_id = surface->id;
    face->orientation = orientation;
    face->material_index = index.face(uses[0].face_id)->material_index;
    face->tolerance = result.geometry_error_bound;
    BLI_addtail(&builder.geometry->faces, face);
    builder.faces.add_new(face->id, face);
    result.faces.append(face->id);
    strip_seeds.append({face->id, profiles.lookup({original.id, original.vertex_ids[0]})});
    auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
    loop->id = builder.id();
    BLI_addtail(&face->loops, loop);
    const int64_t vertices[] = {stations[0][0], stations[1][0], stations[1][1], stations[0][1]};
    const double3 uv[] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (int side = 0; side < 4; side++) {
      const auto &boundary = *builder.edges.lookup(
          new_edge(vertices[side], vertices[(side + 1) % 4]));
      auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
      use->id = builder.id();
      use->edge_id = boundary.id;
      use->reversed = boundary.vertex_ids[0] != vertices[side];
      use->pcurve_id = install_curve(builder,
                                     use->reversed ?
                                         line(uv[(side + 1) % 4], uv[side], {0, 1}, 2) :
                                         line(uv[side], uv[(side + 1) % 4], {0, 1}, 2))
                           ->id;
      use->tolerance = result.geometry_error_bound;
      BLI_addtail(&loop->edge_uses, use);
    }
  }
  for (const auto &original : source.faces) {
    if (!face_corners.contains(original.id)) {
      continue;
    }
    auto &face = *builder.faces.lookup(original.id);
    auto &loop = *static_cast<NurbsTopologyLoop *>(face.loops.first);
    BLI_freelistN(&loop.edge_uses);
    auto append = [&](const int64_t id,
                      const int64_t start,
                      const int64_t use_id,
                      const double tolerance) {
      const auto &boundary = *builder.edges.lookup(id);
      auto projected = project_curve_to_affine_surface(*builder.curves.lookup(boundary.curve_id),
                                                       *builder.surfaces.lookup(face.surface_id),
                                                       options.max_error,
                                                       r_error);
      if (!projected) {
        return false;
      }
      result.geometry_error_bound = std::max(result.geometry_error_bound,
                                             projected->geometry_error_bound);
      auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
      use->id = use_id ? use_id : builder.id();
      use->edge_id = id;
      use->reversed = boundary.vertex_ids[0] != start;
      use->pcurve_id = install_curve(builder, std::move(projected->curve))->id;
      use->tolerance = std::max(tolerance, projected->geometry_error_bound);
      BLI_addtail(&loop.edge_uses, use);
      return true;
    };
    const auto &corners = face_corners.lookup(original.id);
    int i = 0;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(original.loops.first)->edge_uses)
    {
      const auto &corner = corners[i];
      const auto &next = corners[(i + 1) % corners.size()];
      const bool bevel = selected.contains(use.edge_id);
      const int64_t boundary = bevel ? rails.lookup({use.edge_id, face.id}) : use.edge_id;
      const int64_t start = bevel ? face_stations.lookup({corner.vertex, face.id}) :
                                    edge_stations.lookup_default({corner.vertex, use.edge_id},
                                                                 corner.vertex);
      if (!bevel && !shortened.contains(boundary)) {
        BLI_addtail(&loop.edge_uses, MEM_dupalloc(&use));
      }
      else if (!append(boundary, start, use.id, use.tolerance)) {
        return std::nullopt;
      }
      if (affected.contains(next.vertex) && !face_stations.contains({next.vertex, face.id})) {
        const int64_t a = edge_stations.lookup({next.vertex, next.incoming});
        const int64_t b = edge_stations.lookup({next.vertex, next.outgoing});
        const int64_t cap = new_edge(a, b);
        auto &at_vertex = corner_profiles.lookup_or_add_default(next.vertex);
        if (!at_vertex.contains(cap)) {
          at_vertex.append(cap);
        }
        if (!append(cap, a, 0, 0)) {
          return std::nullopt;
        }
      }
      i++;
    }
  }
  for (const int64_t id : affected) {
    auto *removed = builder.vertices.lookup(id);
    BLI_remlink(&builder.geometry->vertices, removed);
    MEM_delete(removed);
    result.source_to_result.add_new(id, 0);
  }
  for (const int64_t id : selected) {
    auto *removed = builder.edges.lookup(id);
    BLI_remlink(&builder.geometry->edges, removed);
    MEM_delete(removed);
    result.source_to_result.add_new(id, 0);
  }
  if (rounded) {
    for (const auto &original : source.vertices) {
      auto *corner = round_corners.lookup_ptr(original.id);
      if (!corner) {
        continue;
      }
      const auto current = TopologyIndex::build(*builder.geometry, r_error);
      if (!current) {
        return std::nullopt;
      }
      const int64_t a = corner->stations[0], b = corner->stations[1], c = corner->stations[2];
      const int64_t ab = edges_by_vertices.lookup(pair_key(a, b));
      const auto incident = current->uses_for_edge(ab);
      if (incident.size() != 1) {
        return fail("Rounded NURBS corner has ambiguous strip incidence");
      }
      const auto &boundary = *builder.edges.lookup(ab);
      const int orientation = -current->face(incident[0].face_id)->orientation *
                              (incident[0].use->reversed ? -1 : 1) *
                              (boundary.vertex_ids[0] != a ? -1 : 1);
      corner->boundary_edges = {
          ab, edges_by_vertices.lookup(pair_key(b, c)), edges_by_vertices.lookup(pair_key(c, a))};
      if (corner->boundary_edges[1] == 0 || corner->boundary_edges[2] == 0) {
        return fail("Rounded NURBS corner is missing a shared profile edge");
      }
      corner->support->id = builder.id();
      auto *surface = corner->support.release();
      BLI_addtail(&builder.geometry->surfaces, surface);
      builder.surfaces.add_new(surface->id, surface);
      auto *face = MEM_new<NurbsTopologyFace>(__func__);
      face->id = builder.id();
      corner->face_id = face->id;
      face->surface_id = surface->id;
      face->orientation = orientation;
      face->material_index = current->face(incident[0].face_id)->material_index;
      face->tolerance = result.geometry_error_bound;
      BLI_addtail(&builder.geometry->faces, face);
      builder.faces.add_new(face->id, face);
      result.faces.append(face->id);
      auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
      loop->id = builder.id();
      BLI_addtail(&face->loops, loop);
      const int64_t starts[] = {a, b, c}, ends[] = {b, c, a};
      const double3 uv[][2] = {
          {{0, 0, 0}, {1, 0, 0}}, {{1, 0, 0}, {1, 1, 0}}, {{0, 1, 0}, {0, 0, 0}}};
      for (int side = 0; side < 3; side++) {
        const auto &edge = *builder.edges.lookup(
            edges_by_vertices.lookup(pair_key(starts[side], ends[side])));
        auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
        use->id = builder.id();
        use->edge_id = edge.id;
        use->reversed = edge.vertex_ids[0] != starts[side];
        use->pcurve_id = install_curve(builder,
                                       use->reversed ? line(uv[side][1], uv[side][0], {0, 1}, 2) :
                                                       line(uv[side][0], uv[side][1], {0, 1}, 2))
                             ->id;
        use->tolerance = result.geometry_error_bound;
        BLI_addtail(&loop->edge_uses, use);
      }
    }
  }
  if (!builder.valid() || !geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  result.geometry = std::move(builder.geometry);
  if (!miter_groups.is_empty()) {
    auto welded = merge_vertex_groups(*result.geometry, miter_groups, options.max_error, r_error);
    if (!welded) {
      return std::nullopt;
    }
    result.geometry_error_bound = add_upper(result.geometry_error_bound,
                                            add_upper(miter_error, welded->geometry_error_bound));
    if (result.geometry_error_bound > options.max_error) {
      return fail("NURBS bevel miter reconstruction exceeds the construction error budget");
    }
    auto remap = [&](const Vector<int64_t> &ids) {
      Vector<int64_t> mapped;
      Set<int64_t> seen;
      for (const int64_t id : ids) {
        const int64_t target = welded->source_to_result.lookup_default(id, id);
        if (target && seen.add(target)) {
          mapped.append(target);
        }
      }
      return mapped;
    };
    result.vertices = remap(result.vertices);
    result.edges = remap(result.edges);
    result.faces = remap(result.faces);
    for (auto &ids : corner_profiles.values()) {
      ids = remap(ids);
    }
    for (const auto item : welded->source_to_result.items()) {
      if (item.key < source.next_id) {
        result.source_to_result.add_overwrite(item.key, item.value);
      }
    }
    result.geometry = std::move(welded->geometry);
  }
  for (const auto &original : source.vertices) {
    if (rounded || !affected.contains(original.id)) {
      continue;
    }
    bool closed = true;
    for (const int64_t id : index.edges_for_vertex(original.id)) {
      closed &= index.uses_for_edge(id).size() == 2;
    }
    if (!closed) {
      continue;
    }
    const auto current = TopologyIndex::build(*result.geometry, r_error);
    if (!current) {
      return std::nullopt;
    }
    Vector<int64_t> boundary;
    for (const int64_t id : corner_profiles.lookup(original.id)) {
      if (current->uses_for_edge(id).size() == 1) {
        boundary.append(id);
      }
    }
    if (boundary.is_empty()) {
      continue;
    }
    auto filled = fill_components(
        *result.geometry, {{}, boundary, {}}, options.max_error, r_error);
    if (!filled) {
      return std::nullopt;
    }
    int material = 0;
    for (const auto &face : source.faces) {
      if (const auto *corners = face_corners.lookup_ptr(face.id)) {
        bool incident = false;
        for (const auto &corner : *corners) {
          incident |= corner.vertex == original.id;
        }
        if (incident) {
          material = face.material_index;
          break;
        }
      }
    }
    for (auto &face : filled->geometry->faces) {
      if (filled->created_faces.contains(face.id)) {
        face.material_index = material;
      }
    }
    result.geometry_error_bound = add_upper(result.geometry_error_bound,
                                            filled->construction_error_bound);
    if (result.geometry_error_bound > options.max_error) {
      return fail("Connected corner surfaces exceed the NURBS construction error budget");
    }
    result.faces.extend(filled->created_faces);
    result.geometry = std::move(filled->geometry);
  }
  if (rounded) {
    /* Plan against the same unpartitioned source and apply all strip cuts together. Applying
     * each ring separately copied and validated the entire growing body once per strip. */
    const double partition_start = BLI_time_now_seconds();
    TopologyCutPlan combined;
    Map<int64_t, int> face_requests;
    Set<int64_t> remaining;
    for (const auto &item : strip_seeds) {
      remaining.add(item.first);
    }
    for (const auto &[face, seed] : strip_seeds) {
      if (!remaining.contains(face)) {
        continue;
      }
      auto plan = plan_loop_cut(
          *result.geometry, seed, options.segments - 1, 0, options.max_error, r_error);
      if (!plan) {
        return std::nullopt;
      }
      combined.transport_error_bound = add_upper(combined.transport_error_bound,
                                                 plan->transport_error_bound);
      combined.edges.extend(std::move(plan->edges));
      for (auto &request : plan->faces) {
        remaining.remove(request.face_id);
        const int slot = face_requests.lookup_or_add(request.face_id, combined.faces.size());
        if (slot == combined.faces.size()) {
          combined.faces.append(std::move(request));
          continue;
        }
        auto &cuts = combined.faces[slot].cuts;
        for (const auto &cut : request.cuts) {
          bool duplicate = false;
          for (const auto &existing : cuts) {
            if (existing.axis == cut.axis && existing.parameter == cut.parameter) {
              if (existing.boundary_parameters != cut.boundary_parameters) {
                return fail("Rounded bevel partitions disagree on a shared cut parameter");
              }
              duplicate = true;
              break;
            }
          }
          if (!duplicate) {
            cuts.append(cut);
          }
        }
      }
    }
    const double plan_ms = (BLI_time_now_seconds() - partition_start) * 1000.0;
    auto cut = partition_components(*result.geometry, combined, options.max_error, r_error);
    if (!cut) {
      return std::nullopt;
    }
    if (!partition_rounded_corner_patches(
            *cut, round_corners, options.segments, options.max_error, r_error))
    {
      return std::nullopt;
    }
    result.geometry_error_bound = add_upper(result.geometry_error_bound,
                                            cut->edit.geometry_error_bound);
    if (result.geometry_error_bound > options.max_error) {
      return fail("Rounded bevel partitions exceed the source construction error budget");
    }
    auto descendants = [](const Vector<int64_t> &ids,
                          const Map<int64_t, Vector<int64_t>> &mapping) {
      Vector<int64_t> result;
      for (const int64_t id : ids) {
        if (const auto *parts = mapping.lookup_ptr(id)) {
          result.extend(*parts);
        }
        else {
          result.append(id);
        }
      }
      return result;
    };
    result.faces = descendants(result.faces, cut->face_descendants);
    result.edges = descendants(result.edges, cut->edge_descendants);
    result.edges.extend(cut->edit.edges);
    result.vertices.extend(cut->edit.vertices);
    result.geometry = std::move(cut->edit.geometry);
    NURBS_DEBUG_LOG(
        "bevel batched strip partition: %.3f ms total, %.3f planning; %d faces, %d segments",
        (BLI_time_now_seconds() - partition_start) * 1000.0,
        plan_ms,
        int(combined.faces.size()),
        options.segments);
  }
  if (!evaluate_boundaries(*result.geometry, {}, r_error)) {
    return std::nullopt;
  }
  if (r_offset_info) {
    *r_offset_info = {options.offset, limit.offset, boundary_blend_corners};
  }
  return result;
}

/** Exact ruled-support path. Unlike planar projection, rails are isocurves of the original
 * supports. The common boundary is a tangent-control curve, so a positive conic blend joins
 * both regular support tangent planes even when the edge or either support is curved.
 * General contact-curve tracing and curved end-cap intersections remain separate solvers. */
static std::optional<TopologyEditResult> bevel_ruled_edge(const NurbsGeometry &source,
                                                          const TopologyIndex &index,
                                                          const int64_t selected,
                                                          BevelOptions options,
                                                          std::string *r_error,
                                                          BevelOffsetInfo *r_info)
{
  auto fail = [&](const char *message) -> std::optional<TopologyEditResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  const auto &original = *index.edge(selected);
  if (original.vertex_ids[0] == original.vertex_ids[1]) {
    return fail("Closed curved bevel needs periodic rail and end-profile topology");
  }
  const auto uses = index.uses_for_edge(selected);
  if (!evaluate_boundaries(source, {}, r_error)) {
    return std::nullopt;
  }
  if (uses.size() != 2 || uses[0].face_id == uses[1].face_id) {
    return fail("Curved bevel requires two distinct adjacent faces");
  }
  if (index.face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1) ==
      index.face(uses[1].face_id)->orientation * (uses[1].use->reversed ? -1 : 1))
  {
    return fail("Curved bevel requires consistent adjacent face orientations");
  }
  for (const int64_t id : original.vertex_ids) {
    if (index.edges_for_vertex(id).size() != 3) {
      return fail("Curved bevel endpoint needs a connected corner solver for this fan");
    }
    for (const int64_t boundary : index.edges_for_vertex(id)) {
      if (index.uses_for_edge(boundary).size() > 2) {
        return fail("Curved bevel cannot join a non-manifold endpoint fan");
      }
    }
  }
  if (source.next_id > std::numeric_limits<int64_t>::max() - 512) {
    return fail("NURBS bevel source identity range is exhausted");
  }
  TopologyBuilder builder(source);
  struct Side {
    const NurbsSurfaceGeometry *surface = nullptr;
    int64_t face = 0;
    int axis = 0;
    bool reverse = false;
    double near = 0, far = 1, parameter = 0, height = 0;
    std::array<int64_t, 2> spokes{};
    std::array<double2, 2> uv;
    OwnedCurve rail;
  };
  std::array<Side, 2> sides;
  TopologyEditResult result;
  double source_tolerance = original.tolerance;
  for (const auto &use : uses) {
    source_tolerance = std::max(source_tolerance, use.use->tolerance);
  }
  for (const int64_t id : original.vertex_ids) {
    source_tolerance = std::max(source_tolerance, index.vertex(id)->tolerance);
  }
  auto construction_tolerance = [&]() {
    return add_upper(source_tolerance, result.geometry_error_bound);
  };
  /* Covers the bounded number of domain/parameter arithmetic operations below. Shape changes
   * are deliberate; this allowance covers only binary64 construction of shared boundaries. */
  double extent = 0;
  for (const auto &surface : source.surfaces) {
    for (const auto &point : Span(surface.points, surface.points_u * surface.points_v)) {
      for (const double coordinate : point.position) {
        extent = std::max(extent, std::abs(coordinate));
      }
    }
  }
  result.geometry_error_bound = add_upper(
      multiply_upper(512 * std::numeric_limits<double>::epsilon(), extent),
      512 * std::numeric_limits<double>::denorm_min());
  auto add_error = [&](const double error) {
    result.geometry_error_bound = add_upper(result.geometry_error_bound, error);
    return std::isfinite(result.geometry_error_bound) &&
           result.geometry_error_bound <= options.max_error;
  };
  if (!add_error(0)) {
    return fail("Curved bevel construction roundoff exceeds the geometry error budget");
  }
  const double2 range(original.parameter_range);
  std::array<double3, 2> inward;
  for (int side = 0; side < 2; side++) {
    auto &plan = sides[side];
    plan.face = uses[side].face_id;
    const auto &face = *index.face(plan.face);
    const auto &surface = *builder.surfaces.lookup(face.surface_id);
    plan.surface = &surface;
    if (!face.loops.is_single() ||
        static_cast<const NurbsTopologyLoop *>(face.loops.first)->is_inner ||
        static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses.count() != 4)
    {
      return fail("Curved bevel contact rails currently require rectangular tensor faces");
    }
    const auto &pcurve = *builder.curves.lookup(uses[side].use->pcurve_id);
    if (pcurve.order != 2 || pcurve.points_num != 2 ||
        pcurve.points[0].weight != pcurve.points[1].weight)
    {
      return fail("Curved bevel requires a natural tensor boundary parameterization");
    }
    const double2 low(surface.knots_u[surface.order_u - 1], surface.knots_v[surface.order_v - 1]);
    const double2 high(surface.knots_u[surface.points_u], surface.knots_v[surface.points_v]);
    const double2 start(pcurve.points[0].position), end(pcurve.points[1].position);
    plan.axis = start.x == end.x ? 0 : 1;
    const int varying = 1 - plan.axis;
    const double *varying_knots = varying == 0 ? surface.knots_u : surface.knots_v;
    const int varying_order = varying == 0 ? surface.order_u : surface.order_v;
    const int varying_count = varying == 0 ? surface.points_u : surface.points_v;
    for (int i = 0; i < varying_order; i++) {
      if (varying_knots[i] != low[varying] || varying_knots[varying_count + i] != high[varying]) {
        return fail("Curved bevel requires clamped source-edge endpoints");
      }
    }
    if (start[plan.axis] != end[plan.axis] ||
        (start[plan.axis] != low[plan.axis] && start[plan.axis] != high[plan.axis]) ||
        std::min(start[varying], end[varying]) != low[varying] ||
        std::max(start[varying], end[varying]) != high[varying] || low[varying] != range[0] ||
        high[varying] != range[1] ||
        (plan.axis == 0 ? surface.order_u != 2 || surface.points_u != 2 :
                          surface.order_v != 2 || surface.points_v != 2))
    {
      return fail("Curved bevel needs contact-curve tracing on this non-ruled support");
    }
    plan.reverse = start[varying] > end[varying];
    plan.near = start[plan.axis];
    plan.far = plan.near == low[plan.axis] ? high[plan.axis] : low[plan.axis];
    const auto boundary = surface_isocurve(
        surface, plan.axis, plan.near, options.max_error, r_error);
    if (!boundary) {
      return std::nullopt;
    }
    const auto &control = *builder.curves.lookup(original.curve_id);
    if (boundary->curve->order != control.order ||
        boundary->curve->points_num != control.points_num ||
        boundary->curve->knots_num != control.knots_num)
    {
      return fail("Curved bevel requires compatible source-edge and support bases");
    }
    for (int i = 0; i < control.points_num; i++) {
      const auto &point = boundary->curve->points[plan.reverse ? control.points_num - 1 - i : i];
      if (double3(point.position) != double3(control.points[i].position) ||
          point.weight != control.points[i].weight)
      {
        return fail("Curved bevel requires common source-edge and support coefficients");
      }
    }
    for (int i = 0; i < control.knots_num; i++) {
      const double knot = plan.reverse ? range[0] + range[1] -
                                             boundary->curve->knots[control.knots_num - 1 - i] :
                                         boundary->curve->knots[i];
      if (knot != control.knots[i]) {
        return fail("Curved bevel requires matching source parameter domains");
      }
    }
    /* Equal transverse weights make the generator's geometric parameter linear. Along-edge
     * weights can be arbitrary positive rational values, including exact circles/arcs. */
    const int count = plan.axis == 0 ? surface.points_v : surface.points_u;
    for (int i = 0; i < count; i++) {
      const auto &a = surface.points[plan.axis == 0 ? 2 * i : i];
      const auto &b = surface.points[plan.axis == 0 ? 2 * i + 1 : count + i];
      if (!(a.weight > 0) || a.weight != b.weight) {
        return fail("Curved bevel needs a rational transverse contact-curve solver");
      }
    }
    Set<int> natural_sides;
    Set<int64_t> natural_corners;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      const auto &uv = *builder.curves.lookup(use.pcurve_id);
      const double2 a(uv.points[0].position), b(uv.points[uv.points_num - 1].position);
      const int fixed = a.x == b.x ? 0 : 1;
      if (uv.order != 2 || uv.points_num != 2 || uv.points[0].weight != uv.points[1].weight ||
          a[fixed] != b[fixed] || (a[fixed] != low[fixed] && a[fixed] != high[fixed]) ||
          std::min(a[1 - fixed], b[1 - fixed]) != low[1 - fixed] ||
          std::max(a[1 - fixed], b[1 - fixed]) != high[1 - fixed] ||
          !natural_sides.add(fixed * 2 + (a[fixed] == high[fixed] ? 1 : 0)))
      {
        return fail("Curved bevel requires four distinct natural tensor boundaries");
      }
      if (use.edge_id == selected) {
        natural_corners.add(original.vertex_ids[use.reversed ? 1 : 0]);
        continue;
      }
      const auto &boundary = *index.edge(use.edge_id);
      natural_corners.add(boundary.vertex_ids[use.reversed ? 1 : 0]);
      if (uv.order != 2 || uv.points_num != 2 || uv.points[0].weight != uv.points[1].weight) {
        return fail("Curved bevel requires full natural tensor boundaries");
      }
      for (int endpoint = 0; endpoint < 2; endpoint++) {
        if (boundary.vertex_ids[0] == original.vertex_ids[endpoint] ||
            boundary.vertex_ids[1] == original.vertex_ids[endpoint])
        {
          plan.spokes[endpoint] = boundary.id;
        }
      }
    }
    if (natural_corners.size() != 4) {
      return fail("Curved bevel requires distinct tensor corners");
    }
    if (!plan.spokes[0] || !plan.spokes[1] || plan.spokes[0] == plan.spokes[1]) {
      return fail("Curved bevel endpoint boundaries are not independent tensor sides");
    }
    double2 midpoint = (low + high) * 0.5;
    midpoint[plan.axis] = plan.near;
    const auto differential = evaluate_surface_differential(surface, midpoint, {}, r_error);
    if (!differential || !differential->normal) {
      return fail("Curved bevel has a singular source tangent frame");
    }
    const double3 along = plan.axis == 0 ? differential->tangent_v : differential->tangent_u;
    const double3 across = (plan.axis == 0 ? differential->tangent_u : differential->tangent_v) *
                           (plan.far - plan.near);
    const double length_squared = math::length_squared(along);
    if (!(length_squared > 0)) {
      return fail("Curved bevel has a degenerate source edge tangent");
    }
    const double3 perpendicular = across - along * (math::dot(across, along) / length_squared);
    plan.height = math::length(perpendicular);
    if (!(plan.height > options.max_error) || !std::isfinite(plan.height)) {
      return fail("Curved bevel has no positive transverse clearance");
    }
    inward[side] = perpendicular / plan.height;
  }
  const double height = std::min(sides[0].height, sides[1].height);
  const double clearance = std::max(64 * std::numeric_limits<double>::epsilon(),
                                    4 * options.max_error / height);
  const double limit = height * (1 - clearance);
  if (!(limit > 0) || (options.offset > limit && !options.clamp_overlap)) {
    return fail("Curved bevel width reaches the opposite boundary; enable Clamp Overlap");
  }
  const double offset = std::min(options.offset, limit);
  Vector<int64_t> cap_charts;
  for (const auto &face : source.faces) {
    if (face.id == sides[0].face || face.id == sides[1].face) {
      continue;
    }
    bool incident = false;
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        const auto &boundary = *index.edge(use.edge_id);
        for (const int64_t id : original.vertex_ids) {
          incident |= boundary.vertex_ids[0] == id || boundary.vertex_ids[1] == id;
        }
      }
    }
    const auto &surface = *builder.surfaces.lookup(face.surface_id);
    if (!incident || surface.order_u != 2 || surface.order_v != 2 || surface.points_u != 2 ||
        surface.points_v != 2)
    {
      continue;
    }
    double twist = 0;
    for (int axis = 0; axis < 3; axis++) {
      const auto value = bounded_add(
          bounded_subtract({surface.points[3].position[axis]}, {surface.points[1].position[axis]}),
          bounded_subtract({surface.points[0].position[axis]},
                           {surface.points[2].position[axis]}));
      twist = add_upper(twist, add_upper(std::abs(value.value), value.error));
    }
    if (twist > options.max_error) {
      cap_charts.append(face.id);
    }
  }
  if (!cap_charts.is_empty()) {
    auto charts = reparameterize_affine_planes(source, cap_charts, options.max_error, r_error);
    if (!charts) {
      return fail("Curved bevel requires a surface-intersection solver for this end cap");
    }
    const auto normalized = TopologyIndex::build(*charts->geometry, r_error);
    if (!normalized) {
      return std::nullopt;
    }
    BevelOffsetInfo info;
    auto candidate = bevel_ruled_edge(
        *charts->geometry, *normalized, selected, options, r_error, &info);
    if (!candidate) {
      return std::nullopt;
    }
    candidate->geometry_error_bound = add_upper(candidate->geometry_error_bound,
                                                charts->geometry_error_bound);
    if (candidate->geometry_error_bound > options.max_error) {
      return fail("Curved bevel and end-cap charts exceed the geometry error budget");
    }
    info.planar_charts += cap_charts.size();
    info.chart_error_bound = add_upper(info.chart_error_bound, charts->geometry_error_bound);
    if (r_info) {
      *r_info = info;
    }
    return candidate;
  }
  const bool rounded = options.segments > 1;
  const double weight = rounded ?
                            std::sqrt(std::max(0.0, (1 - math::dot(inward[0], inward[1])) * 0.5)) :
                            1;
  if (!(weight > 0) || (rounded && weight >= 1)) {
    return fail("Curved bevel requires a regular non-flat corner angle");
  }
  std::array<std::array<int64_t, 2>, 2> stations;
  Map<int64_t, double2> shortened;
  for (int side = 0; side < 2; side++) {
    auto &plan = sides[side];
    plan.parameter = plan.near + (plan.far - plan.near) * (offset / plan.height);
    if (!(plan.parameter > std::min(plan.near, plan.far) &&
          plan.parameter < std::max(plan.near, plan.far)))
    {
      return fail("Curved bevel rail cannot be represented inside its source domain");
    }
    auto rail = surface_isocurve(
        *plan.surface, plan.axis, plan.parameter, options.max_error, r_error);
    if (!rail || !add_error(rail->geometry_error_bound)) {
      return fail("Curved bevel rail construction exceeds the geometry error budget");
    }
    plan.rail = std::move(rail->curve);
    if (plan.reverse) {
      std::reverse(plan.rail->points, plan.rail->points + plan.rail->points_num);
      std::reverse(plan.rail->knots, plan.rail->knots + plan.rail->knots_num);
      for (double &knot : MutableSpan(plan.rail->knots, plan.rail->knots_num)) {
        knot = range[0] + range[1] - knot;
      }
    }
    for (int endpoint = 0; endpoint < 2; endpoint++) {
      const auto &point = plan.rail->points[endpoint ? plan.rail->points_num - 1 : 0];
      stations[side][endpoint] =
          vertex(builder, double3(point.position), construction_tolerance())->id;
      result.vertices.append(stations[side][endpoint]);
      plan.uv[endpoint][plan.axis] = plan.parameter;
      plan.uv[endpoint][1 - plan.axis] = range[plan.reverse ? 1 - endpoint : endpoint];
      auto &spoke = *builder.edges.lookup(plan.spokes[endpoint]);
      const auto *source_use = index.uses_for_edge(spoke.id).data();
      for (const auto &candidate_use : index.uses_for_edge(spoke.id)) {
        if (candidate_use.face_id == plan.face) {
          source_use = &candidate_use;
          break;
        }
      }
      const auto &uv = *builder.curves.lookup(source_use->use->pcurve_id);
      const double fraction = (plan.parameter - uv.points[0].position[plan.axis]) /
                              (uv.points[1].position[plan.axis] -
                               uv.points[0].position[plan.axis]);
      const double parameter = spoke.parameter_range[0] +
                               fraction * (spoke.parameter_range[1] - spoke.parameter_range[0]);
      const int changed_end = spoke.vertex_ids[0] == original.vertex_ids[endpoint] ? 0 : 1;
      double2 interval(spoke.parameter_range);
      interval[changed_end] = parameter;
      auto cropped = restrict_curve(
          *builder.curves.lookup(spoke.curve_id), interval, options.max_error, r_error);
      if (!cropped || !add_error(cropped->geometry_error_bound)) {
        return fail("Curved bevel boundary restriction exceeds the geometry error budget");
      }
      shortened.add_new(spoke.id, interval);
      spoke.curve_id = install_curve(builder, std::move(cropped->curve))->id;
      spoke.vertex_ids[changed_end] = stations[side][endpoint];
      std::copy_n(&interval.x, 2, spoke.parameter_range);
    }
    double2 low(plan.surface->knots_u[plan.surface->order_u - 1],
                plan.surface->knots_v[plan.surface->order_v - 1]);
    double2 high(plan.surface->knots_u[plan.surface->points_u],
                 plan.surface->knots_v[plan.surface->points_v]);
    (plan.far > plan.near ? low : high)[plan.axis] = plan.parameter;
    auto cropped = restrict_surface(*plan.surface, low, high, options.max_error, r_error);
    if (!cropped || !add_error(cropped->geometry_error_bound)) {
      return fail("Curved bevel support restriction exceeds the geometry error budget");
    }
    cropped->surface->id = builder.id();
    auto *support = cropped->surface.release();
    BLI_addtail(&builder.geometry->surfaces, support);
    builder.surfaces.add_new(support->id, support);
    builder.faces.lookup(plan.face)->surface_id = support->id;
  }
  const auto &control = *builder.curves.lookup(original.curve_id);
  OwnedSurface blend;
  if (rounded) {
    auto constructed = conic_curve_blend(
        *sides[0].rail, control, *sides[1].rail, weight, options.max_error, r_error);
    if (!constructed || !add_error(constructed->geometry_error_bound)) {
      return std::nullopt;
    }
    blend = std::move(constructed->surface);
  }
  else {
    blend = ruled_surface(*sides[0].rail, *sides[1].rail, r_error);
  }
  if (!blend) {
    return std::nullopt;
  }
  const int64_t rail_ids[] = {edge(builder,
                                   stations[0][0],
                                   stations[0][1],
                                   std::move(sides[0].rail),
                                   construction_tolerance())
                                  ->id,
                              edge(builder,
                                   stations[1][0],
                                   stations[1][1],
                                   std::move(sides[1].rail),
                                   construction_tolerance())
                                  ->id};
  int64_t profiles[2];
  for (int endpoint = 0; endpoint < 2; endpoint++) {
    auto profile = surface_isocurve(*blend, 0, range[endpoint], options.max_error, r_error);
    if (!profile || !add_error(profile->geometry_error_bound)) {
      return fail("Curved bevel end-profile construction exceeds the geometry error budget");
    }
    profiles[endpoint] = edge(builder,
                              stations[0][endpoint],
                              stations[1][endpoint],
                              std::move(profile->curve),
                              construction_tolerance())
                             ->id;
  }
  /* Shorten every incident p-curve, including an unselected planar end cap. Original supports
   * remain untouched; cropped adjacent faces own private exact restricted supports. */
  for (auto &face : builder.geometry->faces) {
    for (auto &loop : face.loops) {
      for (auto &use : loop.edge_uses) {
        if (const auto *interval = shortened.lookup_ptr(use.edge_id)) {
          auto cropped = restrict_curve(
              *builder.curves.lookup(use.pcurve_id), *interval, options.max_error, r_error);
          if (!cropped || !add_error(cropped->geometry_error_bound)) {
            return fail("Curved bevel trim restriction exceeds the geometry error budget");
          }
          use.pcurve_id = install_curve(builder, std::move(cropped->curve))->id;
        }
        if (use.edge_id == selected) {
          const int side = face.id == sides[0].face ? 0 : 1;
          use.edge_id = rail_ids[side];
          const double2 a = sides[side].uv[0], b = sides[side].uv[1];
          use.pcurve_id = install_curve(builder, line({a.x, a.y, 0}, {b.x, b.y, 0}, range, 2))->id;
        }
      }
    }
  }
  /* One canonical UV for each new corner of a retained tensor face. Oppositely parameterized
   * generator restrictions can otherwise differ by one last-bit subtraction at the endpoint. */
  for (int side = 0; side < 2; side++) {
    const auto &plan = sides[side];
    for (auto &loop : builder.faces.lookup(plan.face)->loops) {
      for (auto &use : loop.edge_uses) {
        for (int endpoint = 0; endpoint < 2; endpoint++) {
          if (use.edge_id != plan.spokes[endpoint]) {
            continue;
          }
          const auto &boundary = *builder.edges.lookup(use.edge_id);
          auto &pcurve = *builder.curves.lookup(use.pcurve_id);
          const int coefficient = boundary.vertex_ids[0] == stations[side][endpoint] ?
                                      0 :
                                      pcurve.points_num - 1;
          for (int axis = 0; axis < 2; axis++) {
            pcurve.points[coefficient].position[axis] = plan.uv[endpoint][axis];
          }
        }
      }
    }
  }
  for (const auto &original_face : source.faces) {
    if (original_face.id == sides[0].face || original_face.id == sides[1].face) {
      continue;
    }
    auto &face = *builder.faces.lookup(original_face.id);
    for (const auto &original_loop : original_face.loops) {
      auto *loop = static_cast<NurbsTopologyLoop *>(face.loops.first);
      while (loop && loop->id != original_loop.id) {
        loop = loop->next;
      }
      for (const auto &original_use : original_loop.edge_uses) {
        const auto &boundary = *index.edge(original_use.edge_id);
        const int64_t end = boundary.vertex_ids[original_use.reversed ? 0 : 1];
        const int endpoint = end == original.vertex_ids[0] ? 0 :
                             end == original.vertex_ids[1] ? 1 :
                                                             -1;
        if (endpoint < 0) {
          continue;
        }
        auto *previous = static_cast<NurbsTopologyEdgeUse *>(loop->edge_uses.first);
        while (previous && previous->id != original_use.id) {
          previous = previous->next;
        }
        const auto &profile_edge = *builder.edges.lookup(profiles[endpoint]);
        const auto &curve = *builder.curves.lookup(profile_edge.curve_id);
        auto projected = project_curve_to_affine_surface(
            curve, *builder.surfaces.lookup(face.surface_id), options.max_error, r_error);
        if (!projected) {
          return fail("Curved bevel requires a surface-intersection solver for this end cap");
        }
        const auto *original_next = original_use.next ? original_use.next :
                                                        static_cast<const NurbsTopologyEdgeUse *>(
                                                            original_loop.edge_uses.first);
        const auto *next = static_cast<const NurbsTopologyEdgeUse *>(loop->edge_uses.first);
        while (next && next->id != original_next->id) {
          next = next->next;
        }
        const auto &previous_uv = *builder.curves.lookup(previous->pcurve_id);
        const auto &next_uv = *builder.curves.lookup(next->pcurve_id);
        const double2 first_uv(
            previous_uv.points[previous->reversed ? 0 : previous_uv.points_num - 1].position);
        const double2 last_uv(
            next_uv.points[next->reversed ? next_uv.points_num - 1 : 0].position);
        auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
        use->id = builder.id();
        use->edge_id = profile_edge.id;
        const auto &changed = *builder.edges.lookup(original_use.edge_id);
        use->reversed = profile_edge.vertex_ids[0] !=
                        changed.vertex_ids[original_use.reversed ? 0 : 1];
        for (int axis = 0; axis < 2; axis++) {
          projected->curve->points[0].position[axis] = (use->reversed ? last_uv : first_uv)[axis];
          projected->curve->points[projected->curve->points_num - 1].position[axis] =
              (use->reversed ? first_uv : last_uv)[axis];
        }
        const auto bound = bilinear_curve_support_deviation_bound(
            curve, *projected->curve, *builder.surfaces.lookup(face.surface_id));
        if (!bound || !add_error(*bound)) {
          MEM_delete(use);
          return fail("Curved bevel end-cap UV reconciliation exceeds the geometry error budget");
        }
        use->pcurve_id = install_curve(builder, std::move(projected->curve))->id;
        use->tolerance = construction_tolerance();
        BLI_insertlinkafter(&loop->edge_uses, previous, use);
      }
    }
  }
  blend->id = builder.id();
  auto *support = blend.release();
  BLI_addtail(&builder.geometry->surfaces, support);
  builder.surfaces.add_new(support->id, support);
  auto *face = MEM_new<NurbsTopologyFace>(__func__);
  face->id = builder.id();
  face->surface_id = support->id;
  face->orientation = -index.face(uses[0].face_id)->orientation * (uses[0].use->reversed ? -1 : 1);
  face->material_index = index.face(uses[0].face_id)->material_index;
  face->tolerance = construction_tolerance();
  BLI_addtail(&builder.geometry->faces, face);
  auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
  loop->id = builder.id();
  BLI_addtail(&face->loops, loop);
  const int64_t boundaries[] = {rail_ids[0], profiles[1], rail_ids[1], profiles[0]};
  const double3 uv[] = {{range[0], 0, 0}, {range[1], 0, 0}, {range[1], 1, 0}, {range[0], 1, 0}};
  for (int i = 0; i < 4; i++) {
    const auto &boundary = *builder.edges.lookup(boundaries[i]);
    const bool reverse = i >= 2;
    auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
    use->id = builder.id();
    use->edge_id = boundary.id;
    use->reversed = reverse;
    use->pcurve_id = install_curve(builder,
                                   line(uv[reverse ? (i + 1) % 4 : i],
                                        uv[reverse ? i : (i + 1) % 4],
                                        double2(boundary.parameter_range),
                                        2))
                         ->id;
    use->tolerance = construction_tolerance();
    BLI_addtail(&loop->edge_uses, use);
  }
  BLI_freelinkN(&builder.geometry->edges, builder.edges.lookup(selected));
  result.source_to_result.add_new(selected, 0);
  for (const int64_t id : original.vertex_ids) {
    BLI_freelinkN(&builder.geometry->vertices, builder.vertices.lookup(id));
    result.source_to_result.add_new(id, 0);
  }
  for (auto &item : builder.geometry->vertices) {
    if (item.id >= source.next_id) {
      item.tolerance = construction_tolerance();
    }
  }
  for (auto &edge : builder.geometry->edges) {
    if (shortened.contains(edge.id) || edge.id >= source.next_id) {
      edge.tolerance = std::max(edge.tolerance, construction_tolerance());
    }
  }
  for (auto &item : builder.geometry->faces) {
    for (auto &boundary : item.loops) {
      for (auto &use : boundary.edge_uses) {
        if (shortened.contains(use.edge_id) || use.edge_id >= source.next_id) {
          use.tolerance = std::max(use.tolerance, construction_tolerance());
        }
      }
    }
  }
  result.edges.extend(rail_ids, 2);
  result.edges.extend(profiles, 2);
  result.faces.append(face->id);
  const int64_t seed = profiles[0];
  result.geometry = std::move(builder.geometry);
  if (!builder.valid() || !geometry_validate_structure(*result.geometry, r_error) ||
      !evaluate_boundaries(*result.geometry, {}, r_error))
  {
    return std::nullopt;
  }
  if (rounded) {
    auto plan = plan_loop_cut(
        *result.geometry, seed, options.segments - 1, 0, options.max_error, r_error);
    auto cut = plan ? partition_components(*result.geometry, *plan, options.max_error, r_error) :
                      std::nullopt;
    if (!cut || !add_error(cut->edit.geometry_error_bound)) {
      return std::nullopt;
    }
    result.faces = cut->face_descendants.lookup_default(face->id, result.faces);
    Vector<int64_t> edges;
    for (const int64_t id : result.edges) {
      const auto *descendants = cut->edge_descendants.lookup_ptr(id);
      if (descendants) {
        edges.extend(*descendants);
      }
      else {
        edges.append(id);
      }
    }
    edges.extend(cut->edit.edges);
    result.edges = std::move(edges);
    result.vertices.extend(cut->edit.vertices);
    result.geometry = std::move(cut->edit.geometry);
  }
  if (r_info) {
    *r_info = {};
    r_info->offset = offset;
    r_info->limit = limit;
    r_info->curved_blend_strips = 1;
  }
  if (r_error) {
    r_error->clear();
  }
  return result;
}

}  // namespace

std::optional<TopologyEditResult> bevel_edges(const NurbsGeometry &source,
                                              const Span<int64_t> edges,
                                              const BevelOptions &options,
                                              std::string *r_error,
                                              BevelOffsetInfo *r_offset_info)
{
  const double bevel_start = BLI_time_now_seconds();
  double topology_ms = 0.0, classification_ms = 0.0;
  std::string local_error;
  if (!r_error) {
    r_error = &local_error;
  }
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<TopologyEditResult> {
    if (r_error) {
      *r_error = message;
    }
    NURBS_DEBUG_LOG(
        "bevel kernel rejected after %.3f ms; width %.9g, segments %d, input edges %d; %s",
        (BLI_time_now_seconds() - bevel_start) * 1000.0,
        options.offset,
        options.segments,
        int(edges.size()),
        message);
    return std::nullopt;
  };
  if (!supported_arithmetic() || !std::isfinite(options.offset) || options.offset < 0 ||
      !std::isfinite(options.max_error) || options.max_error < 0 || options.segments < 1 ||
      options.segments > 100)
  {
    return fail("NURBS bevel requires a finite nonnegative width/error budget and 1–100 segments");
  }
  const double topology_start = BLI_time_now_seconds();
  const auto index = TopologyIndex::build(source, r_error);
  topology_ms = (BLI_time_now_seconds() - topology_start) * 1000.0;
  if (!index) {
    NURBS_DEBUG_LOG("bevel kernel failed building topology index after %.3f ms; total %.3f ms; %s",
                    topology_ms,
                    (BLI_time_now_seconds() - bevel_start) * 1000.0,
                    r_error ? r_error->c_str() : "");
    return std::nullopt;
  }
  Set<int64_t> selected;
  for (const int64_t id : edges) {
    if (!index->edge(id)) {
      return fail("NURBS bevel selection contains an unknown source edge");
    }
    // Mesh bevel skips boundary/wire edges when faces or a larger region are selected.
    if (index->uses_for_edge(id).size() == 2) {
      selected.add(id);
    }
  }
  if (selected.is_empty()) {
    return fail("Select a shared edge between two NURBS faces to bevel");
  }
  if (options.offset == 0) {
    TopologyEditResult result;
    result.geometry.reset(geometry_copy(&source));
    result.edges.extend(edges);
    for (const int64_t id : selected) {
      result.vertices.extend(index->edge(id)->vertex_ids, 2);
    }
    if (r_offset_info) {
      *r_offset_info = {};
    }
    NURBS_DEBUG_LOG(
        "bevel kernel zero-width copy completed in %.3f ms (topology %.3f ms); %d input, "
        "%d selected edges",
        (BLI_time_now_seconds() - bevel_start) * 1000.0,
        topology_ms,
        int(edges.size()),
        int(selected.size()));
    return result;
  }
  const double classification_start = BLI_time_now_seconds();
  Set<int64_t> affected_vertices;
  for (const int64_t id : selected) {
    affected_vertices.add_multiple({index->edge(id)->vertex_ids, 2});
  }
  Map<int64_t, const NurbsSurfaceGeometry *> supports;
  for (const auto &surface : source.surfaces) {
    supports.add_new(surface.id, &surface);
  }
  Vector<int64_t> planar_charts;
  for (const auto &face : source.faces) {
    bool affected = false;
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        const auto &edge = *index->edge(use.edge_id);
        affected |= affected_vertices.contains(edge.vertex_ids[0]) ||
                    affected_vertices.contains(edge.vertex_ids[1]);
      }
    }
    const auto &surface = *supports.lookup(face.surface_id);
    if (!affected || surface.order_u != 2 || surface.order_v != 2 || surface.points_u != 2 ||
        surface.points_v != 2)
    {
      continue;
    }
    double twist = 0;
    for (int axis = 0; axis < 3; axis++) {
      const auto value = bounded_add(
          bounded_subtract({surface.points[3].position[axis]}, {surface.points[1].position[axis]}),
          bounded_subtract({surface.points[0].position[axis]},
                           {surface.points[2].position[axis]}));
      twist = add_upper(twist, add_upper(std::abs(value.value), value.error));
    }
    if (twist > options.max_error) {
      planar_charts.append(face.id);
    }
  }
  classification_ms = (BLI_time_now_seconds() - classification_start) * 1000.0;
  if (!planar_charts.is_empty()) {
    const double chart_start = BLI_time_now_seconds();
    auto charts = reparameterize_affine_planes(source, planar_charts, options.max_error, r_error);
    if (!charts) {
      if (selected.size() == 1) {
        const double ruled_start = BLI_time_now_seconds();
        auto ruled = bevel_ruled_edge(
            source, *index, *selected.begin(), options, r_error, r_offset_info);
        NURBS_DEBUG_LOG(
            "bevel ruled fallback after chart rejection: %.3f ms ruled, %.3f ms chart "
            "stage, %.3f ms total; %d selected edges, %d segments, success %d%s",
            (BLI_time_now_seconds() - ruled_start) * 1000.0,
            (BLI_time_now_seconds() - chart_start) * 1000.0,
            (BLI_time_now_seconds() - bevel_start) * 1000.0,
            int(selected.size()),
            options.segments,
            int(bool(ruled)),
            r_error && !r_error->empty() ? r_error->c_str() : "");
        return ruled;
      }
      NURBS_DEBUG_LOG(
          "bevel planar chart construction failed after %.3f ms; %.3f ms total; %d charts, "
          "%d selected edges; %s",
          (BLI_time_now_seconds() - chart_start) * 1000.0,
          (BLI_time_now_seconds() - bevel_start) * 1000.0,
          int(planar_charts.size()),
          int(selected.size()),
          r_error ? r_error->c_str() : "");
      return std::nullopt;
    }
    const double chart_ms = (BLI_time_now_seconds() - chart_start) * 1000.0;
    const double chart_index_start = BLI_time_now_seconds();
    const auto chart_index = TopologyIndex::build(*charts->geometry, r_error);
    if (!chart_index) {
      NURBS_DEBUG_LOG(
          "bevel failed building reparameterized topology index after %.3f ms; total "
          "%.3f ms; %s",
          (BLI_time_now_seconds() - chart_index_start) * 1000.0,
          (BLI_time_now_seconds() - bevel_start) * 1000.0,
          r_error ? r_error->c_str() : "");
      return std::nullopt;
    }
    const double chart_index_ms = (BLI_time_now_seconds() - chart_index_start) * 1000.0;
    BevelOffsetInfo info;
    const double network_start = BLI_time_now_seconds();
    auto result = bevel_planar_network(
        *charts->geometry, *chart_index, selected, options, r_error, &info);
    const double network_ms = (BLI_time_now_seconds() - network_start) * 1000.0;
    if (!result) {
      NURBS_DEBUG_LOG(
          "bevel planar network failed: %.3f ms network, %.3f ms charts, %.3f ms chart "
          "index, %.3f ms classification, %.3f ms source index; %.3f ms total; %s",
          network_ms,
          chart_ms,
          chart_index_ms,
          classification_ms,
          topology_ms,
          (BLI_time_now_seconds() - bevel_start) * 1000.0,
          r_error ? r_error->c_str() : "");
      return std::nullopt;
    }
    result->geometry_error_bound = add_upper(result->geometry_error_bound,
                                             charts->geometry_error_bound);
    if (result->geometry_error_bound > options.max_error) {
      return fail("NURBS bevel and planar chart exceed the combined geometry error budget");
    }
    info.planar_charts = planar_charts.size();
    info.chart_error_bound = charts->geometry_error_bound;
    if (r_offset_info) {
      *r_offset_info = info;
    }
    NURBS_DEBUG_LOG(
        "bevel planar network complete: %.3f ms network, %.3f ms charts, %.3f ms chart "
        "index, %.3f ms classification, %.3f ms source index; %.3f ms total; %d selected "
        "edges, %d charts, %d segments, error bound %.3g",
        network_ms,
        chart_ms,
        chart_index_ms,
        classification_ms,
        topology_ms,
        (BLI_time_now_seconds() - bevel_start) * 1000.0,
        int(selected.size()),
        int(planar_charts.size()),
        options.segments,
        result->geometry_error_bound);
    return result;
  }
  const double network_start = BLI_time_now_seconds();
  auto result = bevel_planar_network(source, *index, selected, options, r_error, r_offset_info);
  const double network_ms = (BLI_time_now_seconds() - network_start) * 1000.0;
  if (!result && selected.size() == 1 && r_error &&
      (r_error->find("planar") != std::string::npos ||
       r_error->find("straight") != std::string::npos ||
       r_error->find("projection") != std::string::npos))
  {
    const double ruled_start = BLI_time_now_seconds();
    auto ruled = bevel_ruled_edge(
        source, *index, *selected.begin(), options, r_error, r_offset_info);
    NURBS_DEBUG_LOG(
        "bevel ruled fallback: %.3f ms after %.3f ms planar network, %.3f ms total; %d "
        "selected edges, %d segments, success %d%s",
        (BLI_time_now_seconds() - ruled_start) * 1000.0,
        network_ms,
        (BLI_time_now_seconds() - bevel_start) * 1000.0,
        int(selected.size()),
        options.segments,
        int(bool(ruled)),
        r_error && !r_error->empty() ? r_error->c_str() : "");
    return ruled;
  }
  NURBS_DEBUG_LOG(
      "bevel planar network %s: %.3f ms network, %.3f ms classification, %.3f ms source "
      "index; %.3f ms total; %d selected edges, %d segments%s",
      result ? "succeeded" : "failed",
      network_ms,
      classification_ms,
      topology_ms,
      (BLI_time_now_seconds() - bevel_start) * 1000.0,
      int(selected.size()),
      options.segments,
      r_error && !r_error->empty() ? r_error->c_str() : "");
  return result;
}

}  // namespace blender::bke::nurbs
