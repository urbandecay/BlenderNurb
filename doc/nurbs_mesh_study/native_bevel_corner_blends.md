# Native bevel: angled corners and continued face editing

Subsequent correction: [flat faces after reshaping](native_bevel_planar_chart_fix.md).
That report reproduces the planar projection error on a resized box and records the newer
visible desktop tests, kernel checks and construction/validation correction.

This follows `native_bevel_robustness.md`. The previously reported
`Rounded three-edge NURBS corners require right angles within source precision`
restriction is replaced for supported straight, manifold, three-edge fans.
The native Blender kernel remains authoritative. No external kernel was linked.

## Geometry and topology changes

The corner planner retains the certified spherical octant for right-angle corners.
Other three-edge corners receive a positive rational quadratic boundary patch.
Each of its three conic boundaries is shared by identity with the neighboring
bevel strip. Its poles are planned on the corresponding original edge lines.

`rational_coons_triangle` is a reusable BKE construction service. It forms a
homogeneous Coons patch from three rational quadratic curves and a collapsed
fourth boundary. Boundary coefficients are copied exactly. The ordinary Coons
interior has a bounded coefficient/weight construction error. An optional tensor
shape correction supplies an exact interior control and collapsed-boundary weight.
The bevel uses that correction to meet the existing spherical construction
continuously as the input angles approach 90 degrees. Positive weights are
required throughout; boundaries are never fitted or replaced with polylines.
The planner accounts separately for errors in constructing its conic poles,
angle weights and shape weights.

Two selected rounds meeting at one vertex previously constructed incompatible
curves for their shared junction. The planner now defines that profile once per
fan. Unequal angle weights use a common conic weight; both strips consume that
same geometry. This is an explicitly interpolated NURBS blend, not a claim of a
constant circular radius through an arbitrary junction.

`ruled_surface` now accepts different positive boundary weight profiles with the
same basis. The two source curves are copied exactly. Every generator remains
straight, with a rational parameterization when the weights differ. This avoids
rejecting compatible boundaries solely because their projective speeds differ.

The editor reports boundary-blend use in the modal status and Python execution.
The BKE result records the number of such junctions. These constructions provide
shared positional boundaries (G0). Tangent continuity (G1), curvature continuity
(G2) and a constant-radius general corner are **not certified**. The exact
right-angle spherical/circular cases retain their earlier stronger tests.

## SMLib reference and actual reuse

The reference was the public NVIDIA SMLib source at revision
`3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5`:

