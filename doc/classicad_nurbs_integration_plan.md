# classiCAD NURBS integration into Blender Fork

Status: initial proposal, superseded by the broader mesh-editing study at
`doc/nurbs_mesh_editing_study.md`. The user clarified that classiCAD is only
an imperfect proof of concept; Blender mesh behavior is the baseline, and
prototype restrictions are not target requirements. Data-block architecture
must be selected through an integration exercise rather than assumed here.
Inspected Blender branch: blender-5.2.2-stable, e694c2908bb.
Three Luna agents independently inspected classiCAD geometry, classiCAD
interaction behavior, and Blender's native geometry/editor pipeline.
That initial investigation made no application code changes or builds.

## Implementation progress

The stable base is pushed to `origin/main` at `5cd2933d604` (`Fix animation
channel table overflow after feature removal`). The fix removes two obsolete
animation-channel slots that wrote beyond the registration table and corrupted
the driver-variable clipboard. A GUI selection run now exits normally.
NURBS implementation changes are currently local and uncommitted.

The first foundation slice is in place: Blender's Add menu creates an
`OB_NURBS` object backed by a native `ID_NU` data-block, with a default open
cubic curve. The exact degree, order, CVs, weights, reduced knots and parameter
domain live in the data-block; the evaluated edge mesh is derived display data.
The ID has Main-list, depsgraph, Outliner, blend read/write and typed RNA
registration, including `bpy.data.nurbs_geometries`.

The Edit Mode lifecycle now enters and exits against the authoritative NURBS
ID without creating legacy `EditNurb` data. The viewport edit overlay has a
separate draw cache built from each curve's Euclidean control vertices and
control polygon. It does not use sampled proxy vertices as editable points.
Control-point picking and native Select All/None/Invert now operate on the
authoritative CVs. NURBS has its own edit context and keymap, and viewport
selection IDs map to control vertices rather than sampled display points. A
full CMake build and background checks of Add, Edit Mode, selection operations,
and return to Object Mode pass. The installed runtime scripts have been
synchronized from source, and startup imports `space_view3d` and registers the
native circle operator successfully. A native Add NURBS Circle operator
creates a degree-2 rational circle with nine CVs, positive circular-arc
weights, reduced knots, and parameter domain `[0, 4]`.

A native transform adapter sends selected NURBS CVs through Blender's standard
transform system, keeps double coordinates authoritative, and restores the
initial coordinates on the transform cancel path. A rebuilt headless check
verified a single CV moving out of plane, exact coordinate undo/redo through the
NURBS UndoType, and distinct duplicate data: editing the duplicate leaves the
source curve unchanged. A save/reopen check compared the native DNA values for
curve and CV IDs, degree, order, dimension, rational state, all CV coordinates
and weights, knots, domain, and selection flags; they survived exactly. The
`NurbsGeometry` RNA type exposed only the ID during that diagnostic, so it read
native data directly. Read-only curve/CV collections and degree, order,
dimension, rational state, domain, positions, weights and selection are now
implemented in RNA. The rebuilt `blender` and `blenkernel_nurbs_test` targets
pass; all four original kernel tests, five Python regression tests, and registered
CTest `script_nurbs` pass. The Python checks cover exact-circle construction
fields exposed through RNA, native Edit Mode translation and selection, object
duplication ownership, data-block creation/removal, and save/reopen. Blender's RNA
float properties expose float precision; these views never write their rounded
values back into authoritative doubles. Exact storage assertions remain C++
tests until a full-precision interchange API is provided. RNA now exposes the
reduced knot vector and stable curve/CV component IDs. IDs use decimal strings
to preserve all 64 bits through Blender's property system. The regression test
checks their read-only state, uniqueness within a data-block, and save/reopen
persistence. Its Python
save/reopen case covers only exposed fields, not full exact storage fidelity.

The Stage 1 curve and numerical foundation build and its seven validation groups
passed. This is not completion of the broader representation and topology
architecture gate, which remains provisional until the exercise below passes.
A GUI screenshot confirmed that selected
control points and the control polygon are drawn over the NURBS circle. A GUI
modal test confirmed transform preview, Enter commit, Escape cancel, and
undo/redo when the test scene has an explicit baseline undo step. Component
picking exposed a missing `index` vertex attribute required by Blender's
selection shader. Adding the attribute fixed the mismatch: a GUI-context GPU
selection test picked seven distinct control points and verified every returned
stable ID. The same test exited normally after the animation-table fix above.
GTest headers are present in the source checkout. The development build now
enables `WITH_GTESTS=ON` and `WITH_TESTS_SINGLE_BINARY=OFF` so the dedicated
`blenkernel_nurbs_test` target can validate the numerical kernel. Its tests
now pass: exact rational circle evaluation, a non-unit knot domain, affine
transforms preserving curve metadata, and invalid-data rejection.

