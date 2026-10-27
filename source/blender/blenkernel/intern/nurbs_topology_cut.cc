/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <array>

#include "BLI_math_vector.hh"

#include "BKE_nurbs_refine.hh"
#include "BKE_nurbs_topology_edit.hh"

#include "nurbs_math.hh"
#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs {
namespace {

struct Rectangle {
  double2 low, high;
  /** Bottom, right, top, left. */
  std::array<const NurbsTopologyEdge *, 4> edges{};
  std::array<const NurbsTopologyEdgeUse *, 4> uses{};
  std::array<std::array<double2, 2>, 4> uv;
  /** LL, LR, UL, UR. */
  std::array<int64_t, 4> corners{};
  bool counterclockwise = true;
};

static std::optional<Rectangle> rectangle(const NurbsTopologyFace &face,
                                          const TopologyIndex &index,
                                          const Map<int64_t, const NurbsCurveGeometry *> &curves)
{
  if (!face.loops.is_single()) {
    return std::nullopt;
  }
  const auto &loop = *static_cast<const NurbsTopologyLoop *>(face.loops.first);
  if (loop.is_inner || loop.edge_uses.count() != 4) {
    return std::nullopt;
  }
  Rectangle result;
  result.low = double2(INFINITY);
  result.high = double2(-INFINITY);
  std::array<std::array<double2, 2>, 4> endpoints;
  int i = 0;
  for (const auto &use : loop.edge_uses) {
    const auto &curve = *curves.lookup(use.pcurve_id);
    if (curve.order != 2 || curve.points_num != 2 || curve.points[0].weight <= 0 ||
        curve.points[0].weight != curve.points[1].weight || curve.knots[0] != curve.knots[1] ||
        curve.knots[2] != curve.knots[3])
    {
      return std::nullopt;
    }
    const auto &edge = *index.edge(use.edge_id);
    for (int end = 0; end < 2; end++) {
      const auto point = evaluate_curve_bounded(
          curve, edge.parameter_range[end], std::numeric_limits<double>::max());
      if (!point) {
        return std::nullopt;
      }
      endpoints[i][end] = double2(point->position.x, point->position.y);
      result.low = math::min(result.low, endpoints[i][end]);
      result.high = math::max(result.high, endpoints[i][end]);
    }
    i++;
  }
  if (!(result.low.x < result.high.x && result.low.y < result.high.y)) {
    return std::nullopt;
  }
  i = 0;
  std::optional<bool> orientation;
  for (const auto &use : loop.edge_uses) {
    const auto &edge = *index.edge(use.edge_id);
    const auto &uv = endpoints[i++];
    int side = -1;
    if (uv[0].y == uv[1].y) {
      if (uv[0].y == result.low.y) {
        side = 0;
      }
      else if (uv[0].y == result.high.y) {
        side = 2;
      }
    }
    else if (uv[0].x == uv[1].x) {
      if (uv[0].x == result.high.x) {
        side = 1;
      }
      else if (uv[0].x == result.low.x) {
        side = 3;
      }
    }
    if (side < 0 || result.edges[side]) {
      return std::nullopt;
    }
    result.edges[side] = &edge;
    result.uses[side] = &use;
    result.uv[side] = uv;
    const int axis = side % 2 == 0 ? 0 : 1;
    const bool increasing = uv[use.reversed ? 1 : 0][axis] < uv[use.reversed ? 0 : 1][axis];
    const bool ccw = side < 2 ? increasing : !increasing;
    if (orientation && *orientation != ccw) {
      return std::nullopt;
    }
    orientation = ccw;
    for (int end = 0; end < 2; end++) {
      int slot = 0;
      for (int a = 0; a < 2; a++) {
        if (uv[end][a] != result.low[a] && uv[end][a] != result.high[a]) {
          return std::nullopt;
        }
        if (uv[end][a] == result.high[a]) {
          slot |= 1 << a;
        }
      }
      if (result.corners[slot] && result.corners[slot] != edge.vertex_ids[end]) {
        return std::nullopt;
      }
      result.corners[slot] = edge.vertex_ids[end];
    }
  }
  result.counterclockwise = *orientation;
  return result;
}

static int end_side(const int axis, const int end)
{
  return axis == 0 ? (end ? 2 : 0) : (end ? 1 : 3);
}
static double edge_parameter(const Rectangle &rect,
                             const int side,
                             const int axis,
                             const double uv)
{
  const auto &range = rect.edges[side]->parameter_range;
  const double a = rect.uv[side][0][axis], b = rect.uv[side][1][axis];
  if (a == range[0] && b == range[1]) {
    return uv;
  }
  if (uv == a) {
    return range[0];
  }
  if (uv == b) {
    return range[1];
  }
  return range[0] + (uv - a) / (b - a) * (range[1] - range[0]);
}
static double source_coordinate(const NurbsTopologyEdge &edge,
                                const NurbsCurveGeometry &pcurve,
                                const int axis,
                                const double parameter)
{
  if (pcurve.points[0].position[axis] == edge.parameter_range[0] &&
      pcurve.points[1].position[axis] == edge.parameter_range[1] &&
      pcurve.knots[1] == edge.parameter_range[0] && pcurve.knots[2] == edge.parameter_range[1])
  {
    return parameter;
  }
  const auto uv = evaluate_curve_bounded(pcurve, parameter, std::numeric_limits<double>::max());
  return uv ? uv->position[axis] : NAN;
}

static Map<int64_t, const NurbsCurveGeometry *> curve_index(const NurbsGeometry &source)
{
  Map<int64_t, const NurbsCurveGeometry *> result;
  for (const auto &curve : source.curves) {
    result.add_new(curve.id, &curve);
  }
  return result;
}
static bool request_valid(const int cuts, const double max_error, std::string *error)
{
  if (cuts < 1 || cuts > 1000 || !std::isfinite(max_error) || max_error < 0 ||
      !detail::supported_arithmetic())
  {
    if (error) {
      *error = "NURBS cuts need 1–1000 cuts, a finite error budget and supported arithmetic";
    }
    return false;
  }
  return true;
}

static double distance_upper(const double3 &a, const double3 &b)
{
  double bound = 0;
  for (int axis = 0; axis < 3; axis++) {
    double value, residual;
    if (!detail::sum_with_error(a[axis], -b[axis], value, residual)) {
      return INFINITY;
    }
    bound = detail::add_upper(bound, detail::add_upper(std::abs(value), std::abs(residual)));
  }
  return bound;
}

/** Conservative positive-rational support Lipschitz bounds. Needed only to convert a measured
 * parameter-map enclosure into object units; this is not a fitting tolerance or tessellation
 * bound.
 */
static double2 lipschitz(const NurbsSurfaceGeometry &surface)
{
  double minimum = INFINITY, maximum = 0, extent = 0;
  const double3 anchor(surface.points[0].position);
  for (int i = 0; i < surface.points_num; i++) {
    if (!(surface.points[i].weight > 0)) {
      return double2(INFINITY);
    }
    minimum = std::min(minimum, surface.points[i].weight);
    maximum = std::max(maximum, surface.points[i].weight);
    extent = std::max(extent, distance_upper(double3(surface.points[i].position), anchor));
  }
  if (extent == 0) {
    return double2(0);
  }
  const double ratio = std::nextafter(maximum / minimum, INFINITY);
  double2 result;
  for (int axis = 0; axis < 2; axis++) {
    const double *knots = axis ? surface.knots_v : surface.knots_u;
    const int count = axis ? surface.knots_v_num : surface.knots_u_num;
    const int degree = (axis ? surface.order_v : surface.order_u) - 1;
    double span = INFINITY;
    for (int i = 1; i < count; i++) {
      if (knots[i] > knots[i - 1]) {
        double value, residual;
        if (!detail::sum_with_error(knots[i], -knots[i - 1], value, residual)) {
          return double2(INFINITY);
        }
        if (residual < 0) {
          value = std::nextafter(value, -INFINITY);
        }
        span = std::min(span, value);
      }
    }
    const double numerator = detail::multiply_upper(detail::multiply_upper(4.0 * degree, ratio),
                                                    extent);
    result[axis] = std::nextafter(numerator / span, INFINITY);
  }
  return result;
}

}  // namespace

