/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_boolean.hh"
#include "SOL_partition.hh"
#include "boolean_pave.hh"
#include "id_index.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace blender::solid {
namespace {
struct BooleanFailure : std::runtime_error {
  BooleanStatus status;
  BooleanFailure(BooleanStatus status, const std::string &message)
      : std::runtime_error(message), status(status)
  {
  }
};

using Ring = std::vector<Vec3>;
struct Plane {
  Vec3 origin, normal;
};
struct Region {
  Plane plane;
  std::vector<Ring> rings;
  int operand;
};
struct Patch {
  Plane plane;
  Ring points;
};
enum class Location { Outside, Inside, Boundary };

Vec3 normalized(Vec3 v)
{
  const double size = length(v);
  if (!(size > 0) || !std::isfinite(size))
    throw std::runtime_error("Invalid native Boolean support direction");
  return v / size;
}
double distance(const Plane &plane, Vec3 p)
{
  return dot(p - plane.origin, plane.normal);
}
Location contains(const Region &face, Vec3 p, double epsilon)
{
  const Vec3 n = face.plane.normal;
  const int drop = std::abs(n.x) > std::abs(n.y) ? (std::abs(n.x) > std::abs(n.z) ? 0 : 2) :
                                                   (std::abs(n.y) > std::abs(n.z) ? 1 : 2);
  const int x = (drop + 1) % 3, y = (drop + 2) % 3;
  bool inside = false;
  for (const Ring &ring : face.rings) {
    for (size_t i = 0; i < ring.size(); ++i) {
      const Vec3 a = ring[i], b = ring[(i + 1) % ring.size()], ab = b - a;
      const double t = dot(p - a, ab) / dot(ab, ab);
      if (t >= 0 && t <= 1 && length(p - a - ab * t) <= epsilon)
        return Location::Boundary;
      if ((a[y] > p[y]) != (b[y] > p[y]) &&
          p[x] < a[x] + (b[x] - a[x]) * ((p[y] - a[y]) / (b[y] - a[y])))
        inside = !inside;
    }
  }
  return inside ? Location::Inside : Location::Outside;
}

std::vector<Region> read_body(const Model &model, std::span<const Id> ids, int operand)
{
  if (ids.empty())
    return {};
  std::vector<Region> result;
  for (Id id : ids) {
    const Face *face = solid::face(model, id);
    if (!face)
      throw std::runtime_error("Boolean operand references a missing face");
    const Surface *support = surface(model, face->surface);
    if (!support || support->kind != SurfaceKind::Plane)
      throw std::runtime_error("Native Boolean curved-face splitting is not implemented yet");
    Region region{{support->origin,
                   normalized(cross(normalized(support->u), normalized(support->v))) *
                       double(face->orientation)},
                  {},
                  operand};
    auto read_loop = [&](const std::vector<Coedge> &loop) {
      Ring ring;
      for (const Coedge use : loop) {
        const Edge *e = edge(model, use.edge);
        const Curve *c = e ? curve(model, e->curve) : nullptr;
        if (!c || c->kind != CurveKind::Line)
          throw std::runtime_error(
              "Native Boolean curved-boundary splitting is not implemented yet");
        ring.push_back(vertex(model, e->vertices[use.reversed ? 1 : 0])->position);
      }
      if (ring.size() < 3)
        throw std::runtime_error("Native Boolean received an incomplete face loop");
      region.rings.push_back(std::move(ring));
    };
    read_loop(face->outer);
    for (const auto &hole : face->holes)
      read_loop(hole);
    result.push_back(std::move(region));
  }
  return result;
}

bool occupied(const std::vector<Region> &body, Vec3 point, Vec3 side, double epsilon)
{
  for (const Region &face : body) {
    if (std::abs(distance(face.plane, point)) <= epsilon &&
        contains(face, point, epsilon) != Location::Outside)
    {
      const double crossing = dot(face.plane.normal, side);
      if (std::abs(crossing) <= 1e-12)
        throw BooleanFailure(BooleanStatus::NumericalUncertainty,
                             "Native Boolean encountered an unresolved boundary tangency");
      return crossing < 0;
    }
  }
  // Retry a ray which hits a source edge rather than assigning an arbitrary side.
  for (const Vec3 direction : {Vec3{.371, .529, .763},
                               Vec3{.813, .217, .519},
                               Vec3{.173, .947, .271},
                               Vec3{-.619, .419, .677}})
  {
    size_t crossings = 0;
    bool retry = false;
    for (const Region &face : body) {
      const double denominator = dot(direction, face.plane.normal);
      if (std::abs(denominator) < 1e-14)
        continue;
      const double t = -distance(face.plane, point) / denominator;
      if (t <= epsilon)
        continue;
      const Location hit = contains(face, point + direction * t, epsilon);
      if (hit == Location::Boundary) {
        retry = true;
        break;
      }
      crossings += hit == Location::Inside;
    }
    if (!retry)
      return crossings % 2 != 0;
  }
  throw BooleanFailure(BooleanStatus::NumericalUncertainty,
                       "Native Boolean could not classify a face piece reliably");
}

bool combine(bool a, bool b, BooleanOperation operation)
{
  switch (operation) {
    case BooleanOperation::Union:
      return a || b;
    case BooleanOperation::Subtract:
      return a && !b;
    case BooleanOperation::Intersect:
      return a && b;
  }
  return false;
}

void split(const Ring &ring, const Plane &plane, double epsilon, std::vector<Ring> &output)
{
  bool positive = false, negative = false;
  for (Vec3 p : ring) {
    const double d = distance(plane, p);
    positive |= d > epsilon;
    negative |= d < -epsilon;
  }
  if (!positive || !negative) {
    output.push_back(ring);
    return;
  }
  // Use the bounded native face splitter for transverse cuts. Contacts at a
  // pre-existing vertex retain the tolerance-aware ring path below until the
  // shared coincidence stage can reconcile their vertex identities.
  const bool transverse = std::all_of(
      ring.begin(), ring.end(), [&](Vec3 p) { return std::abs(distance(plane, p)) > epsilon; });
  if (transverse) {
    Model cell;
    Vec3 normal{};
    for (size_t i = 0; i < ring.size(); ++i) {
      cell.vertices.push_back({cell.next_id++, ring[i]});
      normal = normal + cross(ring[i] - ring[0], ring[(i + 1) % ring.size()] - ring[0]);
    }
    normal = normalized(normal);
    Surface support;
    support.id = cell.next_id++;
    support.origin = ring[0];
    support.u = normalized(ring[1] - ring[0]);
    support.v = cross(normal, support.u);
    cell.surfaces.push_back(support);
    Face boundary;
    boundary.id = cell.next_id++;
    boundary.surface = support.id;
    for (size_t i = 0; i < ring.size(); ++i) {
      const Id cid = cell.next_id++, eid = cell.next_id++;
      cell.curves.push_back(
          {cid, CurveKind::Line, ring[i], ring[(i + 1) % ring.size()] - ring[i]});
      cell.edges.push_back(
          {eid, cid, {cell.vertices[i].id, cell.vertices[(i + 1) % ring.size()].id}, 0, 1});
      boundary.outer.push_back({eid, false});
    }
    cell.faces.push_back(boundary);
    Surface cutter;
    cutter.origin = plane.origin;
    cutter.u = normalized(
        cross(plane.normal, std::abs(plane.normal.x) < .8 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    cutter.v = cross(plane.normal, cutter.u);
    std::string error;
    auto pieces = boolean_split_planar_face(cell, boundary.id, cutter, &error);
    if (!pieces)
      throw std::runtime_error(error);
    for (const Face &piece : pieces->model.faces) {
      Ring points;
      for (Coedge use : piece.outer) {
        const Edge &e = *edge(pieces->model, use.edge);
        points.push_back(vertex(pieces->model, e.vertices[use.reversed ? 1 : 0])->position);
      }
      output.push_back(std::move(points));
    }
    return;
  }
  Ring front, back;
  for (size_t i = 0; i < ring.size(); ++i) {
    const Vec3 a = ring[i], b = ring[(i + 1) % ring.size()];
    const double da = distance(plane, a), db = distance(plane, b);
    if (da >= -epsilon)
      front.push_back(a);
    if (da <= epsilon)
      back.push_back(a);
    if ((da > epsilon && db < -epsilon) || (da < -epsilon && db > epsilon)) {
      const Vec3 p = a + (b - a) * (da / (da - db));
      front.push_back(p);
      back.push_back(p);
    }
  }
  if (front.size() >= 3)
    output.push_back(std::move(front));
  if (back.size() >= 3)
    output.push_back(std::move(back));
}

bool coplanar(const Plane &a, const Plane &b, double epsilon)
{
  return length(cross(a.normal, b.normal)) < 1e-12 && std::abs(distance(a, b.origin)) <= epsilon;
}

std::vector<Patch> partition(const std::vector<Region> &all,
                             const std::vector<Region> &a,
                             const std::vector<Region> &b,
                             BooleanOperation operation,
                             double epsilon)
{
  std::vector<Patch> output;
  for (const Region &face : all) {
    const Vec3 n = face.plane.normal, origin = face.rings[0][0];
    const Vec3 u = normalized(face.rings[0][1] - origin), v = cross(n, u);
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    for (const Ring &ring : face.rings)
      for (Vec3 p : ring) {
        const double x = dot(p - origin, u), y = dot(p - origin, v);
        xmin = std::min(xmin, x);
        xmax = std::max(xmax, x);
        ymin = std::min(ymin, y);
        ymax = std::max(ymax, y);
      }
    std::vector<Ring> pieces{{origin + u * xmin + v * ymin,
                              origin + u * xmax + v * ymin,
                              origin + u * xmax + v * ymax,
                              origin + u * xmin + v * ymax}};
    std::vector<Plane> cuts;
    for (const Region &other : all) {
      if (coplanar(face.plane, other.plane, epsilon)) {
        for (const Ring &ring : other.rings)
          for (size_t i = 0; i < ring.size(); ++i)
            cuts.push_back({ring[i], normalized(cross(ring[(i + 1) % ring.size()] - ring[i], n))});
      }
      else if (other.operand != face.operand)
        cuts.push_back(other.plane);
    }
    for (const Plane &cut : cuts) {
      std::vector<Ring> next;
      for (const Ring &piece : pieces)
        split(piece, cut, epsilon, next);
      pieces = std::move(next);
      if (pieces.size() > 100000)
        throw std::runtime_error("Native Boolean face arrangement exceeded its capacity");
    }
    for (Ring &piece : pieces) {
      Vec3 center{};
      for (Vec3 p : piece)
        center = center + p;
      center = center / double(piece.size());
      if (contains(face, center, epsilon) != Location::Inside)
        continue;
      // Coincident result faces are owned by the first operand exactly once.
      if (face.operand == 1 && std::any_of(a.begin(), a.end(), [&](const Region &other) {
            return coplanar(face.plane, other.plane, epsilon) &&
                   contains(other, center, epsilon) == Location::Inside;
          }))
        continue;
      const bool minus = combine(
          occupied(a, center, n * -1, epsilon), occupied(b, center, n * -1, epsilon), operation);
      const bool plus = combine(
          occupied(a, center, n, epsilon), occupied(b, center, n, epsilon), operation);
      if (minus == plus)
        continue;
      Plane support = face.plane;
      if (plus) {
        std::reverse(piece.begin(), piece.end());
        support.normal = n * -1;
      }
      output.push_back({support, std::move(piece)});
    }
  }
  return output;
}

Model assemble(const std::vector<Patch> &patches, double epsilon)
{
  struct Group {
    Plane plane;
    std::set<std::pair<int, int>> edges;
  };
  std::vector<Vec3> positions;
  auto vertex_index = [&](Vec3 p) {
    for (size_t i = 0; i < positions.size(); ++i)
      if (length(positions[i] - p) <= epsilon)
        return int(i);
    positions.push_back(p);
    return int(positions.size() - 1);
  };
  std::vector<std::vector<int>> rings;
  for (const Patch &patch : patches) {
    std::vector<int> ids;
    for (Vec3 p : patch.points) {
      const int id = vertex_index(p);
      if (ids.empty() || ids.back() != id)
        ids.push_back(id);
    }
    if (ids.size() > 1 && ids.front() == ids.back())
      ids.pop_back();
    rings.push_back(std::move(ids));
  }
  std::vector<Group> groups;
  for (size_t pi = 0; pi < patches.size(); ++pi) {
    const Patch &patch = patches[pi];
    auto group = std::find_if(groups.begin(), groups.end(), [&](const Group &g) {
      return dot(g.plane.normal, patch.plane.normal) > 1 - 1e-12 &&
             coplanar(g.plane, patch.plane, epsilon);
    });
    if (group == groups.end()) {
      groups.push_back({patch.plane, {}});
      group = groups.end() - 1;
    }
    const auto &ring = rings[pi];
    for (size_t j = 0; j < ring.size(); ++j) {
      const int ai = ring[j], bi = ring[(j + 1) % ring.size()];
      const Vec3 a = positions[ai], d = positions[bi] - a;
      const double dd = dot(d, d);
      if (dd == 0)
        continue;
      std::vector<BooleanPave> points;
      for (size_t k = 0; k < positions.size(); ++k) {
        const double t = dot(positions[k] - a, d) / dd;
        if (t > 0 && t < 1 && length(positions[k] - a - d * t) <= epsilon)
          points.push_back({Id(k + 1), t});
      }
      std::string split_error;
      const auto blocks = boolean_pave_blocks(
          Id(j + 1), {Id(ai + 1), 0}, {Id(bi + 1), 1}, points, &split_error);
      if (!blocks)
        throw std::runtime_error(split_error);
      for (const BooleanPaveBlock &block : *blocks) {
        const std::pair<int, int> e{int(block.first.vertex - 1), int(block.last.vertex - 1)};
        if (!group->edges.erase({e.second, e.first}))
          group->edges.insert(e);
      }
    }
  }
  struct Boundary {
    Plane plane;
    std::vector<std::vector<int>> loops;
  };
  std::vector<Boundary> boundaries;
  std::map<int, std::set<int>> neighbors;
  for (Group &group : groups) {
    Boundary boundary{group.plane, {}};
    while (!group.edges.empty()) {
      const int start = group.edges.begin()->first;
      int current = start;
      std::vector<int> loop;
      do {
        auto it = group.edges.lower_bound({current, -1});
        if (it == group.edges.end() || it->first != current)
          throw std::runtime_error("Native Boolean boundary could not be closed");
        auto next = std::next(it);
        if (next != group.edges.end() && next->first == current)
          throw std::runtime_error("Native Boolean result has a non-manifold face junction");
        const int end = it->second;
        loop.push_back(current);
        neighbors[current].insert(end);
        neighbors[end].insert(current);
        group.edges.erase(it);
        current = end;
      } while (current != start);
      if (loop.size() >= 3)
        boundary.loops.push_back(std::move(loop));
    }
    boundaries.push_back(std::move(boundary));
  }
  Model model;
  std::map<int, Id> vertices;
  std::map<std::pair<int, int>, Id> edges;
  auto append_loop = [&](const std::vector<int> &ring) {
    std::vector<Coedge> loop;
    for (size_t i = 0; i < ring.size(); ++i) {
      const int a = ring[i], b = ring[(i + 1) % ring.size()];
      for (const int index : {a, b})
        if (!vertices.contains(index)) {
          const Id id = model.next_id++;
          vertices[index] = id;
          model.vertices.push_back({id, positions[index]});
        }
      const auto key = std::minmax(a, b);
      if (!edges.contains(key)) {
        const Id cid = model.next_id++, eid = model.next_id++;
        model.curves.push_back({cid,
                                CurveKind::Line,
                                positions[key.first],
                                positions[key.second] - positions[key.first]});
        model.edges.push_back({eid, cid, {vertices[key.first], vertices[key.second]}, 0, 1});
        edges[key] = eid;
      }
      loop.push_back({edges[key], a > b});
    }
    return loop;
  };
  for (Boundary &boundary : boundaries) {
    for (auto &loop : boundary.loops) {
      bool changed = true;
      while (changed && loop.size() > 3) {
        changed = false;
        for (size_t i = 0; i < loop.size(); ++i) {
          const int current = loop[i];
          const Vec3 a = positions[loop[(i + loop.size() - 1) % loop.size()]],
                     p = positions[current], b = positions[loop[(i + 1) % loop.size()]];
          if (neighbors[current].size() == 2 &&
              length(cross(p - a, b - p)) <= epsilon * (length(p - a) + length(b - p)) &&
              dot(p - a, b - p) > 0)
          {
            loop.erase(loop.begin() + i);
            changed = true;
            break;
          }
        }
      }
    }
    auto area = [&](const std::vector<int> &loop) {
      Vec3 sum{};
      for (size_t i = 0; i < loop.size(); ++i)
        sum = sum + cross(positions[loop[i]] - boundary.plane.origin,
                          positions[loop[(i + 1) % loop.size()]] - boundary.plane.origin);
      return dot(sum, boundary.plane.normal);
    };
    for (const auto &outer : boundary.loops) {
      if (area(outer) <= epsilon * epsilon)
        continue;
      Surface support;
      support.id = model.next_id++;
      support.kind = SurfaceKind::Plane;
      support.origin = boundary.plane.origin;
      support.u = normalized(positions[outer[1]] - positions[outer[0]]);
      support.v = cross(boundary.plane.normal, support.u);
      model.surfaces.push_back(support);
      Face face;
      face.id = model.next_id++;
      face.surface = support.id;
      face.outer = append_loop(outer);
      Region container{boundary.plane, {{}}, 0};
      for (int i : outer)
        container.rings[0].push_back(positions[i]);
      for (const auto &hole : boundary.loops)
        if (area(hole) < -epsilon * epsilon &&
            contains(container, positions[hole.front()], epsilon) != Location::Outside)
          face.holes.push_back(append_loop(hole));
      model.faces.push_back(std::move(face));
    }
  }
  return model;
}

class StageTimer {
  BooleanDiagnostics &diagnostics_;
  BooleanStage stage_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

 public:
  StageTimer(BooleanDiagnostics &diagnostics, BooleanStage stage)
      : diagnostics_(diagnostics), stage_(stage)
  {
  }
  ~StageTimer()
  {
    diagnostics_.timings.push_back(
        {stage_,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
             .count()});
  }
};

/** Check the actual selected boundary, including sense, rather than repeatedly walking the
 * entire source from every selected face. A periodic seam can have two uses on one face. */
void check_closed_boundary(const Model &model,
                           std::span<const Id> ids,
                           BooleanStatus failure_status)
{
  struct Uses {
    int count = 0, sense = 0;
  };
  std::map<Id, Uses> uses;
  std::set<Id> unique;
  const IdIndex<Face> faces(model.faces);
  for (Id id : ids) {
    if (!faces.contains(id) || !unique.insert(id).second)
      throw BooleanFailure(failure_status, "Boolean boundary has a missing or repeated face");
    const Face *f = faces.at(id);
    auto loop = [&](const std::vector<Coedge> &boundary) {
      for (const Coedge use : boundary) {
        Uses &entry = uses[use.edge];
        ++entry.count;
        entry.sense += f->orientation * (use.reversed ? -1 : 1);
      }
    };
    loop(f->outer);
    for (const auto &hole : f->holes)
      loop(hole);
  }
  for (const auto &[id, entry] : uses) {
    if (entry.count != 2 || entry.sense != 0)
      throw BooleanFailure(failure_status,
                           "Boolean boundary must be complete, closed and consistently oriented");
  }
}
}  // namespace

const char *boolean_status_name(BooleanStatus status)
{
  switch (status) {
    case BooleanStatus::Success:
      return "success";
    case BooleanStatus::Empty:
      return "empty";
    case BooleanStatus::Unsupported:
      return "unsupported";
    case BooleanStatus::InvalidInput:
      return "invalid_input";
    case BooleanStatus::NumericalUncertainty:
      return "numerical_uncertainty";
    case BooleanStatus::Failed:
      return "failed";
  }
  return "unknown";
}
const char *boolean_path_name(BooleanPath path)
{
  switch (path) {
    case BooleanPath::None:
      return "none";
    case BooleanPath::EmptyOperand:
      return "empty_operand";
    case BooleanPath::Planar:
      return "planar";
    case BooleanPath::CurvedPlanar:
      return "curved_planar";
  }
  return "unknown";
}
const char *boolean_stage_name(BooleanStage stage)
{
  switch (stage) {
    case BooleanStage::InputValidation:
      return "input_validation";
    case BooleanStage::OperandValidation:
      return "operand_validation";
    case BooleanStage::Preparation:
      return "preparation";
    case BooleanStage::CurvedPlanarSolve:
      return "curved_planar_solve";
    case BooleanStage::PartitionClassification:
      return "partition_classification";
    case BooleanStage::Reconstruction:
      return "reconstruction";
    case BooleanStage::OutputValidation:
      return "output_validation";
  }
  return "unknown";
}

BooleanResult boolean_bodies_result(const Model &source,
                                    std::span<const Id> first,
                                    std::span<const Id> second,
                                    BooleanOperation operation)
{
  BooleanResult result;
  auto &diagnostics = result.diagnostics;
  diagnostics.first_face_count = first.size();
  diagnostics.second_face_count = second.size();
  diagnostics.timings.reserve(6);
  try {
    {
      StageTimer timer(diagnostics, BooleanStage::InputValidation);
      if (operation != BooleanOperation::Union && operation != BooleanOperation::Subtract &&
          operation != BooleanOperation::Intersect)
        throw BooleanFailure(BooleanStatus::InvalidInput, "Unknown native Boolean operation");
      std::string error;
      if (!validate(source, &error))
        throw BooleanFailure(BooleanStatus::InvalidInput, error);
    }
    {
      StageTimer timer(diagnostics, BooleanStage::OperandValidation);
      check_closed_boundary(source, first, BooleanStatus::InvalidInput);
      check_closed_boundary(source, second, BooleanStatus::InvalidInput);
    }
    std::optional<Model> output;
    std::string error;
    if (first.empty() || second.empty()) {
      diagnostics.path = BooleanPath::EmptyOperand;
      StageTimer timer(diagnostics, BooleanStage::Reconstruction);
      if (operation == BooleanOperation::Intersect ||
          (operation == BooleanOperation::Subtract && first.empty()))
        output = Model{};
      else {
        const auto remaining = first.empty() ? second : first;
        Model piece = source;
        piece.faces.clear();
        for (Id id : remaining) {
          piece.faces.push_back(*face(source, id));
          piece.faces.back().group = 0;
        }
        output = boolean_sew_faces(std::span<const Model>(&piece, 1), 0, true, &error);
      }
    }
    else {
      std::vector<Region> a, b, all;
      double epsilon = 0;
      bool first_planar, second_planar;
      {
        StageTimer timer(diagnostics, BooleanStage::Preparation);
        auto planar_linear = [&](std::span<const Id> ids) {
          for (Id id : ids) {
            const Face &f = *face(source, id);
            if (surface(source, f.surface)->kind != SurfaceKind::Plane)
              return false;
            auto linear = [&](const std::vector<Coedge> &loop) {
              return std::all_of(loop.begin(), loop.end(), [&](Coedge use) {
                return curve(source, edge(source, use.edge)->curve)->kind == CurveKind::Line;
              });
            };
            if (!linear(f.outer) || !std::all_of(f.holes.begin(), f.holes.end(), linear))
              return false;
          }
          return true;
        };
        first_planar = planar_linear(first);
        second_planar = planar_linear(second);
        if (!first_planar && !second_planar)
          throw BooleanFailure(
              BooleanStatus::Unsupported,
              "Native Boolean requires general curved surface partitioning for this operation");
        if (first_planar && second_planar) {
          diagnostics.path = BooleanPath::Planar;
          a = read_body(source, first, 0);
          b = read_body(source, second, 1);
          all = a;
          all.insert(all.end(), b.begin(), b.end());
          double scale = 1;
          for (const Region &face : all)
            for (const Ring &ring : face.rings)
              for (Vec3 p : ring)
                scale = std::max(scale, length(p));
          epsilon = 256 * std::numeric_limits<double>::epsilon() * scale;
        }
        else
          diagnostics.path = BooleanPath::CurvedPlanar;
      }
      if (diagnostics.path == BooleanPath::CurvedPlanar) {
        /* Existing helper owns its complete partition and reconstruction pipeline. Keep one
         * honest aggregate measurement until those helpers expose their individual stages. */
        StageTimer timer(diagnostics, BooleanStage::CurvedPlanarSolve);
        if (second_planar)
          output = boolean_with_convex_planar_cutter(source, first, second, operation, &error);
        else if (operation == BooleanOperation::Subtract)
          output = boolean_planar_minus_body(source, first, second, &error);
        else
          output = boolean_with_convex_planar_cutter(source, second, first, operation, &error);
      }
      else {
        std::vector<Patch> patches;
        {
          StageTimer timer(diagnostics, BooleanStage::PartitionClassification);
          patches = partition(all, a, b, operation, epsilon);
        }
        {
          StageTimer timer(diagnostics, BooleanStage::Reconstruction);
          output = assemble(patches, epsilon);
        }
      }
    }
    if (!output)
      throw BooleanFailure(BooleanStatus::Failed,
                           error.empty() ? "Native Boolean failed without a result" : error);
    {
      StageTimer timer(diagnostics, BooleanStage::OutputValidation);
      if (!validate(*output, &error))
        throw BooleanFailure(BooleanStatus::Failed, error);
      std::vector<Id> ids;
      ids.reserve(output->faces.size());
      for (const Face &f : output->faces)
        ids.push_back(f.id);
      check_closed_boundary(*output, ids, BooleanStatus::Failed);
      if (ids.empty() && (!output->vertices.empty() || !output->edges.empty() ||
                          !output->curves.empty() || !output->surfaces.empty()))
        throw BooleanFailure(
            BooleanStatus::Failed,
            "Native Boolean returned unattached geometry instead of an empty body");
      if (ids.empty() && operation == BooleanOperation::Union &&
          (!first.empty() || !second.empty()))
        throw BooleanFailure(BooleanStatus::Failed,
                             "Native Boolean Union discarded every nonempty input boundary");
    }
    diagnostics.result_face_count = output->faces.size();
    result.status = output->faces.empty() ? BooleanStatus::Empty : BooleanStatus::Success;
    result.model = std::move(output);
  }
  catch (const BooleanFailure &failure) {
    result.status = failure.status;
    result.message = failure.what();
  }
  catch (const std::exception &failure) {
    result.status = BooleanStatus::Failed;
    result.message = failure.what();
  }
  return result;
}

std::optional<Model> boolean_bodies(const Model &source,
                                    std::span<const Id> first,
                                    std::span<const Id> second,
                                    BooleanOperation operation,
                                    std::string *error)
{
  BooleanResult result = boolean_bodies_result(source, first, second, operation);
  if (error)
    *error = result.message;
  return std::move(result.model);
}
}  // namespace blender::solid
