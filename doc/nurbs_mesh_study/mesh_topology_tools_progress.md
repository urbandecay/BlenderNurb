# Mesh topology tools: Rip, Merge, Ctrl+R and Subdivide

User authorization: implement these in sequence without requiring another prompt.
The Y Split and P Separate changes from preceding slices remain in the worktree.
No extra agents are used. The user-owned `sol_nurbs_editing_handoff.md` stays untouched.

## Architecture

`BKE_nurbs_topology_edit.hh` defines a common independent topology result:
authoritative geometry, source-to-result mapping, semantic output components and
reported construction error. `nurbs_topology_edit.hh` is a private mutable
candidate builder with checked identity allocation and independent resource
cloning. Geometry operations never mutate live owners. BKE edit services map
selection/history and validate candidates. ED shares exact tagged entry,
all-owner preflight, common commit/notification and source precision reports.
Operators/keymaps only orchestrate those services and Blender interaction.

Rip changes vertex/edge incidence and isolates affected supports without changing
shape. Merge must update incidence and repair collapsed boundaries; it must not
remove a closed rational edge solely because its endpoint IDs agree. Real cuts
must partition face loops, split every incident use of a shared edge and create
selectable source identities while retaining the original NURBS support.

## Rip checkpoint (in validation)

Mesh references: `edbm_rip_invoke__vert`, `edbm_rip_invoke__edge` and local
face-fan splitting in `editmesh_rip.cc`. A vertex Rip separates the loops beside
the nearest incident edge; an edge-path Rip extends seam ends through the
opposite fan. Selected faces, single-face corners and non-manifold layouts have
explicit rejection rather than source corruption. Wire junctions detach one
branch. Cursor proxies sample source curves; source geometry is not sampled/fitted.

Implemented: BKE `rip_components`, independent curve/support ownership, common
result selection/active/history mapping; shared ED preflight; `curve.rip` and
`curve.rip_move` with Blender Move, V binding and native context menu. The old
Bezier V binding remains available when the NURBS poll does not apply.

Before committing V, ED also checks the current partial-deformation contract so
unsupported curved movement rejects before leaving an immovable coincident tear.
The topology service itself performs no deformation. Fill Rip (Alt+V), branched
paths and ambiguous periodic seam fans are outside this initial Rip contract.

Exact build/install commands used:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

The new test target first became available after CMake regeneration; an initial
make invocation had already parsed the older target table. The first production
compile caught a missing brace in selection mapping, which was corrected. A
fixture file-read test initially lacked a global Main and was fixed in test setup.
The mesh comparison caught both an invalid orientation assumption and the
single-vertex fan rule; Rip now follows incidence and the actual vertex-specific
mesh rule. Directional cursor points avoid an exact tie between two candidate
fan directions in the grid comparison. Final passing checks supersede initial
attempts and are recorded below when complete.

For viewport tests, use this exact environment and command, replacing TEST_ID:

```sh
nice -n 10 env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

```text
test_nurbs_native.rip_mesh_reference
test_nurbs_native.rip_native_workflow
```

The kernel test writes exact native fixtures for the viewport, compares moved
kernel results against saved mesh topology and checks atomic failures. Run the
mesh reference first, then kernel fixtures, then native viewport, then the full
saved-source comparison:

```sh
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test \
  --gtest_filter='*RipFans*:*RipRejects*' --gtest_color=no
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui \
  BLENDER_NURBS_TOPOLOGY_UI_READY=1 \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
