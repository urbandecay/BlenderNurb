/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 * Mesh Deform modifier binding.
 */

#include <algorithm>
#include <cstdio>

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_memarena.h"
#include "BLI_ordered_edge.hh"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_modifier.hh"

#include "ED_mesh.hh"

#include "eigen_capi.h"

namespace blender {

/* These progress and cursor hooks are intentionally no-ops for binding. */
static void waitcursor(int /*val*/) {}
static void progress_bar(int /*dummy_val*/, const char * /*dummy*/) {}
static void start_progress_bar() {}
static void end_progress_bar() {}
static void error(const char *str)
{
  printf("error: %s\n", str);
}

#define EPSILON 0.0001f

#define MESHDEFORM_TAG_UNTYPED 0
#define MESHDEFORM_TAG_BOUNDARY 1
#define MESHDEFORM_TAG_INTERIOR 2
#define MESHDEFORM_TAG_EXTERIOR 3

/** minimum length for #MDefBoundIsect.len */
#define MESHDEFORM_LEN_THRESHOLD 1e-6f

#define MESHDEFORM_MIN_INFLUENCE 0.0005f

static const int MESHDEFORM_OFFSET[7][3] = {
    {0, 0, 0},
    {1, 0, 0},
    {-1, 0, 0},
    {0, 1, 0},
    {0, -1, 0},
    {0, 0, 1},
    {0, 0, -1},
};

struct MDefBoundIsect {
  /* intersection on the cage 'cagecos' */
  float co[3];
  /* non-facing intersections are considered interior */
  bool facing;
  /* ray-cast index aligned with polygons (ray-hit-triangle isn't needed) */
  int face_index;
  /* distance from 'co' to the ray-cast start (clamped to avoid zero division) */
  float len;
  /* weights aligned with the polygon's loop indices */
  float poly_weights[0];
};

struct MDefBindInfluence {
  MDefBindInfluence *next;
  float weight;
  int vertex;
};

struct MeshDeformBind {
  /* grid dimensions */
  float min[3], max[3];
  float width[3], halfwidth[3];
  int size, size3;

  /* meshes */
  Mesh *cagemesh;
  float (*cagecos)[3];
  float (*vertexcos)[3];
  int verts_num, cage_verts_num;

  /* grids */
  MemArena *memarena;
  MDefBoundIsect *(*boundisect)[6];
  int *semibound;
  int *tag;
  float *phi, *totalphi;

  /* mesh stuff */
  int *inside;
  float *weights;
  MDefBindInfluence **dyngrid;
  float cagemat[4][4];

  /* direct solver */
  int *varidx;

  const BVHTree *bvhtree;
  bke::BVHTreeFromMesh bvhdata;

  /* avoid DM function calls during intersections */
  struct {
    OffsetIndices<int> faces;
    Span<int> corner_verts;
    Span<int3> corner_tris;
    Span<int> tri_faces;
    Span<float3> face_normals;
  } cagemesh_cache;
};

struct MeshDeformIsect {
  float start[3];
  float vec[3];
  float vec_length;
  float lambda;

