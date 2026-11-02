# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Curve corner bevel shortcut, planar boundary, modal cancel, undo and redo."""
import modules.ui_test_utils as ui


def workflow(*, face=False, cancel=False, menu=False, control_corner=False):
    import bpy
    import re
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().one().numpad_7()
    area.spaces.active.region_3d.view_location = (0.5, 0.5, 0)
    area.spaces.active.region_3d.view_distance = 5
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.delete(type='VERT')
        bpy.ops.curve.draw_line(points='1 0 0 0 0 0 0 1 0')
        bpy.ops.curve.select_all(action='SELECT')
        if face:
            bpy.ops.curve.make_segment()
        bpy.ops.ed.undo_push(message='Curve corner source')
    yield

    def counts():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        return tuple(map(int, re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats).groups()))

    if control_corner:
        from mathutils import Vector
        from bpy_extras.view3d_utils import location_3d_to_region_2d
        bpy.context.window_manager.keyconfigs.active.preferences.select_mouse = 'RIGHT'
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.curve.nurbs_control_points_toggle()
            bpy.ops.curve.select_all(action='DESELECT')
        yield
        co = location_3d_to_region_2d(region, area.spaces.active.region_3d, Vector((0, 0, 0)))
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        yield e.rightmouse()
    baseline = counts()
    if menu:
        yield from ui.call_menu(e, 'NURBS -> Bevel Curve Corners')
    else:
        yield e.ctrl.shift.b()
    yield e.text('0.15')
    yield e.esc() if cancel else e.ret()
    if cancel:
        t.assertEqual(counts(), baseline)
    else:
        after = counts()
        t.assertGreater(after[0], baseline[0])
        t.assertGreater(after[1], baseline[1])
        t.assertEqual(after[2], baseline[2])
        yield e.ctrl.z()
        t.assertEqual(counts(), baseline)
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), after)


def wire_corner():
    yield from workflow()


def planar_face_corners():
    yield from workflow(face=True)


def cancel_corner():
    yield from workflow(cancel=True)


def menu_corner():
    yield from workflow(menu=True)


def selected_endpoint_control_corner():
    yield from workflow(control_corner=True)


def control_polygon_workflow(*, divisions=1, cancel=False, adjacent=False):
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().one().numpad_7()
    space = area.spaces.active
    space.region_3d.view_location = (0, 0.5, 0)
    space.region_3d.view_distance = 8
    bpy.context.window_manager.keyconfigs.active.preferences.select_mouse = 'RIGHT'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.delete(type='VERT')
        bpy.ops.curve.draw_spline(method='CONTROL_VERTICES', degree=3,
                                  points='-2 -1 0 -1 2 0 0 -1 0 1 2 0 2 -1 0',
                                  preview_points=divisions)
        bpy.ops.curve.nurbs_control_points_toggle()
        bpy.ops.curve.select_all(action='DESELECT')
    yield
    co = location_3d_to_region_2d(region, space.region_3d, Vector((1, 2, 0)))
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.rightmouse()
    if adjacent:
        co = location_3d_to_region_2d(region, space.region_3d, Vector((0, -1, 0)))
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        yield e.shift.rightmouse()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.ed.undo_push(message='Selected off-curve control handle')

    def positions():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = obj.to_mesh()
            try:
                return sorted(tuple(v.co) for v in mesh.vertices)
            finally:
                obj.to_mesh_clear()

    baseline = positions()
    yield e.ctrl.shift.b()
    yield e.text('0.3')
    yield e.esc() if cancel else e.ret()
    if cancel:
        t.assertEqual(positions(), baseline)
        return
    after = positions()
    t.assertNotEqual(after, baseline, 'Selected off-curve handle did not bevel')
    t.assertEqual(after[0], baseline[0])
    t.assertEqual(after[-1], baseline[-1])
    # New controls stay selected and can be moved immediately.
    yield e.g().z().text('1').ret()
    moved = positions()
    t.assertGreater(max(p[2] for p in moved), 0.05)
    yield e.ctrl.z()
    t.assertEqual(positions(), after)
    yield e.ctrl.z()
    t.assertEqual(positions(), baseline)
    yield e.ctrl.shift.z()
    t.assertEqual(positions(), after)


def off_curve_control_handle():
    yield from control_polygon_workflow()


def divided_spline_control_handle():
    yield from control_polygon_workflow(divisions=16)


def cancel_control_handle():
    yield from control_polygon_workflow(cancel=True)


def adjacent_control_handles():
    yield from control_polygon_workflow(adjacent=True)


