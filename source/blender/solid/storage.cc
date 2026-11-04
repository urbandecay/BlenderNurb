/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_geometry.hh"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace blender::solid {
namespace {
constexpr size_t max_records = 1 << 20;
constexpr size_t max_bytes = 128 << 20;
constexpr uint64_t magic = 0x3144494c4f534e41; /* ANSOLID1, little endian. */
uint64_t checksum(const uint8_t *data, size_t size)
{
  uint64_t value = 14695981039346656037ull;
  for (size_t i = 0; i < size; i++)
    value = (value ^ data[i]) * 1099511628211ull;
  return value;
}
struct Writer {
  std::vector<uint8_t> data;
  void integer(uint64_t value)
  {
    for (int i = 0; i < 8; i++)
      data.push_back(uint8_t(value >> (i * 8)));
  }
  void scalar(double value)
  {
    integer(std::bit_cast<uint64_t>(value));
  }
  void point(Vec3 v)
  {
    scalar(v.x);
    scalar(v.y);
    scalar(v.z);
  }
  template<typename T, typename Fn> void sequence(const std::vector<T> &items, Fn write)
  {
    if (items.size() > max_records)
      throw std::runtime_error("Analytic source record budget exceeded");
    integer(items.size());
    for (const auto &item : items)
      write(item);
  }
};
struct Reader {
  const uint8_t *data;
  size_t size, cursor = 0;
  uint64_t integer()
  {
    if (cursor > size || size - cursor < 8)
      throw std::runtime_error("Truncated analytic source");
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
      value |= uint64_t(data[cursor++]) << (i * 8);
    return value;
  }
  double scalar()
  {
    const double value = std::bit_cast<double>(integer());
    if (!std::isfinite(value))
      throw std::runtime_error("Nonfinite analytic source coordinate");
    return value;
  }
  Vec3 point()
  {
    const double x = scalar(), y = scalar(), z = scalar();
    return {x, y, z};
  }
  template<typename T, typename Fn> std::vector<T> sequence(Fn read)
  {
    const uint64_t count = integer();
    if (count > max_records || count > (size - cursor) / 8)
      throw std::runtime_error("Invalid analytic source record count");
    std::vector<T> items;
    items.reserve(size_t(count));
    for (uint64_t i = 0; i < count; i++)
      items.push_back(read());
    return items;
  }
};
}  // namespace

std::vector<uint8_t> encode(const Model &m, const Selection &input_selection)
{
  if (!validate(m))
    return {};
  const Selection selection = normalize_selection(m, input_selection);
  Writer out;
  out.integer(magic);
  out.integer(5);
  out.integer(m.next_id);
  out.sequence(m.vertices, [&](const Vertex &v) {
    out.integer(v.id);
    out.point(v.position);
  });
  out.sequence(m.curves, [&](const Curve &c) {
    out.integer(c.id);
    out.integer(uint64_t(c.kind));
    out.point(c.origin);
    out.point(c.u);
    out.point(c.v);
    out.point(c.w);
  });
  out.sequence(m.edges, [&](const Edge &e) {
    out.integer(e.id);
    out.integer(e.curve);
    out.integer(e.vertices[0]);
    out.integer(e.vertices[1]);
    out.scalar(e.start);
    out.scalar(e.end);
  });
  out.sequence(m.surfaces, [&](const Surface &s) {
    out.integer(s.id);
    out.integer(uint64_t(s.kind));
    out.point(s.origin);
    out.point(s.u);
    out.point(s.v);
    out.integer(s.curve);
    out.scalar(s.start);
    out.scalar(s.end);
    out.point(s.w);
    for (Vec3 coefficient : s.blend)
      out.point(coefficient);
    out.scalar(s.blend_cot);
    out.scalar(s.blend_v_start);
    out.scalar(s.blend_v_end);
    out.integer(s.other_curve);
    out.integer(s.reverse_first);
    out.integer(s.reverse_second);
  });
  auto coedge = [&](const Coedge &use) {
    out.integer(use.edge);
    out.integer(use.reversed);
  };
  out.sequence(m.faces, [&](const Face &f) {
    out.integer(f.id);
    out.integer(f.surface);
    out.integer(f.orientation > 0 ? 1 : 0);
    out.integer(f.group);
    out.sequence(f.outer, coedge);
    out.sequence(f.holes, [&](const std::vector<Coedge> &hole) { out.sequence(hole, coedge); });
  });
  auto id = [&](Id value) { out.integer(value); };
  out.sequence(selection.vertices, id);
  out.sequence(selection.edges, id);
  out.sequence(selection.faces, id);
  out.integer(selection.mode);
  out.integer(selection.active);
  out.integer(checksum(out.data.data(), out.data.size()));
  return std::move(out.data);
}

