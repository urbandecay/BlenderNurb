# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Orbit and pan during every native drawing tool's construction stages."""
import modules.ui_test_utils as ui


def drawing_navigation():
    import bpy
    import re
    from mathutils import Quaternion, Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    view = space.region_3d

    def screen(point):
        co = location_3d_to_region_2d(region, view, Vector(point))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def counts():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def activate(tool, mode):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_' + tool)
            active = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
            props = active.operator_properties('curve.draw_' + tool)
            props.use_axis_snap = False
            if tool == 'spline':
                props.method = mode
            elif tool in {'arc', 'circle'}:
                props.mode = mode
                props.preview_points = 8
                props.use_angle_snap = False
                if tool == 'circle':
                    props.anchor_tangent_markers = False

    def navigate(tool):
        before = counts()
        start = ui.get_area_center(area)
        rotation = view.view_rotation.copy()
        yield from e.middlemouse.cursor_motion((start, (start[0] + 60, start[1] + 35)))
        t.assertGreater(view.view_rotation.rotation_difference(rotation).angle, 0.01, tool)
        t.assertEqual(counts(), before)
        location = view.view_location.copy()
        yield from e.shift.middlemouse.cursor_motion((start, (start[0] + 45, start[1] - 30)))
        t.assertGreater((view.view_location - location).length, 0.01, tool)
        t.assertEqual(counts(), before)
        with bpy.context.temp_override(window=window, area=area, region=region):
            active = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
            t.assertEqual(active.idname, 'builtin.nurbs_draw_' + tool)

    if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
        yield e.tab()
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().numpad_7()
    window.scene.tool_settings.use_snap = False

    cases = (
        ('line', '', ((-2, -1, 0), (0, 1, 0), (2, -1, 0)), (3, 2)),
        ('spline', 'FIT_POINTS', ((-2, -1, 0), (0, 1, 0), (2, -1, 0)), (2, 1)),
        ('spline', 'CONTROL_VERTICES', ((-2, -1, 0), (0, 1, 0), (2, -1, 0)), (2, 1)),
        ('arc', 'CENTER', ((0, 0, 0), (2, 0, 0), (0, 2, 0)), (9, 8)),
        ('arc', 'ENDPOINTS', ((-2, -1, 0), (2, -1, 0), (0, 1, 0)), (9, 8)),
        ('arc', 'THREE_POINTS', ((-2, -1, 0), (2, -1, 0), (1, 2, 0)), (9, 8)),
        ('circle', 'CENTER', ((0, 0, 0), (2, 0, 0)), (8, 8)),
        ('circle', 'DIAMETER', ((-2, 0, 0), (2, 0, 0)), (8, 8)),
        ('circle', 'THREE_POINTS', ((-2, -1, 0), (2, -1, 0), (0, 2, 0)), (8, 8)),
        ('circle', 'TAN_TAN', ((0, 0.5, 0), (0, 0.5, 0)), (8, 8)),
        ('circle', 'TAN_TAN_TAN', ((0, 0, 0), (0, 0, 0)), (8, 8)),
    )
    for tool, mode, points, added in cases:
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.curve.select_all(action='SELECT')
            bpy.ops.curve.delete(type='VERT')
            if mode == 'TAN_TAN':
                lines = ('-4 -2 0 4 -2 0', '-4 2 0 4 2 0')
            elif mode == 'TAN_TAN_TAN':
                lines = ('-4 -2 0 4 -2 0', '-4 -1.9 0 -0.1 4 0', '4 -1.9 0 0.1 4 0')
            else:
                lines = ()
            for line in lines:
                bpy.ops.curve.draw_line(points=line)
            bpy.ops.curve.select_all(action='SELECT')
            bpy.ops.ed.undo_push(message='Drawing navigation baseline')
        view.view_rotation = Quaternion((1, 0, 0, 0))
        view.view_location = (0, 0, 0)
        view.view_distance = 12
        view.view_perspective = 'ORTHO'
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        before = counts()
        activate(tool, mode)
        for i, point in enumerate(points):
            yield e.cursor_position_set(*screen(point), move=True)
            yield e.leftmouse()
            if i < len(points) - 1 or tool in {'line', 'spline'}:
                yield from navigate(tool)
        if tool in {'line', 'spline'}:
            yield e.ret()
        expected = (before[0] + added[0], before[1] + added[1], 0)
        t.assertEqual(counts(), expected, (tool, mode))
        yield e.ctrl.z()
        t.assertEqual(counts(), before, (tool, mode, 'undo'))
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), expected, (tool, mode, 'redo'))
        print('DRAW_NAVIGATION_PASS', tool, mode)

    # Navigation can also be followed by cancellation without leaving a view gesture active.
    before = counts()
    activate('line', '')
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    yield from navigate('line')
    yield e.esc()
    t.assertEqual(counts(), before)
