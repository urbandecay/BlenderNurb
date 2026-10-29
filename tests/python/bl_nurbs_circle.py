# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exact native circle replay, closure, transforms and atomic rejection."""
import unittest
import math
import bpy
from mathutils import Vector


class NurbsCircleTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.curve.primitive_nurbs_curve_add()
        self.obj = bpy.context.object
        bpy.ops.object.mode_set(mode='EDIT')

    def samples(self):
        bpy.context.view_layer.update()
        obj = self.obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = obj.to_mesh()
        try:
            return [self.obj.matrix_world @ v.co for v in mesh.vertices
                    if (self.obj.matrix_world @ v.co).x > 5]
        finally:
            obj.to_mesh_clear()

    def test_construction_modes(self):
        for mode, points in (
            ('CENTER', '10 0 0 12 0 0'),
            ('DIAMETER', '8 0 0 12 0 0'),
            ('THREE_POINTS', '8 0 0 10 2 0 12 0 0'),
            ('TAN_TAN', '10 0 0 12 0 0'),
            ('TAN_TAN_TAN', '10 0 0 12 0 0'),
        ):
            with self.subTest(mode=mode):
                self.setUp()
                self.assertEqual(bpy.ops.curve.draw_circle(mode=mode, points=points), {'FINISHED'})
                self.assertTrue(self.obj.data.is_native_nurbs)
                samples = self.samples()
                self.assertGreater(len(samples), 8)
                for p in samples:
                    self.assertAlmostEqual((p - Vector((10, 0, 0))).length, 2, places=5)
                    self.assertAlmostEqual(p.z, 0, places=5)
                self.assertLess(min(p.y for p in samples), -1.9)
                self.assertGreater(max(p.y for p in samples), 1.9)
                # Default segment divisions commit the same number of shared endpoints.
                self.assertIn('Edges:32/33', bpy.context.scene.statistics(bpy.context.view_layer).replace(' ', ''))

    def test_segment_count_and_equal_angles(self):
        for count in (4, 7, 16, 65):
            with self.subTest(count=count):
                self.setUp()
                bpy.ops.curve.draw_circle(points='10 0 0 12 0 0', preview_points=count)
                stats = bpy.context.scene.statistics(bpy.context.view_layer).replace(' ', '')
                self.assertIn(f'Edges:{count}/{count + 1}', stats)
                self.assertIn(f'Verts:{count}/{count + 2}', stats)
                samples = self.samples()
                for i in range(count):
                    angle = 2 * math.pi * i / count
                    target = Vector((10 + 2 * math.cos(angle), 2 * math.sin(angle), 0))
                    self.assertLess(min((p - target).length for p in samples), 2e-5)

    def test_tangent_endpoint_replay(self):
        self.assertEqual(bpy.ops.curve.draw_circle(
            mode='TAN_TAN_TAN', points='10 0 0 12 0 0', preview_points=7,
            anchor_tangent_markers=True, tangent_parameters='0.125 0.25 0.25'), {'FINISHED'})
        stats = bpy.context.scene.statistics(bpy.context.view_layer).replace(' ', '')
        self.assertIn('Edges:9/10', stats)
        self.assertIn('Verts:9/11', stats)
        before = stats
        for value in ('-0.1', '1.2', 'bad', '0.1 0.2 0.3 0.4'):
            with self.assertRaises(RuntimeError):
                bpy.ops.curve.draw_circle(mode='TAN_TAN_TAN', points='10 0 0 12 0 0',
                                          anchor_tangent_markers=True, tangent_parameters=value)
            self.assertEqual(bpy.context.scene.statistics(bpy.context.view_layer).replace(' ', ''), before)

    def test_nonuniform_transform(self):
        self.obj.scale = (2, 3, 4)
        bpy.context.view_layer.update()
        bpy.ops.curve.draw_circle(mode='CENTER', points='5 0 0 6 0 0', plane_normal=(0, 0, 1))
        for p in self.samples():
            self.assertAlmostEqual((p - Vector((10, 0, 0))).length, 2, places=5)

    def test_vertical_and_oblique(self):
        for normal in ((1, 0, 0), (0, 1, 1)):
            self.setUp()
            n = Vector(normal).normalized()
            start = Vector((10, 2, -2)) if normal[0] else Vector((12, 0, 0))
            bpy.ops.curve.draw_circle(points=f'10 0 0 {start.x} {start.y} {start.z}', plane_normal=normal)
            for p in self.samples():
                self.assertAlmostEqual((p - Vector((10, 0, 0))).dot(n), 0, places=5)

    def test_rejection_is_atomic(self):
        for mode, points in (('CENTER', '0 0 0 0 0 0'),
                             ('DIAMETER', '0 0 0 0 0 0'),
                             ('THREE_POINTS', '0 0 0 1 0 0 2 0 0')):
            before = bpy.context.scene.statistics(bpy.context.view_layer)
            with self.assertRaises(RuntimeError):
                bpy.ops.curve.draw_circle(mode=mode, points=points)
            self.assertEqual(bpy.context.scene.statistics(bpy.context.view_layer), before)


if __name__ == '__main__':
    unittest.main(argv=['blender'])
