# Native NURBS edge bevel and connected corner repair

This implements the user's first bevel workflow: extrude a line into a face,
extrude its boundary into a second face, select their shared edge and press
Ctrl+B. The same construction also repairs planar end faces on a closed box.
The initial independent-edge implementation failed on connected selections.
The repair described here replaces its geometry path with shared fan planning;
it does not choose an external CAD kernel.

Latest follow-up: [ruled curved surfaces and fresh-runtime checks](native_bevel_ruled_surface_fix.md).
The [Ctrl+B input correction](native_bevel_input_fix.md) shares mesh bevel controls.
The preceding [flat-face correction](native_bevel_planar_chart_fix.md) covers reshaping.
The [angled corner blends](native_bevel_corner_blends.md) record the earlier corner checkpoint.
The earlier [width limiting and rotated rounded corners](native_bevel_robustness.md) remains
the validation record for that checkpoint.
Clamp Overlap now starts enabled by the user's choice, with C to toggle it.
Rounded right-angle corners accept certified source-precision rotation errors.
The report records the current build, source checks, viewport tests and remaining limits.

## Interaction and geometry

Ctrl+B previews the actual editable NURBS candidate. Mouse motion controls
width, numeric entry uses Blender's double input channel, Shift gives precision,
Ctrl snaps the width increment, and the wheel changes the segment count.
Enter/LMB confirms; Esc/RMB restores the original source and selection. The
operator preserves the current V/E/F mode and stays in Edit Mode.

One segment creates a flat chamfer. Two or more partition a smooth positive
rational quadratic profile into editable strips. Increasing the count changes
the topology of that same round rather than approximating it with flat faces.
The partition uses uniform rational parameters, not equal-angle stations.
This is the stated NURBS shape policy; it intentionally differs from mesh
bevel's polygon strips. Planar chamfer topology and coordinates are compared
with the actual BMesh bevel operator.

The operation accepts straight shared edges between convex polygon boundaries on
clamped polynomial bilinear supports whose measured deviation fits the explicit
construction-error budget. It plans selected edge networks together,
including shared corner stations, higher-valence planar chamfer fans, and miter
reconstruction through the existing topology quotient service. Closed boxes can
be beveled with every edge selected; a box merged to a pyramid supports planar
chamfers on supported edge subsets. Some partial selections require a nonplanar
corner cap with more than four sides; those remain explicitly unsupported.
Unselected end faces can have concave boundaries.
Wire/boundary edges in a larger selection are skipped, as in mesh bevel.
Unsupported inputs reject the complete candidate and confirmation reports why.

A single natural shared edge on compatible curved ruled supports now has a
[native isocurve/conic-blend path](native_bevel_ruled_surface_fix.md), including
planar end caps. It preserves the original curved supports and regular rail
tangent planes. Width uses the source midpoint frame; general constant-radius
filleting, curved end-cap intersections and connected curved networks remain pending.

Rounded connected fans currently require valence three. Fully selected right-angle corners
within the recorded source/arithmetic enclosure use a positive
rational spherical-octant support with a certified radial error bound, sharing its arc boundaries with the bevel
strips. Nonorthogonal three-edge corners now use positive boundary-interpolating rational
patches with exact shared conics; unequal two-edge junctions plan one common profile.
Those general blends certify G0 connection, not G1 or constant radius. See the latest follow-up.
There is one NURBS face per fully selected corner, rather than BMesh's polygon grid.
With three segments a full cube therefore has 72 vertices/120 edges/50 faces;
the actual mesh reference has 96/192/98. This is an intentional source geometry
and topology difference, not a conversion to a display mesh.

Loop-slide widths use a least-squares objective modeled on BMesh: strong
opposite-end agreement and a 0.2-weight requested-width preference, with hard
ratios at shared on-edge stations. The computed solution has an explicit
forward-error enclosure. In some partial pyramid selections, BMesh's chain
traversal leaves asymmetric offsets; the native global objective preserves
symmetry. That is the current working policy pending user feedback. Tests do
not relax coordinate tolerances to hide this difference: every cube subset
compares geometry with BMesh, while pyramid subsets compare topology and
single-edge/full selections additionally compare coordinates.

