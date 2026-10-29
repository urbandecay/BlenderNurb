# Native ellipse, polygon and rectangle drawing

The NURBS toolbar now includes Ellipse, Polygon and Rectangle. Each button exposes
its construction modes in the tool header, following radCAD's operators in
`12_28_26_tan3_Done/radCAD/operators/`.

| Tool | Mode | Placement sequence |
| --- | --- | --- |
| Ellipse | Radius | Center, major radius, minor radius |
| Ellipse | Foci | First focus, second focus, point on ellipse |
| Ellipse | Endpoints | Major axis endpoints, minor radius |
| Ellipse | Corners | Opposite bounding rectangle corners |
| Polygon | Center–Corner | Center, vertex |
| Polygon | Center–Tangent | Center, side midpoint (apothem) |
| Polygon | Corner–Corner | Two adjacent vertices |
| Polygon | Edge | Vertex, opposite side midpoint; odd number of sides |
| Rectangle | Center–Corner | Center, corner |
| Rectangle | Corner–Corner | Opposite corners |
| Rectangle | 3 Point | First corner, adjacent corner, signed height |

## Input and navigation

- Ellipse R enters the minor radius. D enters the major diameter for Radius and
  Endpoints. Foci R sets the focus distance during the first stage. K retains
  foci as native vertices.
- Polygon R sets its construction distance. Center–Tangent also accepts A for
  the apothem; Corner–Corner and Edge accept L for the construction span.
- S enters ellipse segments or polygon sides. Wheel adjusts these divisions;
  Edge polygons change by two to preserve the odd side count.
- Rectangle X and Y enter full width and height. Center mode applies half of
  these values on each side of its center. The signed side selected at entry
  stays fixed. Shift toggles a square for the two diagonal constructions.
- P toggles the perpendicular construction plane. Ellipse/polygon X and Y
  select the vertical plane orientation. L toggles plane locking when it is
  not being used as a polygon size shortcut.
- Typed values remain pending until Enter; clicking while typing does not
  commit them. Esc discards the entry. A subsequent click or Enter completes
  placement once the last construction stage is valid.
- Backspace returns to the previous stage. Esc or right click cancels the
  construction. Middle mouse orbits; Shift with middle mouse pans. Ctrl with
  the wheel zooms for tools whose wheel changes segments.
- Snapping uses the same native hover and active-tool feedback as arc and
  circle drawing. Ctrl toggles the scene snap setting temporarily.

## Geometry and overlays

Ellipses use an exact closed rational quadratic NURBS support, with native edges
and shared endpoints at the displayed segment divisions. Preview markers and
committed geometry use the same angular divisions. Polygon and rectangle sides
are native degree-one curves with shared corner vertices and a closed final edge.
Construction happens in world coordinates and is transformed into object space
at full double precision, including when the object has nonuniform scale.

Overlays follow the addon: black curves and endpoint markers, axis-colored
construction guides, full major-axis references for ellipse Radius/Endpoints,
green focus guides, and a green bounding rectangle for ellipse Corners. The
shape overlay popover exposes guide color, axis colors, brightness, marker size,
and measurement visibility. Measurements and pending input appear in the header.

## Validation

`test_nurbs_shapes.construction_modes` exercises all eleven modes through native
workspace tools, checks committed endpoint/edge counts and ellipse equations,
and covers orbit, pan, undo/redo, pending rectangle dimensions and odd polygon
side adjustment. The existing numeric-input regression covers the shared arc,
circle, line and interpolation input paths.