```

## Next gates

1. Finish Rip's saved-source comparison and visual check; build/tests must pass.
2. Implement M Merge modes using shared vertex-group welding and explicit
   collapsed-boundary repair. Check actual BMesh results, circles, failure,
   source selection, undo/redo and file continuation. Build and test before cuts.
3. Implement shared exact edge/face partition services, source ring planning,
   Ctrl+R preview/confirmation/slide and compatible Subdivide. Retain supports;
   report arithmetic error and reject unsupported trim layouts before mutation.
   Compare real mesh cuts and validate curved rational source geometry.

Keep unresolved curved fitting behavior explicit. Fit-point interaction remains
planned after this requested topology slice; it is not activated by these tools.

### Rip gate passed

Final application/test build: `/tmp/blender-nurbs-rip-fan-build.log`, exit 0.
Native event workflow: `/tmp/blender-nurbs-rip-fan-final-ui.log`, exit 0, five
cases (VERT/EDGE/WIRE/GRID_VERT/GRID_EDGE), with V/Move cancel, undo/redo, save
and continued extrusion. Mesh reference: `/tmp/blender-nurbs-rip_mesh_reference-directional.log`,
exit 0, six layouts including the rejected single-face corner.
`/tmp/blender-nurbs-rip-stage-passed.log`: 3/3 C++ tests pass, zero skips,
including all five saved native sources versus actual mesh topology.
Visually inspected `/tmp/blender-nurbs-topology-ui/rip-native-GRID_EDGE.png`.
The Rip gate passed before starting Merge implementation.

## Merge implementation (viewport validation underway)

`nurbs_topology_merge.cc` implements explicit disjoint vertex-group welding,
shared source transforms, exact target identity, constant-edge removal,
representation-equivalent edge coalescing with p-curve reversal, and planar
collapsed-boundary repair. Pinched straight loops decompose into simple cycles,
mirroring BMesh's repeated-vertex face splitting. Supports/curves with distinct
geometry remain distinct; a closed rational circle is not a collapsed edge.
Preexisting repeated seam vertices are distinguished from newly pinched corners.
Equivalent duplicate faces require matching support and parameter boundaries.
Edge/face candidate lookup is bucketed rather than globally quadratic.

`merge_selected_vertices` supplies Center, Cursor, First, Last and Collapse
planning. First/Last require selected vertex history; Collapse groups connected
selected edges. ED uses the common topology preflight/result commit and converts
the world cursor into double object coordinates, rejecting singular transforms.
The M popup mirrors the five mesh modes, exposing First/Last when history permits.
Maximum Error is an explicit source-construction budget; nonzero roundoff is
reported. No distance welding or automatic merge has been activated.

The first Merge tests caught loose result-point selection surviving in Face
mode. Common result mapping now seeds only enabled component domains before
normalization, matching mesh deselection after collapse outside Vertex mode.
The initial viewport test used direct Python execution for non-Center modes;
that did not push an undo step in this event harness. The corrected test uses
real M/Home/Down/Enter popup events for all modes.

Builds passed: `/tmp/blender-nurbs-merge-tested-build.log`,
`merge-selection-build.log` and `merge-final-build.log` under the
`/tmp/blender-nurbs-` prefix. `/tmp/blender-nurbs-merge-install-final.log`
records exit 0. An earlier install attempt overlapped an isolated headless test
running the same binary and could not update its RPATH; it passed after that
process exited. No user process was closed.

`/tmp/blender-nurbs-merge-reviewed-kernel.log`: 6/6 kernel/saved-Rip tests pass,
including seven complete Merge layouts matching actual mesh source files,
rational seam/circle welding, component modes and rejected curved deformation.
The native Merge saved-source test remains gated until its UI files are ready.

Additional viewport IDs using the environment above:

```text
test_nurbs_native.merge_mesh_reference
test_nurbs_native.merge_native_workflow
```

Run final Merge comparisons with:

```sh
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui \
  BLENDER_NURBS_TOPOLOGY_UI_READY=1 \
  BLENDER_NURBS_MERGE_REFERENCE_READY=1 \
  BLENDER_NURBS_MERGE_UI_READY=1 \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
