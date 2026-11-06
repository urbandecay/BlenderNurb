/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
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

enum class CurveKind : uint8_t { Line, Conic, Cubic, RationalQuadratic };

/** Line: origin + u*t. Conic: origin + u*cos(t) + v*sin(t).
 * Independent axes retain exact ellipse/shear geometry under affine object transforms.
 * Circle and ellipse are analytic entities, with no control net, knots, weights or fitting. */
struct Curve {
  Id id = 0;
  CurveKind kind = CurveKind::Line;
  Vec3 origin, u, v;
  /** Cubic: origin + u*t + v*t*t + w*t*t*t.
   * RationalQuadratic: origin/u/v are three points and w holds their weights. */
  Vec3 w;
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

enum class SurfaceKind : uint8_t { Plane, Extrusion, Sphere, Cylinder, Blend, Ruled };

/** Plane: origin + u*s + v*t. Extrusion: curve(start + (end-start)*s) + u*t. */
struct Surface {
  Id id = 0;
  SurfaceKind kind = SurfaceKind::Plane;
  Vec3 origin, u, v;
  Id curve = 0;
  double start = 0, end = 1;
  /** Sphere: x*x+y*y+z*z=1. Cylinder: x*x+y*y=1.
   * Both use the affine frame origin + u*x + v*y + w*z. */
  Vec3 w;
  /** Analytic conic/Hermite corner: conic(theta) +
   * (cos(theta) - blend_cot*sin(theta))*polynomial(v). */
  std::array<Vec3, 4> blend{};
  double blend_cot = 0, blend_v_start = 0, blend_v_end = 1;
  /** Ruled surface between curve and other_curve; start/end trim the profile parameter. */
  Id other_curve = 0;
  bool reverse_first = false, reverse_second = false;
};

struct Face {
  Id id = 0, surface = 0;
  std::vector<Coedge> outer;
  std::vector<std::vector<Coedge>> holes;
  int orientation = 1;
  /** First physical face of one logical surface patch; zero means this face alone. */
  Id group = 0;
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
Vec3 evaluate_blend(const Surface &surface, double theta, double v);
Vec3 blend_theta_tangent(const Surface &surface, double theta, double v);
Vec3 blend_v_direction(const Surface &surface, double v);
Vec3 evaluate_ruled(
    const Surface &surface, const Curve &first, const Curve &last, double u, double v);
Vec3 ruled_normal(
    const Surface &surface, const Curve &first, const Curve &last, double u, double v);
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
/** Copy selected topology with independent identities and shared supports within the copy. */
std::optional<EditResult> duplicate_elements(const Model &model,
                                             Selection selection,
                                             std::string *error = nullptr);
std::optional<EditResult> merge_vertices(const Model &model,
                                         Selection selection,
                                         MergeMode mode,
                                         Vec3 cursor = {},
                                         double threshold = 1e-4,
                                         std::string *error = nullptr);
/** Exact planar bevel by half-space construction, including multi-edge corner intersections. */
enum class BevelCornerStyle { Sharp, Patch };
/** Fillet selected edges, faces, or selected convex vertices with one to 32 subdivisions.
 * Vertex-only fillets currently support corners bounded by three planar faces. */
std::optional<EditResult> fillet(const Model &model,
                                 Selection selection,
                                 double width,
                                 int subdivisions,
                                 std::string *error = nullptr,
                                 bool vertices_only = false,
                                 std::string *warning = nullptr);
std::optional<EditResult> chamfer(const Model &model,
                                  Selection selection,
                                  double width,
                                  bool vertices_only = false,
                                  std::string *error = nullptr,
                                  BevelCornerStyle corner_style = BevelCornerStyle::Sharp);
enum class DeleteMode { Vertices, Edges, Faces, OnlyFaces };
std::optional<EditResult> delete_elements(const Model &model,
                                          Selection selection,
                                          DeleteMode mode,
                                          std::string *error = nullptr);
/** Rebuild exact planes for straight boundaries after component edits; split warped polygons. */
bool refit_linear_faces(Model &model, std::string *error = nullptr);
bool validate(const Model &model, std::string *error = nullptr);
bool validate_body(const Model &model, Id seed_face, std::string *error = nullptr);

/** Validation certificate for an immutable source snapshot. The owner must not retain
 * mutable aliases. A certificate cannot be reused for another model pointer. */
class ValidatedModel {
  std::shared_ptr<const Model> model_;
  explicit ValidatedModel(std::shared_ptr<const Model> model) : model_(std::move(model)) {}
  static ValidatedModel from_validated_extrusion(std::shared_ptr<const Model> model)
  {
    assert(model);
    return ValidatedModel(std::move(model));
  }
  friend class PreparedExtrusion;
 public:
  static std::optional<ValidatedModel> create(std::shared_ptr<const Model> model,
                                             std::string *error = nullptr);
  const Model &model() const { return *model_; }
  bool matches(const std::shared_ptr<const Model> &model) const { return model == model_; }
};

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
  bool has_analytic_normals = true;
  std::vector<Vec3> positions;
  std::vector<std::array<int, 2>> edges;
  std::vector<std::vector<int>> polygons;
  std::vector<Id> vertex_source, edge_source, face_source;
  std::vector<std::vector<Vec3>> corner_normals;
};
/** Derived display only: planar faces have boundary polygons, conic walls have strips and
 * corner blends have sampled patches. Display samples never become source geometry. */
std::optional<DisplayMesh> tessellate(const Model &model,
                                      int circle_segments = 64,
                                      std::string *error = nullptr,
                                      bool calculate_normals = true);

/** Tessellate an already checked immutable snapshot without repeating validation. */
std::optional<DisplayMesh> tessellate(const ValidatedModel &model,
                                      int circle_segments = 64,
                                      std::string *error = nullptr,
                                      bool calculate_normals = true);

/** Versioned, bounded, endian-independent source storage. Display data is never serialized. */
std::vector<uint8_t> encode(const Model &model, const Selection &selection);
std::optional<EditResult> decode(const uint8_t *data, size_t size, std::string *error = nullptr);

}  // namespace blender::solid
