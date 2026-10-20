/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_nurbs.hh"
#include "BKE_nurbs_curve.hh"
#include "BKE_nurbs_surface.hh"
#include "BKE_nurbs_topology.hh"
#include "BLI_listbase.h"

namespace blender::bke::tests {

/** Owned exact quarter-cylinder fixture shared by topology and native-data tests. */
struct NurbsTopologyFixture {
  ListBaseT<NurbsCurve> curves = {};
  NurbsTopology *topology = MEM_new<NurbsTopology>("NurbsTopologyFixture");
  /* Exercise IDs beyond float, 32-bit integer, and signed 64-bit ranges in every consumer. */
  uint64_t next_component_id = (uint64_t(1) << 63) + (uint64_t(1) << 40) + 1;

  uint64_t surface_a_id = 0;
  uint64_t surface_b_id = 0;
  uint64_t shared_edge_id = 0;
  uint64_t shared_edge_curve_id = 0;
  uint64_t face_a_id = 0;
  uint64_t face_b_id = 0;
  uint64_t hole_loop_id = 0;
  uint64_t hole_edge_ids[4] = {};
  uint64_t hole_surface_cv_ids[6] = {};
  uint64_t hole_trim_ids[4] = {};

  NurbsTopologyFixture()
  {
    build_surface(0.0, 1.0, 0.0, 1.0, 0.0, 1.0, surface_a_id);
    build_surface(2.0, 5.0, -3.0, 7.0, 1.0, 2.0, surface_b_id);

    NurbsSurface &surface_a = *static_cast<NurbsSurface *>(topology->surfaces.first);
    NurbsSurface &surface_b = *static_cast<NurbsSurface *>(topology->surfaces.last);

    const uint64_t a_bottom_curve = add_quarter_arc(0.0);
    const uint64_t seam_curve = add_quarter_arc(1.0);
    const uint64_t b_top_curve = add_quarter_arc(2.0);
    shared_edge_curve_id = seam_curve;

    const uint64_t a_left_curve = add_generator(surface_a, 0.0, 0.0, 1.0);
    const uint64_t a_right_curve = add_generator(surface_a, 1.0, 0.0, 1.0);
    const uint64_t b_left_curve = add_generator(surface_b, 2.0, -3.0, 7.0);
    const uint64_t b_right_curve = add_generator(surface_b, 5.0, -3.0, 7.0);

    const uint64_t vertex_a0 = add_vertex(surface_a, 0.0, 0.0);
    const uint64_t vertex_a1 = add_vertex(surface_a, 1.0, 0.0);
    const uint64_t vertex_s0 = add_vertex(surface_a, 0.0, 1.0);
    const uint64_t vertex_s1 = add_vertex(surface_a, 1.0, 1.0);
    const uint64_t vertex_b0 = add_vertex(surface_b, 2.0, 7.0);
    const uint64_t vertex_b1 = add_vertex(surface_b, 5.0, 7.0);

    const uint64_t a_bottom = add_edge(vertex_a0, vertex_a1, a_bottom_curve, 0.0, 1.0);
    const uint64_t a_right = add_edge(vertex_a1, vertex_s1, a_right_curve, 0.0, 1.0);
    shared_edge_id = add_edge(vertex_s0, vertex_s1, seam_curve, 0.0, 1.0);
    find_edge(shared_edge_id)->flag = NURBS_TOPO_SELECT;
    const uint64_t a_left = add_edge(vertex_a0, vertex_s0, a_left_curve, 0.0, 1.0);
    const uint64_t b_right = add_edge(vertex_s1, vertex_b1, b_right_curve, -3.0, 7.0);
    const uint64_t b_top = add_edge(vertex_b0, vertex_b1, b_top_curve, 0.0, 1.0);
    const uint64_t b_left = add_edge(vertex_s0, vertex_b0, b_left_curve, -3.0, 7.0);

    const uint64_t a_bottom_trim = add_uv_line(0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    const uint64_t a_right_trim = add_uv_line(1.0, 0.0, 1.0, 1.0, 0.0, 1.0);
    const uint64_t a_seam_trim = add_uv_line(0.0, 1.0, 1.0, 1.0, 0.0, 1.0);
    const uint64_t a_left_trim = add_uv_line(0.0, 0.0, 0.0, 1.0, 0.0, 1.0);

    const uint64_t b_seam_trim = add_uv_line(2.0, -3.0, 5.0, -3.0, 0.0, 1.0);
    const uint64_t b_right_trim = add_uv_line(5.0, -3.0, 5.0, 7.0, -3.0, 7.0);
    const uint64_t b_top_trim = add_uv_line(2.0, 7.0, 5.0, 7.0, 0.0, 1.0);
    const uint64_t b_left_trim = add_uv_line(2.0, -3.0, 2.0, 7.0, -3.0, 7.0);

    face_a_id = add_face(surface_a_id);
    NurbsTopoFace &face_a = *find_face(face_a_id);
    add_loop(face_a,
             0,
             {{a_bottom, a_bottom_trim, false},
              {a_right, a_right_trim, false},
              {shared_edge_id, a_seam_trim, true},
              {a_left, a_left_trim, true}});

    build_hole(surface_a, face_a);

    face_b_id = add_face(surface_b_id);
    NurbsTopoFace &face_b = *find_face(face_b_id);
    add_loop(face_b,
             0,
             {{shared_edge_id, b_seam_trim, false},
              {b_right, b_right_trim, false},
              {b_top, b_top_trim, true},
              {b_left, b_left_trim, true}});
  }

  ~NurbsTopologyFixture()
  {
    BKE_nurbs_curve_list_free(curves);
    BKE_nurbs_topology_free(topology);
  }

  uint64_t add_closed_circle_wire(const double center_x)
  {
    NurbsCurve *curve = new_curve(3, 2, 9, 0.0, 4.0, true);
    constexpr double middle_weight = 0.7071067811865475244;
    const double positions[9][3] = {{1, 0, 3},
                                    {1, 1, 3},
                                    {0, 1, 3},
                                    {-1, 1, 3},
                                    {-1, 0, 3},
                                    {-1, -1, 3},
                                    {0, -1, 3},
                                    {1, -1, 3},
                                    {1, 0, 3}};
    const double weights[9] = {
        1, middle_weight, 1, middle_weight, 1, middle_weight, 1, middle_weight, 1};
    const double knots[10] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4};
    std::copy(knots, knots + 10, curve->knots);
    for (int i = 0; i < 9; i++) {
      std::copy(positions[i], positions[i] + 3, curve->control_vertices[i].position);
      curve->control_vertices[i].position[0] += center_x;
      curve->control_vertices[i].weight = weights[i];
    }
    BLI_addtail(&curves, curve);
    const double vertex_position[3] = {center_x + 1.0, 0.0, 3.0};
    const uint64_t vertex_id = add_vertex_from_position(vertex_position);
    return add_edge(vertex_id, vertex_id, curve->id, 0.0, 4.0);
  }

