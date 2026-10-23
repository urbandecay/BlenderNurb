# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Print BMesh region behavior for later NURBS parity fixtures; no NURBS acceptance claims."""

import json

import bmesh
import bpy
from mathutils import Vector


def extrude(bm, selected):
    faces_before = set(bm.faces)
    edges = {edge for face in selected for edge in face.edges}
    boundary = [edge for edge in edges if sum(face in selected for face in edge.link_faces) < 2]
    output = bmesh.ops.extrude_face_region(bm, geom=[*edges, *selected])
    cap = {element for element in output["geom"] if isinstance(element, bmesh.types.BMFace)}
    bmesh.ops.translate(
        bm,
        verts=[element for element in output["geom"] if isinstance(element, bmesh.types.BMVert)],
        vec=Vector((0, 0, 2)),
    )
    bm.normal_update()
    new_faces = set(bm.faces) - faces_before
    return cap, {
        "vertices": len(bm.verts),
        "edges": len(bm.edges),
        "faces": len(bm.faces),
        "cap_faces": len(cap),
        "walls": len(new_faces - cap),
        "selected_boundary_edges_before": len(boundary),
        "original_selected_faces_retained": sum(face.is_valid for face in selected),
    }


def ring():
    bm = bmesh.new()
    try:
        vertices = [[bm.verts.new((x, y, 0)) for x in range(4)] for y in range(4)]
        for y in range(3):
            for x in range(3):
                if (x, y) != (1, 1):
                    bm.faces.new((vertices[y][x], vertices[y][x + 1],
                                  vertices[y + 1][x + 1], vertices[y + 1][x]))
        cap, first = extrude(bm, set(bm.faces))
        _, second = extrude(bm, cap)
        print("MESH_REGION_REFERENCE", json.dumps({"fixture": "ring", "first": first, "second": second}))
    finally:
        bm.free()


def nonmanifold(selected_count):
    bm = bmesh.new()
    try:
        a = bm.verts.new((0, 0, 0))
        b = bm.verts.new((1, 0, 0))
        faces = []
        for y, z in ((1, 0), (-1, 0), (0, 1)):
            c = bm.verts.new((1, y, z))
            d = bm.verts.new((0, y, z))
            faces.append(bm.faces.new((a, b, c, d)))
        _, result = extrude(bm, set(faces[:selected_count]))
        print("MESH_REGION_REFERENCE", json.dumps({
            "fixture": "three_faces_share_one_edge", "selected_faces": selected_count, **result,
        }))
    finally:
        bm.free()


print("MESH_REGION_REFERENCE_BINARY", bpy.app.version_string, bpy.app.build_hash.decode())
ring()
for count in (1, 2, 3):
    nonmanifold(count)