The application now links with the latest changes. The standalone test build
exposed untyped ListBase pointer dereferences in the test fixture; these now use
explicit casts. Its link also exhausted the existing three passes through the
static-library dependency cycle after full node-system test initialization.
GTest builds now request four passes on `bf_blenkernel`, following Blender's
existing `LINK_INTERFACE_MULTIPLICITY` mechanism. Both the application and
standalone test executable build successfully with four jobs.

The latest application passes background checks for exact save/reopen and
independent duplication, constructor radius/selection/ID ownership, invalid
curve load recovery, and Apply Transform (including linked data and Make Single
User). The GUI run picks seven distinct CVs and passes actual keyboard
`G X 2 Enter`, Ctrl-Z, Ctrl-Shift-Z, and `G Y 5 Escape`. The first keyboard
harness queued every key in one timer callback and failed to move geometry;
spacing events across timer ticks passes in the same running test window.
A subsequent keyboard `G Z 1 Enter` moves only one selected CV out of plane.
The front-view screenshot shows its raised control polygon and the resulting
spatial rational curve. That test window is no longer running. A reproducible checked-in GUI acceptance
harness remains outstanding.

The kernel review added whole-curve validation for native XYZ dimensionality,
degree/order/count consistency, finite coordinates, positive weights,
nondecreasing finite reduced knots, and matching knot/parameter domains.
Evaluation no longer silently remaps an inconsistent stored domain and rejects
nonfinite results. Invalid loaded curves are removed with a read warning.
Default cubic creation now selects its CVs, matching the circle's add-and-edit
workflow. NURBS now participates in `BKE_object_obdata_size_init` so primitive
radius options scale exact CV coordinates while preserving weights and knots.
These changes passed the kernel and application checks described above.

A native event-simulation test confirmed another missing integration point:
the NURBS keymap had been registered but not attached to the viewport region.
The viewport now attaches the map alongside the other edit-mode handlers, and
the map includes Blender's standard G/R/S transform bindings. The actual
keyboard confirmation/cancellation and keyboard undo/redo path passed in the
GUI run described above; earlier modal tests invoked the operator directly.

Build configuration uses `/home/molotovgirl/Desktop/build_blender_5.2.2`.
Although that path points to `build_linux`, configure through the recorded path
to preserve CMake's compiler commands and their build-path prefix mappings.

The ownership audit found an extra data user in the circle operator: its newly
allocated ID now releases the creator's user before the object takes ownership.
NURBS also now participates in Make Single User's existing ID-copy path.
Apply Transform's single-user unsupported path previously skipped NURBS, while
the shared-data path could bypass that skip; an isolation probe crashed in the
missing copy-type path. Apply Transform now validates NURBS data ownership and
uses a BKE affine transform on double CVs, retaining IDs, weights, degrees and
knots. Dedicated checks cover individual Location/Rotation/Scale, applying all
three, linked selected users, and isolation from an unselected user. These
changes passed the previous rebuild and validation sequence.

Stage 2's planned drawing and manipulation integration is implemented. The
native NURBS menu creates degree-1 lines, degree-4 paths, rational degree-2
arcs and circles, plus the default cubic curve. Primitive placement uses
Blender's existing view/cursor alignment and object transform options. The
standard Add > Curve > NURBS entry now routes to the native `OB_NURBS`
constructors instead of the legacy Curve operators. The old operators remain
registered for scripts during migration. Blender's native G/R/S modal
transforms provide axis constraints, numeric input, confirmation, cancellation
and undo. Native object and collection ownership keeps NURBS objects available
to the View Layer and Outliner, including when an object is moved between
collections.

The integrated viewport path supports control-point and topology-component
picking, curve-only control-point proportional editing, and native snapping to
NURBS control/topology points and exact curve/topology-edge supports. Nearest
curve/edge snapping evaluates the stored NURBS support and refines the closest
parameter; it does not use the display mesh for these targets. The evaluated
mesh remains the target for face, volume, and other unsupported snap modes.
Curve-only custom orientations, rotation/scaling, median/individual pivots,
numeric axis translation, cancel, undo/redo, and snapping have regression
coverage. The native topology exercise continues to cover two rational curved
patches with different parameter domains, a shared seam and a trimmed hole.

The focused suite currently includes nine NURBS C++ suites, registered
background Python checks, save/load/apply coverage, and interactive viewport
acceptance for face/vertex/edge picking, numeric seam translation, undo/redo,
cancel, edge splitting, stable child picking, point and nearest-edge snapping,
and connected proportional falloff versus Euclidean falloff. Collection
membership and View Layer visibility are checked through Blender's native
object/collection model.

