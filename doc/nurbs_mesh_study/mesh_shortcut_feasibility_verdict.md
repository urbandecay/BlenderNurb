# Verdict: Blender mesh-edit shortcuts for native NURBS

Audit baseline: `e649f7e137735ce5501301b7efd440ac68010daf`, after the
selection, extrusion, merge, cut, transform, snapping and viewport checkpoints.
This study checks Blender's default `km_edit_mesh` keymap and its operator
semantics against `km_edit_curve_legacy`, the native NURBS source model, the
current-checkout audit, the 358-row operator parity map and the menu crosswalk.
The inventory includes menus, APIs and data operations as well as keyboard
bindings; it is not 358 keyboard shortcuts. Add-ons, custom keymaps and
runtime-generated tools are outside this verdict.

Follow-up: [native bevel](native_bevel.md) adds a restricted straight-edge
construction. The user has also deferred CV/fit-point interface changes while
existing editing is refined; the original fit-only proposal below records the
earlier audit scope rather than a newly approved interface decision.

## Verdict in one sentence

**Most mesh editing interactions have a meaningful NURBS counterpart, and many
can preserve the same selection/topology behavior exactly, but they cannot all
use the mesh geometry algorithm or produce the same shape.** The right design
is a source-authoritative NURBS topology/geometry kernel in BKE, with Blender
operators and modal interactions in the editor. The current BKE source graph is
a useful start; it is not yet a complete BMesh-equivalent toolset, and BMesh is
a behavioral reference rather than a NURBS geometry implementation.

Keep these source domains distinct:

* **Topology vertex/edge/face**: shared endpoint identities, whole source NURBS
  curves, and trimmed regions of NURBS surfaces. These are the V/E/F edit
  components and must have real shared identity.
* **Curve fit point**: a user-facing interpolation constraint that edits curve
  shape. It is not a topology vertex and is not an internal rational control
  coefficient (CV). The user has specified fit points, not CV editing.
* **Display mesh**: a sampled view/picking representation only. It may provide
  viewport feedback, but it cannot become the edit result or hide a failed
  NURBS fit/intersection.

“Exact” below means exact source construction for the stated preconditions. It
does not mean bitwise equality with Blender's float mesh result or with a
triangulated preview.

## Feasibility by shortcut family

### Selection and visibility

| Mesh shortcut / behavior | Verdict for NURBS | Current native state |
|---|---|---|
| Tab; 1/2/3 V/E/F; Shift+1/2/3 extend modes | **Same interaction and exact component selection.** Use shared topology IDs; keep fit points in their separate curve-shape editing domain. | Native V/E/F modes and UI exist. Ctrl+1/2/3 mesh expand-mode and Ctrl+Tab menu behavior are not routed by the current Curve keymap. |
| A, Alt+A, Ctrl+I; click and box selection | **Same interaction and exact IDs.** Every selection gesture must normalize to the same source components before editing. | Native select-all and supported click/box selection exist; click/box result agrees. |
| L, Shift+L, Ctrl+L linked select | **Same topology semantics.** Traverse explicit incidence; coincident positions do not imply connection. | Implemented for supported topology. Ambiguous hole/connectivity policies reject pending a declared rule. |
| Ctrl+Numpad +/- grow/shrink | **Yes, but follow mesh `region_extend` within the active selection domain.** Do not invent vertex→edge→face mode transitions. | The Curve binding calls legacy curve-array operators; no native BRep route. |
| Shift+Ctrl+Numpad +/- next/previous | **Yes, combinatorial equivalent.** Match BMesh selection history and adjacency heuristics; source IDs are storage references, not selection order. Define ties at holes, repeated seams and non-manifold junctions. | Real Python mesh operators exist; no NURBS adapter/binding. |
| Alt-click loop; Ctrl+Alt-click ring; Ctrl-click shortest path | **Yes on unambiguous shared topology.** Walk source face/edge incidence, not a surface's U/V grid or the preview mesh. Branching, holes and non-manifold cases need explicit stop/tie rules. | Not implemented for native components; the Curve shortest-path binding still reads legacy curve data. |
| Shift+G similar selection | **Yes for declared properties.** Incidence, degree and stored tags can be exact; length/area/normal comparisons require units, tolerances and a curved-face normal rule. | Curve operator only compares legacy spline/control-point properties; no native component criteria. |
| H, Shift+H, Alt+H hide/reveal | **Yes.** Store hidden/selectable state on native IDs and match mesh selection, picking and reveal behavior. | No native hidden state; current Curve operators inspect legacy lists. |
| Shift+Ctrl+M select mirror | **Yes with explicit correspondence.** Prefer a stored counterpart map; coordinate-plane matching needs a declared plane and tolerance. This is not Ctrl+M transform mirror. | No general native counterpart map/selection operator. |
| Ctrl+Shift+Alt+M non-manifold; Ctrl+Shift+Alt+F linked flat faces | **Yes with NURBS definitions.** Non-manifold follows edge-use incidence; linked-flat uses analytic normals and an explicit angle rule. | Source incidence/orientation exists; selection operators are not routed natively. |
| Lasso/circle; Alt+Z X-Ray | **Lasso/circle can use the same gestures** and resolve hits to source IDs. X-Ray is shared viewport behavior, not geometry. | Native depth/X-Ray and click/box are covered; lasso/circle do not select source IDs yet. |

