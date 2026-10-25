# NURBS editing workflow backlog

Baseline for this list: `af317139343937959c6559e60755974948262bf6`.
The full scope remains in the handoff stages, operator inventory/parity maps and
menu crosswalk. This list records active workflow priorities and user reports;
it does not replace those inventories or claim unlisted tools are complete.

## Active: curved fit-point foundation

Target: fit points are the normal curve shape handles. Shared topology vertices remain
line/curve endpoints; internal rational control coefficients are not a user
editing domain. Establish source fitting/refinement services and explicit
interpolation/analytic constraints before activating curve-handle editing.
Freeform anchor behavior and exact-circle dragging choices were asked of the
user and remain open until answered. Preserve exact rational circles/arcs. The
[numerical foundation](fit_point_foundation.md) now supplies rational basis
queries, fixed-basis constraints and explicit new interpolation. Persistent
handles, analytic dragging and viewport integration remain to implement.

## Repaired: vertex snapping and extrusion with snapping enabled

The user's line → face → duplicate → corner snap → E report is now exercised
through actual viewport events. The failure did not require welding or even a
successful corner snap: the native transform cancelled merely because snapping
was enabled, leaving E's initial cap and walls coincident. The earlier
geometry-only test was insufficient to diagnose this editor failure.

Vertex snapping now resolves native topology endpoint IDs and double source
positions, reuses Blender's snap controls, and applies movement through the
shared kernel transform and common commit. Active, Closest, Median and Center
bases, axis/plane constraints, the cursor pivot, transformed owners and an
external mesh vertex target have viewport checks. The complete corner-snap/E
sequence is compared with an actual mesh, with cancellation, undo/redo and saved
source continuation. Separate coincident vertices are not implicitly welded.
See [implementation, reproduction and limits](vertex_snapping.md).

Remaining snapping work includes general native edge/face queries, surface
projection/normal alignment, a custom modal snap base, accumulated points,
geometric Rotate/Scale snapping, multiple moving owners, visibility and caching.
Other native owners and linked-data targets need dedicated interaction coverage.
Partial curved deformation remains governed by the kernel's explicit geometry
support; this point adapter does not add arbitrary curve/surface fitting.

## Remaining planned families

Split/rip (Y/V), Separate (P), exact edge/face splitting, subdivide and real
Ctrl+R topology cuts; merge/weld/dissolve; loop/ring/path selection and slide;
fit-point curve creation/joins and exact analytic primitives; broader extrusion,
bridge/fill and symmetry; lasso/circle selection, visibility, source normal frames,
proportional editing and multiple transform owners; texture UV seams/unwrapping,
attributes/materials/normals; intersections/knife/bisect, inset, offsets/thickness,
booleans and deferred bevel/fillet; modifier/GN/animation/export integration and
full Python component bindings. Dependencies and per-operation geometry policy
still govern implementation order. UV texture data stays separate from geometric
surface parameters and trim p-curves.
