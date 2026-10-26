# Per-face NURBS surface wire grids

## Requested behavior

The surface grid is a nonselectable visual aid. Rectangular patches should keep
their quadrilateral display grid, triangular faces should show a triangular
grid, and collapsing/deleting geometry on one island must not change the grid
on an untouched island. Actual topology edges retain Blender's component
selection overlay and shared source identities.

This is the requested Blender-fork display behavior. It is not a claim that
Rhino or Alias chooses a quad/triangle grid according to the face outline.

## Reproduction before the fix

The new `surface_wire_grid_workflow` creates a tagged NURBS line, extrudes it
into a face and cube, duplicates the cube within the same object, then collapses
the right cube's top edge through the actual M popup. The untouched left cube
switches from a quad grid to a triangular wire grid when the owner enters native
NURBS editing. The mean RGB screenshot difference on the untouched cube was
`0.0367838465949515`, exceeding the test's `0.005` threshold.

Baseline screenshots and source files:
`/tmp/blender-nurbs-grid-baseline/grid-edge-{before,collapse}.{png,blend}`.
Baseline log: `/tmp/blender-nurbs-grid-baseline.log` (expected test failure).

## Shared implementation

`SurfacePreviewTriangle` carries three display-edge flags, separate from its
triangle vertices, per-corner UVs, face identity and source geometry. The shared
BKE tessellator classifies each sampled face chart independently. A single
rectangular UV outline uses U/V grid wires and omits cell diagonals; other trims
retain the triangular preview grid. A rectangular chart may have multiple
topology edges along one side. Closed rational seams are classified in UV, not
by their number of distinct 3D vertices or edges. Face orientation is applied
before assigning each triangle's edge flags.

`evaluate_mesh` transfers those flags to the mesh renderer's existing optimal
display edge mask (historically named `subsurf_optimal_display_edges`). This is
the established mechanism that subdivision uses to hide generated edges. It is
already consumed by both mesh wireframe shaders and line-index extraction,
copied with evaluated meshes, and invalidated after mesh topology changes.
Boundary and loose source edges are always retained. Visibility is combined
across incident preview triangles, so one face cannot hide a shared boundary.

The preview remains triangulated for shading, source-hit resolution and
analytic corner normals. No mesh faces are merged into source NURBS faces. No
coefficients, weights, knots, trims, tolerance, source identities, selection or
file format are changed. No new operator or source editing mode is added.

The shared `curve_preview_options` service also reconciles preview density:
legacy two-by-two surfaces have `2 * resolution` samples, including both ends,
whereas the earlier native preview used only `resolution` intervals. Native
evaluation and the read-only tagged edit overlay now use `2 * resolution - 1`
intervals. The default equal U/V settings therefore keep the same grid when
the source enters native editing. As before, the native preview uses a uniform
grid based on the larger U/V setting. This does not reproduce arbitrary
per-spline or unequal U/V legacy sampling, and increases density compared with
the earlier native path; the existing preview budgets remain enforced.

The denser, nonbinary grid exposed a separate numerical issue: independently
evaluated straight curves and bilinear surfaces can differ by approximately
`1.11e-16` even when the represented boundary is mathematically identical. With
zero source tolerance and zero evaluation allowance, drawing previously failed.
`evaluate_boundaries` now uses a sufficient exact coefficient identity proof
for equal-weight linear curves/p-curves on bilinear supports. Natural sides are
proved from their matching corner coefficients; general straight trims also
require an exactly affine support and exact endpoint correspondence. TwoSum
and FMA product residuals check every arithmetic step used by the proof. A
nonrepresentable proof retains the existing sampled checks.

The result reports `exact_curve_support_identity` and still retains both raw
measured discrepancies. Topological endpoint/curve agreement is still checked.
No epsilon is introduced and no modeling tolerance or evaluation allowance is
increased. A regression deliberately displaces a used support coefficient by
`2^-40`; that actual gap still fails at zero tolerance.

## Scope and limits

Rectangular NURBS charts, including curved ruled patches, use their U/V grid.
Triangular faces use the existing sampled triangular tessellation as their
nonselectable grid; this does not introduce a new triangular NURBS basis.
Other nonrectangular trims, including holes and irregular outlines, retain
their triangular display grid. A four-edge trim is not classified as a full
rectangular chart merely because it has four edges. A dedicated regular grid
for arbitrary curved four-sided trims would require a separate trimmed
parameter-grid service.

Classification is based on the already sampled trim polygon and exact UV
coordinate comparisons; it is a display choice, not an exact trim-validity or
approximation-error certificate. It never changes the modeling tolerance or
merges near-equal source points. The existing uniformly sampled preview has no
certified world-space approximation error bound.

## Validation

Implemented, built, installed and tested locally. The resulting screenshots
were inspected. Not committed or pushed by this change.

