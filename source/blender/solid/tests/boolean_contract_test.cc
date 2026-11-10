/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_boolean.hh"
#include "SOL_classify.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <set>

using namespace blender::solid;

static void require(const bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static Model box(const Vec3 offset = {})
{
  Model model;
  Selection selected;
  Id first = 0, previous = 0;
  const Vec3 points[] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
  std::string error;
  for (int i = 0; i < 4; ++i) {
    const Vec3 a = points[i] + offset, b = points[(i + 1) % 4] + offset;
    const auto edge_curve = line(a, b, &error);
    require(bool(edge_curve), error);
    require(append_curve(
                model, *edge_curve, 0, 1, previous, i == 3 ? first : 0, false, &selected, &error),
            error);
    if (!first)
      first = model.edges.back().vertices[0];
    previous = model.edges.back().vertices[1];
  }
  const auto profile = prepare_profile(model, selected, &error);
  require(bool(profile), error);
  const auto body = extrude(*profile, 2, &error);
  require(bool(body), error);
  return body->model;
}

static Model cylinder(const Vec3 center = {})
{
  Model model;
  Selection selected;
  std::string error;
  Curve circle_curve{0, CurveKind::Conic, center, {1, 0, 0}, {0, 1, 0}};
  require(
      append_curve(model, circle_curve, 0, 2 * std::numbers::pi, 0, 0, true, &selected, &error),
      error);
  const auto profile = prepare_profile(model, selected, &error);
  require(bool(profile), error);
  const auto body = extrude(*profile, 2, &error);
  require(bool(body), error);
  return body->model;
}

/* Append a complete native body and return its remapped face IDs. */
static std::vector<Id> append(Model &to, Model from)
{
  const Id offset = to.next_id;
  for (Vertex &v : from.vertices)
    v.id += offset;
  for (Curve &c : from.curves)
    c.id += offset;
  for (Edge &e : from.edges) {
    e.id += offset;
    e.curve += offset;
    for (Id &v : e.vertices)
      v += offset;
  }
  for (Surface &s : from.surfaces) {
    s.id += offset;
    if (s.curve)
      s.curve += offset;
    if (s.other_curve)
      s.other_curve += offset;
  }
  std::vector<Id> faces;
  for (Face &f : from.faces) {
    f.id += offset;
    f.surface += offset;
    if (f.group)
      f.group += offset;
    for (Coedge &e : f.outer)
      e.edge += offset;
    for (auto &hole : f.holes)
      for (Coedge &e : hole)
        e.edge += offset;
    faces.push_back(f.id);
  }
  to.vertices.insert(to.vertices.end(), from.vertices.begin(), from.vertices.end());
  to.curves.insert(to.curves.end(), from.curves.begin(), from.curves.end());
  to.edges.insert(to.edges.end(), from.edges.begin(), from.edges.end());
  to.surfaces.insert(to.surfaces.end(), from.surfaces.begin(), from.surfaces.end());
  to.faces.insert(to.faces.end(), from.faces.begin(), from.faces.end());
  to.next_id = from.next_id + offset;
  return faces;
}

static double volume(const Model &model)
{
  double result = 0;
  for (const Face &f : model.faces) {
    auto add_loop = [&](const auto &ring) {
      const Vec3 reference =
          vertex(model, edge(model, ring[0].edge)->vertices[ring[0].reversed ? 1 : 0])->position;
      for (size_t i = 1; i + 1 < ring.size(); ++i) {
        const Edge *e = edge(model, ring[i].edge), *next = edge(model, ring[i + 1].edge);
        result += dot(reference,
                      cross(
                          vertex(model, e->vertices[ring[i].reversed ? 1 : 0])->position,
                          vertex(model, next->vertices[ring[i + 1].reversed ? 1 : 0])->position)) /
                  6;
      }
    };
    add_loop(f.outer);
    for (const auto &hole : f.holes)
      add_loop(hole);
  }
  return result;
}

static BooleanResult run(const Model &source,
                         const std::vector<Id> &a,
                         const std::vector<Id> &b,
                         const BooleanOperation operation)
{
  return boolean_bodies_result(source, a, b, operation);
}

static void audit_result(const BooleanResult &result, const std::string &case_name)
{
  require(result.diagnostics.timings.size() <= 6, case_name + " reported too many stage timings");
  std::set<int> stages;
  for (const BooleanTiming &timing : result.diagnostics.timings) {
    require(stages.insert(int(timing.stage)).second, case_name + " repeated a timing stage");
    require(std::isfinite(timing.milliseconds) && timing.milliseconds >= 0,
            case_name + " reported an invalid stage time");
  }
  if (result.status == BooleanStatus::Success) {
    require(result.succeeded() && result.model && !result.model->faces.empty(),
            case_name + " has inconsistent Success status/model");
  }
  else if (result.status == BooleanStatus::Empty) {
    require(result.succeeded() && result.model && result.model->faces.empty(),
            case_name + " has inconsistent Empty status/model");
  }
  else {
    require(!result.succeeded() && !result.model,
            case_name + " unsuccessful status unexpectedly contains a model");
  }
}

static void require_success(const BooleanResult &result, const std::string &case_name)
{
  audit_result(result, case_name);
  require(result.status == BooleanStatus::Success && result.succeeded() && result.model,
          case_name + " did not return a successful model: " + result.message);
  std::string error;
  require(validate(*result.model, &error), case_name + " result is invalid: " + error);
  for (const Face &face : result.model->faces)
    require(validate_body(*result.model, face.id, &error),
            case_name + " has invalid body: " + error);
}

static void expect_point(const Model &model, const Vec3 point, const bool inside)
{
  std::string error;
  const auto actual = classify_body_point(model, point, {}, &error);
  require(bool(actual), "Could not classify result point: " + error);
  require(*actual == (inside ? PointClassification::Inside : PointClassification::Outside),
          "Boolean result has incorrect material membership");
}

static const char *status_name(const BooleanStatus status)
{
  switch (status) {
    case BooleanStatus::Success:
      return "success";
    case BooleanStatus::Empty:
      return "empty";
    case BooleanStatus::Unsupported:
      return "unsupported";
    case BooleanStatus::InvalidInput:
      return "invalid input";
    case BooleanStatus::NumericalUncertainty:
      return "numerical uncertainty";
    case BooleanStatus::Failed:
      return "failed";
  }
  return "unknown";
}

/* A deterministic acceptance fixture whose current status is part of the contract. */
static BooleanResult record_fixture(const std::string &name,
                                    const Model &source,
                                    const std::vector<Id> &a,
                                    const std::vector<Id> &b,
                                    const BooleanStatus expected_status)
{
  const auto source_bytes = encode(source, {});
  const BooleanResult result = run(source, a, b, BooleanOperation::Union);
  require(encode(source, {}) == source_bytes, name + " mutated source operands");
  std::cout << "fixture " << name << ": " << status_name(result.status) << '\n';
  audit_result(result, name);
  require(result.status == expected_status,
          name + " expected " + status_name(expected_status) + " but got " +
              status_name(result.status) + ": " + result.message);
  if (result.status == BooleanStatus::Success)
    require_success(result, name);
  return result;
}

static std::vector<Id> face_ids(const Model &model)
{
  std::vector<Id> ids;
  for (const Face &face : model.faces)
    ids.push_back(face.id);
  return ids;
}

int main()
{
  /* Plane/linear operands give exact overlap volumes and unambiguous membership probes. */
  Model planar_source;
  const std::vector<Id> left = append(planar_source, box());
  const std::vector<Id> right = append(planar_source, box({.5, 0, 0}));
  const auto planar_bytes = encode(planar_source, {});
  for (const BooleanOperation operation :
       {BooleanOperation::Union, BooleanOperation::Subtract, BooleanOperation::Intersect})
  {
    const BooleanResult result = run(planar_source, left, right, operation);
    require_success(result, "Overlapping planar boxes");
    const double expected_volume = operation == BooleanOperation::Union    ? 10 :
                                   operation == BooleanOperation::Subtract ? 2 :
                                                                             6;
    require(std::abs(volume(*result.model) - expected_volume) < 1e-9,
            "Planar set operation returned the wrong volume");
    expect_point(*result.model, {-.75, 0, 1}, operation != BooleanOperation::Intersect);
    expect_point(*result.model, {.75, 0, 1}, operation != BooleanOperation::Subtract);
    expect_point(*result.model, {1.25, 0, 1}, operation == BooleanOperation::Union);

    const auto result_bytes = encode(*result.model, {});
    std::string error;
    const auto restored = decode(result_bytes.data(), result_bytes.size(), &error);
    require(bool(restored), "Boolean result failed native encode/decode: " + error);
    require(encode(restored->model, restored->selection) == result_bytes,
            "Boolean result changed during native encode/decode");
  }
  require(encode(planar_source, {}) == planar_bytes, "Planar operations changed source bytes");
  const auto source_roundtrip = decode(planar_bytes.data(), planar_bytes.size());
  require(bool(source_roundtrip) &&
              encode(source_roundtrip->model, source_roundtrip->selection) == planar_bytes,
          "Native Boolean operands failed encode/decode round trip");

  /* Empty intersection is a valid operation outcome, distinct from solver failure. */
  Model disjoint;
  const auto da = append(disjoint, box()), db = append(disjoint, box({4, 0, 0}));
  const auto disjoint_bytes = encode(disjoint, {});
  const BooleanResult empty = run(disjoint, da, db, BooleanOperation::Intersect);
  require(empty.status == BooleanStatus::Empty && empty.succeeded() && empty.model &&
              empty.model->faces.empty(),
          "Disjoint intersection was not reported as intentional Empty");
  audit_result(empty, "Disjoint intersection");
  require(encode(disjoint, {}) == disjoint_bytes, "Empty intersection mutated source bytes");

  /* The empty operand obeys all set identities in either operand position. */
  Model one_body_source;
  const auto one_body = append(one_body_source, box());
  const std::vector<Id> no_faces;
  auto check_identity = [&](const std::vector<Id> &a,
                            const std::vector<Id> &b,
                            const BooleanOperation operation,
                            const bool expect_body,
                            const std::string &label) {
    const BooleanResult result = run(one_body_source, a, b, operation);
    audit_result(result, label);
    require(result.status == (expect_body ? BooleanStatus::Success : BooleanStatus::Empty),
            label + " returned the wrong empty-operand identity status");
    if (expect_body) {
      require_success(result, label);
      require(std::abs(volume(*result.model) - 8) < 1e-9,
              label + " changed the nonempty operand volume");
      expect_point(*result.model, {0, 0, 1}, true);
      expect_point(*result.model, {1.5, 0, 1}, false);
    }
  };
  check_identity(no_faces, one_body, BooleanOperation::Union, true, "Empty union body");
  check_identity(one_body, no_faces, BooleanOperation::Union, true, "Body union empty");
  check_identity(no_faces, one_body, BooleanOperation::Subtract, false, "Empty subtract body");
  check_identity(one_body, no_faces, BooleanOperation::Subtract, true, "Body subtract empty");
  check_identity(no_faces, one_body, BooleanOperation::Intersect, false, "Empty intersect body");
  check_identity(one_body, no_faces, BooleanOperation::Intersect, false, "Body intersect empty");

  /* The same complete body on both sides follows the ordinary set identities. */
  for (const auto &[operation, should_be_body] : {std::pair{BooleanOperation::Union, true},
                                                  std::pair{BooleanOperation::Intersect, true},
                                                  std::pair{BooleanOperation::Subtract, false}})
  {
    const BooleanResult same = run(one_body_source, one_body, one_body, operation);
    audit_result(same, "Self Boolean");
    require(same.status == (should_be_body ? BooleanStatus::Success : BooleanStatus::Empty),
            "Self Boolean violated its set-operation identity");
    if (should_be_body) {
      require_success(same, "Self Boolean");
      require(std::abs(volume(*same.model) - 8) < 1e-9, "Self Boolean changed body volume");
      expect_point(*same.model, {0, 0, 1}, true);
      expect_point(*same.model, {1.5, 0, 1}, false);
    }
  }

  /* Invalid identifiers, duplicate/overlapping face membership, and partial shells fail clearly.
   */
  auto require_invalid = [&](const BooleanResult &result, const std::string &label) {
    audit_result(result, label);
    require(result.status == BooleanStatus::InvalidInput,
            label + " was not rejected as InvalidInput: " + result.message);
  };
  require_invalid(run(planar_source, {999999}, right, BooleanOperation::Union), "Missing face ID");
  auto duplicate = left;
  duplicate.push_back(duplicate.front());
  require_invalid(run(planar_source, duplicate, right, BooleanOperation::Union),
                  "Duplicate face ID");
  require_invalid(run(planar_source, left, {left.front()}, BooleanOperation::Union),
                  "Overlapping operand face sets");
  auto incomplete = left;
  incomplete.pop_back();
  require_invalid(run(planar_source, incomplete, right, BooleanOperation::Union),
                  "Incomplete body face set");
  require_invalid(run(planar_source, left, right, static_cast<BooleanOperation>(255)),
                  "Invalid operation enum");

  const Model zero_source;
  require_invalid(run(zero_source, {1}, {2}, BooleanOperation::Union), "Zero source");
  Model invalid_source = box();
  invalid_source.faces.pop_back();
  require_invalid(run(invalid_source,
                      face_ids(invalid_source),
                      face_ids(invalid_source),
                      BooleanOperation::Union),
                  "Open invalid source");
  Model flipped_source = box();
  flipped_source.faces.front().outer.front().reversed =
      !flipped_source.faces.front().outer.front().reversed;
  const auto flipped_ids = face_ids(flipped_source);
  require_invalid(run(flipped_source, flipped_ids, flipped_ids, BooleanOperation::Union),
                  "Orientation-flipped source face");
  require(encode(planar_source, {}) == planar_bytes,
          "Rejected invalid-input cases mutated source bytes");

  /* General curved/curved overlap is an explicit current limitation and must preserve source. */
  Model curved_source;
  const auto curved_a = append(curved_source, cylinder());
  const auto curved_b = append(curved_source, cylinder({.5, 0, 0}));
  const auto curved_bytes = encode(curved_source, {});
  const BooleanResult curved = run(curved_source, curved_a, curved_b, BooleanOperation::Union);
  audit_result(curved, "Overlapping curved cylinders");
  require(curved.status == BooleanStatus::Unsupported && !curved.succeeded(),
          "Overlapping curved cylinders must remain explicitly Unsupported");
  require(encode(curved_source, {}) == curved_bytes,
          "Rejected curved-cylinder Boolean changed source bytes");

  /* All three cylinders are present in this deterministic chain source. The first fold is
   * intentionally rejected by the current curved/curved coverage boundary. */
  Model chain_source;
  const auto chain_a = append(chain_source, cylinder());
  const auto chain_b = append(chain_source, cylinder({1.25, 0, 0}));
  const auto chain_c = append(chain_source, cylinder({2.5, 0, 0}));
  require(chain_a.size() == 3 && chain_b.size() == 3 && chain_c.size() == 3,
          "Three-cylinder fixture did not include all three complete bodies");
  const BooleanResult chain_first = record_fixture(
      "three-cylinder first fold", chain_source, chain_a, chain_b, BooleanStatus::Unsupported);
  (void)chain_first;

  /* First create a supported curved result by clipping a cylinder with a convex planar box.
   * Then use that Booleaned curved body as an operand with a fresh overlapping cylinder. */
  Model clipped_source;
  const auto clipped_cylinder = append(clipped_source, cylinder());
  const auto clipping_box = append(clipped_source, box({-.5, -.5, 0}));
  const auto clipped_bytes = encode(clipped_source, {});
  const BooleanResult clipped = run(
      clipped_source, clipped_cylinder, clipping_box, BooleanOperation::Intersect);
  audit_result(clipped, "Cylinder clipped by convex planar box");
  require(clipped.status == BooleanStatus::Success,
          "Cylinder/box intersection did not produce the supported Booleaned curved fixture: " +
              clipped.message);
  require_success(clipped, "Cylinder clipped by convex planar box");
  require(volume(*clipped.model) > 0, "Cylinder/box intersection lost all material");
  expect_point(*clipped.model, {0, 0, 1}, true);
  expect_point(*clipped.model, {.75, 0, 1}, false);
  require(encode(clipped_source, {}) == clipped_bytes,
          "Cylinder/box intersection mutated its native source");

  Model replay_source = *clipped.model;
  const auto booleaned_faces = face_ids(replay_source);
  const auto fresh_cylinder = append(replay_source, cylinder({.25, 0, 0}));
  record_fixture("booleaned curved body plus fresh cylinder",
                 replay_source,
                 booleaned_faces,
                 fresh_cylinder,
                 BooleanStatus::Unsupported);

  /* The compatibility entry point must not leave stale error text after success. */
  std::string compatibility_error = "stale failure";
  const auto compatibility = boolean_bodies(
      planar_source, left, right, BooleanOperation::Union, &compatibility_error);
  require(bool(compatibility) && compatibility_error.empty(),
          "Compatibility Boolean did not clear error text after success");
  require(std::abs(volume(*compatibility) - 10) < 1e-9,
          "Compatibility Boolean returned a wrong planar union");

  std::cout << "Native Boolean contract checks passed\n";
}
