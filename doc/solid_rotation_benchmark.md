# Mesh and analytic solid rotation benchmark — November 4, 2026

The repository had a NURBS rotation profile and a solid extrusion benchmark, but no matched
rotation benchmark for a mesh versus the independent solid kernel. The new
`tests/python/solid_rotation_benchmark.py` fills that gap.

It creates a 2-unit cube, bevels every edge to width 0.2, then applies 40 small Edit Mode
rotations to the source geometry. It drops the first five samples, updates the dependency graph,
and requests the evaluated display mesh after each rotation. Construction and bevel time are
excluded. The mesh bevel uses 32 segments; the solid uses one exact analytic fillet segment.

## Result

Run on November 4, 2026 with the installed Blender 5.2.2 build, factory startup, and process
affinity pinned to one CPU core. The table reports medians across 35 rotation updates.

| Geometry | Display vertices / faces | Update + evaluation | Process CPU per update | CPU used (one core) | Update rate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Beveled mesh | 6,536 / 6,534 | 4.132 ms | 4.138 ms | 100.1% | 242.0 updates/s |
| Analytic solid | 3,080 / 3,270 | 15.905 ms | 15.839 ms | 100.0% | 62.9 updates/s |

The mesh completed these background geometry updates about 3.8 times faster, while producing
about twice as many displayed faces. The solid's CPU use was close to one full core during each
update. Its update rate is not viewport FPS: this benchmark runs in background mode and does not
measure drawing, overlays, GPU presentation, or mouse-event latency. Different bevel segment
counts also mean these two sources do not have identical topology; the mesh uses a sampled
roundover and the solid stores analytic fillet surfaces.

Raw output: `/tmp/solid_rotation_benchmark_pinned.log`. Reproduce with:

```sh
taskset -c 1 /home/molotovgirl/Desktop/build_linux/bin/blender \
  --background --factory-startup --python-exit-code 1 \
  --python tests/python/solid_rotation_benchmark.py
```

## Rotation performance fix

Profiling located two avoidable costs in `editsolid.cc` / `solid_host.cc`: converting already
computed analytic corner normals into legacy fan-space encoding, and rebuilding the entire
edit BMesh after a complete-source transform. `transform_selected` also refitted complete
planes even though their support planes had already been transformed exactly.

The normal writer now follows `nurbs_mesh.cc`: store normalized float corner vectors directly.
Complete transformed faces retain their support identities and ordering. Complete-source
transforms can reuse the existing edit BMesh after checking vertex/edge/face counts and face
connectivity; the current analytic evaluation supplies all positions and normals. A changed
display layout or missing normal layer falls back to the ordinary rebuild. Partial source edits
retain the general reconstruction path. Source validation, display sampling density and
analytic geometry remain active.

The mesh transform implementation (`transform_convert_mesh.cc`) retains its edit connectivity
and chooses normal/tessellation updates according to the transform. The solid change borrows
that reuse principle. The NURBS path additionally reuses boundary/interior evaluations; further
source-sampling reuse is still possible in the solid path.

Final isolated single-core benchmark, same script and source dimensions:

| Geometry | Update + evaluation | CPU time/update | CPU used (one core) | Update rate |
| --- | ---: | ---: | ---: | ---: |
| Mesh | 3.466 ms | 3.472 ms | 100.1% | 288.5 updates/s |
| Solid after fix | 5.148 ms | 5.151 ms | 100.1% | 194.3 updates/s |

The solid was 1.49 times the mesh update cost in this run. The prior recorded run above was
3.85 times the mesh cost. These are separate runs on a shared machine, so compare the broad
improvement rather than treating every millisecond difference as a controlled speedup. The
profiling runs also show the direct effect: the display-build stage dropped from approximately
7–10 ms to 3–5 ms, and the edit-topology stage from approximately 1.5–3 ms to 0.2–0.3 ms.
Other optimized runs measured 5.1–8.5 ms per solid update. Actual viewport FPS is still unmeasured.

