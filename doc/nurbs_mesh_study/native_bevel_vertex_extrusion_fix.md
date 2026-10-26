# Bevel failure after extrusion in the initial vertex mode

The reported `NURBS face is not planar within the explicit geometry error budget`
was reproduced on a copy of the actual unsaved user scene. The active executable
was the current 2026-10-26 19:09:30 build, not a replaced or outdated runtime.
The Blender connector was unavailable; a temporary scene copy was saved through
the Python console and the viewport restored. The live geometry was not edited.
Source copy: `/tmp/nurbs-user-bevel-source.blend`.

## Cause and prevention

Adding a NURBS line and pressing Tab leaves its tagged legacy spline in vertex
mode. Extruding before explicitly selecting a native component mode used the
legacy float control-point transform. Independent additions rounded the corners
of an otherwise translated cap differently. Importing those coordinates later
produced genuinely nonplanar bilinear supports with zero declared tolerance.
The user's top support has a mixed X coefficient of
`-1.1920928955078125e-7`. Reducing bevel width cannot correct that source warp.

Earlier freehand UI coverage pressed `2` immediately after Tab. That entered
native topology before the first extrusion and missed this path.

The default `curve.extrude_move` macro now enters native topology before
extruding a complete tagged source. Source promotion, component construction and
selection are preflighted before committing the owner. Movement then uses the
existing double-precision native transform adapter. No planarity check, fitting
rule or geometry budget was relaxed.

`curve.extrude` remains available as the standalone legacy operation. The hidden
`use_native_topology` override also permits an explicit choice for scripts and
legacy compatibility tests. When unset, the operator identifies the enclosing
`CURVE_OT_extrude_move` macro. It is deliberately not fixed in macro defaults,
because Blender applies those defaults over caller-supplied suboperator values.
Untagged NURBS paths retain their legacy extrusion behavior.

Changes are in `editcurve.cc` and the shared component source preparation in
`editnurbs.cc`. Existing native owners continue through their original route.

## Existing scene recovery

Restarting cannot remove the warp already stored in the original scene. Two
independent files were saved with their main area restored to the 3D viewport:

- `/home/molotovgirl/Desktop/NURBS Cube Before Bevel.blend`: the original source.
- `/home/molotovgirl/Desktop/NURBS Cube Bevel Recovery.blend`: the selected top
  boundary beveled at width 0.1, three segments, with an explicit `max_error=1e-6`.

This recovery is a tolerance-bounded planar reparameterization of an already
rounded legacy input. It is not evidence of a general curved-network fillet.
The operator reported a construction bound of `4.7683729940284508e-7` source units.
Its derived mesh has 884 vertices and 1,764 positive-area polygons; every mesh
edge has two incident polygons, and all coordinates are finite. Saving and
reloading retained native geometry, no legacy splines, and Edit Mode. Validation:
`/tmp/nurbs-user-recovery-validation.log`. The live user scene was left unchanged.

New extrusions pass bevel with the unchanged default error budget of `1e-9`.
The regression uses the user's two displacements and checks one and three bevel
segments. Additional tests cover the explicit legacy override and an ordinary
untagged NURBS path.

## Build and validation

The final binary is `/home/molotovgirl/Desktop/build_linux/bin/blender`, reporting
build 2026-10-26 22:43:54. Build and install returned zero:

```sh
cmake --build ../build_linux --target blender --parallel 4
cmake --install ../build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/nurbs-vertex-extrusion-release-build.log` and
`/tmp/nurbs-vertex-extrusion-release-install.log`.

The background line suite passed **40 tests**:

```sh
../build_linux/bin/blender --background --factory-startup --python-exit-code 1 --python tests/python/bl_nurbs_line.py
```

The newly added regression failed before the correction. Logs:
`/tmp/nurbs-vertex-extrusion-before.log` and
`/tmp/nurbs-vertex-extrusion-release-tests.log`.

All **seven isolated viewport workflows** returned zero:

- `test_nurbs_native.bevel_vertex_freehand_box_workflow`: Tab, E in the initial
  vertex mode, mouse movement, undo to the tagged line, redo to native geometry,
  second extrusion, face-mode entry, Ctrl+B, confirm, undo and redo.
- `test_nurbs_native.bevel_freehand_box_workflow`.
- `test_nurbs_native.bevel_face_launch_ctrl_workflow`.
- The four existing `test_nurbs_line` workflows: line-to-sheet, face-region
  selection gestures, duplicate/repeated extrusion, and deletion/continued
  extrusion. These explicitly select the legacy backend they inspect and
  restore the keymap override afterward.

Each workflow ran in a separate process with temporary user configuration.
Runner: `/tmp/nurbs-vertex-extrusion-run-ui.py`. Exact commands, log paths and
return codes: `/tmp/nurbs-vertex-extrusion-ui-results.json`.

Five compatibility CTest cases passed in 12.81 seconds: `script_nurbs_line`,
`nurbs_composition_fixture`, `script_nurbs_composition`,
`nurbs_composition_saved_sources`, and `script_nurbs_legacy_compat`.
Log: `/tmp/nurbs-vertex-extrusion-release-ctest.log`.

Clang-format 20.1.8 dry-run, Python byte-compilation, and `git diff --check`
passed. No BKE geometry algorithm changed, so its previously validated kernel
suites were not rerun for this editor dispatch correction. Work remains local
and unpushed. Restart Blender to use the changed extrusion behavior.