static std::optional<TopologyCutPlan> plan_loop_cut_impl(const NurbsGeometry &source,
                                                         const int64_t seed_edge,
                                                         const int cuts,
                                                         const double factor,
                                                         const double max_error,
                                                         std::string *r_error,
                                                         const Span<double> seed_parameters)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<TopologyCutPlan> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!request_valid(cuts, max_error, r_error)) {
    return std::nullopt;
  }
  if (!seed_parameters.is_empty() && seed_parameters.size() != cuts) {
    return fail("NURBS loop cut requires one seed parameter per cut");
  }
  if (!std::isfinite(factor) || factor <= -1 || factor >= 1) {
    return fail("NURBS cut slide must remain strictly between the existing boundaries");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index || !index->edge(seed_edge)) {
    return fail("NURBS loop cut seed edge does not exist");
  }
  const auto curves = curve_index(source);
  Map<int64_t, Vector<double>> transported;
  Map<int64_t, Vector<FaceIsoCut>> face_cuts;
  Map<int64_t, Set<std::pair<int, int>>> visited_faces;
  Vector<std::pair<int64_t, int>> pending;
  TopologyCutPlan plan;
  auto canonical_parameter =
      [&](const int64_t edge_id, const int root, const double candidate) -> std::optional<double> {
    const auto &edge = *index->edge(edge_id);
    if (!std::isfinite(candidate) || candidate <= edge.parameter_range[0] ||
        candidate >= edge.parameter_range[1])
    {
      return std::nullopt;
    }
    auto &parameters = transported.lookup_or_add(edge_id, Vector<double>(cuts, NAN));
    if (!std::isnan(parameters[root])) {
      if (parameters[root] != candidate) {
        const auto a = evaluate_curve_bounded(
            *curves.lookup(edge.curve_id), parameters[root], max_error);
        const auto b = evaluate_curve_bounded(*curves.lookup(edge.curve_id), candidate, max_error);
        if (!a || !b) {
          return std::nullopt;
        }
        const double bound = detail::add_upper(distance_upper(a->position, b->position),
                                               detail::add_upper(a->error_bound, b->error_bound));
        if (bound > max_error) {
          return std::nullopt;
        }
        plan.transport_error_bound = std::max(plan.transport_error_bound, bound);
      }
      return parameters[root];
    }
    parameters[root] = candidate;
    pending.append({edge_id, root});
    return candidate;
  };
  const auto &seed = *index->edge(seed_edge);
  for (int i = 0; i < cuts; i++) {
    const double t = seed_parameters.is_empty() ?
                         seed.parameter_range[0] +
                             (seed.parameter_range[1] - seed.parameter_range[0]) *
                                 ((i + 1.0 + factor) / (cuts + 1.0)) :
                         seed_parameters[i];
    if (!canonical_parameter(seed_edge, i, t)) {
      return fail("NURBS loop cut parameters are not representable inside the seed edge");
    }
  }
  for (int current = 0; current < pending.size(); current++) {
    if (pending.size() > 1000000) {
      return fail("NURBS loop cut exceeds the source partition budget");
    }
    const auto [edge_id, root] = pending[current];
    const auto uses = index->uses_for_edge(edge_id);
    if (uses.size() > 2) {
      return fail("NURBS loop cut cannot traverse a non-manifold edge");
    }
    for (const auto &reference : uses) {
      const auto &face = *index->face(reference.face_id);
      const auto rect = rectangle(face, *index, curves);
      if (!rect) {
        continue;
      }
      for (int side = 0; side < 4; side++) {
        if (rect->edges[side]->id != edge_id) {
          continue;
        }
        const int axis = side % 2 == 0 ? 0 : 1;
        if (!visited_faces.lookup_or_add_default(face.id).add({axis, root})) {
          continue;
        }
        const double parameter = source_coordinate(*rect->edges[side],
                                                   *curves.lookup(rect->uses[side]->pcurve_id),
                                                   axis,
                                                   transported.lookup(edge_id)[root]);
        if (!(parameter > rect->low[axis] && parameter < rect->high[axis])) {
          return fail("NURBS loop cut cannot map this boundary into the support interior");
        }
        const int opposite = (side + 2) % 4;
        const auto other = canonical_parameter(
            rect->edges[opposite]->id, root, edge_parameter(*rect, opposite, axis, parameter));
        if (!other) {
          return fail("NURBS loop cut has inconsistent source parameter transport");
        }
        FaceIsoCut line{axis, parameter, {}};
        const int end = side == end_side(axis, 0) ? 0 : 1;
        line.boundary_parameters[end] = transported.lookup(edge_id)[root];
        line.boundary_parameters[1 - end] = *other;
        face_cuts.lookup_or_add_default(face.id).append(line);
      }
    }
  }
  if (face_cuts.is_empty()) {
    return fail("NURBS loop cut requires a rectangular face beside the seed edge");
  }
  for (const auto &edge : source.edges) {
    if (auto *parameters = transported.lookup_ptr(edge.id)) {
      Vector<double> values;
      for (const double t : *parameters) {
        if (!std::isnan(t)) {
          values.append(t);
        }
      }
      std::sort(values.begin(), values.end());
      if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
        return fail("NURBS loop cut produced coincident logical cuts");
      }
      plan.edges.append({edge.id, std::move(values)});
    }
  }
  for (const auto &face : source.faces) {
    if (auto *lines = face_cuts.lookup_ptr(face.id)) {
      plan.faces.append({face.id, std::move(*lines)});
    }
  }
  return plan;
}

