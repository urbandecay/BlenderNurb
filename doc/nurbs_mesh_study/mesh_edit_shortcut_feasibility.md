# Mesh Edit Mode shortcut feasibility for native NURBS

This is a shortcut-level companion to the operator parity map and menu
crosswalk. For the corrected feasibility decision and adversarial reviews, see
[the current mesh-shortcut verdict](mesh_shortcut_feasibility_verdict.md). This
file remains a shortcut-level checklist and older rows below may describe
proposed behavior rather than current implementation. It answers two separate
questions for each binding:

1. Can the familiar Blender shortcut be reused?
2. What should that shortcut do to authoritative NURBS geometry and topology?

## Source and current state

The binding inventory is from `km_edit_mesh` and its helpers in
`scripts/presets/keyconfig/keymap_data/blender_default.py`. This is Blender's
default keymap in this checkout. Mouse-button choices, tool selection, and the
legacy keymap change some bindings. Tool-specific click/drag gestures are
outside this keyboard list.

NURBS editing currently inherits the legacy Curve keymap. The NURBS component
path selectively routes its operators to native source geometry. A key being
bound in that shared keymap does not prove that the operation is implemented
for native NURBS. Some current Curve bindings also conflict with mesh meanings;
those conflicts need mode-aware routing rather than silently keeping old
behavior in the new NURBS workflow.

Current native checkpoints verified in `current_checkout_audit.md` include
component modes 1/2/3, source selection, G/R/S transforms, linked selection,
Shift+D duplication, E extrusion, deletion, and the reported snap-then-extrude
workflow. Click and box selection of the supported source components agree.
Automatic face-normal extrusion, general proportional editing, lasso/circle
selection, general surface snapping, and the broad topology-editing families
are still gates. Fit-point handles are not yet the normal curve editing domain;
internal rational coefficients must not be exposed as mesh vertices.

Labels used below:

* **Reuse** — Blender's key and interaction can stay the same; the operation
  does not depend on mesh-only geometry.
* **NURBS kernel** — keep the key, but implement the operation in the reusable
  NURBS topology/geometry layer and call it from the editor operator.
* **Adapt** — keep the key, with an explicit NURBS-specific shape or metadata
  rule. Exactness depends on the listed case.
* **NURBS-specific counterpart** — keep the familiar key and intent, but
  define a source-native operation instead of running the mesh algorithm on
  an evaluated display mesh.

## Edit mode and component selection

| Mesh binding | NURBS use | Current native state |
|---|---|---|
| Tab — enter/leave Edit Mode | **Reuse.** Same mode transition and undo expectations. | Native component editing entry exists for its supported tagged sources; full source promotion/import is still gated. |
| 1 / 2 / 3 — vertex, edge, face mode | **Reuse.** In the topology workflow, vertex means topological endpoints, edge means a whole source curve/edge, face means a NURBS surface patch/trimmed face. The separate curve-shape handles are fit points; never expose hidden CVs as mesh vertices. | Implemented, including the component-mode UI. |
| Ctrl+1 / Ctrl+2 / Ctrl+3 — expand mode selection; Shift+1 / 2 / 3 — extend mode | **Reuse.** Apply to the same native component IDs and selection history. | Mode switching is present; all selection combinations need continued regression coverage. |
| Ctrl+Tab — selection-mode menu in the legacy keymap | **Reuse.** Show the same three NURBS component modes. | Menu support is keymap-configuration dependent; the modern default uses 1/2/3. |
| A, Alt+A, Ctrl+I — select/toggle, deselect, invert | **Reuse.** Operate on the active native component domain, not display tessellation elements. | Some native selection services exist; do not infer all variants from the shared key binding. |
| Ctrl+Numpad + / - — grow/shrink selection | **NURBS kernel.** Match Blender/BMesh region extension within the active selection domain, then apply Blender's mode flushing. Do not invent endpoint-to-curve-to-face mode transitions. | Not implemented for native topology. The current Curve binding calls legacy curve-array operators. |
| Shift+Ctrl+Numpad + / - — next/previous item | **NURBS kernel.** Match Blender's selection-history and BMesh adjacency behavior, including its tie handling. Stable source IDs are references, not an ordering rule. | Mesh operators are real Python bindings; no native NURBS adapter or Curve key binding exists. |
| L / Shift+L / Ctrl+L — linked pick / deselect linked / select linked | **NURBS kernel.** Traverse the shared source graph; retain Blender's pick, toggle, and history behavior. | Implemented for native topology and checkpointed. |
| Alt-click — edge loop; Ctrl+Alt-click — edge ring | **NURBS kernel.** A loop follows actual shared BRep edges and face adjacency. A surface's U/V isoparametric direction is not automatically a topological loop. | Not implemented as a general NURBS loop/ring selector. |
| Ctrl-click — shortest path | **NURBS kernel.** Find a path over native edge/vertex adjacency; the path may cross face patches. | Not established for native topology. |
| Shift+G — select similar menu | **Adapt.** Compare declared source properties such as curve degree, edge length, face area, or normal angle. Never compare display-mesh triangle counts as source topology. | Native criteria need to be added individually. |
| Click / box select; lasso / circle gestures | **Reuse** the selection gestures and select-operation modes, but resolve hits to source IDs. Same components selected by click, box, or gesture must feed identical later edits. | Click/box normalization is implemented for supported components; lasso/circle remain listed gates. |
| H / Shift+H / Alt+H — hide / hide unselected / reveal | **NURBS kernel.** Hide native IDs and keep hidden components out of picking and edits, while preserving Blender's visibility semantics. | Not established as complete native component visibility. |
| Ctrl+Shift+M — mirror selection | **Adapt.** Use an explicit counterpart map, or a declared geometric tolerance and plane. Do not guess correspondence from tessellation. | Not implemented as general NURBS/BRep symmetry correspondence. |

