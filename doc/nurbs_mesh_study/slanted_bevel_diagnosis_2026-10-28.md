# Slanted box bevel corner diagnosis

**Current status:** a subsequent affine-corner repair is retained locally and the application
has been rebuilt. The original investigation and rejected experiment below are historical.
See the retained repair section at the end for measured results and remaining limitations.
The later smoothed-mesh target is implemented as an explicit surface choice; see
[Subdivision-shaped bevel](subdivision_bevel_2026-10-28.md).

The input is a straight-sided slanted box (parallelepiped) before beveling. It is not an already rounded NURBS shape. The earlier suggestion that an existing curved input was being reduced to its topology cage does not explain this case.

## Confirmed code path

`nurbs_topology_bevel.cc` tests whether the three source edge directions at each corner are orthogonal. Right-angle corners use `rational_spherical_octant_surface`, set `spherical_support`, and subsequently receive the mesh reference vertex placement and whole-row cubic spline construction added in the previous repair.

Nonorthogonal corners take a separate path using `rational_coons_triangle`. That path preserves its three boundary conics, but certifies only positional agreement (G0); tangent continuity is not certified. It does not set `spherical_support`.

Consequently, for a sheared box:

- `needs_mesh_reference` is false when all eight corners are nonorthogonal; the BMesh guide is not built.
- Mesh reference corner vertex placement is skipped.
- The regular chart and whole-row spline solve are skipped.
- Each internal grid edge is instead made with `surface_parameter_line` between its independently placed UV endpoints. Such segments can have different spatial tangents at a shared vertex.

This is a discontinuous algorithm switch as the input departs from a right angle. It explains why a successful regular cube test did not establish robust bevel curve flow.

## Reproduction and measurements

Created the existing two-unit box fixture, then applied shear `y += amount * x` before beveling all twelve edges. Width 0.25; tested 8 and 13 segments. Both NURBS and the real BMesh bevel received the same straight-sided source and width.

Measured the maximum nearest BMesh vertex distance and the maximum angle discontinuity between topologically opposite grid edges at four-valent vertices. Tangents were estimated by samples 0.00001 of the edge parameter interval from each endpoint; the tiny residual in the regular case includes this finite sampling error. These angles describe curve joins, not surface normal continuity.

| Shear | Segments | Blend corners | Maximum vertex distance | Maximum grid join angle |
|---|---:|---:|---:|---:|
| 0 | 8 | 0 | 0.0000000520 | 0.000263 degrees |
| 0 | 13 | 0 | 0.000000107 | 0.000277 degrees |
| 0.000001 | 8 | 8 | 0.0189189 | 54.9256 degrees |
| 0.000001 | 13 | 8 | 0.0141411 | 39.6565 degrees |
| 0.35 | 8 | 8 | 0.0262597 | 64.1663 degrees |
| 0.35 | 13 | 8 | 0.0196639 | 43.0025 degrees |

The nearly zero shear changes the input shape negligibly, yet the curve network changes substantially. This isolates the construction switch rather than merely a visual difference caused by the slanted box.

The diagnostic ran successfully as `NurbsTopologyEditTest.DiagnosticSlantedBevelCurveFlow`. It prints measurements; its successful exit does not mean the defective geometry passed a quality acceptance test. Temporary diagnostic source was removed from the repository afterward and saved at `/tmp/nurbs-slanted-diagnostic-test.cc`. Results: `/tmp/nurbs-slanted-diagnostic-results.log`. Build: `/tmp/nurbs-slanted-diagnostic-build.log`.

## Why existing coverage missed it

Right-angle corner tests compare points with BMesh and inspect complete curves. Skewed-box coverage checks closed incidence, boundary agreement, accepted geometry and tessellation. Those checks establish a valid connected model; they do not check that corner grid trajectories match BMesh or join smoothly. The near-right-angle continuity test checks the surface's distance to the expected rounded box, not the layout of curves across it.

## Repair direction

The nonorthogonal path needs the same geometric requirements as the orthogonal path: corresponding mesh bevel corner points, globally coordinated complete curve rows, and correct spatial tangent joins. Its current Coons support may also differ from Blender's target corner surface, so simply enabling the sphere chart on skewed corners is not valid. First compare the desired mesh corner against that support; if incompatible, construct compatible NURBS patches from Blender's actual corner geometry with shared boundaries and tangent constraints. Increasing viewport tessellation cannot repair these stored curve defects.

