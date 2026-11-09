# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run with blender --background --factory-startup --python tests/python/bl_solid_sketch.py."""

import math
import os
import tempfile
import unittest

import bmesh
import bpy


class SolidSketchTest(unittest.TestCase):
    def test_extrusion_matches_native_mesh_on_adjacent_and_all_faces(self):
        for method in ('REGION', 'INDIVIDUAL', 'ALONG_NORMALS'):
            for count in (2, 3, 6):
                for even in ((False, True) if method == 'ALONG_NORMALS' else (False,)):
                    with self.subTest(method=method, count=count, even=even):
                        self.setUp()
                        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
                        bpy.ops.solid.extrude(distance=2)
                        solid = bpy.context.object
                        predicate = lambda f: (count == 6 or f.normal.z > .9 or
                                               f.normal.x > .9 or
                                               (count == 3 and f.normal.y > .9))
                        self.source_select('FACE', predicate)
                        bm = bmesh.from_edit_mesh(solid.data)
                        bm.verts.index_update()
                        positions = [tuple(v.co) for v in bm.verts]
                        polygons = [[v.index for v in f.verts] for f in bm.faces]
                        bpy.ops.object.mode_set(mode='OBJECT')
                        mesh = bpy.data.meshes.new('Native extrusion reference')
                        mesh.from_pydata(positions, [], polygons)
                        reference = bpy.data.objects.new('Native extrusion reference', mesh)
                        bpy.context.collection.objects.link(reference)
                        solid.select_set(False)
                        reference.select_set(True)
                        bpy.context.view_layer.objects.active = reference
                        bpy.ops.object.mode_set(mode='EDIT')
                        self.source_select('FACE', predicate)
                        if method == 'REGION':
                            bpy.ops.mesh.extrude_region_move(
                                TRANSFORM_OT_translate={'value': (.2, .3, .4)})
                        elif method == 'INDIVIDUAL':
                            bpy.ops.mesh.extrude_faces_move(
                                TRANSFORM_OT_shrink_fatten={'value': .4})
                        else:
                            bpy.ops.mesh.extrude_region_shrink_fatten(
                                TRANSFORM_OT_shrink_fatten={'value': .4, 'use_even_offset': even})
                        def signature(obj):
                            bm = bmesh.from_edit_mesh(obj.data)
                            def coordinates(v):
                                return tuple(round(x, 5) for x in v.co)
                            def face_cycle(f):
                                points = tuple(coordinates(v) for v in f.verts)
                                return min(points[i:] + points[:i] for i in range(len(points)))
                            return (sorted(coordinates(v) for v in bm.verts),
                                    sorted(face_cycle(f) for f in bm.faces),
                                    sum(f.select for f in bm.faces))
                        expected = signature(reference)
                        bpy.ops.object.mode_set(mode='OBJECT')
                        reference.select_set(False)
                        solid.select_set(True)
                        bpy.context.view_layer.objects.active = solid
                        bpy.ops.object.mode_set(mode='EDIT')
                        kwargs = {'method': method, 'use_even_offset': even} if method == 'ALONG_NORMALS' else {}
                        if method == 'REGION':
                            kwargs = {'offset': (.2, .3, .4)}
                        operator = bpy.ops.solid.extrude_individual if method == 'INDIVIDUAL' else bpy.ops.solid.extrude
                        self.assertEqual(operator(
                            distance=.4, **kwargs), {'FINISHED'})
                        self.assertEqual(signature(solid), expected)
                        self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode),
                                         (False, False, True))

    def test_normal_extrusion_of_connected_rounded_body(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.solid.bevel(offset=.2, segments=3)
        bpy.ops.mesh.select_mode(type='FACE')
        bpy.ops.mesh.select_all(action='SELECT')
        self.assertEqual(bpy.ops.solid.extrude(method='ALONG_NORMALS', distance=.05), {'FINISHED'})
        bm = bmesh.from_edit_mesh(obj.data)
        selected = {v for f in bm.faces if f.select for v in f.verts}
        self.assertTrue(selected)
        for axis, minimum, maximum in ((0, -1.05, 1.05), (1, -1.05, 1.05), (2, -.05, 2.05)):
            self.assertAlmostEqual(min(v.co[axis] for v in selected), minimum, places=5)
            self.assertAlmostEqual(max(v.co[axis] for v in selected), maximum, places=5)
        self.assertTrue(all(e.is_manifold for e in bm.edges))
        self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), (False, False, True))

    def test_manifold_extrusion_merges_repeated_coplanar_side_walls(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        self.source_select('FACE', lambda f: all(abs(v.co.z - 2) < 1e-5 for v in f.verts))
        for _ in range(3):
            self.assertEqual(bpy.ops.solid.extrude(method='MANIFOLD', distance=.4), {'FINISHED'})
            self.assertEqual(obj.data.solid_faces_count, 6)
        self.assertAlmostEqual(self.volume(obj), 12.8, places=5)
        bm = bmesh.from_edit_mesh(obj.data)
        self.assertTrue(all(e.is_manifold for e in bm.edges))
        self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), (False, False, True))

        self.setUp()
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        self.source_select('FACE', lambda f: all(abs(v.co.z - 2) < 1e-5 for v in f.verts))
        self.assertEqual(bpy.ops.solid.extrude(method='MANIFOLD', distance=-.4), {'FINISHED'})
        self.assertAlmostEqual(self.volume(obj), 6.4, places=5)
        bm = bmesh.from_edit_mesh(obj.data)
        self.assertTrue(all(e.is_manifold for e in bm.edges))

    def test_curved_normal_extrusion_preserves_cylinder_axis(self):
        for method in ('ALONG_NORMALS', 'INDIVIDUAL'):
            for amount in (.2, -.2):
                with self.subTest(method=method, amount=amount):
                    self.setUp()
                    bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
                    bpy.ops.solid.extrude(distance=2)
                    obj = bpy.context.object
                    self.source_select('FACE', lambda f: abs(f.normal.z) < .5)
                    operator = (bpy.ops.solid.extrude_individual if method == 'INDIVIDUAL'
                                else bpy.ops.solid.extrude)
                    self.assertEqual(operator(method=method, distance=amount), {'FINISHED'})
                    bm = bmesh.from_edit_mesh(obj.data)
                    selected = {v for f in bm.faces if f.select for v in f.verts}
                    self.assertTrue(selected)
                    for v in selected:
                        self.assertAlmostEqual(math.hypot(v.co.x, v.co.y), 1 + amount, places=5)
                    self.assertAlmostEqual(min(v.co.z for v in selected), 0, places=5)
                    self.assertAlmostEqual(max(v.co.z for v in selected), 2, places=5)
                    self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode),
                                     (False, False, True))
                    self.assertAlmostEqual(self.volume(obj), math.pi * (1 + amount)**2 * 2,
                                           delta=.03)

    def test_solid_toolbar_exposes_extrusion_tools(self):
        from bl_ui.space_toolsystem_toolbar import VIEW3D_PT_tools_active
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        ids = set()
        def collect(items):
            for item in items:
                if hasattr(item, 'idname'):
                    ids.add(item.idname)
                elif isinstance(item, tuple):
                    collect(item)
        collect(VIEW3D_PT_tools_active.tools_from_context(bpy.context))
        self.assertTrue({'builtin.extrude_region', 'builtin.extrude_manifold',
                         'builtin.extrude_along_normals', 'builtin.extrude_individual',
                         'builtin.extrude_to_cursor'}.issubset(ids))

    def test_legacy_view3d_extrusion_operators_dispatch_to_analytic_solids(self):
        operators = (
            bpy.ops.view3d.edit_mesh_extrude_individual_move,
            bpy.ops.view3d.edit_mesh_extrude_move_normal,
            bpy.ops.view3d.edit_mesh_extrude_move_shrink_fatten,
            bpy.ops.view3d.edit_mesh_extrude_manifold_normal,
            bpy.ops.view3d.edit_mesh_extrude_cursor,
        )
        for operator in operators:
            with self.subTest(operator=operator.idname()):
                self.setUp()
                bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
                obj = bpy.context.object
                self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
                self.source_select('FACE', lambda face: all(abs(v.co.z - 2) < 1e-5
                                                              for v in face.verts))
                before = obj.data.solid_faces_count

                self.assertEqual(operator(), {'FINISHED'})
                if operator.idname() == 'VIEW3D_OT_edit_mesh_extrude_manifold_normal':
                    self.assertEqual(obj.data.solid_faces_count, before)
                else:
                    self.assertGreater(obj.data.solid_faces_count, before)
                self.assertGreater(self.volume(obj), 8)
                self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode),
                                 (False, False, True))

    def test_individual_face_extrusion_uses_each_analytic_normal(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        obj = bpy.context.object
        self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
        self.source_select('FACE', lambda face: (
            all(abs(v.co.z) < 1e-5 for v in face.verts) or
            all(abs(v.co.z - 2) < 1e-5 for v in face.verts)))
        before = obj.data.solid_faces_count

        self.assertEqual(bpy.ops.solid.extrude(method='INDIVIDUAL', distance=.4), {'FINISHED'})
        self.assertEqual(obj.data.solid_faces_count, before + 8)
        self.assertEqual(obj.data.total_face_sel, 2)
        self.assertAlmostEqual(self.volume(obj), 11.2, places=5)

    def test_repeat_solid_extrusion_commits_as_one_face_mode_edit(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        obj = bpy.context.object
        self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
        self.source_select('FACE', lambda face: all(abs(v.co.z - 2) < 1e-5 for v in face.verts))

        before = obj.data.solid_faces_count
        self.assertEqual(bpy.ops.solid.extrude_repeat(
            steps=2, offset=(0, 0, .25)), {'FINISHED'})
        self.assertEqual(obj.data.solid_faces_count, before + 8)
        self.assertEqual(obj.data.total_face_sel, 1)
        self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode),
                         (False, False, True))
        self.assertAlmostEqual(max(v.co.z for v in obj.data.vertices), 2.5, places=5)

    def test_arbitrary_rotation_preserves_world_geometry_and_resize(self):
        from mathutils import Matrix, Vector

        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.mesh.select_all(action='SELECT')
        obj = bpy.context.object
        orientation = Vector((.62358183, -.50694573, .59510642)).to_track_quat('Z', 'Y').to_matrix()
        angle = 2.348279
        rotation = Matrix.Rotation(angle, 3, orientation.col[2])

        def world_positions():
            bpy.context.view_layer.update()
            return [obj.matrix_world @ v.co for v in bmesh.from_edit_mesh(obj.data).verts]

        # An arbitrary rotation must remain accurate with either uniform or
        # nonuniform object scale. The latter is non-rigid in local coordinates.
        for scale in ((1, 1, 1), (2, 1, .7)):
            obj.scale = scale
            before = world_positions()
            self.assertEqual(bpy.ops.transform.rotate(
                value=angle, orient_axis='Z', orient_type='GLOBAL',
                orient_matrix_type='GLOBAL', orient_matrix=orientation,
                constraint_axis=(False, False, True), center_override=(0, 0, 0)), {'FINISHED'})
            for a, b in zip(before, world_positions()):
                self.assertLess((rotation @ a - b).length, 5e-6)

        before = world_positions()
        factors = Vector((1.3, .8, 1.1))
        self.assertEqual(bpy.ops.transform.resize(
            value=factors, orient_type='GLOBAL', center_override=(0, 0, 0)), {'FINISHED'})
        for a, b in zip(before, world_positions()):
            self.assertLess((Vector(tuple(a[i] * factors[i] for i in range(3))) - b).length, 5e-6)

    def test_native_smooth_wire_extrusion(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.object.shade_smooth()
        bpy.ops.object.mode_set(mode='EDIT')
        self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertIsNone(obj.data.attributes.get('custom_normal'))
        self.assertIsNotNone(obj.data.attributes.get('.solid_blender_normals'))

    def test_native_smooth_survives_solid_edits_and_reload(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        obj = bpy.context.object
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.object.shade_smooth()

        def check_native():
            self.assertIsNone(obj.data.attributes.get('custom_normal'))
            self.assertIsNotNone(obj.data.attributes.get('.solid_blender_normals'))
            self.assertTrue(all(p.use_smooth for p in obj.data.polygons))
            self.assertTrue(all(abs(n.vector.length - 1) < 2e-5
                                for n in obj.data.corner_normals))

        check_native()
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        self.assertEqual(bpy.ops.solid.bevel(offset=.2, segments=4,
                                             corner_topology='TRIANGLES'), {'FINISHED'})
        bpy.ops.mesh.select_mode(type='VERT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.object.mode_set(mode='OBJECT')
        normals_before = [n.vector.copy() for n in obj.data.corner_normals]
        bpy.ops.object.mode_set(mode='EDIT')
        self.assertEqual(bpy.ops.transform.rotate(value=.35, orient_axis='Z'), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        from mathutils import Matrix
        rotation = Matrix.Rotation(.35, 3, 'Z')
        # Native normals use float coordinates on small display facets. Allow less
        # than 0.06 degrees of roundoff; analytic-normal checks remain stricter.
        self.assertLess(max((rotation @ before - after.vector).length
                            for before, after in zip(normals_before, obj.data.corner_normals)), 1e-3)
        bpy.ops.object.mode_set(mode='EDIT')
        self.assertEqual(bpy.ops.transform.resize(value=(1.2, .8, 1.1)), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        check_native()
        name = obj.name
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'native-solid.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
        obj = bpy.data.objects[name]
        check_native()
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.object.shade_flat()
        self.assertIsNone(obj.data.attributes.get('custom_normal'))
        self.assertTrue(all(not p.use_smooth for p in obj.data.polygons))
        bpy.ops.object.shade_smooth(solid_normal_method='ANALYTIC')
        self.assertIsNotNone(obj.data.attributes.get('custom_normal'))
        self.assertIsNone(obj.data.attributes.get('.solid_blender_normals'))
        self.assertTrue(all(p.use_smooth for p in obj.data.polygons))
        bpy.ops.object.shade_smooth()
        check_native()

    def test_flat_and_smooth_solid_normals_survive_edits_and_reload(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        obj = bpy.context.object
        name = obj.name
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.solid.bevel(offset=.2, segments=4, corner_topology='TRIANGLES')
        bpy.ops.object.mode_set(mode='OBJECT')
        topology = (len(obj.data.vertices), len(obj.data.polygons), obj.data.solid_faces_count)
        smooth = [n.vector.copy() for n in obj.data.corner_normals]

        def check_flat():
            mesh = obj.data
            self.assertTrue(all(not p.use_smooth for p in mesh.polygons))
            for p in mesh.polygons:
                normal = mesh.corner_normals[p.loop_start].vector
                self.assertGreater(normal.dot(p.normal), .999)
                for i in p.loop_indices:
                    self.assertLess((mesh.corner_normals[i].vector - normal).length, 2e-6)

        self.assertEqual(bpy.ops.object.shade_flat(), {'FINISHED'})
        check_flat()
        self.assertTrue(any((n.vector - s).length > .01
                            for n, s in zip(obj.data.corner_normals, smooth)))
        self.assertEqual(bpy.ops.object.shade_smooth(solid_normal_method='ANALYTIC'), {'FINISHED'})
        for n, s in zip(obj.data.corner_normals, smooth):
            self.assertLess((n.vector - s).length, 2e-6)
        bpy.ops.object.shade_flat()
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_mode(type='VERT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.transform.rotate(value=.25, orient_axis='Z')
        bpy.ops.object.mode_set(mode='OBJECT')
        check_flat()
        self.assertEqual((len(obj.data.vertices), len(obj.data.polygons),
                          obj.data.solid_faces_count), topology)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'solid-shading.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
        obj = bpy.data.objects[name]
        check_flat()
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.object.shade_smooth(solid_normal_method='ANALYTIC')
        self.assertTrue(all(p.use_smooth for p in obj.data.polygons))
        self.assertTrue(any((obj.data.corner_normals[i].vector - p.normal).length > .01
                            for p in obj.data.polygons for i in p.loop_indices))

    def test_subdivided_fillet_does_not_resample_each_patch_as_a_whole_corner(self):
        for (segments, expected_mesh_faces, expected_source_faces) in (
            (20, 2646, 3446),
            (32, 6534, 8582),
        ):
            if bpy.context.object and bpy.context.object.mode != 'OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')
            for existing in list(bpy.data.objects):
                bpy.data.objects.remove(existing, do_unlink=True)
            bpy.ops.mesh.primitive_cube_add(size=2)
            mesh_obj = bpy.context.object
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.mesh.select_all(action='SELECT')
            self.assertEqual(
                bpy.ops.mesh.bevel(offset=.2, segments=segments, profile=.5), {'FINISHED'})
            self.assertEqual(len(bmesh.from_edit_mesh(mesh_obj.data).faces), expected_mesh_faces)
            bpy.ops.object.mode_set(mode='OBJECT')
            bpy.data.objects.remove(mesh_obj, do_unlink=True)

            bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
            obj = bpy.context.object
            bpy.ops.solid.extrude(distance=2)
            bpy.ops.mesh.select_mode(type='EDGE')
            bpy.ops.mesh.select_all(action='SELECT')
            self.assertEqual(bpy.ops.solid.bevel(offset=.2,
                                                 segments=segments,
                                                 corner_topology='TRIANGLES'), {'FINISHED'})
            self.assertEqual(obj.data.solid_faces_count, expected_source_faces)
            bm = bmesh.from_edit_mesh(obj.data)
            # Display quality follows the analytic surface independently of editable divisions.
            self.assertGreaterEqual(len(bm.faces), expected_source_faces)
            self.assertLess(len(bm.faces), 100000)
            self.assertTrue(all(v.co.length < 4 for v in bm.verts))

            bpy.context.view_layer.update()
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            display = evaluated.to_mesh()
            try:
                self.assertEqual(len(display.polygons), len(bm.faces))
                self.assertEqual(len(display.vertices), len(bm.verts))
                self.assertTrue(all(p.use_smooth for p in display.polygons))
            finally:
                evaluated.to_mesh_clear()

    def test_saved_solid_rebuilds_current_display_mesh_on_load(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        obj = bpy.context.object
        name = obj.name
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        self.assertEqual(bpy.ops.solid.bevel(offset=.2, segments=20,
                                             corner_topology='TRIANGLES'), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        expected_display = (len(obj.data.vertices), len(obj.data.polygons))

        # The analytic source stays intact if the saved display adapter is absent.
        obj.data.clear_geometry()
        self.assertTrue(obj.data.is_solid_model)
        self.assertEqual(len(obj.data.polygons), 0)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'solid-display-rebuild.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)

        obj = bpy.data.objects[name]
        self.assertTrue(obj.data.is_solid_model)
        self.assertEqual(obj.data.solid_faces_count, 3446)
        self.assertEqual((len(obj.data.vertices), len(obj.data.polygons)), expected_display)

    def test_fillet_rotation_preserves_display_and_analytic_normals(self):
        from mathutils import Matrix

        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        obj = bpy.context.object
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        self.assertEqual(bpy.ops.solid.bevel(offset=.2, segments=1), {'FINISHED'})
        bpy.ops.mesh.select_mode(type='VERT')
        bpy.ops.mesh.select_all(action='SELECT')

        def display():
            bpy.context.view_layer.update()
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                self.assertEqual(mesh.attributes['custom_normal'].data_type, 'FLOAT_VECTOR')
                return ([v.co.copy() for v in mesh.vertices],
                        [n.vector.copy() for n in mesh.corner_normals],
                        [tuple(f.vertices) for f in mesh.polygons])
            finally:
                evaluated.to_mesh_clear()

        before = display()
        bm = bmesh.from_edit_mesh(obj.data)
        geometric_normals = [v.normal.copy() for v in bm.verts] + [f.normal.copy() for f in bm.faces]
        angle = .371
        self.assertEqual(bpy.ops.transform.rotate(value=angle, orient_axis='Z',
                                                  center_override=(0, 0, 0)), {'FINISHED'})
        after = display()
        rotation = Matrix.Rotation(angle, 3, 'Z')
        self.assertEqual(before[2], after[2])
        bm = bmesh.from_edit_mesh(obj.data)
        rotated_normals = [v.normal.copy() for v in bm.verts] + [f.normal.copy() for f in bm.faces]
        # Derived normals use tiny float display triangles; allow their roundoff.
        for a, b in zip(geometric_normals, rotated_normals):
            self.assertLess((rotation @ a - b).length, 3e-4)
        for a, b in zip(before[0], after[0]):
            self.assertLess((rotation @ a - b).length, 2e-6)
        for a, b in zip(before[1], after[1]):
            self.assertLess((rotation @ a - b).length, 2e-6)
        for _ in range(8):
            self.assertEqual(bpy.ops.transform.rotate(value=.03, orient_axis='Z',
                                                      center_override=(0, 0, 0)), {'FINISHED'})
        after = display()
        # Duplication rebuilds both components from source rather than using the
        # transformed display cache. The original must agree with fresh sampling.
        self.assertEqual(bpy.ops.solid.duplicate(), {'FINISHED'})
        rebuilt = display()
        self.assertEqual(len(rebuilt[0]), 2 * len(after[0]))
        from mathutils.kdtree import KDTree
        tree = KDTree(len(rebuilt[0]))
        for index, point in enumerate(rebuilt[0]):
            tree.insert(point, index)
        tree.balance()
        for point in after[0]:
            self.assertLess(tree.find(point)[2], 2e-6)
        for a, b in zip(after[1], rebuilt[1]):
            self.assertLess((a - b).length, 2e-6)

    def test_partial_transforms_match_fresh_source_sampling(self):
        from mathutils import Matrix, Vector
        from mathutils.kdtree import KDTree

        for component in (False, True):
            with self.subTest(component=component):
                self.setUp()
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
                    self.source_select('VERT', lambda v: v.co.z > 1.99)
                obj = bpy.context.object
                original_matrix = obj.matrix_world.copy()
                bm = bmesh.from_edit_mesh(obj.data)
                before = [v.co.copy() for v in bm.verts]
                moving = [p.x > 2 if component else p.z > 1.99 for p in before]
                if component:
                    rotation = Matrix.Rotation(.371, 3, 'Z')
                    self.assertEqual(bpy.ops.transform.rotate(value=.371, orient_axis='Z',
                                                              center_override=(0, 0, 0)), {'FINISHED'})
                else:
                    self.assertEqual(bpy.ops.transform.translate(value=(0, 0, .4)), {'FINISHED'})
                bm = bmesh.from_edit_mesh(obj.data)
                self.assertEqual(len(before), len(bm.verts))
                for v, p, selected in zip(bm.verts, before, moving):
                    expected = (rotation @ p if component else p + Vector((0, 0, .4))) if selected else p
                    self.assertLess((v.co - expected).length, 2e-6)
                self.assertEqual(obj.matrix_world, original_matrix)
                positions = [v.co.copy() for v in bm.verts]
                layer = bm.loops.layers.float_vector['custom_normal']
                normals = [loop[layer].copy() for f in bm.faces for loop in f.loops]
                self.assertEqual(bpy.ops.solid.duplicate(), {'FINISHED'})
                rebuilt = bmesh.from_edit_mesh(obj.data)
                tree = KDTree(len(rebuilt.verts))
                for i, v in enumerate(rebuilt.verts):
                    tree.insert(v.co, i)
                tree.balance()
                for p in positions:
                    self.assertLess(tree.find(p)[2], 2e-6)
                layer = rebuilt.loops.layers.float_vector['custom_normal']
                fresh_normals = [loop[layer].copy() for f in rebuilt.faces for loop in f.loops]
                for old, fresh in zip(normals, fresh_normals):
                    self.assertLess((old - fresh).length, 2e-6)

    def setUp(self):
        if bpy.context.object and bpy.context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)

    def volume(self, obj):
        bpy.context.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        bm = bmesh.new()
        try:
            bm.from_mesh(mesh)
            self.assertTrue(bm.faces)
            self.assertTrue(all(edge.is_manifold for edge in bm.edges))
            return bm.calc_volume(signed=True)
        finally:
            bm.free()
            evaluated.to_mesh_clear()

    def make_boolean_pair(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        self.source_select('FACE', lambda face: True)
        self.assertEqual(bpy.ops.solid.duplicate(), {'FINISHED'})
        # Offset in all three axes to avoid coincident faces while retaining an
        # analytically known overlap volume of 1.2 * 1.6 * 1.7 = 3.264.
        self.assertEqual(bpy.ops.transform.translate(value=(.8, .4, .3)), {'FINISHED'})
        self.source_select('FACE', lambda face: True)
        return obj

    def test_boolean_operations_match_expected_volumes_for_each_solver(self):
        expected = {
            'UNION': 12.736,
            'SUBTRACT': 4.736,
            'INTERSECT': 3.264,
            # Split keeps both the inside and outside pieces of the target.
            'SPLIT': 8.0,
        }
        for solver in ('FLOAT', 'EXACT', 'MANIFOLD'):
            for operation, volume in expected.items():
                with self.subTest(solver=solver, operation=operation):
                    self.setUp()
                    obj = self.make_boolean_pair()
                    self.assertEqual(
                        bpy.ops.solid.boolean(operation_mode=operation, solver=solver),
                        {'FINISHED'},
                    )
                    self.assertAlmostEqual(self.volume(obj), volume, places=4)
                    self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode),
                                     (False, False, True))
                    bm = bmesh.from_edit_mesh(obj.data)
                    self.assertTrue(all(edge.is_manifold for edge in bm.edges))
                    self.assertFalse(any(
                        edge.link_faces[0].normal.dot(edge.link_faces[1].normal) > 1 - 1e-6
                        for edge in bm.edges), 'Boolean left diagonals inside flat faces')

    def test_boolean_curved_solid_keeps_closed_analytic_output(self):
        bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        self.source_select('FACE', lambda face: True)
        self.assertEqual(bpy.ops.solid.duplicate(), {'FINISHED'})
        self.assertEqual(bpy.ops.transform.translate(value=(.5, 0, .2)), {'FINISHED'})
        self.source_select('FACE', lambda face: True)
        self.assertEqual(
            bpy.ops.solid.boolean(operation_mode='UNION', solver='EXACT'), {'FINISHED'})
        self.assertGreater(self.volume(obj), 0)
        self.assertGreater(obj.data.solid_faces_count, 6)

    def test_cylinder_boolean_keeps_analytic_boundaries(self):
        for operation in ('UNION', 'SUBTRACT', 'INTERSECT', 'SPLIT'):
            for shift in ((.5, 0, .2), (.5, .3, 0), (.5, .3, 2.5)):
                with self.subTest(operation=operation, shift=shift):
                    self.setUp()
                    bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
                    bpy.ops.solid.extrude(distance=2)
                    obj = bpy.context.object
                    self.source_select('FACE', lambda f: True)
                    bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': shift})
                    self.source_select('FACE', lambda f: True)
                    self.assertEqual(bpy.ops.solid.boolean(operation_mode=operation), {'FINISHED'})
                    if operation == 'INTERSECT' and shift[2] > 2:
                        self.assertEqual(obj.data.solid_faces_count, 0)
                        continue
                    self.assertLess(obj.data.solid_vertices_count, 20)
                    self.assertLess(obj.data.solid_edges_count, 40)
                    self.assertLess(obj.data.solid_faces_count, 24)
                    d = math.hypot(shift[0], shift[1])
                    overlap_area = 2 * math.acos(d / 2) - .5 * d * math.sqrt(4 - d*d)
                    overlap = overlap_area * max(0, 2 - abs(shift[2]))
                    expected = {'UNION': 4*math.pi - overlap,
                                'SUBTRACT': 2*math.pi - overlap,
                                'INTERSECT': overlap, 'SPLIT': 2*math.pi}[operation]
                    self.assertAlmostEqual(self.volume(obj), expected, delta=.025)
                    # Freshly sampling the retained source must reproduce the curved result.
                    count = obj.data.solid_vertices_count
                    bpy.ops.object.mode_set(mode='OBJECT')
                    bpy.ops.object.mode_set(mode='EDIT')
                    self.assertEqual(obj.data.solid_vertices_count, count)

    def test_cylinder_boolean_after_native_selection_mode_changes(self):
        bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.mesh.select_mode(type='FACE')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (.5, 0, .2)})
        bpy.ops.mesh.select_all(action='SELECT')
        for mode in ('VERT', 'EDGE', 'FACE'):
            bpy.ops.mesh.select_mode(type=mode)
        self.assertEqual(bpy.ops.solid.boolean(), {'FINISHED'})

    def test_cylinder_boolean_rotations_and_cutter_options(self):
        area = 2 * math.acos(.25) - .25 * math.sqrt(3.75)
        overlap = area * 1.8
        cases = (({'operation_mode': 'UNION'}, 4*math.pi-overlap),
                 ({'operation_mode': 'SUBTRACT', 'keep_cutter': True}, 4*math.pi-overlap),
                 ({'operation_mode': 'SUBTRACT', 'swap_subtract': True}, 2*math.pi-overlap),
                 ({'operation_mode': 'SUBTRACT', 'invert_cutter': True}, overlap),
                 ({'operation_mode': 'SPLIT', 'keep_cutter': True}, 4*math.pi))
        for kwargs, expected in cases:
            with self.subTest(kwargs=kwargs):
                self.setUp()
                bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
                bpy.ops.solid.extrude(distance=2)
                obj = bpy.context.object
                self.source_select('FACE', lambda f: True)
                bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (.5, 0, .2)})
                self.source_select('FACE', lambda f: True)
                bpy.ops.transform.rotate(value=.37, orient_axis='Y', center_override=(0, 0, 0))
                self.assertEqual(bpy.ops.solid.boolean(**kwargs), {'FINISHED'})
                self.assertAlmostEqual(self.volume(obj), expected, delta=.025)

    def test_elliptical_cylinder_boolean_keeps_analytic_boundaries(self):
        for operation in ('UNION', 'SUBTRACT', 'INTERSECT', 'SPLIT'):
            with self.subTest(operation=operation):
                self.setUp()
                bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
                bpy.ops.solid.extrude(distance=2)
                obj = bpy.context.object
                self.source_select('FACE', lambda f: True)
                bpy.ops.transform.resize(value=(2, 1, 1))
                bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (.5, 0, .2)})
                self.source_select('FACE', lambda f: True)
                self.assertEqual(bpy.ops.solid.boolean(operation_mode=operation), {'FINISHED'})
                self.assertLess(obj.data.solid_vertices_count, 24)
                self.assertLess(obj.data.solid_edges_count, 48)
                self.assertLess(obj.data.solid_faces_count, 28)
                radius_x, radius_y, height, offset = 2.0, 1.0, 2.0, .5
                overlap_area = (2 * radius_x * radius_y * math.acos(offset / (2 * radius_x)) -
                                .5 * offset * radius_y / radius_x *
                                math.sqrt(4 * radius_x * radius_x - offset * offset))
                overlap = overlap_area * (height - .2)
                whole = math.pi * radius_x * radius_y * height
                expected = {'UNION': 2 * whole - overlap,
                            'SUBTRACT': whole - overlap,
                            'INTERSECT': overlap,
                            'SPLIT': whole}[operation]
                self.assertAlmostEqual(self.volume(obj), expected, delta=.04)

    def test_rotated_elliptical_cylinder_boolean_is_closed(self):
        volumes = {}
        for operation in ('UNION', 'SUBTRACT', 'INTERSECT', 'SPLIT'):
            with self.subTest(operation=operation):
                self.setUp()
                bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
                bpy.ops.solid.extrude(distance=2)
                obj = bpy.context.object
                self.source_select('FACE', lambda f: True)
                bpy.ops.transform.resize(value=(2, 1, 1))
                bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (.5, 0, .2)})
                bpy.ops.transform.rotate(value=.4, orient_axis='Z')
                self.source_select('FACE', lambda f: True)
                self.assertEqual(bpy.ops.solid.boolean(operation_mode=operation), {'FINISHED'})
                self.assertLess(obj.data.solid_vertices_count, 24)
                self.assertLess(obj.data.solid_edges_count, 48)
                self.assertLess(obj.data.solid_faces_count, 28)
                volumes[operation] = self.volume(obj)
        # Union/intersection and subtraction/intersection must partition the analytic inputs.
        whole = math.pi * 2 * 2
        self.assertAlmostEqual(volumes['UNION'] + volumes['INTERSECT'], 2 * whole, delta=.05)
        self.assertAlmostEqual(volumes['SUBTRACT'] + volumes['INTERSECT'], whole, delta=.05)
        self.assertAlmostEqual(volumes['SPLIT'], whole, delta=.05)

    def test_skewed_cylinder_boolean_keeps_original_source(self):
        bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        self.source_select('FACE', lambda f: True)
        bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (.5, 0, .2)})
        bpy.ops.transform.rotate(value=.3, orient_axis='Y')
        self.source_select('FACE', lambda f: True)
        before = [tuple(v.co) for v in bmesh.from_edit_mesh(obj.data).verts]
        counts = (obj.data.solid_vertices_count, obj.data.solid_edges_count, obj.data.solid_faces_count)
        with self.assertRaisesRegex(RuntimeError, 'parallel axes'):
            bpy.ops.solid.boolean()
        self.assertEqual(before, [tuple(v.co) for v in bmesh.from_edit_mesh(obj.data).verts])
        self.assertEqual(counts, (obj.data.solid_vertices_count,
                                  obj.data.solid_edges_count, obj.data.solid_faces_count))

    def test_boolean_skewed_extrusions_with_limited_dissolve(self):
        # Reproduce the line -> skewed face -> solid -> translated duplicate workflow
        # recorded in the user crash log. Exercise dissolve on non-axis-aligned faces.
        for solver in ('FLOAT', 'EXACT', 'MANIFOLD'):
            volumes = {}
            for operation in ('UNION', 'SUBTRACT', 'INTERSECT', 'SPLIT'):
                with self.subTest(solver=solver, operation=operation):
                    self.setUp()
                    bpy.ops.solid.sketch_add(type='LINE', size=1)
                    bpy.ops.solid.extrude(distance=2.0503589207262558,
                                          offset=(.642019, 1.91148, .371516))
                    self.source_select('FACE', lambda f: True)
                    bpy.ops.solid.extrude(distance=2.2814648045139863)
                    obj = bpy.context.object
                    original = self.volume(obj)
                    self.source_select('FACE', lambda f: True)
                    bpy.ops.solid.duplicate_move(
                        TRANSFORM_OT_translate={'value': (.8989, .656221, -1.11859)})
                    self.source_select('FACE', lambda f: True)
                    self.assertEqual(bpy.ops.solid.boolean(
                        operation_mode=operation, solver=solver,
                        limited_dissolve=True), {'FINISHED'})
                    volumes[operation] = self.volume(obj)
                    bm = bmesh.from_edit_mesh(obj.data)
                    self.assertFalse(any(
                        edge.link_faces[0].normal.dot(edge.link_faces[1].normal) > 1 - 1e-6
                        for edge in bm.edges), 'Skewed flat faces retained Boolean diagonals')
            self.assertAlmostEqual(volumes['UNION'] + volumes['INTERSECT'],
                                   2 * original, places=4)
            self.assertAlmostEqual(volumes['SUBTRACT'] + volumes['INTERSECT'],
                                   original, places=4)
            self.assertAlmostEqual(volumes['SPLIT'], original, places=4)

    def test_boolean_disjoint_bodies_and_dissolve_redo_options(self):
        for solver in ('FLOAT', 'EXACT', 'MANIFOLD'):
            for operation, expected in (('UNION', 16), ('SUBTRACT', 8),
                                         ('INTERSECT', 0), ('SPLIT', 8)):
                for dissolve, per_iteration in ((False, False), (True, False), (True, True)):
                    with self.subTest(solver=solver, operation=operation,
                                      dissolve=dissolve, per_iteration=per_iteration):
                        self.setUp()
                        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
                        bpy.ops.solid.extrude(distance=2)
                        obj = bpy.context.object
                        self.source_select('FACE', lambda f: True)
                        bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (4, 0, 0)})
                        self.source_select('FACE', lambda f: True)
                        self.assertEqual(bpy.ops.solid.boolean(
                            operation_mode=operation, solver=solver,
                            limited_dissolve=dissolve, dissolve_per_iteration=per_iteration),
                            {'FINISHED'})
                        if expected:
                            self.assertAlmostEqual(self.volume(obj), expected, places=4)
                        else:
                            self.assertEqual(obj.data.solid_faces_count, 0)

    def test_solid_selection_sync_preserves_active_vertex_and_edge(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE')
        bpy.ops.solid.extrude(distance=2)
        for mode in ('VERT', 'EDGE'):
            self.source_select(mode, lambda element: True)
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            elements = bm.verts if mode == 'VERT' else bm.edges
            active = next(iter(elements))
            bm.select_history.clear()
            bm.select_history.add(active)
            bpy.ops.mesh.select_linked(delimit=set())
            self.assertEqual(bm.select_history.active, active)
            self.assertTrue(active.select)

    def test_boolean_preserves_mode_and_selects_result_vertices_edges_and_faces(self):
        for mode, expected in (('VERT', (True, False, False)),
                               ('EDGE', (False, True, False)),
                               ('FACE', (False, False, True))):
            with self.subTest(mode=mode):
                self.setUp()
                obj = self.make_boolean_pair()
                bpy.ops.mesh.select_mode(type=mode)
                bpy.ops.mesh.select_all(action='SELECT')
                self.assertEqual(bpy.ops.solid.boolean(), {'FINISHED'})
                self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), expected)
                bm = bmesh.from_edit_mesh(obj.data)
                # These flags drive the native edit overlay's orange vertex/edge/face colors.
                for elements in (bm.verts, bm.edges, bm.faces):
                    self.assertTrue(elements)
                    self.assertTrue(all(element.select for element in elements))
                self.assertIsNotNone(bm.select_history.active)
                self.assertTrue(bm.select_history.active.select)
                self.assertAlmostEqual(self.volume(obj), 12.736, places=4)

    def test_boolean_keep_cutter_and_swapped_subtraction(self):
        self.setUp()
        obj = self.make_boolean_pair()
        self.assertEqual(
            bpy.ops.solid.boolean(operation_mode='SUBTRACT', solver='EXACT', keep_cutter=True),
            {'FINISHED'},
        )
        self.assertAlmostEqual(self.volume(obj), 12.736, places=4)

        self.setUp()
        obj = self.make_boolean_pair()
        self.assertEqual(
            bpy.ops.solid.boolean(operation_mode='SUBTRACT', solver='EXACT', swap_subtract=True),
            {'FINISHED'},
        )
        self.assertAlmostEqual(self.volume(obj), 4.736, places=4)

    def test_manifold_normal_extrusion_rebuilds_missing_samples_after_reload(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object

        for step in range(4):
            top = 2.0 + step * .2
            self.source_select('FACE', lambda face, z=top: all(abs(v.co.z - z) < 1e-5
                                                                for v in face.verts))
            self.assertEqual(
                bpy.ops.solid.extrude(method='MANIFOLD', distance=.2), {'FINISHED'})

        name = obj.name
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'manifold-extrusion-no-display-cache.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)

        obj = bpy.data.objects[name]
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        self.source_select('FACE', lambda face: all(abs(v.co.z - 2.8) < 1e-5
                                                      for v in face.verts))
        self.assertEqual(bpy.ops.solid.extrude(method='MANIFOLD', distance=.2), {'FINISHED'})
        self.assertAlmostEqual(self.volume(obj), 12.0, places=5)

    def test_low_division_corner_sampling_and_single_face_extrusion(self):
        from mathutils import Vector

        for divisions in (2, 3, 6):
            with self.subTest(divisions=divisions):
                self.setUp()
                bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
                bpy.ops.solid.extrude(distance=2)
                bpy.ops.mesh.select_mode(type='EDGE')
                bpy.ops.mesh.select_all(action='SELECT')
                bpy.ops.solid.bevel(offset=.4, segments=divisions, corner_topology='QUADS')
                obj = bpy.context.object
                bpy.ops.object.mode_set(mode='OBJECT')
                center = Vector((.6, -.6, 1.6))
                mesh = obj.data
                patches = []
                for polygon in mesh.polygons:
                    points = [mesh.vertices[i].co for i in polygon.vertices]
                    if all(p.x >= .6 - 1e-5 and p.y <= -.6 + 1e-5 and
                           p.z >= 1.6 - 1e-5 and abs((p - center).length - .4) < 1e-5
                           for p in points):
                        midpoint = sum(points, Vector()) / len(points)
                        self.assertLess(.4 - (midpoint - center).length, .4 * .002)
                        patches.append(polygon.index)
                self.assertTrue(patches)
                layer = mesh.attributes['.solid_face_id']
                chosen = tuple(layer.data[patches[len(patches) // 2]].value)
                aliases = {i for i, data in enumerate(layer.data) if tuple(data.value) == chosen}
                bpy.ops.object.mode_set(mode='EDIT')
                self.source_select('FACE', lambda face: face.index in aliases)
                self.assertEqual(bpy.ops.solid.extrude(distance=.3), {'FINISHED'})
                self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode),
                                 (False, False, True))
                bpy.ops.object.mode_set(mode='OBJECT')
                self.assertGreater(self.volume(obj), 0)

    def test_face_extrusion_closes_caps_and_preserves_face_mode(self):
        # A planar cap uses profile extrusion; a rounded corner face takes the general
        # face-region path. Both should leave a closed body and keep face selection active.
        self.setUp()
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        self.source_select('FACE', lambda face: all(abs(v.co.z - 2) < 1e-5 for v in face.verts))
        self.assertEqual(bpy.ops.solid.extrude(distance=.5), {'FINISHED'})
        self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), (False, False, True))
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertGreater(self.volume(obj), 8)

        self.setUp()
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        self.source_select('VERT', lambda v: v.co.x < -.99 and v.co.y < -.99 and v.co.z < .01)
        self.assertEqual(bpy.ops.solid.bevel(offset=.2, segments=3, vertices_only=True), {'FINISHED'})
        self.source_select(
            'FACE', lambda face: (face.calc_center_median().x < -.75 and
                                  face.calc_center_median().y < -.75 and
                                  face.calc_center_median().z < .25))
        bm = bmesh.from_edit_mesh(obj.data)
        self.assertTrue(any(face.select for face in bm.faces), "Select a rounded corner face")
        self.assertEqual(bpy.ops.solid.extrude(offset=(-.2, -.2, -.2)), {'FINISHED'})
        self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), (False, False, True))
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertGreater(self.volume(obj), 0)

    def test_rectangle_and_editable_cap(self):
        self.assertEqual(bpy.ops.solid.sketch_add(type='RECTANGLE', size=2), {'FINISHED'})
        obj = bpy.context.object
        self.assertTrue(obj.data.is_solid_model)
        self.assertEqual(obj.type, 'MESH')
        self.assertFalse(bpy.ops.curve.draw_spline.poll())
        self.assertFalse(bpy.ops.curve.nurbs_control_points_toggle.poll())
        self.assertEqual(obj.mode, 'EDIT')
        self.assertEqual(bpy.ops.solid.extrude(distance=3), {'FINISHED'})
        self.assertEqual(tuple(bpy.context.scene.tool_settings.mesh_select_mode), (True, False, False))
        self.assertEqual(bpy.ops.solid.extrude(distance=1), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), 64, places=5)
        bpy.ops.object.mode_set(mode='EDIT')
        self.assertEqual(bpy.ops.solid.extrude(distance=1), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), 80, places=5)

    def test_exact_circle_and_negative_sketch_extrusion(self):
        bpy.ops.solid.sketch_add(type='CIRCLE', size=2)
        self.assertEqual(bpy.ops.solid.fill(), {'FINISHED'})
        self.assertEqual(bpy.ops.solid.extrude(distance=-3), {'FINISHED'})
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), math.pi * 4 * 3, delta=0.5)
        for divisions in (8, 7):
            bpy.ops.solid.sketch_add(type='EMPTY')
            bpy.ops.curve.draw_circle(points='0 0 0 2 0 0', preview_points=divisions)
            self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
            obj = bpy.context.object
            bpy.ops.object.mode_set(mode='OBJECT')
            self.assertAlmostEqual(self.volume(obj), math.pi * 4 * 2, delta=0.5)

    def test_circular_cap_bevel_selection_modes(self):
        # Extrusion keeps vertex mode. A complete cap selected through its source vertices
        # must reach the circular bevel path just as a directly selected face does.
        for divisions in (1, 4, 7):
            for mode in ('VERT', 'EDGE', 'FACE'):
                for segments in (0, 4):
                    for ends in ((0,), (2,), (0, 2)):
                        with self.subTest(divisions=divisions, mode=mode, segments=segments, ends=ends):
                            self.setUp()
                            bpy.ops.solid.sketch_add(type='EMPTY')
                            bpy.ops.curve.draw_circle(points='0 0 0 1 0 0', preview_points=divisions)
                            bpy.ops.solid.extrude(distance=2)
                            obj = bpy.context.object
                            if mode == 'VERT':
                                predicate = lambda v: any(abs(v.co.z - end) < 1e-5 for end in ends)
                            elif mode == 'EDGE':
                                predicate = lambda e: any(all(abs(v.co.z - end) < 1e-5 for v in e.verts) for end in ends)
                            else:
                                predicate = lambda f: any(all(abs(v.co.z - end) < 1e-5 for v in f.verts) for end in ends)
                            self.source_select(mode, predicate)
                            before_mode = tuple(bpy.context.tool_settings.mesh_select_mode)
                            self.assertEqual(bpy.ops.solid.bevel(offset=.15, segments=segments), {'FINISHED'})
                            self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), before_mode)
                            bpy.ops.object.mode_set(mode='OBJECT')
                            result_volume = self.volume(obj)
                            self.assertGreater(result_volume, 5.5)
                            self.assertLess(result_volume, 2 * math.pi)

    def test_circular_cap_bevel_collapses_to_shared_apex(self):
        for end in (0.0, 2.0):
            for segments in (1, 4):
                with self.subTest(end=end, segments=segments):
                    self.setUp()
                    bpy.ops.solid.sketch_add(type='EMPTY')
                    bpy.ops.curve.draw_circle(points='0 0 0 1 0 0', preview_points=8)
                    self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
                    obj = bpy.context.object
                    self.source_select(
                        'FACE', lambda face: all(abs(v.co.z - end) < 1e-5 for v in face.verts))
                    self.assertEqual(bpy.ops.solid.bevel(offset=1.2, segments=segments),
                                     {'FINISHED'})
                    bpy.ops.object.mode_set(mode='OBJECT')

                    # Widths past the cylinder radius clamp at the exact pointed limit. The cap
                    # uses triangular conic sectors sharing one apex and remains watertight.
                    apexes = [v.co for v in obj.data.vertices
                              if abs(v.co.x) < 1e-5 and abs(v.co.y) < 1e-5 and
                              abs(v.co.z - end) < 1e-5]
                    self.assertEqual(len(apexes), 1)
                    self.assertGreater(self.volume(obj), 0)
                    if end == 2.0 and segments == 4:
                        with tempfile.TemporaryDirectory() as directory:
                            path = os.path.join(directory, 'pointed-cap.blend')
                            bpy.ops.wm.save_as_mainfile(filepath=path)
                            bpy.ops.wm.open_mainfile(filepath=path)
                        obj = bpy.context.object
                        self.assertTrue(obj.data.is_solid_model)
                        self.assertEqual(sum(abs(v.co.x) < 1e-5 and abs(v.co.y) < 1e-5 and
                                             abs(v.co.z - end) < 1e-5
                                             for v in obj.data.vertices), 1)
                        self.assertGreater(self.volume(obj), 0)

    def test_fill_caps_extruded_loop_on_solid(self):
        bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
        self.assertEqual(bpy.ops.solid.extrude(offset=(0, 0, 2)), {'FINISHED'})
        obj = bpy.context.object
        self.assertEqual(bpy.ops.solid.extrude(distance=1), {'FINISHED'})
        self.assertEqual(bpy.ops.solid.fill(), {'FINISHED'})
        self.source_select('EDGE', lambda edge: all(abs(v.co.z) < 1e-5 for v in edge.verts))
        self.assertEqual(bpy.ops.solid.fill(), {'FINISHED'})
        self.assertEqual(obj.data.solid_faces_count, 4)
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), math.pi * 3, delta=.5)

    def test_extrude_open_fillet_rim_then_fill(self):
        for divisions in (1, 7):
            for mode in ('VERT', 'EDGE'):
                with self.subTest(divisions=divisions, mode=mode):
                    self.setUp()
                    bpy.ops.solid.sketch_add(type='EMPTY')
                    bpy.ops.curve.draw_circle(points='0 0 0 1 0 0', preview_points=divisions)
                    bpy.ops.solid.extrude(distance=2)
                    obj = bpy.context.object
                    self.source_select('FACE', lambda f: all(abs(v.co.z - 2) < 1e-5 for v in f.verts))
                    bpy.ops.solid.bevel(offset=.2, segments=4)
                    self.source_select('FACE', lambda f: all(abs(v.co.z - 2) < 1e-5 for v in f.verts))
                    bpy.ops.solid.delete(type='ONLY_FACE')
                    if mode == 'VERT':
                        predicate = lambda v: abs(v.co.z - 2) < 1e-5
                    else:
                        predicate = lambda e: all(abs(v.co.z - 2) < 1e-5 for v in e.verts)
                    self.source_select(mode, predicate)
                    before_mode = tuple(bpy.context.tool_settings.mesh_select_mode)
                    self.assertEqual(bpy.ops.solid.extrude(distance=1), {'FINISHED'})
                    self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), before_mode)
                    self.assertEqual(bpy.ops.solid.fill(), {'FINISHED'})
                    bpy.ops.object.mode_set(mode='OBJECT')
                    self.assertGreater(self.volume(obj), math.pi * 2)

    def test_drawn_closed_profile_and_reopen(self):
        bpy.ops.solid.sketch_add(type='EMPTY')
        self.assertEqual(bpy.ops.curve.draw_line(points='0 0 0 2 0 0 2 3 0 0 3 0 0 0 0'), {'FINISHED'})
        self.assertEqual(bpy.ops.solid.extrude(distance=4), {'FINISHED'})
        obj = bpy.context.object
        name = obj.name
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), 24, places=5)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'solid.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
            obj = bpy.data.objects[name]
            self.assertTrue(obj.data.is_solid_model)
            self.assertAlmostEqual(self.volume(obj), 24, places=5)
            bpy.context.view_layer.objects.active = obj
            obj.select_set(True)
            bpy.ops.object.mode_set(mode='EDIT')
            self.assertEqual(bpy.ops.solid.extrude(distance=1), {'FINISHED'})

    def test_all_analytic_drawing_commits_and_display_density(self):
        cases = (
            ('line', '0 0 0 2 0 0 2 1 0', {}, (3, 2)),
            ('arc', '0 0 0 2 0 0', {'mode': 'CENTER', 'sweep': math.pi / 2, 'preview_points': 8}, (9, 8)),
            ('arc', '-2 0 0 2 0 0 0 1 0', {'mode': 'ENDPOINTS', 'preview_points': 8}, (9, 8)),
            ('arc', '-2 0 0 2 0 0 0 1 0', {'mode': 'THREE_POINTS', 'preview_points': 8}, (9, 8)),
            ('circle', '0 0 0 2 0 0', {'mode': 'CENTER', 'preview_points': 8}, (8, 8)),
            ('circle', '-2 0 0 2 0 0', {'mode': 'DIAMETER', 'preview_points': 8}, (8, 8)),
            ('circle', '-2 0 0 2 0 0 0 2 0', {'mode': 'THREE_POINTS', 'preview_points': 8}, (8, 8)),
            ('ellipse', '0 0 0 3 0 0 0 2 0', {'preview_points': 8}, (8, 8)),
            ('rectangle', '-2 -1 0 2 -1 0 2 1 0 -2 1 0 -2 -1 0', {}, (4, 4)),
            ('polygon', '0 0 0 2 0 0 1 2 0 0 0 0', {}, (3, 3)),
        )
        for kind, points, kwargs, expected in cases:
            if bpy.context.object and bpy.context.object.mode != 'OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')
            bpy.ops.solid.sketch_add(type='EMPTY')
            self.assertEqual(getattr(bpy.ops.curve, 'draw_' + kind)(points=points, **kwargs), {'FINISHED'}, kind)
            data = bpy.context.object.data
            self.assertEqual((data.solid_vertices_count, data.solid_edges_count), expected, kind)
            self.assertFalse(bpy.ops.mesh.bevel.poll())
            self.assertFalse(bpy.ops.mesh.subdivide.poll())
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.solid.sketch_add(type='CIRCLE', size=2)
        bpy.ops.solid.extrude(distance=3)
        obj = bpy.context.object
        self.assertEqual(obj.data.solid_faces_count, 3)
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(len(obj.data.polygons), 66)
        self.assertLess(len(obj.data.vertices), 200)

    def test_polygon_maximum_divisions_remain_supported(self):
        bpy.ops.solid.sketch_add(type='EMPTY')
        points = [(math.cos(2 * math.pi * i / 257), math.sin(2 * math.pi * i / 257), 0) for i in range(257)]
        points.append(points[0])
        replay = ' '.join('%.17g %.17g %.17g' % point for point in points)
        self.assertEqual(bpy.ops.curve.draw_polygon(points=replay), {'FINISHED'})
        self.assertEqual(bpy.context.object.data.solid_vertices_count, 257)
        self.assertEqual(bpy.context.object.data.solid_edges_count, 257)
        self.assertEqual(bpy.ops.solid.extrude(distance=1), {'FINISHED'})

    def test_transform_uses_source_and_undo_payload_survives_copy(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.transform.translate(value=(3, 2, 1))
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), 8, places=5)
        self.assertAlmostEqual(obj.data.vertices[0].co.z, 1, places=5)
        copy = obj.copy()
        copy.data = obj.data.copy()
        bpy.context.collection.objects.link(copy)
        self.assertTrue(copy.data.is_solid_model)
        self.assertEqual(copy.data.solid_faces_count, 6)
        bpy.context.view_layer.objects.active = copy
        obj.select_set(False)
        copy.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.solid.extrude(distance=1)
        self.assertEqual(obj.data.solid_faces_count, 6)
        self.assertEqual(copy.data.solid_faces_count, 10)

    def test_apply_object_scale_preserves_exact_source(self):
        bpy.ops.solid.sketch_add(type='CIRCLE', size=2)
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode='OBJECT')
        obj.scale = (2, 1, 1)
        self.assertEqual(bpy.ops.object.transform_apply(location=False, rotation=False, scale=True), {'FINISHED'})
        self.assertEqual(tuple(obj.scale), (1, 1, 1))
        self.assertTrue(obj.data.is_solid_model)
        self.assertEqual(obj.data.solid_edges_count, 1)
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.solid.extrude(distance=3)
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), math.pi * 8 * 3, delta=0.2)

    def test_box_side_face_is_an_exact_editable_plane(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        obj = bpy.context.object
        bm = bmesh.from_edit_mesh(obj.data)
        bpy.ops.mesh.select_all(action='DESELECT')
        side = next(face for face in bm.faces if face.normal.x > 0.9)
        side.select_set(True)
        bm.faces.active = side
        bmesh.update_edit_mesh(obj.data)
        self.assertEqual(bpy.ops.solid.extrude(distance=1), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), 12, places=5)

    def test_closed_analytic_circle_scales_about_its_center(self):
        bpy.ops.solid.sketch_add(type='CIRCLE', size=2)
        self.assertEqual(bpy.ops.transform.resize(value=(2, 2, 2)), {'FINISHED'})
        self.assertEqual(bpy.ops.solid.extrude(distance=3), {'FINISHED'})
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode='OBJECT')
        xs = [v.co.x for v in obj.data.vertices]
        self.assertAlmostEqual(min(xs), -4, places=5)
        self.assertAlmostEqual(max(xs), 4, places=5)
        self.assertAlmostEqual(self.volume(obj), math.pi * 16 * 3, delta=0.5)

    def source_select(self, mode, predicate):
        bpy.ops.mesh.select_mode(type=mode)
        bm = bmesh.from_edit_mesh(bpy.context.object.data)
        for element in (*bm.faces, *bm.edges, *bm.verts):
            element.select_set(False)
        elements = {'VERT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[mode]
        for element in elements:
            if predicate(element):
                element.select_set(True)
        bm.select_flush_mode()
        bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)

    def test_duplicate_vertices_edges_faces_and_exact_circle(self):
        for mode in ('VERT', 'EDGE', 'FACE', 'CIRCLE'):
            with self.subTest(mode=mode):
                self.setUp()
                bpy.ops.solid.sketch_add(
                    type='CIRCLE' if mode == 'CIRCLE' else 'LINE' if mode != 'FACE' else 'RECTANGLE')
                if mode in ('FACE', 'CIRCLE'):
                    bpy.ops.solid.extrude(distance=2)
                elif mode == 'VERT':
                    self.source_select('VERT', lambda v: v.co.x > 0)
                else:
                    self.source_select('EDGE', lambda e: True)
                obj = bpy.context.object
                before = (obj.data.solid_vertices_count, obj.data.solid_edges_count, obj.data.solid_faces_count)
                self.assertEqual(
                    bpy.ops.solid.duplicate_move(
                        TRANSFORM_OT_translate={
                            'value': (
                                0, 0, 4)}), {'FINISHED'})
                delta = {'VERT': (1, 0, 0), 'EDGE': (2, 1, 0), 'FACE': (4, 4, 1), 'CIRCLE': (1, 1, 1)}[mode]
                self.assertEqual(
                    (obj.data.solid_vertices_count,
                     obj.data.solid_edges_count,
                     obj.data.solid_faces_count),
                    tuple(
                        a + b for a,
                        b in zip(
                            before,
                            delta)))
                bpy.ops.object.mode_set(mode='OBJECT')
                heights = [v.co.z for v in obj.data.vertices]
                self.assertAlmostEqual(min(heights), 0)
                self.assertAlmostEqual(max(heights), 6 if mode in ('FACE', 'CIRCLE') else 4)
                if mode == 'CIRCLE':
                    self.assertEqual(obj.data.solid_edges_count, 4)

    def test_open_edge_extrusion_and_legacy_poll(self):
        bpy.ops.solid.sketch_add(type='LINE')
        obj = bpy.context.object
        self.assertEqual(bpy.ops.solid.extrude(distance=1), {'FINISHED'})
        self.assertEqual(obj.data.solid_faces_count, 1)
        self.assertEqual(obj.data.solid_edges_count, 4)
        self.assertEqual(bpy.ops.solid.extrude(offset=(0, 2, 1)), {'FINISHED'})
        self.assertEqual(obj.data.solid_faces_count, 2)
        self.assertEqual(tuple(bpy.context.scene.tool_settings.mesh_select_mode), (True, False, False))
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.curve.primitive_bezier_curve_add()
        bpy.ops.object.mode_set(mode='EDIT')
        self.assertFalse(bpy.ops.solid.extrude.poll())

    def test_vertex_extrusion_and_merge(self):
        bpy.ops.solid.sketch_add(type='LINE')
        self.source_select('VERT', lambda v: v.co.x > 0)
        self.assertEqual(bpy.ops.solid.extrude(offset=(0, 2, 0)), {'FINISHED'})
        obj = bpy.context.object
        self.assertEqual((obj.data.solid_vertices_count, obj.data.solid_edges_count), (3, 2))
        self.source_select('VERT', lambda v: v.co.x > 0)
        self.assertEqual(bpy.ops.solid.merge(type='CENTER'), {'FINISHED'})
        self.assertEqual((obj.data.solid_vertices_count, obj.data.solid_edges_count), (2, 1))
        bpy.ops.object.mode_set(mode='OBJECT')
        positions = [v.co for v in obj.data.vertices]
        self.assertTrue(any(abs(v.y - 1) < 1e-6 for v in positions))

    def test_cap_move_and_body_edge_merge(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        self.assertEqual(bpy.ops.transform.translate(value=(.25, .5, 1)), {'FINISHED'})
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertAlmostEqual(self.volume(obj), 12, places=5)
        bpy.ops.object.mode_set(mode='EDIT')
        self.source_select('VERT', lambda v: v.co.z < .01 and v.co.y < 0)
        self.assertEqual(bpy.ops.solid.merge(type='CENTER'), {'FINISHED'})
        self.assertEqual(obj.data.solid_vertices_count, 7)
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertGreater(self.volume(obj), 0)

    def test_bevel_all_edges_and_save(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        self.source_select('EDGE', lambda e: True)
        self.assertEqual(bpy.ops.solid.bevel(offset=.15), {'FINISHED'})
        obj = bpy.context.object
        self.assertGreater(obj.data.solid_faces_count, 6)
        name = obj.name
        bpy.ops.object.mode_set(mode='OBJECT')
        before = self.volume(obj)
        self.assertGreater(before, 0)
        self.assertLess(before, 8)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'beveled-solid.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
            obj = bpy.data.objects[name]
            self.assertTrue(obj.data.is_solid_model)
            self.assertAlmostEqual(self.volume(obj), before, places=6)

    def test_bevel_corner_topology_quads_and_triangles(self):
        properties = bpy.ops.solid.bevel.get_rna_type().properties
        prop = properties.get('corner_topology')
        self.assertIsNotNone(prop)
        self.assertEqual(prop.default, 'QUADS')
        self.assertNotIn('corner_style', properties)

        for mode in ('EDGE', 'VERT'):
            for segments in (3, 4):
                with self.subTest(mode=mode, segments=segments):
                    face_counts = {}
                    for topology in ('QUADS', 'TRIANGLES'):
                        self.setUp()
                        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
                        bpy.ops.solid.extrude(distance=2)
                        self.source_select(mode, lambda element: True)
                        result = bpy.ops.solid.bevel(offset=.15,
                                                     segments=segments,
                                                     corner_topology=topology,
                                                     vertices_only=(mode == 'VERT'))
                        self.assertEqual(result, {'FINISHED'})
                        obj = bpy.context.object
                        self.assertTrue(obj.data.is_solid_model)
                        face_counts[topology] = obj.data.solid_faces_count
                    self.assertLess(face_counts['QUADS'], face_counts['TRIANGLES'])

    def test_quad_corner_spacing_matches_native_mesh_bevel(self):
        from mathutils.kdtree import KDTree

        width = .55
        for segments in (3, 4, 5, 6, 8, 16):
            with self.subTest(segments=segments):
                self.setUp()
                bpy.ops.mesh.primitive_cube_add(size=2, location=(0, 0, 1))
                native = bpy.context.object
                bpy.ops.object.mode_set(mode='EDIT')
                bpy.ops.mesh.select_all(action='SELECT')
                bpy.ops.mesh.bevel(offset=width, segments=segments, profile=.5)
                bpy.ops.object.mode_set(mode='OBJECT')
                reference = [v.co + native.location for v in native.data.vertices
                             if v.co.x > 1 - width - 1e-5 and
                             v.co.y < width - 1 + 1e-5 and v.co.z > 1 - width - 1e-5]

                bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
                bpy.ops.solid.extrude(distance=2)
                self.source_select('EDGE', lambda edge: True)
                bpy.ops.solid.bevel(offset=width, segments=segments, corner_topology='QUADS')
                obj = bpy.context.object
                bpy.ops.object.mode_set(mode='OBJECT')
                tree = KDTree(len(obj.data.vertices))
                for i, vertex in enumerate(obj.data.vertices):
                    tree.insert(vertex.co, i)
                tree.balance()
                # Compare the independent analytic template with native mesh bevel spacing,
                # including odd center triangles, within 0.25% of the fillet width.
                self.assertTrue(reference)
                for point in reference:
                    self.assertLess(tree.find(point)[2], width * .0025)

    def test_vertex_bevel_and_delete(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        bpy.ops.solid.extrude(distance=2)
        self.source_select('VERT', lambda v: True)
        self.assertEqual(bpy.ops.solid.bevel(offset=.15, vertices_only=True), {'FINISHED'})
        obj = bpy.context.object
        self.assertEqual(obj.data.solid_faces_count, 14)
        self.assertEqual(bpy.ops.solid.delete(type='ONLY_FACE'), {'FINISHED'})
        self.assertEqual(obj.data.solid_faces_count, 6)
        self.source_select('VERT', lambda v: True)
        self.assertEqual(bpy.ops.solid.merge(type='CENTER'), {'FINISHED'})
        self.assertEqual((obj.data.solid_vertices_count, obj.data.solid_edges_count,
                         obj.data.solid_faces_count), (1, 0, 0))

    def test_wire_bevel_and_open_chain_fill(self):
        bpy.ops.solid.sketch_add(type='EMPTY')
        bpy.ops.curve.draw_line(points='0 0 0 2 0 0 2 2 0 0 2 0')
        self.assertEqual(bpy.ops.solid.fill(), {'FINISHED'})
        obj = bpy.context.object
        self.assertEqual(obj.data.solid_faces_count, 1)
        bpy.ops.solid.delete(type='ONLY_FACE')
        self.source_select('VERT', lambda v: True)
        self.assertEqual(bpy.ops.solid.bevel(offset=.2, vertices_only=True), {'FINISHED'})
        self.assertEqual((obj.data.solid_vertices_count, obj.data.solid_edges_count), (8, 8))
        self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertGreater(self.volume(obj), 7)


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SolidSketchTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
