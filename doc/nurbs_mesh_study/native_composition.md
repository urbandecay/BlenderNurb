# Chained native editing checkpoint

Baseline: local work after `e649f7e137735ce5501301b7efd440ac68010daf`, including
the [successive-merge correction](layered_merge_fix.md). This checkpoint tests
how the existing supported tools compose. It does not activate fit-point handles.

## Regression found and corrected

The existing `nurbs_native_topology_api.py` failed on Duplicate → Merge by
Distance, before it could reach its cut/subdivision checks:

```text
NURBS Merge boundary coincides with another face requiring support reconciliation
curve.remove_doubles(): CANCELLED instead of FINISHED
```

The previous chained-merge correction classified changed vertex identity as
geometric movement. An exactly coincident duplicate therefore lost its existing
surface/p-curve representation and requested unnecessary reconstruction. That
reconstruction then encountered the untouched original face, which was not one
of the faces reconstructed by the current transaction.

The shared BKE merge service now tracks displaced vertices separately from
rewired identities. A weld without movement keeps its existing support and
p-curves; equivalent representations can be coalesced by the existing exact
duplicate checks. Partial boundary deformation still uses the shared quotient
planning/reconstruction path.

A genuinely displaced linear patch may also meet an existing affine target.
Reconciliation now checks the complete boundary cycle and proves exact
curve/p-curve/support agreement before retaining that target. It reuses
`linear_curve_support_deviation_bound()`, now a shared BKE query rather than an
evaluation-only helper. Zero proves whole-interval agreement; an unsupported
proof does not authorize deleting a different surface. The service requires
structurally validated inputs, changes no tolerance, and performs no fitting.

The expanded regression covers zero, binary and nonbinary displacement, with
centroid off/on, plus exact rational-circle copies: eight variants. A separate
negative case welds the shared boundary of a flat sheet and a higher-order
bulged sheet. It retains both source faces and their different interiors.

Original failing commands/logs:

```sh
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui /home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup --python-exit-code 1 --python tests/python/nurbs_native_topology_api.py
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_filter=NurbsTopologyEditTest.DistanceWeldingSharesFaceIdentityAndPreservesRationalCircles --gtest_color=no
```

Both originally exited 1. Logs: `/tmp/blender-nurbs-composition-existing-api.log`
and `/tmp/blender-nurbs-composition-weld-before.log`. After the correction both
exit 0; logs end in `existing-api-after.log` and `weld-after.log` under the same
prefix. The former also reaches its expected rejected edits, reload and further
subdivision. Those intentional errors are part of its acceptance checks.

## Mixed operator sequences

`tests/python/nurbs_native_composition.py` runs two independent native sequences
and two persistent Mesh counterparts, with one or two initial loop cuts. Every
checkpoint remains in Edit Mode, checks the component mode and logical totals,
and saves authoritative source. Native checkpoints also evaluate the actual
derived body and verify that legacy splines remain empty. Preview geometry is
never an edit input.

| Checkpoint | One cut: V/E/F | Two cuts: V/E/F |
| --- | --- | --- |
| Initial sheet | 4/4/1 | 4/4/1 |
| Delete only face | 4/4/0 | 4/4/0 |
| Fill | 4/4/1 | 4/4/1 |
| Duplicate sheet | 8/8/2 | 8/8/2 |
| Weld sheet | 4/4/1 | 4/4/1 |
| Loop cut | 6/7/2 | 8/10/3 |
| Subdivide sheet | 15/22/8 | 21/32/12 |
| Rejected endpoint cut | unchanged | unchanged |
| Extrude whole face region | 30/56/28 | 42/80/40 |
| Delete cap, retain wires | 30/56/20 | 42/80/28 |
| Duplicate and move body | 60/112/40 | 84/160/56 |
| By Distance without nearby duplicates | unchanged | unchanged |
| Subdivide bodies and wires | 212/384/160 | 300/544/224 |
| Save/reopen | unchanged | unchanged |
| Merge all to one point | 1/0/0 | 1/0/0 |
| Extrude point | 2/1/0 | 2/1/0 |
| Subdivide wire | 3/2/0 | 3/2/0 |
| Duplicate/move wire | 6/4/0 | 6/4/0 |
| Delete edges | 0/0/0 | 0/0/0 |

