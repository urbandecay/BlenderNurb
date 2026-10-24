/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <limits>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_curve.hh"
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_legacy_edit.hh"
#include "BKE_nurbs_selection.hh"

namespace blender::bke::nurbs {

LegacyTopologyEdit::LegacyTopologyEdit(LegacyTopologyEdit &&other) noexcept
    : added(std::exchange(other.added, {nullptr, nullptr})),
      removed_faces(std::move(other.removed_faces)),
      removed_wires(std::move(other.removed_wires))
{
}

LegacyTopologyEdit::~LegacyTopologyEdit()
{
  BKE_nurbList_free(&added);
}

static bool is_tagged(const Nurb &nurb)
{
  return nurb.topology_face_id != 0 || nurb.topology_edge_ids[0] != 0;
}

static bool has_selected_points(const Nurb &nurb)
{
  if (nurb.hide) {
    return false;
  }
  if (nurb.bezt != nullptr) {
    for (int i = 0; i < nurb.pntsu; i++) {
      const BezTriple &point = nurb.bezt[i];
      if (!point.hide && ((point.f1 | point.f2 | point.f3) & SELECT)) {
        return true;
      }
    }
    return false;
  }
  if (nurb.bp == nullptr) {
    return false;
  }
  for (int i = 0; i < nurb.pntsu * nurb.pntsv; i++) {
    if (!nurb.bp[i].hide && (nurb.bp[i].f1 & SELECT)) {
      return true;
    }
  }
  return false;
}

bool legacy_topology_selection_check(const ListBaseT<Nurb> &source)
{
  return std::any_of(source.begin(), source.end(), [](const Nurb &nurb) {
    return is_tagged(nurb) && has_selected_points(nurb);
  });
}

static void select_all_points(Nurb &nurb, const bool selected)
{
  for (int i = 0; i < nurb.pntsu * nurb.pntsv; i++) {
    SET_FLAG_FROM_TEST(nurb.bp[i].f1, selected, SELECT);
  }
}

struct LegacyEdgeSource {
  const Nurb *nurb;
  int start, end;
  const float *knots;
};

/** Temporary source correspondence; never becomes an editable geometry authority. */
struct LegacyTopologySource {
  OwnedGeometry geometry;
  SelectionState selection;
  Map<int64_t, const Nurb *> faces;
  Map<int64_t, LegacyEdgeSource> edges;
  Map<int64_t, const BPoint *> vertices;
  Map<int64_t, const Nurb *> vertex_splines;

