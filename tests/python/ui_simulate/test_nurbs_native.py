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
