# Native bevel work reuse

The mesh comparison exposed redundant work in the Blender-side NURBS implementation.
The native tool builds rational surfaces and shared trim topology; it does not execute
the SMLib fillet engine. Blender's BMesh bevel provides the interaction and performance
reference, while the native result remains editable NURBS.

## Changes

- Plan the rounded strip partitions against one source and apply them together.
  Previously each strip copied and validated the growing body separately.
- Preserve the complete preflighted candidate through commit and dependency-graph
  evaluation. Immutable geometry is shared with an independently advancing edit
  revision; the renderer gets an independent derived Mesh copy. Full reuse requires
  identical geometry ownership and matching preview settings. A subsequent edit
  clears the prepared evaluation. Cancellation and undo retain source ownership rules.
- Confirm the accepted modal candidate instead of calculating the bevel again.
- Rebuild only faces whose sampled UV trims changed. In the rounded 13-segment box,
  six original faces change while 164 faces retain their triangulation. Positions and
  collapsed pole joins are evaluated for the current source. Sample identities,
  support knots, orientation and settings remain guarded; incompatible layouts rebuild.
- Avoid repeating identical interval endpoints and neutral arithmetic. Equal rational
  weights use the same Euclidean interval result directly instead of constructing and
  discarding a general homogeneous result. Canonical trim coordinates evaluate their
  exact parameter/constant identities without interval reconstruction.
- Group derivative samples by support and cache U/V basis and derivative weights
  within each group. Scalar arithmetic and one-sided knot limits are preserved.
- Use Blender's floating-point `custom_normal` corner attribute for regular source
  frames, as supported by its Free Mesh Normal mode. This avoids calculating normal
  fan spaces, encoding source normals into short pairs, and decoding them again.
  Undefined source frames retain the existing encoded automatic-normal fallback.
- Test translation invariance only on supports referenced by faces. Retained unused
  parent surfaces do not affect shading and must not reject an otherwise valid normal
  cache hit when only the active topology moves. Compare exact displacements, including
  error-free subtraction residuals; the previous roundoff allowance could accept a
  coefficient change as a translation. Small genuine shape changes recalculate normals.

There is no added threading and no reduction in preview resolution. Regular display
normals retain the source float vectors directly, improving on their previous encoding
quantization. This is a runtime optimization; derived caches are not serialized.

## Validation

The kernel checks cover immutable candidate adoption, stale-state rejection, revision
advance, mesh copy independence, dependency-graph handoff, option invalidation and
subsequent edits. A width-change test compares partial and full rebuilds exactly in
positions, triangle indices, face/UV provenance, display wires and normals. It verifies
164 reused faces out of 170, including repeated width changes and skip counters.

Grouped derivatives match scalar point, tangent and normal results exactly, including
opposite internal-knot limits. Normal storage tests verify the float attribute survives
mesh copies and that a stationary interior derivative still receives automatic fallback.
The topology suite also covers all 4,095 edge selections on tapered and skewed boxes.

Kernel test logs are under `/tmp/nurbs-bevel-final-*-tests.log`, with the latest display
checks in `/tmp/nurbs-bevel-translation-final-{mesh,object}-tests.log` and
`/tmp/nurbs-bevel-partial-tessellate-tests.log`. Fixture-gated saved-scene cases are
reported as skipped when their export gates are absent.

The viewport regressions exercise ordinary rounded boxes, tapered mouse bevels,
split-corner deformation and curved supports. The dense mouse regression exercises
13 width positions at 13 segments, cancel, confirm, undo/redo and mode re-entry.
Results and per-process traces are under `/tmp/nurbs-bevel-efficient-ui-*` and
`/tmp/nurbs-bevel-dense-ui-*`.

## Reproducing the comparison

`tests/python/nurbs_bevel_benchmark.py` accepts an active axis aligned native box
fixture through `BLENDER_NURBS_BEVEL_SCENE`. It reloads the fixture between operations,
never saves it, and compares native bevel against a matching eight-vertex mesh box at
1, 6 and 13 segments. It reports operation/evaluation time, not viewport FPS.

The matched viewport run uses the same scripted mouse positions with the previous and
updated executables. Its log timers cover the modal bevel handler and following native
object evaluation; drawing and event-harness waits are excluded. The first width builds
a new preview and the final numeric bevel changes segment counts, so the steady-width
comparison uses the twelve intervening mouse updates. The rendered native preview
retains 9,312 vertices and 18,620 triangles at 13 segments.

