# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare normal methods on the same analytic solid with real solid edit transforms.

Run in background mode. Reports CPU updates, not viewport FPS. Both modes use
the same solid source and display topology; native mode must retain no custom normals.
"""

import bpy, bmesh, json, statistics, time
from array import array

ITERATIONS = 35
WARMUP = 5
ANGLE = .01

def clear():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    for mesh in list(bpy.data.meshes):
        if mesh.users == 0:
            bpy.data.meshes.remove(mesh)

def activate(obj):
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj

def build_solid():
    clear()
    bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
    solid = bpy.context.object
    assert bpy.ops.solid.extrude(distance=2) == {'FINISHED'}
    bpy.ops.mesh.select_mode(type='EDGE')
    bpy.ops.mesh.select_all(action='SELECT')
    assert bpy.ops.solid.bevel(offset=.2, segments=1) == {'FINISHED'}
    bpy.ops.object.mode_set(mode='OBJECT')
    counts = (len(solid.data.vertices), len(solid.data.edges), len(solid.data.polygons), len(solid.data.corner_normals))
    return solid, counts

def read_display_normals(obj):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(dg)
    mesh = evaluated.to_mesh()
    try:
        values = array('f', [0.0]) * (3 * len(mesh.corner_normals))
        mesh.corner_normals.foreach_get('vector', values)
        return len(mesh.vertices), len(mesh.polygons), sum(values[::97])
    finally:
        evaluated.to_mesh_clear()

def bench(kind):
    obj, counts = build_solid()
    activate(obj)
    bpy.ops.object.shade_smooth(solid_normal_method='ANALYTIC' if kind == 'analytic' else 'BLENDER')
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_mode(type='VERT')
    bpy.ops.mesh.select_all(action='SELECT')
    read_display_normals(obj)
    samples = []
    for i in range(ITERATIONS):
        wall0, cpu0 = time.perf_counter(), time.process_time()
        result = bpy.ops.transform.rotate('EXEC_DEFAULT', value=ANGLE, orient_axis='Z', orient_type='GLOBAL', center_override=(0,0,0))
        if result != {'FINISHED'}:
            raise RuntimeError((kind, result))
        display_counts = read_display_normals(obj)
        wall = (time.perf_counter() - wall0) * 1000
        cpu = (time.process_time() - cpu0) * 1000
        if i >= WARMUP:
            samples.append((wall, cpu))
    bm = bmesh.from_edit_mesh(obj.data)
    has_custom = bm.loops.layers.float_vector.get('custom_normal') is not None
    assert has_custom == (kind == 'analytic'), 'Wrong normal method during rotation'
    return {
        'method': kind,
        'display_vertices_edges_faces_corners': counts,
        'median_wall_ms_rotation_update_and_full_normal_read': round(statistics.median(x[0] for x in samples), 3),
        'median_cpu_ms': round(statistics.median(x[1] for x in samples), 3),
        'updates_per_second_background_not_fps': round(1000 / statistics.median(x[0] for x in samples), 1),
        'samples': len(samples),
        'last_display_counts': display_counts,
    }

# Alternate methods on freshly constructed, identical solids in each trial.
import os
order = os.environ.get('NORMAL_BENCH_ORDER', 'analytic,native,native,analytic,analytic,native').split(',')
results = [bench(kind) for kind in order]
print('SOLID_NATIVE_NORMAL_ROTATION ' + json.dumps({'blender': bpy.app.version_string, 'order': order, 'results': results}), flush=True)
