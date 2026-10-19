# SPDX-FileCopyrightText: 2018-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

from bpy.types import (
    Operator,
)
from bpy.props import (
    IntProperty,
)


class CONSTRAINT_OT_add_target(Operator):
    """Add a target to the constraint"""
    bl_idname = "constraint.add_target"
    bl_label = "Add Target"
    bl_options = {'UNDO', 'INTERNAL'}

    @classmethod
    def poll(cls, context):
        constraint = getattr(context, "constraint", None)
        return constraint

    def execute(self, context):
        context.constraint.targets.new()
        return {'FINISHED'}


class CONSTRAINT_OT_remove_target(Operator):
    """Remove the target from the constraint"""
    bl_idname = "constraint.remove_target"
    bl_label = "Remove Target"
    bl_options = {'UNDO', 'INTERNAL'}

    index: IntProperty()

    @classmethod
    def poll(cls, context):
        constraint = getattr(context, "constraint", None)
        return constraint

    def execute(self, context):
        tgts = context.constraint.targets
        tgts.remove(tgts[self.index])
        return {'FINISHED'}


class CONSTRAINT_OT_disable_keep_transform(Operator):
    """Set the influence of this constraint to zero while """ \
        """trying to maintain the object's transformation. Other active """ \
        """constraints can still influence the final transformation"""

    bl_idname = "constraint.disable_keep_transform"
    bl_label = "Disable and Keep Transform"
    bl_options = {'UNDO', 'INTERNAL'}

    @classmethod
    def poll(cls, context):
        constraint = getattr(context, "constraint", None)
        return constraint and constraint.influence > 0.0

    def execute(self, context):
        """Disable constraint while maintaining the visual transform"""

        # This works most of the time, but when there are multiple constraints active
        # there could still be one that overrides the visual transform.
        #
        # Note that executing this operator and then increasing the constraint
        # influence may move the object; this happens when the constraint is
        # additive rather than replacing the transform entirely.

        # Get the matrix in world space.
        ob = context.object
        mat = ob.matrix_world

        context.constraint.influence = 0.0

        # Set the matrix.
        ob.matrix_world = mat

        return {'FINISHED'}


classes = (
    CONSTRAINT_OT_add_target,
    CONSTRAINT_OT_remove_target,
    CONSTRAINT_OT_disable_keep_transform,
)
