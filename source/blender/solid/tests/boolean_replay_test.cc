/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "SOL_boolean_replay.hh"

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

static Model box()
{
  Model model;
  Selection selected;
  Id first = 0, previous = 0;
  const Vec3 points[] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
  std::string error;
  for (int i = 0; i < 4; i++) {
    const auto curve = line(points[i], points[(i + 1) % 4], &error);
    require(bool(curve), error);
    require(
        append_curve(model, *curve, 0, 1, previous, i == 3 ? first : 0, false, &selected, &error),
        error);
    if (!first) {
      first = model.edges.back().vertices[0];
    }
    previous = model.edges.back().vertices[1];
  }
  const auto profile = prepare_profile(model, selected, &error);
  require(bool(profile), error);
  const auto body = extrude(*profile, 2, &error);
  require(bool(body), error);
  return body->model;
}

static uint64_t checksum(const uint8_t *data, const size_t size)
{
  uint64_t value = 14695981039346656037ull;
  for (size_t i = 0; i < size; i++) {
    value = (value ^ data[i]) * 1099511628211ull;
  }
  return value;
}

static void write_u64(std::vector<uint8_t> &bytes, const size_t offset, const uint64_t value)
{
  for (int i = 0; i < 8; i++) {
    bytes[offset + i] = uint8_t(value >> (i * 8));
  }
}

static void refresh_checksum(std::vector<uint8_t> &bytes)
{
  write_u64(bytes, bytes.size() - 8, checksum(bytes.data(), bytes.size() - 8));
}

int main()
{
  std::string error;
  const Model source = box();
  BooleanReplay replay;
  replay.source = source;
  replay.first = {source.faces[0].id, source.faces[1].id};
  replay.second = replay.first; /* Self-operation requests are preserved verbatim. */
  replay.operation = BooleanOperation::Subtract;

  const auto bytes = encode_boolean_replay(replay, &error);
  require(!bytes.empty(), error);
  const auto decoded = decode_boolean_replay(bytes.data(), bytes.size(), &error);
  require(bool(decoded), error);
  require(encode(decoded->source, {}) == encode(source, {}), "Replay changed its native source");
  require(decoded->first == replay.first && decoded->second == replay.second,
          "Replay changed exact operand IDs");
  require(decoded->operation == replay.operation, "Replay changed operation");

  error = "stale diagnostic";
  require(bool(decode_boolean_replay(bytes.data(), bytes.size(), &error)),
          "Replay decode failed after a prior diagnostic: " + error);
  require(error.empty(), "Successful replay decode retained a stale diagnostic");

  BooleanReplay empty;
  empty.source = source;
  empty.operation = BooleanOperation::Intersect;
  const auto empty_bytes = encode_boolean_replay(empty, &error);
  require(!empty_bytes.empty(), error);
  const auto empty_decoded = decode_boolean_replay(empty_bytes.data(), empty_bytes.size(), &error);
  require(bool(empty_decoded), error);
  require(empty_decoded->first.empty() && empty_decoded->second.empty(),
          "Replay did not preserve empty operand inputs");

  BooleanReplay unsupported;
  Selection selection;
  Curve circle{0, CurveKind::Conic, {}, {1, 0, 0}, {0, 1, 0}, {}};
  require(append_curve(
              unsupported.source, circle, 0, 2 * std::numbers::pi, 0, 0, true, &selection, &error),
          error);
  const auto profile = prepare_profile(unsupported.source, selection, &error);
  require(bool(profile), error);
  const auto cylinder = extrude(*profile, 2, &error);
  require(bool(cylinder), error);
  unsupported.source = cylinder->model;
  for (const Face &face : unsupported.source.faces) {
    unsupported.first.push_back(face.id);
  }
  unsupported.second = unsupported.first;
  unsupported.operation = static_cast<BooleanOperation>(42);
  const auto unsupported_bytes = encode_boolean_replay(unsupported, &error);
  require(!unsupported_bytes.empty(), error);
  const auto unsupported_decoded = decode_boolean_replay(
      unsupported_bytes.data(), unsupported_bytes.size(), &error);
  require(bool(unsupported_decoded), error);
  require(encode(unsupported_decoded->source, {}) == encode(unsupported.source, {}),
          "Replay rejected or changed an unsupported curved request");
  require(unsupported_decoded->operation == unsupported.operation,
          "Replay changed an unsupported operation request");

  std::vector<uint8_t> corrupt = bytes;
  corrupt[corrupt.size() / 2] ^= 1;
  require(!decode_boolean_replay(corrupt.data(), corrupt.size(), &error),
          "Replay accepted a checksum-corrupt payload");
  require(!decode_boolean_replay(bytes.data(), bytes.size() - 1, &error),
          "Replay accepted a truncated payload");
  require(!decode_boolean_replay(bytes.data(), bytes.size() / 2, &error),
          "Replay accepted a severely truncated payload");

  std::vector<uint8_t> over_budget = bytes;
  write_u64(over_budget, 24, (1 << 20) + 1);
  refresh_checksum(over_budget);
  require(!decode_boolean_replay(over_budget.data(), over_budget.size(), &error),
          "Replay accepted an over-budget operand count");

  std::vector<uint8_t> operation_overflow = bytes;
  write_u64(operation_overflow, 16, uint64_t(1) << 32);
  refresh_checksum(operation_overflow);
  require(!decode_boolean_replay(operation_overflow.data(), operation_overflow.size(), &error),
          "Replay accepted operation bits outside the serialized enum word");

  std::vector<uint8_t> trailing = bytes;
  trailing.insert(trailing.end() - 8, 0);
  refresh_checksum(trailing);
  require(!decode_boolean_replay(trailing.data(), trailing.size(), &error),
          "Replay accepted trailing bytes");

  BooleanReplay too_many;
  too_many.first.resize((1 << 20) + 1);
  require(encode_boolean_replay(too_many, &error).empty(),
          "Replay encoder accepted an over-budget operand");

  std::cout << "Boolean replay checks passed\n";
}
