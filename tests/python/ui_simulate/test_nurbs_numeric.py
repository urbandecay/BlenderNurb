# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Numeric drawing parameters must stay pending until Enter accepts them."""

import modules.ui_test_utils as ui


def deferred_parameters():
    import bpy
    import hashlib
    import re
    from pathlib import Path
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/nurbs-deferred-numeric')
    output.mkdir(exist_ok=True)

    def screen(point):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(co.x) + region.x, round(co.y) + region.y

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

    def image(name):
        path = str(output / (name + '.png'))
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=path)
        img = bpy.data.images.load(path, check_existing=False)
        try:
            pixels = img.pixels[:]
            width = img.size[0]
            digest = hashlib.sha256()
            # The viewport only: the pending text is shown in the header/footer.
            for y in range(region.y + 12, region.y + region.height - 80):
                row = pixels[(y * width + region.x + 12) * 4:
                             (y * width + region.x + region.width - 12) * 4]
                digest.update(bytes(round(v * 255) for v in row))
            return digest.digest()
        finally:
            bpy.data.images.remove(img)

    if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
        yield e.tab()
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 12
    space.overlay.show_floor = False
    space.overlay.show_axis_x = False
    space.overlay.show_axis_y = False
    window.scene.tool_settings.use_snap = False

    for kind in ('arc', 'circle', 'line', 'spline'):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_' + kind)
            props = window.workspace.tools.from_space_view3d_mode(bpy.context.mode).operator_properties(
                'curve.draw_' + kind)
            props.use_axis_snap = False
            if kind in {'arc', 'circle'}:
                props.mode = 'CENTER'
                props.show_measurements = False
                props.show_compass = False
                props.use_angle_snap = False
            if kind != 'line':
                props.preview_points = 8
        yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
        yield e.leftmouse()
        yield e.cursor_position_set(*screen((1, 0, 0)), move=True)
        if kind == 'arc':
            yield e.leftmouse()
            yield e.cursor_position_set(*screen((0, 1, 0)), move=True)
        before_counts = totals()
        before_points = mesh_points()
        yield e.r()
        baseline = image(kind + '-before')
        yield e.text('3')
        t.assertEqual(image(kind + '-pending'), baseline, kind)
        t.assertEqual(totals(), before_counts)
        # Clicking cannot apply an unfinished numeric entry.
        yield e.leftmouse()
        t.assertEqual(image(kind + '-pending-click'), baseline, kind)
        yield e.ret()
        t.assertNotEqual(image(kind + '-accepted'), baseline, kind)
        if kind == 'arc':
            yield e.a()
            before_extra = image(kind + '-extra-before')
            yield e.text('150')
            t.assertEqual(image(kind + '-extra-pending'), before_extra, kind)
            yield e.ret()
            t.assertNotEqual(image(kind + '-extra-accepted'), before_extra, kind)
        if kind != 'line':
            yield e.s()
            before_segments = image(kind + '-segments-before')
            yield e.text('12')
            t.assertEqual(image(kind + '-segments-pending'), before_segments, kind)
            yield e.esc()
            t.assertEqual(image(kind + '-segments-cancelled'), before_segments, kind)
            yield e.s().text('12').ret()
        yield e.ret()
        delta = tuple(a - b for a, b in zip(totals(), before_counts))
        t.assertEqual(delta, {'arc': (13, 12), 'circle': (12, 12),
                              'line': (2, 1), 'spline': (13, 12)}[kind], kind)
        if kind in {'arc', 'circle'}:
            added = mesh_points() - before_points
            t.assertGreater(len(added), 4)
            for point in added:
                t.assertAlmostEqual(Vector(point).length, 3, delta=0.05)
    print('NURBS_DEFERRED_PARAMETERS_PASS', flush=True)