  static std::optional<LegacyTopologySource> create(const ListBaseT<Nurb> &source,
                                                    std::string *r_error)
  {
    if (r_error != nullptr) {
      r_error->clear();
    }
    auto fail = [&](const char *message) -> std::optional<LegacyTopologySource> {
      if (r_error != nullptr) {
        *r_error = message;
      }
      return std::nullopt;
    };
    LegacyTopologyEdit tagged;
    for (const Nurb &nurb : source) {
      if (is_tagged(nurb)) {
        BLI_addtail(&tagged.added, BKE_nurb_duplicate(&nurb));
      }
      else if (has_selected_points(nurb)) {
        return fail("Select either tagged NURBS topology or legacy geometry for this operation");
      }
    }
    LegacyTopologySource result;
    result.geometry.reset(geometry_from_tagged_legacy(tagged.added, nullptr, r_error));
    if (!result.geometry) {
      return std::nullopt;
    }
    /* Import supports a wider knot domain than this compatibility constructor. End interpolation
     * is necessary to create walls directly from endpoint coefficients without fitting. */
    for (const Nurb &nurb : tagged.added) {
      if (nurb.knotsu[0] != nurb.knotsu[1] || nurb.knotsu[2] != nurb.knotsu[3] ||
          (nurb.pntsv == 2 &&
           (nurb.knotsv[0] != nurb.knotsv[1] || nurb.knotsv[2] != nurb.knotsv[3])))
      {
        return fail("NURBS topology editing currently requires clamped endpoint knots");
      }
    }
    Set<int64_t> visible_faces, visible_edges;
    Vector<int64_t> hits;
    constexpr int corners[4] = {0, 1, 3, 2};
    constexpr int starts[4] = {0, 1, 2, 0};
    constexpr int ends[4] = {1, 3, 3, 2};
    for (const Nurb &nurb : source) {
      if (!is_tagged(nurb)) {
        continue;
      }
      bool visible_face = !nurb.hide;
      for (int corner = 0; corner < (nurb.pntsv == 1 ? 2 : 4); corner++) {
        const BPoint &point = nurb.bp[corners[corner]];
        const int64_t id = nurb.topology_vertex_ids[corner];
        result.vertices.add(id, &point);
        result.vertex_splines.add(id, &nurb);
        visible_face &= !point.hide;
        if (!nurb.hide && !point.hide && (point.f1 & SELECT)) {
          hits.append(id);
        }
      }
      if (nurb.pntsv == 2) {
        result.faces.add_new(nurb.topology_face_id, &nurb);
        if (visible_face) {
          visible_faces.add(nurb.topology_face_id);
        }
      }
      for (int edge = 0; edge < (nurb.pntsv == 1 ? 1 : 4); edge++) {
        const int64_t id = nurb.topology_edge_ids[edge];
        result.edges.add(
            id, {&nurb, starts[edge], ends[edge], edge % 2 == 0 ? nurb.knotsu : nurb.knotsv});
        if (!nurb.hide && !nurb.bp[starts[edge]].hide && !nurb.bp[ends[edge]].hide) {
          visible_edges.add(id);
        }
      }
    }
    auto selection = selection_apply_vertices(
        *result.geometry, {}, hits, SelectionAction::Set, std::nullopt, r_error);
    if (!selection) {
      return std::nullopt;
    }
    selection->faces.remove_if([&](const int64_t id) { return !visible_faces.contains(id); });
    selection->edges.remove_if([&](const int64_t id) { return !visible_edges.contains(id); });
    if (selection->vertices.is_empty()) {
      return fail("No visible NURBS topology vertices are selected");
    }
    result.selection = std::move(*selection);
    return result;
  }
};

static Nurb *copy_face(const Nurb &source, const Map<int64_t, int64_t> &ids)
{
  Nurb *copy = BKE_nurb_duplicate(&source);
  copy->topology_face_id = int(ids.lookup(source.topology_face_id));
  for (int i = 0; i < 4; i++) {
    copy->topology_vertex_ids[i] = int(ids.lookup(source.topology_vertex_ids[i]));
    copy->topology_edge_ids[i] = int(ids.lookup(source.topology_edge_ids[i]));
  }
  select_all_points(*copy, true);
  return copy;
}

static Nurb *make_line(const LegacyEdgeSource &source)
{
  Nurb *line = BKE_nurb_duplicate(source.nurb);
  MEM_delete(line->bp);
  MEM_SAFE_DELETE(line->knotsu);
  MEM_SAFE_DELETE(line->knotsv);
  line->pntsu = 2;
  line->pntsv = 1;
  line->hide = 0;
  line->bp = MEM_new_array<BPoint>(2, __func__);
  line->orderu = 2;
  line->orderv = 2;
  line->flagu = line->flagv = CU_NURB_ENDPOINT;
  line->knotsu = MEM_new_array_uninitialized<float>(4, __func__);
  std::copy_n(source.knots, 4, line->knotsu);
  line->bp[0] = source.nurb->bp[source.start];
  line->bp[1] = source.nurb->bp[source.end];
  line->bp[0].hide = line->bp[1].hide = 0;
  line->topology_face_id = 0;
  std::fill_n(line->topology_vertex_ids, 4, 0);
  std::fill_n(line->topology_edge_ids, 4, 0);
  select_all_points(*line, true);
  return line;
}

static ComponentSelection component_selection(const SelectionState &source,
                                              Vector<int64_t> &vertices,
                                              Vector<int64_t> &edges,
                                              Vector<int64_t> &faces)
{
  vertices.extend(source.vertices.begin(), source.vertices.end());
  edges.extend(source.edges.begin(), source.edges.end());
  faces.extend(source.faces.begin(), source.faces.end());
  return {vertices, edges, faces};
}

std::optional<LegacyTopologyEdit> legacy_topology_duplicate(const ListBaseT<Nurb> &source,
                                                            std::string *r_error)
{
  auto input = LegacyTopologySource::create(source, r_error);
  if (!input) {
    return std::nullopt;
  }
  Vector<int64_t> vertices, edges, faces;
  auto copy = copy_component_closure(
      *input->geometry, component_selection(input->selection, vertices, edges, faces), r_error);
  if (!copy) {
    return std::nullopt;
  }
  if (copy->geometry->next_id > std::numeric_limits<int>::max()) {
    if (r_error != nullptr) {
      *r_error = "Legacy NURBS topology identity space is exhausted";
    }
    return std::nullopt;
  }
  Set<int64_t> face_edges, edge_vertices;
  LegacyTopologyEdit result;
  for (const NurbsTopologyFace &face : input->geometry->faces) {
    if (!input->selection.faces.contains(face.id)) {
      continue;
    }
    BLI_addtail(&result.added, copy_face(*input->faces.lookup(face.id), copy->source_to_copy));
    for (const NurbsTopologyLoop &loop : face.loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        face_edges.add(use.edge_id);
      }
    }
  }
  for (const NurbsTopologyEdge &edge : input->geometry->edges) {
    if (!input->selection.edges.contains(edge.id)) {
      continue;
    }
    edge_vertices.add_multiple(Span(edge.vertex_ids, 2));
    if (face_edges.contains(edge.id)) {
      continue;
    }
    Nurb *line = make_line(input->edges.lookup(edge.id));
    line->topology_edge_ids[0] = int(copy->source_to_copy.lookup(edge.id));
    for (int i = 0; i < 2; i++) {
      line->topology_vertex_ids[i] = int(copy->source_to_copy.lookup(edge.vertex_ids[i]));
    }
    BLI_addtail(&result.added, line);
  }
  for (const int64_t id : vertices) {
    if (!edge_vertices.contains(id)) {
      if (r_error != nullptr) {
        *r_error = "Duplicating isolated NURBS topology vertices is not supported yet";
      }
      return std::nullopt;
    }
  }
  return result;
}

