# NURBS editing workflow backlog

Baseline for this list: `af317139343937959c6559e60755974948262bf6`.
The full scope remains in the handoff stages, operator inventory/parity maps and
menu crosswalk. This list records active workflow priorities and user reports;
it does not replace those inventories or claim unlisted tools are complete.

## Implemented locally: chained editing regression gate

[Mixed native/Mesh workflows](native_composition.md) now compare 38 saved native
checkpoints with actual Mesh counterparts, including Fill, duplicate/weld,
cuts, subdivision, multi-face extrusion, cap deletion, reopening, collapse and
continued wire/surface editing. They check shared identity, source records,
selection/history, evaluation and atomic rejection. A new CTest fixture chain
generates its own source and checks its results; two viewport workflows cover
real keyboard interaction and undo/redo. This found and repaired coincident
Duplicate → By Distance, by distinguishing rewiring from displacement and
proving equivalent affine target boundaries before retaining an existing face.
The user subsequently deferred CV/fit-point interface decisions and prioritized
refining existing editing, beginning with edge bevel.

## Active: native edge bevel and connected corner repair

The [vertex-mode extrusion correction](native_bevel_vertex_extrusion_fix.md)
enters native topology before the first interactive extrusion, preventing float
rounding from warping the faces of an ordinary line → face → box workflow.

The [ruled-surface follow-up](native_bevel_ruled_surface_fix.md) adds exact source
rails and conic blends on one compatible curved ruled edge, including planar
end-cap repair. It also verifies ordinary mouse-extruded boxes in a fresh process.
General curved contact tracing and connected curved corner networks remain pending.

The [input correction](native_bevel_input_fix.md) fixes zero width when Ctrl remains
held after Ctrl+B. Native bevel now uses mesh bevel's shared modal map and user
bindings. Paired mesh/native mouse tests and a visible factory-scene test pass.

[Native edge bevel](native_bevel.md) implements Ctrl+B on straight shared edges
between planar polygon faces. The repair plans connected selections, shared
corner stations, planar miters and higher-valence chamfer fans together.
Rounded orthogonal valence-three corners use smooth rational octants; strips
use the shared partition service. Mouse/numeric preview, wheel segments,
cancel, undo/redo and Python operator access use the native edit pipeline.

The [robustness follow-up](native_bevel_robustness.md) adds native network width
limiting, enabled by default with C to toggle, and a certified spherical-corner
construction that tolerates source-precision rotation errors. It passed 96
kernel tests, 20 isolated viewport workflows and five CTest cases.

The [corner-blend follow-up](native_bevel_corner_blends.md) adds nonorthogonal
valence-three boundary patches, common conics at unequal two-edge junctions, and
private planar quad charts for continued editing. General blends certify G0
connection; constant radius and G1 are not claimed. It passed 101 kernel tests
and eleven isolated viewport workflows, including actual mesh references and
continued G / Shift+D edits.

The [planar-chart correction](native_bevel_planar_chart_fix.md) fixes the ordinary
line → face → box → resize-top → Ctrl+B failure, privately reparameterizing flat
non-affine supports. Construction and validation now use consistent certified
boundary bounds. It passed 104 kernel tests, nine viewport workflows (including
two visible desktop windows and a mouse-width sweep) and five CTest cases.

General curved-support blends, higher-valence rounded corners,
vertex bevel, nonplanar corner caps with more than four sides, custom profiles
and alternative width types remain pending. The earlier connected construction passed 248 kernel tests,
14 isolated viewport workflows and five CTest cases; see the bevel document
for exact scope, commands and evidence.

The reported Bevel → Split → move-one-corner failure is addressed by the
[rectangular trimmed-face deformation rule](trimmed_corner_transform_fix.md)
in the shared component-transform service. It preserves the NURBS basis and
existing interior while recording bounded edge/support correspondence.

## Deferred by user: curved fit-point interface

The existing numerical foundation remains useful. The user considered both CV
and fit-point editing, then requested that handle-mode changes wait while the
existing topology tools are refined. The earlier fit-only interface proposal
below is not a newly approved implementation decision.

The user's pyramid shading patches are addressed in the shared float display
conversion; see [cause, source-preservation contract and checks](pyramid_shading_fix.md).
Exactly collinear float triangles are omitted before Mesh normal encoding,
with explicit counts and provenance back to the retained double preview.

The [decimal-boundary correction](decimal_boundary_agreement_fix.md) extends
whole-interval agreement certificates for rounded straight trims on polynomial
bilinear supports. It fixes the binary64 `0.3` pyramid move with unchanged source
tolerances and zero extra evaluation allowance. Typed native G retains doubles;
the earlier claim that GUI translation could not exercise this input was wrong.
Exact-rational witnesses, genuine-gap rejection and decimal keyboard/Mesh
comparisons exercise the correction.

The intervening user-reported pyramid corner transform failure is repaired in
the shared component deformation service; see the [implementation and regression
checks](pyramid_vertex_transform_fix.md). This extends partial transformation of
three-sided bilinear trims without activating curved fit-point handles.

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
booleans and general bevel/fillet; modifier/GN/animation/export integration and
full Python component bindings. Dependencies and per-operation geometry policy
still govern implementation order. UV texture data stays separate from geometric
surface parameters and trim p-curves.

## Implemented locally: merge repair, By Distance and mesh component overlays

Follow-up: [successive merges](layered_merge_fix.md) repairs box edge → triangular
top → apex, including all four starting edges and V/E/F. The shared merge service
plans surviving boundaries before support deformation, coalesces equal rebuilt
cycles, preserves directed incidence and verifies fractional-coordinate boundary
agreement. Longer collapse chains and saved-source continuation are covered.

Follow-up: [per-face surface wire grids](surface_wire_grid_fix.md) records the
display-grid regression after Collapse and the shared preview/mesh-renderer
filtering used to preserve unaffected rectangular patches.

Follow-up: [cube top → one shared apex](merge_cube_top_fix.md) corrects Center
and Collapse on extruded boxes, including slanted side triangles. It adds
BMesh comparisons, source error-budget checks, eight actual viewport workflows
and saved NURBS source validation.

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