The retained user comparison is `/tmp/nurbs-bevel-mesh-user-comparison.log`. The old
executable is retained separately at `/tmp/blender-before-bevel-performance` for review.

## Matched mouse measurements

Twelve steady width updates at 13 segments, using the same box and mouse sequence:

| Instrumented elapsed time, median | Before | After |
| --- | ---: | ---: |
| Bevel geometry construction | 66.66 ms | 35.39 ms |
| Candidate display preview | 55.39 ms | 21.75 ms |
| Complete modal handler | 126.18 ms | 58.61 ms |
| Following NURBS object evaluation | 63.93 ms | 0.85 ms |
| Handler plus following evaluation | 188.41 ms | 59.80 ms |

The combined median is calculated from paired updates, rather than adding separate
medians. This is approximately 3.15 times faster, or 68% less elapsed time in these
stages. It is not a viewport FPS measurement. Exact summaries are retained in
`/tmp/nurbs-bevel-dense-ui-summary.json`; source logs are
`/tmp/nurbs-bevel-dense-ui-{before,after}-debug.log`.

Mesh parity is not established. The native tool still constructs and checks rational
subpatches and samples their display after a width change. The fresh-operation
benchmark and actual viewport trace should remain separate: scripted operators also
process Blender updates, while the viewport figures above explicitly time the handler
and native object evaluation. Further speed work should target reusable construction
plans and the remaining source evaluation, with matched BMesh measurements throughout.

## Movement after bevel

Both executables loaded the same saved 13-segment native result. The existing drag
benchmark selects the entire model and reports medians over ten translations after
two warmups, including obtaining evaluated mesh data. The file is never saved.

| Preview setting | Display triangles | Before | After |
| --- | ---: | ---: | ---: |
| Default U/V 4 | 18,620 | 33.06 ms | 21.01 ms |
| U/V 6 | 44,220 | 65.36 ms | 31.66 ms |
| U/V 3 | 9,900 | 22.68 ms | 14.38 ms |

These compare equal settings between builds; the application defaults were not changed.
The matching display mesh at the default setting measured 4.43 ms before and 4.72 ms
after, so a substantial native-versus-mesh movement gap remains. Partial selections
that deform supports still require fresh source derivatives.

Exact outputs are `/tmp/nurbs-bevel-final-drag-{before,after}-benchmark.log`.
The fresh bevel comparison is `/tmp/nurbs-bevel-final-bevel-benchmark.log`.

## Refinement follow-up after 9b6e6af58b0

The first efficiency version was committed and pushed as `9b6e6af58b0` on
`nurbs/shared-topology-foundation`. The next local build reduces repeated work inside
rounded strip partitioning:

- Build one knot insertion plan per tensor axis and apply it to every coefficient
  row or column. Knot searches, multiplicities and interval blend weights depend on
  the parameter domain, independently of the row positions.
- Reuse scratch buffers and store the restricted columns directly. Avoid allocating
  a coefficient vector for each row and avoid transposing the whole interval grid
  before checking the maximum coefficient error.
- Evaluate only extremal interval multiplication/division corners, using their sign
  and monotonicity. Intervals crossing zero retain the necessary extra corners.
  Point interval sums share their exact residual calculation. Outward rounding and
  explicit error budget checks remain required.
- Preserve copied constant coefficients directly in equal-weight blends.

An independent long-double tensor basis check exposed an unclamped upper-domain
endpoint issue: knot insertion must follow the complete knot run, whereas point
sampling uses the left evaluation span there. The refinement plan now handles that
endpoint correctly. Curve and tensor regressions cover it, alongside repeated interior
knots, signed coordinates, unequal positive weights, invalid intervals and unchanged
source records.

### Matched viewport comparison

Both builds ran the same dense viewport regression, with identical twelve steady
mouse positions at 13 segments. The following native object evaluation is paired with
each modal handler. The first new preview and final numeric operation are excluded.

| Instrumented elapsed time, median | Pushed build | Follow-up build |
| --- | ---: | ---: |
| Rounded strip partitioning | 26.98 ms | 19.84 ms |
| Bevel geometry construction | 34.11 ms | 27.28 ms |
| Candidate display preview | 21.48 ms | 21.92 ms |
| Handler plus following evaluation | 58.52 ms | 51.95 ms |

The complete measured update takes about 11% less time; strip partitioning takes
about 26% less time. The preview stage is still a substantial remaining cost.
These timings exclude drawing and event harness waits and are not viewport FPS.
Exact summaries and logs are `/tmp/nurbs-bevel-v2-ui-summary.json` and
`/tmp/nurbs-bevel-v2-ui-{before,after}-debug.log`.

