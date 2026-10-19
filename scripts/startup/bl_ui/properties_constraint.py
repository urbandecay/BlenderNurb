# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Panel
from bpy.app.translations import contexts as i18n_contexts


class ObjectConstraintPanel:
    bl_context = "constraint"

    @classmethod
    def poll(cls, context):
        return (context.object)


class OBJECT_PT_constraints(ObjectConstraintPanel, Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_label = "Object Constraints"
    bl_options = {'HIDE_HEADER'}

    def draw(self, _context):
        layout = self.layout

        layout.operator_menu_enum("object.constraint_add", "type", text="Add Object Constraint")

        layout.template_constraints()


class ConstraintButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_label = ""
    bl_options = {'INSTANCED', 'HEADER_LAYOUT_EXPAND'}

    @staticmethod
    def draw_influence(layout, con):
        layout.separator()
        row = layout.row(align=True)
        row.prop(con, "influence")
        row.operator("constraint.disable_keep_transform", text="", icon='CANCEL')

    @staticmethod
    def space_template(layout, con, target=True, owner=True, separator=True):
        if target or owner:
            if separator:
                layout.separator()
            if target:
                layout.prop(con, "target_space", text="Target")
            if owner:
                layout.prop(con, "owner_space", text="Owner")

            if con.target_space == 'CUSTOM' or con.owner_space == 'CUSTOM':
                col = layout.column()
                col.prop(con, "space_object")
                if space_object := con.space_object:
                    match space_object.type:
                        case 'MESH', 'LATTICE':
                            col.prop_search(
                                con, "space_subtarget", con.space_object,
                                "vertex_groups", text="Vertex Group",
                            )

    @staticmethod
    def target_template(layout, con, subtargets=True):
        col = layout.column()
        col.prop(con, "target")  # XXX: limiting settings for only `curves` or some type of object.

        if con.target and subtargets and con.target.type in {'MESH', 'LATTICE'}:
            col.prop_search(con, "subtarget", con.target, "vertex_groups", text="Vertex Group")

    def get_constraint(self, _context):
        con = self.custom_data
        self.layout.context_pointer_set("constraint", con)
        return con

    def draw_header(self, context):
        layout = self.layout
        con = self.get_constraint(context)

        layout.template_constraint_header(con)


    def draw_childof(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row(heading="Location")
        row.use_property_decorate = False
        row.prop(con, "use_location_x", text="X", toggle=True)
        row.prop(con, "use_location_y", text="Y", toggle=True)
        row.prop(con, "use_location_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row(heading="Rotation")
        row.use_property_decorate = False
        row.prop(con, "use_rotation_x", text="X", toggle=True)
        row.prop(con, "use_rotation_y", text="Y", toggle=True)
        row.prop(con, "use_rotation_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row(heading="Scale")
        row.use_property_decorate = False
        row.prop(con, "use_scale_x", text="X", toggle=True)
        row.prop(con, "use_scale_y", text="Y", toggle=True)
        row.prop(con, "use_scale_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row()
        row.operator("constraint.childof_set_inverse")
        row.operator("constraint.childof_clear_inverse")

        self.draw_influence(layout, con)

    def draw_trackto(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "track_axis", expand=True)
        layout.prop(con, "up_axis", text="Up", expand=True)
        layout.prop(con, "use_target_z")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_follow_path(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        if con.use_fixed_location:
            layout.prop(con, "offset_factor", text="Offset Factor")
        else:
            layout.prop(con, "offset")

        layout.prop(con, "forward_axis", expand=True)
        layout.prop(con, "up_axis", expand=True)

        col = layout.column()
        col.prop(con, "use_fixed_location")
        col.prop(con, "use_curve_radius")
        col.prop(con, "use_curve_follow")

        layout.operator("constraint.followpath_path_animate", text="Animate Path", icon='ANIM_DATA')

        self.draw_influence(layout, con)

    def draw_rot_limit(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        # Decorators and property split are really buggy with these properties
        row = layout.row(heading="Limit X", align=True)
        row.use_property_decorate = False
        row.prop(con, "use_limit_x", text="")
        sub = row.column(align=True)
        sub.active = con.use_limit_x
        sub.prop(con, "min_x", text="Min")
        sub.prop(con, "max_x", text="Max")
        row.label(icon='BLANK1')

        row = layout.row(heading="Y", align=True)
        row.use_property_decorate = False
        row.prop(con, "use_limit_y", text="")
        sub = row.column(align=True)
        sub.active = con.use_limit_y
        sub.prop(con, "min_y", text="Min")
        sub.prop(con, "max_y", text="Max")
        row.label(icon='BLANK1')

        row = layout.row(heading="Z", align=True)
        row.use_property_decorate = False
        row.prop(con, "use_limit_z", text="")
        sub = row.column(align=True)
        sub.active = con.use_limit_z
        sub.prop(con, "min_z", text="Min")
        sub.prop(con, "max_z", text="Max")
        row.label(icon='BLANK1')

        layout.prop(con, "euler_order", text="Order")
        layout.prop(con, "use_transform_limit")
        layout.prop(con, "use_legacy_behavior")
        self.space_template(layout, con, target=False, owner=True)

        self.draw_influence(layout, con)

    def draw_loc_limit(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()

        row = col.row(heading="Minimum X", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_x", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_x
        subsub.prop(con, "min_x", text="")
        row.prop_decorator(con, "min_x")

        row = col.row(heading="Y", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_y", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_y
        subsub.prop(con, "min_y", text="")
        row.prop_decorator(con, "min_y")

        row = col.row(heading="Z", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_z", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_z
        subsub.prop(con, "min_z", text="")
        row.prop_decorator(con, "min_z")

        col.separator()

        row = col.row(heading="Maximum X", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_x", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_x
        subsub.prop(con, "max_x", text="")
        row.prop_decorator(con, "max_x")

        row = col.row(heading="Y", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_y", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_y
        subsub.prop(con, "max_y", text="")
        row.prop_decorator(con, "max_y")

        row = col.row(heading="Z", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_z", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_z
        subsub.prop(con, "max_z", text="")
        row.prop_decorator(con, "max_z")

        layout.prop(con, "use_transform_limit")
        self.space_template(layout, con, target=False, owner=True)

        self.draw_influence(layout, con)

    def draw_size_limit(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()

        row = col.row(heading="Minimum X", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_x", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_x
        subsub.prop(con, "min_x", text="")
        row.prop_decorator(con, "min_x")

        row = col.row(heading="Y", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_y", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_y
        subsub.prop(con, "min_y", text="")
        row.prop_decorator(con, "min_y")

        row = col.row(heading="Z", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_z", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_z
        subsub.prop(con, "min_z", text="")
        row.prop_decorator(con, "min_z")

        col.separator()

        row = col.row(heading="Maximum X", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_x", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_x
        subsub.prop(con, "max_x", text="")
        row.prop_decorator(con, "max_x")

        row = col.row(heading="Y", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_y", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_y
        subsub.prop(con, "max_y", text="")
        row.prop_decorator(con, "max_y")

        row = col.row(heading="Z", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_z", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_z
        subsub.prop(con, "max_z", text="")
        row.prop_decorator(con, "max_z")

        layout.prop(con, "use_transform_limit")
        self.space_template(layout, con, target=False, owner=True)

        self.draw_influence(layout, con)

    def draw_rotate_like(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "euler_order", text="Order")

        row = layout.row(heading="Axis", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_x", text="X", toggle=True)
        sub.prop(con, "use_y", text="Y", toggle=True)
        sub.prop(con, "use_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row(heading="Invert", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "invert_x", text="X", toggle=True)
        sub.prop(con, "invert_y", text="Y", toggle=True)
        sub.prop(con, "invert_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        layout.prop(con, "mix_mode", text="Mix", text_ctxt=i18n_contexts.constraint)

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_locate_like(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row(heading="Axis", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_x", text="X", toggle=True)
        sub.prop(con, "use_y", text="Y", toggle=True)
        sub.prop(con, "use_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row(heading="Invert", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "invert_x", text="X", toggle=True)
        sub.prop(con, "invert_y", text="Y", toggle=True)
        sub.prop(con, "invert_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        layout.prop(con, "use_offset")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_size_like(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row(heading="Axis", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_x", text="X", toggle=True)
        sub.prop(con, "use_y", text="Y", toggle=True)
        sub.prop(con, "use_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        col = layout.column()
        col.prop(con, "power")
        col.prop(con, "use_make_uniform")

        col.prop(con, "use_offset")
        row = col.row()
        row.active = con.use_offset
        row.prop(con, "use_add")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_same_volume(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.prop(con, "mode")

        row = layout.row(heading="Free Axis")
        row.prop(con, "free_axis", expand=True)

        layout.prop(con, "volume")

        self.space_template(layout, con, target=False, owner=True)

        self.draw_influence(layout, con)

    def draw_trans_like(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "remove_target_shear")
        layout.prop(con, "mix_mode", text="Mix", text_ctxt=i18n_contexts.constraint)

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_action(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        target_row = layout.row(align=True)
        target_row.active = not con.use_eval_time
        self.target_template(target_row, con)

        row = layout.row(align=True, heading="Evaluation Time")
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_eval_time", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_eval_time
        subsub.prop(con, "eval_time", text="")
        row.prop_decorator(con, "eval_time")

        layout.prop(con, "mix_mode", text="Mix", text_ctxt=i18n_contexts.constraint)

        self.draw_influence(layout, con)

    def draw_lock_track(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "track_axis", expand=True)
        layout.prop(con, "lock_axis", expand=True)

        self.draw_influence(layout, con)

    def draw_dist_limit(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row()
        row.prop(con, "distance")
        row.operator("constraint.limitdistance_reset", text="", icon='X')

        layout.prop(con, "limit_mode", text="Clamp Region")

        layout.prop(con, "use_transform_limit")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_stretch_to(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row()
        row.prop(con, "rest_length")
        row.operator("constraint.stretchto_reset", text="", icon='X')

        layout.separator()

        col = layout.column()
        col.prop(con, "bulge", text="Volume Variation")

        row = col.row(heading="Volume Min", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_bulge_min", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_bulge_min
        subsub.prop(con, "bulge_min", text="")
        row.prop_decorator(con, "bulge_min")

        row = col.row(heading="Max", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_bulge_max", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_bulge_max
        subsub.prop(con, "bulge_max", text="")
        row.prop_decorator(con, "bulge_max")

        row = col.row()
        row.active = con.use_bulge_min or con.use_bulge_max
        row.prop(con, "bulge_smooth", text="Smooth")

        layout.prop(con, "volume", expand=True)
        layout.prop(con, "keep_axis", text="Rotation", expand=True)

        self.draw_influence(layout, con)

    def draw_min_max(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "offset")
        layout.prop(con, "floor_location", expand=True, text="Min/Max")
        layout.prop(con, "use_rotation")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_clamp_to(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "main_axis", expand=True)

        layout.prop(con, "use_cyclic")

        self.draw_influence(layout, con)

    def draw_transform(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "use_motion_extrapolate", text="Extrapolate")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_shrinkwrap(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con, False)

        layout.prop(con, "distance")
        layout.prop(con, "shrinkwrap_type", text="Mode")

        layout.separator()

        if con.shrinkwrap_type == 'PROJECT':
            layout.prop(con, "project_axis", expand=True, text="Project Axis")
            layout.prop(con, "project_axis_space", text="Space")

            if con.project_axis_space == 'CUSTOM':
                col = layout.column()
                col.prop(con, "space_object")
                if space_object := con.space_object:
                    match space_object.type:
                        case 'MESH', 'LATTICE':
                            col.prop_search(
                                con, "space_subtarget",
                                con.space_object, "vertex_groups",
                                text="Vertex Group",
                            )

            layout.prop(con, "project_limit", text="Distance")
            layout.prop(con, "use_project_opposite")

            layout.separator()

            col = layout.column()
            row = col.row()
            row.prop(con, "cull_face", expand=True)
            row = col.row()
            row.active = con.use_project_opposite and con.cull_face != 'OFF'
            row.prop(con, "use_invert_cull")

            layout.separator()

        if con.shrinkwrap_type in {'PROJECT', 'NEAREST_SURFACE', 'TARGET_PROJECT'}:
            layout.prop(con, "wrap_mode", text="Snap Mode")
            row = layout.row(heading="Align to Normal", align=True)
            row.use_property_decorate = False
            sub = row.row(align=True)
            sub.prop(con, "use_track_normal", text="")
            subsub = sub.row(align=True)
            subsub.active = con.use_track_normal
            subsub.prop(con, "track_axis", text="")
            row.prop_decorator(con, "track_axis")

        self.draw_influence(layout, con)

    def draw_damp_track(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "track_axis", expand=True)

        self.draw_influence(layout, con)

    def draw_spline_ik(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        self.draw_influence(layout, con)

    def draw_pivot(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        if con.target:
            layout.prop(con, "offset", text="Pivot Offset")
        else:
            layout.prop(con, "use_relative_location")
            if con.use_relative_location:
                layout.prop(con, "offset", text="Pivot Point")
            else:
                layout.prop(con, "offset", text="Pivot Point")

        col = layout.column()
        col.prop(con, "rotation_range", text="Rotation Range")

        self.draw_influence(layout, con)

    def draw_follow_track(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        clip = None
        if con.use_active_clip:
            clip = context.scene.active_clip
        else:
            clip = con.clip

        layout.prop(con, "use_active_clip")
        layout.prop(con, "use_3d_position")

        row = layout.row()
        row.active = not con.use_3d_position
        row.prop(con, "use_undistorted_position")

        if not con.use_active_clip:
            layout.prop(con, "clip")

        layout.prop(con, "frame_method")

        if clip:
            tracking = clip.tracking

            layout.prop_search(con, "object", tracking, "objects", icon='OBJECT_DATA')

            tracking_object = tracking.objects.get(con.object, tracking.objects[0])

            layout.prop_search(con, "track", tracking_object, "tracks", icon='ANIM_DATA')

        layout.prop(con, "camera")

        row = layout.row()
        row.active = not con.use_3d_position
        row.prop(con, "depth_object")

        layout.operator("clip.constraint_to_fcurve")

        self.draw_influence(layout, con)

    def draw_camera_solver(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.prop(con, "use_active_clip")

        if not con.use_active_clip:
            layout.prop(con, "clip")

        layout.operator("clip.constraint_to_fcurve")

        self.draw_influence(layout, con)

    def draw_object_solver(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        clip = None
        if con.use_active_clip:
            clip = context.scene.active_clip
        else:
            clip = con.clip

        layout.prop(con, "use_active_clip")

        if not con.use_active_clip:
            layout.prop(con, "clip")

        if clip:
            layout.prop_search(con, "object", clip.tracking, "objects", icon='OBJECT_DATA')

        layout.prop(con, "camera")

        row = layout.row()
        row.operator("constraint.objectsolver_set_inverse")
        row.operator("constraint.objectsolver_clear_inverse")

        layout.operator("clip.constraint_to_fcurve")

        self.draw_influence(layout, con)

    def draw_transform_cache(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.template_cache_file(con, "cache_file")

        cache_file = con.cache_file

        if cache_file is not None:
            layout.prop_search(con, "object_path", cache_file, "object_paths")

        self.draw_influence(layout, con)

    def draw_geometry_attribute(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con, False)
        layout.prop(con, "apply_target_transform", text="Offset with Target Transform")

        layout.prop(con, "attribute_name", text="Attribute Name")
        layout.prop(con, "data_type", text="Data Type")
        layout.prop(con, "domain", text="Domain")
        layout.prop(con, "sample_index", text="Sample Index")

        layout.separator()
        layout.prop(con, "mix_mode", text="Mix Mode", text_ctxt=i18n_contexts.constraint)

        if con.data_type == 'FLOAT4X4':
            row = layout.row(heading="Enabled")
            row.prop(con, "mix_loc", text="Location", toggle=True)
            row.prop(con, "mix_rot", text="Rotation", toggle=True)
            row.prop(con, "mix_scl", text="Scale", toggle=True)
            row.label(icon='BLANK1')

        self.draw_influence(layout, con)

# Parent class for constraint sub-panels.


class ConstraintButtonsSubPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_label = ""

    def get_constraint(self, _context):
        con = self.custom_data
        self.layout.context_pointer_set("constraint", con)
        return con

    def draw_transform_from(self, context):
        layout = self.layout
        con = self.get_constraint(context)

        layout.prop(con, "map_from", expand=True)

        layout.use_property_split = True
        layout.use_property_decorate = True

        from_axes = [con.map_to_x_from, con.map_to_y_from, con.map_to_z_from]

        if con.map_from == 'ROTATION':
            layout.prop(con, "from_rotation_mode", text="Mode")

        ext = "" if con.map_from == 'LOCATION' else "_rot" if con.map_from == 'ROTATION' else "_scale"

        col = layout.column(align=True)
        col.active = "X" in from_axes
        col.prop(con, "from_min_x" + ext, text="X Min")
        col.prop(con, "from_max_x" + ext, text="Max")

        col = layout.column(align=True)
        col.active = "Y" in from_axes
        col.prop(con, "from_min_y" + ext, text="Y Min")
        col.prop(con, "from_max_y" + ext, text="Max")

        col = layout.column(align=True)
        col.active = "Z" in from_axes
        col.prop(con, "from_min_z" + ext, text="Z Min")
        col.prop(con, "from_max_z" + ext, text="Max")

    def draw_transform_to(self, context):
        layout = self.layout
        con = self.get_constraint(context)

        layout.prop(con, "map_to", expand=True)

        layout.use_property_split = True
        layout.use_property_decorate = True

        if con.map_to == 'ROTATION':
            layout.prop(con, "to_euler_order", text="Order")

        ext = "" if con.map_to == 'LOCATION' else "_rot" if con.map_to == 'ROTATION' else "_scale"

        col = layout.column(align=True)
        col.prop(con, "map_to_x_from", expand=False, text="X Source Axis")
        col.prop(con, "to_min_x" + ext, text="Min")
        col.prop(con, "to_max_x" + ext, text="Max")

        col = layout.column(align=True)
        col.prop(con, "map_to_y_from", expand=False, text="Y Source Axis")
        col.prop(con, "to_min_y" + ext, text="Min")
        col.prop(con, "to_max_y" + ext, text="Max")

        col = layout.column(align=True)
        col.prop(con, "map_to_z_from", expand=False, text="Z Source Axis")
        col.prop(con, "to_min_z" + ext, text="Min")
        col.prop(con, "to_max_z" + ext, text="Max")

        layout.prop(con, "mix_mode" + ext, text="Mix", text_ctxt=i18n_contexts.constraint)

    def draw_spline_ik_fitting(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()
        col.prop(con, "chain_count")
        col.prop(con, "use_even_divisions")
        col.prop(con, "use_chain_offset")

    def draw_spline_ik_chain_scaling(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.prop(con, "use_curve_radius")

        layout.prop(con, "y_scale_mode")
        layout.prop(con, "xz_scale_mode")

        if con.xz_scale_mode in {'INVERSE_PRESERVE', 'VOLUME_PRESERVE'}:
            layout.prop(con, "use_original_scale")

        if con.xz_scale_mode == 'VOLUME_PRESERVE':
            col = layout.column()
            col.prop(con, "bulge", text="Volume Variation")

            row = col.row(heading="Volume Min")
            row.prop(con, "use_bulge_min", text="")
            sub = row.row()
            sub.active = con.use_bulge_min
            sub.prop(con, "bulge_min", text="")

            row = col.row(heading="Max")
            row.prop(con, "use_bulge_max", text="")
            sub = row.row()
            sub.active = con.use_bulge_max
            sub.prop(con, "bulge_max", text="")

            row = layout.row()
            row.active = con.use_bulge_min or con.use_bulge_max
            row.prop(con, "bulge_smooth", text="Smooth")

    def draw_action_target(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()
        col.active = not con.use_eval_time
        col.prop(con, "transform_channel", text="Channel")
        ConstraintButtonsPanel.space_template(col, con, target=True, owner=False, separator=False)

        sub = col.column(align=True)
        sub.prop(con, "min", text="Range Min")
        sub.prop(con, "max", text="Max")

    def draw_action_action(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column(align=True)
        col.prop(con, "action")
        if con.action:
            col.context_pointer_set("animated_id", con.id_data)
            col.template_search(
                con, "action_slot",
                con, "action_suitable_slots",
                new="",  # No use in making a new slot here.
                unlink="anim.slot_unassign_from_constraint",
                text="Slot",
            )

        col = layout.column(align=True)
        col.prop(con, "frame_start", text="Frame Start")
        col.prop(con, "frame_end", text="End")

    def draw_transform_cache_velocity(self, context):
        self.draw_transform_cache_subpanel(
            context, self.layout.template_cache_file_velocity
        )

    def draw_transform_cache_time(self, context):
        self.draw_transform_cache_subpanel(
            context, self.layout.template_cache_file_time_settings
        )

    def draw_transform_cache_layers(self, context):
        self.draw_transform_cache_subpanel(
            context, self.layout.template_cache_file_layers
        )

    def draw_transform_cache_subpanel(self, context, template_func):
        con = self.get_constraint(context)
        if con.cache_file is None:
            return

        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = True
        template_func(con, "cache_file")

# Child Of Constraint


class OBJECT_PT_bChildOfConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_childof(context)


class OBJECT_PT_bTrackToConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_trackto(context)


class OBJECT_PT_bFollowPathConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_follow_path(context)


class OBJECT_PT_bRotLimitConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_rot_limit(context)


class OBJECT_PT_bLocLimitConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_loc_limit(context)


class OBJECT_PT_bSizeLimitConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_size_limit(context)


class OBJECT_PT_bRotateLikeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_rotate_like(context)


class OBJECT_PT_bLocateLikeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_locate_like(context)


class OBJECT_PT_bSizeLikeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_size_like(context)


class OBJECT_PT_bSameVolumeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_same_volume(context)


class OBJECT_PT_bTransLikeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_trans_like(context)


class OBJECT_PT_bActionConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_action(context)


class OBJECT_PT_bActionConstraint_target(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bActionConstraint"
    bl_label = "Target"

    def draw(self, context):
        self.draw_action_target(context)


class OBJECT_PT_bActionConstraint_action(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bActionConstraint"
    bl_label = "Action"

    def draw(self, context):
        self.draw_action_action(context)


class OBJECT_PT_bLockTrackConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_lock_track(context)


class OBJECT_PT_bDistLimitConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_dist_limit(context)


class OBJECT_PT_bStretchToConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_stretch_to(context)


class OBJECT_PT_bMinMaxConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_min_max(context)


class OBJECT_PT_bClampToConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_clamp_to(context)


class OBJECT_PT_bTransformConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_transform(context)


class OBJECT_PT_bTransformConstraint_source(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformConstraint"
    bl_label = "Map From"

    def draw(self, context):
        self.draw_transform_from(context)


class OBJECT_PT_bTransformConstraint_destination(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformConstraint"
    bl_label = "Map To"

    def draw(self, context):
        self.draw_transform_to(context)


class OBJECT_PT_bShrinkwrapConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_shrinkwrap(context)


class OBJECT_PT_bDampTrackConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_damp_track(context)


class OBJECT_PT_bPivotConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_pivot(context)


class OBJECT_PT_bFollowTrackConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_follow_track(context)


class OBJECT_PT_bCameraSolverConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_camera_solver(context)


class OBJECT_PT_bObjectSolverConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_object_solver(context)


class OBJECT_PT_bTransformCacheConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_transform_cache(context)


class OBJECT_PT_bTransformCacheConstraint_velocity(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformCacheConstraint"
    bl_label = "Velocity"

    def draw(self, context):
        self.draw_transform_cache_velocity(context)


class OBJECT_PT_bTransformCacheConstraint_layers(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformCacheConstraint"
    bl_label = "Override Layers"

    def draw(self, context):
        self.draw_transform_cache_layers(context)


class OBJECT_PT_bTransformCacheConstraint_time(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformCacheConstraint"
    bl_label = "Time"

    def draw(self, context):
        self.draw_transform_cache_time(context)


class OBJECT_PT_bGeometryAttributeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_geometry_attribute(context)


classes = (
    # Object Panels
    OBJECT_PT_constraints,
    OBJECT_PT_bChildOfConstraint,
    OBJECT_PT_bTrackToConstraint,
    OBJECT_PT_bFollowPathConstraint,
    OBJECT_PT_bRotLimitConstraint,
    OBJECT_PT_bLocLimitConstraint,
    OBJECT_PT_bSizeLimitConstraint,
    OBJECT_PT_bRotateLikeConstraint,
    OBJECT_PT_bLocateLikeConstraint,
    OBJECT_PT_bSizeLikeConstraint,
    OBJECT_PT_bSameVolumeConstraint,
    OBJECT_PT_bTransLikeConstraint,
    OBJECT_PT_bActionConstraint,
    OBJECT_PT_bActionConstraint_target,
    OBJECT_PT_bActionConstraint_action,
    OBJECT_PT_bLockTrackConstraint,
    OBJECT_PT_bDistLimitConstraint,
    OBJECT_PT_bStretchToConstraint,
    OBJECT_PT_bMinMaxConstraint,
    OBJECT_PT_bClampToConstraint,
    OBJECT_PT_bTransformConstraint,
    OBJECT_PT_bTransformConstraint_source,
    OBJECT_PT_bTransformConstraint_destination,
    OBJECT_PT_bShrinkwrapConstraint,
    OBJECT_PT_bDampTrackConstraint,
    OBJECT_PT_bPivotConstraint,
    OBJECT_PT_bFollowTrackConstraint,
    OBJECT_PT_bCameraSolverConstraint,
    OBJECT_PT_bObjectSolverConstraint,
    OBJECT_PT_bTransformCacheConstraint,
    OBJECT_PT_bTransformCacheConstraint_time,
    OBJECT_PT_bTransformCacheConstraint_velocity,
    OBJECT_PT_bTransformCacheConstraint_layers,
    OBJECT_PT_bGeometryAttributeConstraint,

)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
