# Current checkout audit

Baseline: `355a9ae11d7e39d75f1341322a70776fb206a6bc`,
“Improve topology-aware NURBS extrusion and selection.”
Working branch: `nurbs/shared-topology-foundation`.
The handoff was the only untracked file on arrival. No other local edits existed.

Latest completed checkpoint: Blender and twelve targeted test executables build;
**204 C++ kernel/editor/integration tests**, **37 NURBS Python cases**, the unit
formatting/conversion regression, autosave and global-undo targets pass. Eleven
isolated viewport event regressions pass after the documented test corrections.
Native **E** and **Delete** now use shared kernel candidates and the common owner
commit. The reported extrude/delete/continue sequence passes on native source,
alongside the repaired tagged prototype. Twenty-two new saved states are checked
against full expected source records. Line extrusion, multi-face boundary walls,
trimmed rational sweeps, cancellation, undo/redo and fresh-file continuation are
verified. A mixed-session entry rejection leak found by the new test is fixed.
Automatic face-normal constraint remains a gate; use supported axis constraints
or Global/Local Move. Native primitive creation/migration remains gated.
Native Shift+D uses exact closure copying, fresh shared identities, remapped
selection/history and the existing Move adapter. Eleven saved duplication states
are checked against actual source geometry, alongside six selection and twenty
movement files. Native source-vertex **G** uses original double source snapshots, shared incident geometry and atomic owner commits.
Click and box selection produce identical stored geometry. Cancellation, undo,
redo, actual F9 field editing, multi-axis input, unit conversion and sub-float
source coordinates have saved-file checks. Twenty saved movement states are
validated against actual NURBS source, including weights, knots, IDs and untouched
coordinates. The F9 fields and the warped native sheet have been visually inspected.

The tagged prototype's duplicate/extrude/delete repairs remain active and tested.
The movement checkpoint verified recovery of four redundant remnants from the
original broken-file reproduction and successful extrusion. This checkpoint
repeats the delete/continue viewport regression; the frozen input is unchanged.
Native L / Shift+L / Ctrl+L now share source connectivity and selection
services, with multi-owner preflight, retained active/history data and explicit
hole-policy handling. The ambiguous hole rule still awaits user input; L picking
requires a source vertex and X-Ray. Fit-point editing,
edge/face picking, R/S, snapping, proportional editing and multi-owner movement remain later gates.
Normal/Gimbal movement is explicitly rejected because its source orientation
adapter is absent. Native active-vertex centers now read the live source ID rather than legacy curve
points. Cursor-to-active, Active Element G/cancel and unchanged saved source are
tested. Edge/face active centers remain part of their future picking/mode gates.
Stage 1 remains incomplete.

See the checkpoints at the end for exact commands, failures and limits,
[the movement tryout](native_movement_tryout.md),
[selection tryout](native_selection_tryout.md),
[native duplication tryout](native_duplication_tryout.md),
[native extrusion/deletion tryout](native_extrusion_tryout.md),
[duplicate/extrude tryout](prototype_duplicate_extrude_tryout.md), and
[delete/continue tryout](prototype_deletion_tryout.md).
The user's deletion sequence is reproduced; their exact working file was not supplied.
Historical checkpoint sections retain the results at their original source state.

`/home/molotovgirl/Desktop/build_blender_5.2.2` is a symlink to `build_linux`;
these are two names for the same build, not independent comparison binaries.

## Evidence labels

* **Source**: inspected in this checkout.
* **Automated**: command actually executed; result recorded below.
* **Interactive**: visually inspected Blender UI, separate from event simulation.
* **Proposed**: intended architecture or behavior.
* **Unknown**: not established by this audit.

## Baseline capabilities

This table records the original baseline commit. Later checkpoint fixes below
supersede its duplicate-ID, shared-edge extrusion, retained-cap and weight-sync
failures; the table is retained as the original evidence, not current status.

| Capability | Evidence and limit |
|---|---|
| New line to sheet; endpoint to connected line | Source and 23-test Python suite pass. Degree-one line and bilinear patch construction use real `Nurb` source data. |
| Persistent shared endpoint/edge IDs | Source. IDs survive ordinary DNA serialization. Python tests exercise continued extrusion after reload; they primarily assert coordinates, not all identity invariants. |
| Shared point movement and linked traversal | Source and targeted Python suite pass for the supported two-point/2-by-2 fixtures. Transforms synchronize copies; linked traversal is implemented separately in ED. No general shared curved edge owner. |
| Rational weights at shared corners | Source: position sync copies all four `BPoint::vec` values, including the rational weight. A rational weight belongs to a curve/surface representation, not to a shared topology vertex. This synchronization cannot be generalized to arbitrary incident supports. |
| Face-region caps and outer walls | Source and baseline diagnostic. Internal selected edge IDs suppress walls. Original faces always remain; repeated planar face extrusion gives 6 then 11 patches, versus 6 then 10 mesh faces. The second NURBS extrusion retains an internal cap. |
| Click versus box selection | Source paths differ. Click calls `BKE_curve_nurbs_topology_selection_set`; box changes projected BPoint flags. Transform syncs selected ID copies, extrusion does not normalize before classifying patch-local selection. Exact reported user sequence remains unknown. |
| Shared edge extrusion | Source and baseline diagnostic: one selected patch copy gives 2→3 patches; both copies of the same edge give 2→4. Selected uses are appended per patch without deduplicating edge IDs. |
| Duplicate inside the same object | Source and baseline diagnostic: `BKE_nurb_copy` retains IDs; `curve.duplicate` has no topology remap. Duplicating a line then translating its duplicate moves both copies to Y=1. Independent source identity is not preserved. |
| Independent V/E/F modes, fit-point default | Absent in source. Existing selection uses CV flags as proxy topology selection. |
| General curved shared geometry, p-curves, trims, arbitrary face loops | Absent in prototype. Four IDs on each patch are not a BRep kernel. |
| Shape keys | Source rejects topology extrusion. Other geometry mutations, shape keys and attribute propagation have not been made generally topology-safe. |
| Precision | Source uses float BPoints, knots, transform locations and display data. No double source or certified numerical tolerance contract. |
| Evaluation | Source NURBS are authoritative for current prototype. `evaluate_surface_object` builds float display lists and derived mesh. This does not provide general trim-aware evaluation or picking. |
| Face orientation/solid validity | Source: cap duplication keeps the original support orientation; no general oriented shell validator exists. Python area checks do not establish outward normals, manifoldness, or a valid closed solid. |
| Visual verification | None claimed. Automated event simulation runs a real Blender window but is not human visual confirmation. |

The original study describes an older baseline and its “no implementation” status
is stale. Its geometry analysis remains research input. The inventory and parity
map each have 358 rows with matching ordered operator IDs; these are proposed
contracts, not 358 verified algorithms. Existing categories count 114 support
preserving, 49 exact construction, 39 numerical, 155 mesh-specific alternatives,
and one unrelated sequencer operation. Those counts are not effort estimates or
the four handoff classifications. In particular, generic BMesh helper rows need
their callers' geometry contracts, not a mechanical category translation.

Static registration counts were rechecked in this source: 148 direct mesh
registrations, 16 mesh macros, 49 direct UV registrations, and 83 BMesh
definitions match the inventory. The operator IDs align between the two CSVs.
The new [four-class workflow map](operator_workflow_classes.csv) covers all
358 rows with conditions and explicitly proposed status; it does not assert
that every operator/property combination has been exercised or ported.

## Exact baseline commands and results

```sh
git rev-parse HEAD
git status --short
git switch -c nurbs/shared-topology-foundation
cmake --build /home/molotovgirl/Desktop/build_linux --target blender --parallel 8
```

Build: **passed**, exit 0. Existing build cache points at this source directory;
binary identifies hash `355a9ae11d7e`. Log: `/tmp/blender-nurbs-baseline-build.log`.

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender \
  --background --factory-startup --python-exit-code 1 \
  --python tests/python/bl_nurbs_line.py
```

**Passed: 23 tests**, exit 0. Log: `/tmp/blender-nurbs-baseline-python.log`.

```sh
python3 tests/python/ui_simulate/run.py \
  --blender /home/molotovgirl/Desktop/build_linux/bin/blender \
  --tests test_nurbs_line.line_to_sheet
```

**Failed before Blender launch**: test module imports `bpy` at module scope;
the external runner imports all test modules using system Python.

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender \
  --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_line.line_to_sheet
```

**Failed**, exit 1, at the final second-extrusion assertion: expects
`point_count_v == 3`, actual `2`. This test predates separate-patch extrusion;
earlier E/transform/undo assertions reached that point successfully. Log:
`/tmp/blender-nurbs-baseline-ui-direct.log`.

This is a stale test failure, not reproduction of the reported box-select case.
That report needs gesture-level fixtures and comparison with matching mesh
geometry. No screenshot or exact original fixture is present in the handoff.
Do not label the source-path difference a proven explanation of that report.
The connected Blender addon-status check also could not reach a running addon;
no live scene was read or modified through that connector.

## Additional baseline diagnostics

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender \
  --background --factory-startup --python-exit-code 1 \
  --python /tmp/blender_nurbs_baseline_diagnostics.py
```

Exit 0; log `/tmp/blender-nurbs-baseline-diagnostics.log`. The diagnostic script
creates fresh fixtures and prints results; it is not a passing acceptance suite:

* Fully selected legacy `surface.primitive_nurbs_surface_surface_add`:
  `curve.extrude` returns `FINISHED` but stays 4-by-4 with 16 points. A following
  transform moves existing geometry. This reproduces the symptom for that
  source, but does not establish which source the user originally selected.
* A shared edge selected on one versus both patch copies gives 3 versus 4 total
  patches from the same two-patch source.
* Duplicate-line translation moves both original and duplicate.
* Matching planar mesh/NURBS region fixtures have face counts 6→10 and 6→11.

The script is preserved as [`baseline_diagnostics.py`](baseline_diagnostics.py)
for subsequent runs. The `/tmp` path in the command above is the exact path used
for the original baseline run; use the preserved repository path to repeat it.

## Gesture coverage added during the audit

The stale UI test now checks connected separate patches and imports `bpy` only
inside its Blender function, allowing the external runner to discover tests.
`face_region_selection_gestures` creates one- and two-face planar sources,
selects all visible corners by actual box drag, Shift-click and linked-hover
events, and invokes `E`, axis constraint, numeric input and confirmation.
It compares the complete resulting source control nets for all three methods.
The check passed on the unchanged baseline extrusion code; it does not resolve
the shared-edge-only or untagged legacy-surface limitations above.

Run UI tests without opening windows on the user's desktop. The bundled Weston
launcher splits unquoted paths in its generated INI, so use a temporary symlink
whose name has no spaces to the bundled dependency directory:

```sh
# /tmp/blender-nurbs-deps-storage points to this checkout's lib/linux_x64.
env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py \
  --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_line.face_region_selection_gestures
```

**Passed**, exit 0. Log `/tmp/blender-nurbs-selection-headless.log`. This is
automated UI evidence, not visual confirmation. The first attempted gesture
test exposed a test-generator yield error; corrected to use the existing
`cursor_motion` helper before the passing run. Its stuck desktop test process
was closed; the user's pre-existing Blender process was left running.

## Next architecture work

See [architecture decision](architecture_decision.md). Prototype deficiencies are
acceptance cases for the shared foundation; no production extrusion patch is
part of the storage experiment.

## Storage experiment build and tests

The experimental owner and kernel services are implemented in
`DNA_nurbs_types.h`, `BKE_nurbs_geometry.hh`, and `intern/nurbs_geometry.cc`;
`Curve` currently owns an optional payload with deep-copy and native blend-file
lifecycle handling. This payload is not routed into creation, drawing,
evaluation, selection, transforms, or editing. It does not replace legacy
`Nurb` data.

Blender itself built successfully:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender --parallel 4
```

The initial storage build identifies hash `355a9ae11d7e`, built 2026-10-23
02:45:38. Standalone test linking initially failed with unresolved references
between existing Blender static libraries. The same failure occurred for the
pre-existing `blenkernel_lib_id_test`. A manual ELF group proved the archive
cycle was the cause. This workaround has now been replaced with Blender's
existing CMake mechanism. Four scans of the cyclic static libraries fixed
these two targets; the later edit-state target required five. The current
standalone kernel-test setting requests five instead of the default three.
Both targets built normally at this checkpoint:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_geometry_test blenkernel_lib_id_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_lib_id_test
```

Build **passed**, exit 0; log `/tmp/blender-nurbs-standard-test-build.log`.
The existing ID suite passed all **19 tests**. The initial NURBS suite passed
all **9 tests**, including deep-copy ownership, ID copy,
native file save/load, memfile save/load, shared edge references, holes,
non-manifold incidence and 64 edges meeting at a vertex. The persistence test
initially crashed because the test had not installed its `Main` as Blender's
global main; the fixture now uses `BKE_blender_globals_main_replace` and clears
it after the test.

The rebuilt Blender Python suite also passed:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup \
  --python-exit-code 1 --python tests/python/bl_nurbs_line.py
```

**Passed: 23 tests**, exit 0. Both UI event-simulation tests passed on the
rebuilt executable:

```sh
env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_line.line_to_sheet

env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_line.face_region_selection_gestures
```

They compare events and resulting geometry. No screen image was inspected, so
there is no visual confirmation. Production selection, transform and extrusion
code remains unchanged; the known baseline extrusion and duplicate failures
are not fixed by this storage experiment. Logs:
`/tmp/blender-nurbs-ui-line-final.log` and
`/tmp/blender-nurbs-ui-face-final.log`.

## Rational source evaluation proof

`intern/nurbs_geometry_evaluate.cc` adds scalar double-precision curve and tensor
surface evaluation directly from stored coefficients, rational weights and full
knot vectors. It preserves the parameter domain and never fits or mutates the
source. Invalid storage, out-of-domain parameters, computed denominator zeros
and unrepresentable arithmetic return explicit errors. Signed-weight evaluation
reports denominator conditioning; this diagnostic is not a geometric error bound
or a certificate that the rational denominator is regular over the whole domain.

The recurrence is the same B-spline basis construction used by Blender's modern
curve evaluation, adapted to the explicit double source and failure contract.
Common weight scaling and recentered, compensated summation avoid needless
overflow and loss of small details far from the origin.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_geometry_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test
```

Build **passed**, exit 0; log `/tmp/blender-nurbs-evaluation-build.log`.
The expanded NURBS suite **passed all 16 tests**, exit 0. New cases include
explicit rational circles/arcs, comparison with an independent rational cubic
Bernstein formula, repeated knots and unclamped endpoints, weight-scale
invariance, signed-weight poles, large coordinates, tiny parameter intervals,
and a rational cylindrical support. A cylinder and cone fixture share one
canonical curved edge but have different surface domains and per-face p-curves;
all seven edges match their incident supports at the 33 tested parameters.
These are sampled measurements on known constructions, not general geometric
validation or trimmed tessellation. The planar hole remains a structural fixture.

This adds kernel evaluation only. Viewport evaluation, picking, fit-point
solving and edit undo remain unimplemented for the new payload. No stage is
declared complete by these tests. The user has clarified fit-point editing only;
the optional CV editing mode has been removed from the architecture proposal.

## Tagged prototype import and component-copy foundation

`intern/nurbs_geometry_legacy.cc` builds an independent geometry candidate from
tagged non-cyclic two-point lines and two-by-two patches. It preserves vertex,
edge and face IDs, copies the source coefficients/weights/full knots exactly
from float to double, and assigns fresh IDs above all legacy IDs to resources,
loops and edge uses. Shared vertices require matching positions; coordinates do
not create identity. Face material indices are retained.

Shared degree-one boundaries can have different knot domains and non-proportional
rational weights. Per-face rational p-curves use a projective parameter change
constructed from cross-products of the source weights. No surface is fitted or
altered to match its neighbor. The seven import tests cover this correspondence,
reversed boundaries, same-sign negative weights, independent coincident wires,
and atomic rejection of missing/conflicting/duplicate IDs and unsupported input.

This is a geometry-only candidate, not owner activation. Selection, fit-point
constraints, per-point radius/tilt/softbody metadata, visibility and other legacy
attributes still need an explicit migration contract. The original `Nurb` data
is untouched. The adapter rejects mixed-sign/zero weights until denominator-domain
validation is available, and rejects duplicate stored wire identities instead of
silently merging them. Higher-degree, periodic and untagged legacy shapes remain
future adapters; unsupported input fails as a whole.

`intern/nurbs_geometry_copy.cc` adds a reusable component-closure copy service.
Faces bring their loops, shared edges, vertices, supports and p-curves; selected
edges do not automatically copy incident faces. Sharing within the copy is
preserved, owned arrays are independent, and every copied record receives a fresh
ID with a complete source-to-copy map. Source order makes this deterministic
regardless of selection order or repeated IDs. Invalid selections and ID-counter
exhaustion fail before returning a candidate. The source and its counter remain
unchanged; a future editing transaction must commit the new fragment and counter
together. This has not been routed into the legacy Duplicate operator.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_geometry_test blenkernel_nurbs_geometry_legacy_test \
  --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
```

Build **passed**, exit 0; log `/tmp/blender-nurbs-component-copy-build.log`.
The geometry suite **passed 21 tests** and the import suite **passed 7 tests**,
both exit 0. Tests include fresh IDs, multi-face shared closure, holes, isolated
vertices, independent storage, exact copied rational coefficients and deterministic
maps. There is no new interactive/visual evidence or native edit-undo evidence
from these kernel tests. Stage 1 remains incomplete.

## Atomic kernel edit state

`BKE_nurbs_edit.hh` and `intern/nurbs_edit.cc` add an owned edit state and
independent snapshots containing geometry, topology selection, active/history
state and the identity counter. Candidate commits reject stale revisions,
invalid structure and invalid selection references before changing the current
state. Restore keeps runtime revisions increasing while restoring the saved
identity counter, so derived caches cannot mistake an old snapshot for current
data. This validates structure and references, not geometric agreement or
denominator regularity. Fit handles remain a separate geometry contract.

```sh
cmake -S '/home/molotovgirl/Desktop/Blender Fork' \
  -B /home/molotovgirl/Desktop/build_linux
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
```

