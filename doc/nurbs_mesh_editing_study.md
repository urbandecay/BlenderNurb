# Blender mesh editing with NURBS: capability and reuse study

## Goal and status

The goal is Blender mesh editing behavior applied to editable NURBS geometry:
the same vertex/edge/face concepts, tools, shortcuts and manipulation experience,
with curved geometry that remains parametric and editable.

classiCAD is a general proof of concept. Its current data structures, bugs,
tool limitations and shortcuts are not requirements. Existing algorithms are
reuse candidates, not an architecture to reproduce unchanged.

This is a source study of Blender Fork at e694c2908bb and the current local
classiCAD checkout. Three Luna agents inspected operator coverage, geometry
and topology, and interaction/adjacent systems; the parent reconciled their
findings. No modeling implementation, build, runtime experiment or benchmark
was performed. The classifications are engineering assessments, not completed
compatibility tests or formal numerical proofs.

## Executive finding

Blender's interaction framework is a strong reuse opportunity. Its mesh
geometry implementations are not interchangeable with NURBS implementations.
The missing foundation is an editable shared topology graph attached to curve
and surface geometry, including trimmed boundaries. Existing NURBS evaluation
and control-point editing alone do not provide that graph.

Broad tool parity is a realistic product direction. A promise that every mesh
tool will produce the same result on arbitrary curved NURBS, exactly and
without approximation, is not a valid engineering contract. Some operations
have multiple useful NURBS interpretations; others require tolerance-managed
intersections, offsets or fitting. Those tools can still exist in the same UI.
Their geometric contract needs to be explicit.

## Requirements clarified in follow-up

- The target is the full Blender-like editing workflow, delivered in stages;
  a first vertical slice is a checkpoint, not the final feature scope.
- Blender supplies the interaction and operation-behavior baseline. Geometry
  operations must work on authoritative NURBS/topology, not commit edits to
  the viewport tessellation and treat that mesh as the model.
- Preserve exact circular arcs and joined spline components through ordinary
  edits. Keep their control points, rational weights, degrees/orders, knot
  vectors and parameter domains unless an explicit operation changes them.
  Rhino-style joining must retain component geometry and continuity instead
  of silently fitting a replacement curve. Knot vectors are essential to this
  preservation, although endpoint/tangent/curvature conditions also determine
  smoothness across a join.
- NURBS curves use fit points as the normal user-facing curve-editing handles,
  not directly edited control vertices. Fit points lie on the represented
  curve; curve construction/editing solves the internal NURBS control vertices
  from those points and the curve's degree, parameterization, knot, weight,
  and continuity constraints. Keep exact rational constructions such as
  circles and arcs exact rather than replacing them with sampled or approximate
  fits. Control vertices may remain available in a separate advanced control-
  net view, but they are not the default curve workflow.
- Ctrl+R should keep Blender's loop-cut interaction and traverse actual face
  topology. On compatible NURBS supports it should create a selectable loop
  and split the face exactly without changing the visible shape. Knot insertion
  adds shape-preserving control rows but does not by itself create that face
  topology. On a 1D curve, knot insertion is the analogous way to add control;
  it is a distinct operation from a face loop cut.
- Bevel should retain Blender's familiar selection and controls. Its NURBS
  geometry path must trim supports and construct bevel surfaces; Blender's
  BMesh bevel implementation is a behavioral reference, not a directly
  reusable NURBS kernel. Exact simple cases and tolerance-managed complex
  cases need separate contracts.
- Viewport and render evaluation must derive display geometry from the same
  authoritative NURBS model. Trims, face materials, UVs, normals and source
  identity must survive that conversion so editing and rendering agree.
- Modifier, vertex-group, attribute, UV, shape-key, sculpt/paint,
  Geometry Nodes and other adjacent workflows are part of the integration
  scope. Each needs an explicit NURBS-native, derived-mesh, or unsupported
  behavior. Blender vertex-group weights are separate data from rational
  NURBS weights.
- Blender's architecture and code conventions are hard requirements. The
  implementation must use Blender's normal ownership, dependency direction,
  data lifecycle, evaluation, undo, RNA and testing patterns. Geometry logic
  belongs in reusable kernel services, with editors and operators calling
  those services; UI code must not become a second geometry engine. The exact
  data owner remains an architecture decision, but a parallel Qt/classiCAD
  model or monolithic operator implementation is out of scope.

