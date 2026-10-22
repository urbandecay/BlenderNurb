# NURBS edge and face extrusion

This is a first mesh-like extrusion slice for the new **Nurbs Line** surface
object. It keeps Blender's NURBS control net as the geometry source and uses
the existing curve editor, transform, and surface evaluation paths.

## Behavior

- **Add > Curve > Nurbs Line** creates a straight degree-one NURBS line in an
  `OB_SURF` object. Its two endpoints and edge receive topology IDs.
- Extruding both selected line endpoints creates a separate bilinear NURBS
  face. The original line is replaced by that face, and the original line edge
  remains one of its boundaries. Only the new edge stays selected for the
  following transform.
- Extruding one selected endpoint of a line creates a new NURBS line segment
  from the old endpoint. The original line remains, and the segment's starting
  point shares the endpoint's topology ID. Its new endpoint is selected.
- Extruding a selected boundary edge of a NURBS face creates a separate face
  patch attached along that edge. Select the edge's control points. The old
  face remains and the new outer edge is selected. Repeated extrusion
  therefore builds adjacent patches instead of extending one control net
  through the shared edge.
- Extruding a fully selected NURBS face control net makes a translated cap and
  one side patch per boundary edge. Shared edges inside the selected region do
  not get walls. The original face region remains as the base.
- Blender's transform operator moves the newly created control points. Surface
  evaluation, drawing, undo, and `.blend` persistence use Blender's existing
  systems.
- Shape-keyed objects report an error for this topology extrusion path; their
  key data is not yet updated by the new geometry creation code.

## Current limits

Topology IDs describe which boundary elements were shared when the extrusion
was made. The new path creates coincident boundary control points in separate
NURBS patches; ordinary curve transforms do not yet synchronize every future
edit across those patch boundaries. So the extrusion result is connected at
creation time, while ongoing seam editing still needs a shared-vertex editing
path. Face selection uses the existing control-point workflow; this change
does not add a separate face-selection mode. Vertex extrusion is supported for
the original two-point line; arbitrary surface-vertex extrusion, cyclic
surfaces, mixed face and edge extrusion, and general BRep operations are not
covered here. Untagged legacy surfaces keep Blender's previous extrusion
path.

## Code and checks

The line factory is in `source/blender/editors/curve/editcurve_add.cc`. The
topology-aware edge and face extrusion code is in
`source/blender/editors/curve/editcurve.cc`. The small topology IDs live on
each `Nurb` in `source/blender/makesdna/DNA_curve_types.h`.

`tests/python/bl_nurbs_line.py` checks line creation and placement, line edge
extrusion, endpoint extrusion, face edge extrusion, whole-face extrusion,
evaluated surface area, transforms, independent splines, weights, file
persistence, undo/redo, and unrelated primitive factories. Run it with the
compiled Blender:

```sh
blender --background --factory-startup --python-exit-code 1 \
  --python tests/python/bl_nurbs_line.py
```
