/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BKE_solid_host.hh"

namespace blender {
struct bContext;
struct Object;
}  // namespace blender

namespace blender::ed::solid_editor {

bool edit_poll(bContext *C);

/** Independent-source edit session. Display geometry is replaceable output, never input. */
class Session {
  Object *object_;
  bke::solid_host::Snapshot source_;
  uint64_t revision_;
  Session(Object &object, bke::solid_host::Snapshot source);

 public:
  static std::unique_ptr<Session> begin(bContext *C, std::string *error = nullptr);
  Object &object() const
  {
    return *object_;
  }
  const bke::solid_host::Snapshot &source() const
  {
    return source_;
  }
  bool apply(bContext *C,
             solid::EditResult candidate,
             std::string *error = nullptr,
             bool reuse_topology = false,
             const solid::Affine *affine = nullptr);
  bool restore(bContext *C, std::string *error = nullptr);
};

}  // namespace blender::ed::solid_editor
