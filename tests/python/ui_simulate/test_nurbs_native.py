# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native source entry and vertex selection through Blender's viewport events.

Requires the explicitly exported C++ fixture in BLENDER_NURBS_PREVIEW_FIXTURE.
The legacy splines collection is empty throughout. No Python geometry substitute
or public native component API is used to make this test pass.
"""

import modules.ui_test_utils as ui


def separate_selection_workflow(cases=('VERT', 'EDGE', 'FACE', 'BOX', 'REGION', 'TAGGED')):
    """P makes independent source objects; undo restores both ownership and source selection."""
    import os
    import re
    from pathlib import Path

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_SEPARATE_OUTPUT', '/tmp/blender-nurbs-separate-ui'))
    output.mkdir(parents=True, exist_ok=True)

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def save(label, suffix):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'separate-{label}-{suffix}.blend'),
                                      check_existing=False)

    for label in cases:
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        window.scene.cursor.location = (0, 0, 0)
        window.scene.tool_settings.use_snap = False
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        original_name = window.view_layer.objects.active.name
        yield e.tab()
        yield e.numpad_7()
        space.shading.show_xray = True
        space.region_3d.view_location = (0, 2, 0)
        space.region_3d.view_distance = 9
        if label == 'TAGGED':
            yield e.e().y().text('2').ret()
            t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
        else:
            yield e.two()
            yield e.e().y().text('2').ret()
            yield e.e().y().text('2').ret()
            if label in {'FACE', 'REGION'}:
                yield e.three()
            elif label in {'VERT', 'BOX'}:
                yield e.one()
            yield e.alt.a()
            if label == 'REGION':
                yield e.a()
            elif label == 'BOX':
                yield e.b()
                yield from e.leftmouse.cursor_motion((screen((-1.2, -0.2, 0)), screen((1.2, 2.2, 0))))
            else:
                position = (-1, 2, 0) if label == 'VERT' else (0, 2, 0) if label == 'EDGE' else (0, 1, 0)
                yield e.cursor_position_set(*screen(position), move=True)
                yield e.leftmouse()
        before_counts = counts() if label != 'TAGGED' else None
        yield e.tab()
        save(label, 'before')
        yield e.tab()
        yield e.p()
        t.assertEqual(len(window.scene.objects), 2, label)
        original = window.scene.objects[original_name]
        detached = next(obj for obj in window.scene.objects if obj != original)
        detached_name = detached.name
        t.assertEqual(original.mode, 'EDIT')
        t.assertEqual(detached.mode, 'OBJECT')
        t.assertEqual(window.view_layer.objects.active, original)
        t.assertTrue(detached.select_get())
        t.assertNotEqual(original.data, detached.data)
        for obj in (original, detached):
            t.assertTrue(obj.data.is_native_nurbs)
            t.assertEqual(len(obj.data.splines), 0)
        expected_remainder = {
            'VERT': (0, 6, 0, 7, 0, 2), 'EDGE': (0, 6, 0, 7, 0, 2),
            'FACE': (0, 4, 0, 4, 0, 1), 'BOX': (0, 4, 0, 4, 0, 1),
            'REGION': (0, 0, 0, 0, 0, 0), 'TAGGED': (0, 4, 0, 4, 0, 1),
        }[label]
        t.assertEqual(counts(), expected_remainder, label)
        yield e.ctrl.z()
        t.assertEqual(len(window.scene.objects), 1, 'Undo must remove the new object')
        if label == 'TAGGED':
            t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
            t.assertTrue(window.view_layer.objects.active.data.has_tagged_nurbs)
        else:
            t.assertEqual(counts(), before_counts)
        yield e.ctrl.shift.z()
        t.assertEqual(len(window.scene.objects), 2)
        t.assertEqual(counts(), expected_remainder)
        yield e.tab()
        save(label, 'after')
        original = window.scene.objects[original_name]
        detached = window.scene.objects[detached_name]
        for obj in window.scene.objects:
            obj.select_set(False)
        detached.select_set(True)
        window.view_layer.objects.active = detached
        yield e.tab()
        expected_detached = {
            'VERT': (1, 1, 0, 0, 0, 0), 'EDGE': (2, 2, 1, 1, 0, 0),
            'FACE': (4, 4, 4, 4, 1, 1), 'BOX': (4, 4, 4, 4, 1, 1),
            'REGION': (6, 6, 7, 7, 2, 2), 'TAGGED': (2, 2, 1, 1, 0, 0),
        }[label]
        t.assertEqual(counts(), expected_detached, label)
        yield e.ctrl.l()
        t.assertEqual(counts(), expected_detached)
        yield e.g().z().text('0.75').esc()
        yield e.g().z().text('1').ret()
        yield e.e().z().text('0.5').ret()
        t.assertGreater(counts()[1] + counts()[3] + counts()[5], sum(expected_detached[1::2]))
        yield e.shift.d().x().text('3').ret()
        yield e.tab()
        save(label, 'continued')
        yield e.numpad_1()
        yield e.numpad_6()
        yield e.numpad_8()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'separate-{label}.png'))
        print('NATIVE_SEPARATE_WORKFLOW_PASS', label)

        # Run actual mesh Separate on the matching source, for persisted topology comparison.
        import bmesh
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        mesh = bpy.data.meshes.new('Separate mesh reference')
        vertices = [(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0)]
        faces = [(0, 1, 3, 2)]
        if label != 'TAGGED':
            vertices.extend([(-1, 4, 0), (1, 4, 0)])
            faces.append((2, 3, 5, 4))
        mesh.from_pydata(vertices, [], faces)
        obj = bpy.data.objects.new('Separate mesh reference', mesh)
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        yield e.tab()
        mode = 'FACE' if label in {'FACE', 'REGION'} else 'EDGE' if label == 'EDGE' else 'VERT'
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.select_mode(type=mode)
        bm = bmesh.from_edit_mesh(mesh)
        bm.verts.ensure_lookup_table()
        bm.faces.ensure_lookup_table()
        for face in bm.faces:
            face.select_set(False)
        for edge in bm.edges:
            edge.select_set(False)
        for vertex in bm.verts:
            vertex.select_set(False)
        if label in {'BOX', 'TAGGED', 'VERT'}:
            selected = {0, 1, 2, 3} if label == 'BOX' else {2, 3} if label == 'TAGGED' else {2}
            for index in selected:
                bm.verts[index].select_set(True)
        elif label == 'EDGE':
            next(edge for edge in bm.edges if {v.index for v in edge.verts} == {2, 3}).select_set(True)
        else:
            for face in bm.faces if label == 'REGION' else [bm.faces[0]]:
                face.select_set(True)
        bm.select_flush_mode()
        bmesh.update_edit_mesh(mesh)
        with bpy.context.temp_override(window=window, area=area, region=region):
            t.assertEqual(bpy.ops.mesh.separate(type='SELECTED'), {'FINISHED'})
        t.assertEqual(counts(), expected_remainder, 'Native remainder must match real mesh Separate')
        yield e.tab()
        save('MESH-' + label, 'after')
        print('MESH_SEPARATE_REFERENCE_PASS', label)


def separate_owners_and_preflight():
    """Separate unique data once, preserve Blender owner properties, and reject before mutation."""
    import os
    from pathlib import Path

    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
        data.curves = ['Native preview sheet']
    material = bpy.data.materials.new('Separate data material')
    override = bpy.data.materials.new('Separate object material')
    originals = []
    for index in range(3):
        curve = data.curves[0] if index < 2 else data.curves[0].copy()
        if not curve.materials:
            curve.materials.append(material)
        obj = bpy.data.objects.new(f'Separate owner {index}', curve)
        window.scene.collection.objects.link(obj)
        obj['source_group'] = 'shared' if index < 2 else 'independent'
        obj.location = (3 * index, 0, 1)
        obj.scale = (-2, 1.5, 0.5)
        obj.material_slots[0].link = 'OBJECT'
        obj.material_slots[0].material = override
        obj.select_set(True)
        originals.append(obj)
    window.view_layer.objects.active = originals[0]
    original_names = {obj.name for obj in originals}
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    yield e.three()
    yield e.a()
    yield e.p()
    detached = [obj for obj in window.scene.objects if obj.type in {'CURVE', 'SURFACE'} and obj.name not in original_names]
    t.assertEqual(len(detached), 2, 'One fragment per unique edited data owner')
    t.assertEqual({obj['source_group'] for obj in detached}, {'shared', 'independent'})
    for obj in detached:
        t.assertEqual(obj.mode, 'OBJECT')
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(tuple(obj.scale), (-2, 1.5, 0.5))
        t.assertEqual(obj.material_slots[0].link, 'OBJECT')
        t.assertEqual(obj.material_slots[0].material, override)
        t.assertEqual(obj.data.materials[0], material)
        t.assertIn(tuple(obj.location), {(0, 0, 1), (3, 0, 1), (6, 0, 1)})
        t.assertNotIn(obj.data, [source.data for source in originals])
    t.assertNotEqual(detached[0].data, detached[1].data)
    t.assertEqual(originals[0].data, originals[1].data)
    yield e.ctrl.z()
    t.assertEqual(len(window.scene.objects), 3)
    yield e.ctrl.shift.z()
    t.assertEqual(len(window.scene.objects), 5)
    yield e.tab()
    output = Path(os.environ.get('BLENDER_NURBS_SEPARATE_OUTPUT', '/tmp/blender-nurbs-separate-ui'))
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'separate-owners.blend'), check_existing=False)
    print('NATIVE_SEPARATE_MULTI_OWNER_MATERIAL_TRANSFORM_PASS')

    # A valid first owner and unsupported second owner must leave both tagged and create no IDs.
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    first = window.view_layer.objects.active
    second = first.copy()
    second.data = first.data.copy()
    window.scene.collection.objects.link(second)
    second.modifiers.new('Unsupported native source modifier', 'SOLIDIFY')
    second.select_set(True)
    window.view_layer.objects.active = first
    yield e.tab()
    before_ids = (len(bpy.data.objects), len(bpy.data.curves))
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertTrue(bpy.ops.curve.separate.poll())
        t.assertEqual(bpy.ops.curve.separate(), {'CANCELLED'})
    t.assertEqual((len(bpy.data.objects), len(bpy.data.curves)), before_ids)
    for obj in (first, second):
        t.assertFalse(obj.data.is_native_nurbs)
        t.assertTrue(obj.data.has_tagged_nurbs)
        t.assertEqual(obj.mode, 'EDIT')
    yield e.tab()
    for obj in (first, second):
        t.assertEqual(len(obj.data.splines), 1)
        t.assertEqual(tuple(obj.data.splines[0].points[0].co), (-1, 0, 0, 1))
        t.assertEqual(tuple(obj.data.splines[0].points[1].co), (1, 0, 0, 1))
    print('NATIVE_SEPARATE_ATOMIC_PREFLIGHT_PASS')


def separate_fresh_continuation():
    """Both saved objects remain exact native owners editable in a fresh Blender process."""
    import os
    import re
    from pathlib import Path

    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    output = Path(os.environ.get('BLENDER_NURBS_SEPARATE_OUTPUT', '/tmp/blender-nurbs-separate-ui'))
    with bpy.data.libraries.load(str(output / 'separate-FACE-after.blend'), link=False) as (source, data):
        data.objects = list(source.objects)
    t.assertEqual(len(data.objects), 2, 'Both separate source objects must persist')
    for obj in data.objects:
        window.scene.collection.objects.link(obj)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    for obj in data.objects:
        for other in window.scene.objects:
            other.select_set(False)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
        yield e.tab()
        yield e.three()
        yield e.a()
        yield e.e().z().text('1').ret()
        stats = window.scene.statistics(window.view_layer)
        t.assertIsNotNone(re.search(r'Verts:4/8 \| Edges:4/12 \| Faces:1/6', stats), stats)
        yield e.ctrl.z()
        yield e.ctrl.shift.z()
        yield e.tab()
    print('NATIVE_SEPARATE_BOTH_OWNERS_FRESH_CONTINUATION_PASS')


def split_selection_workflow(cases=('VERT', 'EDGE', 'FACE', 'BOX', 'REGION')):
    """Y detaches source topology; moving it leaves the unselected sheet intact."""
    import os
    import re
    from pathlib import Path

    import bpy
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_SPLIT_OUTPUT', '/tmp/blender-nurbs-split-ui'))
    output.mkdir(parents=True, exist_ok=True)

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def screen(position):
        co = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(co)
        return round(co.x) + region.x, round(co.y) + region.y

    def save(label, suffix):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'split-{label}-{suffix}.blend'),
                                      check_existing=False)

    for label in cases:
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        window.scene.cursor.location = (0, 0, 0)
        window.scene.tool_settings.use_snap = False
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        yield e.tab()
        yield e.numpad_7()
        space.shading.show_xray = True
        space.overlay.show_stats = True
        space.region_3d.view_location = (0, 2, 0)
        space.region_3d.view_distance = 9
        if label == 'TAGGED':
            # Exercise Y directly after ordinary Add/E, before explicit component-mode entry.
            yield e.e().y().text('2').ret()
            t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
            yield e.y()
            t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
            t.assertEqual(counts(), (2, 6, 1, 5, 0, 1))
            yield e.ctrl.z()
            t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
            t.assertTrue(window.view_layer.objects.active.data.has_tagged_nurbs)
            yield e.ctrl.shift.z()
            t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
            t.assertEqual(counts(), (2, 6, 1, 5, 0, 1))
            yield e.g().z().text('0.5').ret()
            yield e.e().y().text('1').ret()
            t.assertEqual(counts()[5], 2)
            print('NATIVE_SPLIT_TAGGED_UNDO_AND_CONTINUE_PASS')
            continue

        yield e.two()
        yield e.e().y().text('2').ret()
        yield e.e().y().text('2').ret()
        if label in {'FACE', 'REGION'}:
            yield e.three()
        elif label in {'VERT', 'BOX'}:
            yield e.one()
        yield e.alt.a()
        if label == 'REGION':
            yield e.a()
        elif label == 'BOX':
            yield e.b()
            yield from e.leftmouse.cursor_motion((screen((-1.2, -0.2, 0)), screen((1.2, 2.2, 0))))
        else:
            position = (-1, 2, 0) if label == 'VERT' else (0, 2, 0) if label == 'EDGE' else (0, 1, 0)
            yield e.cursor_position_set(*screen(position), move=True)
            yield e.leftmouse()
        before_counts = counts()
        yield e.tab()
        save(label, 'before')
        yield e.tab()
        yield e.y()
        expected = {
            'VERT': (1, 7, 0, 7, 0, 2),
            'EDGE': (2, 8, 1, 8, 0, 2),
            'FACE': (4, 8, 4, 8, 1, 2),
            'BOX': (4, 8, 4, 8, 1, 2),
            'REGION': (6, 6, 7, 7, 2, 2),
        }[label]
        t.assertEqual(counts(), expected, label)
        yield e.ctrl.z()
        t.assertEqual(counts(), before_counts)
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), expected)
        yield e.g().z().text('0.75').esc()
        yield e.g().z().text('0.5').ret()
        yield e.tab()
        save(label, 'after')
        yield e.tab()
        yield e.ctrl.l()
        t.assertEqual(counts(), expected, 'Linked selection must stay in the detached component')
        yield e.e().z().text('0.5').ret()
        t.assertGreater(counts()[3] + counts()[5], expected[3] + expected[5])
        obj = window.view_layer.objects.active
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
        window.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            t.assertIsNotNone(mesh)
        finally:
            evaluated.to_mesh_clear()
        yield e.numpad_1()
        yield e.numpad_6()
        yield e.numpad_8()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'split-{label}.png'))
        print('NATIVE_SPLIT_WORKFLOW_PASS', label, counts())


def split_tagged_workflow():
    yield from split_selection_workflow(cases=('TAGGED',))


def split_across_objects():
    import os
    import re

    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(os.environ['BLENDER_NURBS_PREVIEW_FIXTURE'], link=False) as (_, data):
        data.curves = ['Native preview sheet']
    for index in range(2):
        obj = bpy.data.objects.new(f'Split owner {index}', data.curves[0] if index == 0 else data.curves[0].copy())
        window.scene.collection.objects.link(obj)
        obj.location.x = 3 * index
        obj.select_set(True)
        window.view_layer.objects.active = obj
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    yield e.one()
    yield e.a()
    yield e.y()
    for obj in window.view_layer.objects:
        t.assertEqual(obj.mode, 'EDIT')
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
    stats = window.scene.statistics(window.view_layer)
    t.assertIsNotNone(re.search(r'Verts:8/8 \| Edges:8/8 \| Faces:2/2', stats), stats)
    yield e.ctrl.z()
    yield e.ctrl.shift.z()
    yield e.tab()
    for obj in window.view_layer.objects:
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
    print('NATIVE_SPLIT_MULTI_OWNER_PASS')


def split_fresh_continuation():
    import os
    import re
    from pathlib import Path

    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    output = Path(os.environ.get('BLENDER_NURBS_SPLIT_OUTPUT', '/tmp/blender-nurbs-split-ui'))
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    with bpy.data.libraries.load(str(output / 'split-FACE-after.blend'), link=False) as (source, target):
        target.curves = list(source.curves)
    t.assertEqual(len(target.curves), 1)
    curve = target.curves[0]
    t.assertTrue(curve.is_native_nurbs)
    t.assertEqual(len(curve.splines), 0)
    obj = bpy.data.objects.new('Reopened split source', curve)
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    t.assertEqual(tuple(curve.nurbs_selection_mode), (False, False, True))
    yield e.e().z().text('0.5').ret()
    stats = window.scene.statistics(window.view_layer)
    t.assertIsNotNone(re.search(r'Verts:4/12 \| Edges:4/16 \| Faces:1/7', stats), stats)
    yield e.ctrl.z()
    yield e.ctrl.shift.z()
    yield e.shift.d().x().text('3').ret()
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertEqual(len(window.view_layer.objects.active.data.splines), 0)
    print('NATIVE_SPLIT_FRESH_CONTINUATION_PASS', window.scene.statistics(window.view_layer))


def split_preflight_rejects_without_partial_promotion():
    import bpy

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    first = window.view_layer.objects.active
    second = first.copy()
    second.data = first.data.copy()
    window.scene.collection.objects.link(second)
    second.modifiers.new('Unsupported native source modifier', 'SOLIDIFY')
    second.select_set(True)
    window.view_layer.objects.active = first
    yield e.tab()
    for obj in (first, second):
        t.assertEqual(obj.mode, 'EDIT')
        t.assertFalse(obj.data.is_native_nurbs)
        t.assertTrue(obj.data.has_tagged_nurbs)
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertTrue(bpy.ops.curve.split.poll())
        t.assertEqual(bpy.ops.curve.split(), {'CANCELLED'})
    for obj in (first, second):
        t.assertFalse(obj.data.is_native_nurbs)
        t.assertTrue(obj.data.has_tagged_nurbs)
        t.assertEqual(len(obj.data.splines), 1)
    yield e.tab()
    for obj in (first, second):
        t.assertEqual(len(obj.data.splines[0].points), 2)
        t.assertEqual(tuple(obj.data.splines[0].points[0].co), (-1, 0, 0, 1))
        t.assertEqual(tuple(obj.data.splines[0].points[1].co), (1, 0, 0, 1))
    print('NATIVE_SPLIT_ATOMIC_PREFLIGHT_PASS')


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


def rip_mesh_reference():
    import os
    from pathlib import Path
    import bpy
    import bmesh
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_TOPOLOGY_OUTPUT', '/tmp/blender-nurbs-topology-ui'))
    output.mkdir(parents=True, exist_ok=True)
    layouts = {
        'CORNER': ([(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0)], [], [(0, 1, 3, 2)], [0], None, (-0.8, 0.2, 0)),
        'VERT': ([(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0), (-1, 4, 0), (1, 4, 0)], [], [(0, 1, 3, 2), (2, 3, 5, 4)], [2], None, (-0.8, 2.2, 0)),
        'EDGE': ([(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0), (-1, 4, 0), (1, 4, 0)], [], [(0, 1, 3, 2), (2, 3, 5, 4)], [], (2, 3), (0, 2.2, 0)),
        'WIRE': ([(-1, 0, 0), (0, 0, 0), (1, 0, 0)], [(0, 1), (1, 2)], [], [1], None, (0.2, 0, 0)),
        'GRID_VERT': ([(x, y, 0) for y in range(3) for x in range(3)], [], [(0, 1, 4, 3), (1, 2, 5, 4), (3, 4, 7, 6), (4, 5, 8, 7)], [4], None, (1.15, 1.3, 0)),
        'GRID_EDGE': ([(x, y, 0) for y in range(3) for x in range(3)], [], [(0, 1, 4, 3), (1, 2, 5, 4), (3, 4, 7, 6), (4, 5, 8, 7)], [], (1, 4), (1.2, 0.5, 0)),
    }
    for label, (positions, edges, faces, selected, selected_edge, pointer) in layouts.items():
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        mesh = bpy.data.meshes.new('Mesh rip reference ' + label)
        mesh.from_pydata(positions, edges, faces)
        obj = bpy.data.objects.new(mesh.name, mesh)
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
        yield e.numpad_7()
        space.region_3d.view_location = (0, 2, 0)
        space.region_3d.view_distance = 9
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.select_mode(type='EDGE' if selected_edge else 'VERT')
            bpy.ops.mesh.select_all(action='DESELECT')
        bm = bmesh.from_edit_mesh(mesh)
        bm.verts.ensure_lookup_table()
        for index in selected:
            bm.verts[index].select_set(True)
        if selected_edge:
            next(edge for edge in bm.edges if {v.index for v in edge.verts} == set(selected_edge)).select_set(True)
        bm.select_flush_mode()
        bmesh.update_edit_mesh(mesh)
        co = location_3d_to_region_2d(region, space.region_3d, Vector(pointer))
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        yield e.v().z().text('1').ret()
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'rip-mesh-{label}.blend'), check_existing=False)
        print('RIP_MESH_REFERENCE', label, len(mesh.vertices), len(mesh.edges), len(mesh.polygons),
              [tuple(v.co) for v in mesh.vertices], [tuple(f.vertices) for f in mesh.polygons])


def rip_native_workflow():
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_TOPOLOGY_OUTPUT', '/tmp/blender-nurbs-topology-ui'))
    pointers = {'VERT': (-0.8, 2.2, 0), 'EDGE': (0, 2.2, 0), 'WIRE': (0.2, 0, 0),
                'GRID_VERT': (1.15, 1.3, 0), 'GRID_EDGE': (1.2, 0.5, 0)}
    totals = {'VERT': (7, 8, 2), 'EDGE': (8, 8, 2), 'WIRE': (4, 2, 0),
              'GRID_VERT': (10, 14, 4), 'GRID_EDGE': (11, 14, 4)}

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    for label, pointer in pointers.items():
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        with bpy.data.libraries.load(str(output / f'rip-source-{label}.blend'), link=False) as (_, data):
            data.curves = ['Topology source']
        obj = bpy.data.objects.new('Native Rip ' + label, data.curves[0])
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        window.scene.tool_settings.use_snap = False
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
        yield e.numpad_7()
        space.region_3d.view_location = (0, 2, 0)
        space.region_3d.view_distance = 9
        before = counts()
        co = location_3d_to_region_2d(region, space.region_3d, Vector(pointer))
        yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
        yield e.v().esc()
        t.assertEqual(counts()[1::2], totals[label], label)
        yield e.ctrl.z()
        t.assertEqual(counts(), before)
        yield e.v().z().text('1').ret()
        t.assertEqual(counts()[1::2], totals[label], label)
        after = counts()
        yield e.ctrl.z()
        t.assertEqual(counts(), before)
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), after)
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'rip-native-{label}.blend'), check_existing=False)
        yield e.tab()
        yield e.e().z().text('0.5').ret()
        t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
        t.assertEqual(len(window.view_layer.objects.active.data.splines), 0)
        yield e.numpad_1()
        yield e.numpad_6()
        yield e.numpad_8()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'rip-native-{label}.png'))
        print('NATIVE_RIP_WORKFLOW_PASS', label, counts())


def merge_mesh_reference():
    import os
    from pathlib import Path
    import bpy
    import bmesh
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    output = Path(os.environ.get('BLENDER_NURBS_TOPOLOGY_OUTPUT', '/tmp/blender-nurbs-topology-ui'))
    for label in ('CENTER', 'CURSOR', 'FIRST', 'LAST', 'COLLAPSE', 'ALL', 'OPPOSITE'):
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        nx = 2 if label == 'COLLAPSE' else 1
        positions = [(-1 + 2 * x, 2 * y, 0) for y in range(2) for x in range(nx + 1)]
        faces = [(x, x + 1, x + nx + 2, x + nx + 1) for x in range(nx)]
        mesh = bpy.data.meshes.new('Merge mesh ' + label)
        mesh.from_pydata(positions, [], faces)
        obj = bpy.data.objects.new(mesh.name, mesh)
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        window.scene.cursor.location = (0, 0, 1)
        if label == 'CURSOR':
            obj.location = (2, 1, 0)
            obj.scale = (-2, 3, 1)
            window.scene.cursor.location = (2, 1, 1)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.select_mode(type='VERT')
            bpy.ops.mesh.select_all(action='DESELECT')
        bm = bmesh.from_edit_mesh(mesh)
        bm.verts.ensure_lookup_table()
        selected = (0, 1, 2, 3) if label == 'ALL' else (0, 3) if label == 'OPPOSITE' else (0, 1, 2) if label == 'COLLAPSE' else (1, 0) if label in {'FIRST', 'LAST'} else (0, 1)
        bm.select_history.clear()
        for index in selected:
            bm.verts[index].select_set(True)
            bm.select_history.add(bm.verts[index])
        bm.select_flush_mode()
        bmesh.update_edit_mesh(mesh)
        with bpy.context.temp_override(window=window, area=area, region=region):
            result = bpy.ops.mesh.merge(type=label if label not in {'ALL', 'OPPOSITE'} else 'CENTER')
            t.assertEqual(result, {'FINISHED'})
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'merge-mesh-{label}.blend'), check_existing=False)
        print('MERGE_MESH_REFERENCE_PASS', label, len(mesh.vertices), len(mesh.edges), len(mesh.polygons))


def merge_native_workflow():
    import os
    import re
    from pathlib import Path
    import bpy
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    output = Path(os.environ.get('BLENDER_NURBS_TOPOLOGY_OUTPUT', '/tmp/blender-nurbs-topology-ui'))

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    last_menu_index = 0
    for label in ('CENTER', 'CURSOR', 'FIRST', 'LAST', 'COLLAPSE', 'ALL', 'OPPOSITE'):
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        with bpy.data.libraries.load(str(output / f'merge-source-{label}.blend'), link=False) as (_, data):
            data.curves = ['Topology source']
        obj = bpy.data.objects.new('Native Merge ' + label, data.curves[0])
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        window.scene.tool_settings.use_snap = False
        window.scene.cursor.location = (0, 0, 1)
        if label == 'CURSOR':
            obj.location = (2, 1, 0)
            obj.scale = (-2, 3, 1)
            window.scene.cursor.location = (2, 1, 1)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
        before = counts()
        # Exercise the real M popup for every mode so Blender owns the undo step.
        # Direct bpy operator execution in this event generator does not push that step.
        yield e.m()
        menu_index = {'CENTER': 0, 'CURSOR': 1, 'COLLAPSE': 2, 'FIRST': 3, 'LAST': 4}.get(label, 0)
        # Popup memory aligns the last clicked entry with the cursor. Move onto
        # the requested row explicitly, just as a user chooses it with the mouse.
        center = ui.get_area_center(area)
        row_height = 20 * bpy.context.preferences.system.ui_scale
        yield e.cursor_position_set(center[0] - 60,
                                    round(center[1] + (last_menu_index - menu_index) * row_height),
                                    move=True)
        yield e.leftmouse()
        last_menu_index = menu_index
        expected = (1, 0, 0) if label == 'ALL' else (3, 2, 0) if label == 'OPPOSITE' else (4, 5, 2) if label == 'COLLAPSE' else (3, 3, 1)
        t.assertEqual(counts()[1::2], expected, label)
        t.assertEqual(counts()[0], 1)
        after = counts()
        yield e.ctrl.z()
        t.assertEqual(counts(), before, label)
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), after, label)
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'merge-native-{label}.blend'), check_existing=False)
        obj = window.view_layer.objects.active
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
        yield e.tab()
        yield e.e().z().text('0.5').ret()
        t.assertEqual(counts()[1], expected[0] + 1)
        yield e.numpad_1()
        yield e.numpad_6()
        yield e.numpad_8()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'merge-native-{label}.png'))
        print('NATIVE_MERGE_WORKFLOW_PASS', label, counts())


def merge_tagged_and_preflight():
    import bpy
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.e().y().text('2').ret()
    t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
    yield e.m()
    center = ui.get_area_center(area)
    yield e.cursor_position_set(center[0] - 60, center[1], move=True)
    yield e.leftmouse()
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertIn('Verts:1/3 | Edges:0/3 | Faces:0/1', window.scene.statistics(window.view_layer))
    yield e.ctrl.z()
    t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertTrue(window.view_layer.objects.active.data.has_tagged_nurbs)
    yield e.ctrl.shift.z()
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    yield e.e().z().text('1').ret()
    yield e.tab()
    print('NATIVE_MERGE_TAGGED_UNDO_CONTINUE_PASS')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    first = window.view_layer.objects.active
    second = first.copy()
    second.data = first.data.copy()
    window.scene.collection.objects.link(second)
    second.modifiers.new('Unsupported native source modifier', 'SOLIDIFY')
    second.select_set(True)
    window.view_layer.objects.active = first
    yield e.tab()
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.merge(type='CENTER'), {'CANCELLED'})
    for obj in (first, second):
        t.assertFalse(obj.data.is_native_nurbs)
        t.assertTrue(obj.data.has_tagged_nurbs)
    print('NATIVE_MERGE_ATOMIC_PREFLIGHT_PASS')


def composition_native_workflow(cuts=1):
    """Stack Fill, duplicate/weld, Ctrl+R, subdivision, E, delete and further editing."""
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    view = space.region_3d
    output = Path(os.environ.get('BLENDER_NURBS_COMPOSITION_OUTPUT', '/tmp/blender-nurbs-composition'))
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.context.preferences.view.smooth_view = 0
    window.scene.cursor.location = (0, 0, 0)
    window.scene.tool_settings.use_snap = False
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.two()
    yield e.e().y().text('2').ret()
    yield e.three()
    yield e.a()

    def totals():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def check(expected, mode):
        t.assertEqual(totals(), expected)
        obj = window.view_layer.objects.active
        t.assertEqual(obj.mode, 'EDIT')
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
        t.assertEqual(tuple(obj.data.nurbs_selection_mode), tuple(mode == name for name in ('VERT', 'EDGE', 'FACE')))
        with bpy.context.temp_override(window=window, area=area, region=region):
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh)
                t.assertGreaterEqual(len(mesh.vertices), expected[0])
                if expected[2]:
                    t.assertGreater(len(mesh.polygons), 0)
            finally:
                evaluated.to_mesh_clear()

    def save(stage):
        with bpy.context.temp_override(window=window, area=area, region=region):
            path = output / f'composition-ui-{cuts}-{stage}'
            bpy.ops.wm.save_as_mainfile(filepath=str(path) + '.blend', check_existing=False)
            bpy.ops.screen.screenshot(filepath=str(path) + '.png')

    def merge_menu(row):
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        if row == 0:
            with bpy.context.temp_override(window=window, area=area, region=region):
                t.assertEqual(bpy.ops.curve.merge('EXEC_DEFAULT', True, type='CENTER'), {'FINISHED'})
            yield
            return
        yield e.m()
        center = ui.get_area_center(area)
        yield e.cursor_position_set(center[0] - 60,
                                    round(center[1] - row * 20 * bpy.context.preferences.system.ui_scale - (6 if row == 3 else 0)),
                                    move=True)
        yield e.leftmouse()

    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.delete('EXEC_DEFAULT', True, type='ONLY_FACE'), {'FINISHED'})
    yield
    yield e.two()
    yield e.a()
    yield e.f()
    check((4, 4, 1), 'EDGE')
    yield e.three()
    yield e.a()
    yield e.shift.d().ret()
    check((8, 8, 2), 'FACE')
    yield e.a()
    yield from merge_menu(3)
    t.assertEqual(bpy.context.active_operator.bl_idname, 'CURVE_OT_remove_doubles')
    check((4, 4, 1), 'FACE')
    yield e.ctrl.z()
    check((8, 8, 2), 'FACE')
    yield e.ctrl.shift.z()
    check((4, 4, 1), 'FACE')
    yield e.two()
    yield e.alt.a()
    yield e.numpad_7()
    view.view_location = (0, 1, 0)
    view.view_distance = 9
    yield
    point = location_3d_to_region_2d(region, view, Vector((0, 0, 0)))
    t.assertIsNotNone(point)
    yield e.cursor_position_set(round(point.x) + region.x, round(point.y) + region.y, move=True)
    yield e.ctrl.r()
    if cuts == 2:
        yield e.text('2')
    yield e.ret()
    yield e.esc()
    check((6, 7, 2) if cuts == 1 else (8, 10, 3), 'EDGE')
    yield e.three()
    yield e.a()
    yield from ui.call_operator(e, 'Subdivide')
    sheet = (15, 22, 8) if cuts == 1 else (21, 32, 12)
    box = (30, 56, 28) if cuts == 1 else (42, 80, 40)
    opened = (30, 56, 20) if cuts == 1 else (42, 80, 28)
    doubled = tuple(2 * value for value in opened)
    refined = (212, 384, 160) if cuts == 1 else (300, 544, 224)
    check(sheet, 'FACE')
    yield e.e().z().text('1').ret()
    check(box, 'FACE')
    yield e.ctrl.z()
    check(sheet, 'FACE')
    yield e.ctrl.shift.z()
    check(box, 'FACE')
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.delete('EXEC_DEFAULT', True, type='ONLY_FACE'), {'FINISHED'})
    yield
    check(opened, 'FACE')
    yield e.one()
    yield e.a()
    yield e.shift.d().x().text('4').ret()
    check(doubled, 'VERT')
    yield e.ctrl.z()
    check(opened, 'VERT')
    yield e.ctrl.shift.z()
    check(doubled, 'VERT')
    yield e.a()
    yield from ui.call_operator(e, 'Subdivide')
    check(refined, 'VERT')
    yield e.numpad_1()
    yield e.numpad_6()
    yield e.numpad_8()
    view.view_location = (2, 1, 0.5)
    view.view_distance = 12
    yield e.cursor_position_set(region.x + 20, region.y + 20, move=True)
    save('bodies')
    yield e.tab()
    yield e.tab()
    check(refined, 'VERT')
    yield e.a()
    yield from merge_menu(0)
    t.assertEqual(bpy.context.active_operator.bl_idname, 'CURVE_OT_merge')
    check((1, 0, 0), 'VERT')
    yield e.e().z().text('1').ret()
    check((2, 1, 0), 'VERT')
    yield e.a()
    yield from ui.call_operator(e, 'Subdivide')
    check((3, 2, 0), 'VERT')
    yield e.a()
    yield e.shift.d().x().text('2').ret()
    check((6, 4, 0), 'VERT')
    yield e.x().ret()
    check((3, 2, 0), 'VERT')
    yield e.ctrl.z()
    check((6, 4, 0), 'VERT')
    yield e.ctrl.shift.z()
    check((3, 2, 0), 'VERT')
    yield e.a()
    yield e.e().y().text('1').ret()
    check((6, 7, 2), 'VERT')
    yield e.a()
    yield e.numpad_period()
    yield e.cursor_position_set(region.x + 20, region.y + 20, move=True)
    save('continued')
    print('NURBS_COMPOSITION_UI_PASS', cuts, totals())


def composition_native_multicut_workflow():
    yield from composition_native_workflow(cuts=2)


def loopcut_native_workflow():
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_TOPOLOGY_OUTPUT', '/tmp/blender-nurbs-topology-ui'))

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(xy)
        return round(xy.x) + region.x, round(xy.y) + region.y

    for label in ('CENTER', 'SLIDE', 'MULTI', 'FACE', 'TAGGED'):
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        window.scene.cursor.location = (0, 0, 0)
        window.scene.tool_settings.use_snap = False
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        yield e.tab()
        if label in {'CENTER', 'SLIDE'}:
            yield e.two()
        yield e.e().y().text('2').ret()
        yield e.e().y().text('2').ret()
        yield e.numpad_7()
        space.region_3d.view_location = (0, 2, 0)
        space.region_3d.view_distance = 9
        if label == 'FACE':
            yield e.three()
            yield e.alt.a()
            yield e.cursor_position_set(*screen((0, 1, 0)), move=True)
            yield e.leftmouse()
        before_native = window.view_layer.objects.active.data.is_native_nurbs
        before_mode = tuple(window.view_layer.objects.active.data.nurbs_selection_mode)
        t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
        before_stats = window.scene.statistics(window.view_layer).split(' | Duration:')[0]
        yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
        yield e.ctrl.r()
        # Hover and first-stage cancellation must not change source ownership or topology.
        t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs, before_native)
        t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
        t.assertEqual(window.scene.statistics(window.view_layer).split(' | Duration:')[0], before_stats)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'loopcut-{label}-preview.png'))
        yield e.esc()
        t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs, before_native)
        t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
        yield e.ctrl.r()
        if label == 'MULTI':
            yield e.text('2')
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.screen.screenshot(filepath=str(output / 'loopcut-MULTI-two-cuts-preview.png'))
        yield e.ret()
        t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs, before_native)
        if label == 'SLIDE':
            yield e.text('0.25').ret()
        else:
            yield e.esc()
        expected = (12, 17, 6) if label == 'MULTI' else (9, 12, 4)
        t.assertEqual(counts()[1::2], expected, label)
        t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
        t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
        t.assertEqual(tuple(window.view_layer.objects.active.data.nurbs_selection_mode), before_mode)
        if label == 'MULTI':
            t.assertEqual(before_mode, (True, False, False))
        if label == 'FACE':
            t.assertEqual(before_mode, (False, False, True))
        after = counts()
        yield e.ctrl.z()
        t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
        t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs, before_native)
        if before_native:
            t.assertEqual(counts()[1::2], (6, 7, 2))
        yield e.ctrl.shift.z()
        t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
        t.assertEqual(counts(), after)
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'loopcut-native-{label}.blend'), check_existing=False)
        yield e.tab()
        yield e.g().z().text('0.25').esc()
        yield e.g().x().text('0.25').ret()
        yield e.three()
        yield e.alt.a()
        yield e.cursor_position_set(*screen((-0.5, 1, 0)), move=True)
        yield e.leftmouse()
        yield e.e().z().text('1').ret()
        t.assertGreater(counts()[5], expected[2])
        t.assertEqual(len(window.view_layer.objects.active.data.splines), 0)
        yield e.numpad_1()
        yield e.numpad_6()
        yield e.numpad_8()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'loopcut-{label}-continued.png'))
        print('NATIVE_LOOPCUT_WORKFLOW_PASS', label, counts())


def cuts_mesh_reference():
    import os
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_TOPOLOGY_OUTPUT', '/tmp/blender-nurbs-topology-ui'))
    output.mkdir(parents=True, exist_ok=True)
    for label in ('CENTER', 'SLIDE', 'MULTI', 'TAGGED', 'SUBDIVIDE'):
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        mesh = bpy.data.meshes.new('Mesh cut reference ' + label)
        positions = [(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0)]
        faces = [(0, 1, 3, 2)]
        if label != 'SUBDIVIDE':
            positions.extend([(-1, 4, 0), (1, 4, 0)])
            faces.append((2, 3, 5, 4))
        mesh.from_pydata(positions, [], faces)
        obj = bpy.data.objects.new(mesh.name, mesh)
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
        yield e.numpad_7()
        space.region_3d.view_location = (0, 2, 0)
        space.region_3d.view_distance = 9
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.select_mode(type='FACE' if label == 'SUBDIVIDE' else 'EDGE')
            bpy.ops.mesh.select_all(action='SELECT' if label == 'SUBDIVIDE' else 'DESELECT')
        if label == 'SUBDIVIDE':
            yield from ui.call_operator(e, 'Subdivide')
        else:
            xy = location_3d_to_region_2d(region, space.region_3d, Vector((0, 0, 0)))
            yield e.cursor_position_set(round(xy.x) + region.x, round(xy.y) + region.y, move=True)
            yield e.ctrl.r()
            if label == 'MULTI':
                yield e.text('2')
            yield e.ret()
            if label == 'SLIDE':
                yield e.text('0.25').ret()
            else:
                yield e.esc()
        yield e.tab()
        expected = (9, 12, 4) if label == 'SUBDIVIDE' else (12, 17, 6) if label == 'MULTI' else (9, 12, 4)
        t.assertEqual((len(mesh.vertices), len(mesh.edges), len(mesh.polygons)), expected)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'cut-mesh-{label}.blend'), check_existing=False)
        print('CUT_MESH_REFERENCE_PASS', label, [tuple(v.co) for v in mesh.vertices])


def subdivide_native_workflow():
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_TOPOLOGY_OUTPUT', '/tmp/blender-nurbs-topology-ui'))

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(xy)
        return round(xy.x) + region.x, round(xy.y) + region.y

    for label in ('FULL', 'OPPOSITE', 'WIRE', 'TAGGED'):
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        window.scene.cursor.location = (0, 0, 0)
        window.scene.tool_settings.use_snap = False
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        yield e.tab()
        if label != 'TAGGED':
            yield e.two()
        if label != 'WIRE':
            yield e.e().y().text('2').ret()
        yield e.numpad_7()
        space.region_3d.view_location = (0, 1, 0)
        space.region_3d.view_distance = 7
        if label == 'OPPOSITE':
            yield e.alt.a()
            yield e.cursor_position_set(*screen((0, 0, 0)), move=True)
            yield e.leftmouse()
            yield e.cursor_position_set(*screen((0, 2, 0)), move=True)
            yield e.shift.leftmouse()
        elif label == 'FULL':
            yield e.three()
            yield e.a()
        else:
            yield e.a()
        before_native = window.view_layer.objects.active.data.is_native_nurbs
        yield from ui.call_operator(e, 'Subdivide')
        expected = (3, 2, 0) if label == 'WIRE' else (6, 7, 2) if label == 'OPPOSITE' else (9, 12, 4)
        t.assertEqual(counts()[1::2], expected, label)
        after = counts()
        yield e.ctrl.z()
        t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs, before_native)
        if before_native:
            t.assertEqual(counts()[1::2], (2, 1, 0) if label == 'WIRE' else (4, 4, 1))
        yield e.ctrl.shift.z()
        t.assertEqual(counts(), after)
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'subdivide-native-{label}.blend'), check_existing=False)
        yield e.tab()
        yield e.g().z().text('0.25').ret()
        yield e.e().z().text('1').ret()
        t.assertGreater(counts()[1], expected[0])
        t.assertEqual(len(window.view_layer.objects.active.data.splines), 0)
        yield e.numpad_1()
        yield e.numpad_6()
        yield e.numpad_8()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'subdivide-{label}-continued.png'))
        print('NATIVE_SUBDIVIDE_WORKFLOW_PASS', label, counts())


def navigation_selection_workflow():
    """Compare native V/E/F navigation with actual mesh, including preference-off and empty input."""
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Quaternion, Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    view = space.region_3d
    prefs = bpy.context.preferences
    prefs.view.smooth_view = 0
    prefs.inputs.use_auto_perspective = False
    prefs.inputs.use_mouse_depth_navigate = False
    prefs.inputs.use_zoom_to_mouse = False
    space.shading.show_xray = True
    output = Path(os.environ.get('BLENDER_NURBS_NAVIGATION_OUTPUT', '/tmp/blender-nurbs-navigation-ui'))
    output.mkdir(parents=True, exist_ok=True)
    references = {}
    positions = [(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0), (-1, 4, 0), (1, 4, 0)]
    faces = [(0, 1, 3, 2), (2, 3, 5, 4)]

    def near(a, b, label):
        t.assertLess((Vector(a) - Vector(b)).length, 2e-4, label)

    def screen(world):
        point = location_3d_to_region_2d(region, view, world)
        t.assertIsNotNone(point)
        return point

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    for kind in ('MESH', 'NATIVE'):
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        window.scene.cursor.location = (0, 0, 0)
        window.scene.tool_settings.use_snap = False
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        if kind == 'MESH':
            mesh = bpy.data.meshes.new('Navigation mesh reference')
            mesh.from_pydata(positions, [], faces)
            obj = bpy.data.objects.new(mesh.name, mesh)
            window.scene.collection.objects.link(obj)
            obj.select_set(True)
            window.view_layer.objects.active = obj
            yield e.tab()
        else:
            yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
            yield e.tab()
            yield e.two()
            yield e.e().y().text('2').ret()
            yield e.e().y().text('2').ret()
            obj = window.view_layer.objects.active
            t.assertTrue(obj.data.is_native_nurbs)
        obj.location = (5, -3, 2)
        obj.rotation_euler = (0.2, -0.1, 0.3)
        obj.scale = (-2, 3, 1.5)
        window.view_layer.update()
        owner = obj.matrix_world.copy()
        # Orbit uses median even when the transform pivot is the far-away cursor.
        window.scene.cursor.location = (30, 40, 50)
        window.scene.tool_settings.transform_pivot_point = 'CURSOR'
        for mode, point, selected_ids in (
                ('VERT', (-1, 2, 0), (2,)),
                ('EDGE', (0, 2, 0), (2, 3)),
                ('FACE', (0, 1, 0), (0, 1, 2, 3))):
            yield e.numpad_7()
            view.view_location = owner @ Vector((0, 2, 0))
            view.view_distance = 23
            if mode == 'VERT':
                yield e.one()
            elif mode == 'EDGE':
                yield e.two()
            else:
                yield e.three()
            yield e.alt.a()
            yield e.cursor_position_set(*ui.get_area_center(area), move=True)
            xy = screen(owner @ Vector(point))
            yield e.cursor_position_set(round(xy.x) + region.x, round(xy.y) + region.y, move=True)
            yield e.leftmouse()
            before = counts()
            t.assertEqual(before[0], len(selected_ids), (kind, mode, before))
            selected_points = [owner @ Vector(positions[i]) for i in selected_ids]
            center = sum(selected_points, Vector()) / len(selected_points)
            low = Vector(tuple(min(p[i] for p in selected_points) for i in range(3)))
            high = Vector(tuple(max(p[i] for p in selected_points) for i in range(3)))
            for method in ('TURNTABLE', 'TRACKBALL'):
                prefs.inputs.view_rotate_method = method
                for use_selection in (False, True):
                    prefs.inputs.use_rotate_around_active = use_selection
                    initial = center + Vector((2, -1, 0.5))
                    view.view_rotation = Quaternion((1, 0, 0, 0))
                    view.view_perspective = 'ORTHO'
                    view.view_location = initial
                    view.view_distance = 23
                    start = (region.x + region.width // 2 - 40, region.y + region.height // 2)
                    yield e.cursor_position_set(*start, move=True)
                    anchor = screen(center)
                    yield from e.middlemouse.cursor_motion((start, (start[0] + 40, start[1] + 25),
                                                            (start[0] + 80, start[1] + 50)))
                    t.assertGreater(view.view_rotation.rotation_difference(Quaternion((1, 0, 0, 0))).angle,
                                    0.01)
                    if use_selection:
                        t.assertLess((screen(center) - anchor).length, 0.05, (kind, mode, method))
                        t.assertGreater((view.view_location - initial).length, 0.01)
                    else:
                        near(view.view_location, initial, (kind, mode, method))
                    key = (mode, method, use_selection)
                    if kind == 'MESH':
                        references[key] = (view.view_location.copy(), view.view_rotation.copy())
                    else:
                        location, rotation = references[key]
                        near(view.view_location, location, key)
                        t.assertLess(view.view_rotation.rotation_difference(rotation).angle, 1e-3, key)
                    t.assertEqual(obj.mode, 'EDIT')
                    t.assertEqual(counts(), before)
            # Frame Selected centers selected bounds and preserves point-only zoom behavior.
            view.view_location = (20, 20, 20)
            view.view_distance = 70
            yield e.numpad_period()
            near(view.view_location, (low + high) * 0.5, (kind, mode, 'FRAME'))
            if kind == 'MESH':
                references[(mode, 'FRAME')] = (view.view_location.copy(), view.view_distance)
            else:
                frame_location, frame_distance = references[(mode, 'FRAME')]
                near(view.view_location, frame_location, (mode, 'FRAME'))
                t.assertAlmostEqual(view.view_distance, frame_distance, places=4)
                t.assertEqual(len(obj.data.splines), 0)
            t.assertEqual(obj.mode, 'EDIT')
            t.assertEqual(counts(), before)
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.screen.screenshot(filepath=str(output / f'navigation-{kind}-{mode}.png'))
            # No selection: Blender keeps its last orbit pivot, and framing is a no-op.
            yield e.alt.a()
            view.view_rotation = Quaternion((1, 0, 0, 0))
            view.view_location = center + Vector((2, -1, 0.5))
            view.view_distance = 23
            yield e.cursor_position_set(*start, move=True)
            anchor = screen(center)
            yield from e.middlemouse.cursor_motion((start, (start[0] + 80, start[1] + 50)))
            t.assertLess((screen(center) - anchor).length, 0.05, (kind, mode, 'EMPTY_ORBIT'))
            empty_location, empty_distance = view.view_location.copy(), view.view_distance
            yield e.numpad_period()
            near(view.view_location, empty_location, (kind, mode, 'EMPTY_FRAME'))
            t.assertEqual(view.view_distance, empty_distance)
            print('NATIVE_NAVIGATION_SELECTION_PASS', kind, mode)


def navigation_multi_owner_workflow():
    """Edited owners contribute in world space, including Blender's linked-data mode rules."""
    import bpy
    from mathutils import Quaternion, Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    view = space.region_3d
    prefs = bpy.context.preferences
    prefs.view.smooth_view = 0
    prefs.inputs.use_auto_perspective = False
    prefs.inputs.use_mouse_depth_navigate = False
    prefs.inputs.use_rotate_around_active = True
    positions = [(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0), (-1, 4, 0), (1, 4, 0)]
    references = {}
    for kind in ('MESH', 'NATIVE'):
        for linked in (False, True):
            if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
                yield e.tab()
            for obj in tuple(bpy.data.objects):
                bpy.data.objects.remove(obj, do_unlink=True)
            window.scene.cursor.location = (0, 0, 0)
            yield e.cursor_position_set(*ui.get_area_center(area), move=True)
            if kind == 'MESH':
                mesh = bpy.data.meshes.new('Multi-owner navigation reference')
                mesh.from_pydata(positions, [], [(0, 1, 3, 2), (2, 3, 5, 4)])
                first = bpy.data.objects.new(mesh.name, mesh)
                window.scene.collection.objects.link(first)
                first.select_set(True)
                window.view_layer.objects.active = first
            else:
                yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
                yield e.tab()
                yield e.two()
                yield e.e().y().text('2').ret()
                yield e.e().y().text('2').ret()
                yield e.tab()
                first = window.view_layer.objects.active
                t.assertTrue(first.data.is_native_nurbs)
            first.location = (-4, 0, 0)
            second = first.copy()
            if not linked:
                second.data = first.data.copy()
            window.scene.collection.objects.link(second)
            second.location = (4, 3, 2)
            second.scale = (-2, 0.5, 2)
            second.select_set(True)
            window.view_layer.objects.active = first
            yield e.tab()
            yield e.one()
            yield e.a()
            yield e.numpad_7()
            mode_flags = (first.mode, second.mode)
            if kind == 'NATIVE':
                t.assertEqual(mode_flags, references[(linked, 'MODES')])
            else:
                references[(linked, 'MODES')] = mode_flags
            edited = [obj for obj in (first, second) if obj.mode == 'EDIT']
            points = [obj.matrix_world @ Vector(point) for obj in edited for point in positions]
            orbit_center = sum(points, Vector()) / len(points)
            low = Vector(tuple(min(p[i] for p in points) for i in range(3)))
            high = Vector(tuple(max(p[i] for p in points) for i in range(3)))
            frame_center = (low + high) * 0.5
            view.view_location = (2, 1, 0)
            view.view_distance = 25
            view.view_rotation = Quaternion((1, 0, 0, 0))
            start = (region.x + region.width // 2, region.y + region.height // 2)
            yield e.cursor_position_set(*start, move=True)
            anchor = location_3d_to_region_2d(region, view, orbit_center)
            yield from e.middlemouse.cursor_motion((start, (start[0] + 80, start[1] + 50)))
            after = location_3d_to_region_2d(region, view, orbit_center)
            t.assertLess((after - anchor).length, 0.05, (kind, linked))
            if kind == 'MESH':
                references[linked] = (view.view_location.copy(), view.view_rotation.copy())
            else:
                location, rotation = references[linked]
                t.assertLess((view.view_location - location).length, 2e-4)
                t.assertLess(view.view_rotation.rotation_difference(rotation).angle, 1e-3)
            yield e.numpad_period()
            t.assertLess((view.view_location - frame_center).length, 2e-4, (kind, linked))
            t.assertEqual(first.mode, 'EDIT')
            print('NATIVE_NAVIGATION_MULTI_OWNER_PASS', kind, linked, mode_flags)


def navigation_legacy_curve_workflow():
    """The shared Curve selection reader must preserve ordinary Bezier navigation."""
    import bpy
    from mathutils import Quaternion, Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    view = area.spaces.active.region_3d
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    curve = bpy.data.curves.new('Legacy Bezier navigation', 'CURVE')
    curve.dimensions = '3D'
    spline = curve.splines.new('BEZIER')
    spline.bezier_points.add(1)
    for i, point in enumerate(spline.bezier_points):
        point.co = (2 + 4 * i, 3, 1)
        point.handle_left = (1 + 4 * i, 3, 1)
        point.handle_right = (3 + 4 * i, 3, 1)
        point.select_control_point = i == 0
        point.select_left_handle = point.select_right_handle = False
    obj = bpy.data.objects.new(curve.name, curve)
    window.scene.collection.objects.link(obj)
    obj.select_set(True)
    window.view_layer.objects.active = obj
    prefs = bpy.context.preferences
    prefs.view.smooth_view = 0
    prefs.inputs.use_auto_perspective = False
    prefs.inputs.use_mouse_depth_navigate = False
    prefs.inputs.use_rotate_around_active = True
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.tab()
    yield e.numpad_7()
    view.view_rotation = Quaternion((1, 0, 0, 0))
    view.view_location = (4, 2, 0)
    view.view_distance = 15
    start = (region.x + region.width // 2, region.y + region.height // 2)
    yield e.cursor_position_set(*start, move=True)
    center = Vector((2, 3, 1))
    before = location_3d_to_region_2d(region, view, center)
    yield from e.middlemouse.cursor_motion((start, (start[0] + 80, start[1] + 50)))
    after = location_3d_to_region_2d(region, view, center)
    t.assertLess((after - before).length, 0.05)
    yield e.numpad_period()
    t.assertLess((view.view_location - center).length, 2e-4)
    t.assertEqual(obj.mode, 'EDIT')
    t.assertFalse(curve.is_native_nurbs)
    t.assertEqual(len(curve.splines), 1)
    print('NATIVE_NAVIGATION_LEGACY_BEZIER_PASS')


def merge_reported_workflow(native_entry=True, first_mode='CENTER', box_selection=False):
    """Normal Add/E source, followed by two successive endpoint merges."""
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    window.scene.cursor.location = (0, 0, 0)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    if native_entry:
        yield e.two()
    yield e.e().y().text('2').ret()
    if native_entry:
        yield e.one()
    yield e.numpad_7()
    space.region_3d.view_location = (0, 1, 0)
    space.region_3d.view_distance = 7
    space.shading.show_xray = False

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        return round(xy.x) + region.x, round(xy.y) + region.y

    def totals():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    yield e.alt.a()
    if box_selection:
        yield e.b()
        yield from e.leftmouse.cursor_motion((screen((0.9, -0.1, 0)), screen((1.1, 2.1, 0))))
    else:
        yield e.cursor_position_set(*screen((1, 0, 0)), move=True)
        yield e.leftmouse()
        yield e.cursor_position_set(*screen((1, 2, 0)), move=True)
        yield e.shift.leftmouse()
    print('REPORTED_MERGE_FIRST_SELECTION', window.scene.statistics(window.view_layer))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.m()
    center = ui.get_area_center(area)
    row = 2 if first_mode == 'COLLAPSE' else 0
    yield e.cursor_position_set(center[0] - 60, round(center[1] - row * 20 * bpy.context.preferences.system.ui_scale), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    t.assertEqual(totals(), (3, 3, 1))
    output = Path(os.environ.get('BLENDER_NURBS_OVERLAY_OUTPUT', '/tmp/blender-nurbs-overlay-ui'))
    output.mkdir(parents=True, exist_ok=True)
    label = f'merge-side-{native_entry}-{first_mode}-{box_selection}'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / f'{label}.blend'), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / f'{label}.png'))
    print('REPORTED_MERGE_FIRST_CENTER_PASS')
    yield e.alt.a()
    yield e.cursor_position_set(*screen((-1, 0, 0)), move=True)
    yield e.leftmouse()
    yield e.cursor_position_set(*screen((-1, 2, 0)), move=True)
    yield e.shift.leftmouse()
    print('REPORTED_MERGE_SECOND_SELECTION', window.scene.statistics(window.view_layer))
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.curve.merge(type='COLLAPSE'), {'FINISHED'})
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    t.assertEqual(totals(), (2, 1, 0))
    t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
    print('REPORTED_MERGE_SECOND_COLLAPSE_PASS')


