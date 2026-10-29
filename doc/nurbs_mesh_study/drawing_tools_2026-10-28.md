# Native NURBS drawing tools

The left viewport toolbar in Curve and Surface Edit Mode gains **Line** and
**Curve Interpolation** tools. The geometry operators and mouse interaction are
C++; Blender's standard Python workspace-tool definitions register the buttons,
keymaps, and tool settings. The line and curved icons come from radCAD's
`line_default.svg` and `arc.svg`.

Line places a connected degree-one NURBS chain. Curve Interpolation offers:

- **Fit Points**: chord-parameterized NURBS interpolation with averaged knots,
  using the existing native free-form solver. It passes through the placed points
  within the solver's measured residual tolerance.
- **CVs**: an open clamped NURBS curve whose control polygon is the clicked points.

This intentionally uses native NURBS spline construction rather than radCAD's
sampled Catmull–Rom mesh output. Degree defaults to three and is limited by the
number of input points. The operator supports 2–256 points and degrees 1–5.

Click to place points; Enter or right-click finishes, and Escape cancels. Backspace
removes the last point. X/Y/Z lock a world axis, N locks the hovered face normal,
Shift holds a direction, L toggles drawing-plane locking, Ctrl inverts geometry
snapping, and Tab switches Fit Points/CVs during spline drawing. Typing a length
and pressing Enter places the next point. R explicitly starts length entry for
the next point. S enters the interpolation spline's segment count; those exact
NURBS edge divisions become shared topology endpoints. View navigation passes through the modal
operator. Options are normal Blender operator properties in the existing redo panel;
there is no additional settings panel or add-on options overlay. Tab selects the
curve drawing method while drawing. The redo panel can change method and degree
after confirmation.

The modal preview retains the initial native source and draws a temporary wire.
Confirmation builds and commits one native topology candidate. Cancellation has no
source edits to undo. Existing native endpoints explicitly picked by snapping are
reused; closed polylines reuse their starting endpoint. Snapping to the interior of
an existing edge places a point on its curve but does not split that existing edge.

Preview and confirmation construct the curve in object-local coordinates, including
for nonuniform object transforms. Local input is recorded at double precision for
redo. A spline's native topology currently contains its two endpoints and one edge;
its internal control coefficients are curve data. This work adds drawing modes, not
a new persistent CV or fit-point editing domain.

The tools use the fork's native/tagged NURBS edit entry. They are scoped to one edit
object and do not replace Blender's legacy Bezier drawing tools. General closed
periodic spline construction and radCAD's Ctrl-click sharp interpolation chains
are not included in this initial tool implementation.

Validation:

- Blender rebuilt successfully; the final C++ syntax check also passed.
- `test_nurbs_draw.drawing_workflow` exercised the actual workspace tools with
  simulated clicks, both spline methods, native source creation, undo/redo,
  cancellation, mode changes, and endpoint identity reuse.
- The spline-method check compares the evaluated geometry as well as topology counts.
- The supplied SVGs were normalized to Blender's 1600-unit white icon convention;
  the toolbar screenshot was inspected after rebuilding the icon data.
- Typed numeric length placement and changing the final segment length through redo also passed.
- Viewport log: `/tmp/nurbs-move-optimization/drawing-tools-numeric/run.log`.
- Screenshot: `/tmp/nurbs-move-optimization/drawing-tools-final/drawing-tools.png`.

Typed parameter values stay pending until Enter accepts them. Typing and editing
the entry preserve the current geometry preview; Esc discards the pending value.
