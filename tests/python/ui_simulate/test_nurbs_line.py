# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""NURBS line extrusion through the native viewport keymap and modal tools."""

import modules.ui_test_utils as ui


def _spline(window):
    ob = window.view_layer.objects.active
    ob.update_from_editmode()
    return ob.data.splines[0]


def _coordinates(window):
    return [tuple(round(v, 5) for v in p.co) for p in _spline(window).points]


def line_to_sheet():
    import bpy

    e, t, window = ui.test_window()
    e.cursor_position_set(*ui.get_area_center_from_spacetype(window, 'VIEW_3D'), move=True)
    yield
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    t.assertEqual(window.view_layer.objects.active.type, 'SURFACE')
    yield e.tab()

    # The same E key used for mesh extrusion creates a sheet, with only its new row selected.
    yield e.e().y().text('2').ret()
    spline = _spline(window)
    t.assertEqual((spline.point_count_u, spline.point_count_v), (2, 2))
    t.assertEqual(sum(p.select for p in spline.points), 2)
    t.assertTrue(all(p.co.y == 2.0 for p in spline.points if p.select))
    sheet_coordinates = _coordinates(window)

    yield e.ctrl.z()
    t.assertEqual(_spline(window).point_count_v, 1)
    yield e.ctrl.shift.z()
    t.assertEqual(_coordinates(window), sheet_coordinates)

    # G/R/S manipulate only the selected outgoing edge and keep the source row fixed.
    original_row = [tuple(p.co) for p in _spline(window).points if not p.select]
    yield e.g().z().text('1').ret()
    t.assertTrue(all(p.co.z == 1.0 for p in _spline(window).points if p.select))
    yield e.r().z().text('20').ret()
    yield e.s().text('1.5').ret()
    t.assertEqual([tuple(p.co) for p in _spline(window).points if not p.select], original_row)

    coordinates = _coordinates(window)
    yield e.g().z().text('4').esc()
    t.assertEqual(_coordinates(window), coordinates)

    yield e.e().y().text('1').ret()
    ob = window.view_layer.objects.active
    ob.update_from_editmode()
    t.assertEqual(len(ob.data.splines), 2)
    t.assertTrue(all((s.point_count_u, s.point_count_v) == (2, 2) for s in ob.data.splines))
    t.assertEqual(sum(p.select for s in ob.data.splines for p in s.points), 2)
    yield e.tab()
    ob = window.view_layer.objects.active
    window.view_layer.update()
    evaluated = ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        t.assertGreater(len(mesh.polygons), 0)
        t.assertGreater(sum(p.area for p in mesh.polygons), 0.0)
    finally:
        evaluated.to_mesh_clear()


def face_region_selection_gestures():
    """Identical visible source corners selected by box, click and linked, then modal E."""
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield e.a().x().ret()

    for face_count in (1, 2):
        expected_result = None
        for gesture in ('BOX', 'CLICK', 'LINKED'):
            yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
            yield e.tab()
            yield e.e().y().text('2').ret()
            if face_count == 2:
                yield e.e().y().text('2').ret()
            yield e.numpad_7().home()
            yield e.alt.a()

            ob = window.view_layer.objects.active
            ob.update_from_editmode()
            positions = sorted({tuple(p.co.xyz) for s in ob.data.splines for p in s.points})
            screen_positions = []
            for position in positions:
                co = location_3d_to_region_2d(
                    region, area.spaces.active.region_3d, ob.matrix_world @ Vector(position))
                t.assertIsNotNone(co)
                screen_positions.append((round(co.x) + region.x, round(co.y) + region.y))

            if gesture == 'BOX':
                lower = (min(p[0] for p in screen_positions) - 12,
                         min(p[1] for p in screen_positions) - 12)
                upper = (max(p[0] for p in screen_positions) + 12,
                         max(p[1] for p in screen_positions) + 12)
                yield e.b()
                yield from e.leftmouse.cursor_motion((lower, upper))
            elif gesture == 'CLICK':
                for position in screen_positions:
                    yield e.cursor_position_set(*position, move=True)
                    yield e.shift.leftmouse()
            else:
                yield e.cursor_position_set(*screen_positions[0], move=True)
                yield e.l()

            ob.update_from_editmode()
            t.assertEqual(sum(p.select for s in ob.data.splines for p in s.points), 4 * face_count)
            yield e.e().z().text('3').ret()
            ob.update_from_editmode()
            t.assertEqual(len(ob.data.splines), 6 if face_count == 1 else 10)
            t.assertTrue(all(abs(p.co.z - 3.0) < 1e-5
                             for s in ob.data.splines for p in s.points if p.select))
            result = sorted(tuple(sorted(tuple(round(v, 5) for v in p.co) for p in s.points))
                            for s in ob.data.splines)
            if expected_result is None:
                expected_result = result
            else:
                t.assertEqual(result, expected_result)
            yield e.tab()
            yield e.a().x().ret()


