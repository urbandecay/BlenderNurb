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