### Existing Curve-keymap conflicts to resolve by editing context

| Key in the current Curve keymap | Current Curve action | Mesh meaning / NURBS decision |
|---|---|---|
| Shift+Ctrl+R | Select a Curve row | Mesh uses offset edge-loop cut. Keep row selection only where a fit-point/control-row workflow explicitly needs it; in NURBS topology edge/face mode this key should invoke the NURBS offset-loop-cut equivalent. |
| F | Make a Curve segment | Mesh adds an edge/face from selected elements. In NURBS topology mode, route to source edge/patch creation or fill. Preserve legacy behavior for unrelated Curve editing. |
| V | Set Curve handle type | Mesh rip. NURBS fit-point handles are not Bezier handles or CVs; topology mode should use V for source-aware rip, with the old action kept for the legacy curve mode where it applies. |
| Ctrl+R | Native loop cut and slide on supported rectangular quad rings | Real shared source edges, vertices and restricted NURBS subpatches; hover/count/slide/cancel are tested. General trim cuts and offset cuts remain planned. |
| Ctrl+B / Shift+Ctrl+B | No mesh bevel binding in the legacy Curve map | Add edge/vertex NURBS bevel only with a declared fillet/offset solver and failure/tolerance behavior. |
| Shift+E | No mesh edge-crease binding in the legacy Curve map | Add only after defining NURBS continuity semantics; do not write a mesh crease scalar into rational weights. |
| Shift+Alt+S / Ctrl+Alt+Shift+S | The shared Curve transform template does not bind To Sphere or Shear | Add the mesh keys in native NURBS mode: To Sphere needs a fit rule; shear is an exact affine source transform. |
| Ctrl+E / Ctrl+F / Ctrl+V | Mesh edge/face/vertex menus are not the Curve menus | Add NURBS-context menus with source-native operations and accurate disabled states. |

## Transforms and transform-modal keys

