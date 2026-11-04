/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BKE_solid_host.hh"

namespace blender {
struct bContext;
struct Object;
struct BMPartialUpdate;
}  // namespace blender

namespace blender::ed::solid_editor {

bool edit_poll(bContext *C);

/** Independent-source edit session. Display geometry is replaceable output, never input. */
class Session {
  Object *object_;
  bke::solid_host::Snapshot source_;
  uint64_t revision_;
  bool rigid_selection_ = false;
  std::vector<bool> display_position_mask_, display_normal_mask_;
  BMPartialUpdate *partial_cache_ = nullptr;
  std::vector<bool> partial_mask_;
  std::vector<solid::Vec3> display_positions_, display_normals_;
  solid::Affine display_matrix_;
  bool display_preview_ = false;
  bool display_restored_ = false;
  Session(Object &object, bke::solid_host::Snapshot source);

 public:
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
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
  bool can_preview_affine(const solid::Affine &matrix) const;
  bool has_affine_preview() const
  {
    return display_preview_;
  }
  bool preview_affine(bContext *C, const solid::Affine &matrix, std::string *error);
  bool commit_affine(bContext *C, std::string *error);
};

}  // namespace blender::ed::solid_editor