Blender's mesh implementation contains both a mapped cube-corner construction and a general cubic subdivision/resampling construction. Its dispatch is broader than the native NURBS branch's strict orthogonality test. Reference: [Blender BMesh bevel source](https://github.com/blender/blender/blob/main/source/blender/bmesh/tools/bmesh_bevel.cc).

No production bevel code was changed in this investigation. The reproduction uses a generated slanted box, not the user's exact saved scene; exact screenshot correspondence still requires the same source shape, bevel width and segment settings.

## Rejected repair experiment

After authorization to make the bevel robust, tried extending the complete-row cubic interpolation to nonorthogonal supports. Added exact degree-twelve rational composition of a cubic UV curve with a biquadratic support using outward interval coefficient arithmetic. End directions were transported into the support using its analytic Jacobian.

This experiment establishes that extending the curve solve alone is insufficient:

| Shear | Segments | Maximum row join angle before | After curve-only extension |
|---|---:|---:|---:|
| 0.000001 | 8 | 54.9256 degrees | 0.000933 degrees |
| 0.000001 | 13 | 39.6565 degrees | 0.000395 degrees |
| 0.35 | 8 | 64.1663 degrees | 11.5709 degrees |
| 0.35 | 13 | 43.0025 degrees | 11.5709 degrees |

The interior spline joins become smooth, but a spatial tangent requested at an incoming strip cannot in general be represented by the corner's different tangent plane. Projecting the direction through the support Jacobian leaves a seam mismatch. Mesh-reference vertex deviations are unchanged. The six-case diagnostic also increased from roughly 0.62 seconds to 3.68 seconds; this is total diagnostic execution time, not an interactive frame benchmark.

The diagnostic and the existing rotated/skewed construction and near-right-angle surface tests all executed without structural rejection. That result demonstrates why structural acceptance is insufficient for visual quality. The experimental production changes and temporary test were removed. Patch retained for investigation at `/tmp/nurbs-general-corner-rejected.patch`; measurement log at `/tmp/nurbs-general-corner-results.log`. Existing local rotation performance work was preserved.

The remaining repair must construct the support and grid together. Required acceptance checks include shared boundary position, tangent-plane agreement along whole corner/strip seams, smooth opposite grid-edge tangents, point and shape comparison against Blender, and construction time. Current API also explicitly rejects certain higher-valence corner fans; fixing three-sided slanted corners alone cannot establish general mesh-bevel coverage.

## Retained repair: match Blender's mapped cube corner

Inspection of `bmesh_bevel.cc::bevel_build_rings` established that Blender's
`tri_corner_adj_vmesh` maps the reference cube corner into the actual three-dimensional
corner frame, including its boundary profile vertices. This construction is an affine image
of the reference rounded corner. A Euclidean sphere is its right-angle special case.

The retained change replaces the separate Coons construction for nonorthogonal three-edge
corners with the existing exact rational affine-octant construction. The shared strip profiles
are derived from that same support. Slanted corners consequently receive the mesh guide,
regular chart, and complete-row spline solve already used for ordinary boxes.

Two further fixes are required for that extension:

- Mesh guide points are mapped using the complete inverse corner frame. Dividing each dot
  product by an axis length assumes orthogonal axes and is incorrect for skewed frames.
- Profile guide sampling uses the inverse affine circle frame, which also handles elliptical
  conics. Treating these as world-space circles loses BMesh's actual resampled stations for
  segment counts such as 13.

The roundoff contract is retained: orthogonal corners still use the spherical radius
certificate; other corners report affine NURBS construction error without claiming a fixed
world-space sphere radius. No degree-twelve composition from the rejected experiment remains.

### Regression measurements

The retained regression `RoundedSlantedBoxCornersFollowMeshAndKeepSmoothInteriorRows` checks
five shears (0, 0.000001, 0.35, -0.35, 0.8), each at 2, 3, 8, and 13 segments. It asserts
vertex distance to actual BMesh bevel below 0.000001 and internal curve-join angle below
0.002 degrees, and requires accepted shared boundaries and tessellation.

Across these twenty cases the maximum mesh vertex discrepancy was below 0.00000018 model
units. Internal curve-join measurements were below 0.00082 degrees (finite difference
tangents, so these values include measurement discretization).

For shear 0.35, 8 segments, maximum mesh vertex distance improved from 0.0262597 to
0.000000147. At 13 segments it improved from 0.0196639 to 0.000000160. The large internal
kinks in the original screenshots were reproduced before the change and removed in the
matching after view.

The bevel regression run passed 22 tests, including the exhaustive skewed and tapered box
edge-subset coverage. Three saved-file fixture tests were skipped because their optional
external fixtures were not configured. Separate GUI workflows passed for native and mesh
slanted boxes with 13 segments, including cancel, confirm, undo/redo, mode switching,
continued movement, and duplication. Native and mesh stretched/thin slanted boxes also
passed an 8-segment workflow, cancel, undo and redo.

Artifacts:

- Before: `/tmp/nurbs-move-optimization/affine-before-close/close.png`
- After: `/tmp/nurbs-move-optimization/affine-after-close/close.png`
- Mesh: `/tmp/nurbs-move-optimization/affine-reference-close/close.png`
- Stretched box: `/tmp/nurbs-move-optimization/affine-stretched-native/stretched.png`
- Test results: `/tmp/nurbs-affine-final-tests.log`

### Limits of the result

The repair matches the mapped mesh-corner construction and removes the measured internal
grid distortion. It does not certify G1 continuity across every corner-to-strip seam.
For shear 0.35 a roughly 2.78-degree curve-direction difference remains at some external
corner/strip joins; for shear 0.8 it is about 14.2 degrees. These are distinct from the
now-smooth internal corner grid joins. The implementation continues to report that tangency
is not certified for nonorthogonal blends.

This also does not establish parity with every BMesh corner dispatch: extreme angles that
use BMesh's general subdivision construction, higher-valence fans, and arbitrary curved
input still need separate geometry support and validation. The visual comparison here uses
a generated slanted box and its mesh equivalent, rather than the user's exact saved scene.