Vertex bevel (Shift+Ctrl+B), general curved adjacent supports, rounded
higher-valence corner blends, custom profiles,
and alternative width types remain unimplemented. Widths must stay inside the
neighboring boundaries without rail overlap; the shared width planner now limits
the selected network before its local rail/boundary collapses. This is a restricted native
geometry operation, not general bevel parity or an external kernel decision.

## Shared services and source authority

`BKE_nurbs_topology_edit.hh` exposes `bevel_edges` with explicit offset,
segments and construction-error budget. It owns planning, candidate geometry,
identity maps, boundary repair and validation. Surviving neighboring edges and
faces retain their IDs; support surfaces remain unchanged unless planar miter
reconstruction requires replacement through the shared merge service. Each new
boundary curve has one shared edge identity across its incident faces.
Removed sharp-edge/corner identities map to zero rather than an arbitrary
one-to-many replacement.

Reusable construction services support the operation:

* `reparameterize_affine_planes` certifies whole-support planarity and gives affected flat
  non-affine faces private affine charts, preserving their spatial curves and topology. This
  covers ordinary trapezoidal faces after resizing a box cap, within the explicit geometry budget.

* `ruled_surface` exactly copies two matching positive-rational curve rows into
  a tensor support. It also supplies a future bridge/loft construction primitive.
* `project_curve_to_affine_surface` changes the coordinates of a spatial curve
  into a planar support's parameter space while preserving its basis and weights.
  Its whole-curve certificate includes bilinear support twist and roundoff.
  Parameter values just outside the support domain are snapped only when the
  mapped geometric correction and complete curve/support deviation fit the
  explicit error budget. Larger errors reject the candidate. This is not a
  fitting service.
* `rational_octant_surface` constructs an affine image of a unit spherical
  octant, with positive rational weights and a bounded construction error.
* `rational_spherical_octant_surface` additionally bounds the whole support's
  radial deviation from its intended sphere using the axes' Gram matrix. It
  preserves the supplied axes and rejects an exceeded error budget.

A collapsed natural boundary can close a face's parameter chart at an exact
source pole without inventing an editable edge. Tessellation still rejects
general open parameter loops. At such poles the mesh evaluator uses the
interior limiting normal from the adjacent rational coefficient row; it does
not perturb parameters or use a display-mesh normal as source geometry.

Rounded strips use exact NURBS representations of the intended analytic
profile, subject to explicitly bounded floating-point construction roundoff.
Profile restriction reuses the existing refinement service. Source weights,
knots, domains and unused source resources are retained. No display mesh is
read as edit geometry, no proximity weld occurs, and existing tolerances are
not silently enlarged. The candidate reports its construction bound and
rejects when the requested budget cannot cover it.

Boundary evaluation now recognizes matching natural tensor boundaries and
certifies matching-basis positive-rational curves on exactly affine supports.
It keeps measured sample deviations separately; the certificate proves source
agreement without treating evaluator roundoff as a geometric gap. General
intersection or trim-validity certification is not implied.

The bevel projection path additionally accepts a nearly affine bilinear support
when a conservative whole-curve bound, including its mixed corner coefficient,
fits `max_error`. This handles ordinary rotation/transform roundoff without
silently flattening a real warp. The NURBS remains authoritative; the reported
construction bound is stored with the created face/use tolerances, and an
over-budget projection still fails explicitly.

The editor orchestrates the shared source-preview transaction. Every frame is
constructed from the retained original, preflights the owner's actual preview
resolution, and uses the normal native commit/evaluation/draw pipeline. Cancel
restores geometry, source ID counter, selection/history and dirty state while
runtime cache revisions remain monotonic. Existing Loop Cut callers retain
their final-commit path through the same preview owner.

Python operator access is available in Edit Mode:

```python
bpy.ops.curve.bevel(offset=0.25, segments=3, max_error=1e-9)
bpy.ops.curve.bevel(offset_exact="0.3", segments=1, max_error=1e-9)
```

The hidden exact-width string preserves double replay/API input; an explicitly
changed ordinary width takes precedence. A zero-width execution cancels without
committing. This does not add a public Python component data model.

## Validation commands

Build and install from the Blender source root:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux \
  --target blender blenkernel_nurbs_topology_edit_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux \
  --prefix /home/molotovgirl/Desktop/build_linux/bin
