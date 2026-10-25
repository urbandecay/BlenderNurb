/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstring>

#include "BLI_array.hh"
#include "BLI_kdtree.hh"
#include "BLI_math_vector.hh"
#include "BLI_memory_utils.hh"

#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_topology_edit.hh"
#include "BKE_nurbs_transform.hh"

#include "nurbs_math.hh"
#include "nurbs_topology_edit.hh"

namespace blender::bke::nurbs {
namespace {

static bool full_linear(const NurbsCurveGeometry &curve, const NurbsTopologyEdge &edge)
{
  return curve.order == 2 && curve.points_num == 2 && curve.knots[0] == edge.parameter_range[0] &&
         curve.knots[1] == edge.parameter_range[0] && curve.knots[2] == edge.parameter_range[1] &&
         curve.knots[3] == edge.parameter_range[1];
}

static bool same_curve(const NurbsCurveGeometry &a,
                       const NurbsCurveGeometry &b,
                       const bool reverse)
{
  if (a.order != b.order || a.points_num != b.points_num || a.knots_num != b.knots_num) {
    return false;
  }
  double sum = 0, residual = 0;
  if (reverse && (!detail::sum_with_error(a.knots[0], a.knots[a.knots_num - 1], sum, residual) ||
                  residual != 0))
  {
    return false;
  }
  for (int i = 0; i < a.points_num; i++) {
    const auto &other = b.points[reverse ? b.points_num - 1 - i : i];
    if (a.points[i].weight != other.weight ||
        double3(a.points[i].position) != double3(other.position))
    {
      return false;
    }
  }
  for (int i = 0; i < a.knots_num; i++) {
    if (reverse) {
      double value, error;
      if (!detail::sum_with_error(a.knots[i], b.knots[b.knots_num - 1 - i], value, error) ||
          error != 0 || value != sum)
      {
        return false;
      }
    }
    else if (a.knots[i] != b.knots[i]) {
      return false;
    }
  }
  return true;
}

static bool same_surface(const NurbsSurfaceGeometry &a, const NurbsSurfaceGeometry &b)
{
  return a.order_u == b.order_u && a.order_v == b.order_v && a.points_u == b.points_u &&
         a.points_v == b.points_v && a.knots_u_num == b.knots_u_num &&
         a.knots_v_num == b.knots_v_num &&
         std::memcmp(a.points, b.points, sizeof(NurbsControlPoint) * a.points_num) == 0 &&
         std::memcmp(a.knots_u, b.knots_u, sizeof(double) * a.knots_u_num) == 0 &&
         std::memcmp(a.knots_v, b.knots_v, sizeof(double) * a.knots_v_num) == 0;
}

static void remove_face(NurbsGeometry &geometry, NurbsTopologyFace &face)
{
  for (auto &loop : face.loops) {
    BLI_freelistN(&loop.edge_uses);
  }
  BLI_freelistN(&face.loops);
  BLI_remlink(&geometry.faces, &face);
  MEM_delete(&face);
}

struct FaceRepair {
  int64_t id;
  int material;
  double tolerance;
  std::optional<double3> normal;
  Vector<Vector<int64_t>> cycles;
};

}  // namespace

std::optional<TopologyEditResult> merge_vertex_groups(const NurbsGeometry &source,
                                                      const Span<VertexMergeGroup> groups,
                                                      const double max_position_error,
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
  const auto index = TopologyIndex::build(source, r_error);
  if (!index) {
    return std::nullopt;
  }
  if (groups.is_empty()) {
    return fail("NURBS Merge needs selected vertices");
  }
  Map<int64_t, int64_t> vertex_map;
  Vector<VertexTransform> transforms;
  for (const auto &group : groups) {
    if (group.vertices.is_empty() || !group.vertices.contains(group.survivor) ||
        !std::isfinite(group.target.x) || !std::isfinite(group.target.y) ||
        !std::isfinite(group.target.z))
    {
      return fail("NURBS Merge needs a finite target and a surviving member of each group");
    }
    for (const int64_t id : group.vertices) {
      const auto *vertex = index->vertex(id);
      if (!vertex || !vertex_map.add(id, group.survivor)) {
        return fail("NURBS Merge groups must contain distinct existing vertices");
      }
      auto matrix = double4x4::identity();
      matrix.location() = group.target - double3(vertex->position);
      transforms.append({id, matrix});
    }
  }
  /* Plan faces which the weld removes before requesting geometric deformation.
   * BMesh also drops a face whose boundary has fewer than three surviving vertices.
   * A doomed linear triangle must not block welding by requesting a trim deformation.
   * Closed rational edges/faces are not classified by endpoint count alone. */
  Map<int64_t, const NurbsCurveGeometry *> source_curves;
  for (const auto &curve : source.curves) {
    source_curves.add_new(curve.id, &curve);
  }
  Set<int64_t> removed_faces;
  for (const auto &face : source.faces) {
    if (!face.loops.is_single() ||
        static_cast<const NurbsTopologyLoop *>(face.loops.first)->is_inner)
    {
      continue;
    }
    Set<int64_t> survivors;
    bool linear = true, changed = false;
    for (const auto &use : static_cast<const NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      linear &= full_linear(*source_curves.lookup(edge.curve_id), edge);
      for (const int64_t vertex : edge.vertex_ids) {
        const int64_t mapped = vertex_map.lookup_default(vertex, vertex);
        changed |= vertex != mapped;
        survivors.add(mapped);
      }
    }
    if (linear && changed && survivors.size() < 3) {
      removed_faces.add(face.id);
    }
  }
  OwnedGeometry deform_source;
  if (!removed_faces.is_empty()) {
    deform_source.reset(geometry_copy(&source));
    selection_storage_free(deform_source->stored_selection);
    deform_source->stored_selection = nullptr;
    for (auto &face : deform_source->faces.items_mutable()) {
      if (removed_faces.contains(face.id)) {
        remove_face(*deform_source, face);
      }
    }
  }
  auto moved = transform_vertices(
      deform_source ? *deform_source : source, transforms, max_position_error, r_error);
  if (!moved) {
    return std::nullopt;
  }
  detail::TopologyBuilder builder(*moved->geometry);
  TopologyEditResult result;
  result.geometry_error_bound = moved->position_error_bound;
  result.source_to_result = vertex_map;
  for (const int64_t id : removed_faces) {
    result.source_to_result.add_overwrite(id, 0);
  }
  for (const auto &group : groups) {
    auto &vertex = *builder.vertices.lookup(group.survivor);
    std::copy_n(&group.target.x, 3, vertex.position);
    for (const int64_t id : group.vertices) {
      vertex.tolerance = std::max(vertex.tolerance, builder.vertices.lookup(id)->tolerance);
    }
    result.vertices.append(group.survivor);
  }
  double repair_error = 0;
  Set<int64_t> constant_edges, affected_edges, rewired_edges, touched_faces;
  for (auto &edge : builder.geometry->edges) {
    const bool affected = vertex_map.contains(edge.vertex_ids[0]) ||
                          vertex_map.contains(edge.vertex_ids[1]);
    for (int end = 0; end < 2; end++) {
      const int64_t mapped = vertex_map.lookup_default(edge.vertex_ids[end], edge.vertex_ids[end]);
      if (mapped != edge.vertex_ids[end]) {
        rewired_edges.add(edge.id);
      }
      edge.vertex_ids[end] = mapped;
    }
    if (affected) {
      affected_edges.add(edge.id);
    }
    auto *curve = builder.curves.lookup(edge.curve_id);
    if (affected && full_linear(*curve, edge)) {
      curve = builder.copy_curve(curve->id);
      edge.curve_id = curve->id;
      for (int end = 0; end < 2; end++) {
        const double3 target(builder.vertices.lookup(edge.vertex_ids[end])->position);
        const double3 difference = target - double3(curve->points[end].position);
        const double deviation = std::hypot(difference.x, difference.y, difference.z);
        if (deviation != 0) {
          repair_error = std::max(
              repair_error, std::nextafter(deviation, std::numeric_limits<double>::infinity()));
        }
        std::copy_n(&target.x, 3, curve->points[end].position);
      }
    }
    if (edge.vertex_ids[0] == edge.vertex_ids[1]) {
      const double3 target(builder.vertices.lookup(edge.vertex_ids[0])->position);
      bool constant = true;
      for (int i = 0; i < curve->points_num; i++) {
        constant &= double3(curve->points[i].position) == target;
      }
      if (constant) {
        constant_edges.add(edge.id);
        result.source_to_result.add_overwrite(edge.id, 0);
      }
    }
  }
  result.geometry_error_bound = detail::add_upper(result.geometry_error_bound, repair_error);
  if (result.geometry_error_bound > max_position_error) {
    return fail("NURBS Merge boundary repair exceeds the requested construction error budget");
  }
  if (!builder.valid()) {
    return fail("NURBS identity space is exhausted");
  }
  const double extra = detail::multiply_upper(2, repair_error);
  for (auto &vertex : builder.geometry->vertices) {
    if (vertex_map.contains(vertex.id)) {
      vertex.tolerance = detail::expand_tolerance(vertex.tolerance, extra);
    }
  }
  for (auto &edge : builder.geometry->edges) {
    if (affected_edges.contains(edge.id)) {
      edge.tolerance = detail::expand_tolerance(edge.tolerance, extra);
    }
  }
  for (auto &face : builder.geometry->faces) {
    bool affected = false;
    for (auto &loop : face.loops) {
      for (auto &use : loop.edge_uses) {
        if (affected_edges.contains(use.edge_id)) {
          affected = true;
          use.tolerance = detail::expand_tolerance(use.tolerance, extra);
        }
      }
    }
    if (affected) {
      face.tolerance = detail::expand_tolerance(face.tolerance, extra);
    }
  }
  Map<int64_t, std::pair<int64_t, bool>> edge_map;
  Map<std::pair<int64_t, int64_t>, Vector<NurbsTopologyEdge *>> kept;
  for (auto &edge : builder.geometry->edges) {
    if (constant_edges.contains(edge.id)) {
      continue;
    }
    bool duplicate = false;
    const auto ends = std::minmax(edge.vertex_ids[0], edge.vertex_ids[1]);
    auto &candidates = kept.lookup_or_add_default({ends.first, ends.second});
    for (const auto *other : candidates) {
      if (!rewired_edges.contains(edge.id) && !rewired_edges.contains(other->id)) {
        continue;
      }
      if (edge.parameter_range[0] != other->parameter_range[0] ||
          edge.parameter_range[1] != other->parameter_range[1])
      {
        continue;
      }
      const bool direct = edge.vertex_ids[0] == other->vertex_ids[0] &&
                          edge.vertex_ids[1] == other->vertex_ids[1];
      const bool reverse = edge.vertex_ids[0] == other->vertex_ids[1] &&
                           edge.vertex_ids[1] == other->vertex_ids[0];
      const bool same_direction = direct && same_curve(*builder.curves.lookup(edge.curve_id),
                                                       *builder.curves.lookup(other->curve_id),
                                                       false);
      if (same_direction || (reverse && same_curve(*builder.curves.lookup(edge.curve_id),
                                                   *builder.curves.lookup(other->curve_id),
                                                   true)))
      {
        const bool reversed = !same_direction;
        edge_map.add_new(edge.id, {other->id, reversed});
        result.source_to_result.add_overwrite(edge.id, other->id);
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      candidates.append(&edge);
    }
  }
  Map<int64_t, int64_t> original_use_vertices;
  for (const auto &face : source.faces) {
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        original_use_vertices.add_new(use.id,
                                      index->edge(use.edge_id)->vertex_ids[use.reversed ? 1 : 0]);
      }
    }
  }
  Vector<FaceRepair> repairs;
  for (auto &face : builder.geometry->faces.items_mutable()) {
    bool changed = false;
    for (auto &loop : face.loops) {
      Map<int64_t, int64_t> seen;
      for (auto &use : loop.edge_uses) {
        if (rewired_edges.contains(use.edge_id)) {
          touched_faces.add(face.id);
        }
        if (const auto *replacement = edge_map.lookup_ptr(use.edge_id)) {
          if (replacement->second) {
            const auto &old_edge = *builder.edges.lookup(use.edge_id);
            auto *pcurve = builder.curves.lookup(use.pcurve_id);
            if (!full_linear(*pcurve, old_edge)) {
              return fail("NURBS Merge needs exact p-curve reversal for this shared edge");
            }
            pcurve = builder.copy_curve(pcurve->id);
            std::swap(pcurve->points[0], pcurve->points[1]);
            use.pcurve_id = pcurve->id;
            use.reversed ^= 1;
          }
          use.edge_id = replacement->first;
        }
        const auto &edge = *builder.edges.lookup(use.edge_id);
        changed |= constant_edges.contains(use.edge_id);
        if (!constant_edges.contains(use.edge_id)) {
          const int64_t start = edge.vertex_ids[use.reversed ? 1 : 0];
          const int64_t original = original_use_vertices.lookup(use.id);
          if (const int64_t *previous = seen.lookup_ptr(start)) {
            changed |= *previous != original;
          }
          else {
            seen.add_new(start, original);
          }
        }
      }
    }
    if (!changed) {
      continue;
    }
    if (!face.loops.is_single() || static_cast<NurbsTopologyLoop *>(face.loops.first)->is_inner) {
      return fail("NURBS Merge across collapsed trim holes needs an explicit trim repair");
    }
    FaceRepair repair{face.id, face.material_index, face.tolerance, std::nullopt, {}};
    const auto &surface = *builder.surfaces.lookup(face.surface_id);
    const double2 center(
        (surface.knots_u[surface.order_u - 1] + surface.knots_u[surface.points_u]) * 0.5,
        (surface.knots_v[surface.order_v - 1] + surface.knots_v[surface.points_v]) * 0.5);
    const auto differential = evaluate_surface_differential(surface, center);
    if (differential && differential->normal) {
      repair.normal = *differential->normal * face.orientation;
    }
    Vector<int64_t> path_vertices, path_edges;
    for (const auto &use : static_cast<NurbsTopologyLoop *>(face.loops.first)->edge_uses) {
      if (constant_edges.contains(use.edge_id)) {
        continue;
      }
      const auto &edge = *builder.edges.lookup(use.edge_id);
      if (!full_linear(*builder.curves.lookup(edge.curve_id), edge)) {
        return fail("NURBS Merge cannot silently rebuild a collapsed curved trim boundary");
      }
      const int64_t start = edge.vertex_ids[use.reversed ? 1 : 0];
      const int64_t end = edge.vertex_ids[use.reversed ? 0 : 1];
      if (path_vertices.is_empty()) {
        path_vertices.append(start);
      }
      if (path_vertices.last() != start) {
        return fail("NURBS Merge boundary repair has inconsistent incidence");
      }
      const int previous = path_vertices.first_index_of_try(end);
      if (previous >= 0) {
        Vector<int64_t> cycle;
        for (int i = previous; i < path_edges.size(); i++) {
          cycle.append(path_edges[i]);
        }
        cycle.append(edge.id);
        if (cycle.size() >= 3) {
          repair.cycles.append(std::move(cycle));
        }
        path_vertices.resize(previous + 1);
        path_edges.resize(previous);
      }
      else {
        path_edges.append(edge.id);
        path_vertices.append(end);
      }
    }
    repairs.append(std::move(repair));
    remove_face(*builder.geometry, face);
  }
  for (auto &edge : builder.geometry->edges.items_mutable()) {
    if (constant_edges.contains(edge.id) || edge_map.contains(edge.id)) {
      BLI_remlink(&builder.geometry->edges, &edge);
      MEM_delete(&edge);
    }
  }
  for (auto &vertex : builder.geometry->vertices.items_mutable()) {
    if (vertex_map.lookup_default(vertex.id, vertex.id) != vertex.id) {
      BLI_remlink(&builder.geometry->vertices, &vertex);
      MEM_delete(&vertex);
    }
  }
  if (!builder.valid()) {
    return fail("NURBS identity space is exhausted");
  }
  if (!geometry_validate_structure(*builder.geometry, r_error)) {
    return std::nullopt;
  }
  OwnedGeometry geometry = std::move(builder.geometry);
  for (const auto &repair : repairs) {
    result.source_to_result.add_overwrite(repair.id, 0);
    bool first = true;
    for (const auto &cycle : repair.cycles) {
      auto filled = fill_components(*geometry, {{}, cycle, {}}, r_error);
      if (!filled) {
        return std::nullopt;
      }
      if (filled->created_faces.is_empty()) {
        return fail(
            "NURBS Merge boundary coincides with another face requiring support reconciliation");
      }
      const int64_t created = filled->created_faces[0];
      for (auto &face : filled->geometry->faces) {
        if (face.id != created) {
          continue;
        }
        if (first) {
          face.id = repair.id;
          result.source_to_result.add_overwrite(repair.id, face.id);
          first = false;
        }
        face.material_index = repair.material;
        face.tolerance = std::max(face.tolerance, repair.tolerance);
        if (repair.normal) {
          for (const auto &surface : filled->geometry->surfaces) {
            if (surface.id == face.surface_id) {
              const double2 center(
                  (surface.knots_u[surface.order_u - 1] + surface.knots_u[surface.points_u]) * 0.5,
                  (surface.knots_v[surface.order_v - 1] + surface.knots_v[surface.points_v]) *
                      0.5);
              const auto differential = evaluate_surface_differential(surface, center);
              if (differential && differential->normal) {
                face.orientation = math::dot(*repair.normal, *differential->normal) < 0 ? -1 : 1;
              }
            }
          }
        }
      }
      geometry = std::move(filled->geometry);
    }
  }
  /* A duplicate face is removable only when its NURBS support and boundary really agree.
   * Distinct curved sheets sharing endpoints/boundaries remain distinct geometry. */
  Map<uint64_t, Vector<NurbsTopologyFace *>> kept_faces;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  for (const auto &surface : geometry->surfaces) {
    surfaces.add_new(surface.id, &surface);
  }
  for (auto &face : geometry->faces.items_mutable()) {
    Set<int64_t> boundary;
    for (const auto &loop : face.loops) {
      for (const auto &use : loop.edge_uses) {
        boundary.add(use.edge_id);
      }
    }
    Vector<int64_t> ordered(boundary.begin(), boundary.end());
    std::sort(ordered.begin(), ordered.end());
    uint64_t hash = 1469598103934665603ULL;
    for (const int64_t id : ordered) {
      hash = (hash ^ uint64_t(id)) * 1099511628211ULL;
    }
    auto &candidates = kept_faces.lookup_or_add_default(hash);
    NurbsTopologyFace *duplicate = nullptr;
    for (auto *other : candidates) {
      if (!touched_faces.contains(face.id) && !touched_faces.contains(other->id)) {
        continue;
      }
      if (!face.loops.is_single() || !other->loops.is_single() ||
          !same_surface(*surfaces.lookup(face.surface_id), *surfaces.lookup(other->surface_id)))
      {
        continue;
      }
      Set<int64_t> other_boundary;
      for (const auto &use : static_cast<NurbsTopologyLoop *>(other->loops.first)->edge_uses) {
        other_boundary.add(use.edge_id);
      }
      if (boundary == other_boundary) {
        const auto &loop = *static_cast<NurbsTopologyLoop *>(face.loops.first);
        const auto &other_loop = *static_cast<NurbsTopologyLoop *>(other->loops.first);
        bool equal = loop.edge_uses.count() == other_loop.edge_uses.count();
        for (const auto &use : loop.edge_uses) {
          bool match = false;
          for (const auto &other_use : other_loop.edge_uses) {
            if (use.edge_id == other_use.edge_id) {
              const NurbsCurveGeometry *a = nullptr, *b = nullptr;
              for (const auto &curve : geometry->curves) {
                if (curve.id == use.pcurve_id) {
                  a = &curve;
                }
                if (curve.id == other_use.pcurve_id) {
                  b = &curve;
                }
              }
              match |= a && b && same_curve(*a, *b, false);
            }
          }
          equal &= match;
        }
        if (equal) {
          duplicate = other;
          break;
        }
      }
    }
    if (duplicate) {
      result.source_to_result.add_overwrite(face.id, duplicate->id);
      remove_face(*geometry, face);
    }
    else {
      candidates.append(&face);
    }
  }
  if (!geometry_validate_structure(*geometry, r_error)) {
    return std::nullopt;
  }
  result.geometry = std::move(geometry);
  return result;
}

