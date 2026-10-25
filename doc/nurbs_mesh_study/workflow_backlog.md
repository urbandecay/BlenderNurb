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

## Implemented: selection navigation

Orbit Around Selection and Frame Selected now read native selected endpoints
through shared kernel/editor queries. Orbit follows Blender's selected-vertex
median; framing follows selected world-space bounds. The existing navigation
preference controls it. Native and actual mesh V/E/F viewport results match,
including Turntable/Trackball, preference off, transformed owners, empty input,
multiple edit owners and shared-data Edit Mode rules. Ordinary Bezier navigation
retains its route. [Commands, tests and scope](native_navigation.md).

## Remaining planned families

Separate by Material/Loose Parts, broader edge/face splitting and subdivision,
offset loop cuts; auto-merge/dissolve; loop/ring/path selection and slide;
fit-point curve creation/joins and exact analytic primitives; broader extrusion,
bridge/fill and symmetry; lasso/circle selection, visibility, source normal frames,
proportional editing and multiple transform owners; texture UV seams/unwrapping,
attributes/materials/normals; intersections/knife/bisect, inset, offsets/thickness,
booleans and deferred bevel/fillet; modifier/GN/animation/export integration and
full Python component bindings. Dependencies and per-operation geometry policy
still govern implementation order. UV texture data stays separate from geometric
surface parameters and trim p-curves.

## Implemented locally: merge repair, By Distance and mesh component overlays

Center/Collapse now plan doomed linear faces before deformation. By Distance
uses BMesh's clustering policy with double source coordinates, the common weld
service and selection remapping; it is available in M and `bpy.ops`.
Native component drawing uses actual mesh edit shaders, shared theme colors and
depth/X-Ray routing. A read-only exact adapter covers fresh tagged Add/E sources
in solid view without changing legacy source ownership. Their legacy body
renderer's X-Ray face-opacity difference remains. Commands, reproduction,
comparison evidence and remaining geometric/display limits are in
[the merge and overlay record](merge_overlay_fixes.md).
The subsequent [selected-face depth correction](face_highlight_depth.md) shares
the mesh edit-cage bias rule and adds full interior coverage checks, fixing the
reported speckling on fresh tagged extrusions.

## Implemented: Split Selection (Y)

Y now disconnects the selected source component closure through a shared kernel
extraction service. It preserves exact curves, supports, trims and incidence,
keeps dependencies needed by unselected geometry, and remaps the selected copy.
The extraction result also supplies the source remainder and independent
fragment now used by Separate Selection and the shared topology-edit family.

Ordinary tagged Add sources can enter the native source path and split in one
undoable operator. All edited owners are prepared before any source transition;
unsupported attributes/modifiers reject without a partial promotion. Existing
untagged legacy Curve/Bezier splitting keeps its route.

The complete [Split tryout and validation](native_split_tryout.md) records the
BMesh comparison, viewport cases, source-file checks, exact commands and limits.
The [shortcut audit](mesh_edit_shortcut_feasibility.md) remains the checklist for
mesh key parity; a binding alone is not implementation evidence.

## Implemented: Separate Selection (P)

P extracts the selected native closure into an independent Blender object through
Y's shared BKE topology extraction. It retains exact supports/curves/trims,
internal sharing, object transforms and material slots. The original stays in
Edit Mode with its remainder deselected; both owners remain editable native
NURBS. All source owners are preflighted before scene IDs change, including
supported tagged source entry. By Material/By Loose Parts remain planned.

The [Separate tryout and validation](native_separate_tryout.md) covers actual
mesh comparisons, V/E/F and region/box selections, undo/redo, multiple and linked
data owners, saved source comparisons and fresh continuation on both objects.
The subsequent V, M and Ctrl+R implementation and validation are recorded below.

## Implemented locally: V Rip, M Merge, Ctrl+R and Subdivide

These use shared independent topology candidates, source identity remapping,
selection normalization, tagged entry and owner preflight/commit services.
Rip splits supported manifold fans and wire junctions. Merge supplies Center,
Cursor, First, Last and Collapse with explicit curved-deformation/trim limits;
closed rational circles are not deleted just because their endpoints share an ID.

Ctrl+R follows rectangular quad rings and creates real shared edges/vertices and
independently editable NURBS subpatches. Homogeneous knot refinement preserves
source shape; no display mesh is used for construction. Hover/cancel, cut count,
slide, undo/redo and continued extrusion have real keyboard tests. Subdivide
supports wires, full rectangular faces, one boundary side and opposite sides.
Adjacent two-side/three-side patterns, general trimmed-face cuts, offset cuts
and degenerate slide endpoints remain explicit limits. These are partial geometry
contracts, not full mesh operator parity. See the exact commands, stage results
and limits in [the topology-tools record](mesh_topology_tools_progress.md).

Editing operators are accessible through `bpy.ops`, with Blender's normal
context requirements. Background execution of cuts/subdivision is tested
separately. Interactive Rip needs a 3D View/cursor, like mesh Rip. A direct
`bmesh`-style Python component API remains planned: native component IDs,
fit-point constraints, lifetime and atomic mutation need deliberate bindings.
Kernel C++ functions do not become Python bindings automatically.