No NURBS editing implementation has been started. These are the product
requirements recorded from discussion; continuity level and approximation
tolerance remain open decisions.

## Deliverables and coverage

- [Operator inventory](nurbs_mesh_study/operator_inventory.csv): individual
  static registrations and source locations, including mesh macros and
  related editor/kernel domains.
- [Operator parity map](nurbs_mesh_study/operator_parity_map.csv) with
  [scope notes](nurbs_mesh_study/operator_parity_scope.md): per-registration
  behavior, proposed NURBS result, reusable Blender interaction, and blockers
  across mesh, UV, Python wrapper, and transform registrations.
- [Inventory scope](nurbs_mesh_study/inventory_scope.md): extraction coverage,
  counts, conditionals and limits on interpretation of that inventory.
- [Geometry matrix](nurbs_mesh_study/geometry_capability_matrix.md): operation
  families, topology work, exact constructions, and approximation decisions.
- [Geometry semantics review](nurbs_mesh_study/geometry_semantics_review.md):
  expanded operation-family coverage, recommended NURBS counterparts, and
  explicit Ctrl+R versus knot-refinement behavior.
- [Adjacent mesh capabilities](nurbs_mesh_study/adjacent_mesh_capabilities.md):
  object operations, modifier roster, sculpt/paint/remesh, UVs, attributes,
  animation, Geometry Nodes and evaluation/export.
- [Mesh Edit menu crosswalk](nurbs_mesh_study/edit_mesh_menu_crosswalk.md):
  the visible Mesh menu and Vertex, Edge, and Face context menus, including
  their fixed built-in items and submenus, plus shared transform orientations,
  pivot points, snapping settings, and proportional-edit falloff, mapped back
  to the operator parity map or adjacent systems review. It also maps the
  viewport's object-type Selectability & Visibility controls and Viewport
  Overlays, mesh Edit Mode Tool Options, and mesh Edit Mode overlays to NURBS
  objects, topology, and derived display geometry.

The primary Edit Mode roster in editors/mesh/mesh_ops.cc contains 148 direct
mesh operator registrations and 16 operator macros. These are not 164 distinct
geometry algorithms: many macros combine one geometry operation with a shared
transform. The built-in Python mesh namespace adds Torus and select-next/prev;
Python view3d wrappers add extrusion routing. UV and transform operators are
additional domains. Toolbar/menu property variants and compile-time conditions
matter; merely registering a name does not prove runtime availability.

The combined inventory contains 358 rows: 148 direct mesh operators, 16 mesh
macros, eight Python operators/helpers, 83 BMesh kernel definitions, 49 direct
UV operators and one UV macro, 19 transform-mode operators, five explicit
transform/support operators, two mesh-edit transform helpers, seven View3D
snap actions, nine vertex-group weight actions used by the Mesh menu, five
vertex-group management actions, and six hook actions used by the Vertex
context menu.
These sets overlap in function and must not be added up as independent
features or implementation tasks. The adjacent review also enumerates all 58
source modifier-registry entries, including None.

The main audit is exhaustive over the enumerated source registration sets,
with family-level semantic review. It is not an assertion that every RNA
property combination, brush preset, Geometry Node, external add-on or modal
event sequence has been executed. Adjacent capabilities are explicitly listed
rather than silently considered implemented or irrelevant.

## What can actually be shared

