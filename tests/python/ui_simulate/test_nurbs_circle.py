# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Circle placement, tangent solving, overlays and undo through real viewport events."""
import modules.ui_test_utils as ui


def circle_workflow():
    import bpy
    import re
    import math
    from pathlib import Path
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/nurbs-circle-overlays')
    output.mkdir(exist_ok=True)

    def screen(point):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def counts():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def activate(mode):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_circle')
            tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
            props = tool.operator_properties('curve.draw_circle')
            props.mode = mode
            props.use_axis_snap = False
            props.preview_points = 32
            props.anchor_tangent_markers = False

    def capture(name):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / (name + '.png')))

    if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
        yield e.tab()
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 12
    window.scene.tool_settings.use_snap = False

    for mode, points in (
        ('CENTER', ((0, 0, 0), (2, 0, 0))),
        ('DIAMETER', ((-2, -2, 0), (2, -2, 0))),
        ('THREE_POINTS', ((-2, 1, 0), (2, 1, 0), (1, 3, 0))),
    ):
        before = counts()
        activate(mode)
        for i, point in enumerate(points):
            yield e.cursor_position_set(*screen(point), move=True)
            if i:
                capture(mode.lower() + '-' + str(i))
            yield e.leftmouse()
        t.assertEqual(counts(), (before[0] + 32, before[1] + 32, 0))
        capture(mode.lower() + '-result')
        finished = counts()
        yield e.ctrl.z()
        t.assertEqual(counts(), before)
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), finished)

    before = counts()
    activate('THREE_POINTS')
    yield e.cursor_position_set(*screen((-2, -1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, -1, 0)), move=True)
    yield e.leftmouse().back_space()
    yield e.cursor_position_set(*screen((1, 1, 0)), move=True)
    yield e.esc()
    t.assertEqual(counts(), before)

    # Radius numeric input completes at the second placement.
    activate('CENTER')
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, 0, 0)), move=True)
    yield e.text('3').ret()
    t.assertEqual(counts(), (before[0] + 32, before[1] + 32, 0))

    # A line can share a non-quarter circle division, proving it is a source endpoint.
    before = counts()
    with bpy.context.temp_override(window=window, area=area, region=region):
        window.scene.tool_settings.use_snap = True
        window.scene.tool_settings.snap_elements = {'VERTEX'}
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_line')
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        tool.operator_properties('curve.draw_line').use_axis_snap = False
    yield e.cursor_position_set(*screen((3 * math.cos(math.pi / 8), -3 * math.sin(math.pi / 8), 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, -4, 0)), move=True)
    yield e.leftmouse().ret()
    t.assertEqual(counts(), (before[0] + 1, before[1] + 1, 0))
    window.scene.tool_settings.use_snap = False

    # Clear the source and create separate selected chains for the tangent modes.
    for mode, lines, seed in (
        ('TAN_TAN', ('-4 -2 0 4 -2 0', '-4 2 0 4 2 0'), (0, 0.5, 0)),
        ('TAN_TAN_TAN', ('-4 -2 0 4 -2 0', '-4 -1.9 0 -0.1 4 0',
                         '4 -1.9 0 0.1 4 0'), (0, 0, 0)),
    ):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.curve.select_all(action='SELECT')
            bpy.ops.curve.delete(type='VERT')
            for line in lines:
                bpy.ops.curve.draw_line(points=line)
            bpy.ops.curve.select_all(action='SELECT')
            bpy.ops.ed.undo_push(message='Tangent source curves')
        yield e.cursor_position_set(*screen(seed), move=True)
        before = counts()
        activate(mode)
        yield e.cursor_position_set(*screen(seed), move=True)
        yield e.leftmouse()
        yield e.cursor_position_set(*screen((seed[0] + 0.1, seed[1], 0)), move=True)
        capture(mode.lower())
        yield e.leftmouse()
        t.assertEqual(counts(), (before[0] + 32, before[1] + 32, 0))
        yield e.ctrl.z()
        t.assertEqual(counts(), before)
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), (before[0] + 32, before[1] + 32, 0))

    # Curved native supports and Tab's enclosing solution, not just straight chains.
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.delete(type='VERT')
        for points in ('-3 0 0 -2 0 0', '3 0 0 4 0 0', '0 5 0 1 5 0'):
            bpy.ops.curve.draw_circle(points=points)
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.ed.undo_push(message='Three curved tangent supports')
    yield e.cursor_position_set(*screen((0, 1.6, 0)), move=True)
    before = counts()
    activate('TAN_TAN_TAN')
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((0.1, 1.6, 0)), move=True)
    capture('three-curves-external')
    yield e.tab()
    yield e.t().wheelupmouse()
    capture('three-curves-enclosing')
    yield e.leftmouse()
    t.assertEqual(counts(), (before[0] + 36, before[1] + 36, 0))
    obj = window.view_layer.objects.active
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        t.assertGreater(max(v.co.x for v in mesh.vertices), 4.3)
        t.assertGreater(max(v.co.y for v in mesh.vertices), 5.9)
    finally:
        evaluated.to_mesh_clear()
