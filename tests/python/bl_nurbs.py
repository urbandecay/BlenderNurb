# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native NURBS RNA and operator checks, run with Blender in background mode."""

import math
import os
import tempfile
import unittest
from types import SimpleNamespace

import bpy
from mathutils import Euler, Vector
from bl_ui.space_view3d import VIEW3D_MT_curve_add, VIEW3D_MT_nurbs_add


class _MenuLayout:
    def __init__(self):
        self.operator_context = None
        self.operator_calls = []
        self.menu_calls = []

    def operator(self, operator_id, **properties):
        call = SimpleNamespace(operator_id=operator_id, properties=properties)
        self.operator_calls.append(call)
        return call

    def menu(self, menu_id, **properties):
        self.menu_calls.append(menu_id)

    def separator(self):
        pass


class NurbsGeometryTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_circle_data(self):
        bpy.ops.nurbs.primitive_circle_add(radius=2.5, enter_editmode=False)
        geometry = bpy.context.object.data
        self.assertEqual(geometry.users, 1)
        self.assertEqual(len(geometry.curves), 1)
        curve = geometry.curves[0]
        self.assertEqual((curve.degree, curve.order, curve.dimension), (2, 3, 3))
        self.assertTrue(curve.component_id.isdecimal())
        self.assertTrue(curve.is_property_readonly("component_id"))
        self.assertTrue(curve.is_property_readonly("knots"))
        self.assertTrue(curve.is_rational)
        self.assertEqual(tuple(curve.domain), (0.0, 4.0))
        self.assertEqual(tuple(curve.knots), (0.0, 0.0, 1.0, 1.0, 2.0, 2.0, 3.0, 3.0, 4.0, 4.0))
        self.assertEqual(len(curve.control_vertices), 9)
        component_ids = [curve.component_id]
        component_ids.extend(vertex.component_id for vertex in curve.control_vertices)
        self.assertEqual(len(component_ids), len(set(component_ids)))
        self.assertEqual(tuple(curve.control_vertices[0].position), (2.5, 0.0, 0.0))
        self.assertAlmostEqual(curve.control_vertices[1].weight, math.sqrt(0.5), places=6)
        for vertex in curve.control_vertices:
            self.assertTrue(vertex.select)
            self.assertTrue(vertex.component_id.isdecimal())
            self.assertTrue(vertex.is_property_readonly("component_id"))
            self.assertTrue(vertex.is_property_readonly("position"))
            self.assertTrue(vertex.is_property_readonly("weight"))
        self.assertTrue(curve.is_property_readonly("degree"))

    def test_line_and_arc_constructors(self):
        cursor = bpy.context.scene.cursor
        cursor.location = (2.0, -3.0, 4.0)
        cursor.rotation_euler = Euler((0.31, -0.47, 0.22), 'XYZ')
        bpy.ops.nurbs.primitive_line_add(
            radius=1.0, align='CURSOR', enter_editmode=False
        )
        line_object = bpy.context.object
        self.assertEqual(tuple(line_object.location), tuple(cursor.location))
        for actual, expected in zip(line_object.rotation_euler, cursor.rotation_euler):
            self.assertAlmostEqual(actual, expected, places=5)
        line = line_object.data.curves[0]
        self.assertEqual((line.degree, line.order, line.dimension), (1, 2, 3))
        self.assertFalse(line.is_rational)
        self.assertEqual(tuple(line.domain), (0.0, 1.0))
        self.assertEqual(len(line.control_vertices), 2)
        self.assertEqual(tuple(line.control_vertices[0].position), (-1.0, 0.0, 0.0))
        self.assertEqual(tuple(line.control_vertices[1].position), (1.0, 0.0, 0.0))

        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.nurbs.primitive_arc_add(
            radius=2.5, start_angle=math.radians(30), sweep_angle=math.pi
        )
        arc = bpy.context.object.data.curves[0]
        self.assertEqual((arc.degree, arc.order, arc.dimension), (2, 3, 3))
        self.assertTrue(arc.is_rational)
        self.assertEqual(tuple(arc.domain), (0.0, 2.0))
        self.assertEqual(len(arc.control_vertices), 5)
        self.assertAlmostEqual(
            arc.control_vertices[0].position[0], 2.5 * math.cos(math.radians(30)), delta=2e-6
        )
        self.assertAlmostEqual(
            arc.control_vertices[0].position[1], 2.5 * math.sin(math.radians(30)), delta=2e-6
        )
        self.assertAlmostEqual(arc.control_vertices[1].weight, math.sqrt(0.5), places=6)
        self.assertEqual(tuple(arc.knots), (0.0, 0.0, 1.0, 1.0, 2.0, 2.0))

        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.nurbs.primitive_path_add(radius=1.0, align='WORLD', enter_editmode=False)
        path = bpy.context.object.data.curves[0]
        self.assertEqual((path.degree, path.order, path.dimension), (4, 5, 3))
        self.assertFalse(path.is_rational)
        self.assertEqual(tuple(path.domain), (0.0, 1.0))
        self.assertEqual(tuple(path.knots), (0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0))
        self.assertEqual(
            [tuple(vertex.position) for vertex in path.control_vertices],
            [(-2.0, 0.0, 0.0), (-1.0, 0.0, 0.0), (0.0, 0.0, 0.0),
             (1.0, 0.0, 0.0), (2.0, 0.0, 0.0)],
        )

    def test_add_menu_routes_to_native_nurbs(self):
        curve_layout = _MenuLayout()
        VIEW3D_MT_curve_add.draw(SimpleNamespace(layout=curve_layout), bpy.context)
        self.assertIn("VIEW3D_MT_nurbs_add", curve_layout.menu_calls)
        self.assertNotIn(
            "curve.primitive_nurbs_curve_add",
            [call.operator_id for call in curve_layout.operator_calls],
        )

        nurbs_layout = _MenuLayout()
        VIEW3D_MT_nurbs_add.draw(SimpleNamespace(layout=nurbs_layout), bpy.context)
        calls = {call.operator_id: call for call in nurbs_layout.operator_calls}
        self.assertEqual(
            set(calls),
            {
                "object.add",
                "nurbs.primitive_line_add",
                "nurbs.primitive_arc_add",
                "nurbs.primitive_circle_add",
                "nurbs.primitive_path_add",
            },
        )
        self.assertEqual(calls["object.add"].type, "NURBS")

    def test_id_collection_lifecycle(self):
        geometry = bpy.data.nurbs_geometries.new("Programmatic")
        self.assertEqual(len(geometry.curves), 1)
        self.assertIn(geometry, bpy.data.nurbs_geometries[:])

        group = bpy.data.collections.new("NURBS Group")
        bpy.context.scene.collection.children.link(group)
        obj = bpy.data.objects.new("Programmatic Object", geometry)
        group.objects.link(obj)
        self.assertEqual(geometry.users, 1)
        self.assertEqual(tuple(obj.users_collection), (group,))
        self.assertIn(obj.name, group.all_objects)
        bpy.context.view_layer.update()
        self.assertIs(bpy.context.view_layer.objects.get(obj.name), obj)

        # Moving the object between collections must keep its native NURBS ID
        # attached, as Blender's Outliner collection workflow expects.
        bpy.context.scene.collection.objects.link(obj)
        group.objects.unlink(obj)
        self.assertEqual(geometry.users, 1)
        self.assertIn(obj.name, bpy.context.scene.collection.all_objects)
        bpy.data.collections.remove(group)

        bpy.data.objects.remove(obj, do_unlink=True)
        self.assertEqual(geometry.users, 0)
        bpy.data.nurbs_geometries.remove(geometry)
        self.assertNotIn("Programmatic", bpy.data.nurbs_geometries.keys())

    def test_native_edit_transform(self):
        bpy.ops.object.add(type='NURBS')
        geometry = bpy.context.object.data
        before = [tuple(vertex.position) for vertex in geometry.curves[0].control_vertices]
        bpy.ops.object.mode_set(mode='EDIT')
        self.assertEqual(bpy.context.mode, 'EDIT_NURBS')
        bpy.ops.nurbs.select_all(action='SELECT')
        bpy.ops.transform.translate(value=(0.0, 0.0, 2.0))
        after = [tuple(vertex.position) for vertex in geometry.curves[0].control_vertices]
        self.assertEqual(after, [(x, y, z + 2.0) for x, y, z in before])
        bpy.ops.nurbs.select_all(action='DESELECT')
        self.assertFalse(any(vertex.select for vertex in geometry.curves[0].control_vertices))
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(bpy.context.mode, 'OBJECT')

    def test_custom_orientation_from_curve_only_nurbs(self):
        bpy.ops.object.add(type='NURBS')
        obj = bpy.context.object
        obj.rotation_euler = (0.31, -0.47, 0.22)
        obj.scale = (1.8, 0.65, 1.3)
        bpy.ops.object.mode_set(mode='EDIT')

        view_area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
        window_region = next(
            region for region in view_area.regions if region.type == 'WINDOW'
        )
        orientation_name = 'NURBS Curve Test'
        with bpy.context.temp_override(area=view_area, region=window_region):
            result = bpy.ops.transform.create_orientation(
                name=orientation_name, use=True, overwrite=True
            )
        self.assertEqual(result, {'FINISHED'})

        orientation = bpy.context.scene.transform_orientation_slots[0].custom_orientation
        self.assertEqual(orientation.name, orientation_name)
        matrix = orientation.matrix
        axes = [Vector(matrix[index]) for index in range(3)]
        for axis in axes:
            self.assertAlmostEqual(axis.length, 1.0, places=5)
        for index, axis in enumerate(axes):
            for other_axis in axes[index + 1:]:
                self.assertAlmostEqual(axis.dot(other_axis), 0.0, places=5)
        self.assertGreater(abs(matrix[0][2]) + abs(matrix[1][2]), 1e-3)

    def _two_circle_edit_data(self):
        objects = []
        for x in (-3.0, 3.0):
            bpy.ops.nurbs.primitive_circle_add(enter_editmode=False)
            obj = bpy.context.object
            obj.location.x = x
            objects.append(obj)
        bpy.ops.object.select_all(action='SELECT')
        bpy.context.view_layer.objects.active = objects[0]
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.nurbs.select_all(action='SELECT')
        return objects

    @staticmethod
    def _world_control_positions(objects):
        return [
            [obj.matrix_world @ Vector(vertex.position) for vertex in obj.data.curves[0].control_vertices]
            for obj in objects
        ]

    def test_individual_origins_and_median_pivot(self):
        for operation in ('ROTATE', 'SCALE'):
            for pivot in ('INDIVIDUAL_ORIGINS', 'MEDIAN_POINT'):
                with self.subTest(operation=operation, pivot=pivot):
                    if bpy.context.mode != 'OBJECT':
                        bpy.ops.object.mode_set(mode='OBJECT')
                    bpy.ops.wm.read_factory_settings(use_empty=True)
                    objects = self._two_circle_edit_data()
                    before = self._world_control_positions(objects)
                    centers = [
                        sum(points, Vector()) / len(points)
                        for points in before
                    ]
                    median = sum((sum(points, Vector()) for points in before), Vector()) / sum(
                        len(points) for points in before
                    )
                    bpy.context.scene.tool_settings.transform_pivot_point = pivot
                    view_area = next(
                        area for area in bpy.context.screen.areas if area.type == 'VIEW_3D'
                    )
                    window_region = next(
                        region for region in view_area.regions if region.type == 'WINDOW'
                    )

                    with bpy.context.temp_override(area=view_area, region=window_region):
                        if operation == 'ROTATE':
                            bpy.ops.transform.rotate(
                                value=math.pi / 2, orient_axis='Z', orient_type='GLOBAL'
                            )
                            transform = lambda point, center: center + Vector(
                                ((point - center).y, -(point - center).x, 0.0)
                            )
                        else:
                            bpy.ops.transform.resize(value=(2.0, 2.0, 2.0))
                            transform = lambda point, center: center + 2.0 * (point - center)

                    after = self._world_control_positions(objects)
                    for curve_i, points in enumerate(before):
                        pivot_center = centers[curve_i] if pivot == 'INDIVIDUAL_ORIGINS' else median
                        for point_i, point in enumerate(points):
                            expected = transform(point, pivot_center)
                            self.assertTrue((after[curve_i][point_i] - expected).length < 1e-4)
                    bpy.ops.object.mode_set(mode='OBJECT')

    def test_duplicate_ownership(self):
        bpy.ops.nurbs.primitive_circle_add(enter_editmode=False)
        source = bpy.context.object
        bpy.ops.object.duplicate()
        duplicate = bpy.context.object
        self.assertNotEqual(source.data, duplicate.data)
        self.assertEqual(source.data.users, 1)
        self.assertEqual(duplicate.data.users, 1)
        original = [tuple(vertex.position) for vertex in source.data.curves[0].control_vertices]
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.nurbs.select_all(action='SELECT')
        bpy.ops.transform.translate(value=(0.0, 0.0, 3.0))
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertEqual(original, [tuple(vertex.position) for vertex in source.data.curves[0].control_vertices])
        self.assertEqual(
            [(x, y, z + 3.0) for x, y, z in original],
            [tuple(vertex.position) for vertex in duplicate.data.curves[0].control_vertices],
        )

    def test_save_reopen(self):
        bpy.ops.nurbs.primitive_circle_add(enter_editmode=False)
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.nurbs.select_all(action='SELECT')
        bpy.ops.transform.translate(value=(0.0, 0.0, 1.25))
        bpy.ops.object.mode_set(mode='OBJECT')
        curve = bpy.context.object.data.curves[0]
        before = (
            curve.component_id, curve.degree, curve.order, curve.dimension, curve.is_rational,
            tuple(curve.domain), tuple(curve.knots),
            [(vertex.component_id, tuple(vertex.position), vertex.weight, vertex.select) for vertex in curve.control_vertices],
        )
        with tempfile.TemporaryDirectory() as directory:
            filepath = os.path.join(directory, "native_nurbs.blend")
            bpy.ops.wm.save_as_mainfile(filepath=filepath)
            bpy.ops.wm.open_mainfile(filepath=filepath)
            curve = bpy.context.object.data.curves[0]
            after = (
                curve.component_id, curve.degree, curve.order, curve.dimension, curve.is_rational,
                tuple(curve.domain), tuple(curve.knots),
                [(vertex.component_id, tuple(vertex.position), vertex.weight, vertex.select) for vertex in curve.control_vertices],
            )
            self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main(argv=[__file__])