### Transforms, snapping and view navigation

| Mesh shortcut / behavior | Verdict for NURBS | Current native state |
|---|---|---|
| G/R/S; numeric entry; axes/planes; precision; confirm/cancel; F9; pivots | **Reuse Blender's modal UI.** Affine edits of a whole rational support are exact. Partial edits are exact only when the shared incident source can be updated consistently; otherwise reject or use an explicitly requested, tolerance-checked fit. | Native G/R/S and supported pivots are implemented for restricted topology layouts and a single edit object. This is not fit-point editing or full parity. |
| Ctrl+M transform mirror; Ctrl+Alt+Shift+S shear | **Exact affine operations** on complete rational supports (weights/knots remain); partial component edits need shared-topology/orientation handling. | Transform mirror is bound but rejected by the native adapter. Shear is not bound in the Curve keymap. |
| Shift+W bend; Shift+Alt+S To Sphere | **Meaningful, generally shape-changing.** Needs a fit rule, constraints and explicit residual; do not move hidden CVs by pretending they are mesh vertices. | Bend is rejected by the adapter; To Sphere is not bound. |
| Alt+S shrink/fatten | **Meaningful as an offset/deformation tool**, exact only in restricted cases; general offsets need intersection, trim repair, tolerance and failure reporting. It is not equivalent to an ordinary fit-point drag. | Legacy binding only; native adapter rejects it. |
| O / Shift+O / Alt+O proportional controls and radius adjustment | **Meaningful on fit points once the fit-point edit contract exists.** Reuse falloff UI, solve authoritative curves, preserve analytic circles/arcs, and report error. “Connected” follows source topology where applicable. | UI settings are available; native proportional transform is rejected because the fit-point rule/UI is absent. Two fit-point drag decisions remain open in `fit_point_foundation.md`. |
| Shift+V vertex slide | **Meaningful as source topology sliding** along a connected curve with affected trims updated. Fit-point slide is a separate shape edit. | Not implemented/bound natively. |
| Ctrl+A skin resize | **No general NURBS equivalent.** Only a sweep with an explicit section-radius parameter has a comparable parameter to edit. | Not bound in Curve mode. |
| Shift+Tab / Shift+S snap controls; Ctrl during transform | **Reuse the UI; provide NURBS target queries per snap type.** Snapping coincident components does not weld them. | Native endpoint snapping and snap-then-extrude are implemented. General grid, edge, face, projection, alignment and multi-point cases are incomplete; some unsupported targets can be rejected. |
| Orbit Around Selection; Numpad Period Frame Selected | **Reuse Blender's navigation path.** For curved source edges/faces, use a defined source representative/bounds rule; endpoint median/bounds will not always enclose the visible curve/surface. | Implemented and visually checked for current topology representatives; arbitrary curved extent is not fully represented by endpoint bounds. |
| Home/View All, view-axis keys, MMB, zoom, Trackball/Turntable | **Reuse.** These are viewport navigation, not NURBS operations. | Generic Blender behavior. |

### Topology and geometry tools

