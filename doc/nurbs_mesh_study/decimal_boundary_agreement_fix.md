# Rounded triangular trim boundary agreement

This corrects the numerical follow-up found while testing pyramid shading.
A source-precision C++ translation of a merged pyramid apex by the stored
binary64 value `0.3` could complete within its arithmetic budget, then fail
strict preview evaluation at seven subdivisions. The measured curve/support
difference was about `1.665e-16`, exceeding the source tolerance of about
`1.110e-16`. The existing proof covered exact affine charts and natural support
sides/diagonals, but did not cover this rounded affine trim.

The previous shading regression used a float-rounded C++ `0.3f` input as one
of its cases. That did not represent typed native G accurately: inspection of
`transform_mode_translate.cc` confirms `applyNumInput(double *)` retains typed
translations in doubles, and `transform_convert_nurbs.cc` supports precise
operator replay and `value_precise_x/y/z`. Rotation/scale matrices and ordinary
float operator inputs are separate paths. The earlier claim that the GUI
could not exercise this double input was incorrect. This regression exercises
`0.3`, `0.1`, `1/3`, `-0.3` and the next stored value above `0.3` directly.

## Shared kernel correction

`linear_curve_support_deviation_bound()` in `intern/nurbs_evaluate.cc` now has
a general whole-interval bound for a straight polynomial 3D curve, its straight
polynomial p-curve, and a polynomial bilinear support. Existing exact-identity
and natural-side/diagonal proofs remain as tighter special cases.

Let `P(u,v)` be the support and let the p-curve run linearly from `(u0,v0)` to
`(u1,v1)`. Its support composition is a quadratic Bezier curve with coefficients:

- `B0 = P(u0,v0)`
- `B1 = (P(u0,v1) + P(u1,v0)) / 2`
- `B2 = P(u1,v1)`

Degree elevation of the 3D source line gives `C0`, `(C0+C1)/2`, `C1`.
Subtracting these gives the three coefficients of the gap. Bernstein weights
are nonnegative and sum to one on the edge, so the largest coefficient L1 norm
is a sufficient bound on the Euclidean gap throughout the interval.

The calculation uses the existing private bounded-scalar arithmetic for UV
normalization, products, sums and division. It subtracts line coefficients
before interpolating support coefficients to retain accuracy at large source
origins. The middle difference is evaluated as half the sum of the two cross
support/line differences. Arithmetic uncertainty is included in the bound;
nonrepresentable bounds return no certificate. The query requires supported
round-to-nearest/subnormal arithmetic, correct 3D/2D roles, matching parameter
bases, positive equal weights, and trim endpoints inside the support domain.

The service chooses the smallest applicable sufficient bound. Zero is still
required before claiming exact identity. A nonzero sufficient upper bound is
not a measurement or proof of a nonzero gap. More general rational and
higher-order compositions retain their existing evaluation path.

Boundary evaluation already stores both measured sample differences and any
coefficient bound. The new proof uses that shared path; it changes no source
coefficients, weights, knots, UVs, IDs, selection, tolerances or evaluation
allowance. No source fitting or epsilon adjustment is introduced. Genuine
unsupported or out-of-budget geometry continues to reject.

## Regression coverage

- Raw double pyramid moves above, at 1, 7, 11 and 23 subdivisions, with zero
  additional evaluation allowance and exact source-record preservation.
- Independent GMP rational witnesses over 101 edge positions for nonbinary
  domains, large origins, tiny/subnormal coordinates and extreme finite domains.
  GMP is a test-only optional reference; the production bound uses doubles.
- A deliberately warped interior trim still fails its zero-tolerance boundary
  check. Incompatible weights/parameter bases, incorrect curve/p-curve roles,
  out-of-domain trims and unrepresentable arithmetic do not claim agreement.
- Native and actual Mesh viewport workflows type G X 0.3, verify shaded normals,
  undo/redo, Edit Mode reentry and saved source. Typed G retains the source
  decimal in double precision; ordinary Mesh still uses its existing float path.



## Exact commands and results

The new focused regression failed before the correction (exit 1, missing
whole-interval certificate):

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test --gtest_filter=NurbsMeshEvaluationTest.DecimalPyramidMovesKeepCertifiedBoundariesWithoutAnEvaluationAllowance --gtest_color=no
```

Log: `/tmp/nurbs-decimal-before.log`. The preceding shading investigation's
actual seven-subdivision rejection is retained in
`/tmp/nurbs-pyramid-shading-kernel-tests.log`.

All builds exited 0:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_mesh_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_mesh_test blender --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_mesh_test blender blenkernel_nurbs_tessellate_test blenkernel_nurbs_fill_test blenkernel_nurbs_topology_edit_test blenkernel_nurbs_geometry_test blenkernel_nurbs_transform_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_mesh_test blender --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_edit_test blenkernel_nurbs_object_test --parallel 4
```