New kernel coverage checks grid counts at resolutions 1, 4, 7, 12 and 13; retained
render triangles and source identities; evaluated mesh copy/invalidation;
triangle side faces alongside a quad base after cube-top Collapse; deletion of
the apex; reversed face orientation; holed trims; and a closed exact rational
cylinder with a shared seam.

The viewport workflows compare the untouched cube before/after a top-edge or
whole-top Collapse, vertex deletion and Edit Mode reentry. They cover wireframe
shading and solid shading with the wire overlay and All Edges enabled. They
also exercise undo/redo and verify that an interior display-grid intersection
does not become a selectable topology vertex. Mesh and NURBS click picking
both use `ED_view3d_select_dist_px()` (75 pixels times UI pixel size); a guide
click can select a nearby real corner. The test checks its active source
position against the actual corners, rather than incorrectly expecting every
interior click to deselect everything.

### Commands and results

Build commands (all final commands exited 0):

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_mesh_test blenkernel_nurbs_tessellate_test blenkernel_nurbs_object_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_geometry_test blenkernel_nurbs_fill_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_mesh_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/blender-nurbs-grid-agreement-build.log`,
`/tmp/blender-nurbs-grid-agreement-test-build.log`,
`/tmp/blender-nurbs-grid-final-test-build.log`,
`/tmp/blender-nurbs-grid-install.log`.

Kernel commands (99/99 tests passed, no skips):

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_mesh_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_object_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test --test-assets-dir '/home/molotovgirl/Desktop/Blender Fork/tests/files' --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_fill_test --gtest_color=no
```

Results: mesh 10/10, tessellation 12/12, object evaluation 4/4, geometry/owner
55/55, Fill 18/18. Logs: `/tmp/blender-nurbs-grid-{mesh,tessellate,object,geometry,fill}-tests.log`.

Actual viewport invocations, run from the Blender checkout (the loops enumerate
the individual commands; each test ran in its own Blender/Weston process):

```sh
for test in surface_wire_grid_workflow surface_wire_grid_top_workflow; do
  env -u DISPLAY \
    BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
    WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
    WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
    WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
    BLENDER_NURBS_GRID_OUTPUT=/tmp/blender-nurbs-grid-ui \
    python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
      --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.$test
done
for test in surface_wire_grid_solid_workflow mesh_overlay_parity_workflow; do
  env -u DISPLAY \
    BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
    WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
    WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
    WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
    python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
      --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.$test
done
```

All four workflows passed. The overlay parity workflow compares native NURBS
against actual mesh screenshots in V/E/F modes with X-Ray on/off. Logs:
`/tmp/blender-nurbs-grid-{edge,top,solid,overlay}-ui.log`.

Mean RGB differences on the untouched cube (threshold `0.005`, including small
legacy-float versus native-double evaluation/drawing differences):

| Case | After Collapse | After Delete |
| --- | ---: | ---: |
| Top edge, wireframe | 0.00231084 | 0.00229765 |
| Whole top, wireframe | 0.00229765 | 0.00229765 |
| Top edge, solid + wires + All Edges | 0.00123124 | 0.00120231 |

Compatibility command:

```sh
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^script_nurbs_(line|legacy_compat)$'
```

2/2 passed. Log: `/tmp/blender-nurbs-grid-compat-tests.log`.
`git diff --check`, C++ clang-format checks and Python compilation passed.

After installation, a fresh background process reopened the actual saved
before/Collapse/Delete files and evaluated each. It confirmed retained legacy
ownership before promotion, retained native NURBS with empty legacy splines
after promotion, and nonempty derived display geometry in all three cases:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup --python /tmp/blender-nurbs-grid-reload.py
```

Exit 0, three `NURBS_GRID_RELOAD_PASS` results. Script:
`/tmp/blender-nurbs-grid-reload.py`; log: `/tmp/blender-nurbs-grid-reload.log`.

Screenshots and native source files are under `/tmp/blender-nurbs-grid-ui`,
named `grid-{edge,top,edge-solid}-{before,collapse,delete,reenter}.{png,blend}`.
The wireframe edge/top Collapse, edge Delete, solid-wire Collapse and normal
native selected-face overlay screenshots were inspected. These are automated
viewport events plus screenshot inspection, not a claim of testing the user's
running Blender instance.

During validation, the initial build caught an unavailable `bool3` alias (fixed
with `std::array`). The new copy test initially used a Main-database copy with
no Main; it now uses Blender's `BKE_mesh_copy_for_eval`. An exact world-space
float equality assertion was corrected to account for display evaluation
roundoff, while retaining exact wire-count checks. The negative gap fixture
was corrected to alter a live face support rather than an unused retained
support. Geometry-owner tests were rerun with their required test-assets flag.
An install attempt while test Blender processes were running could not update
RPATH; installation passed after those processes exited. Those failures were
resolved before reporting the final passing results.