def merge_reported_tagged_workflow():
    yield from merge_reported_workflow(native_entry=False)


def merge_reported_collapse_workflow():
    yield from merge_reported_workflow(first_mode='COLLAPSE')


def merge_reported_box_workflow():
    yield from merge_reported_workflow(box_selection=True)


def layered_merge_workflow(first_type='CENTER', second_type='CENTER', component_mode='VERT'):
    """Merge one cube edge, then the triangular top; continue editing the same source."""
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    view = space.region_3d
    output = Path(os.environ.get('BLENDER_NURBS_LAYERED_OUTPUT', '/tmp/blender-nurbs-layered-ui'))
    output.mkdir(parents=True, exist_ok=True)
    label = f'{first_type}-{second_type}-{component_mode}'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.context.preferences.view.smooth_view = 0
    window.scene.cursor.location = (0, 0, 0)
    window.scene.tool_settings.use_snap = False
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.two()
    yield e.e().y().text('2').ret()
    yield e.a()
    yield e.three()
    yield e.g().y().text('-1').ret()
    yield e.e().z().text('2').ret()
    yield e.one()
    space.shading.show_xray = False
    yield e.numpad_7()
    view.view_location = (0, 0, 1)
    view.view_distance = 8
    yield e.alt.a()

    def screen(position):
        point = location_3d_to_region_2d(region, view, Vector(position))
        t.assertIsNotNone(point)
        return round(point.x) + region.x, round(point.y) + region.y

    for i, point in enumerate(((-1, -1, 2), (1, -1, 2))):
        yield e.cursor_position_set(*screen(point), move=True)
        yield e.shift.leftmouse() if i else e.leftmouse()

    def merge(merge_type):
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.m()
        center = ui.get_area_center(area)
        row = 2 if merge_type == 'COLLAPSE' else 0
        yield e.cursor_position_set(center[0] - 60,
                                    round(center[1] - row * 20 * bpy.context.preferences.system.ui_scale),
                                    move=True)
        yield e.leftmouse()
        t.assertEqual(bpy.context.active_operator.bl_idname, 'CURVE_OT_merge')
        t.assertEqual(bpy.context.active_operator.type, merge_type)

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def save(stage):
        obj = window.view_layer.objects.active
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            t.assertIsNotNone(mesh)
            t.assertGreater(len(mesh.polygons), 0, 'The merged NURBS body failed evaluation')
        finally:
            evaluated.to_mesh_clear()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'layered-{label}-{stage}.blend'),
                                      check_existing=False)
            bpy.ops.screen.screenshot(filepath=str(output / f'layered-{label}-{stage}.png'))

    yield from merge(first_type)
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    t.assertEqual(counts()[1::2], (7, 11, 6))
    yield e.one() if component_mode == 'VERT' else e.two() if component_mode == 'EDGE' else e.three()
    yield e.numpad_1()
    space.shading.show_xray = True
    yield e.alt.a()
    yield e.b()
    yield from e.leftmouse.cursor_motion((screen((-1.2, 0, 1.8)), screen((1.2, 0, 2.2))))
    t.assertEqual(counts()[0], 3)
    yield e.numpad_6()
    yield e.numpad_8()
    space.shading.show_xray = False
    save('before-second')
    before = counts()
    yield from merge(second_type)
    t.assertEqual(counts()[1::2], (5, 8, 5))
    # Mesh leaves the tip selected in V mode; E/F flush clears selection without a surviving
    # selected edge/face. Check the same source selection policy here.
    t.assertEqual(counts()[0], 1 if component_mode == 'VERT' else 0)
    t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
    after = counts()
    save('after-second')
    yield e.ctrl.z()
    t.assertEqual(counts(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), after)
    yield e.tab()
    yield e.tab()
    t.assertEqual(counts(), after)
    yield e.one()
    yield e.alt.a()
    yield e.numpad_1()
    yield e.cursor_position_set(*screen((0, 1 / 3, 2)), move=True)
    yield e.leftmouse()
    t.assertEqual(counts()[0], 1)
    yield e.e().z().text('0.5').ret()
    t.assertEqual(counts()[1::2], (6, 9, 5))
    save('continued')
    yield e.ctrl.z()
    t.assertEqual(counts()[1::2], (5, 8, 5))
    print('NURBS_LAYERED_MERGE_PASS', label, counts())


