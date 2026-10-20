/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <utility>

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "DNA_mesh_types.h"
#include "DNA_nurbs_types.h"
#include "DNA_object_types.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_anim_data.hh"
#include "BKE_geometry_set.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_nurbs.hh"
#include "BKE_nurbs_mesh.hh"
#include "BKE_nurbs_topology.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "BLO_read_write.hh"

#include "nurbs_eval.hh"

namespace blender {

static CLG_LogRef LOG = {"bke.nurbs"};

NurbsCurve *BKE_nurbs_curve_copy(const NurbsCurve &curve)
{
  NurbsCurve *copy = MEM_new<NurbsCurve>(__func__);
  *copy = curve;
  copy->next = nullptr;
  copy->prev = nullptr;
  copy->control_vertices = curve.control_vertex_count > 0 && curve.control_vertices != nullptr ?
                               MEM_new_array<NurbsControlVertex>(curve.control_vertex_count,
                                                                 __func__) :
                               nullptr;
  if (copy->control_vertices != nullptr) {
    std::copy_n(curve.control_vertices, curve.control_vertex_count, copy->control_vertices);
  }
  copy->knots = curve.knot_count > 0 && curve.knots != nullptr ?
                    MEM_new_array<double>(curve.knot_count, __func__) :
                    nullptr;
  if (copy->knots != nullptr) {
    std::copy_n(curve.knots, curve.knot_count, copy->knots);
  }
  return copy;
}

void BKE_nurbs_curve_free(NurbsCurve *curve)
{
  if (curve == nullptr) {
    return;
  }
  MEM_delete(curve->control_vertices);
  MEM_delete(curve->knots);
  MEM_delete(curve);
}

void BKE_nurbs_curve_list_free(ListBaseT<NurbsCurve> &curves)
{
  while (NurbsCurve *curve = static_cast<NurbsCurve *>(BLI_pophead(&curves))) {
    BKE_nurbs_curve_free(curve);
  }
}

void BKE_nurbs_curve_list_copy(ListBaseT<NurbsCurve> &destination,
                               const ListBaseT<NurbsCurve> &source)
{
  if (&destination == &source) {
    return;
  }
  BKE_nurbs_curve_list_free(destination);
  for (const NurbsCurve &curve : source) {
    BLI_addtail(&destination, BKE_nurbs_curve_copy(curve));
  }
}

std::size_t BKE_nurbs_curve_list_memory_size(const ListBaseT<NurbsCurve> &curves)
{
  std::size_t size = 0;
  for (const NurbsCurve &curve : curves) {
    size += sizeof(NurbsCurve) +
            sizeof(NurbsControlVertex) * std::size_t(std::max(0, curve.control_vertex_count)) +
            sizeof(double) * std::size_t(std::max(0, curve.knot_count));
  }
  return size;
}

static void nurbs_init_data(ID *id)
{
  NurbsGeometry *geometry = id_cast<NurbsGeometry *>(id);
  INIT_DEFAULT_STRUCT_AFTER(geometry, id);
}

static void nurbs_copy_data(Main * /*bmain*/,
                            std::optional<Library *> /*owner_library*/,
                            ID *id_dst,
                            const ID *id_src,
                            const int /*flag*/)
{
  NurbsGeometry *geometry_dst = id_cast<NurbsGeometry *>(id_dst);
  const NurbsGeometry *geometry_src = id_cast<const NurbsGeometry *>(id_src);

  /* The copied ID still contains the source list links; detach without freeing those records. */
  geometry_dst->curves.clear_no_delete();
  BKE_nurbs_curve_list_copy(geometry_dst->curves, geometry_src->curves);
  geometry_dst->next_component_id = geometry_src->next_component_id;
  geometry_dst->active_control_vertex_id = geometry_src->active_control_vertex_id;
  geometry_dst->batch_cache = nullptr;
  geometry_dst->topology = geometry_src->topology ?
                               BKE_nurbs_topology_copy(*geometry_src->topology) :
                               nullptr;
}

static void nurbs_free_data(ID *id)
{
  NurbsGeometry *geometry = id_cast<NurbsGeometry *>(id);
  BKE_nurbs_batch_cache_free(geometry);
  BKE_animdata_free(&geometry->id, false);

  BKE_nurbs_curve_list_free(geometry->curves);
  BKE_nurbs_topology_free(geometry->topology);
  geometry->topology = nullptr;
}

static void nurbs_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  NurbsGeometry *geometry = id_cast<NurbsGeometry *>(id);
  geometry->batch_cache = nullptr;

