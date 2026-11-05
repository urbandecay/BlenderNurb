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

The heavy solid still contains 8,582 analytic faces. Each preview still copies the source model
and constructs its new faces. Curved profiles still use full display tessellation. A preview-only
edit-mesh path that delays making the analytic B-rep until confirmation could reduce more work,
but would change how preview selection, cancellation and publication fit together.

## Reproduce

```sh
blender --background --factory-startup --python tests/python/solid_face_extrusion_benchmark.py
SOLID_BEVEL_CASE=bevel32 SOLID_BEVEL_MODAL_UPDATES=20 SOLID_BEVEL_MODAL_WARMUP=3 SOLID_BEVEL_CONFIRM=1 blender --factory-startup --enable-event-simulate --python tests/python/solid_face_extrusion_benchmark.py
```

The extrusion benchmark reuses the existing modal benchmark environment option names.

## Follow-up efficiency study

Starting from the pushed preview path above, profiling found duplicate full geometry validation:
extrusion validated the result, then the editor validated the same result again before publishing
it. It also rebuilt the same edge/face closure map on every mouse update.

The first change made the editor reuse the exact immutable-model validation certificate. That
raised the heavy test from 20.9 Hz to about 25 Hz. Profiling then showed the full geometry scan
still dominated. The final change validates only the extrusion delta against the checked,
unchanged source: it verifies the source entities are still an exact prefix, checks all new
vertices, curves, edges, surfaces and faces, then proves the result is closed. The closure proof
is cached separately for positive and negative sweeps because distance changes coordinates but
not topology. Other operations and the public full validator still do complete checks.

The GUI benchmark uses a 2x2x3 box with a width-0.2, 32-segment fillet, the same selected planar
end cap, 40 simulated distance updates, five warmup draws and analytic normals. Final runs both
cancelled and confirmed; each preserved the expected geometry and selection.

| Version | Median draw interval | Draw rate |
| --- | ---: | ---: |
| Before this study | 47.753 ms | 20.9 Hz |
| Final delta check, cancel | 25.787 ms | 38.8 Hz |
| Final delta check, confirm | 24.323 ms | 41.1 Hz |
| Final build after fresh-ID guard, confirm | 23.088 ms | 43.3 Hz |
| Final build after fresh-ID guard, cancel | 22.740 ms | 44.0 Hz |

Across the two final runs, previews took about 47% less time per update and ran about 91% faster.
A plain box stayed at about 60 Hz. The heavy display remained 4,684 vertices, 13,268
edges and 8,586 faces, so the gain came from less CPU work, not simpler visible geometry.

The kernel tests still run the full validator against preview results, including checking that
each preview is a closed valid body, and compare cached display data with full tessellation. The
test suites passed, including 4,095 bevel edge combinations. Curved-profile fallback, normal
switching, cancellation and confirmation are covered.

The remaining large opportunity is preview-only mesh updates with analytic solid creation
deferred until confirmation. That could avoid copying the full B-rep during each drag, but it is a
larger workflow change involving selection identities, cancel/confirm state and publication, so
it was left for a separate study.
