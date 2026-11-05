"""Measure live top-face dragging on a dense solid or its identical plain mesh display.

Run a visible Blender with --factory-startup --enable-event-simulate --python this_file.
SOLID_TRANSFORM_ACTION: FACE_MOVE, MESH_FACE_MOVE, ALL_MOVE, ALL_ROTATE.
SOLID_TRANSFORM_AXIS: X/Y/Z/FREE. SOLID_TRANSFORM_CONFIRM=1 checks confirmation,
preview continuity, analytic normals, undo and redo; otherwise checks cancellation.
Other options: SOLID_TRANSFORM_SEGMENTS, SOLID_TRANSFORM_NORMALS (ANALYTIC/BLENDER),
SOLID_TRANSFORM_UPDATES, SOLID_TRANSFORM_WARMUP. The process replaces its scene and quits.
"""
import bpy, bmesh, json, math, os, statistics, time
from mathutils import Vector
from bpy_extras import view3d_utils

action = os.environ.get('SOLID_TRANSFORM_ACTION', 'FACE_MOVE').upper()
segments = int(os.environ.get('SOLID_TRANSFORM_SEGMENTS', '32'))
axis = os.environ.get('SOLID_TRANSFORM_AXIS', 'Z').upper()
confirm = os.environ.get('SOLID_TRANSFORM_CONFIRM', '0') == '1'
normal_method = os.environ.get('SOLID_TRANSFORM_NORMALS', 'ANALYTIC').upper()
iterations = int(os.environ.get('SOLID_TRANSFORM_UPDATES', '30'))
warmup = int(os.environ.get('SOLID_TRANSFORM_WARMUP', '5'))
assert action in {'FACE_MOVE', 'MESH_FACE_MOVE', 'ALL_MOVE', 'ALL_ROTATE'}
area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
window = bpy.context.window
region = next(r for r in area.regions if r.type == 'WINDOW')
if bpy.context.object and bpy.context.object.mode != 'OBJECT':
    bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
# Same style of workload as the screenshot: dense filleted base plus a tall prism.
bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
obj = bpy.context.object
assert bpy.ops.solid.extrude(distance=2) == {'FINISHED'}
bpy.ops.mesh.select_mode(type='EDGE')
bpy.ops.mesh.select_all(action='SELECT')
assert bpy.ops.solid.bevel(offset=.2, segments=segments) == {'FINISHED'}
bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.object.shade_smooth(solid_normal_method=normal_method)
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_mode(type='FACE')
bpy.ops.mesh.select_all(action='DESELECT')
bm = bmesh.from_edit_mesh(obj.data)
bm.normal_update()
base_cap = max((f for f in bm.faces if f.normal.z > .999), key=lambda f: f.calc_center_median().z)
base_cap.select_set(True)
bm.faces.active = base_cap
bmesh.update_edit_mesh(obj.data)
assert bpy.ops.solid.extrude(distance=2) == {'FINISHED'}
bm = bmesh.from_edit_mesh(obj.data)
bm.normal_update()
top = max((f for f in bm.faces if f.normal.z > .999), key=lambda f: f.calc_center_median().z)
if action in {'FACE_MOVE', 'MESH_FACE_MOVE'}:
    bpy.ops.mesh.select_mode(type='FACE')
    bpy.ops.mesh.select_all(action='DESELECT')
    top.select_set(True)
    bm.faces.active = top
else:
    bpy.ops.mesh.select_mode(type='VERT')
    bpy.ops.mesh.select_all(action='SELECT')
bmesh.update_edit_mesh(obj.data)

