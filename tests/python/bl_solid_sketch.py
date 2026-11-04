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
    def test_subdivided_fillet_does_not_resample_each_patch_as_a_whole_corner(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        obj = bpy.context.object
        bpy.ops.solid.extrude(distance=2)
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        self.assertEqual(bpy.ops.solid.bevel(offset=.2, segments=20), {'FINISHED'})
        self.assertEqual(obj.data.solid_faces_count, 3446)
        bm = bmesh.from_edit_mesh(obj.data)
        self.assertLess(len(bm.faces), 40000)
        self.assertTrue(all(v.co.length < 4 for v in bm.verts))

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
            self.assertLess((rotation @ a - b).length, 1e-4)
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

    def test_rectangle_and_editable_cap(self):
        self.assertEqual(bpy.ops.solid.sketch_add(type='RECTANGLE', size=2), {'FINISHED'})
        obj = bpy.context.object
        self.assertTrue(obj.data.is_solid_model)
        self.assertEqual(obj.type, 'MESH')
        self.assertFalse(bpy.ops.curve.draw_spline.poll())
        self.assertFalse(bpy.ops.curve.nurbs_control_points_toggle.poll())
        self.assertEqual(obj.mode, 'EDIT')
        self.assertEqual(bpy.ops.solid.extrude(distance=3), {'FINISHED'})
        self.assertEqual(tuple(bpy.context.scene.tool_settings.mesh_select_mode), (False, False, True))
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
