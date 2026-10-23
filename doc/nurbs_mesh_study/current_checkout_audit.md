# Current checkout audit

Baseline: `355a9ae11d7e39d75f1341322a70776fb206a6bc`,
“Improve topology-aware NURBS extrusion and selection.”
Working branch: `nurbs/shared-topology-foundation`.
The handoff was the only untracked file on arrival. No other local edits existed.

Latest completed checkpoint: Blender and eight targeted native test executables
build; **126 kernel/integration tests**, **25 NURBS Python tests**, and the
existing autosave and global-undo CTest targets pass. Five isolated viewport event
tests pass: native selection, prototype line/region, mesh undo and font undo.
Native surface topology-vertex selection now works through standard Edit Mode,
click, box, Select All, viewport Ctrl-Z/redo and save/re-entry. Saved-file checks
verify identical click/box component IDs and unchanged NURBS geometry. The actual
native selection screenshot has been visually inspected, as have earlier
sheet/hole/cylinder captures. The original prototype's diagnosed duplicate and
extrusion failures remain unfixed. Native extrusion and Edit Mode geometry
transforms are blocked until their adapters exist. Stage 1 remains incomplete.
Exact commands, earlier failed attempts and limits follow; historical checkpoint
sections describe the state at that time. See the final checkpoint below and
[selection tryout](native_selection_tryout.md) for the current supported subset.

`/home/molotovgirl/Desktop/build_blender_5.2.2` is a symlink to `build_linux`;
these are two names for the same build, not independent comparison binaries.

## Evidence labels

* **Source**: inspected in this checkout.
* **Automated**: command actually executed; result recorded below.
* **Interactive**: visually inspected Blender UI, separate from event simulation.
* **Proposed**: intended architecture or behavior.
* **Unknown**: not established by this audit.

## Baseline capabilities

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
