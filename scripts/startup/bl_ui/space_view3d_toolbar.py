# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Menu, Panel, WindowManager
from bpy.app.translations import (
    pgettext_iface as iface_,
    contexts as i18n_contexts,
)

from bl_ui.properties_paint_common import (
    UnifiedPaintPanel,
    BrushSelectPanel,
    ColorPalettePanel,
    StrokePanel,
    SmoothStrokePanel,
    FalloffPanel,
    DisplayPanel,
    brush_texture_settings,
    brush_settings,
    brush_settings_advanced,
    draw_color_settings,
)
from bl_ui.utils import PresetPanel


class VIEW3D_MT_brush_context_menu(Menu):
    bl_label = "Brush Specials"

    def draw(self, context):
        layout = self.layout

        settings = UnifiedPaintPanel.paint_settings(context)
        brush = getattr(settings, "brush", None)

        # skip if no active brush
        if not brush:
            layout.label(text="No brush selected", icon='INFO')
            return

        if brush.library and brush.library.is_editable:
            layout.operator("brush.asset_save_as", text="Duplicate Asset...", icon='DUPLICATE')
            layout.operator("brush.asset_delete", text="Delete Asset")
        else:
            layout.operator("brush.asset_save_as", text="Save As Asset...", icon='FILE_TICK')
            layout.operator("brush.asset_delete", text="Delete")

        layout.separator()

        layout.operator("brush.asset_edit_metadata", text="Edit Metadata...")
        layout.operator("brush.asset_load_preview", text="Edit Preview Image...")
        layout.operator("brush.asset_save", text="Save Changes to Asset")
        layout.operator("brush.asset_revert", text="Revert to Asset")


class View3DPanel:
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'


# **************** standard tool clusters ******************

# Used by vertex paint.
def draw_vpaint_symmetry(layout, obj):
    mesh = obj.data

    col = layout.column()
    row = col.row(heading="Mirror", align=True)
    row.prop(obj, "use_mesh_mirror_x", text="X", toggle=True)
    row.prop(obj, "use_mesh_mirror_y", text="Y", toggle=True)
    row.prop(obj, "use_mesh_mirror_z", text="Z", toggle=True)

    col = layout.column()
    col.active = not mesh.use_mirror_vertex_groups
    col.prop(mesh, "radial_symmetry", text="Radial")


# ********** default tools for object mode ****************


class VIEW3D_PT_tools_object_options(View3DPanel, Panel):
    bl_category = "Tool"
    bl_context = ".objectmode"  # dot on purpose (access from topbar)
    bl_label = "Options"

    def draw(self, context):
        # layout = self.layout
        pass


class VIEW3D_PT_tools_object_options_transform(View3DPanel, Panel):
    bl_category = "Tool"
    bl_context = ".objectmode"  # dot on purpose (access from topbar)
    bl_label = "Transform"
    bl_parent_id = "VIEW3D_PT_tools_object_options"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False

        tool_settings = context.tool_settings

        col = layout.column(heading="Affect Only", align=True)
        col.prop(tool_settings, "use_transform_data_origin", text="Origins")
        col.prop(tool_settings, "use_transform_pivot_point_align", text="Locations")


# ********** default tools for editmode_mesh ****************


class VIEW3D_PT_tools_meshedit_options(View3DPanel, Panel):
    bl_category = "Tool"
    bl_context = ".mesh_edit"  # dot on purpose (access from topbar)
    bl_label = "Options"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 12

    def draw(self, _context):
        # layout = self.layout
        pass