def layered_merge_collapse_workflow():
    yield from layered_merge_workflow(first_type='COLLAPSE', second_type='COLLAPSE')


def layered_merge_edge_workflow():
    yield from layered_merge_workflow(second_type='COLLAPSE', component_mode='EDGE')


def layered_merge_face_workflow():
    yield from layered_merge_workflow(component_mode='FACE')


def merge_cube_top_workflow(merge_type='CENTER', component_mode='VERT', tagged_entry=False,
                            centered=True, selection_gesture='CAP'):
    """The reported line -> face -> cube -> merge top sequence must produce a pyramid."""
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    window.scene.cursor.location = (0, 0, 0)
    window.scene.tool_settings.use_snap = False
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    if not tagged_entry:
        yield e.two()
    yield e.e().y().text('2').ret()
    yield e.a()
    if not tagged_entry:
        yield e.three()
    if centered:
        yield e.g().y().text('-1').ret()
    yield e.e().z().text('2').ret()
    if component_mode == 'VERT' and not tagged_entry:
        yield e.one()
    elif component_mode == 'EDGE':
        yield e.two()
    elif component_mode == 'FACE':
        yield e.three()
    space.shading.show_xray = False
    if selection_gesture != 'CAP':
        yield e.numpad_7()
        space.region_3d.view_location = (0, 0, 2)
        space.region_3d.view_distance = 8
        yield e.alt.a()

        def screen(point):
            xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
            return round(xy.x) + region.x, round(xy.y) + region.y

        if selection_gesture == 'BOX':
            yield e.b()
            yield from e.leftmouse.cursor_motion((screen((-1.2, -1.2, 2)), screen((1.2, 1.2, 2))))
        else:
            for i, corner in enumerate(((-1, -1, 2), (1, -1, 2), (1, 1, 2), (-1, 1, 2))):
                yield e.cursor_position_set(*screen(corner), move=True)
                yield e.shift.leftmouse() if i else e.leftmouse()
    yield e.numpad_1()
    yield e.numpad_6()
    yield e.numpad_8()
    space.region_3d.view_location = (0, 0 if centered else 1, 1)
    space.region_3d.view_distance = 8

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs, not tagged_entry)

    def tagged_snapshot():
        obj = window.view_layer.objects.active
        obj.update_from_editmode()
        splines = obj.data.splines
        t.assertEqual(len(splines), 6)
        positions = {tuple(point.co[:3]) for spline in splines for point in spline.points}
        selected = {tuple(point.co[:3]) for spline in splines for point in spline.points if point.select}
        t.assertEqual(len(positions), 8)
        t.assertEqual(len(selected), 4)
        t.assertTrue(all(point[2] == 2 for point in selected))
        return tuple(tuple((tuple(point.co), point.select) for point in spline.points) for spline in splines)

    if tagged_entry:
        before = tagged_snapshot()
    else:
        t.assertEqual(counts()[1::2], (8, 12, 6))
        t.assertEqual(counts()[0], 4)
        before = counts()
    output = Path(os.environ.get('BLENDER_NURBS_APEX_OUTPUT', '/tmp/blender-nurbs-apex-ui'))
    output.mkdir(parents=True, exist_ok=True)
    label = f'{merge_type}-{component_mode}-{tagged_entry}-{centered}-{selection_gesture}'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / f'apex-{label}-before.blend'),
                                  check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / f'apex-{label}-before.png'))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.m()
    center = ui.get_area_center(area)
    row = 2 if merge_type == 'COLLAPSE' else 0
    yield e.cursor_position_set(center[0] - 60,
                                round(center[1] - row * 20 * bpy.context.preferences.system.ui_scale),
                                move=True)
    yield e.leftmouse()
    t.assertEqual(bpy.context.active_operator.bl_idname, 'CURVE_OT_merge')
    t.assertEqual(bpy.context.active_operator.type, merge_type)
    t.assertEqual(counts()[1::2], (5, 8, 5))
    t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
    after = counts()
    yield e.ctrl.z()
    t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs, not tagged_entry)
    t.assertEqual(tagged_snapshot() if tagged_entry else counts(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(counts(), after)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / f'apex-{label}.blend'), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / f'apex-{label}.png'))
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    yield e.tab()
    yield e.tab()
    t.assertEqual(counts()[1::2], (5, 8, 5))
    yield e.one()
    yield e.alt.a()
    yield e.numpad_1()
    point = location_3d_to_region_2d(region, space.region_3d, Vector((0, 0 if centered else 1, 2)))
    yield e.cursor_position_set(round(point.x) + region.x, round(point.y) + region.y, move=True)
    yield e.leftmouse()
    t.assertEqual(counts()[0], 1)
    yield e.e().z().text('0.5').ret()
    t.assertEqual(counts()[1::2], (6, 9, 5))
    yield e.ctrl.z()
    t.assertEqual(counts()[1::2], (5, 8, 5))
    print('NATIVE_MERGE_CUBE_TOP_PASS', label, counts())


