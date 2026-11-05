# Face extrusion on filleted solids

Starting version: pushed commit `8b6b0ce26a8`, branch `solid/analytic-editing-workflow`.

## Retained improvements

1. Display tessellation and publication share a validation certificate for the exact immutable
   model pointer. A certificate is only constructed after full validation; mismatched model
   pointers require validation again. Standalone model-based tessellation still validates input.
2. Modal face extrusion owns a checked profile for its lifetime instead of repeatedly validating
   that unchanged input. Every resulting solid still undergoes body and geometry checks.
3. Extrusion drags request connectivity-checked edit-mesh reuse, falling back to a rebuild if
   connectivity changes.
4. Positive extrusion of a straight quad caches its display tessellation. Only the generated
   source vertices move; zero-ID samples belong to the unchanged original body. Wall normal
   directions remain constant. Other boundaries, inward/invalid distances, and normal-method
   changes use full tessellation or the existing rejection path.
5. Selected-face profile preparation avoids constructing the unused full geometry index.

The visible extrusion updates continuously. Confirmation retains the displayed position;
there is no delayed movement to the final shape.

## Controlled live benchmark

Same 2x2x3 solid, all-edge fillet width 0.2, selected planar end face, 20 synthetic distance
changes, first three draws discarded. Reference and optimized runs were sequential in the
same executable using a temporary comparison switch. The reference repeated input validation,
full display tessellation, publication validation, and edit-mesh rebuilding as before.
The comparison switch was removed from the final source.

| Method | Median draw interval | p95 interval | Draw rate |
| --- | ---: | ---: | ---: |
| Original path, 32 subdivisions, analytic normals | 89.738 ms | 106.537 ms | 11.1 Hz |
| Optimized path, same solid and normals | 47.753 ms | 51.238 ms | 20.9 Hz |
| Optimized path, native normals | 46.558 ms | 53.456 ms | 21.5 Hz |
| Optimized path, no bevel | 16.719 ms | 18.240 ms | 59.8 Hz |
| Optimized path, fillet subdivision 1 | 16.674 ms | 22.007 ms | 60.0 Hz |

This is approximately 47% less time per displayed update and 88% higher draw rate for the
heavy fillet. It is a modal-drag improvement, not a claim of a comparable synchronous speedup.
Synchronous fresh-object extrusions in this session were about 129 ms before and 127 ms after
at subdivision 32; that difference is too small relative to variability to claim improvement.
Some intermediate measurements overlapped builds/other tests and were discarded as controls.

## Validation

- Full Blender build and independent analytic/editing suites, including 4,095 bevel edge combinations.
- Prepared and ordinary extrusion outputs compared byte for byte at subdivisions 1 and 32.
- Cached display coordinates, polygons and source identities matched full tessellation; analytic
  normals matched within 1e-12, including cache reuse and switching between normal methods.
- Circle-profile extrusion exercised the uncached curved-boundary fallback.
- Invalid input could not obtain a validation certificate; certificates did not match different
  immutable source pointers.
- Windowed tests covered confirmation, cancellation, native and analytic normals, plain boxes,
  and light/heavy fillets. Confirmation preserved the displayed geometry and selection exactly;
  cancellation restored the original geometry and selection.

## Remaining cost

The heavy solid still contains 8,582 analytic faces. Result construction, full body checks,
model copying and one validation of the immutable publication candidate remain. General curved
profiles still use full display tessellation. Copy-on-write model storage and local geometric
validation would require a larger change and stronger invariants than this optimization.

## Reproduce

```sh
blender --background --factory-startup --python tests/python/solid_face_extrusion_benchmark.py
SOLID_BEVEL_CASE=bevel32 SOLID_BEVEL_MODAL_UPDATES=20 SOLID_BEVEL_MODAL_WARMUP=3 SOLID_BEVEL_CONFIRM=1 blender --factory-startup --enable-event-simulate --python tests/python/solid_face_extrusion_benchmark.py
```

The extrusion benchmark reuses the existing modal benchmark environment option names.
