/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>

using namespace blender::solid;

static void require(bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main()
{
  std::string error;
  Model rectangle;
  Selection selection;
  const Vec3 corners[] = {{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, {-2, 2, 0}};
  Id first = 0, previous = 0;
  for (int i = 0; i < 4; i++) {
    const auto c = line(corners[i], corners[(i + 1) % 4], &error);
    require(bool(c), error);
    require(
        append_curve(rectangle, *c, 0, 1, previous, i == 3 ? first : 0, false, &selection, &error),
        error);
    if (!first)
      first = rectangle.edges.back().vertices[0];
    previous = rectangle.edges.back().vertices[1];
  }
  Selection normalized = normalize_selection(rectangle, selection);
  Affine translation;
  translation.translation = {3, 2, 1};
  const auto moved = transform_selected(rectangle, normalized, translation, &error);
  require(bool(moved), error);
  require(moved->model.vertices.front().position == Vec3{1, 0, 1},
          "Affine edit rounded the source incorrectly");
  Selection partial;
  partial.vertices.push_back(rectangle.vertices.front().id);
  const auto endpoint = transform_selected(rectangle, partial, translation, &error);
  require(bool(endpoint), error);
  require(validate(endpoint->model, &error), error);
  const Model original = rectangle;
  const auto profile = prepare_profile(rectangle, selection, &error);
  require(bool(profile), error);
  for (double distance : {3.0, -3.0}) {
    const auto result = extrude(*profile, distance, &error);
    require(bool(result), error);
    require(result->model.faces.size() == 6 && result->model.vertices.size() == 8 &&
                result->model.edges.size() == 12,
            "Wrong box topology");
    require(validate_body(result->model, result->selection.active, &error), error);
    for (const Face &side : result->model.faces) {
      require(surface(result->model, side.surface)->kind == SurfaceKind::Plane,
              "A straight extrusion wall must be an editable plane");
      Selection selected_side;
      selected_side.mode = 4;
      selected_side.faces.push_back(side.id);
      const auto side_profile = prepare_profile(result->model, selected_side, &error);
      require(bool(side_profile), error);
      require(bool(extrude(*side_profile, 1, &error)), error);
    }

    const auto moved_cap = transform_selected(result->model, result->selection, translation, &error);
    require(bool(moved_cap), error);
    require(validate_body(moved_cap->model, result->selection.active, &error), error);
    Selection all;
    for (const Vertex &v : result->model.vertices)
      all.vertices.push_back(v.id);
    Affine reflection;
    reflection.columns[0] = {-2, 0, 0};
    const auto reflected = transform_selected(result->model, all, reflection, &error);
    require(bool(reflected), error);
    require(validate_body(reflected->model, result->selection.active, &error), error);

    const auto again = prepare_profile(result->model, result->selection, &error);
    require(bool(again), error);
    require(again->normal.z == (distance > 0 ? 1 : -1), "Inward cap orientation");
    require(bool(extrude(*again, 1, &error)), error);
    require(!extrude(*again, -1, &error), "Unsupported inward cut was silently accepted");
  }
  require(rectangle.next_id == original.next_id && rectangle.faces.empty(), "Source was mutated");
  Model disc;
  Selection ring;
  const auto c = circle({0, 0, 0}, {2, 0, 0}, {0, 0, 1}, &error);
  require(bool(c), error);
  require(append_curve(disc, *c, 0, 2 * std::numbers::pi, 0, 0, true, &ring, &error), error);
  Model wound;
  Selection wound_selection;
  require(append_curve(wound, *c, 0, 4 * std::numbers::pi, 0, 0, true, &wound_selection, &error),
          error);
  require(!prepare_profile(wound, wound_selection, &error),
          "A twice-wound conic became a profile");
  const auto disc_profile = prepare_profile(disc, ring, &error);
  require(bool(disc_profile), error);
  const auto cylinder = extrude(*disc_profile, 3, &error);
  require(bool(cylinder), error);
  require(cylinder->model.faces.size() == 3, "An analytic cylinder needs two planes and one wall");
  require(disc.curves.size() == 1 && disc.curves[0].kind == CurveKind::Conic,
          "Circle did not retain one analytic entity");
  Model bad_plane = profile->model;
  bad_plane.surfaces.front().origin.z += 0.1;
  require(!validate(bad_plane, &error), "Boundary curves detached from their plane were accepted");
  Model bad_wall = cylinder->model;
  for (auto &s : bad_wall.surfaces)
    if (s.kind == SurfaceKind::Extrusion) {
      s.u.x += 0.5;
      break;
    }
  require(!validate(bad_wall, &error), "A curved wall detached from its cap was accepted");
  const auto display = tessellate(cylinder->model, 64, &error);
  require(bool(display), error);
  require(display->polygons.size() == 66,
          "Cylinder display should use boundary strips and two caps");
  double volume = 0;
  for (const auto &polygon : display->polygons) {
    for (size_t i = 1; i + 1 < polygon.size(); i++)
      volume += dot(display->positions[polygon[0]],
                    cross(display->positions[polygon[i]], display->positions[polygon[i + 1]])) /
                6;
  }
  require(std::abs(volume - 12 * std::numbers::pi) < .1, "Wrong display volume or winding");
  const auto bytes = encode(cylinder->model, cylinder->selection);
  const auto restored = decode(bytes.data(), bytes.size(), &error);
  require(bool(restored), error);
  require(encode(restored->model, restored->selection) == bytes, "Source round trip changed data");
  auto damaged = bytes;
  damaged[damaged.size() / 2] ^= 1;
  require(!decode(damaged.data(), damaged.size(), &error), "Corrupt source was accepted");
  require(!decode(bytes.data(), bytes.size() - 1, &error), "Truncated source was accepted");
  Model crossed;
  Selection crossed_selection;
  const Vec3 stroke[] = {{0, 0, 0}, {3, 3, 0}, {0, 3, 0}, {2, 0, 0}};
  Id previous_cross = 0, first_cross = 0;
  for (int i = 0; i < 4; ++i) {
    const auto segment = line(stroke[i], stroke[(i + 1) % 4], &error);
    require(bool(segment), error);
    require(append_curve(crossed,
                         *segment,
                         0,
                         1,
                         previous_cross,
                         i == 3 ? first_cross : 0,
                         false,
                         &crossed_selection,
                         &error),
            error);
    if (!first_cross)
      first_cross = crossed.edges.back().vertices[0];
    previous_cross = crossed.edges.back().vertices[1];
  }
  require(!prepare_profile(crossed, crossed_selection, &error),
          "A self-crossed straight boundary became a solid");
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 10000; i++) {
    const auto result = extrude(*profile, 1 + i * .0001, &error);
    require(bool(result), error);
  }
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
  std::cout << "Independent analytic geometry checks passed; box extrusion: " << elapsed / 10000
            << " ms/op (10000 iterations)\n";
}
