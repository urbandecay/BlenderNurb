/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "SOL_geometry.hh"

namespace blender::solid {
/** Own a checked, immutable input throughout a modal extrusion. */
class PreparedExtrusion {
  Profile profile_;
  std::optional<DisplayMesh> display_cache_;
  uint8_t validated_distance_signs_ = 0;
  explicit PreparedExtrusion(Profile profile) : profile_(std::move(profile)) {}
 public:
  static std::optional<PreparedExtrusion> create(Profile profile, std::string *error = nullptr);
  struct Preview {
    std::shared_ptr<const Model> model;
    Selection selection;
    std::optional<ValidatedModel> validation;
    std::shared_ptr<const DisplayMesh> samples;
  };
  std::optional<Preview> preview(double distance, bool analytic, std::string *error = nullptr);
  Vec3 normal() const { return profile_.normal; }
  std::optional<EditResult> evaluate(double distance, std::string *error = nullptr) const;
};
}  // namespace blender::solid