```

### Merge gate passed

Final build: `/tmp/blender-nurbs-merge-final-build.log`, exit 0.
`/tmp/blender-nurbs-merge-stage-passed.log`: 7/7 C++ tests pass, zero skips,
including all saved Rip and seven saved Merge sources versus actual mesh.
`/tmp/blender-nurbs-merge-mouse-menu-ui.log`: seven native M popup cases pass
with real mouse menu choices, undo/redo, saving and continued vertex extrusion.
Popup memory and real row positions are respected; automated Home/number-key
attempts had not activated entries and are superseded by these mouse checks.
`/tmp/blender-nurbs-merge-tagged-ui.log`: tagged Add/E/M promotion, undo/redo,
continued E and atomic multi-owner modifier preflight all pass.
Visually inspected `/tmp/blender-nurbs-topology-ui/merge-native-CURSOR.png` and
the actual M popup capture. No user Blender instance was closed.

The current transform geometry contract still restricts partial curved/trimmed
support deformation, including some further partial edits of newly planar-trimmed
triangular faces. Whole-face transforms/extrusion and the tested continued vertex
extrusion remain supported. A general planar/trim deformation service is separate
from silently fitting those cases. Distance merge, auto-merge, source UV attributes
and arbitrary curved-collapse repair remain planned.

The Merge gate passed before starting cut/refinement implementation.

## Ctrl+R and Subdivide: shared refinement and partition services

`BKE_nurbs_refine.hh` owns bounded homogeneous knot refinement, curve/support
restriction and source isocurve extraction. `OwnedCurve` moved from the fitting
header to geometry ownership; `OwnedSurface` supplies the corresponding RAII
owner. These services do not fit sampled geometry. Outward arithmetic encloses
construction roundoff and preserves provable equal-weight/constant-coordinate
identities. In particular, a rational cylinder's horizontal circular section
retains its circle coefficients and weights at both dyadic and arbitrary tested
heights. A nonzero construction bound is reported rather than hidden.

`BKE_nurbs_topology_edit.hh` now exposes ring planning, a shared edge/face
partition plan/result and subdivision planning. Intersections use logical source
incidence and explicit edge parameters; coincident coordinates do not establish
identity. Every use of a split edge follows the same partition, including
unselected neighboring faces and repeated periodic seam uses. Child faces own
shape-preserving restricted subpatches in the original support parameter space.
One original edge/face identity survives each partition; descendants are returned
for selection/history and future consumers. Candidate construction is independent
and checked against an explicit error/identity/resource budget before commit.

The editor's `TopologyPreview` keeps the original source token and an independent
tagged-entry candidate. Ctrl+R hover/count/slide does not change live source or
promote an owner. First-stage Esc/RMB discards the candidate; Esc/RMB during slide
commits a centered cut, as mesh slide cancellation does. Final confirmation uses
the common validated owner commit once. Preview drawing samples source boundary
curves only for display; neither it nor a tessellated mesh becomes edit authority.
Numeric input uses Blender's shared double NumInput. Precise replay is retained
alongside the visible float operator property. A visible redo-property change
supersedes the prior exact replay value.

The mesh reference caught reversed numeric slide sign in the first editor pass;
the editor now converts its mesh-style slide direction into kernel parameter
direction. Subsequent saved-source comparisons match the actual mesh. Initial
test compilation caught a misnamed selection-reference type and was corrected.
The kernel construction/cell identity estimate was also tightened before final
validation. The runtime string RNA defaults now use Blender's null default
convention, removing startup definition errors from the initial implementation.

### Verified cut contracts

- Kernel strip cuts cross multiple faces with real shared intersections.
  A closed cut around an extruded box produces 12 vertices/20 edges/10 faces;
  every edge retains two incident uses and the new ring remains movable.
- Full-face subdivision creates shared editable grids; a new center point moves
  all four incident subpatch corners. Subdividing a box's top updates every
  neighboring side's boundary uses. Rational periodic cylinders retain circles
  and repeated seam identity. Nonuniform/nonclamped refinement, independent
  long-double rational evaluation and insufficient-budget rejection are tested.
- Five real Ctrl+R workflows: centered Edge, numeric slide Edge, multiple cuts
  from Vertex mode, Face mode, and tagged-entry Vertex mode. Source stays
  unchanged during hover and first-stage cancel; slide cancel, undo/redo, saving,
  G, continued face extrusion and Edit Mode retention pass.
- Four actual menu/search Subdivide workflows: full face, opposite edges,
  wire and tagged entry; undo/redo, saving, G and continued extrusion pass.
- Five actual mesh references and five reopened native files compare complete
  vertex/edge/face incidence. The comparison rounds coordinates to 1e-6 only
  because Mesh stores float coordinates; source construction error is separately
  checked in double and is not derived from that comparison threshold.

Use the earlier viewport command with these test IDs, **one ID per process**:

```text
test_nurbs_native.cuts_mesh_reference
test_nurbs_native.loopcut_native_workflow
test_nurbs_native.subdivide_native_workflow
```

The event harness starts generators asynchronously; passing several IDs in one
invocation made test contexts interfere. That attempt is superseded by the
separate successful runs. Final cut viewport logs:
`/tmp/blender-nurbs-cut-mesh-reference-ui.log`,
`/tmp/blender-nurbs-loopcut-direction-ui.log`,
`/tmp/blender-nurbs-subdivide-ui.log`. No user process was closed.

Run the complete saved-source comparison with:

```sh
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui \
  BLENDER_NURBS_TOPOLOGY_UI_READY=1 \
  BLENDER_NURBS_MERGE_REFERENCE_READY=1 \
  BLENDER_NURBS_MERGE_UI_READY=1 \
  BLENDER_NURBS_CUT_UI_READY=1 \
  /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