Configure and build **passed**, exit 0. Build log:
`/tmp/blender-nurbs-edit-state-build.log`. The edit-state suite passed **5 tests**,
geometry **21**, import **7**, all exit 0: **33 kernel tests** at this checkpoint.
The initial new test compile lacked `BLI_listbase.h`; this was fixed. Its initial
link still failed with four archive scans; five resolved it using the same
existing CMake mechanism. These snapshots alone are not native Blender edit undo.

Native owner/edit-data and Curve undo-callback adapters have since been added
for an internal experiment. They are not activated by production mode entry.
Their rebuild and new integration tests are pending; no new native-undo or UI
success is claimed yet. The original extrusion and duplicate failures remain.

The first native-adapter build stopped with exit 2 because `editcurve.cc`
lacked the logging include and local `LOG` declaration used by the new flush
branch. Those declarations were added; unrelated formatter changes in the
prototype extrusion code were removed. Retry log:
`/tmp/blender-nurbs-native-edit-build-retry.log`; first attempt:
`/tmp/blender-nurbs-native-edit-build.log`.

The new native integration test translation unit compiled independently with
exit 0 while the main rebuild continued:

```sh
nice -n 19 make -C /home/molotovgirl/Desktop/build_linux \
  -f source/blender/blenkernel/CMakeFiles/blenkernel_nurbs_edit_test.dir/build.make \
  source/blender/blenkernel/CMakeFiles/blenkernel_nurbs_edit_test.dir/intern/nurbs_edit_test.cc.o
```

Log `/tmp/blender-nurbs-native-edit-test-object.log`. This is object compilation,
not a linked or passing native integration test.

An additional old-file compatibility test now reads the existing
`render/mesh/texture_space_nurbs.blend` and `alembic/single-curve.blend` fixtures.
The previous successful Blender binary confirmed their one-spline NURBS sources
(8-by-2 and four-point, both order four) and first coefficients. The new test
checks those values and null payload/edit pointers after actual older-file
loading. Its object compiled with exit 0; runtime validation is pending. Run
the rebuilt geometry suite with
`--test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'`.
Compilation log: `/tmp/blender-nurbs-legacy-file-test-object.log`; baseline
inspection log: `/tmp/blender-nurbs-legacy-file-reference.log`.

## Additional BMesh region reference

While the native-edit rebuild ran, the last successfully built executable was
used for a geometry-only BMesh reference probe. It identifies version 5.2.2 LTS,
hash `355a9ae11d7e`, build timestamp 2026-10-23 03:34:44 UTC. This does not test
the pending native adapters or a NURBS implementation.

```sh
nice -n 19 /home/molotovgirl/Desktop/build_linux/bin/blender \
  --background --factory-startup --python-exit-code 1 \
  --python doc/nurbs_mesh_study/mesh_region_reference.py
```

Exit **0**; log `/tmp/blender-nurbs-mesh-region-reference.log`. The diagnostic
prints these actual BMesh counts; it is not an assertion-based NURBS suite:

| Fixture | Result faces | New cap faces | New walls | Selected originals retained |
|---|---:|---:|---:|---:|
| Eight-quad planar ring, first extrusion | 32 | 8 | 16 | 8 |
| Same ring cap, second extrusion | 48 | 8 | 16 | 0 |
| Three faces share one edge, select one | 7 | 1 | 4 | 0 |
| Same fan, select two | 9 | 2 | 6 | 0 |
| Same fan, select all three | 15 | 3 | 9 | 3 |

The ring has twelve exterior and four hole-boundary edges; both receive walls.
`bmo_dupe.cc` classifies an edge with fewer than two selected incident faces as
an extrusion boundary. `bmo_extrude.cc` uses a region-wide original-removal flag
when an input edge has an unselected incident face (unless keeping originals
was requested). Counting only total incidence or treating every edge with an
unselected neighbor as a wall boundary would not reproduce these results.

## Native edit-data checkpoint: built and tested

The complete native-adapter build passed, exit 0:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
```

Log `/tmp/blender-nurbs-native-edit-build-retry.log`. The edit suite passed
**8 tests** and import suite **7**, exit 0, with these commands:

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
```

Logs `/tmp/blender-nurbs-native-edit-tests.log` and
`/tmp/blender-nurbs-native-legacy-tests.log`. The native tests exercise owned
edit entry, independent ID flush, free, conflict rejection and real Curve
undo encode/decode callbacks. Geometry, selection, active/history, ID counter
and monotonic runtime revision are checked. The test invokes callbacks
directly; it does not exercise generic undo-stack ID resolution, memfile
transitions or viewport Ctrl-Z for the new source.

The first older-file test crashed with exit **139** while loading old window
layouts without editor space types registered by kernel test setup. GDB traced
it to `do_version_area_change_space_to_space_action`, whose space-type lookup
was unavailable in that fixture. The kernel test now uses Blender's existing
data-only temporary library reader for the Curve ID. Full old-file loading is
checked separately in the actual Blender application.

The next run exposed an incorrect test expectation: sixteen surface points
had been assumed to mean 4-by-4. Independent parsing of the file's own SDNA
with `tools/modules/blendfile.py` confirmed **8-by-2**. Both test expectations
were corrected. The other Desktop build name was checked and is a symlink to
the same build; it is not independent baseline evidence. Failed logs remain:
`/tmp/blender-nurbs-native-geometry-tests.log`,
`/tmp/blender-nurbs-legacy-file-gdb.log`,
`/tmp/blender-nurbs-native-geometry-tests-fixed.log`,
`/tmp/blender-nurbs-native-legacy-file-app-tests.log`.

Final correction build and geometry run passed, exit 0:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_geometry_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
```

The geometry suite passed **22 tests**. Logs:
`/tmp/blender-nurbs-legacy-file-test-final-build.log` and
`/tmp/blender-nurbs-native-geometry-tests-final.log`. Total: **37 kernel tests**.

Both application suites passed:

```sh
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/blender \
  --background --factory-startup --python-exit-code 1 \
  --python tests/python/bl_nurbs_line.py
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/blender \
  --background --factory-startup --python-exit-code 1 \
  --python tests/python/bl_nurbs_legacy_compat.py
```

**23** existing prototype tests and **2** full-application older-file tests
passed, exit 0. Logs `/tmp/blender-nurbs-native-python-tests.log` and
`/tmp/blender-nurbs-native-legacy-file-app-tests-fixed.log`. CTest discovery
lists both `script_nurbs_line` and `script_nurbs_legacy_compat`; the older-file
script is registered only when the standard test assets are available.

The two headless event-simulation commands recorded above were rerun with
`nice -n 10`; both passed, exit 0. Logs
`/tmp/blender-nurbs-native-ui-line.log` and
`/tmp/blender-nurbs-native-ui-face.log`. These exercise the existing line and
one-/two-face gesture paths, not viewport editing of the new payload. No screen
image was inspected. Native selection persistence, generic undo-stack/memfile
transitions, viewport evaluation/picking and transforms remain promotion gates.
This checkpoint does not fix the diagnosed production duplicate/extrusion bugs
or complete Stage 1.

## Generic stack and persistent selection checkpoint

The next build and generic-stack run passed, exit 0:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
```

Logs `/tmp/blender-nurbs-undo-stack-build.log` and
`/tmp/blender-nurbs-undo-stack-tests.log`. Nine tests passed at that checkpoint.
The new test creates a minimal real window-manager runtime, registers Blender's
undo types, encodes a real memfile root, and pushes/restores adjacent Curve
edit steps through the generic stack. ID references are stored and resolved
by Blender. It never fakes the memfile-written flag. This does not decode across
different memfile generations or exercise GUI mode changes.

Selection persistence then added `NurbsStoredSelection`/typed element records
to the experimental payload and factored the common validator into
`BKE_nurbs_selection.hh` / `intern/nurbs_selection.cc`. Sets serialize in
deterministic domain/ID order; history keeps its order. Stored references,
active state, duplicate entries and owned array sizes are checked. Copy/free
and native read/write own the arrays. Edit data clears its copied stored state
and keeps one live selection authority. A candidate containing both is rejected.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^script_nurbs_(line|legacy_compat)$'
```

All commands passed, exit 0. Build log
`/tmp/blender-nurbs-selection-persistence-build.log`; suites passed **12 + 22 + 7
= 41 kernel tests**. Logs `/tmp/blender-nurbs-selection-edit-tests.log`,
`/tmp/blender-nurbs-selection-geometry-tests.log`,
`/tmp/blender-nurbs-selection-legacy-tests.log`. Native file/memfile and ID-copy
tests now also cover selected V/E/F identities, active/history, independent
arrays and internal edit re-entry. This strengthens the existing tests rather
than adding a new public editing path.

CTest passed both registered Python suites (**23 + 2 tests**); log
`/tmp/blender-nurbs-selection-python-ctest.log`. Both headless gesture commands
were repeated and passed, exit 0; logs
`/tmp/blender-nurbs-selection-ui-line.log` and
`/tmp/blender-nurbs-selection-ui-face.log`. Formatting and `git diff --check`
also passed. No new visual confirmation, native viewport/picking/transform
integration or production extrusion fix is claimed by this checkpoint.


## Checked edit-data flush checkpoint (2026-10-23)

Implemented shared failure propagation rather than an extrusion-specific fix:

* `ED_curve_editnurb_load` returns success. Native source conflicts are reported;
  no owner replacement or edit-data free occurs on failure.
* Generic edit load distinguishes no data from failure. Mode exit preserves
  live edit data on failure. Multi-object exit preflights native sources before
  freeing any object, and the Edit Mode toggle cancels a rejected exit.
* Checked editor flush results distinguish having edits from flushing them
  successfully. Compatibility bool wrappers retain their old `has_edits` meaning.
  A global preflight also checks native Curve owners without Object users.
* Save, startup/recovery/autosave, render, scene-copy, STL/PLY/Alembic/USD export
  and memfile encoding check the result before consuming the ID cache. Save
  callback pairs and autosave retry bookkeeping remain in place. The generic
  undo stack stops when its prerequisite memfile cannot encode. An already
  trimmed redo branch is not restored by this change.
* Structural validation borrows the current live source rather than making a
  deep snapshot just to validate it. This does not certify trim geometry,
  solid validity or numerical regularity.

Commands actually executed:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender --parallel 4
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|blendfile_autosave|global_undo)$'
```

The combined build exited 0; log `/tmp/blender-nurbs-flush-build.log`.
A final Blender target refresh completed successfully in
`/tmp/blender-nurbs-flush-final-build.log`. The tool session for that refresh
closed across a user continuation; its exit code was not retained, but the log
ends with `Built target blender` and the following application runs succeeded.
Kernel tests **15 + 22 + 7 = 44**, all exit 0; logs
`/tmp/blender-nurbs-flush-{edit,geometry,legacy}-tests.log`.
New checks deliberately inject a conflicting legacy source and verify rejection
through Curve load, generic load/flush, mode exit, autosave, memfile encode and
prerequisite undo-root creation. They check geometry, selection, counter,
revision and dirty-flag preservation, successful retry/re-entry, and multi-object
exit without partial freeing. Rejected test cases intentionally print errors.

CTest **4/4 targets passed**, exit 0; log
`/tmp/blender-nurbs-flush-python-ctest.log`. The two NURBS targets contain
23 prototype and 2 older-file tests. Existing autosave and global-undo targets
exercise their normal application paths. The new native failure injection is
covered in C++; regular Save As, render and export failure branches are source
inspected, not separately exercised in the viewport.

Headless event simulation used this command:

```sh
nice -n 10 env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_line.line_to_sheet
```

Repeated with `test_nurbs_line.face_region_selection_gestures`,
`test_undo.view3d_mesh_edit_separate`, and
`test_undo.view3d_font_edit_mode_simple`. **All four passed**, exit 0;
logs `/tmp/blender-nurbs-flush-ui-{line,face,mesh-undo,font-undo}.log`.
No image was inspected. Native payload viewport editing is still not active;
production duplicate/extrusion algorithms and their diagnosed failures remain.


## Shared editor commit checkpoint (2026-10-23)

`ED_nurbs.hh` / `editors/curve/editnurbs.cc` adds the common editor transaction
boundary. It accepts a BKE candidate and expected revision, validates the owner,
then delegates atomic source replacement to the kernel. Only success marks the
Curve's edit/save dirty flag, Main's memfile flush flag, dependency-graph recalc
and a geometry notification. It uses the supplied Main for tagging and notifier
routing. Rejected stale revisions, invalid topology/selection, missing edit data
and conflicting legacy source leave all those states unchanged. Selection-only
candidates follow the same path. Invalidation is conservative; this is not a
per-domain cache performance design. The adapter deliberately does not push an
undo step; Blender operator/modal orchestration owns undo grouping.

The generic native undo-stack test now commits through this adapter, so its
initial real memfile root flushes the pending source/selection rather than
relying on a caller to set the dirty flags manually.

Executed the same combined build, three kernel test commands and four-target
CTest command recorded in the checked-flush checkpoint. **Build exit 0; kernel
18 + 22 + 7 = 47 passed, exit 0; CTest 4/4 passed, exit 0.** Logs:
`/tmp/blender-nurbs-editor-commit-build.log`,
`/tmp/blender-nurbs-editor-commit-{edit,geometry,legacy}-tests.log`, and
`/tmp/blender-nurbs-editor-commit-python-ctest.log`.

The same four viewport test commands, each in a separate Blender process,
passed again, exit 0; logs
`/tmp/blender-nurbs-editor-commit-ui-{line,face,mesh-undo,font-undo}.log`.
A prior attempt to pass face, mesh and font test IDs together in one
`run_blender_setup.py --tests` invocation failed, exit 1: simulated input
interfered, leaving the face test without an active object. Inspection shows
that runner schedules every `easy_keys.run()` timer without awaiting the
previous test. Separate-process runs pass; no modeling algorithm was changed to
address the invocation error. Failed log:
`/tmp/blender-nurbs-editor-commit-ui-regressions.log`.
No visual confirmation or native payload viewport workflow is claimed.


## Edit/object mode and memfile transition checkpoint (2026-10-23)

Added `UndoCrossesEditModeAndMemfileGenerations`: a minimal native WM, window,
workspace and empty screen provide real current-scene/view-layer context for
memfile encode/decode. No GHOST window or viewport is created. The test commits
source and selection through the shared editor adapter, encodes a real memfile
root, pushes before/after Curve edit steps, exits Edit Mode, then pushes two
object-mode memfiles including an object transform. Two complete undo/redo
cycles run with reusable IDs and again with a forced full memfile barrier.
After each decode the test reacquires Main and ID pointers; it checks source
coordinates, persistent ID counter, selection/active/history, mode and object
transform in the appropriate live or stored authority.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test \
  --gtest_filter=NurbsNativeEditTest.UndoCrossesEditModeAndMemfileGenerations
```

First build failed, exit 2, due to two fixture errors: the active-window field
had moved into WM runtime and `BKE_libblock_find_name` returns ID*, requiring
`id_cast`. Fixed the fixture (its window's `active` flag already identifies the
current window) and rebuilt with the same command: **exit 0**. Logs
`/tmp/blender-nurbs-memfile-transition-build.log` and
`/tmp/blender-nurbs-memfile-transition-build-fixed.log`.
Filtered test **passed**, exit 0;
`/tmp/blender-nurbs-memfile-transition-test.log`.

Reran the three complete kernel test commands and four-target CTest command
from the checked-flush checkpoint: **19 + 22 + 7 = 48 kernel tests passed**,
exit 0; **CTest 4/4 passed**, exit 0. Logs
`/tmp/blender-nurbs-memfile-transition-{edit,geometry,legacy}-tests.log` and
`/tmp/blender-nurbs-memfile-transition-python-ctest.log`.
Only test code changed in this checkpoint. The production binary still has the
four passing viewport tests from the preceding shared-commit checkpoint.

This proves valid source restoration through these native undo transitions;
it does not test arbitrary corrupted undo records or callback failure
propagation. Ordinary mode entry, viewport editing/picking, trimmed evaluation
and visual confirmation remain promotion gates. Stage 1 is incomplete.


## Shared ruled-edge construction checkpoint (2026-10-23)

Implemented `BKE_nurbs_construct.hh` / `intern/nurbs_construct.cc`. This is a
kernel construction returning a full owned candidate and identity maps, not a
replacement editor operator. It sweeps canonical edge IDs using
`S(u,v) = C(u) + v*d`, retaining each original edge and allocating a shared cap
vertex and rail per source vertex. Input order and repeated edge IDs do not
change IDs or construction. Multiple faces can share a branch rail. The original
curve basis, weights, knot vector and domain remain authoritative. A partial
edge interval trims the resulting support using p-curves without cropping or
refitting the curve. A closed clamped rational circle produces a cylindrical
support with one seam rail and two distinct p-curves. This is not a general
periodic-representation policy.

Original-face orientation is continued when its incident boundary uses agree;
wires and conflicting/non-manifold orientations use the sweep's parameter
orientation. No solid exterior, G1/G2 continuity or general shell validity is
inferred. Zero-offset construction is allowed as a singular editable state.
The construction currently requires positive rational weights. The authoritative
storage/evaluator are not narrowed by this construction restriction.

The caller supplies an explicit absolute translation error budget. Zero requires
exact coefficient additions. TwoSum residuals and an outward-rounded L1 bound
report accepted coefficient translation roundoff; positive weights make that a
bound on source translation, excluding pre-existing and evaluation error.
Unsupported floating-point modes (rounding/subnormal handling), non-finite
arithmetic, missing IDs, source endpoint discrepancies outside their declared
tolerances and exhausted identity space fail before any source commit. Accepted
pre-existing endpoint discrepancy is measured and reported separately.
Tolerance metadata expands conservatively; no backend tolerance is substituted.
No fitting or coordinate welding occurs. The UI has no default tolerance policy
for this new service yet; callers must not discard its diagnostic results.

Added six construction tests: line ownership/shared original edge; canonical
multi-edge selection with a three-face shared branch; rational arc basis,
domain and p-curve agreement including a trimmed interval; repeated and singular
construction with oriented incidence; explicit rejection/roundoff budgets; and
a closed rational circle with distinct seam uses. Boundary agreement is sampled,
not a general numerical certificate. The existing native file/memfile fixture
now includes a constructed face alongside its original adjacent faces and hole.
A new editor integration check commits/restores constructed source, ID counter
and cap selection together, flushes and re-enters the owner.

Exact combined build command:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
```

