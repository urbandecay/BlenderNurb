/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_set.hh"

#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_selection.hh"
#include "BKE_report.hh"

#include "BLO_read_write.hh"
#include "BLO_readfile.hh"

#include "BLT_translation.hh"

namespace blender::bke::nurbs {

void CurveDeleter::operator()(NurbsCurveGeometry *curve) const
{
  if (curve) {
    MEM_SAFE_DELETE(curve->points);
    MEM_SAFE_DELETE(curve->knots);
    MEM_delete(curve);
  }
}

void SurfaceDeleter::operator()(NurbsSurfaceGeometry *surface) const
{
  if (surface) {
    MEM_SAFE_DELETE(surface->points);
    MEM_SAFE_DELETE(surface->knots_u);
    MEM_SAFE_DELETE(surface->knots_v);
    MEM_delete(surface);
  }
}

template<typename T, typename Fn>
static void copy_list(ListBaseT<T> &dst, const ListBaseT<T> &src, Fn copy_data)
{
  dst.clear_no_delete();
  for (const T &value : src) {
    T *copy = MEM_dupalloc(&value);
    copy_data(*copy);
    BLI_addtail(&dst, copy);
  }
}

NurbsGeometry *geometry_copy(const NurbsGeometry *geometry)
{
  if (geometry == nullptr) {
    return nullptr;
  }
  NurbsGeometry *copy = MEM_dupalloc(geometry);
  copy_list(copy->curves, geometry->curves, [](NurbsCurveGeometry &curve) {
    curve.points = MEM_dupalloc(curve.points);
    curve.knots = MEM_dupalloc(curve.knots);
  });
  copy_list(copy->surfaces, geometry->surfaces, [](NurbsSurfaceGeometry &surface) {
    surface.points = MEM_dupalloc(surface.points);
    surface.knots_u = MEM_dupalloc(surface.knots_u);
    surface.knots_v = MEM_dupalloc(surface.knots_v);
  });
  copy_list(copy->vertices, geometry->vertices, [](NurbsTopologyVertex &) {});
  copy_list(copy->edges, geometry->edges, [](NurbsTopologyEdge &) {});
  copy_list(copy->faces, geometry->faces, [](NurbsTopologyFace &face) {
    const ListBaseT<NurbsTopologyLoop> loops = face.loops;
    copy_list(face.loops, loops, [](NurbsTopologyLoop &loop) {
      const ListBaseT<NurbsTopologyEdgeUse> uses = loop.edge_uses;
      copy_list(loop.edge_uses, uses, [](NurbsTopologyEdgeUse &) {});
    });
  });
  copy->stored_selection = selection_storage_copy(geometry->stored_selection);
  return copy;
}

void geometry_free(NurbsGeometry *geometry)
{
  if (geometry == nullptr) {
    return;
  }
  for (NurbsCurveGeometry &curve : geometry->curves) {
    MEM_SAFE_DELETE(curve.points);
    MEM_SAFE_DELETE(curve.knots);
  }
  BLI_freelistN(&geometry->curves);
  for (NurbsSurfaceGeometry &surface : geometry->surfaces) {
    MEM_SAFE_DELETE(surface.points);
    MEM_SAFE_DELETE(surface.knots_u);
    MEM_SAFE_DELETE(surface.knots_v);
  }
  BLI_freelistN(&geometry->surfaces);
  BLI_freelistN(&geometry->vertices);
  BLI_freelistN(&geometry->edges);
  for (NurbsTopologyFace &face : geometry->faces) {
    for (NurbsTopologyLoop &loop : face.loops) {
      BLI_freelistN(&loop.edge_uses);
    }
    BLI_freelistN(&face.loops);
  }
  BLI_freelistN(&geometry->faces);
  selection_storage_free(geometry->stored_selection);
  MEM_delete(geometry);
}

void geometry_blend_write(BlendWriter *writer, const NurbsGeometry &geometry)
{
  writer->write_struct(&geometry);
  for (const NurbsCurveGeometry &curve : geometry.curves) {
    writer->write_struct(&curve);
    writer->write_struct_array(curve.points_num, curve.points);
    writer->write_double_array(curve.knots_num, curve.knots);
  }
  for (const NurbsSurfaceGeometry &surface : geometry.surfaces) {
    writer->write_struct(&surface);
    writer->write_struct_array(surface.points_num, surface.points);
    writer->write_double_array(surface.knots_u_num, surface.knots_u);
    writer->write_double_array(surface.knots_v_num, surface.knots_v);
  }
  for (const NurbsTopologyVertex &vertex : geometry.vertices) {
    writer->write_struct(&vertex);
  }
  for (const NurbsTopologyEdge &edge : geometry.edges) {
    writer->write_struct(&edge);
  }
  for (const NurbsTopologyFace &face : geometry.faces) {
    writer->write_struct(&face);
    for (const NurbsTopologyLoop &loop : face.loops) {
      writer->write_struct(&loop);
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        writer->write_struct(&use);
      }
    }
  }
  if (geometry.stored_selection != nullptr) {
    const NurbsStoredSelection &selection = *geometry.stored_selection;
    writer->write_struct(&selection);
    writer->write_struct_array(selection.elements_num, selection.elements);
    writer->write_struct_array(selection.history_num, selection.history);
  }
}