All 17 Blender API integration cases passed, including a new check that rotation preserves
connectivity and transforms every display position and analytic corner normal within 2e-6.
The standalone source editing suite passed. Three viewport cases passed: fillet rotation
cancel/confirm/undo/redo, angled whole-body wheel fillet, and source-face selection.

Stage logs are available when Blender is launched with `BLENDER_SOLID_PROFILE=1`. Each display
publication reports display creation, source publication, topology update and edit update time,
plus whether topology was reused. Final timing output is in
`/tmp/solid_rotation_stable_final.log`; pre-fix stage output is in
`/tmp/solid_rotation_stages.log`. This logging is opt-in.

For topology counts, launch Blender with `BLENDER_BEVEL_PROFILE=1`. A mesh bevel prints
`MESH_BEVEL_COUNTS` with the BMesh totals before and after the operation. A solid update prints
`SOLID_DISPLAY_COUNTS` with analytic source counts, sampled display counts, final Mesh datablock
counts, and edit BMesh counts. `sample_boundary_e` is only the analytic edge sample list; it is
not the complete display edge count because face tessellation also creates internal edges. This
distinguishes the solid's mathematical faces from the polygons Blender actually draws.

## Repeated optimization loop — retained changes

A preserved executable, `../build_linux/bin/blender-solid-rotation-baseline`, supplies the
baseline for this loop. Each case creates the filleted solid cube and applies the same
rotation/evaluation workload. Four executables were run three times each, alternating order,
with affinity fixed to CPU 1 and no concurrent build or verification jobs. Each run drops
five warmups and measures 35 rotation updates. The following values are medians of those
three run medians, rather than a comparison of isolated fastest results.

| Trial | Solid update time | Decision |
| --- | ---: | --- |
| Preserved baseline | 5.556 ms | Reference |
| 1: retain double-precision display samples with immutable source snapshots; transform samples on complete rigid source moves | 4.318 ms | Keep |
| 2: transfer those samples directly to the matching edit BMesh, avoiding temporary Mesh construction | 2.709 ms | Keep |
| 3: skip retriangulation when edit topology is reused | 2.690 ms | Reject: under 1% difference, insufficient to distinguish from run noise |

The retained path reduces median update time by 51.2%, approximately 2.05 times the baseline
throughput. CPU time per update fell from 5.562 ms to 2.713 ms. CPU utilization during the
short workload remains approximately one full core; the operation finishes sooner. These
are background geometry update measurements, not viewport FPS.

Caches contain derived double-precision samples and belong to their immutable source model.
They are copied on transformation and never overwritten through a shared snapshot. Selection
changes retain samples; new geometry invalidates them. Caches are runtime-only and are not
serialized. Complete rigid moves preserve source validation and transform source geometry as
usual. Non-rigid transforms and partial edits retain fresh source sampling. Direct BMesh
updates first check connectivity and coordinate range before source publication. The final
build continues recalculating display triangles and normals.

All 17 API cases passed in the retained final build. Rotation coverage now checks repeated
rotations and compares positions/normals against a fresh source rebuild produced by duplicating
the geometry. Three viewport cases passed: rotation cancel/confirm/undo/redo, whole-body fillet
wheel updates and source-face selection.

Raw per-run values and decisions: `/tmp/solid-opt-iterations/summary.json`; individual logs:
`/tmp/solid-opt-iterations/{baseline,iteration1,iteration2,iteration3}-round{0,1,2}.log`.
Final stage logging: `/tmp/solid-opt-iterations/stages-final.log`.

## Live scene correction: modal rotation, 2026-11-04

The previous 2.71 ms result does **not** describe the user's slow scene. Captured the
running scene without changing its original file path (`save_as_mainfile(copy=True)`):
`/tmp/solid-live-captured.blend`. It contains a 20-subdivision solid fillet with
1,848 source vertices, 5,292 source edges, and 3,446 source faces. Its display had
145,472 vertices and 155,046 faces, far larger than the earlier benchmark cube.

