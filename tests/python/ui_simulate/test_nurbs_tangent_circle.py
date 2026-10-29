# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tangent circle placement, fixed three-curve preview, Tab and native undo."""
import modules.ui_test_utils as ui


def tangent_circles():
    import bpy
    import hashlib
    import re
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active

    def screen(point):
        p = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(p.x) + region.x, round(p.y) + region.y

    def counts():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        return tuple(map(int, re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+)', stats).groups()))

    def preview():
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath='/tmp/nurbs-tangent-circle-preview.png')
        image = bpy.data.images.load('/tmp/nurbs-tangent-circle-preview.png', check_existing=False)
        try:
            pixels = image.pixels[:]
            digest = hashlib.sha256()
            for y in range(region.y + 12, region.y + region.height - 80):
                row = pixels[(y * image.size[0] + region.x + 12) * 4:
                             (y * image.size[0] + region.x + region.width - 12) * 4]
                digest.update(bytes(round(v * 255) for v in row))
            return digest.digest()
        finally:
            bpy.data.images.remove(image)

    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 14
    window.scene.tool_settings.use_snap = False
    cases = (
        ('TAN_TAN', False, ('-4 -2 0 4 -2 0', '-4 2 0 4 2 0'), (0, 0.5, 0)),
        ('TAN_TAN_TAN', False, ('-4 -2 0 4 -2 0', '-4 -1.9 0 -0.1 4 0',
                               '4 -1.9 0 0.1 4 0'), (0, 0, 0)),
        ('TAN_TAN_TAN', True, ('-3 0 0 -2 0 0', '3 0 0 4 0 0', '0 5 0 1 5 0'), (0, 1.6, 0)),
    )
    for mode, curved, sources, seed in cases:
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.curve.select_all(action='SELECT')
            bpy.ops.curve.delete(type='VERT')
            for points in sources:
                (bpy.ops.curve.draw_circle if curved else bpy.ops.curve.draw_line)(points=points)
            bpy.ops.curve.select_all(action='SELECT')
            bpy.ops.ed.undo_push(message='Tangent circle source curves')
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_circle')
            props = window.workspace.tools.from_space_view3d_mode(bpy.context.mode).operator_properties('curve.draw_circle')
            props.mode = mode
            props.preview_points = 32
            props.anchor_tangent_markers = False
        yield e.cursor_position_set(*screen(seed), move=True)
        before = counts()
        yield e.leftmouse()
        first = preview()
        yield e.cursor_position_set(*screen((seed[0] + 1, seed[1], 0)), move=True)
        if mode == 'TAN_TAN_TAN':
            t.assertEqual(preview(), first, 'Three-curve solution moved with pointer')
        else:
            t.assertNotEqual(preview(), first, 'Two-curve solution did not follow pointer')
        t.assertEqual(counts(), before)
        extra = 32
        if curved:
            yield e.tab().t().wheelupmouse()
            extra = 36
        yield e.leftmouse()
        t.assertEqual(counts(), tuple(n + extra for n in before), (mode, curved))
        yield e.ctrl.z()
        t.assertEqual(counts(), before)
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), tuple(n + extra for n in before))
    print('NURBS_TANGENT_CIRCLES_PASS', flush=True)
