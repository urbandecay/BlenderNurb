# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import (
    Header,
    Menu,
    Panel,
    SurfaceCurve
)
from bl_ui.properties_paint_common import (
    UnifiedPaintPanel,
    brush_basic_texpaint_settings,
    BrushAssetShelf,
)
from bl_ui.space_toolsystem_common import (
    ToolActivePanelHelper,
)
from bpy.app.translations import (
    pgettext_iface as iface_,
    pgettext_rpt as rpt_,
    contexts as i18n_contexts,
)


def _toggle_xray_operator(layout, context, text=None):
    view = context.space_data
    if view.shading.type == 'WIREFRAME':
        draw_depressed = view.shading.show_xray_wireframe
    else:
        draw_depressed = view.shading.show_xray
    layout.operator(
        "view3d.toggle_xray",
        text=text,
        icon='XRAY',
        depress=draw_depressed,
    )


class VIEW3D_HT_tool_header(Header):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'TOOL_HEADER'

    def draw(self, context):
        layout = self.layout

        self.draw_tool_settings(context)

        layout.separator_spacer()

        self.draw_mode_settings(context)

    def draw_tool_settings(self, context):
        layout = self.layout
        tool_mode = context.mode

        # Active Tool
        # -----------
        from bl_ui.space_toolsystem_common import ToolSelectPanelHelper
        tool = ToolSelectPanelHelper.draw_active_tool_header(
            context, layout,
            tool_key=('VIEW_3D', tool_mode),
        )
        # Object Mode Options
        # -------------------

        # Example of how tool_settings can be accessed as pop-overs.

        # TODO(campbell): editing options should be after active tool options
        # (obviously separated for from the users POV)
        draw_fn = getattr(_draw_tool_settings_context_mode, tool_mode, None)
        if draw_fn is not None:
            is_valid_context = draw_fn(context, layout, tool)

        def draw_3d_brush_settings(layout, tool_mode):
            layout.popover("VIEW3D_PT_tools_brush_settings_advanced", text="Brush")
            layout.popover("VIEW3D_PT_tools_brush_texture")
            layout.popover("VIEW3D_PT_tools_brush_stroke")
            layout.popover("VIEW3D_PT_tools_brush_falloff")
            layout.popover("VIEW3D_PT_tools_brush_display")

        # NOTE: general mode options should be added to `draw_mode_settings`.
        if tool_mode == 'SCULPT':
            if is_valid_context:
                draw_3d_brush_settings(layout, tool_mode)
        elif tool_mode == 'PAINT_VERTEX':
            if is_valid_context:
                draw_3d_brush_settings(layout, tool_mode)
        elif tool_mode == 'EDIT_CURVE':
            pass
        elif tool_mode == 'EDIT_MESH':
            pass
        elif tool_mode == 'PARTICLE':
            # Disable, only shows "Brush" panel, which is already in the top-bar.
            # if tool.use_brushes:
            #     layout.popover_group(context=".paint_common", **popover_kw)
            pass

    def draw_mode_settings(self, context):
        layout = self.layout
        mode_string = context.mode
        tool_settings = context.tool_settings

        def row_for_mirror():
            row = layout.row(align=True)
            row.label(icon='MOD_MIRROR')
            sub = row.row(align=True)
            sub.scale_x = 0.6
            return row, sub

        if mode_string in {'EDIT_MESH', 'SCULPT', 'PAINT_VERTEX'}:
            # Mesh Modes, Use Mesh Symmetry
            ob = context.object
            row, sub = row_for_mirror()
            sub.prop(ob, "use_mesh_mirror_x", text="X", toggle=True)
            sub.prop(ob, "use_mesh_mirror_y", text="Y", toggle=True)
            sub.prop(ob, "use_mesh_mirror_z", text="Z", toggle=True)
            if mode_string == 'EDIT_MESH':
                layout.prop(tool_settings, "use_mesh_automerge", text="")
            elif mode_string == 'SCULPT':
                row.popover(panel="VIEW3D_PT_sculpt_symmetry_for_topbar", text="")
            elif mode_string == 'PAINT_VERTEX':
                row.popover(panel="VIEW3D_PT_tools_vertexpaint_symmetry_for_topbar", text="")
        elif mode_string == 'SCULPT_CURVES':
            ob = context.object
            _row, sub = row_for_mirror()
            sub.prop(ob.data, "use_mirror_x", text="X", toggle=True)
            sub.prop(ob.data, "use_mirror_y", text="Y", toggle=True)
            sub.prop(ob.data, "use_mirror_z", text="Z", toggle=True)

            row = layout.row(align=True)
            row.prop(ob.data, "use_sculpt_collision", icon='MOD_PHYSICS', icon_only=True, toggle=True)
            sub = row.row(align=True)
            sub.active = ob.data.use_sculpt_collision
            sub.prop(ob.data, "surface_collision_distance")

        # Expand panels from the side-bar as popovers.
        popover_kw = {"space_type": 'VIEW_3D', "region_type": 'UI', "category": "Tool"}

        if mode_string == 'SCULPT':
            layout.popover_group(context=".sculpt_mode", **popover_kw)
        elif mode_string == 'PAINT_VERTEX':
            layout.popover_group(context=".vertexpaint", **popover_kw)
        elif mode_string == 'EDIT_TEXT':
            layout.popover_group(context=".text_edit", **popover_kw)
        elif mode_string == 'EDIT_LATTICE':
            layout.popover_group(context=".lattice_edit", **popover_kw)
        elif mode_string == 'EDIT_CURVE':
            layout.popover_group(context=".curve_edit", **popover_kw)
        elif mode_string == 'EDIT_MESH':
            layout.popover_group(context=".mesh_edit", **popover_kw)
        elif mode_string == 'PARTICLE':
            layout.popover_group(context=".particlemode", **popover_kw)
        elif mode_string == 'OBJECT':
            layout.popover_group(context=".objectmode", **popover_kw)


class _draw_tool_settings_context_mode:
    @staticmethod
    def SCULPT(context, layout, tool):
        if (tool is None) or (not tool.use_brushes):
            return False

        paint = context.tool_settings.sculpt
        brush = paint.brush

        BrushAssetShelf.draw_popup_selector(layout, context, brush)

        if brush is None:
            return False

        capabilities = brush.sculpt_capabilities

        ups = paint.unified_paint_settings

        if capabilities.has_color:
            row = layout.row(align=True)
            row.ui_units_x = 4
            UnifiedPaintPanel.prop_unified_color(row, context, brush, "color", text="")
            UnifiedPaintPanel.prop_unified_color(row, context, brush, "secondary_color", text="")
            row.separator()
            layout.prop(brush, "blend", text="", expand=False)

        size = "size"
        size_owner = ups if ups.use_unified_size else brush
        if size_owner.use_locked_size == 'SCENE':
            size = "unprojected_size"

        UnifiedPaintPanel.prop_unified(
            layout,
            context,
            brush,
            size,
            pressure_name="use_pressure_size",
            unified_name="use_unified_size",
            text="Size",
            slider=True,
            header=True,
        )

        # strength, use_strength_pressure
        pressure_name = "use_pressure_strength" if capabilities.has_strength_pressure else None
        UnifiedPaintPanel.prop_unified(
            layout,
            context,
            brush,
            "strength",
            pressure_name=pressure_name,
            unified_name="use_unified_strength",
            text="Strength",
            header=True,
        )

        # direction
        if capabilities.has_direction:
            layout.row().prop(brush, "direction", expand=True, text="")

        return True

    @staticmethod
    def PAINT_VERTEX(context, layout, tool):
        if (tool is None) or (not tool.use_brushes):
            return False

        paint = context.tool_settings.vertex_paint
        brush = paint.brush

        BrushAssetShelf.draw_popup_selector(layout, context, brush)

        if brush is None:
            return False

        brush_basic_texpaint_settings(layout, context, brush, compact=True)

        return True

    @staticmethod
    def PARTICLE(context, layout, tool):
        if (tool is None) or (not tool.use_brushes):
            return False

        # See: `VIEW3D_PT_tools_brush`, basically a duplicate
        tool_settings = context.tool_settings
        settings = tool_settings.particle_edit
        brush = settings.brush
        tool = settings.tool
        if tool == 'NONE':
            return False

        layout.prop(brush, "size", slider=True)
        if tool == 'ADD':
            layout.prop(brush, "count")

            layout.prop(settings, "use_default_interpolate")
            layout.prop(brush, "steps", slider=True)
            layout.prop(settings, "default_key_count", slider=True)
        else:
            layout.prop(brush, "strength", slider=True)

            if tool == 'LENGTH':
                layout.row().prop(brush, "length_mode", expand=True)
            elif tool == 'PUFF':
                layout.row().prop(brush, "puff_mode", expand=True)
                layout.prop(brush, "use_puff_volume")
            elif tool == 'COMB':
                row = layout.row()
                row.active = settings.is_editable
                row.prop(settings, "use_emitter_deflect", text="Deflect Emitter")
                sub = row.row(align=True)
                sub.active = settings.use_emitter_deflect
                sub.prop(settings, "emitter_distance", text="Distance")

        return True

    @staticmethod
    def SCULPT_CURVES(context, layout, tool):
        if (tool is None) or (not tool.use_brushes):
            return False

        tool_settings = context.tool_settings
        paint = tool_settings.curves_sculpt
        brush = paint.brush

        BrushAssetShelf.draw_popup_selector(layout, context, brush)

        if brush is None:
            return False

        UnifiedPaintPanel.prop_unified(
            layout,
            context,
            brush,
            "size",
            unified_name="use_unified_size",
            pressure_name="use_pressure_size",
            text="Size",
            slider=True,
            header=True,
        )

        if brush.curves_sculpt_brush_type not in {'ADD', 'DELETE'}:
            use_strength_pressure = brush.curves_sculpt_brush_type not in {'SLIDE'}
            UnifiedPaintPanel.prop_unified(
                layout,
                context,
                brush,
                "strength",
                unified_name="use_unified_strength",
                pressure_name="use_pressure_strength" if use_strength_pressure else None,
                header=True,
            )

        curves_tool = brush.curves_sculpt_brush_type

        if curves_tool == 'COMB':
            layout.prop(brush, "falloff_shape", expand=True)
            layout.popover("VIEW3D_PT_tools_brush_falloff", text="Brush Falloff")
            layout.popover("VIEW3D_PT_curves_sculpt_parameter_falloff", text="Curve Falloff")
        elif curves_tool == 'ADD':
            layout.prop(brush, "falloff_shape", expand=True)
            layout.prop(brush.curves_sculpt_settings, "add_amount")
            layout.popover("VIEW3D_PT_curves_sculpt_add_shape", text="Curve Shape")
            layout.prop(brush, "use_frontface", text="Front Faces Only")
        elif curves_tool == 'GROW_SHRINK':
            layout.prop(brush, "direction", expand=True, text="")
            layout.prop(brush, "falloff_shape", expand=True)
            layout.popover("VIEW3D_PT_curves_sculpt_grow_shrink_scaling", text="Scaling")
            layout.popover("VIEW3D_PT_tools_brush_falloff")
        elif curves_tool == 'SNAKE_HOOK':
            layout.prop(brush, "falloff_shape", expand=True)
            layout.popover("VIEW3D_PT_tools_brush_falloff")
        elif curves_tool == 'DELETE':
            layout.prop(brush, "falloff_shape", expand=True)
        elif curves_tool == 'SELECTION_PAINT':
            layout.prop(brush, "direction", expand=True, text="")
            layout.prop(brush, "falloff_shape", expand=True)
            layout.popover("VIEW3D_PT_tools_brush_falloff")
        elif curves_tool == 'PINCH':
            layout.prop(brush, "direction", expand=True, text="")
            layout.prop(brush, "falloff_shape", expand=True)
            layout.popover("VIEW3D_PT_tools_brush_falloff")
        elif curves_tool == 'SMOOTH':
            layout.prop(brush, "falloff_shape", expand=True)
            layout.popover("VIEW3D_PT_tools_brush_falloff")
        elif curves_tool == 'PUFF':
            layout.prop(brush, "falloff_shape", expand=True)
            layout.popover("VIEW3D_PT_tools_brush_falloff")
        elif curves_tool == 'DENSITY':
            layout.prop(brush, "falloff_shape", expand=True)
            row = layout.row(align=True)
            row.prop(brush.curves_sculpt_settings, "density_mode", text="", expand=True)
            row = layout.row(align=True)
            row.prop(brush.curves_sculpt_settings, "minimum_distance", text="Distance Min")
            row.operator_context = 'INVOKE_REGION_WIN'
            row.operator("sculpt_curves.min_distance_edit", text="", icon='DRIVER_DISTANCE')
            row = layout.row(align=True)
            row.enabled = brush.curves_sculpt_settings.density_mode != 'REMOVE'
            row.prop(brush.curves_sculpt_settings, "density_add_attempts", text="Count Max")
            layout.popover("VIEW3D_PT_tools_brush_falloff")
            layout.popover("VIEW3D_PT_curves_sculpt_add_shape", text="Curve Shape")
        elif curves_tool == 'SLIDE':
            layout.popover("VIEW3D_PT_tools_brush_falloff")

        return True



class VIEW3D_HT_header(Header):
    bl_space_type = 'VIEW_3D'

    @staticmethod
    def draw_xform_template(layout, context):
        obj = context.active_object
        object_mode = 'OBJECT' if obj is None else obj.mode

        tool_settings = context.tool_settings

        # Mode & Transform Settings
        scene = context.scene

        # Orientation
        if object_mode in {'OBJECT', 'EDIT'}:
            orient_slot = scene.transform_orientation_slots[0]
            row = layout.row(align=True)

            sub = row.row()
            sub.prop_with_popover(
                orient_slot,
                "type",
                text="",
                panel="VIEW3D_PT_transform_orientations",
            )

        # Pivot
        if object_mode in {'OBJECT', 'EDIT'}:
            layout.prop(tool_settings, "transform_pivot_point", text="", icon_only=True)

        # Snap
        show_snap = False
        if obj is None:
            show_snap = True
        else:
            if object_mode not in {
                    'SCULPT', 'SCULPT_CURVES',
                    'VERTEX_PAINT',
            }:
                show_snap = True
            else:

                paint_settings = UnifiedPaintPanel.paint_settings(context)

                if paint_settings:
                    brush = paint_settings.brush
                    if brush and hasattr(brush, "stroke_method") and brush.stroke_method == 'CURVE':
                        show_snap = True

        if show_snap:
            snap_items = bpy.types.ToolSettings.bl_rna.properties["snap_elements"].enum_items
            snap_elements = tool_settings.snap_elements
            if len(snap_elements) == 1:
                text = ""
                for elem in snap_elements:
                    icon = snap_items[elem].icon
                    break
            else:
                text = iface_("Mix", i18n_contexts.editor_view3d)
                icon = 'NONE'
            del snap_items, snap_elements

            row = layout.row(align=True)
            row.prop(tool_settings, "use_snap", text="")

            sub = row.row(align=True)
            sub.popover(
                panel="VIEW3D_PT_snapping",
                icon=icon,
                text=text,
                translate=False,
            )

        # Proportional editing
        if object_mode in {
            'EDIT',
            'PARTICLE_EDIT',
            'OBJECT',
        }:
            row = layout.row(align=True)
            kw = {}
            if object_mode == 'OBJECT':
                attr = "use_proportional_edit_objects"
            else:
                attr = "use_proportional_edit"

                if tool_settings.use_proportional_edit:
                    if tool_settings.use_proportional_connected:
                        kw["icon"] = 'PROP_CON'
                    elif tool_settings.use_proportional_projected:
                        kw["icon"] = 'PROP_PROJECTED'
                    else:
                        kw["icon"] = 'PROP_ON'
                else:
                    kw["icon"] = 'PROP_OFF'

            row.prop(tool_settings, attr, icon_only=True, **kw)
            sub = row.row(align=True)
            sub.active = getattr(tool_settings, attr)
            sub.prop_with_popover(
                tool_settings,
                "proportional_edit_falloff",
                text="",
                icon_only=True,
                panel="VIEW3D_PT_proportional_edit",
            )


    def draw(self, context):
        layout = self.layout

        tool_settings = context.tool_settings
        view = context.space_data
        shading = view.shading

        layout.row(align=True).template_header()

        row = layout.row(align=True)
        obj = context.active_object
        mode_string = context.mode
        object_mode = 'OBJECT' if obj is None else obj.mode
        # Note: This is actually deadly in case enum_items have to be dynamically generated
        #       (because internal RNA array iterator will free everything immediately...).
        # XXX This is an RNA internal issue, not sure how to fix it.
        # Note: Tried to add an accessor to get translated UI strings instead of manual call
        #       to pgettext_iface below, but this fails because translated enum-items
        #       are always dynamically allocated.
        act_mode_item = bpy.types.Object.bl_rna.properties["mode"].enum_items[object_mode]
        act_mode_i18n_context = bpy.types.Object.bl_rna.properties["mode"].translation_context

        sub = row.row(align=True)
        sub.operator_menu_enum(
            "object.mode_set", "mode",
            text=iface_(act_mode_item.name, act_mode_i18n_context),
            icon=act_mode_item.icon,
        )
        del act_mode_item

        layout.template_header_3D_mode()

        # Contains buttons like Mode, Pivot, Layer, Mesh Select Mode...
        if obj:
            if (object_mode == 'EDIT' and obj.type in {'CURVE', 'SURFACE'} and
                    (obj.data.is_native_nurbs or obj.data.has_tagged_nurbs)):
                modes = obj.data.nurbs_selection_mode
                sub = row.row(align=True)
                sub.operator_context = 'INVOKE_REGION_WIN'
                for index, mode, icon in ((0, 'VERT', 'VERTEXSEL'), (1, 'EDGE', 'EDGESEL'), (2, 'FACE', 'FACESEL')):
                    sub.operator("curve.select_mode", text="", icon=icon, depress=modes[index]).type = mode
            # Particle edit
            if object_mode == 'PARTICLE_EDIT':
                row = layout.row()
                row.prop(tool_settings.particle_edit, "select_mode", text="", expand=True)
            elif object_mode in {'EDIT', 'SCULPT_CURVES'} and obj.type == 'CURVES':
                curves = obj.data

                row = layout.row(align=True)
                domain = curves.selection_domain
                row.operator(
                    "curves.set_selection_domain",
                    text="",
                    icon='CURVE_BEZCIRCLE',
                    depress=(domain == 'POINT'),
                ).domain = 'POINT'
                row.operator(
                    "curves.set_selection_domain",
                    text="",
                    icon='CURVE_PATH',
                    depress=(domain == 'CURVE'),
                ).domain = 'CURVE'

        overlay = view.overlay

        VIEW3D_MT_editor_menus.draw_collapsible(context, layout)

        layout.separator_spacer()

        if object_mode == 'SCULPT':
            # If the active tool supports it, show the canvas selector popover.
            from bl_ui.space_toolsystem_common import ToolSelectPanelHelper
            tool = ToolSelectPanelHelper.tool_active_from_context(context)

            is_paint_tool = False
            if tool.use_brushes:
                paint = tool_settings.sculpt
                brush = paint.brush
                if brush:
                    is_paint_tool = brush.sculpt_brush_type in {'PAINT', 'SMEAR'}
            else:
                is_paint_tool = tool and tool.use_paint_canvas

            shading = VIEW3D_PT_shading.get_shading(context)
            color_type = shading.color_type

            row = layout.row()
            row.active = is_paint_tool and color_type == 'VERTEX'

            row.popover(panel="VIEW3D_PT_slots_color_attributes", icon='GROUP_VCOL')

            layout.popover(
                panel="VIEW3D_PT_sculpt_snapping",
                icon='SNAP_INCREMENT',
                text="",
                translate=False,
            )

            layout.popover(
                panel="VIEW3D_PT_sculpt_automasking",
                text="",
                icon=VIEW3D_HT_header._mesh_paint_automasking_icon(tool_settings.sculpt),
            )

        elif object_mode == 'VERTEX_PAINT':
            row = layout.row()
            row.popover(panel="VIEW3D_PT_slots_color_attributes", icon='GROUP_VCOL')
        else:
            # Transform settings depending on tool header visibility
            VIEW3D_HT_header.draw_xform_template(layout, context)

        layout.separator_spacer()

        # Viewport Settings
        layout.popover(
            panel="VIEW3D_PT_object_type_visibility",
            icon_value=view.icon_from_show_object_viewport,
            text="",
        )

        # Gizmo toggle & popover.
        row = layout.row(align=True)
        # FIXME: place-holder icon.
        row.prop(view, "show_gizmo", text="", toggle=True, icon='GIZMO')
        sub = row.row(align=True)
        sub.active = view.show_gizmo
        sub.popover(
            panel="VIEW3D_PT_gizmo_display",
            text="",
        )

        # Overlay toggle & popover.
        row = layout.row(align=True)
        row.prop(overlay, "show_overlays", icon='OVERLAY', text="")
        sub = row.row(align=True)
        sub.active = overlay.show_overlays
        sub.popover(panel="VIEW3D_PT_overlay", text="")

        if mode_string == 'EDIT_MESH':
            sub.popover(panel="VIEW3D_PT_overlay_edit_mesh", text="", icon='EDITMODE_HLT')
        if mode_string == 'EDIT_CURVE':
            sub.popover(panel="VIEW3D_PT_overlay_edit_curve", text="", icon='EDITMODE_HLT')
        elif mode_string == 'EDIT_CURVES':
            sub.popover(panel="VIEW3D_PT_overlay_edit_curves", text="", icon='EDITMODE_HLT')
        elif mode_string == 'SCULPT':
            sub.popover(panel="VIEW3D_PT_overlay_sculpt", text="", icon='SCULPTMODE_HLT')
        elif mode_string == 'SCULPT_CURVES':
            sub.popover(panel="VIEW3D_PT_overlay_sculpt_curves", text="", icon='SCULPTMODE_HLT')
        elif mode_string == 'PAINT_VERTEX':
            sub.popover(panel="VIEW3D_PT_overlay_vertex_paint", text="", icon='VPAINT_HLT')

        row = layout.row()
        row.active = (object_mode == 'EDIT') or (shading.type in {'WIREFRAME', 'SOLID'})

        _toggle_xray_operator(row, context, text="")

        row = layout.row(align=True)
        row.prop(shading, "type", text="", expand=True)
        sub = row.row(align=True)
        # TODO, currently render shading type ignores mesh two-side, until it's supported
        # show the shading popover which shows double-sided option.

        # sub.enabled = shading.type != 'RENDERED'
        sub.popover(panel="VIEW3D_PT_shading", text="")

    @staticmethod
    def _mesh_paint_automasking_icon(paint):
        automask_enabled = (
            paint.mesh_automasking_settings.use_automasking_topology or
            paint.mesh_automasking_settings.use_automasking_face_sets or
            paint.mesh_automasking_settings.use_automasking_boundary_edges or
            paint.mesh_automasking_settings.use_automasking_boundary_face_sets or
            paint.mesh_automasking_settings.use_automasking_cavity or
            paint.mesh_automasking_settings.use_automasking_cavity_inverted or
            paint.mesh_automasking_settings.use_automasking_start_normal or
            paint.mesh_automasking_settings.use_automasking_view_normal
        )

        return 'CLIPUV_DEHLT' if automask_enabled else 'CLIPUV_HLT'


class VIEW3D_MT_editor_menus(Menu):
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        obj = context.active_object
        mode_string = context.mode
        edit_object = context.edit_object
        tool_settings = context.tool_settings
        is_native_nurbs = (edit_object is not None and
                           edit_object.type in {'CURVE', 'SURFACE'} and
                           getattr(edit_object.data, "is_native_nurbs", False))

        layout.menu("VIEW3D_MT_view")

        # Select Menu
        if is_native_nurbs:
            layout.menu("VIEW3D_MT_select_edit_nurbs")
        elif mode_string == 'PAINT_VERTEX':
            mesh = obj.data
            if mesh.use_paint_mask:
                layout.menu("VIEW3D_MT_select_paint_mask")
            elif mesh.use_paint_mask_vertex:
                layout.menu("VIEW3D_MT_select_paint_mask_vertex")
        elif mode_string not in {'SCULPT', 'SCULPT_CURVES'}:
            layout.menu("VIEW3D_MT_select_" + mode_string.lower())

        if mode_string == 'OBJECT':
            layout.menu("VIEW3D_MT_add")
        elif mode_string == 'EDIT_MESH':
            layout.menu("VIEW3D_MT_mesh_add", text="Add", text_ctxt=i18n_contexts.operator_default)
        elif mode_string == 'EDIT_CURVE' and not is_native_nurbs:
            layout.menu("VIEW3D_MT_curve_add", text="Add", text_ctxt=i18n_contexts.operator_default)
        elif mode_string == 'EDIT_CURVES':
            layout.menu("VIEW3D_MT_edit_curves_add", text="Add", text_ctxt=i18n_contexts.operator_default)
        elif mode_string == 'EDIT_SURFACE' and not is_native_nurbs:
            layout.menu("VIEW3D_MT_surface_add", text="Add", text_ctxt=i18n_contexts.operator_default)

        if edit_object:
            if is_native_nurbs or (
                edit_object.type in {'CURVE', 'SURFACE'} and
                getattr(edit_object.data, "has_tagged_nurbs", False)
            ):
                layout.menu("VIEW3D_MT_edit_nurbs")
            else:
                layout.menu("VIEW3D_MT_edit_" + edit_object.type.lower())

            if mode_string == 'EDIT_MESH':
                layout.menu("VIEW3D_MT_edit_mesh_vertices")
                layout.menu("VIEW3D_MT_edit_mesh_edges")
                layout.menu("VIEW3D_MT_edit_mesh_faces")
                layout.menu("VIEW3D_MT_uv_map", text="UV")
                layout.template_node_operator_asset_root_items()
            elif mode_string in {'EDIT_CURVE', 'EDIT_SURFACE'} and not is_native_nurbs:
                layout.menu("VIEW3D_MT_edit_curve_ctrlpoints")
                layout.menu("VIEW3D_MT_edit_curve_segments")
            elif mode_string == 'EDIT_POINTCLOUD':
                layout.template_node_operator_asset_root_items()
            elif mode_string == 'EDIT_CURVES':
                layout.menu("VIEW3D_MT_edit_curves_control_points")
                layout.menu("VIEW3D_MT_edit_curves_segments")
                layout.template_node_operator_asset_root_items()

        elif obj:
            if mode_string != 'SCULPT_CURVES':
                layout.menu("VIEW3D_MT_" + mode_string.lower())
            if mode_string == 'SCULPT':
                layout.menu("VIEW3D_MT_mask")
                layout.menu("VIEW3D_MT_face_sets")
                layout.template_node_operator_asset_root_items()
            elif mode_string == 'SCULPT_CURVES':
                layout.menu("VIEW3D_MT_select_sculpt_curves")
                layout.menu("VIEW3D_MT_sculpt_curves")
                layout.template_node_operator_asset_root_items()
            else:
                layout.template_node_operator_asset_root_items()

        else:
            layout.menu("VIEW3D_MT_object")
            layout.template_node_operator_asset_root_items()


# ********** Menu **********


