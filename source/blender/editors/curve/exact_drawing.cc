/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "exact_drawing.hh"

#include "BKE_context.hh"
#include "BKE_nurbs_edit.hh"
#include "BKE_nurbs_tessellate.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "DNA_object_types.h"
#include "ED_nurbs.hh"
#include "ED_view3d.hh"
#include <algorithm>
#include <cfloat>

namespace blender::ed::exact_drawing {
const Vertex *Index::vertex(int64_t id) const
{
  for (const auto &v : geometry->vertices)
    if (v.id == id)
      return &v;
  return nullptr;
}
const Edge *Index::edge(int64_t id) const
{
  for (const auto &e : geometry->edges)
    if (e.id == id)
      return &e;
  return nullptr;
}
std::optional<Point> evaluate_curve(const Curve &curve, double t, std::string *)
{
  return curve.sample(t);
}
Source::~Source() = default;
std::unique_ptr<Source> Source::begin(bContext *C, std::string *error)
{
  auto result = std::make_unique<Source>();
  auto &view = result->source_;
  if (solid_editor::edit_poll(C)) {
    result->analytic_ = solid_editor::Session::begin(C, error);
    if (!result->analytic_)
      return {};
    const auto &snapshot = result->analytic_->source();
    for (const auto &v : snapshot.model->vertices)
      view.geometry_.vertices.append({int64_t(v.id), {v.position.x, v.position.y, v.position.z}});
    for (const auto &e : snapshot.model->edges)
      view.geometry_.edges.append({int64_t(e.id),
                                   int64_t(e.curve),
                                   {int64_t(e.vertices[0]), int64_t(e.vertices[1])},
                                   {std::min(e.start, e.end), std::max(e.start, e.end)}});
    for (const auto &c : snapshot.model->curves) {
      view.geometry_.curves.append({int64_t(c.id), [c](double t) -> std::optional<Point> {
                                      const auto p = solid::evaluate(c, t);
                                      return Point{{p.x, p.y, p.z}};
                                    }});
    }
    for (solid::Id id : snapshot.selection.edges)
      view.selection_.edges.add(int64_t(id));
  }
  else {
    result->legacy_ = nurbs::TopologyPreview::begin(C, error);
    if (!result->legacy_)
      return {};
    const auto &snapshot = result->legacy_->source();
    for (const auto &v : snapshot.geometry().vertices)
      view.geometry_.vertices.append({v.id, double3(v.position)});
    for (const auto &e : snapshot.geometry().edges)
      view.geometry_.edges.append({e.id,
                                   e.curve_id,
                                   {e.vertex_ids[0], e.vertex_ids[1]},
                                   {e.parameter_range[0], e.parameter_range[1]}});
    for (const auto &c : snapshot.geometry().curves) {
      view.geometry_.curves.append({c.id, [curve = &c](double t) -> std::optional<Point> {
                                      const auto p = bke::nurbs::evaluate_curve(*curve, t);
                                      return p ? std::optional<Point>(Point{p->position}) :
                                                 std::nullopt;
                                    }});
    }
    view.selection_.edges = snapshot.selection().edges;
  }
  return result;
}
Object &Source::object() const
{
  return analytic_ ? analytic_->object() : legacy_->object();
}
const NurbsGeometry *Source::legacy_geometry() const
{
  return legacy_ ? &legacy_->source().geometry() : nullptr;
}
std::optional<int64_t> Source::edge_under_cursor(bContext *C, const int mval[2]) const
{
  if (legacy_)
    return nurbs::topology_edge_under_cursor(C, *legacy_, mval, nullptr);
  float best = 12;
  std::optional<int64_t> hit;
  const double4x4 transform(object().object_to_world());
  ARegion *region = CTX_wm_region(C);
  for (const Edge &edge : source_.geometry_.edges) {
    const Curve *curve = nullptr;
    for (const Curve &c : source_.geometry_.curves)
      if (c.id == edge.curve_id) {
        curve = &c;
        break;
      }
    if (!curve)
      continue;
    float2 previous;
    bool previous_valid = false;
    for (int i = 0; i <= 64; ++i) {
      const auto p = evaluate_curve(*curve,
                                    edge.parameter_range[0] +
                                        (edge.parameter_range[1] - edge.parameter_range[0]) * i /
                                            64.0);
      float2 screen;
      const bool valid = p && ED_view3d_project_float_global(
                                  region,
                                  float3(math::transform_point(transform, p->position)),
                                  screen,
                                  V3D_PROJ_TEST_NOP) == V3D_PROJ_RET_OK;
      if (valid && previous_valid) {
        const float2 delta = screen - previous;
        const float t = math::length_squared(delta) > 0 ?
                            std::clamp(math::dot(float2(mval) - previous, delta) /
                                           math::length_squared(delta),
                                       0.0f,
                                       1.0f) :
                            0;
        const float distance = math::distance(float2(mval), previous + delta * t);
        if (distance < best) {
          best = distance;
          hit = edge.id;
        }
      }
      previous = screen;
      previous_valid = valid;
    }
  }
  return hit;
}
}  // namespace blender::ed::exact_drawing