All four invocations exited 0; logs:
`/tmp/blender-nurbs-ruled-construction-build.log`,
`/tmp/blender-nurbs-ruled-construction-final-build.log`,
`/tmp/blender-nurbs-ruled-construction-precision-build.log`, and
`/tmp/blender-nurbs-ruled-construction-persistence-build.log`.
The first filtered run
`blenkernel_nurbs_geometry_test --gtest_filter='nurbs_geometry.*Ruled*'`
passed five initial tests, exit 0, before the seam/extended persistence checks.
Log `/tmp/blender-nurbs-ruled-construction-first-tests.log`.

The subsequent full geometry run failed, exit 1: its existing persistence
assertion used the last face as the original neighbor. The fixture now contains
a third, constructed face, so that positional assumption was stale. It now
looks up the two original faces by their persistent IDs. Failed log
`/tmp/blender-nurbs-ruled-construction-geometry-tests.log`.

Reran the three full kernel commands and four-target CTest command recorded
above: **20 + 28 + 7 = 55 kernel tests passed**, all exit 0; **CTest 4/4 passed**,
exit 0. Logs `/tmp/blender-nurbs-ruled-construction-{edit,geometry,legacy}-tests-final.log`
and `/tmp/blender-nurbs-ruled-construction-python-ctest-final.log`.
The same four isolated viewport commands passed, exit 0; logs
`/tmp/blender-nurbs-ruled-construction-ui-{line,face,mesh-undo,font-undo}.log`.
They exercise the existing visible prototype and ordinary mesh/text paths;
the new source constructor is not exposed in the viewport. No image was inspected.

The cross-mode/memfile fixture was then strengthened to start from a sheet
produced by this constructor. Its test-only whole-source translation updates
3D curves and supports while leaving p-curves in parameter space. The actual
undo/memfile cycles now verify the constructed face, original/cap edge uses and
shared IDs, in addition to geometry, selection and counters.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
```

Build **exit 0**; all **20 edit tests passed**, exit 0. Logs
`/tmp/blender-nurbs-ruled-undo-build.log` and
`/tmp/blender-nurbs-ruled-undo-edit-tests.log`.

Stage 1 still needs production source evaluation/picking, a user-facing edit
path, transforms and migration before its gate can pass. Face-region extrusion
and the diagnosed production prototype failures remain unfixed. This checkpoint
adds the reusable construction needed by that pipeline without discarding or
silently replacing the current prototype.


## Complete-source affine transforms and arithmetic checkpoint (2026-10-23)

Added `BKE_nurbs_transform.hh` / `intern/nurbs_transform.cc`. A finite affine
`double4x4` transforms every 3D curve, tensor support and topology vertex once;
2D p-curves stay in surface parameters. All topology/resource IDs, incidence,
knots, orders, weights, selection storage and the persistent counter survive.
This is a whole-source geometry service, not partial curved deformation or a
Blender modal transform adapter. Positive 3D rational weights are required.
Singular scale is a legal editable state. Reflection retains face orientation
flags and loop traversal. Blender's separate object Apply Transform correction
option (`object_transform.cc`, `corrective_flip_normals`) remains an integration
choice, not an inferred solid exterior.

The service returns an explicit coefficient-roundoff bound relative to the
supplied matrix, excluding matrix-construction and evaluation error. The caller
provides an absolute error budget. Source tolerances scale by a conservative
linear norm bound and expand for rounding. Signed axis permutations/scales have
an exact norm bound. Other matrices use the tighter of an induced-norm and a
bounded Gram-matrix estimate, avoiding artificial tolerance growth under repeated
nearly rigid rotations. No fitting, degree change or mesh substitution occurs.

Shared private arithmetic in `intern/nurbs_math.hh` now serves construction,
transform and scalar evaluation mode checks. TwoSum and FMA residuals bound
stored-double arithmetic, with outward non-negative sums. Product least-bit
exponents detect possible residual underflow without rejecting exact subnormal
integer scaling. Scalar curve/surface evaluation also rejects unsupported
rounding/subnormal modes explicitly. A volatile probe prevents a compiler from
moving the subnormal check past a caller's mode restoration. The functions do
not silently change the caller's rounding or subnormal-control modes.

Added eight transform tests: source/selection/basis/parameter preservation;
rational shared-boundary evaluation; tolerance scaling and collapsed states;
atomic rejection and explicit rounding budgets; subnormal products; native x86
FTZ/DAZ rejection without changing those modes; exact GMP rational reference
checks for roundoff and norm witnesses; and repeated rotation without artificial
tolerance inflation. The GMP and native SSE checks are conditional on build
capabilities and both ran in this checkout. Reference witnesses are not a
geometric-validity certificate for arbitrary inputs.

The edit-state, owner, native undo and generic memfile-transition fixtures now
translate their source through this actual kernel service, replacing their
manual coordinate-changing test helper. The cross-mode fixture includes a sheet
created by the shared ruled constructor, so its support geometry, p-curves,
shared IDs, selection and counter traverse the same complete source path.

Executed the same combined build command and three kernel test commands recorded
in the preceding checkpoint. The initial build failed, exit 2, because
`nurbs_transform.cc` lacked `BLI_listbase.h` for range iteration. Added it.
All subsequent builds exited 0. Logs:

* `/tmp/blender-nurbs-source-transform-build.log` (initial failure)
* `/tmp/blender-nurbs-source-transform-build-fixed.log`
* `/tmp/blender-nurbs-source-transform-integration-build.log`
* `/tmp/blender-nurbs-source-transform-final-build.log`
* `/tmp/blender-nurbs-source-transform-evaluation-build.log`
* `/tmp/blender-nurbs-source-transform-subnormal-build.log`
* `/tmp/blender-nurbs-source-transform-rotation-build.log` (latest combined build)

The rational-reference test was added with a test-only build:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_geometry_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
```

Both exited 0; logs
`/tmp/blender-nurbs-source-transform-rational-reference-{build,tests}.log`.
Later combined runs include this check and the rotation check.

Latest full kernel run: **20 + 36 + 7 = 63 passed**, all exit 0; logs
`/tmp/blender-nurbs-source-transform-{edit,geometry,legacy}-tests-rotation.log`.
Earlier intermediate passing runs (before the final added tests) are recorded
under `/tmp/blender-nurbs-source-transform-*-tests-final.log` and
`/tmp/blender-nurbs-source-transform-*-tests-evaluation.log`.

The four-target CTest command passed again, **4/4**, exit 0;
`/tmp/blender-nurbs-source-transform-python-ctest-rotation.log`.
The same four isolated viewport commands passed again, all exit 0;
`/tmp/blender-nurbs-source-transform-ui-{line,face,mesh-undo,font-undo}-final.log`.
These are regression checks for the existing visible prototype and mesh/text
workflows. No image was inspected. The new source services remain unexposed in
ordinary viewport editing; the reported prototype extrusion failures are still
pending the shared selection/transform/evaluation integration.


## Retained source snapshots checkpoint (2026-10-23)

`EditData` now publishes immutable geometry/selection/revision states.
`read_snapshot()` retains one complete state under a short mutex, then readers
can evaluate that owned state independently of later edits or owner destruction.
`copy_for_edit()` creates the independent mutable candidate. Commits validate
outside the lock, recheck the expected state, publish once and release the
previous state outside the lock. Existing borrowed accessors remain serialized
editor-thread access; they are not a background lifetime guarantee.

Kernel and editor commit overloads accept an owning read token. The token also
checks the specific runtime state, so an old result cannot be applied to a
recreated edit source merely because both lifetimes have revision 1. Existing
numeric-revision callers remain valid only within the same serialized edit
lifetime. Undo restoration publishes a new state and keeps the revision
monotonic within that lifetime while restoring the captured source ID counter.

Added four checks: a retained state remains coherent after commits and owner
free; a token rejects a recreated same-revision source; concurrent acquisition
keeps geometry, selection, counter and revision together; and editor token
rejection preserves owner dirty/recalc state across free/re-entry. This is
functional concurrency coverage, not ThreadSanitizer verification or proof of
complete depsgraph lifetime handling.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test \
  --gtest_filter=nurbs_edit_state.ConcurrentReadSnapshotsKeepGeometryCounterSelectionAndRevisionCoherent \
  --gtest_repeat=100
```

Build **exit 0**; **24 + 36 + 7 = 67 kernel tests passed**, all exit 0.
The concurrent-read check passed **100 repetitions**, exit 0. Logs:
`/tmp/blender-nurbs-read-snapshot-build.log`,
`/tmp/blender-nurbs-read-snapshot-{edit,geometry,legacy}-tests.log`, and
`/tmp/blender-nurbs-read-snapshot-concurrency-repeat.log`.
The four-target CTest command passed **4/4**, exit 0;
`/tmp/blender-nurbs-read-snapshot-python-ctest.log`.
The same four isolated viewport commands passed, all exit 0;
`/tmp/blender-nurbs-read-snapshot-ui-{line,face,mesh-undo,font-undo}.log`.
No image was inspected. Native source evaluation/drawing and picking remain to
be connected. The snapshot protects retained contents, not acquisition through
an already freed edit container or stale object/scene input context.

The next evaluation design is recorded in
[`evaluation_and_snapshots.md`](evaluation_and_snapshots.md). It distinguishes
source ownership, render provenance, host/COW handoff, approximate preview and
source picking. Blender's CDT can merge inputs, so its indices cannot become
source identities. No preview-driven welding or fitting is authorized.

The user also confirmed interest in Python access. The planned bindings must
invoke these same C++ services for operators and structured editing, including
fit points, source topology, selection and atomic updates. There is no separate
Python geometry implementation. New public bindings are not implemented yet;
existing Python regression tests do not establish native payload API coverage.

## Shared boundary evaluation checkpoint (2026-10-23)

Implemented `BKE_nurbs_evaluate.hh` / `intern/nurbs_evaluate.cc` and registered
them in the kernel target. It is a pure derived-output service, not an activated
viewport path. Canonical edge samples include the union of incident p-curve knot
spans. Endpoint display sharing uses source vertex IDs; coincident identities
remain separate. Every face use retains its own UVs and source face/loop/use IDs,
including holes and distinct seam uses. Results own values without source
pointers. A retained live-state fixture evaluates after commits and owner free.

Sampled endpoint and edge/support disagreement is checked against source
tolerances plus an explicit caller allowance. Accepted measurements and weight
conditioning are returned. Rejection reports source IDs, parameters and measured
versus allowed discrepancy. This does not certify agreement between samples or
the preview's approximation error. Source coefficients, tolerances, topology,
selection and ID counters are never modified. Full-multiplicity discontinuities,
nonpositive weights, unrepresentable subdivisions, invalid arithmetic and output
budget exhaustion are explicit initial restrictions. Boundary sampling does not
establish UV trim validity, triangulation, normals, picking or COW integration.
Further design and promotion gates are in `evaluation_and_snapshots.md`.

Added seven tests and strengthened the existing circle-seam and FTZ/DAZ checks.
Fixtures cover shared curved boundaries with different parameterizations, holes,
coincident vertices, wires, non-manifold/collapsed states, p-curve knot union,
accepted/rejected disagreement, restricted discontinuous intervals, tiny
parameter intervals and source lifetime.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --gtest_filter='nurbs_geometry.BoundaryPreview*:nurbs_geometry.ClosedRationalRuledConstructionSharesOneSeamRailWithDistinctPCurves:nurbs_geometry.SourceTransformRejectsFlushToZeroAndDenormalsAsZeroModes'
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|blendfile_autosave|global_undo)$'
```

Three full target builds exited **0**:
`/tmp/blender-nurbs-boundary-evaluation-{build,build-final,diagnostics-build}.log`.
The first focused run exited **1** (8/9 passed): its geometry-test executable
was linked before a late discontinuity guard was rebuilt into the archive for
the following target. It therefore rejected that case through endpoint agreement
instead of the new continuity check. Source/object/executable timestamps and the
build log confirm the inconsistent checkpoint. Rebuilt all four targets from
unchanged source; focused rerun **9/9 passed**, exit **0**:
`/tmp/blender-nurbs-boundary-evaluation-focused-tests{,-final}.log`.
The final diagnostic-message change was built afterward and all suites rerun.

Final kernel results **24 + 43 + 7 = 74 passed**, all exit **0**:
`/tmp/blender-nurbs-boundary-evaluation-{edit,geometry,legacy}-tests-final.log`.
The CTest command passed **4/4**, exit **0**:
`/tmp/blender-nurbs-boundary-evaluation-python-ctest-tests-final.log`.
The existing 23+2 Python NURBS tests exercise prototype and legacy behavior;
they do not call the new native evaluator or prove Python bindings exist.

The two isolated viewport tests also exited **0**, on the preceding completed
build before the diagnostic-message-only change. No viewport adapter changed.

```sh
nice -n 10 env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_line.line_to_sheet
# Separate Blender process, same command prefix:
# --tests test_nurbs_line.face_region_selection_gestures
```

Logs: `/tmp/blender-nurbs-boundary-evaluation-ui-{line,face}.log`.
No image inspected; no visual confirmation. New native drawing/selection/Python
adapters remain unimplemented; original diagnosed production failures remain.

## Evaluated source handoff checkpoint (2026-10-23)

`Curve::nurbs_eval_source` now has an owned, non-serialized evaluation wrapper.
The dependency graph captures live Edit Mode snapshots at both full copy and
edit-pointer refresh. Object mode copies geometry/stored selection; evaluated
copies retain the state token. Failed native captures preserve an error rather
than returning an older payload. Writable-owner validation rejects evaluated
leases before inspecting borrowed edit pointers. Four integration tests were
added and native/older-file persistence checks strengthened. The standard four-target build passed, exit 0:
`/tmp/blender-nurbs-evaluated-source-build.log`.

An early compile-only check of `nurbs_edit_test.cc` found an implicit C-array to
Span conversion error (exit 1); explicit span construction fixed it (exit 0).
Logs: `/tmp/blender-nurbs-evaluated-source-test-syntax-{flags,fixed}.log`.
The first attempt looked for a compilation database, found none and skipped;
the actual check parsed this target's generated `flags.make` and invoked its
GCC 14 compiler with `-fsyntax-only`. This is not a linked test or runtime pass.

While that broad Curve-layout rebuild ran, independent preview/display
implementations were prepared without changing its target registration:

* `BKE_nurbs_tessellate.hh` / `nurbs_tessellate.cc` and nine tests: filled
  sampled UV preview, holes, shared boundaries, seams, provenance rejection and
  explicit limits. Original source UVs are retained for triangle corners.
* `BKE_nurbs_mesh.hh` / `nurbs_mesh.cc` and four tests: derived float Mesh,
  retained source token/provenance, per-face material indices, explicit float
  overflow and measured quantization, coincident identity and source lifetime.
* `nurbs_test_geometry.hh`: shared native source fixtures for these two suites.

These files were unregistered during the handoff build. After its checks passed,
they were registered and a new build started. Compile-only checks pass after one
missing test include was corrected. They are not part of this 78-test checkpoint.
Link/runtime checks are in progress; nothing has been drawn or visually confirmed.
Exact contract and remaining work are in `evaluation_and_snapshots.md`.


The initial edit suite run passed **27/28**, exit **1**. Its fast-path sentinel
showed a full ID recopy: the harness called raw `DEG_evaluate_on_refresh` without
clearing prior recalc tags as `scene_graph_update_tagged` does. Adding
`DEG_ids_clear_recalc(graph, false)` after each refresh fixed the harness and
proved both full-copy and fast edit-pointer refresh paths. Production code was
unchanged by this correction.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_edit_test --parallel 4
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
```

Corrected test-target build **exit 0**; **28/28 edit tests passed**, exit 0.
Logs: `/tmp/blender-nurbs-evaluated-source-test-build-fixed.log` and
`/tmp/blender-nurbs-evaluated-source-edit-tests{,-fixed}.log`.
The same geometry/assets and legacy commands from the preceding checkpoint
passed **43** and **7**, both exit 0:
`/tmp/blender-nurbs-evaluated-source-{geometry,legacy}-tests.log`.
Total **78 kernel tests passed**. The same four-target CTest command passed
**4/4**, exit 0: `/tmp/blender-nurbs-evaluated-source-python-ctest-tests.log`.
Four isolated viewport commands (same env/runner prefix above) passed, all exit 0:
`test_nurbs_line.line_to_sheet`,
`test_nurbs_line.face_region_selection_gestures`,
`test_undo.view3d_font_edit_mode_simple`,
`test_undo.view3d_mesh_edit_separate`.
Logs: `/tmp/blender-nurbs-evaluated-source-ui-{line,face,font-undo,mesh-undo}.log`.
These remain prototype/regression UI checks; native drawing is not connected
and no image was inspected.


## Filled surface preview and display-Mesh checkpoint (2026-10-23)

Registered `nurbs_tessellate.cc` and `nurbs_mesh.cc` as reusable BKE services,
with nine surface-preview and four display-Mesh tests. Shared source boundaries
retain one sample sequence across faces and seams. Sampled outer/inner loops
produce a derived filled preview; concave regions and holes are checked.
CDT output provenance rejects merged/moved/generated UV vertices or split/lost
boundary intervals. Original support UVs remain per triangle corner. Nested,
touching or intersecting sampled holes and open UV charts fail explicitly.
This validates sampled polygons, not exact curved trims between samples.

The display adapter retains its source read token and preview beside an owned
no-Main Mesh. Float overflow is rejected; float quantization is measured and
reported without rounding the source. Source face material indices transfer to
the display mesh. It does not infer texture UVs or source identities from render
triangles/diagonals. Flat triangle shading is a temporary restriction; analytic
normals and picking remain unimplemented. Native object evaluation was not yet
connected at this checkpoint.

```sh
cmake -S '/home/molotovgirl/Desktop/Blender Fork' -B /home/molotovgirl/Desktop/build_linux
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_tessellate_test blenkernel_nurbs_mesh_test blender \
  blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test
```

Configure and full six-target build **exit 0**:
`/tmp/blender-nurbs-surface-preview-{configure,build}.log`.
The first surface suite passed **8/9**, exit **1**. Its merge fixture assumed
that a large epsilon would force near-site merging in an otherwise unchanged
initial triangulation. That did not happen. Replacing it with an actual duplicate
UV corner exercises the real input-site merge and confirms explicit rejection
without changing source identities. The implementation was unchanged.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_tessellate_test --parallel 4
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test
```

