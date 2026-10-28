# Native NURBS rotation display caching — 2026-10-27

The copied-part rotation profile selected four source topology vertices, yet the display evaluator
recomputed boundaries and interiors across the entire 1,179-face model. After the earlier local
normal-cache improvement, boundary work still took approximately 36–37 ms and interior sampling
9 ms. These were separate from source transformation and drawing.

## Changes retained

- Reuse accepted boundary edge samples when the 3D curve, endpoint positions, parameter range,
  incident p-curves, sampling settings, and relevant tolerance requirements permit it.
- Remap cached edge sample indices into the new boundary result rather than borrowing old indices.
- Reuse a boundary use only when its edge samples, p-curve, support coefficients, incidence, and
  traversal agree, and tolerances have not tightened. Changed inputs receive ordinary evaluation.
- Reuse interior sample positions on supports with identical coefficients, weights, and basis.
  Preserve prior conditioning diagnostics conservatively for retained samples.
- Keep the earlier local partial normal reuse and whole-selection rotation transformation shortcut.
  No display density reduction or deferred source transformation is introduced.

## Measurement

Use the saved 13-segment rounded-box fixture and replay 12 rotations of a duplicated face in the
isolated Weston UI runner. Test both the central flat face and a curved face selected near the
upper corner. A preserved binary from immediately before boundary caching provides the reference:
`/home/molotovgirl/Desktop/build_linux/bin/blender-rotation-before-boundary-cache`.
The reference already contains the preceding local normal-cache optimization.

Median CPU display evaluation time, with geometry density unchanged:

| Test | Before this change | After this change |
| --- | ---: | ---: |
| Duplicated flat face | 75.504 ms | 47.605 ms |
| Duplicated curved face | 77.049 ms | 46.122 ms |
| Flat-face boundary evaluation | 36.026 ms | 14.667 ms |
| Flat-face interior evaluation | 9.006 ms | 0.825 ms |

The flat-face case reuses 2,352 of 2,356 edges and 4,704 of 4,708 boundary uses.
Source transform/commit time remains approximately 13–14 ms. These are CPU stage measurements,
not viewport FPS or complete input-to-display latency; the isolated renderer uses software graphics.

## Correctness

Final suites: 31 display-mesh tests, 58 geometry/owner tests (with the repository test assets),
and 13 tessellation tests passed: 102 total. The application rebuilt successfully.

Both UI cases produce exactly matching display vertex positions, triangle indices, and corner
normals in the before/after JSON output. The curved-face screenshots match pixel for pixel;
the flat-face screenshot differs only in the status-bar rounding-bound text. Curved-face cancel,
undo/redo, and Edit Mode exit/re-entry checks pass.

Regression coverage compares cached and fresh boundary evaluation after vertex, curve, support,
p-curve, and sampling-setting changes. Tightening previously permissive tolerances rejects the
same inconsistent geometry as a fresh evaluation. The mesh suite also compares cached partial
rotation against a fresh complete display evaluation.

Raw logs, screenshots, and geometry outputs are in `/tmp/nurbs-move-optimization/` under
`boundary-before`, `boundary-after`, `corner-boundary-before`, and `corner-boundary-after`.
Runner: `/tmp/run_nurbs_move_optimization.py`; workflow: `/tmp/nurbs_rotate_profile.py`.

Remaining costs include source validation/index construction, cache dependency checks, copying
preview/mesh data, and display mesh construction. Whole-model rotations change all supports and
therefore cannot reuse unchanged-component boundary/interior samples through this cache.
