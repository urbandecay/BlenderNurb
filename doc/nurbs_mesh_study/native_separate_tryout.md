# Native NURBS Separate Selection (P)

P moves the selected source component closure into an independent Blender object.
The original remains active in Edit Mode with its remaining components deselected;
the new object is selected in Object Mode. This implements **Separate Selection**.
Mesh Separate's By Material/By Loose Parts options and popup-menu parity remain
future work. Y disconnects components within one object; P changes object ownership.

## Shared architecture and mesh reference

The behavior references are `mesh_separate_selected` / `mesh_separate_tagged` in
`editmesh_tools.cc`, BMesh duplicate/delete (`DEL_FACES`), and the existing
`curve.separate` object-duplication path in `editcurve.cc`.

The geometry work reuses Y's BKE `extract_components` service. The new BKE
`extract_edit_selection` prepares the remainder and extracted edit snapshots,
clears remainder selection and remaps the complete extracted closure's selection,
active component and history. `geometry_from_edit_snapshot` validates and copies
an edit snapshot into a persistent source, including its selection. Tagged source
entry and undo transitions also reuse that serialization service.

ED preflights every unique edited source, any exact tagged source entry, both
geometry results and persistent extracted payloads before scene IDs change.
Split and Separate share the same owner preparation adapter. The editor calls
Blender's existing object/data duplication service and common source commit; it
does not reconstruct curve/surface geometry. The new data owner retains object
transforms, collections, material slots and existing Blender copy semantics.
No public native component RNA API is added; `bpy.ops.curve.separate()` invokes
the same operation as P. The native context menu exposes Separate Selection.

Each side owns its curves, supports, trims, knots and coefficient buffers.
Geometry remains exact: no fitting, tessellated-source substitution, tolerance
increase or coordinate welding. Incidence needed by unselected geometry stays
in the remainder, and shared identities within the extracted region stay shared.
An object split deliberately creates independent boundary identities between
owners. Multiple instances of the same edited Curve data produce one fragment,
matching Blender's unique-data edit convention.

## Try it

Binary: `/home/molotovgirl/Desktop/build_linux/bin/blender`.

1. Add a NURBS Line, enter Edit Mode and press 2 for Edge mode.
2. E, Y, 2, Enter twice creates two connected NURBS faces.
3. Press 3, deselect all, and select one face.
4. Press P. The face moves into a separate object in the same location.
5. Tab to Object Mode, select the new object and Tab back in. G, E and Shift+D
   act on its independent native source. The other face remains in the original.
6. Undo/redo P restores/removes both the object and the source topology.

Ready file: `/tmp/blender-nurbs-separate-ui/separate-FACE-after.blend`.
It contains both source objects immediately after P. The corresponding
`separate-FACE-continued.blend` shows independent movement, extrusion and cap
duplication. All source splines collections are empty; native NURBS is authoritative.

## Validation

Implemented and built: the Blender application and the native edit/selection
C++ test executables. The build succeeds. Two warnings in unchanged
`view3d_select.cc` concern unused variables; no production compilation error.

Viewport event tests pass for vertex, edge, face, box-selected face, a connected
two-face region and tagged Add/E source entry. Each covers real P, object/mode
state, undo/redo, saved source, linked selection confined to the new owner,
transform cancellation/movement, continued E and Shift+D. The same six layouts
are also separated using the actual mesh operator and saved for comparison.

A second viewport function passes multiple unique edit owners, linked data,
nonuniform/negative object scales, location, object/data material slots and
modifier preflight rejection without partial promotion or new objects/data IDs.
A third passes continued extrusion and undo/redo on **both** saved objects in a
fresh process. Existing tagged and multi-owner Y workflows also pass.

Visually inspected: `/tmp/blender-nurbs-separate-ui/separate-FACE.png` and
`separate-EDGE.png`, showing the original geometry unchanged while separated
geometry is independently moved/extruded/duplicated. This visual claim applies
to these two captures. Other cases are automated event/source checks.

Four new C++ tests cover:

- independent source ownership, source-state retention, V/E/F selection mapping,
  persisted active/history, remainder deselection and continued exact movement;
- exact rational circle/seam and hole-bearing face source preservation;
- empty selection, invalid references and exhausted identity allocation without
  source changes;
- complete saved NURBS records (coefficients, knots, topology, trims, tolerances,
  orientation/material and selection) matching expected exact extraction, and
  both results' vertex positions/edge incidence/face boundaries matching actual
  mesh Separate on all six viewport layouts.

Final suite totals and exact command results are recorded in the appended
checkout-audit checkpoint.

## Commands and logs

Production build and install (exit 0):

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_selection_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

After adding the saved-source/mesh comparison, rebuild its test executable:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_edit_test --parallel 4
```

For each full test ID below, run this command with that ID replacing `TEST_ID`.
Run the selection workflow before fresh continuation and the C++ saved-file test.
All use independent headless Blender processes; no user's running instance closes.

```sh
nice -n 10 env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_SEPARATE_OUTPUT=/tmp/blender-nurbs-separate-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

```text
test_nurbs_native.separate_selection_workflow
test_nurbs_native.separate_owners_and_preflight
test_nurbs_native.separate_fresh_continuation
test_nurbs_native.split_tagged_workflow
test_nurbs_native.split_across_objects
```

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test --gtest_color=no
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup --python /tmp/blender-nurbs-separate-bezier-check.py
python3 -m py_compile tests/python/ui_simulate/test_nurbs_native.py scripts/startup/bl_ui/space_view3d.py
git diff --check
```

The Bezier check creates an ordinary Bezier curve, separates its selected spline
and verifies that the old route creates an independent Bezier object while the
original remains in Edit Mode. It prints `LEGACY_BEZIER_SEPARATE_PASS`.

Logs are `/tmp/blender-nurbs-separate-` plus `build.log`, `install.log`,
`test-build-fixed.log`, `kernel.log`, `parity-ui.log`, `owners-final-ui.log`,
`fresh-ui.log`, `edit-full.log`, `selection-full.log`, `ctest.log`,
`split_tagged_workflow-regression.log`, `split_across_objects-regression.log`
and `bezier-check.log`.

An initial test-only compile failed because the mesh offset-index collection is
not a range iterator; the fixture now iterates its face indices. An early test
attempt ran while the executable was still being linked and returned permission
denied; it passed after linking completed. The owner test initially assumed
standard Blender empty-object creation and parenting RNA. This fork's existing
RNA omits parenting and requires object data, so that test setup was removed;
parenting is **not** claimed as tested. Neither issue required changing unrelated
Blender code. Final commands/results supersede those initial attempts.

## Remaining work

By Material/By Loose Parts separation, V Rip, M Merge/weld/dissolve and real Ctrl+R
cuts remain planned. Partial curved movement still follows the source transform
service's explicit geometry restrictions; separation itself preserves its source
exactly. Automatic source entry remains limited to supported tagged prototype
geometry; unsupported old-file attributes/modifiers reject explicitly. Ordinary
untagged Curve/Bezier Separate retains its existing behavior.

Unused curve/support resources remain in the source remainder under the current
shared extraction/deletion contract. Repeated operations can grow saved payloads;
resource collection is separate work. Visibility and complete native attribute/
Python component domains remain unfinished.
