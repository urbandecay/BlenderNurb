# Native NURBS translation preview

This experiment applies to the app's custom native NURBS editing system. A modal
translation of a fully selected model or an independently duplicated component can
transport its original display instead of sampling its NURBS boundaries and surfaces
after every mouse movement. A duplicated corner can move while its original surface
stays in place, even though both remain in the same native NURBS edit object.

The exact source still goes through the existing geometry transform, precision
checks, edit commit and dependency-graph notifications on every update. This keeps
selection overlays and source-based interaction current. The optimized part is the
derived display: copy its shared mesh data, update positions, preserve triangle
connectivity, source normals, materials and visible wires, and hand the completed
preview to the existing evaluated-candidate pipeline.

Each preview starts from the original immutable display and the total displacement
from the start of the drag. It never translates the preceding preview, avoiding
accumulated display roundoff. The preview is explicitly marked as temporary because
transporting previously rounded samples is not a fresh evaluation of the transformed
source. Confirmation clears the prepared display and evaluates the exact current
source normally. Cancellation restores and evaluates the original source normally.
The temporary cache is not serialized into the model.

The shortcut requires matching preview settings and unchanged source identities,
topology, trims, weights, knots, orientations and material assignments. Each active
surface and 3D edge curve must be either unchanged or translated by the requested
amount. Shared display samples must agree about whether they move. This lets duplicated
surfaces move while the original remains fixed. Unused parent supports retained by
refinement need not move. Changed float triangle visibility, overflowing samples,
undefined normal frames, partial surface deformation or an incompatible cache use
ordinary evaluation. A temporary preview cannot become the base of a later source edit
or another translated preview.

## Measurements

Two isolated viewport runs per executable, ordered before/after/after/before, built
the same 13-segment beveled cube and moved through the same twelve nonzero cursor
positions. Both retained 9,312 display vertices and 18,620 triangles. The timers
measure each modal handler plus its following NURBS object evaluation, paired per
update. Drawing and event-harness waits are excluded; these are not FPS numbers.

| Median elapsed time | Pushed baseline | Translation preview |
| --- | ---: | ---: |
| First comparison | 25.04 ms | 5.48 ms |
| Reverse-order comparison | 19.75 ms | 5.21 ms |
| All 24 updates per executable | 20.13 ms | 5.32 ms |

The pooled result uses about 74% less elapsed time, or roughly 3.8 times faster.
The source transform remains a cost. This experiment does not speed up one-shot
scripted transforms, rotations, partial shape edits or bevel-width construction.

## Duplicated component drag

A second viewport comparison used the same 13-segment bevel, selected one face,
duplicated it, then moved the copy through six repeatable cursor positions. The
display contained 9,376 points and 18,718 triangles during the drag in both builds.
The paired timer includes the transform handler and following object evaluation.

| Median elapsed time | Pushed build | Component preview |
| --- | ---: | ---: |
| First comparison | 25.64 ms | 7.33 ms |
| Reverse-order comparison | 23.46 ms | 7.42 ms |
| All 12 updates per build | 24.08 ms | 7.34 ms |

The copied face moved while all 9,312 points from the original display remained in
place; the duplicate added 64 independently translated display points. The pooled
updates took about 70% less time, or 3.3 times faster. The old path rebuilt the full
display and recalculated normals for roughly 18,700 triangles on each move. The new
path copies the existing display and transports the changed component points, with
no surface resampling; the prepared display handoff took about 1 ms. Confirmation
still performs one ordinary evaluation from the exact NURBS source.

The retained baseline is `build_linux/bin/blender-translation-baseline` beside the
main build. Exact per-update data and comparison results are in
`/tmp/nurbs-translation-summary.json`. Traces and viewport logs are
`/tmp/nurbs-translation-{before,after}[-repeat]-{debug,ui}.log`.

## Validation

All 29 mesh evaluation tests and six dependency-graph/object evaluation tests passed.
New tests exercise a duplicated face moving independently of its original, planar and
rational surfaces, independent renderer ownership, anchoring multiple translations to
the original display, final fresh evaluation, partial edits, changed
material/orientation/trims/support coefficients, undefined normals and float triangle
collapse. The object test exercises prepared preview handoff through the actual
dependency graph and discarding it for final evaluation.

The viewport workflows `test_nurbs_native.bevel_whole_model_translation_workflow` and
`test_nurbs_native.bevel_duplicated_component_translation_workflow` create the dense
bevel and cover repeated mouse positions, cancellation, numeric confirmation, undo,
redo, mode re-entry and moving a duplicated face while its original stays fixed. Both
passed in the final build; the before/after component comparison passed in both orders.
Recomputed
float normals are compared within 2e-6 inside the workflow because even the baseline
can recompute slightly different float normals during a rigid move. Exported final
positions, normals and polygon indices matched exactly between builds in both
comparisons. The final mesh remains derived display data; the editable model remains
NURBS.
