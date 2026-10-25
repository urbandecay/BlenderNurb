/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_attribute.hh"
#include "BKE_nurbs_edit.hh"

#include "DNA_curves_types.h"

#include "GPU_capabilities.hh"

#include "UI_resources.hh"

#include "draw_cache.hh"
#include "draw_cache_impl.hh"

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Curve object display (including legacy curves) for both object and edit modes.
 */
class Curves : Overlay {
 private:
  PassSimple edit_curves_ps_ = {"Curve Edit"};
  PassSimple::Sub *edit_curves_lines_ = nullptr;

  PassSimple edit_curves_handles_ps_ = {"Curve Edit Handles"};
  PassSimple::Sub *edit_curves_points_ = nullptr;
  PassSimple::Sub *edit_curves_handles_ = nullptr;

  PassSimple edit_legacy_curve_ps_ = {"Legacy Curve Edit"};
  PassSimple::Sub *edit_legacy_curve_wires_ = nullptr;
  PassSimple::Sub *edit_legacy_curve_normals_ = nullptr;

  PassSimple edit_legacy_curve_handles_ps_ = {"Legacy Curve Edit Handles"};
  PassSimple::Sub *edit_legacy_curve_points_ = nullptr;
  PassSimple::Sub *edit_legacy_curve_handles_ = nullptr;

  PassSimple edit_legacy_surface_handles_ps = {"Surface Edit"};
  PassSimple::Sub *edit_legacy_surface_handles_ = nullptr;
  /* Handles that are below the geometry and are rendered with lower alpha. */
  PassSimple::Sub *edit_legacy_surface_xray_handles_ = nullptr;

  PassSimple native_faces_ps_ = {"NURBS Selected Faces"};
  PassSimple native_points_ps_ = {"NURBS Component Points"};
  PassSimple native_edges_ps_ = {"NURBS Edges"};
  PassSimple native_facedots_ps_ = {"NURBS Face Dots"};
  PassSimple native_depth_ps_ = {"NURBS Edit Depth"};
  bool native_xray_ = false;
  bool native_xray_enabled_ = false;
  bool native_show_facedots_ = false;
  bool native_needs_depth_ = false;

  View view_edit_cage = {"view_edit_cage"};
  View::OffsetData offset_data_;

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    enabled_ = state.is_space_v3d();

    if (!enabled_) {
      return;
    }

