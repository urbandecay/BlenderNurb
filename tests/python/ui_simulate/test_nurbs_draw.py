# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native drawing through workspace tools, including snapping and cancellation."""

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
    window.scene.tool_settings.use_snap = False
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
        tool.operator_properties('curve.draw_line').use_axis_snap = False
    yield e.cursor_position_set(*screen((-2, -2, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((0, -3, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, -2, 0)), move=True)
    yield e.leftmouse().rightmouse()
    t.assertEqual(counts(), (before[0] + 3, before[1] + 2, before[2]))
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    with bpy.context.temp_override(window=window, area=area, region=region):
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        t.assertEqual(tool.idname, 'builtin.select_box')
    finished = counts()
    yield e.ctrl.z()
    t.assertEqual(counts(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), finished)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_spline')
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        tool.operator_properties('curve.draw_spline').use_axis_snap = False
        tool.operator_properties('curve.draw_spline').preview_points = 1
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
    yield e.ctrl.z()
    t.assertEqual(counts(), finished)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_spline')
    yield e.cursor_position_set(*screen((-2, 1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((-1, 3, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, 3, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, 1, 0)), move=True)
    yield e.leftmouse()
    yield e.tab().ret()
    t.assertLess(highest_y(), fit_height - 0.1)
    after_spline = counts()
    t.assertEqual(after_spline, (finished[0] + 2, finished[1] + 1, finished[2]))
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_spline')
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
        tool.operator_properties('curve.draw_line').use_axis_snap = False
        window.scene.tool_settings.use_snap = True
        window.scene.tool_settings.snap_elements = {'VERTEX'}
    yield e.cursor_position_set(*screen((1, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, -1, 0)), move=True)
    yield e.leftmouse().ret()
    t.assertEqual(counts(), (after_spline[0] + 1, after_spline[1] + 1, after_spline[2]))
    before_numeric = counts()
    with bpy.context.temp_override(window=window, area=area, region=region):
        window.scene.tool_settings.use_snap = False
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_line')
    yield e.cursor_position_set(*screen((3, -3, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((4, -3, 0)), move=True)
    yield e.text('1.5').ret().ret()
    t.assertEqual(counts(), (before_numeric[0] + 2, before_numeric[1] + 1, before_numeric[2]))
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    t.assertEqual(counts(), (before_numeric[0] + 2, before_numeric[1] + 1, before_numeric[2]))
    evaluated = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    t.assertGreater(max(v.co.x for v in mesh.vertices), 4.4)
    evaluated.to_mesh_clear()
    print('NURBS_DRAW_WORKFLOW_PASS', flush=True)


def native_center_snaps():
    """Committed endpoints must land on native face/edge centers from an offset cursor."""
    import bpy
    import os
    from pathlib import Path
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE']) as (_, data):
        data.curves = ['Native preview sheet']
    obj = bpy.data.objects.new('Center snap regression sheet', data.curves[0])
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    obj.scale = (2, 1, -1)
    window.scene.cursor.location = (0, 0, 3)
    output = Path('/tmp/nurbs-center-snaps')
    output.mkdir(exist_ok=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab().numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 12

    def screen(point):
        p = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(p)
        return round(p.x) + region.x, round(p.y) + region.y

    for mode, expected, label in [('FACE_MIDPOINT', (0, 0, 0), 'face'),
                                  ('FACE_MIDPOINT', (0, 0, 0), 'face-oblique'),
                                  ('FACE_MIDPOINT', (0, 0, 0), 'face-back'),
                                  ('EDGE_MIDPOINT', (0, -1, 0), 'edge'),
                                  ('FACE_MIDPOINT', None, 'no-center')]:
        if label == 'face-oblique':
            from mathutils import Quaternion
            import math
            space.region_3d.view_rotation = Quaternion((1, 0, 0), math.radians(65))
        if label == 'face-back':
            yield e.ctrl.numpad_7()
        with bpy.context.temp_override(window=window, area=area, region=region):
            settings = window.scene.tool_settings
            settings.use_snap = True
            settings.snap_elements = {mode} if expected is not None else {'FACE', 'FACE_MIDPOINT'}
            settings.use_snap_self = True
            settings.use_snap_edit = True
            settings.use_snap_nonedit = True
            settings.use_snap_backface_culling = True
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_line')
            tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
            tool.operator_properties('curve.draw_line').use_axis_snap = False
        yield e.cursor_position_set(*screen((-4, -3, 3)), move=True)
        yield e.leftmouse()
        x, y = screen(expected or (0, 0, 0))
        yield e.cursor_position_set(x + (4 if expected is not None else 40), y + 3, move=True)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / (label + '-preview.png')))
        yield e.leftmouse().rightmouse()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            used = {tuple(sorted(k)) for p in mesh.polygons for k in p.edge_keys}
            degrees = {}
            for edge in mesh.edges:
                a, b = edge.vertices
                if tuple(sorted((a, b))) not in used:
                    degrees[a] = degrees.get(a, 0) + 1
                    degrees[b] = degrees.get(b, 0) + 1
            endpoints = [obj.matrix_world @ mesh.vertices[i].co for i, n in degrees.items() if n == 1]
            t.assertEqual(len(endpoints), 2, (label, endpoints))
            if expected is not None:
                t.assertLess(min((v - Vector(expected)).length for v in endpoints), 1e-6,
                             (label, endpoints, expected))
            else:
                t.assertTrue(all(abs(v.z - 3) < 1e-6 for v in endpoints), endpoints)
        finally:
            evaluated.to_mesh_clear()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / (label + '-result.blend')), check_existing=False)
        yield e.ctrl.z()
    print('NURBS_NATIVE_CENTER_SNAPS_PASS', flush=True)


def locked_direction_vertex_snap():
    import bpy
    import re
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    bpy.context.preferences.view.smooth_view = 0
    yield e.tab().one().numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 16
    settings = window.scene.tool_settings
    settings.use_snap = True
    settings.snap_elements = {'VERTEX'}
    settings.use_snap_self = True
    settings.use_snap_edit = True
    settings.use_snap_nonedit = True
    mesh = bpy.data.meshes.new('Off-axis mesh snap target')
    mesh.from_pydata(((2, 2, 0),), (), ())
    target = bpy.data.objects.new(mesh.name, mesh)
    window.scene.collection.objects.link(target)
    with bpy.context.temp_override(window=window, area=area, region=region):
        # The second case picks an authoritative native vertex on the active object.
        bpy.ops.curve.draw_line(points='3 3 0 4 3 0')
        bpy.ops.curve.draw_line(points='-3 -2 0 -3 -4 0')

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(region.x + xy.x), round(region.y + xy.y)

    for start, approach, destination, expected, lock in (
            ((-3, -2, 0), (-2, -2, 0), (2, 2, 0), (2, -2, 0), 'SHIFT'),
            ((-3, -4, 0), (-2, -4, 0), (3, 3, 0), (3, -4, 0), 'X')):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_line')
            tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
            tool.operator_properties('curve.draw_line').use_axis_snap = True
        yield
        yield e.cursor_position_set(*screen(start), move=True)
        yield e.leftmouse()
        yield e.cursor_position_set(*screen(approach), move=True)
        if lock == 'SHIFT':
            e.shift.press()
            yield
        else:
            yield e.x()
        xy = screen(approach)
        e.cursor_position_set(*xy)
        window.event_simulate(type='MOUSEMOVE', value='NOTHING', x=xy[0], y=xy[1], shift=lock == 'SHIFT')
        yield
        # Offset the pointer so using its projected position cannot pass as vertex snapping.
        xy = screen(destination)
        e.cursor_position_set(xy[0] + 5, xy[1] + 3)
        window.event_simulate(type='MOUSEMOVE', value='NOTHING', x=xy[0] + 5, y=xy[1] + 3, shift=lock == 'SHIFT')
        yield
        if lock == 'SHIFT':
            yield e.shift.leftmouse()
        else:
            yield e.leftmouse()
        if lock == 'SHIFT':
            e.shift.release()
            yield
        yield e.ret()
        obj = window.view_layer.objects.active
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        display = evaluated.to_mesh()
        try:
            t.assertTrue(any((v.co - Vector(expected)).length < 1e-5 for v in display.vertices),
                         f'{lock}: missing constrained snap endpoint {expected}: {[tuple(v.co) for v in display.vertices]}')
        finally:
            evaluated.to_mesh_clear()
        t.assertTrue(obj.data.is_native_nurbs)
    # Projecting onto a native target must create a separate endpoint, not weld to that target.
    stats = window.scene.statistics(window.view_layer).replace(',', '')
    match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+)', stats)
    t.assertIsNotNone(match, stats)
    t.assertEqual(tuple(map(int, match.groups())), (8, 5))
    print('PASS NURBS constrained vertex snapping: Shift, X, mesh/native targets, no false weld', flush=True)
