# Native NURBS arc tools

The **Arc** workspace tool is available beside Line and Curve Interpolation in
Curve and Surface Edit Mode. Its tool settings select the RCAD construction mode:

| Mode | First placement | Second placement | Third placement |
| --- | --- | --- | --- |
| Center | Center | Start point / radius | Signed sweep |
| 2 Point | Start endpoint | End endpoint | Bulge above/below the chord midpoint |
| 3 Point | Start endpoint | End endpoint | A point the arc passes through |

The third placement commits one exact native curve support, divided into the
number of curved edges chosen by Segments (32 by default). Every preview division
becomes a shared topology endpoint, with equal angular spacing along the arc.
An open arc has one more endpoint than edges; a full sweep closes the last edge
onto the first endpoint. The through point selects minor or major arcs and winding. The center, bulge and through
point are construction inputs, not additional topology vertices.

Click or Enter confirms a construction stage. Type a radius/chord length during
the second placement, a sweep in degrees during the final Center placement, or
a signed bulge length during the final 2 Point placement. Three Point also accepts
a distance from its start to the through point. Esc or right-click cancels the
whole uncommitted arc; Backspace returns to radius/chord placement. Completion
returns to Box Select, like the other native drawing tools.

X/Y/Z constrain the endpoint chord; for Center they choose its plane normal.
After the chord/radius is established, X/Y/Z can choose a plane containing that
chord/radius. P toggles a perpendicular plane; for vertical chords it alternates
the two reference-plane orientations. L allows surface plane changes while
placing the radius/chord. The final construction plane is held fixed. Shift
locks the chord direction, Ctrl toggles Blender snapping, and Alt bypasses
inference and soft angular/semicircle snapping. Angle snapping defaults to
15-degree increments with a six-degree attraction range.

When snapping is enabled, a target glyph appears under the hovering pointer
before the first placement. Line, Curve Interpolation, Arc and Circle tools use
the same snap query for hover and placement, including native endpoints, edges,
edge/face centers, and grid targets. Hover creates no geometry; during drawing
the modal preview provides the feedback instead.

Viewport navigation stays available throughout construction. Middle mouse orbits
and Shift + middle mouse pans using Blender's configured navigation bindings.
Navigation holds the construction inputs fixed; release the gesture to continue
drawing from the same stage.

## Porting decisions

The source is `radCAD/operators/arc_tools.py`, with input dispatch in
`modal_core.py` and sampling in `geometry_utils.py` in the supplied
`CADdraw/12_28_26_tan3_Done` addon.

RCAD's three classes share staged plane, radius and winding calculations, then
`commit_arc_to_mesh` creates mesh edges from `preview_pts` and invokes its weld
manager. Native NURBS construction instead uses positive rational quadratic
spans of at most 90 degrees, with repeated interior knots and domain [0, 1].
Segment divisions become source edge intervals and shared endpoints without
altering the exact support. Display resolution remains independent. No mesh weld or display-mesh fitting is involved.

The 2 Point sagitta is projected onto the chord's perpendicular bisector. The
3 Point circumcircle chooses the sweep containing its through point. Center
mode unwraps pointer angles across the branch cut, permits either winding, and
limits the result to one revolution. A zero sweep, zero radius, or collinear
three-point construction remains an uncommitted preview until corrected or
cancelled; the replay operator rejects it atomically.

Geometry is constructed in world space and its rational control points are then
transformed into object space. This keeps the drawn result circular under
nonuniform object scale. Input coordinates and interactive sweep replay retain
double precision. Explicit native endpoint snaps reuse the source vertex IDs;
no proximity search welds unrelated source components. The shared native
`TopologyPreview` commit path handles selection, edit lifecycle and undo.

The port uses Blender's existing snap settings, native source endpoint/edge/face
center picking and snap glyphs. Segments controls native edge and endpoint divisions. RCAD mesh weld settings
have no native geometry equivalent. The Center / 2 Point / 3 Point selector remains in the tool settings.

## Construction overlays

The rotating compass follows the drawing plane, keeps a 125-pixel default
screen diameter, and includes the RCAD outer circle, angular ticks, two inner
arcs with closing chords, and center cross. It appears only in the 1 Point
(Center) arc, at its center. The 2 Point and 3 Point tools never show a compass;
they use the addon's chord, height and through-point guides instead.

The compass changes to red, green or blue for X-, Y- or Z-normal planes, and
black for other planes. Chord, bulge and through-point guides use matching axis
colors when aligned, and a neutral color otherwise. Center mode has green
start/end radius guides. Exact-curve previews and their display markers are
black, with yellow snap feedback. These are viewport overlays, not source
geometry.

Live labels beside the pointer identify the active construction stage and show
radius, chord, sweep or sagitta, plus segment count. The wheel changes Segments;
Ctrl-wheel zooms. Three Point follows RCAD's click order: start,
end, then a point on the arc. Collinear third points show a correction hint and
can be moved before committing.

The overlay popover next to the existing selector exposes compass visibility
and diameter only for Center mode. Each mode also exposes its axis colors,
guide colors, segment count, marker size and measurements. Segment divisions
retain the exact NURBS support.

## Validation

- `NurbsCircularArc.*` in `nurbs_geometry_test.cc`: circle radius and plane,
  signed/major/full sweeps, through-point winding, vertical chords and rejection.
- `tests/python/bl_nurbs_arc.py`: all construction modes, nonuniform scale,
  full-circle topology, native persistence and atomic operator rejection.
- `tests/python/ui_simulate/test_nurbs_arc.py`: workspace registration, staged
  interaction, numeric input, plane changes, endpoint sharing, cancellation,
  Backspace, undo and redo.
- Existing `test_nurbs_draw.drawing_workflow`: line/interpolation regression.
