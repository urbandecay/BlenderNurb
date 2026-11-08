/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"
#include "id_index.hh"
#include "extrusion.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

namespace blender::solid {

constexpr double pi = std::numbers::pi;
constexpr double tau = 2 * pi;

Vec3 operator+(Vec3 a, Vec3 b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator-(Vec3 a, Vec3 b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 operator*(Vec3 a, double b)
{
  return {a.x * b, a.y * b, a.z * b};
}
Vec3 operator/(Vec3 a, double b)
{
  return a * (1 / b);
}
double dot(Vec3 a, Vec3 b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(Vec3 a)
{
  return std::hypot(a.x, a.y, a.z);
}

namespace {
void fail(std::string *error, const char *message)
{
  if (error) {
    *error = message;
  }
}
bool finite(Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool near(Vec3 a, Vec3 b)
{
  if (!finite(a) || !finite(b))
    return false;
  return length(a - b) <=
         128 * std::numeric_limits<double>::epsilon() * std::max({1.0, length(a), length(b)});
}
std::array<double, 2> sin_cos(double angle)
{
  angle = std::remainder(angle, tau);
  if (angle == 0) {
    return {0, 1};
  }
  if (angle == pi / 2) {
    return {1, 0};
  }
  if (angle == -pi / 2) {
    return {-1, 0};
  }
  if (std::abs(angle) == pi) {
    return {0, -1};
  }
  return {std::sin(angle), std::cos(angle)};
}
template<typename T> const T *find(const std::vector<T> &items, Id id)
{
  const auto it = std::find_if(items.begin(), items.end(), [=](const T &v) { return v.id == id; });
  return it == items.end() ? nullptr : &*it;
}
struct Index {
  IdIndex<Vertex> vertices;
  IdIndex<Curve> curves;
  IdIndex<Edge> edges;
  IdIndex<Surface> surfaces;
  IdIndex<Face> faces;
  explicit Index(const Model &m, bool include_geometry = true)
      : vertices(m.vertices),
        curves(include_geometry ? IdIndex<Curve>(m.curves) : IdIndex<Curve>()),
        edges(m.edges),
        surfaces(include_geometry ? IdIndex<Surface>(m.surfaces) : IdIndex<Surface>()),
        faces(m.faces)
  {
  }
};
Id begin_vertex(const Edge &e, const Coedge &use)
{
  return e.vertices[use.reversed ? 1 : 0];
}
Id end_vertex(const Edge &e, const Coedge &use)
{
  return e.vertices[use.reversed ? 0 : 1];
}
/* Iterate the outer boundary and holes without allocating a temporary pointer vector. */
struct FaceLoops {
  const Face &face;
  struct Iterator {
    const Face &face;
    size_t index;
    const std::vector<Coedge> *operator*() const
    {
      return index == 0 ? &face.outer : &face.holes[index - 1];
    }
    Iterator &operator++()
    {
      ++index;
      return *this;
    }
    bool operator!=(const Iterator &other) const
    {
      return index != other.index;
    }
  };
  Iterator begin() const { return {face, 0}; }
  Iterator end() const { return {face, face.holes.size() + 1}; }
};
FaceLoops loops(const Face &face)
{
  return {face};
}
Vec3 area_integral(const Curve &c, double a, double b)
{
  if (c.kind == CurveKind::RationalQuadratic) {
    /* Quadrature is only used to determine planar profile area and orientation.
     * The source curve and extrusion retain their exact rational representation. */
    constexpr double nodes[] = {.09501250983763744,
                                .2816035507792589,
                                .4580167776572274,
                                .6178762444026438,
                                .7554044083550030,
                                .8656312023878318,
                                .9445750230732326,
                                .9894009349916499};
    constexpr double weights[] = {.1894506104550685,
                                  .1826034150449236,
                                  .1691565193950025,
                                  .1495959888165767,
                                  .1246289712555339,
                                  .09515851168249278,
                                  .06225352393864789,
                                  .02715245941175409};
    Vec3 integral;
    for (int i = 0; i < 8; ++i) {
      for (const double sign : {-1.0, 1.0}) {
        const double t = (a + b) * .5 + sign * nodes[i] * (b - a) * .5;
        integral = integral + cross(evaluate(c, t), tangent(c, t)) * weights[i];
      }
    }
    return integral * ((b - a) * .5);
  }
  if (c.kind == CurveKind::Cubic) {
    const std::array<Vec3, 4> coefficients{c.origin, c.u, c.v, c.w};
    Vec3 integral;
    for (int i = 0; i < 4; ++i)
      for (int j = 1; j < 4; ++j)
        integral = integral +
                   cross(coefficients[i], coefficients[j]) *
                       (double(j) * (std::pow(b, i + j) - std::pow(a, i + j)) / (i + j));
    return integral;
  }
  if (c.kind == CurveKind::Line) {
    return cross(evaluate(c, a), evaluate(c, b));
  }
  const auto x = sin_cos(a), y = sin_cos(b);
  return cross(c.origin, c.u * (y[1] - x[1]) + c.v * (y[0] - x[0])) + cross(c.u, c.v) * (b - a);
}
}  // namespace

const Vertex *vertex(const Model &m, Id id)
{
  return find(m.vertices, id);
}
const Curve *curve(const Model &m, Id id)
{
  return find(m.curves, id);
}
const Edge *edge(const Model &m, Id id)
{
  return find(m.edges, id);
}
const Surface *surface(const Model &m, Id id)
{
  return find(m.surfaces, id);
}
const Face *face(const Model &m, Id id)
{
  return find(m.faces, id);
}

Vec3 evaluate(const Curve &c, double t)
{
  if (c.kind == CurveKind::RationalQuadratic) {
    const double a = (1 - t) * (1 - t) * c.w.x, b = 2 * t * (1 - t) * c.w.y, d = t * t * c.w.z;
    return (c.origin * a + c.u * b + c.v * d) / (a + b + d);
  }
  if (c.kind == CurveKind::Cubic)
    return c.origin + (c.u + (c.v + c.w * t) * t) * t;
  if (c.kind == CurveKind::Line)
    return c.origin + c.u * t;
  const auto sc = sin_cos(t);
  return c.origin + c.u * sc[1] + c.v * sc[0];
}
Vec3 tangent(const Curve &c, double t)
{
  if (c.kind == CurveKind::RationalQuadratic) {
    const double a = (1 - t) * (1 - t) * c.w.x, b = 2 * t * (1 - t) * c.w.y, d = t * t * c.w.z;
    const double da = -2 * (1 - t) * c.w.x, db = (2 - 4 * t) * c.w.y, dd = 2 * t * c.w.z;
    return (c.origin * da + c.u * db + c.v * dd - evaluate(c, t) * (da + db + dd)) / (a + b + d);
  }
  if (c.kind == CurveKind::Cubic)
    return c.u + c.v * (2 * t) + c.w * (3 * t * t);
  if (c.kind == CurveKind::Line)
    return c.u;
  const auto sc = sin_cos(t);
  return c.v * sc[1] - c.u * sc[0];
}

Vec3 evaluate_blend(const Surface &s, double theta, double v)
{
  const Vec3 p = s.blend[0] + (s.blend[1] + (s.blend[2] + s.blend[3] * v) * v) * v;
  return s.origin + s.u * std::cos(theta) + s.v * std::sin(theta) +
         p * (std::cos(theta) - s.blend_cot * std::sin(theta));
}
Vec3 blend_theta_tangent(const Surface &s, double theta, double v)
{
  const Vec3 p = s.blend[0] + (s.blend[1] + (s.blend[2] + s.blend[3] * v) * v) * v;
  return s.v * std::cos(theta) - s.u * std::sin(theta) -
         p * (std::sin(theta) + s.blend_cot * std::cos(theta));
}
Vec3 blend_v_direction(const Surface &s, double v)
{
  return s.blend[1] + s.blend[2] * (2 * v) + s.blend[3] * (3 * v * v);
}

Vec3 evaluate_cone(const Surface &s, const Curve &base, const double theta, const double v)
{
  return evaluate(base, theta) * (1 - v) + s.origin * v;
}

Vec3 cone_normal(const Surface &s, const Curve &base, const double theta)
{
  return cross(tangent(base, theta), s.origin - evaluate(base, theta));
}

Vec3 evaluate_ruled(const Surface &s, const Curve &a, const Curve &b, double u, double v)
{
  return evaluate(a, s.reverse_first ? 1 - u : u) * (1 - v) +
         evaluate(b, s.reverse_second ? 1 - u : u) * v;
}
Vec3 ruled_normal(const Surface &s, const Curve &a, const Curve &b, double u, double v)
{
  const Vec3 first = evaluate(a, s.reverse_first ? 1 - u : u),
             last = evaluate(b, s.reverse_second ? 1 - u : u);
  const Vec3 du = tangent(a, s.reverse_first ? 1 - u : u) *
                      ((s.reverse_first ? -1 : 1) * (1 - v)) +
                  tangent(b, s.reverse_second ? 1 - u : u) * ((s.reverse_second ? -1 : 1) * v);
  return cross(du, last - first);
}

std::optional<Curve> line(Vec3 a, Vec3 b, std::string *error)
{
  if (!finite(a) || !finite(b) || a == b || !finite(b - a)) {
    fail(error, "A line needs two finite, distinct endpoints");
    return {};
  }
  return Curve{0, CurveKind::Line, a, b - a, {}};
}

std::optional<Curve> ellipse(Vec3 center, Vec3 u, Vec3 v, std::string *error)
{
  if (!finite(center) || !finite(u) || !finite(v) || !(length(cross(u, v)) > 0) ||
      !std::isfinite(length(cross(u, v))))
  {
    fail(error, "A conic needs a finite center and two independent axes");
    return {};
  }
  return Curve{0, CurveKind::Conic, center, u, v};
}

std::optional<Curve> circle(Vec3 center, Vec3 start, Vec3 normal, std::string *error)
{
  const Vec3 radial = start - center;
  const double radius = length(radial), nlen = length(normal);
  if (!(radius > 0) || !(nlen > 0) || !std::isfinite(radius) || !std::isfinite(nlen)) {
    fail(error, "A circle needs a nonzero radius and plane normal");
    return {};
  }
  normal = normal / nlen;
  const Vec3 axis = radial / radius;
  if (std::abs(dot(axis, normal)) > 1e-6) {
    fail(error, "The circle radius must lie in its plane");
    return {};
  }
  normal = normal - axis * dot(axis, normal);
  normal = normal / length(normal);
  return ellipse(center, radial, cross(normal, radial), error);
}

std::optional<Curve> circle_through(Vec3 a, Vec3 through, Vec3 b, std::string *error)
{
  const Vec3 u = through - a, v = b - a, n = cross(u, v);
  const double nn = dot(n, n);
  if (!(nn > 0) || !std::isfinite(nn)) {
    fail(error, "Three distinct noncollinear points are required");
    return {};
  }
  const Vec3 center = a + (cross(v, n) * dot(u, u) + cross(n, u) * dot(v, v)) / (2 * nn);
  return circle(center, a, n, error);
}

bool append_curve(Model &model,
                  Curve c,
                  double a,
                  double b,
                  Id va,
                  Id vb,
                  bool closed,
                  Selection *result,
                  std::string *error)
{
  if (!std::isfinite(a) || !std::isfinite(b) || a == b || !finite(evaluate(c, a)) ||
      !finite(evaluate(c, b)) || model.next_id > std::numeric_limits<Id>::max() - 4)
  {
    fail(error, "Invalid analytic curve interval or exhausted source identities");
    return false;
  }
  const Vec3 start = evaluate(c, a), end = evaluate(c, b);
  const auto *first = va ? vertex(model, va) : nullptr;
  const auto *last = vb ? vertex(model, vb) : nullptr;
  if ((va && (!first || !near(first->position, start))) ||
      (vb && (!last || !near(last->position, end))) || (closed && !near(start, end)))
  {
    fail(error, "Snapped source endpoints do not agree with the analytic curve");
    return false;
  }
  if (!va) {
    va = model.next_id++;
    model.vertices.push_back({va, start});
  }
  if (closed)
    vb = va;
  if (!vb) {
    vb = model.next_id++;
    model.vertices.push_back({vb, end});
  }
  c.id = model.next_id++;
  const Id edge_id = model.next_id++;
  model.curves.push_back(c);
  model.edges.push_back({edge_id, c.id, {va, vb}, a, b});
  if (result) {
    if (std::find(result->vertices.begin(), result->vertices.end(), va) == result->vertices.end())
      result->vertices.push_back(va);
    if (std::find(result->vertices.begin(), result->vertices.end(), vb) == result->vertices.end())
      result->vertices.push_back(vb);
    result->edges.push_back(edge_id);
    result->active = edge_id;
  }
  return true;
}

static bool validate_impl(const Model &m, std::string *error, const Model *trusted_source = nullptr)
{
  if (m.next_id == 0 || (trusted_source && m.next_id < trusted_source->next_id)) {
    fail(error, "Invalid source identity counter");
    return false;
  }
  if (trusted_source &&
      (m.vertices.size() < trusted_source->vertices.size() ||
       m.curves.size() < trusted_source->curves.size() ||
       m.edges.size() < trusted_source->edges.size() ||
       m.surfaces.size() < trusted_source->surfaces.size() ||
       trusted_source->faces.empty() ||
       m.faces.size() < trusted_source->faces.size() - 1))
  {
    fail(error, "Extrusion result lost trusted source geometry");
    return false;
  }
  Index index(m);
  const size_t item_count = m.vertices.size() + m.curves.size() + m.edges.size() +
                            m.surfaces.size() + m.faces.size();
  const bool compact_ids = !trusted_source && m.next_id / 8 < item_count;
  std::vector<uint8_t> used_ids(compact_ids ? size_t(m.next_id) : 0, 0);
  std::unordered_set<Id> sparse_ids;
  if (trusted_source) {
    const size_t trusted_count = trusted_source->vertices.size() + trusted_source->curves.size() +
                                 trusted_source->edges.size() + trusted_source->surfaces.size() +
                                 trusted_source->faces.size();
    if (item_count < trusted_count) {
      fail(error, "Extrusion result lost trusted source geometry");
      return false;
    }
    sparse_ids.reserve(item_count - trusted_count);
  }
  else if (!compact_ids) {
    sparse_ids.reserve(item_count);
  }
  auto identity = [&](Id id) {
    /* IDs below the validated source counter belong to the trusted, immutable input. The
     * extrusion allocator gives every new entity a monotonically increasing ID after it. */
    if (trusted_source && id && id < trusted_source->next_id)
      return true;
    if (!id || id >= m.next_id)
      return false;
    if (trusted_source)
      return sparse_ids.insert(id).second;
    if (!compact_ids)
      return sparse_ids.insert(id).second;
    const bool fresh = !used_ids[id];
    used_ids[id] = 1;
    return fresh;
  };
  struct BoundarySamples {
    bool ready = false;
    std::array<Vec3, 5> points;
  };
  std::vector<BoundarySamples> boundary_samples;
  for (const auto &v : m.vertices) {
    if (trusted_source && v.id && v.id < trusted_source->next_id)
      continue;
    if (!identity(v.id) || !finite(v.position))
      goto invalid;
  }
  for (const auto &c : m.curves) {
    if (trusted_source && c.id && c.id < trusted_source->next_id)
      continue;
    if (!identity(c.id) || !finite(c.origin) || !finite(c.u) || !finite(c.v) || !finite(c.w) ||
        (c.kind != CurveKind::Line && c.kind != CurveKind::Conic && c.kind != CurveKind::Cubic &&
         c.kind != CurveKind::RationalQuadratic) ||
        (c.kind != CurveKind::RationalQuadratic && !(length(c.u) > 0)) ||
        (c.kind == CurveKind::RationalQuadratic &&
         (!(c.w.x > 0) || !(c.w.y > 0) || !(c.w.z > 0) || c.origin == c.v)) ||
        !std::isfinite(length(c.u)) ||
        (c.kind == CurveKind::Conic &&
         (!(length(cross(c.u, c.v)) > 0) || !std::isfinite(length(cross(c.u, c.v))))))
      goto invalid;
  }
  for (const auto &e : m.edges) {
    if (trusted_source && e.id && e.id < trusted_source->next_id)
      continue;
    if (!identity(e.id) || !index.curves.contains(e.curve) ||
        !index.vertices.contains(e.vertices[0]) || !index.vertices.contains(e.vertices[1]) ||
        !std::isfinite(e.start) || !std::isfinite(e.end) || !std::isfinite(e.end - e.start) ||
        e.start == e.end)
      goto invalid;
    const auto &c = *index.curves.at(e.curve);
    if (c.kind == CurveKind::Conic && std::abs(e.end - e.start) > 64 * pi)
      goto invalid;
    if (!near(evaluate(c, e.start), index.vertices.at(e.vertices[0])->position) ||
        !near(evaluate(c, e.end), index.vertices.at(e.vertices[1])->position))
      goto invalid;
  }
  for (const auto &s : m.surfaces) {
    if (trusted_source && s.id && s.id < trusted_source->next_id)
      continue;
    if (!identity(s.id) || !finite(s.origin) || !finite(s.u) || !finite(s.v) || !finite(s.w) ||
        (s.kind != SurfaceKind::Plane && s.kind != SurfaceKind::Extrusion &&
         s.kind != SurfaceKind::Sphere && s.kind != SurfaceKind::Cylinder &&
         s.kind != SurfaceKind::Blend && s.kind != SurfaceKind::Ruled &&
         s.kind != SurfaceKind::Cone) ||
        (s.kind == SurfaceKind::Ruled &&
         (!index.curves.contains(s.curve) || !index.curves.contains(s.other_curve) ||
          !std::isfinite(s.start) || !std::isfinite(s.end) || s.start == s.end)) ||
        (s.kind == SurfaceKind::Cone &&
         (!index.curves.contains(s.curve) || !std::isfinite(s.start) ||
          !std::isfinite(s.end) || s.start == s.end)) ||
        ((s.kind == SurfaceKind::Sphere || s.kind == SurfaceKind::Cylinder) &&
         !(std::abs(dot(s.u, cross(s.v, s.w))) > 0)) ||
        (s.kind == SurfaceKind::Plane &&
         (!(length(cross(s.u, s.v)) > 0) || !std::isfinite(length(cross(s.u, s.v))))) ||
        (s.kind == SurfaceKind::Extrusion &&
         (!index.curves.contains(s.curve) || !(length(s.u) > 0) || !std::isfinite(length(s.u)) ||
          !std::isfinite(s.start) || !std::isfinite(s.end) || s.start == s.end)))
      goto invalid;
  }
  for (const auto &f : m.faces) {
    if (trusted_source && f.id && f.id < trusted_source->next_id) {
      if (!identity(f.id) || (f.orientation != 1 && f.orientation != -1))
        goto invalid;
      continue;
    }
    if (!identity(f.id) || !index.surfaces.contains(f.surface) ||
        (f.orientation != 1 && f.orientation != -1) ||
        (f.group && (!index.faces.contains(f.group) || index.faces.at(f.group)->group != f.group)))
      goto invalid;
    for (const auto *loop : loops(f)) {
      if (loop->empty())
        goto invalid;
      Id first = 0, previous = 0;
      for (const Coedge &use : *loop) {
        if (!index.edges.contains(use.edge))
          goto invalid;
        const auto &e = *index.edges.at(use.edge);
        const Id start = begin_vertex(e, use);
        if (previous && previous != start)
          goto invalid;
        if (!first)
          first = start;
        previous = end_vertex(e, use);
      }
      if (first != previous)
        goto invalid;
    }
    const Surface &support = *index.surfaces.at(f.surface);
    if (support.kind == SurfaceKind::Plane) {
      const Vec3 n = cross(support.u, support.v) / length(cross(support.u, support.v));
      for (const auto *loop : loops(f))
        for (const Coedge &use : *loop) {
          const Curve &c = *index.curves.at(index.edges.at(use.edge)->curve);
          const double tolerance =
              256 * std::numeric_limits<double>::epsilon() *
              std::max({1.0, length(support.origin), length(c.origin), length(c.u), length(c.v)});
          if (c.kind == CurveKind::RationalQuadratic) {
            if (std::abs(dot(c.origin - support.origin, n)) > tolerance ||
                std::abs(dot(c.u - support.origin, n)) > tolerance ||
                std::abs(dot(c.v - support.origin, n)) > tolerance)
              goto invalid;
            continue;
          }
          if (std::abs(dot(c.origin - support.origin, n)) > tolerance ||
              std::abs(dot(c.u, n)) > tolerance ||
              (c.kind != CurveKind::Line && std::abs(dot(c.v, n)) > tolerance) ||
              (c.kind == CurveKind::Cubic && std::abs(dot(c.w, n)) > tolerance))
            goto invalid;
        }
    }
    else if (support.kind == SurfaceKind::Ruled) {
      if (f.outer.size() != 4 || !f.holes.empty())
        goto invalid;
      const Curve &a = *index.curves.at(support.curve), &b = *index.curves.at(support.other_curve);
      for (int i = 0; i < 4; ++i) {
        const Coedge &use = f.outer[i];
        const Edge &e = *index.edges.at(use.edge);
        for (double t : {0.0, .25, .5, .75, 1.0}) {
          const double u = support.start + (support.end - support.start) * (i == 0 ? t :
                                                                            i == 2 ? 1 - t :
                                                                            i == 1 ? 1 :
                                                                                     0);
          const double v = i == 0 ? 0 : i == 1 ? t : i == 2 ? 1 : 1 - t;
          const Vec3 actual = evaluate(*index.curves.at(e.curve),
                                       e.start + (e.end - e.start) * (use.reversed ? 1 - t : t));
          if (length(actual - evaluate_ruled(support, a, b, u, v)) >
              1e-8 * std::max(1.0, length(actual)))
            goto invalid;
        }
      }
    }
    else if (support.kind == SurfaceKind::Cone) {
      if (f.outer.size() != 3 || !f.holes.empty())
        goto invalid;
      const Curve &base = *index.curves.at(support.curve);
      if (base.kind != CurveKind::Conic || index.edges.at(f.outer[0].edge)->curve != base.id)
        goto invalid;
      for (int i = 0; i < 3; ++i) {
        const Coedge &use = f.outer[i];
        const Edge &e = *index.edges.at(use.edge);
        for (double t : {0.0, .25, .5, .75, 1.0}) {
          double theta, v;
          if (i == 0) {
            theta = support.start + (support.end - support.start) * t;
            v = 0;
          }
          else if (i == 1) {
            theta = support.end;
            v = t;
          }
          else {
            theta = support.start;
            v = 1 - t;
          }
          const Vec3 actual = evaluate(*index.curves.at(e.curve),
                                       e.start + (e.end - e.start) * (use.reversed ? 1 - t : t));
          if (length(actual - evaluate_cone(support, base, theta, v)) >
              1e-8 * std::max(1.0, length(actual)))
            goto invalid;
        }
      }
    }
    else if (support.kind == SurfaceKind::Blend) {
      if ((f.outer.size() != 3 && f.outer.size() != 4) || !f.holes.empty() ||
          !std::isfinite(support.blend_cot) || !std::isfinite(support.start) ||
          !std::isfinite(support.end) || support.start >= support.end ||
          !std::isfinite(support.blend_v_start) || !std::isfinite(support.blend_v_end) ||
          support.blend_v_start >= support.blend_v_end)
        goto invalid;
      for (Vec3 p : support.blend)
        if (!finite(p))
          goto invalid;
      const bool collapsed = f.outer.size() == 3;
      for (size_t i = 0; i < f.outer.size(); ++i) {
        const Coedge &use = f.outer[i];
        const Edge &e = *index.edges.at(use.edge);
        for (double fraction : {0.0, .25, .5, .75, 1.0}) {
          double theta, v;
          if (i == 0) {
            theta = support.start + (support.end - support.start) * fraction;
            v = support.blend_v_start;
          }
          else if (i == f.outer.size() - 1) {
            theta = support.start;
            v = support.blend_v_end + (support.blend_v_start - support.blend_v_end) * fraction;
          }
          else if (!collapsed && i == 1) {
            theta = support.end;
            v = support.blend_v_start + (support.blend_v_end - support.blend_v_start) * fraction;
          }
          else {
            theta = support.end + (support.start - support.end) * fraction;
            v = support.blend_v_end;
          }
          const Vec3 point = evaluate(*index.curves.at(e.curve),
                                      e.start + (e.end - e.start) *
                                                    (use.reversed ? 1 - fraction : fraction));
          if (length(point - evaluate_blend(support, theta, v)) >
              1e-8 * std::max(1.0, length(point)))
            goto invalid;
        }
      }
    }
    else if (support.kind == SurfaceKind::Sphere || support.kind == SurfaceKind::Cylinder) {
      const bool cylinder = support.kind == SurfaceKind::Cylinder;
      if (f.outer.size() != (cylinder ? 4 : 3) || !f.holes.empty())
        goto invalid;
      if (cylinder) {
        const Edge &a = *index.edges.at(f.outer[0].edge), &b = *index.edges.at(f.outer[2].edge);
        if (index.curves.at(a.curve)->kind != CurveKind::Conic ||
            index.curves.at(b.curve)->kind != CurveKind::Conic)
          goto invalid;
      }
      const Vec3 vw = cross(support.v, support.w), wu = cross(support.w, support.u),
                 uv = cross(support.u, support.v);
      const double det = dot(support.u, vw);
      if (cylinder) {
        auto local = [&](Vec3 p) {
          p = p - support.origin;
          return Vec3{dot(p, vw) / det, dot(p, wu) / det, dot(p, uv) / det};
        };
        const Edge &a = *index.edges.at(f.outer[0].edge), &b = *index.edges.at(f.outer[2].edge);
        for (double fraction : {0.0, .25, .5, .75, 1.0}) {
          const double ta = a.start +
                            (a.end - a.start) * (f.outer[0].reversed ? 1 - fraction : fraction);
          const double tb = b.start +
                            (b.end - b.start) * (f.outer[2].reversed ? fraction : 1 - fraction);
          const Vec3 pa = local(evaluate(*index.curves.at(a.curve), ta)),
                     pb = local(evaluate(*index.curves.at(b.curve), tb));
          if (std::abs(pa.x - pb.x) > 1e-8 || std::abs(pa.y - pb.y) > 1e-8 || pb.z <= pa.z)
            goto invalid;
        }
      }
      for (const Coedge &use : f.outer) {
        const Edge &e = *index.edges.at(use.edge);
        const Curve &c = *index.curves.at(e.curve);
        if (!cylinder && c.kind != CurveKind::Conic && c.kind != CurveKind::RationalQuadratic)
          goto invalid;
        /* Adjacent corner patches share edges. Reuse exact curve evaluations,
         * but still check every sample against each face's own support frame. */
        if (boundary_samples.empty())
          boundary_samples.resize(m.edges.size());
        auto &samples = boundary_samples[size_t(&e - m.edges.data())];
        if (!samples.ready) {
          for (size_t i = 0; i < samples.points.size(); ++i)
            samples.points[i] = evaluate(c, e.start + (e.end - e.start) * (double(i) * .25));
          samples.ready = true;
        }
        for (Vec3 sample : samples.points) {
          const Vec3 p = sample - support.origin;
          const Vec3 q{dot(p, vw) / det, dot(p, wu) / det, dot(p, uv) / det};
          if (std::abs(q.x * q.x + q.y * q.y + (cylinder ? 0 : q.z * q.z) - 1) > 1e-8)
            goto invalid;
        }
      }
    }
    else {
      if (f.outer.size() != 4 || !f.holes.empty() || f.outer[0].reversed || !f.outer[2].reversed)
        goto invalid;
      const Edge &bottom = *index.edges.at(f.outer[0].edge),
                 &top = *index.edges.at(f.outer[2].edge);
      const Curve &a = *index.curves.at(bottom.curve), &b = *index.curves.at(top.curve);
      auto parameter_agrees = [](double x, double y) {
        return std::abs(x - y) <= 128 * std::numeric_limits<double>::epsilon() *
                                      std::max({1.0, std::abs(x), std::abs(y)});
      };
      const Vec3 control_shift = a.kind == CurveKind::RationalQuadratic ? support.u : Vec3{};
      if (bottom.curve != support.curve || a.kind != b.kind ||
          !near(a.origin + support.u, b.origin) || !near(a.u + control_shift, b.u) ||
          !near(a.v + control_shift, b.v) || !near(a.w, b.w) ||
          !parameter_agrees(bottom.start, support.start) ||
          !parameter_agrees(bottom.end, support.end) ||
          !parameter_agrees(bottom.start, top.start) || !parameter_agrees(bottom.end, top.end))
        goto invalid;
      for (size_t rail : {size_t(1), size_t(3)}) {
        const Edge &e = *index.edges.at(f.outer[rail].edge);
        if (index.curves.at(e.curve)->kind != CurveKind::Line)
          goto invalid;
      }
    }
  }
  return true;
invalid:
  fail(error, "Invalid analytic geometry, identity, boundary or source reference");
  return false;
}

bool validate(const Model &m, std::string *error)
{
  return validate_impl(m, error);
}

static bool validate_extrusion_delta(const Profile &profile,
                                    const Model &result,
                                    std::string *error)
{
  const Model &source = profile.model;
  /* The fast check is valid only for an exact extrusion copy: all source entities remain the
   * original prefix except for deleting/reorienting the selected profile face. Verify that
   * assumption before trusting source validation for the unchanged geometry. */
  if (result.vertices.size() < source.vertices.size() || result.curves.size() < source.curves.size() ||
      result.edges.size() < source.edges.size() || result.surfaces.size() < source.surfaces.size())
  {
    fail(error, "Extrusion result lost trusted source geometry");
    return false;
  }
  for (size_t i = 0; i < source.vertices.size(); ++i)
    if (result.vertices[i].id != source.vertices[i].id ||
        result.vertices[i].position != source.vertices[i].position)
      goto invalid_source;
  for (size_t i = source.vertices.size(); i < result.vertices.size(); ++i)
    if (result.vertices[i].id < source.next_id)
      goto invalid_source;
  for (size_t i = 0; i < source.curves.size(); ++i) {
    const Curve &a = source.curves[i], &b = result.curves[i];
    if (a.id != b.id || a.kind != b.kind || a.origin != b.origin || a.u != b.u || a.v != b.v ||
        a.w != b.w)
      goto invalid_source;
  }
  for (size_t i = source.curves.size(); i < result.curves.size(); ++i)
    if (result.curves[i].id < source.next_id)
      goto invalid_source;
  for (size_t i = 0; i < source.edges.size(); ++i) {
    const Edge &a = source.edges[i], &b = result.edges[i];
    if (a.id != b.id || a.curve != b.curve || a.vertices != b.vertices || a.start != b.start ||
        a.end != b.end)
      goto invalid_source;
  }
  for (size_t i = source.edges.size(); i < result.edges.size(); ++i)
    if (result.edges[i].id < source.next_id)
      goto invalid_source;
  for (size_t i = 0; i < source.surfaces.size(); ++i) {
    const Surface &a = source.surfaces[i], &b = result.surfaces[i];
    if (a.id != b.id || a.kind != b.kind || a.origin != b.origin || a.u != b.u || a.v != b.v ||
        a.curve != b.curve || a.start != b.start || a.end != b.end || a.w != b.w ||
        a.blend != b.blend || a.blend_cot != b.blend_cot ||
        a.blend_v_start != b.blend_v_start || a.blend_v_end != b.blend_v_end ||
        a.other_curve != b.other_curve || a.reverse_first != b.reverse_first ||
        a.reverse_second != b.reverse_second)
      goto invalid_source;
  }
  for (size_t i = source.surfaces.size(); i < result.surfaces.size(); ++i)
    if (result.surfaces[i].id < source.next_id)
      goto invalid_source;
  {
    const IdIndex<Face> source_faces(source.faces);
    std::unordered_set<Id> seen_source_faces;
    seen_source_faces.reserve(source.faces.size());
    auto same_loop = [](const std::vector<Coedge> &a, const std::vector<Coedge> &b) {
      if (a.size() != b.size())
        return false;
      for (size_t i = 0; i < a.size(); ++i)
        if (a[i].edge != b[i].edge || a[i].reversed != b[i].reversed)
          return false;
      return true;
    };
    auto same_face_except_orientation = [&](const Face &a, const Face &b) {
      if (a.id != b.id || a.surface != b.surface || a.group != b.group ||
          !same_loop(a.outer, b.outer) || a.holes.size() != b.holes.size())
        return false;
      for (size_t i = 0; i < a.holes.size(); ++i)
        if (!same_loop(a.holes[i], b.holes[i]))
          return false;
      return true;
    };
    for (const Face &f : result.faces) {
      if (source_faces.contains(f.id)) {
        const Face &original = *source_faces.at(f.id);
        if ((f.id == profile.face && profile.attached) ||
            !same_face_except_orientation(original, f) ||
            (f.id != profile.face && f.orientation != original.orientation) ||
            (f.orientation != 1 && f.orientation != -1) || !seen_source_faces.insert(f.id).second)
          goto invalid_source;
      }
      else if (f.id < source.next_id) {
        goto invalid_source;
      }
    }
    for (const Face &f : source.faces)
      if (seen_source_faces.contains(f.id) == (f.id == profile.face && profile.attached))
        goto invalid_source;
  }
  return validate_impl(result, error, &source);

invalid_source:
  fail(error, "Extrusion changed trusted source geometry outside its profile face");
  return false;
}

std::optional<ValidatedModel> ValidatedModel::create(std::shared_ptr<const Model> model,
                                                      std::string *error)
{
  if (!model || !validate(*model, error))
    return {};
  return ValidatedModel(std::move(model));
}

std::optional<Profile> prepare_profile(const Model &source,
                                       const Selection &selected,
                                       std::string *error,
                                       const bool allow_boundary_cap)
{
  if (!validate(source, error))
    return {};
  Profile out;
  out.model = source;
  const Face *existing = nullptr;
  if (!selected.faces.empty()) {
    if (selected.faces.size() != 1 || !(existing = face(source, selected.faces[0])) ||
        surface(source, existing->surface)->kind != SurfaceKind::Plane)
    {
      fail(error, "Select one planar face or a closed sketch profile");
      return {};
    }
    out.face = existing->id;
    const auto &s = *surface(source, existing->surface);
    out.normal = cross(s.u, s.v);
    out.normal = out.normal / length(out.normal) * existing->orientation;
    int incidence = 0;
    for (const auto &f : source.faces) {
      for (const auto *loop : loops(f)) {
        for (const auto &use : *loop) {
          if (use.edge == existing->outer[0].edge)
            incidence++;
        }
      }
    }
    out.attached = incidence == 2;
    return out;
  }
  Index index(source);
  std::unordered_set<Id> pending(selected.edges.begin(), selected.edges.end());
  if (pending.empty() && !selected.vertices.empty()) {
    const std::unordered_set<Id> verts(selected.vertices.begin(), selected.vertices.end());
    for (const auto &e : source.edges) {
      if (verts.contains(e.vertices[0]) && verts.contains(e.vertices[1]))
        pending.insert(e.id);
    }
  }
  if (pending.empty()) {
    fail(error, "Select one complete closed sketch profile");
    return {};
  }
  std::vector<Coedge> boundary;
  const Edge *first = nullptr;
  for (const auto &e : source.edges)
    if (pending.contains(e.id)) {
      first = &e;
      break;
    }
  if (!first) {
    fail(error, "Unknown selected sketch edge");
    return {};
  }
  const Id start = first->vertices[0];
  Id current = start;
  while (!pending.empty()) {
    const Edge *next = nullptr;
    for (const auto &e : source.edges) {
      if (!pending.contains(e.id) || (e.vertices[0] != current && e.vertices[1] != current))
        continue;
      if (next && current != start) {
        fail(error, "A branched sketch does not define one profile");
        return {};
      }
      next = &e;
      if (boundary.empty())
        break;
    }
    if (!next) {
      fail(error, "Close the sketch boundary before extrusion");
      return {};
    }
    const Coedge use{next->id, next->vertices[0] != current};
    boundary.push_back(use);
    pending.erase(next->id);
    current = end_vertex(*next, use);
    if (current == start)
      break;
  }
  if (!pending.empty() || current != start) {
    fail(error, "Select one closed, unbranched sketch boundary");
    return {};
  }
  std::unordered_map<Id, double> conic_sweeps;
  for (const auto &use : boundary) {
    const Edge &e = *index.edges.at(use.edge);
    if (index.curves.at(e.curve)->kind == CurveKind::Conic &&
        (conic_sweeps[e.curve] += std::abs(e.end - e.start)) > tau + 1e-12)
    {
      fail(error, "A closed profile cannot wind repeatedly around the same conic");
      return {};
    }
  }
  Vec3 area{};
  const Vec3 origin = index.vertices.at(start)->position;
  double scale = 0;
  for (const auto &use : boundary) {
    const auto &e = *index.edges.at(use.edge);
    const auto &c = *index.curves.at(e.curve);
    /* Subtract the same origin before integration to avoid large-translation cancellation. */
    Curve local = c;
    local.origin = local.origin - origin;
    if (c.kind == CurveKind::RationalQuadratic) {
      local.u = local.u - origin;
      local.v = local.v - origin;
    }
    area = area + area_integral(local, e.start, e.end) * (use.reversed ? -1 : 1);
    scale = std::max({scale,
                      length(c.origin - origin),
                      length(c.kind == CurveKind::RationalQuadratic ? local.u : c.u),
                      length(c.kind == CurveKind::RationalQuadratic ? local.v : c.v)});
  }
  if (!(length(area) > 0) || !finite(area)) {
    fail(error, "The sketch must enclose nonzero planar area");
    return {};
  }
  out.normal = area / length(area);
  const double tolerance = 256 * std::numeric_limits<double>::epsilon() * std::max(1.0, scale);
  int boundary_orientation = 0;
  int attached_edges = 0;
  int independent_edges = 0;
  for (const auto &use : boundary) {
    const auto &c = *index.curves.at(index.edges.at(use.edge)->curve);
    const Vec3 u = c.kind == CurveKind::RationalQuadratic ? c.u - origin : c.u;
    const Vec3 v = c.kind == CurveKind::RationalQuadratic ? c.v - origin : c.v;
    if (std::abs(dot(c.origin - origin, out.normal)) > tolerance ||
        std::abs(dot(u, out.normal)) > tolerance ||
        (c.kind != CurveKind::Line && std::abs(dot(v, out.normal)) > tolerance) ||
        (c.kind == CurveKind::Cubic && std::abs(dot(c.w, out.normal)) > tolerance))
    {
      fail(error, "Solid extrusion requires coplanar analytic sketch curves");
      return {};
    }
    int incident_faces = 0;
    for (const auto &f : source.faces) {
      for (const auto *loop : loops(f)) {
        for (const auto &item : *loop) {
          if (item.edge == use.edge) {
            if (++incident_faces > 1) {
              fail(error, "Fill can only cap an open face boundary, not an interior edge loop");
              return {};
            }
            const int required_orientation =
                -f.orientation * (item.reversed ? -1 : 1) * (use.reversed ? -1 : 1);
            if (boundary_orientation && boundary_orientation != required_orientation) {
              fail(error, "The open boundary does not have a consistent face orientation");
              return {};
            }
            boundary_orientation = required_orientation;
          }
        }
      }
    }
    if (incident_faces && !allow_boundary_cap) {
      fail(error, "Select a body face or an independent sketch");
      return {};
    }
    if (incident_faces)
      attached_edges++;
    else
      independent_edges++;
  }
  if (attached_edges && independent_edges) {
    fail(error, "Fill needs a complete open face boundary or an independent sketch");
    return {};
  }
  /* Straight boundaries have an analytic intersection test. Reject crossed or retraced
   * strokes before constructing a face; sampled viewport wires are not a validity test. */
  for (size_t i = 0; i < boundary.size(); ++i) {
    const Edge &a = *index.edges.at(boundary[i].edge);
    const Curve &ca = *index.curves.at(a.curve);
    if (ca.kind != CurveKind::Line)
      continue;
    const Vec3 p = evaluate(ca, a.start), r = evaluate(ca, a.end) - p;
    for (size_t j = i + 1; j < boundary.size(); ++j) {
      const Edge &b = *index.edges.at(boundary[j].edge);
      const Curve &cb = *index.curves.at(b.curve);
      if (cb.kind != CurveKind::Line)
        continue;
      const Vec3 q = evaluate(cb, b.start), s = evaluate(cb, b.end) - q;
      const double determinant = dot(cross(r, s), out.normal);
      const double roundoff = 256 * std::numeric_limits<double>::epsilon() * length(r) * length(s);
      const bool shared = a.vertices[0] == b.vertices[0] || a.vertices[0] == b.vertices[1] ||
                          a.vertices[1] == b.vertices[0] || a.vertices[1] == b.vertices[1];
      bool crossing = false;
      if (std::abs(determinant) > roundoff) {
        const double t = dot(cross(q - p, s), out.normal) / determinant;
        const double u = dot(cross(q - p, r), out.normal) / determinant;
        crossing = !shared && t >= 0 && t <= 1 && u >= 0 && u <= 1;
      }
      else if (length(cross(q - p, r)) <= tolerance * length(r)) {
        const double t = dot(q - p, r) / dot(r, r), u = t + dot(s, r) / dot(r, r);
        crossing = std::min(1.0, std::max(t, u)) - std::max(0.0, std::min(t, u)) >
                   tolerance / length(r);
      }
      if (crossing) {
        fail(error, "The sketch boundary crosses or retraces itself");
        return {};
      }
    }
  }
  Vec3 axis{1, 0, 0};
  if (std::abs(out.normal.x) > .8)
    axis = {0, 1, 0};
  Vec3 u = cross(axis, out.normal);
  u = u / length(u);
  const Vec3 v = cross(out.normal, u);
  if (out.model.next_id > std::numeric_limits<Id>::max() - 2) {
    fail(error, "Source identity budget exhausted");
    return {};
  }
  const Id plane = out.model.next_id++;
  out.face = out.model.next_id++;
  out.model.surfaces.push_back({plane, SurfaceKind::Plane, origin, u, v, 0});
  out.model.faces.push_back(
      {out.face, plane, std::move(boundary), {}, boundary_orientation ? boundary_orientation : 1});
  return out;
}

static bool validate_body_topology(const Model &model, Id seed, std::string *error)
{
  Index index(model, false);
  if (!index.faces.contains(seed)) {
    fail(error, "Unknown body face");
    return false;
  }
  struct Use {
    Id face;
    int sense;
  };
  std::unordered_map<Id, std::vector<Use>> uses;
  uses.reserve(model.edges.size());
  for (const auto &f : model.faces) {
    for (const auto *loop : loops(f)) {
      for (const auto &use : *loop)
        uses[use.edge].push_back({f.id, f.orientation * (use.reversed ? -1 : 1)});
    }
  }
  std::unordered_set<Id> visited{seed};
  std::vector<Id> stack{seed};
  while (!stack.empty()) {
    const auto &f = *index.faces.at(stack.back());
    stack.pop_back();
    for (const auto *loop : loops(f)) {
      for (const auto &use : *loop) {
        const auto &adjacent = uses.at(use.edge);
        if (adjacent.size() != 2 || adjacent[0].sense == adjacent[1].sense) {
          fail(error, "The result is not a closed consistently oriented boundary");
          return false;
        }
        for (const auto &a : adjacent)
          if (visited.insert(a.face).second)
            stack.push_back(a.face);
      }
    }
  }
  return true;
}

bool validate_body(const Model &model, Id seed, std::string *error)
{
  return validate(model, error) && validate_body_topology(model, seed, error);
}

static std::optional<EditResult> extrude_impl(
    const Profile &profile,
    double distance,
    std::string *error,
    bool check_source,
    bool check_body = true)
{
  if (!std::isfinite(distance) || distance == 0 || !finite(profile.normal) ||
      !(length(profile.normal) > 0) || (check_source && !validate(profile.model, error)))
  {
    fail(error, "Extrusion needs a valid profile and finite nonzero distance");
    return {};
  }
  if (profile.attached && distance < 0) {
    fail(error, "An inward cut requires solid intersection and trimming");
    return {};
  }
  const auto *source_face = face(profile.model, profile.face);
  if (!source_face) {
    fail(error, "Unknown profile face");
    return {};
  }
  Index index(profile.model);
  EditResult result;
  result.model = profile.model;
  auto &out = result.model;
  const Vec3 sweep = profile.normal * distance;
  std::unordered_map<Id, Id> vertices, curves, edges, rails;
  std::vector<Coedge> all = source_face->outer;
  for (const auto &hole : source_face->holes)
    all.insert(all.end(), hole.begin(), hole.end());
  if (out.next_id > std::numeric_limits<Id>::max() - 4 - all.size() * 10) {
    fail(error, "Extrusion source identities exhausted");
    return {};
  }
  for (const auto &use : all) {
    const auto &e = *index.edges.at(use.edge);
    for (Id id : e.vertices) {
      if (vertices.contains(id))
        continue;
      const Vec3 a = index.vertices.at(id)->position, b = a + sweep;
      if (a == b || !finite(b)) {
        fail(error, "Extrusion exceeds source precision or range");
        return {};
      }
      vertices[id] = out.next_id++;
      out.vertices.push_back({vertices[id], b});
      const Id c = out.next_id++;
      out.curves.push_back({c, CurveKind::Line, a, sweep, {}});
      rails[id] = out.next_id++;
      out.edges.push_back({rails[id], c, {id, vertices[id]}, 0, 1});
    }
    if (!curves.contains(e.curve)) {
      Curve c = *index.curves.at(e.curve);
      c.id = out.next_id++;
      c.origin = c.origin + sweep;
      if (c.kind == CurveKind::RationalQuadratic) {
        c.u = c.u + sweep;
        c.v = c.v + sweep;
      }
      curves[e.curve] = c.id;
      out.curves.push_back(c);
    }
    if (!edges.contains(e.id)) {
      edges[e.id] = out.next_id++;
      out.edges.push_back({edges[e.id],
                           curves[e.curve],
                           {vertices[e.vertices[0]], vertices[e.vertices[1]]},
                           e.start,
                           e.end});
    }
    const Id support = out.next_id++;
    const Curve &boundary_curve = *index.curves.at(e.curve);
    if (boundary_curve.kind == CurveKind::Line) {
      const Vec3 a = evaluate(boundary_curve, e.start), b = evaluate(boundary_curve, e.end);
      out.surfaces.push_back({support, SurfaceKind::Plane, a, b - a, sweep, 0});
    }
    else {
      out.surfaces.push_back(
          {support, SurfaceKind::Extrusion, {}, sweep, {}, e.curve, e.start, e.end});
    }
    const int sign = source_face->orientation * (use.reversed ? -1 : 1) * (distance < 0 ? -1 : 1);
    out.faces.push_back({out.next_id++,
                         support,
                         {{e.id, false},
                          {rails[e.vertices[1]], false},
                          {edges[e.id], true},
                          {rails[e.vertices[0]], true}},
                         {},
                         sign});
  }
  Surface support = *index.surfaces.at(source_face->surface);
  support.id = out.next_id++;
  support.origin = support.origin + sweep;
  out.surfaces.push_back(support);
  Face cap = *source_face;
  cap.id = out.next_id++;
  cap.surface = support.id;
  cap.orientation *= distance < 0 ? -1 : 1;
  for (auto *loop : [&]() {
         std::vector<std::vector<Coedge> *> ls{&cap.outer};
         for (auto &h : cap.holes)
           ls.push_back(&h);
         return ls;
       }())
    for (auto &use : *loop)
      use.edge = edges.at(use.edge);
  result.selection.mode = 4;
  result.selection.active = cap.id;
  result.selection.faces.push_back(cap.id);
  for (const auto &[old, id] : vertices)
    result.selection.vertices.push_back(id);
  for (const auto &[old, id] : edges)
    result.selection.edges.push_back(id);
  out.faces.push_back(cap);
  for (auto it = out.faces.begin(); it != out.faces.end(); ++it) {
    if (it->id != profile.face)
      continue;
    if (profile.attached)
      out.faces.erase(it);
    else
      it->orientation *= distance > 0 ? -1 : 1;
    break;
  }
  if (check_body && !validate_body(out, cap.id, error))
    return {};
  return result;
}

std::optional<EditResult> extrude(const Profile &profile, double distance, std::string *error)
{
  return extrude_impl(profile, distance, error, true);
}

std::optional<PreparedExtrusion> PreparedExtrusion::create(Profile profile, std::string *error)
{
  if (!validate(profile.model, error))
    return {};
  return PreparedExtrusion(std::move(profile));
}

std::optional<EditResult> PreparedExtrusion::evaluate(double distance, std::string *error) const
{
  return extrude_impl(profile_, distance, error, false);
}

std::optional<PreparedExtrusion::Preview> PreparedExtrusion::preview(
    double distance, bool analytic, std::string *error)
{
  /* The immutable input was validated before the modal tool started. Validate only the generated
   * delta; untouched source geometry retains its existing validation proof. */
  auto result = extrude_impl(profile_, distance, error, false, false);
  if (!result)
    return {};
  auto model = std::make_shared<const Model>(std::move(result->model));
  if (!validate_extrusion_delta(profile_, *model, error))
    return {};
  /* Extrusion distance changes coordinates, never IDs or incidence. Its sign can change
   * orientation, so verify positive and negative sweeps separately, then reuse the matching
   * topology proof for later modal updates. */
  const uint8_t sign_bit = distance > 0 ? 1 : 2;
  if (!(validated_distance_signs_ & sign_bit)) {
    if (!validate_body_topology(*model, result->selection.active, error))
      return {};
    validated_distance_signs_ |= sign_bit;
  }
  std::optional<ValidatedModel> validation =
      ValidatedModel::from_validated_extrusion(model);
  const Face &cap = *face(profile_.model, profile_.face);
  bool linear = distance > 0 && cap.holes.empty() && cap.outer.size() == 4;
  for (const Coedge &use : cap.outer)
    linear &= curve(profile_.model, edge(profile_.model, use.edge)->curve)->kind == CurveKind::Line;
  if (!linear || (display_cache_ && display_cache_->has_analytic_normals != analytic))
    display_cache_.reset();
  DisplayMesh samples;
  /* A positive straight-quad sweep changes only its new source vertices. The side
   * polygons have no interior samples, their normal directions stay fixed, and all
   * zero-ID curve/surface samples belong to the unchanged original body. Other
   * profiles and normal-method changes take the full tessellation path. */
  if (display_cache_) {
    samples = *display_cache_;
    const IdIndex<Vertex> vertices(model->vertices);
    for (size_t i = 0; i < samples.positions.size(); ++i)
      if (samples.vertex_source[i])
        samples.positions[i] = vertices.at(samples.vertex_source[i])->position;
  }
  else {
    auto generated = tessellate(*validation, 64, error, analytic);
    if (!generated)
      return {};
    samples = std::move(*generated);
    if (linear)
      display_cache_ = samples;
  }
  return Preview{std::move(model),
                 std::move(result->selection),
                 std::move(validation),
                 std::make_shared<const DisplayMesh>(std::move(samples))};
}

std::optional<EditResult> transform_selected(const Model &model,
                                             Selection selection,
                                             const Affine &matrix,
                                             std::string *error)
{
  if (!validate(model, error))
    return {};
  auto direction = [&](Vec3 p) {
    return matrix.columns[0] * p.x + matrix.columns[1] * p.y + matrix.columns[2] * p.z;
  };
  auto point = [&](Vec3 p) { return direction(p) + matrix.translation; };
  const double determinant = dot(matrix.columns[0], cross(matrix.columns[1], matrix.columns[2]));
  if (!std::isfinite(determinant) || determinant == 0 || !finite(matrix.translation)) {
    fail(error, "An analytic transform needs a finite invertible matrix");
    return {};
  }
  selection = normalize_selection(model, std::move(selection));
  std::unordered_set<Id> selected(selection.vertices.begin(), selection.vertices.end());
  std::unordered_set<Id> curves, surfaces;
  bool needs_refit = false;
  const bool complete = selected.size() == model.vertices.size();
  const Index index(model);
  for (const Face &f : model.faces) {
    if (complete) {
      surfaces.insert(f.surface);
      continue;
    }
    bool any = false, all = true;
    for (const auto *loop : loops(f))
      for (const Coedge &use : *loop) {
        for (Id v : index.edges.at(use.edge)->vertices) {
          any |= selected.contains(v);
          all &= selected.contains(v);
        }
      }
    if (any && !all) {
      needs_refit = true;
      bool straight_plane = surface(model, f.surface)->kind == SurfaceKind::Plane &&
                            f.holes.empty();
      for (const Coedge &use : f.outer)
        straight_plane &= curve(model, edge(model, use.edge)->curve)->kind == CurveKind::Line;
      if (!straight_plane) {
        fail(error, "Partial curved-surface deformation needs an analytic construction rule");
        return {};
      }
    }
    if (all)
      surfaces.insert(f.surface);
  }
  struct CurveSelection { bool any = false, all = true; };
  std::unordered_map<Id, CurveSelection> curve_selection;
  if (!complete)
    for (const Edge &e : model.edges) {
      auto &state = curve_selection[e.curve];
      for (Id v : e.vertices) {
        state.any |= selected.contains(v);
        state.all &= selected.contains(v);
      }
    }
  for (const Curve &c : model.curves) {
    if (complete) {
      curves.insert(c.id);
      continue;
    }
    const auto [any, all] = curve_selection[c.id];
    if (any && !all && c.kind != CurveKind::Line) {
      fail(error, "Select the complete arc or conic to transform its exact axes");
      return {};
    }
    if (all && any)
      curves.insert(c.id);
  }
  EditResult result{model, std::move(selection)};
  for (Vertex &v : result.model.vertices)
    if (selected.contains(v.id))
      v.position = point(v.position);
  for (Curve &c : result.model.curves) {
    if (curves.contains(c.id)) {
      c.origin = point(c.origin);
      if (c.kind == CurveKind::RationalQuadratic) {
        c.u = point(c.u);
        c.v = point(c.v);
      }
      else {
        c.u = direction(c.u);
        c.v = direction(c.v);
        c.w = direction(c.w);
      }
    }
    else if (c.kind == CurveKind::Line) {
      const Edge *use = nullptr;
      for (const Edge &e : result.model.edges)
        if (e.curve == c.id &&
            (selected.contains(e.vertices[0]) || selected.contains(e.vertices[1])))
        {
          if (use) {
            fail(error, "Shared straight support needs a coordinated endpoint edit");
            return {};
          }
          use = &e;
        }
      if (use) {
        const Vec3 a = vertex(result.model, use->vertices[0])->position,
                   b = vertex(result.model, use->vertices[1])->position;
        c.u = (b - a) / (use->end - use->start);
        c.origin = a - c.u * use->start;
      }
    }
  }
  for (Surface &s : result.model.surfaces)
    if (surfaces.contains(s.id)) {
      s.origin = point(s.origin);
      s.u = direction(s.u);
      s.v = direction(s.v);
      s.w = direction(s.w);
      for (Vec3 &coefficient : s.blend)
        coefficient = direction(coefficient);
    }
  if (determinant < 0)
    for (Face &f : result.model.faces)
      if (surfaces.contains(f.surface))
        f.orientation = -f.orientation;
  /* Affine transforms already move complete support planes exactly. Rebuilding them
   * changes identities and ordering unnecessarily, and defeats display topology reuse. */
  if ((needs_refit && !refit_linear_faces(result.model, error)) || !validate(result.model, error))
    return {};
  return result;
}

Selection normalize_selection(const Model &model, Selection selection)
{
  /* Selection only references topology, never curve or surface support IDs. */
  const Index index(model, false);
  selection.mode = selection.mode & 4 ? 4 : selection.mode & 2 ? 2 : 1;
  auto clean = [](auto &ids, const auto &valid) {
    std::erase_if(ids, [&](Id id) { return !valid.contains(id); });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  };
  clean(selection.vertices, index.vertices);
  clean(selection.edges, index.edges);
  clean(selection.faces, index.faces);
  if (selection.mode == 4) {
    std::unordered_set<Id> groups;
    std::unordered_set<Id> faces(selection.faces.begin(), selection.faces.end());
    for (Id id : selection.faces) {
      const Face &f = *index.faces.at(id);
      groups.insert(f.group ? f.group : f.id);
    }
    for (const Face &f : model.faces)
      if (groups.contains(f.group ? f.group : f.id) &&
          faces.insert(f.id).second)
        selection.faces.push_back(f.id);
  }
  std::unordered_set<Id> vertices(selection.vertices.begin(), selection.vertices.end());
  std::unordered_set<Id> edges(selection.edges.begin(), selection.edges.end());
  auto add = [](auto &ids, auto &set, Id id) {
    if (id && set.insert(id).second)
      ids.push_back(id);
  };
  if (selection.mode == 4) {
    selection.edges.clear();
    edges.clear();
    for (Id id : selection.faces) {
      for (const auto *loop : loops(*index.faces.at(id))) {
        for (const Coedge &use : *loop) {
          if (index.edges.contains(use.edge))
            add(selection.edges, edges, use.edge);
        }
      }
    }
  }
  if (selection.mode == 2 || selection.mode == 4) {
    selection.vertices.clear();
    vertices.clear();
    for (Id id : selection.edges)
      for (Id v : index.edges.at(id)->vertices)
        add(selection.vertices, vertices, v);
  }
  else {
    selection.edges.clear();
    edges.clear();
    for (const Edge &e : model.edges) {
      if (vertices.contains(e.vertices[0]) && vertices.contains(e.vertices[1]))
        add(selection.edges, edges, e.id);
    }
  }
  if (selection.mode != 4) {
    selection.faces.clear();
    for (const Face &f : model.faces) {
      bool all = !f.outer.empty();
      for (const auto *loop : loops(f))
        for (const auto &use : *loop)
          all &= edges.contains(use.edge);
      if (all)
        selection.faces.push_back(f.id);
    }
  }
  if (selection.active && !index.vertices.contains(selection.active) &&
      !index.edges.contains(selection.active) && !index.faces.contains(selection.active))
    selection.active = 0;
  return selection;
}

}  // namespace blender::solid