| Mesh shortcut / operation | Verdict for NURBS | Current native state |
|---|---|---|
| E / Alt+E extrusion menu | **Yes, with source-native construction.** A line/curve extrudes to a connected ruled surface. A face region makes walls only on its outside boundary and shares the result across faces. Normal/individual/manifold variants need separate contracts. | E, line-to-surface, multi-face outer walls and the snap-enabled regression are implemented for supported cases. Alt+E's full menu/variants are not. |
| Shift+D duplicate/move | **Same behavior; exact source copy** with fresh IDs and retained sharing inside the duplicate. | Implemented/tested for supported native components. |
| Ctrl-click dupli-extrude | **Useful counterpart**, but its cursor placement and connection rule must be specified for each component domain. | Not implemented natively. |
| F add edge/face; Alt+F fill; mesh Grid Fill | **Yes.** Make source curves/trim loops and a source patch. Planar and compatible ruled/bilinear cases can be exact; general boundaries need a specified parameterization and a fit tolerance that is measured and reported. Keep intentional corners sharp. | Native fill covers constrained cases; four straight corners can form a bilinear patch including a non-planar one. Arbitrary trimmed boundaries/Grid Fill are not full parity. Curve mode binds F to legacy `make_segment`; Alt+F/menu routing is incomplete. |
| X/Delete; Ctrl+X/Ctrl+Delete dissolve | Delete has **same topology meaning**. Dissolve is exact only when trims can be united on a common support; cross-support reconstruction is a separate tolerance-managed fit. | Native delete works, including delete-top-face then continue editing. General source dissolve is not implemented; the legacy Curve dissolve binding is not native topology dissolve. |
| M merge menu / By Distance; Alt+M split menu; Y split | Merge/split are **meaningful source-topology operations**. Exact identity welding needs compatible incidence; proximity remains an explicit tolerance test and never happens merely from snapping. | M Center/Cursor/First/Last/Collapse and By Distance are implemented. Y split is implemented. Alt+M parity and general trim collapse are incomplete. |
| P separate | **Same closure-extraction behavior**, preserving internal sharing and creating a new owner. | Separate Selection implemented; By Material/Loose Parts variants are not. |
| V rip; Alt+V rip with fill; Alt+D rip edge | **Yes for supported incidence patterns.** Duplicate boundary identities and update trims atomically. Fill/edge-extend variants need their own topology contract. | V rip supports restricted manifold fans/wire junctions. Alt+V and Alt+D are absent. |
| Ctrl+R loop cut; Shift+Ctrl+R offset loop cut; Subdivide | **Yes as real BRep/trim partitions.** Isoparametric cuts are exact and preserve the support. General paths/offsets need surface intersection or parameter curves and tolerance. Knot insertion is shape-preserving representation refinement, not a topology cut. | Ctrl+R works on rectangular quad rings; Subdivide handles wires, full rectangles, one side and opposite sides. General trims, offset cuts and some patterns remain. Shift+Ctrl+R is currently Curve row-select, not offset cut. |
| I inset | **Meaningful.** Planar/compatible curve offsets can be exact. Curved-face inset must declare whether it means parameter-space, 3D offset or geodesic-like distance; general offsets are tolerance-managed. | This shortcut was missing from the older audit; no native inset service/operator. |
| Ctrl+B / Shift+Ctrl+B bevel | **Meaningful as fillet/chamfer.** Exact on constrained analytic/planar cases; general offsets, intersections and corner joins need a tolerance-managed kernel and explicit continuity. | Ctrl+B has a [restricted native straight-edge chamfer/rational round](native_bevel.md) between planar polygon faces, with [private affine chart normalization](native_bevel_planar_chart_fix.md), shared corner planning, planar miters and live source preview. [Network width limiting](native_bevel_robustness.md) starts enabled and uses C to toggle. Right-angle valence-three corners use certified rational octants. [Nonorthogonal three-edge blends and unequal two-edge junctions](native_bevel_corner_blends.md) now share positive rational boundaries, with explicit G0-only continuity. Private planar quad charts support continued edits. Vertex bevel, general curved-face blends and higher-valence rounded corners remain absent. |
| J connect path; K / Shift+K knife | **Meaningful as source curve/trim cuts.** Exact when the path is representable on the support; general spatial cuts need NURBS intersection/projection and explicit tolerances. Never cut the preview mesh. | Not implemented. |
| Ctrl+T / Shift+Ctrl+T triangulate; Alt+J tris-to-quads; Alt+P poke | **Meaningful as face-region partitioning** on the existing NURBS support, not as tessellation changes. Exact only where the partition curve has an exact compatible representation; otherwise trim/intersection work is tolerance-managed. | No general trim partition service for these operations. |
| Alt+R Spin; mesh Screw | **Yes for finite, discrete mesh semantics.** Transform exact copies at each configured step; in extrusion mode connect matching stations with degree-1 ruled patches and shared station identities. Keep sharp station boundaries. Full-turn merge uses known source correspondence, not proximity weld. Continuous revolution is a different NURBS operation. | No native topology Spin/Screw. The old Curve Spin edits a control net and is not this feature. |
| Ctrl+N/Shift+N normals; Alt+N normals menu; Alt+L point normals | **Orientation is meaningful** and derives from support/trim orientation. Shading overrides are separate metadata; they must not alter the source surface to fake a normal. | Partial source orientation data exists; full operators/normal-frame transform support are gated. |
| Ctrl+0…5 subdivision level | **Not an edit-topology subdivision command.** In this keymap it calls `object.subdivision_set` to add/set a Subdivision Surface modifier. Native NURBS evaluation currently rejects modifiers. A NURBS preview-density shortcut could be useful, but it is a distinct display control; knot refinement is not equivalent. | No native counterpart to the modifier action or NURBS preview preset on these keys. |