    offset_data_ = state.offset_data_get();
    native_xray_ = state.xray_flag_enabled;
    native_xray_enabled_ = state.xray_enabled;
    native_needs_depth_ = !state.is_render_depth_available;
    native_show_facedots_ = (state.overlay.edit_flag & V3D_OVERLAY_EDIT_FACE_DOT) || native_xray_;
    gpu::Texture **depth_texture = native_xray_ ? &res.depth_tx : &res.dummy_depth_tx;
    auto native_pass_init = [&](PassSimple &pass, gpu::Shader *shader, float alpha, float offset) {
      pass.init();
      pass.shader_set(shader);
      pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA,
                     state.clipping_plane_count);
      pass.bind_texture("depth_tx", depth_texture);
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      pass.push_constant("wire_shading", state.v3d->shading.type == OB_WIRE);
      pass.push_constant("select_face", false);
      pass.push_constant("select_edge", false);
      pass.push_constant("alpha", alpha);
      pass.push_constant("retopology_offset", 0.0f);
      pass.push_constant("ndc_offset_factor", &state.ndc_offset_factor);
      pass.push_constant("ndc_offset", offset);
      pass.push_constant("data_mask", int4(0xff));
    };
    const float back_alpha = native_xray_ ? 0.5f : 1.0f;
    native_pass_init(native_faces_ps_,
                     res.shaders->mesh_edit_face.get(),
                     (state.overlay.edit_flag & V3D_OVERLAY_EDIT_FACES) ? 1.0f : 0.0f,
                     EditMeshDepthOffset::face(false));
    native_pass_init(native_edges_ps_,
                     res.shaders->mesh_edit_edge.get(),
                     back_alpha,
                     EditMeshDepthOffset::edge);
    native_edges_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL |
                                   DRW_STATE_BLEND_ALPHA | DRW_STATE_FIRST_VERTEX_CONVENTION,
                               state.clipping_plane_count);
    native_edges_ps_.push_constant("do_smooth_wire",
                                   (U.gpu_flag & USER_GPU_FLAG_NO_EDIT_MODE_SMOOTH_WIRE) == 0);
    native_edges_ps_.push_constant("use_vertex_selection", false);
    native_pass_init(native_points_ps_,
                     res.shaders->mesh_edit_vert.get(),
                     back_alpha,
                     EditMeshDepthOffset::vertex);
    native_points_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL |
                                    DRW_STATE_BLEND_ALPHA | DRW_STATE_WRITE_DEPTH,
                                state.clipping_plane_count);
    native_pass_init(native_facedots_ps_,
                     res.shaders->mesh_edit_facedot.get(),
                     back_alpha,
                     EditMeshDepthOffset::vertex);
    native_facedots_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL |
                                      DRW_STATE_BLEND_ALPHA | DRW_STATE_WRITE_DEPTH,
                                  state.clipping_plane_count);
    native_depth_ps_.init();
    native_depth_ps_.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                               state.clipping_plane_count);
    native_depth_ps_.shader_set(res.shaders->mesh_edit_depth.get());
    native_depth_ps_.push_constant("retopology_offset", 0.0f);
    native_depth_ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    native_depth_ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);

    {
      auto &pass = edit_curves_ps_;
      pass.init();
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      {
        auto &sub = pass.sub("Lines");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA |
                          DRW_STATE_WRITE_DEPTH,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->curve_edit_line.get());
        sub.bind_texture("weight_tx", &res.weight_ramp_tx);
        sub.push_constant("use_weight", false);
        edit_curves_lines_ = &sub;
      }
    }

    {
      auto &pass = edit_curves_handles_ps_;
      pass.init();
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      DRWState drw_state = DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA | DRW_STATE_WRITE_DEPTH;
      drw_state |= state.xray_flag_enabled ? DRW_STATE_DEPTH_ALWAYS : DRW_STATE_DEPTH_LESS_EQUAL;
      {
        auto &sub = pass.sub("Handles");
        sub.state_set(drw_state, state.clipping_plane_count);
        sub.shader_set(res.shaders->curve_edit_handles.get());
        sub.push_constant("curve_handle_display", int(state.overlay.handle_display));
        edit_curves_handles_ = &sub;
      }
      {
        auto &sub = pass.sub("Points");
        sub.state_set(drw_state, state.clipping_plane_count);
        sub.shader_set(res.shaders->curve_edit_points.get());
        sub.bind_texture("weight_tx", &res.weight_ramp_tx);
        sub.push_constant("use_weight", false);
        sub.push_constant("do_stroke_endpoints", false);
        sub.push_constant("curve_handle_display", int(state.overlay.handle_display));
        edit_curves_points_ = &sub;
      }
    }

    const bool show_normals = (state.overlay.edit_flag & V3D_OVERLAY_EDIT_CU_NORMALS);
    const bool use_hq_normals = (state.scene->r.perf_flag & SCE_PERF_HQ_NORMALS) ||
                                GPU_use_hq_normals_workaround();

    {
      auto &pass = edit_legacy_curve_ps_;
      pass.init();
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      {
        auto &sub = pass.sub("Wires");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_WRITE_DEPTH,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->legacy_curve_edit_wires.get());
        sub.push_constant("normal_size", 0.0f);
        edit_legacy_curve_wires_ = &sub;
      }
      if (show_normals) {
        auto &sub = pass.sub("Normals");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_WRITE_DEPTH,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->legacy_curve_edit_normals.get());
        sub.push_constant("normal_size", state.overlay.normals_length);
        sub.push_constant("use_hq_normals", use_hq_normals);
        edit_legacy_curve_normals_ = &sub;
      }
      else {
        edit_legacy_curve_normals_ = nullptr;
      }
    }

    {
      auto &pass = edit_legacy_curve_handles_ps_;
      pass.init();
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      {
        auto &sub = pass.sub("Handles");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA, state.clipping_plane_count);
        sub.shader_set(res.shaders->legacy_curve_edit_handles.get());
        sub.push_constant("show_curve_handles", state.overlay.handle_display != CURVE_HANDLE_NONE);
        sub.push_constant("curve_handle_display", int(state.overlay.handle_display));
        sub.push_constant("alpha", 1.0f);
        edit_legacy_curve_handles_ = &sub;
      }
      /* Points need to be rendered after handles. */
      {
        auto &sub = pass.sub("Points");
        sub.state_set(DRW_STATE_WRITE_COLOR, state.clipping_plane_count);
        sub.shader_set(res.shaders->legacy_curve_edit_points.get());
        sub.push_constant("show_curve_handles", state.overlay.handle_display != CURVE_HANDLE_NONE);
        sub.push_constant("curve_handle_display", int(state.overlay.handle_display));
        sub.push_constant("do_stroke_endpoints", false);
        edit_legacy_curve_points_ = &sub;
      }
    }

    {
      auto &pass = edit_legacy_surface_handles_ps;
      pass.init();
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);

      auto create_sub = [&](const char *name, DRWState drw_state, float alpha) {
        auto &sub = pass.sub(name);
        sub.state_set(drw_state, state.clipping_plane_count);
        sub.shader_set(res.shaders->legacy_curve_edit_handles.get());
        sub.push_constant("show_curve_handles", state.overlay.handle_display != CURVE_HANDLE_NONE);
        sub.push_constant("curve_handle_display", int(state.overlay.handle_display));
        sub.push_constant("alpha", alpha);
        return &sub;
      };

      const DRWState state_xray = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_GREATER |
                                  DRW_STATE_BLEND_ALPHA;
      edit_legacy_surface_xray_handles_ = create_sub("SurfaceXrayHandles", state_xray, 0.2f);

      const DRWState state_front = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                                   DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA;
      edit_legacy_surface_handles_ = create_sub("SurfaceHandles", state_front, 1.0f);
    }
  }

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources & /*res*/,
                        const State & /*state*/) final
  {
    if (!enabled_) {
      return;
    }

    Object *ob = ob_ref.object;
    blender::Curves &curves = DRW_object_get_data_for_drawing<blender::Curves>(*ob);
    const bool show_points = bke::AttrDomain(curves.selection_domain) == bke::AttrDomain::Point;

    if (show_points) {
      gpu::Batch *geom = DRW_curves_batch_cache_get_edit_points(&curves);
      edit_curves_points_->draw(geom, manager.unique_handle(ob_ref));
    }
    {
      gpu::Batch *geom = DRW_curves_batch_cache_get_edit_curves_handles(&curves);
      edit_curves_handles_->draw_expand(geom, GPU_PRIM_TRIS, 8, 1, manager.unique_handle(ob_ref));
    }
    {
      gpu::Batch *geom = DRW_curves_batch_cache_get_edit_curves_lines(&curves);
      edit_curves_lines_->draw(geom, manager.unique_handle(ob_ref));
    }
  }

  /* Used for legacy curves. */
  void edit_object_sync_legacy(Manager &manager, const ObjectRef &ob_ref, Resources & /*res*/)
  {
    if (!enabled_) {
      return;
    }

    ResourceHandleRange res_handle = manager.unique_handle(ob_ref);

    Object *ob = ob_ref.object;
    blender::Curve &curve = DRW_object_get_data_for_drawing<blender::Curve>(*ob);

    const auto *source = DRW_curve_batch_cache_get_edit_source(&curve);
    if (source || curve.nurbs_eval_source) {
      if (!source) {
        return;
      }
      const auto mode = source->selection().mode;
      const bool vertex_mode = flag_is_set(mode, bke::nurbs::SelectionMode::Vertex);
      const bool edge_mode = flag_is_set(mode, bke::nurbs::SelectionMode::Edge);
      const bool face_mode = flag_is_set(mode, bke::nurbs::SelectionMode::Face);
      auto &edges = native_edges_ps_.sub("Source Edges");
      edges.push_constant("select_face", face_mode);
      edges.push_constant("select_edge", edge_mode);
      edges.push_constant("use_vertex_selection", vertex_mode);
      edges.draw_expand(DRW_cache_curve_edge_overlay_get(ob), GPU_PRIM_TRIS, 2, 1, res_handle);
      auto &faces = native_faces_ps_.sub("Source Faces");
      faces.push_constant("select_face", face_mode);
      faces.push_constant("select_edge", edge_mode);
      /* Tagged compatibility surfaces still use the legacy body tessellator. Their source
       * overlay is a separate edit cage, like a mesh cage distinct from its evaluated surface.
       * Native body and overlay share a tessellation and use the regular mesh face pass bias. */
      faces.push_constant("ndc_offset",
                          EditMeshDepthOffset::face(curve.nurbs_eval_source == nullptr));
      gpu::Batch *triangles = DRW_curve_batch_cache_get_edit_faces(&curve);
      faces.draw(triangles, res_handle);
      if (native_needs_depth_ && ob->dt > OB_WIRE && !native_xray_enabled_) {
        native_depth_ps_.draw(triangles, res_handle);
      }
      if (vertex_mode) {
        native_points_ps_.draw(DRW_cache_curve_vert_overlay_get(ob), res_handle);
      }
      if (face_mode && native_show_facedots_) {
        native_facedots_ps_.draw(DRW_curve_batch_cache_get_edit_facedots(&curve), res_handle);
      }
      return;
    }
    if (ob->type == OB_CURVES_LEGACY) {
      gpu::Batch *geom = DRW_cache_curve_edge_wire_get(ob);
      edit_legacy_curve_wires_->draw(geom, res_handle);
    }
    if (edit_legacy_curve_normals_ && (curve.flag & CU_3D)) {
      gpu::Batch *geom = DRW_cache_curve_edge_normal_get(ob);
      edit_legacy_curve_normals_->draw_expand(geom, GPU_PRIM_LINES, 2, 1, res_handle);
    }
    {
      gpu::Batch *geom = DRW_cache_curve_edge_overlay_get(ob);
      if (ob->type == OB_CURVES_LEGACY) {
        edit_legacy_curve_handles_->draw_expand(geom, GPU_PRIM_TRIS, 8, 1, res_handle);
      }
      else {
        edit_legacy_surface_xray_handles_->draw_expand(geom, GPU_PRIM_TRIS, 8, 1, res_handle);
        edit_legacy_surface_handles_->draw_expand(geom, GPU_PRIM_TRIS, 8, 1, res_handle);
      }
    }
    {
      gpu::Batch *geom = DRW_cache_curve_vert_overlay_get(ob);
      edit_legacy_curve_points_->draw(geom, res_handle);
    }
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    view_edit_cage.sync(view.viewmat(), offset_data_.winmat_polygon_offset(view.winmat(), 0.5f));

    GPU_framebuffer_bind(framebuffer);
    manager.submit(edit_legacy_curve_ps_, view);
    manager.submit(edit_curves_ps_, view);
    manager.submit(edit_curves_handles_ps_, view_edit_cage);
    manager.submit(edit_legacy_surface_handles_ps, view);
    manager.submit(native_depth_ps_, view);
    if (!native_xray_enabled_) {
      manager.submit(native_faces_ps_, view);
    }
    if (!native_xray_) {
      manager.submit(native_edges_ps_, view);
      manager.submit(native_points_ps_, view);
      manager.submit(native_facedots_ps_, view);
    }
  }

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    view_edit_cage.sync(view.viewmat(), offset_data_.winmat_polygon_offset(view.winmat(), 0.5f));

    GPU_framebuffer_bind(framebuffer);
    manager.submit(edit_legacy_curve_handles_ps_, view);
    if (native_xray_enabled_) {
      manager.submit(native_faces_ps_, view);
    }
    if (native_xray_) {
      manager.submit(native_edges_ps_, view);
      manager.submit(native_points_ps_, view);
      manager.submit(native_facedots_ps_, view);
    }
  }
};

}  // namespace blender::draw::overlay