Corrected target build **exit 0**, surface suite **9/9 passed**, exit **0**.
Logs: `/tmp/blender-nurbs-surface-preview-test-build-fixed.log` and
`/tmp/blender-nurbs-surface-preview-tessellate-tests{,-fixed}.log`.
Display-Mesh suite **4/4 passed**, exit **0**:
`/tmp/blender-nurbs-surface-preview-mesh-tests.log`.
The previous edit/geometry/assets/legacy commands passed **28 + 43 + 7**,
all exit 0: `/tmp/blender-nurbs-surface-preview-{edit,geometry,legacy}-tests.log`.
Total **91 kernel tests passed**. The same four-target CTest command passed
**4/4**, exit 0: `/tmp/blender-nurbs-surface-preview-python-ctest-tests.log`.
No viewport adapter changed in this checkpoint; the four viewport regressions
from the preceding handoff checkpoint remain the last UI verification.
No image inspected; no native visual confirmation.

Source review prompted by the merge fixture also corrected our earlier notes:
`BLI_delaunay_2d.hh` documents a zero-to-1e-8 epsilon substitution, but the actual
current implementation passes epsilon unchanged (`delaunay_calc`, `CDT_state`).
`find_site_merges` merges exactly coincident sites independently of epsilon;
constraint insertion uses epsilon for snapping/intersection handling. Zero is
not a no-merge guarantee. The public-header statement must not be repeated as
verified current behavior. This is a source observation, not a separate runtime
zero-epsilon experiment.

## Native object evaluation checkpoint (2026-10-23)

Added source dispatch in `BKE_displist_make_curveTypes`, an immutable per-object
runtime cache with the retained source token/provenance/diagnostic, and original
object `to_mesh()` capture of live native edit geometry. Native output goes to a
derived GeometrySet Mesh; it never replaces the Curve source. Missing/invalid
capture or unsupported modifiers/legacy offset/bevel settings produce an empty
evaluated result with an explicit diagnostic, without legacy/stale fallback.
Initial uniform resolution uses the maximum U/V setting, agreement allowance is
explicitly zero, and shading remains flat. Render override and overlay/picking
contracts need further checks; no public native creation path is activated.

The first build passed, but all four new object tests failed: the legacy Curve
stores neutral offset as `1.0`, whereas the initial guard assumed zero. The
guard now checks the actual stored default. The corrected full build passed,
and all four object integration tests passed. They exercise full dependency
graph evaluation, refresh after live edit commits, independent retained previous
frames, original-object `to_mesh()` capture, explicit unsupported/error states,
and object runtime copy/cache lifetime. No public source-editing API is implied.

Executed (exit 0):

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_object_test blender blenkernel_nurbs_edit_test \
  blenkernel_nurbs_geometry_test blenkernel_nurbs_geometry_legacy_test \
  blenkernel_nurbs_tessellate_test blenkernel_nurbs_mesh_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test
```

Build log: `/tmp/blender-nurbs-object-evaluation-build-fixed.log`;
object tests: `/tmp/blender-nurbs-object-evaluation-object-tests-fixed.log`.
Reran the other five suites: **28 + 43 + 7 + 9 + 4**, all exit 0;
with the four object tests, **95 kernel tests passed**. Logs are
`/tmp/blender-nurbs-object-evaluation-{edit,geometry,legacy,tessellate,mesh}-tests-final.log`.
The same four-target CTest and four separate-process viewport event commands
recorded above passed again, exit 0. Logs:
`/tmp/blender-nurbs-object-evaluation-python-ctest-tests-final.log` and
`/tmp/blender-nurbs-object-evaluation-ui-{line,face,mesh-undo,font-undo}.log`.

An optional native C++ fixture export and an app-only smoke script exercise real
serialized source without adding a public factory or generating editable meshes.
Executed (exit 0):

```sh
nice -n 10 env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test \
  --gtest_filter=NurbsObjectEvaluationTest.DependencyGraphDisplaysLiveSheetAndRetainsPreviousFrameSource
nice -n 10 env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_PREVIEW_IMAGE=/tmp/blender-nurbs-native-preview.png \
  /home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup \
  --python-exit-code 1 --python doc/nurbs_mesh_study/native_preview_smoke.py
nice -n 10 env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_PREVIEW_IMAGE=/tmp/blender-nurbs-native-preview.png \
  python3 tests/utils/blender_headless.py --factory-startup --python-exit-code 1 \
  --python doc/nurbs_mesh_study/native_preview_smoke.py
```

Logs: `/tmp/blender-nurbs-object-evaluation-{fixture-export,app-preview,viewport-preview}.log`.
The JSON reports 128 sheet triangles, 112 hole-face triangles (area 3), and 176
cylinder triangles; the maximum squared-radius float discrepancy is about
`6.32e-8`. All three source Curve IDs have zero legacy splines. Inspected the
actual 1200x700 OpenGL capture `/tmp/blender-nurbs-native-preview.png`: blue
sheet, orange face with a square hole, green cylinder. The cylinder has visibly
faceted diagonal shading. This is a native object-display check, not visual
proof of native Edit Mode, selection, extrusion or modal undo.

## Source normals and knot-span preview checkpoint (2026-10-23)

Added rational surface derivatives with explicit one-sided knot limits and
source-derived display corner normals. Preview interior samples now subdivide
each support knot span, so a four-span rational circle receives the intended
sampling density throughout the surface. Derivative, crease, extreme-scale and
degenerate-frame checks are built and tested. The first derivative suite passed
8/9: an exact-equality expectation for a rational derivative was too strict for
floating-point evaluation; changed it to a `1e-14` numerical assertion. The mesh
suite passed 7/7. The corrected full build passed, exit 0. Executed:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_differential_test blenkernel_nurbs_mesh_test \
  blenkernel_nurbs_tessellate_test blenkernel_nurbs_object_test blender \
  blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_differential_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|blendfile_autosave|global_undo)$'
```

All passed: **9 + 7 + 9 + 4 + 28 + 43 + 7 = 107 kernel tests**, four CTest
targets (including 25 NURBS Python cases). Build log:
`/tmp/blender-nurbs-surface-normals-build-final.log`; suite logs:
`/tmp/blender-nurbs-surface-normals-{mesh,tessellate,object,edit,geometry,legacy}-tests.log`,
`/tmp/blender-nurbs-surface-normals-differential-tests-final.log`;
CTest: `/tmp/blender-nurbs-surface-normals-python-ctest.log`.
The four viewport event regressions above last passed at the preceding 95-test
checkpoint; this stage reran the native object-display check below.

Repeated the preceding background and actual viewport smoke commands with
`BLENDER_NURBS_PREVIEW_IMAGE=/tmp/blender-nurbs-native-preview-smooth.png`,
both exit 0. Logs:
`/tmp/blender-nurbs-surface-normals-{app-preview,viewport-preview}.log`.
The script now checks custom normals survive dependency evaluation and Python
Mesh extraction, including radial cylinder normals (minimum dot product
`0.9999999362`). The cylinder preview now has 288 vertices / 512 triangles;
sheet and hole counts remain 128 / 112 triangles, hole area 3. Inspected the
new 1200x700 capture: smooth cylinder shading, visible sheet and hole. This is
still an approximate derived display; no approximation certificate, general
trim correctness or solid eligibility is claimed. Internal knot samples and
one-sided normals do not yet establish a general crease-constrained trim
triangulation contract. Collapsed source frames are counted and request
Blender's display-normal fallback; the source remains unchanged.

The fixture SHA-256 before and after is identical:
`7ce0ec39f1c60e4234ba2795efd4650115d658018ef4d57b6bba2b1abf69efbe`.
This work does not change source coefficients or add CV editing. Native Edit
Mode/picking/modal transforms and the production prototype failures remain open.

## Shared component selection normalization checkpoint (2026-10-23)

Added `selection_flush` in the existing BKE selection service, with all seven
V/E/F mode combinations. It reconciles bulk source ID sets downward and upward
without changing geometry, IDs or the input selection, and prunes deselected
active/history entries without inventing inferred history. Holes include all
loop boundaries; repeated seam uses select a shared edge once. No visibility
storage, mode conversion, gesture adapter or native Edit Mode activation is
provided by this service.

Code verification found an important distinction: BMesh's default selection
flush is upward only; individual mesh setters already propagate downward.
The new bulk service corresponds to `BM_mesh_select_mode_flush_ex` with the
explicit `Down` flag. Tests compare actual BMesh on all 512 selection masks of
a quad in seven modes, plus deterministic shared-face/isolated-vertex cases.
Native fixtures cover holes, rational seams, non-manifold incidence, history,
face storage order, atomic failure and retained-snapshot commits.

Initial compile-only checking found a missing ListBase iterator include and
untyped `first` member accesses in the tests; fixed both. The first full build
then found a test passing a C array where an explicit Span was needed. Fixed
that call. The corrected full build passed, exit 0. All nine new selection tests
passed, including **5,390 actual BMesh comparisons** (512 quad states and 258
shared-face states, each in seven modes), plus native-specific fixtures.
Logs: `/tmp/blender-nurbs-selection-flush-{configure,syntax,syntax-fixed,build,build-fixed}.log`.

Executed (all final commands exit 0):

```sh
cmake -S '/home/molotovgirl/Desktop/Blender Fork' -B /home/molotovgirl/Desktop/build_linux
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_selection_test blender blenkernel_nurbs_differential_test \
  blenkernel_nurbs_mesh_test blenkernel_nurbs_tessellate_test blenkernel_nurbs_object_test \
  blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test \
  blenkernel_nurbs_geometry_legacy_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_differential_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|blendfile_autosave|global_undo)$'
```

Results: **9 + 9 + 7 + 9 + 4 + 28 + 43 + 7 = 116 kernel tests passed**;
four application CTest targets passed, including 25 NURBS Python cases. Logs:
`/tmp/blender-nurbs-selection-flush-{selection,differential,mesh,tessellate,object,edit,geometry,legacy}-tests.log`
and `/tmp/blender-nurbs-selection-flush-python-ctest.log`.

Both NURBS viewport event regressions passed again, in separate processes:

```sh
nice -n 10 env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_line.line_to_sheet
nice -n 10 env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_line.face_region_selection_gestures
```

Logs: `/tmp/blender-nurbs-selection-flush-ui-{line,face}.log`. These exercise the
existing prototype, not native-source gesture routing. The native selection
service is verified independently; click/box/linked adapters remain open.
No additional native visual claim beyond the preceding source-normal capture.

User design input requested: in Vertex/Edge mode, should linked selection over
a trimmed face's boundary include its whole connected surface and hole loops,
or only the connected boundary-curve graph? BMesh has no directly corresponding
face-with-holes storage in this checkout. Face-island traversal and vertex/edge
graph traversal must not accidentally choose this NURBS-specific behavior.
Pending reply; no linked-selection default implemented.

## Native Edit Mode and topology-vertex viewport adapters

The following records intermediate findings; final results follow at the end.

Normal Curve/Surface mode entry now calls a checked native edit-source creation
path when a payload exists. It preserves one authority, reports invalid entry
before changing object modes, and reuses the current controller on repeated
entry. Legacy owners retain their prior path. A native Curve keymap context is
available, while legacy-only operator polls reject native owners explicitly.
Mixed native/legacy multi-object sessions are initially rejected by mode toggle.

Added one BKE source-vertex hit application service and a shared ED selection
commit adapter. Select All, click and box route native source IDs through that
path; no coefficients or display indices become editable handles. The initial
gesture adapter requires X-Ray and supports topology vertices. General fit-point
handles, edge/face picking modes, visible-only picking, lasso/circle and linked
selection are not implemented here. Linked selection's hole policy is still
pending user input. Modal transforms and legacy primitive insertion report their
current native-source limitation. Native RNA legacy spline mutations, shape-key
insertion and object Apply Transform are guarded against creating an alternate
source or silently changing the displayed geometry.

The native edit overlay draws source topology vertices and canonical sampled
boundaries. GPU buffers retain the exact immutable source state used to build
them, with state-token invalidation. Shared source components remain shared;
GPU duplication for per-edge colors is derived display data. Blender statistics
read actual source V/E/F counts and selection instead of empty legacy arrays.

Seven direct tests were added for normal entry/reentry/failure, vertex-hit actions,
gesture-equivalent component sets, stale results and clean no-ops. An optional
native file check will read the viewport test's saved click/box fixtures and
verify exact selected source IDs, unchanged coefficients/weights/knots and
incidence. The new viewport event test uses Tab, A/Alt-A, click, Shift-click, B,
Ctrl-Z/redo, exit/reentry and saved files; it also checks unsupported legacy
mutations are refused. No public native geometry creator or element API is added.

Compile-only checks for ten changed C++ files passed (logs
`/tmp/blender-nurbs-edit-ui-syntax-*.log`). The complete Blender/test build is
running: `/tmp/blender-nurbs-edit-ui-build.log`. No new runtime or visual result
is claimed until the build and fixture tests finish. The last completed
checkpoint remains 116 passing kernel tests.

Runtime findings during this integration:

* The initial complete build passed. The first normal-entry test then crashed
  in `WM_main_add_notifier`: the new fixture had not installed its Main as the
  global Main required by Blender's editor lifecycle. GDB confirmed that stack.
  Fixed the fixture using the same globals setup/cleanup as the existing native
  undo tests. The corrected edit suite passes 31 tests; its additional saved-UI
  fixture check is pending the viewport output and skips without those paths.
* The first viewport run reached native click selection, Ctrl-Z/redo, mode exit,
  saving and re-entry. It then exposed an actual numerical preview problem at
  resolution 13: identity p-curve interpolation changed a parameter by one
  rounding step, causing a measured `5.5511151231257827e-17` support discrepancy
  with declared allowance zero. The preview explicitly rejected it; no fitting,
  welding or source-tolerance change was used to make the test pass.
* Added exact evaluation of identity coordinates on degree-one p-curve spans
  with equal nonzero weights. Such a coordinate is mathematically the input
  parameter; retaining it avoids an unnecessary interpolation roundoff before
  support evaluation. This is restricted to parameter curves, preserving the
  common arithmetic used for 3D edges and their supports. Tests cover non-unit
  domains, multiple spans, common positive/negative weight scales and exclusion
  of unequal rational weights. A 13-subdivision preview regression passes with
  **zero allowance and zero measured discrepancy**, for both [-1,1] and [0,1]
  model coordinates. This is an exact special case, not a general evaluation
  error bound or a claim that every native source now displays at every setting.

Current targeted runtime results: geometry 44, tessellation 10, selection 13
and edit 31 pass; one saved-UI validation test awaits the viewport fixtures.
The corrected combined build is running in
`/tmp/blender-nurbs-edit-ui-build-fixed.log`. Initial logs:
`/tmp/blender-nurbs-edit-ui-edit-tests-initial.log`,
`/tmp/blender-nurbs-edit-ui-entry-crash-gdb.log`, and
`/tmp/blender-nurbs-edit-ui-native-viewport-test.log`. Full-suite, corrected
viewport, saved-source and visual checks remain pending at this point.

The corrected viewport run then passed (exit 0):
`/tmp/blender-nurbs-edit-ui-native-viewport-test-fixed.log`. It exercised native
source selection, undo/redo, save/re-entry, resolution 8 versus 13, and rejection
of unsupported legacy spline/primitive/transform paths. Inspected its actual
1920x1012 screenshot: four source corner markers, the bottom two selected, one
selected shared boundary edge, source statistics 2/4 vertices, 1/4 edges, 0/1
faces. Generated `click.blend`, `box.blend`, `try-native-selection.blend` and
`native-edit.png` under `/tmp/blender-nurbs-edit-ui/`.

The saved-source reader fixture initially crashed in
`BKE_blender_globals_main_swap` during low-level file reading; GDB confirmed a
missing global Main there too. Added the required global Main setup/cleanup to
that fixture. This is a reader-harness correction, with its runtime rerun still
pending. Logs: `/tmp/blender-nurbs-edit-ui-saved-source-tests.log` and
`/tmp/blender-nurbs-edit-ui-saved-source-crash-gdb.log`.

The screenshot also exposed old "Control Points" header menus for native data.
Added readonly `Curve.is_native_nurbs` owner metadata (no element bindings) and
native header/context selection menus using the existing supported operators.
Legacy Curve/Surface menus remain on their prior path. Select All extends the
selection so existing active/history state survives, as in Blender. A final
complete build is running in `/tmp/blender-nurbs-edit-ui-build-final.log` after
these changes. It must finish before the final viewport, saved-file and complete
regression reruns. The earlier four-target CTest and other geometry/mesh/object/
legacy suites passed; these final changes have not yet received the full rerun.

### Completed selection checkpoint: final build, installation and regression evidence

All final builds, installation and reruns below completed with exit 0. The
intermediate pending checks above are resolved. No test-harness crashes remain.

```sh
nice -n10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_edit_test blender blenkernel_nurbs_selection_test \
  blenkernel_nurbs_geometry_test blenkernel_nurbs_tessellate_test \
  blenkernel_nurbs_differential_test blenkernel_nurbs_mesh_test \
  blenkernel_nurbs_object_test blenkernel_nurbs_geometry_legacy_test --parallel 4

nice -n10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_edit_test --parallel 4

cmake --install /home/molotovgirl/Desktop/build_linux \
  --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/blender-nurbs-edit-ui-build-final.log`,
`/tmp/blender-nurbs-edit-ui-build-owner-test-final.log`,
`/tmp/blender-nurbs-edit-ui-install.log`. The separate final edit-test build
ensured its executable linked the last editor-library change. Building `blender`
alone had left the installed startup Python scripts stale; installation updated
the native menus. The final viewport test asserts that their registered class
exists, so an old installed menu cannot silently pass this test.

Final eight-suite commands (each exit 0):

```sh
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-edit-ui \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_differential_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
```