  NurbsTopologyFixture(const NurbsTopologyFixture &) = delete;
  NurbsTopologyFixture &operator=(const NurbsTopologyFixture &) = delete;

 private:
  uint64_t allocate_id()
  {
    return next_component_id++;
  }

  NurbsCurve *new_curve(const int dimension,
                        const int degree,
                        const int cv_count,
                        const double domain_start,
                        const double domain_end,
                        const bool rational)
  {
    NurbsCurve *curve = MEM_new<NurbsCurve>(__func__);
    curve->id = allocate_id();
    curve->dimension = dimension;
    curve->degree = degree;
    curve->order = degree + 1;
    curve->control_vertex_count = cv_count;
    curve->knot_count = cv_count + curve->order - 2;
    curve->rational = rational;
    curve->domain[0] = domain_start;
    curve->domain[1] = domain_end;
    curve->control_vertices = MEM_new_array<NurbsControlVertex>(cv_count, __func__);
    curve->knots = MEM_new_array<double>(curve->knot_count, __func__);
    for (int i = 0; i < cv_count; i++) {
      curve->control_vertices[i].id = allocate_id();
    }
    return curve;
  }

  NurbsSurface *build_surface(const double u0,
                              const double u1,
                              const double v0,
                              const double v1,
                              const double z0,
                              const double z1,
                              uint64_t &r_surface_id)
  {
    constexpr double middle_weight = 0.7071067811865475244;
    constexpr double xy[3][2] = {{1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
    constexpr double weights[3] = {1.0, middle_weight, 1.0};
    NurbsSurface *surface = MEM_new<NurbsSurface>(__func__);
    surface->id = allocate_id();
    r_surface_id = surface->id;
    surface->degree_u = 2;
    surface->order_u = 3;
    surface->degree_v = 1;
    surface->order_v = 2;
    surface->control_vertex_count_u = 3;
    surface->control_vertex_count_v = 2;
    surface->knot_count_u = 4;
    surface->knot_count_v = 2;
    surface->rational = 1;
    surface->domain_u[0] = u0;
    surface->domain_u[1] = u1;
    surface->domain_v[0] = v0;
    surface->domain_v[1] = v1;
    surface->control_vertices = MEM_new_array<NurbsControlVertex>(6, __func__);
    surface->knots_u = MEM_new_array<double>(4, __func__);
    surface->knots_v = MEM_new_array<double>(2, __func__);
    const double knots_u[4] = {u0, u0, u1, u1};
    const double knots_v[2] = {v0, v1};
    std::copy(knots_u, knots_u + 4, surface->knots_u);
    std::copy(knots_v, knots_v + 2, surface->knots_v);
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 2; j++) {
        NurbsControlVertex &cv = surface->control_vertices[i * 2 + j];
        cv.id = allocate_id();
        cv.position[0] = xy[i][0];
        cv.position[1] = xy[i][1];
        cv.position[2] = j == 0 ? z0 : z1;
        cv.weight = weights[i];
      }
    }
    BLI_addtail(&topology->surfaces, surface);
    return surface;
  }

