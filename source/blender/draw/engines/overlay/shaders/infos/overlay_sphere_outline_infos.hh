/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_view_infos.hh"
#  include "overlay_common_infos.hh"
#endif

#include "overlay_common_infos.hh"

GPU_SHADER_CREATE_INFO(overlay_sphere_outline_frag_output)
FRAGMENT_OUT(0, float4, frag_color)
FRAGMENT_OUT(1, float4, line_output)
GPU_SHADER_CREATE_END()

GPU_SHADER_INTERFACE_INFO(overlay_sphere_outline_iface)
FLAT(float4, final_color)
FLAT(float2, edge_start)
NO_PERSPECTIVE(float2, edge_pos)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_sphere_outline)
DO_STATIC_COMPILATION()
VERTEX_IN(0, float2, pos)
VERTEX_OUT(overlay_sphere_outline_iface)
VERTEX_SOURCE("overlay_sphere_outline_vert.glsl")
FRAGMENT_SOURCE("overlay_sphere_outline_frag.glsl")
ADDITIONAL_INFO(overlay_sphere_outline_frag_output)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_globals)
PUSH_CONSTANT(float, alpha)
STORAGE_BUF(0, read, float4x4, data_buf[])
GPU_SHADER_CREATE_END()

/* clang-format off */
CREATE_INFO_VARIANT(overlay_sphere_outline_selectable, overlay_sphere_outline, overlay_select)
CREATE_INFO_VARIANT(overlay_sphere_outline_clipped, overlay_sphere_outline, drw_clipped)
CREATE_INFO_VARIANT(overlay_sphere_outline_selectable_clipped, overlay_sphere_outline_selectable, drw_clipped)
/* clang-format on */