  bool isect;
  float u, v;
};

/* ray intersection */

struct MeshRayCallbackData {
  MeshDeformBind *mdb;
  MeshDeformIsect *isec;
};

static void harmonic_ray_callback(void *userdata,
                                  int index,
                                  const BVHTreeRay *ray,
                                  BVHTreeRayHit *hit)
{
  MeshRayCallbackData *data = static_cast<MeshRayCallbackData *>(userdata);
  MeshDeformBind *mdb = data->mdb;
  const Span<int> corner_verts = mdb->cagemesh_cache.corner_verts;
  const Span<int> tri_faces = mdb->cagemesh_cache.tri_faces;
  const Span<float3> face_normals = mdb->cagemesh_cache.face_normals;
  MeshDeformIsect *isec = data->isec;
  float no[3], co[3], dist;
  float *face[3];

  const int3 &tri = mdb->cagemesh_cache.corner_tris[index];

  face[0] = mdb->cagecos[corner_verts[tri[0]]];
  face[1] = mdb->cagecos[corner_verts[tri[1]]];
  face[2] = mdb->cagecos[corner_verts[tri[2]]];

  bool isect_ray_tri = isect_ray_tri_watertight_v3(
      ray->origin, ray->isect_precalc, UNPACK3(face), &dist, nullptr);

  if (!isect_ray_tri || dist > isec->vec_length) {
    return;
  }

  if (!face_normals.is_empty()) {
    copy_v3_v3(no, face_normals[tri_faces[index]]);
  }
  else {
    normal_tri_v3(no, UNPACK3(face));
  }

  madd_v3_v3v3fl(co, ray->origin, ray->direction, dist);
  dist /= isec->vec_length;
  if (dist < hit->dist) {
    hit->index = index;
    hit->dist = dist;
    copy_v3_v3(hit->co, co);

    isec->isect = (dot_v3v3(no, ray->direction) <= 0.0f);
    isec->lambda = dist;
  }
}

static MDefBoundIsect *meshdeform_ray_tree_intersect(MeshDeformBind *mdb,
                                                     const float co1[3],
                                                     const float co2[3])
{
  BVHTreeRayHit hit;
  MeshDeformIsect isect_mdef;
  MeshRayCallbackData data = {
      mdb,
      &isect_mdef,
  };
  float end[3], vec_normal[3];

  /* happens binding when a cage has no faces */
  if (UNLIKELY(mdb->bvhtree == nullptr)) {
    return nullptr;
  }

  /* setup isec */
  memset(&isect_mdef, 0, sizeof(isect_mdef));
  isect_mdef.lambda = 1e10f;

  copy_v3_v3(isect_mdef.start, co1);
  copy_v3_v3(end, co2);
  sub_v3_v3v3(isect_mdef.vec, end, isect_mdef.start);
  isect_mdef.vec_length = normalize_v3_v3(vec_normal, isect_mdef.vec);

  hit.index = -1;
  hit.dist = BVH_RAYCAST_DIST_MAX;
  if (BLI_bvhtree_ray_cast_ex(mdb->bvhtree,
                              isect_mdef.start,
                              vec_normal,
                              0.0,
                              &hit,
                              harmonic_ray_callback,
                              &data,
                              BVH_RAYCAST_WATERTIGHT) != -1)
  {
    const Span<int> corner_verts = mdb->cagemesh_cache.corner_verts;
    const int face_i = mdb->cagemesh_cache.tri_faces[hit.index];
    const IndexRange face = mdb->cagemesh_cache.faces[face_i];
    const float (*cagecos)[3] = mdb->cagecos;
    const float len = isect_mdef.lambda;
    MDefBoundIsect *isect;

    Array<float3, 64> mp_cagecos(face.size());

    /* create MDefBoundIsect, and extra for 'poly_weights[]' */
    isect = static_cast<MDefBoundIsect *>(
        BLI_memarena_alloc(mdb->memarena, sizeof(*isect) + (sizeof(float) * face.size())));

    /* compute intersection coordinate */
    madd_v3_v3v3fl(isect->co, co1, isect_mdef.vec, len);

    isect->facing = isect_mdef.isect;

    isect->face_index = face_i;

    isect->len = max_ff(len_v3v3(co1, isect->co), MESHDEFORM_LEN_THRESHOLD);

    /* compute mean value coordinates for interpolation */
    for (int i = 0; i < face.size(); i++) {
      copy_v3_v3(mp_cagecos[i], cagecos[corner_verts[face[i]]]);
    }

    interp_weights_poly_v3(isect->poly_weights,
                           reinterpret_cast<float (*)[3]>(mp_cagecos.data()),
                           face.size(),
                           isect->co);

    return isect;
  }

  return nullptr;
}

static int meshdeform_inside_cage(MeshDeformBind *mdb, float *co)
{
  MDefBoundIsect *isect;
  float outside[3], start[3], dir[3];
  int i;

  for (i = 1; i <= 6; i++) {
    outside[0] = co[0] + (mdb->max[0] - mdb->min[0] + 1.0f) * MESHDEFORM_OFFSET[i][0];
    outside[1] = co[1] + (mdb->max[1] - mdb->min[1] + 1.0f) * MESHDEFORM_OFFSET[i][1];
    outside[2] = co[2] + (mdb->max[2] - mdb->min[2] + 1.0f) * MESHDEFORM_OFFSET[i][2];

    copy_v3_v3(start, co);
    sub_v3_v3v3(dir, outside, start);
    normalize_v3(dir);

    isect = meshdeform_ray_tree_intersect(mdb, start, outside);
    if (isect && !isect->facing) {
      return 1;
    }
  }

  return 0;
}

/* solving */

BLI_INLINE int meshdeform_index(MeshDeformBind *mdb, int x, int y, int z, int n)
{
  int size = mdb->size;

  x += MESHDEFORM_OFFSET[n][0];
  y += MESHDEFORM_OFFSET[n][1];
  z += MESHDEFORM_OFFSET[n][2];

  if (x < 0 || x >= mdb->size) {
    return -1;
  }
  if (y < 0 || y >= mdb->size) {
    return -1;
  }
  if (z < 0 || z >= mdb->size) {
    return -1;
  }

  return x + y * size + z * size * size;
}

BLI_INLINE void meshdeform_cell_center(
    MeshDeformBind *mdb, int x, int y, int z, int n, float *center)
{
  x += MESHDEFORM_OFFSET[n][0];
  y += MESHDEFORM_OFFSET[n][1];
  z += MESHDEFORM_OFFSET[n][2];

  center[0] = mdb->min[0] + x * mdb->width[0] + mdb->halfwidth[0];
  center[1] = mdb->min[1] + y * mdb->width[1] + mdb->halfwidth[1];
  center[2] = mdb->min[2] + z * mdb->width[2] + mdb->halfwidth[2];
}

static void meshdeform_add_intersections(MeshDeformBind *mdb, int x, int y, int z)
{
  MDefBoundIsect *isect;
  float center[3], ncenter[3];
  int i, a;

  a = meshdeform_index(mdb, x, y, z, 0);
  meshdeform_cell_center(mdb, x, y, z, 0, center);

  /* check each outgoing edge for intersection */
  for (i = 1; i <= 6; i++) {
    if (meshdeform_index(mdb, x, y, z, i) == -1) {
      continue;
    }

    meshdeform_cell_center(mdb, x, y, z, i, ncenter);

    isect = meshdeform_ray_tree_intersect(mdb, center, ncenter);
    if (isect) {
      mdb->boundisect[a][i - 1] = isect;
      mdb->tag[a] = MESHDEFORM_TAG_BOUNDARY;
    }
  }
}

static void meshdeform_bind_floodfill(MeshDeformBind *mdb)
{
  int *stack, *tag = mdb->tag;
  int a, b, i, xyz[3], stacksize, size = mdb->size;

  stack = MEM_new_array_zeroed<int>(mdb->size3, __func__);

  /* we know lower left corner is EXTERIOR because of padding */
  tag[0] = MESHDEFORM_TAG_EXTERIOR;
  stack[0] = 0;
  stacksize = 1;

  /* floodfill exterior tag */
  while (stacksize > 0) {
    a = stack[--stacksize];

    xyz[2] = a / (size * size);
    xyz[1] = (a - xyz[2] * size * size) / size;
    xyz[0] = a - xyz[1] * size - xyz[2] * size * size;

    for (i = 1; i <= 6; i++) {
      b = meshdeform_index(mdb, xyz[0], xyz[1], xyz[2], i);

      if (b != -1) {
        if (tag[b] == MESHDEFORM_TAG_UNTYPED ||
            (tag[b] == MESHDEFORM_TAG_BOUNDARY && !mdb->boundisect[a][i - 1]))
        {
          tag[b] = MESHDEFORM_TAG_EXTERIOR;
          stack[stacksize++] = b;
        }
      }
    }
  }

  /* other cells are interior */
  for (a = 0; a < size * size * size; a++) {
    if (tag[a] == MESHDEFORM_TAG_UNTYPED) {
      tag[a] = MESHDEFORM_TAG_INTERIOR;
    }
  }

#if 0
  {
    int tb, ti, te, ts;
    tb = ti = te = ts = 0;
    for (a = 0; a < size * size * size; a++) {
      if (tag[a] == MESHDEFORM_TAG_BOUNDARY) {
        tb++;
      }
      else if (tag[a] == MESHDEFORM_TAG_INTERIOR) {
        ti++;
      }
      else if (tag[a] == MESHDEFORM_TAG_EXTERIOR) {
        te++;

        if (mdb->semibound[a]) {
          ts++;
        }
      }
    }

    printf("interior %d exterior %d boundary %d semi-boundary %d\n", ti, te, tb, ts);
  }
#endif

  MEM_delete(stack);
}

static float meshdeform_boundary_phi(const MeshDeformBind *mdb,
                                     const MDefBoundIsect *isect,
                                     int cagevert)
{
  const IndexRange face = mdb->cagemesh_cache.faces[isect->face_index];
  const Span<int> corner_verts = mdb->cagemesh_cache.corner_verts;

  for (int i = 0; i < face.size(); i++) {
    if (corner_verts[face[i]] == cagevert) {
      return isect->poly_weights[i];
    }
  }

  return 0.0f;
}

static float meshdeform_interp_w(MeshDeformBind *mdb,
                                 const float *gridvec,
                                 float * /*vec*/,
                                 int /*cagevert*/)
{
  float dvec[3], ivec[3], result = 0.0f;
  float totweight = 0.0f;

  for (int i = 0; i < 3; i++) {
    ivec[i] = int(gridvec[i]);
    dvec[i] = gridvec[i] - ivec[i];
  }

  for (int i = 0; i < 8; i++) {
    int x, y, z;
    float wx, wy, wz;

    if (i & 1) {
      x = ivec[0] + 1;
      wx = dvec[0];
    }
    else {
      x = ivec[0];
      wx = 1.0f - dvec[0];
    }

    if (i & 2) {
      y = ivec[1] + 1;
      wy = dvec[1];
    }
    else {
      y = ivec[1];
      wy = 1.0f - dvec[1];
    }

    if (i & 4) {
      z = ivec[2] + 1;
      wz = dvec[2];
    }
    else {
      z = ivec[2];
      wz = 1.0f - dvec[2];
    }

    CLAMP(x, 0, mdb->size - 1);
    CLAMP(y, 0, mdb->size - 1);
    CLAMP(z, 0, mdb->size - 1);

    int a = meshdeform_index(mdb, x, y, z, 0);
    float weight = wx * wy * wz;
    result += weight * mdb->phi[a];
    totweight += weight;
  }

  if (totweight > 0.0f) {
    result /= totweight;
  }

  return result;
}

static void meshdeform_check_semibound(MeshDeformBind *mdb, int x, int y, int z)
{
  int i, a;

  a = meshdeform_index(mdb, x, y, z, 0);
  if (mdb->tag[a] != MESHDEFORM_TAG_EXTERIOR) {
    return;
  }

  for (i = 1; i <= 6; i++) {
    if (mdb->boundisect[a][i - 1]) {
      mdb->semibound[a] = 1;
    }
  }
}

static float meshdeform_boundary_total_weight(MeshDeformBind *mdb, int x, int y, int z)
{
  float weight, totweight = 0.0f;
  int i, a;

  a = meshdeform_index(mdb, x, y, z, 0);

  /* count weight for neighbor cells */
  for (i = 1; i <= 6; i++) {
    if (meshdeform_index(mdb, x, y, z, i) == -1) {
      continue;
    }

    if (mdb->boundisect[a][i - 1]) {
      weight = 1.0f / mdb->boundisect[a][i - 1]->len;
    }
    else if (!mdb->semibound[a]) {
      weight = 1.0f / mdb->width[0];
    }
    else {
      weight = 0.0f;
    }

    totweight += weight;
  }

  return totweight;
}

static void meshdeform_matrix_add_cell(
    MeshDeformBind *mdb, LinearSolver *context, int x, int y, int z)
{
  MDefBoundIsect *isect;
  float weight, totweight;
  int i, a, acenter;

  acenter = meshdeform_index(mdb, x, y, z, 0);
  if (mdb->tag[acenter] == MESHDEFORM_TAG_EXTERIOR) {
    return;
  }

  EIG_linear_solver_matrix_add(context, mdb->varidx[acenter], mdb->varidx[acenter], 1.0f);

  totweight = meshdeform_boundary_total_weight(mdb, x, y, z);
  for (i = 1; i <= 6; i++) {
    a = meshdeform_index(mdb, x, y, z, i);
    if (a == -1 || mdb->tag[a] == MESHDEFORM_TAG_EXTERIOR) {
      continue;
    }

    isect = mdb->boundisect[acenter][i - 1];
    if (!isect) {
      weight = (1.0f / mdb->width[0]) / totweight;
      EIG_linear_solver_matrix_add(context, mdb->varidx[acenter], mdb->varidx[a], -weight);
    }
  }
}

static void meshdeform_matrix_add_rhs(
    MeshDeformBind *mdb, LinearSolver *context, int x, int y, int z, int cagevert)
{
  MDefBoundIsect *isect;
  float rhs, weight, totweight;
  int i, a, acenter;

  acenter = meshdeform_index(mdb, x, y, z, 0);
  if (mdb->tag[acenter] == MESHDEFORM_TAG_EXTERIOR) {
    return;
  }

  totweight = meshdeform_boundary_total_weight(mdb, x, y, z);
  for (i = 1; i <= 6; i++) {
    a = meshdeform_index(mdb, x, y, z, i);
    if (a == -1) {
      continue;
    }

    isect = mdb->boundisect[acenter][i - 1];

    if (isect) {
      weight = (1.0f / isect->len) / totweight;
      rhs = weight * meshdeform_boundary_phi(mdb, isect, cagevert);
      EIG_linear_solver_right_hand_side_add(context, 0, mdb->varidx[acenter], rhs);
    }
  }
}

static void meshdeform_matrix_add_semibound_phi(
    MeshDeformBind *mdb, int x, int y, int z, int cagevert)
{
  MDefBoundIsect *isect;
  float rhs, weight, totweight;
  int i, a;

  a = meshdeform_index(mdb, x, y, z, 0);
  if (!mdb->semibound[a]) {
    return;
  }

  mdb->phi[a] = 0.0f;

  totweight = meshdeform_boundary_total_weight(mdb, x, y, z);
  for (i = 1; i <= 6; i++) {
    isect = mdb->boundisect[a][i - 1];

    if (isect) {
      weight = (1.0f / isect->len) / totweight;
      rhs = weight * meshdeform_boundary_phi(mdb, isect, cagevert);
      mdb->phi[a] += rhs;
    }
  }
}

static void meshdeform_matrix_add_exterior_phi(
    MeshDeformBind *mdb, int x, int y, int z, int /*cagevert*/)
{
  float phi, totweight;
  int i, a, acenter;

  acenter = meshdeform_index(mdb, x, y, z, 0);
  if (mdb->tag[acenter] != MESHDEFORM_TAG_EXTERIOR || mdb->semibound[acenter]) {
    return;
  }

  phi = 0.0f;
  totweight = 0.0f;
  for (i = 1; i <= 6; i++) {
    a = meshdeform_index(mdb, x, y, z, i);

    if (a != -1 && mdb->semibound[a]) {
      phi += mdb->phi[a];
      totweight += 1.0f;
    }
  }

  if (totweight != 0.0f) {
    mdb->phi[acenter] = phi / totweight;
  }
}

static void meshdeform_matrix_solve(MeshDeformModifierData *mmd, MeshDeformBind *mdb)
{
  LinearSolver *context;
  float vec[3], gridvec[3];
  int a, b, x, y, z, totvar;
  char message[256];

  /* setup variable indices */
  mdb->varidx = MEM_new_array_zeroed<int>(mdb->size3, "MeshDeformDSvaridx");
  for (a = 0, totvar = 0; a < mdb->size3; a++) {
    mdb->varidx[a] = (mdb->tag[a] == MESHDEFORM_TAG_EXTERIOR) ? -1 : totvar++;
  }

  if (totvar == 0) {
    MEM_delete(mdb->varidx);
    return;
  }

  progress_bar(0, "Starting mesh deform solve");

  /* setup linear solver */
  context = EIG_linear_solver_new(totvar, totvar, 1);

  /* build matrix */
  for (z = 0; z < mdb->size; z++) {
    for (y = 0; y < mdb->size; y++) {
      for (x = 0; x < mdb->size; x++) {
        meshdeform_matrix_add_cell(mdb, context, x, y, z);
      }
    }
  }

  /* solve for each cage vert */
  for (a = 0; a < mdb->cage_verts_num; a++) {
    /* fill in right hand side and solve */
    for (z = 0; z < mdb->size; z++) {
      for (y = 0; y < mdb->size; y++) {
        for (x = 0; x < mdb->size; x++) {
          meshdeform_matrix_add_rhs(mdb, context, x, y, z, a);
        }
      }
    }

    if (EIG_linear_solver_solve(context)) {
      for (z = 0; z < mdb->size; z++) {
        for (y = 0; y < mdb->size; y++) {
          for (x = 0; x < mdb->size; x++) {
            meshdeform_matrix_add_semibound_phi(mdb, x, y, z, a);
          }
        }
      }

      for (z = 0; z < mdb->size; z++) {
        for (y = 0; y < mdb->size; y++) {
          for (x = 0; x < mdb->size; x++) {
            meshdeform_matrix_add_exterior_phi(mdb, x, y, z, a);
          }
        }
      }

      for (b = 0; b < mdb->size3; b++) {
        if (mdb->tag[b] != MESHDEFORM_TAG_EXTERIOR) {
          mdb->phi[b] = EIG_linear_solver_variable_get(context, 0, mdb->varidx[b]);
        }
        mdb->totalphi[b] += mdb->phi[b];
      }

      if (mdb->weights) {
        /* static bind : compute weights for each vertex */
        for (b = 0; b < mdb->verts_num; b++) {
          if (mdb->inside[b]) {
            copy_v3_v3(vec, mdb->vertexcos[b]);
            gridvec[0] = (vec[0] - mdb->min[0] - mdb->halfwidth[0]) / mdb->width[0];
            gridvec[1] = (vec[1] - mdb->min[1] - mdb->halfwidth[1]) / mdb->width[1];
            gridvec[2] = (vec[2] - mdb->min[2] - mdb->halfwidth[2]) / mdb->width[2];

            mdb->weights[b * mdb->cage_verts_num + a] = meshdeform_interp_w(mdb, gridvec, vec, a);
          }
        }
      }
      else {
        MDefBindInfluence *inf;

        /* dynamic bind */
        for (b = 0; b < mdb->size3; b++) {
          if (mdb->phi[b] >= MESHDEFORM_MIN_INFLUENCE) {
            inf = static_cast<MDefBindInfluence *>(
                BLI_memarena_alloc(mdb->memarena, sizeof(*inf)));
            inf->vertex = a;
            inf->weight = mdb->phi[b];
            inf->next = mdb->dyngrid[b];
            mdb->dyngrid[b] = inf;
          }
        }
      }
    }
    else {
      BKE_modifier_set_error(
          mmd->object, &mmd->modifier, "Failed to find bind solution (increase precision?)");
      error("Mesh Deform: failed to find bind solution.");
      break;
    }

    SNPRINTF_UTF8(message, "Mesh deform solve %d / %d       |||", a + 1, mdb->cage_verts_num);
    progress_bar(float(a + 1) / float(mdb->cage_verts_num), message);
  }

#if 0
  /* sanity check */
  for (b = 0; b < mdb->size3; b++) {
    if (mdb->tag[b] != MESHDEFORM_TAG_EXTERIOR) {
      if (fabsf(mdb->totalphi[b] - 1.0f) > 1e-4f) {
        printf("totalphi deficiency [%s|%d] %d: %.10f\n",
               (mdb->tag[b] == MESHDEFORM_TAG_INTERIOR) ? "interior" : "boundary",
               mdb->semibound[b],
               mdb->varidx[b],
               mdb->totalphi[b]);
      }
    }
  }
#endif

  /* free */
  MEM_delete(mdb->varidx);

  EIG_linear_solver_delete(context);
}

static void harmonic_coordinates_bind(MeshDeformModifierData *mmd, MeshDeformBind *mdb)
{
  MDefBindInfluence *inf;
  MDefInfluence *mdinf;
  MDefCell *cell;
  float center[3], vec[3], maxwidth, totweight;
  int a, b, x, y, z, totinside, offset;

  /* compute bounding box of the cage mesh */
  INIT_MINMAX(mdb->min, mdb->max);

  for (a = 0; a < mdb->cage_verts_num; a++) {
    minmax_v3v3_v3(mdb->min, mdb->max, mdb->cagecos[a]);
  }

  /* allocate memory */
  mdb->size = (2 << (mmd->gridsize - 1)) + 2;
  mdb->size3 = mdb->size * mdb->size * mdb->size;
  mdb->tag = MEM_new_array_zeroed<int>(mdb->size3, "MeshDeformBindTag");
  mdb->phi = MEM_new_array_zeroed<float>(mdb->size3, "MeshDeformBindPhi");
  mdb->totalphi = MEM_new_array_zeroed<float>(mdb->size3, "MeshDeformBindTotalPhi");
  mdb->boundisect = MEM_new_array_zeroed<MDefBoundIsect *[6]>(mdb->size3, "MDefBoundIsect");
  mdb->semibound = MEM_new_array_zeroed<int>(mdb->size3, "MDefSemiBound");
  mdb->bvhdata = mdb->cagemesh->bvh_corner_tris();
  mdb->bvhtree = mdb->bvhdata.tree;
  mdb->inside = MEM_new_array_zeroed<int>(mdb->verts_num, "MDefInside");

  if (mmd->flag & MOD_MDEF_DYNAMIC_BIND) {
    mdb->dyngrid = MEM_new_array_zeroed<MDefBindInfluence *>(mdb->size3, "MDefDynGrid");
  }
  else {
    mdb->weights = MEM_new_array_zeroed<float>(mdb->verts_num * mdb->cage_verts_num,
                                               "MDefWeights");
  }

  mdb->memarena = BLI_memarena_new(BLI_MEMARENA_STD_BUFSIZE, "harmonic coords arena");
  BLI_memarena_use_calloc(mdb->memarena);

  /* Initialize data from `cagedm` for reuse. */
  {
    Mesh *mesh = mdb->cagemesh;
    mdb->cagemesh_cache.faces = mesh->faces();
    mdb->cagemesh_cache.corner_verts = mesh->corner_verts();
    mdb->cagemesh_cache.corner_tris = mesh->corner_tris();
    mdb->cagemesh_cache.tri_faces = mesh->corner_tri_faces();
    mdb->cagemesh_cache.face_normals = mesh->face_normals();
  }

  /* make bounding box equal size in all directions, add padding, and compute
   * width of the cells */
  maxwidth = -1.0f;
  for (a = 0; a < 3; a++) {
    maxwidth = std::max(mdb->max[a] - mdb->min[a], maxwidth);
  }

  for (a = 0; a < 3; a++) {
    center[a] = (mdb->min[a] + mdb->max[a]) * 0.5f;
    mdb->min[a] = center[a] - maxwidth * 0.5f;
    mdb->max[a] = center[a] + maxwidth * 0.5f;

    mdb->width[a] = (mdb->max[a] - mdb->min[a]) / (mdb->size - 4);
    mdb->min[a] -= 2.1f * mdb->width[a];
    mdb->max[a] += 2.1f * mdb->width[a];

    mdb->width[a] = (mdb->max[a] - mdb->min[a]) / mdb->size;
    mdb->halfwidth[a] = mdb->width[a] * 0.5f;
  }

  progress_bar(0, "Setting up mesh deform system");

  totinside = 0;
  for (a = 0; a < mdb->verts_num; a++) {
    copy_v3_v3(vec, mdb->vertexcos[a]);
    mdb->inside[a] = meshdeform_inside_cage(mdb, vec);
    if (mdb->inside[a]) {
      totinside++;
    }
  }
  (void)totinside; /* Quiet set-but-unused warning (may be removed). */

  /* free temporary MDefBoundIsects */
  BLI_memarena_free(mdb->memarena);
  mdb->memarena = BLI_memarena_new(BLI_MEMARENA_STD_BUFSIZE, "harmonic coords arena");

  /* start with all cells untyped */
  for (a = 0; a < mdb->size3; a++) {
    mdb->tag[a] = MESHDEFORM_TAG_UNTYPED;
  }

  /* detect intersections and tag boundary cells */
  for (z = 0; z < mdb->size; z++) {
    for (y = 0; y < mdb->size; y++) {
      for (x = 0; x < mdb->size; x++) {
        meshdeform_add_intersections(mdb, x, y, z);
      }
    }
  }

  /* compute exterior and interior tags */
  meshdeform_bind_floodfill(mdb);

  for (z = 0; z < mdb->size; z++) {
    for (y = 0; y < mdb->size; y++) {
      for (x = 0; x < mdb->size; x++) {
        meshdeform_check_semibound(mdb, x, y, z);
      }
    }
  }

  /* solve */
  meshdeform_matrix_solve(mmd, mdb);

  /* assign results */
  if (mmd->flag & MOD_MDEF_DYNAMIC_BIND) {
    mmd->influences_num = 0;
    for (a = 0; a < mdb->size3; a++) {
      for (inf = mdb->dyngrid[a]; inf; inf = inf->next) {
        mmd->influences_num++;
      }
    }

    /* convert MDefBindInfluences to smaller MDefInfluences */
    mmd->dyngrid = MEM_new_array<MDefCell>(mdb->size3, "MDefDynGrid");
    mmd->dyngrid_sharing_info = implicit_sharing::info_for_mem_free(mmd->dyngrid);
    mmd->dyninfluences = MEM_new_array<MDefInfluence>(mmd->influences_num, "MDefInfluence");
    mmd->dyninfluences_sharing_info = implicit_sharing::info_for_mem_free(mmd->dyninfluences);
    offset = 0;
    for (a = 0; a < mdb->size3; a++) {
      cell = &mmd->dyngrid[a];
      cell->offset = offset;

      totweight = 0.0f;
      mdinf = mmd->dyninfluences + cell->offset;
      for (inf = mdb->dyngrid[a]; inf; inf = inf->next, mdinf++) {
        mdinf->weight = inf->weight;
        mdinf->vertex = inf->vertex;
        totweight += mdinf->weight;
        cell->influences_num++;
      }

      if (totweight > 0.0f) {
        mdinf = mmd->dyninfluences + cell->offset;
        for (b = 0; b < cell->influences_num; b++, mdinf++) {
          mdinf->weight /= totweight;
        }
      }

      offset += cell->influences_num;
    }

    mmd->dynverts = mdb->inside;
    mmd->dynverts_sharing_info = implicit_sharing::info_for_mem_free(mmd->dynverts);
    mmd->dyngridsize = mdb->size;
    copy_v3_v3(mmd->dyncellmin, mdb->min);
    mmd->dyncellwidth = mdb->width[0];
    MEM_delete(mdb->dyngrid);
  }
  else {
    mmd->bindweights = mdb->weights;
    MEM_delete(mdb->inside);
  }

  MEM_delete(mdb->tag);
  MEM_delete(mdb->phi);
  MEM_delete(mdb->totalphi);
  MEM_delete(mdb->boundisect);
  MEM_delete(mdb->semibound);
  BLI_memarena_free(mdb->memarena);
}

void ED_mesh_deform_bind_callback(Object *object,
                                  MeshDeformModifierData *mmd,
                                  Mesh *cagemesh,
                                  float *vertexcos,
                                  int verts_num,
                                  float cagemat[4][4])
{
  MeshDeformModifierData *mmd_orig = reinterpret_cast<MeshDeformModifierData *>(
      BKE_modifier_get_original(object, &mmd->modifier));
  MeshDeformBind mdb{};
  int a;

  waitcursor(1);
  start_progress_bar();

  /* No need to support other kinds of mesh data as binding is a one-off action. */
  BKE_mesh_wrapper_ensure_mdata(cagemesh);

  /* get mesh and cage mesh */
  mdb.vertexcos = MEM_new_array_zeroed<float[3]>(verts_num, "MeshDeformCos");
  mdb.verts_num = verts_num;

  mdb.cagemesh = cagemesh;
  mdb.cage_verts_num = mdb.cagemesh->verts_num;
  mdb.cagecos = MEM_new_array_zeroed<float[3]>(mdb.cage_verts_num, "MeshDeformBindCos");
  copy_m4_m4(mdb.cagemat, cagemat);

  const Span<float3> positions = mdb.cagemesh->vert_positions();
  for (a = 0; a < mdb.cage_verts_num; a++) {
    copy_v3_v3(mdb.cagecos[a], positions[a]);
  }
  for (a = 0; a < mdb.verts_num; a++) {
    mul_v3_m4v3(mdb.vertexcos[a], mdb.cagemat, vertexcos + a * 3);
  }

  /* solve */
  harmonic_coordinates_bind(mmd_orig, &mdb);

  /* assign bind variables */
  mmd_orig->bindcagecos = reinterpret_cast<float *>(mdb.cagecos);
  mmd_orig->bindcagecos_sharing_info = implicit_sharing::info_for_mem_free(mmd_orig->bindcagecos);
  mmd_orig->verts_num = mdb.verts_num;
  mmd_orig->cage_verts_num = mdb.cage_verts_num;
  copy_m4_m4(mmd_orig->bindmat, mmd_orig->object->object_to_world().ptr());

  /* transform bindcagecos to world space */
  for (a = 0; a < mdb.cage_verts_num; a++) {
    mul_m4_v3(mmd_orig->object->object_to_world().ptr(), mmd_orig->bindcagecos + a * 3);
  }

  /* free */
  MEM_delete(mdb.vertexcos);

  /* compact weights */
  BKE_modifier_mdef_compact_influences(reinterpret_cast<ModifierData *>(mmd_orig));

  end_progress_bar();
  waitcursor(0);
}

}  // namespace blender