Reproduced the user's stall with real modal rotation and simulated mouse motion.
Measured successive POST_PIXEL draw callback intervals, discarding initial three
frames. The timer's event-send-to-draw interval is insufficient: queued redraws can
occur before the newly sent event is processed. Only full successive draw intervals
are used below.

| Same captured scene | Median interval | Observed draw rate |
| --- | ---: | ---: |
| Before these fixes | 453.79 ms | 2.20 FPS |
| After these fixes | 98.26 ms | 10.18 FPS |

This is a single before/after pair on the running desktop, not a universal FPS
claim. Background confirmed-operator timings and synchronous redraw timings are
separate workloads and should not be substituted for these modal results.

Retained fixes:

- Scale spherical patch interior sampling with its angular size, retaining shared
  edge samples and the analytic source. Previously each tiny subdivided patch
  received eight interior rings. The captured scene now displays 11,072 vertices
  and 20,646 faces; source topology remains unchanged.
- Keep baseline display samples in a modal Session when the file was loaded without
  a runtime cache. Previously every mouse movement tessellated the entire source;
  repeated EXEC rotations masked this because each subsequent operator could read
  the cache published by the previous one. Modal display-stage time fell from
  roughly 250 ms per event in the controlled live case to roughly 1 ms.
- Replace repeated linear selection membership searches with hash sets, and replace
  scanning all edges for every source curve with one edge pass. Complete source
  transforms avoid reclassifying every individual boundary.

Residual: about 98 ms per displayed update is still visibly slow. Source transformation,
validation, publication, and viewport work remain. Do not present this build as fully
smooth or use the small-cube result to dismiss the live-scene measurements.

Validation: all 18 Python API tests pass, including a 20-subdivision display density
regression and exact source/fresh sample normal comparisons. Viewport cancel/undo,
angled wheel fillet and source-face click selection checks also pass.

Artifacts: `/tmp/solid-live-info.json`, `/tmp/solid-live-captured.blend`,
`/tmp/solid-live-modal-bench.py`, `/tmp/solid-live-modal-before2.log`,
`/tmp/solid-live-modal-after2.log`, `/tmp/solid-live-api-final.log`,
`/tmp/solid-live-ui-checks.log`. The earlier modal-before/after files used the
incorrect event-send clock and their FPS numbers must not be used.

## Matrix updates during whole-solid drags

Whole-source rigid transforms now update the existing visible BMesh directly from
its starting coordinates and custom normals. Each mouse event applies a matrix;
source model copying, selection normalization, construction and validation happen
once when the user confirms. Confirmation publishes the transformed analytic model
without replacing or repositioning the displayed BMesh. Cancel restores the exact
starting coordinates and corner normal floats. Partial edits, scale and transforms
that are not rigid in object space retain the existing analytic edit path.

The source remains authoritative: display coordinates never become analytic input.
A source transform is computed from the original analytic model and final matrix.

Three alternating comparisons on the same 20-subdivision captured solid, with its
previous excessive sampling already corrected, produced these median draw intervals:

| Trial | Previous implementation | Matrix drag |
| --- | ---: | ---: |
| 1 | 117.53 ms | 24.44 ms |
| 2 (order reversed) | 84.29 ms | 23.42 ms |
| 3 | 86.95 ms | 24.84 ms |
| Median of the three trials | **86.95 ms / 11.50 FPS** | **24.44 ms / 40.92 FPS** |

Retained the matrix path: about 3.56 times the update rate, a 71.9% reduction in
frame interval. Measurements use successive viewport POST_PIXEL draws during mouse
rotation, excluding the initial three frames, with confirmation after 14 events.
Every run also asserts that all 11,072 visible vertex coordinates are bit-for-bit
identical before and after confirmation. These desktop results vary with load.
There is still a source commit/validation cost when confirming, but that operation
leaves the displayed coordinates untouched.

