# NURBS Edit Mode move optimization — 2026-10-27

Starting point: `0e9ad502e726f52222a0f0c7adce97ac181b42dc`, identical to the GitHub branch before these changes.

## Retained changes

1. Cache selected vertex starting positions for the translation precision check. Compare moved vertices directly instead of rebuilding two full topology indexes on every update.
2. For a translation covering every topology vertex, transform active 3D curves and surfaces directly. Preserve unused parent supports, UV curves, identities, weights, basis, and selection. Retain the general component API's geometric endpoint checks, arithmetic error bounds, and structural validation. Partial selections keep the general correspondence path.
3. Reuse the validation guarantee of immutable `ReadSnapshot` states when checking a live owner or publishing an evaluated candidate. New geometry/selection still passes the ordinary create/commit validation; ownership and stale-state checks remain.
4. Store display preview samples and connectivity in an immutable shared allocation. A renderer handoff keeps its own current source lease and independent Mesh, while sharing the read-only preview. This removes a second deep preview copy without changing surface density or display positions.

These changes update source geometry and shaded display during the drag. They do not use the abandoned temporary object-matrix method. Object transforms stay unchanged in Edit Mode.

## Rejected or already present

- Tried replacing stationary coefficient-request masks with empty requests to avoid allocating work for unchanged surfaces. Two existing shared-support conflict tests failed (`TrimmedQuadsRechartSkewLayoutsAndRejectConflictingFields` and `SharedTriangleSupportsRequireIdenticalOwnerDeformation`). Discarded the experiment and restored the original request logic.
- Blender already demotes queued intermediate movement events to `INBETWEEN_MOUSEMOVE`; the transform operator updates mouse input only for `MOUSEMOVE`. No additional event-dropping implementation was added.

## Measurement method

Use the same saved 13-segment beveled native NURBS model for reference and candidate. It has 1,176 topology vertices, 2,352 topology edges, 1,178 topology faces, and a display mesh with 50,680 vertices and 101,356 triangles. Replay identical absolute mouse positions in a private headless Weston session. Compare the preserved original executable against the rebuilt candidate, alternating runs and avoiding concurrent compilation during timing runs.

The reported timings are CPU transform-update and prepared-display-handoff timings from the existing diagnostic logs, not viewport FPS. The isolated display uses software rendering and reports EGL warnings, so its draw times do not represent the user's GPU. Keep display resolution unchanged.

Raw runs, screenshots, geometry output, and logs: `/tmp/nurbs-move-optimization/`. The runner is `/tmp/run_nurbs_move_optimization.py`; the saved-scene interaction checks are `/tmp/nurbs_optimization_ui.py`.

## Checks

- Transform suite: 32 tests, including exact comparison of full-selection translation against the general path, unchanged unused supports, repeated IDs, error budgets, and invalid input endpoints.
- Display mesh suite: 29 tests, including immutable preview sharing and independent renderer Mesh writes.
- Object evaluation suite: 6 tests, including source lease validity and retained cache lifetimes.
- Edit state/owner suite: 50 passing core tests; 10 optional saved-file fixture tests skipped because their separate asset outputs were not provided.
- Whole-model UI checks: repeated positions, live display movement, unchanged object matrix, cancel, confirm, undo/redo, and mode exit/re-entry. Reference and candidate output geometry is identical.
- Duplicate-face UI checks: original remains stationary, duplicated face moves, connectivity and edge lengths remain stable during dragging. Capture the shaded display during the drag.
- Inspected before/during screenshots for shaded-surface and edit-cage alignment. The model-region crops (pixels 250,115 through 490,320) of reference versus final before/during/confirmed screenshots were pixel-identical. The final UI geometry JSON also matched the reference exactly.

## Final timings

Median CPU time per update. Whole-model rows average the medians of two alternating reference/candidate replays; partial rows are one identical duplicate-face replay per build.

| Operation/stage | GitHub reference | Final candidate | Reduction |
| --- | ---: | ---: | ---: |
| Whole-model transform update | 40.984 ms | 27.468 ms | 33.0% |
| Whole-model prepared-display handoff | 6.799 ms | 0.749 ms | 89.0% |
| Duplicated-face transform update | 37.741 ms | 28.057 ms | 25.7% |
| Duplicated-face prepared-display handoff | 7.112 ms | 0.738 ms | 89.6% |

The final whole-model geometry stage measured 8.979–9.610 ms, versus 11.988–13.498 ms in the reference. The separate movement precision check fell from 4.469–4.531 ms to below the log's 0.001 ms resolution on this whole-model case. Display triangle count and shading quality stayed unchanged.

Transform time and display handoff are separate measured stages; these values are not presented as viewport FPS. Full rendering and overlay cost remains outside the transform-update timing.