std::optional<TopologyCutPlan> plan_loop_cut(const NurbsGeometry &source,
                                             const int64_t seed_edge,
                                             const int cuts,
                                             const double factor,
                                             const double max_error,
                                             std::string *r_error)
{
  return plan_loop_cut_impl(source, seed_edge, cuts, factor, max_error, r_error, {});
}

std::optional<TopologyCutPlan> detail::plan_loop_cut_parameters(const NurbsGeometry &source,
                                                                const int64_t seed_edge,
                                                                const Span<double> parameters,
                                                                const double max_error,
                                                                std::string *r_error)
{
  return plan_loop_cut_impl(
      source, seed_edge, parameters.size(), 0, max_error, r_error, parameters);
}

namespace {

struct EdgePieces {
  Vector<double> parameters;
  Vector<int64_t> vertices, edges;
};
struct LoopSegment {
  int64_t edge;
  std::array<double2, 2> uv;
  bool reversed;
};

static NurbsCurveGeometry *install_curve(detail::TopologyBuilder &builder, OwnedCurve curve)
{
  curve->id = builder.id();
  auto *result = curve.release();
  BLI_addtail(&builder.geometry->curves, result);
  if (result->id) {
    builder.curves.add_new(result->id, result);
  }
  return result;
}
static NurbsCurveGeometry *parameter_line(detail::TopologyBuilder &builder,
                                          const std::array<double2, 2> &uv,
                                          const double2 &range)
{
  OwnedCurve curve(MEM_new<NurbsCurveGeometry>(__func__));
  curve->dimension = 2;
  curve->order = curve->points_num = 2;
  curve->knots_num = 4;
  curve->points = MEM_new_array<NurbsControlPoint>(2, __func__);
  curve->knots = MEM_new_array<double>(4, __func__);
  for (int end = 0; end < 2; end++) {
    curve->points[end].position[0] = uv[end].x;
    curve->points[end].position[1] = uv[end].y;
    curve->knots[end * 2] = curve->knots[end * 2 + 1] = range[end];
  }
  return install_curve(builder, std::move(curve));
}

}  // namespace

