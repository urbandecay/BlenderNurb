# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Focused native Boolean operator regressions.

Run with blender --background --factory-startup --python tests/python/bl_solid_boolean_contract.py.
"""

import glob
import os
import subprocess
import sys
import tempfile
import unittest

import bmesh
import bpy


# Blender's --python invocation does not necessarily put this script's directory on sys.path.
_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
if _TESTS_DIR not in sys.path:
    sys.path.insert(0, _TESTS_DIR)

from bl_solid_sketch import SolidSketchTest


class SolidBooleanContractTest(unittest.TestCase):
    # Bind only the shared setup/fixture helpers. Do not inherit SolidSketchTest's full suite.
    setUp = SolidSketchTest.setUp
    source_select = SolidSketchTest.source_select
    volume = SolidSketchTest.volume

    @staticmethod
    def mesh_signature(obj):
        bm = bmesh.from_edit_mesh(obj.data)
        positions = sorted(tuple(round(float(axis), 7) for axis in vertex.co) for vertex in bm.verts)
        return positions

    @staticmethod
    def native_counts(obj):
        return (obj.data.solid_vertices_count,
                obj.data.solid_edges_count,
                obj.data.solid_faces_count)

    def make_three_cylinder_chain(self):
        bpy.ops.solid.sketch_add(type='CIRCLE', size=1)
        self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
        obj = bpy.context.object
        self.source_select('FACE', lambda face: True)
        self.assertEqual(
            bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (1.25, 0, 0)}),
            {'FINISHED'})

        def select_top_cap(center_x):
            self.source_select(
                'FACE',
                lambda face: face.normal.z > .9 and
                all(abs(vertex.co.z - 2) < 1e-5 for vertex in face.verts) and
                abs(face.calc_center_median().x - center_x) < .1)
            bm = bmesh.from_edit_mesh(obj.data)
            self.assertEqual(sum(face.select for face in bm.faces), 1,
                             'Expected to select exactly one cylinder cap')
            bpy.ops.mesh.select_linked()

        # Duplicate the middle cylinder alone to make the final cylinder. The complete three-body
        # selection is restored below before invoking the Boolean operator.
        select_top_cap(1.25)
        self.assertEqual(
            bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (1.25, 0, 0)}),
            {'FINISHED'})
        self.source_select('FACE', lambda face: True)
        self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), (False, False, True))
        self.assertEqual(obj.data.solid_faces_count, 9,
                         'Three-cylinder fixture should contain three complete cylinders')
        return obj

    def call_expected_unsupported(self, obj):
        self.assertEqual(bpy.context.object, obj)
        self.assertEqual(obj.mode, 'EDIT')
        with self.assertRaisesRegex(RuntimeError, 'general curved surface partitioning') as raised:
            bpy.ops.solid.boolean(operation_mode='UNION', solver='SOLID')
        return str(raised.exception)

    def test_disjoint_box_intersection_keeps_empty_solid_in_edit_face_mode(self):
        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        self.assertEqual(bpy.ops.solid.extrude(distance=2), {'FINISHED'})
        obj = bpy.context.object
        self.source_select('FACE', lambda face: True)
        self.assertEqual(
            bpy.ops.solid.duplicate_move(TRANSFORM_OT_translate={'value': (4, 0, 0)}),
            {'FINISHED'})
        self.source_select('FACE', lambda face: True)

        self.assertEqual(
            bpy.ops.solid.boolean(operation_mode='INTERSECT', solver='SOLID'), {'FINISHED'})
        self.assertEqual(obj.mode, 'EDIT')
        self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode), (False, False, True))
        self.assertEqual(obj.data.solid_faces_count, 0,
                         'Disjoint native intersection should publish an empty Solid model')

    def test_curved_chain_failure_capture_replay_and_immutable_source(self):
        obj = self.make_three_cylinder_chain()
        before_mesh = self.mesh_signature(obj)
        before_native = self.native_counts(obj)
        previous_capture_dir = os.environ.get('BLENDER_SOLID_BOOLEAN_CAPTURE_DIR')
        try:
            with tempfile.TemporaryDirectory() as capture_dir:
                os.environ['BLENDER_SOLID_BOOLEAN_CAPTURE_DIR'] = capture_dir
                first_error = self.call_expected_unsupported(obj)
                self.assertEqual(obj.mode, 'EDIT')
                self.assertEqual(tuple(bpy.context.tool_settings.mesh_select_mode),
                                 (False, False, True))
                self.assertEqual(self.mesh_signature(obj), before_mesh)
                self.assertEqual(self.native_counts(obj), before_native)

                captures = glob.glob(os.path.join(capture_dir, '*.solidbool'))
                self.assertEqual(len(captures), 1,
                                 'Expected one native replay capture for the failed operation')
                replay = os.path.join(os.path.dirname(bpy.app.binary_path), 'solid_boolean_replay')
                self.assertTrue(os.path.isfile(replay), 'Native Boolean replay tool is missing')
                replay_result = subprocess.run(
                    [replay, captures[0]], capture_output=True, text=True, timeout=30)
                self.assertEqual(replay_result.returncode, 1, replay_result.stdout + replay_result.stderr)
                self.assertIn('unsupported', replay_result.stdout.lower())
                self.assertIn('general curved surface partitioning', replay_result.stdout)

                # Make the configured capture destination unusable. Diagnostic write failure must
                # leave both the reported solver error and the native source unchanged.
                blocked_destination = os.path.join(capture_dir, 'regular-file')
                with open(blocked_destination, 'w', encoding='utf-8') as blocked_file:
                    blocked_file.write('not a directory')
                os.environ['BLENDER_SOLID_BOOLEAN_CAPTURE_DIR'] = blocked_destination
                second_error = self.call_expected_unsupported(obj)
                self.assertEqual(second_error, first_error,
                                 'Capture I/O failure changed the native Boolean error')
                self.assertEqual(self.mesh_signature(obj), before_mesh)
                self.assertEqual(self.native_counts(obj), before_native)
        finally:
            if previous_capture_dir is None:
                os.environ.pop('BLENDER_SOLID_BOOLEAN_CAPTURE_DIR', None)
            else:
                os.environ['BLENDER_SOLID_BOOLEAN_CAPTURE_DIR'] = previous_capture_dir


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SolidBooleanContractTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
