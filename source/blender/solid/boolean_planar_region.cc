/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_planar_region.hh"

#include "SOL_boolean.hh"
#include "SOL_classify.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>

namespace blender::solid {
namespace {

using Loop = std::vector<Coedge>;
using Wide = std::array<long double, 3>;
constexpr double period = 2 * std::numbers::pi;
constexpr long double roundoff = 128 * std::numeric_limits<double>::epsilon();

std::vector<const Loop *> loops(const Face &face)
{
  std::vector<const Loop *> result{&face.outer};
  for (const Loop &hole : face.holes) {
    result.push_back(&hole);
  }
  return result;
}

Id first_vertex(const Model &model, const Coedge use)
{
  return edge(model, use.edge)->vertices[use.reversed ? 1 : 0];
}

Id last_vertex(const Model &model, const Coedge use)
{
  return edge(model, use.edge)->vertices[use.reversed ? 0 : 1];
}

Id allocate(Model &model)
{
  if (model.next_id == std::numeric_limits<Id>::max()) {
    throw std::runtime_error("Native planar subtraction exhausted identities");
  }
  return model.next_id++;
}

bool exact_sum(const long double a, const long double b, long double &result)
{
  result = a + b;
  const long double virtual_b = result - a;
  return std::isfinite(result) && (a - (result - virtual_b)) + (b - virtual_b) == 0;
}

bool exact_product(const long double a, const long double b, long double &result)
{
  result = a * b;
  return std::isfinite(result) &&
         (a == 0 || b == 0 || std::abs(result) >= std::numeric_limits<long double>::min()) &&
         std::fma(a, b, -result) == 0;
}

bool zero_triple(const Vec3 a, const Vec3 b, const Wide &c)
{
  long double value = 0;
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    long double first, second, difference, term;
    if (!exact_product(a[j], b[k], first) || !exact_product(a[k], b[j], second) ||
        !exact_sum(first, -second, difference) || !exact_product(difference, c[i], term) ||
        !exact_sum(value, term, value))
    {
      return false;
    }
  }
  return value == 0;
}

bool same_plane(const Surface &a, const Surface &b)
{
  if (a.origin == b.origin) {
    auto same_axis = [](const Vec3 first, const Vec3 second) {
      return first == second || first == second * -1;
    };
    if ((same_axis(a.u, b.u) && same_axis(a.v, b.v)) ||
        (same_axis(a.u, b.v) && same_axis(a.v, b.u)))
    {
      return true;
    }
  }
  Wide difference;
  for (int i = 0; i < 3; ++i) {
    if (!exact_sum(b.origin[i], -static_cast<long double>(a.origin[i]), difference[i])) {
      return false;
    }
  }
  return zero_triple(a.u, a.v, difference) &&
         zero_triple(a.u, a.v, {b.u.x, b.u.y, b.u.z}) &&
         zero_triple(a.u, a.v, {b.v.x, b.v.y, b.v.z});
}

long double triple(const Vec3 a, const Vec3 b, const Vec3 normal, const bool magnitude = false)
{
  long double value = 0;
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const long double first = static_cast<long double>(a[j]) * b[k];
    const long double second = static_cast<long double>(a[k]) * b[j];
    value += magnitude ? (std::abs(first) + std::abs(second)) * std::abs(normal[i]) :
                         (first - second) * normal[i];
  }
  return value;
}

long double area(const Model &model, const Loop &loop, const Vec3 normal)
{
  const Vec3 reference = vertex(model, first_vertex(model, loop.front()))->position;
  long double value = 0, magnitude = 0;
  for (const Coedge use : loop) {
    const Edge &e = *edge(model, use.edge);
    const Curve &c = *curve(model, e.curve);
    const long double a = use.reversed ? e.end : e.start;
    const long double b = use.reversed ? e.start : e.end;
    const long double delta = b - a;
    const Vec3 origin = c.origin - reference;
    long double cosine_delta = 0, sine_delta = 0;
    if (c.kind == CurveKind::Conic) {
      const long double twice_half_sine = 2 * std::sin(delta / 2);
      cosine_delta = -std::sin(a / 2 + b / 2) * twice_half_sine;
      sine_delta = std::cos(a / 2 + b / 2) * twice_half_sine;
      value += triple(origin, c.u, normal) * cosine_delta +
               triple(origin, c.v, normal) * sine_delta + triple(c.u, c.v, normal) * delta;
      magnitude += triple(origin, c.u, normal, true) * std::abs(cosine_delta) +
                   triple(origin, c.v, normal, true) * std::abs(sine_delta) +
                   triple(c.u, c.v, normal, true) * std::abs(delta);
    }
    else if (c.kind == CurveKind::Line) {
      value += triple(origin, c.u, normal) * delta;
      magnitude += triple(origin, c.u, normal, true) * std::abs(delta);
    }
    else {
      throw std::runtime_error("Native planar subtraction needs Line/Conic trims");
    }
  }
  if (!std::isfinite(value) || !std::isfinite(magnitude) ||
      std::abs(value) <= roundoff * magnitude)
  {
    throw std::runtime_error("Native planar subtraction cannot certify a boundary winding");
  }
  return value / 2;
}