if action == 'MESH_FACE_MOVE':
    # A plain mesh copy of the exact same tessellated display geometry isolates
    # Blender's ordinary partial-face transform cost from the analytic solid path.
    source_bm = bmesh.from_edit_mesh(obj.data)
    plain_mesh = bpy.data.meshes.new('SolidDisplayMeshReference')
    source_bm.to_mesh(plain_mesh)
    old_obj = obj
    bpy.ops.object.mode_set(mode='OBJECT')
    plain_obj = bpy.data.objects.new('SolidDisplayMeshReference', plain_mesh)
    bpy.context.collection.objects.link(plain_obj)
    old_obj.select_set(False)
    old_obj.hide_viewport = True
    plain_obj.select_set(True)
    bpy.context.view_layer.objects.active = plain_obj
    obj = plain_obj
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_mode(type='FACE')
    bpy.ops.mesh.select_all(action='DESELECT')
    mesh_bm = bmesh.from_edit_mesh(obj.data)
    mesh_bm.normal_update()
    mesh_top = max((f for f in mesh_bm.faces if f.normal.z > .999),
                   key=lambda f: f.calc_center_median().z)
    mesh_top.select_set(True)
    mesh_bm.faces.active = mesh_top
    bmesh.update_edit_mesh(obj.data)
    top = mesh_top

# Count displayed edit geometry for the report.
bm = bmesh.from_edit_mesh(obj.data)
counts = (len(bm.verts), len(bm.edges), len(bm.faces))

def snapshot():
    bmesh.from_edit_mesh(obj.data).verts.index_update()
    bm = bmesh.from_edit_mesh(obj.data)
    return (tuple(tuple(v.co) for v in bm.verts), tuple(v.select for v in bm.verts),
            tuple(e.select for e in bm.edges), tuple(f.select for f in bm.faces))
original = snapshot()
bpy.ops.ed.undo_push(message='Face transform benchmark source')
with bpy.context.temp_override(window=window, area=area, region=region):
    bpy.ops.view3d.view_selected(use_all_regions=False)
bpy.context.view_layer.update()
rv3d = area.spaces.active.region_3d
center_local = top.calc_center_median()
center_world = obj.matrix_world @ Vector(center_local)
center_2d = view3d_utils.location_3d_to_region_2d(region, rv3d, center_world)
assert center_2d is not None
z2 = view3d_utils.location_3d_to_region_2d(region, rv3d, center_world + Vector(tuple(axis == a for a in 'XYZ') if axis != 'FREE' else (0,0,1)))
assert z2 is not None
screen_dir = (z2 - center_2d).normalized()
# Ensure appreciable but safe cumulative motion (< about 0.3 scene units).
step_pixels = max(1.0, min(3.0, (z2-center_2d).length * .008))
start_xy = (round(region.x + center_2d.x), round(region.y + center_2d.y))
state = {'sent': 0, 'waiting': False, 'started': False, 'last_draw': None,
         'frames': [], 'handle': None, 'done': False}

def send(i):
    state['waiting'] = True
    if action == 'ALL_ROTATE':
        x = round(region.x + center_2d.x + 95 * math.cos(i * .13))
        y = round(region.y + center_2d.y + 95 * math.sin(i * .13))
    else:
        x = round(start_xy[0] + screen_dir.x * step_pixels * (i + 1))
        y = round(start_xy[1] + screen_dir.y * step_pixels * (i + 1))
    window.event_simulate(type='MOUSEMOVE', value='NOTHING', x=x, y=y)


def same_coordinates(a, b):
    return len(a) == len(b) and all(abs(x-y) < 2e-6
        for va, vb in zip(sorted(a), sorted(b)) for x,y in zip(va,vb))

def normals():
    bm = bmesh.from_edit_mesh(obj.data)
    layer = bm.loops.layers.float_vector.get('custom_normal')
    if layer is None:
        return {}
    result = {}
    for f in bm.faces:
        key = tuple(sorted(tuple(round(c, 5) for c in v.co) for v in f.verts))
        values = sorted((tuple(l.vert.co), tuple(l[layer])) for l in f.loops)
        result[key] = tuple(component for _, n in values for component in n)
    return result

