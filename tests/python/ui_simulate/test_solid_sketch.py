# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sketch-to-solid through the viewport Add menu, E keymap, numeric modal input and undo."""

import modules.ui_test_utils as ui


def empty_sketch_activates_line_and_extrudes():
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Sketch')
    t.assertTrue(window.view_layer.objects.active.data.is_solid_model)
    with bpy.context.temp_override(window=window, area=area, region=region):
        tool = window.workspace.tools.from_space_view3d_mode(bpy.context.mode)
        t.assertEqual(tool.idname, 'builtin.nurbs_draw_line')
        t.assertEqual(bpy.ops.curve.draw_line(points='0 0 0 2 0 0 2 3 0 0 3 0 0 0 0'), {'FINISHED'})
    yield
    yield e.e().text('2').ret()
    t.assertRegex(bpy.context.scene.statistics(window.view_layer), r'Faces:1/6(?: |$)')
    yield e.tab()
    window.view_layer.update()
    t.assertAlmostEqual(window.view_layer.objects.active.dimensions.z, 2, places=5)


def rectangle_extrude_undo_and_cancel():
    import bpy

    e, t, window = ui.test_window()
    e.cursor_position_set(*ui.get_area_center_from_spacetype(window, 'VIEW_3D'), move=True)
    yield
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Rectangle Sketch')
    obj = window.view_layer.objects.active
    t.assertTrue(obj.data.is_solid_model)
    t.assertEqual(obj.mode, 'EDIT')
    yield e.e().text('3').ret()
    t.assertEqual(tuple(bpy.context.scene.tool_settings.mesh_select_mode), (False, False, True))
    statistics = bpy.context.scene.statistics(window.view_layer)
    t.assertRegex(statistics, r'Faces:1/6(?: |$)')

    yield e.ctrl.z()
    obj = window.view_layer.objects.active
    t.assertTrue(obj.data.is_solid_model)
    t.assertRegex(bpy.context.scene.statistics(window.view_layer), r'Faces:0/0(?: |$)')
    yield e.ctrl.shift.z()
    t.assertRegex(bpy.context.scene.statistics(window.view_layer), r'Faces:1/6(?: |$)')
    yield e.e().text('2').esc()
    t.assertRegex(bpy.context.scene.statistics(window.view_layer), r'Faces:1/6(?: |$)')
    yield e.tab()
    obj = window.view_layer.objects.active
    window.view_layer.update()
    t.assertAlmostEqual(obj.dimensions.z, 3, places=5)
    yield e.tab()
    yield e.e().text('1').ret()
    yield e.tab()
    window.view_layer.update()
    t.assertAlmostEqual(window.view_layer.objects.active.dimensions.z, 4, places=5)


def sketch_modal_cancel_preserves_wire():
    import bpy

    e, t, window = ui.test_window()
    e.cursor_position_set(*ui.get_area_center_from_spacetype(window, 'VIEW_3D'), move=True)
    yield
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Circle Sketch')
    x, y = ui.get_area_center_from_spacetype(window, 'VIEW_3D')
    yield e.cursor_position_set(x, y, move=True)
    yield e.e()
    yield e.cursor_position_set(x, y + 80, move=True)
    t.assertGreater(window.view_layer.objects.active.data.solid_faces_count, 0)
    yield e.esc()
    t.assertRegex(bpy.context.scene.statistics(window.view_layer), r'Faces:0/0(?: |$)')
    yield e.e().text('2').esc()
    obj = window.view_layer.objects.active
    t.assertTrue(obj.data.is_solid_model)
    t.assertEqual(tuple(bpy.context.scene.tool_settings.mesh_select_mode), (True, False, False))
    t.assertRegex(bpy.context.scene.statistics(window.view_layer), r'Faces:0/0(?: |$)')
    yield e.f()
    t.assertRegex(bpy.context.scene.statistics(window.view_layer), r'Faces:1/1(?: |$)')
    yield e.e().text('-2').ret()
    yield e.tab()
    window.view_layer.update()
    t.assertAlmostEqual(window.view_layer.objects.active.dimensions.z, 2, places=5)


def analytic_shape_construction_modes():
    from test_nurbs_shapes import construction_modes
    yield from construction_modes(analytic=True)


