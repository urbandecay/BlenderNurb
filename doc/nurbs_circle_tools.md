# Native NURBS circle tools

The Circle button in Curve and Surface Edit Mode contains all five RCAD modes:

| Mode | Construction |
| --- | --- |
| Center | Center, then radius point |
| 2 Point | Opposite diameter endpoints |
| 3 Point | First point, second point, third point on the circumference |
| Tan Tan | Preselect two separate native curve chains, then move and confirm the circle |
| Tan Tan Tan | Preselect three separate coplanar native curve chains; Tab cycles the eight tangency configurations |

Click or Enter confirms each placement. Esc or right-click discards the preview.
Backspace returns from the third-point stage to the second point. Typed lengths
set radius in Center mode and diameter in 2 Point mode. R selects radius entry
in Center mode; R and D select radius or diameter entry for 2 Point. Enter
accepts the typed value and leaves the circle preview active for confirmation.
S enters the segment count; the resulting divisions become real endpoints on
the exact circle. C toggles angle snapping. P toggles a perpendicular plane, X/Y/Z constrain the
live radius/diameter, X/Y choose vertical plane orientation, L toggles plane
locking, Shift locks direction, Ctrl toggles snapping and Alt bypasses inference.

The Segments setting and mouse wheel set the number of circular edges and
shared endpoints; Ctrl-wheel zooms. Every circumference division shown in the
preview becomes a real topology endpoint on commit. T adds tangency contacts as
extra endpoints (coincident divisions are merged). The circle remains exact. Completion returns to Box Select.

When snapping is enabled, a target glyph appears under the hovering pointer
before the first placement. Line, Curve Interpolation, Arc and Circle tools use
the same snap query for hover and placement, including native endpoints, edges,
edge/face centers, and grid targets. Hover creates no geometry; during drawing
the modal preview provides the feedback instead.

Viewport navigation stays available throughout construction. Middle mouse orbits
and Shift + middle mouse pans using Blender's configured navigation bindings.
Navigation holds the construction inputs fixed; release the gesture to continue
drawing from the same stage.

## Addon behavior studied

The port follows `radCAD/operators/circle_tools.py` (`CircleTool_1Point`,
`CircleTool_2Point`, `CircleTool_3Point`, `CircleTool_TanTan`),
`operators/op_circle_tan_tan_tan.py`, `modal_core.py`, `tool_previews.py` and
`hud_overlay.py` in the supplied `12_28_26_tan3_Done` addon. Modal dispatch uses
`circle_tools.CircleTool_TanTan`, rather than the older standalone two-tangent
operator implementation.

No circle mode displays a compass: `draw_preview_1point` explicitly excludes
`CIRCLE_1POINT`. Center shows the center/radius dots and axis-colored radius.
2 Point shows the diameter endpoints and axis-colored diameter. 3 Point shows
two points and their chord, then the third point and second construction guide.
All three show black circumference previews and round black markers. The 3 Point
guides use the addon's translucent gray fallback. Tangent modes show cyan source
curves and a smooth cyan circle, black marker polygons, and yellow contact radii.
The overlay popover exposes colors, visibility, widths and marker controls.

## Native geometry differences

RCAD commits sampled mesh edges and reconstructs tangent input chains with
Catmull-Rom splines. This port constructs a full exact rational quadratic circle
using the native circular-arc constructor. Each circle has one exact support. Its edge and endpoint count follows the
Segments setting (32 by default), with equal angular spacing. The last edge closes
onto the first endpoint. Preview markers and committed endpoints use the same
source parameters, including optional tangency contacts. Changing the segment
count divides the exact curve without approximating it with straight edges. Construction points are not
extra topology vertices. Explicit seam snaps preserve native vertex identity; unrelated components are not welded.
World-space construction followed by coefficient transformation preserves circles
under nonuniform object transforms. The native commit path provides undo/redo.

Tangent input consists of connected selected native edges. Contacts are projected
onto their original rational supports over the selected parameter intervals.
Two-curve construction follows RCAD's medial-axis iteration. Three-curve construction
solves equal contact distances in the common plane; Tab switches nearest/farthest
contact configurations for external/enclosing solutions. Projection and tangency
solving are numerical, with bounded iterations; convergence is not guaranteed for
every arrangement. Unequal contact radii, endpoint-only contacts lacking tangency,
noncoplanar inputs and degenerate constructions cannot commit. No mesh fitting or
source trimming is performed. Replay stores the solved center/radius inputs for
tangent circles, so redo is independent of the resulting selection.

## Validation

`tests/python/bl_nurbs_circle.py` covers construction replay, complete circumference,
closure, tilted planes, nonuniform transforms and atomic rejection.
`tests/python/ui_simulate/test_nurbs_circle.py` covers staged placement, numeric input,
Backspace, cancellation, tangent input, curved supports, Tab, undo and redo. Screenshots
are saved under `/tmp/nurbs-circle-overlays` for inspection.

Typed parameter values stay pending until Enter accepts them. Typing and editing
the entry preserve the current geometry preview; Esc discards the pending value.
