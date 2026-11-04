# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sketch-to-solid through the viewport Add menu, E keymap, numeric modal input and undo."""

import modules.ui_test_utils as ui


def fillet_rotation_cancel_and_undo():
    import bpy
    import bmesh

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.solid.sketch_add(type='RECTANGLE')
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.solid.bevel(offset=.2, segments=1)
        bpy.ops.mesh.select_mode(type='VERT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.ed.undo_push(message='Fillet before rotation')

    def positions():
        return [tuple(v.co) for v in bmesh.from_edit_mesh(window.view_layer.objects.active.data).verts]

    def normals():
        bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
        layer = bm.loops.layers.float_vector['custom_normal']
        return [tuple(loop[layer]) for face in bm.faces for loop in face.loops]

    before = positions()
    with bpy.context.temp_override(window=window, area=area, region=region):
        # Seed evaluated bounds so stale bounds during a drag cannot pass unnoticed.
        bpy.context.view_layer.update()
        obj = window.view_layer.objects.active
        tuple(obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).bound_box)
    before_normals = normals()
    yield e.r().z().text('20').esc()
    t.assertEqual(positions(), before)
    t.assertEqual(normals(), before_normals)
    yield e.r().z().text('20')
    during_drag = positions()
    during_normals = normals()
    # The evaluated mesh used by drawing and picking must follow the live BMesh.
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.context.view_layer.update()
        obj = window.view_layer.objects.active
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            t.assertEqual([tuple(v.co) for v in mesh.vertices], during_drag)
            for axis in range(3):
                t.assertAlmostEqual(min(p[axis] for p in evaluated.bound_box),
                                    min(p[axis] for p in during_drag), places=5)
                t.assertAlmostEqual(max(p[axis] for p in evaluated.bound_box),
                                    max(p[axis] for p in during_drag), places=5)
        finally:
            evaluated.to_mesh_clear()
    t.assertNotEqual(during_drag, before)
    yield e.ret()
    rotated = positions()
    t.assertEqual(rotated, during_drag)  # Confirmation must not move any visible vertex.
    t.assertEqual(normals(), during_normals)
    t.assertNotEqual(rotated, before)
    yield e.ctrl.z()
    t.assertEqual(positions(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(positions(), rotated)


def _angled_box_wheel_fillet(all_edges=False):
    import bpy
    import bmesh

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.solid.sketch_add(type='RECTANGLE')
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.transform.translate(value=(.4, 0, 0))
        if all_edges:
            bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.ed.undo_push(message='Angled solid before wheel fillet')
    mesh = window.view_layer.objects.active.data
    t.assertEqual(mesh.solid_faces_count, 6)
    yield e.ctrl.b().text('0.12')
    chamfer_count = mesh.solid_faces_count
    t.assertGreater(chamfer_count, 6)
    yield e.wheelupmouse()
    rounded_count = mesh.solid_faces_count
    t.assertGreater(rounded_count, 6)
    t.assertGreater(len(bmesh.from_edit_mesh(mesh).faces), rounded_count)
    t.assertTrue(all(edge.is_manifold for edge in bmesh.from_edit_mesh(mesh).edges))
    yield e.wheelupmouse()
    t.assertGreaterEqual(mesh.solid_faces_count, rounded_count)
    yield e.wheeldownmouse().wheeldownmouse()
    t.assertEqual(mesh.solid_faces_count, chamfer_count)
    yield e.wheelupmouse().ret()
    t.assertEqual(mesh.solid_faces_count, rounded_count)
    yield e.ctrl.z()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 6)
    yield e.ctrl.shift.z()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, rounded_count)


def angled_top_face_wheel_fillet():
    yield from _angled_box_wheel_fillet()


def angled_whole_box_wheel_fillet():
    yield from _angled_box_wheel_fillet(all_edges=True)


def smooth_fillet_trim_pieces_select_as_one_face():
    import bpy
    import bmesh
    import math
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.solid.sketch_add(type='RECTANGLE')
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.transform.translate(value=(.4, 0, 0))
        bpy.ops.solid.bevel(offset=.35, segments=1)
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.wm.tool_set_by_id(name='builtin.select_box')
    yield e.numpad_1()
    area.spaces.active.region_3d.view_location = (.2, 0, 1)
    area.spaces.active.region_3d.view_distance = 3.6
    yield
    selected = None
    for degrees in (45, 84):
        angle = math.radians(degrees)
        point = Vector((.2, -.65 - .35 * math.sin(angle), 1.65 + .35 * math.cos(angle)))
        co = location_3d_to_region_2d(region, area.spaces.active.region_3d, point)
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        yield e.leftmouse()
        current = {i for i, face in enumerate(bmesh.from_edit_mesh(window.view_layer.objects.active.data).faces)
                   if face.select}
        t.assertGreater(len(current), 1)
        if selected is not None:
            t.assertEqual(current, selected)
        selected = current
        yield e.alt.a()