| Area | Existing code to retain/reuse | Work still required |
|---|---|---|
| Viewport navigation, windows, workspaces, context menus | Blender host UI, event routing, preferences and navigation | Add NURBS object/edit contexts and tool availability; preserve the user's navigation preferences. |
| Operator lifecycle | Invoke/modal/execute/cancel, RNA properties, redo panel, reports, keymaps, gizmos | New geometry adapters, atomic operation state and valid result/selection mapping. Existing mesh-only polls need alternatives. |
| G/R/S, constraints, orientations, pivots, numeric input | Shared transform behavior, gizmo logic, mouse mapping, unit UI, all five pivot modes, and Global/Local/View/Cursor/Gimbal orientation modes | A NURBS-aware `Normal` frame; exact or stable CV/BRep pivot centers; selected CAD element adapters and double-precision handling. |
| Object selection, collections, Outliner and visibility | Native scene-object identity, collection organization, active object, and per-viewport object-type filters | Map NURBS curves/surfaces to their logical types; keep component identity/edit selection distinct from object rows; synchronize visibility and selectability across drawing, picking, and transform-snap targets. |
| Component selection | Screen gestures, picking framework, selection conventions and some traversal logic | Topological V/E/F graph, control-net selection, curved-edge picking, trim-aware faces, xray/occlusion and stable IDs. |
| Adjacency and region algorithms | BMesh traversal and combinatorial patterns where independent of geometry | Replace straight-edge/planar-face predicates, propagate p-curves and trims, support holes, reconcile exact geometry. |
| Display and rendering | GPU infrastructure, materials, draw manager, evaluated geometry pipeline, viewport overlays | Revision-aware NURBS tessellation, source-ID provenance, analytic normals, shared-edge crack prevention, trim-aware picking, separate control-net and evaluated-wire displays, and NURBS-aware orientation/statistics overlays. |
| Undo, file lifecycle and depsgraph | Host undo/redo framework, Main/ID ownership, library/copy/versioning conventions | Exact model serialization, edit-mode snapshots/deltas, owned geometry and references, evaluation/cache invalidation. |
| Attributes, materials and UV UI | Generic storage/API patterns, materials and much of the editor UI | Domains/interpolation on topology, CVs and surfaces; texture UVs must remain distinct from geometric UV domains. |
| Modifiers/Geometry Nodes | Stack/graph presentation and evaluated-vs-original separation | Explicit per-operation CAD support, numerical policy or evaluated mesh output; no automatic exact NURBS compatibility. |

These are reuse boundaries, not percentages. A mostly reusable UI still needs
geometry adapters and new context handling. No credible effort percentage
follows from counting source lines or operator names.

### Selection parity

Once actual CAD topology exists, many mesh selection definitions remain useful
unchanged at the conceptual level. Face side counts, vertex valence/poles,
wire/boundary/nonmanifold edges, linked components, boundary loops, more/less,
checker/nth selections and shortest graph paths are meaningful on curved
boundary representations too. They should not be replaced wholesale by CV
rows or removed merely because the geometry is NURBS.

Loops/rings should follow the same topological continuation rules as meshes
where applicable, with the same branch/termination ambiguity at poles and
non-quad faces. Surface U/V rows and cyclic CV grids are separate control-net
selection tools. Geometric criteria such as sharp angle, coplanar/flat linked,
similar area/length and mirror matching need curve/surface-aware measurements
and tolerances rather than polygon normals and straight chord lengths.

Preserve Blender's selection history, active element, deselection rules,
multi-object behavior, hide/reveal, local view, box/lasso/circle selection,
xray and visibility. Picking can use tessellation for coarse hits, then map
back to source face/edge/vertex IDs and refine where necessary. Tessellation
resolution must not change which logical component is selected.

### Transform and deformation parity

| Tool family | Reusable behavior and NURBS interpretation |
|---|---|
| Translate, rotate, scale, trackball, shear, mirror | Reuse native modal controls and affine math. Complete support transforms act on CVs without altering weights/knots; partial topology transforms need incident edge/surface reconstruction. Negative/singular scale requires explicit orientation/degeneracy handling. |
| Bend, nonlinear deform | Reuse input/gizmos, but pointwise nonlinear deformation of an evaluated NURBS is generally different from applying the same map to its CVs. Offer a defined cage deformation or tolerance-managed fitted surface. |
| To Sphere/circularize | Projecting selected topology points or CVs is not the same as constructing an exact sphere/circle. Preserve the mesh-style operation as specified; provide analytic construction separately where desired. |
| Push/Pull | Reuse radial displacement relative to pivot for selected edit targets; define degenerate pivot cases and connected support reconstruction. |
| Edge/vertex slide and even loop spacing | Use actual topology to select continuation. Curved edge slide needs parameter versus arc-length behavior and trim/support updates; keep CV-row slide a distinct operation. |
| Shrink/Fatten | Move geometric targets along defined normals; varying normal offset generally needs fitting or a control-cage deformation contract. This is distinct from uniform translational extrusion and curve radius scaling. |
| Skin Resize, curve radius/tilt | Data-property edits with influence falloff; they do not automatically change exact CAD face geometry. Store the appropriate radius/profile/orientation attribute or declare the construction it drives. |
| Edge/vertex crease and bevel weight | Attribute edits can retain native UI. CAD fillet/continuity constraints and subdivision crease weights are different meanings; assign explicit domains and consumers. |
| Rotate/custom normals | Changes shading metadata where supported, independently of analytic surface derivative normals and geometric continuity. |
| Proportional editing, connected-only, projected falloff | Reuse radius controls and Blender's falloff weights. Use world-space distance for CVs, U/V control-net or BRep graph distance for Connected Only, and screen-space distance for Projected from View. Moving CVs changes the represented surface through its basis functions, so the visible surface does not follow the falloff point-for-point. |
| Orientation tools, transform snapping, pivots and cursor | Reuse shared settings and controls; add exact NURBS vertex, trimmed-edge, face-ray, nearest-surface, and solid-volume snap providers with active component frames; preserve double data at numeric/transform boundaries. Extend Blender's existing transform and viewport snapping infrastructure rather than adding a second snap system. |

