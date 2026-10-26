# Cube top merge into one shared apex

Follow-up: [successive merges](layered_merge_fix.md) extends this correction to
merging a previously merged box, and records the corrected BMesh Collapse
endpoint-incidence weighting at junctions. The shared mean arithmetic below
remains in use; Center averages unique vertices and Collapse averages selected
edge endpoint incidences.

User report: build a cube by extruding a NURBS line into a face, then extruding
that face. Select the top and use M → At Center or Collapse. The top should
become one shared tip, as with a mesh.

## Reproduction and cause

The previously tested single-sheet merge did not cover contracting a cube's
four top corners. A new centered box fixture failed in the kernel and in the
actual Blender M menu. The cube retained 8 vertices, 12 edges and 6 faces after
the failed merge. An uncentered box happened to work. The failure was:

```text
Boundary needs a nonplanar or tolerance-managed construction; no fitting was applied
```

There were two related problems:

* Incremental averaging added a small numerical offset to the center of an
  integer-coordinate square. Blender mesh Merge computes the selected-vertex
  sum before dividing. The offset could make the existing exact plane-chart
  reconstruction reject a side triangle.
* Even after correcting the center, a sheared box could fail: the old projected
  plane chart required exactly representable slope/intercept arithmetic. A
  perfectly valid triangle need not meet that restriction.

The mesh references are `merge_target()` and the Collapse branch of
`edbm_merge_exec()` in `source/blender/editors/mesh/editmesh_tools.cc`. Kernel
regressions run the actual BMesh `pointmerge` and `collapse` operators and compare
all vertices, edges and face boundaries with the NURBS result.

The initial kernel reproduction command exited 1 with that error:

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test \
  --gtest_filter='NurbsTopologyEditTest.MergeExtrudedBoxTopIntoOneSharedApex' --gtest_color=no
```

## Shared kernel correction

`merge_selected_vertices()` now uses a compensated sum followed by division.
It falls back to dividing before summation if the ordinary sum overflows,
preserving the existing ability to merge very large finite coordinates.
Center and Collapse share this center calculation. Collapse still groups
separate selected edge islands individually, as BMesh does; it has not been
replaced with a global Center merge.

The reusable `fill_components()` service now has an affine triangle chart
fallback. It keeps the original three shared vertices, 3D edge curves, edge
identities and parameterizations. Its NURBS support uses the three triangle
corners and a fourth affine coefficient `b + c - a`; straight parameter curves
bound the triangular source region. This is source construction, without a
display-mesh replacement or a fitted boundary.

The existing exact planar chart remains the first choice. The affine fallback
reports a conservative coefficient construction bound. Its default API still
requires zero construction error. Merge explicitly supplies its existing error
budget, incorporates the returned bound in its result and face/use tolerances,
and rejects the independent candidate if the combined budget is exceeded. The
editor already reports any nonzero construction bound. This is arithmetic
roundoff accounting, not a certificate for floating-point surface evaluation.

The ordinary cube regression succeeds with a **zero error budget**. Both
Center and Collapse produce **5 vertices, 8 edges and 5 faces**, with one shared
apex, four side triangles, the bottom face, and two opposing uses per edge.
Arbitrary curved trim collapse remains subject to the existing geometry rules.
This change does not add generic partial deformation of trimmed faces.

## Build and kernel commands

All commands below ran from the Blender checkout. Build/install exited 0:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_topology_edit_test blenkernel_nurbs_fill_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux \
  --prefix /home/molotovgirl/Desktop/build_linux/bin
cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_topology_edit_test --parallel 4

env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui \
  BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 \
  BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 \
  BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_fill_test --gtest_color=no
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

Results: **25/25 topology tests**, **18/18 fill tests**, no skips; **5/5 CTest
targets**. The new checks cover V/E/F, Center/Collapse, sheared support
construction, actual BMesh topology, disjoint Collapse islands, overflowing
center sums, strict error budgets and all eight saved viewport source files.
The saved-file checks read authoritative NURBS geometry, compare it with BMesh,
check component modes and tessellate the stored source.

Logs: `/tmp/blender-nurbs-apex-build.log`,
`/tmp/blender-nurbs-apex-reviewed-build.log`,
`/tmp/blender-nurbs-apex-install.log`,
`/tmp/blender-nurbs-apex-final-tests-build.log`,
`/tmp/blender-nurbs-apex-topology-tests.log`,
`/tmp/blender-nurbs-apex-fill-tests.log`, and
`/tmp/blender-nurbs-apex-compat-tests.log`.

## Actual viewport tests

Each ID ran in its own Blender process, two processes at a time:

```sh
env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

```text
test_nurbs_native.merge_cube_top_workflow
test_nurbs_native.merge_cube_top_collapse_workflow
test_nurbs_native.merge_cube_top_face_workflow
test_nurbs_native.merge_cube_top_edge_workflow
test_nurbs_native.merge_cube_top_tagged_workflow
test_nurbs_native.merge_cube_top_uncentered_workflow
test_nurbs_native.merge_cube_top_box_workflow
test_nurbs_native.merge_cube_top_click_workflow
```

**8/8 pass**, exit 0. They build the cube with actual Add/E/G keys, invoke the
real M popup, check the pyramid, undo/redo, save, leave/re-enter Edit Mode and
continue extruding from the tip. They assert which merge mode the popup actually
invoked. The fresh tagged case also checks undo restores
its original source ownership. Its initial harness check incorrectly expected
native component statistics before source promotion; the corrected check reads
the actual legacy splines/selection before Merge and uses native statistics
afterward. No application change was needed for that harness failure.

Each UI log is `/tmp/blender-nurbs-apex-TEST_FUNCTION.log`. Before/after PNGs
and `.blend` files are in `/tmp/blender-nurbs-apex-ui`. The centered reproduction
failure is `/tmp/blender-nurbs-apex-repro-centered-ui.log`; the uncentered
baseline passed and is recorded in `/tmp/blender-nurbs-apex-repro-ui.log`.
The cube with its top selected, ready for manual M testing, is
`/tmp/blender-nurbs-apex-ui/apex-CENTER-VERT-False-True-CAP-before.blend`.

The cube and resulting pyramid screenshots were visually inspected. This is
separate from automated event execution and saved-source assertions; no change
was made to the user's running Blender instance. The rebuilt executable is
`/home/molotovgirl/Desktop/build_linux/bin/blender`. An already running process
must be restarted to load it.

Clang-format, Python compilation and `git diff --check` passed. The correction
is local and has not been committed or pushed. The user-owned handoff is intact.