The fresh-operation benchmark separately measured native totals of 82.20 to 65.92 ms
at 13 segments (about 20% less time), 39.72 to 32.35 ms at 6, and 10.00 to 8.97 ms
at 1. These include operator and evaluated-mesh access. The logs are
`/tmp/nurbs-bevel-v2-{before,final}-benchmark.log`.

### Follow-up validation

All 55 topology, 26 display mesh and 12 tessellation checks passed. Ten saved-scene
checks remain fixture-gated and were skipped. The topology run includes the exhaustive
box edge subsets. The viewport regression passed in both builds, exercising repeated
widths, cancel, confirm, undo, redo and mode re-entry. Test logs are
`/tmp/nurbs-bevel-v2-{topology,mesh,tessellate}-tests.log`.

At 1, 6 and 13 segments, the displayed positions, polygon vertex indices and corner
normal arrays matched the pushed build exactly. The 13-segment result retains 9,312
vertices and 18,620 triangles. The comparison summary is
`/tmp/nurbs-bevel-v2-geometry-summary.json`; no preview settings were reduced.

## Preview follow-up after 12393f6af50

The refinement checkpoint was pushed as `12393f6af50`, with a full commit description.
The next local build reduces preview work while retaining the same sampling settings:

- For a natural clamped boundary whose varying basis, active coefficients and normalized
  weights match the shared edge curve, reuse its canonical point evaluation. The first
  support sample still checks fixed-axis arithmetic and exact numerical agreement.
  Other representations evaluate their supports normally. Per-sample conditioning is
  retained, and measured deviation/conditioning records match independent scalar checks.
- Evaluate canonical constant/parameter p-curve coordinates directly after validating
  their storage, parameter domain and basis arithmetic. Common nonzero weights give
  condition one. Other coordinates retain the ordinary rational evaluation.
- Construct a complete rectangular preview grid directly. Exact perimeter order, sample
  coordinates and normalized spacing must match the grid. Every cell is an empty
  rectangle, so its diagonal is Delaunay. Holes, extra boundary intervals, close or
  coincident UV samples and incompatible grids use the general triangulator.
  Compatible grids also avoid repeated point-in-polygon tests for their interior samples.
- Allow `BLENDER_NURBS_DEBUG_LOG` to isolate a benchmark's diagnostic file. Normal
  interactive use continues to log automatically through `/tmp/nurbs-debug.log`.

The rectangular path can change triangle diagonals and ordering. Source coefficients,
trims and identities are preserved. Comparisons at 1, 6 and 13 segments retain exactly
matching sets of evaluated vertex positions and source normal vectors at those positions,
with the same vertex/triangle counts. No preview resolution or sampling density is reduced.
The 8-segment bevel uses the grid path for 96 of its 110 faces; it retains 5,952 vertices
and 11,900 triangles.

### Measurements

Fresh operations started from the same saved native box, selected all 12 edges and used
width 0.25. Runs alternated the pushed and local executables in before/after/after/before
order. Each process discarded its warmup, leaving twelve timed samples per build.
Medians come from individual operation-plus-evaluation samples.

| Instrumented elapsed time, median | Pushed build | Local preview build |
| --- | ---: | ---: |
| Fresh 8-segment bevel operator | 43.95 ms | 31.32 ms |
| Fresh 8-segment operation plus evaluation | 49.22 ms | 36.99 ms |
| Steady 13-segment mouse handler plus evaluation | 53.09 ms | 50.52 ms |

Fresh creation takes about 25% less time. The matched mouse sequence takes about 5%
less time; it already reused most triangulation, so the new rectangular construction
primarily benefits fresh previews. These are operation timings, excluding drawing and
event harness waits, rather than measured viewport FPS.

The mesh control used the same box and width with 30 segments, yielding 5,768 vertices
and 11,532 triangles. It measured 17.90 ms in the old-build processes and 17.48 ms in
the new-build processes. The native result therefore remains about twice as slow in
this similar-density fresh-operation comparison. Matching triangle count does not
establish identical geometric approximation error.

The whole-model translation benchmark separately measured 17.41 to 16.23 ms at the
default preview setting, with 18,620 triangles. Its matching display-mesh control changed
from 4.67 to 4.28 ms. This single paired movement run is sensitive to timing variation;
the repeated fresh-bevel and matched viewport comparisons above are the primary evidence.