| Mesh binding | NURBS use | Current native state |
|---|---|---|
| G / R / S and click-drag move | **Reuse** Blender's modal controls, pivots, numeric entry, confirm/cancel, and F9. Apply the result to authoritative source geometry and shared incident topology. | G/R/S are implemented for supported component modes and tested. |
| X / Y / Z axis; Shift+X/Y/Z plane; C constraint toggle | **Reuse** transform controls. The constrained source operation must preserve shared boundaries and report unsupported deformation. | Supported for current tested transforms; source-frame Normal/Gimbal behavior remains gated. |
| Shift during a transform; numeric values; Enter/LMB confirm; Esc/RMB cancel | **Reuse** unchanged. Cancellation must discard the candidate source edit atomically. | Current transform checkpoints cover modal input/cancel for the supported operations. |
| Ctrl during transform / Shift+Tab — snap controls | **Reuse** Blender's snap settings, with NURBS-native target queries. Snapping two components together does not silently weld them. | Vertex snapping to native endpoints and the user's snap-then-E failure are fixed and viewport-tested. General edge/face projection, normal alignment, and geometric R/S snapping remain gates. |
| Shift+W — bend | **Adapt.** Apply a deformation to source geometry through a defined NURBS edit rule. General bending is not an exact rational NURBS operation; require explicit fit tolerance or reject visibly. | Not implemented for the native NURBS editing path. |
| Ctrl+M — mirror selected components | **Reuse** the affine reflection mathematically, then update orientation/trim data and counterpart topology. It is exact for reflected rational source geometry. | General component mirror editing is not checkpointed as a native operator. |
| Shift+Alt+S — To Sphere | **Adapt.** Define which selected source points are projected and how adjacent NURBS shape is refit. A general result is approximate; report the error. | Not implemented. |
| Ctrl+Alt+Shift+S — shear | **Reuse** affine transformation of source points; rational weights/knots stay unchanged. Update orientations where needed. | Not checkpointed as a native shortcut operation. |
| Ctrl+A — skin resize | **NURBS-specific counterpart.** On sweep geometry that retains section/radius parameters, edit those source parameters. For ordinary NURBS faces and curves, report that this context has no skin radius to edit. | Needs a sweep/section source contract; not a generic control-point transform. |
| O / Shift+O / Alt+O — proportional toggle / falloff / connected | **Adapt.** Preserve Blender's controls and falloffs. Influence NURBS fit points through the source geometry solver; “connected” follows native topology. Preserve exact analytic circles/arcs or stop with a clear unsupported/error result. | Not implemented for native NURBS; fit-point handles and analytic edit contracts are still prerequisites. |
| Alt+S — shrink/fatten | **Adapt.** Offset along analytic surface normals. Exact only in constrained cases; general offsets need a tolerance-managed NURBS construction and explicit failure. | Not implemented for native component editing. |
| Shift+V — vertex slide | **NURBS kernel.** A BRep vertex can slide along its incident source edge while updating adjoining trims. A fit point slide is a separate shape edit. | Not implemented as a general source-topology slide. |
| Page Up/Down or wheel during proportional transform | **Reuse** once NURBS proportional editing exists; these only change influence radius. | Depends on proportional editing, which is not implemented. |
| Axis-target modal keys (X/Y/Z, Shift planes, precision Shift, snap Ctrl, confirm/cancel) | **Reuse.** Keep Blender's transform modal contract, route geometry to the source kernel. | Current native transforms support their tested subset; not every Blender transform mode has a NURBS source adapter. |

## Extrusion, creation, and topology edits