### Attributes, UVs and menus

| Mesh shortcut / operation | Verdict for NURBS | Current native state |
|---|---|---|
| Shift+E edge crease; bevel weight; seam/sharp/Freestyle tags | **Some concepts transfer as metadata; crease does not transfer as a scalar.** NURBS continuity is determined by degree, knots, weights and adjacent supports. Texture seams/sharp display tags can live on shared BRep edges. | No complete native metadata domain. Do not write mesh crease values into rational weights or knots. |
| U UV map / unwrap; rotate/reverse face UV | **Yes as a separate texture-coordinate domain.** Intrinsic surface U/V and trim p-curves define geometry and are not texture UVs. Single-patch parameter UV may seed an unwrap; multi-face seams/poles need stored per-face/corner texture data. | No persistent native texture-UV layer/operator. |
| Ctrl+G vertex groups; Ctrl+H hooks | **Yes with stable source IDs.** Group/hook weights are not rational basis weights; a hook must update supports/trims through the kernel. | Not integrated for native domains. |
| Ctrl+F/E/V context menus; F3, Q, F9; undo/redo | **Keep the familiar access points.** Menus must show only supported source operations and expose true parameters/errors. Undo is one atomic source/topology/selection change. | Some custom NURBS menus/operators exist; full mesh-style vertex/edge/face menus and a public component API remain incomplete. `bpy.ops` access does not itself provide a BMesh-like Python component model. |
| Shift+A mesh add menu | **Offer NURBS primitives with exact rational analytic data.** Do not silently turn polygon primitive topology into a smooth analytic primitive. | Native primitive authoring is incomplete; existing legacy add paths are not full native graph creation. |

The built-in mesh menus contain more actions than `km_edit_mesh` binds directly.
The [operator parity map](operator_parity_map.csv) and [menu crosswalk](edit_mesh_menu_crosswalk.md)
cover those operations, including bridge/fill, bisect, knife project, Boolean,
solidify, wireframe, edge rotate, UV variants, attributes and conditional
Freestyle items. The same verdict rule applies: topology/metadata can often be
preserved exactly; offsets, intersections, trims and fitting need explicit
geometry contracts. No menu row proves implementation.

## Current code reality and corrections to older claims

The NURBS owner contains rational supports plus persistent shared topology
identities, face loops, edge uses and p-curves. BKE provides incidence, selection,
copy/extraction, split, deletion, merge, fill, extrusion, restricted cuts and
refinement services. Operators orchestrate them. This architecture can support
mesh-like topology behavior without making BMesh the NURBS geometry kernel.

Implemented does not mean complete: native V/E/F, click/box/linked selection,
supported G/R/S, endpoint snap, duplicate, extrusion, delete, split, separate,
restricted rip/merge/By Distance, rectangular Ctrl+R and restricted Subdivide
exist. Their limits are recorded in [the checkout audit](current_checkout_audit.md)
and [topology progress](mesh_topology_tools_progress.md). In particular, fit
point handles, native hidden state, Grow/Shrink, Next/Previous, loop/ring/path,
lasso/circle, general surface snapping, inset, bevel, knife/intersection,
UV storage and a public BMesh-like Python component API are not complete.

