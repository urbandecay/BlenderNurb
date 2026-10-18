/* SPDX-FileCopyrightText: 2017-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_particle_point)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

#define no_active_weight 666.0f

#define DISCARD_VERTEX \
  gl_Position = float4(0.0f, 0.0f, -3e36f, 0.0f); \
  return;

float3 weight_to_rgb(float t)
{
  if (t == no_active_weight) {
    /* No weight. */
    return theme.colors.wire.rgb;
  }
  if (t > 1.0f || t < 0.0f) {
    /* Error color */
    return float3(1.0f, 0.0f, 1.0f);
  }
  else {
    return texture(weight_tx, t).rgb;
  }
}

void main()
{
#ifdef CURVES_POINT
  bool is_active = (data & EDIT_CURVES_ACTIVE_HANDLE) != 0u;
  bool is_bezier_handle = (data & EDIT_CURVES_BEZIER_HANDLE) != 0u;

  if (is_bezier_handle && ((uint(curve_handle_display) == CURVE_HANDLE_NONE) ||
                           (uint(curve_handle_display) == CURVE_HANDLE_SELECTED) && !is_active))
  {
    DISCARD_VERTEX
  }
#endif

  float3 world_pos = drw_point_object_to_world(pos);
#ifdef CURVES_POINT
  {
    /* Offset the curve radius in the incoming direction,
     * so the curve geometry doesn't occlude the handle.
     * But ensure we don't offset past the near plane. */
    float3 I = drw_world_incident_vector(world_pos);
    float vs_depth = dot(drw_view_position() - world_pos, I);
    float near_plane_distance = abs(drw_view_near()) / dot(I, drw_view_forward());
    float max_offset = vs_depth - (near_plane_distance * 1.01f);
    max_offset = max(0.0f, max_offset);
    /* Compensate for view angle. */
    float radius = rad * 1.5f;
    world_pos += I * min(radius, max_offset);
  }
#endif
  gl_Position = drw_point_world_to_homogenous(world_pos);

  if (use_weight) {
    final_color = float4(weight_to_rgb(selection), 1.0f);
  }
  else {
    final_color = mix(theme.colors.vert, theme.colors.vert_select, selection);
  }

  gl_PointSize = theme.sizes.vert * 2.0f;

  view_clipping_distances(world_pos);
}
