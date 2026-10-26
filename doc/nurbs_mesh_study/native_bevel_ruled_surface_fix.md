# Ruled curved-surface bevel and fresh-runtime verification

The user clarified that the reported face came from an ordinary line → face →
extruded box. That workflow does not imply intentional curvature. A fresh test
using mouse translations already passed before the new geometry changes in this
report. The earlier suggestion that the input fix necessarily exposed curvature
was not established by the screenshot.

At the time of inspection, both open Blender processes referred to deleted,
replaced executables. They started at 12:25:23 and 13:49:21 local time, before
the preceding Ctrl+B fix was built at approximately 14:15. No user project was
opened or inspected. This establishes an outdated-runtime problem; it does not
identify which process produced the screenshot or prove the geometry in that
uninspected scene is planar.

## SMLib study

The source studied is NVIDIA's public revision
`3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5`:

- [SmFilletExecutive.cpp](https://github.com/NVIDIA-Omniverse/usd-brep/blob/3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5/source/SMLib/src/SmFilletExecutive.cpp):
  `CreateFilletBrep` and `GetPreviewFilletSurfaces` separate computing surfaces
  and rails from corner/end trimming and topology insertion.
- [SmFilletSolver.cpp](https://github.com/NVIDIA-Omniverse/usd-brep/blob/3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5/source/SMLib/src/SmFilletSolver.cpp):
  `CalcFilletGeom` attempts analytic cylinder/torus constructions before tracing
  general contact geometry.
- [SmFilletStandardSolver.cpp](https://github.com/NVIDIA-Omniverse/usd-brep/blob/3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5/source/SMLib/src/SmFilletStandardSolver.cpp):
  `SmConstantRadiusFS::LoadJacobian` solves equality of offset-surface positions
  using surface parameters and derivatives. The constant-distance solver adds
  the rail-distance constraint and solves the radius too. These semantics must
  be mapped deliberately to Blender's width setting.
  `SmCircularCrossSectionFSG::CreateSurfaceFromCurves` constructs cross sections,
  chooses ruled/revolved special cases, or skins sections with an explicit
  approximation tolerance and optional derivative constraints.

The native implementation adapts the separation of contact rails, blend
construction and end trimming. No SMLib source, data model, binary dependency
or general tracing solver was imported. Studying an algorithm is not evidence
that its complete general implementation was ported.

## Native geometry change

Previously every nonzero bevel went through planar projection. The added path
handles one natural shared boundary between two compatible clamped positive
rational ruled supports. The along-edge direction can be a higher-order rational
curve, including an arc. The transverse direction has two equal-weight rows;
support/edge bases and coefficients must agree. Four distinct natural corners,
regular endpoint incidence and compatible parameter domains are required.

Rails are exact source isocurves. The retained faces receive exact cropped
original supports through the existing restriction service. Shortened adjacent
edges retain their identities and are restricted with every incident p-curve.
The original sharp edge and its old endpoint identities are replaced by shared
rail/profile topology. Original coefficient resources remain unchanged.

`conic_curve_blend` is a reusable BKE construction service. It copies the two
rail rows and their common tangent-control curve into a positive quadratic
rational tensor support, reporting weight-construction roundoff. Because a ruled
support's transverse generators point toward that common curve, regular rounded
rail joins have the same tangent planes as the original faces. One segment uses
the existing exact ruled chamfer construction. Additional rounded segments use
the shared source partition service.

Width is measured perpendicularly in each surface's midpoint frame. Natural
parameter offsets can therefore have varying world width along a changing
surface frame. The conic family is not generally a constant-radius circular
fillet. The result records `curved_blend_strips`, and the modal header says
`Curved Blend`. Planar bevels keep their previous width and construction rules.
The local clamp limits the opposite tensor boundary; general curved
self-intersection certification remains absent.

Planar end caps are projected and trimmed, with private affine charts when a
flat cap has a non-affine bilinear parameterization. Curved caps still require
surface intersection. A closed-cap tessellation test caught differing last-bit
UV values at an existing shared corner. New profile p-curves now use the cap's
canonical corner UV values, with a whole-curve geometric discrepancy bound
checked before acceptance. No tessellator tolerance was relaxed.

Failures discard the candidate. Input topology, resources and ID counter remain
unchanged. New metadata uses source tolerances plus reported construction
roundoff, rather than assigning the entire maximum error budget as tolerance.
The editor's normal preview/commit/rendering path remains authoritative.

## Validation and evidence

The bent-sheet viewport reproduction failed before the change, retaining
6 vertices / 7 edges / 2 faces. Log:
`/tmp/nurbs-curved-ui-before.log`; saved source:
`/tmp/blender-nurbs-curved-before/bevel-native-corner-curved-round-before.blend`.

New geometry tests cover both open sheets and closed boxes with two bent
bilinear incident faces, one/three/eight segments, planar end caps, clamping,
zero-allowance boundary evaluation, tessellation and source preservation.
A separate rational curved-edge fixture checks both rail normals at five
parameters against the adjoining support normals to 1e-12, plus Python-style
kernel calls without an error-output pointer. The existing all-edge warped-box
test still rejects unsupported general networks without flattening their faces.

Build and install commands:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/nurbs-curved-path-release-build.log`,
`/tmp/nurbs-curved-path-install.log`. Executable:
`/home/molotovgirl/Desktop/build_linux/bin/blender`, build 2026-10-26 19:09:30,
modified branch `nurbs/shared-topology-foundation`, HEAD `824bc36589b6`.

The topology suite runs with the same saved-source fixture environment as
[the planar-chart checkpoint](native_bevel_planar_chart_fix.md). The first full
run passed all 63 tests with no skips in 150.528 seconds; log:
`/tmp/nurbs-curved-final-topology.log`. After final hardening the complete suite
passed again: 63 tests, no skips, 94.079 seconds. Log:
`/tmp/nurbs-curved-release-topology.log`.

Eleven factory-scene UI workflows passed. The runner is
`/tmp/nurbs-curved-run-ui.py`; exact commands, return codes and logs are in
`/tmp/nurbs-curved-ui-results.json`. They cover bent sheets, a bent closed box,
mouse-extruded ordinary boxes, the held-Ctrl mesh/native comparison, rounded
boxes, connected rounded corners, resized boxes, clamp and rotated mouse input.
Curved workflows exercise actual Ctrl+B, wheel segments, cancel, confirmation,
undo/redo, Edit Mode reentry and continued extrusion. Each process has an isolated
temporary user configuration. The final hardened binary also runs
`bevel_curved_round_box_workflow`; log `/tmp/nurbs-curved-release-box-ui.log`.
The rear view with overlays hidden was inspected in
`/tmp/nurbs-curved-cap-shaded.png`; it shows the rounded edge and closed end cap.

A fresh visible desktop process created the cube from a line and beveled all
its edges with Ctrl+B, producing 72 vertices / 120 edges / 50 faces. The test
passed and its finished viewport capture was inspected:
`/tmp/blender-nurbs-curved-visible/bevel-native-connected-round-finished.png`.
Log: `/tmp/nurbs-curved-final-visible-box.log`. A normal interactive process
can open the independently generated saved source to inspect the result:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup /tmp/blender-nurbs-curved-visible/bevel-native-connected-round-restored.blend
```

Five compatibility CTest cases passed in 9.41 seconds; log
`/tmp/nurbs-curved-final-ctest.log`. They are `script_nurbs_line`,
`nurbs_composition_fixture`, `script_nurbs_composition`,
`nurbs_composition_saved_sources` and `script_nurbs_legacy_compat`.
Clang-format dry-run, Python byte-compilation and `git diff --check` passed.
The unrelated missing `object.make_dupli_face` UI warning remains in the logs.

## Remaining scope

General curved support contact tracing, arbitrary trimmed faces, periodic closed
rails, multiple selected curved edges meeting at a corner, curved end-cap
intersection, general constant-radius fillets and global curved collision
handling remain unfinished. They need the corresponding native intersection,
continuation, fitting/error contracts and corner services; the new ruled path
does not establish those capabilities. Changes remain local and unpushed.
