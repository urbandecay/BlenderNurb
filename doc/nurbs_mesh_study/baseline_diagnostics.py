# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Print fresh NURBS/mesh diagnostic fixtures; this is not an acceptance suite.

The recorded baseline in current_checkout_audit.md is commit 355a9ae11d7e.
Run this using Blender's --background --factory-startup --python-exit-code 1.
"""

import bpy


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def nurbs_extrude(displacement):
    bpy.ops.curve.extrude()
    bpy.ops.transform.translate(value=displacement)


reset()
bpy.ops.surface.primitive_nurbs_surface_surface_add(enter_editmode=True)
ob = bpy.context.object
ob.update_from_editmode()
before = [(s.point_count_u, s.point_count_v, len(s.points)) for s in ob.data.splines]
status = bpy.ops.curve.extrude()
ob.update_from_editmode()
after = [(s.point_count_u, s.point_count_v, len(s.points)) for s in ob.data.splines]
print('AUDIT legacy fully selected surface:', status, before, '->', after)

for copies in (1, 2):
    reset()
    bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
    nurbs_extrude((0, 2, 0))
    nurbs_extrude((0, 2, 0))
    ob = bpy.context.object
    ob.update_from_editmode()
    for index, spline in enumerate(ob.data.splines):
        for point in spline.points:
            point.select = index < copies and abs(point.co.y - 2.0) < 1e-5
    before = len(ob.data.splines)
    status = bpy.ops.curve.extrude()
    ob.update_from_editmode()
    print('AUDIT shared edge selected on', copies, 'patch copies:', status,
          before, '->', len(ob.data.splines))

reset()
bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
ob = bpy.context.object
bpy.ops.curve.duplicate()
bpy.ops.transform.translate(value=(0, 1, 0))
ob.update_from_editmode()
print('AUDIT duplicate line then translate:',
      [[tuple(point.co) for point in spline.points] for spline in ob.data.splines])

reset()
bpy.ops.mesh.primitive_plane_add(enter_editmode=True)
ob = bpy.context.object
for displacement in (3, 1):
    bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={'value': (0, 0, displacement)})
    ob.update_from_editmode()
    print('AUDIT mesh region extrusion face count:', len(ob.data.polygons))

reset()
bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
nurbs_extrude((0, 2, 0))
bpy.ops.curve.select_all(action='SELECT')
ob = bpy.context.object
for displacement in (3, 1):
    nurbs_extrude((0, 0, displacement))
    ob.update_from_editmode()
    print('AUDIT NURBS region extrusion face count:', len(ob.data.splines))
