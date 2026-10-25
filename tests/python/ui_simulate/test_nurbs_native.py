# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native source entry and vertex selection through Blender's viewport events.

Requires the explicitly exported C++ fixture in BLENDER_NURBS_PREVIEW_FIXTURE.
The legacy splines collection is empty throughout. No Python geometry substitute
or public native component API is used to make this test pass.
"""

import modules.ui_test_utils as ui


def vertex_selection_and_lifecycle():
    import os
    import re
    from pathlib import Path

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    t.assertTrue(hasattr(bpy.types, "VIEW3D_MT_select_edit_nurbs"), "Updated startup UI scripts are required")
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    output = Path(os.environ.get("BLENDER_NURBS_EDIT_OUTPUT", "/tmp/blender-nurbs-edit-ui"))
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    fixture = os.environ["BLENDER_NURBS_PREVIEW_FIXTURE"]
    with bpy.data.libraries.load(fixture, link=False) as (source, target):
        t.assertIn("Native preview sheet", source.curves)
        target.curves = ["Native preview sheet"]
    curve = target.curves[0]
    t.assertEqual(len(curve.splines), 0)
    t.assertTrue(curve.is_native_nurbs)
    obj = bpy.data.objects.new("Native NURBS edit fixture", curve)
    obj.color = (0.2, 0.5, 0.85, 1.0)
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    window.view_layer.update()
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield e.tab()
    t.assertEqual(obj.mode, 'EDIT')
    t.assertEqual(len(curve.splines), 0)
    space = area.spaces.active
    space.shading.color_type = 'OBJECT'
    space.shading.show_xray = True
    space.overlay.show_stats = True
    yield e.numpad_7()
    space.region_3d.view_location = (0.0, 0.0, 0.0)
    space.region_3d.view_distance = 5.0
    yield

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r"Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)", stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, obj.matrix_world @ Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def save(name):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / name), check_existing=False)

    def display_faces():
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh, "Native preview failed; inspect the reported geometry diagnostic")
                return len(mesh.polygons)
            finally:
                evaluated.to_mesh_clear()

    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    initial_display_faces = display_faces()
    yield e.a()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.alt.a()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))

    bottom = [screen((-1, -1, 0)), screen((1, -1, 0))]
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (1, 4, 0, 4, 0, 1))
    yield e.cursor_position_set(*bottom[1], move=True)
    yield e.shift.leftmouse()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.ctrl.z()
    t.assertEqual(counts(), (1, 4, 0, 4, 0, 1))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    # Undo can replace IDs. Reacquire the original owner rather than retaining RNA pointers.
    obj = window.view_layer.objects.active
    curve = obj.data
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.view3d.snap_cursor_to_active(), {'FINISHED'})
    t.assertEqual(tuple(window.scene.cursor.location), (1, -1, 0))
    window.scene.cursor.location = (0, 0, 0)
    pivot = window.scene.tool_settings.transform_pivot_point
    window.scene.tool_settings.transform_pivot_point = 'ACTIVE_ELEMENT'
    yield e.g().z().text('0.125').esc()
    window.scene.tool_settings.transform_pivot_point = pivot
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.tab()
    save("click.blend")
    yield e.tab()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.alt.a()
    # Vertex picking uses source positions, independently of the display sampling density.
    curve.resolution_u = 13
    curve.resolution_v = 7
    yield
    updated_display_faces = display_faces()
    t.assertGreater(updated_display_faces, initial_display_faces)
    print("NATIVE_NURBS_EDIT_DISPLAY_COUNTS", initial_display_faces, updated_display_faces)

    # Ctrl+L grows the selected source component; L/Shift+L use the same source picker
    # as click selection. Neither invents an active vertex for a bulk/hover selection.
    yield e.ctrl.l()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.leftmouse()
    yield e.ctrl.l()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.ctrl.z()
    t.assertEqual(counts(), (1, 4, 0, 4, 0, 1))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    obj = window.view_layer.objects.active
    curve = obj.data
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.view3d.snap_cursor_to_active(), {'FINISHED'})
    t.assertEqual(tuple(window.scene.cursor.location), (-1, -1, 0))
    window.scene.cursor.location = (0, 0, 0)
    yield e.tab()
    save("linked-click.blend")
    yield e.tab()
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.shift.l()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    obj = window.view_layer.objects.active
    curve = obj.data
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.l()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.g().z().text('0.5').esc()
    yield e.tab()
    save("linked-hover.blend")
    yield e.tab()
    yield e.alt.a()
    lower = min(point[0] for point in bottom) - 10, min(point[1] for point in bottom) - 10
    upper = max(point[0] for point in bottom) + 10, max(point[1] for point in bottom) + 10
    yield e.b()
    yield from e.leftmouse.cursor_motion((lower, upper))
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.ctrl.l()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.tab()
    save("linked-box.blend")
    yield e.tab()
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.shift.l()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    yield e.b()
    yield from e.leftmouse.cursor_motion((lower, upper))
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.tab()
    save("box.blend")
    yield e.tab()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    t.assertEqual(len(curve.splines), 0)
    yield
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / "native-edit.png"))

    # Native E is available; legacy factories must reject creating another source.
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertTrue(bpy.ops.curve.extrude.poll())
        try:
            curve.splines.new('NURBS')
        except RuntimeError as error:
            t.assertIn("legacy splines", str(error))
        else:
            t.fail("Legacy spline creation accepted a native source")
        try:
            bpy.ops.surface.primitive_nurbs_surface_surface_add()
        except RuntimeError as error:
            t.assertIn("native NURBS", str(error))
        else:
            t.fail("Legacy primitive addition accepted a native edit source")
        try:
            result = bpy.ops.transform.rotate(value=1.0, orient_axis='Z', orient_type='NORMAL')
        except RuntimeError as error:
            t.assertIn("Native NURBS", str(error))
        else:
            t.assertEqual(result, {'CANCELLED'})
    t.assertEqual(len(curve.splines), 0)
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.tab()
    obj.location.x = 2.0
    window.view_layer.update()
    with bpy.context.temp_override(window=window, area=area, region=region):
        for operation in (
                lambda: bpy.ops.object.transform_apply(location=True, rotation=False, scale=False),
                lambda: bpy.ops.object.origin_set(type='ORIGIN_CURSOR')):
            try:
                operation()
            except RuntimeError as error:
                t.assertIn("native NURBS", str(error))
            else:
                t.fail("Unsupported native source transform was accepted")
            t.assertEqual(obj.location.x, 2.0)
        try:
            key = obj.shape_key_add(name="unsupported")
        except RuntimeError:
            pass
        else:
            t.assertIsNone(key)
        t.assertIsNone(curve.shape_keys)
    obj.location.x = 0.0
    window.view_layer.update()
    save("try-native-selection.blend")
    print("NATIVE_NURBS_EDIT_UI_ARTIFACTS", output)


def vertex_move_and_lifecycle():
    import os
    from pathlib import Path

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    output = Path(os.environ.get("BLENDER_NURBS_MOVE_OUTPUT", "/tmp/blender-nurbs-move-ui"))
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ["BLENDER_NURBS_PREVIEW_FIXTURE"], link=False) as (_, target):
        target.curves = ["Native preview sheet"]
    obj = bpy.data.objects.new("Native NURBS move fixture", target.curves[0])
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    window.view_layer.update()
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield e.tab()
    space = area.spaces.active
    space.shading.show_xray = True
    yield e.numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 5
    yield

    def current_object():
        return window.view_layer.objects.active

    def preview_max_z():
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            evaluated = current_object().evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh)
                t.assertGreater(len(mesh.polygons), 0)
                return max(vertex.co.z for vertex in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

    def save(name):
        t.assertEqual(len(current_object().data.splines), 0)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / name), check_existing=False)

    co = location_3d_to_region_2d(region, space.region_3d, Vector((-1, -1, 0)))
    t.assertIsNotNone(co)
    corner = round(co.x) + region.x, round(co.y) + region.y
    yield e.alt.a()
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    decimal = '0.12345678901234567'
    yield e.g().z().text(decimal).ret()
    t.assertAlmostEqual(preview_max_z(), float(decimal), places=7)
    # Retain the actual operator metadata for explicit redo execution below.
    move = bpy.context.active_operator
    t.assertEqual(move.bl_idname, 'TRANSFORM_OT_translate')
    precise = move.value_precision
    displayed = tuple(move.value)
    t.assertTrue(precise.startswith('v1 '))
    t.assertEqual(float(move.value_precise_z), float(decimal))
    # Open F9 while Move is the active registered operation, before saving or undoing.
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertTrue(bpy.ops.screen.redo_last.poll())
    yield e.f9()
    yield
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / "native-f9.png"))
    yield e.ret()
    yield
    t.assertAlmostEqual(preview_max_z(), float(decimal), places=7)
    save("move-f9.blend")
    save("move-click.blend")
    yield e.g().z().text('4').esc()
    t.assertAlmostEqual(preview_max_z(), float(decimal), places=7)
    save("move-cancel.blend")
    yield e.ctrl.z()
    t.assertEqual(preview_max_z(), 0)
    save("move-undo.blend")
    yield e.ctrl.shift.z()
    t.assertAlmostEqual(preview_max_z(), float(decimal), places=7)
    save("move-redo.blend")
    # Rewind the movement, then reach the same source corner through box selection.
    yield e.ctrl.z()
    t.assertEqual(preview_max_z(), 0)
    yield e.alt.a()
    lower = corner[0] - 8, corner[1] - 8
    upper = corner[0] + 8, corner[1] + 8
    yield e.b()
    yield from e.leftmouse.cursor_motion((lower, upper))
    yield e.g().z().text(decimal).ret()
    t.assertAlmostEqual(preview_max_z(), float(decimal), places=7)
    save("move-box.blend")
    # A fresh explicit replay must also use the double delta rather than its displayed float.
    yield e.ctrl.z()
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.transform.translate(value=displayed, value_precision=precise)
    t.assertEqual(result, {'FINISHED'})
    save("move-replay.blend")

    def reset_sheet(scale_z=1.0):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.object.mode_set(mode='OBJECT')
        for old in tuple(bpy.data.objects):
            bpy.data.objects.remove(old, do_unlink=True)
        for old in tuple(bpy.data.curves):
            if old.users == 0:
                bpy.data.curves.remove(old)
        with bpy.data.libraries.load(os.environ["BLENDER_NURBS_PREVIEW_FIXTURE"], link=False) as (_, data):
            data.curves = ["Native preview sheet"]
        fresh = bpy.data.objects.new("Native NURBS move fixture", data.curves[0])
        window.scene.collection.objects.link(fresh)
        fresh.scale.z = scale_z
        fresh.select_set(True)
        window.view_layer.objects.active = fresh
        window.scene.unit_settings.system = 'NONE'
        window.scene.unit_settings.scale_length = 1
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.curve.select_all(action='DESELECT')

    reset_sheet()
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    yield e.g().text(decimal).tab().text('0.23456789012345678').tab().text('0.34567890123456789').tab().ret()
    t.assertAlmostEqual(preview_max_z(), 0.34567890123456789, places=7)
    save("move-multi-axis.blend")

    reset_sheet()
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    yield e.g().z().text(decimal).shift.back_space().ret()
    t.assertEqual(preview_max_z(), 0)
    save("move-reset.blend")
    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            result = bpy.ops.transform.translate(value=(1e-30, 0, 0))
        except RuntimeError as error:
            t.assertIn("source coordinate precision", str(error))
        else:
            t.assertEqual(result, {'CANCELLED'})
    save("move-unrepresentable.blend")
    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            result = bpy.ops.transform.translate(value=(0, 0, 1), orient_type='NORMAL')
        except RuntimeError as error:
            t.assertIn("source orientation adapter", str(error))
        else:
            t.assertEqual(result, {'CANCELLED'})
    save("move-unsupported-orientation.blend")
    yield e.g().z().text(decimal).text('-').ret()
    save("move-negative.blend")

    reset_sheet()
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    yield e.g().z().text(decimal).text('/').ret()
    t.assertAlmostEqual(preview_max_z(), 1 / float(decimal), places=5)
    save("move-inverse.blend")

    reset_sheet(scale_z=4)
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    yield e.g().z().z().text(decimal).ret()
    t.assertAlmostEqual(preview_max_z(), float(decimal) / 4, places=7)
    save("move-scaled-object.blend")

    reset_sheet()
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    window.scene.unit_settings.system = 'METRIC'
    window.scene.unit_settings.length_unit = 'METERS'
    yield e.g().z().text('=12.345678901234567cm').ret()
    t.assertAlmostEqual(preview_max_z(), 12.345678901234567 * 0.01, places=7)
    save("move-unit.blend")

    reset_sheet()
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            bpy.ops.transform.translate(value=displayed, value_precision=precise, value_precise_z='1/0')
        except RuntimeError as error:
            t.assertIn("Invalid exact NURBS movement", str(error))
        else:
            t.fail("Invalid precise redo expression was accepted")
    save("move-f9-invalid.blend")
    # An explicit decimal field edit must replace the old precise value even when both
    # values have the same float representation in Blender's ordinary operator property.
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.transform.translate(value=displayed, value_precision=precise,
                                             value_precise_z=repr(displayed[2]))
    t.assertEqual(result, {'FINISHED'})
    save("move-f9-alias-edit.blend")

    reset_sheet()
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    yield e.g().z().text('=1e-200').ret()
    tiny_precision = bpy.context.active_operator.value_precision
    t.assertEqual(float(bpy.context.active_operator.value_precise_z), 1e-200)
    save("move-sub-float.blend")
    reset_sheet()
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.transform.translate(value=(0, 0, 0), value_precision=tiny_precision,
                                             value_precise_z='0')
    t.assertEqual(result, {'FINISHED'})
    save("move-f9-zero-alias.blend")
    yield e.g().z().text('1').ret()
    yield e.f9()
    yield
    # Factory UI, anchored at the selected corner: third decimal field in the Move popup.
    yield e.cursor_position_set(corner[0] + 210, corner[1] - 94, move=True)
    yield e.leftmouse().ctrl.a().text('0.12345679').ret()
    yield e.esc()
    yield
    t.assertAlmostEqual(preview_max_z(), 0.12345679, places=7)
    t.assertEqual(float(bpy.context.active_operator.value_precise_z), 0.12345679)
    save("move-f9-ui-edit.blend")
    yield e.cursor_position_set(*corner, move=True)
    yield e.g().z().text('1').ret()
    yield e.numpad_1().numpad_6().numpad_8()
    save("try-native-move.blend")
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / "native-move.png"))
    print("NATIVE_NURBS_MOVE_UI_ARTIFACTS", output)


def linked_selection_across_objects():
    import os
    import re

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
        data.curves = ['Native preview sheet']
    objects = []
    for index, x in enumerate((-2, 2)):
        curve = data.curves[0] if index == 0 else data.curves[0].copy()
        obj = bpy.data.objects.new('Native linked selection %d' % index, curve)
        window.scene.collection.objects.link(obj)
        obj.location.x = x
        obj.select_set(True)
        objects.append(obj)
    window.view_layer.objects.active = objects[1]
    window.view_layer.update()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    space = area.spaces.active
    space.shading.show_xray = True
    yield e.numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 9
    yield

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    def screen(position):
        point = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(point)
        return round(point.x) + region.x, round(point.y) + region.y

    left, right = screen((-3, -1, 0)), screen((1, -1, 0))
    t.assertEqual(counts(), (0, 8, 0, 8, 0, 2))
    active_name = window.view_layer.objects.active.name
    yield e.cursor_position_set(*left, move=True)
    yield e.l()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    t.assertEqual(window.view_layer.objects.active.name, active_name)
    yield e.ctrl.z()
    t.assertEqual(counts(), (0, 8, 0, 8, 0, 2))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    yield e.cursor_position_set(*right, move=True)
    yield e.shift.l()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    yield e.l()
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    yield e.cursor_position_set(*left, move=True)
    yield e.shift.l()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    yield e.ctrl.l()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    yield e.alt.a()
    yield e.cursor_position_set(*left, move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*right, move=True)
    yield e.shift.leftmouse()
    t.assertEqual(counts(), (2, 8, 0, 8, 0, 2))
    # A seed in each owner grows both components without joining their identities.
    space.shading.show_xray = False
    yield e.ctrl.l()
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    yield e.ctrl.z()
    t.assertEqual(counts(), (2, 8, 0, 8, 0, 2))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    for obj in window.view_layer.objects:
        t.assertEqual(obj.mode, 'EDIT')
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)

    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            result = bpy.ops.curve.duplicate()
        except RuntimeError as error:
            t.assertIn('single edit object', str(error))
        else:
            t.assertEqual(result, {'CANCELLED'})
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            result = bpy.ops.curve.extrude()
        except RuntimeError as error:
            t.assertIn('single edit object', str(error))
        else:
            t.assertEqual(result, {'CANCELLED'})
    yield
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    # Deletion has no modal Move restriction; both owners are preflighted and committed.
    # Python calls default to no undo; explicitly request it for this undo/redo check.
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.delete('EXEC_DEFAULT', True, type='ONLY_FACE'), {'FINISHED'})
    yield
    t.assertEqual(counts(), (8, 8, 8, 8, 0, 0))
    yield e.ctrl.z()
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (8, 8, 8, 8, 0, 0))
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.delete(type='EDGE'), {'FINISHED'})
    yield
    t.assertEqual(counts(), (0, 0, 0, 0, 0, 0))


def linked_selection_rejects_ambiguous_holes_without_partial_changes():
    import os
    import re

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
        data.curves = ['Native preview sheet', 'Native preview hole']
    for curve, x in zip(data.curves, (-2, 2)):
        obj = bpy.data.objects.new(curve.name, curve)
        window.scene.collection.objects.link(obj)
        obj.location.x = x
        obj.select_set(True)
        window.view_layer.objects.active = obj
    window.view_layer.update()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    space = area.spaces.active
    space.shading.show_xray = True
    yield e.numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 9
    yield

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    def screen(position):
        point = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(point)
        return round(point.x) + region.x, round(point.y) + region.y

    yield e.cursor_position_set(*screen((-3, -1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, -1, 0)), move=True)
    yield e.shift.leftmouse()
    expected = (2, 12, 0, 12, 0, 2)
    t.assertEqual(counts(), expected)
    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            result = bpy.ops.curve.select_linked()
        except RuntimeError as error:
            t.assertIn('boundary or surface rule', str(error))
        else:
            t.assertEqual(result, {'CANCELLED'})
    # The other owner's ordinary sheet must remain a single selected point.
    t.assertEqual(counts(), expected)
    yield e.l()
    t.assertEqual(counts(), expected)
    for obj in window.view_layer.objects:
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)


def duplicate_and_move_source_components():
    import os
    import re
    from pathlib import Path

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    output = Path(os.environ.get('BLENDER_NURBS_DUPLICATE_OUTPUT', '/tmp/blender-nurbs-duplicate-ui'))
    output.mkdir(parents=True, exist_ok=True)
    space = area.spaces.active
    space.overlay.show_stats = True

    def reset(name='Native preview sheet'):
        if window.view_layer.objects.active is not None and window.view_layer.objects.active.mode == 'EDIT':
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.object.mode_set(mode='OBJECT')
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        for curve in tuple(bpy.data.curves):
            if curve.users == 0:
                bpy.data.curves.remove(curve)
        with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
            data.curves = [name]
        obj = bpy.data.objects.new('Native duplication fixture', data.curves[0])
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.curve.select_all(action='DESELECT')
        space.shading.show_xray = True

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    def screen(position):
        point = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(point)
        return round(point.x) + region.x, round(point.y) + region.y

    def save(name):
        obj = window.view_layer.objects.active
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / name), check_existing=False)

    reset()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 5
    yield
    yield e.shift.d()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    bottom = [screen((-1, -1, 0)), screen((1, -1, 0))]
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.leftmouse()
    yield e.ctrl.l()
    decimal = '0.12345678901234567'
    yield e.shift.d().z().text(decimal).ret()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertTrue(bpy.ops.screen.redo_last.poll())
    yield e.f9()
    yield
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / 'native-duplicate-f9.png'))
    yield e.ret()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    save('duplicate-click.blend')
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    save('duplicate-undo.blend')
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    save('duplicate-redo.blend')
    # Esc cancels Move while keeping Duplicate, as in Blender mesh editing.
    yield e.shift.d().z().text('2').esc()
    t.assertEqual(counts(), (4, 12, 4, 12, 1, 3))
    save('duplicate-cancel.blend')
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))

    reset()
    yield
    lower, upper = screen((-1, -1, 0)), screen((1, 1, 0))
    lower = lower[0] - 8, lower[1] - 8
    upper = upper[0] + 8, upper[1] + 8
    yield e.b()
    yield from e.leftmouse.cursor_motion((lower, upper))
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.shift.d().z().text(decimal).ret()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    save('duplicate-box.blend')

    reset()
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.leftmouse()
    yield e.shift.d().z().text('0.125').ret()
    t.assertEqual(counts(), (1, 5, 0, 4, 0, 1))
    save('duplicate-point.blend')

    reset()
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*bottom[1], move=True)
    yield e.shift.leftmouse()
    yield e.shift.d().z().text('0.125').ret()
    t.assertEqual(counts(), (2, 6, 1, 5, 0, 1))
    save('duplicate-edge.blend')

    reset()
    yield e.cursor_position_set(*bottom[0], move=True)
    yield e.leftmouse()
    yield e.ctrl.l()
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.curve.duplicate_move(TRANSFORM_OT_translate={'value_precise_z': decimal})
    t.assertEqual(result, {'FINISHED'})
    # Scene.statistics is cached until Blender processes its queued change notifier.
    yield
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    save('duplicate-python.blend')

    reset('Native preview circle')
    yield e.a()
    yield e.shift.d().x().text('3').ret()
    t.assertEqual(counts(), (2, 4, 3, 6, 1, 2))
    save('duplicate-circle.blend')

    reset('Native preview hole')
    yield e.a()
    yield e.shift.d().x().text('3').ret()
    t.assertEqual(counts(), (8, 16, 8, 16, 1, 2))
    save('duplicate-hole.blend')
    space.region_3d.view_location = (1.5, 0, 0)
    space.region_3d.view_distance = 9
    yield e.numpad_7().numpad_2().numpad_2().numpad_6()
    save('try-native-duplicate.blend')
    yield
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / 'native-duplicate.png'))
    print('NATIVE_NURBS_DUPLICATE_UI_ARTIFACTS', output)


def extrude_delete_and_continue_source_components():
    """Exercise the existing E/G macro and Delete menu on authoritative native sources."""
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
    output = Path(os.environ.get('BLENDER_NURBS_EXTRUSION_OUTPUT', '/tmp/blender-nurbs-native-extrusion-ui'))
    output.mkdir(parents=True, exist_ok=True)
    space.overlay.show_stats = True

    def reset(name='Native preview sheet'):
        with bpy.context.temp_override(window=window, area=area, region=region):
            if window.view_layer.objects.active is not None and window.view_layer.objects.active.mode == 'EDIT':
                bpy.ops.object.mode_set(mode='OBJECT')
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        for curve in tuple(bpy.data.curves):
            if curve.users == 0:
                bpy.data.curves.remove(curve)
        with bpy.data.libraries.load(os.environ['BLENDER_NURBS_EXTRUSION_FIXTURE'], link=False) as (_, data):
            data.curves = [name]
        obj = bpy.data.objects.new('Native extrusion fixture', data.curves[0])
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.curve.select_all(action='DESELECT')
        space.shading.show_xray = True
        space.region_3d.view_location = (0, 0, 0)
        space.region_3d.view_distance = 7

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def save(name):
        curve = window.view_layer.objects.active.data
        t.assertTrue(curve.is_native_nurbs)
        t.assertEqual(len(curve.splines), 0)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / name), check_existing=False)

    reset()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    # The same source face must yield identical topology from click, box, or linked selection.
    for gesture in ('click', 'box', 'linked'):
        reset()
        yield
        corners = [screen(point) for point in ((-1, -1, 0), (1, -1, 0), (-1, 1, 0), (1, 1, 0))]
        if gesture == 'box':
            lower = min(p[0] for p in corners) - 8, min(p[1] for p in corners) - 8
            upper = max(p[0] for p in corners) + 8, max(p[1] for p in corners) + 8
            yield e.b()
            yield from e.leftmouse.cursor_motion((lower, upper))
        else:
            yield e.cursor_position_set(*corners[0], move=True)
            yield e.leftmouse()
            if gesture == 'linked':
                yield e.ctrl.l()
            else:
                for corner in corners[1:]:
                    yield e.cursor_position_set(*corner, move=True)
                    yield e.shift.leftmouse()
        t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
        yield e.e().z().text('1').ret()
        t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
        save(f'extrude-{gesture}.blend')

    # Reproduce the reported sequence: two face extrusions, delete the selected top vertices,
    # then select and extrude the remaining open rim. X opens Blender's existing Delete menu.
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (4, 12, 4, 20, 1, 10))
    save('extrude-repeat.blend')
    yield e.x().ret()
    t.assertEqual(counts(), (0, 8, 0, 12, 0, 5))
    save('extrude-delete.blend')
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 12, 4, 20, 1, 10))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (0, 8, 0, 12, 0, 5))
    yield e.numpad_1()
    yield
    ends = [screen((-1, -1, 1)), screen((1, -1, 1))]
    lower = min(p[0] for p in ends) - 8, min(p[1] for p in ends) - 8
    upper = max(p[0] for p in ends) + 8, max(p[1] for p in ends) + 8
    yield e.b()
    yield from e.leftmouse.cursor_motion((lower, upper))
    t.assertEqual(counts(), (4, 8, 4, 12, 0, 5))
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (4, 12, 4, 20, 0, 9))
    save('extrude-delete-continue.blend')
    yield e.numpad_6().numpad_8().home()
    save('try-native-extrude-delete.blend')
    yield
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / 'native-extrude-delete-continue.png'))

    reset()
    yield e.numpad_7()
    yield e.a()
    yield e.shift.d().z().text('0.5').ret()
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (4, 12, 4, 16, 1, 7))
    save('duplicate-then-extrude.blend')

    reset()
    yield e.a()
    yield e.e().z().text('2').esc()
    t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
    save('extrude-cancel.blend')
    yield e.g().z().text('1').ret()
    save('extrude-cancel-then-move.blend')
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    save('extrude-undo.blend')
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
    save('extrude-redo.blend')

    reset('Native preview line')
    yield e.a()
    yield e.e().y().text('2').ret()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    save('extrude-line.blend')
    yield e.e().y().text('1').ret()
    t.assertEqual(counts(), (2, 6, 1, 7, 0, 2))
    save('extrude-line-repeat.blend')
    yield e.a()
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (6, 12, 7, 20, 2, 10))
    save('extrude-two-face-region.blend')

    reset('Native preview trimmed arc')
    yield e.a()
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    save('extrude-trimmed-arc.blend')
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (2, 6, 1, 7, 0, 2))
    save('extrude-trimmed-arc-repeat.blend')

    for name, selected, total in [('Native preview circle', (2, 3, 1), (4, 8, 4)),
                                   ('Native preview hole', (8, 8, 1), (16, 24, 10))]:
        reset(name)
        yield e.a()
        yield e.e().z().text('1').ret()
        t.assertEqual(counts(), (selected[0], total[0], selected[1], total[1], selected[2], total[2]))
        save('extrude-circle.blend' if name.endswith('circle') else 'extrude-hole.blend')

    # Use the native context enum via the existing Python operator, with the same kernel backend.
    reset()
    yield e.a()
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.delete(type='ONLY_FACE'), {'FINISHED'})
    yield
    t.assertEqual(counts(), (4, 4, 4, 4, 0, 0))
    save('delete-only-face.blend')
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.delete(type='EDGE'), {'FINISHED'})
    yield
    t.assertEqual(counts(), (0, 0, 0, 0, 0, 0))
    save('delete-edges.blend')
    reset()
    yield e.a()
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.delete(type='FACE'), {'FINISHED'})
    yield
    t.assertEqual(counts(), (0, 0, 0, 0, 0, 0))
    save('delete-faces.blend')
    print('NATIVE_NURBS_EXTRUSION_UI_ARTIFACTS', output)


def mixed_component_edits_reject_before_mutation():
    import os

    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
        data.curves = ['Native preview sheet']
    native = bpy.data.objects.new('Native mixed edit fixture', data.curves[0])
    legacy_curve = bpy.data.curves.new('Legacy mixed edit fixture', 'SURFACE')
    spline = legacy_curve.splines.new('NURBS')
    spline.points.add(1)
    spline.points[0].co = (-1, 0, 0, 1)
    spline.points[1].co = (1, 0, 0, 1)
    spline.order_u = 2
    spline.use_endpoint_u = True
    legacy = bpy.data.objects.new('Legacy mixed edit fixture', legacy_curve)
    for obj in (native, legacy):
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    # The existing mode-entry gate rejects the mixed session before component operators run.
    # Keep that gate; the operator-side checks also defend against unsupported context callers.
    for active in (legacy, native):
        window.view_layer.objects.active = active
        window.view_layer.update()
        yield e.tab()
        t.assertEqual(native.mode, 'OBJECT')
        t.assertEqual(legacy.mode, 'OBJECT')
        t.assertTrue(native.data.is_native_nurbs)
        t.assertEqual(len(native.data.splines), 0)
        t.assertEqual(len(legacy.data.splines), 1)
        t.assertEqual(len(legacy.data.splines[0].points), 2)
        t.assertEqual(tuple(legacy.data.splines[0].points[0].co), (-1, 0, 0, 1))
        t.assertEqual(tuple(legacy.data.splines[0].points[1].co), (1, 0, 0, 1))
        with bpy.context.temp_override(window=window, area=area, region=region):
            t.assertFalse(bpy.ops.curve.extrude.poll())
            t.assertFalse(bpy.ops.curve.duplicate.poll())
            t.assertFalse(bpy.ops.curve.delete.poll())


def component_selection_modes():
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
    output = Path(os.environ.get('BLENDER_NURBS_COMPONENT_OUTPUT', '/tmp/blender-nurbs-component-ui'))
    output.mkdir(parents=True, exist_ok=True)
    mesh_mode = tuple(window.scene.tool_settings.mesh_select_mode)

    def reset(name='Native preview sheet'):
        with bpy.context.temp_override(window=window, area=area, region=region):
            if window.view_layer.objects.active is not None and window.view_layer.objects.active.mode == 'EDIT':
                bpy.ops.object.mode_set(mode='OBJECT')
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        for curve in tuple(bpy.data.curves):
            if curve.users == 0:
                bpy.data.curves.remove(curve)
        with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
            data.curves = [name]
        obj = bpy.data.objects.new('Native component fixture', data.curves[0])
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.curve.select_all(action='DESELECT')
        space.shading.show_xray = True
        space.overlay.show_stats = True
        space.region_3d.view_location = (0, 0, 0)
        space.region_3d.view_distance = 6

    def mode():
        return tuple(window.view_layer.objects.active.data.nurbs_selection_mode)

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def save(name):
        curve = window.view_layer.objects.active.data
        t.assertTrue(curve.is_native_nurbs)
        t.assertEqual(len(curve.splines), 0)
        t.assertEqual(tuple(window.scene.tool_settings.mesh_select_mode), mesh_mode)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / name), check_existing=False)

    reset()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    t.assertEqual(mode(), (True, False, False))
    yield e.two()
    t.assertEqual(mode(), (False, True, False))
    yield e.a()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.alt.a()
    yield e.cursor_position_set(*screen((0, -1, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.view3d.snap_cursor_to_active(), {'FINISHED'})
    t.assertEqual(tuple(window.scene.cursor.location), (0, -1, 0))
    window.scene.cursor.location = (0, 0, 0)
    save('edge-click.blend')
    yield e.e().z().text('1').ret()
    t.assertEqual(mode(), (False, True, False))
    t.assertEqual(counts(), (2, 6, 1, 7, 0, 2))
    save('edge-extrude.blend')
    yield e.ctrl.z()
    t.assertEqual(mode(), (False, True, False))
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (2, 6, 1, 7, 0, 2))

    reset()
    yield e.three()
    t.assertEqual(mode(), (False, False, True))
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.view3d.snap_cursor_to_active(), {'FINISHED'})
    t.assertEqual(tuple(window.scene.cursor.location), (0, 0, 0))
    save('face-click.blend')
    yield e.e().z().text('1').ret()
    t.assertEqual(mode(), (False, False, True))
    t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
    save('face-extrude.blend')
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (4, 12, 4, 20, 1, 10))
    save('face-extrude-repeat.blend')
    yield e.one()
    t.assertEqual(mode(), (True, False, False))
    t.assertEqual(counts(), (4, 12, 4, 20, 1, 10))
    yield e.ctrl.z()
    t.assertEqual(mode(), (False, False, True))
    yield e.ctrl.shift.z()
    t.assertEqual(mode(), (True, False, False))
    yield e.three()
    yield e.shift.d().x().text('3').ret()
    t.assertEqual(mode(), (False, False, True))
    t.assertEqual(counts(), (4, 16, 4, 24, 1, 11))
    save('face-duplicate.blend')

    reset()
    yield e.two()
    lower = screen((-1.1, -1.1, 0))
    upper = screen((1.1, -0.9, 0))
    yield e.b()
    yield from e.leftmouse.cursor_motion((lower, upper))
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    save('edge-box.blend')
    yield e.e().z().text('1').ret()
    save('edge-box-extrude.blend')

    reset()
    yield e.three()
    center = screen((0, 0, 0))
    yield e.b()
    yield from e.leftmouse.cursor_motion(((center[0] - 8, center[1] - 8), (center[0] + 8, center[1] + 8)))
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    save('face-box.blend')
    yield e.e().z().text('1').ret()
    save('face-box-extrude.blend')

    reset()
    yield e.three()
    yield e.cursor_position_set(*screen((0.4, 0.2, 0)), move=True)
    yield e.l()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    save('face-linked.blend')
    yield e.e().z().text('1').ret()
    save('face-linked-extrude.blend')
    yield e.shift.three()
    # Toggling the last enabled domain keeps a valid mode.
    t.assertEqual(mode(), (False, False, True))
    yield e.shift.two()
    t.assertEqual(mode(), (False, True, True))
    yield e.shift.one()
    t.assertEqual(mode(), (True, True, True))
    yield e.three()
    t.assertEqual(mode(), (False, False, True))
    yield e.numpad_6().numpad_8().home()
    save('try-native-component-modes.blend')
    yield
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / 'native-component-modes.png'))

    # Face mode must not expand two opposite cube faces merely because their closure
    # includes every vertex. Vertex-mode flushing here would incorrectly select all six faces.
    reset()
    yield e.numpad_7()
    yield e.a()
    yield e.e().z().text('1').ret()
    yield e.three()
    yield e.ctrl.numpad_7()
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.shift.leftmouse()
    t.assertEqual(counts(), (8, 8, 8, 12, 2, 6))
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (8, 16, 8, 28, 2, 14))
    save('face-opposite-region.blend')

    reset('Native preview hole')
    yield e.numpad_7()
    yield e.three()
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (0, 8, 0, 8, 0, 1))
    yield e.cursor_position_set(*screen((0, -0.75, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (8, 8, 8, 8, 1, 1))
    yield e.ctrl.l()
    t.assertEqual(counts(), (8, 8, 8, 8, 1, 1))
    save('face-hole.blend')
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (8, 16, 8, 24, 1, 10))
    save('face-hole-extrude.blend')
    print('NATIVE_NURBS_COMPONENT_MODE_UI_ARTIFACTS', output)


def component_selection_visibility():
    import os
    import re

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
        data.curves = ['Native preview sheet']
    obj = bpy.data.objects.new('Native visibility fixture', data.curves[0])
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    window.view_layer.update()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    yield e.numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 6
    space.shading.show_xray = False
    yield

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    yield e.cursor_position_set(*screen((-1, -1, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (1, 4, 0, 4, 0, 1))
    yield e.alt.a()
    yield e.two()
    yield e.cursor_position_set(*screen((0, -1, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.alt.a()
    yield e.three()
    center = screen((0, 0, 0))
    yield e.cursor_position_set(*center, move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.alt.a()
    # Without X-Ray, face box selection uses visible face coverage, as mesh depth selection does.
    corner = screen((-0.9, -0.9, 0))
    yield e.b()
    yield from e.leftmouse.cursor_motion(((corner[0] - 6, corner[1] - 6), (corner[0] + 6, corner[1] + 6)))
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.alt.a()

    # This ordinary mesh is only a foreign scene occluder, never a NURBS source substitute.
    mesh = bpy.data.meshes.new('Visibility occluder')
    mesh.from_pydata([(-2, -2, 0.5), (2, -2, 0.5), (2, 2, 0.5), (-2, 2, 0.5)], [], [(0, 1, 2, 3)])
    occluder = bpy.data.objects.new('Visibility occluder', mesh)
    window.scene.collection.objects.link(occluder)
    window.view_layer.update()
    yield
    yield e.cursor_position_set(*center, move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    yield e.b()
    yield from e.leftmouse.cursor_motion(((center[0] - 8, center[1] - 8), (center[0] + 8, center[1] + 8)))
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    yield e.two()
    yield e.cursor_position_set(*screen((0, -1, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    yield e.one()
    yield e.cursor_position_set(*screen((-1, -1, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (0, 4, 0, 4, 0, 1))
    space.shading.show_xray = True
    yield e.leftmouse()
    t.assertEqual(counts(), (1, 4, 0, 4, 0, 1))
    yield e.alt.a()
    yield e.three()
    yield e.cursor_position_set(*center, move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertEqual(len(window.view_layer.objects.active.data.splines), 0)


def component_modes_across_objects():
    import os
    import re

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
        data.curves = ['Native preview sheet']
    objects = []
    for index, x in enumerate((-2, 2)):
        curve = data.curves[0] if index == 0 else data.curves[0].copy()
        obj = bpy.data.objects.new(f'Native mode owner {index}', curve)
        window.scene.collection.objects.link(obj)
        obj.location.x = x
        objects.append(obj)
    first, second = objects
    second.select_set(True)
    window.view_layer.objects.active = second
    window.view_layer.update()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    yield e.three()
    yield e.tab()
    t.assertEqual(tuple(second.data.nurbs_selection_mode), (False, False, True))
    t.assertEqual(tuple(first.data.nurbs_selection_mode), (True, False, False))
    first.select_set(True)
    window.view_layer.objects.active = first
    window.view_layer.update()
    yield e.tab()

    def modes(expected):
        for obj in window.view_layer.objects:
            t.assertEqual(obj.mode, 'EDIT')
            t.assertEqual(tuple(obj.data.nurbs_selection_mode), expected)
            t.assertEqual(len(obj.data.splines), 0)

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    # Stored owner modes may differ in Object Mode; entering a shared session uses the active one.
    modes((True, False, False))
    yield e.two()
    modes((False, True, False))
    yield e.three()
    modes((False, False, True))
    space = area.spaces.active
    space.shading.show_xray = True
    yield e.numpad_7()
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_distance = 10
    yield
    left = location_3d_to_region_2d(region, space.region_3d, Vector((-2, 0, 0)))
    right = location_3d_to_region_2d(region, space.region_3d, Vector((2, 0, 0)))
    yield e.cursor_position_set(round(left.x) + region.x, round(left.y) + region.y, move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    active_name = window.view_layer.objects.active.name
    yield e.cursor_position_set(round(right.x) + region.x, round(right.y) + region.y, move=True)
    yield e.l()
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    t.assertEqual(window.view_layer.objects.active.name, active_name)
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
    modes((False, False, True))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    modes((False, False, True))
    yield e.shift.two()
    modes((False, True, True))
    yield e.three()
    modes((False, False, True))
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.delete('EXEC_DEFAULT', True, type='FACE'), {'FINISHED'})
    yield
    t.assertEqual(counts(), (0, 0, 0, 0, 0, 0))
    modes((False, False, True))
    yield e.ctrl.z()
    t.assertEqual(counts(), (8, 8, 8, 8, 2, 2))
    modes((False, False, True))


def component_fill():
    """F constructs native source; UI events, undo and save keep the same topology."""
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
    output = Path(os.environ.get('BLENDER_NURBS_FILL_OUTPUT', '/tmp/blender-nurbs-fill-ui'))
    output.mkdir(parents=True, exist_ok=True)

    def reset():
        with bpy.context.temp_override(window=window, area=area, region=region):
            active = window.view_layer.objects.active
            if active is not None and active.mode == 'EDIT':
                bpy.ops.object.mode_set(mode='OBJECT')
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        for curve in tuple(bpy.data.curves):
            if curve.users == 0:
                bpy.data.curves.remove(curve)
        with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
            data.curves = ['Native preview sheet']
        obj = bpy.data.objects.new('Native Fill fixture', data.curves[0])
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.curve.select_mode(type='VERT')
            bpy.ops.curve.select_all(action='DESELECT')
        space.shading.show_xray = True
        space.overlay.show_stats = True
        space.region_3d.view_location = (0, 0, 0)
        space.region_3d.view_distance = 7

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(int(value) for value in match.groups())

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def save(name):
        obj = window.view_layer.objects.active
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
        with bpy.context.temp_override(window=window, area=area, region=region):
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh, 'Native source preview failed')
                t.assertGreater(len(mesh.edges), 0)
            finally:
                evaluated.to_mesh_clear()
            bpy.ops.wm.save_as_mainfile(filepath=str(output / name), check_existing=False)

    def delete(kind):
        with bpy.context.temp_override(window=window, area=area, region=region):
            t.assertEqual(bpy.ops.curve.delete(type=kind), {'FINISHED'})

    reset()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.numpad_7()
    yield e.three()
    yield e.a()
    delete('ONLY_FACE')
    yield e.two()
    yield e.a()
    t.assertEqual(counts(), (4, 4, 4, 4, 0, 0))
    yield e.f()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    save('fill-planar-edge.blend')
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 4, 4, 4, 0, 0))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.three()
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
    save('fill-planar-extrude.blend')

    reset()
    yield e.cursor_position_set(*screen((1, 1, 0)), move=True)
    yield e.leftmouse()
    yield e.g().z().text('1').ret()
    yield e.three()
    yield e.a()
    delete('ONLY_FACE')
    yield e.two()
    yield e.a()
    yield e.f()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    save('fill-bent-edge.blend')
    yield e.three()
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
    save('fill-bent-extrude.blend')
    yield e.numpad_1()
    yield e.numpad_6()
    yield e.numpad_8()
    space.shading.show_xray = False
    yield
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.screen.screenshot(filepath=str(output / 'fill-bent-extrude.png'))

    reset()
    yield e.numpad_7()
    yield e.cursor_position_set(*screen((-1, -1, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((1, 1, 0)), move=True)
    yield e.shift.leftmouse()
    delete('VERT')
    yield e.a()
    t.assertEqual(counts(), (2, 2, 0, 0, 0, 0))
    yield e.f()
    t.assertEqual(counts(), (2, 2, 1, 1, 0, 0))
    save('fill-connect.blend')
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    save('fill-connect-extrude.blend')

    reset()
    yield e.cursor_position_set(*screen((1, 1, 0)), move=True)
    yield e.leftmouse()
    delete('VERT')
    yield e.a()
    t.assertEqual(counts(), (3, 3, 2, 2, 0, 0))
    yield e.f()
    t.assertEqual(counts(), (3, 3, 3, 3, 1, 1))
    save('fill-triangle.blend')
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (3, 6, 3, 9, 1, 5))
    save('fill-triangle-extrude.blend')

    reset()
    yield e.two()
    yield e.cursor_position_set(*screen((1, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.e().x().text('2').ret()
    t.assertEqual(counts(), (2, 6, 1, 7, 0, 2))
    yield e.three()
    yield e.a()
    yield e.f()
    t.assertEqual(counts(), (6, 6, 6, 6, 1, 1))
    save('fill-merge.blend')
    yield e.ctrl.z()
    t.assertEqual(counts(), (6, 6, 7, 7, 2, 2))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (6, 6, 6, 6, 1, 1))
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (6, 12, 6, 18, 1, 8))
    save('fill-merge-extrude.blend')
    yield e.tab()


def two_edge_fill_duplicate_and_extrude():
    """The native editor shares the same two-edge Fill geometry as the compatibility adapter."""
    import os
    import re
    from pathlib import Path
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    fixture = os.environ['BLENDER_NURBS_EXTRUSION_FIXTURE']
    with bpy.data.libraries.load(fixture, link=False) as (_, data):
        data.curves = ['Native preview line']
    obj = bpy.data.objects.new('Native two-edge Fill', data.curves[0])
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    window.view_layer.update()
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield e.tab()
    yield e.a()
    yield e.shift.d().y().text('2').ret()
    yield e.a()
    yield e.two()

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    t.assertEqual(counts(), (4, 4, 2, 2, 0, 0))
    yield e.f()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.ctrl.z()
    t.assertEqual(counts(), (4, 4, 2, 2, 0, 0))
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    yield e.three()
    yield e.shift.d().z().text('2').ret()
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (4, 12, 4, 16, 1, 7))
    obj = window.view_layer.objects.active
    t.assertTrue(obj.data.is_native_nurbs)
    t.assertEqual(len(obj.data.splines), 0)
    output = Path('/tmp/blender-nurbs-two-edge-fill-ui')
    output.mkdir(parents=True, exist_ok=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'native-fill-duplicate-extrude.blend'),
                                  check_existing=False)
    yield e.tab()


def component_modes_from_normal_add():
    """Normal Add exposes real modes, including undo across exact source migration."""
    import re
    from pathlib import Path
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    header = next(r for r in area.regions if r.type == 'HEADER')
    space = area.spaces.active
    output = Path('/tmp/blender-nurbs-normal-mode-ui')
    output.mkdir(parents=True, exist_ok=True)
    mesh_mode = tuple(window.scene.tool_settings.mesh_select_mode)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.numpad_7()
    space.region_3d.view_location = (0, 1, 0)
    space.region_3d.view_distance = 6
    space.shading.show_xray = True
    space.overlay.show_stats = True
    yield

    def curve():
        return window.view_layer.objects.active.data

    def mode():
        return tuple(curve().nurbs_selection_mode)

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def capture(label):
        t.assertEqual(tuple(window.scene.tool_settings.mesh_select_mode), mesh_mode)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / (label + '.png')))

    t.assertTrue(curve().has_tagged_nurbs)
    t.assertFalse(curve().is_native_nurbs)
    capture('buttons-before-entry')
    yield e.cursor_position_set(header.x + 177, header.y + header.height // 2, move=True)
    yield e.leftmouse()
    t.assertTrue(curve().is_native_nurbs)
    t.assertEqual(mode(), (False, True, False))
    t.assertEqual(len(curve().splines), 0)
    t.assertEqual(counts(), (2, 2, 1, 1, 0, 0))
    capture('line-edge-mode')
    yield e.ctrl.z()
    t.assertFalse(curve().is_native_nurbs)
    t.assertTrue(curve().has_tagged_nurbs)
    t.assertEqual(len(curve().splines), 1)
    yield e.ctrl.shift.z()
    t.assertTrue(curve().is_native_nurbs)
    t.assertEqual(mode(), (False, True, False))
    t.assertEqual(counts(), (2, 2, 1, 1, 0, 0))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.e().y().text('2').ret()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    yield e.three()
    yield e.alt.a()
    yield e.cursor_position_set(*screen((0, 1, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
    capture('face-mode')
    yield e.two()
    yield e.alt.a()
    yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
    capture('edge-mode')
    yield e.one()
    yield e.alt.a()
    yield e.cursor_position_set(*screen((-1, 0, 0)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts(), (1, 4, 0, 4, 0, 1))
    capture('vertex-mode')
    # Click the same header button positions used by the native mode controls.
    for x, expected in ((177, (False, True, False)), (197, (False, False, True)),
                        (155, (True, False, False))):
        yield e.cursor_position_set(header.x + x, header.y + header.height // 2, move=True)
        yield e.leftmouse()
        t.assertEqual(mode(), expected)
    yield e.cursor_position_set(header.x + 177, header.y + header.height // 2, move=True)
    # Keep Shift held while the deferred UI button action reads the window event state.
    e.shift.press()
    yield
    yield e.leftmouse()
    e.shift.release()
    yield
    t.assertEqual(mode(), (True, True, False))
    yield e.cursor_position_set(header.x + 197, header.y + header.height // 2, move=True)
    e.shift.press()
    yield
    yield e.leftmouse()
    e.shift.release()
    yield
    t.assertEqual(mode(), (True, True, True))
    yield e.cursor_position_set(header.x + 155, header.y + header.height // 2, move=True)
    yield e.leftmouse()
    t.assertEqual(mode(), (True, False, False))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.three()
    yield e.a()
    yield e.e().z().text('1').ret()
    t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'normal-add-modes.blend'), check_existing=False)
    yield e.tab()
    yield e.tab()
    t.assertEqual(mode(), (False, False, True))
    t.assertEqual(counts(), (4, 8, 4, 12, 1, 6))
    yield e.tab()


def component_modes_on_existing_surface():
    """A model made with the ordinary legacy tools enters the same source mode pipeline."""
    import re
    from pathlib import Path
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    output = Path('/tmp/blender-nurbs-normal-mode-ui')
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.e().y().text('2').ret()
    yield e.a()
    yield e.e().z().text('1').ret()
    yield e.a()
    yield e.r().z().text('23').ret()
    yield e.s().text('1.25').ret()
    obj = window.view_layer.objects.active
    obj.update_from_editmode()
    t.assertFalse(obj.data.is_native_nurbs)
    t.assertEqual(len(obj.data.splines), 6)
    before = [tuple(tuple(point.co) for point in spline.points) for spline in obj.data.splines]
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'existing-before.blend'), check_existing=False)
    def display_faces():
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh, "Source transition lost the NURBS preview")
                return len(mesh.polygons)
            finally:
                evaluated.to_mesh_clear()

    t.assertGreater(display_faces(), 0)
    yield e.three()
    obj = window.view_layer.objects.active
    t.assertTrue(obj.data.is_native_nurbs)
    t.assertEqual(tuple(obj.data.nurbs_selection_mode), (False, False, True))
    stats = window.scene.statistics(window.view_layer)
    match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
    t.assertIsNotNone(match, stats)
    t.assertEqual(tuple(map(int, match.groups())), (8, 8, 12, 12, 6, 6))
    t.assertGreater(display_faces(), 0)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'existing-after.blend'), check_existing=False)
    yield e.ctrl.z()
    obj = window.view_layer.objects.active
    obj.update_from_editmode()
    t.assertFalse(obj.data.is_native_nurbs)
    t.assertEqual([tuple(tuple(point.co) for point in spline.points) for spline in obj.data.splines], before)
    t.assertGreater(display_faces(), 0)
    yield e.ctrl.shift.z()
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertGreater(display_faces(), 0)
    yield e.tab()
    yield e.ctrl.z()
    t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertGreater(display_faces(), 0)
    yield e.ctrl.z()
    t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertGreater(display_faces(), 0)
    yield e.ctrl.shift.z()
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertGreater(display_faces(), 0)
    yield e.ctrl.shift.z()
    t.assertEqual(window.view_layer.objects.active.mode, 'OBJECT')


def component_mode_entry_preflight():
    """A failed entry preserves source, including modifiers on another user of its data."""
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    obj = window.view_layer.objects.active
    linked = bpy.data.objects.new('Other source user', obj.data)
    window.scene.collection.objects.link(linked)
    modifier = linked.modifiers.new('Preserved hook', 'HOOK')
    yield e.tab()
    curve = obj.data
    before = [tuple(p.co) for s in curve.splines for p in s.points]
    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            result = bpy.ops.curve.select_mode(type='EDGE')
        except RuntimeError as error:
            t.assertIn('modifiers', str(error))
        else:
            t.assertEqual(result, {'CANCELLED'})
    t.assertFalse(curve.is_native_nurbs)
    t.assertEqual([tuple(p.co) for s in curve.splines for p in s.points], before)
    t.assertEqual(len(linked.modifiers), 1)
    linked.modifiers.remove(modifier)
    point = curve.splines[0].points[0]
    point.radius = 2
    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            result = bpy.ops.curve.select_mode(type='FACE')
        except RuntimeError as error:
            t.assertIn('radii', str(error))
        else:
            t.assertEqual(result, {'CANCELLED'})
    t.assertFalse(curve.is_native_nurbs)
    t.assertEqual(point.radius, 2)
    point.radius = 1
    yield e.two()
    t.assertTrue(curve.is_native_nurbs)
    t.assertTrue(linked.data.is_native_nurbs)
    t.assertEqual(tuple(curve.nurbs_selection_mode), (False, True, False))
    yield e.ctrl.z()
    t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
    yield e.ctrl.shift.z()
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    yield e.tab()


def rotate_scale_and_lifecycle():
    """Normal Add reaches shared R/S, numeric constraints, modes and source undo."""
    import re
    from pathlib import Path
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/blender-nurbs-rotate-scale-ui')
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.two()
    yield e.e().y().text('2').ret()
    yield e.one()
    yield e.a()
    yield e.numpad_7()
    space.region_3d.view_location = (0, 1, 0)
    space.region_3d.view_distance = 12
    space.shading.show_xray = True
    space.overlay.show_stats = True
    window.scene.tool_settings.transform_pivot_point = 'MEDIAN_POINT'
    window.scene.transform_orientation_slots[0].type = 'GLOBAL'
    yield

    def save(label):
        obj = window.view_layer.objects.active
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        t.assertEqual(tuple(map(int, match.groups()))[1::2], (4, 4, 1))
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh, 'NURBS transform preview failed')
                t.assertGreater(len(mesh.polygons), 0)
            finally:
                evaluated.to_mesh_clear()
            bpy.ops.wm.save_as_mainfile(filepath=str(output / (label + '.blend')), check_existing=False)

    def preview_bounds():
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = window.view_layer.objects.active
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh)
                return tuple((min(v.co[axis] for v in mesh.vertices),
                              max(v.co[axis] for v in mesh.vertices)) for axis in range(3))
            finally:
                evaluated.to_mesh_clear()

    def screen(position):
        point = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(point)
        return round(point.x) + region.x, round(point.y) + region.y

    save('start')
    yield e.s().text('0').ret()
    save('scale-zero')
    yield e.ctrl.z()
    save('scale-zero-undo')
    yield e.s().x().text('-1').ret()
    save('scale-reflection')
    yield e.ctrl.z()
    yield e.r().z().text('90').ret()
    save('rotate-90')
    yield e.s().x().text('2').ret()
    # Open the standard F9 Scale controls while Scale is still the active operation.
    t.assertEqual(bpy.context.active_operator.bl_idname, 'TRANSFORM_OT_resize')
    yield e.f9()
    yield
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / 'scale-f9.png'))
    yield e.cursor_position_set(540, 270, move=True)
    yield e.leftmouse().ctrl.a().text('3').ret()
    yield e.esc()
    yield
    t.assertEqual(tuple(bpy.context.active_operator.value), (3, 1, 1))
    save('scale-f9-3')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.f9()
    yield
    yield e.cursor_position_set(540, 270, move=True)
    yield e.leftmouse().ctrl.a().text('2').ret()
    yield e.esc()
    yield
    t.assertEqual(tuple(bpy.context.active_operator.value), (2, 1, 1))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    save('scale-2')
    yield e.ctrl.z()
    save('scale-undo')
    yield e.ctrl.shift.z()
    save('scale-redo')
    yield e.r().y().text('45').esc()
    yield e.s().z().text('0.25').esc()
    bounds = preview_bounds()
    for transform in ('r', 's'):
        yield e.cursor_position_set(500, 350, move=True)
        yield getattr(e, transform)()
        yield e.cursor_position_set(550, 430, move=True)
        yield
        t.assertNotEqual(preview_bounds(), bounds, 'Mouse transform did not update source preview')
        yield e.esc()
        t.assertEqual(preview_bounds(), bounds)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    save('cancel')
    yield e.g().r().z().text('90').ret()
    save('switch-gr')
    yield e.r().s().x().text('0.5').ret()
    save('switch-rs')
    yield e.three()
    yield e.a()
    window.scene.tool_settings.transform_pivot_point = 'CURSOR'
    window.scene.cursor.location = (0, 0, 0)
    yield e.r().x().text('90').ret()
    yield e.s().z().text('2').ret()
    yield e.numpad_1()
    save('face-rs')
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / 'face-transformed.png'))
    yield e.one()
    yield e.alt.a()
    corner = screen((-0.5, 0, -2))
    yield e.cursor_position_set(*corner, move=True)
    yield e.leftmouse()
    yield e.r().y().text('90').ret()
    save('vertex-rotate')
    yield e.s().x().text('2').ret()
    save('vertex-scale')
    yield e.ctrl.z()
    yield e.ctrl.z()
    yield e.alt.a()
    yield e.b()
    yield from e.leftmouse.cursor_motion(((corner[0] - 7, corner[1] - 7),
                                         (corner[0] + 7, corner[1] + 7)))
    yield e.r().y().text('90').ret()
    save('vertex-box-rotate')
    yield e.two()
    yield e.alt.a()
    yield e.cursor_position_set(*screen((0.5, 0, 2)), move=True)
    yield e.leftmouse()
    window.scene.tool_settings.transform_pivot_point = 'MEDIAN_POINT'
    yield e.s().z().text('0.5').ret()
    save('edge-scale')
    yield e.r().y().text('30').ret()
    save('edge-rotate')
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / 'edge-transformed.png'))
    yield e.tab()
    yield e.tab()
    save('final')
    yield e.rightmouse()
    yield
    with bpy.context.temp_override(window=window):
        bpy.ops.screen.screenshot(filepath=str(output / 'transform-context-menu.png'))
    yield e.esc()


def rotate_scale_mesh_parity():
    """Run the same constrained transforms on native NURBS and actual Blender meshes."""
    import json
    from pathlib import Path
    import bpy
    import bmesh
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Matrix, Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/blender-nurbs-rotate-scale-parity-ui')
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.two()
    yield e.e().y().text('2').ret()
    yield e.three()
    yield e.a()
    yield e.shift.d().x().text('4').ret()
    yield e.a()
    yield e.tab()
    base = window.view_layer.objects.active
    base_curve = base.data
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'start.blend'), check_existing=False)
    bpy.data.objects.remove(base, do_unlink=True)
    coords = [(-1, 0, 0), (1, 0, 0), (1, 2, 0), (-1, 2, 0),
              (3, 0, 0), (5, 0, 0), (5, 2, 0), (3, 2, 0)]
    owner_matrix = (Matrix.Translation((2, -1, 3)) @ Matrix.Rotation(0.4, 4, 'Z') @
                    Matrix.Diagonal((1.5, 0.75, 2, 1)))
    space.shading.show_xray = True
    space.overlay.show_stats = True
    space.region_3d.view_location = owner_matrix @ Vector((2, 1, 0))
    space.region_3d.view_distance = 20
    window.scene.cursor.location = (1.25, 2.5, 0.25)
    yield e.numpad_7()
    results = {'initial': coords, 'cases': []}
    cases = [('individual-vertex', 1, 'INDIVIDUAL_ORIGINS', False),
             ('individual-edge', 2, 'INDIVIDUAL_ORIGINS', False),
             ('individual-face', 3, 'INDIVIDUAL_ORIGINS', False),
             ('bounds', 1, 'BOUNDING_BOX_CENTER', True),
             ('active', 1, 'ACTIVE_ELEMENT', True),
             ('cursor', 1, 'CURSOR', True)]

    def screen(position):
        point = location_3d_to_region_2d(region, space.region_3d, owner_matrix @ Vector(position))
        t.assertIsNotNone(point)
        return round(point.x) + region.x, round(point.y) + region.y

    for label, mode, pivot, partial in cases:
        window.scene.tool_settings.transform_pivot_point = pivot
        window.scene.transform_orientation_slots[0].type = 'GLOBAL'
        native = bpy.data.objects.new('Native ' + label, base_curve.copy())
        window.scene.collection.objects.link(native)
        native.matrix_world = owner_matrix
        native.select_set(True)
        window.view_layer.objects.active = native
        window.view_layer.update()
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
        yield getattr(e, ('one', 'two', 'three')[mode - 1])()
        if partial:
            yield e.alt.a()
            for index in range(3):
                yield e.cursor_position_set(*screen(coords[index]), move=True)
                yield e.shift.leftmouse() if index else e.leftmouse()
        else:
            yield e.a()
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.r().z().text('90').ret()
        yield e.s().x().text('2').ret()
        yield e.s().y().y().text('0.5').ret()
        t.assertTrue(native.data.is_native_nurbs)
        t.assertEqual(len(native.data.splines), 0)
        with bpy.context.temp_override(window=window, area=area, region=region):
            evaluated = native.evaluated_get(bpy.context.evaluated_depsgraph_get())
            display = evaluated.to_mesh()
            try:
                t.assertIsNotNone(display)
                t.assertGreater(len(display.polygons), 0)
            finally:
                evaluated.to_mesh_clear()
            bpy.ops.wm.save_as_mainfile(filepath=str(output / (label + '.blend')), check_existing=False)
            if label == 'individual-face':
                bpy.ops.screen.screenshot(filepath=str(output / 'individual-faces.png'))
        yield e.tab()
        native.select_set(False)
        mesh = bpy.data.meshes.new('Mesh reference')
        mesh.from_pydata(coords, [], [(0, 1, 2, 3), (4, 5, 6, 7)])
        reference = bpy.data.objects.new('Actual mesh ' + label, mesh)
        reference.matrix_world = owner_matrix
        window.scene.collection.objects.link(reference)
        reference.select_set(True)
        window.view_layer.objects.active = reference
        window.scene.tool_settings.mesh_select_mode = tuple(mode == i for i in (1, 2, 3))
        yield e.tab()
        if partial:
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.mesh.select_all(action='DESELECT')
            bm = bmesh.from_edit_mesh(mesh)
            bm.verts.ensure_lookup_table()
            for index in range(3):
                bm.verts[index].select_set(True)
            bm.select_history.clear()
            bm.select_history.add(bm.verts[2])
            bm.select_flush_mode()
            bmesh.update_edit_mesh(mesh)
        else:
            yield e.a()
        yield e.r().z().text('90').ret()
        yield e.s().x().text('2').ret()
        yield e.s().y().y().text('0.5').ret()
        reference.update_from_editmode()
        results['cases'].append({'label': label, 'mode': mode,
                                  'positions': [list(v.co) for v in mesh.vertices]})
        yield e.tab()
        bpy.data.objects.remove(reference, do_unlink=True)
        bpy.data.objects.remove(native, do_unlink=True)
    (output / 'mesh-reference.json').write_text(json.dumps(results, indent=2))
    with (output / 'mesh-reference.txt').open('w') as stream:
        for case in results['cases']:
            stream.write(f"{case['label']} {case['mode']} {len(coords)}\n")
            for initial, final in zip(coords, case['positions']):
                stream.write(' '.join(map(repr, (*initial, *final))) + '\n')


def rotate_scale_fresh_continuation():
    """Reopen the transformed authoritative source in a fresh process and continue extrusion."""
    from pathlib import Path
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    output = Path('/tmp/blender-nurbs-rotate-scale-ui')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(str(output / 'final.blend'), link=False) as (source, target):
        t.assertEqual(len(source.curves), 1)
        target.curves = source.curves[:]
    obj = bpy.data.objects.new('Reopened transformed NURBS', target.curves[0])
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    window.view_layer.update()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    t.assertTrue(obj.data.is_native_nurbs)
    t.assertEqual(tuple(obj.data.nurbs_selection_mode), (False, True, False))
    yield e.e().y().text('1').ret()
    stats = window.scene.statistics(window.view_layer)
    t.assertIn('Verts:2/6 | Edges:1/7 | Faces:0/2', stats)
    def save(label):
        window.view_layer.update()
        with bpy.context.temp_override(window=window, area=area, region=region):
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh)
                t.assertGreater(len(mesh.polygons), 0)
            finally:
                evaluated.to_mesh_clear()
            bpy.ops.wm.save_as_mainfile(filepath=str(output / (label + '.blend')), check_existing=False)

    save('fresh-extruded')
    yield e.s().x().text('0.5').ret()
    save('fresh-scaled')


def snapping_and_extrusion():
    """Exercise E with the same vertex snapping settings as the duplicate-face report."""
    from pathlib import Path
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/blender-nurbs-snapping-ui')
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.numpad_7()
    space.region_3d.view_location = (1, 1, 0)
    space.region_3d.view_distance = 9
    space.shading.show_xray = True
    space.overlay.show_stats = True
    yield e.two()
    yield e.e().y().text('2').ret()
    yield e.three()
    yield e.a()
    yield e.shift.d().x().text('3').ret()
    settings = window.scene.tool_settings
    settings.snap_elements = {'VERTEX'}
    settings.snap_target = 'ACTIVE'
    settings.use_snap = True
    print('NURBS_SNAP_REPRO_BEFORE_E', window.scene.statistics(window.view_layer))
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'before-extrude.blend'), check_existing=False)
    yield e.e().z().text('1').ret()
    print('NURBS_SNAP_REPRO_AFTER_E', window.scene.statistics(window.view_layer))
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'after-extrude.blend'), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / 'after-extrude.png'))
        obj = window.view_layer.objects.active
        window.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            t.assertIsNotNone(mesh)
            t.assertAlmostEqual(max(v.co.z for v in mesh.vertices), 1.0, places=6,
                                msg='E must move the face even with vertex snapping enabled')
        finally:
            evaluated.to_mesh_clear()


def vertex_snap_then_extrude():
    """Run the reported workflow with real viewport events for both mesh and NURBS."""
    import re
    from pathlib import Path
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/blender-nurbs-snapping-ui')
    output.mkdir(parents=True, exist_ok=True)

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def screen(position):
        obj = window.view_layer.objects.active
        co = location_3d_to_region_2d(region, space.region_3d, obj.matrix_world @ Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def active_center(expected):
        with bpy.context.temp_override(window=window, area=area, region=region):
            t.assertEqual(bpy.ops.view3d.snap_cursor_to_active(), {'FINISHED'})
        for actual, value in zip(window.scene.cursor.location, expected):
            t.assertAlmostEqual(actual, value, places=5)

    for kind in ('mesh', 'nurbs'):
        with bpy.context.temp_override(window=window, area=area, region=region):
            if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
                bpy.ops.object.mode_set(mode='OBJECT')
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        settings = window.scene.tool_settings
        settings.use_snap = False
        window.scene.cursor.location = (0, 0, 0)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        if kind == 'nurbs':
            yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        else:
            data = bpy.data.meshes.new('Mesh snapping reference')
            data.from_pydata([(-1, 0, 0), (1, 0, 0)], [(0, 1)], [])
            obj = bpy.data.objects.new('Mesh snapping reference', data)
            window.scene.collection.objects.link(obj)
            obj.select_set(True)
            window.view_layer.objects.active = obj
        yield e.tab()
        yield e.numpad_7()
        space.region_3d.view_location = (1, 2, 0)
        space.region_3d.view_distance = 9
        space.shading.show_xray = True
        space.overlay.show_stats = True
        yield e.two()
        yield e.a()
        yield e.e().y().text('2').ret()
        t.assertEqual(counts(), (2, 4, 1, 4, 0, 1))
        yield e.one()
        yield e.alt.a()
        yield e.cursor_position_set(*screen((-1, 0, 0)), move=True)
        yield e.leftmouse()
        yield e.ctrl.l()
        t.assertEqual(counts(), (4, 4, 4, 4, 1, 1))
        yield e.shift.d().x().text('3').ret()
        t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
        active_center((2, 0, 0))
        settings.snap_elements = {'VERTEX'}
        settings.snap_target = 'ACTIVE'
        settings.use_snap = True
        yield e.cursor_position_set(*screen((2, 0, 0)), move=True)
        yield e.g()
        yield e.cursor_position_set(*screen((1, 2, 0)), move=True)
        yield
        yield e.leftmouse()
        active_center((1, 2, 0))
        t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
        yield e.ctrl.z()
        active_center((2, 0, 0))
        yield e.ctrl.shift.z()
        active_center((1, 2, 0))
        yield e.g()
        yield e.cursor_position_set(*screen((-1, 0, 0)), move=True)
        yield
        yield e.esc()
        active_center((1, 2, 0))
        yield e.three()
        yield e.alt.a()
        yield e.cursor_position_set(*screen((2, 3, 0)), move=True)
        yield e.leftmouse()
        t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
        # Mesh E follows this wire-built face's -Z normal; curve E starts as a free move.
        if kind == 'mesh':
            yield e.e().text('-1').ret()
        else:
            yield e.e().z().text('1').ret()
        t.assertEqual(counts(), (4, 12, 4, 16, 1, 7))
        # Read the active source face, rather than relying on a tessellated display vertex.
        active_center((2, 3, 1))
        yield e.ctrl.z()
        t.assertEqual(counts(), (4, 8, 4, 8, 1, 2))
        yield e.ctrl.shift.z()
        active_center((2, 3, 1))
        space.shading.show_xray = False
        yield e.numpad_6().numpad_8().numpad_8().numpad_8()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / (kind + '-snap-extrude.blend')),
                                       check_existing=False)
            bpy.ops.screen.screenshot(filepath=str(output / (kind + '-snap-extrude.png')))
        print('SNAP_DUPLICATE_EXTRUDE_VIEWPORT_PASS', kind, counts())


def native_vertex_snap_sources(cases=('ACTIVE', 'CLOSEST', 'MEDIAN', 'CENTER', 'CURSOR',
                                     'axis', 'plane', 'precision', 'transformed',
                                     'transformed-closest', 'mesh-target')):
    """Source anchors, transformed owners and double coordinates must survive viewport snapping."""
    from pathlib import Path
    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path('/tmp/blender-nurbs-snapping-ui')
    output.mkdir(parents=True, exist_ok=True)

    def screen_world(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    for case in cases:
        with bpy.context.temp_override(window=window, area=area, region=region):
            if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
                bpy.ops.object.mode_set(mode='OBJECT')
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        settings = window.scene.tool_settings
        settings.use_snap = False
        settings.transform_pivot_point = 'MEDIAN_POINT'
        window.scene.cursor.location = (0, 0, 0)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        obj = window.view_layer.objects.active
        yield e.tab()
        yield e.numpad_7()
        space.region_3d.view_location = (2, 1, 0)
        space.region_3d.view_distance = 12
        space.shading.show_xray = True
        yield e.two()
        yield e.e().y().text('2').ret()
        if case in {'transformed', 'transformed-closest'}:
            obj.rotation_euler.z = 0.5
            obj.scale = (1.5, 0.75, 2)
            obj.location = (3, -2, 0.25)
            window.view_layer.update()
        seed = 2 ** -35 if case == 'precision' else 0
        if seed:
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.curve.select_all(action='SELECT')
                bpy.ops.transform.translate(value_precise_z='0.00000000002910383045673370361328125')
        corner_base = case in {'axis', 'plane', 'precision', 'transformed', 'mesh-target', 'single-vertex'}
        yield e.one() if corner_base else e.three()
        yield e.alt.a()
        local = (-1, 0, seed) if corner_base else (0, 1, seed)
        yield e.cursor_position_set(*screen_world(obj.matrix_world @ Vector(local)), move=True)
        yield e.leftmouse()
        if corner_base:
            yield e.ctrl.l()
        yield e.shift.d().x().text('3').ret()
        if seed:
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.transform.translate(value_precise_z='1.0000000000009094947017729282379150390625')
        elif case == 'plane':
            yield e.g().z().text('1').ret()
        target = obj.matrix_world @ Vector((1, 2, seed))
        if case == 'single-vertex':
            yield e.alt.a()
            yield e.cursor_position_set(*screen_world((2, 0, 0)), move=True)
            yield e.leftmouse()
            target = Vector((-1, 0, 0))
        if case == 'mesh-target':
            data = bpy.data.meshes.new('External vertex snap target')
            data.from_pydata([(0.5, 0.25, 0.125)], [], [])
            other = bpy.data.objects.new('External vertex snap target', data)
            window.scene.collection.objects.link(other)
            other.location = (2, 1, 1)
            other.scale = (2, 3, 4)
            window.view_layer.update()
            target = other.matrix_world @ Vector((0.5, 0.25, 0.125))
        settings.snap_elements = {'VERTEX'}
        settings.snap_target = case if case in {'ACTIVE', 'CLOSEST', 'MEDIAN', 'CENTER'} else 'ACTIVE'
        if case == 'transformed-closest':
            settings.snap_target = 'CLOSEST'
        if case == 'CENTER':
            settings.transform_pivot_point = 'BOUNDING_BOX_CENTER'
        elif case == 'CURSOR':
            settings.snap_target = 'CENTER'
            settings.transform_pivot_point = 'CURSOR'
            window.scene.cursor.location = (4, 5, 0)
        settings.use_snap = True
        yield e.cursor_position_set(*screen_world(obj.matrix_world @ Vector((3, 1, seed))), move=True)
        yield e.g()
        if case == 'axis':
            yield e.x()
        elif case == 'plane':
            yield e.shift.z()
        yield e.cursor_position_set(*screen_world(target), move=True)
        yield
        yield e.leftmouse()
        with bpy.context.temp_override(window=window, area=area, region=region):
            t.assertEqual(bpy.ops.view3d.snap_cursor_to_active(), {'FINISHED'})
        if case == 'axis':
            expected = Vector((1, 0, 0))
        elif case == 'plane':
            expected = Vector((1, 2, 1))
        elif corner_base:
            expected = target
        elif case in {'CLOSEST', 'transformed-closest'}:
            expected = obj.matrix_world @ Vector((2, 1, 0))
        elif case == 'CURSOR':
            expected = Vector((0, -2, 0))
        else:
            expected = Vector((1, 2, 0))
        for actual, value in zip(window.scene.cursor.location, expected):
            t.assertAlmostEqual(actual, value, places=5, msg=case)
        if seed:
            # A float translation loses 2^-40 near 1 and leaves the corner off the target.
            t.assertEqual(window.scene.cursor.location.z, seed)
        if case == 'single-vertex':
            t.assertIn('Verts:1/8', window.scene.statistics(window.view_layer))
            window.view_layer.update()
            with bpy.context.temp_override(window=window, area=area, region=region):
                evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
                mesh = evaluated.to_mesh()
                try:
                    t.assertIsNotNone(mesh)
                    t.assertGreater(len(mesh.polygons), 0)
                finally:
                    evaluated.to_mesh_clear()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / (case + '-snapped.blend')),
                                       check_existing=False)
        print('NATIVE_VERTEX_SNAP_SOURCE_PASS', case, tuple(window.scene.cursor.location))


def native_single_vertex_snap():
    yield from native_vertex_snap_sources(('single-vertex',))


def snapped_face_fresh_continuation():
    """Reopen the saved source, cancel one extrusion's movement, then extrude again."""
    from pathlib import Path
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    output = Path('/tmp/blender-nurbs-snapping-ui')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(str(output / 'nurbs-snap-extrude.blend'), link=False) as (source, target):
        target.curves = list(source.curves)
    curve = next(c for c in target.curves if c.is_native_nurbs)
    t.assertEqual(len(curve.splines), 0)
    obj = bpy.data.objects.new('Saved snapping source', curve)
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    settings = window.scene.tool_settings
    settings.snap_elements = {'VERTEX'}
    settings.snap_target = 'ACTIVE'
    settings.use_snap = True
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    t.assertEqual(tuple(curve.nurbs_selection_mode), (False, False, True))
    yield e.e().esc()
    yield e.e().z().text('1').ret()
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.view3d.snap_cursor_to_active(), {'FINISHED'})
    t.assertEqual(tuple(window.scene.cursor.location), (2, 3, 2))
    t.assertEqual(len(curve.splines), 0)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'fresh-continued.blend'), check_existing=False)
    print('NATIVE_SNAPPED_FACE_FRESH_CONTINUATION_PASS', window.scene.statistics(window.view_layer))