Results: **32 + 13 + 44 + 10 + 9 + 7 + 4 + 7 = 126 passed**, with the
conditional saved-UI source check enabled and passing. Logs:
`/tmp/blender-nurbs-edit-ui-{edit,selection,geometry,tessellate,differential,mesh,object,legacy}-tests-complete.log`.
Click and box files contain selected source vertices 1 and 2, edge 4, and no
selected face. Click history/active is 2; box has no invented history/active.
Geometry resources, IDs, incidence, coefficients, rational weights and knots
match the baseline. Preview triangles change from 128 to 338 at resolution 8
versus 13; the source selection does not change. The seven direct selection/entry
tests, one saved-source test and two numerical tests account for 116→126.

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|blendfile_autosave|global_undo)$'
```

**4/4 CTest targets passed**, including 25 NURBS Python cases; 6.61 seconds.
Log: `/tmp/blender-nurbs-edit-ui-python-ctest-installed.log`.

Final native viewport command, run from this checkout:

```sh
nice -n10 env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-edit-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_native.vertex_selection_and_lifecycle
```

That command and four separate processes using the same runner/environment with
the following test IDs all exited 0:

* `test_nurbs_line.line_to_sheet`
* `test_nurbs_line.face_region_selection_gestures`
* `test_undo.view3d_mesh_edit_separate`
* `test_undo.view3d_font_edit_mode_simple`

Logs: `/tmp/blender-nurbs-edit-ui-native-viewport-test-installed.log` and
`/tmp/blender-nurbs-edit-ui-ui-{line,face,mesh-undo,font-undo}-installed.log`.
These are event regressions, separate from visual inspection. Expected error
reports for refused legacy/native-incompatible mutations are asserted behavior.

**Visually confirmed:** inspected the final installed build's actual screenshot,
`/tmp/blender-nurbs-edit-ui/native-edit.png`: source corner markers and boundaries,
selected bottom two corners/edge, statistics V2/4 E1/4 F0/1, and native View/Select
menus without the old Control Points menu. This is surface topology selection,
not general curve fit-point editing. The prepared file is
`/tmp/blender-nurbs-edit-ui/try-native-selection.blend`; see the tryout guide.

A fresh background Blender process also opened that saved file, verified
`is_native_nurbs`, zero legacy splines and Object Mode, entered Edit Mode and
reported `Verts:2/4 | Edges:1/4 | Faces:0/1`, then exited normally. Log:
`/tmp/blender-nurbs-edit-ui-saved-file-app-open.log`. Baseline fixture SHA-256
remains `7ce0ec39f1c60e4234ba2795efd4650115d658018ef4d57b6bba2b1abf69efbe`.

**Still unsupported:** native Edit Mode geometry transforms/extrusion, curve
fit-point solving, linked selection, edge/face picking modes, visible-only
picking and general multi-owner mutation. Initial click/box selection requires
X-Ray. Ordinary Add NURBS still creates the existing prototype. Unsupported
native operations report/refuse entry rather than mutate empty legacy arrays;
this does not establish every remaining operator/attribute contract. The old
prototype's duplicate-wall and retained-cap failures remain open.

## Duplicate and region-extrusion repair checkpoint

The user's new report was Shift+D appearing to move the original face, followed
by E appearing to duplicate that face. They clarified that sequence after the
initial asynchronous question. The exact file/source owner was not identified.
The shortcut regression below follows duplicate-move and then repeated extrusion;
these are reproduced defects and bounded fixes, not a claim to have reproduced
every variant of the report.

Before changes, reran:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup \
  --python-exit-code 1 --python doc/nurbs_mesh_study/baseline_diagnostics.py
```

Observed the same baseline failures: both original and duplicate lines moved
to Y=1; selecting one versus two stored copies of a shared edge produced 2→3
versus 2→4 patches; repeated face extrusion produced 6→11 patches instead of
the mesh reference's 6→10. The untagged four-by-four full-surface extrusion also
returned FINISHED without changing its source; that separate legacy limitation
is still present.

### Implemented

* Added native `plan_face_region_extrusion`, based on the shared `TopologyIndex`.
  It reports unique wall boundaries, interior edges/vertices and whether attached
  original faces must be removed. Hole loops contribute boundaries; two seam uses
  are internal. More than two selected uses per edge are explicitly unsupported.
* Added `BKE_nurbs_legacy_edit.hh` / `intern/nurbs_legacy_edit.cc` as a restricted
  compatibility client. It imports the tagged two-point/two-by-two graph to
  normalize selected source IDs and obtain native closure-copy maps and region
  plans. It prepares owned new NURBS splines and source-removal IDs. Input geometry
  and selection remain untouched on failure. No new operator or display-mesh
  editing source is introduced.
* Duplication allocates fresh component IDs while preserving shared identity
  inside the copied region. Complete support copies retain their coefficients,
  rational weights, custom knots, materials and point attributes. Shared edges
  are copied once. Unsupported isolated-point duplication reports an error.
* Extrusion creates one wall per region boundary edge, with shared cap vertices
  and connector edges. An attached region replaces its old caps; an isolated
  sheet retains its base. Selected copies of one shared edge give the same
  construction. A wire becomes one connected surface source rather than retaining
  a second stored edge owner. Endpoint extrusion remains supported.
* Removed approximately 500 lines of construction/classification from
  `editcurve.cc`. ED now applies a prepared kernel candidate, cleans animation
  indices of removed splines, sets active/selection state and notifies Blender.
  Unrelated legacy tools retain their normal route. Mixed selected tagged and
  untagged geometry and tagged shape-key mutations report their limits.
* Fixed shared-position propagation to copy XYZ only. A rational weight belongs
  to the individual curve/surface basis and is preserved on incident supports.

This is a repair of the existing usable prototype using shared services, not
promotion of its corner arrays into the general native geometry kernel. Native
Edit Mode D/E/geometry transforms remain blocked until their adapters pass the
separate gates. Neither general curved deformation nor curve fit-point solving
is implemented here.

### Built and tested

Initial syntax checking found two array-to-Span conversions in the new client;
fixed them before the complete build. The initial three-target build passed.
Its test compile reported one range-loop copy warning, corrected in the final
build. Initial new kernel tests passed (14 legacy/import, 16 selection/region).
The first shortcut run passed its editing/undo assertions, then failed at saving
the screenshot due to a missing `bpy` import in the new test. Fixed the harness
and reran it successfully. Logs:
`/tmp/blender-nurbs-duplicate-region-syntax-*.log`,
`/tmp/blender-nurbs-duplicate-region-build.log`,
`/tmp/blender-nurbs-duplicate-region-ui.log` (initial failure),
`/tmp/blender-nurbs-duplicate-region-ui-fixed.log` (exit 0).

Subsequent source review found a wall-direction regression in the compatibility
builder: always using the canonical edge direction reversed two of a sheet's
walls. Corrected it using incident edge-use orientation, reversing V exactly
where needed while preserving the U basis, weights and boundary identities.
Added outward-normal assertions for all six walls in the existing two-face
region regression. For conflicting edge-only incident orientations the explicit
sweep orientation is retained; no outside/solid orientation is inferred.
The first rebuild of that assertion lacked a `BLI_set.hh` test include and
failed compilation; added the include and reran the complete target set. Logs:
`/tmp/blender-nurbs-duplicate-region-build-oriented.log` (exit 2),
`/tmp/blender-nurbs-duplicate-region-build-oriented-fixed.log` (final build).

Final build command, **exit 0**, with no compiler warnings/errors in its log:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_geometry_legacy_test blender blenkernel_nurbs_selection_test \
  blenkernel_nurbs_geometry_test blenkernel_nurbs_edit_test \
  blenkernel_nurbs_differential_test blenkernel_nurbs_tessellate_test \
  blenkernel_nurbs_mesh_test blenkernel_nurbs_object_test --parallel 4
```

Final log: `/tmp/blender-nurbs-duplicate-region-build-oriented-fixed.log`;
`/tmp/blender-nurbs-duplicate-region-build-final.log` records the earlier
successful build before the direction correction. This rebuild's binary
reports commit `2fd4dfd9a156`; these repairs are subsequent working-tree changes.

Final kernel/integration commands (each **exit 0**):

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-edit-ui \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_differential_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test
```

**14 + 16 + 44 + 32 + 9 + 10 + 7 + 4 = 136 passed**, no skips. The ten new
tests include fresh duplicate ownership/internal sharing, custom knot/attribute
preservation, source atomicity, boundary-only walls, attached-region removal,
shared-use selection equivalence and support-weight preservation. Three region
subset cases compare resulting V/E/F counts with actual BMesh extrusion. Hole,
periodic seam and ambiguous non-manifold planning have separate coverage;
the compatibility constructor does not claim those general geometries.
Final post-direction-correction logs:
`/tmp/blender-nurbs-duplicate-region-{geometry_legacy,selection,geometry,edit,differential,tessellate,mesh,object}-tests-oriented.log`.

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|blendfile_autosave|global_undo)$'
```

**4/4 targets passed**, including **31 line/edit cases + 2 old-file cases = 33
NURBS Python tests**, in 6.73 seconds. Added eight application regressions,
including duplicate-move, copied multi-face sharing, independent stored-edge
selection, repeated/attached cap removal and copy/save/reload/extrude. Corrected
two old expected counts that encoded retained internal caps. Log:
`/tmp/blender-nurbs-duplicate-region-ctest-oriented.log`.
The preceding save/reload checkpoint also passed, recorded in
`/tmp/blender-nurbs-duplicate-region-ctest-serialized.log`.

New shortcut regression command, **exit 0**:

```sh
nice -n 10 env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_line.duplicate_and_repeat_face_extrusion
```

It exercises actual Shift+D and E, axis/numeric input, undo/redo, Escape and
saved source geometry. As with mesh E, Escape cancels movement after topology
creation; undo removes the whole extrusion macro. Five additional isolated
processes using that runner passed, for **six viewport regressions total**:

* `test_nurbs_line.line_to_sheet`
* `test_nurbs_line.face_region_selection_gestures` (box/click/linked)
* `test_nurbs_native.vertex_selection_and_lifecycle`, with
  `BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend` and
  `BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-edit-ui`
* `test_undo.view3d_mesh_edit_separate`
* `test_undo.view3d_font_edit_mode_simple`

Final post-direction-correction logs:
`/tmp/blender-nurbs-duplicate-region-ui-oriented-{duplicate,line,region,native,mesh-undo,font-undo}.log`.
The existing baseline empty-keymap/unknown-operator notices are still emitted.
They did not fail these tests.

Reran the diagnostic command above, **exit 0**. The duplicate's original now
stays at Y=0 while the copy moves to Y=1; both shared-edge selections produce
2→3 patches; repeated NURBS and mesh region extrusion both produce 6→10.
Log: `/tmp/blender-nurbs-duplicate-region-diagnostics-after.log`.

**Visually confirmed:** inspected
`/tmp/blender-nurbs-duplicate-region-ui/duplicate-extrude.png`. It shows the
original sheet and a separate filled extruded copy. Internal-cap absence is
established by source assertions, not by this exterior screenshot. The saved
file is `/tmp/blender-nurbs-duplicate-region-ui/try-duplicate-extrude.blend`.
A fresh background process reopened it, verified 11 source splines, extruded
the selected top to 15 without retaining its previous cap, then duplicated and
moved that cap to make 16. The original sheet stayed unchanged. It exited 0;
log: `/tmp/blender-nurbs-duplicate-region-saved-file.log`. It did not overwrite
the supplied tryout file. See the [tryout instructions](prototype_duplicate_extrude_tryout.md).

Fresh-process command, **exit 0** (the line breaks in the expression are for readability):

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup \
  /tmp/blender-nurbs-duplicate-region-ui/try-duplicate-extrude.blend \
  --python-exit-code 1 --python-expr '
import bpy
obj = bpy.context.object
assert not obj.data.is_native_nurbs
assert obj.type == "SURFACE"
assert len(obj.data.splines) == 11
original = [tuple(p.co) for p in obj.data.splines[0].points]
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.curve.extrude_move(TRANSFORM_OT_translate={"value": (0, 0, 1)})
bpy.ops.object.mode_set(mode="OBJECT")
assert len(obj.data.splines) == 15
assert [tuple(p.co) for p in obj.data.splines[0].points] == original
assert not any(all(p.co.z == 4 for p in s.points) for s in obj.data.splines)
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.curve.duplicate_move(TRANSFORM_OT_translate={"value": (4, 0, 0)})
bpy.ops.object.mode_set(mode="OBJECT")
assert len(obj.data.splines) == 16
assert [tuple(p.co) for p in obj.data.splines[0].points] == original
print("SAVED_DUPLICATE_REGION_REOPEN_OK", len(obj.data.splines))'
```

`git diff --check` passes. The original native preview fixture SHA-256 remains
`7ce0ec39f1c60e4234ba2795efd4650115d658018ef4d57b6bba2b1abf69efbe`.

**Remaining limits:** the exact reported file/gesture is unconfirmed; arbitrary
untagged higher-order face extrusion still has its old limitations. Native-owner
geometry transforms/extrusion, fit-point editing, linked selection, edge/face
picking and source-orientation/solid validation remain separate work. Existing
files already damaged by reused prototype IDs may be rejected as inconsistent;
this change does not silently weld or repair them. These results do not promote
the complete native editing system or establish Stage 1 completion.

## Delete, then continue editing: reported failure and repair

The user supplied the exact error **"A legacy line has unexpected patch identities"**
after making a cube, extruding its top again, deleting the new top and attempting
further extrusion. Reproduced before rebuilding with this command, **exit 1**:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup \
  --python-exit-code 1 --python-expr '
import bpy
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.curve.primitive_nurbs_curve_add(enter_editmode=True)
bpy.ops.curve.extrude_move(TRANSFORM_OT_translate={"value": (0, 2, 0)})
bpy.ops.curve.select_all(action="SELECT")
bpy.ops.curve.extrude_move(TRANSFORM_OT_translate={"value": (0, 0, 2)})
bpy.ops.curve.extrude_move(TRANSFORM_OT_translate={"value": (0, 0, 1)})
obj = bpy.context.object
obj.update_from_editmode()
print("BEFORE_DELETE", len(obj.data.splines))
bpy.ops.curve.delete(type="VERT")
obj.update_from_editmode()
print("AFTER_DELETE", [(s.point_count_u, s.point_count_v) for s in obj.data.splines])
bpy.ops.curve.select_all(action="SELECT")
print("EXTRUDE_AFTER_DELETE", bpy.ops.curve.extrude())'
```

Log: `/tmp/blender-nurbs-delete-baseline.log`. Source inspection confirmed the
cause: `ed_surf_delete_selected` shrank four upper wall patches to two-point
lines but retained their face, four edge and four corner IDs. After deletion
there were five intact lower faces and four invalid collapsed records. The
strict importer correctly rejected them; disabling validation would hide the
inconsistent source.

### Shared topology and compatibility implementation

Added `BKE_nurbs_topology.hh` / `intern/nurbs_topology.cc` with
`plan_component_deletion`, using `TopologyIndex`. Vertices remove incident edges
and faces; Edges remove incident faces and newly loose edge endpoints; Faces
remove dependencies unused by surviving geometry; FacesOnly retains boundaries.
Existing unrelated loose geometry survives. These rules were checked against
`bmesh/intern/bmesh_delete.cc`, then actual BMesh deletion in the tests.

The compatibility client prepares removals and surviving loose edge/point source
records. It preserves IDs and NURBS coefficients instead of shrinking tensor
supports into invalid records. ED applies the candidate through the existing
shared adapter, clears removed animation indices and notifies Blender. Existing
Vertices/Segments commands use the corresponding kernel modes; the additional
face modes are kernel capabilities, not a claim that native face selection or
native deletion UI is activated.

Added standalone point support to the tagged importer and source selection/
position helpers. This is needed to preserve the unselected vertices when, for
example, deleting opposite corners of a quad. Those source points can be copied,
moved and extended into valid lines. Their IDs remain independent. Extending a
point with a zero curve weight reports an error without changing the source.
Mixed selected tagged/untagged geometry and tagged shape-key mutations retain
their explicit limitations.

The compatibility adapter also handles one narrow existing-file failure case.
A malformed two-point former patch must name exactly **one surviving declared
edge**, with endpoint IDs matching its original quad edge slot. Those IDs and
the old row/column layout determine endpoint order. Exact coefficients, rational
weights, knots and point/material/visibility attributes then validate that
existing identity. Geometry cannot choose between multiple surviving declarations
or introduce a new connection. Periodic, conflicting or differing records are
not recovered. The strict importer still rejects the malformed input on its own.
Successful ED application reports the number of redundant remnants removed.

An actual broken `.blend` was saved with the pre-fix executable:
`/tmp/blender-nurbs-delete-broken.blend`, SHA-256
`ea596ec1bfb326190b98fc54557cded623681d6269c3504f68ba06167b81362d`.
Log: `/tmp/blender-nurbs-delete-export-broken.log`. Its rim at Z=2 was selected.
The repaired build reopened it in a fresh process, reported **4 recovered
remnants**, completed E+Z1 and produced **9 valid two-by-two patches**, with
maximum Z=3. It saved to `/tmp/blender-nurbs-delete-recovered.blend`; the broken
input file's hash remained unchanged. Final log:
`/tmp/blender-nurbs-delete-recover-strict-final.log`.

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup \
  /tmp/blender-nurbs-delete-broken.blend --python-exit-code 1 --python-expr '
import bpy
obj = bpy.context.object
assert len(obj.data.splines) == 9
bpy.ops.object.mode_set(mode="EDIT")
print("RECOVER_EXTRUDE", bpy.ops.curve.extrude_move(TRANSFORM_OT_translate={"value": (0, 0, 1)}))
bpy.ops.object.mode_set(mode="OBJECT")
assert len(obj.data.splines) == 9
assert all(s.point_count_u == 2 and s.point_count_v == 2 for s in obj.data.splines)
assert max(p.co.z for s in obj.data.splines for p in s.points) == 3
bpy.ops.wm.save_as_mainfile(filepath="/tmp/blender-nurbs-delete-recovered.blend")
print("RECOVERED_SAVED_OLD_DELETE_SOURCE", len(obj.data.splines))'
```

### Build and verification

Initial compile-only checking found a missing list iterator include in the new
topology source; added `BLI_listbase.h` before the build. The other changed
implementation files passed syntax checking. Initial and intermediate complete
builds passed. Later review made alias recovery stricter: surviving declarations
must choose one edge before any geometry comparison, and IDs choose endpoint
order. Added tests for contradictory IDs, multiple surviving edges and periodic
records. No runtime regression failed during this checkpoint.