def untouched_spline_during_bevel():
    """Unselected spline samples stay identical during a different wire's modal bevel."""
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().one().numpad_7()
    space = area.spaces.active
    space.region_3d.view_location = (0, 0.5, 0)
    space.region_3d.view_distance = 16
    bpy.context.window_manager.keyconfigs.active.preferences.select_mouse = 'RIGHT'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.delete(type='VERT')
        bpy.ops.curve.draw_spline(method='CONTROL_VERTICES', degree=3,
                                  points='-5 -1 0 -4 4 0 -3 -3 0 -2 4 0 -1 -1 0',
                                  preview_points=1)
        bpy.ops.curve.draw_line(points='1 0 0 3 2 0 5 0 0')
        bpy.ops.curve.nurbs_control_points_toggle()
        bpy.ops.curve.select_all(action='DESELECT')
    yield
    co = location_3d_to_region_2d(region, space.region_3d, Vector((3, 2, 0)))
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.rightmouse()

    def left_curve():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = obj.to_mesh()
            try:
                return sorted(tuple(v.co) for v in mesh.vertices if v.co.x < 0)
            finally:
                obj.to_mesh_clear()

    baseline = left_curve()
    t.assertGreater(len(baseline), 4)
    yield e.ctrl.shift.b()
    yield e.text('0.3')
    t.assertEqual(left_curve(), baseline, 'Untouched spline changed during bevel preview')
    yield e.wheelupmouse()
    t.assertEqual(left_curve(), baseline, 'Untouched spline changed when segment count changed')
    yield e.esc()
    t.assertEqual(left_curve(), baseline)
    yield e.ctrl.shift.b()
    yield e.text('0.3')
    t.assertEqual(left_curve(), baseline)
    yield e.ret()
    t.assertEqual(left_curve(), baseline)


def zero_chamfer_one_round():
    import bpy
    import re
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().one().numpad_7()
    area.spaces.active.region_3d.view_location = (0.5, 0.5, 0)
    area.spaces.active.region_3d.view_distance = 5
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.delete(type='VERT')
        bpy.ops.curve.draw_line(points='1 0 0 0 0 0 0 1 0')
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.ed.undo_push(message='Segment convention source')

    def counts():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        return tuple(map(int, re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+)', stats).groups()))

    def minimum_xy_sum():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = obj.to_mesh()
            try:
                return min(v.co.x + v.co.y for v in mesh.vertices)
            finally:
                obj.to_mesh_clear()

    t.assertEqual(counts(), (3, 2))
    yield e.ctrl.shift.b()
    yield e.text('0.3')
    t.assertEqual(counts(), (4, 3), 'One round arc should have no interior vertex')
    t.assertLess(minimum_xy_sum(), 0.2)
    yield e.wheeldownmouse()
    t.assertEqual(counts(), (4, 3))
    t.assertAlmostEqual(minimum_xy_sum(), 0.3, places=5)
    yield e.wheeldownmouse()
    t.assertAlmostEqual(minimum_xy_sum(), 0.3, places=5)
    yield e.wheelupmouse()
    t.assertEqual(counts(), (4, 3))
    t.assertLess(minimum_xy_sum(), 0.2)
    yield e.wheelupmouse()
    t.assertEqual(counts(), (5, 4), 'Two arc parts should have one interior vertex')
    t.assertLess(minimum_xy_sum(), 0.2)
    yield e.ret()
    t.assertEqual(counts(), (5, 4))
    yield e.ctrl.z()
    t.assertEqual(counts(), (3, 2))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (5, 4))


def surface_zero_chamfer_one_round():
    """A surface edge bevel starts as one round strip; zero chamfers and two splits it."""
    import bpy
    import re
    import math
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    curve = window.view_layer.objects.active.data
    t.assertEqual((curve.resolution_u, curve.resolution_v), (16, 16))
    yield e.tab().two().e().y().text('2').ret().e().z().text('2').ret()
    yield e.numpad_1().numpad_6().numpad_8()
    space = area.spaces.active
    space.region_3d.view_location = (0, 1.2, 0.7)
    space.region_3d.view_distance = 6
    bpy.context.window_manager.keyconfigs.active.preferences.select_mouse = 'RIGHT'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='DESELECT')
    yield
    co = location_3d_to_region_2d(region, space.region_3d, Vector((0, 2, 0)))
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.rightmouse()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.ed.undo_push(message='Shared surface edge')

    def counts():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        return tuple(map(int, re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats).groups()))

    def corner_distance():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = obj.to_mesh()
            try:
                return min(math.hypot(v.co.y - 2, v.co.z) for v in mesh.vertices)
            finally:
                obj.to_mesh_clear()

    before = counts()
    t.assertEqual(before, (6, 7, 2))
    yield e.ctrl.b()
    yield e.text('0.3')
    t.assertEqual(counts(), (8, 10, 3))
    t.assertLess(corner_distance(), 0.17, 'One segment is still a flat surface chamfer')
    yield e.wheeldownmouse()
    t.assertEqual(counts(), (8, 10, 3))
    t.assertGreater(corner_distance(), 0.20)
    yield e.wheelupmouse()
    t.assertEqual(counts(), (8, 10, 3))
    t.assertLess(corner_distance(), 0.17)
    yield e.wheelupmouse()
    t.assertEqual(counts(), (10, 13, 4))
    t.assertLess(corner_distance(), 0.17)
    yield e.ret()
    t.assertEqual(counts(), (10, 13, 4))
    yield e.ctrl.z()
    t.assertEqual(counts(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (10, 13, 4))
    yield e.ctrl.z()
    yield e.ctrl.b()
    yield e.text('0.3')
    yield e.wheeldownmouse()
    t.assertGreater(corner_distance(), 0.20)
    yield e.ret()
    yield e.ctrl.z()
    yield e.ctrl.b()
    yield e.text('0.3')
    t.assertLess(corner_distance(), 0.17, 'New bevel remembered the last chamfer segment count')
    yield e.esc()