std::set<Id> face_edges(const Face &face)
{
  std::set<Id> result;
  for (const Loop *loop : loops(face)) {
    for (const Coedge use : *loop) {
      if (!result.insert(use.edge).second) {
        throw std::runtime_error("Native planar subtraction found a repeated face edge");
      }
    }
  }
  return result;
}

Vec3 midpoint(const Model &model, const Id id)
{
  const Edge &e = *edge(model, id);
  const double t = e.start / 2 + e.end / 2;
  if (t == e.start || t == e.end) {
    throw std::runtime_error("Native planar subtraction cannot represent an interior parameter");
  }
  return evaluate(*curve(model, e.curve), t);
}

void split_at_vertices(Model &model, const double tolerance)
{
  std::map<Id, std::vector<Id>> replacements;
  std::vector<Edge> output;
  for (const Edge &original : model.edges) {
    const Curve &c = *curve(model, original.curve);
    const double low = std::min(original.start, original.end);
    const double high = std::max(original.start, original.end);
    if (c.kind != CurveKind::Line && c.kind != CurveKind::Conic) {
      throw std::runtime_error("Native planar subtraction needs Line/Conic trims");
    }
    if (c.kind == CurveKind::Conic &&
        (high - low > period || std::max(std::abs(low), std::abs(high)) > 1e12))
    {
      throw std::runtime_error("Native planar subtraction needs a single-wrap conic trim");
    }
    std::vector<std::pair<double, Id>> parameters{{original.start, original.vertices[0]},
                                                 {original.end, original.vertices[1]}};
    auto append = [&](const Vertex &v, const double t) {
      if (std::isfinite(t) && t >= low && t <= high &&
          length(evaluate(c, t) - v.position) <= tolerance)
      {
        parameters.emplace_back(t, v.id);
      }
    };
    for (const Vertex &v : model.vertices) {
      if (v.id == original.vertices[0] || v.id == original.vertices[1]) {
        continue;
      }
      if (c.kind == CurveKind::Line) {
        int axis = 0;
        for (int i = 1; i < 3; ++i) {
          if (std::abs(c.u[i]) > std::abs(c.u[axis])) {
            axis = i;
          }
        }
        const long double t = (static_cast<long double>(v.position[axis]) - c.origin[axis]) /
                              c.u[axis];
        append(v, double(t));
      }
      else {
        const Vec3 normal = cross(c.u, c.v);
        int axis = 0;
        for (int i = 1; i < 3; ++i) {
          if (std::abs(normal[i]) > std::abs(normal[axis])) {
            axis = i;
          }
        }
        const int j = (axis + 1) % 3, k = (axis + 2) % 3;
        const long double qj = static_cast<long double>(v.position[j]) - c.origin[j];
        const long double qk = static_cast<long double>(v.position[k]) - c.origin[k];
        const long double determinant = static_cast<long double>(c.u[j]) * c.v[k] -
                                        static_cast<long double>(c.u[k]) * c.v[j];
        const long double x = (qj * c.v[k] - qk * c.v[j]) / determinant;
        const long double y = (c.u[j] * qk - c.u[k] * qj) / determinant;
        const double phase = std::atan2(double(y), double(x));
        const long double first = std::ceil((static_cast<long double>(low) - phase) / period);
        const long double last = std::floor((static_cast<long double>(high) - phase) / period);
        for (long double turn = first; turn <= last; ++turn) {
          append(v, double(phase + turn * period));
        }
      }
    }
    std::sort(parameters.begin(), parameters.end());
    parameters.erase(std::unique(parameters.begin(), parameters.end()), parameters.end());
    for (size_t i = 1; i < parameters.size(); ++i) {
      if (parameters[i - 1].first == parameters[i].first) {
        throw std::runtime_error("Distinct native vertices have an ambiguous shared parameter");
      }
    }
    if (original.start > original.end) {
      std::reverse(parameters.begin(), parameters.end());
    }
    for (size_t i = 1; i < parameters.size(); ++i) {
      Edge part = original;
      part.id = parameters.size() == 2 ? original.id : allocate(model);
      part.start = parameters[i - 1].first;
      part.end = parameters[i].first;
      part.vertices = {parameters[i - 1].second, parameters[i].second};
      replacements[original.id].push_back(part.id);
      output.push_back(part);
    }
  }
  model.edges = std::move(output);
  for (Face &face : model.faces) {
    auto remap = [&](Loop &loop) {
      Loop replacement;
      for (const Coedge use : loop) {
        const auto &parts = replacements.at(use.edge);
        if (use.reversed) {
          for (auto i = parts.rbegin(); i != parts.rend(); ++i) {
            replacement.push_back({*i, true});
          }
        }
        else {
          for (const Id id : parts) {
            replacement.push_back({id, false});
          }
        }
      }
      loop = std::move(replacement);
    };
    remap(face.outer);
    for (Loop &hole : face.holes) {
      remap(hole);
    }
  }
}

