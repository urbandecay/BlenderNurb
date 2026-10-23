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