Final build, **exit 0**:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_geometry_legacy_test blender blenkernel_nurbs_selection_test \
  blenkernel_nurbs_geometry_test blenkernel_nurbs_edit_test \
  blenkernel_nurbs_differential_test blenkernel_nurbs_tessellate_test \
  blenkernel_nurbs_mesh_test blenkernel_nurbs_object_test --parallel 4
```

Log: `/tmp/blender-nurbs-delete-build-strict-recovery.log`, no compiler warnings/
errors. Earlier successful build logs: `/tmp/blender-nurbs-delete-build.log`,
`/tmp/blender-nurbs-delete-build-final.log`, `/tmp/blender-nurbs-delete-build-reviewed.log`.
The binary reports base commit `5bcbce446488`; these are subsequent source edits.

Final eight kernel/integration suites, **all exit 0**, using the same executable
commands and geometry asset argument as the preceding checkpoint:

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_legacy_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-edit-ui \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_differential_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test
```

**20 + 17 + 44 + 32 + 9 + 10 + 7 + 4 = 143 passed**, no skips. The seven new
tests cover deletion boundary/loose-point ownership, FacesOnly wire retention,
checked existing-file recovery and rejection of undeclared/ambiguous recovery.
The planner test performs **228 direct BMesh deletion comparisons**, covering
every nonempty vertex, edge and face subset of single/shared quads in four modes.
Logs: `/tmp/blender-nurbs-delete-strict-{geometry_legacy,selection,geometry,edit,differential,tessellate,mesh,object}-tests.log`.

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|blendfile_autosave|global_undo)$'
```

**4/4 targets passed**, including **35 editing + 2 old-file cases = 37 NURBS
Python tests**, 6.41 seconds. Log: `/tmp/blender-nurbs-delete-strict-ctest.log`.
Cases include the exact repeated-extrusion/delete/rim-extrusion sequence,
segment deletion, loose point retention/extension and delete/save/reload/edit.

Seven isolated viewport processes passed, using the existing headless runner
and portable Weston paths. Exact new case command:

```sh
nice -n 10 env BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- \
  --tests test_nurbs_line.extrude_delete_top_and_continue
```

Other test IDs: `test_nurbs_line.duplicate_and_repeat_face_extrusion`,
`test_nurbs_line.line_to_sheet`, `test_nurbs_line.face_region_selection_gestures`,
`test_nurbs_native.vertex_selection_and_lifecycle`,
`test_undo.view3d_mesh_edit_separate`, `test_undo.view3d_font_edit_mode_simple`.
Native case environment adds the unchanged baseline preview fixture and
`BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-delete-native-ui-strict`.
Logs: `/tmp/blender-nurbs-delete-strict-ui-{delete,duplicate,line,region,native,mesh-undo,font-undo}.log`.
The new case uses actual X, undo/redo, box selection and E, and then draws/moves/
extrudes loose points in a second scene. Earlier complete pre-review reruns are
retained in `/tmp/blender-nurbs-delete-*-tests-final.log` and
`/tmp/blender-nurbs-delete-ui-*-final.log`.

The fresh native viewport files were also read by the C++ source-ID/geometry
validator, **1/1 passed, exit 0** (a repeat of an existing test, not test 144):

```sh
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-delete-native-ui-strict \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test \
  --gtest_filter='*ViewportFixtureFilesPreserveSelectedSourceIDsAndGeometry'
```

Log: `/tmp/blender-nurbs-delete-strict-saved-native-source.log`.
`git diff --check` passes. The original broken fixture and native preview fixture
hashes remain unchanged.

**Visually confirmed:** inspected the actual screenshot at
`/tmp/blender-nurbs-delete-ui/delete-and-extrude.png`. It shows the surviving
lower body and the new open-rim extrusion. The complete kernel/application
assertions establish its source identity and validity separately from the image.
The supplied file is `/tmp/blender-nurbs-delete-ui/try-delete-and-extrude.blend`;
see [the deletion tryout](prototype_deletion_tryout.md).

This fixes the reported tagged-prototype failure and supplies reusable deletion
planning for the native system. It does not activate native-owner G/D/E/deletion,
general curved fit-point editing or arbitrary legacy-source repair. Native
movement work is preserved separately while this blocking user reproduction is
handled, and resumes after this checkpoint.


## Native movement checkpoint — 2026-10-24

Implemented, built and tested:

* `BKE_nurbs_transform.hh` / `nurbs_transform.cc`: translate topology vertex IDs
  and all incident geometry from an immutable source. A shared curve or support
  must receive the same coefficient request from every owner. Translate complete
  edges/supports, partial clamped linear edges, bilinear corners and complete
  rows in supported ruled tensor faces. Preserve rational weights, knot domains,
  p-curves, IDs and unreferenced resources. Reject unsupported partial curved or
  trimmed deformation and conflicting shared-resource requests atomically.
* `transform_convert_nurbs.cc`: dedicated native source conversion type, original
  and current retained-state tokens, source-aware cancellation, double offsets,
  owner commits through `ED_nurbs.hh`, and single-owner mode/feature guards.
  Float `TransData` proxies never become source coordinates. Every modal update
  starts from the original source, avoiding accumulated frame-by-frame rounding.
* `NUM_USE_DOUBLE`: unit-converted double values, original defaults, expression
  retention across axis changes, double increment/reset/negation/inversion and a
  double `applyNumInput` channel. Existing tools retain their float channel.
* `UnitConversionPrecision`: explicit double unit input without changing the
  default unit catalogue arithmetic or display formatting. The catalogue retains
  the old float results and declared double scales, including squared/cubed and
  velocity expressions. Native double input requests the precise channel.
* Translation replay retains double space/world deltas and the declared float
  orientation. An unchanged replay preserves its original world delta. Native
  F9 uses decimal X/Y/Z text fields; changing a value that has the same float
  representation still changes the source. Normal mesh F9 controls are unchanged.
* Numeric input rejects control characters as text so Return reaches modal
  confirmation in advanced-expression mode. Automatic transform confirmation
  respects a rejected/cancelled geometry update.

The numerical policy accepts finite coefficient-addition rounding and reports
its calculated bound when nonzero. It performs no fitting. A move wholly lost
at source coordinate precision is rejected. This bound is relative to the supplied
local offset; it does not certify real-number decimal parsing, physical units,
float object/orientation/pivot construction or matrix inversion. Mouse input and
orientation matrices still originate in Blender's existing float interaction
system. The project unit scale is also an existing float property. Untouched
coordinate axes retain their original bits.

Unsupported cases are not promoted to a display-mesh editing path. Partial
curved endpoint changes still require the user's fit-point/shape policy. Source
normals exist for evaluation, but a component orientation rule and an owner
adapter are still needed for Normal/Gimbal editing. Active-element center lookup
still needs native source identity integration; default-pivot G is the tested path.
Native E/D/delete and general fit-point editing are not activated by this change.

### Build and C++ checks

The build directory is `/home/molotovgirl/Desktop/build_linux`; the other desktop
build name is its symlink. The final source build completed with:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_edit_test blender --parallel 4
```

Log: `/tmp/blender-nurbs-native-move-orientation-build.log`, exit 0. This builds the
application last, after its final native orientation guard and the source-file
validator. The final test relink completed, exit 0, with:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target \
  blenkernel_unit_test blenkernel_nurbs_transform_test \
  blenkernel_nurbs_geometry_legacy_test blenkernel_nurbs_selection_test \
  blenkernel_nurbs_geometry_test blenkernel_nurbs_edit_test \
  blenkernel_nurbs_differential_test blenkernel_nurbs_tessellate_test \
  blenkernel_nurbs_mesh_test blenkernel_nurbs_object_test \
  editor_util_numinput_test editor_transform_transform_translate_precision_test \
  --parallel 4
```

Log: `/tmp/blender-nurbs-native-move-regression-build-final.log`.
Earlier broad builds recompiled untouched orientation/ruler/window-manager files
with unused-variable/function/parameter warnings. The final two logs have no
compiler errors. No extra system packages were installed.

Every executable ran with `--gtest_color=no`. Counts, all passed without skips:

| Executable suffix | Passed |
|---|---:|
| `blenkernel_nurbs_geometry_legacy_test` | 20 |
| `blenkernel_nurbs_selection_test` | 17 |
| `blenkernel_nurbs_geometry_test` | 44 |
| `blenkernel_nurbs_edit_test` | 33 |
| `blenkernel_nurbs_differential_test` | 9 |
| `blenkernel_nurbs_tessellate_test` | 10 |
| `blenkernel_nurbs_mesh_test` | 7 |
| `blenkernel_nurbs_object_test` | 4 |
| `blenkernel_nurbs_transform_test` | 10 |
| `blenkernel_unit_test` | 3 |
| `editor_util_numinput_test` | 4 |
| `editor_transform_transform_translate_precision_test` | 4 |
| **Total** | **165** |

Executables are in `build_linux/bin/tests`. Geometry passed
`--test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'`.
Only the edit suite received these fixture variables:

```sh
env -u BLENDER_NURBS_PREVIEW_EXPORT \
  BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-move-native-selection-ui \
  BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test \
  --gtest_color=no
```

The other eleven suites ran without export/input fixture variables. Logs:
`/tmp/blender-nurbs-native-move-{suite}-tests-final.log`; suite names are
`nurbs_geometry_legacy`, `nurbs_selection`, `nurbs_geometry`, `nurbs_edit`,
`nurbs_differential`, `nurbs_tessellate`, `nurbs_mesh`, `nurbs_object`,
`nurbs_transform`, `unit`, `editor_util_numinput`, and
`editor_transform_transform_translate_precision`.

The transform tests include shared vertices, weighted bilinear corners, rational
arc/closed-circle row movement, source immutability, conflicting resource owners,
negative weights, explicit addition budgets, zero-offset no-ops and signed zero.
Weight-agreement checks reject a one-bit difference and handle products that
would overflow/underflow if evaluated directly. Shared-sheet and rational-row
preview checks use zero evaluation allowance, including 13 samples per span.

### Application checks

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

**5/5 targets passed**, 9.28 seconds; 37 NURBS Python cases plus the existing
unit/autosave/global-undo regressions. Log:
`/tmp/blender-nurbs-native-move-ctest-final.log`.

Eight viewport tests ran, each in its own application/display-server process:

```sh
nice -n 10 env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-move-native-selection-ui \
  BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

All exit 0:

| Log label | TEST_ID |
|---|---|
| `move` | `test_nurbs_native.vertex_move_and_lifecycle` |
| `native` | `test_nurbs_native.vertex_selection_and_lifecycle` |
| `line` | `test_nurbs_line.line_to_sheet` |
| `region` | `test_nurbs_line.face_region_selection_gestures` |
| `duplicate` | `test_nurbs_line.duplicate_and_repeat_face_extrusion` |
| `delete` | `test_nurbs_line.extrude_delete_top_and_continue` |
| `mesh-undo` | `test_undo.view3d_mesh_edit_separate` |
| `font-undo` | `test_undo.view3d_font_edit_mode_simple` |

Logs: `/tmp/blender-nurbs-native-move-ui-{label}-final.log`. The batch driver is
`/tmp/blender-nurbs-run-move-ui-regressions.py`; it runs the commands sequentially,
never multiplexing event generators in one application. DISPLAY is removed so
tests use their own Wayland display. No existing user Blender process was closed.
The movement test types 17-digit values, changes axes with Tab, cancels G, uses
undo/redo, opens F9 and actually edits its Z field by mouse/keyboard. It also checks
reset, negation, inversion, explicit units, a scaled owner/local axis, rejected
Normal movement, invalid exact fields, float aliases and a `1e-200` source offset.
Transient invalid prefixes while typing full expressions produce Blender's existing
numeric-input reports; intentional rejection cases also produce error reports.
These reports are distinct from a failed test or fitting a source geometry.

Twenty saved movement files are checked by
`ViewportMovementFilesPreservePreciseSourceAndCancellation`, including exact
source coordinates and unchanged IDs/bases/weights. Its latest isolated repeat
passes: `/tmp/blender-nurbs-native-move-saved-source-20-final.log`.
The F9 popup and the oblique sheet capture in `/tmp/blender-nurbs-move-ui` have been
visually inspected. Rendered appearance is not used as a source-precision test.

The original pre-fix broken file was reopened with the current application:

```sh
nice -n 10 /home/molotovgirl/Desktop/build_linux/bin/blender \
  --background --factory-startup \
  --python /tmp/blender-nurbs-recover-after-native-move.py
```

The script opens `/tmp/blender-nurbs-delete-broken.blend`, enters Edit Mode,
executes `curve.extrude_move(TRANSFORM_OT_translate={'value': (0, 0, 1)})`, and
asserts FINISHED, nine 2x2 splines and maximum Z=3. It reports recovery of four
verified remnants and saves separately as
`/tmp/blender-nurbs-delete-recovered-after-native-move.blend`. Log:
`/tmp/blender-nurbs-native-move-old-broken-recovery-final.log`, all assertions pass.
The input hash remains
`ea596ec1bfb326190b98fc54557cded623681d6269c3504f68ba06167b81362d`.

### Failures found and corrected

* Early test target invocation preceded CMake regeneration and had no make rule;
  regeneration registered the new executable. C++ builds then caught missing
  includes, explicit matrix conversion requirements and non-iterable vector
  range-for usage. Corrected before the successful source/test builds above.
* An expanded viewport test passed its float preview check, but the saved-source
  check found centimetres storing `0.12345678629629631` instead of
  `0.12345678901234568`. The old unit constants and nine-digit conversion factors
  were the cause. A global double-constant correction made source input correct
  but changed legacy mixed-unit output (`0.123 mm` to `0.12 mm`). That approach
  was replaced with explicit precise conversion and unchanged legacy defaults.
  Failed logs: `...-saved-source-expanded.log`, `...-unit-python.log` under the
  `/tmp/blender-nurbs-native-move` prefix. Current unit and source tests pass.
* Float F9 fields cannot distinguish edits that alias the same float. Native
  decimal fields solve this at the current adapter boundary, retaining existing
  mesh controls. Both explicit replay and actual popup editing have source tests.
* Advanced-expression input could insert Return's control character and keep G
  running. The shared handler now lets confirmation through; the new control-
  character test and complete viewport sequence pass. One earlier viewport run
  also used an application linked before a newer adapter library; the application
  was explicitly relinked before repeating the checks. Failed runs are retained
  as `...-ui-precise-fields.log` and `...-ui-current.log`.
* CMake initially disabled Wayland because xkbcommon development headers were
  absent. `apt-get download libxkbcommon-dev` fetched `1.6.0-1build1`; it was
  extracted under `/tmp/blender-nurbs-xkbcommon/root` without system installation.
  A local pkg-config wrapper supplies its corrected prefix; its library symlink
  references the existing system `libxkbcommon.so.0`. CMake caches
  `/tmp/blender-nurbs-xkbcommon/pkg-config`. Clearing the failed xkbcommon cache
  and reconfiguring fixed linking. Final GHOST includes both Wayland and X11.

### Fixture provenance

An early regression command inherited `BLENDER_NURBS_PREVIEW_FIXTURE` for every
suite. The object test used that name as an optional export setting and regenerated
our temporary fixture. Its old raw hash must not be claimed unchanged in this
checkpoint. Input and export now use different variables: `PREVIEW_FIXTURE` reads;
`BLENDER_NURBS_PREVIEW_EXPORT` explicitly requests creation in the object test.
The latter is unset during ordinary regression runs.

The regenerated baseline's hash, unchanged throughout final verification, is
`329f75e097dbcf7b580b7bdba5d8e619ac26ca812b9cd3ab38098907059f2e5a`.
A source comparison against three selection files saved before regeneration
(`/tmp/blender-nurbs-delete-native-ui-strict`) passes, verifying unchanged sheet
positions, weights, knots, incidence and IDs. Log:
`/tmp/blender-nurbs-native-move-baseline-vs-earlier-files.log`. This establishes
source equivalence for the sheet; it is not a byte-for-byte identity claim for the
old fixture or a separate comparison of its hole/cylinder payloads. No user's
working file was supplied or overwritten.


The saved movement tryout was also opened in a fresh isolated display process.
It restores **Edit Mode**, 1/4 source vertices selected, 0/4 edges and 0/1 faces
selected, with 128 derived display triangles. The source spline collection is
empty and `is_native_nurbs` is true. Probe:
`/tmp/blender-nurbs-capture-move-tryout.py`; log:
`/tmp/blender-nurbs-native-move-reopen-tryout.log`. Its measured float display
maximum Z is `1.1234568357467651`; this is not presented as a source-coordinate
measurement. The separate source-file checks establish precision.


## Native active-vertex center checkpoint — 2026-10-24

`ED_curve_active_center` now dispatches native owners through their live source
read lease and kernel topology index. The active vertex must exist and be selected.
The shared helper serves both transform active-element pivots and cursor-to-active;
there is no separate operator-specific coordinate copy. Legacy curves retain their
existing branch. Missing/non-vertex active state or a position outside float UI
range returns false without changing the output or source. The query remains a
float UI boundary; it never writes a rounded point into geometry.

`ActiveVertexCenterReadsLiveSourceIdentityWithoutMutation` checks a changed live
point while the persistent ID still contains its older coordinates, retained-state
identity, clean dirty flags, no active point and out-of-range coordinates. The
viewport test selects two corners with the right corner active after undo/redo,
asserts cursor-to-active is `(1, -1, 0)` rather than their median, then exercises
G/cancel with Active Element pivot and validates unchanged selection/source files.

Builds, all exit 0:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_edit_test blender --parallel 4
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_edit_test --parallel 4
```

Logs: `/tmp/blender-nurbs-active-center-build.log` and
`/tmp/blender-nurbs-active-center-test-build.log`. The full twelve-executable test
relink uses the same target list as the movement checkpoint; log:
`/tmp/blender-nurbs-active-center-regression-build.log`, exit 0. All twelve suites
ran again with no skips: **166 passed**, with the edit suite now 34. Driver:
`python3 /tmp/blender-nurbs-run-active-center-kernel-checks.py`. It uses the same
geometry asset argument and read-only fixture policy as the preceding checkpoint,
with selection files in `/tmp/blender-nurbs-active-center-ui`. Per-suite logs:
`/tmp/blender-nurbs-active-center-{suite}-tests-final.log`.