/* These bounds certify existing analytic spans. Parameter subdivision adds no
 * topology, evaluates no polyline approximation, and constructs no intersection. */
std::pair<long double, long double> trig_range(const long double a,
                                              const long double b,
                                              const double start,
                                              const double end)
{
  const long double low = std::min(start, end), high = std::max(start, end);
  auto value = [&](const long double t) { return a * std::cos(t) + b * std::sin(t); };
  long double minimum = std::min(value(low), value(high));
  long double maximum = std::max(value(low), value(high));
  const long double phase = std::atan2(b, a), pi = std::numbers::pi_v<long double>;
  const long double first = std::ceil((low - phase) / pi);
  const long double last = std::floor((high - phase) / pi);
  for (long double turn = first; turn <= last; ++turn) {
    const long double extremum = value(phase + turn * pi);
    minimum = std::min(minimum, extremum);
    maximum = std::max(maximum, extremum);
  }
  const long double pad = roundoff * (std::abs(a) + std::abs(b)) *
                          std::max({1.L, std::abs(low), std::abs(high)});
  return {minimum - pad, maximum + pad};
}

struct Interval {
  const Curve *curve;
  double start, end;
  Id first, last;
};

struct Box {
  Wide low, high;
  long double size = 0;
};

Box bounds(const Interval &span)
{
  Box result;
  const Curve &c = *span.curve;
  for (int i = 0; i < 3; ++i) {
    if (c.kind == CurveKind::Conic) {
      const auto range = trig_range(c.u[i], c.v[i], span.start, span.end);
      const long double pad = roundoff * std::abs(c.origin[i]);
      result.low[i] = c.origin[i] + range.first - pad;
      result.high[i] = c.origin[i] + range.second + pad;
    }
    else {
      const long double a = c.origin[i] + static_cast<long double>(c.u[i]) * span.start;
      const long double b = c.origin[i] + static_cast<long double>(c.u[i]) * span.end;
      const long double pad = roundoff *
                              (std::abs(c.origin[i]) + std::abs(a) + std::abs(b));
      result.low[i] = std::min(a, b) - pad;
      result.high[i] = std::max(a, b) + pad;
    }
    result.size = std::max(result.size, result.high[i] - result.low[i]);
  }
  return result;
}

std::optional<Vec3> outgoing(const Interval &span, const Id junction)
{
  if ((span.first == junction) == (span.last == junction)) {
    return {};
  }
  const bool first = span.first == junction;
  Vec3 direction = tangent(*span.curve, first ? span.start : span.end);
  const double size = length(direction);
  if (!(size > 0) || !std::isfinite(size)) {
    return {};
  }
  const double sign = ((span.end > span.start) == first) ? 1 : -1;
  return direction * (sign / size);
}