class VIEW3D_PT_tools_meshedit_options_transform(View3DPanel, Panel):
    bl_category = "Tool"
    bl_context = ".mesh_edit"  # dot on purpose (access from topbar)
    bl_label = "Transform"
    bl_parent_id = "VIEW3D_PT_tools_meshedit_options"

    @classmethod
    def poll(cls, context):
        return context.active_object

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False

        tool_settings = context.tool_settings
        ob = context.active_object
        mesh = ob.data

        col = layout.column(align=True)
        col.prop(tool_settings, "use_transform_correct_face_attributes")
        sub = col.column(align=True)
        sub.active = tool_settings.use_transform_correct_face_attributes
        sub.prop(tool_settings, "use_transform_correct_keep_connected")
        col.separator()

        col = layout.column(heading="Mirror")
        sub = col.row(align=True)
        sub.prop(mesh, "use_mirror_x", text="X", toggle=True)
        sub.prop(mesh, "use_mirror_y", text="Y", toggle=True)
        sub.prop(mesh, "use_mirror_z", text="Z", toggle=True)

        col = layout.column(align=True)
        col.active = mesh.use_mirror_x or mesh.use_mirror_y or mesh.use_mirror_z
        col.prop(mesh, "use_mirror_topology")
        col.separator()

        col = layout.column(align=True)
        col.prop(tool_settings, "use_mesh_automerge", text="Auto Merge", toggle=False)
        sub = col.column(align=True)
        sub.active = tool_settings.use_mesh_automerge
        sub.prop(tool_settings, "use_mesh_automerge_and_split", toggle=False)
        sub.prop(tool_settings, "double_threshold", text="Threshold")


class VIEW3D_PT_tools_meshedit_options_uvs(View3DPanel, Panel):
    bl_category = "Tool"
    bl_context = ".mesh_edit"  # dot on purpose (access from topbar)
    bl_label = "UVs"
    bl_parent_id = "VIEW3D_PT_tools_meshedit_options"

    def draw(self, context):
        layout = self.layout

        layout.use_property_decorate = False
        layout.use_property_split = True

        tool_settings = context.tool_settings

        layout.prop(tool_settings, "use_edge_path_live_unwrap")


# ********** default tools for paint modes ****************


class View3DPaintPanel(View3DPanel, UnifiedPaintPanel):
    bl_category = "Tool"


class View3DPaintBrushPanel(View3DPaintPanel):
    @classmethod
    def poll(cls, context):
        mode = cls.get_brush_mode(context)
        return mode is not None


class VIEW3D_PT_tools_particlemode(Panel, View3DPaintPanel):
    bl_context = ".paint_common"  # dot on purpose (access from topbar)
    bl_label = "Particle Tool"

    @classmethod
    def poll(cls, context):
        settings = context.tool_settings.particle_edit
        return (settings and settings.brush and context.particle_edit_object)

    def draw(self, context):
        layout = self.layout

        settings = context.tool_settings.particle_edit
        brush = settings.brush
        tool = settings.tool

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        from bl_ui.space_toolsystem_common import ToolSelectPanelHelper
        tool_context = ToolSelectPanelHelper.tool_active_from_context(context)

        if not tool_context:
            # If there is no active tool, then there can't be an active brush.
            tool = None

        if not tool_context.use_brushes:
            tool = None

        if tool is not None:
            col = layout.column()
            col.prop(brush, "size", slider=True)
            if tool == 'ADD':
                col.prop(brush, "count")

                col = layout.column()
                col.prop(settings, "use_default_interpolate")
                col.prop(brush, "steps", slider=True)
                col.prop(settings, "default_key_count", slider=True)
            else:
                col.prop(brush, "strength", slider=True)

                if tool == 'LENGTH':
                    layout.row().prop(brush, "length_mode", expand=True)
                elif tool == 'PUFF':
                    layout.row().prop(brush, "puff_mode", expand=True)
                    layout.prop(brush, "use_puff_volume")
                elif tool == 'COMB':
                    col = layout.column(align=False, heading="Deflect Emitter")
                    row = col.row(align=True)
                    sub = row.row(align=True)
                    sub.prop(settings, "use_emitter_deflect", text="")
                    sub = sub.row(align=True)
                    sub.active = settings.use_emitter_deflect
                    sub.prop(settings, "emitter_distance", text="")


class VIEW3D_PT_tools_brush_select(Panel, View3DPaintBrushPanel, BrushSelectPanel):
    bl_context = ".paint_common"
    bl_label = "Brush Asset"


class VIEW3D_PT_tools_brush_settings(Panel, View3DPaintBrushPanel):
    bl_context = ".paint_common"
    bl_label = "Brush Settings"

    @classmethod
    def poll(cls, context):
        settings = cls.paint_settings(context)
        return settings and settings.brush is not None

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        settings = self.paint_settings(context)
        brush = settings.brush

        brush_settings(layout.column(), context, brush, popover=self.is_popover)


