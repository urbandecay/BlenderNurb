"""Compare viewport rotation of a 32-step mesh bevel and an analytic solid fillet.

Run in a visible Blender session with event simulation enabled and set
SOLID_BENCH_KIND=mesh or SOLID_BENCH_KIND=solid plus SOLID_BENCH_SEGMENTS=1 or 32.
Set SOLID_BENCH_ROTATION=FREE to exercise unconstrained view-axis rotation,
or X/Y/Z for an axis constraint (default Z). Use BLENDER_SOLID_PROFILE=1 to
check that rigid solid drags emit SOLID_AFFINE_DRAG, with no
SOLID_DISPLAY_PROFILE rebuilds before confirmation.
The script replaces the startup scene with a 2-unit cube, measures modal viewport
draw intervals and exits Blender. Both tools use width 0.2 and the same segment
count. Note that one mesh bevel segment makes a chamfer, while the solid's one
segment remains a curved analytic fillet.
"""

import bpy, bmesh, json, math, os, statistics, time

kind = os.environ['SOLID_BENCH_KIND']
segments = int(os.environ.get('SOLID_BENCH_SEGMENTS', '1'))
rotation_mode = os.environ.get('SOLID_BENCH_ROTATION', 'Z').upper()
normal_method = os.environ.get('SOLID_BENCH_NORMALS', 'ANALYTIC').upper()
assert normal_method in {'ANALYTIC', 'BLENDER'}
assert rotation_mode in {'FREE', 'X', 'Y', 'Z'}, rotation_mode
area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
window = bpy.context.window
region = next(r for r in area.regions if r.type == 'WINDOW')

if bpy.context.object and bpy.context.object.mode != 'OBJECT':
    bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)

if kind == 'mesh':
    bpy.ops.mesh.primitive_cube_add(size=2)
    obj = bpy.context.object
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    result = bpy.ops.mesh.bevel(offset=.2, segments=segments, profile=.5)
    assert result == {'FINISHED'}, result
else:
    bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
    obj = bpy.context.object
    assert bpy.ops.solid.extrude(distance=2) == {'FINISHED'}
    bpy.ops.mesh.select_mode(type='EDGE')
    bpy.ops.mesh.select_all(action='SELECT')
    assert bpy.ops.solid.bevel(offset=.2, segments=segments) == {'FINISHED'}
    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.shade_smooth(solid_normal_method=normal_method)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_mode(type='VERT')
    bpy.ops.mesh.select_all(action='SELECT')

bpy.context.view_layer.update()
evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
display = evaluated.to_mesh()
counts = (len(display.vertices), len(display.polygons))
evaluated.to_mesh_clear()
# Frame the same 2-unit shape consistently in the viewport.
with bpy.context.temp_override(window=window, area=area, region=region):
    bpy.ops.view3d.view_selected(use_all_regions=False)

frames = []
state = {'sent': 0, 'waiting': False, 'started': False, 'last_draw': None}
handle = None
iterations = 32
warmup = 5

def send():
    i = state['sent']
    state['sent'] += 1
    state['waiting'] = True
    window.event_simulate(type='MOUSEMOVE', value='NOTHING',
                          x=int(area.width * .55 + 100 * math.cos(i * .13)),
                          y=int(area.height * .55 + 100 * math.sin(i * .13)))

def finish():
    bpy.types.SpaceView3D.draw_handler_remove(handle, 'WINDOW')
    bm = bmesh.from_edit_mesh(obj.data)
    before = [tuple(v.co) for v in bm.verts]
    window.event_simulate(type='RET', value='PRESS')
    def done():
        bm2 = bmesh.from_edit_mesh(obj.data)
        after = [tuple(v.co) for v in bm2.verts]
        if kind == 'solid':
            assert (bm2.loops.layers.float_vector.get('custom_normal') is not None) == (normal_method == 'ANALYTIC')
        print('VIEWPORT_BENCH_RESULT ' + json.dumps({
            'kind': kind,
            'normal_method': normal_method if kind == 'solid' else 'MESH',
            'segments': segments,
            'rotation': rotation_mode,
            'display_vertices': counts[0],
            'display_faces': counts[1],
            'frame_ms': frames,
            'median_ms_after_warmup': statistics.median(frames[warmup:]),
            'fps_after_warmup': 1000 / statistics.median(frames[warmup:]),
            'confirm_local_coords_identical': before == after,
        }), flush=True)
        assert before == after, 'Geometry changed on confirmation'
        bpy.ops.wm.quit_blender()
    bpy.app.timers.register(done, first_interval=.3)

def draw():
    if not state['started'] or not state['waiting']:
        return
    state['waiting'] = False
    now = time.perf_counter()
    frames.append((now - (state['last_draw'] or state['send_time'])) * 1000)
    state['last_draw'] = now
    if len(frames) >= iterations:
        bpy.app.timers.register(finish, first_interval=.001)
    else:
        state['send_time'] = now
        bpy.app.timers.register(send, first_interval=.001)

def start():
    global handle
    with bpy.context.temp_override(window=window, area=area, region=region):
        if rotation_mode == 'FREE':
            bpy.ops.transform.rotate('INVOKE_DEFAULT')
        else:
            bpy.ops.transform.rotate('INVOKE_DEFAULT', orient_axis=rotation_mode,
                                     constraint_axis=tuple(rotation_mode == axis for axis in 'XYZ'))
    handle = bpy.types.SpaceView3D.draw_handler_add(draw, (), 'WINDOW', 'POST_PIXEL')
    state['started'] = True
    state['send_time'] = time.perf_counter()
    send()

bpy.app.timers.register(start, first_interval=1)
