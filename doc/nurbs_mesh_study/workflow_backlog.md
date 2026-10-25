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

## Reported: vertex-to-vertex snapping in Edit Mode

The user tried to snap a NURBS vertex to another vertex and received:
“Native NURBS snapping requires a source-precision snapping adapter.”
Screenshot supplied in the conversation on 2026-10-25. This confirms the existing
explicit snapping gate; the original .blend, exact gesture and target object are
not supplied. Do not call it a repaired or fully reproduced interaction yet.

Required behavior:

- Resolve the snap target to source identity and its authoritative position.
  A display triangle/float vertex is a picking proxy, not the committed target.
- Use the selected source endpoint/active anchor and Blender's snapping controls.
  Distinguish snapping a single endpoint from moving a selected region as a unit.
- Update every incident edge/support through the common geometry request and
  owner commit. Unsupported curved deformation must report and preserve source.
- Handle same-object, other-object, transformed-owner and linked-data targets;
  define source precision and reporting for an external mesh target explicitly.
- Verify modal preview, constraints, confirm/cancel, undo/redo, save/reopen,
  source identity, and endpoint/edge/support agreement. Include the user's
  vertex-to-vertex example and comparison with Blender mesh snapping.

This is pending implementation. The existing G/R/S geometry adapter deliberately
rejects active snapping instead of committing a quantized display-mesh target.

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
