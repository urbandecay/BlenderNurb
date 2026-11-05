# Solid bevel creation optimization — 2026-11-04

Starting point: pushed commit `e2a39e39536`, branch `solid/analytic-editing-workflow`.
Measurements use Blender 5.2.2 LTS on the same machine, fresh 2-unit cubes, all edges selected,
and width 0.2. These measure bevel creation and changing bevel width, rather than object rotation.

## Retained iterations

| Implementation | Solid 32 subdivisions: synchronous median |
| --- | ---: |
| Starting version | 841.613 ms |
| Indexed display lookups, cached generated curves/edges, modal topology reuse | 256.893 ms |
| Also hashed display selection membership and reserved model indexes | 194.852 ms |
| Also topology-only selection indexes, cached validation frames, contiguous corner grids | 147.787 ms |

Each synchronous result is five measured trials after one warmup; mesh/solid ordering alternates.
The final result is about 5.7 times faster. Mesh controls ranged from 20.7 to 28.0 ms during
these runs, so small differences should not be interpreted as precise improvements.

The final 32-subdivision live run sent 12 width-changing events: subsequent C++ updates had
median 145.406 ms, and viewport draw intervals had median 149.823 ms (6.7 Hz) and p95 159.720 ms.
The previous version took roughly 930–950 ms per live update in the preceding study.
For the usual one-subdivision fillet, analytic live updates including wheel changes were
about 5.5 ms with viewport cadence near 59 Hz; native shading was about 2.5–3.1 ms per update.
Both use the same visible geometry; wheel tests temporarily increase the subdivision count.
Viewport cadence is a separate measurement from CPU operator time.

A final ordinary synchronous benchmark gave mesh chamfer 0.655 ms, solid chamfer 1.212 ms,
mesh 32-segment bevel 23.607 ms, and solid 1-subdivision fillet 7.132 ms. The ordinary fillet
first-build time remains in the same range as the preceding study (about 7 ms).
Solid subdivision 32 and mesh segment 32 generate different topology. Both comparisons are
retained to distinguish equal numeric settings from approximately similar visible smoothness.

## Implementation

- Display tessellation builds its ID indexes before its face classification pass. No linear
  model scans remain in that pass, including optional profile logging.
- Fillet profile references own cached curve values, avoiding dangling pointers when the curve
  vector grows. Cached edge records contain endpoints, avoiding rescans of the generated edges.
- Integer corner-grid coordinates index contiguous storage rather than allocating tree nodes.
- Display selection uses hashed membership rather than scanning selected IDs per display element.
- Model validation indexes reserve their capacity. Selection indexes omit unused curve/surface
  supports. Sphere/cylinder validation computes constant frame cross products once per face.
- Modal bevel width updates request existing topology reuse. The adapter verifies display
  connectivity and falls back to rebuilding when it changes, including wheel subdivision changes.
- All existing geometry validation remains enabled. Subdivision semantics, source IDs, normal
  methods, exact surface geometry, and selectable face topology remain intact.

## Validation

- Full Blender build, independent analytic geometry suite, and editing suite passed. The editing
  suite includes 4,095 chamfer edge combinations, skewed/oblique fillets, and other solid edits.
- Added maximum-subdivision rounded-cube coverage, including watertightness and native/analytic
  display geometry and source-ID equivalence.
- A temporary before/after harness compared encoded models, selection, display positions,
  polygons, source IDs, and analytic normals across straight and skewed cubes, selections of
  1/4/12 edges, and subdivisions 1/3/32. Output files matched byte for byte.
- Windowed regression runs covered confirming and cancelling previews, both normal methods,
  subdivision changes, and transitions between chamfer and fillet. Geometry and selection must
  remain unchanged on confirmation or return exactly to their original state on cancellation.

## Remaining cost and boundaries

The 32-subdivision cube still has 8,582 analytic faces, including 8,192 corner triangles.
It remains slower than a 32-segment mesh bevel. Further major gains would require reducing
repeated candidate validation through a rigorously validated immutable snapshot API, or separating
viewport subdivisions from analytic face topology. These are not implemented here: the latter
would change how the user edits individual subdivisions. The retained changes preserve current
geometry and editing behavior and do not substitute a lower-quality or delayed final preview.

## Reproduce

```sh
SOLID_BEVEL_CASE=fillet_same_count SOLID_BEVEL_TRIALS=6 blender --background --factory-startup --python tests/python/solid_bevel_creation_benchmark.py
BLENDER_BEVEL_PROFILE=1 BLENDER_SOLID_PROFILE=1 SOLID_BEVEL_KIND=solid SOLID_BEVEL_CASE=fillet_same_count SOLID_BEVEL_MODAL_UPDATES=12 SOLID_BEVEL_MODAL_WARMUP=2 SOLID_BEVEL_CONFIRM=1 blender --factory-startup --enable-event-simulate --python tests/python/solid_bevel_creation_benchmark.py
SOLID_BEVEL_KIND=solid SOLID_BEVEL_NORMALS=BLENDER SOLID_BEVEL_CONFIRM=1 SOLID_BEVEL_CHANGE_SEGMENTS=1 blender --factory-startup --enable-event-simulate --python tests/python/solid_bevel_creation_benchmark.py
```

## Follow-up after pushed commit b345ebc12f9

A second optimization pass retained these changes:

- A shared temporary ID index uses contiguous pointer slots for compact identity ranges and
  bounded hash tables for sparse ranges. It is used by validation, selection, and display lookup.
- Duplicate-ID validation uses compact byte markers when the identity range is bounded relative
  to model size; sparse/large ranges still use hashed membership.
- Outer boundaries and hole loops are visited without allocating temporary pointer vectors.
- Body validation builds only topology indexes and reserves its edge-adjacency table.
- Sphere/cylinder boundary checks cache five exact curve evaluations per shared edge within
  each validation call. Every sample is still checked against each adjacent face's own support.

Fresh 32-subdivision measurements (seven measured trials after one warmup):

| Follow-up stage | Synchronous solid bevel median |
| --- | ---: |
| Pushed b345ebc12f9 baseline | 141.328 ms |
| Compact geometry indexes and body-adjacency reserve | 134.527 ms |
| Also compact display indexes, identity markers, and boundary iteration | 125.503 ms |
| Also shared-edge curve-sample caching | 103.871 ms |
| Final repeat | 108.263 ms |

This is another roughly 23–26% reduction in creation time. The live 32-subdivision preview
measured 9.0 Hz (median draw interval 111.161 ms, p95 117.259 ms), compared with the previous
recorded run at 6.7 Hz. Normal one-subdivision creation remained around 7 ms; its display
cadence remains near 60 Hz. The geometry counts are unchanged.

Validation passed the full analytic and editing suites, live confirmation/cancellation and
wheel changes with both normal methods, and the existing byte-for-byte geometry comparison.
Additional regressions cover sparse IDs, IDs near UINT64_MAX, duplicate and missing IDs,
closed/open hole boundaries, and a detached sphere support sharing cached edge samples.

Scratch storage is bounded by actual model size rather than arbitrary identity magnitudes.
The shared-edge sample cache adds temporary memory proportional to edge count (five Vec3
values plus a flag per edge when sphere/cylinder checks are needed). No geometry validation
passes are skipped, and the source/file representation and display geometry stay unchanged.
