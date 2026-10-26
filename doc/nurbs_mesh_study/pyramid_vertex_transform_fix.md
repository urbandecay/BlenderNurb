# Moving corners after merging a box into a pyramid

The reported error was reproduced on the installed application with an existing
saved pyramid: selecting its apex and translating it raised
`Partial NURBS editing currently requires a complete clamped tensor face`.
The source contained five real shared topology vertices and five NURBS faces;
this was a geometry-service restriction, not a missing viewport selection.

## Shared deformation rule

Mesh transforms move selected shared vertex identities, irrespective of whether
their incident faces have three or four sides. The references here are
`editors/transform/transform_convert_mesh.cc`, BMesh weld/collapse in
`bmesh/operators/bmo_removedoubles.cc`, and an actual Mesh pyramid subjected to
the same viewport G/R/S/E events.

The corresponding NURBS service is `BKE_nurbs_transform.hh` /
`intern/nurbs_transform.cc`. It now accepts three-sided trims of polynomial
bilinear supports with complete straight boundaries and matching rational
edge/p-curve parameterizations. An affine displacement field in the existing
surface parameters interpolates the three endpoint displacements. Adding that
field to the four support coefficients keeps the existing bilinear mixed term,
including an existing curved interior. It does not fit or flatten the surface.
The entire face still uses the existing affine route when all its corners
receive the same matrix.

The operation retains topology and geometry identities, shared incidence,
knots, parameter domains, rational weights and p-curves. Corner associations,
support domain, nondegenerate parameterization and shared-owner assignments
are checked before committing a candidate. Arithmetic uses bounded sums,
products and division; error is reported through `position_error_bound` and
must fit the caller's explicit budget. Natural tensor corners use exact
integer interpolation weights. No modeling epsilon or preview mesh is used.

Translation now delegates to the same component-transform service as rotation,
scaling and individual transform groups. Pure translations preserve unchanged
coordinate bits. Existing boundary-defined Merge reconstruction remains in
place; triangular supports outside that classifier can now use this deformation
rule without losing their interiors. A previous negative test for moving a
warped triangular trim now verifies successful deformation and preservation of
its mixed term instead.

This does not supply arbitrary higher-order curved-face deformation, general
multi-loop trim deformation or interactive fit-point handles. Unsupported
geometry, incompatible shared-support owners and insufficient numerical budgets
still reject atomically.

## Regression coverage

- Each of the five pyramid corners: G, then rotation, nonuniform and singular
  scale; check unaffected vertices, retained representation, strict preview,
  another edge merge and collapse to one point.
- Triangular trims at natural, interior binary and nonbinary UV coordinates;
  distinct per-vertex matrices and explicit arithmetic-budget rejection.
- Invalid rational correspondence, parameter association/domain, degenerate
  UV triangles and conflicting owners of one support: unchanged source.
- Real click and box selection, vertex/edge/face modes, G/R/S, cancel,
  undo/redo, triangular face extrusion, Edit Mode exit/reentry and screenshots.
- Actual Mesh counterpart for every saved viewport checkpoint. A fresh Blender
  process reopens each final file, moves the selected face again and saves it.
  The C++ saved-source test checks native topology against the Mesh, strict
  surface evaluation, retained trim identities and exact source restoration.
  The `1e-6` coordinate comparison grid accommodates Mesh float coordinates;
  it is separate from the NURBS source arithmetic budget.

The UI functions are `test_nurbs_native.pyramid_movement_workflow`,
`pyramid_movement_mesh_workflow` and `pyramid_movement_box_workflow`.
They invoke `tests/python/nurbs_native_triangle_reload.py` for fresh-process
continuation. Checkpoints and screenshots are in
`/tmp/blender-nurbs-triangle-ui/` by default; set
`BLENDER_NURBS_TRIANGLE_OUTPUT` to override it.

## Commands and validation

The original reproduction exited 1:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup /tmp/blender-nurbs-apex-ui/apex-CENTER-VERT-False-True-CAP.blend --python-exit-code 1 --python-expr 'import bpy; print(bpy.ops.transform.translate(value=(0.25,0,0)))'
```



The application and all seven test targets built successfully (exit 0):

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_transform_test blenkernel_nurbs_topology_edit_test blenkernel_nurbs_edit_test blenkernel_nurbs_geometry_test blenkernel_nurbs_mesh_test blenkernel_nurbs_fill_test blenkernel_nurbs_tessellate_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_transform_test blenkernel_nurbs_topology_edit_test --parallel 4
```