void geometry_blend_read(BlendDataReader *reader, NurbsGeometry &geometry)
{
  BLO_read_struct_list(reader, NurbsCurveGeometry, &geometry.curves);
  for (NurbsCurveGeometry &curve : geometry.curves) {
    BLO_read_array_and_validate_size(reader, &curve.points, &curve.points_num);
    BLO_read_array_and_validate_size(reader, &curve.knots, &curve.knots_num);
  }
  BLO_read_struct_list(reader, NurbsSurfaceGeometry, &geometry.surfaces);
  for (NurbsSurfaceGeometry &surface : geometry.surfaces) {
    BLO_read_array_and_validate_size(reader, &surface.points, &surface.points_num);
    BLO_read_array_and_validate_size(reader, &surface.knots_u, &surface.knots_u_num);
    BLO_read_array_and_validate_size(reader, &surface.knots_v, &surface.knots_v_num);
  }
  BLO_read_struct_list(reader, NurbsTopologyVertex, &geometry.vertices);
  BLO_read_struct_list(reader, NurbsTopologyEdge, &geometry.edges);
  BLO_read_struct_list(reader, NurbsTopologyFace, &geometry.faces);
  for (NurbsTopologyFace &face : geometry.faces) {
    BLO_read_struct_list(reader, NurbsTopologyLoop, &face.loops);
    for (NurbsTopologyLoop &loop : face.loops) {
      BLO_read_struct_list(reader, NurbsTopologyEdgeUse, &loop.edge_uses);
    }
  }
  BLO_read_struct(reader, NurbsStoredSelection, &geometry.stored_selection);
  if (geometry.stored_selection != nullptr) {
    NurbsStoredSelection &selection = *geometry.stored_selection;
    BLO_read_array_and_validate_size(reader, &selection.elements, &selection.elements_num);
    BLO_read_array_and_validate_size(reader, &selection.history, &selection.history_num);
  }
  std::string error;
  if (!geometry_validate_structure(geometry, &error)) {
    BLO_reportf_wrap(BLO_read_data_reports(reader),
                     RPT_ERROR,
                     RPT_("Invalid NURBS source structure: %s"),
                     error.c_str());
  }
}

static bool tolerance_is_valid(const double tolerance)
{
  return std::isfinite(tolerance) && tolerance >= 0.0;
}

static bool position_is_valid(const double position[3])
{
  return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]);
}

static bool points_are_valid(const NurbsControlPoint *points, const int points_num)
{
  if (points == nullptr || points_num <= 0) {
    return false;
  }
  bool has_nonzero_weight = false;
  for (int i = 0; i < points_num; i++) {
    if (!position_is_valid(points[i].position) || !std::isfinite(points[i].weight)) {
      return false;
    }
    has_nonzero_weight |= points[i].weight != 0.0;
  }
  /* Denominator zeros over the parameter domain require geometric validation. */
  return has_nonzero_weight;
}

static bool knots_are_valid(const double *knots,
                            const int knots_num,
                            const int points_num,
                            const int order)
{
  if (order < 2 || points_num < order || knots == nullptr ||
      int64_t(knots_num) != int64_t(points_num) + order)
  {
    return false;
  }
  int multiplicity = 0;
  for (int i = 0; i < knots_num; i++) {
    if (!std::isfinite(knots[i]) || (i > 0 && knots[i] < knots[i - 1])) {
      return false;
    }
    multiplicity = i > 0 && knots[i] == knots[i - 1] ? multiplicity + 1 : 1;
    if (multiplicity > order) {
      return false;
    }
  }
  return knots[order - 1] < knots[points_num];
}