std::optional<TopologyCutResult> partition_components(const NurbsGeometry &source,
                                                      const TopologyCutPlan &plan,
                                                      const double max_error,
                                                      std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<TopologyCutResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!request_valid(1, max_error, r_error)) {
    return std::nullopt;
  }
  if (!std::isfinite(plan.transport_error_bound) || plan.transport_error_bound < 0 ||
      plan.transport_error_bound > max_error)
  {
    return fail("NURBS partition transport exceeds its construction budget");
  }
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  const auto curves = curve_index(source);
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const auto &surface : source.surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  Map<int64_t, Vector<double>> edge_parameters;
  uint64_t estimate = 0, new_vertices = 0;
  int maximum_cuts = 0;
  for (const auto &request : plan.edges) {
    const auto *edge = index->edge(request.edge_id);
    if (!edge || request.parameters.is_empty() || request.parameters.size() > 1000) {
      return fail("NURBS edge partition needs an existing edge and 1–1000 interior parameters");
    }
    for (const double parameter : request.parameters) {
      if (!std::isfinite(parameter) || parameter <= edge->parameter_range[0] ||
          parameter >= edge->parameter_range[1])
      {
        return fail(
            "NURBS edge partition parameters must be finite and strictly inside the source "
            "interval");
      }
    }
    auto &values = edge_parameters.lookup_or_add_default(edge->id);
    values.extend(request.parameters);
  }
  for (auto item : edge_parameters.items()) {
    auto &values = item.value;
    std::sort(values.begin(), values.end());
    values.resize(std::unique(values.begin(), values.end()) - values.begin());
    if (values.size() > 1000) {
      return fail("NURBS edge partition exceeds 1000 cuts per source edge");
    }
    const auto &edge = *index->edge(item.key);
    for (const double parameter : values) {
      if (!std::isfinite(parameter) || parameter <= edge.parameter_range[0] ||
          parameter >= edge.parameter_range[1])
      {
        return fail("NURBS edge partition parameters must be strictly inside the source interval");
      }
    }
    const uint64_t count = uint64_t(values.size());
    estimate += count * (3 + index->uses_for_edge(edge.id).size()) + 1;
    new_vertices += count;
    maximum_cuts = std::max(maximum_cuts, int(count));
  }
  Set<int64_t> requested_faces;
  for (const auto &request : plan.faces) {
    const auto *face = index->face(request.face_id);
    if (!face || !requested_faces.add(face->id) || request.cuts.is_empty() ||
        request.cuts.size() > 2000)
    {
      return fail("NURBS face partition needs one request per existing face");
    }
    int nx = 1, ny = 1;
    for (const auto &line : request.cuts) {
      if (line.axis == 0) {
        nx++;
      }
      else if (line.axis == 1) {
        ny++;
      }
      else {
        return fail("NURBS face cuts need a U or V axis");
      }
    }
    const uint64_t cells = uint64_t(nx) * uint64_t(ny);
    if (cells > 1000000) {
      return fail("NURBS face partition exceeds the cell budget");
    }
    estimate += cells * (3 + 8 * uint64_t(maximum_cuts + 1)) +
                2 * (uint64_t(nx - 1) * ny + uint64_t(ny - 1) * nx);
    const uint64_t interior = uint64_t(nx - 1) * uint64_t(ny - 1);
    new_vertices += interior;
    estimate += interior;
  }
  if (new_vertices > 1000000 ||
      estimate > uint64_t(std::numeric_limits<int64_t>::max() - source.next_id))
  {
    return fail("NURBS partition exceeds its vertex budget or available identity space");
  }
  detail::TopologyBuilder builder(source);
  TopologyCutResult result;
  result.edit.geometry_error_bound = plan.transport_error_bound;
  Map<int64_t, EdgePieces> pieces;
  Set<int64_t> changed_edges, changed_faces, changed_vertices;
  for (const auto &edge : source.edges) {
    EdgePieces split;
    split.parameters.append(edge.parameter_range[0]);
    if (const auto *parameters = edge_parameters.lookup_ptr(edge.id)) {
      split.parameters.extend(*parameters);
    }
    split.parameters.append(edge.parameter_range[1]);
    split.vertices.append(edge.vertex_ids[0]);
    if (split.parameters.size() == 2) {
      split.vertices.append(edge.vertex_ids[1]);
      split.edges.append(edge.id);
      pieces.add_new(edge.id, std::move(split));
      continue;
    }
    for (int i = 1; i < split.parameters.size() - 1; i++) {
      const auto point = evaluate_curve_bounded(
          *curves.lookup(edge.curve_id), split.parameters[i], max_error, r_error);
      if (!point) {
        return std::nullopt;
      }
      auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
      vertex->id = builder.id();
      std::copy_n(&point->position.x, 3, vertex->position);
      vertex->tolerance = detail::add_upper(edge.tolerance, point->error_bound);
      BLI_addtail(&builder.geometry->vertices, vertex);
      builder.vertices.add_new(vertex->id, vertex);
      split.vertices.append(vertex->id);
      result.edit.vertices.append(vertex->id);
      changed_vertices.add(vertex->id);
      result.edit.geometry_error_bound = std::max(result.edit.geometry_error_bound,
                                                  point->error_bound);
    }
    split.vertices.append(edge.vertex_ids[1]);
    for (int i = 0; i < split.parameters.size() - 1; i++) {
      auto restricted = restrict_curve(*curves.lookup(edge.curve_id),
                                       {split.parameters[i], split.parameters[i + 1]},
                                       max_error,
                                       r_error);
      if (!restricted) {
        return std::nullopt;
      }
      result.edit.geometry_error_bound = std::max(result.edit.geometry_error_bound,
                                                  restricted->geometry_error_bound);
      NurbsTopologyEdge *segment = i == 0 ? builder.edges.lookup(edge.id) :
                                            MEM_new<NurbsTopologyEdge>(__func__, edge);
      if (i != 0) {
        segment->id = builder.id();
        BLI_addtail(&builder.geometry->edges, segment);
        builder.edges.add_new(segment->id, segment);
      }
      segment->curve_id = install_curve(builder, std::move(restricted->curve))->id;
      segment->vertex_ids[0] = split.vertices[i];
      segment->vertex_ids[1] = split.vertices[i + 1];
      segment->parameter_range[0] = split.parameters[i];
      segment->parameter_range[1] = split.parameters[i + 1];
      changed_edges.add(segment->id);
      split.edges.append(segment->id);
    }
    result.edge_descendants.add_new(edge.id, split.edges);
    result.edit.source_to_result.add_new(edge.id, split.edges[0]);
    pieces.add_new(edge.id, std::move(split));
  }
  /* Every incident use follows the same edge partition, including faces outside the cut ring. */
  for (auto &face : builder.geometry->faces) {
    for (auto &loop : face.loops) {
      for (auto &use : loop.edge_uses.items_mutable()) {
        const auto &split = pieces.lookup(use.edge_id);
        if (split.edges.size() == 1) {
          continue;
        }
        const NurbsTopologyEdgeUse original = use;
        use.edge_id = split.edges[use.reversed ? split.edges.size() - 1 : 0];
        NurbsTopologyEdgeUse *previous = &use;
        for (int i = 1; i < split.edges.size(); i++) {
          auto *copy = MEM_new<NurbsTopologyEdgeUse>(__func__, original);
          copy->id = builder.id();
          copy->edge_id = split.edges[use.reversed ? split.edges.size() - 1 - i : i];
          BLI_insertlinkafter(&loop.edge_uses, previous, copy);
          previous = copy;
        }
        changed_faces.add(face.id);
      }
    }
  }
  for (const auto &request : plan.faces) {
    const auto &original = *index->face(request.face_id);
    const auto rect = rectangle(original, *index, curves);
    if (!rect) {
      return fail("NURBS face partition requires one rectangular affine parameter boundary");
    }
    const auto &support = *surfaces.lookup(original.surface_id);
    const double2 derivative_bound = lipschitz(support);
    std::array<Vector<FaceIsoCut>, 2> lines;
    for (const auto &line : request.cuts) {
      if (!std::isfinite(line.parameter) || line.parameter <= rect->low[line.axis] ||
          line.parameter >= rect->high[line.axis])
      {
        return fail("NURBS face cuts must lie strictly inside their source support rectangle");
      }
      lines[line.axis].append(line);
    }
    std::array<Vector<double>, 2> coordinates;
    for (int axis = 0; axis < 2; axis++) {
      std::sort(lines[axis].begin(), lines[axis].end(), [](const auto &a, const auto &b) {
        return a.parameter < b.parameter;
      });
      coordinates[axis].append(rect->low[axis]);
      for (const auto &line : lines[axis]) {
        if (line.parameter == coordinates[axis].last()) {
          return fail("NURBS face partition repeats a cut coordinate");
        }
        coordinates[axis].append(line.parameter);
      }
      coordinates[axis].append(rect->high[axis]);
    }
    const int nu = int(coordinates[0].size()), nv = int(coordinates[1].size());
    auto boundary_parameter = [&](const int side, const int coordinate) {
      const int axis = side % 2 == 0 ? 0 : 1;
      if (coordinate == 0 || coordinate == coordinates[axis].size() - 1) {
        return edge_parameter(*rect, side, axis, coordinates[axis][coordinate]);
      }
      const int end = side == end_side(axis, 0) ? 0 : 1;
      return lines[axis][coordinate - 1].boundary_parameters[end];
    };
    auto boundary_vertex = [&](const int side, const int coordinate) -> int64_t {
      const auto &split = pieces.lookup(rect->edges[side]->id);
      const double parameter = boundary_parameter(side, coordinate);
      const int i = split.parameters.first_index_of_try(parameter);
      return i < 0 ? 0 : split.vertices[i];
    };
    Vector<int64_t> grid(nu * nv, 0);
    for (int j = 0; j < nv; j++) {
      for (int i = 0; i < nu; i++) {
        int64_t id = 0;
        if ((i == 0 || i == nu - 1) && (j == 0 || j == nv - 1)) {
          id = rect->corners[(i == nu - 1 ? 1 : 0) + (j == nv - 1 ? 2 : 0)];
        }
        else if (j == 0 || j == nv - 1) {
          id = boundary_vertex(j == 0 ? 0 : 2, i);
        }
        else if (i == 0 || i == nu - 1) {
          id = boundary_vertex(i == 0 ? 3 : 1, j);
        }
        else {
          auto iso = surface_isocurve(support, 0, coordinates[0][i], max_error, r_error);
          if (!iso) {
            return std::nullopt;
          }
          const auto point = evaluate_curve_bounded(
              *iso->curve, coordinates[1][j], max_error, r_error);
          if (!point) {
            return std::nullopt;
          }
          const double error = detail::add_upper(iso->geometry_error_bound, point->error_bound);
          result.edit.geometry_error_bound = std::max(result.edit.geometry_error_bound, error);
          auto *vertex = MEM_new<NurbsTopologyVertex>(__func__);
          vertex->id = builder.id();
          std::copy_n(&point->position.x, 3, vertex->position);
          vertex->tolerance = detail::add_upper(original.tolerance, error);
          BLI_addtail(&builder.geometry->vertices, vertex);
          builder.vertices.add_new(vertex->id, vertex);
          id = vertex->id;
          result.edit.vertices.append(id);
          changed_vertices.add(id);
        }
        if (!id) {
          return fail("NURBS face cut lost its logical shared-edge intersection");
        }
        grid[j * nu + i] = id;
      }
    }
    Map<std::pair<int, int>, int64_t> vertical, horizontal;
    auto add_isoline = [&](const int axis, const int fixed, const int varying) -> bool {
      auto iso = surface_isocurve(support, axis, coordinates[axis][fixed], max_error, r_error);
      if (!iso) {
        return false;
      }
      const double2 range(coordinates[1 - axis][varying], coordinates[1 - axis][varying + 1]);
      auto restricted = restrict_curve(*iso->curve, range, max_error, r_error);
      if (!restricted) {
        return false;
      }
      const double error = detail::add_upper(iso->geometry_error_bound,
                                             restricted->geometry_error_bound);
      result.edit.geometry_error_bound = std::max(result.edit.geometry_error_bound, error);
      auto *edge = MEM_new<NurbsTopologyEdge>(__func__);
      edge->id = builder.id();
      edge->curve_id = install_curve(builder, std::move(restricted->curve))->id;
      edge->vertex_ids[0] = grid[axis == 0 ? varying * nu + fixed : fixed * nu + varying];
      edge->vertex_ids[1] =
          grid[axis == 0 ? (varying + 1) * nu + fixed : fixed * nu + varying + 1];
      edge->parameter_range[0] = range.x;
      edge->parameter_range[1] = range.y;
      edge->tolerance = detail::add_upper(original.tolerance, error);
      BLI_addtail(&builder.geometry->edges, edge);
      builder.edges.add_new(edge->id, edge);
      (axis == 0 ? vertical : horizontal).add_new({fixed, varying}, edge->id);
      changed_edges.add(edge->id);
      result.edit.edges.append(edge->id);
      return true;
    };
    for (int i = 1; i < nu - 1; i++) {
      for (int j = 0; j < nv - 1; j++) {
        if (!add_isoline(0, i, j)) {
          return std::nullopt;
        }
      }
    }
    for (int j = 1; j < nv - 1; j++) {
      for (int i = 0; i < nu - 1; i++) {
        if (!add_isoline(1, j, i)) {
          return std::nullopt;
        }
      }
    }
    auto boundary_segments =
        [&](const int side, const int interval, Vector<LoopSegment> &output) -> bool {
      const int axis = side % 2 == 0 ? 0 : 1;
      const auto &split = pieces.lookup(rect->edges[side]->id);
      const double start = boundary_parameter(side, interval),
                   end = boundary_parameter(side, interval + 1);
      const int first = split.parameters.first_index_of_try(start),
                last = split.parameters.first_index_of_try(end);
      if (first < 0 || last < 0 || first == last) {
        return false;
      }
      const bool reversed = first > last;
      for (int k = first; k != last; k += reversed ? -1 : 1) {
        const int segment = reversed ? k - 1 : k;
        const auto &edge = *builder.edges.lookup(split.edges[segment]);
        std::array<double2, 2> uv;
        for (int endpoint = 0; endpoint < 2; endpoint++) {
          const double parameter = edge.parameter_range[endpoint];
          uv[endpoint] = rect->uv[side][0];
          uv[endpoint][axis] = parameter == start ?
                                   coordinates[axis][interval] :
                               parameter == end ?
                                   coordinates[axis][interval + 1] :
                                   source_coordinate(*rect->edges[side],
                                                     *curves.lookup(rect->uses[side]->pcurve_id),
                                                     axis,
                                                     parameter);
          const auto original_uv = evaluate_curve_bounded(
              *curves.lookup(rect->uses[side]->pcurve_id),
              parameter,
              std::numeric_limits<double>::max(),
              r_error);
          if (!original_uv) {
            return false;
          }
          const double uv_error = detail::add_upper(
              distance_upper(original_uv->position, {uv[endpoint].x, uv[endpoint].y, 0}),
              original_uv->error_bound);
          const double error = uv_error == 0 ?
                                   0 :
                                   detail::multiply_upper(
                                       uv_error, std::max(derivative_bound.x, derivative_bound.y));
          result.edit.geometry_error_bound = std::max(result.edit.geometry_error_bound, error);
        }
        output.append({edge.id, uv, reversed});
      }
      return true;
    };
    auto erase = builder.faces.lookup(original.id);
    for (auto &loop : erase->loops) {
      BLI_freelistN(&loop.edge_uses);
    }
    BLI_freelistN(&erase->loops);
    BLI_remlink(&builder.geometry->faces, erase);
    MEM_delete(erase);
    Vector<int64_t> descendants;
    for (int j = 0; j < nv - 1; j++) {
      for (int i = 0; i < nu - 1; i++) {
        auto restricted = restrict_surface(support,
                                           {coordinates[0][i], coordinates[1][j]},
                                           {coordinates[0][i + 1], coordinates[1][j + 1]},
                                           max_error,
                                           r_error);
        if (!restricted) {
          return std::nullopt;
        }
        result.edit.geometry_error_bound = std::max(result.edit.geometry_error_bound,
                                                    restricted->geometry_error_bound);
        auto *surface = restricted->surface.release();
        surface->id = builder.id();
        BLI_addtail(&builder.geometry->surfaces, surface);
        builder.surfaces.add_new(surface->id, surface);
        auto *face = MEM_new<NurbsTopologyFace>(__func__, original);
        face->id = descendants.is_empty() ? original.id : builder.id();
        face->surface_id = surface->id;
        face->loops.clear_no_delete();
        BLI_addtail(&builder.geometry->faces, face);
        builder.faces.add_overwrite(face->id, face);
        descendants.append(face->id);
        result.edit.faces.append(face->id);
        changed_faces.add(face->id);
        auto *loop = MEM_new<NurbsTopologyLoop>(__func__);
        loop->id = builder.id();
        BLI_addtail(&face->loops, loop);
        std::array<Vector<LoopSegment>, 4> sides;
        if (j == 0) {
          if (!boundary_segments(0, i, sides[0])) {
            return fail("NURBS cut lost its bottom boundary");
          }
        }
        else {
          sides[0].append({horizontal.lookup({j, i}),
                           {{{coordinates[0][i], coordinates[1][j]},
                             {coordinates[0][i + 1], coordinates[1][j]}}},
                           false});
        }
        if (i == nu - 2) {
          if (!boundary_segments(1, j, sides[1])) {
            return fail("NURBS cut lost its right boundary");
          }
        }
        else {
          sides[1].append({vertical.lookup({i + 1, j}),
                           {{{coordinates[0][i + 1], coordinates[1][j]},
                             {coordinates[0][i + 1], coordinates[1][j + 1]}}},
                           false});
        }
        if (j == nv - 2) {
          if (!boundary_segments(2, i, sides[2])) {
            return fail("NURBS cut lost its top boundary");
          }
        }
        else {
          sides[2].append({horizontal.lookup({j + 1, i}),
                           {{{coordinates[0][i], coordinates[1][j + 1]},
                             {coordinates[0][i + 1], coordinates[1][j + 1]}}},
                           false});
        }
        if (i == 0) {
          if (!boundary_segments(3, j, sides[3])) {
            return fail("NURBS cut lost its left boundary");
          }
        }
        else {
          sides[3].append({vertical.lookup({i, j}),
                           {{{coordinates[0][i], coordinates[1][j]},
                             {coordinates[0][i], coordinates[1][j + 1]}}},
                           false});
        }
        Vector<LoopSegment> boundary;
        for (int side = 0; side < 4; side++) {
          if (side >= 2) {
            std::reverse(sides[side].begin(), sides[side].end());
            for (auto &segment : sides[side]) {
              segment.reversed = !segment.reversed;
            }
          }
          boundary.extend(sides[side]);
        }
        if (!rect->counterclockwise) {
          std::reverse(boundary.begin(), boundary.end());
          for (auto &segment : boundary) {
            segment.reversed = !segment.reversed;
          }
        }
        for (const auto &segment : boundary) {
          const auto &edge = *builder.edges.lookup(segment.edge);
          auto *use = MEM_new<NurbsTopologyEdgeUse>(__func__);
          use->id = builder.id();
          use->edge_id = edge.id;
          use->reversed = segment.reversed;
          use->pcurve_id = parameter_line(builder,
                                          segment.uv,
                                          {edge.parameter_range[0], edge.parameter_range[1]})
                               ->id;
          use->tolerance = original.tolerance;
          BLI_addtail(&loop->edge_uses, use);
        }
      }
    }
    result.face_descendants.add_new(original.id, std::move(descendants));
    result.edit.source_to_result.add_new(original.id, original.id);
  }
  if (!builder.valid()) {
    return fail("NURBS identity space is exhausted");
  }
  if (!std::isfinite(result.edit.geometry_error_bound) ||
      result.edit.geometry_error_bound > max_error)
  {
    return fail("NURBS partition construction exceeds the requested error budget");
  }
  const double extra = detail::multiply_upper(2, result.edit.geometry_error_bound);
  for (auto &vertex : builder.geometry->vertices) {
    if (changed_vertices.contains(vertex.id)) {
      vertex.tolerance = detail::add_upper(vertex.tolerance, extra);
    }
  }
  for (auto &edge : builder.geometry->edges) {
    if (changed_edges.contains(edge.id)) {
      edge.tolerance = detail::add_upper(edge.tolerance, extra);
    }
  }
  for (auto &face : builder.geometry->faces) {
    if (changed_faces.contains(face.id)) {
      face.tolerance = detail::add_upper(face.tolerance, extra);
    }
    for (auto &loop : face.loops) {
      for (auto &use : loop.edge_uses) {
        if (changed_edges.contains(use.edge_id) || changed_faces.contains(face.id)) {
          use.tolerance = detail::add_upper(use.tolerance, extra);
        }
      }
    }
  }
  if (!geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  result.edit.geometry = std::move(builder.geometry);
  return result;
}

