# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Interactive UI acceptance test for native NURBS topology editing.

Run from a GUI Blender build (the window stays open after completion)::

  blender --enable-event-simulate --python tests/python/bl_nurbs_topology_ui.py -- \
      --fixture /tmp/topology.blend --result /tmp/nurbs-ui-result.json \
      [--screenshot /tmp/nurbs-ui.png]

The script is timer driven so each simulated key is delivered in its own event
loop turn. It deliberately does not quit Blender or reopen the fixture.
"""

import argparse
import json
import math
import os
import sys
import traceback

import bpy
from mathutils import Vector


def _arguments():
    if "--" not in sys.argv:
        raise RuntimeError("Expected -- --fixture path --result jsonpath")
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--result", required=True)
    parser.add_argument("--screenshot")
    parser.add_argument("--quit", action="store_true", help="Quit this test Blender after it finishes")
    return parser.parse_args(sys.argv[sys.argv.index("--") + 1 :])


ARGS = _arguments()
RESULT_PATH = os.path.abspath(ARGS.result)
STEPS = []
WINDOW = bpy.context.window
AREA = None
REGION = None
OBJ = None
GEOMETRY_NAME = None
INITIAL_VERTEX_POSITIONS = {}
SEAM_VERTEX_IDS = set()
SEAM_EDGE_IDS = set()
SEAM_SUPPORT_IDS = set()
FACE_IDS_EXPECTED = []
PICKED_CHILD_SOURCE = None
KEY_EVENT_MAP = {
    "Z": "Z",
    "ZERO": "ZERO",
    "PERIOD": "PERIOD",
    "TWO": "TWO",
    "FIVE": "FIVE",
    "RET": "RET",
    "ESC": "ESC",
}


def _write_result(status, error=None):
    payload = {
        "status": status,
        "steps": list(STEPS),
        "error": error,
        "limitations": [
            "UI selection coordinates are projected from the specified analytic fixture samples.",
            "Face IDs are matched against the fixture's stable face collection ordering.",
        ],
    }
    os.makedirs(os.path.dirname(RESULT_PATH) or ".", exist_ok=True)
    with open(RESULT_PATH, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")


def _record(name, **data):
    STEPS.append({"step": name, **data})
    _write_result("running")


def _geometry():
    return bpy.data.nurbs_geometries[GEOMETRY_NAME]


def _snapshot():
    geometry = _geometry()
    return {
        "active": geometry.active_component_id,
        "vertices": {v.component_id: tuple(v.position) for v in geometry.vertices},
        "edges": [(e.component_id, e.support_curve_id, tuple(e.domain), e.select)
                  for e in geometry.edges],
        "faces": [(f.component_id, f.surface_id, f.select) for f in geometry.faces],
        "history": [(s.component_id, s.type) for s in geometry.selection_history],
    }


def _assert_selection(expected_ids, expected_type, label):
    snap = _snapshot()
    if expected_type == "VERTEX":
        selected = {v.component_id for v in _geometry().vertices if v.select}
    elif expected_type == "EDGE":
        selected = {e.component_id for e in _geometry().edges if e.select}
    else:
        selected = {f.component_id for f in _geometry().faces if f.select}
    assert selected == set(expected_ids), f"{label}: selected {selected}, expected {set(expected_ids)}"
    assert snap["active"] in selected, f"{label}: active component is not selected"
    assert any(i == snap["active"] and t == expected_type for i, t in snap["history"]), \
        f"{label}: selection history does not end on active {expected_type}"
    return snap


def _view_context():
    return bpy.context.temp_override(window=WINDOW, area=AREA, region=REGION)


def _project(world):
    from bpy_extras import view3d_utils
    region_3d = AREA.spaces.active.region_3d
    return view3d_utils.location_3d_to_region_2d(REGION, region_3d, Vector(world))


def _simulate(key, ctrl=False, shift=False, region_xy=None):
    if key not in KEY_EVENT_MAP:
        raise AssertionError("Unknown simulated key " + key)
    event_type = KEY_EVENT_MAP[key]
    # event_simulate takes window coordinates. A press/release pair is one
    # logical key; the next distinct key is scheduled on a later timer tick.
    if region_xy is None:
        x = REGION.x + REGION.width // 2
        y = REGION.y + REGION.height // 2
    else:
        x = REGION.x + int(region_xy[0])
        y = REGION.y + int(region_xy[1])
    text = {"ZERO": "0", "TWO": "2", "FIVE": "5", "PERIOD": "."}.get(key)
    WINDOW.event_simulate(type="MOUSEMOVE", value="NOTHING", x=x, y=y)
    press_args = {"type": event_type, "value": "PRESS", "ctrl": ctrl, "shift": shift,
                  "x": x, "y": y}
    if text is not None:
        press_args["unicode"] = text
    WINDOW.event_simulate(**press_args)
    WINDOW.event_simulate(type=event_type, value="RELEASE", ctrl=ctrl, shift=shift,
                          x=x, y=y)


def _guard_timer(callback):
    def guarded():
        try:
            return callback()
        except Exception:
            error = traceback.format_exc()
            _write_result("failed", error)
            print(error, file=sys.stderr, flush=True)
            if ARGS.quit:
                bpy.ops.wm.quit_blender()
            return None
    return guarded


def _gpu_pick(world, extend=False):
    xy = _project(world)
    assert xy is not None, f"Pick point {world} is outside the view"
    with _view_context():
        result = bpy.ops.view3d.select(
            "EXEC_DEFAULT", location=(int(xy.x), int(xy.y)),
            extend=extend, deselect=False, toggle=False,)
    assert "FINISHED" in result, f"GPU viewport selection failed at {tuple(xy)}: {result}"
    return (int(xy.x), int(xy.y))


def _simulate_mouse(region_xy):
    x = REGION.x + int(region_xy.x)
    y = REGION.y + int(region_xy.y)
    return WINDOW.event_simulate(type="MOUSEMOVE", value="NOTHING", x=x, y=y)


def _set_mode(mode):
    with _view_context():
        result = bpy.ops.nurbs.select_mode(type=mode)
    assert result in ({"FINISHED"}, {"CANCELLED"}), \
        f"Could not set NURBS {mode} mode: {result}"


def _find_view3d():
    global AREA, REGION
    for area in WINDOW.screen.areas:
        if area.type == "VIEW_3D":
            AREA = area
            REGION = next(region for region in area.regions if region.type == "WINDOW")
            break
    assert AREA is not None, "No 3D View area in interactive window"
    space = AREA.spaces.active
    rv3d = space.region_3d
    rv3d.view_perspective = "ORTHO"
    rv3d.view_location = (0.5, 0.5, 1.0)
    rv3d.view_distance = 4.0
    rv3d.view_rotation = Vector((4.0, 4.0, 2.0)).to_track_quat("Z", "Y")
    space.overlay.show_floor = False
    space.overlay.show_axis_x = False
    space.overlay.show_axis_y = False


def _setup():
    global OBJ, GEOMETRY_NAME, INITIAL_VERTEX_POSITIONS, SEAM_VERTEX_IDS
    global SEAM_EDGE_IDS, SEAM_SUPPORT_IDS, FACE_IDS_EXPECTED
    assert bpy.app.use_event_simulate, "Launch Blender with --enable-event-simulate"
    assert os.path.isfile(ARGS.fixture), "Fixture does not exist: " + ARGS.fixture
    _find_view3d()
    # Remove startup scene geometry so the GPU picker can only hit the native
    # fixture object. Keep the current screen and 3D View intact.
    for existing in list(bpy.context.scene.objects):
        bpy.data.objects.remove(existing, do_unlink=True)
    with bpy.data.libraries.load(os.path.abspath(ARGS.fixture), link=False) as (source, target):
        assert source.nurbs_geometries, "Fixture contains no nurbs_geometries"
        target.nurbs_geometries = list(source.nurbs_geometries)
    geometry = bpy.data.nurbs_geometries[-1]
    GEOMETRY_NAME = geometry.name
    OBJ = bpy.data.objects.new("NURBS UI Acceptance", geometry)
    bpy.context.scene.collection.objects.link(OBJ)
    OBJ.select_set(True)
    bpy.context.view_layer.objects.active = OBJ
    bpy.ops.object.mode_set(mode="EDIT")
    assert geometry.has_topology
    faces = list(geometry.faces)
    assert len(faces) == 2, f"Expected two fixture faces, got {len(faces)}"
    FACE_IDS_EXPECTED = [face.component_id for face in faces]
    INITIAL_VERTEX_POSITIONS = {v.component_id: tuple(v.position) for v in geometry.vertices}
    SEAM_VERTEX_IDS = {i for i, p in INITIAL_VERTEX_POSITIONS.items() if abs(p[2] - 1.0) < 1e-6}
    assert len(SEAM_VERTEX_IDS) >= 3, "Fixture seam must contain at least three vertices"
    selected_edges = [e for e in geometry.edges if e.select]
    assert len(selected_edges) == 2, "Fixture must start with its two split seam edges selected"
    SEAM_EDGE_IDS = {e.component_id for e in selected_edges}
    SEAM_SUPPORT_IDS = {e.support_curve_id for e in selected_edges}
    assert geometry.active_component_id in SEAM_EDGE_IDS
    assert any(i in SEAM_EDGE_IDS and kind == "EDGE"
               for i, kind in ((s.component_id, s.type) for s in geometry.selection_history))
    _record("fixture_loaded", face_ids=FACE_IDS_EXPECTED,
            seam_edge_ids=sorted(SEAM_EDGE_IDS), seam_support_ids=sorted(SEAM_SUPPORT_IDS),
            seam_vertex_ids=sorted(SEAM_VERTEX_IDS))


def _check_faces():
    # Analytic quarter-circle in the fixture: (cos(pi*u/2), sin(pi*u/2), z).
    # At this camera, the two interior samples have distinct screen positions.
    _set_mode("FACE")
    with _view_context():
        bpy.ops.nurbs.select_all(action="DESELECT")
    ua, va = 0.1, 0.1
    pa = (math.cos(math.pi * 0.5 * ua), math.sin(math.pi * 0.5 * ua), va)
    pick_a = _gpu_pick(pa)
    selected_a = {f.component_id for f in _geometry().faces if f.select}
    assert selected_a == {FACE_IDS_EXPECTED[0]}, \
        f"A interior sample selected {selected_a}, expected first ordered face {FACE_IDS_EXPECTED[0]}"
    snap_a = _snapshot()
    assert snap_a["active"] in selected_a and any(i == snap_a["active"] and t == "FACE"
        for i, t in snap_a["history"])
    pb = (math.cos(math.pi * 0.5 * 0.5), math.sin(math.pi * 0.5 * 0.5), 1.6)
    pick_b = _gpu_pick(pb)
    selected_b = {f.component_id for f in _geometry().faces if f.select}
    assert selected_b == {FACE_IDS_EXPECTED[1]}, \
        f"B interior sample selected {selected_b}, expected second ordered face {FACE_IDS_EXPECTED[1]}"
    snap_b = _snapshot()
    assert snap_b["active"] in selected_b and any(i == snap_b["active"] and t == "FACE"
        for i, t in snap_b["history"])
    _record("gpu_face_picks", face_a=FACE_IDS_EXPECTED[0], face_b=FACE_IDS_EXPECTED[1],
            pick_a=pick_a, pick_b=pick_b)


def _check_vertex_pick():
    _set_mode("VERTEX")
    with _view_context():
        bpy.ops.nurbs.select_all(action="DESELECT")
    vertex = next(v for v in _geometry().vertices
                  if v.component_id in SEAM_VERTEX_IDS and
                  all(abs(a - b) < 1e-6 for a, b in zip(v.position, (1.0, 0.0, 1.0))))
    coords = _gpu_pick(vertex.position)
    _assert_selection({vertex.component_id}, "VERTEX", "seam endpoint GPU pick")
    _record("gpu_vertex_pick", vertex_id=vertex.component_id, pick=coords)


def _check_edge_picks():
    _set_mode("EDGE")
    with _view_context():
        bpy.ops.nurbs.select_all(action="DESELECT")
    # Support curve is the exact rational quarter-circle. Evaluate the
    # quadratic rational Bernstein form at t=.25 and .75; split seam children
    # span [0,.5] and [.5,1].
    w = math.sqrt(0.5)
    def quarter(t):
        a = (1.0 - t) ** 2
        b = 2.0 * (1.0 - t) * t * w
        c = t * t
        denom = a + b + c
        return ((a + b) / denom, (b + c) / denom, 1.0)
    def edge_near(t):
        p = quarter(t)
        return min((e for e in _geometry().edges if e.support_curve_id in SEAM_SUPPORT_IDS),
                   key=lambda e: abs((e.domain[0] + e.domain[1]) * 0.5 - t))
    edge_a, edge_b = edge_near(0.25), edge_near(0.75)
    assert edge_a.component_id != edge_b.component_id
    _gpu_pick(quarter(0.25))
    selected_a = {e.component_id for e in _geometry().edges if e.select}
    assert selected_a == {edge_a.component_id}, f"First seam edge pick selected {selected_a}"
    _gpu_pick(quarter(0.75), extend=True)
    selected_b = {e.component_id for e in _geometry().edges if e.select}
    assert selected_b == {edge_a.component_id, edge_b.component_id}, \
        f"Extend edge pick selected {selected_b}"
    assert selected_b == SEAM_EDGE_IDS, \
        f"GPU picked seam IDs {selected_b}, fixture initialized {SEAM_EDGE_IDS}"
    snap = _assert_selection(selected_b, "EDGE", "extended seam edge picks")
    _record("gpu_edge_picks", edge_ids=sorted(selected_b),
            support_ids=sorted(SEAM_SUPPORT_IDS), history=snap["history"])


def _check_native_point_snap():
    # Move a curve-only NURBS control point onto a topological vertex belonging
    # to another NURBS object. This exercises exact edit-data snapping rather
    # than sampled vertices from the display mesh.
    with _view_context():
        bpy.ops.object.mode_set(mode="OBJECT")
    OBJ.select_set(False)
    with _view_context():
        bpy.ops.object.add(type="NURBS", location=(6.0, 0.0, 2.0))
    source = bpy.context.object
    source.name = "NURBS Snap Source"
    source.scale = (3.0, 3.0, 3.0)
    OBJ.select_set(True)
    source.select_set(True)
    bpy.context.view_layer.objects.active = source
    with _view_context():
        bpy.ops.object.mode_set(mode="EDIT")
    _set_mode("VERTEX")
    with _view_context():
        bpy.ops.nurbs.select_all(action="DESELECT")

    target = next(v for v in _geometry().vertices
                  if all(abs(a - b) < 1e-6 for a, b in zip(v.position, (1.0, 0.0, 0.0))))
    target_world = OBJ.matrix_world @ Vector(target.position)
    source_curve = source.data.curves[0]
    source_control_vertex = source_curve.control_vertices[0]
    source_world = source.matrix_world @ Vector(source_control_vertex.position)
    assert _project(target_world) is not None and _project(source_world) is not None, \
        "NURBS snap source or target is outside the viewport"

    source_pixel = _gpu_pick(source_world)
    selected_source_ids = {cv.component_id for curve in source.data.curves
                           for cv in curve.control_vertices if cv.select}
    assert selected_source_ids == {source_control_vertex.component_id}, \
        f"Snap source pick selected {selected_source_ids}"
    target_pixel = _project(target_world)
    _simulate_mouse(Vector(source_pixel))

    state = {"step": 0}

    def tick():
        if state["step"] == 0:
            with _view_context():
                result = bpy.ops.transform.translate(
                    "INVOKE_DEFAULT",
                    snap=True,
                    snap_elements={"VERTEX"},
                    snap_target="CLOSEST",
                    use_snap_self=True,
                    use_snap_edit=True,
                    use_snap_nonedit=True,
                )
            assert "RUNNING_MODAL" in result, f"Could not start snapped NURBS translation: {result}"
        elif state["step"] == 1:
            _simulate_mouse(target_pixel)
        elif state["step"] == 2:
            _simulate("RET", region_xy=target_pixel)
        else:
            moved = source.matrix_world @ Vector(source_control_vertex.position)
            assert (moved - target_world).length < 2e-4, \
                f"NURBS point snap missed its exact target: got {tuple(moved)}, " \
                f"expected {tuple(target_world)}"
            _record("native_point_snap", source_id=source_control_vertex.component_id,
                    target_id=target.component_id, world_position=list(moved))
            with _view_context():
                bpy.ops.object.mode_set(mode="OBJECT")
            bpy.data.objects.remove(source, do_unlink=True)
            OBJ.select_set(True)
            bpy.context.view_layer.objects.active = OBJ
            with _view_context():
                bpy.ops.object.mode_set(mode="EDIT")
            timer = _check_translation()
            bpy.app.timers.register(_guard_timer(timer), first_interval=0.35)
            return None
        state["step"] += 1
        return 0.35

    return tick


def _check_translation():
    global INITIAL_VERTEX_POSITIONS
    # The selected edges from fixture setup select only their shared seam CVs.
    _set_mode("EDGE")
    with _view_context():
        bpy.ops.nurbs.select_all(action="DESELECT")
    # Re-select the original seam children through GPU viewport picking after
    # the earlier component-mode acceptance steps.
    w = math.sqrt(0.5)
    def quarter(t):
        a, b, c = (1.0 - t) ** 2, 2.0 * (1.0 - t) * t * w, t * t
        d = a + b + c
        return ((a + b) / d, (b + c) / d, 1.0)
    for t in (0.25, 0.75):
        _gpu_pick(quarter(t), extend=(t > 0.25))
    before = _snapshot()
    selected = {e.component_id for e in _geometry().edges if e.select}
    assert selected == SEAM_EDGE_IDS, f"Could not restore fixture seam selection: {selected}"
    assert {e.support_curve_id for e in _geometry().edges if e.select} == SEAM_SUPPORT_IDS
    seam_ids_now = {v.component_id for v in _geometry().vertices if v.select}
    # Edge selection may not flag incident vertices in RNA; derive expected
    # seam controls from fixture geometry's middle row, preserved by IDs.
    assert seam_ids_now.issubset(SEAM_VERTEX_IDS)
    bpy.ops.ed.undo_push()
    _record("translation_baseline", selected_edges=sorted(selected), active=before["active"])

    with _view_context():
        result = bpy.ops.transform.translate("INVOKE_DEFAULT")
    assert "RUNNING_MODAL" in result, f"Could not start modal NURBS translation: {result}"

    keys = ["Z", "ZERO", "PERIOD", "TWO", "FIVE", "RET"]
    state = {"index": 0}
    def key_timer():
        i = state["index"]
        if i < len(keys):
            _simulate(keys[i])
            state["index"] += 1
            return 0.35
        _verify_translation()
        return None
    return key_timer


def _verify_translation():
    geometry = _geometry()
    now = {v.component_id: tuple(v.position) for v in geometry.vertices}
    moved = set()
    for component_id, p in INITIAL_VERTEX_POSITIONS.items():
        q = now[component_id]
        expected_z = 1.25 * p[2] if p[2] <= 1.0 else p[2] + 0.25 * (2.0 - p[2])
        if abs(expected_z - p[2]) > 1e-6:
            moved.add(component_id)
        assert abs(q[0] - p[0]) < 2e-5 and abs(q[1] - p[1]) < 2e-5
        assert abs(q[2] - expected_z) < 2e-5, \
            f"Transform mismatch for {component_id}: before={p}, after={q}"
    _record("typed_translate", moved_ids=sorted(moved), delta=(0.0, 0.0, 0.25))
    _start_undo_redo()


def _start_undo_redo():
    state = {"index": 0}
    def tick():
        if state["index"] == 0:
            with _view_context():
                result = bpy.ops.ed.undo()
            assert "FINISHED" in result, f"NURBS undo failed: {result}"
        elif state["index"] == 1:
            now = {v.component_id: tuple(v.position) for v in _geometry().vertices}
            for component_id, p in INITIAL_VERTEX_POSITIONS.items():
                assert all(abs(a - b) < 2e-5 for a, b in zip(now[component_id], p)), \
                    f"Undo did not restore vertex {component_id}"
            _record("operator_undo", restored=True)
            with _view_context():
                result = bpy.ops.ed.redo()
            assert "FINISHED" in result, f"NURBS redo failed: {result}"
        else:
            baseline = INITIAL_VERTEX_POSITIONS
            now = {v.component_id: tuple(v.position) for v in _geometry().vertices}
            for component_id, p in baseline.items():
                expected_z = 1.25 * p[2] if p[2] <= 1.0 else p[2] + 0.25 * (2.0 - p[2])
                assert abs(now[component_id][2] - expected_z) < 2e-5, \
                    f"Redo did not restore translated vertex {component_id}"
            _record("operator_undo_redo", undone=True, redone=True)
            _start_cancel()
            return None
        state["index"] += 1
        return 0.35
    bpy.app.timers.register(_guard_timer(tick), first_interval=0.35)


def _start_cancel():
    # Re-select seam and start a real keyboard transform; escape must return
    # to the redo state without changing its positions.
    with _view_context():
        bpy.ops.nurbs.select_all(action="DESELECT")
    w = math.sqrt(0.5)
    def quarter(t):
        a, b, c = (1.0 - t) ** 2, 2.0 * (1.0 - t) * t * w, t * t
        d = a + b + c
        return ((a + b) / d, (b + c) / d, 1.0)
    for t in (0.25, 0.75):
        point = quarter(t)
        _gpu_pick((point[0], point[1], 1.25), extend=(t > 0.25))
    state = {"index": 0}
    with _view_context():
        result = bpy.ops.transform.translate("INVOKE_DEFAULT")
    assert "RUNNING_MODAL" in result, f"Could not start modal NURBS translation: {result}"

    keys = ["Z", "ZERO", "PERIOD", "FIVE", "ESC"]
    def tick():
        if state["index"] < len(keys):
            _simulate(keys[state["index"]])
            state["index"] += 1
            return 0.35
        now = {v.component_id: tuple(v.position) for v in _geometry().vertices}
        for component_id, p in INITIAL_VERTEX_POSITIONS.items():
            expected_z = 1.25 * p[2] if p[2] <= 1.0 else p[2] + 0.25 * (2.0 - p[2])
            assert abs(now[component_id][2] - expected_z) < 2e-5, \
                "Escape did not cancel the in-progress transform"
        _record("keyboard_cancel", restored=True)
        _run_split()
        return None
    bpy.app.timers.register(_guard_timer(tick), first_interval=0.35)


def _run_split():
    global PICKED_CHILD_SOURCE
    selected_before = {e.component_id for e in _geometry().edges if e.select}
    supports_before = {e.support_curve_id for e in _geometry().edges
                       if e.component_id in selected_before}
    face_count = len(_geometry().faces)
    with _view_context():
        result = bpy.ops.nurbs.edge_split(factor=0.5)
    assert "FINISHED" in result, f"Native edge split failed: {result}"
    geometry = _geometry()
    selected = [e for e in geometry.edges if e.select]
    assert len(selected) == 4, f"Expected 4 selected child edges, got {len(selected)}"
    assert {e.support_curve_id for e in selected} == supports_before
    assert len(geometry.faces) == face_count, "Edge split changed face count"
    assert geometry.active_component_id in {e.component_id for e in selected}
    edge_ids = {e.component_id for e in geometry.edges}
    assert all(item.component_id in edge_ids and item.component_id in
               {e.component_id for e in selected}
               for item in geometry.selection_history if item.type == "EDGE")
    source_support = next(iter(supports_before))
    candidates = [e for e in selected if e.support_curve_id == source_support]
    containing = [e for e in candidates if e.domain[0] <= 0.125 <= e.domain[1]]
    assert containing, "No split child contains source parameter 0.125"
    PICKED_CHILD_SOURCE = (containing[0].component_id, containing[0].support_curve_id,
                           tuple(containing[0].domain))
    _record("edge_split", selected_child_ids=sorted(e.component_id for e in selected),
            support_ids=sorted(supports_before), face_count=face_count)
    _check_split_child_pick()


def _check_split_child_pick():
    edge_id, support_id, domain = PICKED_CHILD_SOURCE
    # Fixture support is quarter-circle; child-domain .125 is source parameter .125.
    t = 0.125
    w = math.sqrt(0.5)
    a, b, c = (1.0 - t) ** 2, 2.0 * (1.0 - t) * t * w, t * t
    d = a + b + c
    point = ((a + b) / d, (b + c) / d, 1.25)
    with _view_context():
        bpy.ops.nurbs.select_all(action="DESELECT")
    _set_mode("EDGE")
    _gpu_pick(point)
    selected = {e.component_id for e in _geometry().edges if e.select}
    assert selected == {edge_id}, \
        f"Post-split pick selected {selected}, expected stable child ID {edge_id}"
    _assert_selection({edge_id}, "EDGE", "post-split stable ID pick")
    _record("post_split_child_pick", edge_id=edge_id,
            source_support=support_id, domain=list(domain))
    _check_nurbs_connected_proportional()


def _check_nurbs_connected_proportional():
    with _view_context():
        bpy.ops.object.mode_set(mode="OBJECT")
    OBJ.select_set(False)
    def create_source(name):
        with _view_context():
            result = bpy.ops.object.add(type="NURBS", location=(1.5, 1.5, 0.0))
        assert "FINISHED" in result, f"Could not create proportional test curve: {result}"
        source = bpy.context.object
        source.name = name
        with _view_context():
            bpy.ops.object.mode_set(mode="EDIT")
        _set_mode("VERTEX")
        with _view_context():
            bpy.ops.nurbs.select_all(action="DESELECT")
        control_vertices = list(source.data.curves[0].control_vertices)
        baseline = [Vector(vertex.position) for vertex in control_vertices]
        _gpu_pick(source.matrix_world @ baseline[0])
        selected_ids = {vertex.component_id for curve in source.data.curves
                        for vertex in curve.control_vertices if vertex.select}
        assert selected_ids == {control_vertices[0].component_id}, \
            f"Proportional test pick selected {selected_ids}"
        return source, control_vertices, baseline

    def translate(control_vertices, baseline, proportional_connected):
        with _view_context():
            result = bpy.ops.transform.translate(
                "EXEC_DEFAULT",
                value=(0.0, 0.0, 1.0),
                use_proportional_edit=True,
                use_proportional_connected=proportional_connected,
                proportional_size=4.5,
            )
        assert "FINISHED" in result, f"Proportional NURBS translation failed: {result}"
        return [Vector(vertex.position) - position
                for vertex, position in zip(control_vertices, baseline)]

    source, control_vertices, baseline = create_source("NURBS Connected Proportional Source")
    connected_delta = translate(control_vertices, baseline, True)
    assert abs(connected_delta[0].z - 1.0) < 1e-5
    assert 0.0 < connected_delta[1].z < 1.0, \
        f"Connected proportional falloff did not reach the first neighbor: {connected_delta}"
    assert 0.0 < connected_delta[2].z < connected_delta[1].z, \
        f"Connected control-polygon distances are not increasing: {connected_delta}"
    assert abs(connected_delta[3].z) < 1e-6, \
        f"Connected falloff reached beyond its world-space radius: {connected_delta}"

    with _view_context():
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.data.objects.remove(source, do_unlink=True)

    source, control_vertices, baseline = create_source("NURBS Euclidean Proportional Source")
    disconnected_delta = translate(control_vertices, baseline, False)
    assert disconnected_delta[2].z > connected_delta[2].z * 2.0, \
        f"Connected mode did not follow the control polygon: connected={connected_delta}, " \
        f"Euclidean={disconnected_delta}"
    _record("connected_proportional_distances",
            connected=[delta.z for delta in connected_delta],
            euclidean=[delta.z for delta in disconnected_delta])

    with _view_context():
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.data.objects.remove(source, do_unlink=True)
    _check_nurbs_curve_nearest_snap()


def _check_nurbs_curve_nearest_snap():
    # The target is a rational quarter-circle. The expected snap location is
    # evaluated at the middle of its parameter domain, between its CVs.
    for existing in list(bpy.context.scene.objects):
        bpy.data.objects.remove(existing, do_unlink=True)
    with _view_context():
        result = bpy.ops.nurbs.primitive_arc_add(
            radius=1.0,
            start_angle=0.0,
            sweep_angle=math.pi * 0.5,
            location=(0.0, 0.0, 1.0),
            rotation=(0.0, 0.0, 0.0),
            enter_editmode=False,
        )
    assert "FINISHED" in result, f"Could not create exact snap target arc: {result}"
    target = bpy.context.object
    target.select_set(False)

    with _view_context():
        result = bpy.ops.object.add(type="NURBS", location=(3.0, 0.0, 1.0))
    assert "FINISHED" in result, f"Could not create NURBS snap source: {result}"
    source = bpy.context.object
    with _view_context():
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.nurbs.select_all(action="DESELECT")
    source_control_vertex = source.data.curves[0].control_vertices[0]
    source_world = source.matrix_world @ Vector(source_control_vertex.position)
    target_world = Vector((math.sqrt(0.5), math.sqrt(0.5), 1.0))
    source_pixel = _gpu_pick(source_world)
    target_pixel = _project(target_world)
    assert target_pixel is not None, "NURBS nearest-snap target is outside the viewport"
    _simulate_mouse(Vector(source_pixel))

    state = {"step": 0}

    def tick():
        if state["step"] == 0:
            with _view_context():
                result = bpy.ops.transform.translate(
                    "INVOKE_DEFAULT",
                    snap=True,
                    snap_elements={"EDGE"},
                    snap_target="CLOSEST",
                    use_snap_self=True,
                    use_snap_edit=True,
                    use_snap_nonedit=True,
                )
            assert "RUNNING_MODAL" in result, f"Could not start NURBS edge snap: {result}"
        elif state["step"] == 1:
            _simulate_mouse(target_pixel)
        elif state["step"] == 2:
            _simulate("RET", region_xy=target_pixel)
        else:
            moved = source.matrix_world @ Vector(source_control_vertex.position)
            radius_error = abs(math.hypot(moved.x, moved.y) - 1.0)
            angle_error = abs(math.atan2(moved.y, moved.x) - math.pi * 0.25)
            assert radius_error < 1e-5 and abs(moved.z - 1.0) < 1e-5, \
                f"NURBS curve snap left the exact arc: got {tuple(moved)}, " \
                f"radial error {radius_error}"
            assert angle_error < 0.01, \
                f"NURBS curve snap did not follow the mouse to the arc midpoint: {tuple(moved)}"
            _record("native_curve_nearest_snap",
                    target=target.name,
                    world_position=list(moved),
                    radial_error=radius_error,
                    angular_error=angle_error)
            with _view_context():
                bpy.ops.object.mode_set(mode="OBJECT")
            bpy.data.objects.remove(source, do_unlink=True)
            bpy.data.objects.remove(target, do_unlink=True)
            _check_nurbs_topology_edge_snap()
            return None
        state["step"] += 1
        return 0.35

    bpy.app.timers.register(_guard_timer(tick), first_interval=0.35)


def _check_nurbs_topology_edge_snap():
    # Reuse the split and translated rational seam from the topology fixture.
    geometry = bpy.data.nurbs_geometries[GEOMETRY_NAME]
    seam_edge = next(
        edge for edge in geometry.edges if edge.support_curve_id in SEAM_SUPPORT_IDS
    )
    parameter = (seam_edge.domain[0] + seam_edge.domain[1]) * 0.5
    a = (1.0 - parameter) ** 2
    b = 2.0 * (1.0 - parameter) * parameter * math.sqrt(0.5)
    c = parameter ** 2
    denominator = a + b + c
    expected = Vector(((a + b) / denominator, (b + c) / denominator, 1.25))

    target = bpy.data.objects.new("NURBS Topology Snap Target", geometry)
    bpy.context.scene.collection.objects.link(target)
    target.select_set(False)
    with _view_context():
        result = bpy.ops.object.add(
            type="NURBS",
            location=(expected.x + 2.0, expected.y, expected.z),
        )
    assert "FINISHED" in result, f"Could not create topology snap source: {result}"
    source = bpy.context.object
    with _view_context():
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.nurbs.select_all(action="DESELECT")
    source_control_vertex = source.data.curves[0].control_vertices[0]
    source_world = source.matrix_world @ Vector(source_control_vertex.position)
    source_pixel = _gpu_pick(source_world)
    target_pixel = _project(expected)
    assert target_pixel is not None, "NURBS topology edge target is outside the viewport"
    _simulate_mouse(Vector(source_pixel))

    state = {"step": 0}

    def tick():
        if state["step"] == 0:
            with _view_context():
                result = bpy.ops.transform.translate(
                    "INVOKE_DEFAULT",
                    snap=True,
                    snap_elements={"EDGE"},
                    snap_target="CLOSEST",
                    use_snap_self=True,
                    use_snap_edit=True,
                    use_snap_nonedit=True,
                )
            assert "RUNNING_MODAL" in result, f"Could not start NURBS topology edge snap: {result}"
        elif state["step"] == 1:
            _simulate_mouse(target_pixel)
        elif state["step"] == 2:
            _simulate("RET", region_xy=target_pixel)
        else:
            moved = source.matrix_world @ Vector(source_control_vertex.position)
            radius_error = abs(math.hypot(moved.x, moved.y) - 1.0)
            assert radius_error < 1e-5 and abs(moved.z - 1.25) < 1e-5, \
                f"NURBS topology edge snap left its rational support: {tuple(moved)}, " \
                f"radial error {radius_error}"
            _record("native_topology_edge_nearest_snap",
                    target=target.name,
                    edge_id=seam_edge.component_id,
                    world_position=list(moved),
                    radial_error=radius_error)
            with _view_context():
                bpy.ops.object.mode_set(mode="OBJECT")
            _finish()
            return None
        state["step"] += 1
        return 0.35

    bpy.app.timers.register(_guard_timer(tick), first_interval=0.35)


def _finish():
    if ARGS.screenshot:
        bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)
        bpy.ops.screen.screenshot(filepath=os.path.abspath(ARGS.screenshot))
    _write_result("passed")
    if ARGS.quit:
        print("NURBS topology UI acceptance passed; closing test Blender", flush=True)
        bpy.ops.wm.quit_blender()
    else:
        print("NURBS topology UI acceptance passed; Blender remains open", flush=True)


def _main_timer():
    try:
        _setup()
        # Allow a complete viewport redraw after setting the view and adding
        # the fixture object before issuing GPU selections.
        bpy.app.timers.register(_guard_timer(_acceptance_timer), first_interval=0.75)
        return None
    except Exception:
        error = traceback.format_exc()
        _write_result("failed", error)
        print(error, file=sys.stderr, flush=True)
        return None


def _acceptance_timer():
    try:
        _check_faces()
        _check_vertex_pick()
        _check_edge_picks()
        timer = _check_native_point_snap()
        bpy.app.timers.register(_guard_timer(timer), first_interval=0.35)
        return None
    except Exception:
        error = traceback.format_exc()
        _write_result("failed", error)
        print(error, file=sys.stderr, flush=True)
        return None


_write_result("running")
bpy.app.timers.register(_main_timer, first_interval=0.5)