Validation: all 18 API tests pass, including comparing rotated display geometry and
normals against a fresh analytic rebuild. Three viewport checks pass; the rotation
check now verifies that geometry visibly changes *during* rotation, stays identical
on confirmation, and supports cancel, undo and redo. An additional check verifies
exact corner normal restoration on cancel and preservation on confirmation.

Artifacts: `/tmp/solid-matrix-input.blend` (same analytic shape as the live capture,
normalized once by the previous implementation using a zero-angle rotation),
`/tmp/solid-matrix-modal-bench.py`, `/tmp/solid-matrix-compare/results.json`,
`/tmp/solid-matrix-compare/{before,matrix}-{0,1,2}.log`,
`/tmp/solid-matrix-api.log`, `/tmp/solid-matrix-ui.log`, and
`/tmp/solid-matrix-ui-normals.log`. The previous executable is preserved as
`build_linux/bin/blender-before-matrix-drag`.

## Five additional optimization trials — all discarded

Tested all five proposals against the retained matrix-drag build. Each performance
trial used three alternating before/after pairs, 32 mouse events on the same
20-subdivision scene, and discarded the first five frame intervals. The following
are medians of the three per-run medians; each row has its own contemporary baseline.
Desktop load varied, so compare within each row rather than between rows.

| Trial | Baseline frame ms | Candidate frame ms | Decision |
| --- | ---: | ---: | --- |
| 1. Reuse face triangulation | 21.68 | 22.94 | Discard: no repeatable gain |
| 2. Transform geometric normals instead of recomputing them | 21.95 | 21.59 | Discard: small difference within run variation |
| 3. Remove duplicate geometry tags and notifiers | 22.69 | 22.17 | Discard: no reliable update-stage or frame improvement |
| 4. Replace per-vertex overflow preflight with eight bounding-box corners | 24.95 | 26.62 | Discard: no repeatable gain |
| 5. Preserve graphics index/selection buffers during rigid motion | 19.74 | 17.23 | Discard: visual correctness failure |

Trial 5 initially looked promising: about 50.7 to 58.0 FPS, with improvements in all
three pairs. Numeric geometry, bounds, normals, cancel, confirmation and dependency
checks passed. The final screenshot comparison nevertheless showed that the shaded
surface was not synchronized with the selected edges while dragging; a full update
on confirmation corrected its appearance. This violates the requirement that the
solid look correct throughout rotation and not change appearance on confirmation.

Confirmed the visual failure against the baseline with identical inputs and a
0.6-second pause before taking the image, so ongoing motion and image accumulation
would not explain the difference. In the object crop (x=1010..1260, y=395..610), the
baseline had zero gray-to-gray pixels changing by more than 10 RGB levels at
confirmation; the graphics prototype had 1,032. Screenshots also showed visibly
incorrect corner shading during the prototype's drag. The missing drawing-cache
refresh was not isolated, and none of that prototype was retained or installed.

Restored the original matrix-drag implementation, including its normal geometry
updates. Do not treat the prototype's FPS as a shipped improvement. These trials
produced no additional retained performance change.

Additional regression checks cover derived geometric normals (1e-4 tolerance for
roundoff from tiny float display triangles; existing analytic corner-normal checks
remain at 2e-6), evaluated vertex positions and bounds during a drag, and another
mesh using Shrinkwrap to follow the solid during rotation and return on cancel.

Artifacts: `/tmp/solid-five-trials/trial{1,2,3,4,5}.json`, corresponding
`trialN-{before,after}-{0,1,2}.log` files, `compare.py`, `bench.py`, source snapshots
for rejected trials, and `gpu-trial.patch`. The JSON field `cpu_stage_ms` in trials
1–4 is a legacy label for wall-clock update-stage time, not measured process CPU
time. Trial 5 correctly labels it `update_stage_ms`.