def duplicate_and_repeat_face_extrusion():
    """Actual Shift+D/E macros preserve independent copies and replace attached caps."""
    from pathlib import Path

    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.e().y().text('2').ret()
    yield e.a()

    def splines():
        obj = window.view_layer.objects.active
        obj.update_from_editmode()
        return obj.data.splines

    def coordinates():
        return sorted(tuple(sorted(tuple(point.co) for point in spline.points))
                      for spline in splines())

    original = coordinates()[0]
    yield e.shift.d().z().text('2').ret()
    t.assertEqual(len(splines()), 2)
    t.assertEqual(tuple(sorted(tuple(point.co) for point in splines()[0].points)), original)
    t.assertTrue(all(point.co.z == 2.0 for point in splines()[1].points))
    yield e.ctrl.z()
    t.assertEqual(len(splines()), 1)
    yield e.ctrl.shift.z()
    t.assertEqual(len(splines()), 2)

    yield e.e().z().text('1').ret()
    t.assertEqual(len(splines()), 7)
    yield e.e().z().text('1').ret()
    t.assertEqual(len(splines()), 11)  # Separate original sheet plus ten extrusion faces.
    t.assertFalse(any(all(point.co.z == 3.0 for point in spline.points) for spline in splines()))
    t.assertEqual(tuple(sorted(tuple(point.co) for point in splines()[0].points)), original)
    t.assertEqual(sum(point.select for spline in splines() for point in spline.points), 12)
    before_cancel = coordinates()
    # Blender's E commits topology first; Escape cancels its move. Undo removes that extrusion.
    yield e.e().z().text('3').esc()
    t.assertEqual(len(splines()), 15)
    yield e.ctrl.z()
    t.assertEqual(coordinates(), before_cancel)
    yield e.ctrl.shift.z()
    t.assertEqual(len(splines()), 15)
    yield e.ctrl.z()
    t.assertEqual(coordinates(), before_cancel)
    yield e.tab()
    yield e.numpad_1().numpad_6().numpad_8().home()
    yield
    output = Path('/tmp/blender-nurbs-duplicate-region-ui')
    output.mkdir(exist_ok=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'try-duplicate-extrude.blend'), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / 'duplicate-extrude.png'))
    print('NURBS_DUPLICATE_REGION_UI_ARTIFACTS', output)


