# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Background operator access: no editor region, mouse, or display-mesh source.

Run the topology C++ tests with BLENDER_NURBS_TOPOLOGY_OUTPUT first to generate
the exact native source fixture. That fixture's filename supplies a known seed
identity; a public native component data API is separate, unfinished work.
"""

import os
from pathlib import Path
import re
import tempfile
import unittest

import bpy


class NativeTopologyOperatorAPI(unittest.TestCase):
    def test_cut_subdivide_save_and_rejected_edit(self):
        directory = Path(os.environ['BLENDER_NURBS_TOPOLOGY_OUTPUT'])
        fixtures = list(directory.glob('cut-api-source-*.blend'))
        self.assertEqual(len(fixtures), 1, 'Generate the exact C++ source fixture first')
        fixture = fixtures[0]
        seed = fixture.stem.removeprefix('cut-api-source-')
        bpy.ops.wm.read_factory_settings(use_empty=True)
        with bpy.data.libraries.load(str(fixture), link=False) as (available, loaded):
            loaded.curves = available.curves
        self.assertEqual(len(loaded.curves), 1)
        obj = bpy.data.objects.new('Native API source', loaded.curves[0])
        bpy.context.scene.collection.objects.link(obj)
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode='EDIT')

        def totals():
            bpy.context.view_layer.update()
            # Background bpy calls do not process the UI notifier that clears
            # cached statistics (mesh operators behave the same way). Query a
            # fresh layer rather than treating that cache as source geometry.
            scene = bpy.context.scene
            fresh = scene.view_layers.new('API fresh statistics')
            try:
                fresh.objects.active = bpy.context.view_layer.objects.active
                fresh.update()
                stats = scene.statistics(fresh)
            finally:
                scene.view_layers.remove(fresh)
            match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
            self.assertIsNotNone(match, stats)
            return tuple(map(int, match.groups()))

        self.assertEqual(totals(), (4, 4, 1))
        self.assertEqual(bpy.ops.curve.duplicate(), {'FINISHED'})
        bpy.ops.curve.select_all(action='SELECT')
        self.assertEqual(totals(), (8, 8, 2))
        self.assertEqual(bpy.ops.curve.remove_doubles(threshold=0.0001), {'FINISHED'})
        self.assertEqual(totals(), (4, 4, 1))
        self.assertEqual(bpy.context.object.mode, 'EDIT')
        self.assertEqual(tuple(bpy.context.object.data.nurbs_selection_mode), (False, False, True))
        self.assertEqual(bpy.ops.curve.loopcut_slide(
            edge_id=seed, number_cuts=2, factor_exact='0.12345678901234567'), {'FINISHED'})
        self.assertEqual(totals(), (8, 10, 3))
        bpy.ops.curve.select_all(action='SELECT')
        self.assertEqual(bpy.ops.curve.subdivide(number_cuts=1), {'FINISHED'})
        self.assertEqual(totals(), (21, 32, 12))
        before = totals()
        with self.assertRaises(RuntimeError):
            bpy.ops.curve.loopcut_slide(edge_id=seed, factor=1)
        self.assertEqual(totals(), before)
        with self.assertRaises(RuntimeError):
            bpy.ops.curve.loopcut_slide(edge_id='9223372036854775807')
        self.assertEqual(totals(), before)
        bpy.ops.object.mode_set(mode='OBJECT')
        self.assertTrue(obj.data.is_native_nurbs)
        self.assertEqual(len(obj.data.splines), 0)
        with tempfile.TemporaryDirectory(prefix='blender-nurbs-api-') as temporary:
            saved = str(Path(temporary) / 'cuts.blend')
            bpy.ops.wm.save_as_mainfile(filepath=saved, check_existing=False)
            bpy.ops.wm.open_mainfile(filepath=saved)
            bpy.ops.object.mode_set(mode='EDIT')
            self.assertEqual(totals(), before)
            self.assertEqual(bpy.ops.curve.subdivide(number_cuts=1), {'FINISHED'})
            self.assertEqual(totals(), (65, 112, 48))
        print('NATIVE_TOPOLOGY_BACKGROUND_PYTHON_API_PASS')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