static bool range_is_valid(const NurbsCurveGeometry &curve, const double range[2])
{
  return std::isfinite(range[0]) && std::isfinite(range[1]) && range[0] < range[1] &&
         range[0] >= curve.knots[curve.order - 1] && range[1] <= curve.knots[curve.points_num];
}

bool curve_validate_structure(const NurbsCurveGeometry &curve, std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (!ELEM(curve.dimension, 2, 3) ||
      !knots_are_valid(curve.knots, curve.knots_num, curve.points_num, curve.order) ||
      !points_are_valid(curve.points, curve.points_num))
  {
    if (r_error != nullptr) {
      *r_error = "Invalid curve coefficients, weights or knots";
    }
    return false;
  }
  if (curve.dimension == 2) {
    for (int i = 0; i < curve.points_num; i++) {
      if (curve.points[i].position[2] != 0.0) {
        if (r_error != nullptr) {
          *r_error = "A p-curve must have two-dimensional coefficients";
        }
        return false;
      }
    }
  }
  return true;
}

bool surface_validate_structure(const NurbsSurfaceGeometry &surface, std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (int64_t(surface.points_u) * surface.points_v != surface.points_num ||
      !knots_are_valid(surface.knots_u, surface.knots_u_num, surface.points_u, surface.order_u) ||
      !knots_are_valid(surface.knots_v, surface.knots_v_num, surface.points_v, surface.order_v) ||
      !points_are_valid(surface.points, surface.points_num))
  {
    if (r_error != nullptr) {
      *r_error = "Invalid surface dimensions, coefficients, weights or knots";
    }
    return false;
  }
  return true;
}

bool geometry_validate_structure(const NurbsGeometry &geometry, std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return false;
  };
  Set<int64_t> ids;
  auto add_id = [&](const int64_t id) { return id > 0 && id < geometry.next_id && ids.add(id); };
  if (geometry.next_id <= 0) {
    return fail("The next identity must be positive");
  }
  Map<int64_t, const NurbsCurveGeometry *> curves;
  Map<int64_t, const NurbsSurfaceGeometry *> surfaces;
  Set<int64_t> vertices;
  Set<int64_t> faces;
  Map<int64_t, const NurbsTopologyEdge *> edges;
  for (const NurbsCurveGeometry &curve : geometry.curves) {
    if (!add_id(curve.id)) {
      return fail("Invalid or repeated curve identity");
    }
    if (!curve_validate_structure(curve, r_error)) {
      return false;
    }
    curves.add_new(curve.id, &curve);
  }
  for (const NurbsSurfaceGeometry &surface : geometry.surfaces) {
    if (!add_id(surface.id)) {
      return fail("Invalid or repeated surface identity");
    }
    if (!surface_validate_structure(surface, r_error)) {
      return false;
    }
    surfaces.add_new(surface.id, &surface);
  }
  for (const NurbsTopologyVertex &vertex : geometry.vertices) {
    if (!add_id(vertex.id) || !position_is_valid(vertex.position) ||
        !tolerance_is_valid(vertex.tolerance))
    {
      return fail("Invalid or repeated vertex identity, position or tolerance");
    }
    vertices.add_new(vertex.id);
  }
  for (const NurbsTopologyEdge &edge : geometry.edges) {
    if (!add_id(edge.id) || !vertices.contains(edge.vertex_ids[0]) ||
        !vertices.contains(edge.vertex_ids[1]))
    {
      return fail("Invalid edge identity or endpoint reference");
    }
    const NurbsCurveGeometry *curve = curves.lookup_default(edge.curve_id, nullptr);
    if (curve == nullptr || curve->dimension != 3 ||
        !range_is_valid(*curve, edge.parameter_range) || !tolerance_is_valid(edge.tolerance))
    {
      return fail("Invalid edge curve, parameter range or tolerance");
    }
    edges.add_new(edge.id, &edge);
  }
  for (const NurbsTopologyFace &face : geometry.faces) {
    if (!add_id(face.id) || !surfaces.contains(face.surface_id) ||
        !ELEM(face.orientation, -1, 1) || !tolerance_is_valid(face.tolerance) ||
        face.material_index < 0)
    {
      return fail("Invalid face identity, support, orientation, material or tolerance");
    }
    faces.add_new(face.id);
    int outer_loops = 0;
    for (const NurbsTopologyLoop &loop : face.loops) {
      if (!add_id(loop.id) || !ELEM(loop.is_inner, 0, 1) || loop.edge_uses.is_empty()) {
        return fail("Invalid boundary loop");
      }
      outer_loops += !loop.is_inner;
      int64_t first_vertex = 0, previous_vertex = 0;
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        const NurbsTopologyEdge *edge = edges.lookup_default(use.edge_id, nullptr);
        const NurbsCurveGeometry *pcurve = curves.lookup_default(use.pcurve_id, nullptr);
        if (!add_id(use.id) || edge == nullptr || pcurve == nullptr || pcurve->dimension != 2 ||
            !ELEM(use.reversed, 0, 1) || !tolerance_is_valid(use.tolerance) ||
            !range_is_valid(*pcurve, edge->parameter_range))
        {
          return fail("Invalid boundary edge use or p-curve reference");
        }
        const int64_t start = edge->vertex_ids[use.reversed ? 1 : 0];
        const int64_t end = edge->vertex_ids[use.reversed ? 0 : 1];
        if (first_vertex == 0) {
          first_vertex = start;
        }
        else if (start != previous_vertex) {
          return fail("Boundary uses do not form a connected vertex chain");
        }
        previous_vertex = end;
      }
      if (previous_vertex != first_vertex) {
        return fail("Boundary vertex chain is not closed");
      }
    }
    if (outer_loops != 1) {
      return fail("A face must have one outer boundary");
    }
  }
  if (geometry.stored_selection != nullptr) {
    const auto selection = selection_from_storage(geometry.stored_selection, r_error);
    if (!selection) {
      return false;
    }
    return selection_validate(
        *selection,
        [&](const ElementRef element) {
          switch (element.domain) {
            case ElementDomain::Vertex:
              return vertices.contains(element.id);
            case ElementDomain::Edge:
              return edges.contains(element.id);
            case ElementDomain::Face:
              return faces.contains(element.id);
          }
          return false;
        },
        r_error);
  }
  return true;
}

