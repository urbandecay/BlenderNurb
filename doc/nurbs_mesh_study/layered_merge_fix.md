# Successive NURBS merges

Follow-up: [mixed editing workflows](native_composition.md) catches and fixes
the subsequent coincident Duplicate → By Distance regression. It distinguishes
vertex rewiring from geometric displacement and permits a proved equivalent
affine weld target while preserving different curved interiors. It adds a
self-contained CTest dependency chain and checks complete source records across
rejection, no-op welding and reopening.

Reported sequence: extrude a line into a face, extrude the face into a box,
merge one top edge, then merge the remaining triangular top. A mesh permits
both merges. Native NURBS rejected the second with:

```text
Partial NURBS editing currently requires a complete clamped tensor face
```

The new kernel regression reproduced that failure before the correction.
Actual viewport events also reproduced the failed M operation, after selecting
the three remaining top corners. The reproduction files and screenshot are in
`/tmp/blender-nurbs-layered-baseline`; the user supplied a screenshot, not a
source `.blend` file. The first attempt at the event fixture selected a lower
legacy endpoint in top view. Entering native component mode before selection
made the fixture's selected source components unambiguous.

## Shared kernel change

The first merge reconstructs triangular trimmed faces. The second merge used
to request partial support deformation before planning the resulting boundary,
and the deformation service required four tensor corners. Testing one merge
from an untouched box did not establish that merges compose.

`merge_vertex_groups()` now plans quotient boundaries before support
deformation, following the ordering in BMesh `bmo_weld_verts_exec()` and
`remdoubles_createface()` (`bmesh/operators/bmo_removedoubles.cc`). All native
Merge variants and By Distance use this shared BKE service.

* Recognize affected boundary-defined source patches: polynomial degree-one
  affine supports, complete bilinear tensor rectangles, and canonical affine
  triangle charts, including their recorded coefficient construction roundoff.
* Detach these faces from the independent deformation candidate, together with
  faces that the weld removes. Apply the existing source edge/vertex transforms,
  then resolve shared vertex/edge identities and directed boundary cycles.
* Reconstruct all surviving cycles through the common NURBS Fill service.
  Triangles use affine supports, quads use bilinear supports, and larger
  polygons require exact planar construction. Preserve surviving face IDs,
  material indices, declared tolerance budgets and effective edge-use winding.
  Winding comes from incidence rather than a comparison with the old normal.
* Coalesce identical cycles reconstructed in this transaction. This fixes
  tetrahedron-to-triangle collapse, where two old faces become one. Old curved
  faces still require explicit support reconciliation; sharing boundary IDs
  alone does not prove that their interiors are equal.
* Leave unmoved component groups and unrelated patch resources intact. Preserve
  rational curves, higher-order supports and arbitrary warped trims through
  their existing deformation contracts; do not flatten them into polygon fills.

The affine fourth-corner constructor is shared privately between Fill and
source-family recognition in `nurbs_math.hh`. It retains its conservative
roundoff accounting. No display mesh supplies construction data. Unsupported
geometry or exceeded construction budgets reject the independent candidate.

An additional BMesh comparison corrected Collapse at junctions: BMesh averages
both endpoint incidences of every selected edge, so higher-valence vertices
contribute more often. Center averages unique selected vertices. Separate
selected edge islands remain separate Collapse groups.

## Boundary evaluation after fractional merges

The first successful keyboard run exposed a further evaluation failure. The
second merge places a tip at a third of the old width. Independent source-curve
and surface calculations differed by about `1.57e-16`, exceeding a recorded
construction tolerance of about `1.11e-16`. A keyboard operator returning
FINISHED was insufficient: the evaluated body could disappear.

Boundary evaluation now retains an optional coefficient-based bound, alongside
the original measured discrepancies. It proves exact agreement for supported
affine charts using Blender's existing adaptive exact `orient3d` predicate when
rounded UV normalization cannot establish an identity. Predicate inputs are
restricted to a conservative exponent range; other inputs retain the ordinary
checks. Literal natural parameter endpoints are recognized without subtracting
and dividing their knot span.

For a straight chord between opposite bilinear corners, the entire boundary
gap is bounded by one quarter of the mixed coefficient's norm:

```text
D = P00 + P11 - P10 - P01
S(chord_parameter) - chord = +/- t(1-t) D
max gap <= ||D||_1 / 4
```

TwoSum residuals and outward arithmetic bound `D`. This accounts for the
already reported rounding in an affine triangle's fourth coefficient. It does
not increase a tolerance or infer an evaluation epsilon. Raw measured
deviations remain available, and diagnostics distinguish measured and checked
values. A new test rejects a genuine midpoint gap even with endpoint-only
sampling, rejects a budget just below its bound, and accepts the declared bound
without changing source coefficients or tolerance. Source endpoint/vertex
agreement is still checked independently.

## Built and tested