  NurbsCurve *new_quarter_arc(const double z)
  {
    constexpr double middle_weight = 0.7071067811865475244;
    NurbsCurve *curve = new_curve(3, 2, 3, 0.0, 1.0, true);
    const double knots[4] = {0.0, 0.0, 1.0, 1.0};
    const double positions[3][3] = {{1.0, 0.0, z}, {1.0, 1.0, z}, {0.0, 1.0, z}};
    std::copy(knots, knots + 4, curve->knots);
    for (int i = 0; i < 3; i++) {
      std::copy(positions[i], positions[i] + 3, curve->control_vertices[i].position);
      curve->control_vertices[i].weight = i == 1 ? middle_weight : 1.0;
    }
    return curve;
  }

  uint64_t add_quarter_arc(const double z)
  {
    NurbsCurve *curve = new_quarter_arc(z);
    BLI_addtail(&curves, curve);
    return curve->id;
  }

  NurbsCurve *new_arc_segment(const double z, const double start, const double end)
  {
    NurbsCurve *source = new_quarter_arc(z);
    NurbsCurve left, tail, before, segment;
    if (!BKE_nurbs_curve_split(*source, end, left, tail) ||
        !BKE_nurbs_curve_split(left, start, before, segment))
    {
      if (left.control_vertices != nullptr) {
        MEM_delete(left.control_vertices);
        MEM_delete(left.knots);
      }
      if (tail.control_vertices != nullptr) {
        MEM_delete(tail.control_vertices);
        MEM_delete(tail.knots);
      }
      if (before.control_vertices != nullptr) {
        MEM_delete(before.control_vertices);
        MEM_delete(before.knots);
      }
      if (segment.control_vertices != nullptr) {
        MEM_delete(segment.control_vertices);
        MEM_delete(segment.knots);
      }
      MEM_delete(source->control_vertices);
      MEM_delete(source->knots);
      MEM_delete(source);
      return nullptr;
    }
    MEM_delete(left.control_vertices);
    MEM_delete(left.knots);
    MEM_delete(tail.control_vertices);
    MEM_delete(tail.knots);
    MEM_delete(before.control_vertices);
    MEM_delete(before.knots);
    MEM_delete(source->control_vertices);
    MEM_delete(source->knots);
    MEM_delete(source);
    segment.id = allocate_id();
    for (int i = 0; i < segment.control_vertex_count; i++) {
      segment.control_vertices[i].id = allocate_id();
    }
    return MEM_new<NurbsCurve>(__func__, segment);
  }