```

The isolated viewport command is run once for each function listed below:

```sh
env -u DISPLAY \
 BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
 WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
 WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
 WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
 python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
 --python tests/python/ui_simulate/run_blender_setup.py -- \
 --tests test_nurbs_native.bevel_corner_workflow
```

Functions: `bevel_corner_workflow`, `bevel_box_workflow`,
`bevel_round_workflow`, `bevel_round_box_workflow`, `bevel_vertex_workflow`,
and the actual mesh reference `bevel_mesh_workflow`.

These save before/cancelled/finished/restored/continued states under
`/tmp/blender-nurbs-bevel-ui`. The saved-source test checks all source records,
ID counters, selected IDs, active/history and selection mode. It replays the
kernel operation from the saved input, checks zero-extra-allowance evaluation,
and compares the planar open-corner result and continuation with the actual
mesh files. Closed outputs retain two face uses on every shared edge.

Continuation explicitly cancels E's initial movement and uses G Z 0.5 on both
sources. The first saved-file comparison exposed that mesh E initially uses a
normal constraint while native E still lacks it; identical subsequent Z keys
therefore did not request the same displacement. Automatic native face-normal
extrusion remains an existing limitation, not a bevel fix or a claimed parity.

## Connected-network checkpoint validation

The original independent-edge checkpoint passed 234 kernel tests, six isolated
viewport workflows and five CTest cases. The user's subsequent report exposed
an untested connected-corner case: selecting all edges of a newly extruded
native box and pressing Ctrl+B left its 8/12/6 topology unchanged. The same
interaction on the mesh reference created 24/48/26. The initial native repro
log is `/tmp/nurbs-bevel-connected-repro.log`.

The connected-network Blender build and installation succeeded. All 14 isolated
viewport functions returned exit 0 and printed their `NURBS_BEVEL_*_PASS` marker.
The command above is unchanged; substitute these function names:

* `bevel_corner_workflow`, `bevel_box_workflow`, `bevel_round_workflow`,
  `bevel_round_box_workflow`, `bevel_vertex_workflow`, `bevel_mesh_workflow`;
* `bevel_mouse_workflow`, `bevel_mesh_mouse_workflow`;
* `bevel_connected_edges_workflow`, `bevel_mesh_connected_edges_workflow`;
* `bevel_pyramid_workflow`, `bevel_mesh_pyramid_workflow`;
* `bevel_connected_round_workflow`, `bevel_mesh_connected_round_workflow`.

Logs are `/tmp/nurbs-final-FUNCTION.log`. Each process uses factory startup and
its own Weston instance. The connected cases check preview, exact cancellation
topology, confirm, undo/redo, mode preservation and Edit Mode re-entry. Their
before/cancelled/finished/restored files are under `/tmp/blender-nurbs-bevel-ui`.
The chamfered box, chamfered pyramid and rounded connected box screenshots were
visually inspected. They show joined corners and clean shading. This visual
check does not claim coverage of arbitrary materials, views or geometry.

The pyramid sweep exercises all 255 nonempty edge subsets. Eight subsets
(60, 61, 83, 87, 163, 171, 204, 206 in the fixture's stable edge order) require
unsupported nonplanar caps; the test requires that exact rejection list and
checks source preservation. The other 247 compare topology against actual
BMesh. Independent edges and the complete selection also compare coordinates.
A separate reflection check protects the deliberate symmetric width policy.
Nine representative connected rounded cube selections check closed shared
incidence, consistent orientation and zero-extra-allowance tessellation.

The rounded full-box check additionally compares every derived preview point
with the analytic rounded-box radius, and every display corner normal with its
analytic outward normal. It requires zero undefined normal corners. The
collapsed-chart regression accepts only an exactly collapsed natural boundary
and still rejects a gap into the parameter-chart interior.

The final regression gate build and full saved-source run returned exit 0.
The saved-source test checks exact original/cancelled and finished/restored
records, selection/history/mode, and kernel replay from saved input for the
single-edge, connected box, pyramid and rounded connected box workflows.
Planar outputs also compare with the actual mesh viewport files.

| Suite | Passed |
|---|---:|
| Topology editing | 46 |
| Geometry | 55 |
| Mesh evaluation | 18 |
| Tessellation | 12 |
| Fill | 18 |
| Transform | 26 |
| Edit lifecycle | 60 |
| Object ownership | 4 |
| Differential evaluation | 9 |
| **Total** | **248** |

All suites returned exit 0, with no skipped tests. The full topology log is
`/tmp/nurbs-connected-topology_edit-regression.log`. Python compilation,
`git diff --check`, and clang-format `--dry-run --Werror` on changed C++ files
also passed. BMesh reference code is unchanged. The protected user handoff
retains SHA-256
`8f8fcc63d9fa66ce55eda5673826dbfd9f1da2f4cdb9696b5bbb52916f9d5d9a`.

The local binary is `/home/molotovgirl/Desktop/build_linux/bin/blender`.
All viewport tests create their own shapes using factory startup in isolated
Weston instances; they do not read the user's open project. An already running
Blender process needs restarting to load a rebuilt executable. This work is
currently uncommitted and unpushed.

## Regression reproduction

The complete build command for the repaired construction was:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target \
 blender blenkernel_nurbs_topology_edit_test blenkernel_nurbs_geometry_test \
 blenkernel_nurbs_mesh_test blenkernel_nurbs_tessellate_test \
 blenkernel_nurbs_fill_test blenkernel_nurbs_transform_test \
 blenkernel_nurbs_edit_test blenkernel_nurbs_object_test \
 blenkernel_nurbs_differential_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux \
 --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Build and installation returned exit 0. Logs:
`/tmp/nurbs-bevel-connected-final-build.log` and
`/tmp/nurbs-bevel-connected-install.log`. The final test-only rebuilds are
`/tmp/nurbs-bevel-expanded-tests-build.log` and
`/tmp/nurbs-bevel-final-regression-gate-build.log`.

Each kernel executable is run as
`/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_SUITE_test --gtest_color=no`.
Geometry additionally receives
`--test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files'`.
Per-suite logs are `/tmp/nurbs-connected-SUITE-regression.log`.
The topology saved-source gates are:

