/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 */

#include <cstdio>
#include <cstring>

#include "ANIM_rna.hh"
#include "ANIM_visualkey.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"

#include "DNA_constraint_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

namespace blender::animrig {

/* Internal status codes for visualkey_can_use. */
enum {
  VISUALKEY_NONE = 0,
  VISUALKEY_LOC,
  VISUALKEY_ROT,
  VISUALKEY_SCA,
};

bool visualkey_can_use(PointerRNA *ptr, PropertyRNA *prop)
{
  bConstraint *con = nullptr;
  bool has_rigidbody = false;

  if (ELEM(nullptr, ptr, ptr->data, prop)) {
    return false;
  }

  if (ptr->type != RNA_Object) {
    return false;
  }

  Object *ob = static_cast<Object *>(ptr->data);
  RigidBodyOb *rbo = ob->rigidbody_object;
  con = static_cast<bConstraint *>(ob->constraints.first);

  /* Active rigidbody objects only, as only those are affected by simulation. */
  has_rigidbody = ((rbo) && (rbo->type == RBO_TYPE_ACTIVE));

  /* Rigid body transforms are always visually keyed. */
  if (has_rigidbody) {
    return true;
  }

  /* Only do visual keying on transforms. */
  const char *identifier = RNA_property_identifier(prop);
  if (identifier == nullptr) {
    printf("%s failed: nullptr identifier\n", __func__);
    return false;
  }

  short searchtype = VISUALKEY_NONE;
  if (strstr(identifier, "location")) {
    searchtype = VISUALKEY_LOC;
  }
  else if (strstr(identifier, "rotation")) {
    searchtype = VISUALKEY_ROT;
  }
  else if (strstr(identifier, "scale")) {
    searchtype = VISUALKEY_SCA;
  }
  else {
    printf("%s failed: identifier - '%s'\n", __func__, identifier);
    return false;
  }

  /* Check constraints. */
  for (; con; con = con->next) {
    /* only consider constraint if it is not disabled, and has influence */
    if (con->flag & CONSTRAINT_DISABLE) {
      continue;
    }
    if (con->enforce == 0.0f) {
      continue;
    }

    /* Some constraints may alter these transforms. */
    switch (con->type) {
      /* Multi-transform constraints. */
      case CONSTRAINT_TYPE_CHILDOF:
        return true;
      case CONSTRAINT_TYPE_TRANSFORM:
      case CONSTRAINT_TYPE_TRANSLIKE:
        return true;
      case CONSTRAINT_TYPE_FOLLOWPATH:
        return true;

      /* Single-transform constraints. */
      case CONSTRAINT_TYPE_TRACKTO:
        if (searchtype == VISUALKEY_ROT) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_DAMPTRACK:
        if (searchtype == VISUALKEY_ROT) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_ROTLIMIT:
        if (searchtype == VISUALKEY_ROT) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_LOCLIMIT:
        if (searchtype == VISUALKEY_LOC) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_SIZELIMIT:
        if (searchtype == VISUALKEY_SCA) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_DISTLIMIT:
        if (searchtype == VISUALKEY_LOC) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_ROTLIKE:
        if (searchtype == VISUALKEY_ROT) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_LOCLIKE:
        if (searchtype == VISUALKEY_LOC) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_SIZELIKE:
        if (searchtype == VISUALKEY_SCA) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_LOCKTRACK:
        if (searchtype == VISUALKEY_ROT) {
          return true;
        }
        break;
      case CONSTRAINT_TYPE_MINMAX:
        if (searchtype == VISUALKEY_LOC) {
          return true;
        }
        break;

      default:
        break;
    }
  }

  return false;
}

Vector<float> visualkey_get_values(PointerRNA *ptr, PropertyRNA *prop)
{
  Vector<float> values;
  const char *identifier = RNA_property_identifier(prop);
  float tmat[4][4];
  int rotmode;

  /* Handle Object transforms only. Other pointer types use their property values directly.
   * Assume that array_index will be sane.
   */
  if (ptr->type == RNA_Object) {
    Object *ob = static_cast<Object *>(ptr->data);
    /* Loc code is specific... */
    if (strstr(identifier, "location")) {
      values.extend({ob->object_to_world().location(), 3});
      return values;
    }

    copy_m4_m4(tmat, ob->object_to_world().ptr());
    rotmode = ob->rotmode;
  }
  else {
    return get_rna_values(ptr, prop);
  }

  /* Rot/Scale code are common! */
  if (strstr(identifier, "rotation_euler")) {
    values.resize(3);
    mat4_to_eulO(values.data(), rotmode, tmat);
    return values;
  }

  if (strstr(identifier, "rotation_quaternion")) {
    values.resize(4);
    mat4_to_quat(values.data(), tmat);
    return values;
  }

  if (strstr(identifier, "rotation_axis_angle")) {
    /* w = 0, x,y,z = 1,2,3 */
    values.resize(4);
    mat4_to_axis_angle(values.data() + 1, values.data() + 0, tmat);
    return values;
  }

  if (strstr(identifier, "scale")) {
    values.resize(3);
    mat4_to_size(values.data(), tmat);
    return values;
  }

  /* As the function hasn't returned yet, read value from system in the default way. */
  return get_rna_values(ptr, prop);
}
}  // namespace blender::animrig