def extrude_delete_top_and_continue():
    """Delete the second extrusion's top vertices, then extrude the surviving cube's rim."""
    from pathlib import Path

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.e().y().text('2').ret()
    yield e.a()
    yield e.e().z().text('2').ret()
    yield e.e().z().text('1').ret()

    def splines():
        obj = window.view_layer.objects.active
        obj.update_from_editmode()
        return obj.data.splines

    t.assertEqual(len(splines()), 10)
    yield e.x().ret()
    t.assertEqual(len(splines()), 5)
    t.assertTrue(all((s.point_count_u, s.point_count_v) == (2, 2) for s in splines()))
    t.assertEqual(max(point.co.z for s in splines() for point in s.points), 2.0)
    yield e.ctrl.z()
    t.assertEqual(len(splines()), 10)
    yield e.ctrl.shift.z()
    t.assertEqual(len(splines()), 5)
    yield e.numpad_1().home()
    area.spaces.active.shading.show_xray = True
    yield
    obj = window.view_layer.objects.active
    rim = []
    for position in ((-1, 0, 2), (1, 0, 2)):
        co = location_3d_to_region_2d(region, area.spaces.active.region_3d,
                                     obj.matrix_world @ Vector(position))
        t.assertIsNotNone(co)
        rim.append((round(co.x) + region.x, round(co.y) + region.y))
    lower = min(p[0] for p in rim) - 12, min(p[1] for p in rim) - 12
    upper = max(p[0] for p in rim) + 12, max(p[1] for p in rim) + 12
    yield e.b()
    yield from e.leftmouse.cursor_motion((lower, upper))
    t.assertEqual(sum(point.select for s in splines() for point in s.points), 8)
    original = [tuple(tuple(point.co) for point in s.points) for s in splines()]
    yield e.e().z().text('1').ret()
    t.assertEqual(len(splines()), 9)
    t.assertEqual([tuple(tuple(point.co) for point in s.points) for s in splines()[:5]], original)
    t.assertTrue(all((s.point_count_u, s.point_count_v) == (2, 2) for s in splines()))
    t.assertEqual(max(point.co.z for s in splines() for point in s.points), 3.0)
    yield e.tab()
    yield e.numpad_6().numpad_8().home()
    yield
    output = Path('/tmp/blender-nurbs-delete-ui')
    output.mkdir(exist_ok=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'try-delete-and-extrude.blend'), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / 'delete-and-extrude.png'))
    print('NURBS_DELETE_UI_ARTIFACTS', output)

    # Deleting opposite corners must retain the other two source vertices, just as mesh deletion.
    # Exercise their drawing, G, extrusion and undo through actual events as well.
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.e().y().text('2').ret()
    yield e.numpad_7().home()
    yield e.alt.a()
    obj = window.view_layer.objects.active
    for index, position in enumerate(((-1, 0, 0), (1, 2, 0))):
        co = location_3d_to_region_2d(region, area.spaces.active.region_3d,
                                     obj.matrix_world @ Vector(position))
        t.assertIsNotNone(co)
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        if index == 0:
            yield e.leftmouse()
        else:
            yield e.shift.leftmouse()
    yield e.x().ret()
    t.assertEqual(len(splines()), 2)
    t.assertTrue(all(len(spline.points) == 1 for spline in splines()))
    yield e.a()
    yield e.g().x().text('1').ret()
    t.assertEqual({tuple(spline.points[0].co.xyz) for spline in splines()}, {(2, 0, 0), (0, 2, 0)})
    yield e.e().z().text('1').ret()
    t.assertTrue(all(len(spline.points) == 2 for spline in splines()))
    yield e.ctrl.z()
    t.assertTrue(all(len(spline.points) == 1 for spline in splines()))
    yield e.ctrl.shift.z()
    t.assertTrue(all(len(spline.points) == 2 for spline in splines()))


def fill_two_lines_duplicate_and_extrude():
    """Normal Add workflow must use shared Fill rather than mutate a tagged control net."""
    from pathlib import Path
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.numpad_7()
    area.spaces.active.region_3d.view_location = (0, 1, 0)
    area.spaces.active.region_3d.view_distance = 6
    yield e.shift.d().y().text('2').ret()
    yield e.a()
    yield e.f()

    def assert_face():
        obj = window.view_layer.objects.active
        obj.update_from_editmode()
        t.assertEqual(len(obj.data.splines), 1)
        spline = obj.data.splines[0]
        t.assertEqual((spline.point_count_u, spline.point_count_v, spline.order_u, spline.order_v),
                      (2, 2, 2, 2))
        t.assertFalse(spline.use_cyclic_u or spline.use_cyclic_v)
        t.assertTrue(spline.use_endpoint_u and spline.use_endpoint_v)
        return spline

    assert_face()
    yield e.ctrl.z()
    obj = window.view_layer.objects.active
    obj.update_from_editmode()
    t.assertEqual(len(obj.data.splines), 2)
    t.assertTrue(all((s.point_count_u, s.point_count_v) == (2, 1) for s in obj.data.splines))
    yield e.ctrl.shift.z()
    assert_face()
    output = Path('/tmp/blender-nurbs-two-edge-fill-ui')
    output.mkdir(parents=True, exist_ok=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'sharp-fill.blend'), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / 'sharp-fill.png'))
    yield e.shift.d().z().text('2').ret()
    yield e.e().z().text('1').ret()
    obj = window.view_layer.objects.active
    obj.update_from_editmode()
    t.assertEqual(len(obj.data.splines), 7)
    t.assertTrue(all((s.point_count_u, s.point_count_v, s.order_u, s.order_v) == (2, 2, 2, 2)
                    for s in obj.data.splines))
    # Legacy storage repeats shared corners in neighboring patches; verify the four
    # selected cap positions rather than counting their storage aliases as vertices.
    selected = {tuple(p.co) for s in obj.data.splines for p in s.points if p.select}
    t.assertEqual(len(selected), 4)
    t.assertTrue(all(co[2] == 3 for co in selected))
    yield e.ctrl.z()
    obj = window.view_layer.objects.active
    obj.update_from_editmode()
    t.assertEqual(len(obj.data.splines), 2)
    yield e.ctrl.shift.z()
    obj = window.view_layer.objects.active
    obj.update_from_editmode()
    t.assertEqual(len(obj.data.splines), 7)
    yield e.tab()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'fill-duplicate-extrude.blend'), check_existing=False)
    window.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        t.assertGreater(len(mesh.polygons), 0)
    finally:
        evaluated.to_mesh_clear()



