/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "ED_solid.hh"
#include <functional>

namespace blender {
struct bContext;
struct Object;
struct NurbsGeometry;
namespace ed::nurbs {
class TopologyPreview;
}
}  // namespace blender

namespace blender::ed::exact_drawing {
struct Point {
  double3 position;
};
struct Curve {
  int64_t id = 0;
  std::function<std::optional<Point>(double)> sample;
};
struct Vertex {
  int64_t id = 0;
  double3 position;
};
struct Edge {
  int64_t id = 0, curve_id = 0;
  int64_t vertex_ids[2]{};
  double parameter_range[2]{};
};
struct Geometry {
  Vector<Vertex> vertices;
  Vector<Edge> edges;
  Vector<Curve> curves;
};
struct Selection {
  Set<int64_t> edges;
};
struct Snapshot {
  Geometry geometry_;
  Selection selection_;
  const Geometry &geometry() const
  {
    return geometry_;
  }
  const Selection &selection() const
  {
    return selection_;
  }
};
struct Index {
  const Geometry *geometry;
  static std::optional<Index> build(const Geometry &geometry)
  {
    return Index{&geometry};
  }
  const Vertex *vertex(int64_t id) const;
  const Edge *edge(int64_t id) const;
};
std::optional<Point> evaluate_curve(const Curve &curve, double t, std::string *error = nullptr);

/** Shared gesture code reads native source entities through this small view. Each backend
 * retains its own immutable source; analytic objects never construct a NURBS snapshot. */
class Source {
  std::unique_ptr<nurbs::TopologyPreview> legacy_;
  std::unique_ptr<solid_editor::Session> analytic_;
  Snapshot source_;

 public:
  ~Source();
  static std::unique_ptr<Source> begin(bContext *C, std::string *error = nullptr);
  Object &object() const;
  bool analytic() const
  {
    return bool(analytic_);
  }
  const Snapshot &source() const
  {
    return source_;
  }
  const NurbsGeometry *legacy_geometry() const;
  std::optional<int64_t> edge_under_cursor(bContext *C, const int mval[2]) const;
};
}  // namespace blender::ed::exact_drawing