def analytic_deferred_numeric_input():
    from test_nurbs_numeric import deferred_parameters
    yield from deferred_parameters(analytic=True)


def analytic_arc_modes_numeric_and_snapping():
    from test_nurbs_arc import arc_workflow
    yield from arc_workflow(analytic=True)


def analytic_tangent_circle_modes():
    from test_nurbs_tangent_circle import tangent_circles
    yield from tangent_circles(analytic=True)


def analytic_snap_hover_and_anchor_reuse():
    from test_nurbs_draw_hover import drawing_snap_hover
    yield from drawing_snap_hover(analytic=True)


def analytic_line_backspace_preserves_construction():
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Sketch')
    yield e.numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 12
    window.scene.tool_settings.use_snap = False
    for point in ((0, 0, 0), (1, 0, 0), (1, 1, 0)):
        p = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        yield e.cursor_position_set(round(p.x) + region.x, round(p.y) + region.y, move=True)
        yield e.leftmouse()
    yield e.back_space().ret()
    data = window.view_layer.objects.active.data
    t.assertEqual((data.solid_vertices_count, data.solid_edges_count), (2, 1))

    yield e.ctrl.z()
    data = window.view_layer.objects.active.data
    t.assertEqual((data.solid_vertices_count, data.solid_edges_count), (0, 0))
    yield e.ctrl.shift.z()
    data = window.view_layer.objects.active.data
    t.assertEqual((data.solid_vertices_count, data.solid_edges_count), (2, 1))


def edge_extrusion_axis_cancel_and_undo():
    import bpy
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Line Sketch')
    yield e.numpad_1()
    x, y = ui.get_area_center(area)
    yield e.e()
    yield e.cursor_position_set(x, y + 80, move=True)
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 1)
    yield e.esc()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 0)
    yield e.e().z().text('2').ret()
    data = window.view_layer.objects.active.data
    t.assertEqual((data.solid_vertices_count, data.solid_edges_count, data.solid_faces_count), (4, 4, 1))
    yield e.e().y().text('3').esc()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 1)
    yield e.ctrl.z()
    data = window.view_layer.objects.active.data
    t.assertEqual((data.solid_vertices_count, data.solid_edges_count, data.solid_faces_count), (2, 1, 0))
    yield e.ctrl.shift.z()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 1)
    yield e.e().y().text('3').ret()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 2)


def bevel_shortcuts_cancel_and_undo():
    import bpy
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Rectangle Sketch')
    yield e.e().text('2').ret()
    yield e.two().a()
    yield e.ctrl.b().text('0.15').esc()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 6)
    yield e.ctrl.b().text('0.15').ret()
    t.assertGreater(window.view_layer.objects.active.data.solid_faces_count, 6)
    yield e.ctrl.z()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 6)
    yield e.ctrl.shift.z()
    t.assertGreater(window.view_layer.objects.active.data.solid_faces_count, 6)
    yield e.ctrl.z()
    yield e.one().a()
    yield e.ctrl.shift.b().text('0.1').ret()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 14)


def vertex_extrude_merge_menu_and_delete():
    import bpy
    import bmesh
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Line Sketch')
    with bpy.context.temp_override(window=window, area=area, region=region):
        bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
        for element in (*bm.faces, *bm.edges, *bm.verts):
            element.select_set(False)
        for v in bm.verts:
            v.select_set(v.co.x > 0)
        bm.select_flush_mode()
        bmesh.update_edit_mesh(window.view_layer.objects.active.data)
    yield e.e().z().text('2').ret()
    data = window.view_layer.objects.active.data
    t.assertEqual((data.solid_vertices_count, data.solid_edges_count), (3, 2))
    yield e.a()
    yield from ui.call_menu(e, 'Solid -> Merge -> At Center')
    data = window.view_layer.objects.active.data
    t.assertEqual((data.solid_vertices_count, data.solid_edges_count), (1, 0))
    yield e.ctrl.z()
    t.assertEqual(window.view_layer.objects.active.data.solid_vertices_count, 3)
    yield e.a()
    yield from ui.call_menu(e, 'Solid -> Delete -> Vertices')
    t.assertEqual(window.view_layer.objects.active.data.solid_vertices_count, 0)