def merge_cube_top_collapse_workflow():
    yield from merge_cube_top_workflow(merge_type='COLLAPSE')


def merge_cube_top_face_workflow():
    yield from merge_cube_top_workflow(component_mode='FACE')


def merge_cube_top_edge_workflow():
    yield from merge_cube_top_workflow(component_mode='EDGE')


def merge_cube_top_tagged_workflow():
    yield from merge_cube_top_workflow(tagged_entry=True)


def merge_cube_top_uncentered_workflow():
    yield from merge_cube_top_workflow(centered=False)


def merge_cube_top_box_workflow():
    yield from merge_cube_top_workflow(selection_gesture='BOX')


def merge_cube_top_click_workflow():
    yield from merge_cube_top_workflow(selection_gesture='CLICK')


def surface_wire_grid_workflow(collapse_top=False, solid_wire=False):
    """Collapsing one tagged cube must preserve the other cube's quad display grid."""
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    view = space.region_3d
    output = Path(os.environ.get('BLENDER_NURBS_GRID_OUTPUT', '/tmp/blender-nurbs-grid-ui'))
    output.mkdir(parents=True, exist_ok=True)
    label = ('top' if collapse_top else 'edge') + ('-solid' if solid_wire else '')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.context.preferences.view.smooth_view = 0
    bpy.context.preferences.inputs.use_rotate_around_active = False
    window.scene.cursor.location = (0, 0, 0)
    window.scene.tool_settings.use_snap = False
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.e().y().text('2').ret()
    yield e.a()
    yield e.e().z().text('2').ret()
    yield e.a()
    yield e.shift.d().x().text('4').ret()
    t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
    space.shading.type = 'WIREFRAME'
    space.shading.show_xray_wireframe = True
    space.shading.xray_alpha_wireframe = 1.0
    if solid_wire:
        space.shading.type = 'SOLID'
        space.shading.show_xray = True
        space.shading.xray_alpha = 1.0
        space.overlay.show_wireframes = True
        space.overlay.wireframe_threshold = 1.0
        window.view_layer.objects.active.show_all_edges = True
    space.overlay.show_text = False
    space.overlay.show_floor = False
    space.overlay.show_axis_x = False
    space.overlay.show_axis_y = False
    space.overlay.show_cursor = False
    space.show_gizmo = False
    yield e.numpad_1()
    view.view_location = (2, 1, 1)
    view.view_distance = 14
    yield e.alt.a()

    def screen(position):
        point = location_3d_to_region_2d(region, view, Vector(position))
        t.assertIsNotNone(point)
        return round(point.x) + region.x, round(point.y) + region.y

    yield e.b()
    yield from e.leftmouse.cursor_motion((screen((2.8 if collapse_top else 4.8, 0, 1.8)),
                                          screen((5.2, 0, 2.2))))
    view.view_rotation = Vector((5, -8, 4)).to_track_quat('Z', 'Y')
    view.view_perspective = 'ORTHO'
    yield e.cursor_position_set(region.x + 20, region.y + 20, move=True)

    corners = [screen((x, y, z)) for x in (-1, 1) for y in (0, 2) for z in (0, 2)]
    crop = (min(p[0] for p in corners) - 5, max(p[0] for p in corners) + 6,
            min(p[1] for p in corners) - 5, max(p[1] for p in corners) + 6)

    def capture(stage):
        path = output / f'grid-{label}-{stage}.png'
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(path))
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'grid-{label}-{stage}.blend'),
                                      check_existing=False)
        image = bpy.data.images.load(str(path), check_existing=False)
        try:
            width, height = image.size
            pixels = tuple(image.pixels)
            x0, x1, y0, y1 = crop
            t.assertTrue(0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height)
            return [pixels[(y * width + x) * 4 + c]
                    for y in range(y0, y1) for x in range(x0, x1) for c in range(3)]
        finally:
            bpy.data.images.remove(image)

    reference = capture('before')
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.m()
    center = ui.get_area_center(area)
    yield e.cursor_position_set(center[0] - 60,
                                round(center[1] - 40 * bpy.context.preferences.system.ui_scale),
                                move=True)
    yield e.leftmouse()
    t.assertEqual(bpy.context.active_operator.bl_idname, 'CURVE_OT_merge')
    t.assertEqual(bpy.context.active_operator.type, 'COLLAPSE')
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    stats = window.scene.statistics(window.view_layer)
    match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
    t.assertIsNotNone(match, stats)
    counts = tuple(map(int, match.groups()))
    t.assertEqual(counts[1::2], (13, 20, 11) if collapse_top else (15, 23, 12))
    t.assertEqual(counts[0], 1)
    yield e.cursor_position_set(region.x + 20, region.y + 20, move=True)
    after = capture('collapse')
    error = sum(abs(a - b) for a, b in zip(reference, after)) / len(reference)
    print('NURBS_UNTOUCHED_GRID_PIXEL_ERROR', label, 'collapse', error)
    t.assertLess(error, 0.005, 'Collapsing the right cube changed the left cube display grid')
    yield e.ctrl.z()
    t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
    yield e.ctrl.shift.z()
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    yield e.x().ret()
    yield e.cursor_position_set(region.x + 20, region.y + 20, move=True)
    after_delete = capture('delete')
    error = sum(abs(a - b) for a, b in zip(reference, after_delete)) / len(reference)
    print('NURBS_UNTOUCHED_GRID_PIXEL_ERROR', label, 'delete', error)
    t.assertLess(error, 0.005, 'Deleting the collapsed corner changed the left cube display grid')
    t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
    # Mesh and NURBS picking both search within 75 pixels. A guide click may therefore select
    # a nearby real corner, but the grid intersection must never become an editing handle.
    yield e.alt.a()
    resolution = 2 * max(window.view_layer.objects.active.data.resolution_u,
                         window.view_layer.objects.active.data.resolution_v) - 1
    candidates = [screen((-1 + 2 * i / resolution, 0, 2 * j / resolution))
                  for i in range(1, resolution) for j in range(1, resolution)]

    def distance_from_vertices(point):
        return min((point[0] - corner[0]) ** 2 + (point[1] - corner[1]) ** 2 for corner in corners)

    guide = max(candidates, key=distance_from_vertices)
    t.assertGreater(distance_from_vertices(guide), 20 ** 2)
    yield e.cursor_position_set(*guide, move=True)
    yield e.leftmouse()
    t.assertIn('Verts:1/', window.scene.statistics(window.view_layer))
    with bpy.context.temp_override(window=window, area=area, region=region):
        t.assertEqual(bpy.ops.view3d.snap_cursor_to_active(), {'FINISHED'})
    selected = tuple(round(value, 6) for value in window.scene.cursor.location)
    t.assertIn(selected, [(x, y, z) for x in (-1, 1) for y in (0, 2) for z in (0, 2)])
    yield e.alt.a()
    yield e.tab()
    yield e.tab()
    yield e.cursor_position_set(region.x + 20, region.y + 20, move=True)
    reentered = capture('reenter')
    error = sum(abs(a - b) for a, b in zip(after_delete, reentered)) / len(after_delete)
    t.assertLess(error, 0.005, 'Reentering Edit Mode lost the per-face display grid')
    print('NURBS_SURFACE_WIRE_GRID_PASS', label, window.scene.statistics(window.view_layer))


