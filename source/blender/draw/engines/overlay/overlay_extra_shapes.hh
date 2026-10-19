/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "DRW_render.hh"
#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Extra primitive shapes used by camera reconstruction markers and force fields.
 *
 * These are helper display shapes, separate from scene object geometry.
 */
class ExtraShapes {
  using HelperInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;

 public:
  struct CallBuffers {
    const SelectionType selection_type_;
    HelperInstanceBuf plain_axes_buf = {selection_type_, "plain_axes_buf"};
    HelperInstanceBuf single_arrow_buf = {selection_type_, "single_arrow_buf"};
    HelperInstanceBuf cube_buf = {selection_type_, "cube_buf"};
    HelperInstanceBuf circle_buf = {selection_type_, "circle_buf"};
    HelperInstanceBuf sphere_buf = {selection_type_, "sphere_buf"};
    HelperInstanceBuf cone_buf = {selection_type_, "cone_buf"};
    HelperInstanceBuf arrows_buf = {selection_type_, "arrows_buf"};
  };

  static void begin_sync(CallBuffers &call_buffers)
  {
    call_buffers.plain_axes_buf.clear();
    call_buffers.single_arrow_buf.clear();
    call_buffers.cube_buf.clear();
    call_buffers.circle_buf.clear();
    call_buffers.sphere_buf.clear();
    call_buffers.cone_buf.clear();
    call_buffers.arrows_buf.clear();
  }

  static void object_sync(const select::ID select_id,
                          const float4x4 &matrix,
                          const float draw_size,
                          const char draw_type,
                          const float4 &color,
                          CallBuffers &call_buffers)
  {
    const ExtraInstanceData data(matrix, color, draw_size);

    switch (draw_type) {
      case OB_PLAINAXES:
        call_buffers.plain_axes_buf.append(data, select_id);
        break;
      case OB_SINGLE_ARROW:
        call_buffers.single_arrow_buf.append(data, select_id);
        break;
      case OB_CUBE:
        call_buffers.cube_buf.append(data, select_id);
        break;
      case OB_CIRCLE:
        call_buffers.circle_buf.append(data, select_id);
        break;
      case OB_HELPER_SPHERE:
        call_buffers.sphere_buf.append(data, select_id);
        break;
      case OB_HELPER_CONE:
        call_buffers.cone_buf.append(data, select_id);
        break;
      case OB_ARROWS:
        call_buffers.arrows_buf.append(data, select_id);
        break;
    }
  }

  static void end_sync(Resources &res,
                       const State &state,
                       PassSimple::Sub &pass,
                       CallBuffers &call_buffers)
  {
    pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                   state.clipping_plane_count);
    pass.shader_set(res.shaders->extra_shape.get());
    pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);

    call_buffers.plain_axes_buf.end_sync(pass, res.shapes.plain_axes.get());
    call_buffers.single_arrow_buf.end_sync(pass, res.shapes.single_arrow.get());
    call_buffers.cube_buf.end_sync(pass, res.shapes.cube.get());
    call_buffers.circle_buf.end_sync(pass, res.shapes.circle.get());
    call_buffers.sphere_buf.end_sync(pass, res.shapes.helper_sphere.get());
    call_buffers.cone_buf.end_sync(pass, res.shapes.helper_cone.get());
    call_buffers.arrows_buf.end_sync(pass, res.shapes.arrows.get());
  }
};

}  // namespace blender::draw::overlay
