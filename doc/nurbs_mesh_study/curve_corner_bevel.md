# Native NURBS curve corner bevel

In native NURBS edit mode, select a corner vertex where two straight edges meet and press
**Shift+Ctrl+B**. Move the mouse or type a distance, then click/Enter to accept or Esc to cancel.
The NURBS menu and native curve context menu also provide **Bevel Curve Corners**. The binding
appears in the Curve keymap in Preferences, using `curve.bevel` with Affect set to Vertices.
Ctrl+B explicitly uses Affect Edges for surface bevels and follows the same segment convention.

Width is the setback distance along each of the two incident edges. Zero segments replace the
corner with a straight chamfer. One creates one rational quadratic circular fillet with no
internal topology vertices. Two or more segments partition that same circular fillet whose
endpoint tangents follow the incident lines. More segments do not approximate the circle with
a polyline. The shortcut/menu start with one segment; the mouse wheel changes
this during preview. Selected endpoints and straight subdivision stations are skipped. The
selected corner is replaced by selected fillet endpoints and any new partition stations.

All selected eligible corners use the same width. Clamp Overlap limits that width before the
remaining length of an incident edge vanishes, including edges with both endpoints selected.
Other global intersections are subject to the existing preview/trim validation.

Loose curve paths in any plane and corners of supported planar face boundaries are handled.
Faces retain their identities and use private affine plane charts with updated rational trim
curves. Work happens on independent source candidates; cancel restores the original geometry,
and accepted changes use normal undo/redo. Display geometry is evaluated from the NURBS source.

Existing curved incident edges require a curve fillet intersection solver and are rejected.
Branching vertices, solid vertices with more than two incident edges, and nonplanar incident
faces require other corner/surface constructions and are also rejected without changing source.

## Control point mode

Shift+Ctrl+B also reads selected source control handles. Endpoint controls that identify a
straight on-curve corner use the topology fillet above. Interior controls of a clamped wire
support instead bevel the **control polygon**: replace the selected corner by two setback
handles for a zero-segment chamfer, or a rounded profile for one or more segments. A rounded
cage still needs its middle control handle even without a topology split. These
handles define the smooth NURBS curve; the resulting curve is not claimed to be a circular
fillet of its original locus. Degree, original clamped domain and endpoint coefficients are
preserved. Unselected coefficients remain unchanged. Additional local interior knots provide
the extra basis intervals, so the curve shape changes around the edited cage.

The replacement handles remain selected. All wire edges sharing the edited support keep their
parameter ranges and identities; their intermediate topology stations are reevaluated from the
changed support. Cancel, undo and redo retain control selection as well as source geometry.
Attached surface supports and moving junctions with other curve supports need coordinated
basis/deformation edits and are rejected atomically. This operation does not silently convert
curves to polylines or change the degree of a spline.

## Preview and shared endpoints

Bevel preview uses the object's normal display sampling settings. It does not lower global
boundary resolution during mouse movement: an unselected spline in the same object must retain
identical display samples during preview, wheel changes, cancel and confirmation.

A cropped support may already have a tolerated rounding difference between its endpoint
coefficient/evaluation and the authoritative topology vertex. When control-cage bevel leaves
that endpoint's evaluated position unchanged, it also retains the topology vertex unchanged.
This preserves attached, unedited wire pieces. Genuine motion at an attached junction still
requires a coordinated deformation and is rejected.

## NURBS surface edges

Ctrl+B uses the same numbering: zero makes a flat chamfer, one builds the entire rounded
NURBS strip with no internal partitions, and two or more divide that same rounded strip into
the requested count. The modal wheel can reach zero and the redo panel uses the same range.
This applies to both the planar edge-network constructor and compatible curved ruled supports.
Shared rounded corner patches are constructed even at one segment; partitioning starts at two.
The kernel's default zero remains a basic chamfer for calls that omit a segment count, while
the interactive operator defaults to one rounded strip.

## Starting settings

The shortcuts and NURBS menu entries explicitly start a new bevel at one segment (an unsplit
round), independent of the segment count used by the previous bevel. New NURBS lines, circles,
paths and surface objects start with Preview Resolution U and V set to 16. Saved objects retain
their existing resolution settings, and the preview values remain editable.
