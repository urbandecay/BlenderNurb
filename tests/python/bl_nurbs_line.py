# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""NURBS line creation and native line-to-surface editing tests.

Run with: blender --background --factory-startup --python-exit-code 1
--python tests/python/bl_nurbs_line.py
"""

import math
import os
import tempfile
import unittest

import bpy


class NurbsLineTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def add_line(self, **kwargs):
        self.assertEqual(
            bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=False, **kwargs), {'FINISHED'})
        ob = bpy.context.object
        self.assertEqual(ob.type, 'SURFACE')
        return ob

    def extrude(self, displacement):
        bpy.ops.object.mode_set(mode='EDIT')
        self.extrude_selected(displacement)

    def extrude_selected(self, displacement):
        self.assertEqual(bpy.ops.curve.extrude(), {'FINISHED'})
        self.assertEqual(bpy.ops.transform.translate(value=displacement), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')

    def coordinates(self, spline):
        return sorted(tuple(round(v, 5) for v in p.co) for p in spline.points)

    def assert_sheet_area(self, ob, area=None):
        bpy.context.view_layer.update()
        evaluated = ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        try:
            self.assertGreater(len(mesh.polygons), 0)
            evaluated_area = sum(p.area for p in mesh.polygons)
            self.assertGreater(evaluated_area, 0.0)
            if area is not None:
                self.assertAlmostEqual(evaluated_area, area, places=4)
            self.assertTrue(all(math.isfinite(v) for p in mesh.vertices for v in p.co))
        finally:
            evaluated.to_mesh_clear()

    def test_edge_extrudes_to_filled_sheet(self):
        ob = self.add_line()
        spline = ob.data.splines[0]
        self.assertEqual((spline.point_count_u, spline.point_count_v), (2, 1))
        self.assertEqual(self.coordinates(spline), [(-1.0, 0.0, 0.0, 1.0), (1.0, 0.0, 0.0, 1.0)])

        self.extrude((0.0, 2.0, 0.0))
        self.assertIs(bpy.context.object, ob)
        spline = ob.data.splines[0]
        self.assertEqual((spline.point_count_u, spline.point_count_v), (2, 2))
        self.assertEqual((spline.order_u, spline.order_v), (2, 2))
        self.assertTrue(spline.use_endpoint_u and spline.use_endpoint_v)
        self.assertEqual(
            self.coordinates(spline),
            [(-1.0, 0.0, 0.0, 1.0), (-1.0, 2.0, 0.0, 1.0),
             (1.0, 0.0, 0.0, 1.0), (1.0, 2.0, 0.0, 1.0)],
        )
        selected = [p for p in spline.points if p.select]
        self.assertEqual(len(selected), 2)
        self.assertTrue(all(p.co.y == 2.0 for p in selected))
        self.assert_sheet_area(ob, 4.0)

    def test_repeat_extrusion_keeps_existing_sheet(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 1.0, 0.0))
        self.assertEqual(len(ob.data.splines), 2)
        self.assertTrue(all((s.point_count_u, s.point_count_v) == (2, 2)
                            for s in ob.data.splines))
        shared_edge = set(self.coordinates(ob.data.splines[0])) & set(
            self.coordinates(ob.data.splines[1]))
        self.assertEqual(shared_edge,
                         {(-1.0, 2.0, 0.0, 1.0), (1.0, 2.0, 0.0, 1.0)})
        selected = [p for s in ob.data.splines for p in s.points if p.select]
        self.assertEqual(len(selected), 2)
        self.assertTrue(all(p.co.y == 3.0 for p in selected))
        self.assert_sheet_area(ob, 6.0)

    def test_face_extrusion_adds_cap_and_boundary_walls(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.select_all(action='SELECT')
        self.extrude_selected((0.0, 0.0, 3.0))
        self.assertEqual(len(ob.data.splines), 6)
        self.assertTrue(all((s.point_count_u, s.point_count_v) == (2, 2)
                            for s in ob.data.splines))
        self.assert_sheet_area(ob, 32.0)

    def test_primitive_placement_and_radius(self):
        ob = self.add_line(radius=3.0, location=(1.0, 2.0, 3.0), rotation=(0.0, 0.0, math.pi / 2))
        world = sorted(tuple(round(v, 5) for v in (ob.matrix_world @ p.co.xyz))
                       for p in ob.data.splines[0].points)
        self.assertEqual(world, [(1.0, -1.0, 3.0), (1.0, 5.0, 3.0)])

    def test_single_endpoint_extends_line(self):
        ob = self.add_line()
        ob.data.splines[0].points[0].select = False
        self.extrude((1.0, 0.0, 0.0))
        self.assertEqual(len(ob.data.splines), 2)
        source, segment = ob.data.splines
        self.assertEqual((source.point_count_u, source.point_count_v), (2, 1))
        self.assertEqual((segment.point_count_u, segment.point_count_v), (2, 1))
        self.assertEqual(
            self.coordinates(source),
            [(-1.0, 0.0, 0.0, 1.0), (1.0, 0.0, 0.0, 1.0)],
        )
        self.assertEqual(self.coordinates(segment),
                         [(1.0, 0.0, 0.0, 1.0), (2.0, 0.0, 0.0, 1.0)])

    def test_new_edge_supports_native_transforms(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        original = [tuple(p.co) for p in ob.data.splines[0].points if not p.select]
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.transform.translate(value=(0.0, 0.0, 1.0))
        bpy.ops.transform.rotate(value=0.3, orient_axis='Z')
        bpy.ops.transform.resize(value=(1.4, 1.4, 1.4))
        bpy.ops.object.mode_set(mode='OBJECT')
        spline = ob.data.splines[0]
        self.assertEqual([tuple(p.co) for p in spline.points if not p.select], original)
        self.assertEqual(sum(p.select for p in spline.points), 2)
        self.assertTrue(all(math.isfinite(v) for p in spline.points for v in p.co))
        self.assert_sheet_area(ob)

    def test_selected_spline_only_is_extruded(self):
        ob = self.add_line()
        original = self.coordinates(ob.data.splines[0])
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True, location=(0.0, 4.0, 0.0))
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertIs(bpy.context.object, ob)
        self.assertEqual(len(ob.data.splines), 2)
        self.extrude((0.0, 2.0, 0.0))
        self.assertEqual(self.coordinates(ob.data.splines[0]), original)
        self.assertEqual(ob.data.splines[0].point_count_v, 1)
        self.assertEqual(ob.data.splines[1].point_count_v, 2)
        self.assert_sheet_area(ob, 4.0)

    def test_weights_are_copied_to_new_edge(self):
        ob = self.add_line()
        ob.data.splines[0].points[0].co.w = 0.5
        ob.data.splines[0].points[1].co.w = 2.0
        self.extrude((0.0, 2.0, 0.0))
        spline = ob.data.splines[0]
        self.assertEqual([p.co.w for p in spline.points], [0.5, 2.0, 0.5, 2.0])
        self.assert_sheet_area(ob, 4.0)

    def test_save_reload_preserves_exact_control_net(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        name = ob.name
        coordinates = self.coordinates(ob.data.splines[0])
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'nurbs_line_sheet.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
            ob = bpy.data.objects[name]
            self.assertEqual(ob.type, 'SURFACE')
            spline = ob.data.splines[0]
            self.assertEqual((spline.point_count_u, spline.point_count_v), (2, 2))
            self.assertEqual(self.coordinates(spline), coordinates)
            self.assert_sheet_area(ob, 4.0)

    def test_save_reload_preserves_edge_extrusion_topology(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        name = ob.name
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'nurbs_line_topology.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
            ob = bpy.data.objects[name]
            bpy.ops.object.mode_set(mode='EDIT')
            for point in ob.data.splines[0].points:
                point.select = abs(point.co.y - 2.0) < 1e-5
            self.extrude_selected((0.0, 1.0, 0.0))
            self.assertEqual(len(ob.data.splines), 2)
            shared_edge = set(self.coordinates(ob.data.splines[0])) & set(
                self.coordinates(ob.data.splines[1]))
            self.assertEqual(shared_edge,
                             {(-1.0, 2.0, 0.0, 1.0), (1.0, 2.0, 0.0, 1.0)})
            self.assert_sheet_area(ob, 6.0)

    def test_undo_redo_preserves_line_and_sheet(self):
        ob = self.add_line()
        name = ob.name
        bpy.ops.ed.undo_push(message='NURBS line')
        self.extrude((0.0, 2.0, 0.0))
        bpy.ops.ed.undo_push(message='NURBS sheet')
        bpy.ops.ed.undo()
        ob = bpy.data.objects[name]
        self.assertEqual(ob.data.splines[0].point_count_v, 1)
        bpy.ops.ed.redo()
        ob = bpy.data.objects[name]
        self.assertEqual(ob.data.splines[0].point_count_v, 2)
        self.assert_sheet_area(ob, 4.0)

    def test_other_primitives_keep_their_representation(self):
        bpy.ops.curve.primitive_bezier_curve_add(enter_editmode=False)
        self.assertEqual(bpy.context.object.type, 'CURVE')
        self.assertEqual(len(bpy.context.object.data.splines[0].bezier_points), 2)
        bpy.ops.surface.primitive_nurbs_surface_curve_add(enter_editmode=False)
        self.assertEqual(bpy.context.object.type, 'SURFACE')
        self.assertEqual(len(bpy.context.object.data.splines[0].points), 4)
        bpy.ops.curve.primitive_nurbs_circle_add(enter_editmode=False)
        self.assertEqual(bpy.context.object.type, 'CURVE')
        self.assertEqual(len(bpy.context.object.data.splines[0].points), 8)


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    unittest.main()
