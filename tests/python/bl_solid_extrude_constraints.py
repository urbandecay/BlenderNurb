# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run in a separate GUI process with --factory-startup --enable-event-simulate.

Drag along and across the projected normal of a rotated solid face. Both extrusion
tools must preserve the face's tangential coordinates for every mouse position.
"""
import os
import bpy
import bmesh
from mathutils import Vector
from bpy_extras import view3d_utils

CASES = (
    ('REGION', bpy.ops.view3d.edit_mesh_extrude_move_normal),
    ('ALONG_NORMALS', bpy.ops.view3d.edit_mesh_extrude_move_shrink_fatten),
    ('MANIFOLD', bpy.ops.view3d.edit_mesh_extrude_manifold_normal),
    ('ALONG_NORMALS_CURVED', bpy.ops.view3d.edit_mesh_extrude_move_shrink_fatten),
    ('MANIFOLD_CURVED', bpy.ops.view3d.edit_mesh_extrude_manifold_normal),
    ('ALONG_NORMALS_ELLIPTICAL', bpy.ops.view3d.edit_mesh_extrude_move_shrink_fatten),
    ('INDIVIDUAL_CURVED', bpy.ops.view3d.edit_mesh_extrude_individual_move),
)
if os.environ.get('SOLID_CONSTRAINT_CASE'):
    CASES = tuple(c for c in CASES if c[0] == os.environ['SOLID_CONSTRAINT_CASE'])
window = bpy.context.window
area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
region = next(r for r in area.regions if r.type == 'WINDOW')
state = {'case': 0}


def fail(message):
    print('SOLID_NORMAL_CONSTRAINT_FAIL', CASES[state['case']][0], message, flush=True)
    os._exit(1)


def setup():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
    bpy.ops.solid.extrude(distance=2)
    obj = bpy.context.object
    case_name = CASES[state['case']][0]
    curved = 'CURVED' in case_name or 'ELLIPTICAL' in case_name
    if 'ELLIPTICAL' in case_name:
        bpy.ops.object.mode_set(mode='OBJECT')
        obj.scale = (2, 1, 1)
        bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
        bpy.ops.object.mode_set(mode='EDIT')
    state['curved'] = curved
    if curved:
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.solid.bevel(offset=.2, segments=3)
    obj.rotation_euler = (.3, .5, .2)
    bpy.ops.mesh.select_mode(type='FACE')
    bm = bmesh.from_edit_mesh(obj.data)
    bm.normal_update()
    bm.faces.ensure_lookup_table()
    bm.faces.index_update()
    top = (next(f for f in bm.faces if min(f.normal) > .12 and
                f.calc_center_median().x > (1.5 if 'ELLIPTICAL' in case_name else .75) and
                f.calc_center_median().y > .75 and
                f.calc_center_median().z > 1.75) if curved else
           next(f for f in bm.faces if f.normal.z > .999))
    for f in bm.faces:
        f.select_set(False)
    top.select_set(True)
    bm.faces.active = top
    candidate_indices = []
    if case_name == 'INDIVIDUAL_CURVED':
        def corner_face(face):
            center = face.calc_center_median()
            return (min(face.normal) > .12 and center.x > .75 and center.y > .75 and
                    center.z > 1.75)
        candidate_indices = [face.index for face in bm.faces if corner_face(face)]
        candidate_indices.sort(
            key=lambda index: (bm.faces[index].calc_center_median() -
                               top.calc_center_median()).length)
    bmesh.update_edit_mesh(obj.data)
    state['obj'] = obj
    state['base_volume'] = bm.calc_volume(signed=False)
    bm.faces.index_update()
    active_source_index = top.index
    bpy.ops.object.mode_set(mode='OBJECT')
    mesh = obj.data
    ids = mesh.attributes['.solid_face_id'].data
    selected_ids = {tuple(ids[active_source_index].value)}
    if case_name == 'INDIVIDUAL_CURVED':
        for index in candidate_indices:
            selected_ids.add(tuple(ids[index].value))
            if len(selected_ids) >= 3:
                break
    if case_name == 'INDIVIDUAL_CURVED' and len(selected_ids) < 3:
        fail('The curved-face fixture did not select three analytic faces')
    polygons = [f for f in mesh.polygons if tuple(ids[f.index].value) in selected_ids]
    indices = {i for f in polygons for i in f.vertices}
    state['original'] = sorted(tuple(mesh.vertices[i].co) for i in indices)
    state['individual'] = case_name == 'INDIVIDUAL_CURVED'
    state['selected_poly_count'] = len(polygons)
    local_normal = Vector()
    for f in polygons:
        n = sum((mesh.corner_normals[i].vector for i in f.loop_indices), Vector()).normalized()
        local_normal += n * f.area
    local_normal.normalize()
    state['normal'] = local_normal
    state['center'] = sum((mesh.vertices[i].co for i in indices), Vector()) / len(indices)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_mode(type='FACE')
    bm = bmesh.from_edit_mesh(obj.data)
    bm.faces.ensure_lookup_table()
    for element in (*bm.faces, *bm.edges, *bm.verts):
        element.select_set(False)
    for polygon in polygons:
        bm.faces[polygon.index].select_set(True)
    bm.faces.active = bm.faces[active_source_index]
    bm.select_flush_mode()
    bmesh.update_edit_mesh(obj.data)
    area.spaces.active.region_3d.view_perspective = 'ORTHO'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.view3d.view_selected(use_all_regions=False)
    bpy.context.view_layer.update()
    center = obj.matrix_world @ state['center']
    normal = obj.matrix_world.to_3x3() @ state['normal']
    rv3d = area.spaces.active.region_3d
    p = view3d_utils.location_3d_to_region_2d(region, rv3d, center)
    q = view3d_utils.location_3d_to_region_2d(region, rv3d, center + normal)
    if p is None or q is None or (q - p).length < 4:
        fail('Cannot project the tilted face normal')
    direction = (q - p).normalized()
    start = Vector((region.x + p.x, region.y + p.y))
    end = start + direction * 45
    state['positions'] = [start, end, end + Vector((-direction.y, direction.x)) * 90]
    mouse(start)
    bpy.app.timers.register(invoke, first_interval=.25)


def mouse(position):
    window.event_simulate(type='MOUSEMOVE', value='NOTHING',
                          x=round(position.x), y=round(position.y))


def invoke():
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = CASES[state['case']][1]('INVOKE_REGION_WIN')
    if result != {'FINISHED'}:
        fail(('Wrapper did not hand off to modal extrusion', result))
    mouse(state['positions'][1])
    bpy.app.timers.register(sideways, first_interval=.3)


def check_cap():
    bm = bmesh.from_edit_mesh(state['obj'].data)
    if CASES[state['case']][0] == 'MANIFOLD' and len(bm.faces) != 6:
        fail(('Manifold preview added a section to the flat prism', len(bm.faces)))
    selected = [f for f in bm.faces if f.select]
    if state.get('individual'):
        if len(selected) != state['selected_poly_count']:
            fail(('Individual extrusion did not select all new caps', len(selected),
                  state['selected_poly_count']))
        cap_center = sum((v.co for f in selected for v in f.verts), Vector()) / sum(
            len(f.verts) for f in selected)
        delta = cap_center - Vector(state['center'])
        if delta.dot(state['normal']) <= .00001:
            fail(('Individual corner faces did not extrude outward', tuple(delta)))
        if bpy.context.tool_settings.mesh_select_mode[:] != (False, False, True):
            fail('Individual extrusion changed face selection mode')
        return
    cap_vertices = sorted(tuple(v.co) for v in {v for f in selected for v in f.verts})
    if len(cap_vertices) != len(state['original']):
        fail(('Extrusion changed the cap sampling', len(cap_vertices), len(state['original'])))
    if state['curved'] and CASES[state['case']][0] == 'ALONG_NORMALS_CURVED':
        center = Vector((.8, .8, 1.8))
        radii = [(Vector(p) - center).length for p in cap_vertices]
        if min(radii) <= .20001 or max(radii) - min(radii) > 1e-5:
            fail(('Cap left its original normal rays', min(radii), max(radii)))
    else:
        delta = (sum((Vector(p) for p in cap_vertices), Vector()) -
                 sum((Vector(p) for p in state['original']), Vector())) / len(cap_vertices)
        if delta.cross(state['normal']).length > 2e-5:
            fail(('Mouse moved the cap sideways off its normal', tuple(delta), tuple(state['normal'])))
        if delta.dot(state['normal']) <= .00001:
            fail('No outward extrusion was produced')
        if not CASES[state['case']][0].endswith('_ELLIPTICAL'):
            for p in state['original']:
                expected = Vector(p) + delta
                deviation = min((Vector(q) - expected).length for q in cap_vertices)
                if deviation > 2e-5:
                    fail(('Rigid normal extrusion changed the cap shape', deviation,
                          len(cap_vertices), tuple(delta), tuple(state['normal'])))
    if bpy.context.tool_settings.mesh_select_mode[:] != (False, False, True):
        fail('Extrusion changed face selection mode')


def sideways():
    check_cap()
    mouse(state['positions'][2])
    bpy.app.timers.register(confirm, first_interval=.3)


def confirm():
    check_cap()
    window.event_simulate(type='RET', value='PRESS')
    bpy.app.timers.register(verify, first_interval=.3)


def verify():
    check_cap()
    if CASES[state['case']][0] == 'MANIFOLD_CURVED':
        face_count = state['obj'].data.solid_faces_count
        for amount in (.05, -.025, -.1):
            result = bpy.ops.solid.extrude(method='MANIFOLD', distance=amount)
            if result != {'FINISHED'} or state['obj'].data.solid_faces_count != face_count:
                fail(('Extending/retracting a curved cap added a section', amount, result))
        bm = bmesh.from_edit_mesh(state['obj'].data)
        if bm.calc_volume(signed=False) >= state['base_volume']:
            fail('Pushing the curved cap below its attachment did not create a recess')
    print('SOLID_NORMAL_CONSTRAINT_PASS', CASES[state['case']][0], flush=True)
    state['case'] += 1
    if state['case'] == len(CASES):
        bpy.ops.wm.quit_blender()
    else:
        bpy.app.timers.register(setup, first_interval=.3)


bpy.app.timers.register(setup, first_interval=1)
bpy.app.timers.register(lambda: fail('Timed out'), first_interval=30)
