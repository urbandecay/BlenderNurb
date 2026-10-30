# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""GG sliding of native NURBS vertices through viewport events."""
import modules.ui_test_utils as ui


def vertex_slide_workflow():
    import bpy
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
    yield e.tab().one().numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 10
    window.scene.tool_settings.use_snap = False

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(region.x + xy.x), round(region.y + xy.y)

    def bounds():
        obj = window.view_layer.objects.active
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            return (min(v.co.x for v in mesh.vertices), max(v.co.x for v in mesh.vertices),
                    max(abs(v.co.y) + abs(v.co.z) for v in mesh.vertices))
        finally:
            evaluated.to_mesh_clear()

    def assert_bounds(expected):
        for actual, value in zip(bounds(), expected):
            t.assertAlmostEqual(actual, value, places=5)
        t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)

    yield e.alt.a()
    yield e.cursor_position_set(*screen((1, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.g().g()
    yield e.cursor_position_set(*screen((0.5, 0.4, 0)), move=True)
    # Cursor motion has a perpendicular component: the vertex must stay on its edge.
    t.assertLess(bounds()[1], 1)
    t.assertAlmostEqual(bounds()[2], 0, places=5)
    yield e.esc()
    assert_bounds((-1, 1, 0))
    yield e.g().g().text('0.5').ret()
    assert_bounds((-1, 0, 0))
    yield e.ctrl.z()
    assert_bounds((-1, 1, 0))
    yield e.ctrl.shift.z()
    assert_bounds((-1, 0, 0))
    # A later ordinary move must use the translation adapter again.
    yield e.g().y().text('1').ret()
    t.assertAlmostEqual(bounds()[2], 1, places=5)
    print('PASS NURBS GG: constrained mouse motion, cancel, numeric half slide, undo, redo, move')
    yield e.ctrl.z()
    yield e.a().g().g().text('0.25').ret()
    assert_bounds((-0.75, -0.25, 0))
    print('PASS NURBS GG: multiple selected vertices slide independently')


def vertex_slide_corner():
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().one().numpad_7()
    space = area.spaces.active
    space.region_3d.view_location = (0, 2, 0)
    space.region_3d.view_distance = 12
    window.scene.tool_settings.use_snap = False
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.draw_line(points='0 3 0\n2 3 0\n2 5 0\n')

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(region.x + xy.x), round(region.y + xy.y)

    def has_vertex(point):
        obj = window.view_layer.objects.active
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            return any((v.co - Vector(point)).length < 1e-5 for v in mesh.vertices)
        finally:
            evaluated.to_mesh_clear()

    for target, expected in (((2, 4, 0), (2, 4, 0)), ((1, 3, 0), (1, 3, 0))):
        yield e.alt.a()
        yield e.cursor_position_set(*screen((2, 3, 0)), move=True)
        yield e.leftmouse().g().g()
        yield e.cursor_position_set(*screen(target), move=True)
        yield e.text('0.5').ret()
        t.assertTrue(has_vertex(expected), str(expected))
        t.assertFalse(has_vertex((2, 3, 0)))
        yield e.ctrl.z()
        t.assertTrue(has_vertex((2, 3, 0)))
    print('PASS NURBS GG: mouse direction chooses either incident edge at a corner')
