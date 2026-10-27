/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_nurbs_debug.hh"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "BLI_time.h"

namespace blender::bke::nurbs {
namespace {

std::mutex debug_log_mutex;
FILE *debug_log_file = nullptr;

}  // namespace

void debug_log(const char *file, const int line, const char *format, ...)
{
  std::lock_guard lock(debug_log_mutex);
  if (debug_log_file == nullptr) {
    debug_log_file = std::fopen("/tmp/nurbs-debug.log", "w");
    if (debug_log_file == nullptr) {
      return;
    }
    std::fprintf(debug_log_file, "=== Geometry diagnostic session started ===\n");
  }
  std::fprintf(debug_log_file, "[%.6f] %s:%d ", BLI_time_now_seconds(), file, line);
  va_list args;
  va_start(args, format);
  std::vfprintf(debug_log_file, format, args);
  va_end(args);
  std::fputc('\n', debug_log_file);
  std::fflush(debug_log_file);
}

}  // namespace blender::bke::nurbs
