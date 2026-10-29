# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise every radCAD shape construction through the native modal tools."""

import modules.ui_test_utils as ui


def construction_modes():
    import bpy
    import math
    import hashlib
    import re
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Quaternion, Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active

    def screen(point):
        p = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(p.x) + region.x, round(p.y) + region.y

    def totals():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        return tuple(map(int, re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+)', stats).groups()))

    def mesh_points():
        obj = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = obj.to_mesh()
        try:
            return {tuple(round(v, 5) for v in point.co) for point in mesh.vertices}
        finally:
            obj.to_mesh_clear()

    def preview_hash():
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath='/tmp/nurbs-shapes-pending.png')
        image = bpy.data.images.load('/tmp/nurbs-shapes-pending.png', check_existing=False)
        try:
            pixels = image.pixels[:]
            width = image.size[0]
            digest = hashlib.sha256()
            for y in range(region.y + 12, region.y + region.height - 80):
                row = pixels[(y * width + region.x + 12) * 4:
                             (y * width + region.x + region.width - 12) * 4]
                digest.update(bytes(round(v * 255) for v in row))
            return digest.digest()
        finally:
            bpy.data.images.remove(image)

    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().numpad_7()
    space.region_3d.view_distance = 15
    window.scene.tool_settings.use_snap = False
    cases = (
        ('ellipse', 'RADIUS', ((0, 0), (2, 0), (0, 1)), 8),
        ('ellipse', 'ENDPOINTS', ((-2, 0), (2, 0), (0, 1)), 8),
        ('ellipse', 'FOCI', ((-1, 0), (1, 0), (0, 2)), 8),
        ('ellipse', 'CORNERS', ((-2, -1), (2, 1)), 8),
        ('polygon', 'CENTER_CORNER', ((0, 0), (2, 0)), 6),
        ('polygon', 'CENTER_TANGENT', ((0, 0), (2, 0)), 6),
        ('polygon', 'CORNER_CORNER', ((0, 0), (2, 0)), 6),
        ('polygon', 'EDGE', ((0, 0), (2, 0)), 7),
        ('rectangle', 'CENTER_CORNER', ((0, 0), (2, 1)), 4),
        ('rectangle', 'CORNER_CORNER', ((-2, -1), (2, 1)), 4),
        ('rectangle', 'THREE_POINTS', ((-2, -1), (2, -1), (2, 1)), 4),
    )
    for case_index, (kind, mode, clicks, count) in enumerate(cases):
        offset = (case_index + 1) * 20
        space.region_3d.view_location = (offset, 0, 0)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_' + kind)
            props = window.workspace.tools.from_space_view3d_mode(bpy.context.mode).operator_properties(
                'curve.draw_' + kind)
            props.mode = mode
            props.use_axis_snap = False
            props.keep_foci = False
            if kind != 'rectangle':
                props.preview_points = 8 if kind == 'ellipse' else 6
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        before = totals()
        old_points = mesh_points()
        for point in clicks[:-1]:
            yield e.cursor_position_set(*screen((point[0] + offset, point[1], 0)), move=True)
            yield e.leftmouse()
        # Native orbit and pan keep the same modal drawing operation active.
        start = ui.get_area_center(area)
        rotation = space.region_3d.view_rotation.copy()
        yield from e.middlemouse.cursor_motion((start, (start[0] + 40, start[1] + 25)))
        t.assertGreater(space.region_3d.view_rotation.rotation_difference(rotation).angle, 0.01)
        location = space.region_3d.view_location.copy()
        yield from e.shift.middlemouse.cursor_motion((start, (start[0] + 30, start[1] - 20)))
        t.assertGreater((space.region_3d.view_location - location).length, 0.01)
        t.assertEqual(totals(), before)
        space.region_3d.view_rotation = Quaternion((1, 0, 0, 0))
        space.region_3d.view_perspective = 'ORTHO'
        space.region_3d.view_location = (offset, 0, 0)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        if mode == 'FOCI':
            yield e.k()
        final = clicks[-1]
        yield e.cursor_position_set(*screen((final[0] + offset, final[1], 0)), move=True)
        t.assertEqual(totals(), before, (kind, mode, 'preview modified source'))
        yield e.leftmouse()
        delta = tuple(a - b for a, b in zip(totals(), before))
        t.assertEqual(delta, (count + (2 if mode == 'FOCI' else 0), count), (kind, mode))
        added = {Vector((p[0] - offset, p[1], p[2]))[:] for p in mesh_points() - old_points}
        t.assertGreaterEqual(len(added), count)
        for x, y, z in added:
            t.assertAlmostEqual(z, 0, delta=1e-4)
            if kind == 'ellipse':
                if mode == 'FOCI' and abs(abs(x) - 1) < 0.02 and abs(y) < 0.02:
                    continue
                rx, ry = (math.sqrt(5), 2) if mode == 'FOCI' else (2, 1)
                t.assertAlmostEqual((x / rx) ** 2 + (y / ry) ** 2, 1, delta=0.04)
        committed = totals()
        yield e.ctrl.z()
        t.assertEqual(totals(), before, (kind, mode, 'undo'))
        yield e.ctrl.shift.z()
        t.assertEqual(totals(), committed, (kind, mode, 'redo'))
        print('NURBS_SHAPE_MODE_PASS', kind, mode, flush=True)

    # Rectangle dimensions remain editable together and are full sizes in center mode.
    offset = 260
    space.region_3d.view_location = (offset, 0, 0)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_rectangle')
        props = window.workspace.tools.from_space_view3d_mode(bpy.context.mode).operator_properties(
            'curve.draw_rectangle')
        props.mode = 'CENTER_CORNER'
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.cursor_position_set(*screen((offset, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((offset + 1, 1, 0)), move=True)
    before = totals()
    old_points = mesh_points()
    yield e.x()
    original = preview_hash()
    yield e.text('6')
    t.assertEqual(preview_hash(), original)
    yield e.leftmouse()
    t.assertEqual(preview_hash(), original)
    yield e.ret()
    t.assertNotEqual(preview_hash(), original)
    yield e.y().text('4').ret()
    t.assertEqual(totals(), before)
    yield e.ret()
    added = mesh_points() - old_points
    t.assertEqual(tuple(a - b for a, b in zip(totals(), before)), (4, 4))
    t.assertAlmostEqual(min(p[0] for p in added) - offset, -3, delta=0.01)
    t.assertAlmostEqual(max(p[0] for p in added) - offset, 3, delta=0.01)
    t.assertAlmostEqual(min(p[1] for p in added), -2, delta=0.01)
    t.assertAlmostEqual(max(p[1] for p in added), 2, delta=0.01)
    print('NURBS_SHAPE_PARAMETERS_PASS', flush=True)

    # Odd-side Edge polygons must decrease as well as increase with the wheel.
    space.region_3d.view_location = (280, 0, 0)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_polygon')
        props = window.workspace.tools.from_space_view3d_mode(bpy.context.mode).operator_properties('curve.draw_polygon')
        props.mode = 'EDGE'
        props.preview_points = 7
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.cursor_position_set(*screen((280, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((282, 0, 0)), move=True)
    before = totals()
    yield e.wheeldownmouse().leftmouse()
    t.assertEqual(tuple(a - b for a, b in zip(totals(), before)), (5, 5))
    print('NURBS_SHAPE_ODD_SIDES_PASS', flush=True)