  uint64_t add_generator(const NurbsSurface &surface,
                         const double u,
                         const double v0,
                         const double v1)
  {
    double bottom[3], top[3];
    BKE_nurbs_surface_evaluate(surface, u, v0, bottom);
    BKE_nurbs_surface_evaluate(surface, u, v1, top);
    NurbsCurve *curve = new_curve(3, 1, 2, v0, v1, false);
    curve->knots[0] = v0;
    curve->knots[1] = v1;
    std::copy(bottom, bottom + 3, curve->control_vertices[0].position);
    std::copy(top, top + 3, curve->control_vertices[1].position);
    BLI_addtail(&curves, curve);
    return curve->id;
  }

  uint64_t add_uv_line(const double u0,
                       const double v0,
                       const double u1,
                       const double v1,
                       const double t0,
                       const double t1)
  {
    NurbsCurve *curve = new_curve(2, 1, 2, t0, t1, false);
    curve->knots[0] = t0;
    curve->knots[1] = t1;
    curve->control_vertices[0].position[0] = u0;
    curve->control_vertices[0].position[1] = v0;
    curve->control_vertices[0].position[2] = 0.0;
    curve->control_vertices[1].position[0] = u1;
    curve->control_vertices[1].position[1] = v1;
    curve->control_vertices[1].position[2] = 0.0;
    BLI_addtail(&topology->trim_curves, curve);
    return curve->id;
  }

  uint64_t add_uv_arc(const double u0, const double u1, const double v)
  {
    /* The exact 3D arc is carried by the edge support; this exact degree-one p-curve maps its
     * increasing edge parameter to the cylinder's U parameter. */
    return add_uv_line(u0, v, u1, v, u0, u1);
  }

  uint64_t add_hole_arc_edge(const double v,
                             const uint64_t start_vertex,
                             const uint64_t end_vertex)
  {
    NurbsCurve *curve = new_arc_segment(v, 0.25, 0.75);
    if (curve == nullptr) {
      return 0;
    }
    BLI_addtail(&curves, curve);
    const uint64_t edge_id = add_edge(start_vertex, end_vertex, curve->id, 0.25, 0.75);
    const uint64_t trim_id = add_uv_arc(0.25, 0.75, v);
    hole_trim_ids[v == 0.75 ? 0 : 2] = trim_id;
    return edge_id;
  }

  uint64_t add_hole_generator_edge(const NurbsSurface &surface,
                                   const double u,
                                   const double v0,
                                   const double v1,
                                   const int side,
                                   uint64_t &r_start_vertex,
                                   uint64_t &r_end_vertex)
  {
    double start[3], end[3];
    BKE_nurbs_surface_evaluate(surface, u, v0, start);
    BKE_nurbs_surface_evaluate(surface, u, v1, end);
    NurbsCurve *curve = new_curve(3, 1, 2, v0, v1, false);
    curve->knots[0] = v0;
    curve->knots[1] = v1;
    std::copy(start, start + 3, curve->control_vertices[0].position);
    std::copy(end, end + 3, curve->control_vertices[1].position);
    BLI_addtail(&curves, curve);
    r_start_vertex = add_vertex_from_position(start);
    r_end_vertex = add_vertex_from_position(end);
    const uint64_t edge_id = add_edge(r_start_vertex, r_end_vertex, curve->id, v0, v1);
    const uint64_t trim_id = add_uv_line(u, v0, u, v1, v0, v1);
    hole_trim_ids[side == 0 ? 3 : 1] = trim_id;
    return edge_id;
  }

  uint64_t add_vertex(const NurbsSurface &surface, const double u, const double v)
  {
    double position[3];
    BKE_nurbs_surface_evaluate(surface, u, v, position);
    return add_vertex_from_position(position);
  }

  uint64_t add_vertex_from_position(const double position[3])
  {
    NurbsTopoVertex *vertex = MEM_new<NurbsTopoVertex>(__func__);
    vertex->id = allocate_id();
    std::copy(position, position + 3, vertex->position);
    BLI_addtail(&topology->vertices, vertex);
    return vertex->id;
  }

  uint64_t add_edge(const uint64_t v0,
                    const uint64_t v1,
                    const uint64_t curve_id,
                    const double t0,
                    const double t1)
  {
    NurbsTopoEdge *edge = MEM_new<NurbsTopoEdge>(__func__);
    edge->id = allocate_id();
    edge->vertex_ids[0] = v0;
    edge->vertex_ids[1] = v1;
    edge->curve_id = curve_id;
    edge->domain[0] = t0;
    edge->domain[1] = t1;
    BLI_addtail(&topology->edges, edge);
    return edge->id;
  }