std::pair<long double, long double> derivative_range(const Interval &span,
                                                    const Vec3 axis,
                                                    const Id junction)
{
  long double u = 0, v = 0, u_magnitude = 0, v_magnitude = 0;
  for (int i = 0; i < 3; ++i) {
    const long double up = static_cast<long double>(axis[i]) * span.curve->u[i];
    const long double vp = static_cast<long double>(axis[i]) * span.curve->v[i];
    u += up;
    v += vp;
    u_magnitude += std::abs(up);
    v_magnitude += std::abs(vp);
  }
  std::pair<long double, long double> range;
  if (span.curve->kind == CurveKind::Conic) {
    range = trig_range(v, -u, span.start, span.end);
    range.first -= roundoff * (u_magnitude + v_magnitude);
    range.second += roundoff * (u_magnitude + v_magnitude);
  }
  else {
    range = {u - roundoff * u_magnitude, u + roundoff * u_magnitude};
  }
  if ((span.end > span.start) != (span.first == junction)) {
    range = {-range.second, -range.first};
  }
  return range;
}

bool separated_at_junction(const Interval &a, const Interval &b)
{
  for (const Id junction : {a.first, a.last}) {
    if (!junction || (junction != b.first && junction != b.last)) {
      continue;
    }
    const auto ad = outgoing(a, junction), bd = outgoing(b, junction);
    if (!ad || !bd) {
      continue;
    }
    Vec3 axis = *ad - *bd;
    const Vec3 cross_direction = cross(*ad, *bd);
    const double cross_size = length(cross_direction);
    if (cross_size > 0 && std::isfinite(cross_size)) {
      /* This bisector avoids the second-order cancellation in a-b when
       * two outgoing unit tangents make a small, nonzero angle. The scalar
       * derivative bounds below still certify the chosen separating axis. */
      axis = cross(*ad + *bd, cross_direction / cross_size);
    }
    const auto ar = derivative_range(a, axis, junction);
    const auto br = derivative_range(b, axis, junction);
    if (ar.first > 0 && br.second < 0) {
      return true;
    }
  }
  return false;
}

bool certify_pair(const Interval &a, const Interval &b, const int depth, int &budget)
{
  if (--budget < 0) {
    return false;
  }
  const Box ab = bounds(a), bb = bounds(b);
  for (int i = 0; i < 3; ++i) {
    if (ab.high[i] < bb.low[i] || bb.high[i] < ab.low[i]) {
      return true;
    }
  }
  if (separated_at_junction(a, b)) {
    return true;
  }
  if (depth == 48) {
    return false;
  }
  auto split = [&](const Interval &span, const Interval &other) {
    const double middle = span.start / 2 + span.end / 2;
    if (middle == span.start || middle == span.end) {
      return false;
    }
    const Interval first{span.curve, span.start, middle, span.first, 0};
    const Interval last{span.curve, middle, span.end, 0, span.last};
    return certify_pair(first, other, depth + 1, budget) &&
           certify_pair(last, other, depth + 1, budget);
  };
  return ab.size >= bb.size ? split(a, b) : split(b, a);
}

void certify_arrangement(const Model &model)
{
  for (size_t i = 0; i < model.edges.size(); ++i) {
    const Edge &a = model.edges[i];
    for (size_t j = i + 1; j < model.edges.size(); ++j) {
      const Edge &b = model.edges[j];
      const Interval ai{curve(model, a.curve), a.start, a.end, a.vertices[0], a.vertices[1]};
      const Interval bi{curve(model, b.curve), b.start, b.end, b.vertices[0], b.vertices[1]};
      int budget = 4096;
      if (!certify_pair(ai, bi, 0, budget)) {
        throw std::runtime_error(
            "Native planar overlay has an unmarked crossing or ambiguous boundary contact");
      }
    }
  }
}

PointClassification classify(const Model &model, const Face &face, const Vec3 point)
{
  std::string message;
  const auto state = classify_face_trim_point(model, face, point, {0, 0}, &message);
  if (!state || *state == PointClassification::Uncertain) {
    throw std::runtime_error("Native planar containment is unresolved: " + message);
  }
  return *state;
}