These are **38 native checkpoints paired with 38 actual Mesh checkpoints**.
Mesh's opposite-edge Subdivide supplies the loop-cut topology reference;
its post-operation selection is not claimed to be modal Ctrl+R selection.
Later stages explicitly select the intended domain before continuing.

`SavedMixedWorkflowsMatchMeshAndKeepSharedSourceIncidence` reads all pairs. It
compares logical vertices, edge endpoints and face boundaries with the saved
Mesh, verifies explicit connected components and opposing shared uses, and
tessellates source with zero extra evaluation allowance. A `1e-6` comparison grid
handles Mesh float coordinates; this is not a modeling/fitting tolerance.
Native vertex and face tolerances are separately checked against `1e-9`.

The complete source-record comparator is now shared between native edit and
topology tests. It verifies IDs, the allocation counter, coefficient buffers,
weights, knots, domains, trim records, materials and tolerances. It establishes
that the rejected edit, the no-weld operation and reload preserve their expected
source records. Rejection/reload also preserve selection and history. New edits
continue after both rejection and reopening.

## Automated CTest dependency chain

The new CTest fixture chain generates its own source, runs the operator script,
then independently reads and checks the saved results:

```text
nurbs_composition_fixture
    -> script_nurbs_composition
        -> nurbs_composition_saved_sources
```

It supports split and aggregate GTest targets. Output is isolated under
`build_linux/tests/nurbs_composition`, with source fixtures and states in their
own subdirectories. The GTest commands must report one passing test, so a renamed
or unmatched filter cannot silently reuse old output. The two new UI workflows
are also listed for configurations with `WITH_UI_TESTS` enabled; this checkout's
UI configuration is off, so the GUI runs below used the headless event runner.

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
cmake --build /home/molotovgirl/Desktop/build_linux --target rebuild_cache
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources)$'
```

**5/5 pass**, exit 0; the three composition targets also pass after the final
CTest result checks were added. Logs: `/tmp/blender-nurbs-composition-ctest.log`,
`/tmp/blender-nurbs-composition-cmake.log` and
`/tmp/blender-nurbs-composition-ctest-final.log`.

The standalone manual command is:

```sh
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui /home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup --python-exit-code 1 --python tests/python/nurbs_native_composition.py
```

It passes, exit 0, in `/tmp/blender-nurbs-composition-python.log`. Outputs are in
`/tmp/blender-nurbs-composition`. As with Mesh, background scene statistics need
a fresh view layer because normal UI cache invalidation does not run there.

## Build and C++ results

All following builds completed with exit 0:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_topology_edit_test blenkernel_nurbs_edit_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test blenkernel_nurbs_mesh_test blenkernel_nurbs_geometry_test blenkernel_nurbs_tessellate_test blenkernel_nurbs_fill_test blenkernel_nurbs_edit_test --parallel 4
```

Logs: `/tmp/blender-nurbs-composition-weld-build.log`,
`/tmp/blender-nurbs-composition-atomic-build.log` and
`/tmp/blender-nurbs-composition-final-build.log`.

```sh
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files' --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_fill_test --gtest_color=no
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui BLENDER_NURBS_EXTRUSION_FIXTURE=/tmp/blender-nurbs-native-extrusion-fixture.blend BLENDER_NURBS_EXTRUSION_OUTPUT=/tmp/blender-nurbs-native-extrusion-ui BLENDER_NURBS_COMPONENT_OUTPUT=/tmp/blender-nurbs-component-ui BLENDER_NURBS_FILL_OUTPUT=/tmp/blender-nurbs-fill-ui BLENDER_NURBS_MODE_ENTRY_OUTPUT=/tmp/blender-nurbs-normal-mode-ui BLENDER_NURBS_AFFINE_OUTPUT=/tmp/blender-nurbs-rotate-scale-ui BLENDER_NURBS_AFFINE_PARITY_OUTPUT=/tmp/blender-nurbs-rotate-scale-parity-ui BLENDER_NURBS_SPLIT_OUTPUT=/tmp/blender-nurbs-split-ui BLENDER_NURBS_SEPARATE_OUTPUT=/tmp/blender-nurbs-separate-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test --gtest_color=no
```