def surface_wire_grid_top_workflow():
    yield from surface_wire_grid_workflow(collapse_top=True)


def surface_wire_grid_solid_workflow():
    yield from surface_wire_grid_workflow(solid_wire=True)


def mesh_overlay_parity_workflow(custom_theme=False, tagged_entry=False):
    """Compare mesh shader colors and hidden-point depth behavior with the native source overlay."""
    import os
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    view = space.region_3d
    output = Path(os.environ.get('BLENDER_NURBS_OVERLAY_OUTPUT', '/tmp/blender-nurbs-overlay-ui'))
    output.mkdir(parents=True, exist_ok=True)
    if custom_theme:
        theme = bpy.context.preferences.themes[0].view_3d
        theme.wire_edit = (0.1, 0.8, 0.2)
        theme.edge_select = (0.9, 0.2, 0.3)
        theme.edge_mode_select = (0.1, 0.5, 0.95)
        theme.vertex_select = (0.8, 0.8, 0.1)
        theme.editmesh_active = (0.7, 0.2, 0.8, 1)
        theme.face_select = (0.2, 0.6, 0.8, 0.25)
        theme.face_mode_select = (0.8, 0.3, 0.2, 0.35)
    bpy.context.preferences.view.smooth_view = 0
    bpy.context.preferences.inputs.use_rotate_around_active = False
    space.overlay.show_text = False
    space.overlay.show_floor = False
    space.overlay.show_axis_x = False
    space.overlay.show_axis_y = False
    space.overlay.show_cursor = False
    space.show_gizmo = False
    points = [(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0),
              (-1, 0, 2), (1, 0, 2), (-1, 2, 2), (1, 2, 2)]
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4),
             (1, 3, 7, 5), (3, 2, 6, 7), (2, 0, 4, 6)]
    references = {}
    probes = {'HIDDEN_VERTEX': (-1, 2, 0), 'VISIBLE_VERTEX': (1, 0, 2),
              'FRONT_EDGE': (1, 0, 1), 'BACK_EDGE': (-1, 2, 1),
              'FACE': (0, 0, 1), 'UNSELECTED_EDGE': (-1, 1, 2)}

    def pixel(point):
        xy = location_3d_to_region_2d(region, view, Vector(point))
        t.assertIsNotNone(xy)
        return round(xy.x) + region.x, round(xy.y) + region.y

    def read_patches(path):
        image = bpy.data.images.load(str(path), check_existing=False)
        try:
            width, height = image.size
            pixels = tuple(image.pixels)
            result = {}
            for name, point in probes.items():
                x, y = pixel(point)
                patch = []
                for yy in range(y - 3, y + 4):
                    for xx in range(x - 3, x + 4):
                        t.assertTrue(0 <= xx < width and 0 <= yy < height)
                        index = (yy * width + xx) * 4
                        patch.extend(pixels[index:index + 3])
                result[name] = patch
            return result
        finally:
            bpy.data.images.remove(image)

    for kind in ('MESH', 'TAGGED' if tagged_entry else 'NATIVE'):
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        window.scene.cursor.location = (0, 0, 0)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        if kind == 'MESH':
            mesh = bpy.data.meshes.new('Overlay mesh reference')
            mesh.from_pydata(points, [], faces)
            obj = bpy.data.objects.new(mesh.name, mesh)
            window.scene.collection.objects.link(obj)
            obj.select_set(True)
            window.view_layer.objects.active = obj
            yield e.tab()
        else:
            yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
            yield e.tab()
            if not tagged_entry:
                yield e.two()
            yield e.e().y().text('2').ret()
            if not tagged_entry:
                yield e.three()
            yield e.a()
            yield e.e().z().text('2').ret()
            t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs, not tagged_entry)
        for mode in (('VERT',) if tagged_entry else ('VERT', 'EDGE', 'FACE')):
            yield e.cursor_position_set(*ui.get_area_center(area), move=True)
            view.view_rotation = Vector((4, -6, 3)).to_track_quat('Z', 'Y')
            view.view_perspective = 'ORTHO'
            view.view_location = (0, 1, 1)
            view.view_distance = 8
            if mode == 'VERT' and kind != 'TAGGED':
                yield e.one()
            elif mode == 'EDGE':
                yield e.two()
            elif mode == 'FACE':
                yield e.three()
            expected_mode = tuple(name == mode for name in ('VERT', 'EDGE', 'FACE'))
            if kind == 'MESH':
                t.assertEqual(tuple(window.scene.tool_settings.mesh_select_mode), expected_mode)
            else:
                t.assertEqual(tuple(window.view_layer.objects.active.data.nurbs_selection_mode), expected_mode)
            space.shading.show_xray = True
            space.shading.xray_alpha = 0.5
            yield e.alt.a()
            if mode == 'VERT':
                yield e.cursor_position_set(*pixel((1, 0, 2)), move=True)
                yield e.leftmouse()
                yield e.cursor_position_set(*pixel((-1, 2, 0)), move=True)
                yield e.shift.leftmouse()
            elif mode == 'EDGE':
                yield e.cursor_position_set(*pixel((1, 0, 1)), move=True)
                yield e.leftmouse()
            else:
                space.shading.show_xray = False
                yield e.cursor_position_set(*pixel((0, 0, 1)), move=True)
                yield e.leftmouse()
            # Compatibility geometry retains the legacy body renderer. Its X-Ray face opacity
            # differs; compare that renderer separately from the native overlay's full matrix.
            for xray in ((False,) if tagged_entry else (False, True)):
                space.shading.show_xray = xray
                yield e.cursor_position_set(region.x + region.width // 2, region.y + 30, move=True)
                prefix = 'custom-' if custom_theme else ''
                path = output / f'overlay-{prefix}{kind}-{mode}-{xray}.png'
                with bpy.context.temp_override(window=window, area=area, region=region):
                    bpy.ops.screen.screenshot(filepath=str(path))
                patches = read_patches(path)
                if kind == 'MESH':
                    references[(mode, xray)] = patches
                else:
                    for name, patch in patches.items():
                        expected = references[(mode, xray)][name]
                        error = sum(abs(a - b) for a, b in zip(patch, expected)) / len(patch)
                        print('NATIVE_OVERLAY_PIXEL_ERROR', mode, xray, name, error)
                        t.assertLess(error, 0.06, (mode, xray, name))
                t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
                if kind == 'TAGGED':
                    t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
                    t.assertTrue(window.view_layer.objects.active.data.has_tagged_nurbs)
                print('NATIVE_OVERLAY_PARITY_PASS', kind, mode, xray)


def mesh_overlay_custom_theme_workflow():
    yield from mesh_overlay_parity_workflow(custom_theme=True)


def mesh_overlay_tagged_workflow():
    """Untouched Add/E/E source uses the component overlay without changing source storage."""
    yield from mesh_overlay_parity_workflow(tagged_entry=True)


def selected_face_depth_workflow():
    """Every interior pixel of a selected face must receive the mesh selection tint."""
    import os
    from pathlib import Path
    import bpy
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    view = space.region_3d
    output = Path(os.environ.get('BLENDER_NURBS_OVERLAY_OUTPUT', '/tmp/blender-nurbs-overlay-ui'))
    output.mkdir(parents=True, exist_ok=True)
    bpy.context.preferences.view.smooth_view = 0
    bpy.context.preferences.inputs.use_rotate_around_active = False
    space.overlay.show_text = False
    space.overlay.show_floor = False
    space.overlay.show_axis_x = False
    space.overlay.show_axis_y = False
    space.overlay.show_cursor = False
    space.show_gizmo = False
    space.shading.show_xray = False
    points = [(-1, 0, 0), (1, 0, 0), (-1, 2, 0), (1, 2, 0),
              (-1, 0, 2), (1, 0, 2), (-1, 2, 2), (1, 2, 2)]
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4),
             (1, 3, 7, 5), (3, 2, 6, 7), (2, 0, 4, 6)]
    references = {}

    def pixel(point):
        xy = location_3d_to_region_2d(region, view, Vector(point))
        t.assertIsNotNone(xy)
        return round(xy.x) + region.x, round(xy.y) + region.y

    def capture(path):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(path))
        image = bpy.data.images.load(str(path), check_existing=False)
        try:
            return image.size[0], tuple(image.pixels)
        finally:
            bpy.data.images.remove(image)

    for kind in ('MESH_VERT', 'NATIVE_VERT', 'TAGGED_VERT', 'MESH_FACE', 'NATIVE_FACE'):
        if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
            yield e.tab()
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        window.scene.cursor.location = (0, 0, 0)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        mesh_source = kind.startswith('MESH')
        face_mode = kind.endswith('FACE')
        if mesh_source:
            mesh = bpy.data.meshes.new('Selected face depth reference')
            mesh.from_pydata(points, [], faces)
            obj = bpy.data.objects.new(mesh.name, mesh)
            window.scene.collection.objects.link(obj)
            obj.select_set(True)
            window.view_layer.objects.active = obj
            yield e.tab()
        else:
            yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
            yield e.tab()
            if kind != 'TAGGED_VERT':
                yield e.two()
            yield e.e().y().text('2').ret()
            yield e.a()
            yield e.e().z().text('2').ret()
        if kind != 'TAGGED_VERT':
            if face_mode:
                yield e.three()
            else:
                yield e.one()
        expected_mode = (False, False, True) if face_mode else (True, False, False)
        actual_mode = (window.scene.tool_settings.mesh_select_mode if mesh_source else
                       window.view_layer.objects.active.data.nurbs_selection_mode)
        t.assertEqual(tuple(actual_mode), expected_mode)
        view.view_rotation = Vector((4, -6, 6)).to_track_quat('Z', 'Y')
        view.view_perspective = 'ORTHO'
        view.view_location = (0, 1, 1)
        view.view_distance = 5
        yield e.alt.a()
        for point in ([(0, 1, 2)] if face_mode else points[4:]):
            yield e.cursor_position_set(*pixel(point), move=True)
            yield e.shift.leftmouse()
        if not mesh_source:
            t.assertEqual(window.view_layer.objects.active.data.is_native_nurbs,
                          kind != 'TAGGED_VERT')

        views = [(direction, distance)
                 for direction in ((4, -6, 3), (1.3, -1.8, 4.5), (-5, -3, 2))
                 for distance in (5, 3)]
        for angle, (direction, distance) in enumerate(views):
            for projection in ('PERSP', 'ORTHO'):
                view.view_rotation = Vector(direction).to_track_quat('Z', 'Y')
                view.view_perspective = projection
                # Aim at the selected face so close perspective views stay above its plane.
                view.view_location = (0, 1, 2)
                view.view_distance = distance
                images = []
                for selected_tint in (False, True):
                    space.overlay.show_faces = selected_tint
                    yield e.cursor_position_set(region.x + region.width // 2, region.y + 20, move=True)
                    width, pixels = capture(output / f'face-depth-{kind}-{angle}-{projection}-{selected_tint}.png')
                    images.append(pixels)
                key = (face_mode, angle, projection)
                if mesh_source:
                    polygon = [pixel(p) for p in ((-.85, .15, 2), (.85, .15, 2),
                                                 (.85, 1.85, 2), (-.85, 1.85, 2))]
                    reference = []
                    for y in range(max(region.y + 10, min(p[1] for p in polygon)),
                                   min(region.y + region.height - 10, max(p[1] for p in polygon)) + 1):
                        for x in range(max(region.x + 10, min(p[0] for p in polygon)),
                                       min(region.x + region.width - 10, max(p[0] for p in polygon)) + 1):
                            crosses = [(b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])
                                       for a, b in zip(polygon, polygon[1:] + polygon[:1])]
                            if not (all(c >= 0 for c in crosses) or all(c <= 0 for c in crosses)):
                                continue
                            i = (y * width + x) * 4
                            delta = tuple(images[1][i + c] - images[0][i + c] for c in range(3))
                            length_squared = sum(d * d for d in delta)
                            if length_squared > 0.0004:
                                reference.append((i, delta, length_squared))
                    t.assertGreater(len(reference), 1000, (kind, angle, projection))
                    references[key] = reference
                else:
                    reference = references[key]
                    missed = 0
                    for i, expected, length_squared in reference:
                        delta = tuple(images[1][i + c] - images[0][i + c] for c in range(3))
                        strength = sum(a * b for a, b in zip(delta, expected)) / length_squared
                        missed += strength < 0.8
                    coverage = 1 - missed / len(reference)
                    print('SELECTED_FACE_DEPTH_COVERAGE', kind, angle, projection,
                          coverage, missed, len(reference), flush=True)
                    t.assertGreaterEqual(coverage, 0.995, (kind, angle, projection, missed))
    print('SELECTED_FACE_DEPTH_WORKFLOW_PASS')


def distance_merge_native_workflow():
    """M / By Distance welds duplicated source faces, retains editing mode and supports undo/E."""
    import os
    import re
    from pathlib import Path
    import bpy
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    output = Path(os.environ.get('BLENDER_NURBS_OVERLAY_OUTPUT', '/tmp/blender-nurbs-overlay-ui'))
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    window.scene.cursor.location = (0, 0, 0)
    window.scene.tool_settings.use_snap = False
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    yield e.two()
    yield e.e().y().text('2').ret()
    yield e.three()
    yield e.a()
    yield e.shift.d().x().text('0.00001').ret()
    yield e.one()
    yield e.a()

    def totals():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    t.assertEqual(totals(), (8, 8, 2))
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.m()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.screen.screenshot(filepath=str(output / 'merge-menu-with-distance.png'))
    center = ui.get_area_center(area)
    row_height = 20 * bpy.context.preferences.system.ui_scale
    yield e.cursor_position_set(center[0] - 60, round(center[1] - 3 * row_height - 6), move=True)
    yield e.leftmouse()
    t.assertEqual(totals(), (4, 4, 1))
    t.assertEqual(window.view_layer.objects.active.mode, 'EDIT')
    t.assertEqual(tuple(window.view_layer.objects.active.data.nurbs_selection_mode), (True, False, False))
    yield e.ctrl.z()
    t.assertEqual(totals(), (8, 8, 2))
    yield e.ctrl.shift.z()
    t.assertEqual(totals(), (4, 4, 1))
    yield e.three()
    yield e.a()
    yield e.e().z().text('1').ret()
    t.assertEqual(totals(), (8, 12, 6))
    t.assertEqual(len(window.view_layer.objects.active.data.splines), 0)
    print('NATIVE_DISTANCE_MERGE_MENU_UNDO_CONTINUE_PASS')


