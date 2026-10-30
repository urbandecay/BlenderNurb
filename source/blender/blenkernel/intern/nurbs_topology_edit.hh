/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <limits>

#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_selection.hh"
#include "BLI_listbase.h"
#include "MEM_guardedalloc.h"

namespace blender::bke::nurbs {
struct TopologyCutPlan;
struct TopologyEditResult;
}

namespace blender::bke::nurbs::detail {

/** Join compatible curved patch charts and their redundant boundary stations without fitting. */
std::optional<TopologyEditResult> dissolve_compatible_patches(const NurbsGeometry &source,
                                                              const SelectionState &selection,
                                                              double angle_limit,
                                                              std::string *r_error);

/** Transport explicitly supplied seed-curve parameters through the same source loop used by
 * the uniform editing tool. Rounded bevels use this to place equal-angle circle stations. */
std::optional<TopologyCutPlan> plan_loop_cut_parameters(const NurbsGeometry &source,
                                                        int64_t seed_edge,
                                                        Span<double> parameters,
                                                        double max_error,
                                                        std::string *r_error);

/** Construct shared native polynomial patches from the Catmull-Clark limit of a bevel cage. */
struct SubdivisionBevelFit {
  OwnedGeometry geometry;
  double sampled_deviation = 0;
  double coefficient_error = 0;
};
std::optional<SubdivisionBevelFit> subdivision_bevel_surfaces(const NurbsGeometry &source,
                                                              double max_error,
                                                              std::string *r_error);

/** Private candidate builder shared by topology mutation services. Allocation never wraps IDs.
 * A failed builder is discarded; callers must check valid() before validating/returning it.
 */
class TopologyBuilder {
 public:
  OwnedGeometry geometry;
  Map<int64_t, NurbsCurveGeometry *> curves;
  Map<int64_t, NurbsSurfaceGeometry *> surfaces;
  Map<int64_t, NurbsTopologyVertex *> vertices;
  Map<int64_t, NurbsTopologyEdge *> edges;
  Map<int64_t, NurbsTopologyFace *> faces;

  explicit TopologyBuilder(const NurbsGeometry &source) : geometry(geometry_copy(&source))
  {
    selection_storage_free(geometry->stored_selection);
    geometry->stored_selection = nullptr;
    for (auto &item : geometry->curves) {
      curves.add_new(item.id, &item);
    }
    for (auto &item : geometry->surfaces) {
      surfaces.add_new(item.id, &item);
    }
    for (auto &item : geometry->vertices) {
      vertices.add_new(item.id, &item);
    }
    for (auto &item : geometry->edges) {
      edges.add_new(item.id, &item);
    }
    for (auto &item : geometry->faces) {
      faces.add_new(item.id, &item);
    }
  }

  bool valid() const
  {
    return valid_;
  }
  int64_t id()
  {
    if (geometry->next_id == std::numeric_limits<int64_t>::max()) {
      valid_ = false;
      return 0;
    }
    return geometry->next_id++;
  }

  NurbsCurveGeometry *copy_curve(const int64_t source_id)
  {
    auto *copy = MEM_dupalloc(curves.lookup(source_id));
    copy->id = id();
    copy->points = MEM_dupalloc(copy->points);
    copy->knots = MEM_dupalloc(copy->knots);
    BLI_addtail(&geometry->curves, copy);
    if (copy->id) {
      curves.add_new(copy->id, copy);
    }
    return copy;
  }
  NurbsSurfaceGeometry *copy_surface(const int64_t source_id)
  {
    auto *copy = MEM_dupalloc(surfaces.lookup(source_id));
    copy->id = id();
    copy->points = MEM_dupalloc(copy->points);
    copy->knots_u = MEM_dupalloc(copy->knots_u);
    copy->knots_v = MEM_dupalloc(copy->knots_v);
    BLI_addtail(&geometry->surfaces, copy);
    if (copy->id) {
      surfaces.add_new(copy->id, copy);
    }
    return copy;
  }

 private:
  bool valid_ = true;
};

}  // namespace blender::bke::nurbs::detail