def finish():
    if state['done']:
        return None
    state['done'] = True
    bpy.types.SpaceView3D.draw_handler_remove(state['handle'], 'WINDOW')
    preview = snapshot()
    assert preview[0] != original[0], 'No geometry moved during the benchmark'
    preview_normals = normals()
    window.event_simulate(type='RET' if confirm else 'ESC', value='PRESS')
    def report():
        global obj
        restored = snapshot() == original
        checks = {}
        if confirm:
            checks['confirm_matches_preview'] = same_coordinates(snapshot()[0], preview[0])
            after_normals = normals()
            checks['confirm_matches_normals'] = (preview_normals.keys() == after_normals.keys() and
                all(max((abs(a-b) for a,b in zip(preview_normals[k], after_normals[k])), default=0) < 1e-4
                    for k in preview_normals))
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.ed.undo()
            obj = bpy.context.edit_object
            checks['undo_restores_source'] = snapshot() == original
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.ed.redo()
            obj = bpy.context.edit_object
            checks['redo_matches_preview'] = same_coordinates(snapshot()[0], preview[0])
            if action == 'FACE_MOVE':
                before_z = max(v[2] for v in snapshot()[0])
                checks['subsequent_extrude_works'] = bpy.ops.solid.extrude(distance=.125) == {'FINISHED'}
                checks['extrude_uses_committed_source'] = abs(
                    max(v[2] for v in snapshot()[0]) - before_z - .125) < 2e-6
        else:
            checks['cancel_restored_geometry_and_selection'] = restored
        samples = state['frames'][warmup:]
        median_ms = statistics.median(samples) if samples else 0
        sorted_samples = sorted(samples)
        p90_ms = sorted_samples[min(len(sorted_samples)-1, int(.9*(len(sorted_samples)-1)))] if sorted_samples else 0
        result = {'action': action, 'segments': segments, 'normal_method': normal_method, 'edit_bmesh_counts': counts,
                  'updates': len(state['frames']), 'median_event_to_draw_ms': round(median_ms, 3),
                  'p90_event_to_draw_ms': round(p90_ms, 3),
                  'approx_fps': round(1000/median_ms, 1) if median_ms else 0,
                  'axis': axis, 'confirm': confirm, **checks}
        print('SOLID_FACE_TRANSFORM_BENCHMARK ' + json.dumps(result), flush=True)
        if not all(checks.values()):
            print('SOLID_FACE_TRANSFORM_CHECK_FAILED', flush=True)
            os._exit(1)
        bpy.ops.wm.quit_blender()
        return None
    bpy.app.timers.register(report, first_interval=.4)
    return None

def draw():
    if not state['started'] or not state['waiting'] or bpy.context.region != region:
        return
    state['waiting'] = False
    now = time.perf_counter()
    prior = state['last_draw'] or state['send_time']
    state['frames'].append((now-prior)*1000)
    state['last_draw'] = now
    if len(state['frames']) >= iterations:
        bpy.app.timers.register(finish, first_interval=.02)
    else:
        state['send_time'] = now
        send(state['sent'])
        state['sent'] += 1

def start_modal():
    with bpy.context.temp_override(window=window, area=area, region=region):
        if action == 'ALL_ROTATE':
            result = bpy.ops.transform.rotate('INVOKE_DEFAULT', orient_axis='Z', constraint_axis=(False,False,True))
        else:
            result = bpy.ops.transform.translate('INVOKE_DEFAULT', constraint_axis=tuple(axis == a for a in 'XYZ'))
    assert result == {'RUNNING_MODAL'}, result
    state['handle'] = bpy.types.SpaceView3D.draw_handler_add(draw, (), 'WINDOW', 'POST_PIXEL')
    state['started'] = True
    state['send_time'] = time.perf_counter()
    send(0)
    state['sent'] = 1
    return None

def set_pointer():
    window.event_simulate(type='MOUSEMOVE', value='NOTHING', x=start_xy[0], y=start_xy[1])
    bpy.app.timers.register(start_modal, first_interval=.15)
    return None

bpy.app.timers.register(set_pointer, first_interval=.8)

def watchdog():
    print('SOLID_FACE_TRANSFORM_TIMEOUT', flush=True)
    os._exit(1)

bpy.app.timers.register(watchdog, first_interval=45)