void certify_regions(const Model &model, const Vec3 normal)
{
  const Face &source = model.faces.front();
  const auto source_edges = face_edges(source);
  std::vector<std::set<Id>> region_edges;
  for (const Face &face : model.faces) {
    const bool positive = area(model, face.outer, normal) > 0;
    Face outer = face;
    outer.holes.clear();
    for (const Loop &hole : face.holes) {
      if ((area(model, hole, normal) > 0) == positive) {
        throw std::runtime_error("Native planar face has inconsistent hole winding");
      }
      for (const Coedge use : hole) {
        if (classify(model, outer, midpoint(model, use.edge)) != PointClassification::Inside) {
          throw std::runtime_error("Native planar hole is not strictly inside its outer boundary");
        }
      }
    }
    for (size_t i = 0; i < face.holes.size(); ++i) {
      Face hole_region = outer;
      hole_region.outer = face.holes[i];
      for (size_t j = 0; j < face.holes.size(); ++j) {
        if (i == j) {
          continue;
        }
        for (const Coedge use : face.holes[j]) {
          if (classify(model, hole_region, midpoint(model, use.edge)) !=
              PointClassification::Outside)
          {
            throw std::runtime_error("Native planar face has overlapping or nested hole trims");
          }
        }
      }
    }
    region_edges.push_back(face_edges(face));
  }
  for (size_t i = 1; i < model.faces.size(); ++i) {
    const Face &region = model.faces[i];
    for (const Id id : region_edges[i]) {
      if (source_edges.contains(id)) {
        continue;
      }
      if (classify(model, source, midpoint(model, id)) != PointClassification::Inside) {
        throw std::runtime_error("A planar subtraction region is not certified contained");
      }
    }
    for (const Loop &hole : source.holes) {
      for (const Coedge use : hole) {
        if (!region_edges[i].contains(use.edge) &&
            classify(model, region, midpoint(model, use.edge)) != PointClassification::Outside)
        {
          throw std::runtime_error("A planar subtraction region covers an excluded source hole");
        }
      }
    }
    for (size_t j = 1; j < model.faces.size(); ++j) {
      if (i == j) {
        continue;
      }
      for (const Id id : region_edges[i]) {
        if (!region_edges[j].contains(id) &&
            classify(model, model.faces[j], midpoint(model, id)) != PointClassification::Outside)
        {
          throw std::runtime_error("Planar subtraction regions overlap or have unmatched contacts");
        }
      }
    }
  }
}

std::vector<Loop> remaining_loops(const Model &model, const Vec3 normal)
{
  const bool source_positive = area(model, model.faces.front().outer, normal) > 0;
  std::map<Id, std::vector<Coedge>> uses;
  for (size_t i = 0; i < model.faces.size(); ++i) {
    const Face &face = model.faces[i];
    const bool reverse = i != 0 && ((area(model, face.outer, normal) > 0) == source_positive);
    for (const Loop *loop : loops(face)) {
      for (Coedge use : *loop) {
        use.reversed ^= reverse;
        uses[use.edge].push_back(use);
      }
    }
  }
  std::map<Id, Coedge> outgoing, remaining;
  std::set<Id> incoming;
  for (const auto &[id, occurrences] : uses) {
    if (occurrences.size() == 2 && occurrences[0].reversed != occurrences[1].reversed) {
      continue;
    }
    if (occurrences.size() != 1) {
      throw std::runtime_error("Native planar regions have repeated or overlapping boundary uses");
    }
    const Coedge use = occurrences.front();
    if (!outgoing.emplace(first_vertex(model, use), use).second ||
        !incoming.insert(last_vertex(model, use)).second)
    {
      throw std::runtime_error("Native planar difference has an ambiguous boundary junction");
    }
    remaining.emplace(id, use);
  }
  std::vector<Loop> result;
  while (!remaining.empty()) {
    Loop loop;
    Coedge use = remaining.begin()->second;
    const Id first = first_vertex(model, use);
    for (;;) {
      if (!remaining.erase(use.edge)) {
        throw std::runtime_error("Native planar difference did not form simple closed cycles");
      }
      loop.push_back(use);
      const Id last = last_vertex(model, use);
      if (last == first) {
        break;
      }
      const auto next = outgoing.find(last);
      if (next == outgoing.end()) {
        throw std::runtime_error("Native planar difference produced an open boundary");
      }
      use = next->second;
    }
    result.push_back(std::move(loop));
  }
  return result;
}

}  // namespace

