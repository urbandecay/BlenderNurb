# classiCAD NURBS integration into Blender Fork

Status: initial proposal, superseded by the broader mesh-editing study at
`doc/nurbs_mesh_editing_study.md`. The user clarified that classiCAD is only
an imperfect proof of concept; Blender mesh behavior is the baseline, and
prototype restrictions are not target requirements. Data-block architecture
must be selected through an integration exercise rather than assumed here.
Inspected Blender branch: blender-5.2.2-stable, e694c2908bb.
Three Luna agents independently inspected classiCAD geometry, classiCAD
interaction behavior, and Blender's native geometry/editor pipeline.
This investigation made no application code changes or builds.

## Intended outcome

Use classiCAD's exact NURBS geometry and supported modeling behavior inside
Blender's native viewport, Object/Edit modes, selection, manipulators, modal
tools, undo, Outliner, and file persistence. The C++ geometry is authoritative;
tessellated meshes and curves serve display, rendering, and picking.

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
evaluation, tessellation and save/load. Display an exact circle and spatial
NURBS curve, select/edit their CVs with native Grab, cancel, undo/redo and
duplicate. Preserve weights, knots/domains and curve identity throughout.
Prove this complete path before adding the whole tool catalog.

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
