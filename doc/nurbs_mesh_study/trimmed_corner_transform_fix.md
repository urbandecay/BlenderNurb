# Deforming a corner after Bevel and Split

The reported sequence is Bevel, split the faces apart, select one vertex, and
move it to bend its face. An isolated factory-startup viewport reproduction
created an L from a NURBS line, beveled the shared edge by 0.25, selected the
original base face, pressed Y, moved the detached face up by 1, then moved one
corner up by 0.375. The old build rejected that last movement with
`Partial trimmed NURBS face editing requires a deformation rule`.

The saved failing input is
`/tmp/blender-nurbs-trimmed-transform-repro-ui/bevel-native-corner-split-deform-split-before-deform.blend`.
Replaying its selected-vertex move in a fresh background process also failed
with the same message, exit 1. These are generated fixtures, not a user project.

## Shared geometry rule

Blender's mesh transform adapter, `transform_convert_mesh.cc:VertsToTransData`,
maps selected topology vertices to their coordinates. Splitting changes their
connectivity; the resulting face can still deform through those corners.
Native NURBS uses the same transform interaction but must update its incident
NURBS support and shared boundary curves consistently.

Bevel retains the original neighboring support surfaces and trims their visible
faces inward. Split and Separate correctly copy those trims and their exact
geometry. The missing rule was in `nurbs_transform.cc`: partial corner edits
expected the visible corners to be the natural support corners.

The shared component-transform service now also accepts rectangular trims of
clamped polynomial bilinear supports, with complete straight boundary curves
and matching positive rational edge/p-curve profiles. It interpolates the four
topology-corner displacements over the actual parameter rectangle and adds that
bilinear displacement field to the existing support. The same field restricts
to the endpoint interpolation used by each shared boundary. This preserves the
existing interior contribution, including a warped interior, rather than
rebuilding a surface from its boundary. Existing triangular displacement fields
share the coefficient application and ownership checks.

The edit retains topology/resource IDs, knots, degrees, domains, rational
weights, and all p-curves. Split geometry remains independent: moving one
detached corner leaves every other topology vertex and unrelated support's
coefficients unchanged. The input remains unchanged on success and failure.

## Explicit numerical agreement

Corner-field arithmetic reports its coefficient roundoff. A shared whole-edge
certificate additionally handles constant-U/V trims with matching rational
endpoint profiles: the polynomial bilinear support is affine along that edge,
so two bounded coefficient gaps bound the complete rational boundary.

Before any new allowance is recorded, the original edge/support certificate
must fit the declared source tolerance. The result's `boundary_error_bound`
reports any additional certified allowance beyond the scaled input tolerance;
it must fit the caller's explicit error budget. The new per-use tolerance
records the certified output bound. Real input gaps, conflicting owners and
over-budget arithmetic reject the complete candidate. No evaluator sample,
display mesh, inferred epsilon, fitting or source flattening supplies geometry.

This rule does not supply arbitrary skew/multiple-loop trim deformation,
higher-order freeform corner editing, or a rule for moving one endpoint of an
existing curved NURBS boundary. Those layouts retain explicit rejection.

## Regression checks

- Move the connected bevel strip, exercising both neighboring trimmed faces.
- Split each of the three L faces and move each detached corner independently;
  check every other vertex and unrelated support remains unchanged.
- Interpolate distinct corner transforms on binary and nonbinary rectangular
  trims, non-default parameter domains, rational boundary profiles, and an
  existing warped support. Compare interior samples with the original surface
  plus the independently evaluated intended displacement; retain all bases/IDs.
- Reject zero arithmetic budgets when needed, existing out-of-tolerance seams,
  incompatible shared owners, and unsupported skew trims without source changes.
- Viewport reproduction and actual mesh counterpart: Ctrl+B, Y, single-vertex G,
  cancel, undo/redo, and Edit Mode reentry.

Build command (exit 0):

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_transform_test blenkernel_nurbs_tessellate_test blenkernel_nurbs_topology_edit_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_transform_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/nurbs-trimmed-transform-final-build.log`,
`/tmp/nurbs-trimmed-transform-isolation-build.log`, and
`/tmp/nurbs-trimmed-transform-install.log`. A missing vector min/max include
in an intermediate build was corrected before these successful builds.

Kernel commands (exit 0, **90 passed, no skips**):

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition BLENDER_NURBS_TRIANGLE_OUTPUT=/tmp/blender-nurbs-triangle-ui BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-bevel-ui BLENDER_NURBS_BEVEL_UI_READY=1 BLENDER_NURBS_BEVEL_MOUSE_UI_READY=1 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
```

Transform: 30 passed; tessellation: 12 passed; topology: 48 passed, including
the existing saved-source and BMesh comparison gates. Logs:
`/tmp/nurbs-trimmed-{transform,tessellate,topology}-tests.log`.

Viewport commands (both exit 0):

```sh
env -u DISPLAY BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-trimmed-transform-final-ui python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_split_vertex_deformation_workflow
env -u DISPLAY BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-trimmed-transform-mesh-ui python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_split_vertex_deformation_mesh_workflow
```

Both report `NURBS_BEVEL_SPLIT_DEFORM_PASS`. The moved corner reaches
`(-1, 0, 1.375)`, cancel retains it, undo restores `(-1, 0, 1)`, redo restores the
move, and Edit Mode reentry retains it. Each result has ten topology vertices,
eleven edges, and three faces, matching the mesh counterpart. The NURBS remains
a native source with an empty legacy splines collection. Screenshots of the
moved corner were visually inspected in both output directories:
`bevel-{native,mesh}-corner-split-deform-split-vertex-moved.png`.
Logs: `/tmp/nurbs-trimmed-transform-{final,mesh}-ui.log`.

The transform adapter also reports the new additional edge/surface agreement
bound on confirmation. Its final build and install both exit 0:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/nurbs-trimmed-transform-report-build.log` and
`/tmp/nurbs-trimmed-transform-final-install.log`. The native viewport test above
was rerun after this reporting change and passed.

Fresh-process replay of the originally failing generated input (exit 0):

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup /tmp/blender-nurbs-trimmed-transform-repro-ui/bevel-native-corner-split-deform-split-before-deform.blend --python-exit-code 1 --python-expr 'import bpy; obj=bpy.context.object; assert obj.data.is_native_nurbs and len(obj.data.splines)==0; assert bpy.ops.transform.translate(value=(0,0,0.375))=={"FINISHED"}; ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get()); mesh=ev.to_mesh(); assert mesh is not None and len(mesh.polygons)>0; assert min((v.co.x+1)**2+v.co.y**2+(v.co.z-1.375)**2 for v in mesh.vertices)<1e-12; print("REOPENED_TRIMMED_CORNER_MOVE_PASS",len(mesh.polygons)); ev.to_mesh_clear()'
```

It reports `REOPENED_TRIMMED_CORNER_MOVE_PASS 294`, coefficient roundoff
`7.9301644616082624e-17`, and additional edge/surface agreement bound
`2.7755575615628914e-17`, in local units. The installed Blender reports build
`2026-10-26 13:23:25` UTC. Log: `/tmp/nurbs-trimmed-transform-reopened.log`.

Python compilation and `git diff --check` pass. The user-owned handoff remains
unchanged. Changes are local, uncommitted and unpushed; existing user Blender
processes were not closed. Restart the local binary to load the repair.