std::optional<EditResult> decode(const uint8_t *data, size_t size, std::string *error)
{
  try {
    if (!data || size < 32 || size > max_bytes)
      throw std::runtime_error("Invalid analytic source size");
    Reader tail{data + size - 8, 8};
    if (checksum(data, size - 8) != tail.integer())
      throw std::runtime_error("Analytic source checksum mismatch");
    Reader in{data, size - 8};
    if (in.integer() != magic)
      throw std::runtime_error("Unsupported analytic source version");
    const uint64_t version = in.integer();
    if (version < 1 || version > 5)
      throw std::runtime_error("Unsupported analytic source version");
    EditResult result;
    auto &m = result.model;
    m.next_id = in.integer();
    m.vertices = in.sequence<Vertex>([&]() {
      Vertex v;
      v.id = in.integer();
      v.position = in.point();
      return v;
    });
    m.curves = in.sequence<Curve>([&]() {
      Curve c;
      c.id = in.integer();
      const auto kind = in.integer();
      if (kind > 3 || (version < 5 && kind > 2) || (version < 4 && kind > 1))
        throw std::runtime_error("Unsupported analytic curve type");
      c.kind = CurveKind(kind);
      c.origin = in.point();
      c.u = in.point();
      c.v = in.point();
      if (version >= 4)
        c.w = in.point();
      return c;
    });
    m.edges = in.sequence<Edge>([&]() {
      Edge e;
      e.id = in.integer();
      e.curve = in.integer();
      e.vertices[0] = in.integer();
      e.vertices[1] = in.integer();
      e.start = in.scalar();
      e.end = in.scalar();
      return e;
    });
    m.surfaces = in.sequence<Surface>([&]() {
      Surface s;
      s.id = in.integer();
      const auto kind = in.integer();
      if (kind > 5 || (version < 5 && kind > 4) || (version < 4 && kind > 3) ||
          (version < 3 && kind > 2) || (version == 1 && kind > 1))
        throw std::runtime_error("Unsupported analytic surface type");
      s.kind = SurfaceKind(kind);
      s.origin = in.point();
      s.u = in.point();
      s.v = in.point();
      s.curve = in.integer();
      s.start = in.scalar();
      s.end = in.scalar();
      if (version >= 2)
        s.w = in.point();
      if (version >= 4) {
        for (Vec3 &coefficient : s.blend)
          coefficient = in.point();
        s.blend_cot = in.scalar();
        s.blend_v_start = in.scalar();
        s.blend_v_end = in.scalar();
      }
      if (version >= 5) {
        s.other_curve = in.integer();
        s.reverse_first = in.integer() != 0;
        s.reverse_second = in.integer() != 0;
      }
      return s;
    });
    auto coedge = [&]() {
      Coedge use;
      use.edge = in.integer();
      const auto sense = in.integer();
      if (sense > 1)
        throw std::runtime_error("Invalid coedge sense");
      use.reversed = bool(sense);
      return use;
    };
    m.faces = in.sequence<Face>([&]() {
      Face f;
      f.id = in.integer();
      f.surface = in.integer();
      const auto sense = in.integer();
      if (sense > 1)
        throw std::runtime_error("Invalid face sense");
      f.orientation = sense ? 1 : -1;
      if (version >= 4)
        f.group = in.integer();
      f.outer = in.sequence<Coedge>(coedge);
      f.holes = in.sequence<std::vector<Coedge>>([&]() { return in.sequence<Coedge>(coedge); });
      return f;
    });
    auto id = [&]() { return in.integer(); };
    auto &s = result.selection;
    s.vertices = in.sequence<Id>(id);
    s.edges = in.sequence<Id>(id);
    s.faces = in.sequence<Id>(id);
    const auto mode = in.integer();
    if (mode == 0 || mode > 7)
      throw std::runtime_error("Invalid analytic selection mode");
    s.mode = int(mode);
    s.active = in.integer();
    if (in.cursor != in.size || !validate(m, error))
      return {};
    for (Id value : s.vertices)
      if (!vertex(m, value))
        throw std::runtime_error("Unknown selected vertex");
    for (Id value : s.edges)
      if (!edge(m, value))
        throw std::runtime_error("Unknown selected edge");
    for (Id value : s.faces)
      if (!face(m, value))
        throw std::runtime_error("Unknown selected face");
    if (s.active && !vertex(m, s.active) && !edge(m, s.active) && !face(m, s.active))
      throw std::runtime_error("Unknown active source element");
    return result;
  }
  catch (const std::exception &exception) {
    if (error)
      *error = exception.what();
    return {};
  }
}

}  // namespace blender::solid
