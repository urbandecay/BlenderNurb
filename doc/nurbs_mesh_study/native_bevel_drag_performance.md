# NURBS drag performance after bevel

This records the earlier drag-cache change. Later bevel construction, preview reuse,
and floating-point normal storage changes are documented in
[Native bevel work reuse](native_bevel_performance_fix.md). The byte comparisons below
refer to the earlier pair of executables.

Moving the beveled cube in Edit Mode dropped frames. The old path copied and
validated the editable NURBS source and regenerated its display mesh after each
change. Profiling the user's recovered cube at preview U/V 4 measured median
costs of approximately 3.9 ms in surface tessellation, 3.5 ms in source-normal
calculation, and 1.6 ms in the remaining display-mesh construction. These are
instrumented component timings from `/tmp/nurbs-drag-profile.log`, not direct
viewport frame times.

## Change

The evaluated object retains its previous immutable source and UV preview across
the next dependency-graph evaluation. The new preview re-evaluates all boundaries
from the current source. It reuses the old UV triangulation only if the preview
settings, sampled boundary identities/parameters, face orientation and support
knot vectors match. Collapsed pole joins are checked again against the current
support. A mismatch falls back to the full tessellator. Interior positions are
always evaluated from the current NURBS; float-degenerate triangles, mesh edges,
materials, selection overlays and normals are rebuilt for that state. The cache
is per evaluated object and owns its source lease; it is not serialized and does
not turn the display mesh into editable geometry.

Within one display build, repeated triangle corners now share their evaluated
normal when they refer to the same face, UV and meaningful one-sided knot limit.
Different faces and opposite sides of an internal knot remain separate. A new
crease test compares both sides with independently evaluated source derivatives.

The cache does not avoid every cost. Transform commits and boundary evaluation
still process the source, and changed UV topology or display settings require a
full rebuild. This change does not establish mesh-equivalent performance for all
models or frame rates on all hardware.

## SMLib comparison

The bevel's construction approach was informed by SMLib, but this display-cache
implementation is new Blender-side code. SMLib's
[fillet executive](https://github.com/NVIDIA-Omniverse/usd-brep/blob/3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5/source/SMLib/src/SmFilletExecutive.cpp)
uses `SmCurveCache` for curve continuity data and notifies faces after a topology
edit. Its [curve cache](https://github.com/NVIDIA-Omniverse/usd-brep/blob/3fd5b9548d19aa8c12c29bc9637a96d42a06f8a5/source/SMLib/src/SmCurveCache.cpp)
also constructs subdivision data for curve tessellation. That is a related
caching principle; it is not evidence that SMLib uses this Blender UV-triangle
cache. No SMLib implementation was copied.

## Measurements

Both builds ran on the same machine with the same source files and U/V preview
resolution 4. The benchmark reloaded each scene, selected all in Edit Mode and
performed 12 scripted translations; it reports the median of the last 10 calls,
including a view-layer update and obtaining evaluated mesh data. The mesh
comparison is made from the same derived display geometry. This does not directly
measure viewport FPS.

| Fixture | Display triangles | Before | After | Mesh comparison |
| --- | ---: | ---: | ---: | ---: |
| User's recovered beveled cube | 1,764 | 12.53 ms | 5.75 ms | 0.61–0.75 ms |
| Generated fully rounded box | 5,180 | 23.97 ms | 11.52 ms | 1.09–1.13 ms |

At preview U/V 6, the fully rounded box measured 61.17 ms before and 22.67 ms
after. The older executable was copied from the user's still-running prior build;
both runs used factory startup. Exact outputs: `/tmp/nurbs-drag-compare-final.log`.
The reusable benchmark is `tests/python/nurbs_drag_benchmark.py`:

```sh
BLENDER_NURBS_DRAG_SCENE='/path/to/native-nurbs-scene.blend' \
  /home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup \
  --python-exit-code 1 --python tests/python/nurbs_drag_benchmark.py
```

An isolated visible-window test on the same fully rounded box also exercised 20
actual modal mouse movements and canceled the drag. Its median interval between
test event steps fell from 116.7 ms to 99.8 ms. The event harness waits four UI
ticks per step, so those numbers are not per-frame latency or FPS. Results:
`/tmp/nurbs-drag-visible-results.log`.

For both fixtures, all 5,292 and 15,540 displayed corner-normal floats matched
the previous build byte-for-byte, both before and after a nonbinary translation.
Logs: `/tmp/nurbs-drag-{normals,moved-normals}-{before,after}.log`.

## Validation

The new mesh tests compare cached and fresh previews, source positions, face/UV
provenance, visible wires and display normals for whole-surface and single-corner
moves, nonbinary offsets and fully rounded boxes. Changed resolution and face
orientation exercise fallback. A collapsed-pole test checks both reuse and
rejection after an invalid UV edit. The dependency-graph integration test confirms
that a live move retains and uses the prior preview.

The UI regression starts in vertex mode, extrudes a line into a box, bevels,
drags, cancels, moves numerically, undoes and redoes. Four additional isolated
workflows cover a rounded box, a tapered mouse bevel, a split/moved corner and a
curved ruled box. Scripts and logs are under `/tmp/nurbs-drag-*-ui*`.

The final kernel runs passed 140 tests: 21 mesh evaluation, 12 tessellation,
4 object/dependency-graph, 31 transform, 9 differential and 63 topology. The
topology suite ran with the saved-scene fixture gates enabled and had no skips.
Logs: `/tmp/nurbs-drag-mesh-release-2.log`, `/tmp/nurbs-drag-tessellate-tests.log`,
`/tmp/nurbs-drag-object-tests.log`, `/tmp/nurbs-drag-transform-tests.log`,
`/tmp/nurbs-drag-differential-tests.log` and `/tmp/nurbs-drag-topology-tests.log`.

All five existing composition/line/legacy compatibility CTest cases passed:
`/tmp/nurbs-drag-ctest.log`. The five viewport workflows passed; their results are
`/tmp/nurbs-drag-final-ui.log` and `/tmp/nurbs-drag-ui-regressions.log`.
Clang-format 20.1.8, Python byte-compilation and `git diff --check` passed.
The final Blender executable was rebuilt and installed at
`/home/molotovgirl/Desktop/build_linux/bin/blender`; logs are
`/tmp/nurbs-drag-final-build.log` and `/tmp/nurbs-drag-final-install.log`.