- [SmFilletCorner.cpp](https://github.com/NVIDIA-Omniverse/usd-brep/blob/3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5/source/SMLib/src/SmFilletCorner.cpp),
  `CreateCornerPatch` and `MakeAnalyticCornerSurface`: attempt an analytic corner,
  then construct a patch from the ordered boundary curves. A three-sided corner
  is represented with a collapsed fourth boundary.
- [SmBSplineSurfaceNLib.cpp](https://github.com/NVIDIA-Omniverse/usd-brep/blob/3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5/source/SMLib/src/SmBSplineSurfaceNLib.cpp),
  `CreateCornerBlend`: distinguishes boundary interpolation from the separate G1
  construction using neighboring surfaces, UV curves and cross tangents.

The native implementation adapts that analytic-then-boundary-patch strategy and
implements the homogeneous construction using Blender's bounded arithmetic and
existing data model. This is algorithm reuse. SMLib's entire corner solver,
intersection system, derivative-based G1 construction and trimming engine were
not ported. No SMLib source or binary dependency was added to the Blender build.

## Editing the result

The viewport test also reproduced a second error after a top-face bevel: G on
the selected round did nothing, and Python reported
`Partial trimmed NURBS face editing requires a deformation rule`.

The retained flat neighboring face could have a nonrectangular four-sided trim.
The previous displacement service handled rectangular trims and triangles.
`reparameterize_planar_quads` now gives a supported convex straight-sided quad a
complete bilinear corner chart before the existing corner transform is applied.
It allocates private supports and p-curves, retaining the face, edge, use and
vertex identities, the spatial boundary curves, selection and face senses.
Other owners of the old geometry remain unchanged. Both UV winding directions
are tested. Existing rectangular and genuinely curved cases retain their own
routes and contracts.

The whole-face discrepancy is bounded by the input support's bilinear twist plus
the certified input boundary gap. Both normalized UV products lie in [0,1], so
their difference is bounded by one; endpoint errors enter as a convex combination.
New boundary correspondence is checked separately. Invalid input boundaries and
budgets too small for the measured deviation reject atomically. The transform
reports the number of changed charts and their whole-face error bound; that bound
is combined with the transform's arithmetic and boundary errors within the
explicit operation budget. There is no mesh source or hidden fitting allowance.

## Validation commands

Build and install, from the Blender source root:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test blenkernel_nurbs_transform_test blenkernel_nurbs_tessellate_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Build logs: `/tmp/nurbs-blend-final-build.log` and the final comment/documentation
cleanup rebuild `/tmp/nurbs-blend-release-build.log`. Initial targeted compilation caught
test code treating `PointEvaluation` as a vector; that test was corrected and the
build rerun. `/tmp/nurbs-coons-build.log` retains the initial compiler failure.

Kernel suites:

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition BLENDER_NURBS_TRIANGLE_OUTPUT=/tmp/blender-nurbs-triangle-ui BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-bevel-ui BLENDER_NURBS_BEVEL_UI_READY=1 BLENDER_NURBS_BEVEL_MOUSE_UI_READY=1 BLENDER_NURBS_BEVEL_CLAMP_UI_READY=1 BLENDER_NURBS_BEVEL_BLEND_OUTPUT=/tmp/blender-nurbs-bevel-coons-ui BLENDER_NURBS_BEVEL_BLEND_UI_READY=1 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
```

Logs: `/tmp/nurbs-blend-final-{transform,tessellation,topology}-tests.log` and
`/tmp/nurbs-blend-final-ctest.log`.

Kernel result: **58 topology + 31 transform + 12 tessellation = 101 passed,
no skips**. The topology suite took 50.253 seconds in the recorded full run.
The five CTest cases all passed in 8.31 seconds. Both final build commands and
the install returned zero; install log: `/tmp/nurbs-blend-final-install.log`.

The new subset test checks all 4,095 nonempty edge selections of a box sheared
in three directions, at three rounded segments. It checks shared two-use
incidence, opposed face orientations, source boundary evaluation and unchanged
input geometry/counters. It first failed at selection mask 5 with unequal
profiles; `/tmp/nurbs-coons-subsets-before-joints.log` retains that failure.
The first successful complete run is `/tmp/nurbs-coons-subsets-after-joints.log`.
This supplements the existing ordinary/clamped chamfer checks on all cube subsets.

Additional tests cover full rotated and skewed boxes, defined display normals,
the spherical limit for shears 1e-10, 1e-7 and 0.001, independent homogeneous Coons
evaluation, unequal-weight ruled generators, invalid boundaries and budgets,
private chart ownership and winding. Saved viewport sources are replayed against
the BKE bevel and transform services, checking exact source/selection restoration
after cancel, undo/redo and Edit Mode reentry. Continued G and Shift+D results
are validated as authoritative NURBS sources with no extra evaluation allowance.

Viewport command template (substitute the function name):

```sh
env -u DISPLAY BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-bevel-coons-ui BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_skewed_round_box_workflow
```

`python3 /tmp/nurbs-blend-run-ui.py` runs eleven independent factory scenes in
groups of three. Exact arguments, exit codes and log paths are recorded in
`/tmp/nurbs-blend-ui-results.json`; summary: `/tmp/nurbs-blend-ui-summary.log`.
All eleven returned zero:

- New native cases: `bevel_skewed_round_box_workflow`,
  `bevel_skewed_round_face_workflow`, `bevel_skewed_clamped_round_box_workflow`.
  The source is created through line extrusion, face extrusion and corner-region
  movement. These tests invoke Ctrl+B, scroll to three segments, cancel, confirm,
  undo/redo, reenter Edit Mode, move the result and duplicate it.
- Actual mesh references: `bevel_skewed_round_box_mesh_workflow`,
  `bevel_skewed_round_face_mesh_workflow`. The top-face round has matching logical
  counts of 20 vertices / 36 edges / 18 faces. After duplicating the selected ring,
  both have 36 / 64 / 30. This does not claim identical curved/polygon positions.
- Regressions: `bevel_round_workflow`, `bevel_round_box_workflow`,
  `bevel_connected_round_workflow`, `bevel_rotated_clamped_round_box_workflow`,
  `bevel_clamped_round_mouse_workflow`, `bevel_split_vertex_deformation_workflow`.
  These use `/tmp/blender-nurbs-bevel-ui` to refresh their existing fixtures.

The retained background reproduction of the continued-movement failure is
`/tmp/nurbs-coons-face-transform-repro.log`.
`/tmp/nurbs-coons-face-transform-fixed.log` records the fixed native operation and
its reported whole-face chart error of 1.8181389484649544e-16 local units.

The following final screenshots were visually inspected in addition to the
automated checks (all under `/tmp/blender-nurbs-bevel-coons-ui`):

- `bevel-native-connected-round-skewed-shaded.png`
- `bevel-native-connected-round-skewed-face-shaded.png`
- `bevel-native-connected-round-clamped-skewed-shaded.png`
- `bevel-mesh-connected-round-skewed-shaded.png`
- `bevel-mesh-connected-round-skewed-face-shaded.png`

The captures show the generated bevels and their shaded surfaces. They do not
establish global intersection freedom or G1 continuity for arbitrary geometry.

The public Python operator also passed on the generated angled box with 2, 8,
32 and 100 segments. Each result stayed in Edit Mode with native NURBS source
and no legacy splines; derived geometry was evaluated successfully. Command:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup --python-exit-code 1 --python /tmp/nurbs-blend-python-check.py
```

Log: `/tmp/nurbs-blend-python-api.log`. The four segment counts produced 3,836,
11,900, 44,156 and 135,548 derived triangles, respectively. The largest reported
construction bound was 6.6042420119008978e-13 source units, below the explicit
1e-9 budget. These triangles are measurements of derived display output.

Clang-format 20.1.8 `--dry-run --Werror` passed on the ten touched C++ headers
and implementations. `python3 -m py_compile tests/python/ui_simulate/test_nurbs_native.py`
and `git diff --check` returned zero. The installed binary is
`/home/molotovgirl/Desktop/build_linux/bin/blender`, built 2026-10-26 17:12:43.

## Scope

This fixes supported nonorthogonal three-edge rounds, unequal two-edge junctions
and continued editing of their flat four-sided neighbors. It does not establish
general curved-support intersections, arbitrary higher-valence round corners,
vertex bevel, custom profiles or global self-intersection handling. Coons-family
corners are positionally connected blends; they are not certified G1 fillets.

Builds, kernel tests, actual Ctrl+B tests and visual inspection of generated
scenes are separate evidence. No user's project was loaded. Changes are local;
no commit or push was made. The handoff remains unchanged (SHA256
`8f8fcc63d9fa66ce55eda5673826dbfd9f1da2f4cdb9696b5bbb52916f9d5d9a`).