std::optional<Model> boolean_subtract_planar_regions(const Model &source,
                                                    const Id face_id,
                                                    const std::span<const Model> regions,
                                                    const double tolerance,
                                                    std::string *error)
{
  try {
    if (!std::isfinite(tolerance) || tolerance < 0) {
      throw std::runtime_error("Native planar subtraction needs a finite nonnegative tolerance");
    }
    const Face *input = face(source, face_id);
    if (!input) {
      throw std::runtime_error("Native planar subtraction references an unknown source face");
    }
    const Surface *support = surface(source, input->surface);
    if (!support || support->kind != SurfaceKind::Plane) {
      throw std::runtime_error("Native planar subtraction needs a planar source face");
    }
    Model source_piece = source;
    source_piece.faces = {*input};
    source_piece.faces.front().group = 0;
    std::vector<Model> pieces{std::move(source_piece)};
    for (const Model &region : regions) {
      if (region.faces.size() != 1) {
        throw std::runtime_error("Each native planar subtraction region must contain one face");
      }
      const Surface *plane = surface(region, region.faces.front().surface);
      if (!plane || plane->kind != SurfaceKind::Plane || !same_plane(*support, *plane)) {
        throw std::runtime_error("Native planar subtraction regions need a certified common plane");
      }
      pieces.push_back(region);
      pieces.back().faces.front().group = 0;
    }
    std::string message;
    auto model = boolean_sew_faces(pieces, tolerance, false, &message);
    if (!model) {
      throw std::runtime_error(message);
    }
    split_at_vertices(*model, tolerance);
    auto arrangement = boolean_sew_faces(std::span<const Model>(&*model, 1),
                                         tolerance, false, &message);
    if (!arrangement) {
      throw std::runtime_error(message);
    }
    certify_arrangement(*arrangement);
    const Surface &plane = *surface(*arrangement, arrangement->faces.front().surface);
    const Vec3 normal = cross(plane.u, plane.v) / length(cross(plane.u, plane.v));
    certify_regions(*arrangement, normal);
    const Face prototype = arrangement->faces.front();
    const bool positive = area(*arrangement, prototype.outer, normal) > 0;
    std::vector<Face> faces;
    std::vector<Loop> holes;
    for (Loop &loop : remaining_loops(*arrangement, normal)) {
      if ((area(*arrangement, loop, normal) > 0) != positive) {
        holes.push_back(std::move(loop));
      }
      else {
        faces.push_back({allocate(*arrangement), prototype.surface,
                         std::move(loop), {}, prototype.orientation});
      }
    }
    for (const Loop &hole : holes) {
      Face *owner = nullptr;
      long double owner_area = std::numeric_limits<long double>::infinity();
      const Vec3 point = vertex(*arrangement, first_vertex(*arrangement, hole.front()))->position;
      for (Face &outer : faces) {
        Face outer_region = outer;
        outer_region.holes.clear();
        const auto state = classify(*arrangement, outer_region, point);
        if (state == PointClassification::Boundary) {
          throw std::runtime_error("Native planar difference has touching outer/hole boundaries");
        }
        if (state == PointClassification::Inside) {
          const long double outer_area = std::abs(area(*arrangement, outer.outer, normal));
          if (outer_area == owner_area) {
            throw std::runtime_error("Native planar difference has ambiguous hole ownership");
          }
          /* A hole can lie inside both the source outer loop and a remaining
           * island. Its owner is the innermost positive loop, independently
           * of the order in which removal regions were supplied. */
          if (outer_area < owner_area) {
            owner = &outer;
            owner_area = outer_area;
          }
        }
      }
      if (!owner) {
        throw std::runtime_error("Native planar difference has an uncontained hole");
      }
      owner->holes.push_back(hole);
    }
    if (faces.empty()) {
      return Model{};
    }
    arrangement->faces = std::move(faces);
    auto result = boolean_sew_faces(std::span<const Model>(&*arrangement, 1),
                                    tolerance, false, &message);
    if (!result) {
      throw std::runtime_error(message);
    }
    return result;
  }
  catch (const std::exception &exception) {
    if (error) {
      *error = exception.what();
    }
    return {};
  }
}

}  // namespace blender::solid