class VIEW3D_PT_tools_brush_settings_advanced(Panel, View3DPaintBrushPanel):
    bl_context = ".paint_common"
    bl_parent_id = "VIEW3D_PT_tools_brush_settings"
    bl_label = "Advanced"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 14

    @classmethod
    def poll(cls, context):
        mode = cls.get_brush_mode(context)
        return mode is not None and mode != 'SCULPT_CURVES'

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        settings = UnifiedPaintPanel.paint_settings(context)
        brush = settings.brush

        brush_settings_advanced(layout.column(), context, settings, brush, self.is_popover)


class VIEW3D_PT_tools_brush_color(Panel, View3DPaintPanel):
    bl_context = ".paint_common"  # dot on purpose (access from topbar)
    bl_parent_id = "VIEW3D_PT_tools_brush_settings"
    bl_label = "Color Picker"

    @classmethod
    def poll(cls, context):
        settings = cls.paint_settings(context)
        brush = settings.brush

        if context.vertex_paint_object:
            capabilities = brush.vertex_paint_capabilities
            return capabilities.has_color
        elif context.sculpt_object:
            capabilities = brush.sculpt_capabilities
            return capabilities.has_color

        return False

    def draw(self, context):
        layout = self.layout
        settings = self.paint_settings(context)
        brush = settings.brush

        draw_color_settings(context, layout, brush, color_type=not context.vertex_paint_object)


class VIEW3D_PT_tools_brush_swatches(Panel, View3DPaintPanel, ColorPalettePanel):
    bl_context = ".paint_common"
    bl_parent_id = "VIEW3D_PT_tools_brush_settings"
    bl_label = "Color Palette"
    bl_options = {'DEFAULT_CLOSED'}