std::optional<TopologyEditResult> merge_selected_vertices(const NurbsGeometry &source,
                                                          const SelectionState &selection,
                                                          const VertexMergeMode mode,
                                                          const double3 &cursor_target,
                                                          const double max_position_error,
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
  if (!ELEM(mode,
            VertexMergeMode::Center,
            VertexMergeMode::Cursor,
            VertexMergeMode::First,
            VertexMergeMode::Last,
            VertexMergeMode::Collapse))
  {
    return fail("Unknown NURBS merge mode");
  }
  const auto normalized = selection_flush(source, selection, selection.mode, r_error);
  const auto index = TopologyIndex::build(source, r_error);
  if (!normalized || !index) {
    return std::nullopt;
  }
  if (normalized->vertices != selection.vertices || normalized->edges != selection.edges ||
      normalized->faces != selection.faces)
  {
    return fail("NURBS Merge requires normalized component selection");
  }
  if (selection.vertices.is_empty()) {
    return std::nullopt;
  }
  Vector<VertexMergeGroup> groups;
  Set<int64_t> assigned;
  for (const auto &vertex : source.vertices) {
    if (!selection.vertices.contains(vertex.id) || !assigned.add(vertex.id)) {
      continue;
    }
    VertexMergeGroup group;
    group.survivor = vertex.id;
    group.vertices.append(vertex.id);
    if (mode == VertexMergeMode::Collapse) {
      for (int i = 0; i < group.vertices.size(); i++) {
        for (const int64_t edge_id : index->edges_for_vertex(group.vertices[i])) {
          if (!selection.edges.contains(edge_id)) {
            continue;
          }
          for (const int64_t endpoint : index->edge(edge_id)->vertex_ids) {
            if (assigned.add(endpoint)) {
              group.vertices.append(endpoint);
            }
          }
        }
      }
    }
    else {
      for (const auto &other : source.vertices) {
        if (selection.vertices.contains(other.id) && assigned.add(other.id)) {
          group.vertices.append(other.id);
        }
      }
    }
    if (mode == VertexMergeMode::Cursor) {
      group.target = cursor_target;
    }
    else if (ELEM(mode, VertexMergeMode::First, VertexMergeMode::Last)) {
      if (selection.history.is_empty()) {
        return fail("At First/Last needs selected vertex history");
      }
      const ElementRef target = mode == VertexMergeMode::First ? selection.history.first() :
                                                                 selection.history.last();
      if (target.domain != ElementDomain::Vertex || !selection.vertices.contains(target.id)) {
        return fail("At First/Last needs a selected vertex at that end of selection history");
      }
      group.survivor = target.id;
      group.target = double3(index->vertex(target.id)->position);
    }
    else {
      // Incremental mean avoids overflowing a sum when the requested center is representable.
      group.target = double3(0);
      int count = 0;
      for (const int64_t id : group.vertices) {
        const double3 point(index->vertex(id)->position);
        count++;
        const double fraction = 1.0 / count;
        group.target = group.target * (1.0 - fraction) + point * fraction;
      }
    }
    groups.append(std::move(group));
  }
  return merge_vertex_groups(source, groups, max_position_error, r_error);
}