std::optional<LegacyTopologyEdit> legacy_topology_extrude(const ListBaseT<Nurb> &source,
                                                          std::string *r_error)
{
  auto input = LegacyTopologySource::create(source, r_error);
  if (!input) {
    return std::nullopt;
  }
  auto fail = [&](const char *message) -> std::optional<LegacyTopologyEdit> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  Vector<int64_t> vertices, edges, faces;
  auto cap = copy_component_closure(
      *input->geometry, component_selection(input->selection, vertices, edges, faces), r_error);
  if (!cap) {
    return std::nullopt;
  }
  const uint64_t extra = uint64_t(vertices.size()) * 2 + edges.size();
  if (extra > uint64_t(std::numeric_limits<int>::max()) ||
      cap->geometry->next_id > std::numeric_limits<int>::max() - int64_t(extra))
  {
    return fail("Legacy NURBS topology identity space is exhausted");
  }
  int next_id = int(cap->geometry->next_id);
  LegacyTopologyEdit result;
  Set<int64_t> boundary_edges, covered_vertices;
  if (!faces.is_empty()) {
    const auto plan = plan_face_region_extrusion(*input->geometry, faces, r_error);
    if (!plan) {
      return std::nullopt;
    }
    boundary_edges.add_multiple(plan->boundary_edges);
    Set<int64_t> region_edges;
    for (const NurbsTopologyFace &face : input->geometry->faces) {
      if (!input->selection.faces.contains(face.id)) {
        continue;
      }
      BLI_addtail(&result.added, copy_face(*input->faces.lookup(face.id), cap->source_to_copy));
      if (plan->remove_original_faces) {
        result.removed_faces.append(int(face.id));
      }
      for (const NurbsTopologyLoop &loop : face.loops) {
        for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
          region_edges.add(use.edge_id);
        }
      }
    }
    for (const int64_t id : edges) {
      if (!region_edges.contains(id)) {
        return fail("Extrude the NURBS face region and disconnected edges separately");
      }
    }
  }
  else {
    boundary_edges.add_multiple(edges);
  }
  const auto index = TopologyIndex::build(*input->geometry);
  Map<int64_t, int> rails;
  auto rail = [&](const int64_t id) {
    return rails.lookup_or_add_cb(id, [&]() { return next_id++; });
  };
  for (const NurbsTopologyEdge &edge : input->geometry->edges) {
    if (!input->selection.edges.contains(edge.id)) {
      continue;
    }
    covered_vertices.add_multiple(Span(edge.vertex_ids, 2));
    if (!boundary_edges.contains(edge.id)) {
      continue;
    }
    const LegacyEdgeSource &edge_source = input->edges.lookup(edge.id);
    Nurb *wall = make_line(edge_source);
    MEM_delete(wall->bp);
    wall->pntsv = 2;
    wall->bp = MEM_new_array<BPoint>(4, __func__);
    wall->orderu = wall->orderv = 2;
    wall->flagu = wall->flagv = CU_NURB_ENDPOINT;
    BKE_nurb_knot_calc_v(wall);
    wall->bp[0] = wall->bp[2] = edge_source.nurb->bp[edge_source.start];
    wall->bp[1] = wall->bp[3] = edge_source.nurb->bp[edge_source.end];
    for (int i = 0; i < 4; i++) {
      wall->bp[i].hide = 0;
    }
    select_all_points(*wall, false);
    if (faces.is_empty()) {
      wall->bp[2].f1 |= SELECT;
      wall->bp[3].f1 |= SELECT;
    }
    wall->topology_face_id = next_id++;
    const int a = int(edge.vertex_ids[0]), b = int(edge.vertex_ids[1]);
    const int cap_a = int(cap->source_to_copy.lookup(a));
    const int cap_b = int(cap->source_to_copy.lookup(b));
    const int vertex_ids[4] = {a, b, cap_b, cap_a};
    const int edge_ids[4] = {
        int(edge.id), rail(b), int(cap->source_to_copy.lookup(edge.id)), rail(a)};
    std::copy_n(vertex_ids, 4, wall->topology_vertex_ids);
    std::copy_n(edge_ids, 4, wall->topology_edge_ids);
    std::optional<int> orientation;
    bool conflicting = false;
    for (const EdgeUseReference &reference : index->uses_for_edge(edge.id)) {
      if (!faces.is_empty() && !input->selection.faces.contains(reference.face_id)) {
        continue;
      }
      int sign = index->face(reference.face_id)->orientation * (reference.use->reversed ? -1 : 1);
      /* Region walls oppose their cap's use; edge-only walls oppose the existing source use.
       * The top and bottom of a ruled support have opposite loop traversal. */
      if (faces.is_empty()) {
        sign = -sign;
      }
      conflicting |= orientation && *orientation != sign;
      orientation = sign;
    }
    if (orientation && !conflicting && *orientation < 0) {
      /* Reverse V exactly, keeping the U basis, rational weights and shared boundary IDs. */
      std::swap(wall->bp[0], wall->bp[2]);
      std::swap(wall->bp[1], wall->bp[3]);
      std::swap(wall->topology_vertex_ids[0], wall->topology_vertex_ids[3]);
      std::swap(wall->topology_vertex_ids[1], wall->topology_vertex_ids[2]);
      std::swap(wall->topology_edge_ids[0], wall->topology_edge_ids[2]);
    }
    BLI_addtail(&result.added, wall);
    if (edge_source.nurb->pntsv == 1) {
      result.removed_wires.append(int(edge.id));
    }
  }
  for (const int64_t id : vertices) {
    if (covered_vertices.contains(id)) {
      continue;
    }
    if (!faces.is_empty()) {
      return fail("Extrude the NURBS face region and isolated vertices separately");
    }
    /* A selected endpoint extends to a line, with one new source vertex and edge identity. */
    const BPoint &point = *input->vertices.lookup(id);
    const Nurb &parent = *input->vertex_splines.lookup(id);
    const int point_index = int(&point - parent.bp);
    const LegacyEdgeSource source_point = {&parent, point_index, point_index, parent.knotsu};
    Nurb *line = make_line(source_point);
    line->bp[0] = line->bp[1] = point;
    line->bp[0].hide = line->bp[1].hide = 0;
    line->bp[0].f1 &= ~SELECT;
    line->bp[1].f1 |= SELECT;
    line->topology_vertex_ids[0] = int(id);
    line->topology_vertex_ids[1] = int(cap->source_to_copy.lookup(id));
    line->topology_edge_ids[0] = next_id++;
    BLI_addtail(&result.added, line);
  }
  /* A wire becomes the source boundary of a wall; don't retain a second stored edge owner. */
  return result;
}

}  // namespace blender::bke::nurbs
