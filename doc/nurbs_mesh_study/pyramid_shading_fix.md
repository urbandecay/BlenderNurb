# Shading after merging and moving a pyramid

The user's dark triangular patches were reproduced in the actual viewport on
Blender 5.2.2 LTS build `2026-10-26 00:51:04`. The scene was made through
NURBS Line → E to a face → E to a box → Merge Center of the top → G X 0.5.
The same construction on an actual Mesh pyramid supplies the shading reference.

## Cause and correction

The double-precision surface tessellation can contain very thin triangles.
Some become exactly collinear when converted to the renderer's float positions.
They have no drawable area, but their degenerate reference frames enter Mesh's
custom corner-normal encoding and corrupt normals on nearby visible triangles.

The reproduction contained 342 derived display triangles, 24 with zero area.
Fourteen corners of nondegenerate triangles had normals disagreeing with their
planar Mesh face normals; the worst dot product was about 0.562, and the shortest
corner-normal length was about 0.750. The viewport capture visibly reproduced
the user's dark patches. A separate experiment removing those collapsed display
triangles before running Blender's normal setter removed all bad corners.

`BKE_nurbs_mesh.hh` / `intern/nurbs_mesh.cc` now omit exactly collinear float
triangles before constructing Mesh corner-normal spaces. The check uses Blender's
robust 2D orientation predicate on the three coordinate projections of the stored
float positions. There is no area threshold, proximity weld, source mutation,
normal-cache override or forced flat shading. Thin representable faces remain.
The same Mesh edge construction, custom normal encoding and renderer continue to
consume the retained faces, including analytic normals on curved NURBS surfaces.

`MeshEvaluation::mesh_to_preview_triangle` maps every retained Mesh face to the
original double preview triangle. `skipped_degenerate_float_triangles` reports
what was omitted. The full double preview remains intact for source IDs, UVs,
selection, face anchors and editing intermediates; source boundary wires also
remain. Material and wire-mask transfer use the explicit face map. No NURBS
coefficients, weights, knots, trim curves, tolerance, identity or file format
are changed.

A genuinely collapsed source face therefore remains in the source and double
preview while having no shaded Mesh faces. Its boundary wires remain available.
The regression for collapsed editing geometry now verifies that contract.
Float-overflow rejection and quantization diagnostics retain their existing
contracts. Undefined source normals on retained faces still use the counted
Mesh display fallback; they are distinct from triangles with no float area.

## Checks

The kernel regression creates and merges a box, then checks the original apex,
G X 0.5, and a float-rounded C++ G X 0.3 input. Each runs
at 1, 4, 7, 11, 23 and 32 subdivisions. It checks decoded corner-normal lengths,
agreement with ordinary Mesh face normals, monotone face provenance, unchanged
source records and accounting for every preview triangle. The existing rational
cylinder and G0 join tests continue to check smooth curved shading and creases.
A separate `1e-20`-wide sheet verifies that positive-area thin faces are retained.

A raw double C++ offset of exactly the stored binary64 `0.3` exposed a separate
pre-existing boundary-evaluation certificate limitation: strict sampled checks
can mistake evaluator roundoff for a source gap on a rounded affine trim. The
shading C++ test used `double(0.3f)`, without increasing tolerance or evaluation
allowance. That input was mistakenly described as native typed G: the
native adapter actually preserves typed translations in doubles. The subsequent
[decimal-boundary correction](decimal_boundary_agreement_fix.md) fixes the
rounded-trim proof and tests actual typed G X 0.3 against Mesh.

Viewport tests `pyramid_shading_workflow` and
`pyramid_shading_mesh_workflow` create the native and actual Mesh reference,
move the apex, inspect evaluated shading normals, exercise undo/redo and Edit
Mode reentry, and save close-up screenshots. The native test failed on the old
binary with `24 != 0` zero-area display faces and the bad normals above.

Artifacts:
- `/tmp/nurbs-pyramid-shading-before.png`
- `/tmp/blender-nurbs-shading-ui/pyramid-shading-{native,mesh}-{moved,restored}.{blend,png}`
- `/tmp/nurbs-pyramid-shading-diagnose.log`
- `/tmp/nurbs-pyramid-shading-isolate.log`
- `/tmp/nurbs-pyramid-shading-ui-before.log`



## Exact validation commands and results

