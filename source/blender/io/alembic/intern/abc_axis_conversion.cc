/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup Alembic
 */

#include "abc_axis_conversion.h"

#include "BLI_assert.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"

#include "BKE_object_types.hh"

#include "DNA_object_types.h"

namespace blender::io::alembic {

void create_swapped_rotation_matrix(float rot_x_mat[3][3],
                                    float rot_y_mat[3][3],
                                    float rot_z_mat[3][3],
                                    const float euler[3],
                                    AbcAxisSwapMode mode)
{
  const float rx = euler[0];
  float ry;
  float rz;

  /* Apply transformation */
  switch (mode) {
    case ABC_ZUP_FROM_YUP:
      ry = -euler[2];
      rz = euler[1];
      break;
    case ABC_YUP_FROM_ZUP:
      ry = euler[2];
      rz = -euler[1];
      break;
    default:
      ry = 0.0f;
      rz = 0.0f;
      BLI_assert(false);
      break;
  }

  unit_m3(rot_x_mat);
  unit_m3(rot_y_mat);
  unit_m3(rot_z_mat);

  rot_x_mat[1][1] = cos(rx);
  rot_x_mat[2][1] = -sin(rx);
  rot_x_mat[1][2] = sin(rx);
  rot_x_mat[2][2] = cos(rx);

  rot_y_mat[2][2] = cos(ry);
  rot_y_mat[0][2] = -sin(ry);
  rot_y_mat[2][0] = sin(ry);
  rot_y_mat[0][0] = cos(ry);

  rot_z_mat[0][0] = cos(rz);
  rot_z_mat[1][0] = -sin(rz);
  rot_z_mat[0][1] = sin(rz);
  rot_z_mat[1][1] = cos(rz);
}  // namespace
   // alembicvoidcreate_swapped_rotation_matrix(floatrot_x_mat[3][3],floatrot_y_mat[3][3],floatrot_z_mat[3][3],constfloateuler[3],AbcAxisSwapModemode)

void copy_m44_axis_swap(float dst_mat[4][4], float src_mat[4][4], AbcAxisSwapMode mode)
{
  /* Convert the coordinate basis with a matrix conjugation. This preserves shear and homogeneous
   * terms, which occur naturally when flattening a hierarchy with non-uniform parent scale. */
  float basis[4][4], inverse_basis[4][4], intermediate[4][4];
  unit_m4(basis);
  unit_m4(inverse_basis);

  switch (mode) {
    case ABC_YUP_FROM_ZUP:
      /* (x, y, z) -> (x, z, -y). */
      basis[1][1] = 0.0f;
      basis[1][2] = -1.0f;
      basis[2][1] = 1.0f;
      basis[2][2] = 0.0f;
      inverse_basis[1][1] = 0.0f;
      inverse_basis[1][2] = 1.0f;
      inverse_basis[2][1] = -1.0f;
      inverse_basis[2][2] = 0.0f;
      break;
    case ABC_ZUP_FROM_YUP:
      /* (x, y, z) -> (x, -z, y). */
      basis[1][1] = 0.0f;
      basis[1][2] = 1.0f;
      basis[2][1] = -1.0f;
      basis[2][2] = 0.0f;
      inverse_basis[1][1] = 0.0f;
      inverse_basis[1][2] = -1.0f;
      inverse_basis[2][1] = 1.0f;
      inverse_basis[2][2] = 0.0f;
      break;
    default:
      BLI_assert(false);
      copy_m4_m4(dst_mat, src_mat);
      return;
  }

  mul_m4_m4m4(intermediate, inverse_basis, src_mat);
  mul_m4_m4m4(dst_mat, intermediate, basis);
}

void create_transform_matrix(Object *obj,
                             float r_yup_mat[4][4],
                             AbcMatrixMode mode,
                             Object *proxy_from)
{
  float zup_mat[4][4];

  /* Object-parent relationships are not part of the exported transform model. Even callers that
   * request the legacy local mode receive the evaluated world transform. */
  (void)mode;
  copy_m4_m4(zup_mat, obj->object_to_world().ptr());

  if (proxy_from) {
    mul_m4_m4m4(zup_mat, proxy_from->object_to_world().ptr(), zup_mat);
  }

  copy_m44_axis_swap(r_yup_mat, zup_mat, ABC_YUP_FROM_ZUP);
}

}  // namespace blender::io::alembic
