# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare mesh and analytic-solid bevel creation.

Background mode times the synchronous bevel operator on fresh, matching 2-unit cubes.
Windowed mode invokes the real modal bevel tools and measures actual viewport draw cadence while
synthetic mouse input continuously changes the bevel width.

Background:
  blender --background --factory-startup --python tests/python/solid_bevel_creation_benchmark.py

Windowed, once per method/case:
  SOLID_BEVEL_KIND=mesh SOLID_BEVEL_CASE=fillet blender --factory-startup \
    --enable-event-simulate --python tests/python/solid_bevel_creation_benchmark.py
  SOLID_BEVEL_KIND=solid SOLID_BEVEL_CASE=fillet blender --factory-startup \
    --enable-event-simulate --python tests/python/solid_bevel_creation_benchmark.py
Same numeric subdivision count, background only:
  SOLID_BEVEL_CASE=fillet_same_count SOLID_BEVEL_TRIALS=2 blender --background \
    --python tests/python/solid_bevel_creation_benchmark.py

The chamfer pair uses one mesh bevel segment and zero solid fillet subdivisions. The rounded
pair uses 32 mesh bevel segments and one exact solid fillet subdivision; the solid viewport
still samples the analytic surfaces at its normal display resolution.
"""

import json
import os
import statistics
import time

import bmesh
import bpy
from mathutils import Vector


WIDTH = 0.2
TRIALS = int(os.environ.get("SOLID_BEVEL_TRIALS", "6"))
WARMUP = 1
MODAL_UPDATES = int(os.environ.get("SOLID_BEVEL_MODAL_UPDATES", "35"))
MODAL_WARMUP = int(os.environ.get("SOLID_BEVEL_MODAL_WARMUP", "5"))
MESH_MODAL_MOUSE_MARGIN_PIXELS = 4

CASES = {
    "chamfer": {"mesh": 1, "solid": 0},
    "fillet": {"mesh": 32, "solid": 1},
    "fillet_same_count": {"mesh": 32, "solid": 32},
}


def clear_scene():
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def create_cube(kind):
    clear_scene()
    if kind == "mesh":
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode="EDIT")
    else:
        bpy.ops.solid.sketch_add(type="RECTANGLE", size=1)
        obj = bpy.context.object
        assert bpy.ops.solid.extrude(distance=2) == {"FINISHED"}
    bpy.ops.mesh.select_mode(type="EDGE")
    bpy.ops.mesh.select_all(action="SELECT")
    return obj


def bevel(kind, segments, mode):
    if kind == "mesh":
        return bpy.ops.mesh.bevel(
            mode, offset=WIDTH, segments=segments, profile=0.5, affect="EDGES"
        )
    return bpy.ops.solid.bevel(mode, offset=WIDTH, segments=segments)


def edit_counts(obj):
    bm = bmesh.from_edit_mesh(obj.data)
    return (len(bm.verts), len(bm.edges), len(bm.faces))


def background_benchmark():
    selected_case = os.environ.get("SOLID_BEVEL_CASE")
    case_names = [selected_case] if selected_case else ["chamfer", "fillet"]
    if any(case not in CASES for case in case_names):
        raise ValueError(case_names)
    samples = {(case, kind): [] for case in case_names for kind in ("mesh", "solid")}
    counts = {}
    configs = [(case, kind) for case in case_names for kind in ("mesh", "solid")]
    for trial in range(TRIALS):
        ordered = configs if trial % 2 == 0 else list(reversed(configs))
        for case, kind in ordered:
            obj = create_cube(kind)
            segments = CASES[case][kind]
            wall_start = time.perf_counter()
            result = bevel(kind, segments, "EXEC_DEFAULT")
            wall_ms = (time.perf_counter() - wall_start) * 1000
            assert result == {"FINISHED"}, (case, kind, result)
            counts[(case, kind)] = edit_counts(obj)
            if trial >= WARMUP:
                samples[(case, kind)].append(wall_ms)

    output = []
    for case in case_names:
        for kind in ("mesh", "solid"):
            rows = samples[(case, kind)]
            wall = statistics.median(rows)
            output.append(
                {
                    "case": case,
                    "kind": kind,
                    "width": WIDTH,
                    "segments": CASES[case][kind],
                    "input_topology": [8, 12, 6],
                    "output_topology_vertices_edges_faces": counts[(case, kind)],
                    "samples": len(rows),
                    "median_operator_wall_ms": round(wall, 3),
                    "operator_updates_per_second_background_not_fps": round(1000 / wall, 1),
                }
            )
    print(
        "SOLID_BEVEL_EXEC_BENCHMARK "
        + json.dumps(
            {
                "blender": bpy.app.version_string,
                "trials_per_case": TRIALS,
                "warmup_trials_discarded": WARMUP,
                "results": output,
            }
        ),
        flush=True,
    )


def windowed_benchmark():
    kind = os.environ.get("SOLID_BEVEL_KIND", "mesh").lower()
    case = os.environ.get("SOLID_BEVEL_CASE", "fillet").lower()
    if kind not in ("mesh", "solid") or case not in CASES:
        raise ValueError((kind, case))
    if MODAL_WARMUP >= MODAL_UPDATES:
        raise ValueError("Modal warmup must be shorter than modal updates")
    segments = CASES[case][kind]
    obj = create_cube(kind)

    window = bpy.context.window
    area = next(area for area in bpy.context.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.view3d.view_selected(use_all_regions=False)
    bm = bmesh.from_edit_mesh(obj.data)
    if kind == "solid":
        # The solid tool scales width at its first selected source vertex. Mesh bevel scales from
        # the median of the selected geometry.
        center_local = bm.verts[0].co.copy()
    else:
        center_local = sum((vert.co for vert in bm.verts), Vector((0.0, 0.0, 0.0))) / max(
            1, len(bm.verts)
        )
    center_world = obj.matrix_world @ center_local

    state = {
        "active": False,
        "send_count": 0,
        "draw_times": [],
        "last_counts": None,
        "handle": None,
        "invoke_ms": None,
        "target_delta": None,
        "pixels_per_unit": None,
        "start_x": None,
        "start_y": None,
    }

    def simulate_mouse(local_x):
        window.event_simulate(
            type="MOUSEMOVE",
            value="NOTHING",
            x=region.x + local_x,
            y=region.y + state["start_y"],
        )

    def remove_handler():
        if state["handle"] is not None:
            bpy.types.SpaceView3D.draw_handler_remove(state["handle"], "WINDOW")
            state["handle"] = None

    def finish():
        state["active"] = False
        remove_handler()
        window.event_simulate(type="ESC", value="PRESS")

        def report_after_cancel():
            retained = state["draw_times"][MODAL_WARMUP:]
            intervals_ms = [
                (later - earlier) * 1000 for earlier, later in zip(retained, retained[1:])
            ]
            median_ms = statistics.median(intervals_ms) if intervals_ms else 0.0
            p95_ms = (
                sorted(intervals_ms)[int(0.95 * (len(intervals_ms) - 1))]
                if intervals_ms
                else 0.0
            )
            draw_hz = 1000 / median_ms if median_ms else 0.0
            print("SOLID_BEVEL_MODAL_BENCHMARK " + json.dumps({
                "kind": kind,
                "case": case,
                "width_target": WIDTH,
                "segments": segments,
                "target_mouse_delta_pixels": state["target_delta"],
                "projected_pixels_per_world_unit": round(state["pixels_per_unit"], 3),
                "mouse_events_sent": state["send_count"],
                "viewport_draw_callbacks": len(state["draw_times"]),
                "warmup_draws_discarded": MODAL_WARMUP,
                "modal_invoke_ms": round(state["invoke_ms"], 3),
                "median_viewport_draw_interval_ms": round(median_ms, 3),
                "p95_viewport_draw_interval_ms": round(p95_ms, 3),
                "viewport_draw_rate_hz": round(draw_hz, 1),
                "preview_vertices_edges_faces": state["last_counts"],
            }), flush=True)
            bpy.ops.wm.quit_blender()
            return None

        bpy.app.timers.register(report_after_cancel, first_interval=0.3)

    def send_next():
        index = state["send_count"]
        if index >= MODAL_UPDATES:
            # Let the final queued mouse event and its redraw complete, then stop collecting
            # before the tool is cancelled and the viewport returns to idle.
            def stop_sampling():
                state["active"] = False
                bpy.app.timers.register(finish, first_interval=0.25)
                return None

            bpy.app.timers.register(stop_sampling, first_interval=0.05)
            return None
        # Vary the width by a few viewport pixels so every event triggers a real bevel rebuild.
        delta = state["target_delta"] + (index % 5 - 2)
        simulate_mouse(state["start_x"] + delta)
        state["send_count"] += 1
        # Keep changing the width at a faster cadence than a normal display refresh. Blender's
        # event loop naturally delays subsequent input while a bevel update is still running.
        return 0.001

    def draw_callback():
        if not state["active"] or bpy.context.region != region:
            return
        state["draw_times"].append(time.perf_counter())
        state["last_counts"] = edit_counts(obj)

    def prepare_modal():
        # Let view_selected update the view matrices before estimating the modal width scale.
        bpy.context.view_layer.update()
        from bpy_extras import view3d_utils

        rv3d = area.spaces.active.region_3d
        center_2d = view3d_utils.location_3d_to_region_2d(region, rv3d, center_world)
        view_right = rv3d.view_rotation @ Vector((1, 0, 0))
        right_2d = view3d_utils.location_3d_to_region_2d(
            region, rv3d, center_world + view_right
        )
        if center_2d is None or right_2d is None:
            raise RuntimeError("Cube center is outside the viewport")
        pixels_per_unit = (right_2d - center_2d).length
        if pixels_per_unit <= 0:
            raise RuntimeError("Could not calculate viewport width scale")
        state["pixels_per_unit"] = pixels_per_unit
        mouse_margin = MESH_MODAL_MOUSE_MARGIN_PIXELS if kind == "mesh" else 0
        state["target_delta"] = max(1, round(WIDTH * pixels_per_unit) + mouse_margin)
        state["start_x"] = round(center_2d.x + 40)
        state["start_y"] = round(center_2d.y)
        state["handle"] = bpy.types.SpaceView3D.draw_handler_add(
            draw_callback, (), "WINDOW", "POST_PIXEL"
        )
        # Put the pointer at a known start location before invoking the modal tool.
        simulate_mouse(state["start_x"])
        bpy.app.timers.register(start_modal, first_interval=0.2)
        return None

    def start_modal():
        with bpy.context.temp_override(window=window, area=area, region=region):
            start = time.perf_counter()
            if kind == "mesh":
                result = bpy.ops.mesh.bevel(
                    "INVOKE_DEFAULT", offset=WIDTH, segments=segments, profile=0.5, affect="EDGES"
                )
            else:
                result = bpy.ops.solid.bevel("INVOKE_DEFAULT", offset=WIDTH, segments=segments)
            state["invoke_ms"] = (time.perf_counter() - start) * 1000
        if result != {"RUNNING_MODAL"}:
            remove_handler()
            raise RuntimeError((kind, case, result))
        state["active"] = True
        bpy.app.timers.register(send_next, first_interval=0.05)
        return None

    bpy.app.timers.register(prepare_modal, first_interval=0.2)


if bpy.app.background:
    background_benchmark()
else:
    windowed_benchmark()
