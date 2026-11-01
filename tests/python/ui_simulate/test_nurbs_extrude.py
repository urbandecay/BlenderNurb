# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native face extrusion direction and manifold topology match mesh workflows."""
import modules.ui_test_utils as ui


def workflow(*, manifold=False, inward=False, rotated=False, zero=False, collision=False, cancel=False,
             menu=None):
    import math
    import re
    import bpy
    from mathutils import Vector
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    window.scene.tool_settings.use_snap = False
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().e().y().text('2').ret().a().three()
    if not rotated:
        yield e.e().text('2').ret()
    else:
        yield e.r().y().text('37').ret()

    def points():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return [Vector(v.co) for v in mesh.vertices]
            finally:
                evaluated.to_mesh_clear()

    def totals():
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)',
                          window.scene.statistics(window.view_layer).replace(',', ''))
        t.assertIsNotNone(match)
        return tuple(map(int, match.groups()))[1::2]

    previous_mouse = bpy.context.window_manager.keyconfigs.active.preferences.select_mouse
    if collision:
        from mathutils import Vector
        from bpy_extras.view3d_utils import location_3d_to_region_2d
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.curve.draw_line(points='0 -1 3 0 3 3')
            bpy.ops.curve.select_all(action='DESELECT')
        bpy.context.window_manager.keyconfigs.active.preferences.select_mouse = 'RIGHT'
        yield e.three().numpad_7()
        area.spaces.active.region_3d.view_location = (0, 1, 0)
        area.spaces.active.region_3d.view_distance = 8
        yield
        co = location_3d_to_region_2d(region, area.spaces.active.region_3d, Vector((0, 1, 2)))
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        yield e.rightmouse()
        t.assertIn('Faces:1/6', window.scene.statistics(window.view_layer))
    baseline = points()
    before = totals()
    if menu == 'ALT_E':
        yield e.alt.e()
        cx, cy = ui.get_area_center(area)
        yield e.cursor_position_set(cx - 80, cy - (20 if manifold else 0), move=True)
        yield e.leftmouse()
    elif menu == 'NURBS':
        yield from ui.call_menu(e, 'NURBS -> Extrude Manifold' if manifold else 'NURBS -> Extrude')
    elif manifold:
        yield from ui.call_operator(e, 'Extrude Manifold')
    else:
        yield e.e()
    distance = 0 if zero else -1 if inward else 1
    if cancel:
        yield e.text('1').esc()
        t.assertAlmostEqual(max(p.z for p in points()), 2, places=5)
        yield e.ctrl.z()
        t.assertEqual(totals(), before)
        return
    yield e.text(str(distance)).ret()
    moved = points()
    if rotated:
        direction = Vector((math.sin(math.radians(37)), 0, math.cos(math.radians(37))))
        for point in baseline:
            t.assertTrue(any((candidate - point - direction).length < 1e-5 for candidate in moved))
    else:
        t.assertAlmostEqual(max(p.z for p in moved), 2 + distance, places=5)
        if manifold:
            if collision:
                t.assertEqual(totals(), (12, 17, 6))
            else:
                t.assertEqual(totals(), before)
                t.assertEqual(totals(), (8, 12, 6))
        else:
            t.assertEqual(totals(), (12, 20, 10))
    after = sorted(tuple(round(v, 5) for v in p) for p in moved)
    yield e.ctrl.z()
    t.assertEqual(totals(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(sorted(tuple(round(v, 5) for v in p) for p in points()), after)
    bpy.context.window_manager.keyconfigs.active.preferences.select_mouse = previous_mouse


def face_normal():
    yield from workflow()


def rotated_face_normal():
    yield from workflow(rotated=True)


def manifold_outward():
    yield from workflow(manifold=True)


def alt_e_manifold():
    yield from workflow(manifold=True, menu='ALT_E')


def alt_e_extrude():
    yield from workflow(menu='ALT_E')


def alt_e_manifold_inward():
    yield from workflow(manifold=True, inward=True, menu='ALT_E')


def alt_e_manifold_cancel():
    yield from workflow(manifold=True, cancel=True, menu='ALT_E')


def nurbs_menu_manifold():
    yield from workflow(manifold=True, menu='NURBS')


def manifold_inward():
    yield from workflow(manifold=True, inward=True)


def manifold_zero():
    yield from workflow(manifold=True, zero=True)


def curved_cap(*, inward=False):
    import math
    import re
    import bpy
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    window.scene.tool_settings.use_snap = False
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
        bpy.ops.curve.draw_arc(mode='CENTER', points='0 0 0 2 0 0', sweep=math.pi,
                               sweep_exact=str(math.pi), preview_points=32)
        bpy.ops.curve.select_all(action='INVERT')
        bpy.ops.curve.delete(type='VERT')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.two().a()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.dissolve_limited()
    yield e.f().f().three().e().text('2').ret()
    before = window.scene.statistics(window.view_layer)
    def height():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                points = [tuple(v.co) for v in mesh.vertices]
                maximum = max(p[2] for p in points)
                t.assertTrue(all(abs(math.hypot(x, y) - 2) < 1e-5 or abs(y) < 1e-5 or
                                 abs(z) < 1e-5 or abs(z - maximum) < 1e-5
                                 for x, y, z in points))
                return max(p[2] for p in points)
            finally:
                evaluated.to_mesh_clear()
    t.assertAlmostEqual(height(), 2, places=5)
    yield from ui.call_operator(e, 'Extrude Manifold')
    yield e.text('-1' if inward else '1').ret()
    t.assertAlmostEqual(height(), 1 if inward else 3, places=5)
    def totals(value):
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', value)
        return tuple(map(int, match.groups()))[1::2]
    t.assertEqual(totals(window.scene.statistics(window.view_layer)), totals(before))
    yield e.ctrl.z()
    t.assertAlmostEqual(height(), 2, places=5)
    yield e.ctrl.shift.z()
    t.assertAlmostEqual(height(), 1 if inward else 3, places=5)


def curved_cap_inward():
    yield from curved_cap(inward=True)


def manifold_edge_intersections():
    yield from workflow(manifold=True, collision=True)


def manifold_cancel():
    yield from workflow(manifold=True, cancel=True)


def face_extrude_cancel():
    yield from workflow(cancel=True)


def through_hole(*, depth=4, cancel=False):
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    window.scene.tool_settings.use_snap = False
    bpy.context.window_manager.keyconfigs.active.preferences.select_mouse = 'RIGHT'
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().e().y().text('3').ret().a().three().e().text('3').ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.subdivide(number_cuts=2)
    yield e.alt.a().numpad_7()
    area.spaces.active.region_3d.view_location = (0, 1.5, 0)
    area.spaces.active.region_3d.view_distance = 8
    yield
    co = location_3d_to_region_2d(region, area.spaces.active.region_3d, Vector((0, 1.5, 3)))
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.rightmouse()
    before = window.scene.statistics(window.view_layer).split(' | Duration:')[0]
    t.assertIn('Faces:1/', before)

    def hits_floor():
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            obj = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = obj.to_mesh()
            try:
                from mathutils.bvhtree import BVHTree
                tree = BVHTree.FromPolygons([v.co for v in mesh.vertices],
                                           [list(p.vertices) for p in mesh.polygons])
                hit = tree.ray_cast(Vector((0, 1.5, -1)), Vector((0, 0, 1)))
                t.assertGreaterEqual(min(v.co.z for v in mesh.vertices), -1e-6)
                return hit[0] is not None
            finally:
                obj.to_mesh_clear()
    t.assertTrue(hits_floor())
    yield from ui.call_operator(e, 'Extrude Manifold')
    yield e.text(str(-depth))
    t.assertEqual(hits_floor(), depth < 3)
    if depth == 4 and not cancel:
        yield e.back_space().text('1')
        t.assertTrue(hits_floor())
        yield e.back_space().text('4')
        t.assertFalse(hits_floor())
    if cancel:
        yield e.esc()
        t.assertTrue(hits_floor())
        # Like mesh extrusion, Esc cancels movement; Undo removes the zero-distance extrusion.
        yield e.ctrl.z()
        t.assertTrue(hits_floor())
        t.assertEqual(window.scene.statistics(window.view_layer).split(' | Duration:')[0], before)
        return
    yield e.ret()
    t.assertEqual(hits_floor(), depth < 3)
    after = window.scene.statistics(window.view_layer).split(' | Duration:')[0]
    yield e.ctrl.z()
    t.assertTrue(hits_floor())
    t.assertEqual(window.scene.statistics(window.view_layer).split(' | Duration:')[0], before)
    yield e.ctrl.shift.z()
    t.assertEqual(hits_floor(), depth < 3)
    t.assertEqual(window.scene.statistics(window.view_layer).split(' | Duration:')[0], after)


def inward_pocket():
    yield from through_hole(depth=1)


def through_hole_at_wall():
    yield from through_hole(depth=3)


def through_hole_cancel():
    yield from through_hole(cancel=True)
