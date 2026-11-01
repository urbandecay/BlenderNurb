# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Control-point shortcuts must update the evaluated curve and visible edit cage."""

import modules.ui_test_utils as ui


def shortcut_visibility():
    import math
    import tempfile
    from pathlib import Path

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_arc(
            mode='CENTER', points='0 0 0 2 0 0', sweep=math.pi,
            sweep_exact=str(math.pi), preview_points=32)
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.dissolve_limited()
    obj = window.view_layer.objects.active
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    area.spaces.active.region_3d.view_location = (0, 1, 0)
    area.spaces.active.region_3d.view_distance = 8
    yield

    def modal_operators():
        return {op.bl_idname for op in window.modal_operators}

    def check_setting(expected):
        t.assertEqual(obj.data.show_nurbs_control_points, expected)
        with bpy.context.temp_override(window=window, area=area, region=region):
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            t.assertEqual(evaluated.data.show_nurbs_control_points, expected)

    # (2, 2, 0) is a coefficient control point outside this semicircle, so changes here
    # prove the control cage was drawn rather than merely testing the saved Boolean.
    point = location_3d_to_region_2d(region, area.spaces.active.region_3d, Vector((2, 2, 0)))
    t.assertIsNotNone(point)
    x, y = round(point.x) + region.x, round(point.y) + region.y

    with tempfile.TemporaryDirectory(prefix='nurbs-control-points-') as directory:
        def control_point_pixels(label):
            path = str(Path(directory) / (label + '.png'))
            with bpy.context.temp_override(window=window):
                bpy.ops.screen.screenshot(filepath=path)
            image = bpy.data.images.load(path, check_existing=False)
            try:
                width, height = image.size
                t.assertTrue(8 <= x < width - 8 and 8 <= y < height - 8)
                pixels = tuple(image.pixels)
                return [pixels[(py * width + px) * 4:(py * width + px) * 4 + 3]
                        for py in range(y - 6, y + 7) for px in range(x - 6, x + 7)]
            finally:
                bpy.data.images.remove(image)

        def changed_pixels(a, b):
            return sum(max(abs(x - y) for x, y in zip(pa, pb)) > 0.05 for pa, pb in zip(a, b))

        check_setting(False)
        before = control_point_pixels('before')
        e.c.press()
        yield
        t.assertIn('CURVE_OT_nurbs_control_points_sequence', modal_operators())
        t.assertNotIn('VIEW3D_OT_select_circle', modal_operators())
        e.p.press()
        yield
        e.p.release()
        e.c.release()
        yield
        check_setting(True)
        t.assertNotIn('VIEW3D_OT_select_circle', modal_operators())
        t.assertGreater(changed_pixels(before, control_point_pixels('enabled')), 8)

        yield e.two()
        t.assertGreater(changed_pixels(before, control_point_pixels('edge-mode')), 8)

        e.c.press()
        yield
        e.p.press()
        yield
        e.p.release()
        e.c.release()
        yield
        check_setting(False)
        t.assertEqual(changed_pixels(before, control_point_pixels('disabled')), 0)

        e.c.press()
        yield
        t.assertNotIn('VIEW3D_OT_select_circle', modal_operators())
        e.c.release()
        yield
        t.assertIn('VIEW3D_OT_select_circle', modal_operators())
        yield e.esc()


