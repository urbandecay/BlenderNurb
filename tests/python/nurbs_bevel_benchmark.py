# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare native and mesh bevel on an axis aligned native box fixture.

BLENDER_NURBS_BEVEL_SCENE must name a saved native box before beveling, with the
box active. Run in a separate background process. The file is reloaded between
calls and never saved. This measures operation/evaluation time, not viewport FPS.
"""

import json
import os
import statistics
import time

import bpy


source = os.environ['BLENDER_NURBS_BEVEL_SCENE']
iterations = int(os.environ.get('BLENDER_NURBS_BEVEL_ITERATIONS', '5'))
assert iterations >= 2
results = []

for kind in ('nurbs', 'mesh'):
    for segments in (1, 6, 13):
        timings = []
        for iteration in range(iterations):
            bpy.ops.wm.open_mainfile(filepath=source, use_scripts=False)
            obj = bpy.context.object
            assert obj is not None and obj.data.is_native_nurbs
            bpy.context.view_layer.update()
            if kind == 'mesh':
                evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
                display = evaluated.to_mesh()
                try:
                    low = tuple(min(vertex.co[axis] for vertex in display.vertices) for axis in range(3))
                    high = tuple(max(vertex.co[axis] for vertex in display.vertices) for axis in range(3))
                    assert all(max(abs(value) for value in face.normal) > 1 - 1e-6
                               for face in display.polygons), 'Fixture must be an axis aligned box'
                finally:
                    evaluated.to_mesh_clear()
                if obj.mode == 'EDIT':
                    bpy.ops.object.mode_set(mode='OBJECT')
                bpy.ops.object.select_all(action='DESELECT')
                mesh = bpy.data.meshes.new('Matching bevel box')
                mesh.from_pydata(
                    [(x, y, z) for z in (low[2], high[2])
                     for x, y in ((low[0], low[1]), (high[0], low[1]),
                                  (high[0], high[1]), (low[0], high[1]))],
                    [],
                    [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
                     (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)],
                )
                replica = bpy.data.objects.new('Matching bevel box', mesh)
                bpy.context.scene.collection.objects.link(replica)
                replica.matrix_world = obj.matrix_world.copy()
                bpy.data.objects.remove(obj, do_unlink=True)
                replica.select_set(True)
                bpy.context.view_layer.objects.active = replica
                obj = replica
                bpy.ops.object.mode_set(mode='EDIT')
                bpy.ops.mesh.select_all(action='SELECT')
            else:
                if obj.mode != 'EDIT':
                    bpy.ops.object.mode_set(mode='EDIT')
                bpy.ops.curve.select_mode(type='EDGE')
                bpy.ops.curve.select_all(action='SELECT')
            bpy.context.view_layer.update()
            start = time.perf_counter()
            operator = bpy.ops.curve.bevel if kind == 'nurbs' else bpy.ops.mesh.bevel
            assert operator(offset=0.25, segments=segments) == {'FINISHED'}
            operated = time.perf_counter()
            bpy.context.view_layer.update()
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            display = evaluated.to_mesh()
            try:
                assert display and len(display.polygons)
                counts = (len(display.vertices), len(display.polygons))
            finally:
                evaluated.to_mesh_clear()
            end = time.perf_counter()
            if iteration:
                timings.append(tuple(1000 * value for value in
                                     (operated - start, end - operated, end - start)))
        results.append({
            'kind': kind, 'segments': segments, 'display_vertices_and_faces': counts,
            'median_ms': {name: round(statistics.median(row[i] for row in timings), 3)
                          for i, name in enumerate(('operator', 'evaluation', 'total'))},
        })

print('NURBS_BEVEL_BENCHMARK', json.dumps(results), flush=True)