| Mesh binding | NURBS use | Current native state |
|---|---|---|
| E — extrude/move; Alt+E — extrusion menu | **NURBS kernel.** Wire curve extrusion creates a connected ruled surface. Face-region extrusion creates walls only on the region's outer boundary; internal shared edges stay shared. Selection gesture must not affect the result. | Native E, line-to-surface extrusion, shared multi-face boundary behavior, and the snap-then-extrude regression are implemented/tested. Automatic face-normal constraint and broader geometry cases remain gates. |
| Shift+D — duplicate/move | **Reuse** the copy-and-move interaction; allocate fresh topology IDs and remap internal sharing/history. Do not keep shared IDs between the source and its duplicate. | Implemented and checkpointed for supported native topology. |
| Ctrl-click cursor dupli-extrude | **Adapt.** Reuse cursor placement, but make the duplicate/extrusion topology rule explicit for endpoints, curves, and face regions. | Not established for native NURBS. |
| F — add edge/face; Alt+F — fill | **NURBS kernel.** Build actual source curves/trim boundaries and a NURBS patch. Planar and compatible ruled/bilinear cases can be exact; arbitrary boundaries need a declared fit/parameterization, measured tolerance, and visible failure. Keep corners sharp when requested. | Native fill handles constrained cases, including a bilinear patch from four straight corners. This is not full mesh Fill/Grid Fill parity. Curve mode currently binds F to `curve.make_segment`; Alt+F is not routed to a native fill menu. |
| Shift+A — add geometry menu | **Adapt.** Use familiar menu access for NURBS lines, curves, surfaces, and exact analytic primitives. Circles/arcs must remain rational analytic geometry. | Native primitive creation and migration remain gates. |
| Y — split; Alt+M — split menu | **NURBS kernel.** Disconnect the selected topology while retaining the represented shape and sharing within each side. Cutting an edge/face at a new parameter is a separate operation. | Y is implemented through shared extraction, including exact tagged entry and multiple native owners; see `native_split_tryout.md`. Alt+M menu parity and edge/face cuts remain later work. |
| V / Alt+V / Alt+D — rip region / rip with fill / rip edge | **NURBS kernel.** Duplicate the affected source boundary/edge identities and update face trims atomically. Maintain the original shape on both sides of the tear. | V is implemented/tested for manifold vertex/edge fans and wire junctions, using actual mesh references. Rip with Fill and broader fan patterns remain planned. |
| P — separate | **NURBS kernel.** Move selected source components to a new owner while preserving exact geometry and internal sharing; copy boundaries needed by both objects. | Separate Selection is implemented through shared extraction and Blender object duplication; see `native_separate_tryout.md`. By Material/By Loose Parts and mesh popup-menu parity remain planned. |
| M — merge menu | **Adapt.** Weld only with an explicit merge action and compatibility checks. Compare source endpoints/curves or use a declared tolerance. Do not infer a weld from snapping or display coincidence. | Center/Cursor/First/Last/Collapse and By Distance are implemented/tested. Curved trim collapse and automatic merge remain limited/planned. |
| X / Delete — delete menu | **NURBS kernel.** Delete selected vertices/edges/faces with the selected mesh-style mode, preserving valid remaining source topology. | Native deletion is implemented/tested, including the reported delete-top-face then continue-edit case. |
| Ctrl+X / Ctrl+Delete — dissolve | **Adapt.** Remove topology while preserving the same support when possible. If dissolving requires a changed fit, report its tolerance and failure. | Not implemented as general dissolve. |
| Ctrl+R — loop cut; Shift+Ctrl+R — offset loop cut | **NURBS kernel.** Make real, selectable BRep trim partitions along the existing support. Knot insertion is shape-preserving control-net refinement and is not a loop cut. Isoparametric cuts can be exact; general cuts need an explicit intersection/curve solver. | Ctrl+R is implemented/tested for rectangular quad rings, with independent hover preview, count, source-parameter slide, undo/redo and continued editing. Offset/general trim cuts and degenerate endpoints remain planned. |
| I — inset faces | **Adapt.** Planar/compatible curve offsets can be exact. Curved-face inset must declare its distance metric (parameter-space, 3D offset or geodesic-like); general offsets need tolerance-managed intersections and trim repair. | No native inset operator/kernel. This shortcut was omitted from the earlier audit. |
| Subdivide (menu/search) | **NURBS kernel.** Share edge splits across every incident face; partition selected rectangular supports into independently editable subpatches. | Wires, full rectangular faces, one side and opposite sides are implemented. Adjacent two-side and three-side mesh patterns need a diagonal partition service and reject before mutation. |
| J — connect vertex path | **NURBS kernel.** Create a source curve path on the trimmed face and partition its trims without changing the support where possible. | Not implemented. |
| Ctrl+B / Shift+Ctrl+B — edge/vertex bevel | **Adapt.** Reuse the modal tool. Fillet/chamfer needs NURBS offset/intersection and corner joins; exact only for constrained cases, otherwise tolerance-managed with clear failure. | Not implemented; fillet/bevel is a later kernel family. |
| K / Shift+K — knife / selected or cut-through variants | **NURBS kernel.** Intersect source curves/surfaces, create trim partitions, and honor visibility/selection options. Do not cut the viewport tessellation. | Not implemented. |
| Ctrl+F / Ctrl+E / Ctrl+V — face / edge / vertex menus | **Reuse** the menu access, but populate it with available NURBS-native operations and disable unsupported entries accurately. | Menu crosswalk exists; full native menu/operator parity is incomplete. |
| Alt+R — spin (legacy keymap) | **Adapt.** Preserve Blender's finite step count, angle, axis, cursor center, duplicate/extrude mode, selection and full-turn identity weld. Transform rational copies exactly and connect compatible stations with degree-1 ruled patches. A continuous NURBS revolution is a separate operation. | Finite native topology Spin is not implemented. Legacy Curve Spin edits a control net and is not this feature. |

## Mesh operations with no one-to-one source geometry

