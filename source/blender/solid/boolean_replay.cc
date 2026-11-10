/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_boolean_replay.hh"

#include <bit>
#include <limits>
#include <stdexcept>

namespace blender::solid {
namespace {
constexpr size_t max_bytes = 128 << 20;
constexpr size_t max_operand_ids = 1 << 20;
constexpr uint64_t magic = 0x3159504c52534f42; /* BOSRLPY1, little endian. */
constexpr uint64_t version = 1;
constexpr size_t fixed_size = 6 * sizeof(uint64_t) + sizeof(uint64_t); /* Header and checksum. */

uint64_t checksum(const uint8_t *data, const size_t size)
{
  uint64_t value = 14695981039346656037ull;
  for (size_t i = 0; i < size; i++) {
    value = (value ^ data[i]) * 1099511628211ull;
  }
  return value;
}

struct Writer {
  std::vector<uint8_t> data;

  void integer(const uint64_t value)
  {
    for (int i = 0; i < 8; i++) {
      data.push_back(uint8_t(value >> (i * 8)));
    }
  }
};

struct Reader {
  const uint8_t *data;
  size_t size;
  size_t cursor = 0;

  uint64_t integer()
  {
    if (cursor > size || size - cursor < sizeof(uint64_t)) {
      throw std::runtime_error("Truncated Boolean replay");
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
      value |= uint64_t(data[cursor++]) << (i * 8);
    }
    return value;
  }
};

void add_budget(size_t &total, const size_t count, const size_t bytes_per_item, const size_t limit)
{
  if (total > limit || count > (limit - total) / bytes_per_item) {
    throw std::runtime_error("Boolean replay byte budget exceeded");
  }
  total += count * bytes_per_item;
}

void check_source_budget(const Model &model, const size_t limit)
{
  /* Conservative upper bound for the source serializer. Check before it builds its temporary
   * byte vector, since the replay envelope has a stricter total-size limit. */
  if (model.vertices.size() > max_operand_ids || model.curves.size() > max_operand_ids ||
      model.edges.size() > max_operand_ids || model.surfaces.size() > max_operand_ids ||
      model.faces.size() > max_operand_ids)
  {
    throw std::runtime_error("Boolean replay source record budget exceeded");
  }
  size_t total = 128;
  add_budget(total, model.vertices.size(), 40, limit);
  add_budget(total, model.curves.size(), 120, limit);
  add_budget(total, model.edges.size(), 56, limit);
  add_budget(total, model.surfaces.size(), 288, limit);
  add_budget(total, model.faces.size(), 64, limit);
  size_t holes = 0, coedges = 0;
  for (const Face &face : model.faces) {
    if (face.outer.size() > max_operand_ids || face.holes.size() > max_operand_ids) {
      throw std::runtime_error("Boolean replay source record budget exceeded");
    }
    coedges += face.outer.size();
    holes += face.holes.size();
    for (const auto &hole : face.holes) {
      if (hole.size() > max_operand_ids || coedges > max_bytes / sizeof(Coedge) - hole.size()) {
        throw std::runtime_error("Boolean replay source record budget exceeded");
      }
      coedges += hole.size();
    }
    if (coedges > max_bytes / sizeof(Coedge)) {
      throw std::runtime_error("Boolean replay source record budget exceeded");
    }
  }
  add_budget(total, holes, 8, limit);
  add_budget(total, coedges, 16, limit);
}

}  // namespace

std::vector<uint8_t> encode_boolean_replay(const BooleanReplay &replay, std::string *error)
{
  if (error) {
    error->clear();
  }
  try {
    if (replay.first.size() > max_operand_ids || replay.second.size() > max_operand_ids) {
      throw std::runtime_error("Boolean replay operand ID budget exceeded");
    }

    const size_t ids_size = (replay.first.size() + replay.second.size()) * sizeof(uint64_t);
    if (ids_size > max_bytes - fixed_size) {
      throw std::runtime_error("Boolean replay byte budget exceeded");
    }

    /* The stored source remains the exact pre-operation model. An empty selection is only a
     * transport detail; operand IDs and the operation are stored separately below. */
    check_source_budget(replay.source, max_bytes - fixed_size - ids_size);
    const std::vector<uint8_t> source = encode(replay.source, {});
    if (source.empty()) {
      throw std::runtime_error("Boolean replay source could not be encoded");
    }

    if (source.size() > max_bytes - fixed_size ||
        ids_size > max_bytes - fixed_size - source.size())
    {
      throw std::runtime_error("Boolean replay byte budget exceeded");
    }

    Writer out;
    out.data.reserve(fixed_size + ids_size + source.size());
    out.integer(magic);
    out.integer(version);
    const int32_t operation = static_cast<int32_t>(replay.operation);
    out.integer(std::bit_cast<uint32_t>(operation));
    out.integer(replay.first.size());
    out.integer(replay.second.size());
    out.integer(source.size());
    for (const Id id : replay.first) {
      out.integer(id);
    }
    for (const Id id : replay.second) {
      out.integer(id);
    }
    out.data.insert(out.data.end(), source.begin(), source.end());
    out.integer(checksum(out.data.data(), out.data.size()));
    if (error) {
      error->clear();
    }
    return std::move(out.data);
  }
  catch (const std::exception &failure) {
    if (error) {
      *error = failure.what();
    }
    return {};
  }
}

std::optional<BooleanReplay> decode_boolean_replay(const uint8_t *data,
                                                   const size_t size,
                                                   std::string *error)
{
  if (error) {
    error->clear();
  }
  try {
    if (!data || size < fixed_size || size > max_bytes) {
      throw std::runtime_error("Invalid Boolean replay size");
    }
    Reader tail{data + size - sizeof(uint64_t), sizeof(uint64_t)};
    if (checksum(data, size - sizeof(uint64_t)) != tail.integer()) {
      throw std::runtime_error("Boolean replay checksum mismatch");
    }

    Reader in{data, size - sizeof(uint64_t)};
    if (in.integer() != magic || in.integer() != version) {
      throw std::runtime_error("Unsupported Boolean replay version");
    }
    const uint64_t operation_word = in.integer();
    if (operation_word > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("Invalid Boolean replay operation value");
    }
    const int32_t operation = std::bit_cast<int32_t>(uint32_t(operation_word));
    const uint64_t first_count = in.integer();
    const uint64_t second_count = in.integer();
    const uint64_t source_size = in.integer();
    if (first_count > max_operand_ids || second_count > max_operand_ids) {
      throw std::runtime_error("Boolean replay operand ID budget exceeded");
    }

    /* Check all attacker-controlled counts and lengths before reserving either ID vector. */
    const size_t available = in.size - in.cursor;
    if (first_count > available / sizeof(uint64_t)) {
      throw std::runtime_error("Invalid Boolean replay operand length");
    }
    const size_t first_bytes = size_t(first_count) * sizeof(uint64_t);
    if (second_count > (available - first_bytes) / sizeof(uint64_t)) {
      throw std::runtime_error("Invalid Boolean replay operand length");
    }
    const size_t second_bytes = size_t(second_count) * sizeof(uint64_t);
    const size_t ids_bytes = first_bytes + second_bytes;
    if (source_size != available - ids_bytes) {
      throw std::runtime_error("Invalid Boolean replay source length");
    }

    BooleanReplay replay;
    replay.operation = static_cast<BooleanOperation>(operation);
    replay.first.reserve(size_t(first_count));
    replay.second.reserve(size_t(second_count));
    for (uint64_t i = 0; i < first_count; i++) {
      replay.first.push_back(in.integer());
    }
    for (uint64_t i = 0; i < second_count; i++) {
      replay.second.push_back(in.integer());
    }

    auto source = decode(data + in.cursor, size_t(source_size), error);
    if (!source) {
      if (error && error->empty()) {
        *error = "Invalid Boolean replay source";
      }
      return {};
    }
    replay.source = std::move(source->model);
    if (error) {
      error->clear();
    }
    return replay;
  }
  catch (const std::exception &failure) {
    if (error) {
      *error = failure.what();
    }
    return {};
  }
}

}  // namespace blender::solid