Visual evidence: `/tmp/solid-five-trials/{baseline,gpu}-settled-{during,confirmed}.png`.
Prototype checks: `trial5-api.log`, `trial5-ui.log`, `trial5-dependent.log`.
Restored-build checks: `final-api.log`, `final-ui.log`; build/install logs:
`final-build.log`, `final-install.log`.

## Mesh-inspired follow-up trials — retained partial updates and corrected drawing reuse

Compared Blender's `transform_convert_mesh.cc` partial update path and
`transform_convert_object.cc` object matrix path against solid editing. Each timing
comparison below used three alternating before/after pairs, 32 modal mouse events,
and excluded the first five viewport draw intervals. Numbers are medians of per-run
medians. They measure displayed update intervals, not background operator throughput.
Each row has its own contemporary baseline; desktop load varies.

| Experiment | Baseline frame ms | Candidate frame ms | Result |
| --- | ---: | ---: | --- |
| Whole-solid drag using temporary object transform channels | 18.72 | 16.80 | Discarded: origin and transform fields change during Edit Mode, then revert on confirmation |
| Partial edit reuses checked connectivity and affected normal/triangle regions | 388.98 | 255.29 | Retained; improved all three pairs, but analytic construction remained expensive |
| Extend matrix drag to an isolated selected component | 392.84 | 28.40 | Retained; about 2.55 to 35.21 FPS before drawing reuse |
| Corrected drawing-cache reuse on whole-solid rotation | 19.04 | 17.30 | Retained; about 52.53 to 57.81 FPS, improved all three pairs |
| Add corrected drawing reuse to isolated-component matrix drag | 27.42 | 22.06 | Retained together; about 36.47 to 45.32 FPS, improved all three pairs |

The partial scene is one analytic object containing two copies of the captured
20-subdivision filleted solid, with only the second component selected. It has
22,144 display vertices. These results do not describe moving an arbitrary face
of a connected curved solid. Connected edits retain their analytic construction
rules; compatible edits reuse verified display connectivity and cache the affected
BMesh region rather than replacing the entire edit mesh.

An isolated-component matrix drag is allowed only when no source edge, face
(including holes), curve support or surface support spans moving and stationary
parts. Display movement is selected by source IDs, including sampled vertices and
face-corner normals. Unselected display coordinates remain unchanged. Confirmation
transforms and validates the independent analytic source once and leaves the live
BMesh coordinates and normals untouched. The object matrix and origin stay fixed.

### Drawing-cache failure isolated and corrected

Preserving face triangulation alone did not fix the previous visual failure.
Discarding all graphics buffers while bypassing geometry evaluation also failed.
The missing signal was to Workbench: its completed shaded image can be reused until
`DEG_get_update_count` changes. Edit overlays can update independently. The cache
prototype therefore moved edit edges while leaving a previously rendered surface
on screen, even after a stationary pause.

The retained path dirties position/normal-dependent graphics buffers while keeping
connectivity and selection buffers. It requests an object transform evaluation,
without changing transform channels, to refresh Workbench's accumulated image.
It also invalidates mesh position/normal/bounds caches and refreshes evaluated
bounds. Face triangulation stays consistent with retained graphics indices during
rigid movement. The regular geometry evaluation path remains active for modifiers,
dependent geometry, rendered/material views, or evaluated data that does not
share the unchanged edit BMesh directly.

With the same 0.6-second stationary screenshot comparison used earlier, the fixed
whole-solid trial had zero gray surface pixels changing by more than 10 RGB levels
at confirmation (the broken trial had 3,158 in the contemporary run). The combined
partial-component build likewise had zero changed gray pixels in the selected
component crop `(1142,515)..(1421,773)`, with mean gray difference zero. Numeric
coordinates also remain exactly unchanged on confirmation in the retained path.

