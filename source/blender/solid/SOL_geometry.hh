/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/* Independent analytic solid kernel. This interface has no dependency on Blender's curve,
 * surface, mesh, edit-data or evaluation implementations. Host adapters own those concerns. */
namespace blender::solid {

using Id = uint64_t;

struct Vec3 {
  double x = 0, y = 0, z = 0;
  double &operator[](int i)
  {
    assert(i >= 0 && i < 3);
    return i == 0 ? x : i == 1 ? y : z;
  }
  double operator[](int i) const
  {
    assert(i >= 0 && i < 3);
    return i == 0 ? x : i == 1 ? y : z;
  }
  friend bool operator==(const Vec3 &, const Vec3 &) = default;
};
Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator*(Vec3 a, double b);
Vec3 operator/(Vec3 a, double b);
double dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
double length(Vec3 a);

enum class CurveKind : uint8_t { Line, Conic };

/** Line: origin + u*t. Conic: origin + u*cos(t) + v*sin(t).
 * Independent axes retain exact ellipse/shear geometry under affine object transforms.
 * Circle and ellipse are analytic entities, with no control net, knots, weights or fitting. */
struct Curve {
  Id id = 0;
  CurveKind kind = CurveKind::Line;
  Vec3 origin, u, v;
};

struct Vertex {
  Id id = 0;
  Vec3 position;
};

struct Edge {
  Id id = 0, curve = 0;
  std::array<Id, 2> vertices{};
  double start = 0, end = 1;
};

struct Coedge {
  Id edge = 0;
  bool reversed = false;
};

enum class SurfaceKind : uint8_t { Plane, Extrusion };

/** Plane: origin + u*s + v*t. Extrusion: curve(start + (end-start)*s) + u*t. */
struct Surface {
  Id id = 0;
  SurfaceKind kind = SurfaceKind::Plane;
  Vec3 origin, u, v;
  Id curve = 0;
  double start = 0, end = 1;
};

struct Face {
  Id id = 0, surface = 0;
  std::vector<Coedge> outer;
  std::vector<std::vector<Coedge>> holes;
  int orientation = 1;
};

struct Model {
  Id next_id = 1;
  std::vector<Vertex> vertices;
  std::vector<Curve> curves;
  std::vector<Edge> edges;
  std::vector<Surface> surfaces;
  std::vector<Face> faces;
};

struct Selection {
  std::vector<Id> vertices, edges, faces;
  int mode = 1;
  Id active = 0;
};

Selection normalize_selection(const Model &model, Selection selection);

Vec3 evaluate(const Curve &curve, double parameter);
Vec3 tangent(const Curve &curve, double parameter);
const Vertex *vertex(const Model &model, Id id);
const Curve *curve(const Model &model, Id id);
const Edge *edge(const Model &model, Id id);
const Surface *surface(const Model &model, Id id);
const Face *face(const Model &model, Id id);

std::optional<Curve> line(Vec3 start, Vec3 end, std::string *error = nullptr);
std::optional<Curve> circle(Vec3 center, Vec3 start, Vec3 normal, std::string *error = nullptr);
std::optional<Curve> circle_through(Vec3 a, Vec3 through, Vec3 b, std::string *error = nullptr);
std::optional<Curve> ellipse(Vec3 center, Vec3 u, Vec3 v, std::string *error = nullptr);

/** Append an analytic interval, sharing explicitly supplied endpoint identities. Zero means
 * allocate a new endpoint. A closed interval may reuse its own first endpoint. */
bool append_curve(Model &model,
                  Curve curve,
                  double start,
                  double end,
                  Id start_vertex,
                  Id end_vertex,
                  bool closed,
                  Selection *result,
                  std::string *error = nullptr);

struct Profile {
  Model model;
  Id face = 0;
  Vec3 normal;
  bool attached = false;
};
std::optional<Profile> prepare_profile(const Model &source,
                                       const Selection &selection,
                                       std::string *error = nullptr);

struct EditResult {
  Model model;
  Selection selection;
};
std::optional<EditResult> extrude(const Profile &profile,
                                  double distance,
                                  std::string *error = nullptr);
/** Sweep selected source vertices into edges, or selected source edges into analytic sheets. */
std::optional<EditResult> extrude_elements(const Model &model,
                                           Selection selection,
                                           Vec3 offset,
                                           std::string *error = nullptr);
enum class MergeMode { Center, First, Last, Cursor, Distance, Collapse };
std::optional<EditResult> merge_vertices(const Model &model,
                                         Selection selection,
                                         MergeMode mode,
                                         Vec3 cursor = {},
                                         double threshold = 1e-4,
                                         std::string *error = nullptr);
/** Exact planar bevel by half-space construction, including multi-edge corner intersections. */
std::optional<EditResult> chamfer(const Model &model,
                                  Selection selection,
                                  double width,
                                  bool vertices_only = false,
                                  std::string *error = nullptr);
enum class DeleteMode { Vertices, Edges, Faces, OnlyFaces };
std::optional<EditResult> delete_elements(const Model &model,
                                          Selection selection,
                                          DeleteMode mode,
                                          std::string *error = nullptr);
/** Rebuild exact planes for straight boundaries after component edits; split warped polygons. */
bool refit_linear_faces(Model &model, std::string *error = nullptr);
bool validate(const Model &model, std::string *error = nullptr);
bool validate_body(const Model &model, Id seed_face, std::string *error = nullptr);

struct Affine {
  std::array<Vec3, 3> columns{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  Vec3 translation;
};
/** Exact affine component edits. Straight boundaries refit planes and split warped polygons;
 * partial conic and curved-surface deformation needs an explicit construction rule. */
std::optional<EditResult> transform_selected(const Model &model,
                                             Selection selection,
                                             const Affine &transform,
                                             std::string *error = nullptr);

struct DisplayMesh {
  std::vector<Vec3> positions;
  std::vector<std::array<int, 2>> edges;
  std::vector<std::vector<int>> polygons;
  std::vector<Id> vertex_source, edge_source, face_source;
  std::vector<std::vector<Vec3>> corner_normals;
};
/** Derived display only: planar faces have boundary polygons and conic walls have strips.
 * No surface grids, curve fitting, source reconstruction or per-frame source serialization. */
std::optional<DisplayMesh> tessellate(const Model &model,
                                      int circle_segments = 64,
                                      std::string *error = nullptr);

/** Versioned, bounded, endian-independent source storage. Display data is never serialized. */
std::vector<uint8_t> encode(const Model &model, const Selection &selection);
std::optional<EditResult> decode(const uint8_t *data, size_t size, std::string *error = nullptr);

}  // namespace blender::solid