Sequence-slide is part of the generic transform registry but belongs to the
sequencer rather than mesh geometry. Its inventory entry is a coverage boundary,
not a proposed CAD modeling tool.

## The central model distinction

A topological vertex is a shared geometric corner. A NURBS CV is a coefficient
of the curve/surface; it usually does not lie on the visible geometry. A curve
fit point is an editable point constrained to lie on its curve; it is not
automatically a topological corner. Users must be able to select and edit
actual corners, edges and faces as requested, and edit curves through fit
points by default. An optional control-net view can expose CVs without
pretending that a CV is a fit point or a topological corner.

The fit-point workflow needs an explicit interpolation contract. Its solver
must define point parameterization, degree, end conditions, knot generation,
and rational constraints, then regenerate valid NURBS control vertices while
preserving the requested fit points. Operations that change fit points must
also define how they preserve or deliberately update the curve's shape,
parameter domain, and exact analytic constructions. Do not implement fit-point
editing as moving the nearest existing CV or as fitting sampled display
vertices.

Each topological edge needs geometry and a parameter interval. Each face
needs a support surface, oriented boundary loops, and each edge's curve in
that surface's UV domain (a p-curve). Neighboring faces share the topological
edge while retaining their own surface parameterizations. Stable IDs and
old-to-new mappings must survive splits, merges and undo.