Summaries are `/tmp/nurbs-bevel-v3-abba-summary.json`,
`/tmp/nurbs-bevel-v3-ui-summary.json` and `/tmp/nurbs-bevel-v3-geometry-summary.json`.
Per-process traces use `/tmp/nurbs-bevel-v3-abba-*` and
`/tmp/nurbs-bevel-v3-ui-{before,after}*`. Movement outputs are
`/tmp/nurbs-bevel-v3-drag-{before,after}.log`.

### Validation

All 57 geometry, 55 topology, 13 tessellation and 26 display-mesh checks passed
(151 total). Ten saved-scene topology checks were fixture-gated and skipped.
The geometry suite used the repository's actual older-file assets. New checks verify
scalar boundary metrics on curved and differently parameterized supports, constant
weight scaling, rejection of unsupported fixed-axis arithmetic, and exact retention
of every boundary interval on open and periodic rectangular grids.

The dense viewport regression passed in both builds, including repeated widths,
cancellation, confirmation, undo/redo and mode re-entry. The local rendered result was
also inspected. Kernel logs are `/tmp/nurbs-bevel-v3-*-tests.log`.

## Surface evaluation follow-up

The proposed per-face normal cache was measured and removed. In the 13-segment width
refresh, the 164 retained UV layouts still use changing rounded support coefficients;
the six original planar faces change their sampled trims. No face met both the
unchanged-derivative and matching-triangle requirements (0 of 170 faces). Retaining
triangulation therefore does not establish that its source normals can be reused.

The retained change reduces calculation work within each immutable surface batch:

- Group interior position samples by face in fresh, partial and complete preview refreshes.
  Validate the support once and calculate basis weights once per distinct U/V value.
  Preserve the existing first-use point indexing and scalar summation order.
- Share the weight scale and recentering origin while all nonzero-weight coefficients
  of the same tensor span participate. Endpoints, zero basis products and underflowed
  products retain independently calculated active-coefficient frames. The cache ends
  with the batch and is never reused across changing support geometry.
- Pass grouped basis arrays and weighted point results directly into derivative
  calculation, avoiding copies of the two basis arrays for each normal sample.

### Matched measurements

The previous local preview build and this build ran the same dense mouse workflow in
before/after/after/before order. Each process contributed the same twelve steady width
updates at 13 segments, giving 24 samples per build. The first fresh preview and final
numeric operation are excluded. Combined medians use each paired handler/evaluation.

| Instrumented elapsed time, median | Previous local build | Surface evaluation build |
| --- | ---: | ---: |
| Candidate display preview | 20.557 ms | 19.264 ms |
| Mouse handler plus following evaluation | 48.228 ms | 46.827 ms |

This is about 6% less preview time and 3% less combined update time in this run.
Drawing and event-harness waits are excluded; these are not viewport FPS measurements.
Both builds retain 9,312 display vertices and 18,620 triangles at 13 segments.

Fresh creation did not establish an improvement. A separate alternating comparison
collected 16 operation-plus-evaluation samples per build at 8 segments: native medians
were 42.760 and 43.904 ms. The similar-density 30-segment mesh control measured 19.682
and 20.154 ms. The control also varied, so the fresh-operation result should not be
presented as a speed gain. Native counts remain 5,952 vertices and 11,900 triangles;
the mesh control has 5,768 vertices and 11,532 triangles, without a matched surface
approximation-error claim.

Summaries and individual samples are `/tmp/nurbs-point-frame-summary.json` and
`/tmp/nurbs-point-frame-fresh-summary.json`. Process traces use
`/tmp/nurbs-point-frame-matched-*` and `/tmp/nurbs-point-frame-fresh-*`.

### Validation

All 55 geometry, 26 display-mesh, 13 tessellation and 11 differential checks passed
(105 checks). New comparisons cover repeated knots, span changes, endpoint limits,
zero and signed weights, large translated coordinates, invalid parameters, denominator
zeros and underflowed basis products. Grouped positions, conditioning records,
tangents and normals match scalar evaluation exactly in the covered cases.

The dense mouse workflow passed in all four measured processes, including cancel,
confirm, undo/redo and mode re-entry. Independently exported positions, edge indices,
triangle corner indices and corner normals match the previous build byte for byte
at 1, 8 and 13 segments. The hashes are in
`/tmp/nurbs-point-batch-geometry-before.json` and
`/tmp/nurbs-point-frame-geometry-after.json`. Kernel logs use
`/tmp/nurbs-point-frame-*-tests.log`, with the final geometry check in
`/tmp/nurbs-point-frame-geometry-final-tests.log`.
