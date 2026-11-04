/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "SOL_geometry.hh"
#include <map>

namespace blender::solid {
std::optional<EditResult> fillet_trimmed_edges(const Model &model,
                                               const Selection &selection,
                                               const std::map<Id, Vec3> &normals,
                                               const std::map<Id, std::vector<Id>> &uses,
                                               const std::map<Id, std::vector<Id>> &vertex_faces,
                                               double width,
                                               int subdivisions,
                                               double eps,
                                               std::string *error);
}
