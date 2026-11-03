# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure the actual Blender sketch/extrude/evaluate path, excluding sketch creation.

Run with a factory-startup background Blender. Timings include operator dispatch, source
construction, display publication and dependency graph evaluation. They do not measure
mouse-to-screen frame latency. The same script can compare the previous implementation.
"""
import bpy
import json
import statistics
import time

for ob in tuple(bpy.data.objects):
    bpy.data.objects.remove(ob, do_unlink=True)

for kind in ('RECTANGLE', 'CIRCLE'):
    timings = []
    dimensions = []
    for iteration in range(30):
        bpy.ops.solid.sketch_add(type=kind, size=2)
        obj = bpy.context.object
        start = time.perf_counter()
        bpy.ops.solid.extrude(distance=3)
        bpy.context.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        dimensions.append((len(mesh.vertices), len(mesh.polygons)))
        evaluated.to_mesh_clear()
        timings.append((time.perf_counter() - start) * 1000)
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.data.objects.remove(obj, do_unlink=True)
    ordered = sorted(timings[5:])
    print('SOLID_BENCHMARK ' + json.dumps(dict(profile=kind,
                                               iterations=len(ordered),
                                               median_ms=statistics.median(ordered),
                                               p95_ms=ordered[int(.95 * (len(ordered) - 1))],
                                               display_vertices=dimensions[-1][0],
                                               display_faces=dimensions[-1][1])),
          flush=True)
