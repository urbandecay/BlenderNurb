/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_cylinder_unify.hh"
#include "SOL_boolean.hh"
#include "SOL_cells.hh"
#include "SOL_partition.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>

using namespace blender::solid;

static void require(const bool condition, const std::string &message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static Model cylinder(const double height)
{
  Model model;
  Selection selected;
  std::string error;
  Curve circle{0, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}};
  require(append_curve(model, circle, 0, 2 * std::numbers::pi, 0, 0, true, &selected, &error), error);
  const auto profile = prepare_profile(model, selected, &error);
  require(bool(profile), error);
  const auto body = extrude(*profile, height, &error);
  require(bool(body), error);
  const auto native = boolean_partition_periodic_faces(body->model, &error);
  require(bool(native), error);
  return *native;
}

static Surface plane(const Vec3 origin, const Vec3 u, const Vec3 v)
{
  Surface result;
  result.origin = origin;
  result.u = u;
  result.v = v;
  return result;
}

static size_t walls(const Model &model)
{
  return size_t(std::count_if(model.faces.begin(), model.faces.end(), [&](const Face &f) {
    return surface(model, f.surface)->kind == SurfaceKind::Cylinder;
  }));
}

static void closed(const Model &model)
{
  std::string error;
  require(validate(model, &error), error);
  for (const Face &f : model.faces) {
    require(validate_body(model, f.id, &error), error);
    require(f.group == 0, "Cylinder unification must remove partitions, not hide them in face groups");
  }
}

static Model partition_assemble(const Model &source,
                                const std::vector<Surface> &planes,
                                const bool all)
{
  std::string error;
  const auto saved = encode(source, {});
  const auto cells = boolean_partition_body(source, planes, 1e-12, 4096, &error);
  require(bool(cells), "Cylinder partition failed: " + error);
  std::vector<Model> keep;
  for (const BooleanCell &cell : *cells) {
    if (all || cell.sides.front() > 0) {
      keep.push_back(cell.model);
    }
  }
  const auto assembled = boolean_assemble_cells(keep, 1e-12, &error);
  require(bool(assembled), "Cylinder cells failed reconstruction: " + error);
  require(saved == encode(source, {}), "Partition/assembly changed the source cylinder");
  return *assembled;
}

static Model unify(const Model &source)
{
  std::string error;
  const auto saved = encode(source, {});
  const auto merged = boolean_unify_cylindrical_faces(source, &error);
  require(bool(merged), "Cylinder unification failed: " + error);
  require(saved == encode(source, {}), "Cylinder unification changed its source");
  closed(*merged);
  return *merged;
}

int main()
{
  std::string error;
  for (const double height : {2., -2.}) {
    const Model original = cylinder(height);
    const std::vector<Surface> cuts{
        plane({.2, 0, 0}, {0, 1, 0}, {0, 0, 1}),
        plane({0, 0, height * .4}, {1, 0, 0}, {0, 1, 0}),
        plane({0, 0, height * .7}, {1, 0, 0}, {0, 1, 0})};
    const Model assembled = partition_assemble(original, cuts, true);
    require(walls(assembled) > 2, "Test did not create artificial cylindrical partitions");
    const Model restored = unify(assembled);
    require(restored.faces.size() == 3 && walls(restored) == 1,
            "Reassembling every cylinder cell must restore one wall and two analytic caps");
    const Face &wall = *std::find_if(restored.faces.begin(), restored.faces.end(), [&](const Face &f) {
      return surface(restored, f.surface)->kind == SurfaceKind::Cylinder;
    });
    require(wall.outer.size() == 4 && wall.holes.empty(),
            "Restored full cylinder wall must have two conics and a twice-used true seam; uses=" +
                std::to_string(wall.outer.size()));
    require(wall.outer[1].edge == wall.outer[3].edge &&
                wall.outer[1].reversed != wall.outer[3].reversed,
            "Full cylindrical wall did not retain a genuine paired periodic seam");
    for (const size_t i : {size_t(0), size_t(2)}) {
      const Edge &e = *edge(restored, wall.outer[i].edge);
      require(curve(restored, e.curve)->kind == CurveKind::Conic &&
                  e.vertices[0] == e.vertices[1] &&
                  std::abs(std::abs(e.end - e.start) - 2 * std::numbers::pi) < 1e-12,
              "Restored cylinder rim lost its closed native conic");
    }
    const auto reopened = boolean_partition_periodic_faces(restored, &error);
    require(reopened && walls(*reopened) == 2,
            "Compact cylinder could not reopen into bounded periodic charts: " + error);
    for (const Face &f : reopened->faces) {
      require(validate_body(*reopened, f.id, &error), error);
    }
    require(bool(tessellate(restored, 24, &error)), "Restored cylinder display failed: " + error);

    const Model partial = partition_assemble(original, cuts, false);
    require(walls(partial) > 1, "Partial cylinder test did not create artificial partitions");
    const Model bounded = unify(partial);
    require(walls(bounded) == 1 && bounded.faces.size() == 4,
            "Bounded partial cylinder must merge its wall and retain three planar boundaries");
    require(bool(tessellate(bounded, 24, &error)), "Bounded cylinder display failed: " + error);

    Model incompatible_seams = assembled;
    // The geometric generators remain exact, but every segment receives its
    // own parameterization. The unifier must retain the component rather than
    // silently rewrite support geometry to invent a compact seam.
    for (Edge &e : incompatible_seams.edges) {
      const Curve &old = *curve(incompatible_seams, e.curve);
      if (old.kind != CurveKind::Line) {
        continue;
      }
      const Vec3 a = vertex(incompatible_seams, e.vertices[0])->position;
      const Vec3 b = vertex(incompatible_seams, e.vertices[1])->position;
      if (a.x != b.x || a.y != b.y) {
        continue;
      }
      Curve replacement;
      replacement.id = incompatible_seams.next_id++;
      replacement.kind = CurveKind::Line;
      replacement.origin = a;
      replacement.u = b - a;
      incompatible_seams.curves.push_back(replacement);
      e.curve = replacement.id;
      e.start = 0;
      e.end = 1;
    }
    const Model retained = unify(incompatible_seams);
    require(walls(retained) == walls(incompatible_seams),
            "An uncertified seam chain must conservatively retain its cylindrical components");

    Model different_frames = original;
    const Face &first_wall = *std::find_if(
        different_frames.faces.begin(), different_frames.faces.end(), [&](const Face &f) {
          return surface(different_frames, f.surface)->kind == SurfaceKind::Cylinder;
        });
    Surface replacement = *surface(different_frames, first_wall.surface);
    replacement.id = different_frames.next_id++;
    replacement.u = replacement.u * -1;
    replacement.v = replacement.v * -1;
    different_frames.surfaces.push_back(replacement);
    // Copy the face index before changing its surface: the other chart keeps
    // the original frame even when the two source faces share a surface ID.
    const size_t first_wall_index = size_t(&first_wall - different_frames.faces.data());
    different_frames.faces[first_wall_index].surface = replacement.id;
    const Model distinct = unify(different_frames);
    require(walls(distinct) == 2,
            "Equivalent cylindrical geometry with distinct frames must not be snapped together");
  }
  require(bool(boolean_unify_cylindrical_faces({}, &error)), "Empty model unification failed");
  Model open = cylinder(2);
  open.faces.pop_back();
  const auto saved = encode(open, {});
  require(!boolean_unify_cylindrical_faces(open, &error) && !error.empty() &&
              saved == encode(open, {}),
          "Open source must fail transactionally");
  std::cout << "Native cylindrical face unification tests passed\n";
}
