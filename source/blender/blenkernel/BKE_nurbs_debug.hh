/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

namespace blender::bke::nurbs {

/** Append one diagnostic line to /tmp/nurbs-debug.log, or BLENDER_NURBS_DEBUG_LOG when set.
 * Intended for temporary profiling; the override isolates benchmark processes from live edits. */
void debug_log(const char *file, int line, const char *format, ...);

}  // namespace blender::bke::nurbs

#define NURBS_DEBUG_LOG(...) \
  ::blender::bke::nurbs::debug_log(__FILE__, __LINE__, __VA_ARGS__)