Stage 2 does not remove Blender's legacy Curve data type or unregister its old
NURBS operators; the normal Add menu now uses the native object path, while the
legacy operators remain callable by scripts. This stage adds Blender-style
primitive placement, not point-by-point CAD drafting. Exact surface/face
snapping, topology-aware custom orientations, topology proportional editing
and arbitrary NURBS surface deformation remain future work. Those are beyond
the Stage 2 checklist and do not establish full mesh-edit parity.

## Intended outcome

Use classiCAD's exact NURBS geometry and supported modeling behavior inside
Blender's native viewport, Object/Edit modes, selection, manipulators, modal
tools, undo, Outliner, and file persistence. The C++ geometry is authoritative;
tessellated meshes and curves serve display, rendering, and picking.

The final workflow replaces Blender's old interactive NURBS editing workflow.
The separate native object is an integration slice during development, not the
final delivery of two competing NURBS editors. Migrate the existing NURBS
creation/editing entry points as their replacements pass the behavior gates.
Retain spline functionality used by Geometry Nodes and other retained features.
The broader study's curved-face-with-hole, shared-edge and topology-split
architecture exercise still needs to pass; the circle slice alone does not
prove the final topology architecture.

## Evidence and limits

- classiCAD already implements rational planar/spatial curves, explicit work
  frames, Rhino reduced knots, exact curve operations, tensor surfaces, UV
  trims, and ruled extrusion. See src/core/geometry/nurbs_curve.*, work_plane.*,
  curve_editing.*, nurbs_surface.*, nurbs_surface_factory.*.
- Solid storage includes an extrusion seed and optional authoritative edited
  boundaryFaces. Connected corner and face operations are restricted; see
  nurbs_solid.*, nurbs_solid_face_extrusion.* and the project contracts
  CONNECTED_SURFACE_VERTEX_EDITING.md and SOLID_FACE_EXTRUSION.md.
  General BRep topology, arbitrary curved-face extrusion, and general Booleans
  are not already implemented. Orthogonal rectangular face sweeps use an
  analytic arrangement with an explicit two-million-cell rejection limit.
- classiCAD's geometry modules avoid QWidget interaction, but their types still
  depend on Qt containers, QPointF, QString and qreal. Its current top-level
  build requires Qt Widgets/OpenGL; it is not an independent portable kernel.
- classiCAD's current Blender adapter embeds exact document JSON in
  classiCAD.DocumentData and generates display proxies. load_project reads
  the embedded JSON, not edits to Blender's scene geometry. At 1,000 objects
  it batches proxies by layer. This is file exchange, not native CAD editing.
  See src/core/serialization/blender_project_adapter.py.
- Blender legacy Curve/Nurb supports rational curves and tensor patches, but
  uses float CV/weight/knot storage and lacks CAD trim/solid connectivity.
  Some existing edit operators regenerate knots. See DNA_curve_types.h,
  blenkernel/intern/curve.cc and editors/curve/editcurve.cc.
- Blender's newer CurvesGeometry supports NURBS and custom knot slices in this
  checkout, despite an outdated header comment. It remains a float-based,
  one-parameter curve system without tensor surfaces or solid topology.
- classiCAD component selection/source extraction still partly belongs to
  viewport_widget.cpp. Its Qt event routing and snapshot history should not
  be transplanted into Blender's editor framework.
- The classiCAD architecture ledger reports outstanding broad regression
  failures. This audit did not rerun those checks; prototype behavior must be
  captured and checked before it is used as a porting baseline.

## Recommended architecture

1. Extract a shared C++ geometry kernel using explicit double scalars and
   framework-neutral value/container/error contracts. Adapt classiCAD's Qt
   values at its boundary. Preserve its representation and parameterization;
   do not create independent geometry algorithms in the two hosts. Extract
   incrementally rather than refactoring all of classiCAD in one step.
2. Add a native CAD geometry data block referenced by ordinary Blender Objects.
   Its persisted records cover curves, work frames, surface CV nets/UV trims,
   extrusion seeds and edited exact boundaries. Follow Blender DNA, ID/RNA,
   ownership, copy, blend read/write, versioning and depsgraph conventions.
   A new ID needs an available registry identifier; select that during design.
   Blender storage adapters and runtime views use the shared geometry contract
   with exactly one authoritative representation per object.
3. Use stable geometry/component IDs and explicit connectivity/weld references.
   Render triangles and control-cage indices map to those IDs. Operations
   return selection remapping after topology changes; triangle indices and
   coincident coordinates alone cannot define persistent ownership/connectivity.
