/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "SOL_boolean.hh"
#include "SOL_boolean_replay.hh"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr size_t max_replay_bytes = 128u * 1024u * 1024u;

int usage(const char *program)
{
  std::cerr << "Usage: " << program << " <request.solidbool>\n";
  return 2;
}

bool read_request(const char *path, std::vector<uint8_t> &bytes, std::string &error)
{
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Could not open replay request";
    return false;
  }

  file.seekg(0, std::ios::end);
  const std::streampos end = file.tellg();
  if (!file || end < std::streampos(0)) {
    error = "Could not determine replay request size";
    return false;
  }
  const std::streamoff size = end;
  if (static_cast<uintmax_t>(size) > max_replay_bytes) {
    error = "Replay request exceeds the 128 MiB size limit";
    return false;
  }

  file.seekg(0, std::ios::beg);
  if (!file) {
    error = "Could not seek to replay request start";
    return false;
  }

  bytes.resize(static_cast<size_t>(size));
  if (size > 0) {
    file.read(reinterpret_cast<char *>(bytes.data()), size);
    if (file.gcount() != size || !file) {
      error = "Could not read the complete replay request";
      return false;
    }
  }
  if (file.peek() != std::char_traits<char>::eof()) {
    error = "Replay request changed or exceeds its measured size";
    return false;
  }
  if (file.bad()) {
    error = "I/O error while reading replay request";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    return usage(argv[0]);
  }

  try {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!read_request(argv[1], bytes, error)) {
      std::cerr << "Boolean replay I/O error: " << error << '\n';
      return 2;
    }

    const std::optional<blender::solid::BooleanReplay> replay =
        blender::solid::decode_boolean_replay(bytes.data(), bytes.size(), &error);
    if (!replay) {
      std::cerr << "Malformed Boolean replay: " << (error.empty() ? "decode failed" : error)
                << '\n';
      return 2;
    }
    const blender::solid::BooleanResult result = blender::solid::boolean_bodies_result(
        replay->source, replay->first, replay->second, replay->operation);

    std::cout << "Boolean replay: " << blender::solid::boolean_status_name(result.status) << '\n'
              << "Path: " << blender::solid::boolean_path_name(result.diagnostics.path) << '\n'
              << "Source faces: " << replay->source.faces.size() << '\n'
              << "First operand faces: " << result.diagnostics.first_face_count << '\n'
              << "Second operand faces: " << result.diagnostics.second_face_count << '\n'
              << "Result faces: " << result.diagnostics.result_face_count << '\n';
    for (const blender::solid::BooleanTiming &timing : result.diagnostics.timings) {
      std::cout << "Stage " << blender::solid::boolean_stage_name(timing.stage) << ": "
                << timing.milliseconds << " ms\n";
    }
    if (!result.message.empty()) {
      std::cout << "Message: " << result.message << '\n';
    }
    return result.succeeded() ? 0 : 1;
  }
  catch (const std::exception &exception) {
    std::cerr << "Boolean replay failed with an exception: " << exception.what() << '\n';
    return 1;
  }
  catch (...) {
    std::cerr << "Boolean replay failed with an unknown exception\n";
    return 1;
  }
}
