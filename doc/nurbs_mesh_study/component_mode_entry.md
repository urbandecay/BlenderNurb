# Component modes from the ordinary NURBS workflow

This stage connects normal Add/prototype objects to the existing shared NURBS
editing system when the user chooses Vertex, Edge or Face mode. The user's
interaction definition is: vertices are line/curve endpoints, edges are NURBS
lines or curves, and faces are NURBS surface patches. The storage uses stable
identities for those relationships; the geometry remains rational NURBS.
Control coefficients are internal geometry data, not a new CV editing mode.

The implementation uses the existing native selection, picking, drawing and
geometry services. Verification results and exact commands are recorded in the
[checkout audit](current_checkout_audit.md). This is a restricted source-entry
checkpoint, not completion of the full NURBS editing pipeline.

## Mesh behavior reference

Blender's mesh mode header and `MESH_OT_select_mode` select the primary domain,
reconcile its dependent selection through BMesh, and redraw the appropriate
handles/highlights. The native NURBS selection service already compares those
flush rules against actual BMesh, including all seven combined modes. Its
viewport picker filters source hits by the enabled domain. Its draw cache emits
endpoint markers only in Vertex mode, curves in Edge mode, and face markers/tint
in Face mode. Lower-domain flags required by topology operations are not extra
independent picks. A selected edge's endpoint closure therefore stays connected
without displaying vertex handles in exclusive Edge mode.

The missing integration was source activation: ordinary Add still creates the
tagged prototype, while the mode operator/picker/drawer require the native source.
Adding buttons alone would not fix that. A second selection implementation on
the legacy control net would reproduce the earlier face-region ambiguity.

## Source entry and undo

The header shows the existing three mode controls for a native source or a
complete tagged prototype. The existing 1/2/3 operator prepares an exact import
before changing an owner. It copies the tagged source's NURBS coefficients,
weights and knots into native geometry, preserves explicit endpoint/edge/face
identities, normalizes selection, and preflights the existing native preview.
It installs one source authority: the legacy edit list and persistent cache are
released together, and the native editable source and persistent cache are
installed together. No geometry is reconstructed from display triangles.

Preparation is read-only. The prepared record checks its owner, edit container,
active endpoint, source bytes and ID session identity before commit. All edit
owners and other object users with modifiers are checked before any source
changes. Selection and all subsequent covered tools use the existing kernel and
owner commit after entry, rather than new operator-specific component flags.

Edit undo now supports this deliberate source-kind transition. Undo restores the
complete original tagged list, its attributes and key-index map. Redo restores
the native geometry, mode and selection. Existing same-kind native snapshots keep
their normal revision behavior. Retained read snapshots own their geometry across
the transition; a new edit lifetime has a different state token even when its
component IDs match. Generic edit/object-mode undo and saving are covered. Source-kind undo changes
explicitly refresh the evaluated owner, so a legacy preview cannot be reused
without the native immutable source handoff.

## Scope and preservation

Entry currently covers the fully tagged endpoint/line/2-by-2-patch prototype and
existing native sources. It does not guess connectivity in untagged old curves,
fit replacement surfaces, weld coordinates, or convert arbitrary old CV curves
into endpoint handles. Ordinary old-file loading and unrelated legacy curve
editing remain available. General legacy curves, circles and surface primitives
need their own verified import, fit-handle and seam/pole policies.

Unsupported data rejects before source mutation: shape keys, Curve animation,
modifiers on any object user of the Curve, curve offsets/bevels/taper objects,
hidden legacy components, non-positive weights, unmapped point radius/tilt/
soft-body attributes and differing per-spline preview resolutions. Source
coefficients are not fitted, rounded back to floats or assigned a larger model
tolerance to make entry succeed.

Native movement, E, Shift+D, F and component deletion are the existing covered
subset. Broader transforms (including native R/S), snapping, lasso/circle
selection, per-component visibility and full component-data Python bindings
remain unfinished. Choosing a component mode activates that native subset; it
does not claim the rest of the editing pipeline is complete.

## Try the ordinary workflow

Launch `/home/molotovgirl/Desktop/build_linux/bin/blender`. Use **Add → Curve →
Nurbs Line**, then **Tab**. The three component buttons appear beside the Edit
Mode menu. Click a button or press **1 / 2 / 3** to enter the shared source path.
Vertices are endpoints, Edge mode selects whole NURBS curves, and Face mode
selects whole NURBS patches. Exclusive Edge mode does not show endpoint dots.
Shift-click a button or Shift+1/2/3 combines modes, keeping at least one enabled.

For a surface: select the line in Edge mode, **E, Y, 2, Enter**. Choose Face mode,
select the sheet, then **E, Z, 1, Enter**. Continue using the covered Move,
Duplicate, Fill and Delete operations. Face extrusion currently needs an explicit
axis or supported Global/Local movement; automatic normal constraint is later
work. Native Rotate/Scale are still gated. Undo the source-entry step to return
to the original tagged source; redo restores native geometry and selection.

An existing valid tagged line/patch model can enter the same way. Untagged old
CV curves and general NURBS primitives need a separate verified import policy;
this checkpoint does not turn their control coefficients into endpoint handles.
The [prepared component examples](native_component_modes_tryout.md) remain useful
for broader native geometry that the ordinary Add prototype cannot represent.


Verification: **237 C++ tests, zero skips; 39 NURBS Python cases; five CTest
targets; 22 viewport cases during this stage**, including the three final
source-entry cases. The three mode screenshots and pre-entry header were
visually inspected. This confirms the covered subset; it does not certify the
unfinished general editing pipeline.
