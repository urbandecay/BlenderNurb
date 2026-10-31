# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native arc operator replay, geometry, persistence and rejection checks.

blender --background --factory-startup --python-exit-code 1 --python tests/python/bl_nurbs_arc.py
"""

import math
from pathlib import Path
import re
import tempfile
import unittest

import bpy
from mathutils import Vector


class NurbsArcTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.curve.primitive_nurbs_curve_add()
        self.obj = bpy.context.object
        bpy.ops.object.mode_set(mode='EDIT')

    def totals(self):
        scene = bpy.context.scene
        layer = scene.view_layers.new('Arc statistics')
        try:
            layer.objects.active = self.obj
            layer.update()
            stats = scene.statistics(layer)
        finally:
            scene.view_layers.remove(layer)
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
        self.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def world_arc_points(self):
        bpy.context.view_layer.update()
        evaluated = self.obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            points = [self.obj.matrix_world @ v.co for v in mesh.vertices]
            points = [p for p in points if p.x > 5]
            self.assertGreater(len(points), 4)
            return points
        finally:
            evaluated.to_mesh_clear()

    def test_center_clockwise_major_and_full_circle(self):
        for sweep in (math.pi / 2, -math.pi / 2, 1.75 * math.pi, 2 * math.pi, -2 * math.pi):
            with self.subTest(sweep=sweep):
                self.setUp()
                self.assertEqual(bpy.ops.curve.draw_arc(
                    mode='CENTER', points='10 0 0 11 0 0', sweep=sweep), {'FINISHED'})
                expected_vertices = 34 if abs(sweep) == 2 * math.pi else 35
                self.assertEqual(self.totals(), (expected_vertices, 33, 0))
                self.assertTrue(self.obj.data.is_native_nurbs)
                self.assertEqual(len(self.obj.data.splines), 0)
                points = self.world_arc_points()
                for point in points:
                    self.assertAlmostEqual((point - Vector((10, 0, 0))).length, 1, places=5)
                end = Vector((10 + math.cos(sweep), math.sin(sweep), 0))
                self.assertLess(min((p - end).length for p in points), 2e-6)
                if abs(sweep) == math.pi / 2:
                    self.assertTrue(all(p.y * sweep >= -1e-6 for p in points))

    def test_fill_closes_dissolved_semicircle_with_a_straight_edge(self):
        for count in (4, 7, 32):
            with self.subTest(count=count):
                self.setUp()
                bpy.ops.curve.draw_arc(mode='CENTER', points='10 0 0 12 0 0',
                                       sweep=math.pi, preview_points=count)
                bpy.ops.curve.dissolve_limited()
                self.assertEqual(self.totals(), (4, 2, 0))
                arc_points = self.world_arc_points()
                self.assertEqual(bpy.ops.curve.make_segment(), {'FINISHED'})
                self.assertEqual(self.totals(), (4, 3, 0))
                bpy.context.view_layer.update()
                evaluated = self.obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
                mesh = evaluated.to_mesh()
                try:
                    diameter_length = sum(
                        (mesh.vertices[e.vertices[0]].co - mesh.vertices[e.vertices[1]].co).length
                        for e in mesh.edges
                        if all(mesh.vertices[i].co.x > 5 and
                               abs(mesh.vertices[i].co.y) < 1e-5 and
                               abs(mesh.vertices[i].co.z) < 1e-5 for i in e.vertices))
                    self.assertAlmostEqual(diameter_length, 4, places=5,
                                           msg='F must add the straight diameter')
                finally:
                    evaluated.to_mesh_clear()
                after = self.world_arc_points()
                for p in arc_points:
                    self.assertLess(min((p - q).length for q in after), 1e-5,
                                    'Adding the chord must preserve the curved boundary')
                with tempfile.TemporaryDirectory() as directory:
                    path = str(Path(directory) / 'closed-semicircle.blend')
                    bpy.ops.wm.save_as_mainfile(filepath=path, check_existing=False)
                    bpy.ops.wm.open_mainfile(filepath=path)
                self.obj = bpy.context.object
                self.assertTrue(self.obj.data.is_native_nurbs)
                self.assertEqual(self.totals(), (4, 3, 0))

    def test_fill_closed_semicircle_creates_a_face(self):
        for count, sweep in ((4, math.pi), (7, math.pi), (32, math.pi), (32, -math.pi)):
            with self.subTest(count=count, sweep=sweep):
                self.setUp()
                bpy.ops.curve.draw_arc(mode='CENTER', points='10 0 0 12 0 0',
                                       sweep=sweep, preview_points=count)
                bpy.ops.curve.dissolve_limited()
                bpy.ops.curve.make_segment()
                self.assertEqual(bpy.ops.curve.make_segment(), {'FINISHED'})
                self.assertEqual(self.totals(), (4, 3, 1))

                def check_face():
                    bpy.context.view_layer.update()
                    evaluated = self.obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
                    mesh = evaluated.to_mesh()
                    try:
                        self.assertGreater(len(mesh.polygons), 0)
                        self.assertAlmostEqual(sum(p.area for p in mesh.polygons), 2 * math.pi,
                                               delta=0.03)
                        for p in mesh.polygons:
                            self.assertGreater(p.area, 0)
                            self.assertTrue(all(abs(mesh.vertices[i].co.z) < 1e-5
                                                for i in p.vertices))
                    finally:
                        evaluated.to_mesh_clear()

                check_face()
                # Repeating F must not duplicate the existing face.
                bpy.ops.curve.make_segment()
                self.assertEqual(self.totals(), (4, 3, 1))
                with tempfile.TemporaryDirectory() as directory:
                    path = str(Path(directory) / 'semicircle-face.blend')
                    bpy.ops.wm.save_as_mainfile(filepath=path, check_existing=False)
                    bpy.ops.wm.open_mainfile(filepath=path)
                self.obj = bpy.context.object
                self.assertTrue(self.obj.data.is_native_nurbs)
                self.assertEqual(self.totals(), (4, 3, 1))
                check_face()

    def test_endpoints_bulge_and_three_point_major(self):
        for mode in ('ENDPOINTS', 'THREE_POINTS'):
            for height in (-2, -1, 0.25, 1, 2):
                with self.subTest(mode=mode, height=height):
                    self.setUp()
                    self.assertEqual(bpy.ops.curve.draw_arc(
                        mode=mode, points=f'9 0 0 11 0 0 10 {height} 0'), {'FINISHED'})
                    self.assertEqual(self.totals(), (35, 33, 0))
                    center_y = (height * height - 1) / (2 * height)
                    radius = abs(height - center_y)
                    points = self.world_arc_points()
                    for point in points:
                        self.assertAlmostEqual((point - Vector((10, center_y, 0))).length,
                                               radius, places=5)
                    peak = max(p.y for p in points) if height > 0 else min(p.y for p in points)
                    # Display tessellation need not sample the exact arc midpoint.
                    self.assertAlmostEqual(peak, height, delta=0.02)

    def test_segment_count_and_equal_angles(self):
        for count in (4, 7, 32):
            for sweep in (0.3, 2.8, -5.7):
                with self.subTest(count=count, sweep=sweep):
                    self.setUp()
                    bpy.ops.curve.draw_arc(mode='CENTER', points='10 0 0 12 0 0',
                                           sweep=sweep, preview_points=count)
                    self.assertEqual(self.totals(), (count + 3, count + 1, 0))
                    samples = self.world_arc_points()
                    for i in range(count + 1):
                        angle = sweep * i / count
                        target = Vector((10 + 2 * math.cos(angle), 2 * math.sin(angle), 0))
                        self.assertLess(min((p - target).length for p in samples), 2e-5)

    def test_world_circle_under_nonuniform_scale(self):
        self.obj.scale = (2, 3, 4)
        self.obj.rotation_euler = (0, 0, 0.2)
        bpy.context.view_layer.update()
        center = self.obj.matrix_world @ Vector((5, 0, 0))
        self.assertEqual(bpy.ops.curve.draw_arc(
            mode='CENTER', points='5 0 0 5.5 0 0', sweep=1.5 * math.pi), {'FINISHED'})
        for point in self.world_arc_points():
            self.assertAlmostEqual((point - center).length, 1, places=5)

    def test_vertical_chord_and_file_round_trip(self):
        self.assertEqual(bpy.ops.curve.draw_arc(
            mode='THREE_POINTS', points='10 0 -1 10 0 1 10 1 0'), {'FINISHED'})
        for p in self.world_arc_points():
            self.assertAlmostEqual(p.x, 10, places=6)
            self.assertAlmostEqual(math.hypot(p.y, p.z), 1, places=5)
        before = self.totals()
        bpy.ops.object.mode_set(mode='OBJECT')
        with tempfile.TemporaryDirectory(prefix='nurbs-arc-') as directory:
            filename = str(Path(directory) / 'arc.blend')
            bpy.ops.wm.save_as_mainfile(filepath=filename, check_existing=False)
            bpy.ops.wm.open_mainfile(filepath=filename)
            self.obj = bpy.context.object
            self.assertTrue(self.obj.data.is_native_nurbs)
            bpy.ops.object.mode_set(mode='EDIT')
            self.assertEqual(self.totals(), before)
            self.assertEqual(bpy.ops.curve.draw_arc(
                mode='CENTER', points='15 0 0 16 0 0', sweep=math.pi), {'FINISHED'})
            self.assertEqual(self.totals(), (before[0] + 33, before[1] + 32, 0))

    def test_rejection_is_atomic(self):
        # Enter native geometry before checking rejected operations.
        bpy.ops.curve.draw_arc(mode='CENTER', points='10 0 0 11 0 0', sweep=math.pi)
        before = self.totals()
        for options in (
                dict(mode='CENTER', points='10 0 0 10 0 0', sweep=math.pi),
                dict(mode='CENTER', points='10 0 0 11 0 0', sweep=0),
                dict(mode='THREE_POINTS', points='9 0 0 11 0 0 10 0 0'),
                dict(mode='ENDPOINTS', points='9 0 0 11 0 0 10 0 0'),
                dict(mode='THREE_POINTS', points='9 0 0 11 0 0')):
            with self.subTest(options=options):
                with self.assertRaises(RuntimeError):
                    bpy.ops.curve.draw_arc(**options)
                self.assertEqual(self.totals(), before)
        bpy.ops.object.mode_set(mode='OBJECT')
        self.obj.scale.x = 0
        bpy.ops.object.mode_set(mode='EDIT')
        with self.assertRaises(RuntimeError):
            bpy.ops.curve.draw_arc(mode='CENTER', points='10 0 0 11 0 0', sweep=math.pi)
        self.assertEqual(self.totals(), before)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
