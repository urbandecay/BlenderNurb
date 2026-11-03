/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "BLI_math_matrix_types.hh"
#include "SOL_geometry.hh"

namespace blender {
struct Mesh;
struct SolidStorage;
struct BlendWriter;
struct BlendDataReader;
}  // namespace blender

namespace blender::bke::solid_host {

struct Snapshot {
  std::shared_ptr<const solid::Model> model;
  solid::Selection selection;
  uint64_t revision = 0;
};

SolidStorage *storage_create(solid::Model model,
                             solid::Selection selection,
                             std::string *error = nullptr);
SolidStorage *storage_copy(const SolidStorage *source);
void storage_free(SolidStorage *storage);
void storage_write(BlendWriter *writer, const SolidStorage &storage);
void storage_read(BlendDataReader *reader, SolidStorage &storage);
std::optional<Snapshot> read(const Mesh &mesh, std::string *error = nullptr);
/** Read only source identities from the edit selection; never import display coordinates. */
std::optional<Snapshot> read_edit(const Mesh &mesh, std::string *error = nullptr);
void sync_edit_selection(Mesh &mesh);

/** Only source snapshots may be published. Display edits cannot be imported as source.
 * Selection-only updates retain the immutable model and its display sampling cache. */
bool publish(Mesh &mesh,
             Snapshot candidate,
             uint64_t expected_revision,
             std::string *error = nullptr);

/** Apply a datablock transform to the independent source, then regenerate its display. */
bool transform_mesh(Mesh &mesh, const float4x4 &matrix, std::string *error = nullptr);

struct MeshDeleter {
  void operator()(Mesh *mesh) const;
};
using OwnedMesh = std::unique_ptr<Mesh, MeshDeleter>;
OwnedMesh display_mesh(const Snapshot &source,
                       int circle_segments = 64,
                       std::string *error = nullptr);

}  // namespace blender::bke::solid_host