All builds exited 0:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_mesh_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_mesh_test blender blenkernel_nurbs_object_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_mesh_test blenkernel_nurbs_edit_test blenkernel_nurbs_topology_edit_test --parallel 4
```

Logs: `/tmp/nurbs-pyramid-shading-{kernel-build,build,regression-build}.log`.

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test --gtest_color=no
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition BLENDER_NURBS_TRIANGLE_OUTPUT=/tmp/blender-nurbs-triangle-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui BLENDER_NURBS_EXTRUSION_FIXTURE=/tmp/blender-nurbs-native-extrusion-fixture.blend BLENDER_NURBS_EXTRUSION_OUTPUT=/tmp/blender-nurbs-native-extrusion-ui BLENDER_NURBS_COMPONENT_OUTPUT=/tmp/blender-nurbs-component-ui BLENDER_NURBS_FILL_OUTPUT=/tmp/blender-nurbs-fill-ui BLENDER_NURBS_MODE_ENTRY_OUTPUT=/tmp/blender-nurbs-normal-mode-ui BLENDER_NURBS_AFFINE_OUTPUT=/tmp/blender-nurbs-rotate-scale-ui BLENDER_NURBS_AFFINE_PARITY_OUTPUT=/tmp/blender-nurbs-rotate-scale-parity-ui BLENDER_NURBS_SPLIT_OUTPUT=/tmp/blender-nurbs-split-ui BLENDER_NURBS_SEPARATE_OUTPUT=/tmp/blender-nurbs-separate-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test --gtest_color=no
```

**110 C++ tests pass, no skips**: Mesh evaluation 13, dependency graph/runtime 4,
editing/lifecycle 60 and topology 33. Logs:
`/tmp/nurbs-pyramid-shading-{mesh-final,object-tests,edit-tests,topology-tests}.log`.

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
```

**5/5 pass**, exit 0, 8.33 seconds. Log:
`/tmp/nurbs-pyramid-shading-ctest.log`.

```sh
for shading_test in pyramid_shading_workflow pyramid_shading_mesh_workflow pyramid_movement_workflow
do
  env -u DISPLAY BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests "test_nurbs_native.${shading_test}" > "/tmp/nurbs-pyramid-shading-${shading_test}.log" 2>&1 || exit $?
done
```

**3/3 viewport workflows pass**, exit 0. The movement regression also reopens
in a child process and continues editing. The native shading test reports
318 display triangles, zero zero-area triangles, a minimum corner/face normal
dot product of `0.9999998323619366`, and normal lengths within about `1e-7` of
one. These checks pass after undo/redo and Edit Mode reentry too. The actual
Mesh counterpart has five polygons and passes the same shading checks.

The close-up before/after native and actual Mesh screenshots were visually
inspected. An additional RGB comparison of 14,041 pixels strictly inside the
front triangular face, away from edges and the cursor, gave **zero channel
difference** between the corrected native and Mesh screenshots. This is a
specific planar scene comparison, not a general renderer equivalence claim.

```sh
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup /tmp/blender-nurbs-shading-ui/pyramid-shading-native-moved.blend --python-exit-code 1 --python-expr 'import bpy; obj=bpy.context.object; assert obj.data.is_native_nurbs and len(obj.data.splines)==0; ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get()); mesh=ev.to_mesh(); assert mesh is not None and len(mesh.polygons)>0; assert all(p.area>0 for p in mesh.polygons); normals=mesh.corner_normals; assert all(normals[i].vector.dot(p.normal)>0.9999 for p in mesh.polygons for i in p.loop_indices); assert all(abs(n.vector.length-1)<1e-4 for n in normals); print("INSTALLED_PYRAMID_SHADING_PASS",bpy.app.build_date,bpy.app.build_time,len(mesh.polygons)); ev.to_mesh_clear()'
```

Both exit 0. The installed Blender 5.2.2 LTS reports build
`2026-10-26 01:13:11` UTC and passes the saved pyramid's shading assertions.
Logs: `/tmp/nurbs-pyramid-shading-install.log` and
`/tmp/nurbs-pyramid-shading-installed-check.log`. The independent diagnostic in
`/tmp/nurbs-pyramid-shading-installed.log` also reports `BAD 0`.

```sh
/tmp/blender-nurbs-deps-storage/llvm/bin/clang-format --dry-run --Werror source/blender/blenkernel/BKE_nurbs_mesh.hh source/blender/blenkernel/intern/nurbs_mesh.cc source/blender/blenkernel/intern/nurbs_mesh_test.cc
python3 -m py_compile tests/python/ui_simulate/test_nurbs_native.py
git diff --check
```

All exit 0. The changes are local, uncommitted and unpushed. The user-owned
handoff is untouched, and no user Blender process was closed. Restart
`/home/molotovgirl/Desktop/build_linux/bin/blender` for the updated executable.