The legacy Curve keymap is not the Mesh keymap. It binds some same-looking
letters to Curve operators, and several selection/hide operators still traverse
legacy `Nurb`/BPoint arrays rather than native component IDs. Native promotion
does not make those routes native automatically. Important conflicts include
`F` (legacy make-segment versus mesh add/fill), `V` (rip versus legacy handle
type), and `Shift+Ctrl+R` (legacy select-row versus mesh offset cut). These
need explicit native-mode routing while preserving unrelated Curve/Bezier and
old-file behavior.

The previous `mesh_edit_shortcut_feasibility.md` is a design checklist, but its
status and some recommendations are stale. This verdict supersedes those points:

* M → By Distance, Y split, P Separate Selection, restricted V rip, Ctrl+R and
  some Subdivide cases are implemented; they must not remain on a “planned” list.
* It omitted I / Inset.
* Grow/Shrink must follow BMesh active-domain region extension, not cross-mode
  endpoint→curve→face steps.
* Next/Previous follow selection history and BMesh adjacency heuristics, not a
  sorted stable-ID sequence.
* Ctrl+0…5 is a Subdivision Surface modifier level, not source refinement or
  display resolution. A preview-sampling shortcut would be a new, separate
  contract.
* Spin means finite step copies/extrusion in the mesh operator. A continuous
  revolution is a separate NURBS feature.

## Adversarial review and resolved disagreements

Three independent reviews covered selection/transforms/navigation, topology and
construction, and fit points/data. Reviewers cross-checked the source claims
and challenged each other's proposed counterparts. The resulting decisions are:

1. **Finite Spin — accepted with stricter conditions.** One review proposed
   exact stepwise transformed copies and ruled patches. The adversarial check
   confirmed the geometry for matching rational parameterizations, then added
   degree-1 angular patches (to preserve sharp station corners), shared IDs
   across all stations, outer-boundary-only walls, BMesh-compatible selection,
   and identity-based full-turn closure. Arbitrary resampling/fitting would
   invalidate the exactness claim. This is still not continuous revolution.
2. **Ctrl+0…5 — rejected as a topology/refinement mapping.** The source binding
   and Python operator change a Subsurf modifier. The NURBS evaluator currently
   rejects modifiers. A preview-density preset could use similar interaction,
   but it would be a new display-only feature.
3. **Grow/Next selection — corrected to mesh source behavior.** Grow/Shrink
   follows active-domain BMesh region extension. Next/Previous uses BMesh
   adjacency and selection history. Stable NURBS IDs preserve references; they
   do not define the order.
4. **Fit points versus CVs — confirmed.** The rational coefficients are not
   user mesh vertices. Fit-point editing needs its own stable handles and
   constrained solve; exact circle/arc intent must be retained.

## Recommended implementation direction

1. Keep the existing BKE topology graph as the shared identity/selection
   backbone. Add geometry families there, not as editor-only one-off operators.
2. Finish native selection semantics and mode-aware keymap routing, including
   visibility, grow/shrink, history-based next/previous, loops/rings, paths,
   menus and explicit legacy fallback.
3. Extend exact same-support topology work: cut/subdivide patterns, inset where
   the support permits it, fill/bridge, triangulate/poke partitions and
   dissolve/edge operations. Validate shared IDs and atomic failure.
4. Build the fit-point identity/edit system separately from topology vertices.
   Decide free-form and analytic-circle drag behavior before enabling those
   handles; keep exact rational circles/arcs exact.
5. Add a shared tolerance/intersection layer for knife, bisect, offsets,
   bevel/fillet and Boolean work. Every operation exposes a tolerance and
   returns its measured error or a clear rejection.
6. Add separate texture UV, selection visibility and non-geometric attribute
   domains. Bind them to stable native identities and expose them to Python.
7. Reproduce Blender's finite Spin/Screw behavior as discrete source geometry;
   build continuous revolve/sweep as separate NURBS-specific operations.

This is a feasibility verdict, not a claim that these features have been
implemented. No application code, build, or tests were changed or run for this
documentation-only study. The current source/build/test evidence remains in the
linked checkout and feature checkpoint documents.
