# Solid viewport normal methods

In Object Mode, open **Object** in the 3D Viewport header to choose a shading method for
the selected solid. The mesh object context menu also has these entries when your keymap
opens that menu from the mouse button you use for context menus. With a right-click-select
keymap, right-click selects; use the header's **Object** menu instead.

- **Shade Smooth** calculates normals using Blender's ordinary mesh smoothing. It removes
  `custom_normal` and stores the selection in `.solid_blender_normals` so rebuilding the
  display, editing, undo, and saving/loading preserve the method.
- **Shade Analytic** calculates normals from the solid's surface definitions and installs
  the resulting float corner normals. It removes the native-method marker.

The Shade Smooth redo panel also exposes **Solid Normals: Blender Smooth / Analytic**.
Shade Flat keeps the selected method for subsequent solid edits. Existing solids keep
analytic normals until a different method is selected. Neither method changes source
geometry or display sampling density. Both use Blender's mesh drawing pipeline.

Native-mode source tessellation omits analytic shading-normal evaluation and does not
store sampled analytic corner normals. It continues to evaluate analytic positions and
validate source geometry: the mathematical solid is independent of its viewport normals.
The rigid rotation preview updates positions in either mode, transforming custom normals
only when the analytic layer exists. Blender calculates its geometric normals in native
mode. Cancel restores the starting positions; confirmation preserves visible coordinates.

Analytic shading restoration copies normals directly from matching cached samples. It no
longer creates a temporary Mesh just to retrieve the normal attribute, and samples generated
when restoring shading after loading are retained in the source runtime cache.

## Validation

`tests/python/bl_solid_sketch.py` covers method switching, analytic flat/smooth,
native fillets and extrusion, rigid rotation and normal orientation, nonuniform scaling,
and file round trips. `tests/python/ui_simulate/test_solid_sketch.py` covers live rotation,
cancel, confirmation, undo and redo in both methods. The independent kernel geometry test
checks that disabling analytic normals preserves positions and topology and emits no normals.

For a repeatable CPU comparison using actual solid transforms in both modes:

```sh
../build_linux/bin/blender --background --factory-startup --python-exit-code 1 \
  --python tests/python/solid_normals_benchmark.py
```

This measures confirmed Edit Mode rotation updates, evaluation and reading all normals;
it does not measure viewport FPS or isolate the normal calculation from the transform.
For modal viewport rotation, run `tests/python/solid_viewport_rotation_benchmark.py`
with `SOLID_BENCH_KIND=solid`, `SOLID_BENCH_ROTATION=FREE`, and
`SOLID_BENCH_NORMALS=ANALYTIC` or `BLENDER`, using `--enable-event-simulate`.

## Measurements on the completed implementation

The CPU comparison used a 2-unit cube with a width-0.2 fillet, one fillet subdivision,
and identical display topology in both modes: 3,080 vertices, 3,270 polygons and 12,696
corners. Six fresh-solid trials alternated methods (analytic/native, native/analytic,
analytic/native). Each trial discarded five updates and measured thirty. Builds and
other tests had completed before timing.

| Actual solid method | Per-trial median wall time, ms | Median of trial medians | Median process CPU time |
| --- | --- | ---: | ---: |
| Analytic | 3.631, 3.707, 4.227 | 3.707 ms | 16.320 ms |
| Blender Smooth | 3.053, 3.566, 4.402 | 3.566 ms | 16.140 ms |

The approximately 4% difference is smaller than trial variation; native was slower in
one pair. These results do not establish a consistent rotation speedup. Process CPU
sums worker-thread time and can exceed wall time.

A separate modal viewport test used the saved scene at `/tmp/solid-live-captured.blend`.
The file currently contains a display with 2,760 vertices, 3,926 polygons and 13,368
corners; this must not be confused with earlier measurements of its much denser state.
Both modes used the same actual solid source and displayed topology. Successive
POST_PIXEL draws measured 16.616 ms (Analytic, 60.18 FPS) and 16.632 ms (Blender Smooth,
60.13 FPS), after ten warmups with forty retained intervals. Both were limited near the
display's 60 Hz refresh rate, so this test cannot reveal throughput above that limit.
Confirmation preserved every displayed coordinate in both modes.

Logs: `/tmp/solid-normal-final-benchmark.log`, `/tmp/solid-actual-dense-analytic.log`,
and `/tmp/solid-actual-dense-native.log`. Validation logs: `/tmp/solid-normal-api-complete.log`
(24 cases), `/tmp/solid-normal-ui.log` (both rotation/undo cases), and
`/tmp/solid-normal-kernel.log` (independent geometry and editing suites).