std::optional<TopologyEditResult> subdivide_components(const NurbsGeometry &source,
                                                       const ComponentSelection &selection,
                                                       const int cuts,
                                                       const double max_error,
                                                       std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<TopologyEditResult> {
    if (r_error) {
      *r_error = message;
    }
    return std::nullopt;
  };
  if (!request_valid(cuts, max_error, r_error)) {
    return std::nullopt;
  }
  const auto closure = component_closure(source, selection, r_error);
  const auto index = TopologyIndex::build(source, r_error);
  if (!closure || !index) {
    return std::nullopt;
  }
  if (closure->edges.is_empty()) {
    return fail("NURBS subdivision needs selected edges or faces");
  }
  const auto curves = curve_index(source);
  TopologyCutPlan plan;
  Map<int64_t, Vector<double>> parameters;
  for (const auto &edge : source.edges) {
    if (!closure->edges.contains(edge.id)) {
      continue;
    }
    Vector<double> values;
    for (int i = 0; i < cuts; i++) {
      values.append(edge.parameter_range[0] + (edge.parameter_range[1] - edge.parameter_range[0]) *
                                                  ((i + 1.0) / (cuts + 1.0)));
    }
    parameters.add_new(edge.id, values);
    plan.edges.append({edge.id, std::move(values)});
  }
  for (const auto &face : source.faces) {
    bool any = false;
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        any |= closure->edges.contains(use.edge_id);
      }
    }
    if (!any) {
      continue;
    }
    const auto rect = rectangle(face, *index, curves);
    if (!rect) {
      return fail("NURBS subdivision currently requires rectangular faces or independent wires");
    }
    std::array<bool, 4> selected;
    int count = 0;
    for (int side = 0; side < 4; side++) {
      selected[side] = closure->edges.contains(rect->edges[side]->id);
      count += selected[side];
    }
    if (count == 1) {
      continue;
    }
    if (count == 3 || (count == 2 && !(selected[0] == selected[2] && selected[1] == selected[3])))
    {
      return fail(
          "Adjacent-side NURBS subdivision needs a diagonal parameter-space partition; select "
          "one, opposite or all sides");
    }
    FacePartitionRequest request{face.id, {}};
    for (int axis = 0; axis < 2; axis++) {
      const int low_side = end_side(axis, 0), high_side = end_side(axis, 1);
      if (!selected[low_side] || !selected[high_side]) {
        continue;
      }
      const bool low_increasing = rect->uv[low_side][0][axis] < rect->uv[low_side][1][axis];
      const bool high_increasing = rect->uv[high_side][0][axis] < rect->uv[high_side][1][axis];
      for (int i = 0; i < cuts; i++) {
        const double low_t = parameters.lookup(
            rect->edges[low_side]->id)[low_increasing ? i : cuts - 1 - i];
        const double high_t = parameters.lookup(
            rect->edges[high_side]->id)[high_increasing ? i : cuts - 1 - i];
        const double uv = source_coordinate(
            *rect->edges[low_side], *curves.lookup(rect->uses[low_side]->pcurve_id), axis, low_t);
        request.cuts.append({axis, uv, {low_t, high_t}});
      }
    }
    if (!request.cuts.is_empty()) {
      plan.faces.append(std::move(request));
    }
  }
  auto partitioned = partition_components(source, plan, max_error, r_error);
  if (!partitioned) {
    return std::nullopt;
  }
  TopologyEditResult result = std::move(partitioned->edit);
  Set<int64_t> vertices(result.vertices.as_span());
  for (const int64_t id : closure->vertices) {
    vertices.add(id);
  }
  result.vertices.clear();
  for (const auto &vertex : result.geometry->vertices) {
    if (vertices.contains(vertex.id)) {
      result.vertices.append(vertex.id);
    }
  }
  Set<int64_t> edges(result.edges.as_span());
  for (const int64_t id : closure->edges) {
    if (const auto *children = partitioned->edge_descendants.lookup_ptr(id)) {
      edges.add_multiple(*children);
    }
  }
  result.edges.clear();
  for (const auto &edge : result.geometry->edges) {
    if (edges.contains(edge.id)) {
      result.edges.append(edge.id);
    }
  }
  result.faces.clear();
  for (const int64_t id : closure->faces) {
    if (const auto *children = partitioned->face_descendants.lookup_ptr(id)) {
      result.faces.extend(*children);
    }
  }
  return result;
}

}  // namespace blender::bke::nurbs