  writer->write_id_struct(id_address, geometry);
  BKE_id_blend_write(writer, &geometry->id);

  writer->write_struct_list(&geometry->curves);
  for (const NurbsCurve &curve : geometry->curves) {
    writer->write_struct_array(curve.control_vertex_count, curve.control_vertices);
    writer->write_double_array(curve.knot_count, curve.knots);
  }
  if (geometry->topology) {
    writer->write_struct(geometry->topology);
    BKE_nurbs_topology_blend_write(writer, *geometry->topology);
  }
}

static void nurbs_blend_read_data(BlendDataReader *reader, ID *id)
{
  NurbsGeometry *geometry = id_cast<NurbsGeometry *>(id);
  geometry->batch_cache = nullptr;

  BLO_read_struct_list(reader, NurbsCurve, &geometry->curves);
  for (NurbsCurve *curve = static_cast<NurbsCurve *>(geometry->curves.first); curve != nullptr;) {
    NurbsCurve *next = curve->next;
    const bool control_vertices_ok = BLO_read_array(
        reader, &curve->control_vertices, std::max(0, curve->control_vertex_count));
    const bool knots_ok = BLO_read_array(reader, &curve->knots, std::max(0, curve->knot_count));
    /* Native object curves are always object-local XYZ curves. Dimension-2 curves are reserved
     * for UV supports owned by topology and must not enter the standalone geometry ID. */
    if (!control_vertices_ok || !knots_ok || curve->dimension != 3 ||
        !BKE_nurbs_curve_is_valid(*curve))
    {
      BLO_reportf_wrap(BLO_read_data_reports(reader),
                       RPT_WARNING,
                       RPT_("Invalid NURBS curve removed from '%s'"),
                       geometry->id.name + 2);
      geometry->active_control_vertex_id = 0;
      BLI_remlink(&geometry->curves, curve);
      BKE_nurbs_curve_free(curve);
    }
    curve = next;
  }
  BLO_read_struct(reader, NurbsTopology, &geometry->topology);
  if (geometry->topology) {
    const bool arrays_ok = BKE_nurbs_topology_blend_read(reader, *geometry->topology);
    if (!arrays_ok || !BKE_nurbs_topology_is_valid(geometry->curves, *geometry->topology)) {
      BLO_reportf_wrap(BLO_read_data_reports(reader),
                       RPT_WARNING,
                       RPT_("Invalid NURBS topology removed from '%s'"),
                       geometry->id.name + 2);
      BKE_nurbs_topology_free(geometry->topology);
      geometry->topology = nullptr;
    }
  }

  const uint64_t next_component_id = BKE_nurbs_component_id_next(geometry->curves,
                                                                 geometry->topology);
  if (geometry->next_component_id != next_component_id &&
      (next_component_id == 0 || geometry->next_component_id == 0 ||
       geometry->next_component_id < next_component_id))
  {
    /* A zero result means UINT64_MAX is already allocated, so the allocator is exhausted. */
    geometry->next_component_id = next_component_id;
    BLO_reportf_wrap(BLO_read_data_reports(reader),
                     RPT_WARNING,
                     RPT_("Invalid NURBS component ID allocator repaired in '%s'"),
                     geometry->id.name + 2);
  }
}

