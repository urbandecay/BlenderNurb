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