```sh
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui \
 BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 \
 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 \
 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui \
 BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui \
 BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition \
 BLENDER_NURBS_TRIANGLE_OUTPUT=/tmp/blender-nurbs-triangle-ui \
 BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-bevel-ui \
 BLENDER_NURBS_BEVEL_UI_READY=1 \
 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
```

Edit lifecycle uses these fixture variables:

```sh
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend \
 BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui \
 BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui \
 BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui \
 BLENDER_NURBS_EXTRUSION_FIXTURE=/tmp/blender-nurbs-native-extrusion-fixture.blend \
 BLENDER_NURBS_EXTRUSION_OUTPUT=/tmp/blender-nurbs-native-extrusion-ui \
 BLENDER_NURBS_COMPONENT_OUTPUT=/tmp/blender-nurbs-component-ui \
 BLENDER_NURBS_FILL_OUTPUT=/tmp/blender-nurbs-fill-ui \
 BLENDER_NURBS_MODE_ENTRY_OUTPUT=/tmp/blender-nurbs-normal-mode-ui \
 BLENDER_NURBS_AFFINE_OUTPUT=/tmp/blender-nurbs-rotate-scale-ui \
 BLENDER_NURBS_AFFINE_PARITY_OUTPUT=/tmp/blender-nurbs-rotate-scale-parity-ui \
 BLENDER_NURBS_SPLIT_OUTPUT=/tmp/blender-nurbs-split-ui \
 BLENDER_NURBS_SEPARATE_OUTPUT=/tmp/blender-nurbs-separate-ui \
 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test --gtest_color=no
```

The composition and legacy chain command is:

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
 -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
