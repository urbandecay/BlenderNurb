/* SPDX-FileCopyrightText: 2023 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <pxr/usd/usd/prim.h>

namespace blender {

struct Main;
struct Object;
struct ReportList;

namespace io::usd {

void import_blendshapes(Main *bmain,
                        Object *mesh_obj,
                        const pxr::UsdPrim &prim,
                        ReportList *reports);

}  // namespace io::usd
}  // namespace blender
