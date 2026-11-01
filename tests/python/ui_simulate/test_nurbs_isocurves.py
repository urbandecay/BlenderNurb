# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Surface isocurves are visible only with wire display and never become source edges."""
import modules.ui_test_utils as ui


def wire_display_toggle():
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().e().y().text('2').ret().alt.a().numpad_7()
    properties = ui.get_window_area_by_type(window, 'PROPERTIES')
    if properties:
        properties.spaces.active.context = 'DATA'
    space.region_3d.view_location = (0, 1, 0)
    space.region_3d.view_distance = 5
    space.shading.type = 'SOLID'
    space.overlay.show_floor = False
    space.overlay.show_axis_x = False
    space.overlay.show_axis_y = False
    space.overlay.show_cursor = False
    space.show_gizmo = False
    space.overlay.show_wireframes = False
    yield e.cursor_position_set(20, 20, move=True)

    def sample(label, point=(0, 0.74, 0)):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath='/tmp/nurbs-isocurve-' + label + '.png')
        image = bpy.data.images.load('/tmp/nurbs-isocurve-' + label + '.png', check_existing=False)
        try:
            x, y = location_3d_to_region_2d(region, space.region_3d, Vector(point))
            x, y = round(x) + region.x, round(y) + region.y
            pixels = tuple(image.pixels)
            w = image.size[0]
            return [pixels[4 * (j*w+i)+c] for j in range(y-2,y+3)
                    for i in range(x-2,x+3) for c in range(3)]
        finally:
            bpy.data.images.remove(image)
    solid = sample('solid')
    space.overlay.show_wireframes = True
    yield
    wire = sample('wire')
    t.assertGreater(max(abs(a-b) for a,b in zip(solid,wire)), 0.03)
    curve = window.view_layer.objects.active.data
    t.assertEqual(curve.nurbs_isocurve_density, 1)
    resolution = (curve.resolution_u, curve.resolution_v)
    sparse = sample('sparse-quarter', (-0.5, 0.74, 0))
    curve.nurbs_isocurve_density = 3
    yield
    dense = sample('dense-quarter', (-0.5, 0.74, 0))
    t.assertGreater(max(abs(a-b) for a,b in zip(sparse,dense)), 0.03)
    t.assertEqual((curve.resolution_u, curve.resolution_v), resolution)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath='/tmp/nurbs-isocurve-density.blend', check_existing=False,
                                   copy=True)
    with bpy.data.libraries.load('/tmp/nurbs-isocurve-density.blend') as (source, target):
        target.curves = [curve.name]
    stored = target.curves[0]
    t.assertEqual(stored.nurbs_isocurve_density, 3)
    bpy.data.curves.remove(stored)
    curve.nurbs_isocurve_density = 1
    space.overlay.show_wireframes = False
    yield
    restored = sample('restored')
    t.assertLess(max(abs(a-b) for a,b in zip(solid,restored)), 0.01)
    t.assertIn('Edges:0/4', window.scene.statistics(window.view_layer))