def pyramid_movement_workflow(mesh_reference=False, box_selection=False):
    """G/R/S after Merge must deform the same source components as a mesh pyramid."""
    import os
    import re
    from pathlib import Path
    import bpy
    import bmesh
    from mathutils import Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_TRIANGLE_OUTPUT', '/tmp/blender-nurbs-triangle-ui'))
    output.mkdir(parents=True, exist_ok=True)
    label = 'mesh' if mesh_reference else 'box' if box_selection else 'native'
    if window.view_layer.objects.active and window.view_layer.objects.active.mode == 'EDIT':
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
    if mesh_reference:
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        bpy.ops.mesh.primitive_cube_add(size=2, location=(0, 0, 1))
        bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.select_all(action='DESELECT')
        bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
        for vertex in bm.verts:
            vertex.select_set(vertex.co.z == 2)
        bmesh.update_edit_mesh(window.view_layer.objects.active.data)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.merge('EXEC_DEFAULT', True, type='CENTER')
    else:
        yield from merge_cube_top_workflow()

    bpy.context.preferences.view.smooth_view = 0
    window.scene.tool_settings.use_snap = False
    window.scene.tool_settings.transform_pivot_point = 'MEDIAN_POINT'
    space.shading.show_xray = False
    yield e.one()
    yield e.numpad_1()
    yield e.numpad_6()
    yield e.numpad_8()
    space.region_3d.view_location = (0, 0, 1)
    space.region_3d.view_distance = 8
    yield e.alt.a()

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def screen(point):
        xy = location_3d_to_region_2d(region, space.region_3d, Vector(point))
        t.assertIsNotNone(xy)
        return round(xy.x) + region.x, round(xy.y) + region.y

    def pick(point, vertices):
        yield e.alt.a()
        x, y = screen(point)
        if box_selection:
            yield e.b()
            yield from e.leftmouse.cursor_motion(((x - 5, y - 5), (x + 5, y + 5)))
        else:
            yield e.cursor_position_set(x, y, move=True)
            yield e.leftmouse()
        t.assertEqual(counts()[0], vertices)

    def save(stage):
        obj = window.view_layer.objects.active
        t.assertEqual(obj.mode, 'EDIT')
        if not mesh_reference:
            t.assertTrue(obj.data.is_native_nurbs)
            t.assertEqual(len(obj.data.splines), 0)
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                t.assertIsNotNone(mesh)
                t.assertGreater(len(mesh.polygons), 0)
            finally:
                evaluated.to_mesh_clear()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'triangle-{label}-{stage}.blend'),
                                      check_existing=False)
            bpy.ops.screen.screenshot(filepath=str(output / f'triangle-{label}-{stage}.png'))

    t.assertEqual(counts()[1::2], (5, 8, 5))
    save('initial')
    yield from pick((0, 0, 2), 1)
    yield e.g().x().text('0.5').ret()
    save('apex-move')
    yield e.ctrl.z()
    yield e.ctrl.shift.z()
    yield e.g().z().text('1').esc()
    save('apex-restored')
    yield from pick((-1, -1, 0), 1)
    yield e.g().z().text('0.25').ret()
    save('base-move')
    yield e.two()
    yield from pick((0, -1, 0.125), 2)
    yield e.g().x().text('0.25').ret()
    yield e.s().z().text('2').ret()
    yield e.r().z().text('15').ret()
    t.assertEqual(counts()[1::2], (5, 8, 5))
    save('edge-transform')
    yield e.three()
    # The rotated edge's midpoint remains (0.25, -1, 0.125).
    yield from pick(((0.25 + 0.25 + 0.5) / 3, -2 / 3, (0.375 - 0.125 + 2) / 3), 3)
    yield e.g().y().text('-0.25').ret()
    save('face-move')
    yield e.e().y().text('-0.5').ret()
    t.assertEqual(counts()[1::2], (8, 14, 8))
    save('extrude')
    yield e.g().x().text('0.125').ret()
    yield e.ctrl.z()
    yield e.tab()
    yield e.tab()
    t.assertEqual(counts()[1::2], (8, 14, 8))
    save('reload')
    import subprocess
    child = subprocess.run(
        [bpy.app.binary_path, '--background', '--factory-startup',
         str(output / f'triangle-{label}-reload.blend'), '--python-exit-code', '1',
         '--python', str(Path(__file__).resolve().parents[1] / 'nurbs_native_triangle_reload.py')],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60,
    )
    t.assertEqual(child.returncode, 0, child.stdout)
    t.assertIn('TRIANGLE_FRESH_MOVE_PASS', child.stdout)
    print('NURBS_TRIANGLE_MOVEMENT_PASS', label, counts())


def pyramid_movement_mesh_workflow():
    yield from pyramid_movement_workflow(mesh_reference=True)


def pyramid_movement_box_workflow():
    yield from pyramid_movement_workflow(box_selection=True)


def pyramid_shading_workflow(mesh_reference=False, move_amount='0.5'):
    """A merged/moved planar pyramid must shade like its actual Mesh counterpart."""
    import os
    from pathlib import Path
    import bpy
    import bmesh

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_SHADING_OUTPUT', '/tmp/blender-nurbs-shading-ui'))
    output.mkdir(parents=True, exist_ok=True)
    label = 'mesh' if mesh_reference else 'native'
    if move_amount != '0.5':
        label += '-decimal'
    if mesh_reference:
        for obj in tuple(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        bpy.ops.mesh.primitive_cube_add(size=2, location=(0, 0, 1))
        bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.tab()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.select_all(action='DESELECT')
        bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
        for vertex in bm.verts:
            vertex.select_set(vertex.co.z == 2)
        bmesh.update_edit_mesh(window.view_layer.objects.active.data)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.merge('EXEC_DEFAULT', True, type='CENTER')
    else:
        yield from merge_cube_top_workflow()
    yield e.one()
    yield e.numpad_1()
    yield e.numpad_6()
    yield e.numpad_8()
    space.region_3d.view_location = (0.2, 0, 1)
    space.region_3d.view_distance = 3.5
    space.shading.show_xray = False
    yield e.g().x().text(move_amount).ret()

    def save(stage):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'pyramid-shading-{label}-{stage}.blend'),
                                      check_existing=False)
            bpy.ops.screen.screenshot(filepath=str(output / f'pyramid-shading-{label}-{stage}.png'))

    def check_normals():
        obj = window.view_layer.objects.active
        t.assertEqual(obj.mode, 'EDIT')
        if not mesh_reference:
            t.assertTrue(obj.data.is_native_nurbs)
            t.assertEqual(len(obj.data.splines), 0)
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            t.assertIsNotNone(mesh)
            t.assertGreater(len(mesh.polygons), 0)
            zero_area = sum(p.area == 0 for p in mesh.polygons)
            normals = mesh.corner_normals
            dots = [normals[i].vector.dot(p.normal) for p in mesh.polygons
                    for i in p.loop_indices if p.area != 0]
            lengths = [normal.vector.length for normal in normals]
            print('PYRAMID_SHADING_METRICS', label, len(mesh.polygons), zero_area,
                  min(dots), min(lengths), max(lengths))
            t.assertEqual(zero_area, 0)
            t.assertGreater(min(dots), 0.9999)
            for length in lengths:
                t.assertAlmostEqual(length, 1, delta=1e-4)
        finally:
            evaluated.to_mesh_clear()

    save('moved')
    check_normals()
    yield e.ctrl.z()
    check_normals()
    yield e.ctrl.shift.z()
    check_normals()
    yield e.tab()
    yield e.tab()
    check_normals()
    save('restored')
    print('NURBS_PYRAMID_SHADING_PASS', label)


def pyramid_shading_mesh_workflow():
    yield from pyramid_shading_workflow(mesh_reference=True)


def pyramid_decimal_move_workflow():
    yield from pyramid_shading_workflow(move_amount='0.3')


def pyramid_decimal_move_mesh_workflow():
    yield from pyramid_shading_workflow(mesh_reference=True, move_amount='0.3')


def bevel_corner_workflow(closed=False, rounded=False, mesh_reference=False, vertex_mode=False,
                          mouse_input=False, mouse_sweep=False, face_depth=2.0, wall_height=2.0,
                          mouse_limits=False, rotated=False, split_deformation=False,
                          clamp_test=False, curved=False):
    """Extruded corner -> Ctrl+B preview/cancel/confirm -> undo/reload/continued extrusion."""
    import math
    import os
    import re
    from pathlib import Path
    import bpy
    from mathutils import Matrix, Vector
    from bpy_extras.view3d_utils import location_3d_to_region_2d

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    output = Path(os.environ.get('BLENDER_NURBS_BEVEL_OUTPUT', '/tmp/blender-nurbs-bevel-ui'))
    output.mkdir(parents=True, exist_ok=True)
    label = ('mesh' if mesh_reference else 'native') + ('-box' if closed else '-corner')
    if curved:
        label += '-curved'
    if rounded:
        label += '-round'
    if vertex_mode:
        label += '-vertex'
    if mouse_input:
        label += '-mouse'
    if mouse_sweep:
        label += '-sweep'
    if mouse_limits:
        label += '-limits'
    if clamp_test:
        label += '-clamped'
    if rotated:
        label += '-rotated'
    if split_deformation:
        label += '-split-deform'
    if face_depth != 2.0 or wall_height != 2.0:
        label += '-decimal'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    window.scene.cursor.location = (0, 0, 0)
    window.scene.tool_settings.use_snap = False
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    if mesh_reference:
        mesh = bpy.data.meshes.new('Bevel mesh reference')
        mesh.from_pydata([(-1, 0, 0), (1, 0, 0), (-1, face_depth, 0), (1, face_depth, 0),
                          (-1, face_depth, wall_height), (1, face_depth, wall_height)], [], [(0, 1, 3, 2), (2, 3, 5, 4)])
        obj = bpy.data.objects.new(mesh.name, mesh)
        window.scene.collection.objects.link(obj)
        obj.select_set(True)
        window.view_layer.objects.active = obj
        yield e.tab()
        yield e.two()
    else:
        yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        yield e.tab()
        yield e.two()
        yield e.e().y().text(str(face_depth)).ret()
        if closed:
            yield e.a()
            yield e.three()
        yield e.e().z().text(str(wall_height)).ret()
        yield e.two()
    obj = window.view_layer.objects.active
    rotation_degrees = 21.256
    if rotated:
        # Rotate the source coefficients around the cursor. This introduces the same tiny
        # non-affine roundoff that can appear after ordinary transforms in a user scene.
        window.scene.tool_settings.transform_pivot_point = 'CURSOR'
        window.scene.cursor.location = (0, 0, 0)
        yield e.a()
        yield e.r().z().text(str(rotation_degrees)).ret()
        window.scene.tool_settings.transform_pivot_point = 'MEDIAN_POINT'
    yield e.numpad_1()
    yield e.numpad_6()
    yield e.numpad_8()
    space.region_3d.view_location = (0, 1.2, 0.7)
    space.region_3d.view_distance = 6
    space.shading.show_xray = False
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)

    def counts():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def display_positions():
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        t.assertIsNotNone(mesh)
        try:
            return tuple(tuple(vertex.co) for vertex in mesh.vertices)
        finally:
            evaluated.to_mesh_clear()

    def display_width():
        # On this L-shaped source the base face ends at Y = face_depth - width.
        # Measure its visible rail as well as counting topology, so a frozen preview fails.
        inverse_rotation = Matrix.Identity(4)
        if rotated:
            inverse_rotation = Matrix.Rotation(-math.radians(rotation_degrees), 4, 'Z')
        base = [(inverse_rotation @ Vector(point)).y for point in display_positions()
                if abs((inverse_rotation @ Vector(point)).z) < 1e-7]
        t.assertTrue(base)
        return face_depth - max(base)

    def mode():
        return tuple(window.scene.tool_settings.mesh_select_mode if mesh_reference else obj.data.nurbs_selection_mode)

    def save(stage):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'bevel-{label}-{stage}.blend'), check_existing=False)

    def screenshot(stage):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'bevel-{label}-{stage}.png'))

    yield e.alt.a()
    if curved:
        if closed:
            space.shading.show_xray = True
        yield e.one()
        for position, axis, distance in (((-1, 0, 0), 'z', '0.4'),
                                          ((1, face_depth, wall_height), 'y', '0.5')):
            point = location_3d_to_region_2d(region, space.region_3d, Vector(position))
            t.assertIsNotNone(point)
            yield e.cursor_position_set(round(point.x) + region.x,
                                        round(point.y) + region.y, move=True)
            yield e.leftmouse()
            t.assertEqual(counts()[0], 1)
            yield e.g()
            yield getattr(e, axis)()
            yield e.text(distance).ret()
            yield e.alt.a()
        yield e.two()
    target = Vector((0, 0, wall_height)) if closed and not curved else Vector((0, face_depth, 0))
    if rotated:
        target = Matrix.Rotation(math.radians(rotation_degrees), 4, 'Z') @ target
    co = location_3d_to_region_2d(region, space.region_3d, target)
    t.assertIsNotNone(co)
    yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
    yield e.leftmouse()
    if curved and closed:
        space.shading.show_xray = False
    if vertex_mode:
        yield e.one()
    before = counts()
    t.assertEqual(before[0], 2)
    t.assertEqual(before[2], 1)
    before_mode = mode()
    if not mesh_reference:
        with bpy.context.temp_override(window=window, area=area, region=region):
            t.assertEqual(bpy.ops.curve.bevel('EXEC_DEFAULT', offset=0), {'CANCELLED'})
        t.assertEqual(counts(), before)
        t.assertEqual(mode(), before_mode)
    save('before')
    yield e.ctrl.b().text('0.25')
    t.assertEqual(obj.mode, 'EDIT')
    t.assertEqual(mode(), before_mode)
    t.assertEqual(counts()[1::2], tuple(a + b for a, b in zip(before[1::2], (2, 3, 1))))
    screenshot('preview')
    yield e.esc()
    t.assertEqual(counts(), before)
    t.assertEqual(mode(), before_mode)
    save('cancelled')
    if not mesh_reference:
        # Explicitly disable the native default clamp to exercise rejected-width behavior.
        yield e.ctrl.b().c().text('3')
        t.assertEqual(counts(), before)
        yield e.esc()
        t.assertEqual(counts(), before)
    yield e.ctrl.b()
    if mouse_input:
        if clamp_test:
            if mesh_reference:
                # Mesh Edit Mode defaults to Clamp Overlap off.
                yield e.c()
            if rounded:
                yield e.wheelupmouse().wheelupmouse()
            segments = 3 if rounded else 1
            expected = tuple(a + b for a, b in zip(before[1::2],
                                                  (2 * segments, 3 * segments, segments)))
            plateau = None
            for pixels in (16, 240, 280, 24, 260, 32, 300):
                yield e.cursor_position_set(round(co.x) + region.x + pixels,
                                            round(co.y) + region.y, move=True)
                t.assertEqual(counts()[1::2], expected)
                width = display_width()
                print('NURBS_BEVEL_CLAMP', label, pixels, width, flush=True)
                if pixels >= 240:
                    t.assertAlmostEqual(width, min(face_depth, wall_height), delta=1e-6)
                    current = display_positions()
                    if plateau is not None:
                        t.assertEqual(current, plateau,
                                      'The clamped result depends on earlier mouse positions')
                    plateau = current
                    save('clamped')
                else:
                    t.assertLess(width, min(face_depth, wall_height))
                t.assertEqual(mode(), before_mode)
            screenshot('clamped')
            if not mesh_reference:
                # C must disable and re-enable the actual geometry limit, rather than
                # silently accepting a failed request or clearing the accepted preview.
                yield e.c()
                t.assertEqual(display_positions(), plateau)
                yield e.c()
                t.assertEqual(display_positions(), plateau)
            yield e.esc()
            t.assertEqual(counts(), before)
            save('clamp-cancelled')
            yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y,
                                        move=True)
            yield e.ctrl.b()
            if mesh_reference:
                yield e.c()
            if rounded:
                yield e.wheelupmouse().wheelupmouse()
            yield e.cursor_position_set(round(co.x) + region.x + 280,
                                        round(co.y) + region.y, move=True)
            t.assertEqual(counts()[1::2], expected)
            t.assertAlmostEqual(display_width(), min(face_depth, wall_height), delta=1e-6)
            yield e.ret()
            t.assertEqual(counts()[1::2], expected)
            save('clamp-confirmed')
            yield e.ctrl.z()
            obj = window.view_layer.objects.active
            t.assertEqual(counts(), before)
            yield e.ctrl.shift.z()
            obj = window.view_layer.objects.active
            t.assertEqual(counts()[1::2], expected)
            yield e.tab().tab()
            obj = window.view_layer.objects.active
            t.assertEqual(counts()[1::2], expected)
            save('clamp-restored')
            # Continue editing the actual limiting result, including its very narrow
            # retained faces. Undoing across a Tab transition would test mode undo instead.
            yield e.three()
            yield e.e().esc()
            yield e.g().z().text('0.5').ret()
            t.assertEqual(obj.mode, 'EDIT')
            t.assertGreater(counts()[5], expected[2])
            save('clamp-continued')
            screenshot('clamp-continued')
            print('NURBS_BEVEL_CLAMP_PASS', label, counts(), flush=True)
            return
        if mouse_limits:
            yield e.c()
            if rounded:
                yield e.wheelupmouse().wheelupmouse()
            segments = 3 if rounded else 1
            expected = tuple(a + b for a, b in zip(before[1::2],
                                                  (2 * segments, 3 * segments, segments)))
            previous_valid = None
            for pixels in (16, 240, 24, 260, 32, 280, 40):
                yield e.cursor_position_set(round(co.x) + region.x + pixels,
                                            round(co.y) + region.y, move=True)
                actual = counts()[1::2]
                print('NURBS_BEVEL_MOUSE_LIMIT', label, pixels, actual, flush=True)
                if actual != expected:
                    screenshot('limits-failed')
                    yield e.ret()
                t.assertEqual(actual, expected, f'Preview disappeared at mouse offset {pixels}')
                if pixels >= 240:
                    t.assertEqual(display_positions(), previous_valid)
                else:
                    current = display_positions()
                    if previous_valid is not None:
                        t.assertNotEqual(current, previous_valid)
                    previous_valid = current
                if pixels == 16:
                    save('last-valid')
                    # Mesh bevel ignores queued intermediate moves. A stale zero-width event
                    # must not reset this accepted preview while the cursor is being dragged.
                    window.event_simulate(type='INBETWEEN_MOUSEMOVE', value='NOTHING',
                                          x=round(co.x) + region.x,
                                          y=round(co.y) + region.y)
                    yield
                    t.assertEqual(counts()[1::2], expected)
                elif pixels == 240:
                    save('held-invalid')
                    screenshot('held-invalid')
            # Zero is an explicit request to show the original, even after a held preview.
            yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
            t.assertEqual(counts(), before)
            yield e.cursor_position_set(round(co.x) + region.x + 24,
                                        round(co.y) + region.y, move=True)
            t.assertEqual(counts()[1::2], expected)
            yield e.cursor_position_set(round(co.x) + region.x + 260,
                                        round(co.y) + region.y, move=True)
            yield e.esc()
            t.assertEqual(counts(), before)
            save('held-cancelled')
            yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
            yield e.ctrl.b().c()
            if rounded:
                yield e.wheelupmouse().wheelupmouse()
            yield e.cursor_position_set(round(co.x) + region.x + 24,
                                        round(co.y) + region.y, move=True)
            yield e.cursor_position_set(round(co.x) + region.x + 260,
                                        round(co.y) + region.y, move=True)
            # A displayed previous width must not silently accept the rejected request.
            yield e.ret()
            t.assertEqual(counts(), before)
            save('rejected')
            # Dismiss the confirmation error report before starting another operator.
            yield e.esc()
            yield e.cursor_position_set(round(co.x) + region.x, round(co.y) + region.y, move=True)
            yield e.ctrl.b().c()
            if rounded:
                yield e.wheelupmouse().wheelupmouse()
        if mouse_sweep:
            if rounded:
                yield e.wheelupmouse().wheelupmouse()
            segments = 3 if rounded else 1
            expected = tuple(a + b for a, b in zip(before[1::2],
                                                  (2 * segments, 3 * segments, segments)))
            widths = {}
            for pixels in (*range(8, 65), *range(63, 7, -1)):
                yield e.cursor_position_set(round(co.x) + region.x + pixels,
                                            round(co.y) + region.y, move=True)
                actual = counts()[1::2]
                print('NURBS_BEVEL_SWEEP', label, pixels, actual, flush=True)
                if actual != expected:
                    screenshot('sweep-failed')
                    yield e.ret()
                t.assertEqual(actual, expected, f'Bevel disappeared at mouse offset {pixels}')
                width = display_width()
                t.assertGreater(width, 0)
                if pixels in widths:
                    t.assertAlmostEqual(width, widths[pixels], delta=1e-6)
                else:
                    if widths:
                        t.assertGreater(width, widths[pixels - 1] + 1e-7)
                    widths[pixels] = width
        yield e.cursor_position_set(round(co.x) + region.x + 35, round(co.y) + region.y, move=True)
    else:
        yield e.text('0.25')
    if rounded and not (mouse_sweep or mouse_limits):
        yield e.wheelupmouse()
        yield e.wheelupmouse()
    yield e.ret()
    after = counts()
    segments = 3 if rounded else 1
    t.assertEqual(after[1::2], tuple(a + b for a, b in zip(before[1::2], (2 * segments, 3 * segments, segments))))
    t.assertEqual(obj.mode, 'EDIT')
    t.assertEqual(mode(), before_mode)
    screenshot('finished')
    save('finished')
    yield e.ctrl.z()
    obj = window.view_layer.objects.active
    t.assertEqual(counts(), before)
    t.assertEqual(obj.mode, 'EDIT')
    yield e.ctrl.shift.z()
    obj = window.view_layer.objects.active
    t.assertEqual(counts(), after)
    t.assertEqual(mode(), before_mode)
    yield e.tab()
    yield e.tab()
    obj = window.view_layer.objects.active
    t.assertEqual(counts(), after)
    if not mesh_reference:
        t.assertTrue(obj.data.is_native_nurbs)
        t.assertEqual(len(obj.data.splines), 0)
    save('restored')
    if split_deformation:
        def click(position):
            point = location_3d_to_region_2d(region, space.region_3d, Vector(position))
            t.assertIsNotNone(point)
            yield e.cursor_position_set(round(point.x) + region.x,
                                        round(point.y) + region.y, move=True)
            yield e.leftmouse()

        def has_display_point(position):
            return min((Vector(point) - Vector(position)).length
                       for point in display_positions()) < 1e-6

        yield e.three()
        yield e.alt.a()
        yield from click((0, 0.5, 0))
        t.assertEqual(counts()[4], 1)
        yield e.y()
        yield e.g().z().text('1').ret()
        yield e.one()
        yield e.alt.a()
        yield from click((-1, 0, 1))
        t.assertEqual(counts()[0], 1)
        before_deform = counts()
        t.assertTrue(has_display_point((-1, 0, 1)))
        save('split-before-deform')
        yield e.g().z().text('0.375').ret()
        t.assertEqual(counts(), before_deform)
        t.assertTrue(has_display_point((-1, 0, 1.375)),
                     'The detached trimmed face did not deform with its selected corner')
        t.assertEqual(obj.mode, 'EDIT')
        save('split-vertex-moved')
        screenshot('split-vertex-moved')
        yield e.ctrl.z()
        obj = window.view_layer.objects.active
        t.assertTrue(has_display_point((-1, 0, 1)))
        yield e.ctrl.shift.z()
        obj = window.view_layer.objects.active
        t.assertTrue(has_display_point((-1, 0, 1.375)))
        yield e.g().z().text('2').esc()
        t.assertTrue(has_display_point((-1, 0, 1.375)))
        yield e.tab()
        yield e.tab()
        obj = window.view_layer.objects.active
        t.assertTrue(has_display_point((-1, 0, 1.375)))
        save('split-restored')
        print('NURBS_BEVEL_SPLIT_DEFORM_PASS', label, counts())
        return
    # The bevel's selected region must remain usable by the shared extrusion pipeline.
    yield e.three()
    # Mesh E initially constrains to the face normal; native E does not yet do that.
    # Cancel only the move, then request the same explicit global displacement on both.
    yield e.e().esc()
    yield e.g().z().text('0.5').ret()
    t.assertEqual(obj.mode, 'EDIT')
    t.assertGreater(counts()[5], after[5])
    save('continued')
    screenshot('continued')
    print('NURBS_BEVEL_WORKFLOW_PASS', label, before, after, counts())