```

`/tmp/blender-nurbs-cut-stage-saved.log`: **16/16 pass, zero skips**, exit 0.
Visually inspected the source cut preview, two-cut preview, continued
slide/extrusion and full-face Subdivide/extrusion captures:
`loopcut-MULTI-two-cuts-preview.png`, `loopcut-SLIDE-continued.png` and
`subdivide-FULL-continued.png` in `/tmp/blender-nurbs-topology-ui`.

### Python access

Registered editing operators are available through `bpy.ops`, using the same
kernel and geometry restrictions as their UI. For example:

```python
bpy.ops.curve.merge(type='CENTER')
bpy.ops.curve.subdivide(number_cuts=1)
bpy.ops.curve.loopcut_slide(edge_id=known_source_edge_id_as_string, number_cuts=2)
```

These examples require an appropriate edited owner/selection. Interactive Rip
requires invocation in a 3D View with a cursor, like mesh Rip. Loop Cut also has
noninteractive execution when a source seed ID is supplied. The seed ID string
avoids truncating persistent 64-bit identity to an RNA int. This is operator
access, **not** a completed direct `bmesh`-style Python component API; public
component enumeration, fit-point constraints and safe mutation/lifetime bindings
remain planned. C++ kernel APIs are not automatically exposed to Python.

The standalone background test verifies cut/subdivide execution without a region,
double slide input, rejection without mutation, save/reopen and further cuts:

```sh
nice -n 10 env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui \
  /home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup \
  --python-exit-code 1 --python tests/python/nurbs_native_topology_api.py
```

Generate its exact source fixture with the C++ suite above first. The test uses a
fresh statistics view layer because Blender's normal background execution does
not process the UI notifier that clears cached statistics; the same cache
behavior was independently reproduced with mesh Subdivide. It does not count
display tessellation as source geometry.

### Remaining limits

The current face-cut contract is a single rectangular affine parameter boundary.
General trims/holes, diagonal partitions, knife/intersections, offset-loop cuts,
even-distance slide, endpoint collapse and general edge slide remain planned.
Subdivide supports wires, full rectangular faces, one boundary side and opposite
sides. Adjacent two-side/three-side patterns reject atomically; the actual mesh
results include diagonal/triangle partitions needing a separate shared service.
Ctrl+R currently edits one unique source owner at a time (linked instances of
that source are supported); it does not claim multi-object picking parity.
Mouse speed uses projected source-edge direction; source parameter slide on a
curved patch does not promise equal world-space distance. Partial subsequent
deformation retains the existing geometry contract. Arbitrary curved fitting,
analytic fit-point dragging, distance welding and direct Python component access
are not silently substituted by these topology operations.

## Final regression checkpoint

Production and all relevant test targets built successfully, exit 0:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_edit_test blenkernel_nurbs_selection_test blenkernel_nurbs_fit_test blenkernel_nurbs_transform_test --parallel 4
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_topology_edit_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/blender-nurbs-topology-final-build.log`,
`/tmp/blender-nurbs-cut-saved-tests-fixed-build.log`,
`/tmp/blender-nurbs-topology-final-app-build.log`,
`/tmp/blender-nurbs-topology-final-install.log`.
Final formatting-only rebuild/install: the same build command with `--target
blender`, then the install command above; both exit 0.
Logs: `/tmp/blender-nurbs-topology-formatted-build.log` and
`/tmp/blender-nurbs-topology-formatted-install.log`.

