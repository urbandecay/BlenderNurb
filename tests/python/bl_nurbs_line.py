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

    def test_fill_two_duplicated_lines_keeps_sharp_corners_and_allows_duplicate_extrude(self):
        for separate_add in (False, True):
            bpy.ops.wm.read_factory_settings(use_empty=True)
            ob = self.add_line()
            bpy.ops.object.mode_set(mode='EDIT')
            if separate_add:
                self.assertEqual(bpy.ops.curve.primitive_nurbs_curve_add(
                    enter_editmode=True, location=(0, 2, 0)), {'FINISHED'})
            else:
                self.assertEqual(bpy.ops.curve.duplicate_move(
                    TRANSFORM_OT_translate={'value': (0, 2, 0)}), {'FINISHED'})
            bpy.ops.curve.select_all(action='SELECT')
            self.assertEqual(bpy.ops.curve.make_segment(), {'FINISHED'})
            bpy.ops.object.mode_set(mode='OBJECT')
            self.assertEqual(len(ob.data.splines), 1)
            patch = ob.data.splines[0]
            self.assertEqual((patch.point_count_u, patch.point_count_v, patch.order_u, patch.order_v),
                             (2, 2, 2, 2))
            self.assertFalse(patch.use_cyclic_u or patch.use_cyclic_v)
            self.assertTrue(patch.use_endpoint_u and patch.use_endpoint_v)
            self.assertEqual(self.coordinates(patch),
                             [(-1, 0, 0, 1), (-1, 2, 0, 1), (1, 0, 0, 1), (1, 2, 0, 1)])
            self.assert_sheet_area(ob, 4)
            bpy.ops.object.mode_set(mode='EDIT')
            self.assertEqual(bpy.ops.curve.make_segment(), {'CANCELLED'})
            self.assertEqual(bpy.ops.curve.duplicate_move(
                TRANSFORM_OT_translate={'value': (0, 0, 2)}), {'FINISHED'})
            self.extrude_selected((0, 0, 1))
            self.assertEqual(len(ob.data.splines), 7)
            self.assertTrue(all((s.point_count_u, s.point_count_v, s.order_u, s.order_v) == (2, 2, 2, 2)
                                for s in ob.data.splines))
            self.assert_sheet_area(ob, 20)

    def test_fill_after_individual_cube_vertex_extrusions_allows_third_face_use(self):
        ob = self.add_line()
        self.extrude((0, 2, 0))
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.select_all(action='SELECT')
        self.extrude_selected((0, 0, 1))
        self.assertEqual(len(ob.data.splines), 6)

        def select(positions):
            for spline in ob.data.splines:
                for point in spline.points:
                    point.select = tuple(point.co.xyz) in positions

        for x in (-1, 1):
            bpy.ops.object.mode_set(mode='EDIT')
            select({(x, 0, 1)})
            self.extrude_selected((0, 0, 1))
        bpy.ops.object.mode_set(mode='EDIT')
        select({(-1, 0, 2), (1, 0, 2)})
        self.assertEqual(bpy.ops.curve.make_segment(), {'FINISHED'})
        select({(-1, 0, 1), (1, 0, 1), (-1, 0, 2), (1, 0, 2)})
        self.assertEqual(bpy.ops.curve.make_segment(), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(len(ob.data.splines), 7)
        self.assertTrue(all((s.point_count_u, s.point_count_v) == (2, 2)
                            for s in ob.data.splines))
        self.assert_sheet_area(ob, 18)
        bpy.ops.object.mode_set(mode='EDIT')
        self.extrude_selected((0, -1, 0))
        self.assertEqual(len(ob.data.splines), 11)
        self.assert_sheet_area(ob)

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

    def test_transforming_a_shared_edge_keeps_both_faces_connected(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 1.0, 0.0))
        self.assertEqual(len(ob.data.splines), 2)

        bpy.ops.object.mode_set(mode='EDIT')
        for spline_index, spline in enumerate(ob.data.splines):
            for point in spline.points:
                point.select = spline_index == 0 and abs(point.co.y - 2.0) < 1e-5

        transforms = (
            (bpy.ops.transform.translate, {'value': (0.0, 0.0, 1.0)}),
            (bpy.ops.transform.rotate, {'value': 0.25, 'orient_axis': 'Z'}),
            (bpy.ops.transform.resize, {'value': (1.2, 1.2, 1.2)}),
        )
        for transform, kwargs in transforms:
            self.assertEqual(transform(**kwargs), {'FINISHED'})
            patch_points = [
                {tuple(round(value, 5) for value in point.co.xyz) for point in spline.points}
                for spline in ob.data.splines
            ]
            self.assertEqual(len(patch_points[0] & patch_points[1]), 2)

        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(
            sum(point.select for spline in ob.data.splines for point in spline.points), 4)
        self.assert_sheet_area(ob)

    def test_shared_vertex_transform_updates_all_three_faces(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 2.0, 0.0))

        bpy.ops.object.mode_set(mode='EDIT')
        for spline_index, spline in enumerate(ob.data.splines):
            for point in spline.points:
                point.select = spline_index == 0 and abs(point.co.x - 1.0) < 1e-5
        self.extrude_selected((2.0, 0.0, 0.0))
        self.assertEqual(len(ob.data.splines), 3)

        bpy.ops.object.mode_set(mode='EDIT')
        for spline_index, spline in enumerate(ob.data.splines):
            for point in spline.points:
                point.select = (spline_index == 0 and abs(point.co.x - 1.0) < 1e-5 and
                                abs(point.co.y - 2.0) < 1e-5)
        self.assertEqual(
            bpy.ops.transform.translate(value=(0.0, 0.0, 1.0)), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')

        shared_vertex_points = [
            point
            for spline in ob.data.splines
            for point in spline.points
            if abs(point.co.x - 1.0) < 1e-5 and abs(point.co.y - 2.0) < 1e-5
        ]
        self.assertEqual(len(shared_vertex_points), 3)
        self.assertTrue(all(abs(point.co.z - 1.0) < 1e-5 for point in shared_vertex_points))
        self.assert_sheet_area(ob)

    def test_extruded_shared_edge_stays_connected_across_three_faces(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 2.0, 0.0))

        bpy.ops.object.mode_set(mode='EDIT')
        for spline_index, spline in enumerate(ob.data.splines):
            for point in spline.points:
                point.select = (spline_index == 0 and abs(point.co.y - 2.0) < 1e-5)
        self.extrude_selected((0.0, 0.0, 1.0))
        self.assertEqual(len(ob.data.splines), 3)

        shared_edge_points = [
            point
            for spline in ob.data.splines
            for point in spline.points
            if abs(point.co.y - 2.0) < 1e-5 and abs(point.co.z) < 1e-5
        ]
        self.assertEqual(len(shared_edge_points), 6)

        bpy.ops.object.mode_set(mode='EDIT')
        for spline_index, spline in enumerate(ob.data.splines):
            for point in spline.points:
                point.select = (spline_index == 0 and abs(point.co.y - 2.0) < 1e-5 and
                                abs(point.co.z) < 1e-5)
        self.assertEqual(
            bpy.ops.transform.translate(value=(1.0, 0.0, 0.0)), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')

        moved_shared_edge_points = [
            point
            for spline in ob.data.splines
            for point in spline.points
            if (abs(point.co.y - 2.0) < 1e-5 and abs(point.co.z) < 1e-5 and
                (abs(point.co.x) < 1e-5 or abs(point.co.x - 2.0) < 1e-5))
        ]
        self.assertEqual(len(moved_shared_edge_points), 6)
        self.assert_sheet_area(ob)

    def test_select_linked_expands_across_shared_nurbs_face_edges(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 1.0, 0.0))
        self.assertEqual(len(ob.data.splines), 2)

        bpy.ops.object.mode_set(mode='EDIT')
        for spline in ob.data.splines:
            for point in spline.points:
                point.select = False
        ob.data.splines[0].points[0].select = True

        self.assertEqual(bpy.ops.curve.select_linked(), {'FINISHED'})
        self.assertEqual(sum(point.select for spline in ob.data.splines
                             for point in spline.points), 8)
        self.assertTrue(all(point.select for spline in ob.data.splines
                            for point in spline.points))

        self.assertEqual(bpy.ops.curve.extrude(), {'FINISHED'})
        self.assertGreater(len(ob.data.splines), 2)
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assert_sheet_area(ob)

    def test_select_linked_does_not_select_disconnected_nurbs_face_island(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))

        bpy.ops.object.mode_set(mode='EDIT')
        for point in ob.data.splines[0].points:
            point.select = False
        bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True, location=(10.0, 0.0, 0.0))
        self.extrude_selected((0.0, 2.0, 0.0))
        self.assertEqual(len(ob.data.splines), 2)

        bpy.ops.object.mode_set(mode='EDIT')
        for spline in ob.data.splines:
            for point in spline.points:
                point.select = False
        ob.data.splines[0].points[0].select = True

        self.assertEqual(bpy.ops.curve.select_linked(), {'FINISHED'})
        selected_points = [point.select for spline in ob.data.splines for point in spline.points]
        self.assertEqual(sum(selected_points), 4)
        self.assertEqual(sum(selected_points[:4]), 4)
        self.assertEqual(sum(selected_points[4:]), 0)

    def test_select_linked_does_not_cross_hidden_shared_edge(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 1.0, 0.0))

        bpy.ops.object.mode_set(mode='EDIT')
        first, second = ob.data.splines
        shared_points = set(self.coordinates(first)) & set(self.coordinates(second))
        for point in second.points:
            if tuple(round(value, 5) for value in point.co) in shared_points:
                point.hide = True
            point.select = False
        for point in first.points:
            point.select = False
        first.points[0].select = True

        self.assertEqual(bpy.ops.curve.select_linked(), {'FINISHED'})
        selected_points = [point.select for spline in ob.data.splines for point in spline.points]
        self.assertEqual(sum(selected_points), 4)
        self.assertEqual(sum(selected_points[:4]), 4)
        self.assertEqual(sum(selected_points[4:]), 0)

    def test_select_linked_does_not_bridge_hidden_face_vertices(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))

        bpy.ops.object.mode_set(mode='EDIT')
        face = next(spline for spline in ob.data.splines if spline.point_count_v == 2)
        for point in face.points:
            point.select = False
        face.points[1].hide = True
        face.points[2].hide = True
        face.points[0].select = True

        self.assertEqual(bpy.ops.curve.select_linked(), {'FINISHED'})
        selected_points = [point for point in face.points if point.select]
        self.assertEqual(selected_points, [face.points[0]])

    def test_select_linked_expands_across_shared_nurbs_line_vertices(self):
        ob = self.add_line()
        ob.data.splines[0].points[0].select = False
        self.extrude((1.0, 0.0, 0.0))
        self.assertEqual(len(ob.data.splines), 2)

        bpy.ops.object.mode_set(mode='EDIT')
        for spline in ob.data.splines:
            for point in spline.points:
                point.select = False
        ob.data.splines[0].points[1].select = True

        self.assertEqual(bpy.ops.curve.select_linked(), {'FINISHED'})
        self.assertEqual(sum(point.select for spline in ob.data.splines
                             for point in spline.points), 4)
        self.assertTrue(all(point.select for spline in ob.data.splines
                            for point in spline.points))

    def test_select_linked_follows_wire_between_faces_at_shared_vertices(self):
        ob = self.add_line()

        bpy.ops.object.mode_set(mode='EDIT')
        for point in ob.data.splines[0].points:
            point.select = point.co.x < 0.0
        self.extrude_selected((0.0, 2.0, 0.0))

        bpy.ops.object.mode_set(mode='EDIT')
        for spline in ob.data.splines:
            for point in spline.points:
                point.select = (abs(point.co.x - 1.0) < 1e-5 and
                                abs(point.co.y) < 1e-5)
        self.extrude_selected((0.0, -2.0, 0.0))

        bpy.ops.object.mode_set(mode='EDIT')
        for spline in ob.data.splines:
            is_first_branch = any(abs(point.co.x + 1.0) < 1e-5 for point in spline.points) and any(
                abs(point.co.y - 2.0) < 1e-5 for point in spline.points)
            for point in spline.points:
                point.select = is_first_branch
        self.extrude_selected((0.0, 0.0, 1.0))

        bpy.ops.object.mode_set(mode='EDIT')
        for spline in ob.data.splines:
            is_second_branch = any(abs(point.co.x - 1.0) < 1e-5 for point in spline.points) and any(
                abs(point.co.y + 2.0) < 1e-5 for point in spline.points)
            for point in spline.points:
                point.select = is_second_branch
        self.extrude_selected((0.0, 0.0, 1.0))

        self.assertEqual(len(ob.data.splines), 3)
        bpy.ops.object.mode_set(mode='EDIT')
        faces = [spline for spline in ob.data.splines if spline.point_count_v == 2]
        self.assertEqual(len(faces), 2)
        for spline in ob.data.splines:
            for point in spline.points:
                point.select = False
        faces[0].points[0].select = True

        self.assertEqual(bpy.ops.curve.select_linked(), {'FINISHED'})
        self.assertEqual(
            sorted(sum(point.select for point in spline.points) for spline in ob.data.splines),
            [2, 4, 4],
        )

    def test_face_extrusion_ignores_synced_shared_edge_points(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 1.0, 0.0))
        self.assertEqual(len(ob.data.splines), 2)

        bpy.ops.object.mode_set(mode='EDIT')
        first, second = ob.data.splines
        shared_points = set(self.coordinates(first)) & set(self.coordinates(second))
        self.assertEqual(len(shared_points), 2)
        for spline in ob.data.splines:
            for point in spline.points:
                point.select = False
        for point in first.points:
            point.select = True
        for point in second.points:
            if tuple(round(value, 5) for value in point.co) in shared_points:
                point.select = True

        self.assertEqual(bpy.ops.curve.extrude(), {'FINISHED'})
        self.assertEqual(len(ob.data.splines), 6)
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assert_sheet_area(ob)

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

    def test_face_cap_can_be_extruded_again_without_moving_instead(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.select_all(action='SELECT')
        self.extrude_selected((0.0, 0.0, 3.0))
        self.assertEqual(len(ob.data.splines), 6)

        bpy.ops.object.mode_set(mode='EDIT')
        self.extrude_selected((0.0, 0.0, 1.0))

        self.assertEqual(len(ob.data.splines), 10)
        self.assert_sheet_area(ob)

    def test_duplicate_move_line_leaves_original_in_place(self):
        ob = self.add_line()
        original = self.coordinates(ob.data.splines[0])
        bpy.ops.object.mode_set(mode='EDIT')
        self.assertEqual(bpy.ops.curve.duplicate_move(
            TRANSFORM_OT_translate={'value': (0.0, 2.0, 0.0)}), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(len(ob.data.splines), 2)
        self.assertEqual(self.coordinates(ob.data.splines[0]), original)
        self.assertTrue(all(point.co.y == 2.0 for point in ob.data.splines[1].points))

    def test_duplicate_move_face_is_disconnected_and_can_extrude(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        original = self.coordinates(ob.data.splines[0])
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.select_all(action='SELECT')
        self.assertEqual(bpy.ops.curve.duplicate_move(
            TRANSFORM_OT_translate={'value': (0.0, 0.0, 2.0)}), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(len(ob.data.splines), 2)
        self.assertEqual(self.coordinates(ob.data.splines[0]), original)
        self.assertTrue(all(point.co.z == 2.0 for point in ob.data.splines[1].points))
        bpy.ops.object.mode_set(mode='EDIT')
        self.extrude_selected((0.0, 0.0, 1.0))
        self.assertEqual(len(ob.data.splines), 7)
        self.assertEqual(self.coordinates(ob.data.splines[0]), original)
        self.assertEqual(sum(all(point.co.z == 3.0 for point in spline.points)
                             for spline in ob.data.splines), 1)

    def test_duplicated_face_region_keeps_its_shared_edge(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 2.0, 0.0))
        originals = [self.coordinates(spline) for spline in ob.data.splines]
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.select_all(action='SELECT')
        bpy.ops.curve.duplicate_move(TRANSFORM_OT_translate={'value': (4.0, 0.0, 0.0)})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(len(ob.data.splines), 4)
        self.assertEqual([self.coordinates(spline) for spline in ob.data.splines[:2]], originals)
        bpy.ops.object.mode_set(mode='EDIT')
        self.extrude_selected((0.0, 0.0, 1.0))
        self.assertEqual(len(ob.data.splines), 12)  # Two original sheets, ten faces on the copy.
        self.assertEqual([self.coordinates(spline) for spline in ob.data.splines[:2]], originals)

    def test_shared_edge_extrusion_is_independent_of_selected_patch_copies(self):
        results = []
        for copies in (1, 2):
            bpy.ops.wm.read_factory_settings(use_empty=True)
            ob = self.add_line()
            self.extrude((0.0, 2.0, 0.0))
            self.extrude((0.0, 2.0, 0.0))
            bpy.ops.object.mode_set(mode='EDIT')
            for index, spline in enumerate(ob.data.splines):
                for point in spline.points:
                    point.select = index < copies and point.co.y == 2.0
            self.extrude_selected((0.0, 0.0, 1.0))
            self.assertEqual(len(ob.data.splines), 3)
            results.append(sorted(self.coordinates(spline) for spline in ob.data.splines))
        self.assertEqual(results[0], results[1])

    def test_attached_face_extrusion_replaces_source_face(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        self.extrude((0.0, 2.0, 0.0))
        bpy.ops.object.mode_set(mode='EDIT')
        for index, spline in enumerate(ob.data.splines):
            for point in spline.points:
                point.select = index == 0
        self.extrude_selected((0.0, 0.0, 1.0))
        self.assertEqual(len(ob.data.splines), 6)
        self.assertEqual(sum(all(point.co.z == 0.0 for point in spline.points)
                             for spline in ob.data.splines), 1)
        self.assertEqual(sum(all(point.co.z == 1.0 for point in spline.points)
                             for spline in ob.data.splines), 1)

    def test_repeated_face_extrusion_does_not_leave_internal_cap(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.select_all(action='SELECT')
        self.extrude_selected((0.0, 0.0, 3.0))
        bpy.ops.object.mode_set(mode='EDIT')
        self.extrude_selected((0.0, 0.0, 1.0))
        self.assertEqual(len(ob.data.splines), 10)
        self.assertFalse(any(all(point.co.z == 3.0 for point in spline.points)
                             for spline in ob.data.splines))

    def test_single_vertex_duplicate_is_independent_and_can_extend(self):
        ob = self.add_line()
        bpy.ops.object.mode_set(mode='EDIT')
        ob.data.splines[0].points[1].select = False
        before = self.coordinates(ob.data.splines[0])
        bpy.ops.curve.duplicate_move(TRANSFORM_OT_translate={'value': (0.0, 1.0, 0.0)})
        self.assertEqual(len(ob.data.splines), 2)
        self.assertEqual(self.coordinates(ob.data.splines[0]), before)
        self.assertEqual(ob.data.splines[1].point_count_u, 1)
        self.assertEqual(tuple(ob.data.splines[1].points[0].co), (-1.0, 1.0, 0.0, 1.0))
        self.extrude_selected((0.0, 1.0, 0.0))
        self.assertEqual(len(ob.data.splines), 2)
        self.assertEqual(self.coordinates(ob.data.splines[0]), before)
        self.assertEqual({point.co.y for point in ob.data.splines[1].points}, {1.0, 2.0})

    def make_twice_extruded_cube(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.select_all(action='SELECT')
        self.extrude_selected((0.0, 0.0, 2.0))
        bpy.ops.object.mode_set(mode='EDIT')
        self.extrude_selected((0.0, 0.0, 1.0))
        self.assertEqual(len(ob.data.splines), 10)
        bpy.ops.object.mode_set(mode='EDIT')
        return ob

    def test_delete_top_vertices_then_extrude_surviving_cube_rim(self):
        ob = self.make_twice_extruded_cube()
        self.assertEqual(bpy.ops.curve.delete(type='VERT'), {'FINISHED'})
        ob.update_from_editmode()
        self.assertEqual(len(ob.data.splines), 5)
        self.assertTrue(all((s.point_count_u, s.point_count_v) == (2, 2) for s in ob.data.splines))
        self.assertEqual(max(point.co.z for s in ob.data.splines for point in s.points), 2.0)
        original = [self.coordinates(spline) for spline in ob.data.splines]
        for spline in ob.data.splines:
            for point in spline.points:
                point.select = point.co.z == 2.0
        self.extrude_selected((0.0, 0.0, 1.0))
        self.assertEqual(len(ob.data.splines), 9)
        self.assertEqual([self.coordinates(spline) for spline in ob.data.splines[:5]], original)
        self.assert_sheet_area(ob)

    def test_delete_top_segments_keeps_valid_source_wires(self):
        ob = self.make_twice_extruded_cube()
        self.assertEqual(bpy.ops.curve.delete(type='SEGMENT'), {'FINISHED'})
        ob.update_from_editmode()
        self.assertEqual(sum(s.point_count_v == 2 for s in ob.data.splines), 5)
        self.assertEqual(sum(s.point_count_v == 1 for s in ob.data.splines), 4)
        bpy.ops.curve.select_all(action='SELECT')
        self.assertEqual(bpy.ops.curve.duplicate(), {'FINISHED'})
        bpy.ops.transform.translate(value=(4.0, 0.0, 0.0))
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(len(ob.data.splines), 18)

    def test_delete_opposite_corners_keeps_two_editable_source_points(self):
        ob = self.add_line()
        self.extrude((0.0, 2.0, 0.0))
        bpy.ops.object.mode_set(mode='EDIT')
        for i, point in enumerate(ob.data.splines[0].points):
            point.select = i in {0, 3}
        self.assertEqual(bpy.ops.curve.delete(type='VERT'), {'FINISHED'})
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(len(ob.data.splines), 2)
        self.assertTrue(all(len(spline.points) == 1 for spline in ob.data.splines))
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.select_all(action='SELECT')
        self.extrude_selected((0.0, 0.0, 1.0))
        self.assertEqual(len(ob.data.splines), 2)
        self.assertTrue(all(len(spline.points) == 2 for spline in ob.data.splines))

    def test_delete_reload_then_continue_extrusion(self):
        ob = self.make_twice_extruded_cube()
        name = ob.name
        bpy.ops.curve.delete(type='VERT')
        bpy.ops.object.mode_set(mode='OBJECT')
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'nurbs_after_top_delete.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
            ob = bpy.data.objects[name]
            bpy.ops.object.mode_set(mode='EDIT')
            for spline in ob.data.splines:
                for point in spline.points:
                    point.select = point.co.z == 2.0
            self.extrude_selected((0.0, 0.0, 1.0))
            self.assertEqual(len(ob.data.splines), 9)

    def test_duplicate_save_reload_keeps_copy_independent_for_extrusion(self):
        ob = self.add_line()
        original = self.coordinates(ob.data.splines[0])
        name = ob.name
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.curve.duplicate_move(TRANSFORM_OT_translate={'value': (0.0, 2.0, 0.0)})
        bpy.ops.object.mode_set(mode='OBJECT')
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'independent_nurbs_duplicate.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
            ob = bpy.data.objects[name]
            self.assertEqual(self.coordinates(ob.data.splines[0]), original)
            bpy.ops.object.mode_set(mode='EDIT')
            self.extrude_selected((0.0, 1.0, 0.0))
            self.assertEqual(len(ob.data.splines), 2)
            self.assertEqual(self.coordinates(ob.data.splines[0]), original)
            self.assertEqual((ob.data.splines[1].point_count_u, ob.data.splines[1].point_count_v), (2, 2))
            self.assertEqual({point.co.y for point in ob.data.splines[1].points}, {2.0, 3.0})

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