class VIEW3D_PT_slots_color_attributes(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Color Attributes"
    bl_ui_units_x = 12

    def draw_header(self, context):
        me = context.object.data
        active_color = me.color_attributes.active_color
        self.bl_label = (
            active_color.name if active_color else
            iface_("Color Attributes")
        )

    def draw(self, context):
        ob = context.object
        mesh = ob.data

        layout = self.layout
        row = layout.row()

        col = row.column()
        col.template_list(
            "MESH_UL_color_attributes",
            "color_attributes",
            mesh,
            "color_attributes",
            mesh.color_attributes,
            "active_color_index",
            rows=3,
        )

        col = row.column(align=True)
        col.operator("geometry.color_attribute_add", icon='ADD', text="")
        col.operator("geometry.color_attribute_remove", icon='REMOVE', text="")

        col.separator()

        col.menu("MESH_MT_color_attribute_context_menu", icon='DOWNARROW_HLT', text="")


class VIEW3D_PT_slots_vertex_groups(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Vertex Groups"
    bl_ui_units_x = 12

    def draw_header(self, context):
        ob = context.object
        groups = ob.vertex_groups
        self.bl_label = (
            groups.active.name if groups and groups.active else
            iface_("Vertex Groups")
        )

    def draw(self, context):
        layout = self.layout
        row = layout.row()
        col = row.column()

        ob = context.object
        group = ob.vertex_groups.active

        rows = 3
        if group:
            rows = 5

        row = layout.row()
        row.template_list("MESH_UL_vgroups", "", ob, "vertex_groups", ob.vertex_groups, "active_index", rows=rows)

        col = row.column(align=True)

        col.operator("object.vertex_group_add", icon='ADD', text="")
        props = col.operator("object.vertex_group_remove", icon='REMOVE', text="")
        props.all_unlocked = props.all = False

        col.separator()

        col.menu("MESH_MT_vertex_group_context_menu", icon='DOWNARROW_HLT', text="")

        if group:
            col.separator()
            col.operator("object.vertex_group_move", icon='TRIA_UP', text="").direction = 'UP'
            col.operator("object.vertex_group_move", icon='TRIA_DOWN', text="").direction = 'DOWN'


class VIEW3D_PT_tools_brush_display(Panel, View3DPaintBrushPanel, DisplayPanel):
    bl_context = ".paint_common"
    bl_parent_id = "VIEW3D_PT_tools_brush_settings"
    bl_label = "Cursor"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 15


class VIEW3D_PT_tools_brush_texture(Panel, View3DPaintPanel):
    bl_context = ".paint_common"
    bl_parent_id = "VIEW3D_PT_tools_brush_settings"
    bl_label = "Texture"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 13

    @classmethod
    def poll(cls, context):
        if (
                (settings := cls.paint_settings(context)) and
                (brush := settings.brush)
        ):
            if context.sculpt_object or context.vertex_paint_object:
                return True
        return False

    def draw(self, context):
        layout = self.layout

        settings = self.paint_settings(context)
        brush = settings.brush
        tex_slot = brush.texture_slot

        col = layout.column()
        col.template_ID_preview(tex_slot, "texture", new="texture.new", rows=3, cols=8)

        brush_texture_settings(col, brush, context.sculpt_object)


class VIEW3D_PT_tools_brush_stroke(Panel, View3DPaintPanel, StrokePanel):
    bl_context = ".paint_common"  # dot on purpose (access from topbar)
    bl_label = "Stroke"
    bl_parent_id = "VIEW3D_PT_tools_brush_settings"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 14


class VIEW3D_PT_tools_brush_stroke_smooth_stroke(Panel, View3DPaintPanel, SmoothStrokePanel):
    bl_context = ".paint_common"  # dot on purpose (access from topbar)
    bl_label = "Stabilize Stroke"
    bl_parent_id = "VIEW3D_PT_tools_brush_stroke"
    bl_options = {'DEFAULT_CLOSED'}


class VIEW3D_PT_tools_brush_falloff(Panel, View3DPaintPanel, FalloffPanel):
    bl_context = ".paint_common"  # dot on purpose (access from topbar)
    bl_parent_id = "VIEW3D_PT_tools_brush_settings"
    bl_label = "Falloff"
    bl_options = {'DEFAULT_CLOSED'}


class VIEW3D_PT_tools_brush_falloff_frontface(View3DPaintPanel, Panel):
    bl_context = ".paint_common"  # dot on purpose (access from topbar)
    bl_label = "Front-Face Falloff"
    bl_parent_id = "VIEW3D_PT_tools_brush_falloff"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return context.vertex_paint_object

    def draw_header(self, context):
        settings = self.paint_settings(context)
        brush = settings.brush

        self.layout.prop(brush, "use_frontface_falloff", text=self.bl_label if self.is_popover else "")

    def draw(self, context):
        settings = self.paint_settings(context)
        brush = settings.brush

        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False

        row = layout.row()
        row.active = brush.use_frontface_falloff
        row.prop(brush, "falloff_angle", text="Angle")


class VIEW3D_PT_sculpt_dyntopo(Panel, View3DPaintPanel):
    bl_context = ".sculpt_mode"  # dot on purpose (access from topbar)
    bl_label = "Dyntopo"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 12

    @classmethod
    def poll(cls, context):
        paint_settings = cls.paint_settings(context)
        return (context.sculpt_object and context.tool_settings.sculpt and paint_settings)

    def draw_header(self, context):
        is_popover = self.is_popover
        layout = self.layout
        layout.operator(
            "sculpt.dynamic_topology_toggle",
            icon='CHECKBOX_HLT' if context.sculpt_object.use_dynamic_topology_sculpting else 'CHECKBOX_DEHLT',
            text="",
            emboss=is_popover,
        )

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        tool_settings = context.tool_settings
        sculpt = tool_settings.sculpt
        settings = self.paint_settings(context)
        brush = settings.brush

        col = layout.column()
        col.active = context.sculpt_object.use_dynamic_topology_sculpting

        sub = col.column()
        sub.active = (brush and brush.sculpt_capabilities.has_dyntopo) or sculpt.detail_type_method == 'MANUAL'
        if sculpt.detail_type_method in {'CONSTANT', 'MANUAL'}:
            row = sub.row(align=True)
            row.prop(sculpt, "constant_detail_resolution")
            props = row.operator("sculpt.sample_detail_size", text="", icon='EYEDROPPER')
            props.mode = 'DYNTOPO'
        elif (sculpt.detail_type_method == 'BRUSH'):
            sub.prop(sculpt, "detail_percent")
        else:
            sub.prop(sculpt, "detail_size")
        sub.prop(sculpt, "detail_refine_method", text="Refine Method")
        sub.prop(sculpt, "detail_type_method", text="Detailing")

        if sculpt.detail_type_method in {'CONSTANT', 'MANUAL'}:
            col.operator("sculpt.detail_flood_fill")


class VIEW3D_PT_sculpt_voxel_remesh(Panel, View3DPaintPanel):
    bl_context = ".sculpt_mode"  # dot on purpose (access from topbar)
    bl_label = "Remesh"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 12

    @classmethod
    def poll(cls, context):
        return (context.sculpt_object and context.tool_settings.sculpt)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        col = layout.column()
        mesh = context.active_object.data
        row = col.row(align=True)
        row.prop(mesh, "remesh_voxel_size")
        props = row.operator("sculpt.sample_detail_size", text="", icon='EYEDROPPER')
        props.mode = 'VOXEL'
        col.prop(mesh, "remesh_voxel_adaptivity")
        col.prop(mesh, "use_remesh_fix_poles")

        col = layout.column(heading="Preserve", align=True)
        col.prop(mesh, "use_remesh_preserve_volume", text="Volume")
        col.prop(mesh, "use_remesh_preserve_attributes", text="Attributes")

        layout.operator("object.voxel_remesh", text="Remesh")


class VIEW3D_PT_sculpt_options(Panel, View3DPaintPanel):
    bl_context = ".sculpt_mode"  # dot on purpose (access from topbar)
    bl_label = "Options"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 13

    @classmethod
    def poll(cls, context):
        return (context.sculpt_object and context.tool_settings.sculpt)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        tool_settings = context.tool_settings
        sculpt = tool_settings.sculpt

        col = layout.column(heading="Display", align=True)
        col.prop(sculpt, "show_low_resolution")
        col.prop(sculpt, "use_sculpt_delay_updates")
        col.prop(sculpt, "use_deform_only")


class VIEW3D_PT_sculpt_options_gravity(Panel, View3DPaintPanel):
    bl_context = ".sculpt_mode"  # dot on purpose (access from topbar)
    bl_parent_id = "VIEW3D_PT_sculpt_options"
    bl_label = "Gravity"

    @classmethod
    def poll(cls, context):
        return (context.sculpt_object and context.tool_settings.sculpt)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        tool_settings = context.tool_settings
        sculpt = tool_settings.sculpt
        capabilities = sculpt.brush.sculpt_capabilities

        col = layout.column()
        col.active = capabilities.has_gravity
        col.prop(sculpt, "gravity", slider=True, text="Factor")
        col.prop(sculpt, "gravity_object")


class VIEW3D_PT_sculpt_symmetry(Panel, View3DPaintPanel):
    bl_context = ".sculpt_mode"  # dot on purpose (access from topbar)
    bl_label = "Symmetry"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return (
            (context.sculpt_object and context.tool_settings.sculpt) and
            # When used in the tool header, this is explicitly included next to the XYZ symmetry buttons.
            (context.region.type != 'TOOL_HEADER')
        )

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        sculpt = context.tool_settings.sculpt

        row = layout.row(align=True, heading="Mirror")

        ob = context.object
        mesh = ob.data
        row.prop(mesh, "use_mirror_x", text="X", toggle=True)
        row.prop(mesh, "use_mirror_y", text="Y", toggle=True)
        row.prop(mesh, "use_mirror_z", text="Z", toggle=True)

        row = layout.row(align=True, heading="Lock")
        row.prop(sculpt, "lock_x", text="X", toggle=True)
        row.prop(sculpt, "lock_y", text="Y", toggle=True)
        row.prop(sculpt, "lock_z", text="Z", toggle=True)

        row = layout.row(align=True, heading="Tiling")
        row.prop(sculpt, "tile_x", text="X", toggle=True)
        row.prop(sculpt, "tile_y", text="Y", toggle=True)
        row.prop(sculpt, "tile_z", text="Z", toggle=True)

        layout.prop(sculpt, "use_symmetry_feather", text="Feather")
        layout.prop(mesh, "radial_symmetry", text="Radial")
        layout.prop(sculpt, "tile_offset", text="Tile Offset")

        layout.separator()

        layout.label(text="Symmetrize")
        layout.prop(sculpt, "symmetrize_direction")
        layout.prop(WindowManager.operator_properties_last("sculpt.symmetrize"), "merge_tolerance")
        layout.operator("sculpt.symmetrize")


class VIEW3D_PT_sculpt_symmetry_for_topbar(Panel):
    bl_space_type = 'TOPBAR'
    bl_region_type = 'HEADER'
    bl_label = "Symmetry"
    bl_ui_units_x = 13

    draw = VIEW3D_PT_sculpt_symmetry.draw


class VIEW3D_PT_curves_sculpt_symmetry(Panel, View3DPaintPanel):
    bl_context = ".curves_sculpt"  # dot on purpose (access from topbar)
    bl_label = "Symmetry"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return ob and ob.type == 'CURVES'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        ob = context.object
        curves = ob.data

        row = layout.row(align=True, heading="Mirror")
        row.prop(curves, "use_mirror_x", text="X", toggle=True)
        row.prop(curves, "use_mirror_y", text="Y", toggle=True)
        row.prop(curves, "use_mirror_z", text="Z", toggle=True)


class VIEW3D_PT_curves_sculpt_symmetry_for_topbar(Panel):
    bl_space_type = 'TOPBAR'
    bl_region_type = 'HEADER'
    bl_label = "Symmetry"

    draw = VIEW3D_PT_curves_sculpt_symmetry.draw


# ********** default tools for vertex-paint ****************


class VIEW3D_PT_tools_vertexpaint_options(Panel, View3DPaintPanel):
    bl_context = ".vertexpaint"  # dot on purpose (access from topbar)
    bl_label = "Options"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, _context):
        # This is currently unused, since there aren't any Vertex Paint mode specific options.
        return False

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False


class VIEW3D_PT_tools_vertexpaint_symmetry(Panel, View3DPaintPanel):
    bl_context = ".vertexpaint"  # dot on purpose (access from topbar)
    bl_options = {'DEFAULT_CLOSED'}
    bl_label = "Symmetry"

    @classmethod
    def poll(cls, context):
        # When used in the tool header, this is explicitly included next to the XYZ symmetry buttons.
        return (context.region.type != 'TOOL_HEADER')

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        ob = context.object

        draw_vpaint_symmetry(layout, ob)


class VIEW3D_PT_tools_vertexpaint_symmetry_for_topbar(Panel):
    bl_space_type = 'TOPBAR'
    bl_region_type = 'HEADER'
    bl_label = "Symmetry"

    draw = VIEW3D_PT_tools_vertexpaint_symmetry.draw


class VIEW3D_PT_tools_particlemode_options(View3DPanel, Panel):
    """Default tools for particle mode"""
    bl_category = "Tool"
    bl_context = ".particlemode"
    bl_label = "Options"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        pe = context.tool_settings.particle_edit
        ob = pe.object

        layout.prop(pe, "type", text="Editing Type")

        ptcache = None

        if pe.type == 'PARTICLES':
            if ob.particle_systems:
                if len(ob.particle_systems) > 1:
                    layout.template_list(
                        "UI_UL_list", "particle_systems", ob, "particle_systems",
                        ob.particle_systems, "active_index", rows=2, maxrows=3,
                    )

                ptcache = ob.particle_systems.active.point_cache
        else:
            for md in ob.modifiers:
                if md.type == pe.type:
                    ptcache = md.point_cache

        if ptcache and len(ptcache.point_caches) > 1:
            layout.template_list(
                "UI_UL_list", "particles_point_caches", ptcache, "point_caches",
                ptcache.point_caches, "active_index", rows=2, maxrows=3,
            )

        if not pe.is_editable:
            layout.label(text="Point cache must be baked")
            layout.label(text="in memory to enable editing!")

        col = layout.column(align=True)
        col.active = pe.is_editable

        if not pe.is_hair:
            col.prop(pe, "use_auto_velocity", text="Auto-Velocity")
            col.separator()

        sub = col.column(align=True, heading="Mirror")
        sub.prop(ob.data, "use_mirror_x")
        if pe.tool == 'ADD':
            sub.prop(ob.data, "use_mirror_topology")
        col.separator()

        sub = col.column(align=True, heading="Preserve")
        sub.prop(pe, "use_preserve_length", text="Strand Lengths")
        sub.prop(pe, "use_preserve_root", text="Root Positions")


class VIEW3D_PT_tools_particlemode_options_shapecut(View3DPanel, Panel):
    """Default tools for particle mode"""
    bl_category = "Tool"
    bl_parent_id = "VIEW3D_PT_tools_particlemode_options"
    bl_label = "Cut Particles to Shape"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        pe = context.tool_settings.particle_edit

        layout.prop(pe, "shape_object")
        layout.operator("particle.shape_cut", text="Cut")


class VIEW3D_PT_tools_particlemode_options_display(View3DPanel, Panel):
    """Default tools for particle mode"""
    bl_category = "Tool"
    bl_parent_id = "VIEW3D_PT_tools_particlemode_options"
    bl_label = "Viewport Display"

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        pe = context.tool_settings.particle_edit

        col = layout.column()
        col.active = pe.is_editable
        col.prop(pe, "display_step", text="Path Steps")
        if pe.is_hair:
            col.prop(pe, "show_particles", text="Children")
        else:
            if pe.type == 'PARTICLES':
                col.prop(pe, "show_particles", text="Particles")
            col = layout.column(align=False, heading="Fade Time")
            row = col.row(align=True)
            sub = row.row(align=True)
            sub.prop(pe, "use_fade_time", text="")
            sub = sub.row(align=True)
            sub.active = pe.use_fade_time
            sub.prop(pe, "fade_frames", slider=True, text="")


def tool_use_brush(context):
    from bl_ui.space_toolsystem_common import ToolSelectPanelHelper
    tool = ToolSelectPanelHelper.tool_active_from_context(context)
    if tool and tool.use_brushes is False:
        return False

    return True


classes = (
    VIEW3D_MT_brush_context_menu,
    VIEW3D_PT_tools_object_options,
    VIEW3D_PT_tools_object_options_transform,
    VIEW3D_PT_tools_meshedit_options,
    VIEW3D_PT_tools_meshedit_options_transform,
    VIEW3D_PT_tools_meshedit_options_uvs,
    VIEW3D_PT_slots_color_attributes,
    VIEW3D_PT_slots_vertex_groups,
    VIEW3D_PT_tools_brush_select,
    VIEW3D_PT_tools_brush_settings,
    VIEW3D_PT_tools_brush_color,
    VIEW3D_PT_tools_brush_swatches,
    VIEW3D_PT_tools_brush_settings_advanced,
    VIEW3D_PT_tools_brush_texture,
    VIEW3D_PT_tools_brush_stroke,
    VIEW3D_PT_tools_brush_stroke_smooth_stroke,
    VIEW3D_PT_tools_brush_falloff,
    VIEW3D_PT_tools_brush_falloff_frontface,
    VIEW3D_PT_tools_brush_display,
    VIEW3D_PT_tools_weight_gradient,

    VIEW3D_PT_sculpt_dyntopo,
    VIEW3D_PT_sculpt_voxel_remesh,
    VIEW3D_PT_sculpt_symmetry,
    VIEW3D_PT_sculpt_symmetry_for_topbar,
    VIEW3D_PT_sculpt_options,
    VIEW3D_PT_sculpt_options_gravity,

    VIEW3D_PT_curves_sculpt_symmetry,
    VIEW3D_PT_curves_sculpt_symmetry_for_topbar,

    VIEW3D_PT_tools_vertexpaint_symmetry,
    VIEW3D_PT_tools_vertexpaint_symmetry_for_topbar,
    VIEW3D_PT_tools_vertexpaint_options,

    VIEW3D_PT_tools_particlemode,
    VIEW3D_PT_tools_particlemode_options,
    VIEW3D_PT_tools_particlemode_options_shapecut,
    VIEW3D_PT_tools_particlemode_options_display,


)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
