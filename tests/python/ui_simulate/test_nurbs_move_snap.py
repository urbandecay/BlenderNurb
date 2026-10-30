# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Changing viewport snapping during a native NURBS move."""
import modules.ui_test_utils as ui


def move_snap_button():
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    header = next(r for r in area.regions if r.type == 'HEADER')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    curve_obj = window.view_layer.objects.active
    yield e.tab().one().numpad_7()
    space = area.spaces.active
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 10

    target_mesh = bpy.data.meshes.new('Move snap target')
    target_mesh.from_pydata(((0, 2, 0), (3, 2, 0), (0, 5, 0)), (), ((0, 1, 2),))
    target = bpy.data.objects.new('Move snap target', target_mesh)
    window.scene.collection.objects.link(target)

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(region.x + xy.x), round(region.y + xy.y)

    settings = window.scene.tool_settings
    settings.snap_elements = {'VERTEX'}
    settings.use_snap = False
    yield e.alt.a()
    yield e.cursor_position_set(*screen((1, 0, 0)), move=True)
    yield e.leftmouse().g()
    yield e.cursor_position_set(*screen((0.08, 2, 0)), move=True)
    # Click the viewport magnet while the transform is still running.
    yield e.cursor_position_set(header.x + header.width // 2 + 7, header.y + 13, move=True)
    yield e.leftmouse()
    t.assertTrue(settings.use_snap)
    t.assertIn('TRANSFORM_OT_translate', [op.bl_idname for op in window.modal_operators])
    # Returning to the viewport must refresh snap state and snap the moved vertex.
    yield e.cursor_position_set(*screen((0.08, 2, 0)), move=True)
    evaluated = curve_obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        t.assertTrue(any((v.co - Vector((0, 2, 0))).length < 1e-4 for v in mesh.vertices),
                     'Grabbed NURBS vertex did not snap to the target after enabling the magnet')
    finally:
        evaluated.to_mesh_clear()
    t.assertIn('TRANSFORM_OT_translate', [op.bl_idname for op in window.modal_operators])
    yield e.ret()
    t.assertNotIn('TRANSFORM_OT_translate', [op.bl_idname for op in window.modal_operators])
    print('PASS viewport snap button toggles and snaps during NURBS move', flush=True)


def move_custom_snap_base():
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    curve_obj = window.view_layer.objects.active
    yield e.tab().one().numpad_7().a()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.curve.subdivide(number_cuts=1)
    t.assertTrue(curve_obj.data.is_native_nurbs)
    space.region_3d.view_location = (0, 1, 0)
    space.region_3d.view_distance = 12

    target_mesh = bpy.data.meshes.new('Custom base target')
    target_mesh.from_pydata(((0, 2, 0), (3, 2, 0), (0, 5, 0)), (), ((0, 1, 2),))
    target = bpy.data.objects.new('Custom base target', target_mesh)
    window.scene.collection.objects.link(target)
    settings = window.scene.tool_settings

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(region.x + xy.x), round(region.y + xy.y)

    def assert_line(left, right):
        evaluated = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            coordinates = [tuple(v.co) for v in mesh.vertices]
            t.assertTrue(any((v.co - Vector(left)).length < 1e-4 for v in mesh.vertices),
                         f'Expected {left}, got {coordinates}')
            t.assertTrue(any((v.co - Vector(right)).length < 1e-4 for v in mesh.vertices),
                         f'Expected {right}, got {coordinates}')
        finally:
            evaluated.to_mesh_clear()
        t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)

    # A selected endpoint, an external endpoint, and an edge midpoint can all be bases.
    for elements, base, destination, left, right, snap_enabled in (
            ({'VERTEX'}, (-1, 0, 0), (0, 2, 0), (0, 2, 0), (2, 2, 0), True),
            ({'VERTEX'}, (3, 2, 0), (0, 2, 0), (-4, 0, 0), (-2, 0, 0), False),
            ({'EDGE_MIDPOINT'}, (1.5, 2, 0), (0, 3.5, 0), (-2.5, 1.5, 0), (-0.5, 1.5, 0), True)):
        settings.snap_elements = elements
        settings.use_snap = snap_enabled
        yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
        yield e.g().b()
        yield e.cursor_position_set(*screen(base), move=True)
        yield e.leftmouse()
        t.assertIn('TRANSFORM_OT_translate', [op.bl_idname for op in window.modal_operators])
        yield e.cursor_position_set(*screen(destination), move=True)
        assert_line(left, right)
        yield e.esc()
        assert_line((-1, 0, 0), (1, 0, 0))

    settings.snap_elements = {'VERTEX'}
    settings.use_snap = True
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.g().b()
    yield e.cursor_position_set(*screen((-1, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((0, 2, 0)), move=True)
    yield e.ret()
    assert_line((0, 2, 0), (2, 2, 0))
    yield e.ctrl.z()
    assert_line((-1, 0, 0), (1, 0, 0))
    yield e.ctrl.shift.z()
    assert_line((0, 2, 0), (2, 2, 0))
    print('PASS NURBS G B: selected and external bases, edge midpoint, cancel, confirm, undo, redo', flush=True)
