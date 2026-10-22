# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""NURBS line extrusion through the native viewport keymap and modal tools."""

import bpy

import modules.ui_test_utils as ui


def _spline(window):
    ob = window.view_layer.objects.active
    ob.update_from_editmode()
    return ob.data.splines[0]


def _coordinates(window):
    return [tuple(round(v, 5) for v in p.co) for p in _spline(window).points]


def line_to_sheet():
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
    t.assertEqual(_spline(window).point_count_v, 3)
    t.assertEqual(sum(p.select for p in _spline(window).points), 2)
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