def bevel_box_workflow():
    yield from bevel_corner_workflow(closed=True)


def bevel_round_workflow():
    yield from bevel_corner_workflow(rounded=True)


def bevel_curved_workflow():
    yield from bevel_corner_workflow(curved=True)


def bevel_curved_round_workflow():
    yield from bevel_corner_workflow(curved=True, rounded=True)


def bevel_curved_round_box_workflow():
    yield from bevel_corner_workflow(closed=True, curved=True, rounded=True)


def bevel_freehand_box_workflow(vertex_start=False):
    """Use mouse translations, rather than binary-friendly numeric extrusion fixtures."""
    import os
    import re
    from pathlib import Path
    import bpy
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    output = Path(os.environ.get('BLENDER_NURBS_BEVEL_OUTPUT', '/tmp/blender-nurbs-freehand-bevel'))
    output.mkdir(parents=True, exist_ok=True)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    center = ui.get_area_center(area)
    yield e.cursor_position_set(*center, move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab()
    if not vertex_start:
        yield e.two()
    yield e.e()
    yield e.cursor_position_set(center[0] + 90, center[1] + 90, move=True)
    yield e.leftmouse()
    if vertex_start:
        t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
        t.assertEqual(len(window.view_layer.objects.active.data.splines), 0)
        yield e.ctrl.z()
        t.assertFalse(window.view_layer.objects.active.data.is_native_nurbs)
        t.assertEqual(len(window.view_layer.objects.active.data.splines), 1)
        yield e.ctrl.shift.z()
        t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
        yield e.a()
    else:
        yield e.three().a()
    yield e.e()
    yield e.cursor_position_set(center[0] + 140, center[1] - 50, move=True)
    yield e.leftmouse()
    if vertex_start:
        yield e.three()
    obj = window.view_layer.objects.active

    def totals():
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)',
                          window.scene.statistics(window.view_layer))
        t.assertIsNotNone(match)
        return tuple(map(int, match.groups()))[1::2]

    t.assertEqual(totals(), (8, 12, 6))
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / 'freehand-box-before.blend'), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / 'freehand-box-before.png'))
    yield e.ctrl.b().text('0.1')
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.screen.screenshot(filepath=str(output / 'freehand-box-preview.png'))
    t.assertEqual(totals(), (12, 20, 10))
    yield e.ret()
    t.assertEqual(totals(), (12, 20, 10))
    t.assertEqual(obj.mode, 'EDIT')
    yield e.ctrl.z()
    t.assertEqual(totals(), (8, 12, 6))
    yield e.ctrl.shift.z()
    t.assertEqual(totals(), (12, 20, 10))
    if vertex_start:
        def preview_positions():
            window.view_layer.update()
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                return tuple(tuple(vertex.co) for vertex in mesh.vertices)
            finally:
                evaluated.to_mesh_clear()

        yield e.a()
        before_move = preview_positions()
        yield e.cursor_position_set(*center, move=True)
        yield e.g()
        yield e.cursor_position_set(center[0] + 55, center[1] + 30, move=True)
        moved = preview_positions()
        t.assertEqual(len(moved), len(before_move))
        t.assertGreater(max(sum((a - b) ** 2 for a, b in zip(p, q))
                            for p, q in zip(moved, before_move)), 1e-6)
        yield e.esc()
        t.assertEqual(preview_positions(), before_move)
        yield e.g().x().text('0.2').ret()
        t.assertNotEqual(preview_positions(), before_move)
        yield e.ctrl.z()
        t.assertEqual(preview_positions(), before_move)
        yield e.ctrl.shift.z()
        t.assertNotEqual(preview_positions(), before_move)
    print('NURBS_FREEHAND_BEVEL_PASS', flush=True)


def bevel_vertex_freehand_box_workflow():
    yield from bevel_freehand_box_workflow(vertex_start=True)


def bevel_round_box_workflow():
    yield from bevel_corner_workflow(closed=True, rounded=True)


def bevel_vertex_workflow():
    yield from bevel_corner_workflow(vertex_mode=True)


def bevel_mesh_workflow():
    yield from bevel_corner_workflow(mesh_reference=True)


def bevel_mouse_workflow():
    yield from bevel_corner_workflow(mouse_input=True)


def bevel_mesh_mouse_workflow():
    yield from bevel_corner_workflow(mesh_reference=True, mouse_input=True)


def bevel_connected_edges_workflow(mesh_reference=False, pyramid=False, rounded=False,
                                   rotated=False, clamped=False, skewed=False, face_selection=False,
                                   tapered=False, mouse_input=False, hold_launch_ctrl=False,
                                   remap_snap=False, release_launch_ctrl=False, segments=3):
    """A -> Ctrl+B on an extruded box must bevel its connected edge network."""
    import os
    import re
    from pathlib import Path
    import bpy
    import math

    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    output = Path(os.environ.get('BLENDER_NURBS_BEVEL_OUTPUT', '/tmp/blender-nurbs-bevel-ui'))
    output.mkdir(parents=True, exist_ok=True)
    if remap_snap:
        keymap = bpy.context.window_manager.keyconfigs.user.keymaps['Bevel Modal Map']
        changed = 0
        for item in keymap.keymap_items:
            if item.propvalue in {'SNAP_ON', 'SNAP_OFF'} and item.type == 'LEFT_CTRL':
                item.type = 'LEFT_ALT'
                changed += 1
        t.assertEqual(changed, 2)
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    if mesh_reference:
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.mesh.primitive_cube_add()
        for vertex in window.view_layer.objects.active.data.vertices:
            if skewed:
                vertex.co.y += vertex.co.x * math.sin(math.radians(15))
                vertex.co.x *= math.cos(math.radians(15))
            vertex.co.y += 1
            vertex.co.z += 1
            if skewed and vertex.co.z > 1:
                vertex.co.x += 0.3
                vertex.co.y -= 0.2
            if tapered and vertex.co.z > 1:
                vertex.co.x *= 1.3
        yield e.tab()
        yield e.two()
        if pyramid:
            import bmesh
            yield e.one()
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.mesh.select_all(action='DESELECT')
            bm = bmesh.from_edit_mesh(window.view_layer.objects.active.data)
            for vertex in bm.verts:
                vertex.select_set(vertex.co.z == 2)
            bmesh.update_edit_mesh(window.view_layer.objects.active.data)
            del vertex, bm
    else:
        yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
        yield e.tab()
        yield e.two()
        if skewed:
            yield e.r().z().text('15').ret()
        yield e.e().y().text('2').ret()
        yield e.a().three()
        yield e.e().z().text('2').ret()
        if skewed:
            yield e.g().x().text('0.3').ret().g().y().text('-0.2').ret()
        if tapered:
            yield e.s().x().text('1.3').ret()
    if pyramid:
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.m()
        center = ui.get_area_center(area)
        yield e.cursor_position_set(center[0] - 60, center[1], move=True)
        yield e.leftmouse()
    if not mesh_reference or pyramid:
        yield e.two()
    obj = window.view_layer.objects.active
    yield e.a()
    if rotated:
        yield e.r().z().text('21.256').ret()
    yield e.numpad_1().numpad_6().numpad_8()
    area.spaces.active.region_3d.view_distance = 7
    if face_selection:
        from bpy_extras.view3d_utils import location_3d_to_region_2d
        from mathutils import Vector
        yield e.three().alt.a()
        xy = location_3d_to_region_2d(region, area.spaces.active.region_3d,
                                     Vector((0.3, 0.8, 2)) if skewed else Vector((0, 1, 2)))
        yield e.cursor_position_set(int(region.x + xy.x), int(region.y + xy.y), move=True)
        yield e.leftmouse()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    label = 'mesh' if mesh_reference else 'native'
    shape = 'pyramid' if pyramid else 'connected'
    if rounded:
        shape += '-round'
    if rotated:
        shape += '-rotated'
    if clamped:
        shape += '-clamped'
    if skewed:
        shape += '-skewed'
    if tapered:
        shape += '-tapered'
    if face_selection:
        shape += '-face'
    if hold_launch_ctrl:
        shape += '-launch-ctrl'
    if remap_snap:
        shape += '-snap-alt'
    if release_launch_ctrl:
        shape += '-released'
    before = (5, 8, 5) if pyramid else (8, 12, 6)
    after = (16, 32, 18) if pyramid else (24, 48, 26)
    half, odd = divmod(segments, 2)
    corner_interior = 3 * half * half if odd else 3 * half * (half - 1) + 1
    corner_faces = 3 * half * (half + odd) + odd
    rounded_counts = (24 * segments + 8 * corner_interior,
                      12 * (segments + 1) ** 2,
                      12 * segments + 6 + 8 * corner_faces)
    if rounded:
        after = rounded_counts
    if face_selection:
        after = (20, 36, 18) if rounded else (12, 20, 10)
    if segments != 3:
        t.assertTrue(rounded and not pyramid and not face_selection)
        after = rounded_counts
        shape += f'-segments-{segments}'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / f'bevel-{label}-{shape}-before.blend'), check_existing=False)

    def totals():
        stats = window.scene.statistics(window.view_layer)
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)',
                          stats.replace(',', ''))
        t.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))[1::2]

    def coordinates():
        window.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            return sorted(tuple(round(value, 6) for value in vertex.co) for vertex in mesh.vertices)
        finally:
            evaluated.to_mesh_clear()

    if mouse_input:
        from bpy_extras.view3d_utils import location_3d_to_region_2d
        from mathutils import Vector
        if hold_launch_ctrl:
            area.spaces.active.region_3d.view_location = (0, 1, 2)
            area.spaces.active.region_3d.view_distance = 2
            yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        xy = location_3d_to_region_2d(region, area.spaces.active.region_3d,
                                     Vector((0, 1, 2 if face_selection else 1)))
        center = (round(region.x + xy.x), round(region.y + xy.y))
        yield e.cursor_position_set(*center, move=True)
        if hold_launch_ctrl:
            for event_type, value in (('LEFT_CTRL', 'PRESS'), ('B', 'PRESS'), ('B', 'RELEASE')):
                window.event_simulate(type=event_type, value=value, ctrl=True,
                                      x=center[0], y=center[1])
                yield
            if release_launch_ctrl:
                window.event_simulate(type='LEFT_CTRL', value='RELEASE', ctrl=False,
                                      x=center[0], y=center[1])
                yield
        else:
            yield e.ctrl.b()
        if rounded:
            for _ in range(segments - 1):
                yield e.wheelupmouse()
        if hold_launch_ctrl:
            window.event_simulate(type='MOUSEMOVE', value='NOTHING', ctrl=not release_launch_ctrl,
                                  x=center[0] + 8, y=center[1])
            yield
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.screen.screenshot(filepath=str(output / f'bevel-{label}-{shape}-ctrl-held.png'))
            print('BEVEL_LAUNCH_CTRL', label, totals(), flush=True)
            t.assertEqual(totals(), after)

            def rail_width():
                values = [point[1] for point in coordinates() if abs(point[2] - 2) < 1e-6]
                t.assertTrue(values)
                return min(values)

            widths = {'launch': rail_width()}
            t.assertGreater(widths['launch'], 0)
            t.assertLess(widths['launch'], 0.05)
            right = area.spaces.active.region_3d.view_matrix.inverted().to_3x3() @ Vector((1, 0, 0))
            offset = location_3d_to_region_2d(region, area.spaces.active.region_3d,
                                             Vector((0, 1, 2)) + 0.1 * right)
            pixel_size = 0.1 / (offset - xy).length
            initial_radius = math.hypot(center[0] - region.x - xy.x, center[1] - region.y - xy.y)
            radius = math.hypot(center[0] + 8 - region.x - xy.x, center[1] - region.y - xy.y)
            expected_width = max(0, (radius - initial_radius - 5) * pixel_size)
            t.assertAlmostEqual(widths['launch'], expected_width, delta=2e-6)
            window.event_simulate(type='LEFT_CTRL', value='RELEASE', ctrl=False,
                                  x=center[0] + 8, y=center[1])
            yield
            snap_key = 'LEFT_ALT' if remap_snap else 'LEFT_CTRL'
            window.event_simulate(type=snap_key, value='PRESS', ctrl=not remap_snap, alt=remap_snap,
                                  x=center[0] + 8, y=center[1])
            yield
            window.event_simulate(type='MOUSEMOVE', value='NOTHING', ctrl=not remap_snap, alt=remap_snap,
                                  x=center[0] + 28, y=center[1])
            yield
            widths['snap'] = rail_width()
            t.assertAlmostEqual(widths['snap'], 0.1, delta=2e-6)
            window.event_simulate(type=snap_key, value='RELEASE', ctrl=False, alt=False,
                                  x=center[0] + 28, y=center[1])
            yield
            window.event_simulate(type='LEFT_SHIFT', value='PRESS', shift=True,
                                  x=center[0] + 28, y=center[1])
            yield
            window.event_simulate(type='MOUSEMOVE', value='NOTHING', shift=True,
                                  x=center[0] + 38, y=center[1])
            yield
            widths['precision'] = rail_width()
            t.assertGreater(widths['precision'], widths['snap'])
            t.assertLess(widths['precision'] - widths['snap'], 0.02)
            window.event_simulate(type='LEFT_SHIFT', value='RELEASE', shift=False,
                                  x=center[0] + 38, y=center[1])
            yield
            import json
            (output / f'bevel-{label}-{shape}-widths.json').write_text(json.dumps(widths))
            print('BEVEL_INPUT_WIDTHS', label, widths, flush=True)
        else:
            previews = {}
            distances = ((10, 25, 60, 25) if segments == 3 else
                         (10, 25, 60, 25, 45, 20, 60, 25, 45, 20, 30, 50, 40))
            for distance in distances:
                yield e.cursor_position_set(center[0] + distance, center[1], move=True)
                t.assertEqual(totals(), after)
                current = coordinates()
                if distance in previews:
                    t.assertEqual(current, previews[distance])
                else:
                    previews[distance] = current
            t.assertTrue(previews[10] != previews[25], 'The mouse did not change the bevel width')
            t.assertTrue(previews[25] != previews[60], 'The wider bevel did not update')
    else:
        yield e.ctrl.b().text('100' if clamped else '0.25')
        if clamped and mesh_reference:
            yield e.c()
        if rounded:
            for _ in range(segments - 1):
                yield e.wheelupmouse()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.screen.screenshot(filepath=str(output / f'bevel-{label}-{shape}-preview.png'))
    t.assertEqual(totals(), after)
    preview_display_points = (len(coordinates()) if rounded and not mesh_reference and
                              segments >= 8 else None)
    yield e.esc()
    t.assertEqual(totals(), before)
    if hold_launch_ctrl:
        area.spaces.active.region_3d.view_distance = 7
        area.spaces.active.region_3d.view_location = (0, 1, 1)
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / f'bevel-{label}-{shape}-cancelled.blend'), check_existing=False)
    yield e.ctrl.b().text('100' if clamped else '0.25')
    if clamped and mesh_reference:
        yield e.c()
    if rounded:
        for _ in range(segments - 1):
            yield e.wheelupmouse()
    yield e.ret()
    t.assertEqual(totals(), after)
    if preview_display_points is not None:
        t.assertGreater(len(coordinates()), preview_display_points,
                        'Confirmed bevel retained the lower-density drag preview')
    t.assertEqual(obj.mode, 'EDIT')
    t.assertEqual(tuple(window.scene.tool_settings.mesh_select_mode if mesh_reference else
                        obj.data.nurbs_selection_mode),
                  (False, False, True) if face_selection else (False, True, False))
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / f'bevel-{label}-{shape}-finished.blend'), check_existing=False)
        bpy.ops.screen.screenshot(filepath=str(output / f'bevel-{label}-{shape}-finished.png'))
    if skewed or tapered:
        area.spaces.active.overlay.show_overlays = False
        area.spaces.active.region_3d.view_distance = 4.4
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(output / f'bevel-{label}-{shape}-shaded.png'))
        area.spaces.active.overlay.show_overlays = True
        area.spaces.active.region_3d.view_distance = 7
        yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.ctrl.z()
    t.assertEqual(totals(), before)
    yield e.ctrl.shift.z()
    t.assertEqual(totals(), after)
    yield e.tab().tab()
    obj = window.view_layer.objects.active
    t.assertEqual(obj.mode, 'EDIT')
    t.assertEqual(totals(), after)
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.wm.save_as_mainfile(filepath=str(output / f'bevel-{label}-{shape}-restored.blend'), check_existing=False)
    if skewed or tapered or hold_launch_ctrl:
        original = coordinates()
        yield e.g().z().text('0.15').ret()
        t.assertEqual(totals(), after)
        t.assertTrue(coordinates() != original, 'Moving the beveled selection did nothing')
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'bevel-{label}-{shape}-continued.blend'), check_existing=False)
        yield e.ctrl.z()
        t.assertEqual(coordinates(), original)
        yield e.shift.d().z().text('3').ret()
        t.assertGreater(totals()[0], after[0])
        t.assertEqual(obj.mode, 'EDIT')
        if not mesh_reference:
            t.assertTrue(obj.data.is_native_nurbs)
            t.assertEqual(len(obj.data.splines), 0)
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.save_as_mainfile(filepath=str(output / f'bevel-{label}-{shape}-duplicated.blend'), check_existing=False)
    print('NURBS_BEVEL_CONNECTED_PASS', label, shape, totals())


