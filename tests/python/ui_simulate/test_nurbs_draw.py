# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native drawing through workspace tools, including redo and cancellation."""

import modules.ui_test_utils as ui


def drawing_workflow():
    import bpy
    import os
    import re
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    space.show_region_toolbar = True
    from bl_ui.space_toolsystem_common import ToolSelectPanelHelper
    t.assertGreater(ToolSelectPanelHelper._icon_value_from_icon_handle('rna:NURBS_DRAW_LINE'), 0)
    t.assertGreater(ToolSelectPanelHelper._icon_value_from_icon_handle('rna:NURBS_DRAW_SPLINE'), 0)

    def counts():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))[1::2]

    def highest_y():
        obj = window.view_layer.objects.active
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        try:
            return max(v.co.y for v in mesh.vertices)
        finally:
            evaluated.to_mesh_clear()

    def screen(point):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
        yield e.tab()
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 12
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    before = counts()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_line')
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        tool.operator_properties('curve.draw_line').use_snap = False
        tool.operator_properties('curve.draw_line').use_axis_snap = False
    yield e.cursor_position_set(*screen((-2, -2, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((0, -3, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, -2, 0)), move=True)
    yield e.leftmouse().ret()
    t.assertEqual(counts(), (before[0] + 3, before[1] + 2, before[2]))
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    finished = counts()
    yield e.ctrl.z()
    t.assertEqual(counts(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), finished)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_spline')
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        tool.operator_properties('curve.draw_spline').use_snap = False
        tool.operator_properties('curve.draw_spline').use_axis_snap = False
    yield e.cursor_position_set(*screen((-2, 1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((-1, 3, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, 3, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, 1, 0)), move=True)
    yield e.leftmouse().ret()
    t.assertEqual(counts(), (finished[0] + 2, finished[1] + 1, finished[2]))
    fit_height = highest_y()
    with bpy.context.temp_override(window=window, area=area, region=region):
        last = bpy.context.window_manager.operators[-1]
        t.assertEqual(last.bl_idname, 'CURVE_OT_draw_spline')
        last.properties.method = 'CONTROL_VERTICES'
        t.assertEqual(bpy.ops.ed.undo_redo(), {'FINISHED'})
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    t.assertLess(highest_y(), fit_height - 0.1)
    after_spline = counts()
    t.assertEqual(after_spline, (finished[0] + 2, finished[1] + 1, finished[2]))
    yield e.cursor_position_set(*screen((-2, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((0, 2, 0)), move=True)
    yield e.leftmouse().esc()
    t.assertEqual(counts(), after_spline)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.screen.screenshot(filepath=os.path.join(os.environ['BLENDER_NURBS_BEVEL_OUTPUT'], 'drawing-tools.png'))
    yield e.tab().tab()
    t.assertEqual(counts(), after_spline)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_line')
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        tool.operator_properties('curve.draw_line').use_snap = True
        tool.operator_properties('curve.draw_line').use_axis_snap = False
    yield e.cursor_position_set(*screen((1, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, -1, 0)), move=True)
    yield e.leftmouse().ret()
    t.assertEqual(counts(), (after_spline[0] + 1, after_spline[1] + 1, after_spline[2]))
    before_numeric = counts()
    with bpy.context.temp_override(window=window, area=area, region=region):
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        tool.operator_properties('curve.draw_line').use_snap = False
    yield e.cursor_position_set(*screen((3, -3, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((4, -3, 0)), move=True)
    yield e.text('1.5').ret().ret()
    t.assertEqual(counts(), (before_numeric[0] + 2, before_numeric[1] + 1, before_numeric[2]))
    with bpy.context.temp_override(window=window, area=area, region=region):
        last = bpy.context.window_manager.operators[-1]
        t.assertEqual(last.bl_idname, 'CURVE_OT_draw_line')
        t.assertAlmostEqual(last.properties.segment_length, 1.5, places=5)
        last.properties.segment_length = 2.5
        t.assertEqual(bpy.ops.ed.undo_redo(), {'FINISHED'})
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    t.assertEqual(counts(), (before_numeric[0] + 2, before_numeric[1] + 1, before_numeric[2]))
    evaluated = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    t.assertGreater(max(v.co.x for v in mesh.vertices), 5.4)
    evaluated.to_mesh_clear()
    print('NURBS_DRAW_WORKFLOW_PASS', flush=True)