  uint64_t add_face(const uint64_t surface_id)
  {
    NurbsTopoFace *face = MEM_new<NurbsTopoFace>(__func__);
    face->id = allocate_id();
    face->surface_id = surface_id;
    BLI_addtail(&topology->faces, face);
    return face->id;
  }

  struct UseSpec {
    uint64_t edge_id;
    uint64_t trim_id;
    bool reversed;
  };

  void add_loop(NurbsTopoFace &face, const int role, const std::initializer_list<UseSpec> uses)
  {
    const int index = face.loop_count++;
    if (face.loops == nullptr) {
      face.loops = MEM_new_array<NurbsTrimLoop>(1, __func__);
    }
    else {
      NurbsTrimLoop *new_loops = MEM_new_array<NurbsTrimLoop>(face.loop_count, __func__);
      std::copy_n(face.loops, index, new_loops);
      MEM_delete(face.loops);
      face.loops = new_loops;
    }
    NurbsTrimLoop &loop = face.loops[index];
    loop.id = allocate_id();
    loop.role = role;
    loop.use_count = int(uses.size());
    loop.uses = MEM_new_array<NurbsEdgeUse>(loop.use_count, __func__);
    int use_index = 0;
    for (const UseSpec &spec : uses) {
      NurbsEdgeUse &use = loop.uses[use_index++];
      use.id = allocate_id();
      use.edge_id = spec.edge_id;
      use.trim_curve_id = spec.trim_id;
      use.reversed = spec.reversed;
    }
  }

  NurbsTopoFace *find_face(const uint64_t id)
  {
    for (NurbsTopoFace &face : topology->faces) {
      if (face.id == id) {
        return &face;
      }
    }
    return nullptr;
  }

  NurbsTopoEdge *find_edge(const uint64_t id)
  {
    for (NurbsTopoEdge &edge : topology->edges) {
      if (edge.id == id) {
        return &edge;
      }
    }
    return nullptr;
  }

  void build_hole(const NurbsSurface &surface, NurbsTopoFace &face)
  {
    uint64_t left_bottom_vertex, left_top_vertex;
    uint64_t right_bottom_vertex, right_top_vertex;
    const uint64_t left = add_hole_generator_edge(
        surface, 0.25, 0.25, 0.75, 0, left_bottom_vertex, left_top_vertex);
    const uint64_t right = add_hole_generator_edge(
        surface, 0.75, 0.25, 0.75, 1, right_bottom_vertex, right_top_vertex);
    const uint64_t top = add_hole_arc_edge(0.75, left_top_vertex, right_top_vertex);
    const uint64_t bottom = add_hole_arc_edge(0.25, left_bottom_vertex, right_bottom_vertex);
    hole_edge_ids[0] = left;
    hole_edge_ids[1] = top;
    hole_edge_ids[2] = right;
    hole_edge_ids[3] = bottom;
    hole_loop_id = allocate_id();

    const int index = face.loop_count++;
    NurbsTrimLoop *new_loops = MEM_new_array<NurbsTrimLoop>(face.loop_count, __func__);
    std::copy_n(face.loops, index, new_loops);
    MEM_delete(face.loops);
    face.loops = new_loops;
    NurbsTrimLoop &loop = face.loops[index];
    loop.id = hole_loop_id;
    loop.role = 1;
    loop.use_count = 4;
    loop.uses = MEM_new_array<NurbsEdgeUse>(4, __func__);
    const UseSpec uses[4] = {{left, hole_trim_ids[3], false},
                             {top, hole_trim_ids[0], false},
                             {right, hole_trim_ids[1], true},
                             {bottom, hole_trim_ids[2], true}};
    for (int i = 0; i < 4; i++) {
      loop.uses[i].id = allocate_id();
      loop.uses[i].edge_id = uses[i].edge_id;
      loop.uses[i].trim_curve_id = uses[i].trim_id;
      loop.uses[i].reversed = uses[i].reversed;
    }
    for (int i = 0; i < 6; i++) {
      hole_surface_cv_ids[i] = surface.control_vertices[i].id;
    }
  }
};

}  // namespace blender::bke::tests
