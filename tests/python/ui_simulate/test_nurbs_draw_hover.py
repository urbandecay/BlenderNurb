# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Visible snap targets before the first click for every NURBS drawing mode."""
import modules.ui_test_utils as ui


def drawing_snap_hover():
    import bpy
    import re
    from pathlib import Path
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/nurbs-draw-hover')
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

    def glyph_pixels(name, point, yellow):
        path = str(output / (name + '.png'))
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=path)
        image = bpy.data.images.load(path, check_existing=False)
        try:
            width, height = image.size
            pixels = image.pixels[:]
            cx, cy = screen(point)
            found = 0
            for y in range(max(0, cy - 17), min(height, cy + 18)):
                for x in range(max(0, cx - 17), min(width, cx + 18)):
                    if abs(y - cy) <= 1 or (x - cx) ** 2 + (y - cy) ** 2 < 12:
                        continue  # Exclude the existing source endpoint itself.
                    r, g, b = pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]
                    if (r > 0.8 and g > 0.5 and b < 0.3) if yellow else min(r, g, b) > 0.8:
                        found += 1
            return found
        finally:
            bpy.data.images.remove(image)

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
    window.scene.cursor.location = (-5, -5, 0)
    mesh = bpy.data.meshes.new('Hover face target')
    mesh.from_pydata(((2, 2, 0), (4, 2, 0), (4, 4, 0), (2, 4, 0)), (), ((0, 1, 2, 3),))
    face = bpy.data.objects.new('Hover face target', mesh)
    window.scene.collection.objects.link(face)
    settings = window.scene.tool_settings
    settings.use_snap_self = True
    settings.use_snap_edit = True
    settings.use_snap_nonedit = True
    cases = (('line', ''), ('spline', 'FIT_POINTS'), ('spline', 'CONTROL_VERTICES'),
             ('arc', 'CENTER'), ('arc', 'ENDPOINTS'), ('arc', 'THREE_POINTS'),
             ('circle', 'CENTER'), ('circle', 'DIAMETER'), ('circle', 'THREE_POINTS'),
             ('circle', 'TAN_TAN'), ('circle', 'TAN_TAN_TAN'))
    targets = (('VERTEX', (1, 0, 0)), ('EDGE', (0.3, 0, 0)),
               ('EDGE_MIDPOINT', (0, 0, 0)), ('FACE', (3, 3, 0)),
               ('FACE_MIDPOINT', (3, 3, 0)), ('GRID', (0, 2, 0)))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    baseline = counts()
    for tool, mode in cases:
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_' + tool)
            active = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
            props = active.operator_properties('curve.draw_' + tool)
            props.use_axis_snap = False
            if tool == 'spline':
                props.method = mode
            elif tool in {'arc', 'circle'}:
                props.mode = mode
        checks = targets if mode in ('', 'FIT_POINTS', 'CENTER') else targets[:1]
        for snap, point in checks:
            settings.use_snap = True
            settings.snap_elements = {snap}
            x, y = screen(point)
            yield e.cursor_position_set(x + 4, y + 3, move=True)
            name = tool + '-' + mode.lower() + '-' + snap.lower()
            t.assertGreater(glyph_pixels(name, point, tool in {'arc', 'circle'}),
                            1 if snap == 'GRID' else 5, name)
            t.assertEqual(counts(), baseline, name)
        settings.use_snap = False
        settings.snap_elements = {'VERTEX'}
        x, y = screen((1, 0, 0))
        yield e.cursor_position_set(x + 5, y + 3, move=True)
        name = tool + '-' + mode.lower() + '-disabled'
        t.assertEqual(glyph_pixels(name, (1, 0, 0), tool in {'arc', 'circle'}), 0, name)
        if mode in ('', 'FIT_POINTS', 'CENTER'):
            e.ctrl.press()
            yield
            yield e.cursor_position_set(x + 6, y + 3, move=True)
            t.assertGreater(glyph_pixels(tool + '-ctrl-enabled', (1, 0, 0),
                                         tool in {'arc', 'circle'}), 5)
            e.ctrl.release()
            yield
            yield e.cursor_position_set(x + 5, y + 3, move=True)
            t.assertEqual(glyph_pixels(tool + '-ctrl-released', (1, 0, 0),
                                       tool in {'arc', 'circle'}), 0)
    # The first placed vertex must use the target shown during hover.
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_line')
    settings.use_snap = True
    settings.snap_elements = {'VERTEX'}
    x, y = screen((1, 0, 0))
    yield e.cursor_position_set(x + 5, y + 3, move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, -1, 0)), move=True)
    yield e.leftmouse().ret()
    t.assertEqual(counts(), (baseline[0] + 1, baseline[1] + 1, 0))
