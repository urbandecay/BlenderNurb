/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_topology_edit.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_curve.hh"
#include "BKE_nurbs_surface.hh"
#include "BKE_nurbs_surface_curve.hh"
#include "BKE_nurbs_topology.hh"

namespace blender {

namespace {

bool fail(std::string *r_error, const char *message)
{
  if (r_error != nullptr) {
    *r_error = message;
  }
  return false;
}

void free_curve_data(NurbsCurve &curve)
{
  if (curve.control_vertices != nullptr) {
    MEM_delete(curve.control_vertices);
  }
  if (curve.knots != nullptr) {
    MEM_delete(curve.knots);
  }
  curve.control_vertices = nullptr;
  curve.knots = nullptr;
}

struct OwnedCurve {
  NurbsCurve curve = {};

  OwnedCurve() = default;
  OwnedCurve(const OwnedCurve &) = delete;
  OwnedCurve &operator=(const OwnedCurve &) = delete;

  OwnedCurve(OwnedCurve &&other) noexcept : curve(other.curve)
  {
    other.curve.control_vertices = nullptr;
    other.curve.knots = nullptr;
  }

  OwnedCurve &operator=(OwnedCurve &&other) noexcept
  {
    if (this != &other) {
      free_curve_data(curve);
      curve = other.curve;
      other.curve.control_vertices = nullptr;
      other.curve.knots = nullptr;
    }
    return *this;
  }