4. Implement Blender-native edit data, component selection, transform adapters,
   modal operators, status feedback, cancellation and undo. Reuse Blender's
   existing G/R/S interaction, axis constraints and numeric entry where useful;
   route resulting edits to exact geometry rather than BMesh proxy vertices.
   Add host-specific drawing/workplane/snap adapters for CAD operations.
5. Generate derived tessellation and control cages with source provenance.
   Cache per geometry revision; Object Mode movement should transform cached
   display data without retessellating. Integrate bounds, picking, depth,
   shading and render evaluation through Blender's native pipeline.
6. Persist CAD data natively in .blend. Import existing classiCAD embedded
   documents directly from JSON, preserving entity identity even when their
   older display proxies were batched. Keep .3dm import as a separate adapter;
   a Rhino exporter is additional work, not an existing reusable capability.

## Blender integration and code-style rules

### Connected topology spike

The concrete record, parameterization, edge-split and curved-fixture contract
is in `doc/nurbs_topology_contract.md`. Its bounded connected-topology gate has
passed; broader surface modeling remains open.

Luna agents implemented shared curve ownership helpers, an exact homogeneous
curve splitter and a tensor-product surface evaluator. The curve and surface
evaluators now share reduced-knot basis logic. Their foundation build and
validation passed before Stage 2 integration. Surface and topology records are
now attached to native ID copy/free, undo and blend IO. New code provides
derived trimmed-face tessellation, mesh face provenance, vertex/edge/face
picking, selection history and an atomic boundary-translation exercise.
The integrated build and topology acceptance have passed. Earlier surface DNA
padding issues were fixed. Recent integration fixes address missing RNA DNA
mappings, enum defaults, fixture allocation APIs, the empty-face mesh path,
boundary tessellation roundoff, and a null loose-edge draw batch for NURBS
surface proxies.

The boundary translation exercise accepts complete clamped isoparametric
surface boundaries with linear UV trim supports. It reconstructs affected
exact isocurves, including hole boundaries, and rejects incompatible cases
atomically. This is a deliberately bounded architecture test. Arbitrary
vertex/face deformation, rotations of connected surfaces, general snapping,
construction tools and the full manipulation-parity milestone remain work.

Reproducible acceptance sources are `tests/python/bl_nurbs_topology.py` for
native operators, undo/redo and save/reopen, and
`tests/python/bl_nurbs_topology_ui.py` for GPU picks and modal viewport input.
The passing UI run checked typed translation and keyboard cancel; undo/redo
were invoked through Blender's editor operators. The harness leaves its Blender
window open after completion. A C++ blendfile roundtrip generates its exact
shared-edge/hole fixture through the opt-in `BLENDER_NURBS_TEST_ARTIFACT` path;
no test-only geometry constructor is exposed in the application.

The curve slice does not prove the final geometry architecture. The required
next storage probe uses genuinely curved rational tensor-product supports, a
trimmed face with a hole, and a boundary edge shared by two faces. Store one
topological edge ID with separate oriented UV p-curves for its two face uses;
coincident display vertices do not establish connectivity. Surface parameters
and trimming p-curves are independent of texture UV mappings.

The probe must split that shared edge exactly, create one topological vertex
and two child edge IDs, update both incident trim loops, and return an explicit
selection remap. Keep both surface supports and the hole unchanged. Preserve
edge and p-curve parameter intervals, handling reversed uses explicitly.
Validate referential integrity, closed oriented loops, positive finite rational
supports, and agreement between the 3D edge and each surface/p-curve pair under
a named tolerance. Commit atomically only after validation succeeds. Copy,
undo, save/reopen and display picking must retain this connectivity and remap.

Current `ID_NU` lifecycle callbacks manually own each curve's CV and knot
arrays. Before introducing nested surface/trim/topology ownership, centralize
record allocation, clone, free and validation in BKE helpers. Keep DNA records
plain, with persistent pointers fully covered by copy and blend IO. Runtime
caches stay separate. classiCAD's specialized extrusion/trim representation is
a reference; it does not supply this complete shared-edge topology contract.

### Integration rules

- Keep persistent geometry in DNA structs and manage its ID lifecycle in
  `blenkernel`: allocation, copy, free, blend read/write, and runtime-cache
  invalidation follow the nearest existing Blender ID/data type.
- Keep editor interaction in Blender's editor/operator and transform systems.
  Keep viewport batches and drawing in `draw` caches and overlays. Do not add a
  second Qt-style UI or history layer inside Blender.
