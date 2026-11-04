# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare repeated Edit Mode rotations of a beveled mesh cube and analytic solid cube.

Run with:
  blender --background --factory-startup --python tests/python/solid_rotation_benchmark.py

The benchmark excludes object/sketch construction and bevel creation. Each sample applies one
small source-geometry rotation, updates the dependency graph, and requests the evaluated display
mesh. It reports CPU update throughput, not viewport FPS: background mode does not draw or
measure GPU presentation. CPU percentage is process CPU time divided by wall time and may
exceed 100% when Blender uses multiple cores.
"""

import json
import statistics
import time

import bmesh
import bpy


ITERATIONS = 40
WARMUP = 5
ROTATION_RADIANS = 0.01
BEVEL_WIDTH = 0.2


def clear_scene():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)


def create_mesh_cube():
    bpy.ops.mesh.primitive_cube_add(size=2)
    obj = bpy.context.object
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    # Match the solid viewport's 32-step curve tessellation closely enough to compare both
    # the source edit path and the cost of producing a similarly smooth displayed fillet.
    assert bpy.ops.mesh.bevel(offset=BEVEL_WIDTH, segments=32) == {'FINISHED'}
    return obj


def create_solid_cube():
    bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
    obj = bpy.context.object
    assert bpy.ops.solid.extrude(distance=2) == {'FINISHED'}
    bpy.ops.mesh.select_mode(type='EDGE')
    bpy.ops.mesh.select_all(action='SELECT')
    assert bpy.ops.solid.bevel(offset=BEVEL_WIDTH, segments=1) == {'FINISHED'}
    return obj


def evaluate_display(obj):
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    display = evaluated.to_mesh()
    try:
        assert display and len(display.vertices) and len(display.polygons)
        return len(display.vertices), len(display.polygons)
    finally:
        evaluated.to_mesh_clear()


def benchmark(kind, create):
    clear_scene()
    obj = create()
    if kind == 'mesh':
        bpy.ops.mesh.select_mode(type='VERT')
        bm = bmesh.from_edit_mesh(obj.data)
        for vert in bm.verts:
            vert.select_set(True)
        bmesh.update_edit_mesh(obj.data)
    else:
        bpy.ops.mesh.select_mode(type='VERT')
        bpy.ops.mesh.select_all(action='SELECT')
    evaluate_display(obj)

    samples = []
    display_counts = None
    for iteration in range(ITERATIONS):
        wall_start = time.perf_counter()
        cpu_start = time.process_time()
        result = bpy.ops.transform.rotate(
            'EXEC_DEFAULT',
            value=ROTATION_RADIANS,
            orient_axis='Z',
            orient_type='GLOBAL',
            center_override=(0, 0, 0),
        )
        assert result == {'FINISHED'}, (kind, result)
        display_counts = evaluate_display(obj)
        wall_ms = (time.perf_counter() - wall_start) * 1000
        cpu_ms = (time.process_time() - cpu_start) * 1000
        if iteration >= WARMUP:
            samples.append((wall_ms, cpu_ms, 100 * cpu_ms / wall_ms if wall_ms else 0))

    wall = statistics.median(row[0] for row in samples)
    cpu = statistics.median(row[1] for row in samples)
    print('SOLID_ROTATION_BENCHMARK ' + json.dumps({
        'kind': kind,
        'samples': len(samples),
        'rotation_radians_per_sample': ROTATION_RADIANS,
        'bevel_width': BEVEL_WIDTH,
        'mesh_bevel_segments': 32 if kind == 'mesh' else None,
        'solid_bevel_segments': 1 if kind == 'solid' else None,
        'display_vertices': display_counts[0],
        'display_faces': display_counts[1],
        'median_wall_ms_per_rotation_update_and_evaluation': round(wall, 3),
        'median_process_cpu_ms_per_sample': round(cpu, 3),
        'median_process_cpu_percent_of_one_core': round(
            statistics.median(row[2] for row in samples), 1),
        'cpu_update_rate_hz_not_viewport_fps': round(1000 / wall, 1) if wall else None,
    }), flush=True)


benchmark('mesh', create_mesh_cube)
benchmark('solid', create_solid_cube)
