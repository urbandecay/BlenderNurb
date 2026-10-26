# Ctrl+B launch modifier and mesh bevel controls

The user reported that the tool opened on an extruded box's selected top face,
but its width stayed at zero. This was reproduced independently in a factory
scene. It is a separate failure from the earlier planar chart and corner fixes.

## Cause and mesh reference

The native operator checked `event->modifier & KM_CTRL` on every mouse move.
Ctrl still held from launching Ctrl+B therefore enabled 0.1-unit snapping.
Small positive widths rounded to zero. The previous UI helper released Ctrl
before dragging, so those tests missed the failure.

Mesh bevel starts with snapping disabled. It enables it on a new `SNAP_ON`
modal action after invocation. The paired before-fix test used the same view,
selected top face, and event sequence for mesh and NURBS: Ctrl press, B press
and release, then an eight-pixel move while Ctrl remained held. Native stayed
at 8 vertices / 12 edges / 6 faces and width zero. Mesh produced the expected
12 / 20 / 10 topology at width approximately 0.013211.

Before-fix logs are `/tmp/nurbs-face-input-before-bevel_face_launch_ctrl_workflow.log`
and `/tmp/nurbs-face-input-before-bevel_face_launch_ctrl_mesh_workflow.log`.
The old captures are under `/tmp/blender-nurbs-face-input-ui`.

## Change

`ED_keymap_curve` assigns `CURVE_OT_bevel` to Blender's existing `Bevel Modal Map`.
The native operator consumes its named confirm/cancel, segment, clamp, snap and
precision actions, including user remappings. It does not copy the mesh editor's
private numeric enum. Status hints resolve the same user bindings.

Snap and precision are explicit modal state. Modifier transitions rebase the
current width and mouse position, matching the mesh's five-pixel launch margin
when the width is still zero. Input and status changes are in the curve editor;
the native BKE geometry planner, error budgets and persistent topology are
unchanged in this correction.

## Final validation

The final executable was built with:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Both returned zero. Logs: `/tmp/nurbs-bevel-input-release-build.log` and
`/tmp/nurbs-bevel-input-final-install.log`. The executable is
`/home/molotovgirl/Desktop/build_linux/bin/blender`, build date/time
2026-10-26 18:14:57, branch `nurbs/shared-topology-foundation`, modified
HEAD `824bc36589b6`. Changes are local and unpushed.

Twelve isolated factory-scene UI workflows passed. Exact commands, return codes
and per-test logs are recorded in `/tmp/nurbs-bevel-input-ui-results.json`.
`/tmp/nurbs-bevel-input-run-ui.py` runs the batch. Each process used its own
temporary `BLENDER_USER_CONFIG`, including tests that remap snap to Alt.

The six new tests pair NURBS and actual mesh bevel with Ctrl held during launch,
Ctrl released before the first move, and snap remapped to Alt. Independent camera
projection checks the expected launch width. The measured rail widths matched
between NURBS and mesh within 2e-6 in every pair:

| Input sequence | NURBS | Mesh |
| --- | ---: | ---: |
| Eight-pixel launch movement | 0.013211 | 0.013211 |
| New explicit snap press and movement | 0.100000 | 0.100000 |
| Release snap, enable precision, move again | 0.104404 | 0.104404 |

These values were the same across all three paired sequences at the test view
size. JSON measurements and saved sources are under
`/tmp/blender-nurbs-face-input-final-ui`. Each workflow also checks cancel,
numeric confirmation, face selection mode, undo/redo, Edit Mode reentry, G and
Shift+D. The six regressions cover an open rounded corner, a rounded box,
connected rounded edges, tapered mouse input, clamping and rotated mouse input.

A separate visible desktop test also passed and closed automatically:

```sh
env BLENDER_USER_CONFIG=/tmp/nurbs-bevel-input-config-visible BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-bevel-input-visible /home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup --enable-event-simulate --window-geometry 100 70 1400 900 --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_face_launch_ctrl_workflow
```

Log: `/tmp/nurbs-bevel-input-visible.log`. Its different window size gives a
different pixel-to-width scale, also checked by independent projection. Inspected
captures: `bevel-native-connected-face-launch-ctrl-ctrl-held.png` and
`bevel-native-connected-face-launch-ctrl-finished.png` in that output directory.
The final paired mesh/native launch captures were also visually inspected.
No user project was opened or modified.

Five compatibility checks passed in 7.89 seconds:

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
```

Log: `/tmp/nurbs-bevel-input-ctest.log`. Clang-format 20.1.8 dry-run passed on
`curve_ops.cc` and `editnurbs_bevel.cc`; Python byte-compilation and
`git diff --check` passed. The headless logs contain EGL fallback diagnostics;
the UI tests reached their pass markers and exited zero. The existing unrelated
`object.make_dupli_face` UI warning remains outside this correction.

The kernel suites recorded in the earlier planar-chart report were not rerun
for this editor-only correction. General curved-face bevels, vertex bevel,
custom profiles and higher-valence rounded blends remain incomplete. The shared
modal map does not imply that every mesh bevel feature is implemented.