def right_click_editing():
    """Select real coefficients with RMB, deform their curve, cancel, and undo/redo."""
    import math

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    preferences = bpy.context.window_manager.keyconfigs.active.preferences
    previous_mouse = preferences.select_mouse
    preferences.select_mouse = 'RIGHT'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_arc(mode='CENTER', points='0 0 0 2 0 0', sweep=math.pi,
                               sweep_exact=str(math.pi), preview_points=32)
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.dissolve_limited()
        bpy.ops.curve.select_all(action='DESELECT')
        bpy.ops.curve.nurbs_control_points_toggle()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    area.spaces.active.region_3d.view_location = (0, 1, 0)
    area.spaces.active.region_3d.view_distance = 10
    area.tag_redraw()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)
    yield

    def screen(position):
        co = location_3d_to_region_2d(region, area.spaces.active.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def points():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return sorted(tuple(round(value, 6) for value in vertex.co) for vertex in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

    baseline = points()
    t.assertTrue(all(abs(p[2]) < 1e-6 for p in baseline))
    yield e.cursor_position_set(*screen((2, 2, 0)), move=True)
    yield e.rightmouse()
    yield
    yield e.g().z().text('1').ret()
    moved = points()
    t.assertGreater(max(p[2] for p in moved), 0.4)
    t.assertLess(max(p[2] for p in moved), 0.42)
    yield e.g().z().text('3').esc()
    t.assertEqual(points(), moved)
    yield e.ctrl.z()
    t.assertEqual(points(), baseline)
    yield e.ctrl.shift.z()
    t.assertEqual(points(), moved)

    yield e.cursor_position_set(*screen((-2, 2, 0)), move=True)
    yield e.shift.rightmouse()
    yield e.g().z().text('1').ret()
    both = points()
    t.assertGreater(max(p[2] for p in both if p[0] < -0.2), 0.4)
    t.assertGreater(max(p[2] for p in both if p[0] > 0.2), 0.8)
    yield e.r().z().text('30').ret()
    t.assertNotEqual(points(), both)
    yield e.ctrl.z()
    t.assertEqual(points(), both)
    yield e.s().text('2').ret()
    t.assertNotEqual(points(), both)
    yield e.ctrl.z()
    t.assertEqual(points(), both)

    yield e.cursor_position_set(*screen((2, 0, 0)), move=True)
    yield e.rightmouse()
    yield e.g().x().text('1').ret()
    t.assertGreater(max(p[0] for p in points()), 2.9)
    preferences.select_mouse = previous_mouse


def bulk_selection_editing():
    """A, box, circle and lasso must select coefficients rather than topology stations."""
    import math
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_arc(mode='CENTER', points='0 0 0 2 0 0', sweep=math.pi,
                               sweep_exact=str(math.pi), preview_points=32)
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.dissolve_limited()
        bpy.ops.curve.nurbs_control_points_toggle()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    area.spaces.active.region_3d.view_location = (0, 1, 0)
    area.spaces.active.region_3d.view_distance = 10
    yield

    def points():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return sorted(tuple(round(v, 5) for v in vert.co) for vert in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

    def screen(position):
        co = location_3d_to_region_2d(region, area.spaces.active.region_3d, Vector(position))
        return round(co.x), round(co.y)

    baseline = points()
    yield e.a().g().z().text('1').ret()
    t.assertEqual(points(), sorted((x, y, round(z + 1, 5)) for x, y, z in baseline))
    yield e.ctrl.z()
    t.assertEqual(points(), baseline)
    x, y = screen((2, 2, 0))
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='DESELECT')
        bpy.ops.view3d.select_box(xmin=x - 8, xmax=x + 8, ymin=y - 8, ymax=y + 8, mode='SET')
    yield e.g().z().text('1').ret()
    t.assertGreater(max(p[2] for p in points()), 0.4)
    t.assertLess(max(p[2] for p in points()), 0.42)
    yield e.ctrl.z()
    t.assertEqual(points(), baseline)
    for kind in ('circle', 'lasso'):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.curve.select_all(action='DESELECT')
            if kind == 'circle':
                bpy.ops.view3d.select_circle(x=x, y=y, radius=8, mode='SET')
            else:
                bpy.ops.view3d.select_lasso(path=[{'name': '', 'loc': (px, py), 'time': 0}
                    for px, py in ((x-8, y-8), (x+8, y-8), (x+8, y+8), (x-8, y+8))], mode='SET')
        yield e.g().z().text('1').ret()
        t.assertGreater(max(p[2] for p in points()), 0.4)
        t.assertLess(max(p[2] for p in points()), 0.42)
        yield e.ctrl.z()
        t.assertEqual(points(), baseline)


def surface_boundary_editing():
    """A visible boundary handle must deform the surface and retain its opposite boundary."""
    import math
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Euler, Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    preferences = bpy.context.window_manager.keyconfigs.active.preferences
    previous_mouse = preferences.select_mouse
    preferences.select_mouse = 'RIGHT'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_arc(mode='CENTER', points='0 0 0 2 0 0', sweep=math.pi,
                               sweep_exact=str(math.pi), preview_points=32)
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.dissolve_limited()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.two().a().e().z().text('2').ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='DESELECT')
        bpy.ops.curve.nurbs_control_points_toggle()
    space = area.spaces.active
    space.region_3d.view_location = (0, 1, 1)
    space.region_3d.view_distance = 11
    space.region_3d.view_rotation = Euler((math.radians(55), 0, math.radians(15))).to_quaternion()
    space.region_3d.view_perspective = 'ORTHO'
    space.shading.show_xray = True
    area.tag_redraw()
    yield

    def points():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return sorted(tuple(round(v, 5) for v in vert.co) for vert in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

    baseline = points()
    t.assertTrue(any(p[2] == 2 for p in baseline))
    co = location_3d_to_region_2d(region, space.region_3d, Vector((2, 2, 0)))
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.rightmouse()
    yield e.g().x().text('1').ret()
    moved = points()
    t.assertNotEqual(moved, baseline)
    t.assertEqual([p for p in moved if p[2] == 2], [p for p in baseline if p[2] == 2])
    t.assertGreater(max(p[0] for p in moved), max(p[0] for p in baseline))
    yield e.g().z().text('3').esc()
    t.assertEqual(points(), moved)
    yield e.ctrl.z()
    t.assertEqual(points(), baseline)
    yield e.ctrl.shift.z()
    t.assertEqual(points(), moved)
    preferences.select_mouse = previous_mouse


def per_arc_circle_controls(*, arc_count=12):
    """Every equal arc has a single tangent handle, including arcs crossing a quadrant."""
    import math
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    preferences = bpy.context.window_manager.keyconfigs.active.preferences
    previous_mouse = preferences.select_mouse
    preferences.select_mouse = 'RIGHT'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_circle(points='0 0 0 2 0 0', preview_points=arc_count)
        # The primitive supplies the edit owner; remove its original line after adding the circle.
        bpy.ops.curve.select_all(action='INVERT')
        bpy.ops.curve.delete(type='VERT')
        bpy.ops.curve.select_all(action='DESELECT')
        bpy.ops.curve.nurbs_control_points_toggle()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    space = area.spaces.active
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 8
    area.tag_redraw()
    yield

    def points():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return sorted(tuple(round(v, 6) for v in vert.co) for vert in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    baseline = points()
    half = math.pi / arc_count
    arc_index = arc_count // 4
    low_angle = 2 * half * arc_index
    high_angle = low_angle + 2 * half
    handle_angle = low_angle + half
    radius = 2 / math.cos(half)
    yield e.cursor_position_set(*screen((radius * math.cos(handle_angle),
                                         radius * math.sin(handle_angle), 0)), move=True)
    yield e.rightmouse()
    # Selecting materializes the quadratic and can change preview sample positions.
    # Verify the actual circle, then use that selected state as the transform baseline.
    baseline = points()
    t.assertTrue(all(abs(math.hypot(x, y) - 2) < 1e-5 and abs(z) < 1e-5
                     for x, y, z in baseline))
    yield e.g().z().text('1').ret()
    moved = points()
    lifted = [p for p in moved if p[2] > 1e-5]
    t.assertTrue(lifted)
    t.assertGreater(max(p[2] for p in lifted), 0.45)
    t.assertLess(max(p[2] for p in lifted), 0.55)
    t.assertTrue(all(low_angle - 1e-5 <= math.atan2(y, x) <= high_angle + 1e-5
                     for x, y, z in lifted))
    yield e.g().z().text('3').esc()
    t.assertEqual(points(), moved)
    yield e.ctrl.z()
    t.assertEqual(points(), baseline)
    yield e.ctrl.shift.z()
    t.assertEqual(points(), moved)
    handle_angle += 2 * half
    yield e.cursor_position_set(*screen((radius * math.cos(handle_angle),
                                         radius * math.sin(handle_angle), 0)), move=True)
    yield e.shift.rightmouse()
    yield e.g().z().text('1').ret()
    lifted = [p for p in points() if p[2] > 1e-5]
    t.assertTrue(all(low_angle - 1e-5 <= math.atan2(y, x) <= high_angle + 2 * half + 1e-5
                     for x, y, z in lifted))
    t.assertGreater(max(z for x, y, z in lifted if math.atan2(y, x) > high_angle), 0.45)
    t.assertGreater(max(z for x, y, z in lifted if math.atan2(y, x) < high_angle), 0.9)
    previous_max = max(z for x, y, z in lifted)
    yield e.a().g().z().text('1').ret()
    t.assertGreaterEqual(min(p[2] for p in points()), 1 - 1e-5)
    t.assertAlmostEqual(max(p[2] for p in points()), previous_max + 1, places=5)
    preferences.select_mouse = previous_mouse


def ten_arc_circle_controls():
    yield from per_arc_circle_controls(arc_count=10)


def thirteen_arc_circle_controls():
    yield from per_arc_circle_controls(arc_count=13)


def toggle_preserves_extrusion_selection():
    """CP inherits an extruded cap's selected controls, allowing an immediate G."""
    import math
    import tempfile
    from pathlib import Path
    import bpy
    from mathutils import Euler, Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_arc(mode='CENTER', points='0 0 0 2 0 0', sweep=math.pi,
                               sweep_exact=str(math.pi), preview_points=32)
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.dissolve_limited()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.two().a().e().z().text('2').ret()
    space = area.spaces.active
    space.region_3d.view_location = (0, 1, 1)
    space.region_3d.view_distance = 11
    space.region_3d.view_rotation = Euler((math.radians(55), 0, math.radians(15))).to_quaternion()
    space.region_3d.view_perspective = 'ORTHO'
    space.shading.show_xray = True
    yield

    def points():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return sorted(tuple(round(v, 5) for v in vert.co) for vert in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

    baseline = points()
    e.c.press()
    yield
    e.p.press()
    yield
    e.p.release()
    e.c.release()
    yield
    t.assertTrue(window.view_layer.objects.active.data.show_nurbs_control_points)
    t.assertEqual(points(), baseline)
    # The off-curve tangent coefficient must already have the selected orange color.
    co = location_3d_to_region_2d(region, space.region_3d, Vector((2, 2, 2)))
    x, y = round(co.x) + region.x, round(co.y) + region.y
    with tempfile.TemporaryDirectory(prefix='nurbs-inherited-control-selection-') as directory:
        path = str(Path(directory) / 'selected.png')
        with bpy.context.temp_override(window=window):
            bpy.ops.screen.screenshot(filepath=path)
        image = bpy.data.images.load(path, check_existing=False)
        try:
            width, height = image.size
            pixels = tuple(image.pixels)
            colors = [pixels[(py * width + px) * 4:(py * width + px) * 4 + 3]
                      for py in range(y - 5, y + 6) for px in range(x - 5, x + 6)]
            t.assertTrue(any(r > .7 and .2 < g < .8 and b < .2 for r, g, b in colors))
        finally:
            bpy.data.images.remove(image)
    yield e.ctrl.z()
    t.assertFalse(window.view_layer.objects.active.data.show_nurbs_control_points)
    yield e.ctrl.shift.z()
    t.assertTrue(window.view_layer.objects.active.data.show_nurbs_control_points)
    yield e.g().z().text('1').ret()
    moved = points()
    t.assertAlmostEqual(max(p[2] for p in moved), 3, places=5)
    t.assertEqual([p for p in moved if p[2] == 0], [p for p in baseline if p[2] == 0])
    yield e.g().z().text('3').esc()
    t.assertEqual(points(), moved)
    yield e.ctrl.z()
    t.assertEqual(points(), baseline)
    yield e.ctrl.shift.z()
    t.assertEqual(points(), moved)


def toggle_preserves_single_vertex_selection():
    """A selected curve endpoint becomes a selected endpoint control, without selecting its arc."""
    import math
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    preferences = bpy.context.window_manager.keyconfigs.active.preferences
    old_mouse = preferences.select_mouse
    preferences.select_mouse = 'RIGHT'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_arc(mode='CENTER', points='0 0 0 2 0 0', sweep=math.pi,
                               sweep_exact=str(math.pi), preview_points=32)
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.dissolve_limited()
        bpy.ops.curve.select_all(action='DESELECT')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    space = area.spaces.active
    space.region_3d.view_location = (0, 1, 0)
    space.region_3d.view_distance = 10
    yield
    co = location_3d_to_region_2d(region, space.region_3d, Vector((2, 0, 0)))
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.rightmouse()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.nurbs_control_points_toggle()
    yield e.g().z().text('1').ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        obj = window.view_layer.objects.active
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            points = [tuple(v.co) for v in mesh.vertices]
            t.assertTrue(any(abs(x - 2) < 1e-5 and abs(y) < 1e-5 and abs(z - 1) < 1e-5
                             for x, y, z in points))
            t.assertTrue(any(abs(x + 2) < 1e-5 and abs(y) < 1e-5 and abs(z) < 1e-5
                             for x, y, z in points))
        finally:
            evaluated.to_mesh_clear()
    preferences.select_mouse = old_mouse


def extruded_circle_four_vertices():
    """Select four ring vertices, show CP, and move them without a false boundary error."""
    import math
    import re
    import bpy
    from mathutils import Euler, Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    preferences = bpy.context.window_manager.keyconfigs.active.preferences
    old_mouse = preferences.select_mouse
    preferences.select_mouse = 'RIGHT'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_circle(points='0 0 0 2 0 0', preview_points=10)
        bpy.ops.curve.select_all(action='INVERT')
        bpy.ops.curve.delete(type='VERT')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.two().a().e().z().text('2').ret().one().alt.a()
    space = area.spaces.active
    space.region_3d.view_location = (0, 0, 1)
    space.region_3d.view_distance = 11
    space.region_3d.view_rotation = Euler((math.radians(55), 0, math.radians(15))).to_quaternion()
    space.region_3d.view_perspective = 'ORTHO'
    space.shading.show_xray = True
    yield
    selected = [(2 * math.cos(2 * math.pi * i / 10), 2 * math.sin(2 * math.pi * i / 10), z)
                for i in (2, 7) for z in (0, 2)]
    for i, position in enumerate(selected):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        yield e.shift.rightmouse() if i else e.rightmouse()
    stats = window.scene.statistics(window.view_layer).replace(',', '')
    t.assertIsNotNone(re.search(r'Verts:4/20', stats), stats)
    e.c.press()
    yield
    e.p.press()
    yield
    e.p.release()
    e.c.release()
    yield

    def points():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return sorted(tuple(round(v, 5) for v in vert.co) for vert in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

    baseline = points()
    yield e.g().z().text('1').ret()
    moved = points()
    t.assertNotEqual(moved, baseline)
    for x, y, z in selected:
        t.assertTrue(any(abs(px-x) < 1e-4 and abs(py-y) < 1e-4 and abs(pz-z-1) < 1e-4
                         for px, py, pz in moved))
    t.assertTrue(any(abs(x-2) < 1e-4 and abs(y) < 1e-4 and abs(z) < 1e-4
                     for x, y, z in moved))
    yield e.g().z().text('3').esc()
    t.assertEqual(points(), moved)
    yield e.ctrl.z()
    t.assertEqual(points(), baseline)
    yield e.ctrl.shift.z()
    t.assertEqual(points(), moved)
    preferences.select_mouse = old_mouse


def extruded_circle_control_spacing(*, existing_controls=False, highlight=False):
    """An arc across an old quarter join has one editable tangent handle after extrusion."""
    import math
    import bpy
    from mathutils import Euler, Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    preferences = bpy.context.window_manager.keyconfigs.active.preferences
    previous_mouse = preferences.select_mouse
    preferences.select_mouse = 'RIGHT'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_circle(points='0 0 0 2 0 0', preview_points=10)
        bpy.ops.curve.select_all(action='INVERT')
        bpy.ops.curve.delete(type='VERT')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.two().a().e().z().text('2').ret()
    space = area.spaces.active
    space.region_3d.view_location = (0, 0, 1)
    space.region_3d.view_distance = 10
    space.region_3d.view_rotation = Euler((math.radians(55), 0, math.radians(15))).to_quaternion()
    space.region_3d.view_perspective = 'ORTHO'
    space.shading.show_xray = True
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.select_all(action='DESELECT')
        if existing_controls:
            # Recreate the old saved state: cropped controls were already materialized.
            window.view_layer.objects.active.data.show_nurbs_control_points = True
            bpy.ops.curve.select_all(action='SELECT')
            window.view_layer.objects.active.data.show_nurbs_control_points = False
        else:
            bpy.ops.curve.nurbs_control_points_toggle()
    yield
    if existing_controls:
        e.c.press()
        yield
        e.p.press()
        yield
        e.p.release()
        e.c.release()
        yield

    def points():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return sorted(tuple(round(v, 6) for v in vert.co) for vert in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

    baseline = points()
    t.assertTrue(all(abs(math.hypot(x, y) - 2) < 1e-5 for x, y, z in baseline))
    if highlight:
        import tempfile
        from pathlib import Path
        yield e.one()
        selected = [(2 * math.cos(a), 2 * math.sin(a), 2)
                    for a in (2 * math.pi / 10, 4 * math.pi / 10)]
        screens = [location_3d_to_region_2d(region, space.region_3d, Vector(p)) for p in selected]
        for i, co in enumerate(screens):
            yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
            yield e.shift.rightmouse() if i else e.rightmouse()
        with tempfile.TemporaryDirectory(prefix='nurbs-cp-multiple-highlight-') as directory:
            path = str(Path(directory) / 'selected.png')
            with bpy.context.temp_override(window=window):
                bpy.ops.screen.screenshot(filepath=path)
            image = bpy.data.images.load(path, check_existing=False)
            try:
                width, height = image.size
                pixels = tuple(image.pixels)
                for co in screens:
                    x, y = round(co.x) + region.x, round(co.y) + region.y
                    colors = [pixels[(py * width + px) * 4:(py * width + px) * 4 + 3]
                              for py in range(y - 2, y + 3) for px in range(x - 2, x + 3)]
                    t.assertTrue(any(r > .7 and g > .2 for r, g, b in colors),
                                 'A selected CP endpoint was covered by an unselected copy')
            finally:
                bpy.data.images.remove(image)
        yield e.g().z().text('1').ret()
        moved = points()
        for x, y, z in selected:
            t.assertTrue(any(abs(px-x) < 1e-4 and abs(py-y) < 1e-4 and abs(pz-z-1) < 1e-4
                             for px, py, pz in moved))
        yield e.ctrl.z()
        t.assertEqual(points(), baseline)
        preferences.select_mouse = previous_mouse
        return
    if existing_controls:
        yield e.g().z().text('1').ret()
        moved = points()
        t.assertEqual(moved, sorted((x, y, round(z + 1, 6)) for x, y, z in baseline))
        yield e.ctrl.z()
        t.assertEqual(points(), baseline)
        preferences.select_mouse = previous_mouse
        return
    co = location_3d_to_region_2d(region, space.region_3d, Vector((0, 2 / math.cos(math.pi / 10), 2)))
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.rightmouse()
    yield e.g().z().text('1').ret()
    moved = points()
    t.assertGreater(max(z for x, y, z in moved), 2.45)
    t.assertLess(max(z for x, y, z in moved), 2.55)
    t.assertEqual([p for p in baseline if p[2] == 0], [p for p in moved if p[2] == 0])
    yield e.g().z().text('3').esc()
    t.assertEqual(points(), moved)
    yield e.ctrl.z()
    t.assertEqual(points(), baseline)
    yield e.ctrl.shift.z()
    t.assertEqual(points(), moved)
    preferences.select_mouse = previous_mouse


def existing_extruded_circle_control_spacing():
    yield from extruded_circle_control_spacing(existing_controls=True)


def selected_control_points_stay_highlighted():
    yield from extruded_circle_control_spacing(highlight=True)
