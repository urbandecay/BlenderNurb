# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Arc workspace tool: construction stages, numeric input, snapping, undo and cancellation."""

import modules.ui_test_utils as ui


def arc_workflow():
    import bpy
    import re
    from bpy_extras.view3d_utils import (
        location_3d_to_region_2d, region_2d_to_origin_3d, region_2d_to_vector_3d)
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active

    def counts():
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def screen(point):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def mesh_points():
        obj = window.view_layer.objects.active
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            return {tuple(round(value, 5) for value in v.co) for v in mesh.vertices}
        finally:
            evaluated.to_mesh_clear()

    def activate(mode):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_arc')
            tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
            props = tool.operator_properties('curve.draw_arc')
            props.mode = mode
            props.use_axis_snap = False
            props.use_angle_snap = True
            props.preview_points = 32

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

    # All modes commit the displayed divisions as exact curved edges and shared endpoints.
    for mode, points in (
            ('CENTER', ((0, 0, 0), (2, 0, 0), (0, 2, 0))),
            ('ENDPOINTS', ((-2, -2, 0), (2, -2, 0), (0, -4, 0))),
            ('THREE_POINTS', ((-2, 2, 0), (2, 2, 0), (0, 4, 0)))):
        before = counts()
        activate(mode)
        for i, point in enumerate(points):
            yield e.cursor_position_set(*screen(point), move=True)
            if i == 2:
                yield e.wheelupmouse()
            yield e.leftmouse()
        t.assertEqual(counts(), (before[0] + 34, before[1] + 33, 0))
        t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
        with bpy.context.temp_override(window=window, area=area, region=region):
            tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
            t.assertEqual(tool.idname, 'builtin.select_box')
        finished = counts()
        yield e.ctrl.z()
        t.assertEqual(counts(), before)
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), finished)

    # Cancellation discards both stages without changing native source or selection.
    for cancel, stage in (('esc', 1), ('rightmouse', 2)):
        before = counts()
        activate('ENDPOINTS')
        yield e.cursor_position_set(*screen((-3, 0, 0)), move=True)
        yield e.leftmouse()
        if stage == 2:
            yield e.cursor_position_set(*screen((3, 0, 0)), move=True)
            yield e.leftmouse()
        yield getattr(e, cancel)()
        t.assertEqual(counts(), before)

    # A numeric radius and a numeric signed sweep use distinct length/angle units.
    before_points = mesh_points()
    before = counts()
    activate('CENTER')
    cx, cy = screen((0, 0, 0))
    co = (cx - region.x, cy - region.y)
    origin = region_2d_to_origin_3d(region, space.region_3d, co)
    direction = region_2d_to_vector_3d(region, space.region_3d, co)
    numeric_center = origin - direction * (origin.z / direction.z)
    yield e.cursor_position_set(cx, cy, move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, 0, 0)), move=True)
    yield e.text('3').ret()
    yield e.cursor_position_set(*screen((0, 2, 0)), move=True)
    yield e.text('-90').ret()
    t.assertEqual(counts(), (before[0] + 33, before[1] + 32, 0))
    added = mesh_points() - before_points
    t.assertGreater(len(added), 4)
    for x, y, z in added:
        t.assertAlmostEqual((Vector((x, y, z)) - numeric_center).length, 3, delta=1e-4)
        t.assertGreaterEqual(x, numeric_center.x - 0.03)
        t.assertLessEqual(y, numeric_center.y + 0.03)

    # Unwrap the pointer across 180 degrees to retain the intended major sweep.
    before = counts()
    before_points = mesh_points()
    activate('CENTER')
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2.5, 0, 0)), move=True)
    yield e.leftmouse()
    for point in ((0, 2.5, 0), (-2.5, 0, 0), (0, -2.5, 0)):
        yield e.cursor_position_set(*screen(point), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (before[0] + 33, before[1] + 32, 0))
    added = mesh_points() - before_points
    t.assertGreater(max(p[1] for p in added), 2)
    t.assertLess(min(p[1] for p in added), -2)
    t.assertLess(min(p[0] for p in added), -2)

    # A full sweep closes the last segment onto the first endpoint.
    before = counts()
    activate('CENTER')
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.text('360').ret()
    t.assertEqual(counts(), (before[0] + 32, before[1] + 32, 0))

    # Backspace returns to chord placement; a degenerate through point cannot commit.
    before = counts()
    activate('THREE_POINTS')
    yield e.cursor_position_set(*screen((-3, -1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((3, -1, 0)), move=True)
    yield e.leftmouse().back_space()
    yield e.cursor_position_set(*screen((3, -1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((0, -1, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), before)
    yield e.cursor_position_set(*screen((0, -3, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (before[0] + 33, before[1] + 32, 0))

    # Reuse native endpoint identities rather than duplicating coincident arc endpoints.
    before = counts()
    window.scene.tool_settings.use_snap = True
    window.scene.tool_settings.snap_elements = {'VERTEX'}
    activate('CENTER')
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((0, -1, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (before[0] + 32, before[1] + 32, 0))
    window.scene.tool_settings.use_snap = False

    # Vertical chords choose a stable plane. P alternates its two orientations.
    from mathutils import Quaternion
    for mode, toggles, peak in (
            ('ENDPOINTS', 1, (4, 1, 0)),
            ('THREE_POINTS', 2, (5, 0, 0))):
        yield e.numpad_1()
        before = counts()
        before_points = mesh_points()
        activate(mode)
        yield e.cursor_position_set(*screen((4, 0, -1)), move=True)
        yield e.leftmouse()
        yield e.cursor_position_set(*screen((4, 0, 1)), move=True)
        yield e.leftmouse()
        for _ in range(toggles):
            yield e.p()
        if toggles == 1:
            space.region_3d.view_rotation = Quaternion((0.5, 0.5, 0.5, 0.5))
            yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.cursor_position_set(*screen(peak), move=True)
        yield e.leftmouse()
        t.assertEqual(counts(), (before[0] + 33, before[1] + 32, 0))
        added = mesh_points() - before_points
        t.assertGreater(len(added), 4)
        for x, y, z in added:
            t.assertAlmostEqual((x - 4) ** 2 + y * y + z * z, 1, delta=0.05)
            t.assertAlmostEqual(x if toggles == 1 else y, 4 if toggles == 1 else 0, delta=0.03)


def arc_overlays_and_three_point():
    """Check rendered compass colors and a non-midpoint third click's actual geometry."""
    import bpy
    import math
    from pathlib import Path
    import re
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Quaternion, Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/nurbs-arc-overlays')
    output.mkdir(exist_ok=True)

    def screen(point):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def totals():
        text = window.scene.statistics(window.view_layer).replace(',', '')
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', text)
        t.assertIsNotNone(match, text)
        return tuple(map(int, match.groups()))

    def screenshot(name, center, channel=None, *, max_dominant=None):
        path = str(output / (name + '.png'))
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=path)
        if channel is None:
            return
        image = bpy.data.images.load(path, check_existing=False)
        try:
            pixels = image.pixels[:]
            width, height = image.size
            cx, cy = screen(center)
            dominant = 0
            for y in range(max(0, cy - 68), min(height, cy + 69)):
                for x in range(max(0, cx - 68), min(width, cx + 69)):
                    rgb = pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]
                    if rgb[channel] > 0.1 and all(
                            rgb[channel] > rgb[c] * 1.4 for c in range(3) if c != channel):
                        dominant += 1
            if max_dominant is None:
                t.assertGreater(dominant, 60, (name, dominant))
            else:
                t.assertLessEqual(dominant, max_dominant, (name, dominant))
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
    window.scene.tool_settings.use_snap = False
    space.overlay.show_floor = False
    space.overlay.show_axis_x = False
    space.overlay.show_axis_y = False
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_arc')
        props = window.workspace.tools.from_space_view3d_mode(bpy.context.mode).operator_properties('curve.draw_arc')
        props.mode = 'CENTER'
        props.use_axis_snap = False
        props.show_compass = True
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, 1, 0)), move=True)
    screenshot('center-blue', (0, 0, 0), 2)
    # Reorient the same live compass onto each principal plane.
    yield e.x()
    space.region_3d.view_rotation = Quaternion((0.5, 0.5, 0.5, 0.5))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.cursor_position_set(*screen((0, 2, 1)), move=True)
    screenshot('center-red', (0, 0, 0), 0)
    yield e.y()
    space.region_3d.view_rotation = Quaternion((math.sqrt(0.5), math.sqrt(0.5), 0, 0))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.cursor_position_set(*screen((2, 0, 1)), move=True)
    screenshot('center-green', (0, 0, 0), 1)
    yield e.esc().numpad_7()

    # Even when the Center compass preference is enabled, the 2 Point tool must
    # never draw a compass, either while placing its chord or while placing its height.
    before = totals()
    with bpy.context.temp_override(window=window, area=area, region=region):
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        props = tool.operator_properties('curve.draw_arc')
        props.mode = 'ENDPOINTS'
        props.show_compass = True
    yield e.cursor_position_set(*screen((-2, -1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, -1, 0)), move=True)
    screenshot('two-point-chord-no-compass', (-2, -1, 0), 2, max_dominant=0)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((0, 2, 0)), move=True)
    screenshot('two-point-height-no-compass', (0, -1, 0), 2, max_dominant=0)
    yield e.esc()
    t.assertEqual(totals(), before)

    # Select the existing 3 Point button's RNA property. The third click is deliberately
    # away from the chord midpoint so a center arc or a sagitta projection cannot pass.
    before = totals()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_arc')
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        props = tool.operator_properties('curve.draw_arc')
        props.mode = 'THREE_POINTS'
        props.use_axis_snap = False
    yield e.cursor_position_set(*screen((-2, -1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((2, -1, 0)), move=True)
    screenshot('three-point-chord-no-compass', (-2, -1, 0), 2, max_dominant=0)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, 2, 0)), move=True)
    screenshot('three-point-preview', (0, -1, 0), 2, max_dominant=0)
    t.assertEqual(totals(), before)
    yield e.leftmouse()
    t.assertEqual(totals(), (before[0] + 33, before[1] + 32, 0))
    obj = window.view_layer.objects.active
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        arc = [v.co.copy() for v in mesh.vertices if abs(v.co.y) > 0.1]
        t.assertGreater(len(arc), 6)
        for p in arc:
            t.assertAlmostEqual(p.length, math.sqrt(5), delta=0.03)
        t.assertLess(min((p - Vector((1, 2, 0))).length for p in arc), 0.15)
        t.assertGreater(max(p.y for p in arc), 2.15)
        t.assertGreater(min(p.y for p in arc), -1.03)
    finally:
        evaluated.to_mesh_clear()
    screenshot('three-point-result', (0, 0, 0))
    # Cancelled draws and callbacks leave the next draw and undo/redo usable.
    finished = totals()
    yield e.ctrl.z()
    t.assertEqual(totals(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(totals(), finished)


def arc_preview_matches_commit():
    """Compare preview and committed arc bounds and endpoint marker footprints."""
    import bpy
    import math
    from pathlib import Path
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/nurbs-arc-preview-size')
    output.mkdir(exist_ok=True)

    def screen(point):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def capture(name, committed):
        path = str(output / (name + '.png'))
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=path)
        image = bpy.data.images.load(path, check_existing=False)
        try:
            pixels = image.pixels[:]
            width, height = image.size
            left, bottom = screen((-3.3, 0.4, 0))
            right, top = screen((3.3, 3.3, 0))
            mask = set()
            for y in range(max(0, bottom), min(height, top)):
                for x in range(max(0, left), min(width, right)):
                    r, g, b = pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]
                    if (r > 0.6 and g > 0.2 and b < 0.25) if committed else max(r, g, b) < 0.08:
                        mask.add((x, y))
            t.assertGreater(len(mask), 100)
            return mask
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
    window.scene.tool_settings.use_snap = False

    for mode in ('CENTER', 'ENDPOINTS', 'THREE_POINTS'):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_arc')
            props = window.workspace.tools.from_space_view3d_mode(bpy.context.mode).operator_properties('curve.draw_arc')
            props.mode = mode
            props.use_axis_snap = False
            props.use_angle_snap = False
            props.show_compass = False
            props.show_measurements = False
            props.preview_points = 16
            props.preview_point_size = 5
            props.start_color = (0, 0, 0, 0)
            props.end_color = (0, 0, 0, 0)
            props.overlay_color = (0, 0, 0, 0)
        points = ((0, 0, 0), (3, 0, 0)) if mode == 'CENTER' else ((-3, 0, 0), (3, 0, 0))
        for point in points:
            yield e.cursor_position_set(*screen(point), move=True)
            yield e.leftmouse()
        yield e.cursor_position_set(*screen((0, 3, 0)), move=True)
        if mode == 'CENTER':
            yield e.cursor_position_set(*screen((-3, 0, 0)), move=True)
            yield e.text('180')
        preview = capture(mode.lower() + '-preview', False)
        yield e.leftmouse()
        committed = capture(mode.lower() + '-committed', True)
        for axis in (0, 1):
            t.assertLessEqual(abs(min(p[axis] for p in preview) - min(p[axis] for p in committed)), 2)
            t.assertLessEqual(abs(max(p[axis] for p in preview) - max(p[axis] for p in committed)), 2)
        # At the apex the wire is horizontal, so vertical width measures the dot.
        cx, cy = screen((0, 3, 0))
        def marker_height(mask):
            ys = [y for x, y in mask if abs(x - cx) <= 1 and abs(y - cy) <= 5]
            t.assertTrue(ys)
            return max(ys) - min(ys) + 1
        t.assertLessEqual(abs(marker_height(preview) - marker_height(committed)), 1)
        yield e.ctrl.z()
