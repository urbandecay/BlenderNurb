/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"

#include "BKE_curve.hh"
#include "BKE_nurbs_construct.hh"
#include "BKE_nurbs_legacy_edit.hh"
#include "BKE_nurbs_selection.hh"
#include "BKE_nurbs_topology.hh"

namespace blender::bke::nurbs {

LegacyTopologyEdit::LegacyTopologyEdit(LegacyTopologyEdit &&other) noexcept
    : added(std::exchange(other.added, {nullptr, nullptr})),
      removed_faces(std::move(other.removed_faces)),
      removed_wires(std::move(other.removed_wires)),
      removed_points(std::move(other.removed_points)),
      recovered_patch_remnants(std::exchange(other.recovered_patch_remnants, 0))
{
}

LegacyTopologyEdit::~LegacyTopologyEdit()
{
  BKE_nurbList_free(&added);
}

static bool is_tagged(const Nurb &nurb)
{
  return nurb.topology_face_id != 0 || nurb.topology_edge_ids[0] != 0 ||
         nurb.topology_vertex_ids[0] != 0;
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

/**
 * Older surface deletion collapsed a patch to two points without changing its patch IDs. Only
 * discard such an invalid record when ONE of its declared edge IDs is already owned by an intact
 * face and its exact line geometry/attributes match that edge. Coordinates validate an existing
 * identity; they never create a connection, choose an undeclared edge or weld separate sources.
 */
static Map<const Nurb *, std::array<int, 2>> recover_redundant_patch_aliases(
    const ListBaseT<Nurb> &source)
{
  Map<const Nurb *, std::array<int, 2>> result;
  constexpr int starts[4] = {0, 1, 2, 0};
  constexpr int ends[4] = {1, 3, 3, 2};
  constexpr int vertex_starts[4] = {0, 1, 3, 0};
  constexpr int vertex_ends[4] = {1, 2, 2, 3};
  auto same_point = [](const BPoint &a, const BPoint &b) {
    return std::equal(a.vec, a.vec + 4, b.vec) && a.radius == b.radius && a.tilt == b.tilt &&
           a.weight == b.weight && a.hide == b.hide && (a.f1 & ~SELECT) == (b.f1 & ~SELECT);
  };
  for (const Nurb &remnant : source) {
    if (remnant.type != CU_NURBS || remnant.bp == nullptr || remnant.pntsu != 2 ||
        remnant.pntsv != 1 || remnant.orderu != 2 || remnant.knotsu == nullptr ||
        (remnant.flagu & CU_NURB_CYCLIC) || remnant.topology_face_id <= 0 ||
        std::any_of(remnant.topology_vertex_ids,
                    remnant.topology_vertex_ids + 4,
                    [](const int id) { return id <= 0; }) ||
        std::any_of(remnant.topology_edge_ids,
                    remnant.topology_edge_ids + 4,
                    [](const int id) { return id <= 0; }) ||
        Set<int>(Span(remnant.topology_vertex_ids, 4)).size() != 4 ||
        Set<int>(Span(remnant.topology_edge_ids, 4)).size() != 4)
    {
      continue;
    }
    int surviving_slot = -1;
    bool ambiguous = false, geometry_verified = false;
    for (const Nurb &face : source) {
      if (face.type != CU_NURBS || face.bp == nullptr || face.pntsu != 2 || face.pntsv != 2 ||
          face.orderu != 2 || face.orderv != 2 || face.knotsu == nullptr ||
          face.knotsv == nullptr || face.topology_face_id <= 0)
      {
        continue;
      }
      if (face.topology_face_id == remnant.topology_face_id) {
        ambiguous = true;
      }
      for (int edge = 0; edge < 4; edge++) {
        const int id = face.topology_edge_ids[edge];
        const int *declared = std::find(
            remnant.topology_edge_ids, remnant.topology_edge_ids + 4, id);
        if (id <= 0 || declared == remnant.topology_edge_ids + 4) {
          continue;
        }
        const int slot = int(declared - remnant.topology_edge_ids);
        const int expected_a = remnant.topology_vertex_ids[vertex_starts[slot]];
        const int expected_b = remnant.topology_vertex_ids[vertex_ends[slot]];
        const int a = face.topology_vertex_ids[vertex_starts[edge]];
        const int b = face.topology_vertex_ids[vertex_ends[edge]];
        const bool forward = expected_a == a && expected_b == b;
        if ((!forward && !(expected_a == b && expected_b == a)) ||
            (surviving_slot != -1 && surviving_slot != slot))
        {
          ambiguous = true;
        }
        surviving_slot = slot;
        /* IDs choose the surviving edge, its old row/column and endpoint order. Exact geometry
         * only validates that choice; it cannot select between multiple surviving declarations. */
        if (face.hide != remnant.hide || face.mat_nr != remnant.mat_nr ||
            face.flag != remnant.flag)
        {
          continue;
        }
        const float *knots = edge % 2 == 0 ? face.knotsu : face.knotsv;
        if (std::equal(knots, knots + 4, remnant.knotsu) &&
            same_point(remnant.bp[0], face.bp[forward ? starts[edge] : ends[edge]]) &&
            same_point(remnant.bp[1], face.bp[forward ? ends[edge] : starts[edge]]))
        {
          geometry_verified = true;
        }
      }
    }
    if (!ambiguous && geometry_verified && surviving_slot != -1) {
      result.add_new(&remnant,
                     {remnant.topology_vertex_ids[vertex_starts[surviving_slot]],
                      remnant.topology_vertex_ids[vertex_ends[surviving_slot]]});
    }
  }
  return result;
}

/** Temporary source correspondence; never becomes an editable geometry authority. */
struct LegacyTopologySource {
  OwnedGeometry geometry;
  SelectionState selection;
  Map<int64_t, const Nurb *> faces;
  Map<int64_t, LegacyEdgeSource> edges;
  Map<int64_t, const BPoint *> vertices;
  Map<int64_t, const Nurb *> vertex_splines;
  Vector<int> recovered_faces;

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
    const auto recovered = recover_redundant_patch_aliases(source);
    for (const Nurb &nurb : source) {
      if (recovered.contains(&nurb)) {
        continue;
      }
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
    for (const Nurb &nurb : source) {
      int64_t highest = nurb.topology_face_id;
      for (int i = 0; i < 4; i++) {
        highest = std::max(highest, int64_t(nurb.topology_vertex_ids[i]));
        highest = std::max(highest, int64_t(nurb.topology_edge_ids[i]));
      }
      result.geometry->next_id = std::max(result.geometry->next_id, highest + 1);
      if (recovered.contains(&nurb)) {
        result.recovered_faces.append(nurb.topology_face_id);
      }
    }
    /* Import supports a wider knot domain than this compatibility constructor. End interpolation
     * is necessary to create walls directly from endpoint coefficients without fitting. */
    for (const Nurb &nurb : tagged.added) {
      if (nurb.pntsu == 1 && nurb.pntsv == 1) {
        continue;
      }
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
      if (const std::array<int, 2> *vertices = recovered.lookup_ptr(&nurb)) {
        for (int i = 0; i < 2; i++) {
          if (!nurb.hide && !nurb.bp[i].hide && (nurb.bp[i].f1 & SELECT)) {
            hits.append((*vertices)[i]);
          }
        }
        continue;
      }
      if (!is_tagged(nurb)) {
        continue;
      }
      bool visible_face = !nurb.hide;
      for (int corner = 0; corner < (nurb.pntsv == 1 ? nurb.pntsu : 4); corner++) {
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
      for (int edge = 0; edge < (nurb.pntsv == 1 ? nurb.pntsu - 1 : 4); edge++) {
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
  if (source.knots != nullptr) {
    std::copy_n(source.knots, 4, line->knotsu);
  }
  else {
    line->knotsu[0] = line->knotsu[1] = 0;
    line->knotsu[2] = line->knotsu[3] = 1;
  }
  line->bp[0] = source.nurb->bp[source.start];
  line->bp[1] = source.nurb->bp[source.end];
  line->bp[0].hide = line->bp[1].hide = 0;
  line->topology_face_id = 0;
  std::fill_n(line->topology_vertex_ids, 4, 0);
  std::fill_n(line->topology_edge_ids, 4, 0);
  select_all_points(*line, true);
  return line;
}

static Nurb *make_point(const Nurb &parent, const BPoint &point, const int id)
{
  Nurb *result = BKE_nurb_duplicate(&parent);
  MEM_delete(result->bp);
  MEM_SAFE_DELETE(result->knotsu);
  MEM_SAFE_DELETE(result->knotsv);
  result->bp = MEM_new<BPoint>(__func__, point);
  result->pntsu = result->pntsv = 1;
  result->orderu = result->orderv = 1;
  result->hide = result->bp[0].hide = 0;
  result->topology_face_id = 0;
  std::fill_n(result->topology_edge_ids, 4, 0);
  std::fill_n(result->topology_vertex_ids, 4, 0);
  result->topology_vertex_ids[0] = id;
  return result;
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
  result.removed_faces = input->recovered_faces;
  result.recovered_patch_remnants = input->recovered_faces.size();
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
      Nurb *point = make_point(*input->vertex_splines.lookup(id),
                               *input->vertices.lookup(id),
                               int(copy->source_to_copy.lookup(id)));
      point->bp[0].f1 |= SELECT;
      BLI_addtail(&result.added, point);
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
  result.removed_faces = input->recovered_faces;
  result.recovered_patch_remnants = input->recovered_faces.size();
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
    if (point.vec[3] == 0) {
      return fail("Extending a loose NURBS point requires a nonzero curve weight");
    }
    const Nurb &parent = *input->vertex_splines.lookup(id);
    const int point_index = int(&point - parent.bp);
    const LegacyEdgeSource source_point = {
        &parent, point_index, point_index, parent.pntsu == 1 ? nullptr : parent.knotsu};
    Nurb *line = make_line(source_point);
    line->bp[0] = line->bp[1] = point;
    line->bp[0].hide = line->bp[1].hide = 0;
    line->bp[0].f1 &= ~SELECT;
    line->bp[1].f1 |= SELECT;
    line->topology_vertex_ids[0] = int(id);
    line->topology_vertex_ids[1] = int(cap->source_to_copy.lookup(id));
    line->topology_edge_ids[0] = next_id++;
    BLI_addtail(&result.added, line);
    if (parent.pntsu == 1 && parent.pntsv == 1) {
      result.removed_points.append(int(id));
    }
  }
  /* A wire becomes the source boundary of a wall; don't retain a second stored edge owner. */
  return result;
}

std::optional<LegacyTopologyEdit> legacy_topology_fill(const ListBaseT<Nurb> &source,
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
  auto filled = fill_components(
      *input->geometry, component_selection(input->selection, vertices, edges, faces), r_error);
  if (!filled) {
    return std::nullopt;
  }
  LegacyTopologyEdit result;
  if (filled->created_edges.is_empty() && filled->created_faces.is_empty()) {
    return result;
  }
  if (filled->geometry->next_id > std::numeric_limits<int>::max()) {
    return fail("Legacy NURBS topology identity space is exhausted");
  }
  result.removed_faces = input->recovered_faces;
  result.recovered_patch_remnants = input->recovered_faces.size();
  for (const int64_t id : filled->removed.faces) {
    result.removed_faces.append(int(id));
  }
  const auto index = TopologyIndex::build(*filled->geometry, r_error);
  if (!index) {
    return std::nullopt;
  }
  Set<int64_t> absorbed_vertices;
  if (filled->created_faces.is_empty()) {
    for (const int64_t id : filled->created_edges) {
      const auto &edge = *index->edge(id);
      const auto &parent = *input->vertex_splines.lookup(edge.vertex_ids[0]);
      const auto &a = *input->vertices.lookup(edge.vertex_ids[0]);
      const auto &b = *input->vertices.lookup(edge.vertex_ids[1]);
      Nurb *line = make_line({&parent, int(&a - parent.bp), int(&a - parent.bp), nullptr});
      line->bp[0] = a;
      line->bp[1] = b;
      line->bp[0].vec[3] = line->bp[1].vec[3] = 1;
      line->topology_edge_ids[0] = int(id);
      for (int i = 0; i < 2; i++) {
        line->topology_vertex_ids[i] = int(edge.vertex_ids[i]);
        absorbed_vertices.add(edge.vertex_ids[i]);
      }
      select_all_points(*line, true);
      BLI_addtail(&result.added, line);
    }
  }
  for (const int64_t id : filled->created_faces) {
    const auto &face = *index->face(id);
    const auto *loop = static_cast<const NurbsTopologyLoop *>(face.loops.first);
    const NurbsSurfaceGeometry *surface = nullptr;
    for (const auto &item : filled->geometry->surfaces) {
      if (item.id == face.surface_id) {
        surface = &item;
        break;
      }
    }
    if (!face.loops.is_single() || loop->is_inner || loop->edge_uses.count() != 4 ||
        surface == nullptr || surface->order_u != 2 || surface->order_v != 2 ||
        surface->points_num != 4 || surface->knots_u[1] != 0 || surface->knots_u[2] != 1 ||
        surface->knots_v[1] != 0 || surface->knots_v[2] != 1)
    {
      return fail("This fill requires native trimmed-face storage; legacy topology is unchanged");
    }
    int64_t corner_ids[4], edge_ids[4];
    float domains[2][4] = {{0, 0, 1, 1}, {0, 0, 1, 1}};
    bool has_domain[2] = {false, false};
    int slot = 0;
    for (const auto &use : loop->edge_uses) {
      const auto &edge = *index->edge(use.edge_id);
      corner_ids[slot] = edge.vertex_ids[use.reversed ? 1 : 0];
      edge_ids[slot] = edge.id;
      absorbed_vertices.add(corner_ids[slot]);
      if (const LegacyEdgeSource *original = input->edges.lookup_ptr(edge.id)) {
        if (original->nurb->bp[original->start].vec[3] != 1 ||
            original->nurb->bp[original->end].vec[3] != 1)
        {
          return fail("Legacy quad fill cannot preserve these rational boundary weights");
        }
        const int axis = slot % 2;
        if (has_domain[axis] && !std::equal(original->knots, original->knots + 4, domains[axis])) {
          return fail("Legacy quad fill requires matching opposite-edge parameter domains");
        }
        std::copy_n(original->knots, 4, domains[axis]);
        has_domain[axis] = true;
        if (original->nurb->pntsv == 1) {
          result.removed_wires.append(int(edge.id));
        }
      }
      slot++;
    }
    constexpr int point_slots[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
      if (double3(surface->points[point_slots[i]].position) !=
              double3(index->vertex(corner_ids[i])->position) ||
          surface->points[point_slots[i]].weight != 1)
      {
        return fail(
            "This fill requires native trimmed-face storage; legacy topology is unchanged");
      }
    }
    const auto &parent = *input->vertex_splines.lookup(corner_ids[0]);
    Nurb *patch = BKE_nurb_duplicate(&parent);
    MEM_delete(patch->bp);
    MEM_SAFE_DELETE(patch->knotsu);
    MEM_SAFE_DELETE(patch->knotsv);
    patch->pntsu = patch->pntsv = patch->orderu = patch->orderv = 2;
    patch->flagu = patch->flagv = CU_NURB_ENDPOINT;
    patch->hide = 0;
    patch->bp = MEM_new_array<BPoint>(4, __func__);
    patch->knotsu = MEM_new_array_uninitialized<float>(4, __func__);
    patch->knotsv = MEM_new_array_uninitialized<float>(4, __func__);
    std::copy_n(domains[0], 4, patch->knotsu);
    std::copy_n(domains[1], 4, patch->knotsv);
    for (int i = 0; i < 4; i++) {
      patch->bp[point_slots[i]] = *input->vertices.lookup(corner_ids[i]);
      patch->bp[point_slots[i]].vec[3] = 1;
      patch->bp[point_slots[i]].hide = 0;
      patch->topology_vertex_ids[i] = int(corner_ids[i]);
      patch->topology_edge_ids[i] = int(edge_ids[i]);
    }
    if (face.orientation < 0) {
      std::swap(patch->bp[0], patch->bp[2]);
      std::swap(patch->bp[1], patch->bp[3]);
      std::swap(patch->topology_vertex_ids[0], patch->topology_vertex_ids[3]);
      std::swap(patch->topology_vertex_ids[1], patch->topology_vertex_ids[2]);
      std::swap(patch->topology_edge_ids[0], patch->topology_edge_ids[2]);
    }
    patch->topology_face_id = int(id);
    patch->mat_nr = face.material_index;
    select_all_points(*patch, true);
    BLI_addtail(&result.added, patch);
  }
  for (const int64_t id : absorbed_vertices) {
    const Nurb &parent = *input->vertex_splines.lookup(id);
    if (parent.pntsu == 1 && parent.pntsv == 1) {
      result.removed_points.append(int(id));
    }
  }
  // Check the complete tagged candidate before ED transfers any source ownership.
  LegacyTopologyEdit candidate;
  for (const Nurb &nurb : source) {
    if (is_tagged(nurb) && !result.removed_faces.contains(nurb.topology_face_id) &&
        !(nurb.pntsv == 1 && result.removed_wires.contains(nurb.topology_edge_ids[0])) &&
        !(nurb.pntsu == 1 && result.removed_points.contains(nurb.topology_vertex_ids[0])))
    {
      BLI_addtail(&candidate.added, BKE_nurb_duplicate(&nurb));
    }
  }
  for (const Nurb &nurb : result.added) {
    BLI_addtail(&candidate.added, BKE_nurb_duplicate(&nurb));
  }
  OwnedGeometry checked(geometry_from_tagged_legacy(candidate.added, nullptr, r_error));
  if (!checked) {
    return std::nullopt;
  }
  return result;
}

std::optional<LegacyTopologyEdit> legacy_topology_delete(const ListBaseT<Nurb> &source,
                                                         const DeleteMode mode,
                                                         std::string *r_error)
{
  auto input = LegacyTopologySource::create(source, r_error);
  if (!input) {
    return std::nullopt;
  }
  Vector<int64_t> vertices, edges, faces;
  const auto plan = plan_component_deletion(
      *input->geometry,
      component_selection(input->selection, vertices, edges, faces),
      mode,
      r_error);
  if (!plan) {
    return std::nullopt;
  }
  const auto index = TopologyIndex::build(*input->geometry);
  LegacyTopologyEdit result;
  result.removed_faces = input->recovered_faces;
  result.recovered_patch_remnants = input->recovered_faces.size();
  for (const int64_t id : plan->faces) {
    result.removed_faces.append(int(id));
  }
  for (const Nurb &nurb : source) {
    if (nurb.pntsv != 1 || !is_tagged(nurb)) {
      continue;
    }
    if (nurb.pntsu == 2 && plan->edges.contains(nurb.topology_edge_ids[0])) {
      result.removed_wires.append(nurb.topology_edge_ids[0]);
    }
    else if (nurb.pntsu == 1 && plan->vertices.contains(nurb.topology_vertex_ids[0])) {
      result.removed_points.append(nurb.topology_vertex_ids[0]);
    }
  }
  for (const NurbsTopologyEdge &edge : input->geometry->edges) {
    const Span<EdgeUseReference> uses = index->uses_for_edge(edge.id);
    if (plan->edges.contains(edge.id) || uses.is_empty() ||
        std::any_of(uses.begin(), uses.end(), [&](const EdgeUseReference &use) {
          return !plan->faces.contains(use.face_id);
        }))
    {
      continue;
    }
    Nurb *wire = make_line(input->edges.lookup(edge.id));
    wire->topology_edge_ids[0] = int(edge.id);
    wire->topology_vertex_ids[0] = int(edge.vertex_ids[0]);
    wire->topology_vertex_ids[1] = int(edge.vertex_ids[1]);
    select_all_points(*wire, false);
    BLI_addtail(&result.added, wire);
  }
  for (const NurbsTopologyVertex &vertex : input->geometry->vertices) {
    if (plan->vertices.contains(vertex.id)) {
      continue;
    }
    const Span<int64_t> incident_edges = index->edges_for_vertex(vertex.id);
    if (incident_edges.is_empty() ||
        std::any_of(incident_edges.begin(), incident_edges.end(), [&](const int64_t id) {
          return !plan->edges.contains(id);
        }))
    {
      continue;
    }
    Nurb *point = make_point(*input->vertex_splines.lookup(vertex.id),
                             *input->vertices.lookup(vertex.id),
                             int(vertex.id));
    select_all_points(*point, false);
    BLI_addtail(&result.added, point);
  }
  return result;
}

}  // namespace blender::bke::nurbs