class VIEW3D_MT_select_edit_nurbs(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout
        layout.operator_enum("curve.select_mode", "type")
        layout.separator()
        layout.operator("curve.select_all", text="All", icon='SELECT_SET').action = 'SELECT'
        layout.operator("curve.select_all", text="None", icon='SELECT_SUBTRACT').action = 'DESELECT'
        layout.operator("curve.select_all", text="Invert", icon='SELECT_EXTEND').action = 'INVERT'
        layout.separator()
        layout.operator("view3d.select_box", text="Box Select")
        layout.separator()
        layout.operator("curve.select_linked", text="Linked")


# ********** Utilities **********


class ShowHideMenu:
    bl_label = "Show/Hide"
    _operator_name = ""

    def draw(self, _context):
        layout = self.layout

        layout.operator("{:s}.reveal".format(self._operator_name))
        layout.operator("{:s}.hide".format(self._operator_name), text="Hide Selected").unselected = False
        layout.operator("{:s}.hide".format(self._operator_name), text="Hide Unselected").unselected = True


# Standard transforms which apply to all cases (mix-in class, not used directly).
class VIEW3D_MT_transform_base:
    bl_label = "Transform"
    bl_category = "View"

    # TODO: get rid of the custom text strings?
    def draw(self, context):
        layout = self.layout

        layout.operator("transform.translate")
        layout.operator("transform.rotate")
        layout.operator("transform.resize", text="Scale")

        layout.separator()

        layout.operator("transform.tosphere", text="To Sphere")
        layout.operator("transform.shear", text="Shear")
        layout.operator("transform.bend", text="Bend")
        layout.operator("transform.push_pull", text="Push/Pull")


# Generic transform menu - geometry types
class VIEW3D_MT_transform(VIEW3D_MT_transform_base, Menu):
    def draw(self, context):
        # base menu
        VIEW3D_MT_transform_base.draw(self, context)

        # generic...
        layout = self.layout
        layout.separator()
        if context.mode == 'EDIT_MESH':
            layout.operator("mesh.circularize", text="To Circle")
            layout.operator("mesh.flatten", text="Flatten")
            layout.operator("transform.shrink_fatten", text="Shrink/Fatten")
            layout.operator("mesh.space_edge_loops_evenly", text="Space Edge Loops Evenly")
            layout.operator("transform.skin_resize")
        elif context.mode in {'EDIT_CURVE', 'EDIT_CURVES', 'EDIT_POINTCLOUD'}:
            layout.operator("transform.transform", text="Radius").mode = 'CURVE_SHRINKFATTEN'

        if context.mode in {
            'EDIT_MESH',
            'EDIT_SURFACE',
            'EDIT_CURVE',
            'EDIT_CURVES',
            'EDIT_LATTICE',
            'EDIT_POINTCLOUD',
        }:
            layout.operator("transform.vertex_warp", text="Warp")
            layout.operator_context = 'EXEC_REGION_WIN'
            layout.operator("transform.vertex_random", text="Randomize").offset = 0.1
            layout.operator_context = 'INVOKE_REGION_WIN'


# Object-specific extensions to Transform menu
class VIEW3D_MT_transform_object(VIEW3D_MT_transform_base, Menu):
    def draw(self, context):
        layout = self.layout

        # base menu
        VIEW3D_MT_transform_base.draw(self, context)

        # object-specific option follow...
        layout.separator()

        layout.operator("transform.translate", text="Move Texture Space").texture_space = True
        layout.operator("transform.resize", text="Scale Texture Space").texture_space = True

        layout.separator()

        layout.operator_context = 'EXEC_REGION_WIN'
        # XXX: see `alignmenu()` in `edit.c` of b2.4x to get this working.
        layout.operator("transform.transform", text="Align to Transform Orientation").mode = 'ALIGN'

        layout.separator()

        layout.operator("object.randomize_transform")
        layout.operator("object.align")

        # TODO: there is a strange context bug here.
        """
        layout.operator_context = 'INVOKE_REGION_WIN'
        layout.operator("object.transform_axis_target")
        """


class VIEW3D_MT_mirror(Menu):
    bl_label = "Mirror"
    bl_translation_context = i18n_contexts.operator_default

    def draw(self, _context):
        layout = self.layout

        layout.operator("transform.mirror", text="Interactive Mirror")

        layout.separator()

        layout.operator_context = 'EXEC_REGION_WIN'

        for (space_name, space_id) in (("Global", 'GLOBAL'), ("Local", 'LOCAL')):
            for axis_index, axis_name in enumerate("XYZ"):
                props = layout.operator(
                    "transform.mirror",
                    text="{:s} {:s}".format(axis_name, iface_(space_name)),
                    translate=False,
                )
                props.constraint_axis[axis_index] = True
                props.orient_type = space_id

            if space_id == 'GLOBAL':
                layout.separator()


class VIEW3D_MT_snap(Menu):
    bl_label = "Snap"

    def draw(self, _context):
        layout = self.layout

        layout.operator("view3d.snap_selected_to_grid", text="Selection to Grid")
        layout.operator("view3d.snap_selected_to_cursor", text="Selection to Cursor").use_offset = False
        layout.operator("view3d.snap_selected_to_cursor", text="Selection to Cursor (Keep Offset)").use_offset = True
        layout.operator("view3d.snap_selected_to_active", text="Selection to Active")

        layout.separator()

        layout.operator("view3d.snap_cursor_to_selected", text="Cursor to Selected")
        layout.operator("view3d.snap_cursor_to_center", text="Cursor to World Origin")
        layout.operator("view3d.snap_cursor_to_grid", text="Cursor to Grid")
        layout.operator("view3d.snap_cursor_to_active", text="Cursor to Active")


class VIEW3D_MT_uv_map(Menu):
    bl_label = "UV Mapping"

    def draw(self, _context):
        layout = self.layout

        layout.menu_contents("IMAGE_MT_uvs_unwrap")

        layout.separator()

        layout.operator_context = 'INVOKE_REGION_WIN'
        layout.operator("uv.project_from_view").scale_to_bounds = False
        layout.operator("uv.project_from_view", text="Project from View (Bounds)").scale_to_bounds = True

        layout.separator()

        layout.operator("mesh.mark_seam", icon='EDGE_SEAM').clear = False
        layout.operator("mesh.mark_seam", text="Clear Seam").clear = True

        layout.separator()

        layout.operator("uv.reset")

        layout.template_node_operator_asset_menu_items(catalog_path="UV")


# ********** View menus **********


class VIEW3D_MT_view(Menu):
    bl_label = "View"

    def draw(self, context):
        layout = self.layout
        view = context.space_data

        layout.prop(view, "show_region_toolbar")
        layout.prop(view, "show_region_ui")
        layout.prop(view, "show_region_tool_header")
        layout.prop(view, "show_region_asset_shelf")
        layout.prop(view, "show_region_hud")

        layout.separator()

        if context.mode in {'PAINT_VERTEX', 'SCULPT'}:
            layout.operator("view3d.view_selected", text="Center Last Stroke")
        else:
            layout.operator("view3d.view_selected", text="Frame Selected")
        if view.region_quadviews:
            layout.operator("view3d.view_selected", text="Frame Selected (Quad View)").use_all_regions = True

        layout.operator("view3d.view_all").center = False
        layout.operator("view3d.view_persportho", text="Perspective/Orthographic")
        layout.menu("VIEW3D_MT_view_local")
        layout.prop(view, "show_viewer", text="Viewer Node")

        layout.separator()

        layout.menu("VIEW3D_MT_view_cameras", text="Cameras")

        layout.separator()
        layout.menu("VIEW3D_MT_view_viewpoint")
        layout.menu("VIEW3D_MT_view_navigation")
        layout.menu("VIEW3D_MT_view_align")

        layout.separator()

        layout.operator_context = 'INVOKE_REGION_WIN'
        layout.menu("VIEW3D_MT_view_regions", text="View Regions")

        layout.separator()

        layout.operator("screen.animation_play", text="Play Animation")

        layout.separator()

        layout.operator(
            "render.opengl",
            text="Render Viewport Preview",
            icon='RENDER_STILL',
        )
        layout.operator(
            "render.opengl",
            text="Render Playblast",
            icon='RENDER_ANIMATION',
        ).animation = True
        props = layout.operator(
            "render.opengl",
            text="Render Playblast on Keyframes",
            icon='RENDER_ANIMATION',
        )
        props.animation = True
        props.render_keyed_only = True

        layout.separator()

        layout.menu("INFO_MT_area")


class VIEW3D_MT_view_local(Menu):
    bl_label = "Local View"

    def draw(self, _context):
        layout = self.layout

        layout.operator("view3d.localview", text="Toggle Local View")
        layout.operator("view3d.localview_remove_from")


class VIEW3D_MT_view_cameras(Menu):
    bl_label = "Cameras"

    def draw(self, _context):
        layout = self.layout

        layout.operator("view3d.object_as_camera")
        layout.operator("view3d.view_camera", text="Active Camera")
        layout.operator("view3d.view_center_camera")


class VIEW3D_MT_view_viewpoint(Menu):
    bl_label = "Viewpoint"

    def draw(self, _context):
        layout = self.layout

        layout.operator("view3d.view_camera", text="Camera", text_ctxt=i18n_contexts.editor_view3d)

        layout.separator()

        layout.operator("view3d.view_axis", text="Top", text_ctxt=i18n_contexts.editor_view3d).type = 'TOP'
        layout.operator("view3d.view_axis", text="Bottom", text_ctxt=i18n_contexts.editor_view3d).type = 'BOTTOM'

        layout.separator()

        layout.operator("view3d.view_axis", text="Front", text_ctxt=i18n_contexts.editor_view3d).type = 'FRONT'
        layout.operator("view3d.view_axis", text="Back", text_ctxt=i18n_contexts.editor_view3d).type = 'BACK'

        layout.separator()

        layout.operator("view3d.view_axis", text="Right", text_ctxt=i18n_contexts.editor_view3d).type = 'RIGHT'
        layout.operator("view3d.view_axis", text="Left", text_ctxt=i18n_contexts.editor_view3d).type = 'LEFT'


class VIEW3D_MT_view_navigation(Menu):
    bl_label = "Navigation"

    def draw(self, _context):
        from math import pi
        layout = self.layout

        layout.operator_enum("view3d.view_orbit", "type")
        props = layout.operator("view3d.view_orbit", text="Orbit Opposite")
        props.type = 'ORBITRIGHT'
        props.angle = pi

        layout.separator()

        layout.operator("view3d.view_roll", text="Roll Left").type = 'LEFT'
        layout.operator("view3d.view_roll", text="Roll Right").type = 'RIGHT'

        layout.separator()

        layout.operator_enum("view3d.view_pan", "type")

        layout.separator()

        layout.operator("view3d.zoom", text="Zoom In").delta = 1
        layout.operator("view3d.zoom", text="Zoom Out").delta = -1
        layout.operator("view3d.zoom_border", text="Zoom Region...")
        layout.operator("view3d.dolly", text="Dolly View...")
        layout.operator("view3d.zoom_camera_1_to_1", text="Zoom Camera 1:1")

        layout.separator()

        layout.operator("view3d.fly")
        layout.operator("view3d.walk")


class VIEW3D_MT_view_align(Menu):
    bl_label = "Align View"

    def draw(self, _context):
        layout = self.layout

        layout.menu("VIEW3D_MT_view_align_selected")

        layout.separator()

        layout.operator("view3d.camera_to_view", text="Align Active Camera to View")
        layout.operator("view3d.camera_to_view_selected", text="Align Active Camera to Selected")

        layout.separator()

        layout.operator("view3d.view_all", text="Center Cursor and Frame All").center = True
        layout.operator("view3d.view_center_cursor")

        layout.separator()

        layout.operator("view3d.view_lock_to_active")
        layout.operator("view3d.view_lock_clear")


class VIEW3D_MT_view_align_selected(Menu):
    bl_label = "Align View to Active"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("view3d.view_axis", text="Top", text_ctxt=i18n_contexts.editor_view3d)
        props.align_active = True
        props.type = 'TOP'

        props = layout.operator("view3d.view_axis", text="Bottom", text_ctxt=i18n_contexts.editor_view3d)
        props.align_active = True
        props.type = 'BOTTOM'

        layout.separator()

        props = layout.operator("view3d.view_axis", text="Front", text_ctxt=i18n_contexts.editor_view3d)
        props.align_active = True
        props.type = 'FRONT'

        props = layout.operator("view3d.view_axis", text="Back", text_ctxt=i18n_contexts.editor_view3d)
        props.align_active = True
        props.type = 'BACK'

        layout.separator()

        props = layout.operator("view3d.view_axis", text="Right", text_ctxt=i18n_contexts.editor_view3d)
        props.align_active = True
        props.type = 'RIGHT'

        props = layout.operator("view3d.view_axis", text="Left", text_ctxt=i18n_contexts.editor_view3d)
        props.align_active = True
        props.type = 'LEFT'


class VIEW3D_MT_view_regions(Menu):
    bl_label = "View Regions"

    def draw(self, _context):
        layout = self.layout
        layout.operator("view3d.clip_border", text="Clipping Region...")
        layout.operator("view3d.render_border", text="Render Region...")

        layout.separator()

        layout.operator("view3d.clear_render_border")


# ********** Select menus, suffix from context.mode **********

class VIEW3D_MT_select_object(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.select_all", text="All").action = 'SELECT'
        layout.operator("object.select_all", text="None").action = 'DESELECT'
        layout.operator("object.select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("view3d.select_box")
        layout.operator("view3d.select_circle")
        layout.operator_menu_enum("view3d.select_lasso", "mode")

        layout.separator()

        layout.operator("object.select_camera", text="Select Active Camera")
        layout.operator("object.select_mirror")
        layout.operator("object.select_random", text="Select Random")

        layout.separator()

        layout.operator_menu_enum("object.select_by_type", "type", text="Select All by Type")
        layout.operator("object.select_grouped", text="Select Grouped by Type").type = 'TYPE'
        layout.operator("object.select_grouped", text="Select Grouped by Collection").type = 'COLLECTION'
        layout.operator("object.select_grouped", text="Select Grouped by Hook").type = 'HOOK'
        layout.operator("object.select_grouped", text="Select Grouped by Pass").type = 'PASS'
        layout.operator("object.select_grouped", text="Select Grouped by Color").type = 'COLOR'
        layout.operator("object.select_grouped", text="Select Grouped by Keying Set").type = 'KEYINGSET'
        layout.operator("object.select_grouped", text="Select Grouped by Light Type").type = 'LIGHT_TYPE'
        layout.operator_menu_enum("object.select_linked", "type", text="Select Linked")
        layout.operator("object.select_pattern", text="Select Pattern...")


class VIEW3D_MT_select_particle(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("particle.select_all", text="All").action = 'SELECT'
        layout.operator("particle.select_all", text="None").action = 'DESELECT'
        layout.operator("particle.select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("view3d.select_box")
        layout.operator("view3d.select_circle")
        layout.operator_menu_enum("view3d.select_lasso", "mode")

        layout.separator()

        layout.operator("particle.select_random")

        layout.separator()

        layout.operator("particle.select_more", text="More")
        layout.operator("particle.select_less", text="Less")

        layout.separator()

        layout.operator("particle.select_linked", text="Select Linked")

        layout.separator()

        layout.operator("particle.select_roots", text="Roots")
        layout.operator("particle.select_tips", text="Tips")


class VIEW3D_MT_edit_mesh_select_similar(Menu):
    bl_label = "Select Similar"

    def draw(self, _context):
        layout = self.layout

        layout.operator_enum("mesh.select_similar", "type")

        layout.separator()

        layout.operator("mesh.select_similar_region", text="Face Regions")


class VIEW3D_MT_edit_mesh_select_by_trait(Menu):
    bl_label = "Select All by Trait"

    def draw(self, context):
        layout = self.layout
        _is_vert_mode, _is_edge_mode, is_face_mode = context.tool_settings.mesh_select_mode

        if is_face_mode is False:
            layout.operator("mesh.select_non_manifold", text="Non Manifold")
        layout.operator("mesh.select_loose", text="Loose Geometry")
        layout.operator("mesh.select_interior_faces", text="Interior Faces")
        layout.operator("mesh.select_face_by_sides", text="Faces by Sides")
        layout.operator("mesh.select_by_pole_count", text="Poles by Count")

        layout.separator()

        layout.operator("mesh.select_ungrouped", text="Ungrouped Vertices")


class VIEW3D_MT_edit_mesh_select_more_less(Menu):
    bl_label = "Select More/Less"

    def draw(self, _context):
        layout = self.layout

        layout.operator("mesh.select_more", text="More")
        layout.operator("mesh.select_less", text="Less")

        layout.separator()

        layout.operator("mesh.select_next_item", text="Next Active")
        layout.operator("mesh.select_prev_item", text="Previous Active")


class VIEW3D_MT_edit_mesh_select_linked(Menu):
    bl_label = "Select Linked"

    def draw(self, _context):
        layout = self.layout

        layout.operator("mesh.select_linked", text="Linked")
        layout.operator("mesh.shortest_path_select", text="Shortest Path")
        layout.operator("mesh.faces_select_linked_flat", text="Linked Flat Faces")


class VIEW3D_MT_edit_mesh_select_loops(Menu):
    bl_label = "Select Loops"

    def draw(self, _context):
        layout = self.layout

        layout.operator("mesh.select_edge_loop_multi", text="Edge Loops")
        layout.operator("mesh.select_edge_ring_multi", text="Edge Rings")
        layout.operator("mesh.select_boundary_loop_multi", text="Boundary Loops")

        layout.separator()

        layout.operator("mesh.loop_to_region", text="Loop Inner-Region")
        layout.operator("mesh.region_to_loop", text="Boundary of Selected")


class VIEW3D_MT_select_edit_mesh(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        # primitive
        layout.operator("mesh.select_all", text="All").action = 'SELECT'
        layout.operator("mesh.select_all", text="None").action = 'DESELECT'
        layout.operator("mesh.select_all", text="Invert").action = 'INVERT'

        # gesture
        layout.separator()
        layout.operator("view3d.select_box")
        layout.operator("view3d.select_circle")
        layout.operator_menu_enum("view3d.select_lasso", "mode")

        # numeric
        layout.separator()
        layout.operator("mesh.select_mirror")
        layout.operator("mesh.select_random", text="Select Random")
        layout.operator("mesh.select_nth")

        # more/less
        layout.separator()
        layout.menu("VIEW3D_MT_edit_mesh_select_more_less", text="More/Less")

        # grouped
        layout.separator()
        layout.menu("VIEW3D_MT_edit_mesh_select_similar")
        layout.menu("VIEW3D_MT_edit_mesh_select_by_trait")
        layout.menu("VIEW3D_MT_edit_mesh_select_linked")
        layout.menu("VIEW3D_MT_edit_mesh_select_loops")

        # geometric
        layout.separator()
        layout.operator("mesh.edges_select_sharp", text="Sharp Edges")
        layout.operator("mesh.select_axis", text="Side of Active")

        # attribute
        layout.separator()
        layout.operator("mesh.select_by_attribute", text="By Attribute")

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_select_edit_curve(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curve.select_all", text="All").action = 'SELECT'
        layout.operator("curve.select_all", text="None").action = 'DESELECT'
        layout.operator("curve.select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("view3d.select_box")
        layout.operator("view3d.select_circle")
        layout.operator_menu_enum("view3d.select_lasso", "mode")

        layout.separator()

        layout.operator("curve.select_random")
        layout.operator("curve.select_nth")

        layout.separator()

        layout.operator("curve.select_more", text="More")
        layout.operator("curve.select_less", text="Less")

        layout.separator()

        layout.operator("curve.select_linked", text="Select Linked")
        layout.operator_menu_enum("curve.select_similar", "type")

        layout.separator()

        layout.operator("curve.de_select_first")
        layout.operator("curve.de_select_last")
        layout.operator("curve.select_next")
        layout.operator("curve.select_previous")


class VIEW3D_MT_select_edit_surface(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curve.select_all", text="All").action = 'SELECT'
        layout.operator("curve.select_all", text="None").action = 'DESELECT'
        layout.operator("curve.select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("view3d.select_box")
        layout.operator("view3d.select_circle")
        layout.operator_menu_enum("view3d.select_lasso", "mode")

        layout.separator()

        layout.operator("curve.select_random")
        layout.operator("curve.select_nth")

        layout.separator()

        layout.operator("curve.select_more", text="More")
        layout.operator("curve.select_less", text="Less")

        layout.separator()

        layout.operator("curve.select_linked", text="Select Linked")
        layout.operator_menu_enum("curve.select_similar", "type")

        layout.separator()

        layout.operator("curve.select_row", text="Control Point Row")


class VIEW3D_MT_select_edit_text(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("font.select_all", text="All")

        layout.separator()

        layout.operator("font.move_select", text="Top").type = 'TEXT_BEGIN'
        layout.operator("font.move_select", text="Bottom").type = 'TEXT_END'

        layout.separator()

        layout.operator("font.move_select", text="Previous Block").type = 'PREVIOUS_PAGE'
        layout.operator("font.move_select", text="Next Block").type = 'NEXT_PAGE'

        layout.separator()

        layout.operator("font.move_select", text="Line Begin").type = 'LINE_BEGIN'
        layout.operator("font.move_select", text="Line End").type = 'LINE_END'

        layout.separator()

        layout.operator("font.move_select", text="Previous Line").type = 'PREVIOUS_LINE'
        layout.operator("font.move_select", text="Next Line").type = 'NEXT_LINE'

        layout.separator()

        layout.operator("font.move_select", text="Previous Word").type = 'PREVIOUS_WORD'
        layout.operator("font.move_select", text="Next Word").type = 'NEXT_WORD'


class VIEW3D_MT_edit_lattice_context_menu(Menu):
    bl_label = "Lattice"

    def draw(self, _context):
        layout = self.layout

        layout.menu("VIEW3D_MT_mirror")
        layout.operator_menu_enum("lattice.flip", "axis")
        layout.menu("VIEW3D_MT_snap")

        layout.separator()

        layout.operator("lattice.make_regular")


class VIEW3D_MT_select_edit_lattice(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("lattice.select_all", text="All").action = 'SELECT'
        layout.operator("lattice.select_all", text="None").action = 'DESELECT'
        layout.operator("lattice.select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("view3d.select_box")
        layout.operator("view3d.select_circle")
        layout.operator_menu_enum("view3d.select_lasso", "mode")

        layout.separator()

        layout.operator("lattice.select_mirror")
        layout.operator("lattice.select_random")

        layout.separator()

        layout.operator("lattice.select_more", text="More")
        layout.operator("lattice.select_less", text="Less")

        layout.separator()

        layout.operator("lattice.select_ungrouped", text="Ungrouped Vertices")


class VIEW3D_MT_select_paint_mask(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("paint.face_select_all", text="All").action = 'SELECT'
        layout.operator("paint.face_select_all", text="None").action = 'DESELECT'
        layout.operator("paint.face_select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("view3d.select_box")
        layout.operator("view3d.select_circle")
        layout.operator_menu_enum("view3d.select_lasso", "mode")

        layout.separator()

        layout.operator("paint.face_select_more", text="More")
        layout.operator("paint.face_select_less", text="Less")

        layout.separator()

        layout.operator("paint.face_select_linked")

        layout.separator()
        layout.operator("paint.face_vert_reveal", text="Reveal Hidden")
        layout.operator("paint.face_select_hide", text="Hide Selected")
        props = layout.operator("paint.face_select_hide", text="Hide Unselected")
        props.unselected = True


class VIEW3D_MT_select_paint_mask_vertex(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("paint.vert_select_all", text="All").action = 'SELECT'
        layout.operator("paint.vert_select_all", text="None").action = 'DESELECT'
        layout.operator("paint.vert_select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("view3d.select_box")
        layout.operator("view3d.select_circle")
        layout.operator_menu_enum("view3d.select_lasso", "mode")

        layout.separator()

        layout.operator("paint.vert_select_more", text="More")
        layout.operator("paint.vert_select_less", text="Less")

        layout.separator()

        layout.operator("paint.vert_select_linked", text="Select Linked")

        layout.separator()

        layout.operator("paint.vert_select_ungrouped", text="Ungrouped Vertices")

        layout.separator()
        layout.operator("paint.face_vert_reveal", text="Reveal Hidden")
        layout.operator("paint.vert_select_hide", text="Hide Selected")
        props = layout.operator("paint.vert_select_hide", text="Hide Unselected")
        props.unselected = True


class VIEW3D_MT_select_edit_pointcloud(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("pointcloud.select_all", text="All").action = 'SELECT'
        layout.operator("pointcloud.select_all", text="None").action = 'DESELECT'
        layout.operator("pointcloud.select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("pointcloud.select_random")

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_edit_curves_select_more_less(Menu):
    bl_label = "Select More/Less"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curves.select_more", text="More")
        layout.operator("curves.select_less", text="Less")


class VIEW3D_MT_select_edit_curves(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curves.select_all", text="All").action = 'SELECT'
        layout.operator("curves.select_all", text="None").action = 'DESELECT'
        layout.operator("curves.select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("curves.select_random")

        layout.separator()

        layout.menu("VIEW3D_MT_edit_curves_select_more_less", text="More/Less")

        layout.separator()

        layout.operator("curves.select_linked")

        layout.separator()

        layout.operator("curves.select_ends", text="Endpoints")

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_select_sculpt_curves(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curves.select_all", text="All").action = 'SELECT'
        layout.operator("curves.select_all", text="None").action = 'DESELECT'
        layout.operator("curves.select_all", text="Invert").action = 'INVERT'

        layout.separator()

        layout.operator("sculpt_curves.select_random")

        layout.separator()

        layout.operator("curves.select_ends", text="Endpoints")
        layout.operator("sculpt_curves.select_grow", text="Grow Selection")

        layout.template_node_operator_asset_menu_items(catalog_path="Select")


class VIEW3D_MT_mesh_add(Menu):
    bl_idname = "VIEW3D_MT_mesh_add"
    bl_label = "Mesh"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout

        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("mesh.primitive_plane_add", text="Plane", icon='MESH_PLANE')
        layout.operator("mesh.primitive_cube_add", text="Cube", icon='MESH_CUBE')
        layout.operator("mesh.primitive_circle_add", text="Circle", icon='MESH_CIRCLE')
        layout.operator("mesh.primitive_uv_sphere_add", text="UV Sphere", icon='MESH_UVSPHERE')
        layout.operator("mesh.primitive_ico_sphere_add", text="Ico Sphere", icon='MESH_ICOSPHERE')
        layout.operator("mesh.primitive_cylinder_add", text="Cylinder", icon='MESH_CYLINDER')
        layout.operator("mesh.primitive_cone_add", text="Cone", icon='MESH_CONE')
        layout.operator("mesh.primitive_torus_add", text="Torus", icon='MESH_TORUS')

        layout.separator()

        layout.operator("mesh.primitive_grid_add", text="Grid", icon='MESH_GRID')
        layout.operator("mesh.primitive_monkey_add", text="Monkey", icon='MESH_MONKEY')

        layout.template_node_operator_asset_menu_items(catalog_path="Add")


class VIEW3D_MT_curve_add(Menu):
    bl_idname = "VIEW3D_MT_curve_add"
    bl_label = "Curve"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout

        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("curve.primitive_bezier_curve_add", text="Bézier", icon='CURVE_BEZCURVE')
        layout.operator("curve.primitive_bezier_circle_add", text="Circle", icon='CURVE_BEZCIRCLE')

        layout.separator()

        layout.operator("curve.primitive_nurbs_curve_add", text="Nurbs Line", icon='CURVE_NCURVE')
        layout.operator("curve.primitive_nurbs_circle_add", text="Nurbs Circle", icon='CURVE_NCIRCLE')
        layout.operator("curve.primitive_nurbs_path_add", text="Path", icon='CURVE_PATH')

        layout.separator()

        layout.operator("object.curves_empty_hair_add", text="Empty Hair", icon='CURVES_DATA')
        layout.operator("object.quick_fur", text="Fur", icon='CURVES_DATA')

        experimental = context.preferences.experimental
        if experimental.use_new_curves_tools:
            layout.operator("object.curves_random_add", text="Random", icon='CURVES_DATA')


class VIEW3D_MT_surface_add(Menu):
    bl_idname = "VIEW3D_MT_surface_add"
    bl_label = "Surface"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout

        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("surface.primitive_nurbs_surface_curve_add", text="Nurbs Curve", icon='SURFACE_NCURVE')
        layout.operator("surface.primitive_nurbs_surface_circle_add", text="Nurbs Circle", icon='SURFACE_NCIRCLE')
        layout.operator("surface.primitive_nurbs_surface_surface_add", text="Nurbs Surface", icon='SURFACE_NSURFACE')
        layout.operator(
            "surface.primitive_nurbs_surface_cylinder_add",
            text="Nurbs Cylinder", icon='SURFACE_NCYLINDER',
        )
        layout.operator("surface.primitive_nurbs_surface_sphere_add", text="Nurbs Sphere", icon='SURFACE_NSPHERE')
        layout.operator("surface.primitive_nurbs_surface_torus_add", text="Nurbs Torus", icon='SURFACE_NTORUS')


class TOPBAR_MT_edit_curve_add(Menu):
    bl_idname = "TOPBAR_MT_edit_curve_add"
    bl_label = "Add"
    bl_translation_context = i18n_contexts.operator_default
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout

        is_surf = context.active_object.type == 'SURFACE'

        layout.operator_context = 'EXEC_REGION_WIN'

        if is_surf:
            VIEW3D_MT_surface_add.draw(self, context)
        else:
            VIEW3D_MT_curve_add.draw(self, context)


class VIEW3D_MT_light_add(Menu):
    bl_idname = "VIEW3D_MT_light_add"
    bl_context = i18n_contexts.id_light
    bl_label = "Light"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout

        layout.operator_context = 'INVOKE_REGION_WIN'
        layout.operator_enum("object.light_add", "type")


class VIEW3D_MT_lightprobe_add(Menu):
    bl_idname = "VIEW3D_MT_lightprobe_add"
    bl_label = "Light Probe"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout

        layout.operator_context = 'INVOKE_REGION_WIN'
        layout.operator_enum("object.lightprobe_add", "type")


class VIEW3D_MT_camera_add(Menu):
    bl_idname = "VIEW3D_MT_camera_add"
    bl_label = "Camera"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout
        layout.operator_context = 'EXEC_REGION_WIN'
        layout.operator("object.camera_add", text="Camera", icon='OUTLINER_OB_CAMERA')


class VIEW3D_MT_volume_add(Menu):
    bl_idname = "VIEW3D_MT_volume_add"
    bl_label = "Volume"
    bl_translation_context = i18n_contexts.id_id
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout
        layout.operator("object.volume_import", text="Import OpenVDB...", icon='OUTLINER_DATA_VOLUME')
        layout.operator(
            "object.volume_add", text="Empty",
            text_ctxt=i18n_contexts.id_volume,
            icon='OUTLINER_DATA_VOLUME',
        )


class VIEW3D_MT_lattice_add(Menu):
    bl_idname = "VIEW3D_MT_lattice_add"
    bl_label = "Lattice"
    bl_translation_context = i18n_contexts.operator_default
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout
        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("object.add", text="Lattice", icon='OUTLINER_OB_LATTICE').type = 'LATTICE'
        layout.operator("object.lattice_add_to_selected", text="Lattice Deform Selected", icon='OUTLINER_OB_LATTICE')


class VIEW3D_MT_add(Menu):
    bl_label = "Add"
    bl_translation_context = i18n_contexts.operator_default
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout

        if layout.operator_context == 'EXEC_REGION_WIN':
            layout.operator_context = 'INVOKE_REGION_WIN'
            layout.operator("WM_OT_search_single_menu", text="Search...", icon='VIEWZOOM').menu_idname = "VIEW3D_MT_add"
            layout.separator()

        # NOTE: don't use 'EXEC_SCREEN' or operators won't get the `v3d` context.

        # NOTE: was `EXEC_AREA`, but this context does not have the `rv3d`, which prevents
        #       "align_view" to work on first call (see #32719).
        layout.operator_context = 'EXEC_REGION_WIN'

        # layout.operator_menu_enum("object.mesh_add", "type", text="Mesh", icon='OUTLINER_OB_MESH')
        layout.menu("VIEW3D_MT_mesh_add", icon='OUTLINER_OB_MESH')

        # layout.operator_menu_enum("object.curve_add", "type", text="Curve", icon='OUTLINER_OB_CURVE')
        layout.menu("VIEW3D_MT_curve_add", icon='OUTLINER_OB_CURVE')
        # layout.operator_menu_enum("object.surface_add", "type", text="Surface", icon='OUTLINER_OB_SURFACE')
        layout.menu("VIEW3D_MT_surface_add", icon='OUTLINER_OB_SURFACE')
        layout.operator("object.text_add", text="Text", icon='OUTLINER_OB_FONT')
        layout.operator("object.pointcloud_random_add", text="Point Cloud", icon='OUTLINER_OB_POINTCLOUD')
        layout.menu("VIEW3D_MT_volume_add", text="Volume", text_ctxt=i18n_contexts.id_id, icon='OUTLINER_OB_VOLUME')

        layout.separator()

        layout.menu("VIEW3D_MT_lattice_add", icon='OUTLINER_OB_LATTICE')

        layout.separator()

        layout.menu("VIEW3D_MT_image_add", text="Image", icon='OUTLINER_OB_IMAGE')

        layout.separator()

        layout.menu("VIEW3D_MT_light_add", icon='OUTLINER_OB_LIGHT')
        layout.menu("VIEW3D_MT_lightprobe_add", icon='OUTLINER_OB_LIGHTPROBE')

        layout.separator()

        if VIEW3D_MT_camera_add.is_extended():
            layout.menu("VIEW3D_MT_camera_add", icon='OUTLINER_OB_CAMERA')
        else:
            VIEW3D_MT_camera_add.draw(self, context)

        layout.separator()

        layout.operator_menu_enum("object.effector_add", "type", text="Force Field", icon='OUTLINER_OB_FORCE_FIELD')

        layout.separator()

class VIEW3D_MT_image_add(Menu):
    bl_label = "Add Image"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout
        # Explicitly set background mode on/off as operator will try to
        # auto detect which mode to use otherwise.
        layout.operator("image.import_as_mesh_planes", text="Mesh Plane...", icon='MESH_PLANE')


class VIEW3D_MT_object_relations(Menu):
    bl_label = "Relations"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.make_dupli_face")

        layout.separator()

        layout.operator_menu_enum("object.make_local", "type", text="Make Local")
        layout.menu("VIEW3D_MT_make_single_user")


class VIEW3D_MT_object_liboverride(Menu):
    bl_label = "Library Override"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.make_override_library", text="Make")
        layout.operator("object.reset_override_library", text="Reset")
        layout.operator("object.clear_override_library", text="Clear")


class VIEW3D_MT_object(Menu):
    bl_context = "objectmode"
    bl_label = "Object"

    def draw(self, context):
        layout = self.layout

        ob = context.object

        layout.menu("VIEW3D_MT_transform_object")
        layout.operator_menu_enum("object.origin_set", text="Set Origin", property="type")
        layout.menu("VIEW3D_MT_mirror")
        layout.menu("VIEW3D_MT_object_clear")
        layout.menu("VIEW3D_MT_object_apply")
        layout.menu("VIEW3D_MT_snap")

        layout.separator()

        layout.operator("object.duplicate_move")
        layout.operator("object.duplicate_move_linked")
        layout.operator("object.join")

        layout.separator()

        layout.operator("view3d.copybuffer", text="Copy Objects", icon='COPYDOWN')
        layout.operator("view3d.pastebuffer", text="Paste Objects", icon='PASTEDOWN')

        layout.separator()

        layout.menu("VIEW3D_MT_object_asset", icon='ASSET_MANAGER')
        layout.menu("VIEW3D_MT_object_collection")

        layout.separator()

        layout.menu("VIEW3D_MT_object_liboverride", icon='LIBRARY_DATA_OVERRIDE')
        layout.menu("VIEW3D_MT_object_relations")
        layout.menu("VIEW3D_MT_object_modifiers", icon='MODIFIER')
        layout.menu("VIEW3D_MT_object_constraints", icon='CONSTRAINT')
        layout.menu("VIEW3D_MT_object_track")
        layout.menu("VIEW3D_MT_make_links")

        layout.separator()

        layout.operator("object.shade_smooth")
        if ob and ob.type in {'MESH', 'CURVE', 'FONT'}:
            layout.operator("object.shade_auto_smooth")
        layout.operator("object.shade_flat")

        layout.separator()

        layout.menu("VIEW3D_MT_object_animation")
        layout.menu("VIEW3D_MT_object_rigid_body")

        layout.separator()

        layout.menu("VIEW3D_MT_object_quick_effects")

        layout.separator()

        layout.menu("VIEW3D_MT_object_convert")

        layout.separator()

        layout.menu("VIEW3D_MT_object_showhide")
        layout.menu("VIEW3D_MT_object_cleanup")

        layout.separator()

        layout.operator_context = 'EXEC_REGION_WIN'
        layout.operator("object.delete", text="Delete").use_global = False
        layout.operator("object.delete", text="Delete Global").use_global = True

        layout.template_node_operator_asset_menu_items(catalog_path="Object")


class VIEW3D_MT_object_animation(Menu):
    bl_label = "Animation"

    def draw(self, _context):
        layout = self.layout

        layout.operator("anim.keyframe_insert", text="Insert Keyframe")
        layout.operator("anim.keyframe_insert_menu", text="Insert Keyframe with Keying Set...").always_prompt = True
        layout.operator("anim.keyframe_delete_v3d", text="Delete Keyframes...")
        layout.operator("anim.keyframe_clear_v3d", text="Clear Keyframes...")
        layout.operator("anim.keying_set_active_set", text="Change Keying Set...")

        layout.separator()

        layout.operator("nla.bake", text="Bake Action...")
        layout.operator("anim.replace_action")
        layout.operator("anim.replace_action_new")


class VIEW3D_MT_object_rigid_body(Menu):
    bl_label = "Rigid Body"

    def draw(self, _context):
        layout = self.layout

        layout.operator("rigidbody.objects_add", text="Add Active").type = 'ACTIVE'
        layout.operator("rigidbody.objects_add", text="Add Passive").type = 'PASSIVE'

        layout.separator()

        layout.operator("rigidbody.objects_remove", text="Remove")

        layout.separator()

        layout.operator("rigidbody.shape_change", text="Change Shape")
        layout.operator("rigidbody.mass_calculate", text="Calculate Mass")
        layout.operator("rigidbody.object_settings_copy", text="Copy from Active")
        layout.operator("object.visual_transform_apply", text="Apply Transformation")
        layout.operator("rigidbody.bake_to_keyframes", text="Bake to Keyframes")

        layout.separator()



class VIEW3D_MT_object_clear(Menu):
    bl_label = "Clear"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.location_clear", text="Location", text_ctxt=i18n_contexts.default).clear_delta = False
        layout.operator("object.rotation_clear", text="Rotation", text_ctxt=i18n_contexts.default).clear_delta = False
        layout.operator("object.scale_clear", text="Scale", text_ctxt=i18n_contexts.default).clear_delta = False

        layout.separator()



class VIEW3D_MT_object_context_menu(Menu):
    bl_label = "Object"

    def draw(self, context):
        layout = self.layout

        view = context.space_data

        obj = context.object

        selected_objects_len = len(context.selected_objects)

        # If nothing is selected
        # (disabled for now until it can be made more useful).
        '''
        if selected_objects_len == 0:

            layout.menu("VIEW3D_MT_add", text="Add", text_ctxt=i18n_contexts.operator_default)
            layout.operator("view3d.pastebuffer", text="Paste Objects", icon='PASTEDOWN')

            return
        '''

        # If something is selected

        # Individual object types.
        if obj is None:
            pass

        elif obj.type == 'CAMERA':
            layout.operator_context = 'INVOKE_REGION_WIN'

            layout.operator("view3d.object_as_camera", text="Set Active Camera")

            if obj.data.type == 'PERSP':
                props = layout.operator("wm.context_modal_mouse", text="Adjust Focal Length")
                props.data_path_iter = "selected_editable_objects"
                props.data_path_item = "data.lens"
                props.input_scale = 0.1
                if obj.data.lens_unit == 'MILLIMETERS':
                    props.header_text = rpt_("Camera Focal Length: %.1fmm")
                else:
                    props.header_text = rpt_("Camera Focal Length: %.1f\u00B0")

            else:
                props = layout.operator("wm.context_modal_mouse", text="Camera Lens Scale")
                props.data_path_iter = "selected_editable_objects"
                props.data_path_item = "data.ortho_scale"
                props.input_scale = 0.01
                props.header_text = rpt_("Camera Lens Scale: %.3f")

            if not obj.data.dof.focus_object:
                if view and view.camera == obj and view.region_3d.view_perspective == 'CAMERA':
                    props = layout.operator("ui.eyedropper_depth", text="DOF Distance (Pick)")
                else:
                    props = layout.operator("wm.context_modal_mouse", text="Adjust Focus Distance")
                    props.data_path_iter = "selected_editable_objects"
                    props.data_path_item = "data.dof.focus_distance"
                    props.input_scale = 0.02
                    props.header_text = rpt_("Focus Distance: %.3f")

            layout.separator()

            layout.operator("object.transform_axis_target")

            layout.separator()

        elif obj.type in {'CURVE', 'FONT'}:
            layout.operator_context = 'INVOKE_REGION_WIN'

            props = layout.operator("wm.context_modal_mouse", text="Adjust Extrusion")
            props.data_path_iter = "selected_editable_objects"
            props.data_path_item = "data.extrude"
            props.input_scale = 0.01
            props.header_text = rpt_("Extrude: %.3f")

            props = layout.operator("wm.context_modal_mouse", text="Adjust Offset")
            props.data_path_iter = "selected_editable_objects"
            props.data_path_item = "data.offset"
            props.input_scale = 0.01
            props.header_text = rpt_("Offset: %.3f")

            layout.separator()

        elif obj.type == 'LIGHT':
            light = obj.data

            layout.operator_context = 'INVOKE_REGION_WIN'

            props = layout.operator("wm.context_modal_mouse", text="Adjust Light Power")
            props.data_path_iter = "selected_editable_objects"
            props.data_path_item = "data.energy"
            props.input_scale = 1.0
            props.header_text = rpt_("Light Power: %.3f")

            if light.type == 'AREA':
                if light.shape in {'RECTANGLE', 'ELLIPSE'}:
                    props = layout.operator("wm.context_modal_mouse", text="Adjust Area Light X Size")
                    props.data_path_iter = "selected_editable_objects"
                    props.data_path_item = "data.size"
                    props.header_text = rpt_("Light Size X: %.3f")

                    props = layout.operator("wm.context_modal_mouse", text="Adjust Area Light Y Size")
                    props.data_path_iter = "selected_editable_objects"
                    props.data_path_item = "data.size_y"
                    props.header_text = rpt_("Light Size Y: %.3f")
                else:
                    props = layout.operator("wm.context_modal_mouse", text="Adjust Area Light Size")
                    props.data_path_iter = "selected_editable_objects"
                    props.data_path_item = "data.size"
                    props.header_text = rpt_("Light Size: %.3f")

            elif light.type in {'SPOT', 'POINT'}:
                props = layout.operator("wm.context_modal_mouse", text="Adjust Light Radius")
                props.data_path_iter = "selected_editable_objects"
                props.data_path_item = "data.shadow_soft_size"
                props.header_text = rpt_("Light Radius: %.3f")

            elif light.type == 'SUN':
                props = layout.operator("wm.context_modal_mouse", text="Adjust Sun Light Angle")
                props.data_path_iter = "selected_editable_objects"
                props.data_path_item = "data.angle"
                props.header_text = rpt_("Light Angle: %.3f")

            if light.type == 'SPOT':
                layout.separator()

                props = layout.operator("wm.context_modal_mouse", text="Adjust Spot Light Beam Angle")
                props.data_path_iter = "selected_editable_objects"
                props.data_path_item = "data.spot_size"
                props.input_scale = 0.01
                props.header_text = rpt_("Beam Angle: %.2f")

                props = layout.operator("wm.context_modal_mouse", text="Adjust Spot Light Blend")
                props.data_path_iter = "selected_editable_objects"
                props.data_path_item = "data.spot_blend"
                props.input_scale = -0.01
                props.header_text = rpt_("Spot Blend: %.2f")

            layout.separator()

            layout.operator("object.transform_axis_target")

            layout.separator()

        # Shared among some object types.
        if obj is not None:
            if obj.type in {'MESH', 'CURVE', 'SURFACE'}:
                layout.operator("object.shade_smooth")
                if obj.type == 'MESH':
                    layout.operator("object.shade_auto_smooth")
                layout.operator("object.shade_flat")

                layout.separator()

            if obj.type in {'MESH', 'CURVE', 'SURFACE'}:
                if selected_objects_len > 1:
                    layout.operator("object.join")

            if obj.type in {
                    'MESH', 'CURVE', 'CURVES', 'SURFACE', 'POINTCLOUD',
                    'META', 'FONT'
            }:
                layout.operator_menu_enum("object.convert", "target")

            if (obj.type in {
                'MESH',
                'CURVE',
                'CURVES',
                'SURFACE',
                'LATTICE',
                'META',
                'FONT',
                'POINTCLOUD',
            }):
                layout.operator_context = 'INVOKE_REGION_WIN'
                layout.operator_menu_enum("object.origin_set", text="Set Origin", property="type")
                layout.operator_context = 'INVOKE_DEFAULT'

                layout.separator()

        # Shared among all object types
        layout.operator("view3d.copybuffer", text="Copy Objects", icon='COPYDOWN')
        layout.operator("view3d.pastebuffer", text="Paste Objects", icon='PASTEDOWN')

        layout.separator()

        layout.operator("object.duplicate_move", icon='DUPLICATE')
        layout.operator("object.duplicate_move_linked")

        layout.separator()

        props = layout.operator("wm.call_panel", text="Rename Active Object...")
        props.name = "TOPBAR_PT_name"
        props.keep_open = False

        layout.separator()

        layout.menu("VIEW3D_MT_mirror")
        layout.menu("VIEW3D_MT_snap")
        layout.operator_context = 'INVOKE_REGION_WIN'

        if view and view.local_view:
            layout.operator("view3d.localview_remove_from")
        else:
            layout.menu("OBJECT_MT_move_to_collection")

        layout.separator()

        layout.operator("anim.keyframe_insert", text="Insert Keyframe")
        layout.operator("anim.keyframe_insert_menu", text="Insert Keyframe with Keying Set...").always_prompt = True

        layout.separator()

        layout.operator_context = 'EXEC_REGION_WIN'
        layout.operator("object.delete", text="Delete").use_global = False

        layout.template_node_operator_asset_menu_items(catalog_path="Object")


class VIEW3D_MT_object_shading(Menu):
    # XXX, this menu is a place to store shading operator in object mode
    bl_label = "Shading"

    def draw(self, _context):
        layout = self.layout
        layout.operator("object.shade_smooth", text="Smooth")
        layout.operator("object.shade_flat", text="Flat")


class VIEW3D_MT_object_apply(Menu):
    bl_label = "Apply"

    def draw(self, _context):
        layout = self.layout

        # Need invoke for the popup confirming the multi-user data operation
        layout.operator_context = 'INVOKE_DEFAULT'

        props = layout.operator("object.transform_apply", text="Location", text_ctxt=i18n_contexts.default)
        props.location, props.rotation, props.scale = True, False, False

        props = layout.operator("object.transform_apply", text="Rotation", text_ctxt=i18n_contexts.default)
        props.location, props.rotation, props.scale = False, True, False

        props = layout.operator("object.transform_apply", text="Scale", text_ctxt=i18n_contexts.default)
        props.location, props.rotation, props.scale = False, False, True

        props = layout.operator("object.transform_apply", text="All Transforms", text_ctxt=i18n_contexts.default)
        props.location, props.rotation, props.scale = True, True, True

        props = layout.operator("object.transform_apply", text="Rotation & Scale", text_ctxt=i18n_contexts.default)
        props.location, props.rotation, props.scale = False, True, True

        layout.separator()

        layout.operator(
            "object.transforms_to_deltas",
            text="Location to Deltas",
            text_ctxt=i18n_contexts.default,
        ).mode = 'LOC'
        layout.operator(
            "object.transforms_to_deltas",
            text="Rotation to Deltas",
            text_ctxt=i18n_contexts.default,
        ).mode = 'ROT'
        layout.operator(
            "object.transforms_to_deltas",
            text="Scale to Deltas",
            text_ctxt=i18n_contexts.default,
        ).mode = 'SCALE'

        layout.operator(
            "object.transforms_to_deltas",
            text="All Transforms to Deltas",
            text_ctxt=i18n_contexts.default,
        ).mode = 'ALL'
        layout.operator("object.anim_transforms_to_deltas")

        layout.separator()

        layout.operator(
            "object.visual_transform_apply",
            text="Visual Transform",
            text_ctxt=i18n_contexts.default,
        )
        layout.operator(
            "object.convert",
            text="Visual Geometry to Mesh",
            text_ctxt=i18n_contexts.default,
        ).target = 'MESH'
        layout.operator("object.visual_geometry_to_objects")
        layout.operator("object.duplicates_make_real")

        layout.template_node_operator_asset_menu_items(catalog_path="Object/Apply")


class VIEW3D_MT_object_track(Menu):
    bl_label = "Track"
    bl_translation_context = i18n_contexts.constraint

    def draw(self, _context):
        layout = self.layout

        layout.operator_enum("object.track_set", "type")

        layout.separator()

        layout.operator_enum("object.track_clear", "type")


class VIEW3D_MT_object_collection(Menu):
    bl_label = "Collection"

    def draw(self, _context):
        layout = self.layout

        layout.menu("OBJECT_MT_move_to_collection")
        layout.menu("OBJECT_MT_link_to_collection")

        layout.separator()

        layout.operator("collection.create")
        # layout.operator_menu_enum("collection.objects_remove", "collection")  # BUGGY
        layout.operator("collection.objects_remove")
        layout.operator("collection.objects_remove_all")

        layout.separator()

        layout.operator("collection.objects_add_active")
        layout.operator("collection.objects_remove_active")


class VIEW3D_MT_object_constraints(Menu):
    bl_label = "Constraints"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.constraint_add_with_targets")
        layout.operator("object.constraints_copy")

        layout.separator()

        layout.operator("object.constraints_clear")


class VIEW3D_MT_object_modifiers(Menu):
    bl_label = "Modifiers"

    def draw(self, _context):
        active_object = bpy.context.active_object
        supported_types = {
            'MESH',
            'CURVE',
            'CURVES',
            'SURFACE',
            'FONT',
            'VOLUME',
            'LATTICE',
            'POINTCLOUD'}

        layout = self.layout

        if active_object:
            if active_object.type in supported_types:
                layout.menu("OBJECT_MT_modifier_add", text="Add Modifier")

        layout.operator("object.modifiers_copy_to_selected", text="Copy Modifiers to Selected Objects")

        layout.separator()

        layout.operator("object.modifiers_clear")


class VIEW3D_MT_object_quick_effects(Menu):
    bl_label = "Quick Effects"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.quick_fur")
        layout.operator("object.quick_explode")
        layout.operator("object.quick_smoke")
        layout.operator("object.quick_liquid")
        layout.template_node_operator_asset_menu_items(catalog_path="Object/Quick Effects")


class VIEW3D_MT_object_showhide(Menu):
    bl_label = "Show/Hide"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.hide_view_clear")

        layout.separator()

        layout.operator("object.hide_view_set", text="Hide Selected").unselected = False
        layout.operator("object.hide_view_set", text="Hide Unselected").unselected = True


class VIEW3D_MT_object_cleanup(Menu):
    bl_label = "Clean Up"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.vertex_group_clean", text="Clean Vertex Group Weights").group_select_mode = 'ALL'
        layout.operator("object.vertex_group_limit_total", text="Limit Total Vertex Groups").group_select_mode = 'ALL'

        layout.separator()

        layout.operator("object.material_slot_remove_unused", text="Remove Unused Material Slots")
        layout.operator("object.material_slot_remove_all", text="Remove All Materials")


class VIEW3D_MT_object_asset(Menu):
    bl_label = "Asset"

    def draw(self, _context):
        layout = self.layout

        layout.operator("asset.mark")
        layout.operator("asset.clear", text="Clear Asset").set_fake_user = False
        layout.operator("asset.clear", text="Clear Asset (Set Fake User)").set_fake_user = True


class VIEW3D_MT_make_single_user(Menu):
    bl_label = "Make Single User"

    def draw(self, _context):
        layout = self.layout
        layout.operator_context = 'EXEC_REGION_WIN'

        props = layout.operator("object.make_single_user", text="Object")
        props.object = True
        props.obdata = props.material = props.animation = props.obdata_animation = False

        props = layout.operator("object.make_single_user", text="Object & Data")
        props.object = props.obdata = True
        props.material = props.animation = props.obdata_animation = False

        props = layout.operator("object.make_single_user", text="Object & Data & Materials")
        props.object = props.obdata = props.material = True
        props.animation = props.obdata_animation = False

        props = layout.operator("object.make_single_user", text="Materials")
        props.material = True
        props.object = props.obdata = props.animation = props.obdata_animation = False

        props = layout.operator("object.make_single_user", text="Object Animation")
        props.animation = True
        props.object = props.obdata = props.material = props.obdata_animation = False

        props = layout.operator("object.make_single_user", text="Object Data Animation")
        props.obdata_animation = props.obdata = True
        props.object = props.material = props.animation = False


class VIEW3D_MT_object_convert(Menu):
    bl_label = "Convert"

    def draw(self, context):
        layout = self.layout
        ob = context.active_object

        layout.operator_enum("object.convert", "target")

        if ob and ob.type == 'CURVES':
            layout.separator()

            layout.operator("curves.convert_to_particle_system", text="Particle System")

        layout.template_node_operator_asset_menu_items(catalog_path="Object/Convert")


class VIEW3D_MT_make_links(Menu):
    bl_label = "Link/Transfer Data"

    def draw(self, _context):
        layout = self.layout
        operator_context_default = layout.operator_context

        if len(bpy.data.scenes) > 10:
            layout.operator_context = 'INVOKE_REGION_WIN'
            layout.operator("object.make_links_scene", text="Link Objects to Scene...", icon='SCENE_DATA')
        else:
            layout.operator_context = 'EXEC_REGION_WIN'
            layout.operator_menu_enum("object.make_links_scene", "scene", text="Link Objects to Scene")

        layout.separator()

        layout.operator_context = operator_context_default

        layout.operator_enum("object.make_links_data", "type")  # inline

        layout.operator("object.join_uvs", text="Copy UV Maps")

        layout.separator()
        layout.operator_context = 'INVOKE_DEFAULT'
        layout.operator("object.data_transfer")
        layout.operator("object.datalayout_transfer")

        layout.separator()
        layout.operator_menu_enum("object.light_linking_receivers_link", "link_state")
        layout.operator_menu_enum("object.light_linking_blockers_link", "link_state")


class VIEW3D_MT_paint_vertex(Menu):
    bl_label = "Paint"

    def draw(self, _context):
        layout = self.layout

        layout.operator("paint.vertex_color_smooth")
        layout.operator("paint.vertex_color_dirt")
        layout.operator("paint.vertex_color_from_weight")

        layout.separator()

        layout.operator("paint.vertex_color_invert", text="Invert")
        layout.operator("paint.vertex_color_levels", text="Levels")
        layout.operator("paint.vertex_color_hsv", text="Hue/Saturation/Value")
        layout.operator("paint.vertex_color_brightness_contrast", text="Brightness/Contrast")

        layout.separator()

        layout.operator("paint.vertex_color_set")
        layout.operator("paint.sample_color").merged = False


class VIEW3D_MT_hook(Menu):
    bl_label = "Hooks"

    def draw(self, context):
        layout = self.layout
        layout.operator_context = 'EXEC_AREA'
        layout.operator("object.hook_add_selob")

        if any([mod.type == 'HOOK' for mod in context.active_object.modifiers]):
            layout.separator()

            layout.operator_menu_enum("object.hook_assign", "modifier")
            layout.operator_menu_enum("object.hook_remove", "modifier")

            layout.separator()

            layout.operator_menu_enum("object.hook_select", "modifier")
            layout.operator_menu_enum("object.hook_reset", "modifier")
            layout.operator_menu_enum("object.hook_recenter", "modifier")


class VIEW3D_MT_vertex_group(Menu):
    bl_label = "Vertex Groups"

    def draw(self, context):
        layout = self.layout

        layout.operator_context = 'EXEC_AREA'
        layout.operator("object.vertex_group_assign_new")

        ob = context.active_object
        if ob.mode == 'EDIT':
            if ob.vertex_groups.active:
                layout.separator()

                layout.operator("object.vertex_group_assign", text="Assign to Active Group")
                layout.operator(
                    "object.vertex_group_remove_from",
                    text="Remove from Active Group",
                ).use_all_groups = False
                layout.operator("object.vertex_group_remove_from", text="Remove from All").use_all_groups = True

        if ob.vertex_groups.active:
            layout.separator()

            layout.operator_menu_enum("object.vertex_group_set_active", "group", text="Set Active Group")
            layout.operator("object.vertex_group_remove", text="Remove Active Group").all = False
            layout.operator("object.vertex_group_remove", text="Remove All Groups").all = True


class VIEW3D_MT_vertex_group_lock(Menu):
    bl_label = "Vertex Group Locks"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("object.vertex_group_lock", icon='LOCKED', text="Lock All")
        props.action = 'LOCK'

        layout.separator()

        props = layout.operator("object.vertex_group_lock", icon='UNLOCKED', text="Unlock All")
        props.action = 'UNLOCK'

        layout.separator()

        props = layout.operator("object.vertex_group_lock", icon='ARROW_LEFTRIGHT', text="Invert Locks")
        props.action = 'INVERT'


class VIEW3D_MT_vertex_group_weights(Menu):
    bl_label = "Weights"

    @staticmethod
    def draw_generic(layout, is_editmode=False):
        layout.operator("object.vertex_group_normalize_all", text="Normalize All")
        layout.operator("object.vertex_group_normalize", text="Normalize")

        layout.separator()

        # Using default context for 'flipping along axis', to differentiate from 'symmetrizing' (i.e.
        # 'mirrored copy').
        # See https://projects.blender.org/blender/blender/issues/43295#issuecomment-1400465
        layout.operator("object.vertex_group_mirror", text="Mirror", text_ctxt=i18n_contexts.default)
        layout.operator("object.vertex_group_invert", text="Invert")
        layout.operator("object.vertex_group_clean", text="Clean")

        layout.separator()

        layout.operator("object.vertex_group_quantize", text="Quantize")
        layout.operator("object.vertex_group_levels", text="Levels")
        layout.operator("object.vertex_group_smooth", text="Smooth")
        layout.operator("object.vertex_group_limit_total", text="Limit Total")

        if not is_editmode:
            props = layout.operator("object.data_transfer", text="Transfer Weights")
            props.use_reverse_transfer = True
            props.data_type = 'VGROUP_WEIGHTS'

        layout.separator()

        layout.menu("VIEW3D_MT_vertex_group_lock", text="Locks")

    def draw(self, _context):
        self.draw_generic(self.layout, is_editmode=False)


class VIEW3D_MT_sculpt(Menu):
    bl_label = "Sculpt"

    def draw(self, context):
        layout = self.layout

        layout.menu("VIEW3D_MT_sculpt_transform", text="Transform")

        layout.separator()

        props = layout.operator("sculpt.face_set_change_visibility", text="Toggle Visibility")
        props.mode = 'TOGGLE'

        props = layout.operator("sculpt.face_set_change_visibility", text="Hide Active Face Set")
        props.mode = 'HIDE_ACTIVE'

        props = layout.operator("paint.hide_show_all", text="Show All")
        props.action = 'SHOW'

        layout.operator("paint.visibility_invert", text="Invert Visible")

        props = layout.operator("paint.hide_show_masked", text="Hide Masked")
        props.action = 'HIDE'

        props = layout.operator("paint.visibility_filter", text="Grow Visibility")
        props.action = 'GROW'

        props = layout.operator("paint.visibility_filter", text="Shrink Visibility")
        props.action = 'SHRINK'

        layout.menu("VIEW3D_MT_sculpt_showhide", text="Show/Hide")

        layout.separator()

        # Fair Positions
        props = layout.operator("sculpt.face_set_edit", text="Fair Positions")
        props.mode = 'FAIR_POSITIONS'

        # Fair Tangency
        props = layout.operator("sculpt.face_set_edit", text="Fair Tangency")
        props.mode = 'FAIR_TANGENCY'

        # Project
        layout.operator("sculpt.project_line_gesture", text="Line Project")

        # Trim/Add
        layout.menu("VIEW3D_MT_sculpt_trim", text="Trim/Add")

        layout.separator()

        layout.menu("VIEW3D_MT_add_object", text="Add Primitive")

        layout.separator()

        sculpt_filters_types = [
            ('SMOOTH', iface_("Smooth", i18n_contexts.operator_default)),
            ('SURFACE_SMOOTH', iface_("Surface Smooth")),
            ('INFLATE', iface_("Inflate")),
            ('RELAX', iface_("Relax Topology")),
            ('RELAX_FACE_SETS', iface_("Relax Face Sets")),
            ('SHARPEN', iface_("Sharpen")),
            ('ENHANCE_DETAILS', iface_("Enhance Details")),
            ('ERASE_DISPLACEMENT', iface_("Erase Multires Displacement")),
            ('RANDOM', iface_("Randomize")),
        ]

        for filter_type, ui_name in sculpt_filters_types:
            props = layout.operator("sculpt.mesh_filter", text=ui_name, translate=False)
            props.type = filter_type

        layout.separator()

        props = layout.operator("sculpt.color_filter", text="Set Vertex Color")
        props.type = 'FILL'
        props.strength = 1.0
        props.use_immediate = True
        layout.operator("paint.sample_color", text="Sample Color")

        layout.separator()

        layout.menu("VIEW3D_MT_sculpt_set_pivot", text="Set Pivot")

        layout.separator()

        # Rebuild BVH
        layout.operator("sculpt.optimize")

        layout.operator(
            "sculpt.dynamic_topology_toggle", text="Dynamic Topology",
            icon='CHECKBOX_HLT' if context.sculpt_object.use_dynamic_topology_sculpting else 'CHECKBOX_DEHLT',
        )

        layout.separator()

        layout.operator("object.transfer_mode", text="Transfer Sculpt Mode")


class VIEW3D_MT_sculpt_transform(Menu):
    bl_label = "Transform"

    def draw(self, _context):
        layout = self.layout

        layout.operator("transform.translate")
        layout.operator("transform.rotate")
        layout.operator("transform.resize", text="Scale")

        layout.separator()
        props = layout.operator("sculpt.mesh_filter", text="To Sphere")
        props.type = 'SPHERE'


class VIEW3D_MT_sculpt_showhide(Menu):
    bl_label = "Show/Hide"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("paint.hide_show", text="Box Hide")
        props.action = 'HIDE'

        props = layout.operator("paint.hide_show_lasso_gesture", text="Lasso Hide")
        props.action = 'HIDE'

        props = layout.operator("paint.hide_show_line_gesture", text="Line Hide")
        props.action = 'HIDE'

        props = layout.operator("paint.hide_show_polyline_gesture", text="Polyline Hide")
        props.action = 'HIDE'

        layout.separator()

        props = layout.operator("paint.hide_show", text="Box Show")
        props.action = 'SHOW'

        props = layout.operator("paint.hide_show_lasso_gesture", text="Lasso Show")
        props.action = 'SHOW'

        props = layout.operator("paint.hide_show_line_gesture", text="Line Show")
        props.action = 'SHOW'

        props = layout.operator("paint.hide_show_polyline_gesture", text="Polyline Show")
        props.action = 'SHOW'


class VIEW3D_MT_sculpt_trim(Menu):
    bl_label = "Trim/Add"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("sculpt.trim_box_gesture", text="Box Trim")
        props.trim_mode = 'DIFFERENCE'

        props = layout.operator("sculpt.trim_lasso_gesture", text="Lasso Trim")
        props.trim_mode = 'DIFFERENCE'

        props = layout.operator("sculpt.trim_line_gesture", text="Line Trim")
        props.trim_mode = 'DIFFERENCE'

        props = layout.operator("sculpt.trim_polyline_gesture", text="Polyline Trim")
        props.trim_mode = 'DIFFERENCE'

        layout.separator()

        props = layout.operator("sculpt.trim_box_gesture", text="Box Add")
        props.trim_mode = 'JOIN'

        props = layout.operator("sculpt.trim_lasso_gesture", text="Lasso Add")
        props.trim_mode = 'JOIN'

        props = layout.operator("sculpt.trim_polyline_gesture", text="Polyline Add")
        props.trim_mode = 'JOIN'


class VIEW3D_MT_add_object(Menu):
    bl_label = "Add Primitive"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("wm.tool_set_by_id", text="Add Cube")
        props.name = "builtin.primitive_cube_add"
        props.space_type = 'VIEW_3D'

        props = layout.operator("wm.tool_set_by_id", text="Add Cylinder")
        props.name = "builtin.primitive_cylinder_add"
        props.space_type = 'VIEW_3D'

        props = layout.operator("wm.tool_set_by_id", text="Add Cone")
        props.name = "builtin.primitive_cone_add"
        props.space_type = 'VIEW_3D'

        props = layout.operator("wm.tool_set_by_id", text="Add UV Sphere")
        props.name = "builtin.primitive_uv_sphere_add"
        props.space_type = 'VIEW_3D'

        props = layout.operator("wm.tool_set_by_id", text="Add Ico Sphere")
        props.name = "builtin.primitive_ico_sphere_add"
        props.space_type = 'VIEW_3D'


class VIEW3D_MT_sculpt_curves(Menu):
    bl_label = "Curves"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curves.snap_curves_to_surface", text="Snap to Deformed Surface").attach_mode = 'DEFORM'
        layout.operator("curves.snap_curves_to_surface", text="Snap to Nearest Surface").attach_mode = 'NEAREST'
        layout.separator()
        layout.operator("curves.convert_to_particle_system", text="Convert to Particle System")

        layout.template_node_operator_asset_menu_items(catalog_path="Curves")


class VIEW3D_MT_mask(Menu):
    bl_label = "Mask"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("paint.mask_flood_fill", text="Fill Mask")
        props.mode = 'VALUE'
        props.value = 1

        props = layout.operator("paint.mask_flood_fill", text="Clear Mask")
        props.mode = 'VALUE'
        props.value = 0

        props = layout.operator("paint.mask_flood_fill", text="Invert Mask")
        props.mode = 'INVERT'

        layout.separator()

        props = layout.operator("paint.mask_box_gesture", text="Box Mask")
        props.mode = 'VALUE'
        props.value = 0

        props = layout.operator("paint.mask_lasso_gesture", text="Lasso Mask")
        props = layout.operator("paint.mask_line_gesture", text="Line Mask")
        props = layout.operator("paint.mask_polyline_gesture", text="Polyline Mask")

        layout.separator()

        props = layout.operator("sculpt.mask_filter", text="Smooth Mask")
        props.filter_type = 'SMOOTH'

        props = layout.operator("sculpt.mask_filter", text="Sharpen Mask")
        props.filter_type = 'SHARPEN'

        props = layout.operator("sculpt.mask_filter", text="Grow Mask")
        props.filter_type = 'GROW'

        props = layout.operator("sculpt.mask_filter", text="Shrink Mask")
        props.filter_type = 'SHRINK'

        props = layout.operator("sculpt.mask_filter", text="Increase Contrast")
        props.filter_type = 'CONTRAST_INCREASE'
        props.auto_iteration_count = False

        props = layout.operator("sculpt.mask_filter", text="Decrease Contrast")
        props.filter_type = 'CONTRAST_DECREASE'
        props.auto_iteration_count = False

        layout.separator()

        props = layout.operator("sculpt.expand", text="Expand Mask by Topology")
        props.target = 'MASK'
        props.falloff_type = 'GEODESIC'
        props.invert = False
        props.use_auto_mask = False
        props.use_mask_preserve = True

        props = layout.operator("sculpt.expand", text="Expand Mask by Normals")
        props.target = 'MASK'
        props.falloff_type = 'NORMALS'
        props.invert = False
        props.use_mask_preserve = True

        layout.separator()

        props = layout.operator("sculpt.paint_mask_extract", text="Mask Extract")

        layout.separator()

        props = layout.operator("sculpt.paint_mask_slice", text="Mask Slice")
        props.fill_holes = False
        props.new_object = False
        props = layout.operator("sculpt.paint_mask_slice", text="Mask Slice and Fill Holes")
        props.new_object = False
        props = layout.operator("sculpt.paint_mask_slice", text="Mask Slice to New Object")

        layout.separator()

        props = layout.operator("sculpt.mask_from_cavity", text="Mask from Cavity")
        props.settings_source = 'OPERATOR'

        props = layout.operator("sculpt.mask_from_boundary", text="Mask from Mesh Boundary")
        props.settings_source = 'OPERATOR'
        props.boundary_mode = 'MESH'

        props = layout.operator("sculpt.mask_from_boundary", text="Mask from Face Sets Boundary")
        props.settings_source = 'OPERATOR'
        props.boundary_mode = 'FACE_SETS'

        props = layout.operator("sculpt.mask_by_color", text="Mask by Color")

        layout.separator()

        layout.menu("VIEW3D_MT_random_mask", text="Random Mask")

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_face_sets(Menu):
    bl_label = "Face Sets"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("sculpt.face_sets_create", text="Face Set from Masked")
        props.mode = 'MASKED'

        props = layout.operator("sculpt.face_sets_create", text="Face Set from Visible")
        props.mode = 'VISIBLE'

        props = layout.operator("sculpt.face_sets_create", text="Face Set from Edit Mode Selection")
        props.mode = 'SELECTION'

        layout.separator()

        layout.menu("VIEW3D_MT_face_sets_init", text="Initialize Face Sets")

        layout.separator()

        props = layout.operator("sculpt.face_set_edit", text="Grow Face Set")
        props.mode = 'GROW'

        props = layout.operator("sculpt.face_set_edit", text="Shrink Face Set")
        props.mode = 'SHRINK'

        layout.separator()

        props = layout.operator("sculpt.expand", text="Expand Face Set by Topology")
        props.target = 'FACE_SETS'
        props.falloff_type = 'GEODESIC'
        props.invert = False
        props.use_mask_preserve = False
        props.use_modify_active = False

        props = layout.operator("sculpt.expand", text="Expand Active Face Set")
        props.target = 'FACE_SETS'
        props.falloff_type = 'BOUNDARY_FACE_SET'
        props.invert = False
        props.use_mask_preserve = False
        props.use_modify_active = True

        layout.separator()

        props = layout.operator("sculpt.face_set_extract", text="Extract Face Set")

        layout.separator()

        props = layout.operator("sculpt.face_sets_randomize_colors", text="Randomize Colors")

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_sculpt_set_pivot(Menu):
    bl_label = "Sculpt Set Pivot"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("sculpt.set_pivot_position", text="Pivot to Origin")
        props.mode = 'ORIGIN'

        props = layout.operator("sculpt.set_pivot_position", text="Pivot to Unmasked")
        props.mode = 'UNMASKED'

        props = layout.operator("sculpt.set_pivot_position", text="Pivot to Mask Border")
        props.mode = 'BORDER'

        props = layout.operator("sculpt.set_pivot_position", text="Pivot to Active Vertex")
        props.mode = 'ACTIVE'

        props = layout.operator("sculpt.set_pivot_position", text="Pivot to Surface Under Cursor")
        props.mode = 'SURFACE'


class VIEW3D_MT_face_sets_init(Menu):
    bl_label = "Face Sets Init"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("sculpt.face_sets_init", text="By Loose Parts")
        props.mode = 'LOOSE_PARTS'

        props = layout.operator("sculpt.face_sets_init", text="By Face Set Boundaries")
        props.mode = 'FACE_SET_BOUNDARIES'

        props = layout.operator("sculpt.face_sets_init", text="By Materials")
        props.mode = 'MATERIALS'

        props = layout.operator("sculpt.face_sets_init", text="By Normals")
        props.mode = 'NORMALS'

        props = layout.operator("sculpt.face_sets_init", text="By UV Seams")
        props.mode = 'UV_SEAMS'

        props = layout.operator("sculpt.face_sets_init", text="By Edge Creases")
        props.mode = 'CREASES'

        props = layout.operator("sculpt.face_sets_init", text="By Edge Bevel Weight")
        props.mode = 'BEVEL_WEIGHT'

        props = layout.operator("sculpt.face_sets_init", text="By Sharp Edges")
        props.mode = 'SHARP_EDGES'


class VIEW3D_MT_random_mask(Menu):
    bl_label = "Random Mask"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("sculpt.mask_init", text="Per Vertex")
        props.mode = 'RANDOM_PER_VERTEX'

        props = layout.operator("sculpt.mask_init", text="Per Face Set")
        props.mode = 'RANDOM_PER_FACE_SET'

        props = layout.operator("sculpt.mask_init", text="Per Loose Part")
        props.mode = 'RANDOM_PER_LOOSE_PART'


class VIEW3D_MT_particle(Menu):
    bl_label = "Particle"

    def draw(self, context):
        layout = self.layout
        tool_settings = context.tool_settings

        particle_edit = tool_settings.particle_edit

        layout.operator("particle.mirror")

        layout.operator("particle.remove_doubles")

        layout.separator()

        if particle_edit.select_mode == 'POINT':
            layout.operator("particle.subdivide")

        layout.operator("particle.unify_length")
        layout.operator("particle.rekey")
        layout.operator("particle.weight_set")

        layout.separator()

        layout.menu("VIEW3D_MT_particle_showhide")

        layout.separator()

        layout.operator("particle.delete")


class VIEW3D_MT_particle_context_menu(Menu):
    bl_label = "Particle"

    def draw(self, context):
        layout = self.layout
        tool_settings = context.tool_settings

        particle_edit = tool_settings.particle_edit

        layout.operator("particle.rekey")

        layout.separator()

        layout.operator("particle.delete")

        layout.separator()

        layout.operator("particle.remove_doubles")
        layout.operator("particle.unify_length")

        if particle_edit.select_mode == 'POINT':
            layout.operator("particle.subdivide")

        layout.operator("particle.weight_set")

        layout.separator()

        layout.operator("particle.mirror")

        if particle_edit.select_mode == 'POINT':
            layout.separator()

            layout.operator("particle.select_all", text="All").action = 'SELECT'
            layout.operator("particle.select_all", text="None").action = 'DESELECT'
            layout.operator("particle.select_all", text="Invert").action = 'INVERT'

            layout.separator()

            layout.operator("particle.select_roots")
            layout.operator("particle.select_tips")

            layout.separator()

            layout.operator("particle.select_random")

            layout.separator()

            layout.operator("particle.select_more")
            layout.operator("particle.select_less")

            layout.separator()

            layout.operator("particle.select_linked", text="Select Linked")


class VIEW3D_MT_particle_showhide(ShowHideMenu, Menu):
    _operator_name = "particle"


# ********** Edit Menus, suffix from ob.type **********


class VIEW3D_MT_edit_mesh(Menu):
    bl_label = "Mesh"

    def draw(self, _context):
        layout = self.layout

        with_bullet = bpy.app.build_options.bullet

        layout.menu("VIEW3D_MT_transform")
        layout.menu("VIEW3D_MT_mirror")
        layout.menu("VIEW3D_MT_snap")

        layout.separator()

        layout.operator("mesh.duplicate_move", text="Duplicate")
        layout.menu("VIEW3D_MT_edit_mesh_extrude")

        layout.separator()

        layout.menu("VIEW3D_MT_edit_mesh_merge", text="Merge")
        layout.menu("VIEW3D_MT_edit_mesh_split", text="Split")
        layout.operator_menu_enum("mesh.separate", "type")

        layout.separator()

        layout.operator("mesh.bisect")
        layout.operator("mesh.knife_project")
        props = layout.operator("mesh.knife_tool")
        props.use_occlude_geometry = True
        props.only_selected = False

        if with_bullet:
            layout.operator("mesh.convex_hull")

        layout.separator()

        layout.operator("mesh.symmetrize")
        layout.operator("mesh.symmetry_snap")

        layout.separator()

        layout.menu("VIEW3D_MT_edit_mesh_normals")
        layout.menu("VIEW3D_MT_edit_mesh_shading")
        layout.menu("VIEW3D_MT_edit_mesh_weights")
        layout.operator("mesh.attribute_set")
        layout.operator_menu_enum("mesh.sort_elements", "type", text="Sort Elements")

        layout.separator()

        layout.menu("VIEW3D_MT_edit_mesh_showhide")
        layout.menu("VIEW3D_MT_edit_mesh_clean")

        layout.separator()

        layout.menu("VIEW3D_MT_edit_mesh_delete")

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_edit_mesh_context_menu(Menu):
    bl_label = ""

    def draw(self, context):

        def count_selected_items_for_objects_in_mode():
            selected_verts_len = 0
            selected_edges_len = 0
            selected_faces_len = 0
            for ob in context.objects_in_mode_unique_data:
                v, e, f = ob.data.count_selected_items()
                selected_verts_len += v
                selected_edges_len += e
                selected_faces_len += f
            return (selected_verts_len, selected_edges_len, selected_faces_len)

        is_vert_mode, is_edge_mode, is_face_mode = context.tool_settings.mesh_select_mode
        selected_verts_len, selected_edges_len, selected_faces_len = count_selected_items_for_objects_in_mode()

        del count_selected_items_for_objects_in_mode

        layout = self.layout

        with_freestyle = bpy.app.build_options.freestyle

        layout.operator_context = 'INVOKE_REGION_WIN'

        # If nothing is selected
        # (disabled for now until it can be made more useful).
        '''
        # If nothing is selected
        if not (selected_verts_len or selected_edges_len or selected_faces_len):
            layout.menu("VIEW3D_MT_mesh_add", text="Add", text_ctxt=i18n_contexts.operator_default)

            return
        '''

        # Else something is selected

        row = layout.row()

        if is_vert_mode:
            col = row.column(align=True)

            col.label(text="Vertex", icon='VERTEXSEL')
            col.separator()

            # Additive Operators
            col.operator("mesh.subdivide", text="Subdivide")

            col.separator()

            col.operator("mesh.extrude_vertices_move", text="Extrude Vertices")
            col.operator("mesh.bevel", text="Bevel Vertices").affect = 'VERTICES'

            if selected_verts_len > 1:
                col.separator()
                col.operator("mesh.edge_face_add", text="New Edge/Face from Vertices")
                col.operator("mesh.vert_connect_path", text="Connect Vertex Path")
                col.operator("mesh.vert_connect", text="Connect Vertex Pairs")

            col.separator()

            # Deform Operators
            col.operator("transform.push_pull", text="Push/Pull")
            col.operator("transform.shrink_fatten", text="Shrink/Fatten")
            col.operator("transform.shear", text="Shear")
            col.operator("transform.vert_slide", text="Slide Vertices")
            col.operator_context = 'EXEC_REGION_WIN'
            col.operator("transform.vertex_random", text="Randomize Vertices").offset = 0.1
            col.operator("mesh.vertices_smooth", text="Smooth Vertices").factor = 0.5
            col.operator_context = 'INVOKE_REGION_WIN'
            col.operator("mesh.vertices_smooth_laplacian", text="Smooth Laplacian")

            col.separator()

            col.menu("VIEW3D_MT_mirror", text="Mirror Vertices")
            col.menu("VIEW3D_MT_snap", text="Snap Vertices")

            col.separator()

            col.operator("transform.vert_crease", icon='VERTEX_CREASE')

            col.separator()

            # Removal Operators
            if selected_verts_len > 1:
                col.menu("VIEW3D_MT_edit_mesh_merge", text="Merge Vertices")
            col.operator("mesh.split")
            col.operator_menu_enum("mesh.separate", "type")
            col.operator("mesh.dissolve_verts")
            col.operator("mesh.delete", text="Delete Vertices").type = 'VERT'

        if is_edge_mode:
            col = row.column(align=True)
            col.label(text="Edge", icon='EDGESEL')
            col.separator()

            # Additive Operators
            col.operator("mesh.subdivide", text="Subdivide")

            col.separator()

            col.operator("mesh.extrude_edges_move", text="Extrude Edges")
            col.operator("mesh.bevel", text="Bevel Edges").affect = 'EDGES'
            if selected_edges_len >= 2:
                col.operator("mesh.bridge_edge_loops")
            if selected_edges_len >= 1:
                col.operator("mesh.edge_face_add", text="New Face from Edges")
            if selected_edges_len >= 2:
                col.operator("mesh.fill")

            col.separator()

            props = col.operator("mesh.loopcut_slide")
            props.TRANSFORM_OT_edge_slide.release_confirm = False
            col.operator("mesh.offset_edge_loops_slide")

            col.separator()

            col.operator("mesh.knife_tool")
            col.operator("mesh.bisect")

            col.separator()

            # Deform Operators
            col.operator("mesh.edge_rotate", text="Rotate Edge CW").use_ccw = False
            col.operator("transform.edge_slide")
            col.operator("mesh.edge_split")

            col.separator()

            # Edge Flags
            col.operator("transform.edge_bevelweight", icon='EDGE_BEVEL')
            col.operator("transform.edge_crease", icon='EDGE_CREASE')

            col.separator()

            col.operator("mesh.mark_seam", icon='EDGE_SEAM').clear = False
            col.operator("mesh.mark_seam", text="Clear Seam").clear = True

            col.separator()

            col.operator("mesh.mark_sharp", icon='EDGE_SHARP').clear = False
            col.operator("mesh.mark_sharp", text="Clear Sharp").clear = True
            col.operator("mesh.set_sharpness_by_angle")

            if with_freestyle:
                col.separator()

                col.operator("mesh.mark_freestyle_edge").clear = False
                col.operator("mesh.mark_freestyle_edge", text="Clear Freestyle Edge").clear = True

            col.separator()

            # Removal Operators
            col.operator("mesh.unsubdivide")
            col.operator("mesh.split")
            col.operator_menu_enum("mesh.separate", "type")
            col.operator("mesh.dissolve_edges")
            col.operator("mesh.delete", text="Delete Edges").type = 'EDGE'

        if is_face_mode:
            col = row.column(align=True)

            col.label(text="Face", icon='FACESEL')
            col.separator()

            # Additive Operators
            col.operator("mesh.subdivide", text="Subdivide")

            col.separator()

            col.operator("view3d.edit_mesh_extrude_move_normal", text="Extrude Faces")
            col.operator("view3d.edit_mesh_extrude_move_shrink_fatten", text="Extrude Faces Along Normals")
            col.operator("mesh.extrude_faces_move", text="Extrude Individual Faces")

            col.operator("mesh.inset")
            col.operator("mesh.poke")

            if selected_faces_len >= 2:
                col.operator("mesh.bridge_edge_loops", text="Bridge Faces")

            col.separator()

            # Modify Operators
            col.menu("VIEW3D_MT_uv_map", text="UV Unwrap Faces")

            col.separator()

            props = col.operator("mesh.quads_convert_to_tris")
            props.quad_method = props.ngon_method = 'BEAUTY'
            col.operator("mesh.tris_convert_to_quads")

            col.separator()

            col.operator("mesh.faces_shade_smooth")
            col.operator("mesh.faces_shade_flat")

            col.separator()

            # Removal Operators
            col.operator("mesh.unsubdivide")
            col.operator("mesh.split")
            col.operator_menu_enum("mesh.separate", "type")
            col.operator("mesh.dissolve_faces")
            col.operator("mesh.delete", text="Delete Faces").type = 'FACE'


class VIEW3D_MT_edit_mesh_select_mode(Menu):
    bl_label = "Mesh Select Mode"

    def draw(self, _context):
        layout = self.layout

        layout.operator_context = 'INVOKE_REGION_WIN'
        layout.operator("mesh.select_mode", text="Vertex", icon='VERTEXSEL').type = 'VERT'
        layout.operator("mesh.select_mode", text="Edge", icon='EDGESEL').type = 'EDGE'
        layout.operator("mesh.select_mode", text="Face", icon='FACESEL').type = 'FACE'


class VIEW3D_MT_edit_mesh_extrude(Menu):
    bl_label = "Extrude"

    def draw(self, context):
        from math import pi

        layout = self.layout
        layout.operator_context = 'INVOKE_REGION_WIN'

        tool_settings = context.tool_settings
        select_mode = tool_settings.mesh_select_mode
        ob = context.object
        mesh = ob.data

        if mesh.total_face_sel:
            layout.operator("view3d.edit_mesh_extrude_move_normal", text="Extrude Faces")
            layout.operator("view3d.edit_mesh_extrude_move_shrink_fatten", text="Extrude Faces Along Normals")
            layout.operator("mesh.extrude_faces_move", text="Extrude Individual Faces")
            layout.operator("view3d.edit_mesh_extrude_manifold_normal", text="Extrude Manifold")

        if mesh.total_edge_sel and (select_mode[0] or select_mode[1]):
            layout.operator("mesh.extrude_edges_move", text="Extrude Edges")

        if mesh.total_vert_sel and select_mode[0]:
            layout.operator("mesh.extrude_vertices_move", text="Extrude Vertices")

        layout.separator()

        layout.operator("mesh.extrude_repeat")
        layout.operator("mesh.spin").angle = pi * 2
        layout.template_node_operator_asset_menu_items(catalog_path="Mesh/Extrude")


class VIEW3D_MT_edit_mesh_vertices(Menu):
    bl_label = "Vertex"

    def draw(self, _context):
        layout = self.layout
        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("mesh.extrude_vertices_move", text="Extrude Vertices")
        layout.operator("mesh.dupli_extrude_cursor").rotate_source = True
        layout.operator("mesh.bevel", text="Bevel Vertices").affect = 'VERTICES'

        layout.separator()

        layout.operator("mesh.edge_face_add", text="New Edge/Face from Vertices")
        layout.operator("mesh.vert_connect_path", text="Connect Vertex Path")
        layout.operator("mesh.vert_connect", text="Connect Vertex Pairs")

        layout.separator()

        props = layout.operator("mesh.rip_move", text="Rip Vertices")
        props.MESH_OT_rip.use_fill = False
        props = layout.operator("mesh.rip_move", text="Rip Vertices and Fill")
        props.MESH_OT_rip.use_fill = True
        layout.operator("mesh.rip_edge_move", text="Rip Vertices and Extend")

        layout.separator()

        layout.operator("transform.vert_slide", text="Slide Vertices")
        layout.operator_context = 'EXEC_REGION_WIN'
        layout.operator("mesh.vertices_smooth", text="Smooth Vertices").factor = 0.5
        layout.operator("mesh.vertices_smooth_laplacian", text="Smooth Vertices (Laplacian)")
        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.separator()

        layout.operator("transform.vert_crease", icon='VERTEX_CREASE')

        layout.separator()

        layout.operator("mesh.blend_from_shape")
        layout.operator("mesh.shape_propagate_to_all", text="Propagate to Shapes")

        layout.separator()

        layout.menu("VIEW3D_MT_vertex_group")
        layout.menu("VIEW3D_MT_hook")

        layout.separator()

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_edit_mesh_edges(Menu):
    bl_label = "Edge"

    def draw(self, _context):
        layout = self.layout

        with_freestyle = bpy.app.build_options.freestyle

        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("mesh.extrude_edges_move", text="Extrude Edges")
        layout.operator("mesh.bevel", text="Bevel Edges").affect = 'EDGES'
        layout.operator("mesh.bridge_edge_loops")
        layout.operator("mesh.screw")

        layout.separator()

        layout.operator("mesh.subdivide")
        layout.operator("mesh.subdivide_edgering")
        layout.operator("mesh.unsubdivide")

        layout.separator()

        layout.operator("mesh.edge_rotate", text="Rotate Edge CW").use_ccw = False
        layout.operator("mesh.edge_rotate", text="Rotate Edge CCW").use_ccw = True

        layout.separator()

        layout.operator("transform.edge_slide")
        props = layout.operator("mesh.loopcut_slide")
        props.TRANSFORM_OT_edge_slide.release_confirm = False
        layout.operator("mesh.offset_edge_loops_slide")

        layout.separator()

        layout.operator("transform.edge_bevelweight", icon='EDGE_BEVEL')
        layout.operator("transform.edge_crease", icon='EDGE_CREASE')

        layout.separator()

        layout.operator("mesh.mark_seam", icon='EDGE_SEAM').clear = False
        layout.operator("mesh.mark_seam", text="Clear Seam").clear = True

        layout.separator()

        layout.operator("mesh.mark_sharp", icon='EDGE_SHARP')
        layout.operator("mesh.mark_sharp", text="Clear Sharp").clear = True

        layout.operator("mesh.mark_sharp", text="Mark Sharp from Vertices").use_verts = True
        props = layout.operator("mesh.mark_sharp", text="Clear Sharp from Vertices")
        props.use_verts = True
        props.clear = True

        layout.operator("mesh.set_sharpness_by_angle")

        if with_freestyle:
            layout.separator()

            layout.operator("mesh.mark_freestyle_edge").clear = False
            layout.operator("mesh.mark_freestyle_edge", text="Clear Freestyle Edge").clear = True

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_edit_mesh_faces_data(Menu):
    bl_label = "Face Data"

    def draw(self, _context):
        layout = self.layout

        with_freestyle = bpy.app.build_options.freestyle

        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("mesh.colors_rotate")
        layout.operator("mesh.colors_reverse")

        layout.separator()

        layout.operator("mesh.uvs_rotate")
        layout.operator("mesh.uvs_reverse")

        layout.separator()

        layout.operator("mesh.flip_quad_tessellation")

        if with_freestyle:
            layout.separator()
            layout.operator("mesh.mark_freestyle_face").clear = False
            layout.operator("mesh.mark_freestyle_face", text="Clear Freestyle Face").clear = True
        layout.template_node_operator_asset_menu_items(catalog_path="Face/Face Data")


class VIEW3D_MT_edit_mesh_faces(Menu):
    bl_label = "Face"
    bl_idname = "VIEW3D_MT_edit_mesh_faces"

    def draw(self, context):
        layout = self.layout

        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("view3d.edit_mesh_extrude_move_normal", text="Extrude Faces")
        layout.operator("view3d.edit_mesh_extrude_move_shrink_fatten", text="Extrude Faces Along Normals")
        layout.operator("mesh.extrude_faces_move", text="Extrude Individual Faces")

        layout.separator()

        layout.operator("mesh.inset")
        layout.operator("mesh.poke")
        props = layout.operator("mesh.quads_convert_to_tris")
        props.quad_method = props.ngon_method = 'BEAUTY'
        layout.operator("mesh.tris_convert_to_quads")
        layout.operator("mesh.solidify", text="Solidify Faces")
        layout.operator("mesh.wireframe")

        layout.separator()

        layout.operator("mesh.fill")
        layout.operator("mesh.fill_grid")
        layout.operator("mesh.beautify_fill")

        layout.separator()

        layout.operator("mesh.intersect")
        layout.operator("mesh.intersect_boolean")

        layout.separator()

        layout.operator("mesh.face_split_by_edges")

        layout.separator()

        layout.operator("mesh.faces_shade_smooth")
        layout.operator("mesh.faces_shade_flat")

        layout.separator()

        layout.menu("VIEW3D_MT_edit_mesh_faces_data")

        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_edit_mesh_normals_select_strength(Menu):
    bl_label = "Select by Face Strength"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("mesh.mod_weighted_strength", text="Weak")
        props.set = False
        props.face_strength = 'WEAK'

        props = layout.operator("mesh.mod_weighted_strength", text="Medium")
        props.set = False
        props.face_strength = 'MEDIUM'

        props = layout.operator("mesh.mod_weighted_strength", text="Strong")
        props.set = False
        props.face_strength = 'STRONG'


class VIEW3D_MT_edit_mesh_normals_set_strength(Menu):
    bl_label = "Set Face Strength"

    def draw(self, _context):
        layout = self.layout

        props = layout.operator("mesh.mod_weighted_strength", text="Weak")
        props.set = True
        props.face_strength = 'WEAK'

        props = layout.operator("mesh.mod_weighted_strength", text="Medium")
        props.set = True
        props.face_strength = 'MEDIUM'

        props = layout.operator("mesh.mod_weighted_strength", text="Strong")
        props.set = True
        props.face_strength = 'STRONG'


class VIEW3D_MT_edit_mesh_normals_average(Menu):
    bl_label = "Average"

    def draw(self, _context):
        layout = self.layout

        layout.operator("mesh.average_normals", text="Custom Normal").average_type = 'CUSTOM_NORMAL'
        layout.operator("mesh.average_normals", text="Face Area").average_type = 'FACE_AREA'
        layout.operator("mesh.average_normals", text="Corner Angle").average_type = 'CORNER_ANGLE'


class VIEW3D_MT_edit_mesh_normals(Menu):
    bl_label = "Normals"

    def draw(self, _context):
        layout = self.layout

        layout.operator("mesh.flip_normals", text="Flip")
        layout.operator("mesh.normals_make_consistent", text="Recalculate Outside").inside = False
        layout.operator("mesh.normals_make_consistent", text="Recalculate Inside").inside = True

        layout.separator()

        layout.operator("mesh.set_normals_from_faces", text="Set from Faces")

        layout.operator_context = 'INVOKE_REGION_WIN'
        layout.operator("transform.rotate_normal", text="Rotate...")
        layout.operator("mesh.point_normals", text="Point to Target...")
        layout.operator_context = 'EXEC_REGION_WIN'

        layout.operator("mesh.merge_normals", text="Merge")
        layout.operator("mesh.split_normals", text="Split")
        layout.menu("VIEW3D_MT_edit_mesh_normals_average", text="Average", text_ctxt=i18n_contexts.id_mesh)

        layout.separator()

        layout.operator("mesh.normals_tools", text="Copy Vector").mode = 'COPY'
        layout.operator("mesh.normals_tools", text="Paste Vector").mode = 'PASTE'

        layout.operator("mesh.smooth_normals", text="Smooth Vectors")
        layout.operator("mesh.normals_tools", text="Reset Vectors").mode = 'RESET'

        layout.separator()

        layout.menu("VIEW3D_MT_edit_mesh_normals_select_strength")
        layout.menu("VIEW3D_MT_edit_mesh_normals_set_strength")
        layout.template_node_operator_asset_menu_items(catalog_path="Mesh/Normals")


class VIEW3D_MT_edit_mesh_shading(Menu):
    bl_label = "Shading"

    def draw(self, _context):
        layout = self.layout

        layout.operator("mesh.faces_shade_smooth", text="Smooth Faces")
        layout.operator("mesh.faces_shade_flat", text="Flat Faces")

        layout.separator()

        layout.operator("mesh.mark_sharp", text="Smooth Edges").clear = True
        layout.operator("mesh.mark_sharp", text="Sharp Edges")

        layout.separator()

        props = layout.operator("mesh.mark_sharp", text="Smooth Vertices")
        props.use_verts = True
        props.clear = True

        layout.operator("mesh.mark_sharp", text="Sharp Vertices").use_verts = True
        layout.template_node_operator_asset_menu_items(catalog_path="Mesh/Shading")


class VIEW3D_MT_edit_mesh_weights(Menu):
    bl_label = "Weights"

    def draw(self, _context):
        layout = self.layout
        VIEW3D_MT_vertex_group_weights.draw_generic(layout, is_editmode=True)
        layout.template_node_operator_asset_menu_items(catalog_path="Mesh/Weights")


class VIEW3D_MT_edit_mesh_clean(Menu):
    bl_label = "Clean Up"

    def draw(self, _context):
        layout = self.layout

        layout.operator("mesh.delete_loose")

        layout.separator()

        layout.operator("mesh.decimate")
        layout.operator("mesh.dissolve_degenerate")
        layout.operator("mesh.dissolve_limited")
        layout.operator("mesh.face_make_planar")

        layout.separator()

        layout.operator("mesh.vert_connect_nonplanar")
        layout.operator("mesh.vert_connect_concave")
        layout.operator("mesh.remove_doubles")
        layout.operator("mesh.fill_holes")

        layout.template_node_operator_asset_menu_items(catalog_path="Mesh/Clean Up")


class VIEW3D_MT_edit_mesh_delete(Menu):
    bl_label = "Delete"

    def draw(self, _context):
        layout = self.layout

        layout.operator_enum("mesh.delete", "type")

        layout.separator()

        layout.operator("mesh.dissolve_verts")
        layout.operator("mesh.dissolve_edges")
        layout.operator("mesh.dissolve_faces")

        layout.separator()

        layout.operator("mesh.dissolve_limited")

        layout.separator()

        layout.operator("mesh.edge_collapse")
        layout.operator("mesh.delete_edgeloop", text="Edge Loops")

        layout.template_node_operator_asset_menu_items(catalog_path="Mesh/Delete")


class VIEW3D_MT_edit_mesh_merge(Menu):
    bl_label = "Merge"

    def draw(self, _context):
        layout = self.layout

        layout.operator_enum("mesh.merge", "type")

        layout.separator()

        layout.operator("mesh.remove_doubles", text="By Distance")

        layout.template_node_operator_asset_menu_items(catalog_path="Mesh/Merge")


class VIEW3D_MT_edit_mesh_split(Menu):
    bl_label = "Split"

    def draw(self, _context):
        layout = self.layout

        layout.operator("mesh.split", text="Selection")

        layout.separator()

        layout.operator_enum("mesh.edge_split", "type")

        layout.template_node_operator_asset_menu_items(catalog_path="Mesh/Split")


class VIEW3D_MT_edit_mesh_showhide(ShowHideMenu, Menu):
    _operator_name = "mesh"


# Edit Curve
# draw_curve is used by VIEW3D_MT_edit_curve and VIEW3D_MT_edit_surface


def draw_curve(self, _context):
    layout = self.layout

    layout.menu("VIEW3D_MT_transform")
    layout.menu("VIEW3D_MT_mirror")
    layout.menu("VIEW3D_MT_snap")

    layout.separator()

    layout.operator("curve.spin")
    layout.operator("curve.duplicate_move")

    layout.separator()

    layout.operator("curve.split")
    layout.operator("curve.separate")

    layout.separator()

    layout.operator("curve.cyclic_toggle")
    layout.operator_menu_enum("curve.spline_type_set", "type")

    layout.separator()

    layout.menu("VIEW3D_MT_edit_curve_showhide")
    layout.menu("VIEW3D_MT_edit_curve_clean")
    layout.menu("VIEW3D_MT_edit_curve_delete")


class VIEW3D_MT_edit_curve(Menu):
    bl_label = "Curve"

    draw = draw_curve


class VIEW3D_MT_edit_curve_ctrlpoints(Menu):
    bl_label = "Control Points"

    def draw(self, context):
        layout = self.layout

        edit_object = context.edit_object

        if edit_object.type in {'CURVE', 'SURFACE'}:
            layout.operator("curve.extrude_move")
            layout.operator("curve.vertex_add")

            layout.separator()

            layout.operator("curve.make_segment")

            layout.separator()

            if edit_object.type == 'CURVE':
                layout.operator("transform.tilt")
                layout.operator("curve.tilt_clear")

                layout.separator()

                layout.operator_menu_enum("curve.handle_type_set", "type")
                layout.operator("curve.normals_make_consistent")

                layout.separator()

            layout.operator("curve.smooth")
            if edit_object.type == 'CURVE':
                layout.operator("curve.smooth_tilt")
                layout.operator("curve.smooth_radius")
                layout.operator("curve.smooth_weight")

            layout.separator()

        layout.menu("VIEW3D_MT_hook")

        layout.separator()


class VIEW3D_MT_edit_curve_segments(Menu):
    bl_label = "Segments"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curve.subdivide")
        layout.operator("curve.switch_direction")


class VIEW3D_MT_edit_curve_clean(Menu):
    bl_label = "Clean Up"

    def draw(self, context):
        layout = self.layout

        layout.operator("curve.decimate")

        edit_object = context.edit_object
        if edit_object and edit_object.type in {'CURVE', 'SURFACE'} and (
            getattr(edit_object.data, "is_native_nurbs", False)
            or getattr(edit_object.data, "has_tagged_nurbs", False)
        ):
            layout.separator()
            layout.operator("curve.dissolve_limited")


class VIEW3D_MT_edit_nurbs(Menu):
    bl_label = "NURBS"

    def draw(self, _context):
        layout = self.layout
        layout.menu("VIEW3D_MT_edit_nurbs_transform", text="Transform")
        row = layout.row()
        row.enabled = False
        row.menu("VIEW3D_MT_mirror")
        layout.menu("VIEW3D_MT_edit_nurbs_snap", text="Snap")
        layout.separator()
        layout.operator("curve.duplicate_move", text="Duplicate")
        layout.operator("curve.extrude_move", text="Extrude")
        layout.separator()
        layout.menu("VIEW3D_MT_edit_nurbs_merge", text="Merge")
        layout.operator("curve.split", text="Split")
        layout.operator("curve.separate", text="Separate")
        layout.separator()
        layout.operator("curve.subdivide")
        layout.operator("curve.loopcut_slide", text="Loop Cut and Slide")
        layout.operator("curve.bevel", text="Bevel Edges")
        layout.separator()
        for operator in ("mesh.bisect", "mesh.knife_project", "mesh.knife_tool", "mesh.convex_hull"):
            row = layout.row()
            row.enabled = False
            row.operator(operator)
        layout.separator()
        for operator in ("mesh.symmetrize", "mesh.symmetry_snap"):
            row = layout.row()
            row.enabled = False
            row.operator(operator)
        layout.separator()
        for menu in ("VIEW3D_MT_edit_mesh_normals", "VIEW3D_MT_edit_mesh_shading", "VIEW3D_MT_edit_mesh_weights"):
            row = layout.row()
            row.enabled = False
            row.menu(menu)
        row = layout.row()
        row.enabled = False
        row.operator("mesh.attribute_set")
        row = layout.row()
        row.enabled = False
        row.operator_menu_enum("mesh.sort_elements", "type", text="Sort Elements")
        layout.separator()
        row = layout.row()
        row.enabled = False
        row.menu("VIEW3D_MT_edit_curve_showhide")
        layout.menu("VIEW3D_MT_edit_nurbs_clean")
        layout.separator()
        layout.menu("VIEW3D_MT_edit_nurbs_delete")


class VIEW3D_MT_edit_nurbs_transform(Menu):
    bl_label = "Transform"

    def draw(self, _context):
        layout = self.layout
        layout.operator("transform.translate")
        layout.operator("transform.rotate")
        layout.operator("transform.resize", text="Scale")
        layout.operator("transform.vert_slide", text="Vertex Slide")
        layout.separator()
        for operator in ("transform.tosphere", "transform.shear", "transform.bend", "transform.push_pull"):
            row = layout.row()
            row.enabled = False
            row.operator(operator)


class VIEW3D_MT_edit_nurbs_snap(Menu):
    bl_label = "Snap"

    def draw(self, _context):
        layout = self.layout
        row = layout.column()
        row.enabled = False
        row.operator("view3d.snap_selected_to_grid", text="Selection to Grid")
        row.operator("view3d.snap_selected_to_cursor", text="Selection to Cursor").use_offset = False
        row.operator("view3d.snap_selected_to_cursor", text="Selection to Cursor (Keep Offset)").use_offset = True
        row.operator("view3d.snap_selected_to_active", text="Selection to Active")
        layout.separator()
        row = layout.row()
        row.enabled = False
        row.operator("view3d.snap_cursor_to_selected", text="Cursor to Selected")
        layout.operator("view3d.snap_cursor_to_center", text="Cursor to World Origin")
        layout.operator("view3d.snap_cursor_to_grid", text="Cursor to Grid")
        row = layout.row()
        row.enabled = False
        row.operator("view3d.snap_cursor_to_active", text="Cursor to Active")


class VIEW3D_MT_edit_nurbs_clean(Menu):
    bl_label = "Clean Up"

    def draw(self, _context):
        layout = self.layout
        row = layout.row()
        row.enabled = False
        row.operator("mesh.delete_loose")
        layout.separator()
        row = layout.column()
        row.enabled = False
        row.operator("mesh.decimate")
        row.operator("mesh.dissolve_degenerate")
        layout.operator("curve.dissolve_limited")
        row = layout.row()
        row.enabled = False
        row.operator("mesh.face_make_planar")
        layout.separator()
        row = layout.column()
        row.enabled = False
        row.operator("mesh.vert_connect_nonplanar")
        row.operator("mesh.vert_connect_concave")
        layout.operator("curve.remove_doubles", text="Merge by Distance")
        row = layout.row()
        row.enabled = False
        row.operator("mesh.fill_holes")


class VIEW3D_MT_edit_nurbs_delete(Menu):
    bl_label = "Delete"

    def draw(self, _context):
        layout = self.layout
        layout.operator_enum("curve.delete", "type")
        layout.separator()
        layout.operator("curve.dissolve_limited")


class VIEW3D_MT_edit_nurbs_merge(Menu):
    bl_label = "Merge"

    @classmethod
    def poll(cls, context):
        obj = context.edit_object
        return (obj is not None and obj.type in {'CURVE', 'SURFACE'} and
                (obj.data.is_native_nurbs or obj.data.has_tagged_nurbs))

    def draw(self, _context):
        layout = self.layout
        layout.operator_enum("curve.merge", "type")
        layout.separator()
        layout.operator("curve.remove_doubles", text="By Distance")


class VIEW3D_MT_edit_curve_context_menu(Menu):
    bl_label = "Curve"

    def draw(self, context):
        # TODO(campbell): match mesh vertex menu.

        layout = self.layout
        edit_object = context.edit_object
        if edit_object and getattr(edit_object.data, "is_native_nurbs", False):
            layout.operator_context = 'INVOKE_DEFAULT'
            layout.operator("curve.make_segment", text="Make Edge/Face")
            layout.operator("curve.extrude_move")
            layout.operator("curve.duplicate_move")
            layout.operator("curve.split", text="Split Selection")
            layout.operator("curve.separate", text="Separate Selection")
            layout.operator("curve.rip_move", text="Rip")
            layout.menu("VIEW3D_MT_edit_nurbs_merge", text="Merge")
            layout.operator("curve.subdivide")
            layout.operator("curve.loopcut_slide", text="Loop Cut and Slide")
            layout.operator("curve.bevel", text="Bevel Edges")
            layout.separator()
            layout.operator("transform.translate")
            layout.operator("transform.rotate")
            layout.operator("transform.resize", text="Scale")
            layout.separator()
            layout.menu_contents("VIEW3D_MT_select_edit_nurbs")
            return

        layout.operator_context = 'INVOKE_DEFAULT'

        # Add
        layout.operator("curve.subdivide")
        layout.operator("curve.extrude_move")
        layout.operator("curve.make_segment")
        layout.operator("curve.duplicate_move")

        layout.separator()

        # Transform
        layout.operator("transform.transform", text="Radius").mode = 'CURVE_SHRINKFATTEN'
        layout.operator("transform.tilt")
        layout.operator("curve.tilt_clear")
        layout.operator("curve.smooth")
        layout.operator("curve.smooth_tilt")
        layout.operator("curve.smooth_radius")

        layout.separator()

        layout.menu("VIEW3D_MT_mirror")
        layout.menu("VIEW3D_MT_snap")

        layout.separator()

        # Modify
        layout.operator_menu_enum("curve.spline_type_set", "type")
        layout.operator_menu_enum("curve.handle_type_set", "type")
        layout.operator("curve.cyclic_toggle")
        layout.operator("curve.switch_direction")

        layout.separator()

        layout.operator("curve.normals_make_consistent")
        layout.operator("curve.spline_weight_set")
        layout.operator("curve.radius_set")

        layout.separator()

        # Remove
        layout.operator("curve.split")
        layout.operator("curve.decimate")
        layout.operator("curve.separate")
        layout.operator("curve.dissolve_verts")
        layout.operator("curve.delete", text="Delete Segment").type = 'SEGMENT'
        layout.operator("curve.delete", text="Delete Point").type = 'VERT'


class VIEW3D_MT_edit_curve_delete(Menu):
    bl_label = "Delete"

    def draw(self, _context):
        layout = self.layout

        layout.operator_enum("curve.delete", "type")

        layout.separator()

        layout.operator("curve.dissolve_verts")


class VIEW3D_MT_edit_curve_showhide(ShowHideMenu, Menu):
    _operator_name = "curve"


class VIEW3D_MT_edit_surface(Menu):
    bl_label = "Surface"

    draw = draw_curve


class VIEW3D_MT_edit_font_chars(Menu):
    bl_label = "Special Characters"

    def draw(self, _context):
        layout = self.layout

        layout.operator("font.text_insert", text="Copyright \u00A9").text = "\u00A9"
        layout.operator("font.text_insert", text="Registered Trademark \u00AE").text = "\u00AE"

        layout.separator()

        layout.operator("font.text_insert", text="Degree \u00B0").text = "\u00B0"
        layout.operator("font.text_insert", text="Multiplication \u00D7").text = "\u00D7"
        layout.operator("font.text_insert", text="Circle \u2022").text = "\u2022"

        layout.separator()

        layout.operator("font.text_insert", text="Superscript \u00B9").text = "\u00B9"
        layout.operator("font.text_insert", text="Superscript \u00B2").text = "\u00B2"
        layout.operator("font.text_insert", text="Superscript \u00B3").text = "\u00B3"

        layout.separator()

        layout.operator("font.text_insert", text="Guillemet \u00BB").text = "\u00BB"
        layout.operator("font.text_insert", text="Guillemet \u00AB").text = "\u00AB"
        layout.operator("font.text_insert", text="Per Mille \u2030").text = "\u2030"

        layout.separator()

        layout.operator("font.text_insert", text="Euro \u20AC").text = "\u20AC"
        layout.operator("font.text_insert", text="Florin \u0192").text = "\u0192"
        layout.operator("font.text_insert", text="Pound \u00A3").text = "\u00A3"
        layout.operator("font.text_insert", text="Yen \u00A5").text = "\u00A5"

        layout.separator()

        layout.operator("font.text_insert", text="German Eszett \u00DF").text = "\u00DF"
        layout.operator("font.text_insert", text="Inverted Question Mark \u00BF").text = "\u00BF"
        layout.operator("font.text_insert", text="Inverted Exclamation Mark \u00A1").text = "\u00A1"


class VIEW3D_MT_edit_font_kerning(Menu):
    bl_label = "Kerning"

    def draw(self, context):
        layout = self.layout

        ob = context.active_object
        text = ob.data
        kerning = text.edit_format.kerning

        layout.operator("font.change_spacing", text="Decrease Kerning").delta = -1.0
        layout.operator("font.change_spacing", text="Increase Kerning").delta = 1.0
        layout.operator("font.change_spacing", text="Reset Kerning").delta = -kerning


class VIEW3D_MT_edit_font_delete(Menu):
    bl_label = "Delete"

    def draw(self, _context):
        layout = self.layout

        layout.operator("font.delete", text="Previous Character").type = 'PREVIOUS_CHARACTER'
        layout.operator("font.delete", text="Next Character").type = 'NEXT_CHARACTER'
        layout.operator("font.delete", text="Previous Word").type = 'PREVIOUS_WORD'
        layout.operator("font.delete", text="Next Word").type = 'NEXT_WORD'


class VIEW3D_MT_edit_font(Menu):
    bl_label = "Text"

    def draw(self, _context):
        layout = self.layout

        layout.operator("font.text_cut", text="Cut")
        layout.operator("font.text_copy", text="Copy", icon='COPYDOWN')
        layout.operator("font.text_paste", text="Paste", icon='PASTEDOWN')

        layout.separator()

        layout.operator("font.text_paste_from_file")

        layout.separator()

        layout.operator("font.case_set", text="To Uppercase").case = 'UPPER'
        layout.operator("font.case_set", text="To Lowercase").case = 'LOWER'

        layout.separator()

        layout.operator("FONT_OT_text_insert_unicode")
        layout.menu("VIEW3D_MT_edit_font_chars")

        layout.separator()

        layout.operator("font.style_toggle", text="Toggle Bold", icon='BOLD').style = 'BOLD'
        layout.operator("font.style_toggle", text="Toggle Italic", icon='ITALIC').style = 'ITALIC'
        layout.operator("font.style_toggle", text="Toggle Underline", icon='UNDERLINE').style = 'UNDERLINE'
        layout.operator("font.style_toggle", text="Toggle Small Caps", icon='SMALL_CAPS').style = 'SMALL_CAPS'

        layout.menu("VIEW3D_MT_edit_font_kerning")

        layout.separator()

        layout.menu("VIEW3D_MT_edit_font_delete")


class VIEW3D_MT_edit_font_context_menu(Menu):
    bl_label = "Text"

    def draw(self, _context):
        layout = self.layout

        layout.operator_context = 'INVOKE_DEFAULT'

        layout.operator("font.text_cut", text="Cut")
        layout.operator("font.text_copy", text="Copy", icon='COPYDOWN')
        layout.operator("font.text_paste", text="Paste", icon='PASTEDOWN')

        layout.separator()

        layout.operator("font.select_all")

        layout.separator()

        layout.menu("VIEW3D_MT_edit_font")


class VIEW3D_MT_edit_lattice(Menu):
    bl_label = "Lattice"

    def draw(self, _context):
        layout = self.layout

        layout.separator()

        layout.menu("VIEW3D_MT_transform")
        layout.menu("VIEW3D_MT_mirror")
        layout.menu("VIEW3D_MT_snap")
        layout.operator_menu_enum("lattice.flip", "axis")

        layout.separator()

        layout.operator("lattice.make_regular")

        layout.separator()


class VIEW3D_MT_edit_curves_add(Menu):
    bl_label = "Add"
    bl_translation_context = i18n_contexts.operator_default

    def draw(self, _context):
        layout = self.layout

        layout.operator("curves.add_bezier", text="Bézier", icon='CURVE_BEZCURVE')
        layout.operator("curves.add_circle", text="Circle", icon='CURVE_BEZCIRCLE')


class VIEW3D_MT_edit_curves(Menu):
    bl_label = "Curves"

    def draw(self, _context):
        layout = self.layout

        layout.menu("VIEW3D_MT_transform")
        layout.menu("VIEW3D_MT_mirror")
        layout.menu("VIEW3D_MT_snap")

        layout.separator()

        layout.operator("curves.duplicate_move")
        layout.operator("curves.extrude_move")

        layout.separator()

        layout.operator("curves.attribute_set")
        layout.operator_menu_enum("curves.curve_type_set", "type")
        layout.operator("curves.cyclic_toggle")
        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)

        layout.separator()

        layout.operator("curves.separate")
        layout.operator("curves.delete")


class VIEW3D_MT_edit_curves_control_points(Menu):
    bl_label = "Control Points"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curves.extrude_move")
        layout.operator_menu_enum("curves.handle_type_set", "type")


class VIEW3D_MT_edit_curves_segments(Menu):
    bl_label = "Segments"

    def draw(self, _context):
        layout = self.layout

        layout.operator("curves.subdivide")
        layout.operator("curves.switch_direction")


class VIEW3D_MT_edit_curves_context_menu(Menu):
    bl_label = "Curves"

    def draw(self, _context):
        layout = self.layout

        layout.operator_context = 'INVOKE_DEFAULT'

        # Additive Operators
        layout.operator("curves.subdivide")

        layout.separator()

        layout.operator("curves.extrude_move")

        layout.separator()

        # Deform Operators
        layout.menu("VIEW3D_MT_mirror")
        layout.menu("VIEW3D_MT_snap")

        layout.separator()

        # Modify Flags
        layout.operator_menu_enum("curves.curve_type_set", "type")
        layout.operator_menu_enum("curves.handle_type_set", "type")
        layout.operator("curves.cyclic_toggle")
        layout.operator("curves.switch_direction")

        layout.separator()

        # Removal Operators
        layout.operator("curves.separate")
        layout.operator("curves.delete")

        layout.separator()

        layout.operator("curves.split")


class VIEW3D_MT_edit_pointcloud(Menu):
    bl_label = "Point Cloud"

    def draw(self, context):
        layout = self.layout
        layout.menu("VIEW3D_MT_transform")
        layout.separator()
        layout.operator("pointcloud.duplicate_move")
        layout.separator()
        layout.operator("pointcloud.attribute_set")
        layout.operator("pointcloud.delete")
        layout.operator("pointcloud.separate")
        layout.template_node_operator_asset_menu_items(catalog_path=self.bl_label)


class VIEW3D_MT_object_mode_pie(Menu):
    bl_label = "Mode"

    def draw(self, _context):
        layout = self.layout

        pie = layout.menu_pie()
        pie.operator_enum("object.mode_set", "mode")


class VIEW3D_MT_view_pie(Menu):
    bl_label = "View"
    bl_idname = "VIEW3D_MT_view_pie"

    def draw(self, _context):
        layout = self.layout

        pie = layout.menu_pie()
        pie.operator_enum("view3d.view_axis", "type")
        pie.operator("view3d.view_camera", text="View Camera", icon='CAMERA_DATA')
        pie.operator("view3d.view_selected", text="View Selected", icon='ZOOM_SELECTED')


class VIEW3D_MT_transform_gizmo_pie(Menu):
    bl_label = "View"

    def draw(self, context):
        layout = self.layout

        pie = layout.menu_pie()
        # 1: Left
        pie.operator("view3d.transform_gizmo_set", text="Move").type = {'TRANSLATE'}
        # 2: Right
        pie.operator("view3d.transform_gizmo_set", text="Rotate").type = {'ROTATE'}
        # 3: Down
        pie.operator("view3d.transform_gizmo_set", text="Scale").type = {'SCALE'}
        # 4: Up
        pie.prop(context.space_data, "show_gizmo", text="Show Gizmos", icon='GIZMO')
        # 5: Up/Left
        pie.operator("view3d.transform_gizmo_set", text="All").type = {'TRANSLATE', 'ROTATE', 'SCALE'}


class VIEW3D_MT_shading_pie(Menu):
    bl_label = "Shading"

    def draw(self, context):
        layout = self.layout
        pie = layout.menu_pie()

        view = context.space_data

        pie.prop(view.shading, "type", expand=True)


class VIEW3D_MT_shading_ex_pie(Menu):
    bl_label = "Shading"

    def draw(self, context):
        layout = self.layout
        pie = layout.menu_pie()

        view = context.space_data

        pie.prop_enum(view.shading, "type", value='WIREFRAME')
        pie.prop_enum(view.shading, "type", value='SOLID')

        _toggle_xray_operator(pie, context)

        pie.prop(view.overlay, "show_overlays", text="Toggle Overlays", icon='OVERLAY')

        pie.prop_enum(view.shading, "type", value='MATERIAL')
        pie.prop_enum(view.shading, "type", value='RENDERED')


class VIEW3D_MT_pivot_pie(Menu):
    bl_label = "Pivot Point"

    def draw(self, context):
        layout = self.layout
        pie = layout.menu_pie()

        tool_settings = context.tool_settings
        obj = context.active_object
        mode = context.mode

        pie.prop_enum(tool_settings, "transform_pivot_point", value='BOUNDING_BOX_CENTER')
        pie.prop_enum(tool_settings, "transform_pivot_point", value='CURSOR')
        pie.prop_enum(tool_settings, "transform_pivot_point", value='INDIVIDUAL_ORIGINS')
        pie.prop_enum(tool_settings, "transform_pivot_point", value='MEDIAN_POINT')
        pie.prop_enum(tool_settings, "transform_pivot_point", value='ACTIVE_ELEMENT')
        if (obj is None) or (mode == 'OBJECT'):
            pie.prop(tool_settings, "use_transform_pivot_point_align")


class VIEW3D_MT_orientations_pie(Menu):
    bl_label = "Orientation"

    def draw(self, context):
        layout = self.layout
        pie = layout.menu_pie()
        scene = context.scene

        pie.prop(scene.transform_orientation_slots[0], "type", expand=True)


class VIEW3D_MT_snap_pie(Menu):
    bl_label = "Snap"

    def draw(self, _context):
        layout = self.layout
        pie = layout.menu_pie()

        pie.operator("view3d.snap_cursor_to_grid", text="Cursor to Grid", icon='CURSOR')
        pie.operator("view3d.snap_selected_to_grid", text="Selection to Grid", icon='RESTRICT_SELECT_OFF')
        pie.operator("view3d.snap_cursor_to_selected", text="Cursor to Selected", icon='CURSOR')
        pie.operator(
            "view3d.snap_selected_to_cursor",
            text="Selection to Cursor",
            icon='RESTRICT_SELECT_OFF',
        ).use_offset = False
        pie.operator(
            "view3d.snap_selected_to_cursor",
            text="Selection to Cursor (Keep Offset)",
            icon='RESTRICT_SELECT_OFF',
        ).use_offset = True
        pie.operator("view3d.snap_selected_to_active", text="Selection to Active", icon='RESTRICT_SELECT_OFF')
        pie.operator("view3d.snap_cursor_to_center", text="Cursor to World Origin", icon='CURSOR')
        pie.operator("view3d.snap_cursor_to_active", text="Cursor to Active", icon='CURSOR')


class VIEW3D_MT_proportional_editing_falloff_pie(Menu):
    bl_label = "Proportional Editing Falloff"

    def draw(self, context):
        layout = self.layout
        pie = layout.menu_pie()

        tool_settings = context.scene.tool_settings

        pie.prop(tool_settings, "proportional_edit_falloff", expand=True)


class VIEW3D_MT_sculpt_mask_edit_pie(Menu):
    bl_label = "Mask Edit"

    def draw(self, _context):
        layout = self.layout
        pie = layout.menu_pie()

        props = pie.operator("paint.mask_flood_fill", text="Invert Mask")
        props.mode = 'INVERT'
        props = pie.operator("paint.mask_flood_fill", text="Clear Mask")
        props.mode = 'VALUE'
        props.value = 0.0
        props = pie.operator("sculpt.mask_filter", text="Smooth Mask")
        props.filter_type = 'SMOOTH'
        props = pie.operator("sculpt.mask_filter", text="Sharpen Mask")
        props.filter_type = 'SHARPEN'
        props = pie.operator("sculpt.mask_filter", text="Grow Mask")
        props.filter_type = 'GROW'
        props = pie.operator("sculpt.mask_filter", text="Shrink Mask")
        props.filter_type = 'SHRINK'
        props = pie.operator("sculpt.mask_filter", text="Increase Contrast")
        props.filter_type = 'CONTRAST_INCREASE'
        props.auto_iteration_count = False
        props = pie.operator("sculpt.mask_filter", text="Decrease Contrast")
        props.filter_type = 'CONTRAST_DECREASE'
        props.auto_iteration_count = False


class VIEW3D_MT_sculpt_automasking_pie(Menu):
    bl_label = "Automasking"

    def draw(self, context):
        layout = self.layout
        pie = layout.menu_pie()

        tool_settings = context.tool_settings
        paint = tool_settings.sculpt

        settings = paint.mesh_automasking_settings

        pie.prop(settings, "use_automasking_topology", text="Topology")
        pie.prop(settings, "use_automasking_face_sets", text="Face Sets")
        pie.prop(settings, "use_automasking_boundary_edges", text="Mesh Boundary")
        pie.prop(settings, "use_automasking_boundary_face_sets", text="Face Sets Boundary")
        pie.prop(settings, "use_automasking_cavity", text="Cavity")
        pie.prop(settings, "use_automasking_cavity_inverted", text="Cavity (Inverted)")
        pie.prop(settings, "use_automasking_start_normal", text="Area Normal")
        pie.prop(settings, "use_automasking_view_normal", text="View Normal")


class VIEW3D_MT_sculpt_face_sets_edit_pie(Menu):

    bl_label = "Face Sets Edit"

    def draw(self, _context):
        layout = self.layout
        pie = layout.menu_pie()

        props = pie.operator("sculpt.face_sets_create", text="Face Set from Masked")
        props.mode = 'MASKED'

        props = pie.operator("sculpt.face_sets_create", text="Face Set from Visible")
        props.mode = 'VISIBLE'

        pie.operator("paint.visibility_invert", text="Invert Visible")

        props = pie.operator("paint.hide_show_all", text="Show All")
        props.action = 'SHOW'


# ********** Panel **********


class VIEW3D_PT_active_tool(Panel, ToolActivePanelHelper):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Tool"
    # See comment below.
    # bl_options = {'HIDE_HEADER'}

    # Don't show in properties editor.
    @classmethod
    def poll(cls, context):
        return context.area.type == 'VIEW_3D'


# FIXME(campbell): remove this second panel once 'HIDE_HEADER' works with category tabs,
# Currently pinning allows ordering headerless panels below panels with headers.
class VIEW3D_PT_active_tool_duplicate(Panel, ToolActivePanelHelper):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Tool"
    bl_options = {'HIDE_HEADER'}

    # Only show in properties editor.
    @classmethod
    def poll(cls, context):
        return context.area.type != 'VIEW_3D'


class VIEW3D_PT_view3d_properties(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "View"
    bl_label = "View"

    def draw(self, context):
        layout = self.layout

        view = context.space_data

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        col = layout.column()

        subcol = col.column()
        subcol.active = bool(view.region_3d.view_perspective != 'CAMERA' or view.region_quadviews)
        subcol.prop(view, "lens", text="Focal Length")

        subcol = col.column(align=True)
        subcol.prop(view, "clip_start", text="Clip Start")
        subcol.prop(view, "clip_end", text="End", text_ctxt=i18n_contexts.id_camera)

        layout.separator()

        col = layout.column(align=False, heading="Local Camera")
        col.use_property_decorate = False
        row = col.row(align=True)
        sub = row.row(align=True)
        sub.prop(view, "use_local_camera", text="")
        sub = sub.row(align=True)
        sub.enabled = view.use_local_camera
        sub.prop(view, "camera", text="")

        sub = col.row()
        sub.active = view.region_3d.view_perspective == 'CAMERA'
        sub.prop(view.overlay, "show_camera_passepartout", text="Passepartout")

        layout.separator()

        col = layout.column(align=True)
        col.prop(view, "use_render_border")
        col.active = view.region_3d.view_perspective != 'CAMERA'


class VIEW3D_PT_view3d_lock(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "View"
    bl_label = "View Lock"
    bl_parent_id = "VIEW3D_PT_view3d_properties"

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        view = context.space_data

        col = layout.column(align=True)
        sub = col.column()
        sub.active = bool(view.region_3d.view_perspective != 'CAMERA' or view.region_quadviews)

        sub.prop(view, "lock_object")
        col = layout.column(heading="Lock", align=True)
        if not view.lock_object:
            col.prop(view, "lock_cursor", text="To 3D Cursor")
        col.prop(view, "lock_camera", text="Camera to View")
        col.prop(view.region_3d, "lock_rotation", text="Rotation")


class VIEW3D_PT_view3d_cursor(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "View"
    bl_label = "3D Cursor"

    def draw(self, context):
        layout = self.layout

        cursor = context.scene.cursor

        layout.column().prop(cursor, "location", text="Location")
        rotation_mode = cursor.rotation_mode
        if rotation_mode == 'QUATERNION':
            layout.column().prop(cursor, "rotation_quaternion", text="Rotation")
        elif rotation_mode == 'AXIS_ANGLE':
            layout.column().prop(cursor, "rotation_axis_angle", text="Rotation")
        else:
            layout.column().prop(cursor, "rotation_euler", text="Rotation")
        layout.prop(cursor, "rotation_mode", text="")


class VIEW3D_PT_collections(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "View"
    bl_label = "Collections"
    bl_options = {'DEFAULT_CLOSED'}

    def _draw_collection(self, layout, view_layer, use_local_collections, collection, index):
        need_separator = index
        for child in collection.children:
            index += 1

            if child.exclude:
                continue

            if child.collection.hide_viewport:
                continue

            if need_separator:
                layout.separator()
                need_separator = False

            icon = 'BLANK1'
            # has_objects = True
            if child.has_selected_objects(view_layer):
                icon = 'LAYER_ACTIVE'
            elif child.has_objects():
                icon = 'LAYER_USED'
            else:
                # has_objects = False
                pass

            row = layout.row()
            row.use_property_decorate = False
            sub = row.split(factor=0.98)
            subrow = sub.row()
            subrow.alignment = 'LEFT'
            subrow.operator(
                "object.hide_collection", text=child.name, icon=icon, emboss=False,
            ).collection_index = index

            sub = row.split()
            subrow = sub.row(align=True)
            subrow.alignment = 'RIGHT'
            if not use_local_collections:
                subrow.active = collection.is_visible  # Parent collection runtime visibility
                subrow.prop(child, "hide_viewport", text="", emboss=False)
            else:
                subrow.active = collection.visible_get()  # Parent collection runtime visibility
                icon = 'HIDE_OFF' if child.visible_get() else 'HIDE_ON'
                props = subrow.operator("object.hide_collection", text="", icon=icon, emboss=False)
                props.collection_index = index
                props.toggle = True

        for child in collection.children:
            index = self._draw_collection(layout, view_layer, use_local_collections, child, index)

        return index

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = False

        view = context.space_data
        view_layer = context.view_layer

        layout.use_property_split = True
        layout.prop(view, "use_local_collections")
        layout.separator()

        # We pass index 0 here because the index is increased
        # so the first real index is 1
        # And we start with index as 1 because we skip the master collection
        self._draw_collection(layout, view_layer, view.use_local_collections, view_layer.layer_collection, 0)


class VIEW3D_PT_object_type_visibility(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Selectability & Visibility"
    bl_ui_units_x = 8

    # Allows derived classes to pass view data other than context.space_data.
    # This is used by the official VR add-on, which passes XrSessionSettings
    # since VR has a 3D view that only exists for the duration of the VR session.
    def draw_ex(self, _context, view, show_select):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        layout.label(text="Selectability & Visibility")
        layout.separator()
        col = layout.column(align=True)

        attr_object_types = (
            ("mesh", "Mesh", 'OUTLINER_OB_MESH'),
            ("curve", "Curve", 'OUTLINER_OB_CURVE'),
            ("surf", "Surface", 'OUTLINER_OB_SURFACE'),
            ("font", "Text", 'OUTLINER_OB_FONT'),
            ("curves", "Hair Curves", 'OUTLINER_OB_CURVES'),
            ("pointcloud", "Point Cloud", 'OUTLINER_OB_POINTCLOUD'),
            ("volume", "Volume", 'OUTLINER_OB_VOLUME'),
            ("lattice", "Lattice", 'OUTLINER_OB_LATTICE'),
            ("light", "Light", 'OUTLINER_OB_LIGHT'),
            ("light_probe", "Light Probe", 'OUTLINER_OB_LIGHTPROBE'),
            ("camera", "Camera", 'OUTLINER_OB_CAMERA'),
        )

        for attr, attr_name, attr_icon in attr_object_types:
            if attr is None:
                col.separator()
                continue

            attr_v = "show_object_viewport_" + attr
            icon_v = 'HIDE_OFF' if getattr(view, attr_v) else 'HIDE_ON'

            row = col.row(align=True)
            row.label(text=attr_name, icon=attr_icon)

            if show_select:
                attr_s = "show_object_select_" + attr
                icon_s = 'RESTRICT_SELECT_OFF' if getattr(view, attr_s) else 'RESTRICT_SELECT_ON'

                rowsub = row.row(align=True)
                rowsub.active = getattr(view, attr_v)
                rowsub.prop(view, attr_s, text="", icon=icon_s, emboss=False)

            row.prop(view, attr_v, text="", icon=icon_v, emboss=False)

    def draw(self, context):
        view = context.space_data
        self.draw_ex(context, view, True)


class VIEW3D_PT_shading(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Shading"
    bl_ui_units_x = 12

    @classmethod
    def get_shading(cls, context):
        # Get settings from 3D viewport or OpenGL render engine
        view = context.space_data
        if view.type == 'VIEW_3D':
            return view.shading
        else:
            return context.scene.display.shading

    def draw(self, _context):
        layout = self.layout
        layout.label(text="Viewport Shading")


class VIEW3D_PT_shading_lighting(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Lighting"
    bl_parent_id = "VIEW3D_PT_shading"

    @classmethod
    def poll(cls, context):
        shading = VIEW3D_PT_shading.get_shading(context)
        if shading.type in {'SOLID', 'MATERIAL'}:
            return True
        if shading.type == 'RENDERED':
            engine = context.scene.render.engine
            if engine == 'BLENDER_EEVEE':
                return True
        return False

    def draw(self, context):
        layout = self.layout
        shading = VIEW3D_PT_shading.get_shading(context)

        col = layout.column()
        split = col.split(factor=0.95)

        if shading.type == 'SOLID':
            col.row().prop(shading, "light", expand=True)
            col = split.column()

            split = layout.split(factor=0.95)
            col = split.column()
            sub = col.row()

            if shading.light == 'STUDIO':
                prefs = context.preferences
                system = prefs.system

                if not system.use_studio_light_edit:
                    sub.scale_y = 0.6  # Smaller studio-light preview.
                    sub.template_icon_view(shading, "studio_light", scale_popup=3.0)
                else:
                    sub.prop(
                        system,
                        "use_studio_light_edit",
                        text="Disable Studio Light Edit",
                        icon='NONE',
                        toggle=True,
                    )

                col = split.column()
                col.operator("screen.userpref_show", emboss=False, text="", icon='PREFERENCES').section = 'LIGHTS'

                split = layout.split(factor=0.95)
                col = split.column()

                row = col.row(align=True)
                row.prop(shading, "use_world_space_lighting", text="", icon='WORLD', toggle=True)
                row = row.row(align=True)
                row.active = shading.use_world_space_lighting
                row.prop(shading, "studiolight_rotate_z", text="Rotation")
                col = split.column()  # to align properly with above

            elif shading.light == 'MATCAP':
                sub.scale_y = 0.6  # smaller matcap preview
                sub.template_icon_view(shading, "studio_light", scale_popup=3.0)

                col = split.column()
                col.operator("screen.userpref_show", emboss=False, text="", icon='PREFERENCES').section = 'LIGHTS'
                col.operator("view3d.toggle_matcap_flip", emboss=False, text="", icon='ARROW_LEFTRIGHT')

        elif shading.type == 'MATERIAL':
            col.prop(shading, "use_scene_lights")
            col.prop(shading, "use_scene_world")
            col = layout.column()
            split = col.split(factor=0.95)

            if not shading.use_scene_world:
                col = split.column()
                sub = col.row()
                sub.scale_y = 0.6
                sub.template_icon_view(shading, "studio_light", scale_popup=3)

                col = split.column()
                col.operator("screen.userpref_show", emboss=False, text="", icon='PREFERENCES').section = 'LIGHTS'

                split = layout.split(factor=0.95)
                col = split.column()

                row = col.row(align=True)
                row.prop(shading, "use_studiolight_view_rotation", text="", icon='WORLD', toggle=True)
                row = row.row(align=True)
                row.prop(shading, "studiolight_rotate_z", text="Rotation")

                col.prop(shading, "studiolight_intensity")
                col.prop(shading, "studiolight_background_alpha")
                col.prop(shading, "studiolight_background_blur")
                col = split.column()  # to align properly with above

        elif shading.type == 'RENDERED':
            col.prop(shading, "use_scene_lights_render")
            col.prop(shading, "use_scene_world_render")

            if not shading.use_scene_world_render:
                col = layout.column()
                split = col.split(factor=0.95)

                col = split.column()
                sub = col.row()
                sub.scale_y = 0.6
                sub.template_icon_view(shading, "studio_light", scale_popup=3)

                col = split.column()
                col.operator("screen.userpref_show", emboss=False, text="", icon='PREFERENCES').section = 'LIGHTS'

                split = layout.split(factor=0.95)
                col = split.column()
                col.prop(shading, "studiolight_rotate_z", text="Rotation")
                col.prop(shading, "studiolight_intensity")
                col.prop(shading, "studiolight_background_alpha")
                col.prop(shading, "studiolight_background_blur")
                col = split.column()  # to align properly with above


class VIEW3D_PT_shading_color(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Color"
    bl_parent_id = "VIEW3D_PT_shading"

    def _draw_color_type(self, context):
        layout = self.layout
        shading = VIEW3D_PT_shading.get_shading(context)

        layout.grid_flow(row_major=True, columns=3, align=True).prop(shading, "color_type", expand=True)
        if shading.color_type == 'SINGLE':
            layout.row().prop(shading, "single_color", text="")

    def _draw_background_color(self, context):
        layout = self.layout
        shading = VIEW3D_PT_shading.get_shading(context)

        layout.row().label(text="Background")
        layout.row().prop(shading, "background_type", expand=True)
        if shading.background_type == 'VIEWPORT':
            layout.row().prop(shading, "background_color", text="")

    def draw(self, context):
        layout = self.layout
        shading = VIEW3D_PT_shading.get_shading(context)

        layout.label(text="Wireframe")
        layout.row().prop(shading, "wireframe_color_type", expand=True)
        layout.separator()

        if shading.type == 'SOLID':
            layout.label(text="Object")
            self._draw_color_type(context)
            layout.separator()
            self._draw_background_color(context)
        elif shading.type == 'WIREFRAME':
            self._draw_background_color(context)


class VIEW3D_PT_shading_options(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Options"
    bl_parent_id = "VIEW3D_PT_shading"

    @classmethod
    def poll(cls, context):
        shading = VIEW3D_PT_shading.get_shading(context)
        return shading.type in {'WIREFRAME', 'SOLID'}

    def draw(self, context):
        layout = self.layout

        shading = VIEW3D_PT_shading.get_shading(context)

        col = layout.column()

        if shading.type == 'SOLID':
            col.prop(shading, "show_backface_culling")

        if shading.type in {'WIREFRAME', 'SOLID'}:
            row = col.split()
            row.prop(shading, "show_object_outline")
            sub = row.row()
            sub.active = shading.show_object_outline
            sub.prop(shading, "object_outline_color", text="")

        if shading.type == 'SOLID' and shading.light in {'STUDIO', 'MATCAP'}:
            sub = col.row()
            studio_light = shading.selected_studio_light
            sub.active = (studio_light is not None) and studio_light.has_specular_highlight_pass
            sub.prop(shading, "show_specular_highlight", text="Specular Lighting")

        row = col.row(align=True)

        if shading.type == 'WIREFRAME':
            row.prop(shading, "show_xray_wireframe", text="")
            sub = row.row()
            sub.active = shading.show_xray_wireframe
            sub.prop(shading, "xray_alpha_wireframe", text="X-Ray")
        elif shading.type == 'SOLID':
            row.prop(shading, "show_xray", text="")
            sub = row.row()
            sub.active = shading.show_xray
            sub.prop(shading, "xray_alpha", text="X-Ray")
            # X-ray mode is off when alpha is 1.0
            xray_active = shading.show_xray and shading.xray_alpha != 1

            row = col.row(align=True)
            row.prop(shading, "show_shadows", text="")
            row.active = not xray_active
            sub = row.row(align=True)
            sub.active = shading.show_shadows
            sub.prop(shading, "shadow_intensity", text="Shadow")
            sub.popover(
                panel="VIEW3D_PT_shading_options_shadow",
                icon='PREFERENCES',
                text="",
            )

            row = col.row()
            row.active = not xray_active
            row.prop(shading, "use_dof", text="Depth of Field")


class VIEW3D_PT_shading_options_shadow(Panel):
    bl_label = "Shadow Settings"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_ui_units_x = 12

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        scene = context.scene

        col = layout.column()
        col.prop(scene.display, "light_direction", text="Direction")
        col.prop(scene.display, "shadow_shift", text="Offset")
        col.prop(scene.display, "shadow_focus", text="Focus")


class VIEW3D_PT_shading_options_ssao(Panel):
    bl_label = "SSAO Settings"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        scene = context.scene

        col = layout.column(align=True)
        col.prop(scene.display, "matcap_ssao_samples")
        col.prop(scene.display, "matcap_ssao_distance")
        col.prop(scene.display, "matcap_ssao_attenuation")


class VIEW3D_PT_shading_cavity(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Cavity"
    bl_parent_id = "VIEW3D_PT_shading"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        shading = VIEW3D_PT_shading.get_shading(context)
        return shading.type in {'SOLID'}

    def draw_header(self, context):
        layout = self.layout
        shading = VIEW3D_PT_shading.get_shading(context)
        xray_active = shading.show_xray and shading.xray_alpha != 1

        row = layout.row()
        row.active = not xray_active
        row.prop(shading, "show_cavity")
        row = row.row()
        row.active = shading.show_cavity
        row.prop(shading, "cavity_type", text="Type")

    def draw(self, context):
        layout = self.layout
        shading = VIEW3D_PT_shading.get_shading(context)
        xray_active = shading.show_xray and shading.xray_alpha != 1

        col = layout.column()
        col.active = not xray_active and shading.show_cavity

        if shading.cavity_type in {'WORLD', 'BOTH'}:
            row = col.row()
            row.label(text="World Space")
            row.popover(
                panel="VIEW3D_PT_shading_options_ssao",
                icon='PREFERENCES',
                text="",
            )

            row = col.row()
            row.prop(shading, "cavity_ridge_factor", text="Ridge")
            row.prop(shading, "cavity_valley_factor", text="Valley")

        if shading.cavity_type in {'SCREEN', 'BOTH'}:
            col.label(text="Screen Space")
            row = col.row()
            row.prop(shading, "curvature_ridge_factor", text="Ridge")
            row.prop(shading, "curvature_valley_factor", text="Valley")


class VIEW3D_PT_shading_render_pass(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Render Pass"
    bl_parent_id = "VIEW3D_PT_shading"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (
            (context.space_data.shading.type == 'MATERIAL') or
            (context.engine in cls.COMPAT_ENGINES and context.space_data.shading.type == 'RENDERED')
        )

    def draw(self, context):
        shading = context.space_data.shading

        layout = self.layout
        layout.prop(shading, "render_pass", text="")


class VIEW3D_PT_shading_compositor(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Compositor"
    bl_parent_id = "VIEW3D_PT_shading"
    bl_order = 10

    @classmethod
    def poll(cls, context):
        return context.space_data.shading.type in {'MATERIAL', 'RENDERED'}

    def draw(self, context):
        shading = context.space_data.shading
        row = self.layout.row()
        row.prop(shading, "use_compositor", expand=True)


class VIEW3D_PT_gizmo_display(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Gizmos"
    bl_ui_units_x = 8

    def draw(self, context):
        layout = self.layout

        scene = context.scene
        view = context.space_data

        col = layout.column()
        col.label(text="Viewport Gizmos")
        col.separator()

        col.active = view.show_gizmo
        colsub = col.column()
        colsub.prop(view, "show_gizmo_navigate", text="Navigate")
        colsub.prop(view, "show_gizmo_tool", text="Active Tools")
        colsub.prop(view, "show_gizmo_modifier", text="Active Modifier")
        colsub.prop(view, "show_gizmo_context", text="Active Object")

        layout.separator()

        col = layout.column()
        col.active = view.show_gizmo and view.show_gizmo_context
        col.label(text="Object Gizmos")
        col.prop(scene.transform_orientation_slots[1], "type", text="")
        col.prop(view, "show_gizmo_object_translate", text="Move", text_ctxt=i18n_contexts.operator_default)
        col.prop(view, "show_gizmo_object_rotate", text="Rotate", text_ctxt=i18n_contexts.operator_default)
        col.prop(view, "show_gizmo_object_scale", text="Scale", text_ctxt=i18n_contexts.operator_default)

        layout.separator()

        col = layout.column()
        col.active = view.show_gizmo
        col.label(text="Light")
        col.prop(view, "show_gizmo_light_size", text="Size")
        col.prop(view, "show_gizmo_light_look_at", text="Look At")
        col.label(text="Camera")
        col.prop(view, "show_gizmo_camera_lens", text="Lens")
        col.prop(view, "show_gizmo_camera_dof_distance", text="Focus Distance")


class VIEW3D_PT_overlay(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Overlays"
    bl_ui_units_x = 14

    def draw(self, _context):
        layout = self.layout
        layout.label(text="Viewport Overlays")


class VIEW3D_PT_overlay_guides(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay"
    bl_label = "Guides"

    def draw(self, context):
        layout = self.layout

        view = context.space_data
        scene = context.scene

        overlay = view.overlay
        shading = view.shading
        display_all = overlay.show_overlays

        col = layout.column()
        col.active = display_all

        split = col.split()
        sub = split.column()

        row = sub.row()
        row_el = row.column()
        row_el.prop(overlay, "show_ortho_grid", text="Grid")
        grid_active = bool(
            view.region_quadviews or
            (view.region_3d.is_orthographic_side_view and not view.region_3d.is_perspective)
        )
        row_el.active = grid_active
        row.prop(overlay, "show_floor", text="Floor", text_ctxt=i18n_contexts.editor_view3d)

        if overlay.show_floor or overlay.show_ortho_grid:
            sub = col.row(align=True)
            sub.active = (
                (overlay.show_floor and not view.region_3d.is_orthographic_side_view) or
                (overlay.show_ortho_grid and grid_active)
            )
            sub.prop(overlay, "grid_scale", text="Scale")
            sub = sub.row(align=True)
            sub.active = scene.unit_settings.system == 'NONE'
            sub.prop(overlay, "grid_subdivisions", text="Subdivisions")

        sub = split.column()
        row = sub.row()
        row.label(text="Axes")

        subrow = row.row(align=True)
        subrow.prop(overlay, "show_axis_x", text="X", toggle=True)
        subrow.prop(overlay, "show_axis_y", text="Y", toggle=True)
        subrow.prop(overlay, "show_axis_z", text="Z", toggle=True)

        split = col.split()
        sub = split.column()
        row = sub.row()
        row.prop(overlay, "show_cursor", text="3D Cursor")

        if view.region_3d.view_perspective == 'CAMERA':
            sub.prop(overlay, "show_camera_guides", text="Camera Guides")

        if shading.type == 'MATERIAL':
            row = col.row()
            row.active = shading.render_pass == 'COMBINED'
            row.prop(overlay, "show_look_dev")


class VIEW3D_PT_overlay_text(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay"
    bl_label = "Text"

    def draw(self, context):
        layout = self.layout

        view = context.space_data
        overlay = view.overlay

        split = layout.split()
        sub = split.column(align=True)
        sub.prop(overlay, "show_text", text="General Info")
        sub.prop(overlay, "show_stats", text="Statistics")

        sub = split.column(align=True)
        sub.prop(overlay, "show_performance", text="Performance")


class VIEW3D_PT_overlay_object(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay"
    bl_label = "Objects"

    def draw(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays
        mode = context.mode

        col = layout.column(align=True)
        col.active = display_all

        split = col.split()

        sub = split.column(align=True)
        sub.prop(overlay, "show_extras", text="Extras")
        subsub = sub.column()
        subsub.active = overlay.show_extras
        subsub.prop(overlay, "show_light_colors")
        sub.prop(overlay, "show_relationship_lines")
        sub.prop(overlay, "show_outline_selected")

        sub = split.column(align=True)
        sub.prop(overlay, "show_motion_paths")

        can_show_object_origins = mode not in {
            'PAINT_2D',
            'SCULPT',
            'PAINT_VERTEX',
            'SCULPT_CURVES',
        }
        subsub = sub.column()
        subsub.active = can_show_object_origins
        subsub.prop(overlay, "show_object_origins", text="Origins")
        subsub = sub.column()
        subsub.active = can_show_object_origins and overlay.show_object_origins
        subsub.prop(overlay, "show_object_origins_all", text="Origins (All)")


class VIEW3D_PT_overlay_geometry(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay"
    bl_label = "Geometry"

    def draw(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays
        is_wireframes = view.shading.type == 'WIREFRAME'

        col = layout.column()
        col.active = display_all

        row = col.row(align=True)
        if not is_wireframes:
            row.prop(overlay, "show_wireframes", text="")
        sub = row.row()
        sub.active = overlay.show_wireframes or is_wireframes
        sub.prop(overlay, "wireframe_threshold", text="Wireframe")
        sub.prop(overlay, "wireframe_opacity", text="Opacity")

        row = col.row(align=True)

        # These properties should be always available in the UI for all modes
        # other than Object.
        # Even when the Fade Inactive Geometry overlay is not affecting the
        # current active object depending on its mode, it will always affect
        # the rest of the scene.
        if context.mode != 'OBJECT':
            row.prop(overlay, "show_fade_inactive", text="")
            sub = row.row()
            sub.active = overlay.show_fade_inactive
            sub.prop(overlay, "fade_inactive_alpha", text="Fade Inactive Geometry")

        col = layout.column(align=True)
        col.active = display_all

        col.prop(overlay, "show_face_orientation")

        # sub.prop(overlay, "show_onion_skins")


class VIEW3D_PT_overlay_viewer_node(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay"
    bl_label = "Viewer Node"

    def draw(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays

        col = layout.column()
        col.active = display_all

        row = col.row(align=True)
        row.active = view.show_viewer
        row.prop(overlay, "show_viewer_attribute", text="")
        subrow = row.row(align=True)
        subrow.active = overlay.show_viewer_attribute
        subrow.prop(overlay, "viewer_attribute_opacity", text="Color Opacity")

        row = col.row(align=True)
        row.active = view.show_viewer and overlay.show_text
        row.prop(overlay, "show_viewer_text", text="Attribute Text")


class VIEW3D_PT_overlay_motion_tracking(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay"
    bl_label = "Motion Tracking"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays
        layout.active = display_all
        layout.prop(view, "show_reconstruction", text=self.bl_label)

    def draw(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay
        layout.active = overlay.show_overlays and view.show_reconstruction

        col = layout.column()

        split = col.split()

        sub = split.column(align=True)
        sub.prop(view, "show_camera_path", text="Camera Path")

        sub = split.column()
        sub.prop(view, "show_bundle_names", text="Marker Names")

        col = layout.column()
        col.label(text="Tracks")
        row = col.row(align=True)
        row.prop(view, "tracks_display_type", text="")
        row.prop(view, "tracks_display_size", text="Size")


class VIEW3D_PT_overlay_edit_mesh(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Mesh Edit Mode"
    bl_ui_units_x = 14

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def draw(self, context):
        layout = self.layout
        layout.label(text="Mesh Edit Mode Overlays")

        view = context.space_data
        shading = view.shading
        overlay = view.overlay
        display_all = overlay.show_overlays

        is_any_solid_shading = not (shading.show_xray or (shading.type == 'WIREFRAME'))

        col = layout.column()
        col.active = display_all

        row = col.row(align=True)
        row.prop(overlay, "show_edge_bevel_weight", text="Bevel", icon='EDGE_BEVEL', toggle=True)
        row.prop(overlay, "show_edge_crease", text="Crease", icon='EDGE_CREASE', toggle=True)
        row.prop(overlay, "show_edge_seams", text="Seam", icon='EDGE_SEAM', toggle=True)
        row.prop(
            overlay,
            "show_edge_sharp",
            text="Sharp",
            icon='EDGE_SHARP',
            text_ctxt=i18n_contexts.plural,
            toggle=True,
        )

        col.separator()
        split = col.split()

        sub = split.column()
        sub.prop(overlay, "show_faces", text="Faces")
        sub = split.column()
        sub.active = is_any_solid_shading
        sub.prop(overlay, "show_face_center", text="Center")

        col.prop(overlay, "show_extra_indices", text="Indices")


class VIEW3D_PT_overlay_edit_mesh_shading(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay_edit_mesh"
    bl_label = "Shading"

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def draw(self, context):
        layout = self.layout

        view = context.space_data
        shading = view.shading
        overlay = view.overlay
        tool_settings = context.tool_settings
        display_all = overlay.show_overlays
        statvis = tool_settings.statvis

        col = layout.column()
        col.active = display_all

        row = col.row(align=True)
        row.prop(overlay, "show_retopology", text="")
        sub = row.row()
        sub.active = overlay.show_retopology
        sub.prop(overlay, "retopology_offset", text="Retopology")

        col.prop(overlay, "show_weight", text="Vertex Group Weights")
        if overlay.show_weight:
            row = col.split(factor=0.33)
            row.label(text="Zero Weights")
            sub = row.row()
            sub.prop(tool_settings, "vertex_group_user", expand=True)

        if shading.type == 'WIREFRAME':
            xray = shading.show_xray_wireframe and shading.xray_alpha_wireframe < 1.0
        elif shading.type == 'SOLID':
            xray = shading.show_xray and shading.xray_alpha < 1.0
        else:
            xray = False
        statvis_active = not xray
        row = col.row()
        row.active = statvis_active
        row.prop(overlay, "show_statvis")
        if overlay.show_statvis:
            col = col.column()
            col.active = statvis_active

            sub = col.split()
            sub.label(text="Type")
            sub.prop(statvis, "type", text="")

            statvis_type = statvis.type
            if statvis_type == 'OVERHANG':
                row = col.row(align=True)
                row.prop(statvis, "overhang_min", text="Minimum")
                row.prop(statvis, "overhang_max", text="Maximum")
                col.row().prop(statvis, "overhang_axis", expand=True)
            elif statvis_type == 'THICKNESS':
                row = col.row(align=True)
                row.prop(statvis, "thickness_min", text="Minimum")
                row.prop(statvis, "thickness_max", text="Maximum")
                col.prop(statvis, "thickness_samples")
            elif statvis_type == 'INTERSECT':
                pass
            elif statvis_type == 'DISTORT':
                row = col.row(align=True)
                row.prop(statvis, "distort_min", text="Minimum")
                row.prop(statvis, "distort_max", text="Maximum")
            elif statvis_type == 'SHARP':
                row = col.row(align=True)
                row.prop(statvis, "sharp_min", text="Minimum")
                row.prop(statvis, "sharp_max", text="Maximum")


class VIEW3D_PT_overlay_edit_mesh_measurement(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay_edit_mesh"
    bl_label = "Measurement"

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def draw(self, context):
        layout = self.layout

        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays

        col = layout.column()
        col.active = display_all

        split = col.split()

        sub = split.column()
        sub.prop(overlay, "show_extra_edge_length", text="Edge Length")
        sub.prop(overlay, "show_extra_edge_angle", text="Edge Angle")

        sub = split.column()
        sub.prop(overlay, "show_extra_face_area", text="Face Area")
        sub.prop(overlay, "show_extra_face_angle", text="Face Angle")


class VIEW3D_PT_overlay_edit_mesh_normals(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay_edit_mesh"
    bl_label = "Normals"

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def draw(self, context):
        layout = self.layout

        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays

        col = layout.column()
        col.active = display_all

        row = col.row(align=True)
        row.prop(overlay, "show_vertex_normals", text="", icon='NORMALS_VERTEX')
        row.prop(overlay, "show_split_normals", text="", icon='NORMALS_VERTEX_FACE')
        row.prop(overlay, "show_face_normals", text="", icon='NORMALS_FACE')

        sub = row.row(align=True)
        sub.active = overlay.show_vertex_normals or overlay.show_face_normals or overlay.show_split_normals
        if overlay.use_normals_constant_screen_size:
            sub.prop(overlay, "normals_constant_screen_size", text="Size")
        else:
            sub.prop(overlay, "normals_length", text="Size")

        row.prop(overlay, "use_normals_constant_screen_size", text="", icon='FIXED_SIZE')


class VIEW3D_PT_overlay_edit_mesh_freestyle(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay_edit_mesh"
    bl_label = "Freestyle"

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH' and bpy.app.build_options.freestyle

    def draw(self, context):
        layout = self.layout

        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays

        col = layout.column()
        col.active = display_all

        row = col.row()
        row.prop(overlay, "show_freestyle_edge_marks", text="Edge Marks")
        row.prop(overlay, "show_freestyle_face_marks", text="Face Marks")


class VIEW3D_PT_overlay_edit_curve(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Curve Edit Mode"

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_CURVE'

    def draw(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays

        layout.label(text="Curve Edit Mode Overlays")

        col = layout.column()
        col.active = display_all

        row = col.row()
        row.prop(overlay, "display_handle", text="Handles")

        row = col.row()
        row.prop(overlay, "show_curve_normals", text="")
        sub = row.row()
        sub.active = overlay.show_curve_normals
        sub.prop(overlay, "normals_length", text="Normals")


class VIEW3D_PT_overlay_edit_curves(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Curves Edit Mode"

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_CURVES'

    def draw(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays

        layout.label(text="Curves Edit Mode Overlays")

        col = layout.column()
        col.active = display_all

        row = col.row()
        row.prop(overlay, "display_handle", text="Handles")


class VIEW3D_PT_overlay_sculpt(Panel):
    bl_space_type = 'VIEW_3D'
    bl_context = ".sculpt_mode"
    bl_region_type = 'HEADER'
    bl_label = "Sculpt"

    @classmethod
    def poll(cls, context):
        return context.mode == 'SCULPT'

    def draw(self, context):
        prefs = context.preferences
        layout = self.layout

        sculpt = context.scene.tool_settings.sculpt
        view = context.space_data
        overlay = view.overlay

        layout.label(text="Sculpt Mode Overlays")

        row = layout.row(align=True)
        row.prop(overlay, "show_sculpt_mask", text="")
        sub = row.row()
        sub.active = overlay.show_sculpt_mask
        sub.prop(overlay, "sculpt_mode_mask_opacity", text="Mask")

        row = layout.row(align=True)
        row.prop(overlay, "show_sculpt_face_sets", text="")
        sub = row.row()
        sub.active = overlay.show_sculpt_face_sets
        row.prop(overlay, "sculpt_mode_face_sets_opacity", text="Face Sets")

        use_debug = prefs.experimental.use_paint_debug and prefs.view.show_developer_ui
        if use_debug:
            row = layout.row(align=True)
            row.prop(sculpt, "show_bvh_nodes")


class VIEW3D_PT_overlay_sculpt_curves(Panel):
    bl_space_type = 'VIEW_3D'
    bl_context = ".curves_sculpt"
    bl_region_type = 'HEADER'
    bl_label = "Sculpt"

    @classmethod
    def poll(cls, context):
        return context.mode == 'SCULPT_CURVES'

    def draw(self, context):
        layout = self.layout

        view = context.space_data
        overlay = view.overlay

        layout.label(text="Curve Sculpt Overlays")

        row = layout.row(align=True)
        row.active = overlay.show_overlays
        row.prop(overlay, "sculpt_mode_mask_opacity", text="Selection Opacity")

        row = layout.row(align=True)
        row.active = overlay.show_overlays
        row.prop(overlay, "show_sculpt_curves_cage", text="")
        subrow = row.row(align=True)
        subrow.active = overlay.show_sculpt_curves_cage
        subrow.prop(overlay, "sculpt_curves_cage_opacity", text="Cage Opacity")


class VIEW3D_PT_overlay_vertex_paint(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Vertex Paint"

    @classmethod
    def poll(cls, context):
        return context.mode == 'PAINT_VERTEX'

    def draw(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay
        display_all = overlay.show_overlays

        layout.label(text="Vertex Paint Overlays")

        col = layout.column()
        col.active = display_all

        col.prop(overlay, "vertex_paint_mode_opacity")
        col.prop(overlay, "show_paint_wire")


class VIEW3D_PT_snapping(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Snapping"

    def draw(self, context):
        tool_settings = context.tool_settings
        obj = context.active_object
        object_mode = 'OBJECT' if obj is None else obj.mode

        layout = self.layout
        col = layout.column()

        col.label(text="Snap Base")
        row = col.row(align=True)
        row.prop(tool_settings, "snap_target", expand=True)

        col.label(text="Snap Target")
        col.prop(tool_settings, "snap_elements_base", expand=True)

        col.label(text="Snap Target for Individual Elements")
        col.prop(tool_settings, "snap_elements_individual", expand=True)

        col.separator()

        if 'INCREMENT' in tool_settings.snap_elements:
            col.prop(tool_settings, "use_snap_grid_absolute")

        if 'VOLUME' in tool_settings.snap_elements:
            col.prop(tool_settings, "use_snap_peel_object")

        if 'FACE_NEAREST' in tool_settings.snap_elements:
            col.prop(tool_settings, "use_snap_to_same_target")
            if object_mode == 'EDIT':
                col.prop(tool_settings, "snap_face_nearest_steps")

        col.separator()

        col.prop(tool_settings, "use_snap_align_rotation")
        col.prop(tool_settings, "use_snap_backface_culling")

        col.separator()

        if obj:
            col.label(text="Target Selection")
            col_targetsel = col.column(align=True)
            if object_mode == 'EDIT' and obj.type not in {'LATTICE', 'META', 'FONT'}:
                col_targetsel.prop(
                    tool_settings,
                    "use_snap_self",
                    text="Include Active",
                    icon='EDITMODE_HLT',
                )
                col_targetsel.prop(
                    tool_settings,
                    "use_snap_edit",
                    text="Include Edited",
                    icon='OUTLINER_DATA_MESH',
                )
                col_targetsel.prop(
                    tool_settings,
                    "use_snap_nonedit",
                    text="Include Non-Edited",
                    icon='OUTLINER_OB_MESH',
                )
            col_targetsel.prop(
                tool_settings,
                "use_snap_selectable",
                text="Exclude Non-Selectable",
                icon='RESTRICT_SELECT_OFF',
            )

        col.label(text="Affect")
        row = col.row(align=True)
        row.prop(
            tool_settings,
            "use_snap_translate",
            text="Move",
            text_ctxt=i18n_contexts.operator_default,
            toggle=True,
        )
        row.prop(
            tool_settings,
            "use_snap_rotate",
            text="Rotate",
            text_ctxt=i18n_contexts.operator_default,
            toggle=True,
        )
        row.prop(
            tool_settings,
            "use_snap_scale",
            text="Scale",
            text_ctxt=i18n_contexts.operator_default,
            toggle=True,
        )
        col.label(text="Rotation Increment")
        row = col.row(align=True)
        row.prop(tool_settings, "snap_angle_increment_3d", text="")
        row.prop(tool_settings, "snap_angle_increment_3d_precision", text="")


class VIEW3D_PT_sculpt_snapping(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Snapping"

    def draw(self, context):
        layout = self.layout
        tool_settings = context.tool_settings
        col = layout.column()

        col.label(text="Rotation Increment")
        row = col.row(align=True)
        row.prop(tool_settings, "snap_angle_increment_3d", text="")


class VIEW3D_PT_proportional_edit(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Proportional Editing"
    bl_ui_units_x = 8

    def draw(self, context):
        layout = self.layout
        tool_settings = context.tool_settings
        col = layout.column()
        col.active = (
            tool_settings.use_proportional_edit_objects if context.mode == 'OBJECT' else
            tool_settings.use_proportional_edit
        )

        if context.mode != 'OBJECT':
            col.prop(tool_settings, "use_proportional_connected")
            sub = col.column()
            sub.active = not tool_settings.use_proportional_connected
            sub.prop(tool_settings, "use_proportional_projected")
            col.separator()

        col.prop(tool_settings, "proportional_edit_falloff", expand=True)
        col.prop(tool_settings, "proportional_distance")


class VIEW3D_PT_transform_orientations(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Transform Orientations"
    bl_ui_units_x = 8

    def draw(self, context):
        layout = self.layout
        layout.label(text="Transform Orientations")

        scene = context.scene
        orient_slot = scene.transform_orientation_slots[0]
        orientation = orient_slot.custom_orientation

        row = layout.row()
        col = row.column()
        col.prop(orient_slot, "type", expand=True)
        row.operator("transform.create_orientation", text="", icon='ADD', emboss=False).use = True

        if orientation:
            row = layout.row(align=False)
            row.prop(orientation, "name", text="", icon='OBJECT_ORIGIN')
            row.operator("transform.delete_orientation", text="", icon='X', emboss=False)


class VIEW3D_PT_quad_view(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "View"
    bl_label = "Quad View"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        view = context.space_data
        return view.region_quadviews

    def draw(self, context):
        layout = self.layout

        view = context.space_data

        region = view.region_quadviews[2]
        col = layout.column()
        col.prop(region, "lock_rotation")
        row = col.row()
        row.enabled = region.lock_rotation
        row.prop(region, "show_sync_view")
        row = col.row()
        row.enabled = region.lock_rotation and region.show_sync_view
        row.prop(region, "use_box_clip")


    # NOTE: this is just a wrapper around the generic GP Panel


    # NOTE: this is just a wrapper around the generic GP Panel


class VIEW3D_PT_view3d_stereo(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "View"
    bl_label = "Stereoscopy"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        scene = context.scene

        multiview = scene.render.use_multiview
        return multiview

    def draw(self, context):
        layout = self.layout
        view = context.space_data

        basic_stereo = context.scene.render.views_format == 'STEREO_3D'

        col = layout.column()
        col.row().prop(view, "stereo_3d_camera", expand=True)

        col.label(text="Display")
        row = col.row()
        row.active = basic_stereo
        row.prop(view, "show_stereo_3d_cameras")
        row = col.row()
        row.active = basic_stereo
        split = row.split()
        split.prop(view, "show_stereo_3d_convergence_plane")
        split = row.split()
        split.prop(view, "stereo_3d_convergence_plane_alpha", text="Alpha")
        split.active = view.show_stereo_3d_convergence_plane
        row = col.row()
        split = row.split()
        split.prop(view, "show_stereo_3d_volume")
        split = row.split()
        split.active = view.show_stereo_3d_volume
        split.prop(view, "stereo_3d_volume_alpha", text="Alpha")


class VIEW3D_PT_context_properties(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Item"
    bl_label = "Properties"
    bl_options = {'DEFAULT_CLOSED'}

    @staticmethod
    def _active_context_member(context):
        obj = context.object
        if obj:
            object_mode = obj.mode
            return "object"

        return ""

    @classmethod
    def poll(cls, context):
        import rna_prop_ui
        member = cls._active_context_member(context)

        if member:
            context_member, member = rna_prop_ui.rna_idprop_context_value(context, member, object)
            return context_member and rna_prop_ui.rna_idprop_has_properties(context_member)

        return False

    def draw(self, context):
        import rna_prop_ui
        member = VIEW3D_PT_context_properties._active_context_member(context)

        if member:
            # Draw with no edit button
            rna_prop_ui.draw(self.layout, context, member, object, use_edit=False)


class VIEW3D_PT_active_spline(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Item"
    bl_label = "Active Spline"

    @classmethod
    def poll(cls, context):
        ob = context.object
        if ob is None or ob.type not in {'CURVE', 'SURFACE'} or ob.mode != 'EDIT':
            return False
        curve = ob.data
        return curve.splines.active is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        curve = context.object.data
        act_spline = curve.splines.active
        is_surf = type(curve) is SurfaceCurve
        is_poly = (act_spline.type == 'POLY')

        col = layout.column()

        if is_poly:
            # These settings are below but its easier to have
            # polys set aside since they use so few settings

            col.prop(act_spline, "use_cyclic_u")
            col.prop(act_spline, "use_smooth")
        else:

            sub = col.column(heading="Cyclic", align=True)
            sub.prop(act_spline, "use_cyclic_u", text="U")
            if is_surf:
                sub.prop(act_spline, "use_cyclic_v", text="V")

            if act_spline.type == 'NURBS':
                sub = col.column(heading="Bézier", align=True)
                # sub.active = (not act_spline.use_cyclic_u)
                sub.prop(act_spline, "use_bezier_u", text="U")

                if is_surf:
                    subsub = sub.column()
                    subsub.prop(act_spline, "use_bezier_v", text="V")

                sub = col.column(heading="Endpoint", align=True)
                sub.prop(act_spline, "use_endpoint_u", text="U")

                if is_surf:
                    subsub = sub.column()
                    subsub.prop(act_spline, "use_endpoint_v", text="V")

                sub = col.column(align=True)
                sub.prop(act_spline, "order_u", text="Order U")

                if is_surf:
                    sub.prop(act_spline, "order_v", text="V")

            sub = col.column(align=True)
            sub.prop(act_spline, "resolution_u", text="Resolution U")
            if is_surf:
                sub.prop(act_spline, "resolution_v", text="V")

            if act_spline.type == 'BEZIER':

                col.separator()

                sub = col.column()
                sub.active = (curve.dimensions == '3D')
                sub.prop(act_spline, "tilt_interpolation", text="Interpolation Tilt")

                col.prop(act_spline, "radius_interpolation", text="Radius")

            layout.prop(act_spline, "use_smooth")
            if act_spline.type == 'NURBS':
                col = None
                for direction in range(2):
                    message = act_spline.valid_message(direction)
                    if not message:
                        continue
                    if col is None:
                        layout.separator()
                        col = layout.column(align=True)
                    col.label(text=message, icon='INFO')
                del col


class VIEW3D_PT_paint_vertex_context_menu(Panel):
    # Only for popover, these are dummy values.
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'WINDOW'
    bl_label = "Vertex Paint"

    def draw(self, context):
        layout = self.layout

        brush = context.tool_settings.vertex_paint.brush
        capabilities = brush.vertex_paint_capabilities

        if capabilities.has_color:
            split = layout.split(factor=0.1)
            UnifiedPaintPanel.prop_unified_color(split, context, brush, "color", text="")
            UnifiedPaintPanel.prop_unified_color_picker(split, context, brush, "color", value_slider=True)
            layout.prop(brush, "blend", text="")

        UnifiedPaintPanel.prop_unified(
            layout,
            context,
            brush,
            "size",
            unified_name="use_unified_size",
            pressure_name="use_pressure_size",
            slider=True,
        )
        UnifiedPaintPanel.prop_unified(
            layout,
            context,
            brush,
            "strength",
            unified_name="use_unified_strength",
            pressure_name="use_pressure_strength",
            slider=True,
        )


class VIEW3D_PT_sculpt_automasking(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Auto-Masking"
    bl_ui_units_x = 10

    def draw(self, context):
        layout = self.layout

        tool_settings = context.tool_settings
        paint = tool_settings.sculpt

        settings = paint.mesh_automasking_settings

        layout.label(text="Auto-Masking")

        col = layout.column(align=True)
        col.prop(settings, "use_automasking_topology", text="Topology")
        col.prop(settings, "use_automasking_face_sets", text="Face Sets")

        col.separator()

        col = layout.column(align=True)
        row = col.row()
        row.prop(settings, "use_automasking_boundary_edges", text="Mesh Boundary")

        if settings.use_automasking_boundary_edges:
            props = row.operator("sculpt.mask_from_boundary", text="Create Mask")
            props.settings_source = 'SCENE'
            props.boundary_mode = 'MESH'

        row = col.row()
        row.prop(settings, "use_automasking_boundary_face_sets", text="Face Sets Boundary")

        if settings.use_automasking_boundary_face_sets:
            props = row.operator("sculpt.mask_from_boundary", text="Create Mask")
            props.settings_source = 'SCENE'
            props.boundary_mode = 'FACE_SETS'

        if settings.use_automasking_boundary_edges or settings.use_automasking_boundary_face_sets:
            col.prop(settings, "boundary_edges_propagation_steps")

        col.separator()

        col = layout.column(align=True)
        row = col.row()
        row.prop(settings, "use_automasking_cavity", text="Cavity")

        is_cavity_active = settings.use_automasking_cavity or settings.use_automasking_cavity_inverted

        if is_cavity_active:
            props = row.operator("sculpt.mask_from_cavity", text="Create Mask")
            props.settings_source = 'SCENE'

        col.prop(settings, "use_automasking_cavity_inverted", text="Cavity (inverted)")

        if is_cavity_active:
            col = layout.column(align=True)
            col.prop(settings, "cavity_factor", text="Factor")
            col.prop(settings, "cavity_blur_steps", text="Blur")

            col = layout.column()
            col.prop(settings, "use_automasking_custom_cavity_curve", text="Custom Curve")

            if settings.use_automasking_custom_cavity_curve:
                col.template_curve_mapping(settings, "cavity_curve", brush=True)

        col.separator()

        col = layout.column(align=True)
        col.prop(settings, "use_automasking_view_normal", text="View Normal")

        if settings.use_automasking_view_normal:
            col.prop(settings, "use_automasking_view_occlusion", text="Occlusion")
            subcol = col.column(align=True)
            subcol.active = not settings.use_automasking_view_occlusion
            subcol.prop(settings, "view_normal_limit", text="Limit")
            subcol.prop(settings, "view_normal_falloff", text="Falloff")

        col = layout.column()
        col.prop(settings, "use_automasking_start_normal", text="Area Normal")

        if settings.use_automasking_start_normal:
            col = layout.column(align=True)
            col.prop(settings, "start_normal_limit", text="Limit")
            col.prop(settings, "start_normal_falloff", text="Falloff")


class VIEW3D_PT_sculpt_context_menu(Panel):
    # Only for popover, these are dummy values.
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'WINDOW'
    bl_label = "Sculpt"

    def draw(self, context):
        layout = self.layout

        paint = context.tool_settings.sculpt
        brush = paint.brush
        capabilities = brush.sculpt_capabilities

        if capabilities.has_color:
            split = layout.split(factor=0.1)
            UnifiedPaintPanel.prop_unified_color(split, context, brush, "color", text="")
            UnifiedPaintPanel.prop_unified_color_picker(split, context, brush, "color", value_slider=True)
            layout.prop(brush, "blend", text="")

        ups = paint.unified_paint_settings
        size = "size"
        size_owner = ups if ups.use_unified_size else brush
        if size_owner.use_locked_size == 'SCENE':
            size = "unprojected_size"

        UnifiedPaintPanel.prop_unified(
            layout,
            context,
            brush,
            size,
            unified_paint_settings_override=ups,
            unified_name="use_unified_size",
            pressure_name="use_pressure_size",
            text="Size",
            slider=True,
        )
        UnifiedPaintPanel.prop_unified(
            layout,
            context,
            brush,
            "strength",
            unified_paint_settings_override=ups,
            unified_name="use_unified_strength",
            pressure_name="use_pressure_strength",
            slider=True,
        )

        if capabilities.has_auto_smooth:
            layout.prop(brush, "auto_smooth_factor", slider=True)

        if capabilities.has_normal_weight:
            layout.prop(brush, "normal_weight", slider=True)

        if capabilities.has_pinch_factor:
            text = "Pinch"
            if brush.sculpt_brush_type in {'BLOB', 'SNAKE_HOOK'}:
                text = "Magnify"
            layout.prop(brush, "crease_pinch_factor", slider=True, text=text)

        if capabilities.has_rake_factor:
            layout.prop(brush, "rake_factor", slider=True)

        if capabilities.has_plane_offset:
            layout.prop(brush, "plane_offset", slider=True)
            layout.prop(brush, "plane_trim", slider=True, text="Distance")

        if capabilities.has_height:
            layout.prop(brush, "height", slider=True, text="Height")


class VIEW3D_PT_curves_sculpt_add_shape(Panel):
    # Only for popover, these are dummy values.
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'WINDOW'
    bl_label = "Curves Sculpt Add Curve Options"

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        settings = UnifiedPaintPanel.paint_settings(context)
        brush = settings.brush

        col = layout.column(heading="Interpolate", align=True)
        col.prop(brush.curves_sculpt_settings, "use_length_interpolate", text="Length")
        col.prop(brush.curves_sculpt_settings, "use_radius_interpolate", text="Radius")
        col.prop(brush.curves_sculpt_settings, "use_shape_interpolate", text="Shape")
        col.prop(brush.curves_sculpt_settings, "use_point_count_interpolate", text="Point Count")

        col = layout.column()
        col.active = not brush.curves_sculpt_settings.use_length_interpolate
        col.prop(brush.curves_sculpt_settings, "curve_length", text="Length")

        col = layout.column()
        col.active = not brush.curves_sculpt_settings.use_radius_interpolate
        col.prop(brush.curves_sculpt_settings, "curve_radius", text="Radius")

        col = layout.column()
        col.active = not brush.curves_sculpt_settings.use_point_count_interpolate
        col.prop(brush.curves_sculpt_settings, "points_per_curve", text="Points")


class VIEW3D_PT_curves_sculpt_parameter_falloff(Panel):
    # Only for popover, these are dummy values.
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'WINDOW'
    bl_label = "Curves Sculpt Parameter Falloff"

    def draw(self, context):
        layout = self.layout

        settings = UnifiedPaintPanel.paint_settings(context)
        brush = settings.brush

        layout.template_curve_mapping(
            brush.curves_sculpt_settings,
            "curve_parameter_falloff",
            brush=True,
            show_presets=True,
        )


class VIEW3D_PT_curves_sculpt_grow_shrink_scaling(Panel):
    # Only for popover, these are dummy values.
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'WINDOW'
    bl_label = "Curves Grow/Shrink Scaling"
    bl_ui_units_x = 12

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        settings = UnifiedPaintPanel.paint_settings(context)
        brush = settings.brush

        layout.prop(brush.curves_sculpt_settings, "use_uniform_scale")
        layout.prop(brush.curves_sculpt_settings, "minimum_length")


class VIEW3D_PT_viewport_debug(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "VIEW3D_PT_overlay"
    bl_label = "Viewport Debug"

    @classmethod
    def poll(cls, context):
        prefs = context.preferences
        return prefs.experimental.use_viewport_debug

    def draw(self, context):
        layout = self.layout
        view = context.space_data
        overlay = view.overlay

        layout.prop(overlay, "use_debug_freeze_view_culling")


class View3DAssetShelf(BrushAssetShelf):
    bl_space_type = "VIEW_3D"


class VIEW3D_AST_brush_sculpt(View3DAssetShelf, bpy.types.AssetShelf):
    mode = 'SCULPT'
    mode_prop = "use_paint_sculpt"
    brush_type_prop = "sculpt_brush_type"


class VIEW3D_AST_brush_sculpt_curves(View3DAssetShelf, bpy.types.AssetShelf):
    mode = 'SCULPT_CURVES'
    mode_prop = "use_paint_sculpt_curves"
    brush_type_prop = "curves_sculpt_brush_type"


class VIEW3D_AST_brush_vertex_paint(View3DAssetShelf, bpy.types.AssetShelf):
    mode = 'VERTEX_PAINT'
    mode_prop = "use_paint_vertex"
    brush_type_prop = "vertex_brush_type"


class VIEW3D_PT_nurbs_arc_overlay(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Arc Overlays"
    bl_ui_units_x = 14

    def draw(self, context):
        tool = context.workspace.tools.from_space_view3d_mode(context.mode, create=False)
        if tool is None or tool.idname != "builtin.nurbs_draw_arc":
            return
        props = tool.operator_properties("curve.draw_arc")
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        if props.mode == 'CENTER':
            layout.prop(props, "show_compass")
            row = layout.row()
            row.active = props.show_compass
            row.prop(props, "compass_size")
        layout.prop(props, "use_axis_colors")
        row = layout.row()
        row.active = props.use_axis_colors
        row.prop(props, "axis_color_dim")
        if props.mode == 'CENTER':
            layout.prop(props, "start_color")
            layout.prop(props, "end_color")
        else:
            layout.prop(props, "overlay_color")
        layout.separator()
        layout.prop(props, "preview_points")
        layout.prop(props, "preview_point_size")
        layout.prop(props, "show_measurements")


class VIEW3D_PT_nurbs_circle_overlay(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Circle Overlays"
    bl_ui_units_x = 14

    def draw(self, context):
        tool = context.workspace.tools.from_space_view3d_mode(context.mode, create=False)
        if tool is None or tool.idname != "builtin.nurbs_draw_circle":
            return
        props = tool.operator_properties("curve.draw_circle")
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        if props.mode in {'TAN_TAN', 'TAN_TAN_TAN'}:
            layout.prop(props, "show_source_curves")
            layout.prop(props, "tangent_color")
            layout.prop(props, "source_width")
            layout.prop(props, "show_tangents")
            layout.prop(props, "anchor_tangent_markers")
        else:
            layout.prop(props, "use_axis_colors")
            layout.prop(props, "axis_color_dim")
            layout.prop(props, "overlay_color")
        layout.prop(props, "preview_points")
        layout.prop(props, "preview_point_size")
        layout.prop(props, "show_measurements")


class VIEW3D_PT_nurbs_shape_overlay(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Drawing Overlays"
    bl_ui_units_x = 14

    def draw(self, context):
        tool = context.workspace.tools.from_space_view3d_mode(context.mode, create=False)
        if tool is None or tool.idname not in {
            "builtin.nurbs_draw_ellipse", "builtin.nurbs_draw_polygon", "builtin.nurbs_draw_rectangle",
        }:
            return
        props = tool.operator_properties("curve.draw_" + tool.idname.removeprefix("builtin.nurbs_draw_"))
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop(props, "use_axis_colors")
        layout.prop(props, "axis_color_dim")
        layout.prop(props, "overlay_color")
        layout.prop(props, "preview_point_size")
        layout.prop(props, "show_measurements")


classes = (
    VIEW3D_PT_nurbs_shape_overlay,
    VIEW3D_PT_nurbs_circle_overlay,
    VIEW3D_PT_nurbs_arc_overlay,
    VIEW3D_HT_header,
    VIEW3D_HT_tool_header,
    VIEW3D_MT_editor_menus,
    VIEW3D_MT_select_edit_nurbs,
    VIEW3D_MT_transform,
    VIEW3D_MT_transform_object,
    VIEW3D_MT_mirror,
    VIEW3D_MT_snap,
    VIEW3D_MT_uv_map,
    VIEW3D_MT_view,
    VIEW3D_MT_view_local,
    VIEW3D_MT_view_cameras,
    VIEW3D_MT_view_navigation,
    VIEW3D_MT_view_align,
    VIEW3D_MT_view_align_selected,
    VIEW3D_MT_view_viewpoint,
    VIEW3D_MT_view_regions,
    VIEW3D_MT_select_object,
    VIEW3D_MT_select_particle,
    VIEW3D_MT_edit_mesh,
    VIEW3D_MT_edit_mesh_select_similar,
    VIEW3D_MT_edit_mesh_select_by_trait,
    VIEW3D_MT_edit_mesh_select_more_less,
    VIEW3D_MT_select_edit_mesh,
    VIEW3D_MT_select_edit_curve,
    VIEW3D_MT_select_edit_surface,
    VIEW3D_MT_select_edit_text,
    VIEW3D_MT_edit_lattice_context_menu,
    VIEW3D_MT_select_edit_lattice,
    VIEW3D_MT_select_paint_mask,
    VIEW3D_MT_select_paint_mask_vertex,
    VIEW3D_MT_select_edit_pointcloud,
    VIEW3D_MT_edit_curves_select_more_less,
    VIEW3D_MT_select_edit_curves,
    VIEW3D_MT_select_sculpt_curves,
    VIEW3D_MT_mesh_add,
    VIEW3D_MT_curve_add,
    VIEW3D_MT_surface_add,
    TOPBAR_MT_edit_curve_add,
    VIEW3D_MT_light_add,
    VIEW3D_MT_lightprobe_add,
    VIEW3D_MT_camera_add,
    VIEW3D_MT_volume_add,
    VIEW3D_MT_lattice_add,
    VIEW3D_MT_add,
    VIEW3D_MT_image_add,
    VIEW3D_MT_object,
    VIEW3D_MT_object_animation,
    VIEW3D_MT_object_asset,
    VIEW3D_MT_object_rigid_body,
    VIEW3D_MT_object_clear,
    VIEW3D_MT_object_context_menu,
    VIEW3D_MT_object_convert,
    VIEW3D_MT_object_shading,
    VIEW3D_MT_object_apply,
    VIEW3D_MT_object_relations,
    VIEW3D_MT_object_liboverride,
    VIEW3D_MT_object_track,
    VIEW3D_MT_object_collection,
    VIEW3D_MT_object_constraints,
    VIEW3D_MT_object_modifiers,
    VIEW3D_MT_object_quick_effects,
    VIEW3D_MT_object_showhide,
    VIEW3D_MT_object_cleanup,
    VIEW3D_MT_make_single_user,
    VIEW3D_MT_make_links,
    VIEW3D_MT_paint_vertex,
    VIEW3D_MT_hook,
    VIEW3D_MT_vertex_group,
    VIEW3D_MT_vertex_group_weights,
    VIEW3D_MT_vertex_group_lock,
    VIEW3D_MT_sculpt,
    VIEW3D_MT_sculpt_set_pivot,
    VIEW3D_MT_sculpt_transform,
    VIEW3D_MT_sculpt_showhide,
    VIEW3D_MT_sculpt_trim,
    VIEW3D_MT_add_object,
    VIEW3D_MT_mask,
    VIEW3D_MT_face_sets,
    VIEW3D_MT_face_sets_init,
    VIEW3D_MT_random_mask,
    VIEW3D_MT_particle,
    VIEW3D_MT_particle_context_menu,
    VIEW3D_MT_particle_showhide,
    VIEW3D_MT_edit_mesh_context_menu,
    VIEW3D_MT_edit_mesh_select_mode,
    VIEW3D_MT_edit_mesh_select_linked,
    VIEW3D_MT_edit_mesh_select_loops,
    VIEW3D_MT_edit_mesh_extrude,
    VIEW3D_MT_edit_mesh_vertices,
    VIEW3D_MT_edit_mesh_edges,
    VIEW3D_MT_edit_mesh_faces,
    VIEW3D_MT_edit_mesh_faces_data,
    VIEW3D_MT_edit_mesh_normals,
    VIEW3D_MT_edit_mesh_normals_select_strength,
    VIEW3D_MT_edit_mesh_normals_set_strength,
    VIEW3D_MT_edit_mesh_normals_average,
    VIEW3D_MT_edit_mesh_shading,
    VIEW3D_MT_edit_mesh_weights,
    VIEW3D_MT_edit_mesh_clean,
    VIEW3D_MT_edit_mesh_delete,
    VIEW3D_MT_edit_mesh_merge,
    VIEW3D_MT_edit_mesh_split,
    VIEW3D_MT_edit_mesh_showhide,
    VIEW3D_MT_edit_curve,
    VIEW3D_MT_edit_curve_ctrlpoints,
    VIEW3D_MT_edit_curve_segments,
    VIEW3D_MT_edit_curve_clean,
    VIEW3D_MT_edit_curve_context_menu,
    VIEW3D_MT_edit_nurbs_merge,
    VIEW3D_MT_edit_nurbs,
    VIEW3D_MT_edit_nurbs_transform,
    VIEW3D_MT_edit_nurbs_snap,
    VIEW3D_MT_edit_nurbs_clean,
    VIEW3D_MT_edit_nurbs_delete,
    VIEW3D_MT_edit_curve_delete,
    VIEW3D_MT_edit_curve_showhide,
    VIEW3D_MT_edit_surface,
    VIEW3D_MT_edit_font,
    VIEW3D_MT_edit_font_chars,
    VIEW3D_MT_edit_font_kerning,
    VIEW3D_MT_edit_font_delete,
    VIEW3D_MT_edit_font_context_menu,
    VIEW3D_MT_edit_lattice,
    VIEW3D_MT_edit_curves,
    VIEW3D_MT_edit_curves_add,
    VIEW3D_MT_edit_curves_segments,
    VIEW3D_MT_edit_curves_control_points,
    VIEW3D_MT_edit_curves_context_menu,
    VIEW3D_MT_edit_pointcloud,
    VIEW3D_MT_object_mode_pie,
    VIEW3D_MT_view_pie,
    VIEW3D_MT_transform_gizmo_pie,
    VIEW3D_MT_shading_pie,
    VIEW3D_MT_shading_ex_pie,
    VIEW3D_MT_pivot_pie,
    VIEW3D_MT_snap_pie,
    VIEW3D_MT_orientations_pie,
    VIEW3D_MT_proportional_editing_falloff_pie,
    VIEW3D_MT_sculpt_mask_edit_pie,
    VIEW3D_MT_sculpt_automasking_pie,
    VIEW3D_MT_sculpt_face_sets_edit_pie,
    VIEW3D_MT_sculpt_curves,
    VIEW3D_PT_active_tool,
    VIEW3D_PT_active_tool_duplicate,
    VIEW3D_PT_view3d_properties,
    VIEW3D_PT_view3d_lock,
    VIEW3D_PT_view3d_cursor,
    VIEW3D_PT_collections,
    VIEW3D_PT_object_type_visibility,
    VIEW3D_PT_quad_view,
    VIEW3D_PT_view3d_stereo,
    VIEW3D_PT_shading,
    VIEW3D_PT_shading_lighting,
    VIEW3D_PT_shading_color,
    VIEW3D_PT_shading_options,
    VIEW3D_PT_shading_options_shadow,
    VIEW3D_PT_shading_options_ssao,
    VIEW3D_PT_shading_cavity,
    VIEW3D_PT_shading_render_pass,
    VIEW3D_PT_shading_compositor,
    VIEW3D_PT_gizmo_display,
    VIEW3D_PT_overlay,
    VIEW3D_PT_overlay_guides,
    VIEW3D_PT_overlay_text,
    VIEW3D_PT_overlay_object,
    VIEW3D_PT_overlay_geometry,
    VIEW3D_PT_overlay_viewer_node,
    VIEW3D_PT_overlay_motion_tracking,
    VIEW3D_PT_overlay_edit_mesh,
    VIEW3D_PT_overlay_edit_mesh_shading,
    VIEW3D_PT_overlay_edit_mesh_measurement,
    VIEW3D_PT_overlay_edit_mesh_normals,
    VIEW3D_PT_overlay_edit_mesh_freestyle,
    VIEW3D_PT_overlay_edit_curve,
    VIEW3D_PT_overlay_edit_curves,
    VIEW3D_PT_overlay_vertex_paint,
    VIEW3D_PT_overlay_sculpt,
    VIEW3D_PT_overlay_sculpt_curves,
    VIEW3D_PT_snapping,
    VIEW3D_PT_sculpt_snapping,
    VIEW3D_PT_proportional_edit,
    VIEW3D_PT_transform_orientations,
    VIEW3D_PT_context_properties,
    VIEW3D_PT_paint_vertex_context_menu,
    VIEW3D_PT_sculpt_automasking,
    VIEW3D_PT_sculpt_context_menu,
    VIEW3D_PT_curves_sculpt_add_shape,
    VIEW3D_PT_curves_sculpt_parameter_falloff,
    VIEW3D_PT_curves_sculpt_grow_shrink_scaling,
    VIEW3D_PT_viewport_debug,
    VIEW3D_PT_active_spline,
    VIEW3D_AST_brush_sculpt,
    VIEW3D_AST_brush_sculpt_curves,
    VIEW3D_AST_brush_vertex_paint,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