This is a boundary-representation model with a Blender-style editing API.
The generic topology architecture is consistent with established CAD systems'
separation of edge curves, per-face p-curves and tolerances; see the primary
[OCCT topology overview](https://sso.opencascade.com/doc/occt-6.8.0/overview/html/technical_overview.html).
That reference supports the model distinction, not a requirement to adopt OCCT.

Three types of connectivity must remain distinct:

1. Objects sharing a position but remaining independent.
2. Topological entities deliberately welded/shared at G0 positional continuity.
3. A tangent/curvature relationship (G1/G2), requiring additional constraints.

Matching CV coordinates is not a general continuity solver. Conversely,
keeping arbitrary curved surfaces connected after moving a common corner is
not defined by moving that corner alone. An edge/surface deformation policy
must determine how nearby geometry follows.

Open wire edges, open sheets and disconnected pieces must be legal editing
states. Blender also permits nonmanifold and self-intersecting states. A CAD
editor that rejects every non-solid edit would fail the requested UX. Track
wire/sheet/shell validity separately from eligibility for closed-solid tools;
decide which invalid/intersecting states are allowed rather than accidentally
enforcing closed-solid validation everywhere.

## Important differences that need designed behavior

| Familiar mesh action | Candidate NURBS behavior | Decision needed |
|---|---|---|
| Move vertex/edge/face | Move topological geometry and reconcile incident supports; move selected whole supports exactly where appropriate | Local deformation scope, edge anchoring, neighboring-face reconstruction and continuity preservation. |
| Loop Cut / Ctrl+R | Traverse actual face topology, create a selectable edge loop, and split compatible NURBS supports exactly while retaining the shape | Define loop traversal and sliding across mixed supports; distinguish the face-topology cut from shape-preserving knot/CV refinement. |
| Subdivide | Partition face regions, insert knots, or refine the modeling cage | These are different operations; Blender's cuts/smoothing options need distinct mappings. |
| Triangulate/Poke | Partition a curved face's UV region while keeping the same support | Curved triangular faces versus intentionally faceted output. A triangular trim is still a curved face. |
| Dissolve/Tris to Quads | Remove trims between same-support regions exactly; otherwise preserve a compound or construct a new support | Whether a face may contain several support patches, or whether merging implies a fit. |
| Extrude region/individual | Translated exact caps and ruled boundary walls; region topology versus per-face duplication | Shared displacement versus varying normal displacement; curved boundaries, overlaps and manifold repair. |
| Bevel/Inset/Solidify | Exact planar/analytic cases; tolerance-managed curved blends and offsets | Radius/thickness meaning, corner treatment, limits and numerical policy. |
| Merge/Auto Merge | Weld topology with reconciled edge geometry and trims | Distance threshold, active/last/center position, self-intersections and whether smoothness is requested. |
| Smooth/Proportional editing | Shared interaction with CV-, topology- or surface-distance influence | Which geometry is influenced and how connected curved supports deform. |
| Normals and shading | Exact derivative normals plus separate render normal/sharpness metadata | Reversing orientation changes topology meaning; smooth shading does not establish geometric G1 continuity. |
| UV editing and texture unwrap | Reuse Blender's UV editor interaction and texture-map operations after adding a NURBS/BRep texture-map layer | Blender's native unwrap is mesh-only. Store texture coordinates/mappings separately from NURBS surface parameters and trim p-curves; distinguish texture unwrap from flattening geometry for fabrication. |

No ordinary mesh tool should silently edit the display tessellation and call
that an exact CAD operation. A display mesh may seed a solver or fitting
algorithm. That algorithm still needs a defined result in authoritative CAD
geometry and validation against its error target.

### Texture UV mapping and NURBS unwrapping

Blender's standard UV editing and unwrap path is limited to mesh edit data:
`ED_uvedit_test` in `source/blender/editors/uvedit/uvedit_ops.cc` rejects
objects that are not meshes. The UV manual likewise describes UVs on mesh
faces. That is a real gap for native legacy NURBS curves/surfaces, but it is
not a gap in the geometric NURBS model itself. Each NURBS surface already has
intrinsic `(u, v)` parameters used for evaluation and trim curves.

Keep three concepts separate:

1. **Geometric surface parameters** `(u, v)`: locate points on the exact
   NURBS surface and express trim p-curves. Changing these can change the
   geometry representation or trim; they are not a texture editor's UVs.
2. **Texture coordinates** `(s, t)`: say which point of an image/material is
   sampled at each surface point. An initial surface-mapping mode can map the
   normalized geometric parameter domain into texture space without flattening
   or changing the NURBS.
3. **Physical flattening**: creates a planar pattern for fabrication. Exact
   flattening is available only for developable cases; flattening a general
   doubly curved surface needs a distortion/approximation contract. Keep this
   separate from texture UV unwrap.

For the BlenderNurb fork, native NURBS UV editing is a worthwhile feature if
the goal includes applying image textures, painting materials, decals, or
rendering textured NURBS surfaces. Blender's current UV editor/unwrap
operators are mesh-only: `ED_uvedit_test` rejects objects whose type is not
`OB_MESH`, and the UV algorithms operate on BMesh faces/corners. The legacy
`Nurb` data stores control points and knot vectors, while `Curve` stores
materials and texture-space bounds; it has no mesh-loop UV layer for a native
NURBS surface. Simply letting the mesh UV operator accept `OB_SURF` or
converting the surface to a temporary mesh would not provide persistent,
editable NURBS texture mapping.

Implement the feature inside Blender's normal Curve/Surface data, evaluation,
RNA, draw-cache, UV editor, and dependency-graph paths. Prioritize a native
texture mapping channel and rendering support before the full unwrap editor;
the latter depends on a stable surface mapping and clear patch/seam identity.

Recommended implementation order:

1. Add a texture-mapping channel to native NURBS surface data and connect it
   to Blender's image-texture/material and draw/evaluation paths. Start with
   surface mapping from normalized `(u, v)`, then planar/box projections and
   editable mapping transforms. Define periodic-surface seam behavior.
2. Give each NURBS surface patch a stable chart identity. Add seam selection
   between joined patches and on periodic boundaries, then extend the UV
   Editor to display and edit those charts. Preserve intentional texture
   seams without changing surface knots, trims, or shared geometry.
3. Reuse Blender's unwrap interaction and solver where possible. An adaptive
   evaluated mesh can provide samples, but the result must map back to
   persistent source-surface parameter coordinates and be evaluated when
   drawing/rendering. Never save UVs only on a disposable tessellation mesh.
4. If sheet-metal/pattern flattening is in scope, add separate develop,
   unroll, and approximate-squish tools with strain/error reporting.

Rhino is a useful behavior reference: its texture mapping can use surface
parameters by default, and its Unwrap workflow supports surfaces, polysurfaces,
and extrusions by selecting seams and editing flattened texture meshes. Its
physical `UnrollSrf` workflow is separate. See the [Rhino texture mapping and unwrap
reference](https://docs.mcneel.com/rhino/8/help/en-us/properties/texturemapping.htm)
and [Rhino UnrollSrf reference](https://docs.mcneel.com/rhino/8/help/en-us/commands/unrollsrf.htm).

## Exactness and practical limits

Use per-operation contracts rather than a global exact/unsupported switch:

- **Support preserving:** duplicate/separate; split edge curves; partition UV
  trims; insert knots; remove a seam on a compatible shared support. Original
  geometry can remain unchanged despite topology changes.
- **Exact construction:** affine transforms of complete supports; straight
  extrusion/ruled surfaces; rational circular arcs and suitable revolutions;
  specified planar constructions. Validate connectivity and degeneracy.
- **Tolerance managed:** general intersections, arbitrary curved offsets,
  many surface blends/projections, and fitted deformation. Preserve original
  supports where possible and limit approximation to constructed curves or
  new supports. CAD Boolean systems explicitly use tolerances; see
  [OCCT Boolean operations](https://sso.opencascade.com/doc/occt-6.8.0/overview/html/occt_user_guides__boolean_operations.html).
- **Representation dependent:** mesh subdivision, remesh, smooth primitive
  conversion, edge collapse and reconstruction. Define the intended geometry
  before choosing an implementation.

Degree-one NURBS can represent straight edges and planar polygonal faces, but
an arbitrary bilinear quad is not the same shape as two planar mesh triangles.
For strict faceted parity, retain the source triangle supports or a compound
face representation; do not substitute a different interior unnoticed.

A continuous helix is an example where finite ordinary rational NURBS does
not supply the ideal analytic geometry. McNeel describes its generated helix
as an approximation in this [developer explanation](https://discourse.mcneel.com/t/exact-curve-modeling-with-nurbs-quadrant-helix-problem-with-curvature-and-torsion-values/127981/2).
Blender's finite stepped Spin/Screw can instead be reproduced as discrete
geometry. Choosing a smooth continuous sweep is a different construction.

Numerical work must report the intended tolerance and distinguish measured
sample deviation from a certified bound. Failures/uncertain results must not
silently overwrite the source. Neither double storage nor rational weights
alone guarantee robust intersections, watertightness or CAD accuracy.

## Native implementation options

These are two separate decisions: where data lives in Blender, and which
geometry/topology services operate on it. A new ID does not solve geometry;
extending an existing ID does not prevent adding a general topology service.

| Option | Useful reuse | Principal issue |
|---|---|---|
| Extend legacy Curve/Nurb | Existing rational tensor patches, edit tools and object handling | Float data, knot-regeneration paths, absent shared CAD topology/trims; adding CAD faces changes the schema and many callers. |
| Extend newer CurvesGeometry | Curve attributes, NURBS evaluation/custom knots, modern caches | One-parameter float curve model; tensor surfaces and shell topology are additional capabilities. |
| Dedicated CAD data under native Blender objects | Clear topology/geometry ownership, precise types and persistence | More host integration: object type/ID or native payload, RNA, depsgraph, transforms, undo, draw and conversions. |
| CAD-aware BMesh-like graph | Familiar incidence/operator API and potential combinatorial reuse | Exact curves/supports, p-curves, holes, stable IDs and precision still need design; unmodified BMesh geometry routines are unsafe for general curved faces. |

## Blender architecture contract

The data owner can be selected after the storage spike, but the dependency
direction is fixed. Keep responsibilities in Blender's existing layers:

| Layer | Responsibility |
|---|---|
| DNA/ID ownership and file lifecycle | Persist authoritative NURBS geometry and topology; follow Blender copy/free/read/write/versioning and library-link rules. No editor-owned model state. |
| BKE geometry/topology services | Validate and edit curves, surfaces, trims and incidence. Expose reusable operations with stable old-to-new ID maps and atomic failure. No dependency on editor/UI code. |
| RNA and ED/WM operators | Expose properties, polls, invoke/modal/execute/cancel, selection updates, reports and undo integration. Operators orchestrate BKE calls rather than embedding geometry kernels. |
| Depsgraph and evaluated geometry | Produce revision-aware derived tessellation and modifier output. Preserve source NURBS as authoritative; invalidate only affected caches. |
| Drawing, picking and render integration | Consume evaluated data with source identity, trims, materials, attributes and analytic normals. Never commit tessellation edits back as exact NURBS. |
| Tests and validation | Test BKE geometry independently, then operator/save/undo/evaluation integration using Blender's established test organization. |

Match nearby Blender code for naming, formatting, ownership, allocation,
container use, error reporting and test placement. Do not introduce a parallel
Qt/classiCAD application layer, a second geometry model, or large operator
files that own unrelated kernels. A small architecture/storage integration
spike must prove these boundaries before broad feature ports begin.

Existing BMesh has float vertex positions and polygon face loops; hole-loop
support is disabled in this checkout (bmesh_class.hh:24,97,277 onward).
Custom metadata alone does not make geometric kernels curve-aware. Audit
each candidate helper for straight-edge assumptions, normal/area calculations,
triangulation assumptions, data propagation and lifetime before reusing it.

Transform reuse also has a precision boundary: TransDataBasic.loc/iloc/center
are float in transform.hh:383 onward, and NumInput.val is float in
ED_numinput.hh:36. Preserve double model data, use original-geometry plus
transformation deltas, and investigate double numeric/placement paths; simply
copying double CV arrays through native float transform locations loses data.

For general Boolean, blend, offset and surface-intersection support, compare
a mature BRep kernel integration against implementing the numerical services
locally. classiCAD's openNURBS usage and prototype algorithms do not establish
that those general modeling services already exist. A kernel evaluation must
cover open/nonmanifold editing, geometric history maps, tolerance/units,
periodic/singular surfaces, licensing/distribution, dependency/build cost and
native Blender integration. No dependency is selected by this document.

## Recommended implementation strategy

1. **Lock the behavior matrix and precision policy.** Keep Blender mesh UX as
   the baseline. List every operator family with its topology target, geometry
   result, exact/fit policy, failure behavior, attribute propagation and
   resulting selection. Do not duplicate prototype bugs or arbitrary limits.
2. **Prove storage and topology together.** Small throwaway integration
   exercises compare extending legacy NURBS with a dedicated CAD payload.
   Exercise a curved face with a hole and shared edge, not just a circle.
   Verify native save/reopen, picking, one topology split and one native Grab
   operation without corrupting support data. Select architecture from evidence
   and document its Blender-style data ownership, module boundaries, lifecycle,
   evaluation, undo and test integration before production feature work.
3. **Establish complete Edit Mode infrastructure.** Actual V/E/F selection,
   optional CV cages, multi-object state, active-element history, snapping,
   G/R/S adapters, cancel/redo/undo, source-ID mapping and cache invalidation.
   Open wires/sheets and mixed-degree curved faces belong in this milestone.
4. **Implement foundational topology operations.** Create, duplicate,
   delete, separate, split/rip, weld, support-preserving cuts/partitions,
   component selection propagation, and region/individual extrusions. Reuse
   native command choreography while keeping geometry-aware commits.
5. **Complete construction and numerical operations.** Fill/bridge,
   loop cuts/slides, inset/bevel/solidify, general knife/intersection/Boolean,
   surface fitting, constraints and continuity. Mature-kernel evaluation must
   precede building a whole numerical CAD kernel by accident.
6. **Complete surrounding capabilities explicitly.** UVs, materials/attributes,
   modifiers, vertex groups, sculpt/paint/remesh, Geometry Nodes and export each
   need their own precise capability policy. They remain part of the full
   target; implement and validate them in stages alongside Edit Mode parity.

This is a general curved-topology editor project, not just a port of classiCAD
or a replacement of Blender's NURBS evaluator. classiCAD can donate verified
constructors, evaluators, exact split operations and useful interaction ideas.
It must not be copied wholesale or used as the definition of correctness.

## Acceptance and test design for implementation

For every operation, check input state, preview, confirm, cancel, redo-last,
undo/redo, resulting selection, attribute propagation, file save/reopen and
downstream evaluation. Compare against native mesh behavior on planar/faceted
fixtures, and against NURBS contracts on curved fixtures. Do not claim curved
parity merely because a cube works.

Required fixture families: open wire, isolated point, open sheet, closed solid,
holes, loose/disconnected pieces, nonmanifold edge, high-valence vertex,
periodic seams, sphere poles, mixed degrees/weights/knots, tiny spans, nearly
coincident boundaries, self-intersections, nonuniform/negative object scale,
large coordinates/small dimensions, linked data and multi-object edit.

Geometry checks: support/edge/p-curve agreement, valid parameter ranges,
appropriate knot/domain preservation, orientation, intended shell validity,
continuity claims, unchanged untouched geometry and documented fit error.
Topology checks: incidence, holes, stable identity maps and no selection jump.
Interaction checks: native/keymap conventions, numerical/axis constraints,
xray/occlusion, active pivots, snapping and history. Performance checks:
bounded invalidation, no scene-wide tessellation on object movement, responsive
modal previews, shared-boundary crack prevention and undo memory cost.

No such implementation checks were run in this source study.

## Recommended defaults for implementation

The user asked for Blender-like mesh editing on NURBS and does not want to
specify every operation one by one. Use the following defaults unless a
specific Blender behavior or mathematical constraint requires a documented
exception:

1. Keep Blender's tool entry points, selection conventions, modal previews,
   shortcuts, confirmation/cancel, undo and redo. Implement geometry through
   NURBS-aware backends; do not route authoritative edits through a display
   mesh.
2. Map every registered mesh operation and each connected system to a NURBS
   behavior. Reuse Blender's UI/operator code where the data contract permits;
   otherwise preserve its intent and controls with a geometry-aware backend.
   Record a concrete alternative and reason when exact parity is impossible.
3. Prefer shape-preserving topology changes and exact NURBS constructions.
   Keep CVs, weights, degrees, knots, domains, components and established
   continuity unchanged unless the selected operation is supposed to change
   them. Do not silently fit/rebuild ordinary joined curves.
4. Use explicit face topology for mesh-like cuts and regions. Ctrl+R creates
   selectable edge loops; knot insertion/control-net refinement remains a
   distinct, shape-preserving edit. Triangulate/Poke preserve the support when
   possible; when Blender's polygon result defines intentionally faceted
   geometry, retain that meaning with separate degree-one supports.
5. Preserve declared smooth/sharp relationships through edits when the
   operation can do so. Do not invent smoothness at a sharp seam. Provide
   explicit transition tools for G1/G2 continuity and preserve exact circular
   arc components and their knot data.
6. Give every operation an exactness contract. Use project-level geometric
   tolerances for numerical intersections/fits; report approximation and fail
   clearly when the tolerance cannot be met. Keep the source recoverable.
7. Keep modifier, vertex-group, UV, attribute, shape-key, sculpt/paint,
   Geometry Nodes, render and export behavior in scope. Use native NURBS
   behavior where meaningful; otherwise use clearly identified derived
   evaluation or an explicit conversion. Never confuse Blender group weights
   with rational NURBS weights.
8. Follow Blender's module boundaries, code conventions, data ownership and
   lifecycle from the first implementation. Architecture selection and an
   end-to-end storage/evaluation/editing spike precede broad tool ports.

The full parity map should resolve individual operators against these defaults
and surface only genuine product decisions or technical blockers. This source
study and its supplements contain no application code changes or repository
push.
