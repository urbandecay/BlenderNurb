# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""App/visual check for the explicit C++ native-NURBS fixture.

Run only in a separate --factory-startup Blender process. This replaces that
process's startup objects; it never attaches to an existing Blender instance.
The fixture's authoritative payloads are created by the kernel integration test,
not synthesized from Python display meshes. No public component API is implied.
"""

import json
import math
import os
from pathlib import Path

import bpy
from mathutils import Euler


fixture = Path(os.environ["BLENDER_NURBS_PREVIEW_FIXTURE"])
output = Path(os.environ.get("BLENDER_NURBS_PREVIEW_IMAGE", "/tmp/blender-nurbs-native-preview.png"))
names = ("Native preview sheet", "Native preview hole", "Native preview circle")

for obj in tuple(bpy.data.objects):
    bpy.data.objects.remove(obj, do_unlink=True)

with bpy.data.libraries.load(str(fixture), link=False) as (source, target):
    assert all(name in source.curves for name in names), tuple(source.curves)
    target.curves = list(names)

colors = ((0.2, 0.5, 0.85, 1.0), (0.95, 0.5, 0.15, 1.0), (0.25, 0.7, 0.4, 1.0))
objects = []
for index, curve in enumerate(target.curves):
    assert curve is not None
    assert len(curve.splines) == 0, "A legacy control net must not supply this preview"
    obj = bpy.data.objects.new(names[index], curve)
    bpy.context.scene.collection.objects.link(obj)
    assert obj.type == 'SURFACE', obj.type
    obj.location.x = (index - 1) * 3.0
    obj.color = colors[index]
    objects.append(obj)

bpy.context.view_layer.update()
depsgraph = bpy.context.evaluated_depsgraph_get()
results = []
for obj in objects:
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        assert mesh is not None, obj.name
        assert len(mesh.polygons) > 1, (obj.name, len(mesh.vertices), len(mesh.polygons))
        assert mesh.has_custom_normals, obj.name
        entry = {
            "object": obj.name,
            "source_legacy_splines": len(obj.data.splines),
            "display_vertices": len(mesh.vertices),
            "display_edges": len(mesh.edges),
            "display_triangles": len(mesh.polygons),
        }
        if obj.name == names[1]:
            area = sum(polygon.area for polygon in mesh.polygons)
            assert abs(area - 3.0) < 1.0e-5, area
            for polygon in mesh.polygons:
                center = polygon.center
                assert not (-0.5 < center.x < 0.5 and -0.5 < center.y < 0.5), center
            entry["sampled_display_area"] = area
        if obj.name == names[2]:
            radius_error = max(abs(vertex.co.x ** 2 + vertex.co.y ** 2 - 1.0)
                               for vertex in mesh.vertices)
            assert radius_error < 1.0e-5, radius_error
            entry["float_display_squared_radius_error"] = radius_error
            minimum_radial_dot = 1.0
            for loop in mesh.loops:
                position = mesh.vertices[loop.vertex_index].co
                normal = mesh.corner_normals[loop.index].vector
                dot = (position.x * normal.x + position.y * normal.y) / math.hypot(position.x, position.y)
                minimum_radial_dot = min(minimum_radial_dot, dot)
                assert abs(normal.z) < 1.0e-3, normal
            assert minimum_radial_dot > 0.9999, minimum_radial_dot
            entry["minimum_radial_normal_dot"] = minimum_radial_dot
        results.append(entry)
    finally:
        evaluated.to_mesh_clear()

output.with_suffix(".json").write_text(json.dumps(results, indent=2) + "\n")
print("NATIVE_NURBS_APP_PREVIEW", json.dumps(results))

if not bpy.app.background:
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.render.resolution_x = 1200
    scene.render.resolution_y = 700
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.filepath = str(output)
    area = next(area for area in bpy.context.window.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'SOLID'
    space.shading.color_type = 'OBJECT'
    space.overlay.show_overlays = False
    space.region_3d.view_perspective = 'ORTHO'
    space.region_3d.view_location = (0.0, 0.0, 0.7)
    space.region_3d.view_distance = 11.0
    space.region_3d.view_rotation = Euler((math.radians(48), 0, math.radians(12)), 'XYZ').to_quaternion()

    def capture():
        try:
            with bpy.context.temp_override(window=bpy.context.window, area=area, region=region):
                bpy.ops.render.opengl(write_still=True, view_context=True)
            assert output.exists(), output
            print("NATIVE_NURBS_VIEWPORT_IMAGE", output)
        except Exception:
            import traceback
            traceback.print_exc()
            os._exit(1)
        bpy.ops.wm.quit_blender()
        return None

    bpy.app.timers.register(capture, first_interval=3.0)