std::optional<TopologyIndex> TopologyIndex::build(const NurbsGeometry &geometry,
                                                  std::string *r_error)
{
  if (!geometry_validate_structure(geometry, r_error)) {
    return std::nullopt;
  }
  TopologyIndex index;
  for (const NurbsTopologyVertex &vertex : geometry.vertices) {
    index.vertices_.add_new(vertex.id, &vertex);
  }
  for (const NurbsTopologyEdge &edge : geometry.edges) {
    index.edges_.add_new(edge.id, &edge);
    index.vertex_edges_.lookup_or_add_default(edge.vertex_ids[0]).append(edge.id);
    if (edge.vertex_ids[0] != edge.vertex_ids[1]) {
      index.vertex_edges_.lookup_or_add_default(edge.vertex_ids[1]).append(edge.id);
    }
  }
  for (const NurbsTopologyFace &face : geometry.faces) {
    index.faces_.add_new(face.id, &face);
    for (const NurbsTopologyLoop &loop : face.loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        index.edge_uses_.lookup_or_add_default(use.edge_id).append({face.id, loop.id, &use});
      }
    }
  }
  return index;
}

const NurbsTopologyVertex *TopologyIndex::vertex(const int64_t id) const
{
  return vertices_.lookup_default(id, nullptr);
}

const NurbsTopologyEdge *TopologyIndex::edge(const int64_t id) const
{
  return edges_.lookup_default(id, nullptr);
}

const NurbsTopologyFace *TopologyIndex::face(const int64_t id) const
{
  return faces_.lookup_default(id, nullptr);
}

Span<int64_t> TopologyIndex::edges_for_vertex(const int64_t id) const
{
  const Vector<int64_t> *edges = vertex_edges_.lookup_ptr(id);
  return edges ? edges->as_span() : Span<int64_t>();
}

Span<EdgeUseReference> TopologyIndex::uses_for_edge(const int64_t id) const
{
  const Vector<EdgeUseReference> *uses = edge_uses_.lookup_ptr(id);
  return uses ? uses->as_span() : Span<EdgeUseReference>();
}

}  // namespace blender::bke::nurbs