Validation: **19 API tests and 9 viewport tests passed**. Checks include fresh
analytic source sampling after component rotation and connected planar deformation,
unchanged stationary geometry, original object matrix, live movement, exact corner
normal/coordinate preservation on cancel and confirmation, undo/redo, Shrinkwrap
dependency updates, source-face selection, angled filleting, and vertex/edge/face
duplication. Built and installed to `build_linux/bin`.

Artifacts in `/tmp/solid-mesh-trials/`: `object.json`, `partial.json`, `component.json`,
`cache3.json`, `integrated.json`, corresponding paired logs, scripts and discarded
source snapshots; `api-final.log`, `ui-final.log`, `build-final.log`, `install-final.log`;
`{base,object,cache,cache2,cache3,component-final}-{during,confirmed}.png`,
`component-visual.json`. `update_stage_ms` is wall-clock editor-stage duration,
not process CPU usage. Performance comparisons ran without concurrent builds or
other test runs. No temporary object-channel transform code was retained.


## Matched mesh bevel versus analytic fillet viewport rotation

The first viewport comparison above used 32 mesh bevel segments against one solid
fillet segment. That was a mismatch. I reran the tests with the same segment count
on both tools: once at one segment, and again at 32. Each comparison has three
alternating mesh/solid pairs. Every run uses a 2-unit cube, bevel width 0.2, the
same simulated rotation path, 32 displayed updates, and excludes the first five.
Both runs use the same visible viewport and confirm without moving the geometry.

| Matching segment count | Mesh display vertices / faces | Mesh FPS | Solid display vertices / faces | Solid FPS |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 24 / 26 | 59.80 | 3,080 / 3,270 | 59.31 |
| 32 | 6,536 / 6,534 | 59.92 | 25,352 / 49,926 | 33.64 |

The one-segment row does **not** compare the same shape: one mesh bevel segment is
a flat chamfer, while one solid segment is still a curved fillet. At 32 segments,
both produce rounded profiles. In that matched case, the mesh reached about 60 FPS
and the solid about 34 FPS. The solid display had about 3.9 times as many vertices
and 7.6 times as many faces. The viewport cap limited the mesh result, so this test
cannot say how much faster it would run above 60 FPS. It does show that the current
32-segment solid display is substantially heavier than the 32-segment mesh bevel.

At the visually comparable settings used in the earlier trial (32 mesh segments,
one exact solid segment), both reached roughly 60 FPS, but those subdivision counts
were not equal. The test cannot be used to claim the solid is faster. The equal-32
comparison instead identifies display tessellation density as a likely contributor
to the solid's slower rotation; confirming the share of CPU time would need a separate
uncapped or per-stage timing measurement.

Reproduction helper: `tests/python/solid_viewport_rotation_benchmark.py`. It replaces
the factory startup scene and exits Blender when finished. Run each segment count for
both `SOLID_BENCH_KIND=mesh` and `SOLID_BENCH_KIND=solid`, setting
`SOLID_BENCH_SEGMENTS=1` or `32`. Three paired run logs and summaries are in
`/tmp/solid-mesh-viewport-matched/`.

## Display tessellation fix

The 32-segment result above exposed redundant display geometry. Each small analytic
sphere patch at a rounded corner already has a triangular boundary. The tessellator
was splitting each of its three short curved edges into at least two display edges,
then fanning that six-point boundary into more triangles. It also resampled each
short rational-curve segment even though the source fillet had already divided that
curve into 32 pieces.

The tessellator now keeps a small triangular sphere patch as one display triangle
with the exact analytic normal at each corner. It gives a small rational-curve piece
one display span, while still sampling a larger unsplit curve according to viewport
resolution. This changes only the display mesh; the analytic fillet construction,
source faces, and requested segment count are unchanged.

Three alternating viewport runs per tool, with both tools set to 32 segments:

| Geometry | Display vertices / faces | Median rotation rate |
| --- | ---: | ---: |
| Blender mesh bevel | 6,536 / 6,534 | 59.85 FPS |
| Analytic solid fillet, before fix | 25,352 / 49,926 | 33.64 FPS |
| Analytic solid fillet, after fix | 12,872 / 9,734 | 60.62 FPS |