  ~OwnedCurve()
  {
    free_curve_data(curve);
  }
};

const NurbsCurve *find_curve(const ListBaseT<NurbsCurve> &curves, const uint64_t id)
{
  for (const NurbsCurve &curve : curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

NurbsCurve *find_curve(ListBaseT<NurbsCurve> &curves, const uint64_t id)
{
  for (NurbsCurve &curve : curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

NurbsSurface *find_surface(NurbsTopology &topology, const uint64_t id)
{
  for (NurbsSurface &surface : topology.surfaces) {
    if (surface.id == id) {
      return &surface;
    }
  }
  return nullptr;
}

const NurbsSurface *find_surface(const NurbsTopology &topology, const uint64_t id)
{
  for (const NurbsSurface &surface : topology.surfaces) {
    if (surface.id == id) {
      return &surface;
    }
  }
  return nullptr;
}

const NurbsTopoEdge *find_edge(const NurbsTopology &topology, const uint64_t id)
{
  for (const NurbsTopoEdge &edge : topology.edges) {
    if (edge.id == id) {
      return &edge;
    }
  }
  return nullptr;
}

const NurbsCurve *find_trim(const NurbsTopology &topology, const uint64_t id)
{
  for (const NurbsCurve &curve : topology.trim_curves) {
    if (curve.id == id) {
      return &curve;
    }
  }
  return nullptr;
}

bool copy_curve(const NurbsCurve &source, NurbsCurve &r_curve)
{
  if (!BKE_nurbs_curve_is_valid(source) || r_curve.control_vertices != nullptr ||
      r_curve.knots != nullptr)
  {
    return false;
  }
  NurbsCurve copy = source;
  copy.next = nullptr;
  copy.prev = nullptr;
  copy.control_vertices = MEM_new_array<NurbsControlVertex>(source.control_vertex_count, __func__);
  copy.knots = MEM_new_array<double>(source.knot_count, __func__);
  std::copy_n(source.control_vertices, source.control_vertex_count, copy.control_vertices);
  std::copy_n(source.knots, source.knot_count, copy.knots);
  r_curve = copy;
  return true;
}

bool is_clamped_boundary(const double *knots,
                         const int knot_count,
                         const int degree,
                         const double domain[2])
{
  if (knots == nullptr || degree < 1 || knot_count < 2 * degree) {
    return false;
  }
  for (int i = 0; i < degree; i++) {
    if (knots[i] != domain[0] || knots[knot_count - 1 - i] != domain[1]) {
      return false;
    }
  }
  return true;
}

bool normalized_parameter_close(const double a,
                                const double b,
                                const double domain[2],
                                const double tolerance)
{
  return std::abs((a - b) / (domain[1] - domain[0])) <= tolerance;
}

struct IsoBoundary {
  bool varying_u = false;
  double fixed_parameter = 0.0;
  int fixed_index = 0;
};

bool get_linear_trim_boundary(const NurbsCurve &trim,
                              const NurbsSurface &surface,
                              const double parameter_start,
                              const double parameter_end,
                              const double parameter_tolerance,
                              IsoBoundary &r_boundary,
                              double &r_varying_start,
                              double &r_varying_end)
{
  if (!BKE_nurbs_curve_is_valid(trim) || trim.dimension != 2 || trim.degree != 1 ||
      trim.order != 2 || trim.rational != 0 || trim.control_vertex_count != 2 ||
      parameter_start < trim.domain[0] || parameter_end > trim.domain[1])
  {
    return false;
  }
  double start[3], end[3];
  if (!BKE_nurbs_curve_evaluate(trim, parameter_start, start) ||
      !BKE_nurbs_curve_evaluate(trim, parameter_end, end) || start[2] != 0.0 || end[2] != 0.0)
  {
    return false;
  }

  const bool fixed_v = normalized_parameter_close(
      start[1], end[1], surface.domain_v, parameter_tolerance);
  const bool fixed_u = normalized_parameter_close(
      start[0], end[0], surface.domain_u, parameter_tolerance);
  if (fixed_v == fixed_u) {
    return false;
  }

  if (fixed_v) {
    const bool at_v_start = normalized_parameter_close(
        start[1], surface.domain_v[0], surface.domain_v, parameter_tolerance);
    const bool at_v_end = normalized_parameter_close(
        start[1], surface.domain_v[1], surface.domain_v, parameter_tolerance);
    if (!normalized_parameter_close(start[1], end[1], surface.domain_v, parameter_tolerance) ||
        start[1] < surface.domain_v[0] || start[1] > surface.domain_v[1] ||
        start[0] < surface.domain_u[0] || start[0] > surface.domain_u[1] ||
        end[0] < surface.domain_u[0] || end[0] > surface.domain_u[1] ||
        normalized_parameter_close(start[0], end[0], surface.domain_u, parameter_tolerance))
    {
      return false;
    }
    r_boundary.varying_u = true;
    r_boundary.fixed_parameter = at_v_start ? surface.domain_v[0] :
                                 at_v_end   ? surface.domain_v[1] :
                                              start[1];
    r_boundary.fixed_index = at_v_start ? 0 : at_v_end ? surface.control_vertex_count_v - 1 : -1;
    r_varying_start = start[0];
    r_varying_end = end[0];
    return true;
  }

  const bool at_u_start = normalized_parameter_close(
      start[0], surface.domain_u[0], surface.domain_u, parameter_tolerance);
  const bool at_u_end = normalized_parameter_close(
      start[0], surface.domain_u[1], surface.domain_u, parameter_tolerance);
  if (!normalized_parameter_close(start[0], end[0], surface.domain_u, parameter_tolerance) ||
      start[0] < surface.domain_u[0] || start[0] > surface.domain_u[1] ||
      start[1] < surface.domain_v[0] || start[1] > surface.domain_v[1] ||
      end[1] < surface.domain_v[0] || end[1] > surface.domain_v[1] ||
      normalized_parameter_close(start[1], end[1], surface.domain_v, parameter_tolerance))
  {
    return false;
  }
  r_boundary.varying_u = false;
  r_boundary.fixed_parameter = at_u_start ? surface.domain_u[0] :
                               at_u_end   ? surface.domain_u[1] :
                                            start[0];
  r_boundary.fixed_index = at_u_start ? 0 : at_u_end ? surface.control_vertex_count_u - 1 : -1;
  r_varying_start = start[1];
  r_varying_end = end[1];
  return true;
}

bool extract_interval(const NurbsCurve &source,
                      const double start,
                      const double end,
                      OwnedCurve &r_curve)
{
  const double low = std::min(start, end);
  const double high = std::max(start, end);
  if (!BKE_nurbs_curve_is_valid(source) || low < source.domain[0] || high > source.domain[1] ||
      low >= high)
  {
    return false;
  }

  OwnedCurve after_low;
  if (low == source.domain[0]) {
    if (!copy_curve(source, after_low.curve)) {
      return false;
    }
  }
  else {
    OwnedCurve left;
    if (!BKE_nurbs_curve_split(source, low, left.curve, after_low.curve)) {
      return false;
    }
  }

  if (high == after_low.curve.domain[1]) {
    return copy_curve(after_low.curve, r_curve.curve);
  }
  OwnedCurve left;
  OwnedCurve right;
  if (!BKE_nurbs_curve_split(after_low.curve, high, left.curve, right.curve)) {
    return false;
  }
  r_curve = std::move(left);
  return true;
}

void reverse_curve_parameterization(NurbsCurve &curve)
{
  const double sum = curve.domain[0] + curve.domain[1];
  std::reverse(curve.control_vertices, curve.control_vertices + curve.control_vertex_count);
  for (int i = 0; i < curve.knot_count / 2; i++) {
    const int opposite = curve.knot_count - i - 1;
    const double left = curve.knots[i];
    const double right = curve.knots[opposite];
    curve.knots[i] = sum - right;
    curve.knots[opposite] = sum - left;
  }
  if (curve.knot_count % 2 != 0) {
    const int middle = curve.knot_count / 2;
    curve.knots[middle] = sum - curve.knots[middle];
  }
}

bool curve_close(const NurbsCurve &a,
                 const NurbsCurve &b,
                 const double length_tolerance,
                 const double parameter_tolerance)
{
  if (a.degree != b.degree || a.order != b.order ||
      a.control_vertex_count != b.control_vertex_count || a.knot_count != b.knot_count ||
      a.rational != b.rational ||
      std::abs((a.domain[0] - b.domain[0]) / (a.domain[1] - a.domain[0])) > parameter_tolerance ||
      std::abs((a.domain[1] - b.domain[1]) / (a.domain[1] - a.domain[0])) > parameter_tolerance)
  {
    return false;
  }
  for (int i = 0; i < a.knot_count; i++) {
    if (std::abs((a.knots[i] - b.knots[i]) / (a.domain[1] - a.domain[0])) > parameter_tolerance) {
      return false;
    }
  }
  for (int i = 0; i < a.control_vertex_count; i++) {
    if (std::abs(a.control_vertices[i].weight - b.control_vertices[i].weight) >
        parameter_tolerance)
    {
      return false;
    }
    for (int axis = 0; axis < 3; axis++) {
      if (std::abs(a.control_vertices[i].position[axis] - b.control_vertices[i].position[axis]) >
          length_tolerance)
      {
        return false;
      }
    }
  }
  return true;
}

bool make_reconciled_curve(const NurbsCurve &source_curve,
                           const NurbsCurve &trim,
                           const NurbsSurface &surface,
                           const double parameter_tolerance,
                           OwnedCurve &r_curve)
{
  IsoBoundary boundary;
  double varying_start, varying_end;
  if (!get_linear_trim_boundary(trim,
                                surface,
                                source_curve.domain[0],
                                source_curve.domain[1],
                                parameter_tolerance,
                                boundary,
                                varying_start,
                                varying_end))
  {
    return false;
  }
  OwnedCurve isocurve;
  if (!BKE_nurbs_surface_isocurve(
          surface, boundary.varying_u, boundary.fixed_parameter, isocurve.curve))
  {
    return false;
  }
  if (!source_curve.rational) {
    const double common_weight = isocurve.curve.control_vertices[0].weight;
    bool constant_weights = true;
    for (int i = 1; i < isocurve.curve.control_vertex_count; i++) {
      constant_weights &= isocurve.curve.control_vertices[i].weight == common_weight;
    }
    if (constant_weights) {
      /* Remove an exact common homogeneous scale before interval extraction. */
      isocurve.curve.rational = 0;
      for (int i = 0; i < isocurve.curve.control_vertex_count; i++) {
        isocurve.curve.control_vertices[i].weight = 1.0;
      }
    }
  }
  OwnedCurve cropped;
  if (!extract_interval(isocurve.curve, varying_start, varying_end, cropped)) {
    return false;
  }
  if (varying_start > varying_end) {
    reverse_curve_parameterization(cropped.curve);
  }

  const double parameter_scale = (source_curve.domain[1] - source_curve.domain[0]) /
                                 (varying_end - varying_start);
  for (int i = 0; i < cropped.curve.knot_count; i++) {
    cropped.curve.knots[i] = source_curve.domain[0] +
                             (cropped.curve.knots[i] -
                              (varying_start < varying_end ? varying_start : varying_end)) *
                                 std::abs(parameter_scale);
  }
  cropped.curve.domain[0] = source_curve.domain[0];
  cropped.curve.domain[1] = source_curve.domain[1];

  if (cropped.curve.degree != source_curve.degree || cropped.curve.order != source_curve.order ||
      cropped.curve.control_vertex_count != source_curve.control_vertex_count ||
      cropped.curve.knot_count != source_curve.knot_count)
  {
    return false;
  }

  cropped.curve.id = source_curve.id;
  cropped.curve.dimension = source_curve.dimension;
  if (!source_curve.rational) {
    const double weight = cropped.curve.control_vertices[0].weight;
    for (int i = 1; i < cropped.curve.control_vertex_count; i++) {
      if (cropped.curve.control_vertices[i].weight != weight) {
        return false;
      }
    }
    cropped.curve.rational = 0;
    for (int i = 0; i < cropped.curve.control_vertex_count; i++) {
      cropped.curve.control_vertices[i].weight = 1.0;
    }
  }
  else {
    cropped.curve.rational = 1;
  }
  for (int i = 0; i < cropped.curve.control_vertex_count; i++) {
    cropped.curve.control_vertices[i].id = source_curve.control_vertices[i].id;
    cropped.curve.control_vertices[i].flag = source_curve.control_vertices[i].flag;
    std::memcpy(cropped.curve.control_vertices[i]._pad,
                source_curve.control_vertices[i]._pad,
                sizeof(cropped.curve.control_vertices[i]._pad));
  }
  if (!BKE_nurbs_curve_is_valid(cropped.curve)) {
    return false;
  }
  r_curve = std::move(cropped);
  return true;
}

bool translate_boundary_rows(NurbsGeometry &staged,
                             const uint64_t support_curve_id,
                             const double3 &delta,
                             Set<uint64_t> &r_affected_face_ids,
                             std::string *r_error)
{
  NurbsTopology &topology = *staged.topology;
  const NurbsCurve *support = find_curve(staged.curves, support_curve_id);
  if (support == nullptr) {
    return fail(r_error, "The requested boundary curve support does not exist.");
  }
  Set<uint64_t> translated_control_vertices;
  bool found_use = false;
  for (NurbsTopoFace &face : topology.faces) {
    NurbsSurface *surface = find_surface(topology, face.surface_id);
    if (surface == nullptr) {
      return fail(r_error, "A face references a missing surface support.");
    }
    for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
      const NurbsTrimLoop &loop = face.loops[loop_index];
      for (int use_index = 0; use_index < loop.use_count; use_index++) {
        const NurbsEdgeUse &use = loop.uses[use_index];
        const NurbsTopoEdge *edge = find_edge(topology, use.edge_id);
        if (edge == nullptr || edge->curve_id != support_curve_id) {
          continue;
        }
        found_use = true;
        const NurbsCurve *trim = find_trim(topology, use.trim_curve_id);
        IsoBoundary boundary;
        double varying_start, varying_end;
        if (trim == nullptr ||
            !get_linear_trim_boundary(*trim,
                                      *surface,
                                      support->domain[0],
                                      support->domain[1],
                                      topology.parameter_tolerance,
                                      boundary,
                                      varying_start,
                                      varying_end) ||
            boundary.fixed_index < 0 ||
            !(boundary.varying_u ? is_clamped_boundary(surface->knots_v,
                                                       surface->knot_count_v,
                                                       surface->degree_v,
                                                       surface->domain_v) :
                                   is_clamped_boundary(surface->knots_u,
                                                       surface->knot_count_u,
                                                       surface->degree_u,
                                                       surface->domain_u)))
        {
          /* The trim must cover the full support domain and therefore the full surface boundary.
           * The endpoint check below is deliberately orientation agnostic. */
          return fail(r_error,
                      "The requested support must have a linear UV trim over a complete surface "
                      "boundary.");
        }
        const double *varying_domain = boundary.varying_u ? surface->domain_u : surface->domain_v;
        if (!(normalized_parameter_close(varying_start,
                                         varying_domain[0],
                                         varying_domain,
                                         topology.parameter_tolerance) &&
              normalized_parameter_close(
                  varying_end, varying_domain[1], varying_domain, topology.parameter_tolerance)) &&
            !(normalized_parameter_close(varying_start,
                                         varying_domain[1],
                                         varying_domain,
                                         topology.parameter_tolerance) &&
              normalized_parameter_close(
                  varying_end, varying_domain[0], varying_domain, topology.parameter_tolerance)))
        {
          return fail(r_error, "The requested UV trim does not span a complete surface boundary.");
        }
        r_affected_face_ids.add(face.id);
        if (boundary.varying_u) {
          for (int u_index = 0; u_index < surface->control_vertex_count_u; u_index++) {
            translated_control_vertices.add(
                surface
                    ->control_vertices[u_index * surface->control_vertex_count_v +
                                       boundary.fixed_index]
                    .id);
          }
        }
        else {
          for (int v_index = 0; v_index < surface->control_vertex_count_v; v_index++) {
            translated_control_vertices.add(
                surface
                    ->control_vertices[boundary.fixed_index * surface->control_vertex_count_v +
                                       v_index]
                    .id);
          }
        }
      }
    }
  }
  if (!found_use || r_affected_face_ids.is_empty()) {
    return fail(r_error, "The requested curve support is not used by any trimmed face boundary.");
  }
  for (NurbsSurface &surface : topology.surfaces) {
    const int count = surface.control_vertex_count_u * surface.control_vertex_count_v;
    for (int i = 0; i < count; i++) {
      NurbsControlVertex &cv = surface.control_vertices[i];
      if (translated_control_vertices.contains(cv.id)) {
        cv.position[0] += delta.x;
        cv.position[1] += delta.y;
        cv.position[2] += delta.z;
      }
    }
  }
  return true;
}

bool reconcile_affected_edges(NurbsGeometry &staged,
                              const Set<uint64_t> &affected_face_ids,
                              std::string *r_error)
{
  NurbsTopology &topology = *staged.topology;
  Vector<uint64_t> edge_ids;
  Set<uint64_t> seen_edges;
  for (const NurbsTopoFace &face : topology.faces) {
    if (!affected_face_ids.contains(face.id)) {
      continue;
    }
    for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
      const NurbsTrimLoop &loop = face.loops[loop_index];
      for (int use_index = 0; use_index < loop.use_count; use_index++) {
        if (seen_edges.add(loop.uses[use_index].edge_id)) {
          edge_ids.append(loop.uses[use_index].edge_id);
        }
      }
    }
  }

  Vector<uint64_t> replacement_ids;
  Vector<OwnedCurve> replacements;
  for (const uint64_t edge_id : edge_ids) {
    const NurbsTopoEdge *edge = find_edge(topology, edge_id);
    NurbsCurve *source_curve = edge == nullptr ? nullptr :
                                                 find_curve(staged.curves, edge->curve_id);
    if (edge == nullptr || source_curve == nullptr) {
      return fail(r_error, "An affected face edge has a missing curve support.");
    }
    bool generated = false;
    for (const NurbsTopoFace &face : topology.faces) {
      if (!affected_face_ids.contains(face.id)) {
        continue;
      }
      const NurbsSurface *surface = find_surface(topology, face.surface_id);
      if (surface == nullptr) {
        return fail(r_error, "An affected face references a missing surface support.");
      }
      for (int loop_index = 0; loop_index < face.loop_count; loop_index++) {
        const NurbsTrimLoop &loop = face.loops[loop_index];
        for (int use_index = 0; use_index < loop.use_count; use_index++) {
          const NurbsEdgeUse &use = loop.uses[use_index];
          if (use.edge_id != edge_id) {
            continue;
          }
          const NurbsCurve *trim = find_trim(topology, use.trim_curve_id);
          OwnedCurve candidate;
          if (trim == nullptr ||
              !make_reconciled_curve(
                  *source_curve, *trim, *surface, topology.parameter_tolerance, candidate))
          {
            return fail(
                r_error,
                "An affected edge cannot be reconstructed from a linear isoparametric trim.");
          }

          int replacement_index = -1;
          for (int i = 0; i < replacement_ids.size(); i++) {
            if (replacement_ids[i] == source_curve->id) {
              replacement_index = i;
              break;
            }
          }
          if (replacement_index == -1) {
            replacement_ids.append(source_curve->id);
            replacements.append(std::move(candidate));
            replacement_index = int(replacements.size()) - 1;
          }
          else if (!curve_close(replacements[replacement_index].curve,
                                candidate.curve,
                                topology.tolerance,
                                topology.parameter_tolerance))
          {
            return fail(r_error,
                        "Incident surfaces produce inconsistent exact reconstructions for a "
                        "shared edge support.");
          }
          generated = true;
        }
      }
    }
    if (!generated) {
      return fail(r_error, "An affected edge has no incident trim use on an affected face.");
    }
  }

  for (int i = 0; i < replacement_ids.size(); i++) {
    NurbsCurve *destination = find_curve(staged.curves, replacement_ids[i]);
    const NurbsCurve &replacement = replacements[i].curve;
    if (destination == nullptr ||
        destination->control_vertex_count != replacement.control_vertex_count ||
        destination->knot_count != replacement.knot_count)
    {
      return fail(r_error, "Reconstructed edge support has an incompatible CV or knot count.");
    }
    destination->degree = replacement.degree;
    destination->order = replacement.order;
    destination->dimension = replacement.dimension;
    destination->rational = replacement.rational;
    destination->domain[0] = replacement.domain[0];
    destination->domain[1] = replacement.domain[1];
    std::copy_n(replacement.knots, replacement.knot_count, destination->knots);
    for (int cv_index = 0; cv_index < replacement.control_vertex_count; cv_index++) {
      const uint64_t cv_id = destination->control_vertices[cv_index].id;
      const eNurbsControlVertexFlag flag = destination->control_vertices[cv_index].flag;
      std::memcpy(destination->control_vertices[cv_index]._pad,
                  replacement.control_vertices[cv_index]._pad,
                  sizeof(destination->control_vertices[cv_index]._pad));
      destination->control_vertices[cv_index] = replacement.control_vertices[cv_index];
      destination->control_vertices[cv_index].id = cv_id;
      destination->control_vertices[cv_index].flag = flag;
    }
  }
  return true;
}

bool reconcile_vertices(NurbsTopology &topology,
                        const ListBaseT<NurbsCurve> &curves,
                        std::string *r_error)
{
  for (NurbsTopoVertex &vertex : topology.vertices) {
    bool have_position = false;
    double position[3] = {};
    for (const NurbsTopoEdge &edge : topology.edges) {
      const NurbsCurve *curve = find_curve(curves, edge.curve_id);
      if (curve == nullptr) {
        return fail(r_error, "A topology edge references a missing curve support.");
      }
      for (int endpoint = 0; endpoint < 2; endpoint++) {
        if (edge.vertex_ids[endpoint] != vertex.id) {
          continue;
        }
        double endpoint_position[3];
        if (!BKE_nurbs_curve_evaluate(*curve, edge.domain[endpoint], endpoint_position)) {
          return fail(r_error, "A topological vertex endpoint could not be evaluated.");
        }
        if (!have_position) {
          std::copy_n(endpoint_position, 3, position);
          have_position = true;
        }
        else if (std::hypot(std::hypot(endpoint_position[0] - position[0],
                                       endpoint_position[1] - position[1]),
                            endpoint_position[2] - position[2]) > topology.tolerance)
        {
          return fail(r_error, "Incident edge supports disagree at a shared topological vertex.");
        }
      }
    }
    if (have_position) {
      std::copy_n(position, 3, vertex.position);
    }
  }
  return true;
}

}  // namespace

bool BKE_nurbs_topology_translate_boundary(NurbsGeometry &geometry,
                                           const uint64_t support_curve_id,
                                           const double3 &delta,
                                           std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (geometry.topology == nullptr || !std::isfinite(delta.x) || !std::isfinite(delta.y) ||
      !std::isfinite(delta.z) || support_curve_id == 0)
  {
    return fail(r_error, "Boundary translation requires finite displacement and valid topology.");
  }
  if (!BKE_nurbs_topology_is_valid(geometry.curves, *geometry.topology, r_error)) {
    return false;
  }

  ListBaseT<NurbsCurve> staged_curves = {};
  BKE_nurbs_curve_list_copy(staged_curves, geometry.curves);
  NurbsTopology *staged_topology = BKE_nurbs_topology_copy(*geometry.topology);
  NurbsGeometry staged_geometry = {};
  staged_geometry.curves = staged_curves;
  staged_geometry.topology = staged_topology;

  Set<uint64_t> affected_face_ids;
  if (!translate_boundary_rows(
          staged_geometry, support_curve_id, delta, affected_face_ids, r_error) ||
      !reconcile_affected_edges(staged_geometry, affected_face_ids, r_error) ||
      !reconcile_vertices(*staged_topology, staged_curves, r_error) ||
      !BKE_nurbs_topology_is_valid(staged_curves, *staged_topology, r_error))
  {
    BKE_nurbs_curve_list_free(staged_curves);
    BKE_nurbs_topology_free(staged_topology);
    return false;
  }

  BKE_nurbs_curve_list_free(geometry.curves);
  BKE_nurbs_topology_free(geometry.topology);
  geometry.curves = staged_curves;
  geometry.topology = staged_topology;
  BKE_nurbs_batch_cache_dirty_tag(&geometry, BKE_NURBS_BATCH_DIRTY_ALL);
  return true;
}

}  // namespace blender