std::optional<TopologyEditResult> merge_by_distance(const NurbsGeometry &source,
                                                    const SelectionState &selection,
                                                    const double distance,
                                                    const bool use_centroid,
                                                    const bool use_unselected,
                                                    const double max_position_error,
                                                    std::string *r_error)
{
  if (r_error) {
    r_error->clear();
  }
  if (!std::isfinite(distance) || distance < 0 || !std::isfinite(max_position_error) ||
      max_position_error < 0 || !detail::supported_arithmetic())
  {
    if (r_error) {
      *r_error =
          "NURBS distance merge needs a finite non-negative distance/error budget and supported "
          "arithmetic";
    }
    return std::nullopt;
  }
  const auto selected = selection_flush(source, selection, selection.mode, r_error);
  if (!selected) {
    return std::nullopt;
  }
  Vector<const NurbsTopologyVertex *> vertices;
  for (const auto &vertex : source.vertices) {
    if (use_unselected || selected->vertices.contains(vertex.id)) {
      vertices.append(&vertex);
    }
  }
  if (selected->vertices.is_empty()) {
    return std::nullopt;
  }
  KDTree<double3> *tree = kdtree_new<double3>(vertices.size());
  BLI_SCOPED_DEFER([&]() { kdtree_free<double3>(tree); });
  Array<int> duplicates(vertices.size(), -1);
  bool has_anchors = false;
  Map<int64_t, const NurbsTopologyVertex *> vertex_lookup;
  for (const int i : vertices.index_range()) {
    vertex_lookup.add_new(vertices[i]->id, vertices[i]);
    kdtree_insert<double3>(tree, i, double3(vertices[i]->position));
    if (use_unselected && !selected->vertices.contains(vertices[i]->id)) {
      duplicates[i] = i;
      has_anchors = true;
    }
  }
  kdtree_balance<double3>(tree);
  /* Same clustering and stable target choice as bmesh_find_doubles_by_distance_impl,
   * retaining source doubles instead of truncating geometry to Mesh coordinates. */
  auto choose_target = [&](const int *cluster, const int count) {
    if (count == 2) {
      return cluster[0] < cluster[1] ? 0 : 1;
    }
    double3 center(0);
    for (int i = 0; i < count; i++) {
      center += double3(vertices[cluster[i]]->position) / double(count);
    }
    int best = count - 1;
    double best_distance = math::distance_squared(center,
                                                  double3(vertices[cluster[best]]->position));
    for (int i = 0; i < count - 1; i++) {
      const double candidate = math::distance_squared(center,
                                                      double3(vertices[cluster[i]]->position));
      if (candidate < best_distance || (candidate == best_distance && cluster[i] < cluster[best]))
      {
        best = i;
        best_distance = candidate;
      }
    }
    return best;
  };
  kdtree_calc_duplicates_cb<double3>(
      tree, distance, duplicates.data(), has_anchors, choose_target);
  Map<int, Vector<int64_t>> clusters;
  for (const int i : vertices.index_range()) {
    const int target = duplicates[i] == -1 ? i : duplicates[i];
    clusters.lookup_or_add_default(target).append(vertices[i]->id);
  }
  Vector<VertexMergeGroup> groups;
  for (const int i : vertices.index_range()) {
    auto *members = clusters.lookup_ptr(i);
    if (!members || (members->size() == 1 && !selected->vertices.contains(vertices[i]->id))) {
      continue;
    }
    VertexMergeGroup group;
    group.vertices = *members;
    group.survivor = vertices[i]->id;
    group.target = double3(vertices[i]->position);
    if (use_centroid && members->size() > 1) {
      group.target = double3(0);
      for (const int64_t id : *members) {
        group.target += double3(vertex_lookup.lookup(id)->position) / double(members->size());
      }
    }
    groups.append(std::move(group));
  }
  if (std::none_of(groups.begin(), groups.end(), [](const auto &group) {
        return group.vertices.size() > 1;
      }))
  {
    TopologyEditResult result;
    result.geometry.reset(geometry_copy(&source));
    selection_storage_free(result.geometry->stored_selection);
    result.geometry->stored_selection = nullptr;
    result.vertices = Vector<int64_t>(selected->vertices.begin(), selected->vertices.end());
    result.edges = Vector<int64_t>(selected->edges.begin(), selected->edges.end());
    result.faces = Vector<int64_t>(selected->faces.begin(), selected->faces.end());
    return result;
  }
  auto result = merge_vertex_groups(source, groups, max_position_error, r_error);
  if (!result) {
    return std::nullopt;
  }
  Map<int64_t, int64_t> remap;
  for (const auto &vertex : source.vertices) {
    remap.add_new(vertex.id, result->source_to_result.lookup_default(vertex.id, vertex.id));
  }
  for (const auto &edge : source.edges) {
    remap.add_new(edge.id, result->source_to_result.lookup_default(edge.id, edge.id));
  }
  for (const auto &face : source.faces) {
    remap.add_new(face.id, result->source_to_result.lookup_default(face.id, face.id));
  }
  const auto restored = selection_remap(
      source, *result->geometry, *selected, remap, selection.mode, r_error);
  if (!restored) {
    return std::nullopt;
  }
  result->vertices = Vector<int64_t>(restored->vertices.begin(), restored->vertices.end());
  result->edges = Vector<int64_t>(restored->edges.begin(), restored->edges.end());
  result->faces = Vector<int64_t>(restored->faces.begin(), restored->faces.end());
  return result;
}

}  // namespace blender::bke::nurbs