def bevel_eight_segment_round_mouse_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, mouse_input=True, segments=8)


def bevel_eight_segment_mesh_mouse_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, rounded=True,
                                             mouse_input=True, segments=8)


def bevel_dense_round_mouse_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, mouse_input=True, segments=13)


def bevel_whole_model_translation_workflow():
    """Dense whole-model drag: repeat positions, cancel, confirm, undo/redo and mode re-entry."""
    import bpy
    import json
    import os

    yield from bevel_connected_edges_workflow(rounded=True, segments=13)
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    obj = window.view_layer.objects.active
    center = ui.get_area_center(area)

    def display():
        window.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            return (tuple(tuple(v.co) for v in mesh.vertices),
                    tuple(tuple(n.vector) for n in mesh.corner_normals),
                    tuple(tuple(p.vertices) for p in mesh.polygons))
        finally:
            evaluated.to_mesh_clear()

    def assert_display_equal(a, b):
        t.assertEqual(a[0], b[0])
        t.assertEqual(a[2], b[2])
        t.assertEqual(len(a[1]), len(b[1]))
        # Recomputed float normals can differ by a few ulps after an otherwise rigid move.
        t.assertLessEqual(max(abs(x - y) for na, nb in zip(a[1], b[1])
                              for x, y in zip(na, nb)), 2e-6)

    yield e.cursor_position_set(*center, move=True)
    yield e.a()
    original = display()
    object_matrix = obj.matrix_world.copy()
    yield e.g().x()
    frames = {}
    for distance in (20, 40, 60, 80, 100, 40, 80, 20, 60, 100, 40, 20):
        yield e.cursor_position_set(center[0] + distance, center[1], move=True)
        current = display()
        t.assertEqual(obj.matrix_world, object_matrix,
                      'Edit Mode movement must retain the object transform')
        t.assertNotEqual(current[0], original[0])
        t.assertEqual(current[2], original[2])
        if distance in frames:
            assert_display_equal(current, frames[distance])
        frames[distance] = current
    yield e.esc()
    assert_display_equal(display(), original)
    yield e.cursor_position_set(*center, move=True)
    yield e.g().x().text('0.25').ret()
    confirmed = display()
    t.assertNotEqual(confirmed[0], original[0])
    yield e.ctrl.z()
    assert_display_equal(display(), original)
    yield e.ctrl.shift.z()
    assert_display_equal(display(), confirmed)
    yield e.tab().tab()
    assert_display_equal(display(), confirmed)
    if path := os.environ.get('BLENDER_NURBS_TRANSLATION_RESULT'):
        with open(path, 'w') as output:
            json.dump(display(), output)
    print('NURBS_WHOLE_MODEL_TRANSLATION_PASS', len(confirmed[0]), len(confirmed[2]), flush=True)


def bevel_duplicated_component_translation_workflow():
    """Move a duplicated bevel face while preserving the stationary original display."""
    from collections import Counter
    import bpy
    import os
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector

    yield from bevel_connected_edges_workflow(rounded=True, segments=13)
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    obj = window.view_layer.objects.active
    space.region_3d.view_location = (0, 1, 1)
    space.region_3d.view_distance = 7
    yield e.numpad_7()
    yield e.three().alt.a()

    def screen(position):
        point = location_3d_to_region_2d(region, space.region_3d, Vector(position))
        t.assertIsNotNone(point)
        return round(point.x) + region.x, round(point.y) + region.y

    def display():
        window.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            return (tuple(tuple(vertex.co) for vertex in mesh.vertices),
                    tuple(tuple(polygon.vertices) for polygon in mesh.polygons),
                    tuple(sorted((mesh.vertices[edge.vertices[0]].co -
                                  mesh.vertices[edge.vertices[1]].co).length for edge in mesh.edges)))
        finally:
            evaluated.to_mesh_clear()

    def selected_faces():
        import re
        stats = window.scene.statistics(window.view_layer).replace(',', '')
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)',
                          stats)
        t.assertIsNotNone(match, stats)
        return int(match.group(5))

    face_center = screen((0, 1, 2))
    yield e.cursor_position_set(*face_center, move=True)
    yield e.leftmouse()
    t.assertGreater(selected_faces(), 0)
    before_duplicate = display()
    yield e.shift.d()
    duplicate_initial = display()
    t.assertGreater(len(duplicate_initial[0]), len(before_duplicate[0]))
    frames = {}
    for distance in (20, 40, 60, 20, 40, 60):
        yield e.cursor_position_set(face_center[0] + distance, face_center[1], move=True)
        current = display()
        t.assertEqual(current[1], duplicate_initial[1])
        t.assertEqual(len(current[2]), len(duplicate_initial[2]))
        t.assertLessEqual(max(abs(a - b) for a, b in zip(current[2], duplicate_initial[2])),
                         2e-6, 'Duplicating and moving a face stretched display edges')
        t.assertTrue(current[0] != duplicate_initial[0])
        if distance in frames:
            t.assertEqual(current[0], frames[distance])
        frames[distance] = current[0]
    moved_preview = display()[0]
    original_counts = Counter(before_duplicate[0])
    moved_counts = Counter(moved_preview)
    unchanged = sum((original_counts & moved_counts).values())
    moved = len(moved_preview) - unchanged
    t.assertGreater(unchanged, 0, 'The original surface moved with the duplicated face')
    t.assertGreater(moved, 0, 'The duplicated face did not move')
    t.assertLess(moved, len(moved_preview))
    if output := os.environ.get('BLENDER_NURBS_TRANSLATION_CAPTURE_DIR'):
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=os.path.join(output, 'duplicate-during-drag.png'))
    yield e.leftmouse()
    t.assertEqual(obj.mode, 'EDIT')
    t.assertTrue(obj.data.is_native_nurbs)
    t.assertEqual(len(obj.data.splines), 0)
    print('NURBS_DUPLICATED_COMPONENT_TRANSLATION_PASS', unchanged, moved, flush=True)


def bevel_mesh_connected_edges_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True)


def bevel_pyramid_workflow():
    yield from bevel_connected_edges_workflow(pyramid=True)


def bevel_mesh_pyramid_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, pyramid=True)


def bevel_connected_round_workflow():
    yield from bevel_connected_edges_workflow(rounded=True)


def bevel_mesh_connected_round_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, rounded=True)


def bevel_mouse_sweep_workflow():
    yield from bevel_corner_workflow(mouse_input=True, mouse_sweep=True)


def bevel_mesh_mouse_sweep_workflow():
    yield from bevel_corner_workflow(mesh_reference=True, mouse_input=True, mouse_sweep=True)


def bevel_round_mouse_sweep_workflow():
    yield from bevel_corner_workflow(rounded=True, mouse_input=True, mouse_sweep=True)


def bevel_decimal_mouse_sweep_workflow():
    yield from bevel_corner_workflow(mouse_input=True, mouse_sweep=True,
                                    face_depth=1.71342, wall_height=2.31871)


def bevel_mouse_limits_workflow():
    yield from bevel_corner_workflow(mouse_input=True, mouse_limits=True)


def bevel_round_mouse_limits_workflow():
    yield from bevel_corner_workflow(rounded=True, mouse_input=True, mouse_limits=True)


def bevel_rotated_mouse_sweep_workflow():
    yield from bevel_corner_workflow(mouse_input=True, mouse_sweep=True, rotated=True)


def bevel_rotated_mesh_mouse_sweep_workflow():
    yield from bevel_corner_workflow(mesh_reference=True, mouse_input=True, mouse_sweep=True,
                                    rotated=True)


def bevel_split_vertex_deformation_workflow():
    yield from bevel_corner_workflow(split_deformation=True)


def bevel_split_vertex_deformation_mesh_workflow():
    yield from bevel_corner_workflow(mesh_reference=True, split_deformation=True)


def bevel_clamped_mouse_workflow():
    yield from bevel_corner_workflow(mouse_input=True, clamp_test=True)


def bevel_clamped_round_mouse_workflow():
    yield from bevel_corner_workflow(rounded=True, mouse_input=True, clamp_test=True)


def bevel_clamped_mesh_mouse_workflow():
    yield from bevel_corner_workflow(mesh_reference=True, mouse_input=True, clamp_test=True)


def bevel_rotated_round_box_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, rotated=True)


def bevel_rotated_clamped_round_box_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, rotated=True, clamped=True)


def bevel_subdivision_exec_workflow():
    import bpy
    import re
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().e().y().text('2').ret().a().three().e().z().text('2').ret().two().a()

    def totals():
        match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)',
                          window.scene.statistics(window.view_layer).replace(',', ''))
        t.assertIsNotNone(match)
        return tuple(map(int, match.groups()))[1::2]

    def display():
        window.view_layer.update()
        evaluated = window.view_layer.objects.active.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            return tuple(tuple(vertex.co) for vertex in mesh.vertices)
        finally:
            evaluated.to_mesh_clear()

    t.assertEqual(totals(), (8, 12, 6))
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.curve.bevel(offset=0.25, segments=8, surface_shape='SUBDIVISION')
    t.assertEqual(result, {'FINISHED'})
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    t.assertEqual(totals(), (488, 972, 486))
    smooth = display()
    yield e.ctrl.z()
    t.assertEqual(totals(), (8, 12, 6))
    yield e.two().a()
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.curve.bevel(offset=0.25, segments=8, surface_shape='CIRCULAR')
    t.assertEqual(result, {'FINISHED'})
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    t.assertEqual(totals(), (488, 972, 486))
    t.assertNotEqual(display(), smooth)
    t.assertTrue(window.view_layer.objects.active.data.is_native_nurbs)
    bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape = 'CIRCULAR'
    print('NURBS_SUBDIVISION_EXEC_PASS', flush=True)


def bevel_subdivision_redo_workflow():
    import bpy
    import re
    import os
    e, t, window = ui.test_window()
    area = ui.get_window_area_by_type(window, 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    previous = bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape
    bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape = 'CIRCULAR'
    for obj in tuple(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield from ui.call_menu(e, 'Add -> Curve -> Nurbs Line')
    yield e.tab().two().e().y().text('2').ret().a().three().e().z().text('2').ret().two().a()
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.ctrl.b().text('0.25')
    for _ in range(7):
        yield e.wheelupmouse()
    yield e.ret()
    last = bpy.context.window_manager.operators[-1]
    t.assertEqual(last.bl_idname, 'CURVE_OT_bevel')
    t.assertEqual(last.properties.surface_shape, 'CIRCULAR')
    last.properties.surface_shape = 'SUBDIVISION'
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.ed.undo_redo()
    t.assertEqual(result, {'FINISHED'})
    yield e.cursor_position_set(*ui.get_area_center(area), move=True)
    stats = window.scene.statistics(window.view_layer).replace(',', '')
    match = re.search(r'Verts:(\d+)/(\d+) \| Edges:(\d+)/(\d+) \| Faces:(\d+)/(\d+)', stats)
    t.assertIsNotNone(match)
    t.assertEqual(tuple(map(int, match.groups()))[1::2], (488, 972, 486))
    t.assertEqual(bpy.context.window_manager.operators[-1].properties.surface_shape, 'SUBDIVISION')
    t.assertEqual(bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape,
                  'SUBDIVISION')
    yield e.f9()
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.screen.screenshot(filepath=os.path.join(os.environ['BLENDER_NURBS_BEVEL_OUTPUT'], 'surface-shape-settings.png'))
    yield e.esc()
    bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape = previous
    print('NURBS_SUBDIVISION_REDO_PASS', flush=True)


def bevel_subdivision_round_box_workflow():
    import bpy
    previous = bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape
    bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape = 'SUBDIVISION'
    try:
        yield from bevel_connected_edges_workflow(rounded=True, skewed=True, segments=8)
    finally:
        bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape = previous


def bevel_subdivision_odd_round_box_workflow():
    import bpy
    previous = bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape
    bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape = 'SUBDIVISION'
    try:
        yield from bevel_connected_edges_workflow(rounded=True, skewed=True, segments=13)
    finally:
        bpy.context.window_manager.operator_properties_last('CURVE_OT_bevel').surface_shape = previous


def bevel_skewed_round_box_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, skewed=True)


def bevel_skewed_round_box_mesh_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, rounded=True, skewed=True)


def bevel_skewed_round_face_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, skewed=True, face_selection=True)


def bevel_skewed_round_face_mesh_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, rounded=True, skewed=True,
                                              face_selection=True)


def bevel_skewed_clamped_round_box_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, skewed=True, clamped=True)


def bevel_tapered_round_box_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, tapered=True)


def bevel_tapered_round_box_mesh_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, rounded=True, tapered=True)


def bevel_tapered_round_face_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, tapered=True, face_selection=True)


def bevel_tapered_round_face_mesh_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, rounded=True, tapered=True,
                                              face_selection=True)


def bevel_tapered_clamped_round_box_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, tapered=True, clamped=True)


def bevel_tapered_round_mouse_workflow():
    yield from bevel_connected_edges_workflow(rounded=True, tapered=True, mouse_input=True)


def bevel_face_launch_ctrl_workflow():
    yield from bevel_connected_edges_workflow(face_selection=True, mouse_input=True,
                                              hold_launch_ctrl=True)


def bevel_face_launch_ctrl_mesh_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, face_selection=True,
                                              mouse_input=True, hold_launch_ctrl=True)


def bevel_face_remapped_snap_workflow():
    yield from bevel_connected_edges_workflow(face_selection=True, mouse_input=True,
                                              hold_launch_ctrl=True, remap_snap=True)


def bevel_face_remapped_snap_mesh_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, face_selection=True,
                                              mouse_input=True, hold_launch_ctrl=True, remap_snap=True)


def bevel_face_released_ctrl_workflow():
    yield from bevel_connected_edges_workflow(face_selection=True, mouse_input=True,
                                              hold_launch_ctrl=True, release_launch_ctrl=True)


def bevel_face_released_ctrl_mesh_workflow():
    yield from bevel_connected_edges_workflow(mesh_reference=True, face_selection=True,
                                              mouse_input=True, hold_launch_ctrl=True,
                                              release_launch_ctrl=True)