- Treat NURBS doubles as authoritative. GPU float batches, evaluated meshes,
  picking accelerators, and other caches are disposable derived data, with
  explicit invalidation at the owning data boundary.
- Preserve Blender's module and dependency direction (`makesdna` → `blenkernel`
  → `depsgraph`/`draw`/`editors`), use existing allocation and container APIs,
  and keep each source change in the subsystem that owns that behavior.
- Follow nearby Blender C++ conventions for naming, formatting, comments,
  callbacks, CMake registration, and operator lifecycle. Reuse established
  Blender abstractions where their data model fits; do not make a mesh proxy the
  authority merely to reuse mesh-only algorithms.

Precision requires an explicit contract: exact model data remains double,
but Blender Object matrices, native transform plumbing, GPU vertices and some
snapping paths use floats. Define object-local geometry, unit conversion and
double edit-delta handling before promising CAD tolerance or large-coordinate
accuracy. Display origin rebasing and double placement are options to evaluate.

## Delivery sequence and acceptance gates

### 0. Behavior and representation contract

Use the Blender mesh-edit behavior baseline and implementation defaults in
`doc/nurbs_mesh_editing_study.md`; do not require the user to enumerate tools or
choose each operation individually. Record representative classiCAD scenes,
exact knots/weights/domains, selection expectations and operations. Establish
which prototype failures predate integration. Define stable component IDs,
placement, units, connectivity, unsupported-operation policy and CAD-vs-mesh
conversion as part of the architecture and storage spike.

### 1. Exact curve vertical slice

Extract only the kernel needed for a rational curve and add native storage,
evaluation, tessellation and save/load. Start the user workflow from Blender's
Add menu by creating an actual NURBS element, then edit its CVs in the
viewport. Display an exact circle and spatial NURBS curve; support native Grab,
cancel, undo/redo and duplicate. Preserve weights, knots/domains and curve
identity throughout. Prove this complete path before adding the whole tool
catalog.

### 2. Drawing and manipulation parity

Port line/arc/circle and required spline constructors; add workplane capture,
CAD snapping, axis/numeric input and native modal feedback. Integrate rotate,
scale, component picking, object grouping and Outliner synchronization.
Use Blender input conventions unless a user-approved CAD behavior overrides
them. Preserve separate components and one undo entry per committed operation.

### 3. Exact surfaces

Add tensor surfaces, trim loops, surface cage editing, source-aware face and
edge picking, planar Fill and exact curve-to-ruled-surface extrusion. Preserve
UV domains, rational weights, trimmed holes and source ownership on edits.

### 4. Supported solids and connected editing

Port extrusion seed/boundary storage and classiCAD's supported corner,
edge and planar-face operations. Add persistent weld connectivity and exact
subdivision/remapping. Reproduce normal Extrude versus Extrude with Division,
Weld to Snap, inward/outward steps and repeated extrusion. Explicitly reject
unsupported curved/trimmed/general-topology edits without committing geometry.
This milestone proves the user's complete solid-modeling prototype workflow.

### 5. Production integration

Complete the requested remaining constructors/operations and interchange.
Measure interactive large scenes, revision caches, selection and undo cost.
Integrate render engines and modifiers under an explicit evaluation policy:
an evaluated mesh can represent a rendered result, but cannot silently replace
the authoritative CAD model. Any conversion to editable mesh is explicit.

Each milestone needs both exact-geometry checks and native Blender interaction
checks: select, invoke, constrain, preview, confirm/cancel, undo/redo and
save/reopen. Build and run the installed application with matching runtime
scripts; a successful C++ link alone is insufficient, as the previous stale
startup-script failure demonstrated.

## Current implementation defaults

This historical proposal's earlier questionnaire is superseded. Use Blender's
mesh-edit selection, modal, transform, confirm/cancel, undo/redo and keymap
behavior as the default interaction contract, with NURBS-aware geometry behind
it. Keep topological vertices/edges/faces distinct from control points. Make
the complete inventoried mesh workflow the product target and deliver it in
stages; use exact NURBS/BRep operations where possible and explicit derived
mesh or tolerance-managed results where exact NURBS output is not available.
See `doc/nurbs_mesh_editing_study.md` for the complete behavior map, connected
systems, architecture contract and staged implementation strategy.

## Conclusion

The existing classiCAD model and Blender editor infrastructure substantially
reduce algorithm and UI work. A first exact-curve editing slice is contained.
Full CAD editing parity is substantial native integration, particularly stable
selection/connectivity, precision, trimmed/solid topology, undo and persistence.
Use the existing prototype as a verified behavior reference and reuse its
supported algorithms; do not assume Blender's existing NURBS already provides
the required CAD object model.
