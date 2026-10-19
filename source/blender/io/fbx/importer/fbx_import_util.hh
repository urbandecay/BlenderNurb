/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_set.hh"

#include "ufbx.h"

namespace blender {

struct ID;
struct Object;
struct Key;
struct Material;

namespace io::fbx {

const char *get_fbx_name(const ufbx_string &name, const char *def = "Untitled");

struct FbxElementMapping {
  Set<Object *> imported_objects;
  Map<const ufbx_element *, Object *> el_to_object;
  Map<const ufbx_element *, Key *> el_to_shape_key;
  Map<const ufbx_material *, Material *> mat_to_material;

  ufbx_matrix global_conv_matrix;
};

void matrix_to_m44(const ufbx_matrix &src, float dst[4][4]);
void ufbx_matrix_to_obj(const ufbx_matrix &mtx, Object *obj);
void node_matrix_to_obj(const ufbx_node *node, Object *obj);
void read_custom_properties(const ufbx_props &props, ID &id, bool enums_as_strings);

//@TODO remove debug file print once things are working properly
// #define FBX_DEBUG_PRINT

#ifdef FBX_DEBUG_PRINT
extern FILE *g_debug_file;

inline double adjf(double f)
{
  if (fabs(f) < 0.0005) {
    return 0.0;
  }
  return f;
}

void print_matrix(const ufbx_matrix &m);
#endif

}  // namespace io::fbx
}  // namespace blender