Logs include `/tmp/nurbs-decimal-{repro-build,kernel-build,bound-build,final-build,relink,lifecycle-build-final}.log`.

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files' --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_fill_test --gtest_color=no
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition BLENDER_NURBS_TRIANGLE_OUTPUT=/tmp/blender-nurbs-triangle-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
env BLENDER_NURBS_PREVIEW_FIXTURE=/tmp/blender-nurbs-native-preview.blend BLENDER_NURBS_EDIT_OUTPUT=/tmp/blender-nurbs-linked-ui BLENDER_NURBS_MOVE_OUTPUT=/tmp/blender-nurbs-move-ui BLENDER_NURBS_DUPLICATE_OUTPUT=/tmp/blender-nurbs-duplicate-ui BLENDER_NURBS_EXTRUSION_FIXTURE=/tmp/blender-nurbs-native-extrusion-fixture.blend BLENDER_NURBS_EXTRUSION_OUTPUT=/tmp/blender-nurbs-native-extrusion-ui BLENDER_NURBS_COMPONENT_OUTPUT=/tmp/blender-nurbs-component-ui BLENDER_NURBS_FILL_OUTPUT=/tmp/blender-nurbs-fill-ui BLENDER_NURBS_MODE_ENTRY_OUTPUT=/tmp/blender-nurbs-normal-mode-ui BLENDER_NURBS_AFFINE_OUTPUT=/tmp/blender-nurbs-rotate-scale-ui BLENDER_NURBS_AFFINE_PARITY_OUTPUT=/tmp/blender-nurbs-rotate-scale-parity-ui BLENDER_NURBS_SPLIT_OUTPUT=/tmp/blender-nurbs-split-ui BLENDER_NURBS_SEPARATE_OUTPUT=/tmp/blender-nurbs-separate-ui /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_edit_test --gtest_color=no
```

**226 C++ checks pass without skips**: Mesh 18, object/runtime 4, transforms 26,
geometry 55, tessellation 12, Fill 18, topology 33, editing/lifecycle 60.
Final logs: `/tmp/nurbs-decimal-{mesh,object,transform,geometry,tessellate,fill,topology,edit}-final.log`.
The raw double regression also asserts that measured sample disagreement can
exceed the source tolerance while the sufficient gap bound fits it, and that
those measured discrepancies remain reported. Every source record remains
unchanged by evaluation.

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
```

**5/5 pass**, exit 0, 9.40 seconds. Log:
`/tmp/nurbs-decimal-ctest-final.log`.

```sh
for decimal_test in pyramid_decimal_move_workflow pyramid_decimal_move_mesh_workflow
do
  env -u DISPLAY BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests "test_nurbs_native.${decimal_test}" > "/tmp/nurbs-decimal-${decimal_test}.log" 2>&1 || exit $?
done
```

**2/2 viewport workflows pass**, exit 0, including native line → face → box →
Merge, typed G X 0.3, undo/redo and Edit Mode reentry. The actual Mesh case uses
its own cube and Mesh Merge. The close-up saved native and Mesh screenshots
were visually inspected:
`/tmp/blender-nurbs-shading-ui/pyramid-shading-{native,mesh}-decimal-moved.png`.
Native normals have minimum face/corner dot `0.9999998509883881`, unit lengths
within roughly `1e-7`, and no zero-area display triangles. The source stays
native with no legacy splines.

```sh
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup /tmp/blender-nurbs-apex-ui/apex-CENTER-VERT-False-True-CAP.blend --python-exit-code 1 --python-expr 'import bpy; obj=bpy.context.object; assert obj.data.is_native_nurbs and len(obj.data.splines)==0; assert bpy.ops.transform.translate(value_precise_x="0.3")=={"FINISHED"}; ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get()); mesh=ev.to_mesh(); assert mesh is not None and len(mesh.polygons)>0; assert all(p.area>0 for p in mesh.polygons); normals=mesh.corner_normals; assert all(normals[i].vector.dot(p.normal)>0.9999 for p in mesh.polygons for i in p.loop_indices); print("INSTALLED_PRECISE_DECIMAL_MOVE_PASS",bpy.app.build_date,bpy.app.build_time,len(mesh.polygons)); ev.to_mesh_clear()'
```

Both exit 0. This checks the existing precise Python operator API, not only
float RNA `value` input. Logs: `/tmp/nurbs-decimal-install.log` and
`/tmp/nurbs-decimal-installed.log`.

```sh
/tmp/blender-nurbs-deps-storage/llvm/bin/clang-format --dry-run --Werror source/blender/blenkernel/BKE_nurbs_evaluate.hh source/blender/blenkernel/intern/nurbs_evaluate.cc source/blender/blenkernel/intern/nurbs_mesh_test.cc
python3 -m py_compile tests/python/ui_simulate/test_nurbs_native.py
git diff --check
```

All exit 0. The correction is implemented, built, tested through kernel and
application paths, visually checked and installed locally. It is uncommitted
and unpushed. The user-owned handoff is unchanged. Fit-point policy discussion
was deferred by the user; no new fit-point editing default was activated.