IDTypeInfo IDType_ID_NU = {
    .id_code = NurbsGeometry::id_type,
    .id_filter = FILTER_ID_NU,
    .dependencies_id_types = 0,
    .main_listbase_index = INDEX_ID_NU,
    .struct_size = sizeof(NurbsGeometry),
    .name = "NURBS Geometry",
    .name_plural = N_("nurbs_geometries"),
    .translation_context = BLT_I18NCONTEXT_ID_ID,
    .flags = IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    .asset_type_info = nullptr,

    .init_data = nurbs_init_data,
    .copy_data = nurbs_copy_data,
    .free_data = nurbs_free_data,
    .make_local = nullptr,
    .foreach_id = nullptr,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = nullptr,
    .owner_pointer_get = nullptr,

    .blend_write = nurbs_blend_write,
    .blend_read_data = nurbs_blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

NurbsGeometry *BKE_nurbs_add(Main *bmain, const char *name)
{
  NurbsGeometry *geometry = BKE_id_new<NurbsGeometry>(bmain, name);

  /* Start the user with an editable, open cubic NURBS curve. The knot array uses the reduced
   * Rhino/openNURBS convention, so the endpoint knots are each stored three times. */
  constexpr int control_vertex_count = 4;
  constexpr int knot_count = 6;
  NurbsCurve *curve = MEM_new<NurbsCurve>(__func__);
  curve->id = geometry->next_component_id++;
  curve->degree = 3;
  curve->order = 4;
  curve->dimension = 3;
  curve->control_vertex_count = control_vertex_count;
  curve->knot_count = knot_count;
  curve->domain[0] = 0.0;
  curve->domain[1] = 1.0;
  curve->control_vertices = MEM_new_array<NurbsControlVertex>(control_vertex_count, __func__);
  curve->knots = MEM_new_array<double>(knot_count, __func__);

  const double positions[control_vertex_count][3] = {
      {-1.5, 0.0, 0.0}, {-0.5, 1.0, 0.0}, {0.5, -1.0, 0.0}, {1.5, 0.0, 0.0}};
  for (int i = 0; i < control_vertex_count; i++) {
    curve->control_vertices[i].id = geometry->next_component_id++;
    for (int axis = 0; axis < 3; axis++) {
      curve->control_vertices[i].position[axis] = positions[i][axis];
    }
    curve->control_vertices[i].weight = 1.0;
    curve->control_vertices[i].flag = NURBS_CV_SELECT;
  }
  const double knots[knot_count] = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
  std::copy(knots, knots + knot_count, curve->knots);
  BLI_addtail(&geometry->curves, curve);
  geometry->active_control_vertex_id = curve->control_vertices[0].id;
  return geometry;
}

NurbsGeometry *BKE_nurbs_add_path(Main *bmain, const char *name)
{
  NurbsGeometry *geometry = BKE_id_new<NurbsGeometry>(bmain, name);

  /* Match Blender's former five-point NURBS path while storing it as a clamped degree-four
   * curve. With the reduced Rhino/openNURBS knot convention, each clamped endpoint occurs four
   * times in the stored array. */
  constexpr int control_vertex_count = 5;
  constexpr int knot_count = 8;
  const double knots[knot_count] = {0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0};

  NurbsCurve *curve = MEM_new<NurbsCurve>(__func__);
  curve->id = geometry->next_component_id++;
  curve->degree = 4;
  curve->order = 5;
  curve->dimension = 3;
  curve->control_vertex_count = control_vertex_count;
  curve->knot_count = knot_count;
  curve->domain[0] = 0.0;
  curve->domain[1] = 1.0;
  curve->control_vertices = MEM_new_array<NurbsControlVertex>(control_vertex_count, __func__);
  curve->knots = MEM_new_array<double>(knot_count, __func__);

  for (int i = 0; i < control_vertex_count; i++) {
    NurbsControlVertex &control_vertex = curve->control_vertices[i];
    control_vertex.id = geometry->next_component_id++;
    control_vertex.position[0] = double(i - 2);
    control_vertex.weight = 1.0;
    control_vertex.flag = NURBS_CV_SELECT;
  }
  std::copy(knots, knots + knot_count, curve->knots);
  geometry->active_control_vertex_id = curve->control_vertices[0].id;
  BLI_addtail(&geometry->curves, curve);
  return geometry;
}

NurbsGeometry *BKE_nurbs_add_circle(Main *bmain, const char *name)
{
  NurbsGeometry *geometry = BKE_id_new<NurbsGeometry>(bmain, name);

  constexpr int control_vertex_count = 9;
  constexpr int knot_count = control_vertex_count + 3 - 2;
  constexpr double half_sqrt_two = 0.70710678118654752440;
  const double positions[control_vertex_count][3] = {
      {1.0, 0.0, 0.0},
      {1.0, 1.0, 0.0},
      {0.0, 1.0, 0.0},
      {-1.0, 1.0, 0.0},
      {-1.0, 0.0, 0.0},
      {-1.0, -1.0, 0.0},
      {0.0, -1.0, 0.0},
      {1.0, -1.0, 0.0},
      {1.0, 0.0, 0.0},
  };
  const double weights[control_vertex_count] = {
      1.0, half_sqrt_two, 1.0, half_sqrt_two, 1.0, half_sqrt_two, 1.0, half_sqrt_two, 1.0};
  const double knots[knot_count] = {0.0, 0.0, 1.0, 1.0, 2.0, 2.0, 3.0, 3.0, 4.0, 4.0};

  NurbsCurve *curve = MEM_new<NurbsCurve>(__func__);
  curve->id = geometry->next_component_id++;
  curve->degree = 2;
  curve->order = 3;
  curve->dimension = 3;
  curve->rational = true;
  curve->control_vertex_count = control_vertex_count;
  curve->knot_count = knot_count;
  curve->domain[0] = 0.0;
  curve->domain[1] = 4.0;
  curve->control_vertices = MEM_new_array<NurbsControlVertex>(control_vertex_count, __func__);
  curve->knots = MEM_new_array<double>(knot_count, __func__);

  for (int i = 0; i < control_vertex_count; i++) {
    NurbsControlVertex &control_vertex = curve->control_vertices[i];
    control_vertex.id = geometry->next_component_id++;
    copy_v3_v3_db(control_vertex.position, positions[i]);
    control_vertex.weight = weights[i];
    control_vertex.flag = NURBS_CV_SELECT;
  }
  std::copy(knots, knots + knot_count, curve->knots);
  geometry->active_control_vertex_id = curve->control_vertices[0].id;
  BLI_addtail(&geometry->curves, curve);
  return geometry;
}

NurbsGeometry *BKE_nurbs_add_line(Main *bmain,
                                 const char *name,
                                 const double start[3],
                                 const double end[3])
{
  if (start == nullptr || end == nullptr) {
    return nullptr;
  }
  for (int axis = 0; axis < 3; axis++) {
    if (!std::isfinite(start[axis]) || !std::isfinite(end[axis])) {
      return nullptr;
    }
  }

  NurbsGeometry *geometry = BKE_id_new<NurbsGeometry>(bmain, name);
  NurbsCurve *curve = MEM_new<NurbsCurve>(__func__);
  curve->id = geometry->next_component_id++;
  curve->degree = 1;
  curve->order = 2;
  curve->dimension = 3;
  curve->control_vertex_count = 2;
  curve->knot_count = 2;
  curve->domain[0] = 0.0;
  curve->domain[1] = 1.0;
  curve->control_vertices = MEM_new_array<NurbsControlVertex>(2, __func__);
  curve->knots = MEM_new_array<double>(2, __func__);

  for (int i = 0; i < 2; i++) {
    NurbsControlVertex &control_vertex = curve->control_vertices[i];
    control_vertex.id = geometry->next_component_id++;
    const double *position = i == 0 ? start : end;
    for (int axis = 0; axis < 3; axis++) {
      control_vertex.position[axis] = position[axis];
    }
    control_vertex.weight = 1.0;
    control_vertex.flag = NURBS_CV_SELECT;
  }
  curve->knots[0] = 0.0;
  curve->knots[1] = 1.0;
  geometry->active_control_vertex_id = curve->control_vertices[0].id;
  BLI_addtail(&geometry->curves, curve);
  return geometry;
}

NurbsGeometry *BKE_nurbs_add_arc(Main *bmain,
                                 const char *name,
                                 const double radius,
                                 const double start_angle,
                                 const double sweep_angle)
{
  constexpr double pi = 3.14159265358979323846264338327950288;
  constexpr double two_pi = 2.0 * pi;
  constexpr double angle_tolerance = 1e-6;
  if (!std::isfinite(radius) || radius <= 0.0 || !std::isfinite(start_angle) ||
      !std::isfinite(sweep_angle) || sweep_angle == 0.0 ||
      std::abs(sweep_angle) > two_pi + angle_tolerance)
  {
    return nullptr;
  }

  const double quarter_turn_count = std::round(sweep_angle / (pi * 0.5));
  const double nearest_quarter_turn = quarter_turn_count * (pi * 0.5);
  const double exact_sweep = std::abs(sweep_angle) > two_pi ?
                                 std::copysign(two_pi, sweep_angle) :
                             std::abs(sweep_angle - nearest_quarter_turn) <= angle_tolerance ?
                                 nearest_quarter_turn :
                                 sweep_angle;
  const int span_count = std::max(1, int(std::ceil(std::abs(exact_sweep) / (pi * 0.5))));
  const int control_vertex_count = span_count * 2 + 1;
  const int knot_count = control_vertex_count + 1;
  const double span_angle = exact_sweep / span_count;
  NurbsGeometry *geometry = BKE_id_new<NurbsGeometry>(bmain, name);
  NurbsCurve *curve = MEM_new<NurbsCurve>(__func__);
  curve->id = geometry->next_component_id++;
  curve->degree = 2;
  curve->order = 3;
  curve->dimension = 3;
  curve->rational = true;
  curve->control_vertex_count = control_vertex_count;
  curve->knot_count = knot_count;
  curve->domain[0] = 0.0;
  curve->domain[1] = double(span_count);
  curve->control_vertices = MEM_new_array<NurbsControlVertex>(control_vertex_count, __func__);
  curve->knots = MEM_new_array<double>(knot_count, __func__);

  for (int span = 0; span < span_count; span++) {
    const double angle_start = start_angle + span_angle * span;
    const double angle_end = angle_start + span_angle;
    const double angle_middle = (angle_start + angle_end) * 0.5;
    const double weight = std::cos(std::abs(span_angle) * 0.5);
    const double positions[3][3] = {
        {radius * std::cos(angle_start), radius * std::sin(angle_start), 0.0},
        {radius * std::cos(angle_middle) / weight,
         radius * std::sin(angle_middle) / weight,
         0.0},
        {radius * std::cos(angle_end), radius * std::sin(angle_end), 0.0},
    };
    for (int local_index = 0; local_index < 3; local_index++) {
      const int index = span * 2 + local_index;
      if (span > 0 && local_index == 0) {
        continue;
      }
      NurbsControlVertex &control_vertex = curve->control_vertices[index];
      control_vertex.id = geometry->next_component_id++;
      for (int axis = 0; axis < 3; axis++) {
        control_vertex.position[axis] = positions[local_index][axis];
      }
      control_vertex.weight = local_index == 1 ? weight : 1.0;
      control_vertex.flag = NURBS_CV_SELECT;
    }
  }

  int knot_index = 0;
  curve->knots[knot_index++] = 0.0;
  curve->knots[knot_index++] = 0.0;
  for (int span = 1; span < span_count; span++) {
    curve->knots[knot_index++] = double(span);
    curve->knots[knot_index++] = double(span);
  }
  curve->knots[knot_index++] = double(span_count);
  curve->knots[knot_index++] = double(span_count);
  BLI_assert(knot_index == knot_count);

  geometry->active_control_vertex_id = curve->control_vertices[0].id;
  BLI_addtail(&geometry->curves, curve);
  return geometry;
}

static double nurbs_curve_full_knot_at(const NurbsCurve &curve, const int index)
{
  if (index == 0) {
    return curve.knots[0];
  }
  if (index == curve.knot_count + 1) {
    return curve.knots[curve.knot_count - 1];
  }
  return curve.knots[index - 1];
}

bool BKE_nurbs_curve_is_valid(const NurbsCurve &curve)
{
  if ((curve.dimension != 2 && curve.dimension != 3) || curve.order < 2 ||
      curve.degree != curve.order - 1 || curve.control_vertex_count < curve.order ||
      int64_t(curve.knot_count) != int64_t(curve.control_vertex_count) + curve.order - 2 ||
      curve.knots == nullptr || curve.control_vertices == nullptr ||
      !std::isfinite(curve.domain[0]) || !std::isfinite(curve.domain[1]) ||
      curve.domain[1] <= curve.domain[0])
  {
    return false;
  }

  for (int i = 0; i < curve.knot_count; i++) {
    if (!std::isfinite(curve.knots[i]) || (i > 0 && curve.knots[i] < curve.knots[i - 1])) {
      return false;
    }
  }

  if (curve.domain[0] != nurbs_curve_full_knot_at(curve, curve.degree) ||
      curve.domain[1] != nurbs_curve_full_knot_at(curve, curve.control_vertex_count))
  {
    return false;
  }
  for (int i = 0; i < curve.control_vertex_count; i++) {
    const NurbsControlVertex &control_vertex = curve.control_vertices[i];
    if (!std::isfinite(control_vertex.weight) || control_vertex.weight <= 0.0 ||
        !std::isfinite(control_vertex.position[0]) || !std::isfinite(control_vertex.position[1]) ||
        !std::isfinite(control_vertex.position[2]) ||
        (curve.dimension == 2 && control_vertex.position[2] != 0.0))
    {
      return false;
    }
  }
  return true;
}

bool BKE_nurbs_curve_evaluate(const NurbsCurve &curve,
                              const double parameter,
                              double r_position[3])
{
  if (!BKE_nurbs_curve_is_valid(curve) || !std::isfinite(parameter) ||
      parameter < curve.domain[0] || parameter > curve.domain[1])
  {
    return false;
  }
  const int control_vertex_count = curve.control_vertex_count;
  const int order = curve.order;
  int span;
  Vector<double> basis;
  if (!bke::nurbs::basis_weights(
          curve.knots, curve.knot_count, control_vertex_count, order, parameter, span, basis))
  {
    return false;
  }

  double homogeneous[3] = {};
  double weight_sum = 0.0;
  for (int i = 0; i < order; i++) {
    const NurbsControlVertex &control_vertex = curve.control_vertices[span - order + 1 + i];
    const double weight = curve.rational ? control_vertex.weight : 1.0;
    if (!std::isfinite(weight) || weight <= 0.0 || !std::isfinite(control_vertex.position[0]) ||
        !std::isfinite(control_vertex.position[1]) || !std::isfinite(control_vertex.position[2]))
    {
      return false;
    }
    const double weighted_basis = basis[i] * weight;
    weight_sum += weighted_basis;
    for (int axis = 0; axis < 3; axis++) {
      homogeneous[axis] += weighted_basis * control_vertex.position[axis];
    }
  }
  if (!std::isfinite(weight_sum) || weight_sum == 0.0) {
    return false;
  }
  for (int axis = 0; axis < 3; axis++) {
    r_position[axis] = homogeneous[axis] / weight_sum;
    if (!std::isfinite(r_position[axis])) {
      return false;
    }
  }
  return true;
}

void BKE_nurbs_transform(NurbsGeometry &geometry, const float4x4 &matrix)
{
  const double4x4 double_matrix(matrix);
  const auto transform_position = [&](double position[3]) {
    const double3 transformed = math::transform_point(double_matrix, double3(position));
    for (int axis = 0; axis < 3; axis++) {
      position[axis] = transformed[axis];
    }
  };
  for (NurbsCurve &curve : geometry.curves) {
    for (int i = 0; i < curve.control_vertex_count; i++) {
      transform_position(curve.control_vertices[i].position);
    }
  }
  if (geometry.topology) {
    for (NurbsSurface &surface : geometry.topology->surfaces) {
      const int64_t count = int64_t(surface.control_vertex_count_u) *
                            surface.control_vertex_count_v;
      for (int64_t i = 0; i < count; i++) {
        transform_position(surface.control_vertices[i].position);
      }
    }
    for (NurbsTopoVertex &vertex : geometry.topology->vertices) {
      transform_position(vertex.position);
    }
  }
  BKE_nurbs_batch_cache_dirty_tag(&geometry, BKE_NURBS_BATCH_DIRTY_ALL);
}

void BKE_nurbs_data_update(Object *object)
{
  BKE_object_free_derived_caches(object);

  const NurbsGeometry *geometry = id_cast<const NurbsGeometry *>(object->data);
  Mesh *mesh = BKE_nurbs_mesh_create(*geometry, 16);
  if (mesh == nullptr) {
    CLOG_WARN(&LOG, "Failed to evaluate NURBS geometry '%s'", geometry->id.name + 2);
    mesh = BKE_mesh_new_nomain(0, 0, 0, 0);
  }
  BKE_object_eval_assign_data(object, &mesh->id, true);
  bke::GeometrySet geometry_set = bke::GeometrySet::from_mesh(
      mesh, bke::GeometryOwnershipType::ReadOnly);
  object->runtime->geometry_set_eval = new bke::GeometrySet(std::move(geometry_set));
}

void (*BKE_nurbs_batch_cache_dirty_tag_cb)(NurbsGeometry *geometry, int mode) = nullptr;
void (*BKE_nurbs_batch_cache_free_cb)(NurbsGeometry *geometry) = nullptr;

void BKE_nurbs_batch_cache_dirty_tag(NurbsGeometry *geometry, const int mode)
{
  if (geometry->batch_cache && BKE_nurbs_batch_cache_dirty_tag_cb) {
    BKE_nurbs_batch_cache_dirty_tag_cb(geometry, mode);
  }
}

void BKE_nurbs_batch_cache_free(NurbsGeometry *geometry)
{
  if (geometry->batch_cache && BKE_nurbs_batch_cache_free_cb) {
    BKE_nurbs_batch_cache_free_cb(geometry);
  }
}

}  // namespace blender
