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
            result = bpy.ops.transform.translate(value=(0.0, 0.0, 1.0))
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