The solid's displayed polygons fell by about 81% from the original result, and its
rotation reached the same approximately 60 FPS viewport cap as the mesh in this test.
It still draws more vertices and faces than the mesh, so matching FPS here does not
prove they will perform equally above the cap or on a heavier scene. The solid's
32-segment exact corner representation also has more source patches than the mesh's
bevel strips; the fix removes redundant *display* samples rather than merging those
analytic patches.

The 32-segment Python regression now checks both source and evaluated display counts.
All 19 solid API tests passed after the final tessellation change. Six viewport
integration tests also passed, covering fillet rotation/undo, angled whole-body
filleting, source-face picking, dependent display updates, isolated component
rotation, and connected planar face movement. Benchmark details and raw paired runs
are in `/tmp/solid-mesh-viewport-after2-run.log` and
`/tmp/solid-mesh-viewport-after/`.

## Simple cube face-count diagnosis — November 4, 2026

To isolate the count mismatch, created the same 2-unit cube twice, beveled every
edge to width 0.2, and compared mesh bevel and solid fillet at 20 and 32 segments.
The mesh bevel uses profile 0.5. Counts below distinguish the solid's analytic
source faces from the polygons that make up its viewport display.

| Segments | Mesh bevel faces | Solid source faces | Solid display faces |
| ---: | ---: | ---: | ---: |
| 20 | 2,646 | 3,446 | 3,446 |
| 32 | 6,534 | 8,582 | 8,582 |

Before the display change, 816 sphere patches at 20 segments and 384 patches at 32
segments had one extra sampled boundary point. The tessellator fanned each patch into
four polygons. It now draws each small patch as one face and keeps the same boundary
points and per-corner normals. That removed 2,448 display faces at 20 segments and
1,152 at 32. The source solid is unchanged. The viewport already had smooth shading
enabled on every face.

| Segments | Mesh vertices / faces | Solid display vertices / faces | Solid source faces |
| ---: | ---: | ---: | ---: |
| 20 | 2,648 / 2,646 | 2,256 / 3,446 | 3,446 |
| 32 | 6,536 / 6,534 | 4,680 / 8,582 | 8,582 |

This also removed unused center vertices from the small sphere patches: 3,200 at
20 segments and 8,192 at 32. A 640×640 Workbench render before and after looked the
same; the image RMSE was 8.65e-5 on a 0–1 scale. This is a visual check from one
view, not a guarantee for every angle or lighting setup.

The solid source itself still has more corner patches than the mesh bevel. For these
settings the analytic source creates 8 × segments² spherical patches, while the
mesh bevel output contains 8 × 0.75 × segments² corner faces. The two kernels use
different corner layouts, so the solid still displays more faces at equal segment
counts even after removing the extra display fan.

The old saved display caused a separate mismatch. Earlier live-scene logs showed
14,166 display faces; the preserved rotation capture held 155,046. Those are saved
generated Mesh counts, not analytic source counts. Runtime display samples are not
serialized, so the old loader kept that saved Mesh instead of rebuilding it. Mesh
loading now rebuilds the display after the source and datablock links are read. A
regression test clears the saved display, reloads the file, and confirms it rebuilds
3,446 faces from the intact 3,446-face source. I also opened the captured live `.blend`
with the rebuilt binary: its 155,046-face display rebuilt to 3,926 faces; the source
still has 3,446 faces. That edited solid has three displayed faces per ruled source
patch (720 total); the clean cube has one per patch (240 total).

Reproduction: set `BLENDER_BEVEL_PROFILE=1` and run
`/tmp/solid_vs_mesh_cube_counts.py` with the built Blender binary. The log prints
mesh totals, analytic source/display totals, surface-kind breakdowns, and sphere
patch boundary histograms. The solid API suite checks both segment counts, smooth
shading, and rebuilding the saved display on load.