| Mesh binding | NURBS use | Current native state |
|---|---|---|
| Ctrl+T / Shift+Ctrl+T — triangulate; Alt+J — tris to quads | **NURBS-specific counterpart.** Partition a trimmed NURBS face into triangular source regions, or merge compatible neighboring triangles back to a four-sided trim region, while retaining the exact underlying support. Never operate on display tessellation. | Not implemented; add to source trim-topology operations. |
| Shift+E — edge crease | **Adapt.** Offer a NURBS continuity/crease command based on knot multiplicity and adjacent support continuity. A mesh crease float is not a NURBS knot or rational weight. | No native continuity operation. |
| Ctrl+Shift+N / Shift+N — set/recalculate face orientation (legacy Ctrl+N) | **Adapt.** Reverse oriented face/trim data or recalculate shell orientation. Surface normals come from source derivatives; do not alter rational geometry to fake normals. | Source normals and face orientation have kernel work, but complete operator parity is not established. |
| Alt+N — normals menu; Alt+L — point normals | **Adapt.** Keep shading overrides separate from analytic source normals. | Not established as a complete native source operation. |
| Ctrl+G / Ctrl+Alt+G — vertex groups / remove from group | **Adapt.** Store deform weights on explicit fit-point or BRep-component IDs. Keep them separate from rational basis weights. | Not integrated as native component domains. |
| Ctrl+H — hooks menu | **Adapt.** Bind hooks to stable source IDs; update affected supports and trims through the source kernel. | Not integrated for native topology. |
| U — UV map menu | **Adapt.** Texture UVs are separate from NURBS surface U/V and trim p-curves. Provide unwrap/seam tools only after a persistent texture-UV domain exists. | Not implemented for native NURBS. |
| Alt+P — poke (legacy keymap) | **Adapt.** Could create a new patch partition point with an explicit surface construction. It is not mesh poke on tessellation triangles. | No direct counterpart implemented. |
| Ctrl+Shift+Alt+M — select non-manifold (legacy keymap) | **NURBS kernel.** Select source edges by BRep incidence/boundary status. | Incidence is represented in native topology; shortcut/operator parity is not established. |
| Ctrl+Shift+Alt+F — linked flat faces (legacy keymap) | **Adapt.** Traverse adjacent source faces using analytic normal-angle criteria. Do not inspect display triangles as the source. | Not established. |
| Shift+Alt+F — beautify fill (legacy keymap) | **Adapt.** Repartition trims on the same support if useful; never reposition the source surface just to improve display triangulation. | No direct source operation established. |
| Ctrl+0…5 — subdivision level (legacy keymap) | **No direct NURBS topology counterpart.** In Blender this calls `object.subdivision_set` to create/set a Subdivision Surface modifier; it does not refine edit topology. A NURBS preview-density preset is a separate display-only feature, not knot refinement. | Native NURBS evaluation currently rejects modifiers; preview-density key behavior is not implemented. |

## Generic keys that still apply

* **F3** operator search and **Q** context/quick menu should expose the
  NURBS-native versions by name and respect unavailable geometry cases.
* **F9** should edit the last NURBS operation's real parameters and show its
  fitting/intersection tolerance when applicable.
* **Undo/redo** must restore the authoritative NURBS source, identities,
  topology, and selection as one transaction.
* **Alt+Z** X-Ray and the viewport select tools are generic presentation and
  picking controls. Their hits must still resolve back to source component IDs.

## Fit-point decisions before implementing the normal curve handles

The user has specified that curve editing should use fit points, not CVs, and
that exact rational circles and arcs must be preserved. The current numerical
foundation supports explicit, checked free-form fitting, but the two interactive
rules below are still open and materially affect every fit-point drag shortcut:

1. When one free-form fit point is dragged, should the other fit points stay
   fixed, or should nearby points move with a smooth falloff?
2. When a fit point on an exact circle is dragged, should the other defining
   fit points stay fixed while the center/radius can change, or should the
   center stay fixed while the radius changes?

Until those rules are chosen, do not activate a generic mesh-vertex transform
that silently moves hidden NURBS coefficients or converts an analytic circle
to a merely fitted curve.

## Implementation order implied by this audit

1. Keep the working selection/transform/duplicate/extrude/delete/snap behavior
   stable, with identical source selection regardless of click or box gesture.
2. Complete native selection growth, visibility, history-based next/previous,
   path/loop selection and source topology operations in the shared kernel.
3. Extend the implemented split/separate/merge/Ctrl+R services; add general
   dissolve/trim cuts with shared identities and transaction-level validation.
4. Establish fit-point identity/drag rules and analytic-primitive constraints
   before enabling the normal fit-point editing workflow.
5. Add tolerance-managed construction families (knife/intersection, offsets,
   bevel/fillet) and persistent non-geometric domains (UVs, attributes,
   weights, shading) on their own explicit contracts.

This order is a dependency suggestion, not a commitment to copy mesh
algorithms. The intended experience is to retain the familiar shortcuts and
provide a useful NURBS counterpart wherever the action applies. Each operation
must be described, validated, undoable, and surfaced to Python without
substituting a display mesh for the source. Sweep-specific radius editing,
source triangulation/recombination, and progressive refinement belong with
their geometry families; they do not need to block the shared topology work.
