# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Mixed source-edit sequences compared with persistent Blender mesh counterparts.

Generate cut-api-source-*.blend with the topology C++ suite first. Checkpoints
contain authoritative NURBS or Mesh data for independent C++ topology checks.
Native preview meshes are checked for evaluation, never used as edit inputs.
"""

import os
from pathlib import Path
import re
import unittest

import bpy
import bmesh


STAGES = (
    'initial', 'delete-face', 'fill', 'duplicate-sheet', 'weld-sheet',
    'loop-cut', 'subdivide-sheet', 'rejected-cut', 'extrude-region', 'delete-cap',
    'duplicate-body', 'no-weld', 'subdivide-body', 'reload',
    'merge-all', 'extrude-point', 'subdivide-wire', 'duplicate-wire', 'delete-edges',
)


class NativeCompositionTest(unittest.TestCase):
    def setUp(self):
        self.output = Path(os.environ.get('BLENDER_NURBS_COMPOSITION_OUTPUT',
                                          '/tmp/blender-nurbs-composition'))
        self.output.mkdir(parents=True, exist_ok=True)
        fixtures = list(Path(os.environ['BLENDER_NURBS_TOPOLOGY_OUTPUT']).glob('cut-api-source-*.blend'))
        self.assertEqual(len(fixtures), 1)
        self.fixture = fixtures[0]
        self.seed = self.fixture.stem.removeprefix('cut-api-source-')

    def totals(self):
        # Mesh and Curve statistics caches require UI notifiers. A fresh layer reads the
        # live edit source in background execution without relying on those cached totals.
        bpy.context.view_layer.update()
        scene = bpy.context.scene
        layer = scene.view_layers.new('Composition source statistics')
        try:
            layer.objects.active = bpy.context.object
            layer.update()
            stats = scene.statistics(layer)
        finally:
            scene.view_layers.remove(layer)
        match = re.search(r'Verts:\d+/(\d+) \| Edges:\d+/(\d+) \| Faces:\d+/(\d+)', stats)
        self.assertIsNotNone(match, stats)
        return tuple(map(int, match.groups()))

    def run_workflow(self, native, cuts):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        if native:
            with bpy.data.libraries.load(str(self.fixture), link=False) as (available, loaded):
                loaded.curves = available.curves
            data = loaded.curves[0]
            data.resolution_u = data.resolution_v = 4
        else:
            data = bpy.data.meshes.new('Composition mesh source')
            data.from_pydata(((0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)), (), ((0, 1, 2, 3),))
        obj = bpy.data.objects.new('Composition native' if native else 'Composition mesh', data)
        bpy.context.scene.collection.objects.link(obj)
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.context.scene.tool_settings.use_snap = False
        bpy.ops.object.mode_set(mode='EDIT')
        ops = bpy.ops.curve if native else bpy.ops.mesh
        label = 'native' if native else 'mesh'
        results = {}
        mode = 'FACE'

        def set_mode(value):
            nonlocal mode
            requested = tuple(value == name for name in ('VERT', 'EDGE', 'FACE'))
            current = tuple(bpy.context.object.data.nurbs_selection_mode) if native else tuple(
                bpy.context.scene.tool_settings.mesh_select_mode)
            if current != requested:
                self.assertEqual(ops.select_mode(type=value), {'FINISHED'})
            mode = value

        def select_all():
            self.assertEqual(ops.select_all(action='SELECT'), {'FINISHED'})

        def checkpoint(stage):
            self.assertEqual(bpy.context.object.mode, 'EDIT', stage)
            actual_mode = tuple(bpy.context.object.data.nurbs_selection_mode) if native else tuple(
                bpy.context.scene.tool_settings.mesh_select_mode)
            self.assertEqual(actual_mode, tuple(mode == name for name in ('VERT', 'EDGE', 'FACE')), stage)
            counts = self.totals()
            results[stage] = counts
            if native:
                self.assertTrue(bpy.context.object.data.is_native_nurbs)
                self.assertEqual(len(bpy.context.object.data.splines), 0)
                evaluated = bpy.context.object.evaluated_get(bpy.context.evaluated_depsgraph_get())
                mesh = evaluated.to_mesh()
                try:
                    if counts[0]:
                        self.assertIsNotNone(mesh, stage)
                        self.assertGreaterEqual(len(mesh.vertices), counts[0], stage)
                        if counts[2]:
                            self.assertGreater(len(mesh.polygons), 0, stage)
                    elif mesh is not None:
                        self.assertEqual(len(mesh.vertices), 0, stage)
                finally:
                    evaluated.to_mesh_clear()
            path = self.output / f'composition-{label}-{cuts}-{stage}.blend'
            self.assertEqual(bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False), {'FINISHED'})
            print('COMPOSITION_CHECKPOINT', label, cuts, stage, counts, flush=True)

        def extrude(offset, point=False):
            operator = bpy.ops.curve.extrude_move if native else (
                bpy.ops.mesh.extrude_vertices_move if point else bpy.ops.mesh.extrude_region_move)
            self.assertEqual(operator(TRANSFORM_OT_translate={'value': offset}), {'FINISHED'})

        def duplicate(offset=(0, 0, 0)):
            self.assertEqual(ops.duplicate(), {'FINISHED'})
            if any(offset):
                self.assertEqual(bpy.ops.transform.translate(value=offset), {'FINISHED'})

        set_mode('FACE')
        select_all()
        checkpoint('initial')
        self.assertEqual(ops.delete(type='ONLY_FACE'), {'FINISHED'})
        checkpoint('delete-face')
        set_mode('EDGE')
        select_all()
        self.assertEqual((bpy.ops.curve.make_segment if native else bpy.ops.mesh.edge_face_add)(), {'FINISHED'})
        checkpoint('fill')
        set_mode('FACE')
        select_all()
        duplicate()
        checkpoint('duplicate-sheet')
        select_all()
        self.assertEqual(ops.remove_doubles(threshold=0.0001), {'FINISHED'})
        checkpoint('weld-sheet')
        set_mode('EDGE')
        if native:
            self.assertEqual(bpy.ops.curve.loopcut_slide(edge_id=self.seed, number_cuts=cuts), {'FINISHED'})
        else:
            # The mesh loop-cut kernel subdivides the opposite edge ring. Select those
            # two source sides; selection after Subdivide is not the modal Ctrl+R result.
            bpy.ops.mesh.select_all(action='DESELECT')
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            for edge in bm.edges:
                edge.select_set(all(v.co.y == 0 for v in edge.verts) or
                                all(v.co.y == 1 for v in edge.verts))
            bm.select_flush_mode()
            bmesh.update_edit_mesh(bpy.context.object.data)
            self.assertEqual(bpy.ops.mesh.subdivide(number_cuts=cuts), {'FINISHED'})
        checkpoint('loop-cut')
        set_mode('FACE')
        select_all()
        self.assertEqual(ops.subdivide(number_cuts=1), {'FINISHED'})
        checkpoint('subdivide-sheet')
        if native:
            with self.assertRaisesRegex(RuntimeError, 'strictly between'):
                bpy.ops.curve.loopcut_slide(edge_id=self.seed, factor=1)
        checkpoint('rejected-cut')
        extrude((0, 0, 1))
        checkpoint('extrude-region')
        self.assertEqual(ops.delete(type='ONLY_FACE'), {'FINISHED'})
        checkpoint('delete-cap')
        set_mode('VERT')
        select_all()
        duplicate((3, 0, 0))
        checkpoint('duplicate-body')
        select_all()
        self.assertEqual(ops.remove_doubles(threshold=0.0001), {'FINISHED'})
        checkpoint('no-weld')
        select_all()
        self.assertEqual(ops.subdivide(number_cuts=1), {'FINISHED'})
        checkpoint('subdivide-body')
        reload_path = self.output / f'composition-{label}-{cuts}-subdivide-body.blend'
        self.assertEqual(bpy.ops.wm.open_mainfile(filepath=str(reload_path)), {'FINISHED'})
        if bpy.context.object.mode != 'EDIT':
            bpy.ops.object.mode_set(mode='EDIT')
        checkpoint('reload')
        select_all()
        self.assertEqual(ops.merge(type='CENTER'), {'FINISHED'})
        checkpoint('merge-all')
        select_all()
        extrude((0, 0, 1), point=True)
        checkpoint('extrude-point')
        select_all()
        self.assertEqual(ops.subdivide(number_cuts=1), {'FINISHED'})
        checkpoint('subdivide-wire')
        select_all()
        duplicate((2, 0, 0))
        checkpoint('duplicate-wire')
        select_all()
        self.assertEqual(ops.delete(type='EDGE'), {'FINISHED'})
        checkpoint('delete-edges')
        self.assertEqual(tuple(results), STAGES)
        return results

    def test_mixed_edit_sequences(self):
        completed = 0
        for cuts in (1, 2):
            with self.subTest(cuts=cuts):
                native = self.run_workflow(True, cuts)
                mesh = self.run_workflow(False, cuts)
                self.assertEqual(native, mesh)
                completed += 1
        if completed == 2:
            print('NATIVE_MESH_COMPOSITION_PASS', flush=True)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