Two relevant viewport runs repeat the preceding isolated Wayland command, each
in its own process: `test_nurbs_native.vertex_selection_and_lifecycle` with
`BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-active-center-ui`, and
`test_nurbs_line.line_to_sheet`. Both pass; logs:
`/tmp/blender-nurbs-active-center-pivot-ui.log` and
`/tmp/blender-nurbs-active-center-legacy-line-ui.log`.
The saved-source validator was repeated after the new UI run, 1/1 passed:
`/tmp/blender-nurbs-active-center-saved-source-final.log`.
The frozen baseline hash remains `329f75e097dbcf7b580b7bdba5d8e619ac26ca812b9cd3ab38098907059f2e5a`.
The preceding five CTest targets and other six viewport regressions are unchanged
checks from the full movement checkpoint; they were not unnecessarily rerun for
this read-only editor query. General fit-point editing, Normal/Gimbal orientation,
edge/face picking and the remaining native editing gates remain incomplete.

## Native linked-selection checkpoint — 2026-10-24

`find_linked_components` in BKE walks the shared `TopologyIndex`; it does not
infer adjacency from positions, shared support resources, display samples or
legacy control nets. Requests accept vertex, edge and face IDs. Results follow
source order, include loose vertices/wires and non-manifold incidence, and do
not allocate identities or change geometry. An explicit face seed includes all
of its boundaries. Closed rational seam edges occur once in the result.

The connectivity rule is explicit: `VertexGraph` follows shared endpoints;
`FaceBoundaries` also connects every loop of an incident face. `RequireAgreement`
rejects a request when those alternatives differ. The viewport currently uses
`RequireAgreement` because the user's hole-boundary choice remains unanswered.
An initial implementation joined every face boundary; review against the earlier
open question caught that implicit policy before commit. Both alternatives now
have kernel tests, without choosing a user-facing default for ambiguous holes.
This graph closure matches mesh vertex-mode shell connectivity on ordinary
faces; it is not a claim of mesh face-mode/delimiter parity.

Native L, Shift+L and Ctrl+L reuse existing Curve operators/key bindings, with
source-inclusive polls only for those operators. Click and linked-pick share one
source-vertex projection/nearest-hit service and retained read leases. All routes
use the same kernel selection normalization and owner commit as click/box/A.
Linked selection preserves surviving active/history data and does not invent a
hover-active vertex or change the active object. Ctrl+L preflights all native edit
owners before applying selection; a rejected hole request cannot partly change
another owner. No-op requests retain the source token and clean dirty flags.
Legacy Curve/Surface branches retain their existing behavior.

Built with exit 0:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_selection_test blenkernel_nurbs_edit_test blender --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux \
  --prefix /home/molotovgirl/Desktop/build_linux/bin
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_geometry_legacy_test blenkernel_nurbs_selection_test \
  blenkernel_nurbs_geometry_test blenkernel_nurbs_edit_test \
  blenkernel_nurbs_differential_test blenkernel_nurbs_tessellate_test \
  blenkernel_nurbs_mesh_test blenkernel_nurbs_object_test blenkernel_nurbs_transform_test \
  blenkernel_unit_test editor_util_numinput_test \
  editor_transform_transform_translate_precision_test --parallel 4
```

The application was built last after the final C++ edits. Startup menus were
installed after their change; later changes were C++ adapters and repository UI
tests. Logs: `/tmp/blender-nurbs-linked-policy-build.log`,
`/tmp/blender-nurbs-linked-install.log` and `...-install-final.log`,
`/tmp/blender-nurbs-linked-regression-build-final.log`. Earlier intermediate
builds are retained as `...-test-build.log`, `...-app-build.log` and
`...-regression-build.log` under the same prefix.

Final kernel/editor/integration run:

```sh
python3 /tmp/blender-nurbs-run-linked-kernel-checks.py
```

**173 tests passed, no skips**, with per-suite counts
20/23/44/35/9/10/7/4/10/3/4/4 in the target order above. Summary:
`/tmp/blender-nurbs-linked-kernel-summary.log`; per-suite logs:
`/tmp/blender-nurbs-linked-{suite}-tests-final.log`. The edit suite reads the frozen
fixture and latest six selection/twenty movement files; the geometry suite uses
`--test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'`.
`BLENDER_NURBS_PREVIEW_EXPORT` is unset throughout ordinary tests.

Application checks:

```sh
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

**5/5 passed**, including the 37 existing NURBS Python cases. Log:
`/tmp/blender-nurbs-linked-ctest-final.log` (8.93 seconds).

Seven independent real-window event tests pass. Driver:
`python3 /tmp/blender-nurbs-run-linked-ui-final.py`. Every subprocess uses:

```sh
nice -n 10 env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui \
  BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

The exact `TEST_ID` values are:

* `test_nurbs_native.linked_selection_rejects_ambiguous_holes_without_partial_changes`
* `test_nurbs_native.vertex_selection_and_lifecycle`
* `test_nurbs_native.linked_selection_across_objects`
* `test_nurbs_native.vertex_move_and_lifecycle`
* `test_nurbs_line.face_region_selection_gestures`
* `test_nurbs_line.extrude_delete_top_and_continue`
* `test_nurbs_line.duplicate_and_repeat_face_extrusion`

Logs: `/tmp/blender-nurbs-linked-ui-{holes,native,linked-multiobject,move,legacy-region,legacy-delete,legacy-duplicate}-final.log`.
Click/box/hover linked selection, subtraction, undo/redo, retained active center,
changed display density, G/cancel and multi-owner behavior are exercised. Ctrl+L
works with X-Ray off. L/Shift+L still require X-Ray and a source-vertex hit; edge
and face-interior picking are not activated. Mesh face-mode shells, delimiters,
visibility storage, fit handles and public native component bindings remain gates.
Linked-pick still follows the existing Curve invoke-only lifecycle; no stable-ID
redo API is claimed.

The source-file validator now checks six selection files, including
`linked-click.blend`, `linked-hover.blend` and `linked-box.blend`, for actual
source geometry, weights, knots, identities, selection/history and incidence.
Movement still validates twenty files against the source. No display mesh is
used as a geometry or precision oracle. The native fixture and original broken
file hashes remain the frozen values recorded above; input/export variables
remain separate.

Visual evidence: a fresh isolated process reopened
`/tmp/blender-nurbs-linked-ui/linked-hover.blend`, entered Edit Mode and captured
`/tmp/blender-nurbs-linked-ui/native-linked-reopened.png`. That image was inspected:
four selected source corners, four edges, one NURBS face. Probe:
`/tmp/blender-nurbs-capture-linked-tryout.py`; successful log:
`/tmp/blender-nurbs-linked-reopen-final.log`. The first probe captured immediately
after changing mode, before the next UI redraw; it was corrected to defer capture
and the successful log shows 4/4 vertices, 4/4 edges, 1/1 faces and one owner.
Event tests establish keyboard behavior; the image alone does not.

Native duplication is the next proposed slice: preserve the existing exact closure
copy and identity maps, assemble a full candidate in BKE, remap selection/history,
and reuse the current owner commit and native Move adapter. Native Shift+D is
not implemented by this checkpoint. Curve fit-point deformation and hole-link
policy still require the user's design input; duplication of an unchanged
rational source does not require choosing its deformation behavior.

## Native duplication checkpoint — 2026-10-24

`duplicate_components` creates a full independent candidate using the existing
exact `copy_component_closure`. Each copied record gets a fresh identity;
sharing inside the copied group is retained, and original records stay intact.
No coordinates, weights, knots, parameter ranges, p-curves or orientations are
refitted. Stored selection is not silently redirected by this geometry service.

`selection_remap` is a separate reusable kernel service. It validates the source,
maps component sets and history to target identities, drops unmapped elements,
and flushes in an explicit selection mode. Repeated mapped history targets keep
the last occurrence. Wrong target domains/IDs reject the candidate. Duplication
uses its one-to-one ID map; more general map support is not a claim of mesh weld
or subdivision behavior.

ED orchestrates these services and one common source-token commit. The existing
Curve Duplicate operator dispatches native owners; the existing Duplicate/Move
macro supplies Shift+D and native G. The native adapter rejects non-normalized
vertex-mode selection and multiple edit owners before mutation. Legacy branches
remain intact. Empty selection and rejected requests preserve the state token,
ID counter and dirty flags. No new one-off operator or public component binding
was introduced. The existing Python duplicate/move macro accepts precise movement
strings. Native extrusion is still blocked.

Kernel-only build and checks, all exit 0:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_geometry_test blenkernel_nurbs_selection_test blender --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files' --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test --gtest_color=no
```

**47 geometry + 26 selection tests passed** before editor activation. Logs:
`/tmp/blender-nurbs-native-duplicate-kernel-build.log`,
`...-geometry-kernel-stage.log`, `...-selection-kernel-stage.log`.
New checks include shared multi-face/hole closure identity, original and copied
rational resource bytes, exhaustion/empty requests, selection/history compared
with actual BMesh Duplicate, all seven selection modes, and partial/merged maps.
This comparison covers duplicate counts and selection/history, not every BMesh
edge-winding, attribute or mode interaction.

Editor/application build, exit 0:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_edit_test blender --parallel 4
```

Log: `/tmp/blender-nurbs-native-duplicate-app-build.log`. Two focused editor tests
pass (`...-editor-focused.log`): copied geometry moves independently and retains
old snapshots/persistent data until flush; empty, inconsistent-mode and exhausted
requests leave source and dirty flags unchanged. Source comments describing the
payload as unused by editing operators were corrected; no DNA layout changed.
Only test/artifact changes followed the final product build.

All twelve test targets were relinked with the same complete target list as the
linked-selection checkpoint; log:
`/tmp/blender-nurbs-native-duplicate-regression-build-final.log`, exit 0.

```sh
python3 /tmp/blender-nurbs-run-native-duplicate-kernel-checks.py
```

**182 passed, no skips**: suite counts 20/26/47/38/9/10/7/4/10/3/4/4 in that
same target order. Summary:
`/tmp/blender-nurbs-native-duplicate-kernel-summary.log`; per-suite logs:
`/tmp/blender-nurbs-native-duplicate-{suite}-tests-final.log`. The edit suite has
all four read-only inputs: `BLENDER_NURBS_PREVIEW_FIXTURE`,
`BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui`,
`BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui`, and
`BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui`. The preview input
remains `/tmp/blender-nurbs-native-preview.blend`; export is unset.

```sh
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

**5/5 passed**, including the existing 37 NURBS Python cases. Log:
`/tmp/blender-nurbs-native-duplicate-ctest-final.log`, 8.85 seconds.

Eight isolated viewport runs pass. Driver:
`python3 /tmp/blender-nurbs-run-native-duplicate-ui-final.py`. It uses the exact
isolated Wayland command from the linked-selection checkpoint, adds
`BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui`, and runs its
seven listed test IDs plus `test_nurbs_native.duplicate_and_move_source_components`.
Logs: `/tmp/blender-nurbs-native-duplicate-ui-{duplicate,holes,native,linked-multiobject,move,legacy-region,legacy-delete,legacy-duplicate}-final.log`.
The multiple-object test now also verifies rejection before duplicate mutation.

The new source-file validator compares eleven actual saved files: click/linked
and box copies, undo, redo, cancelled Move, loose point, wire edge, precise Python
macro, rational circle, holed face and the prepared tryout. It checks coefficients
and knots byte-for-byte against the expected kernel candidate, plus all topology
IDs/references, tolerances, loops, edge uses, orientation, material index and
selection/history. Originals remain independent. Focused log:
`/tmp/blender-nurbs-native-duplicate-saved-source-11.log`, 1/1 passed. The full
38-test edit run includes this validator again.

An expanded UI test initially read `Scene.statistics` immediately after a Python
macro call and saw the previous 4/4 count. A diagnostic saved and reopened the
actual source, showing 4/8 vertices and 1/2 faces selected. `info_stats.cc` caches
statistics; `wm_event_system.cc` clears that cache when queued notifiers run.
The test now yields to that event cycle before asserting. Product code and
expected counts were unchanged. Reproduction:
`/tmp/blender-nurbs-native-duplicate-macro-repro.py`; logs:
`...-macro-repro.log` and `...-macro-reopen-factory.log`. The final viewport run
and independent saved-source check both pass.

Images actually inspected: `native-duplicate.png` (two independent holed faces,
copy selected) and `native-duplicate-f9.png` (Add Duplicate popup with precise Z),
both under `/tmp/blender-nurbs-duplicate-ui`. The tryout was also reopened in a
fresh isolated application; it restores Edit Mode, 8/16 source vertices, 8/16
edges and 1/2 faces selected, one owner, no legacy splines. Probe:
`/tmp/blender-nurbs-capture-duplicate-tryout.py`; log:
`/tmp/blender-nurbs-native-duplicate-reopen-final.log`; inspected image:
`/tmp/blender-nurbs-duplicate-ui/native-duplicate-reopened.png`.
These images are separate evidence from automated keyboard and source checks.

Frozen input hashes remain unchanged:
`329f75e097dbcf7b580b7bdba5d8e619ac26ca812b9cd3ab38098907059f2e5a`
(native preview) and
`ea596ec1bfb326190b98fc54557cded623681d6269c3504f68ba06167b81362d`
(original broken deletion file). No user's working file was overwritten.
Stage 1 remains incomplete: the next construction slice needs a shared native
region/extrusion service, not an ED-only face workaround. Fit-point deformation,
normal native creation/migration, edge/face picking, other transforms and the
pending hole-connectivity choice retain their existing gates.

## Shared deletion-candidate service — 2026-10-24

This is an additional kernel prerequisite for native extrusion, not activation of
native Delete or Extrude. `delete_components` applies the existing incidence plan
to an independent full candidate. It frees removed face loops/uses and topology
records, keeps remaining identities and the counter, and leaves coefficient
resources intact. It prunes deleted stored-selection/history references through
`selection_remove_ids`; mode-dependent selection flushing remains a caller's
explicit responsibility. A removed active element can fall back to surviving
history. A pre-existing absent active element stays absent on a no-op request.

The existing BMesh deletion comparison now checks actual candidate counts and
surviving vertex/edge/face IDs, not only the plan. It covers all 228 nonempty
component subsets across the quad and two-face fixtures and four deletion modes.
Two additional tests cover retained rational resources and stored selection,
wrong-domain/invalid-mode failures, empty requests and an exhausted ID counter
(which deletion does not advance). Source remains unchanged on rejection.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_selection_test blender --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test --gtest_color=no
```

Final build exit 0 and **28/28 selection tests passed**, no skips. Logs:
`/tmp/blender-nurbs-native-deletion-kernel-build-fixed.log` and
`/tmp/blender-nurbs-native-deletion-kernel-tests.log`. The first build used a removed
`LISTBASE_FOREACH_MUTABLE` macro and failed; it was corrected to this checkout's
`ListBaseT::items_mutable()` iterator before the successful build. Initial log:
`/tmp/blender-nurbs-native-deletion-kernel-build.log`.
The native-duplication checkpoint's full 182-test/eight-viewport/five-CTest results
remain separately recorded above. This additive service has no editor caller yet;
those unchanged application suites were not repeated or relabeled as new results.

The extrusion review also identified two requirements before its activation:
retained original region faces must reverse orientation as BMesh does, and cap
movement along walls made from interior curve trims needs a complete ruled-row
translation rule. Current partial-tensor G correctly rejects those trimmed
walls; interior trim endpoints must not be treated as CV indices. Neither a
special ED transform fallback nor a general curved deformation rule is implied.

## Shared region-construction checkpoint — 2026-10-24

`extrude_components` now constructs one full candidate for faces, edges and
isolated vertices. It copies the component closure once, translates 3D cap
resources without changing their bases/weights or 2D p-curves, creates one rail
per required boundary vertex, and builds ruled walls only on selected region
boundaries and independent selected wires. Shared interior edges and periodic
seams do not produce walls. The existing `extrude_edges` entry point is a thin
wrapper; `EdgeExtrusionResult` aliases the common `ExtrusionResult`.

Retained original selected faces reverse face sense, matching actual BMesh.
The cap keeps its original parameterization/sense. Region walls oppose their
cap uses; edge-only walls oppose existing source uses. When replacing a region,
the common deletion-candidate service removes old faces and unused interior
edges/vertices after walls establish surviving boundary incidence. The result
includes its removed-component plan and shared cap/rail/wall ID maps.
No coordinate welding, fitting, display-mesh construction or inferred solid
outside is involved. Declared oriented source uses drive face sense; general
trim-winding and geometric/solid validity are not certified by this operation.

The same positive-weight, supported-arithmetic, endpoint-agreement and explicit
translation-roundoff budget gates apply to cap supports and curves. P-curves
remain in parameter space. The shared addition helper now skips zero axes so
untouched signed-zero coordinates retain their bits. Zero displacement remains
an allowed singular editing intermediate with distinct source identities.

New geometry checks cover connected cube construction and repeat extrusion,
vertex deletion followed by continued boundary extrusion, multi-face outer walls,
five-face cube-region interior cleanup, holes, curved cap bases/parameter curves,
mixed V/E/F zero sweeps, signed zero and rejected cap weights. Actual BMesh
comparison covers all **67 nonempty face subsets** across a sheet, adjacent faces
and a cube, including retained-face/wall normals and degenerate wall frames.
A separate periodic NURBS case checks the shared seam, closed-boundary rails,
weights and knot vectors; it is not represented as a BMesh face with self-loops.
The BMesh reference fixture now applies declared NURBS face sense when importing.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_geometry_test blenkernel_nurbs_selection_test blender --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test \
  --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files' --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test --gtest_color=no
