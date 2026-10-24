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

    lower = min(point[0] for point in bottom) - 10, min(point[1] for point in bottom) - 10
    upper = max(point[0] for point in bottom) + 10, max(point[1] for point in bottom) + 10
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

    # Legacy tools must report unsupported native edits instead of creating another source.
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertFalse(bpy.ops.curve.extrude.poll())
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
            result = bpy.ops.transform.rotate(value=1.0, orient_axis='Z')
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