def _cube_vertex_extensions_fill(native):
    """Both owners use the same constructor when a cube edge gains a third face."""
    import os
    import re
    from pathlib import Path
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    if native:
        with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
            data.curves = ['Native preview sheet']
        obj = bpy.data.objects.new('Native cube Fill', data.curves[0])
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        window.view_layer.update()
        yield e.tab()
        base_y = -1
    else:
        yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        yield e.tab()
        yield e.e().y().text('2').ret()
        base_y = 0
    yield e.a()
    yield e.e().z().text('1').ret()
    if native:
        yield e.one()
    space.shading.show_xray = True
    space.overlay.show_stats = True
    yield e.numpad_1()
    yield e.numpad_6()
    yield e.numpad_8()
    space.region_3d.view_location = (0, base_y + 1, 1)
    space.region_3d.view_distance = 8
    yield

    def counts(verts, edges, faces, wires=0):
        obj = window.view_layer.objects.active
        if native:
            t.assertTrue(obj.data.is_native_nurbs)
            t.assertEqual(len(obj.data.splines), 0)
            stats = window.scene.statistics(window.view_layer)
            match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
            t.assertIsNotNone(match, stats)
            result = tuple(map(int, match.groups()))
            t.assertEqual((result[1], result[3], result[5]), (verts, edges, faces))
        else:
            obj.update_from_editmode()
            t.assertEqual(sum(s.point_count_v == 2 for s in obj.data.splines), faces)
            t.assertEqual(sum(s.point_count_v == 1 for s in obj.data.splines), wires)
            positions = {tuple(p.co) for s in obj.data.splines for p in s.points}
            t.assertEqual(len(positions), verts)

    def screen(position):
        obj = window.view_layer.objects.active
        co = location_3d_to_region_2d(region, space.region_3d, obj.matrix_world @ Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    counts(8, 12, 6)
    for i, x in enumerate((-1, 1)):
        yield e.alt.a()
        yield e.cursor_position_set(*screen((x, base_y, 1)), move=True)
        yield e.leftmouse()
        yield e.e().z().text('1').ret()
        counts(9 + i, 13 + i, 6, 1 + i)
    yield e.cursor_position_set(*screen((-1, base_y, 2)), move=True)
    yield e.shift.leftmouse()
    yield e.f()
    counts(10, 15, 6, 3)
    yield e.alt.a()
    for point in ((-1, base_y, 1), (1, base_y, 1), (-1, base_y, 2), (1, base_y, 2)):
        yield e.cursor_position_set(*screen(point), move=True)
        yield e.shift.leftmouse()
    yield e.f()
    counts(10, 15, 7)
    output = Path('/tmp/blender-nurbs-nonmanifold-fill-ui')
    output.mkdir(parents=True, exist_ok=True)
    label = 'native' if native else 'tagged'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / (label + '-filled.blend')), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / (label + '-filled.png')))
    yield e.ctrl.z()
    counts(10, 15, 6, 3)
    yield e.ctrl.shift.z()
    counts(10, 15, 7)
    if native:
        yield e.three()
    yield e.e().y().text('-1').ret()
    counts(14, 23, 11)
    yield e.ctrl.z()
    counts(10, 15, 7)
    yield e.ctrl.shift.z()
    counts(14, 23, 11)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / (label + '-continued.blend')), check_existing=False)
    yield e.tab()


def fill_from_cube_vertex_extensions():
    yield from _cube_vertex_extensions_fill(False)


def native_fill_from_cube_vertex_extensions():
    yield from _cube_vertex_extensions_fill(True)
