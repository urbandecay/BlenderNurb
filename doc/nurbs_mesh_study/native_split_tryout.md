# Native NURBS Split Selection (Y)

Y disconnects selected source components while retaining their exact NURBS
shape. It is distinct from P/Separate (a new object), V/Rip (an interactive
tear), and cutting an edge or face into additional selectable elements.

## Mesh reference and shared architecture

The behavior reference is `edbm_split_exec` in `editmesh_tools.cc`,
`bmo_split_exec` in `bmesh/operators/bmo_dupe.cc`, and the `DEL_FACES` deletion
context in `bmesh_delete.cc`. Blender duplicates the selected closure, removes
the originals that unselected incidence no longer needs, then selects the copy.
An unselected endpoint copied as a wire dependency is not implicitly deleted.

`extract_components` in BKE produces a complete independent fragment, a source
remainder, a removal plan and identity maps. `split_components` reunites the two
results in the same owner. This is shared topology work now used by [Separate Selection](native_separate_tryout.md);
the editor does not reconstruct curves or supports. Existing closure-copy,
incidence, deletion, source commit and undo services remain the shared path.

Copies preserve coefficient positions, rational weights, knots, degrees,
parameter ranges, tolerances, surface orientation/material, loop orientation,
holes and per-use p-curves. New identities are allocated deterministically, with
sharing preserved within each side of the split. Coincident coordinates never
cause welding. No fitting or numerical tolerance increase occurs.

`selection_remap_closure` selects all copied dependencies and remaps existing
active/history entries. Duplicate now uses that same service. This handles a
selected wire whose copied endpoint was not explicitly selected in Face mode.
The full source candidate is validated before the editor commits it.

The existing `curve.split` operator, Y binding and native context menu route to
these services. Ordinary tagged Add sources use prepared exact source entry;
the split is finished on the temporary edit source before replacing any live
owner. All owners, including linked-data modifier users, are checked first.
There is no separate legacy topology-splitting algorithm. Untagged legacy Curve
and Bezier editing retains the existing operator path.

## Validation

Implemented and built: `blender`, `blenkernel_nurbs_selection_test` and
`blenkernel_nurbs_edit_test`.

* **40/40 selection C++ tests pass**, including 861 comparisons against actual
  BMesh split across all seven component-mode combinations. The fixtures cover
  sheets, adjacent/non-manifold faces, wire junctions and loose points. Copied
  provenance verifies surviving identities, endpoints, face connectivity and
  selection rather than only comparing counts.
* **56/56 edit C++ tests pass**, with no skips. Three new tests verify component
  mode commits/history, failure atomicity, and the complete saved source records
  from five viewport cases. These checks compare exact vertices, curves,
  supports, knots, trims, tolerances, identities and selection.
* Exact rational circle/seam and hole extraction checks preserve coefficients,
  knots and face-use mappings while owning independent buffers.
* The new viewport workflow exercises vertex, edge and face mode, box selection,
  a connected two-face region, Y undo/redo, G cancellation/movement, Ctrl+L,
  continued E and save/reopen. A fresh process continues E and Shift+D from the
  saved split face. Separate viewport functions cover ordinary Add/E/Y source
  entry, multiple edited objects and modifier rejection before any owner is
  promoted.
* Five selected CTest targets pass, including the existing 39 NURBS Python
  cases. The unchanged Bezier split route also passes a direct application check.
* All five new viewport functions pass: nine cases across five isolated Blender
  processes. The existing mesh/NURBS corner-snap/extrude workflow and native
  Rotate/Scale lifecycle workflow also pass (three further cases).

Visually inspected: the face- and edge-mode post-split/continued-extrusion
screenshots in `/tmp/blender-nurbs-split-ui/split-FACE.png` and `split-EDGE.png`.
This visual claim applies to those two captures; rational seam/hole coverage is
from the kernel checks. No user's running Blender instance was closed.

Exact primary build/install commands:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_selection_test blenkernel_nurbs_edit_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test --gtest_color=no
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_selection_test --parallel 4
env BLENDER_NURBS_SPLIT_OUTPUT=/tmp/blender-nurbs-split-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test --gtest_filter='*Split*' --gtest_color=no
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

For each viewport function below, run this exact command with its full test ID
in place of `TEST_ID`. Run the primary workflow before the fresh continuation.

```sh
nice -n 10 env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_SPLIT_OUTPUT=/tmp/blender-nurbs-split-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

```text
test_nurbs_native.split_selection_workflow
test_nurbs_native.split_tagged_workflow
test_nurbs_native.split_across_objects
test_nurbs_native.split_fresh_continuation
test_nurbs_native.split_preflight_rejects_without_partial_promotion
```

The full edit suite uses the same artifact environment as the previous transform
checkpoint, adding `BLENDER_NURBS_SPLIT_OUTPUT`. The exact environment and command
are recorded in the appended checkout-audit checkpoint.

Logs use `/tmp/blender-nurbs-split-` followed by:
`build-reviewed.log`, `selection-build-final.log`, `install.log`, `selection-full-final.log`, `edit-full.log`,
`edit-tests.log`, `ctest.log`, `ui.log`, `tagged-ui.log`, `multi-owner-ui.log`,
`fresh-ui.log`, `preflight-ui-fixed.log`, `snap-regression-ui.log` and
`transform-regression-ui.log`.

The first test build failed because a test fixture passed an array where an
explicit span was required; that was corrected. The first differential test
then exposed the unselected wire-endpoint behavior above. Both issues were
corrected before the passing build/tests. Two warnings in unchanged
`view3d_select.cc` concern unused variables; no compilation error remains.
That first differential failure also bypassed BMesh-operation cleanup at its
failed assertion and triggered the leak checker (exit 134). The reference test
now uses scope cleanup so an assertion cannot leak the active BMesh operation.
The first preflight test expected a Python exception; this operator reports
through the window-manager report list and returns `CANCELLED`. The corrected
test checks that return value and verifies both owners retain all original
tagged source records and coordinates.

## Scope and remaining work

The native geometry operation preserves any structurally valid input without
changing its shape. Later movement still follows the existing transform
geometry restrictions; this split does not add arbitrary curved deformation.
Automatic source entry remains restricted to the supported tagged prototype,
with unsupported old-file attributes/modifiers rejected explicitly.

Unused geometry resources remain in the remainder, matching the current
deletion-service contract; resource collection is separate work. This can grow
saved payloads after repeated splits. Separate Selection now uses this foundation. Rip, Separate by Material/Loose Parts,
new edge/face partitions, Ctrl+R, visibility storage and full Python component
bindings remain unfinished.
`bpy.ops.curve.split()` uses the same native operation as Y.

For a ready source example, open `/tmp/blender-nurbs-split-ui/split-FACE-after.blend`
in `/home/molotovgirl/Desktop/build_linux/bin/blender`. The selected NURBS face
has been detached and moved upward by 0.5; the adjacent face stays at its original
position. Enter Edit Mode and continue extruding or moving it.
