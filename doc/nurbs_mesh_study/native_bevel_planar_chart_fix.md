# Bevel failure after resizing a flat face

The reported message was reproduced exactly:
`Planar NURBS projection exceeds the source construction error budget`.

## Reproduction in a visible Blender window

The factory-scene sequence was: add NURBS Line, extrude Y by 2, select the face,
extrude Z by 2, scale the top in X by 1.3, select the box edges, then Ctrl+B,
width 0.25 and three segments. The old build retained 8 vertices / 12 edges /
6 faces instead of creating the bevel. This was an actual viewport failure,
not just a kernel test. The window was opened on the user's desktop using:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup --enable-event-simulate --window-geometry 100 70 1400 900 --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_tapered_round_box_workflow --keep-open
```

Log: `/tmp/nurbs-budget-desktop-before.log`. Screenshot:
`/tmp/nurbs-budget-desktop-before.png`. The generated scene was saved as
`/tmp/blender-nurbs-bevel-ui/bevel-native-connected-round-tapered-before.blend`.
The background operator reproduction, preserving the full error, is
`/tmp/nurbs-budget-background-before.log`.

## Root cause and correction

Scaling the top creates trapezoidal side faces. These faces remain flat, but
their bilinear NURBS parameterization is not affine: its mixed coefficient can
be large even when the whole surface lies exactly in a plane. The old projection
path treated that coefficient as unsupported geometric deviation. The previous
rotated/sheared parallelogram fixtures did not exercise this ordinary edit.

`reparameterize_affine_planes` is a shared BKE construction service. It proves
whole-support planarity using bounded coordinate projection of all four positive
polynomial support coefficients. It then allocates a private affine plane chart
and new p-curves for each requested face. The chart encloses the positive spatial
boundary control hull. Spatial boundary curves, vertices, edges, face/use IDs,
face senses, material data, selection and other owners of the original support
are retained. No fitting or display-mesh editing occurs.

The service validates the original boundary tolerance before changing its chart,
checks source orientation, records plane/projection/construction error, and
rejects a genuinely curved or folded source that cannot meet its contract.
Identity exhaustion and other failures leave the source unchanged. Bevel requests
these charts for affected polynomial faces whose non-affine mixed coefficient
exceeds the operation budget, then runs the existing common bevel planner.
The result reports changed charts and their geometry bound. The same 1e-9
default budget is retained; it was not increased to make this case pass.

The first narrower-top test also caught inconsistent proof use: construction
certified a boundary within 5.0832863829678873e-16, while validation used a
different, more conservative bound of 5.3599454956542642e-16. The evaluator now
uses the minimum of its valid whole-curve certificates, including the bilinear
certificate used by construction. The minimum of valid upper bounds is still
a valid upper bound. Neither source tolerance nor an evaluation allowance is
silently enlarged. The initial failure is retained in
`/tmp/nurbs-planar-chart-targeted-tests.log`.

## Build and kernel validation

These commands returned zero:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test blenkernel_nurbs_transform_test blenkernel_nurbs_tessellate_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_topology_edit_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/nurbs-planar-chart-final-build.log`,
`/tmp/nurbs-planar-chart-test-build.log`,
`/tmp/nurbs-planar-chart-final-install.log`.

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition BLENDER_NURBS_TRIANGLE_OUTPUT=/tmp/blender-nurbs-triangle-ui BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-bevel-ui BLENDER_NURBS_BEVEL_UI_READY=1 BLENDER_NURBS_BEVEL_MOUSE_UI_READY=1 BLENDER_NURBS_BEVEL_CLAMP_UI_READY=1 BLENDER_NURBS_BEVEL_BLEND_OUTPUT=/tmp/blender-nurbs-bevel-coons-ui BLENDER_NURBS_BEVEL_BLEND_UI_READY=1 BLENDER_NURBS_BEVEL_PLANAR_OUTPUT=/tmp/blender-nurbs-planar-ui BLENDER_NURBS_BEVEL_PLANAR_UI_READY=1 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
```

Results: **61 topology + 31 transform + 12 tessellation = 104 passed, no skips**.
The topology suite took 83.345 seconds. The five CTest cases passed in 9.34 seconds.
Logs: `/tmp/nurbs-planar-chart-final-topology-tests.log`,
`/tmp/nurbs-planar-final-transform-tests.log`,
`/tmp/nurbs-planar-final-tessellation-tests.log`,
`/tmp/nurbs-planar-chart-final-ctest.log`.

New tests check top X scales 0.5, 1.3 and 2.0 at one, three and eight segments,
actual BMesh chamfer coordinates, source preservation, source identity exhaustion,
rejection of a genuinely curved face, and all 4,095 nonempty edge selections on
the tapered box. The existing 4,095 selections on a sheared box and the ordinary /
clamped chamfer subset tests also ran. Saved viewport sources replay against BKE,
including exact cancel and undo/redo restoration, Edit Mode reentry and continued G.

## Viewport validation and visual confirmation

Nine independent workflows passed. Two were fresh, visible desktop windows:

```sh
env BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-planar-ui /home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup --enable-event-simulate --window-geometry 100 70 1400 900 --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_tapered_round_box_workflow
env BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-planar-ui /home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup --enable-event-simulate --window-geometry 100 70 1400 900 --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_tapered_round_face_workflow
```

Logs: `/tmp/nurbs-planar-visible-box.log`, `/tmp/nurbs-planar-visible-face.log`.
Both used Ctrl+B and wheel events, cancel, confirm, undo/redo, Tab reentry,
movement and Shift+D. They closed automatically after completing the checks.
No user's scene was loaded or altered.

The clamped tapered-box case also passed using the headless command template:

```sh
env -u DISPLAY BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-planar-ui BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_tapered_clamped_round_box_workflow
```

Log: `/tmp/nurbs-planar-clamped-box-ui.log`.
`python3 /tmp/nurbs-planar-run-ui.py` ran the remaining six cases, with exact
arguments, exit codes and log paths in `/tmp/nurbs-planar-ui-results.json`:

- Actual mesh references: `bevel_tapered_round_box_mesh_workflow` and
  `bevel_tapered_round_face_mesh_workflow`.
- Mouse width/replay: `bevel_tapered_round_mouse_workflow`. It tests three actual
  mouse distances, requires different visible geometry at each width, and requires
  the same geometry when returning to a previous distance. Then it cancels and
  completes the ordinary confirmation / continued-edit workflow.
- Regressions: `bevel_rotated_clamped_round_box_workflow`,
  `bevel_clamped_round_mouse_workflow`, `bevel_split_vertex_deformation_workflow`.

The following final captures were inspected (under `/tmp/blender-nurbs-planar-ui`):

- `bevel-native-connected-round-tapered-finished.png`
- `bevel-native-connected-round-tapered-face-shaded.png`
- `bevel-native-connected-round-clamped-tapered-shaded.png`

The same saved before-scene that failed now returns FINISHED through the Python
operator; `/tmp/nurbs-budget-background-fixed.log` records two reparameterized
flat faces and a chart error bound of 2.380739294065477e-16 source units.

The installed binary is `/home/molotovgirl/Desktop/build_linux/bin/blender`,
built 2026-10-26 17:33:30. These results establish the reproduced planar-layout
fix and listed regressions. They do not establish universal curved-face fillets
or certified G1 continuity of the general corner blends. Changes remain local
and unpushed. The user-owned handoff SHA256 remains
`8f8fcc63d9fa66ce55eda5673826dbfd9f1da2f4cdb9696b5bbb52916f9d5d9a`.

Clang-format 20.1.8 `--dry-run --Werror` passed on the seven changed C++ headers
and implementations. Python byte-compilation of `test_nurbs_native.py` and
`git diff --check` also returned zero.