```

Final build exit 0; **54 geometry + 30 selection tests pass**. Logs:
`/tmp/blender-nurbs-native-region-kernel-build-fixed.log`,
`...-geometry-full.log` and `...-selection-full.log`.
The initial build's wrapper reported exit 143 after its log reached the completed
application; the build was repeated successfully. A later test-adapter compile
failed because this checkout's `BM_face_normal_flip` also needs its BMesh argument;
that call was fixed before the successful build. Earlier logs are retained as
`...-kernel-build.log` and `...-kernel-build-final.log`. No compile error was
accepted as a completed build.

All twelve test targets were relinked using the complete target list recorded
in the linked-selection checkpoint. Log:
`/tmp/blender-nurbs-native-region-regression-build-final.log`, exit 0.

```sh
python3 /tmp/blender-nurbs-run-native-region-kernel-checks.py
```

**193 passed, no skips**: counts 20/30/54/38/9/10/7/4/10/3/4/4 in the same target
order. Summary: `/tmp/blender-nurbs-native-region-kernel-summary.log`; suite logs:
`/tmp/blender-nurbs-native-region-{suite}-tests-final.log`. The same read-only
preview/selection/movement/duplication inputs and geometry asset argument from
the preceding checkpoint are used, with export unset.

```sh
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
python3 /tmp/blender-nurbs-run-native-region-ui-final.py
```

**5/5 CTest targets** and **9/9 isolated viewport tests** pass. CTest log:
`/tmp/blender-nurbs-native-region-ctest-final.log` (8.85 seconds).
The viewport driver repeats the native-duplication checkpoint's eight exact test
IDs/environment and adds `test_nurbs_line.line_to_sheet`. Logs:
`/tmp/blender-nurbs-native-region-ui-{line,duplicate,holes,native,linked-multiobject,move,legacy-region,legacy-delete,legacy-duplicate}-final.log`.
All 37 NURBS Python cases and the saved-source validators remain passing.
Both frozen input hashes remain unchanged.

These application checks establish that existing native and tagged-prototype
workflows still pass. They do **not** test a native E shortcut: it is still polled
out. No new visual claim is made for native region extrusion. The next geometry
adapter must admit complete ruled-boundary row movement along rectangular
interior curve trims through exact basis/parameter correspondence. Individual
trim-endpoint deformation, general curve fit-point changes and Normal/Gimbal
orientation remain their existing explicit gates. No ED-only CV approximation
will stand in for that shared geometry rule. Stage 1 remains incomplete.

## Complete ruled-row translation on rectangular trims — 2026-10-24

The shared vertex-translation service now proves a complete ruled-boundary row
request from its NURBS parameterization before using the existing coefficient
request/ownership/budget pipeline. It works in either parameter direction and
with interior trims or nonclamped bases in the other direction. It does not
interpret a trim endpoint as the first/last control coefficient.

Admissible faces have one rectangular four-use boundary, a complete clamped
linear direction with two rows, and exact boundary correspondence. Horizontal
p-curves preserve the curve parameter; their 3D curves match the support row's
basis, coefficients and projective weights. Support-row weight profiles must be
proportional. Rail p-curve weights may compose a rational linear mapping when
the exact projective products match. The existing exponent/GCD matcher avoids
intermediate product overflow/underflow and rejects one-bit differences.
Shared source corners bind rows to topology. All endpoints across each selected
row must agree on selection, and all owners of a shared resource must request
the same change. A measured boundary gap above declared tolerance rejects the
operation. General curved endpoint changes and other trimmed deformations
remain unsupported; no fitting or basis change is substituted. The prior
complete-tensor corner rule remains available for its existing cases.

These are exact source-basis correspondence checks plus the existing measured
endpoint tolerance and coefficient-addition budget. This does not introduce a
real-number evaluation-error certificate or a general surface/trim validator.
Weights, knots, p-curves, component IDs and unreferenced resources stay unchanged.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_transform_test blender --parallel 4
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_transform_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
```

All builds exit 0; **16/16 transform tests pass**, including six added tests.
Logs: `/tmp/blender-nurbs-trimmed-row-build.log`, `...-test-build-final.log`,
`...-build-final.log`, `...-tests-final.log`. The second command refines a test
fixture so each horizontal curve still matches its row while the two row weight
profiles differ; rejection cannot pass merely because a horizontal boundary was
left inconsistent. A cosmetic blank-line change was followed by the final
application build. No product change followed that build.

Coverage includes both axes, nonclamped bases, interior quarter-arc trims,
large/small power-of-two weight scales whose naive profile products overflow,
composed rational rail mappings, single-endpoint rejection, one-bit mismatch,
nonproportional rows, shared-owner conflict/compatible requests, untouched
orphan supports, and a gap beyond declared tolerance. Preview checks use an
explicit evaluation allowance; preview samples do not become editable source.

The twelve-test-target relink repeats the complete list above; log:
`/tmp/blender-nurbs-trimmed-row-regression-build-final.log`, exit 0.

```sh
python3 /tmp/blender-nurbs-run-trimmed-row-kernel-checks.py
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
python3 /tmp/blender-nurbs-run-trimmed-row-ui-final.py
```

**199 C++ tests passed, no skips**; suite counts
20/30/54/38/9/10/7/4/16/3/4/4 in the same target order. Summary:
`/tmp/blender-nurbs-trimmed-row-kernel-summary.log`; suite logs:
`/tmp/blender-nurbs-trimmed-row-{suite}-tests-final.log`.
Read-only preview/selection/movement/duplication inputs and the geometry asset
argument are unchanged. Both frozen input hashes remain unchanged.
**5/5 CTest** targets pass, including 37 NURBS Python cases; log:
`/tmp/blender-nurbs-trimmed-row-ctest-final.log` (8.90 seconds).
**9/9 isolated viewport tests** pass with the same IDs/environment as the region
checkpoint; logs: `/tmp/blender-nurbs-trimmed-row-ui-{label}-final.log`.
The saved-source validators pass again. No new visual claim is made for the
new trimmed-row cases; those cases are tested directly through the kernel.

The native E/Delete editor adapters are next. Normal/Gimbal frames, general
curve fit-point deformation, native creation/migration, edge/face picking and the
pending hole-linked selection choice retain their explicit gates. This rule
resolves the complete ruled-row construction dependency; it does not resolve
those separate contracts or complete Stage 1.

## Native E and Delete through shared component candidates

Implemented and built on top of `86358bde802` (complete ruled-row trim movement).
This checkpoint activates native-source editor extrusion and deletion; earlier
statements that these two editor paths are gated describe earlier checkpoints.
It does not activate general fit-point deformation, automatic face-normal E,
edge/face-interior picking or native primitive creation/migration.

`editnurbs.cc` now assembles Duplicate, Extrude and Delete candidates through a
shared editor helper. Geometry construction remains in BKE. The helper validates
normalized source selection and gathers component IDs, then calls the existing
kernel services. Copy/extrude remaps cap selection/history; delete prunes removed
identities and flushes surviving selection. All use the existing retained-token
owner commit, dirty/depsgraph/notifier path. No new operator or display-mesh edit
path was added. Existing legacy branches remain active.

- Native `CURVE_OT_extrude` dispatches to the shared adapter before legacy CV
  selection checks. Its existing `curve.extrude_move` macro supplies E followed by
  G. Topology is created at zero displacement; G uses that post-extrusion source
  snapshot. Esc retains the extrusion and cancels only movement, matching mesh
  interaction. Non-translation modes reject before mutation.
- Native `CURVE_OT_delete` offers Vertices, Edges, Faces and Only Faces via a
  contextual enum. Legacy Vertices/Segments values and identifiers remain stable;
  native-only modes cannot enter a legacy deletion branch. The enum callback also
  checks the context object's type before interpreting its data as a Curve.
- Delete preflights all native edit owners. E/Shift+D reject multiple edit owners
  before changing topology because the current native Move adapter supports one.
  The existing mode-entry rule still rejects mixed native/legacy sessions with
  either owner active. Component operators also check mixed sources defensively.
- Extrusion reports any measured pre-existing source endpoint discrepancy that is
  within declared tolerance. It does not fit, weld or silently change that input.
  The initial sweep has zero translation roundoff; modal Move retains its own
  explicit numerical bounds. Source beyond declared endpoint tolerance rejects.
- Removed topology does not trigger coefficient-resource garbage collection.
  Orphan resources remain valid source records with stable IDs/counters.

### Source and viewport evidence

The original reported sequence now also runs on the native payload:

| Step | Shared source V/E/F | Selected V/E/F |
|---|---|---|
| Initial face | 4 / 4 / 1 | 4 / 4 / 1 |
| E Z 1 | 8 / 12 / 6 | 4 / 4 / 1 |
| E Z 1 again | 12 / 20 / 10 | 4 / 4 / 1 |
| X, Enter (top vertices) | 8 / 12 / 5 | 0 / 0 / 0 |
| Box-select surviving rim | 8 / 12 / 5 | 4 / 4 / 0 |
| E Z 1 | 12 / 20 / 9 | 4 / 4 / 0 |

The result has an open top, as mesh vertex deletion would produce. Its last four
faces are connected walls; no detached cap or invalid legacy patch declaration
is created. Native source contains no legacy splines throughout.

The same test checks click/box/linked face selection, line-to-sheet and repeated
line sweep, a two-face region with six boundary walls and no internal selected
wall/rail, a hole, a periodic rational cylinder, an interior-trimmed rational arc,
Shift+D then E, E/Esc then G, undo/redo and all four deletion modes. Twenty-two
saved states are read by the C++ integration test and compared against complete
kernel-composed expected source: identities and sharing, control coefficients,
knots, weights, trim ranges, p-curves, face/loop/use orientation, material/tolerance
fields and stored selection/history/active data. This is stronger than comparing
display polygons or only topology counts.

A fresh Blender process reopened `try-native-extrude-delete.blend` directly in
Edit Mode and executed Python `curve.extrude_move` with precise Z = "1". Its
source statistics changed from 12/20/9 to 16/28/13, with the new four-vertex rim
selected. Source stayed native, with zero legacy splines. The input was not
saved over. Both its original viewport result and the reopened process's result
were visually inspected; the reopened extended top is partly outside that
camera's framing. Source/identity claims come from automated checks, not images.

See [native extrusion/deletion tryout](native_extrusion_tryout.md) for prepared
files and current interaction limits. These files are native fixtures; the
ordinary Add NURBS path still creates the restricted tagged prototype.

### Reproduced test failures and correction

The first expanded eleven-case viewport wrapper passed nine cases and failed two
new checks. The multi-owner Delete undo test had used a Python operator call
without requesting undo (the Python wrapper defaults to `false`). It therefore
undid the preceding selection operation. The test now calls
`curve.delete('EXEC_DEFAULT', True, type='ONLY_FACE')`; multi-owner delete,
undo/redo and edge cleanup pass without product undo changes.

The mixed-source test initially tried to enter an Edit Mode session that existing
code explicitly prohibits. It now tests that entry rejection through actual Tab
events with each owner active, checking that both remain in Object Mode and
both sources remain unchanged. It does not claim mixed-session editing support.

That rejection test also revealed a genuine 16-byte leak in the earlier native
mode-entry guard (`2fd4dfd9a15`). A guarded-allocation run identified
`object_bases_iterator_begin`. The early return inside
`FOREACH_SELECTED_OBJECT_BEGIN` skipped its iterator-end cleanup. The guard now
records the mismatch, breaks, executes `FOREACH_SELECTED_OBJECT_END`, and only
then reports/cancels. The guarded rejection regression passes without the
allocation warning. This fixes the earlier guard; it does not relax the source
ownership rule. Failed logs are retained rather than overwritten.

### Exact build and verification commands

All build commands used the same canonical build tree (the other build name is
only a symlink), `nice -n 10`, and `--parallel 4`. Product code was built before
running its viewport checks. Test-only edits did not require another application
build. The final mode-entry cleanup did require and receive a product rebuild.

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_object_test --parallel 4

nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blenkernel_nurbs_geometry_legacy_test blenkernel_nurbs_selection_test \
  blenkernel_nurbs_geometry_test blenkernel_nurbs_edit_test \
  blenkernel_nurbs_differential_test blenkernel_nurbs_tessellate_test \
  blenkernel_nurbs_mesh_test blenkernel_nurbs_object_test \
  blenkernel_nurbs_transform_test blenkernel_unit_test \
  editor_util_numinput_test editor_transform_transform_translate_precision_test \
  --parallel 4
```

Product build logs: `/tmp/blender-nurbs-native-component-build.log`,
`-build-final.log`, and `-mixed-cleanup-build.log` with the same prefix.
Twelve-target relink logs: `/tmp/blender-nurbs-native-component-regression-build.log`
and `-regression-build-final.log`. Each completed with exit 0. No compiler errors
were left unresolved.

The new fixture was explicitly exported to a new path, without overwriting the
frozen earlier fixture:

```sh
env BLENDER_NURBS_PREVIEW_EXPORT=/tmp/blender-nurbs-native-extrusion-fixture.blend \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test \
  --gtest_filter='*DependencyGraphDisplaysLiveSheetAndRetainsPreviousFrameSource'
```

Result: exit 0, one test passed. Export is unset in ordinary test runs. The fixture
adds a native wire and a trimmed rational arc to the existing sheet/hole/cylinder
samples. Its SHA-256 is
`1085cb447a543093900335f92cee77739fd839db5ae61e30cfde8d42ccc48538`.
The read-only earlier preview and original broken-file reproduction retain
`329f75e097dbcf7b580b7bdba5d8e619ac26ca812b9cd3ab38098907059f2e5a` and
`ea596ec1bfb326190b98fc54557cded623681d6269c3504f68ba06167b81362d`.

The C++ wrapper command was:

```sh
python3 /tmp/blender-nurbs-run-native-component-kernel-checks.py
```

It invokes each listed executable with `--gtest_color=no`; `nurbs_geometry` also
receives `--test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'`.
The edit suite receives these read-only inputs/output directories:

```text
BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend
BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui
BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui
BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui
BLENDER_NURBS_EXTRUSION_FIXTURE=/tmp/blender-nurbs-native-extrusion-fixture.blend
BLENDER_NURBS_EXTRUSION_OUTPUT=/tmp/blender-nurbs-native-extrusion-ui
```

| Executable suffix | Passed |
|---|---|
| `blenkernel_nurbs_geometry_legacy_test` | 20 |
| `blenkernel_nurbs_selection_test` | 30 |
| `blenkernel_nurbs_geometry_test` | 54 |
| `blenkernel_nurbs_edit_test` | 43 |
| `blenkernel_nurbs_differential_test` | 9 |
| `blenkernel_nurbs_tessellate_test` | 10 |
| `blenkernel_nurbs_mesh_test` | 7 |
| `blenkernel_nurbs_object_test` | 4 |
| `blenkernel_nurbs_transform_test` | 16 |
| `blenkernel_unit_test` | 3 |
| `editor_util_numinput_test` | 4 |
| `editor_transform_transform_translate_precision_test` | 4 |

Total: **204 passed, no skips**, exit 0. Per-suite logs are
`/tmp/blender-nurbs-native-component-<suite>-tests-final.log`; summary is
`/tmp/blender-nurbs-native-component-kernel-summary.log`.

The viewport wrapper command was
`python3 /tmp/blender-nurbs-run-native-component-ui-final.py`. It launches each
case below in its own factory-startup Blender process on an isolated Weston
server, using this exact argument/environment pattern:

```sh
nice -n 10 env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
  BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui \
  BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui \
  BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui \
  BLENDER_NURBS_EXTRUSION_FIXTURE=/tmp/blender-nurbs-native-extrusion-fixture.blend \
  BLENDER_NURBS_EXTRUSION_OUTPUT=/tmp/blender-nurbs-native-extrusion-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

| Label | TEST_ID | Result |
|---|---|---|
| line | `test_nurbs_line.line_to_sheet` | exit 0 |
| duplicate | `test_nurbs_native.duplicate_and_move_source_components` | exit 0 |
| holes | `test_nurbs_native.linked_selection_rejects_ambiguous_holes_without_partial_changes` | exit 0 |
| native | `test_nurbs_native.vertex_selection_and_lifecycle` | exit 0 |
| linked-multiobject | `test_nurbs_native.linked_selection_across_objects` | corrected test rerun exit 0 |
| move | `test_nurbs_native.vertex_move_and_lifecycle` | exit 0 |
| legacy-region | `test_nurbs_line.face_region_selection_gestures` | exit 0 |
| legacy-delete | `test_nurbs_line.extrude_delete_top_and_continue` | exit 0 |
| legacy-duplicate | `test_nurbs_line.duplicate_and_repeat_face_extrusion` | exit 0 |
| extrusion | `test_nurbs_native.extrude_delete_and_continue_source_components` | exit 0 |
| mixed | `test_nurbs_native.mixed_component_edits_reject_before_mutation` | corrected entry test and final guarded cleanup rerun exit 0 |

Eleven cases pass after corrections. Original wrapper failure logs remain in
`/tmp/blender-nurbs-native-component-ui-<label>-final.log`. Successful reruns are
`-ui-linked-multiobject-fixed.log` and `-ui-mixed-cleanup-final.log` with the same
prefix. The latter uses `--debug-memory` before `--enable-event-simulate`, and
exits without a leaked-allocation report.

Fresh reopen command used the same isolated server environment:

```sh
python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  /tmp/blender-nurbs-native-extrusion-ui/try-native-extrude-delete.blend \
  --python /tmp/blender-nurbs-reopen-extrusion.py
```

Result: exit 0; assertions above pass. Log:
`/tmp/blender-nurbs-native-component-reopen.log`.
The timer script checks native ownership, empty legacy splines and source V/E/F
counts before/after `curve.extrude_move(TRANSFORM_OT_translate={'value_precise_z': '1'})`.

```sh
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux \
  --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

Result: **5/5 passed**, exit 0; includes the unchanged **37 NURBS Python cases**.
Final log: `/tmp/blender-nurbs-native-component-ctest-final.log`
(after the guarded iterator cleanup rebuild); the earlier run also passed in
`/tmp/blender-nurbs-native-component-ctest.log`.
`git diff --check` and Python byte compilation also pass.

**Implemented:** native E/Delete routing through shared services and owner commit.
**Built:** application plus twelve relevant C++ test executables.
**Tested:** source readback, selection gestures, shared regions, repeat/delete/
continue, rational/trimmed geometry, modal cancel, undo/redo, multi-owner deletion,
rejection paths, original prototype compatibility, autosave and global undo.
**Visually confirmed:** original native delete/continue result and fresh reopened
continuation. Other cases have automated evidence, not individual visual signoff.
**Still gated:** fit-point data/constraints, broad transforms, source normal frames,
edge/face picking, native creation/migration, full component Python bindings,
general geometric/solid validity and resource collection. Stage 1 is incomplete.