def starter_line_extrusion_returns_to_selection():
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Line Sketch')
    def active_tool():
        return window.workspace.tools.from_space_view3d_mode('EDIT_MESH').idname
    t.assertEqual(active_tool(), 'builtin.nurbs_draw_line')
    yield e.e().z().text('2').esc()
    t.assertEqual(active_tool(), 'builtin.nurbs_draw_line')
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 0)
    yield e.e().z().text('2').ret()
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 1)
    t.assertEqual(active_tool(), 'builtin.select_box')


def starter_line_extrusion_exec_returns_to_selection():
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Line Sketch')
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.solid.extrude(offset=(0, 0, 2)), {'FINISHED'})
    t.assertEqual(window.view_layer.objects.active.data.solid_faces_count, 1)
    t.assertEqual(window.workspace.tools.from_space_view3d_mode('EDIT_MESH').idname, 'builtin.select_box')


def fillet_click_selects_source_face():
    import bpy
    import bmesh
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.solid.sketch_add(type='RECTANGLE')
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.solid.bevel(offset=.2, segments=1)
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.wm.tool_set_by_id(name='builtin.select_box')
    yield e.numpad_1()
    area.spaces.active.region_3d.view_location = (0, 0, 1)
    area.spaces.active.region_3d.view_distance = 6
    yield
    point = Vector((0, -.8 - .2 / 2**.5, 1.8 + .2 / 2**.5))
    co = location_3d_to_region_2d(region, area.spaces.active.region_3d, point)
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.leftmouse()
    mesh = window.view_layer.objects.active.data
    t.assertEqual(sum(f.select for f in bmesh.from_edit_mesh(mesh).faces), 16)
    yield e.shift.leftmouse()
    t.assertEqual(sum(f.select for f in bmesh.from_edit_mesh(mesh).faces), 0)


def sketch_rightclick_returns_to_selection():
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Line Sketch')
    yield e.rightmouse()
    t.assertEqual(window.workspace.tools.from_space_view3d_mode('EDIT_MESH').idname, 'builtin.select_box')
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.wm.tool_set_by_id(name='builtin.solid_extrude'), {'FINISHED'})
        t.assertEqual(window.workspace.tools.from_space_view3d_mode('EDIT_MESH').idname, 'builtin.solid_extrude')
        bpy.ops.wm.tool_set_by_id(name='builtin.nurbs_draw_line')
    yield e.numpad_7()
    area.spaces.active.region_3d.view_location = (0, 0, 0)
    area.spaces.active.region_3d.view_distance = 10
    window.scene.tool_settings.use_snap = False
    yield
    for point in ((2, 2, 0), (3, 2, 0)):
        co = location_3d_to_region_2d(region, area.spaces.active.region_3d, Vector(point))
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        yield e.leftmouse()
    yield e.rightmouse()
    t.assertEqual(window.workspace.tools.from_space_view3d_mode('EDIT_MESH').idname, 'builtin.select_box')
    t.assertEqual(window.view_layer.objects.active.data.solid_edges_count, 2)


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


def _duplicate_move(mode):
    import bpy
    import bmesh
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    yield from ui.call_menu(e, 'Add -> Solid -> Rectangle Sketch' if mode == 'FACE' else 'Add -> Solid -> Line Sketch')
    if mode == 'FACE':
        yield e.e().text('2').ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.mesh.select_mode(type=mode)
        if mode == 'VERT':
            bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
            for element in (*bm.faces, *bm.edges, *bm.verts):
                element.select_set(False)
            for v in bm.verts:
                v.select_set(v.co.x > 0)
            bm.select_flush_mode()
            bmesh.update_edit_mesh(window.view_layer.objects.active.data)

    def counts():
        d = window.view_layer.objects.active.data
        return (d.solid_vertices_count, d.solid_edges_count, d.solid_faces_count)
    before = counts()
    delta = {'VERT': (1, 0, 0), 'EDGE': (2, 1, 0), 'FACE': (4, 4, 1)}[mode]
    expected = tuple(a + b for a, b in zip(before, delta))
    yield e.shift.d().z().text('3').ret()
    t.assertEqual(counts(), expected)
    bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
    heights = [v.co.z for v in bm.verts]
    t.assertAlmostEqual(min(heights), 0)
    t.assertAlmostEqual(max(heights), 5 if mode == 'FACE' else 3)
    yield e.ctrl.z()
    t.assertEqual(counts(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), expected)
    yield e.shift.d().z().text('1').esc()
    t.assertEqual(counts(), tuple(a + b for a, b in zip(expected, delta)))