```

This returned exit 0, 5/5 passed. Log: `/tmp/nurbs-connected-ctest.log`.


## Mouse preview stability follow-up

The user reported the bevel alternating between beveled and sharp while
moving the mouse. A fresh L-shaped source reproduced the reset: a valid
16-pixel move produced 8/10/3 topology, while an oversized 240-pixel move
restored the original 6/7/2. Screenshot and failing log:
`/tmp/blender-nurbs-bevel-ui/bevel-native-corner-mouse-limits-limits-failed.png`
and `/tmp/nurbs-jumpy-bevel_mouse_limits-repro.log`.

The cause was in editor preview orchestration: every failed candidate called
`cancel_preview`, even when a valid candidate was already displayed. The
geometry service's rejection was correct; resetting the accepted preview was
the unwanted behavior. The editor now retains the last accepted source on a
failed request, reports the reason and identifies the previous width/segments
being displayed. Confirmation still rejects the failed request. Explicit
cancel or a zero-width request restores the original. This does not implement
Mesh's overlap clamping or hide a failed geometry/tolerance check.

Mesh bevel handles only the final `MOUSEMOVE` in a queued batch. The native
operator now follows that rule, skipping `INBETWEEN_MOUSEMOVE` events instead
of rebuilding source and display geometry for every queued intermediate point.

Build/install commands returned exit 0:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux \
 --prefix /home/molotovgirl/Desktop/build_linux/bin
cmake --build /home/molotovgirl/Desktop/build_linux \
 --target blenkernel_nurbs_topology_edit_test --parallel 4
```

Logs: `/tmp/nurbs-bevel-mouse-stability-build.log`,
`/tmp/nurbs-bevel-mouse-stability-install.log`, and
`/tmp/nurbs-bevel-mouse-saved-test-build.log`.

The isolated viewport command above is used with these added functions:
`bevel_mouse_sweep_workflow`, `bevel_mesh_mouse_sweep_workflow`,
`bevel_round_mouse_sweep_workflow`, `bevel_decimal_mouse_sweep_workflow`,
`bevel_mouse_limits_workflow`, and `bevel_round_mouse_limits_workflow`.
The sweep checks every integer pixel from 8 through 64 and back (113 positions),
including ordinary and decimal-sized faces, with chamfer and round previews.
The mesh reference needs at least its five-pixel activation margin; an initial
four-pixel sample stayed at zero width as expected for that input policy.

The limit tests alternate small and oversized mouse positions, send a queued
intermediate zero-width event, explicitly request zero width, cancel a held
preview, reject invalid confirmation, then confirm, undo/redo, re-enter Edit
Mode and continue extruding. Both limit workflows passed on the repaired
binary. The rounded held-preview screenshot was visually inspected; the
bevel remains present and the status explains the held width.

The saved-source test additionally reads each limit workflow's `last-valid`
and `held-invalid` files and requires exact equality of geometry, identity
counter and selection/history/mode. It compares `held-cancelled` and `rejected`
with `before`. Run the full topology command above with the additional
environment assignment `BLENDER_NURBS_BEVEL_MOUSE_UI_READY=1`.
That run returned exit 0: 46/46 passed, no skips. Log:
`/tmp/nurbs-bevel-mouse-topology-regression.log`.

A further measured sweep reads the derived display mesh at every sample. The
base face's visible rail must advance with every increasing pixel position,
and return to the same width on the reverse sweep. This checks actual size
changes in addition to component counts. It does not use the display mesh as
edit geometry. The limit test also requires identical display coordinates at
an invalid request and the preceding accepted width.

Final viewport regressions returned exit 0, 9/9 passed:

* Measured drag/limit cases: `bevel_mouse_sweep_workflow`,
  `bevel_round_mouse_sweep_workflow`, `bevel_decimal_mouse_sweep_workflow`,
  `bevel_mouse_limits_workflow`, `bevel_round_mouse_limits_workflow`.
  Logs: `/tmp/nurbs-jumpy-FUNCTION-measured.log`.
* Existing interaction regressions: `bevel_corner_workflow`,
  `bevel_box_workflow`, `bevel_round_box_workflow`,
  `bevel_connected_round_workflow`.
  Logs: `/tmp/nurbs-jumpy-FUNCTION-fixed.log`.

All printed `NURBS_BEVEL_*_PASS`. Each measured sweep covered 113 positions
with strictly increasing visible width on the outward drag and matching widths
on the return. The earlier isolated mesh sweep also passed, logged in
`/tmp/nurbs-jumpy-bevel_mesh_mouse_sweep_workflow.log`.

Python compilation, clang-format `--dry-run --Werror` on the changed C++ files,
and `git diff --check` passed. The binary remains
`/home/molotovgirl/Desktop/build_linux/bin/blender`; restart an existing instance
to load this change. This follow-up is uncommitted and unpushed.
