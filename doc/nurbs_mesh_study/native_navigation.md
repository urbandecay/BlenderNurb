# Native NURBS selection navigation

## Implemented and built

Native NURBS Edit Mode now supplies selected source positions to Blender's
existing Orbit Around Selection and Frame Selected paths. The existing
Preferences > Navigation > Orbit Around Selection switch controls the behavior;
no separate NURBS preference is introduced. Turntable/Trackball, sensitivity,
smooth view and the other view navigation settings keep their Blender ownership.
This change does not claim that every preference or every NURBS editor feature
has been audited.

The user confirmed mesh behavior as the reference after the curved-edge pivot
question. Orbit uses the arithmetic mean of unique selected topology endpoints,
including the endpoint closure of selected edges/faces. Frame Selected uses the
world-space bounds of those endpoints. No whole-curve centroid, control-net
pivot, display-resolution-dependent center or new fit point is invented. A
closed rational edge can have a single seam endpoint; this change preserves that
existing topology/handle contract. Persistent fit-point handles remain separate
planned work.

## Shared implementation

`bke::nurbs::selection_positions` normalizes the stored V/E/F selection using
the existing kernel flush rules and returns unique endpoint positions in source
order and double precision. It validates references, retains periodic shared
identity and performs no source mutation or identity allocation.

`ed::nurbs::view_selection_positions` takes a retained source snapshot, applies
the requested object/relative-object matrix in double precision, then creates
checked float viewport proxies. An unrepresentable coordinate rejects the whole
query. No proxy writes back into the NURBS source.

The native branch in `gizmo_3d_foreach_selected` supplies those positions to the
shared median-pivot reader used by `view3d_orbit_calc_center`. It also makes them
available to the existing gizmo selection statistics. It preserves the ordinary
Curve/Bezier branch when no native source exists. The native branch in
`ED_view3d_minmax_verts` supplies world-space positions to Frame Selected.
Both use the existing authoritative source capture service; a failed native
capture never falls through to stale legacy points or a display mesh.

Blender retains its last valid orbit pivot on empty selection and makes Frame
Selected a no-op. Multiple edited objects are transformed into the active
object's common space for orbit, and world space for framing. Shared-data
instances follow Blender's actual Edit Mode flags, matching the mesh reference.

## Commands and results

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_selection_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_selection_test --gtest_color=no
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

Build/install: exit 0, logs `/tmp/blender-nurbs-navigation-build-fixed.log` and
`/tmp/blender-nurbs-navigation-install.log`. The initial build caught an attempted
range loop over a math vector; it was replaced with explicit axis iteration
before the successful build and UI checks. Existing unused-variable warnings
in the view3d sources are unrelated to the new branch.

Selection suite: **42/42 pass**, zero skips, exit 0,
`/tmp/blender-nurbs-navigation-selection-tests.log`. New tests compare selection
positions against actual BMesh across all seven selection mode combinations and
multiple layouts/masks. They check double precision, repeated periodic identity,
empty input and rejected invalid selection/nonfinite source.

Selected CTest: **5/5 pass**, exit 0, 9.13 seconds,
`/tmp/blender-nurbs-navigation-ctest.log`.

Viewport command (one TEST_ID per process):

```sh
nice -n 10 env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  BLENDER_NURBS_NAVIGATION_OUTPUT=/tmp/blender-nurbs-navigation-ui \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

```text
test_nurbs_native.navigation_selection_workflow
test_nurbs_native.navigation_multi_owner_workflow
test_nurbs_native.navigation_legacy_curve_workflow
```

All three pass, exit 0:

- `/tmp/blender-nurbs-navigation-ui.log`: actual mesh and native V/E/F picking;
  real MMB orbit in Turntable and Trackball; preference on/off; rotation,
  negative/nonuniform owner scale and location; unchanged screen position of the
  selected orbit center; comparison of view transforms against actual mesh;
  Numpad Period framing, point-only zoom, empty-selection fallback and no-op;
  unchanged component counts and retained Edit Mode. The far-away 3D cursor is
  the transform pivot, verifying that orbit still chooses the selected median.
- `/tmp/blender-nurbs-navigation-multi-fixed-ui.log`: actual mesh/native
  independent owners and shared-data instances, with different object matrices;
  orbit median and selected bounds match. The initial test incorrectly assumed
  both shared-data instances entered Edit Mode. The actual mesh reference has
  `('EDIT', 'OBJECT')`; the corrected test and native code follow that rule.
- `/tmp/blender-nurbs-navigation-legacy-ui.log`: ordinary Bezier point orbit
  and framing remain functional through the existing legacy branch.

Visually inspected `/tmp/blender-nurbs-navigation-ui/navigation-NATIVE-FACE.png`:
the selected face is framed, with Face selection and Edit Mode retained.
Other behavior claims above come from the automated view/selection assertions.
UI comparison thresholds are viewport/float comparison tolerances, not source
geometry fitting tolerances. All navigation queries leave source geometry
authoritative and unchanged.

```sh
python3 -m py_compile tests/python/ui_simulate/test_nurbs_native.py
git diff --check
/tmp/blender-nurbs-deps-storage/llvm/bin/clang-format --dry-run --Werror source/blender/blenkernel/BKE_nurbs_selection.hh source/blender/blenkernel/intern/nurbs_selection.cc source/blender/blenkernel/intern/nurbs_selection_test.cc source/blender/editors/include/ED_nurbs.hh source/blender/editors/curve/editnurbs.cc source/blender/editors/transform/transform_gizmo_3d.cc source/blender/editors/space_view3d/view3d_snap.cc
```

All exit 0. The binary is `/home/molotovgirl/Desktop/build_linux/bin/blender`.
The navigation changes and prior topology work remain local/uncommitted/unpushed.
No user process was closed, and the user-owned handoff was not edited.