def duplicate_vertex_move():
    yield from _duplicate_move('VERT')


def duplicate_edge_move():
    yield from _duplicate_move('EDGE')


def duplicate_face_move():
    yield from _duplicate_move('FACE')


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


def fillet_rotation_updates_dependent_mesh():
    import bpy
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.solid.sketch_add(type='RECTANGLE')
        bpy.ops.solid.extrude(distance=2)
        source = window.view_layer.objects.active
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.solid.bevel(offset=.2, segments=1)
        bpy.ops.mesh.select_mode(type='VERT')
        bpy.ops.mesh.select_all(action='SELECT')
        mesh = bpy.data.meshes.new('Dependent point')
        mesh.from_pydata([(1.5, 0, 1)], [], [])
        dependent = bpy.data.objects.new('Dependent point', mesh)
        bpy.context.collection.objects.link(dependent)
        shrinkwrap = dependent.modifiers.new('Follow solid', 'SHRINKWRAP')
        shrinkwrap.target = source
        shrinkwrap.wrap_method = 'NEAREST_SURFACEPOINT'

    def position():
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.context.view_layer.update()
            evaluated = dependent.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return Vector(mesh.vertices[0].co)
            finally:
                evaluated.to_mesh_clear()

    before = position()
    yield e.r().z().text('20')
    t.assertGreater((position() - before).length, .01)
    yield e.esc()
    t.assertLess((position() - before).length, 1e-5)


def _partial_transform_cancel_and_undo(component):
    import bpy
    import bmesh

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.a().x().ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        if component:
            bpy.ops.mesh.select_mode(type='EDGE')
            bpy.ops.mesh.select_all(action='SELECT')
            bpy.ops.solid.bevel(offset=.2, segments=1)
            bpy.ops.mesh.select_mode(type='VERT')
            bpy.ops.mesh.select_all(action='SELECT')
            bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (4, 0, 0)})
        else:
            bpy.ops.mesh.select_mode(type='VERT')
            bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
            for element in (*bm.faces, *bm.edges, *bm.verts):
                element.select_set(False)
            for vertex in bm.verts:
                vertex.select_set(vertex.co.z > 1.99)
            bm.select_flush_mode()
            bmesh.update_edit_mesh(window.view_layer.objects.active.data)
        bpy.ops.ed.undo_push(message='Partial solid before transform')
    obj = window.view_layer.objects.active
    matrix = obj.matrix_world.copy()

    def snapshot():
        bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
        layer = bm.loops.layers.float_vector['custom_normal']
        return ([tuple(v.co) for v in bm.verts],
                [tuple(loop[layer]) for face in bm.faces for loop in face.loops])

    before = snapshot()
    stationary = [i for i, p in enumerate(before[0]) if p[0] < 2] if component else [
        i for i, p in enumerate(before[0]) if p[2] < .01]
    command = e.r().z().text('20') if component else e.g().z().text('.4')
    yield command
    during = snapshot()
    t.assertNotEqual(during[0], before[0])
    t.assertEqual(obj.matrix_world, matrix)
    for i in stationary:
        t.assertEqual(during[0][i], before[0][i])
    yield e.esc()
    t.assertEqual(snapshot(), before)
    yield e.r().z().text('20') if component else e.g().z().text('.4')
    during = snapshot()
    yield e.ret()
    t.assertEqual(snapshot(), during)
    t.assertEqual(window.view_layer.objects.active.matrix_world, matrix)
    yield e.ctrl.z()
    t.assertEqual(snapshot(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(snapshot(), during)


def isolated_fillet_component_rotation():
    yield from _partial_transform_cancel_and_undo(True)


def connected_planar_face_move():
    yield from _partial_transform_cancel_and_undo(False)
