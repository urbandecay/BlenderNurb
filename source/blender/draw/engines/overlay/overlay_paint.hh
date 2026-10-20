/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "draw_cache.hh"
#include "draw_cache_impl.hh"

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Display vertex paint selection overlays.
 */
class Paints : Overlay {

 private:
  /* Draw selection state on top of the mesh to communicate which areas can be painted on. */
  PassSimple paint_region_ps_ = {"paint_region_ps_"};
  PassSimple::Sub *paint_region_edge_ps_ = nullptr;
  PassSimple::Sub *paint_region_face_ps_ = nullptr;
  PassSimple::Sub *paint_region_vert_ps_ = nullptr;

  bool show_wires_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    enabled_ = state.is_space_v3d() && !res.is_selection() &&
               state.ctx_mode == CTX_MODE_PAINT_VERTEX;

    /* Init in any case to release the data. */
    paint_region_ps_.init();
    if (!enabled_) {
      return;
    }

    show_wires_ = state.overlay.paint_flag & V3D_OVERLAY_PAINT_WIRE;

    {
      auto &pass = paint_region_ps_;
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      {
        auto &sub = pass.sub("Face");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                          DRW_STATE_BLEND_ALPHA,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->paint_region_face.get());
        sub.push_constant("ucolor", float4(1.0, 1.0, 1.0, 0.2));
        paint_region_face_ps_ = &sub;
      }
      {
        auto &sub = pass.sub("Edge");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                          DRW_STATE_BLEND_ALPHA,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->paint_region_edge.get());
        paint_region_edge_ps_ = &sub;
      }
      {
        auto &sub = pass.sub("Vert");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->paint_region_vert.get());
        paint_region_vert_ps_ = &sub;
      }
    }

  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final
  {
    if (!enabled_) {
      return;
    }

    if (ob_ref.object->type != OB_MESH) {
      /* Only meshes are supported for now. */
      return;
    }

    if (state.ctx_mode != CTX_MODE_PAINT_VERTEX ||
        ob_ref.object->mode != OB_MODE_VERTEX_PAINT)
    {
        return;
    }

    /* Drawing of vertex paint color is done by the render engine (i.e. workbench). */

    /* Selection Display. */
    {
      /* NOTE(fclem): Why do we need original mesh here, only to get the flag? */
      const Mesh &mesh_orig = DRW_object_get_data_for_drawing<Mesh>(
          *DEG_get_original(ob_ref.object));
      const bool use_face_selection = (mesh_orig.editflag & ME_EDIT_PAINT_FACE_SEL);
      const bool use_vert_selection = (mesh_orig.editflag & ME_EDIT_PAINT_VERT_SEL);
      if (use_face_selection || show_wires_) {
        gpu::Batch *geom = DRW_cache_mesh_paint_overlay_edges_get(ob_ref.object);
        paint_region_edge_ps_->push_constant("use_select", use_face_selection);
        paint_region_edge_ps_->draw(geom, manager.unique_handle(ob_ref));
      }
      if (use_face_selection) {
        gpu::Batch *geom = DRW_cache_mesh_paint_overlay_surface_get(ob_ref.object);
        paint_region_face_ps_->draw(geom, manager.unique_handle(ob_ref));
      }
      if (use_vert_selection) {
        gpu::Batch *geom = DRW_cache_mesh_paint_overlay_verts_get(ob_ref.object);
        paint_region_vert_ps_->draw(geom, manager.unique_handle(ob_ref));
      }
    }
  }

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    /* TODO(fclem): Draw this onto the line frame-buffer to get wide-line and anti-aliasing.
     * Just need to make sure the shaders output line data. */
    manager.submit(paint_region_ps_, view);
  }
};

}  // namespace blender::draw::overlay