Logs: `/tmp/blender-nurbs-triangle-final-build.log` and
`/tmp/blender-nurbs-triangle-relink.log`. Initial test-fixture compile errors
were corrected before proceeding. An early CTest run overlapped relinking and
could not execute the topology fixture; the complete run below was repeated
after linking finished.

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition BLENDER_NURBS_TRIANGLE_OUTPUT=/tmp/blender-nurbs-triangle-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files' --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_fill_test --gtest_color=no
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui BLENDER_NURBS_EXTRUSION_FIXTURE=/tmp/blender-nurbs-native-extrusion-fixture.blend BLENDER_NURBS_EXTRUSION_OUTPUT=/tmp/blender-nurbs-native-extrusion-ui BLENDER_NURBS_COMPONENT_OUTPUT=/tmp/blender-nurbs-component-ui BLENDER_NURBS_FILL_OUTPUT=/tmp/blender-nurbs-fill-ui BLENDER_NURBS_MODE_ENTRY_OUTPUT=/tmp/blender-nurbs-normal-mode-ui BLENDER_NURBS_AFFINE_OUTPUT=/tmp/blender-nurbs-rotate-scale-ui BLENDER_NURBS_AFFINE_PARITY_OUTPUT=/tmp/blender-nurbs-rotate-scale-parity-ui BLENDER_NURBS_SPLIT_OUTPUT=/tmp/blender-nurbs-split-ui BLENDER_NURBS_SEPARATE_OUTPUT=/tmp/blender-nurbs-separate-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test --gtest_color=no
```

All **215 C++ tests pass, with no skips**: transform 26, topology 33,
editing/lifecycle 60, geometry/ownership 55, Mesh evaluation 11, Fill 18,
tessellation 12. Final logs:
`/tmp/blender-nurbs-triangle-{transform-final,topology-final,edit-tests,geometry-tests,mesh-tests,fill-tests,tessellate-tests}.log`.

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
```

**5/5 pass**, exit 0, 8.14 seconds. Log:
`/tmp/blender-nurbs-triangle-ctest-final.log`.

Run each viewport function in its own Blender process (the timer-based setup
runner does not sequentially await multiple function arguments):

```sh
for triangle_test in pyramid_movement_workflow pyramid_movement_mesh_workflow pyramid_movement_box_workflow
do
  env -u DISPLAY BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests "test_nurbs_native.${triangle_test}" > "/tmp/blender-nurbs-triangle-final-${triangle_test}.log" 2>&1 || exit $?
done
```

**3/3 pass**, exit 0, including the child-process reload/move assertions.
There are nine saved states per workflow: **18 native states paired with nine
Mesh reference states**. The saved-source C++ case passes for every pair,
including exact source checks after cancellation/undo/redo, Edit Mode reentry,
and independent continuation after fresh-process loading.

Screenshots of the moved apex, edge transformation and continued face extrusion
were visually inspected:
`/tmp/blender-nurbs-triangle-ui/triangle-native-{apex-move,edge-transform,extrude}.png`.
The automated checks also verify nonempty evaluated surfaces at every UI save.
The existing unrelated empty Paint-keymap and unknown
`object.make_dupli_face` warnings remain in the headless UI logs.

```sh
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup /tmp/blender-nurbs-apex-ui/apex-CENTER-VERT-False-True-CAP.blend --python-exit-code 1 --python-expr 'import bpy; obj=bpy.context.object; assert obj.data.is_native_nurbs and len(obj.data.splines)==0; assert bpy.ops.transform.translate(value=(0.25,0,0))=={"FINISHED"}; ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get()); mesh=ev.to_mesh(); assert mesh is not None and len(mesh.polygons)>0; print("INSTALLED_PYRAMID_MOVE_PASS",bpy.app.version_string,bpy.app.build_date,bpy.app.build_time,len(mesh.polygons)); ev.to_mesh_clear()'
```

Both exit 0. The installed application reports Blender 5.2.2 LTS, build
`2026-10-26 00:51:04` UTC. The original apex translation now finishes and its
NURBS source evaluates successfully (342 derived display triangles).
Logs: `/tmp/blender-nurbs-triangle-install.log` and
`/tmp/blender-nurbs-triangle-installed.log`.

Clang-format dry-run with `--Werror`, Python compilation and `git diff --check`
pass. Changes remain local, uncommitted and unpushed. The user-owned handoff is
untouched. Restart `/home/molotovgirl/Desktop/build_linux/bin/blender` to load
the updated binary; no existing user Blender process was closed.
