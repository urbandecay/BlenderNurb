# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare Edit Mode move/evaluation costs on a native NURBS scene and its display mesh.

Run in a separate background process with BLENDER_NURBS_DRAG_SCENE pointing to a .blend
whose active object is native NURBS. The scene is reloaded between cases and never saved.
This measures scripted updates, not viewport frame rate.
"""

import json
import os
import statistics
import time

import bpy


source = os.environ['BLENDER_NURBS_DRAG_SCENE']
results = []
for kind, resolution in (('nurbs', None), ('mesh', None), ('nurbs', 6), ('nurbs', 3)):
    bpy.ops.wm.open_mainfile(filepath=source, use_scripts=False)
    obj = bpy.context.object
    assert obj is not None and obj.data.is_native_nurbs
    if obj.mode == 'EDIT':
        bpy.ops.object.mode_set(mode='OBJECT')
    if kind == 'nurbs' and resolution is not None:
        obj.data.resolution_u = resolution
        obj.data.resolution_v = resolution
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    display = evaluated.to_mesh()
    try:
        vertices, polygons = len(display.vertices), len(display.polygons)
    finally:
        evaluated.to_mesh_clear()
    if kind == 'mesh':
        mesh = bpy.data.meshes.new_from_object(evaluated)
        replica = bpy.data.objects.new('Benchmark display mesh', mesh)
        bpy.context.scene.collection.objects.link(replica)
        bpy.ops.object.select_all(action='DESELECT')
        replica.select_set(True)
        bpy.context.view_layer.objects.active = replica
        bpy.data.objects.remove(obj, do_unlink=True)
        obj = replica
    bpy.ops.object.mode_set(mode='EDIT')
    if kind == 'nurbs':
        bpy.ops.curve.select_all(action='SELECT')
    else:
        bpy.ops.mesh.select_all(action='SELECT')
    timings = []
    for i in range(12):
        start = time.perf_counter()
        assert bpy.ops.transform.translate(value=(0.001, 0, 0)) == {'FINISHED'}
        moved = time.perf_counter()
        bpy.context.view_layer.update()
        updated = time.perf_counter()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        display = evaluated.to_mesh()
        assert display and len(display.polygons) > 0
        evaluated.to_mesh_clear()
        end = time.perf_counter()
        if i >= 2:
            timings.append(tuple(1000 * value for value in
                                 (moved - start, updated - moved, end - updated, end - start)))
    labels = ('operator', 'view_layer_update', 'evaluated_mesh', 'total')
    median_ms = {label: round(statistics.median(row[i] for row in timings), 3)
                 for i, label in enumerate(labels)}
    results.append({'kind': kind, 'resolution': resolution or 'default',
                    'vertices': vertices, 'polygons': polygons, 'median_ms': median_ms})
print('NURBS_DRAG_BENCHMARK', json.dumps(results), flush=True)