| Suite | Passed |
| --- | ---: |
| Topology | 32 |
| Mesh evaluation | 11 |
| Geometry/ownership | 55 |
| Tessellation | 12 |
| Fill | 18 |
| Editing/lifecycle | 60 |
| Total, no skips | **188** |

Logs: `/tmp/blender-nurbs-composition-{topology,mesh,geometry,tessellate,fill,edit}-final.log`.

## Viewport evidence and installation

Run the following for each function listed below:

```sh
env -u DISPLAY BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.FUNCTION
```

* `composition_native_workflow`
* `composition_native_multicut_workflow`
* `distance_merge_native_workflow`

**3/3 pass**, exit 0. Logs:
`/tmp/blender-nurbs-composition-{single,multi,distance}-ui.log`.
The two composition cases exercise F, Shift+D, M/By Distance, Ctrl+R, Subdivide
search, E, Tab, Frame Selected, X, undo/redo and subsequent construction after
collapse. Delete Only Faces and the later global Center merge use undoable
operator execution in the same viewport context; that Center invocation is not
claimed as an M-menu keyboard check. The existing distance case checks actual
M-menu welding after a tiny displacement, undo/redo and E continuation.

Fixture corrections resolved an unchanged-mode operator returning CANCELLED,
projection before view matrices had refreshed, and assumptions about popup
position/keyboard activation after a remembered menu choice. Source counts and
requested operator identity are checked rather than accepting a keyboard event
as evidence of a successful operation. Final logs have no evaluation error or
assertion failure. Existing unrelated empty Paint-keymap and unknown
`object.make_dupli_face` warnings remain.

Screenshots of both subdivided body pairs and the final reconstructed sheet
were visually inspected. Files include:

* `/tmp/blender-nurbs-composition/composition-ui-2-bodies.blend` / `.png`
* `/tmp/blender-nurbs-composition/composition-ui-2-continued.blend` / `.png`

```sh
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup /tmp/blender-nurbs-composition/composition-ui-2-bodies.blend --python-exit-code 1 --python-expr 'import bpy; obj=bpy.context.object; assert obj.data.is_native_nurbs and len(obj.data.splines)==0; ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get()); mesh=ev.to_mesh(); assert mesh is not None and len(mesh.polygons)>0; print("INSTALLED_COMPOSITION_BODY_PASS",bpy.app.version_string,len(mesh.vertices),len(mesh.polygons)); ev.to_mesh_clear()'
```

Both exit 0. Logs: `/tmp/blender-nurbs-composition-install.log` and
`/tmp/blender-nurbs-composition-installed.log`. The installed Blender 5.2.2 LTS
reports build `2026-10-26 00:06:51` UTC and re-evaluates the larger saved source
into 11,628 display vertices and 21,952 display triangles. The editable source
remains **300 vertices, 544 edges and 224 NURBS faces**.

Clang-format dry-run with `--Werror`, Python compilation and `git diff --check`
pass. The work is local, uncommitted and unpushed; the user-owned handoff is
unchanged. Restart `/home/molotovgirl/Desktop/build_linux/bin/blender` to use the
new executable. No user Blender instance was closed.

## Scope

These are regression gates for the currently supported source families, not
universal coverage of arbitrary edit order or curved trim geometry. General
support equivalence/reconciliation, arbitrary trimmed cuts, partial curved
deformation, surface fitting, persistent fit handles and direct Python component
bindings remain separate unfinished services. `bpy.ops` uses the actual native
editor/kernel path; it does not imply a public BMesh-like source data API.