The complete edit-suite environment/command in `current_checkout_audit.md`
was rerun with output `/tmp/blender-nurbs-topology-regression-edit.log`:
**60/60 pass, zero skips**, exit 0. Other kernel regression commands:

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_fit_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

Results: **40/40 selection**, **14/14 fitting**, **22/22 transforms**, all exit 0;
logs `/tmp/blender-nurbs-topology-regression-{selection,fit,transform}.log`.
Together with the 16 topology tests, **152 C++ tests pass with zero skips**.
Selected CTest: **5/5 pass**, exit 0, 6.61 seconds;
`/tmp/blender-nurbs-topology-regression-ctest.log`.
The final background Python API command above passes, exit 0:
`/tmp/blender-nurbs-topology-python-api-final.log`,
`NATIVE_TOPOLOGY_BACKGROUND_PYTHON_API_PASS`. Startup RNA definition errors are
absent; its two intentionally rejected edits report their expected errors.

Python compile:

```sh
python3 -m py_compile tests/python/nurbs_native_topology_api.py tests/python/ui_simulate/test_nurbs_native.py scripts/presets/keyconfig/keymap_data/blender_default.py scripts/startup/bl_ui/space_view3d.py
git diff --check
```

Both exit 0. Clang Format `--dry-run --Werror` passes on all 27 changed/new C++
files (`/tmp/blender-nurbs-topology-format-final.log`). Its first pass found only
the new null-default RNA line wrapping, which was formatted. This formatting-only
change does not alter the already tested operator behavior.

All Y/P/V/M/cut work at this checkpoint is **local, uncommitted and unpushed**.
The binary is `/home/molotovgirl/Desktop/build_linux/bin/blender`; launching it
starts this build. An already running user process retains its prior executable.
This record distinguishes implementation, successful builds, automated checks
and the specifically viewed captures; it does not claim general curved-edit or
full Blender mesh parity.

## Ctrl+R mode and editor-state correction

The first Ctrl+R version changed Face to Edge and changed multi-cut Vertex to
Edge. The operator now passes the original Vertex/Edge/Face mode through to the
common result selection builder for every cut count. Five actual UI workflows
pass: one centered Edge cut, numeric slide in Edge mode, two cuts from Vertex
mode, a cut in Face mode and tagged-source entry in Vertex mode. Each checks the
mode and `Object.mode` while previewing, after cancel, after cutting, after undo
and after redo. Continued movement/extrusion remains in Edit Mode. The Face-mode
continued-edit capture shows the Face selector and Edit Mode active.

`/tmp/blender-nurbs-loopcut-mode-build.log` builds Blender and the topology test
after the operator correction, exit 0; `/tmp/blender-nurbs-loopcut-mode-install.log`
installs it. The final viewport run is `/tmp/blender-nurbs-loopcut-mode-final-ui.log`;
all five layouts pass. A first Face-mode test switched modes before creating the
line-extruded faces, leaving no cuttable surface. The test now creates the source
first, then selects a face and enters Face mode. The corrected run passes.
The saved fixtures are checked against their original selection modes and
geometry by the updated C++ suite, which passes **16/16** in
`/tmp/blender-nurbs-loopcut-mode-kernel-final.log`. Two test compile attempts
caught mismatches with Blender's Set API; the corrected test target builds in
`/tmp/blender-nurbs-loopcut-mode-tests-build-final.log`.