The app and all six test targets built. Commands used during validation:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test blenkernel_nurbs_mesh_test blenkernel_nurbs_geometry_test blenkernel_nurbs_tessellate_test blenkernel_nurbs_fill_test blenkernel_nurbs_transform_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_topology_edit_test blenkernel_nurbs_mesh_test blenkernel_nurbs_geometry_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test blenkernel_nurbs_tessellate_test blenkernel_nurbs_fill_test blenkernel_nurbs_transform_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_topology_edit_test blenkernel_nurbs_tessellate_test blenkernel_nurbs_fill_test blenkernel_nurbs_transform_test --parallel 4
```

Relevant build logs are `/tmp/blender-nurbs-layered-boundary-build.log`,
`/tmp/blender-nurbs-layered-predicate-build.log`,
`/tmp/blender-nurbs-layered-validation-build.log` and
`/tmp/blender-nurbs-layered-validation-retry-build.log`. Two test-fixture build
errors were corrected: an array required explicit Span length, and DNA list
heads required typed casts. The third command above built Blender, then exited
2 on the list casts; the fourth command completed all remaining targets with
exit 0. The first and second commands completed with exit 0 after their fixes.
Temporary coefficient diagnostics were removed. Earlier failures are retained
in `/tmp/blender-nurbs-layered-kernel-before.log`,
`/tmp/blender-nurbs-layered-chain-before.log`,
`/tmp/blender-nurbs-layered-body-before.log` and
`/tmp/blender-nurbs-layered-debug.log`; the topology-final log is the passing run.

Kernel commands:

```sh
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files' --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_fill_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
```

| Suite | Passed |
| --- | ---: |
| Topology editing | 30 |
| Mesh evaluation | 11 |
| Geometry and ownership | 55 |
| Tessellation | 12 |
| Fill | 18 |
| Transforms | 22 |
| Total, no skips | **148** |

Logs: `/tmp/blender-nurbs-layered-topology-final.log` and
`/tmp/blender-nurbs-layered-{mesh,geometry,tessellate,fill,transform}-tests.log`.
Regressions include all four starting top edges, Center/Collapse at both steps
and V/E/F selection (**48 combinations**), repeated edge merges from box to
faces, wire and one vertex, junction centroids, opposing shared uses, unchanged
patch resources, rejection of flattening warped trims, strict zero-allowance
preview evaluation, and twelve saved source states. Saved NURBS topology is
compared with actual BMesh `pointmerge`/`collapse`, allowing for Mesh float
coordinates while separately checking NURBS double targets and budgets.

Viewport command, with `FUNCTION` replaced by each name below:

```sh
env -u DISPLAY BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.FUNCTION
```

* `layered_merge_workflow`
* `layered_merge_collapse_workflow`
* `layered_merge_edge_workflow`
* `layered_merge_face_workflow`
* `surface_wire_grid_workflow`

**5/5 passed**, exit 0. Logs are
`/tmp/blender-nurbs-layered-{center,collapse,edge,face,grid}-ui.log`; none of
these final logs contains a NURBS evaluation error or a test assertion failure.

The four layered workflows use real Add/E, click/box selection and M popup
events, assert the requested merge variant, verify a nonempty evaluated body,
undo/redo, retained Edit Mode, source mode persistence and continued E from the
tip. The grid workflow verifies that another cube's rectangular display grid
survives Collapse/Delete. Screenshots and blends are in
`/tmp/blender-nurbs-layered-ui` and `/tmp/blender-nurbs-grid-ui`.

A fixture initially expected a selected tip in every component mode. An actual
Mesh/BMesh Python comparison established the correct behavior: V retains one
selected vertex; E/F clear selection when no selected edge/face survives. The
fixture now checks that behavior. This was a test correction, not an application
change. Evidence: `/tmp/blender-nurbs-layered-mesh-selection.py` and `.log`.

Fresh-process persistence and Python operator continuation:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup --python-exit-code 1 --python /tmp/blender-nurbs-layered-reload.py
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^script_nurbs_(line|legacy_compat)$'
```

The reload script opens twelve saved files, performs four second merges via
`bpy.ops.curve.merge`, and compares the resulting evaluated geometry with the
saved result. **Pass, exit 0; both compatibility targets pass, exit 0.** Scene
statistics remain cached in background mode until UI
notifiers run; a separate actual Mesh check reproduces the same behavior.
The script uses real evaluated geometry after operations, with authoritative
topology independently checked by the C++ saved-file test. Logs are
`/tmp/blender-nurbs-layered-reload.log` and
`/tmp/blender-nurbs-layered-compat-tests.log`.

Installation completed with exit 0:

```sh
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup /tmp/blender-nurbs-layered-ui/layered-CENTER-CENTER-VERT-after-second.blend --python-exit-code 1 --python-expr 'import bpy; obj=bpy.context.object; assert obj.data.is_native_nurbs and len(obj.data.splines)==0; ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get()); mesh=ev.to_mesh(); assert mesh is not None and len(mesh.polygons)>0; print("INSTALLED_LAYERED_NURBS_BODY_PASS",bpy.app.version_string,len(mesh.vertices),len(mesh.polygons)); ev.to_mesh_clear()'
```

The installed binary reopened the native source and produced 167 display
vertices and 330 triangles, exit 0. Logs:
`/tmp/blender-nurbs-layered-install.log` and
`/tmp/blender-nurbs-layered-installed-check.log`. It reports Blender 5.2.2 LTS,
build `2026-10-25 22:47:51`. The commit hash remains `e649f7e13773` because
these changes are not committed. Existing user Blender processes were not
closed; restart `/home/molotovgirl/Desktop/build_linux/bin/blender` to use the
new build.

Clang-format dry-run with `--Werror`, Python compilation and `git diff --check`
all pass. The user-owned untracked handoff was preserved.

## Scope

This repairs sequential merges of the supported source patch families. It does
not establish arbitrary curved trim deformation, general nonplanar polygon
construction, hole collapse or a universal geometry kernel. Generic partial G/R/S
on triangular trims still needs its own deformation service. These limits must
remain explicit; neither a display mesh nor unreported fitting supplies a result.

Before/after screenshots of the wedge and resulting pyramid were visually
inspected, including Vertex and Face modes. This is separate from automated
viewport and saved-source assertions. The changes are local and unpushed.
