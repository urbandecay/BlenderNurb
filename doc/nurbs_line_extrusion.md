# NURBS line to surface extrusion

This is the first line-to-sheet interaction slice of the mesh-like NURBS
workflow. It uses Blender's existing native surface representation and editor
operators, rather than a mesh proxy or Python-owned geometry.

## Behavior

- **Add > Curve > Nurbs Line** creates a straight, two-control-point,
  degree-one NURBS span. The object uses Blender's surface-capable `OB_SURF`
  type from creation, including while it is still a line.
- With both endpoints selected in Edit Mode, **E** duplicates the selected row
  in the V direction. Moving it produces a filled tensor-product NURBS sheet,
  with degree one in U and V. The original row stays in place; only the new
  row remains selected. The same object and spline own the result.
- Another **E** on the selected outgoing row extends the sheet. Selecting just
  one endpoint while the object is still a line extends the line instead.
- Placement, radius, native transforms and constraints, undo, tessellation,
  drawing, and `.blend` persistence use the existing Blender surface pipeline.
  Evaluation creates the display mesh from the authoritative NURBS net.
- Other primitive factories retain their existing geometry.

Lines created before this change as `OB_CURVES_LEGACY` objects keep their
existing curve-edit behavior. Add a fresh **Nurbs Line** to test this slice.
The Outliner identifies a new line as a surface-capable object.

## Implementation boundary

The line factory belongs beside Blender's existing curve/surface primitive
factories in `source/blender/editors/curve/editcurve_add.cc`. Extrusion reuses
`ed_editnurb_extrude_flag`, selected-point transformation reuses Blender's
transform operators, and evaluated surface drawing uses the existing BKE
surface pipeline. No object-type conversion is performed during extrusion.

This slice retains Blender's existing float control-point and knot storage.
It does not establish the broader double-precision CAD/BRep architecture,
arbitrary face/edge topology, caps, or face-to-solid extrusion described in
`nurbs_mesh_editing_study.md`. A selected control-net row is not yet a general
CAD edge or a mesh face-selection mode.

## Regression checks

`tests/python/bl_nurbs_line.py` checks creation, filled evaluated area, outgoing
row selection, repeated extrusion, endpoint-only extrusion, native transforms,
independent splines, rational weights, save/reload, undo/redo, and preservation
of the other primitive factories. Run with the compiled Blender:

```sh
blender --background --factory-startup --python-exit-code 1 \
  --python tests/python/bl_nurbs_line.py
```

The native surface extrusion was first checked independently against a
two-point surface fixture: it produced a 2-by-2 control net, selected only the
outgoing row, and evaluated to a filled sheet with the expected area.
The compiled primitive integration checks must pass before this change is
reported ready for use.
