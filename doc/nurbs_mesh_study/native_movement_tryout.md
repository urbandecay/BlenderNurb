# Native NURBS movement tryout

Use this fork's current executable and the generated native source file:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup \
  /tmp/blender-nurbs-move-ui/try-native-move.blend
```

The file reopens in Edit Mode with four source vertices and one NURBS face,
with one corner selected and raised. It contains no legacy splines.
Keep the default pivot and Global or Local orientation for this checkpoint.

Try G, an axis and a number, then Enter. Escape cancels; Ctrl+Z and Ctrl+Shift+Z
undo/redo. Click or B-box selection of the same corners uses the same source IDs
and gives the same geometry. X-Ray picking is currently required. F9 exposes
editable decimal X/Y/Z fields for native movement, preserving digits that ordinary
float fields cannot store. Plain F9 expressions are in Blender units; explicit
length units use the scene scale. Mesh F9 controls are unchanged.

Moving a corner updates its shared incident edges and NURBS support. It does not
edit the display mesh. Coefficients and rational weights are internal. This is
surface topology-vertex editing, not the general curve fit-point system.

Precise operator input is also available in native Edit Mode:

```python
bpy.ops.transform.translate(value_precise_z="0.12345678901234567")
```

The existing `value=(x, y, z)` interface remains a float boundary. The decimal
properties are a temporary precise operator interface; a public component and
fit-point API is not implemented yet.

Current supported geometry: partial linear edges, bilinear corners, complete
supported ruled boundary rows, and whole-component translations. Rational arc
and closed-circle row movement preserves the source basis/weights in kernel tests.
Partial curved endpoint movement and partial trimmed deformation reject until
shape/fit-point rules are chosen. Arithmetic rounding is measured and reported;
a move completely lost at source coordinate precision rejects. No fitting occurs.

Native E, Shift+D and deletion are not activated here. R/S, Normal/Gimbal movement,
snapping, proportional editing, multiple edit owners, fit-point editing, linked
selection and edge/face picking remain later work. Active-element pivots still need
native identity integration; use the default pivot. Mouse, orientation, project-unit
scale and pivot construction still have Blender float boundaries; the reported
coefficient-addition bound does not certify those inputs.

The ordinary Add NURBS commands still create the tagged prototype. For the
reported duplicate/extrude/delete repair, use
[the deletion tryout](prototype_deletion_tryout.md) and
[the duplicate/extrude tryout](prototype_duplicate_extrude_tryout.md).

The movement checkpoint is built and tested: 165 C++ tests, five application
CTest targets and eight isolated viewport runs pass. Twenty saved movement states
are checked against actual NURBS source. Actual F9 field editing, units, cancellation,
undo/redo and float-alias edits are covered. Screenshots are in
`/tmp/blender-nurbs-move-ui`; commands, failed attempts and precise limits are in
[the checkout audit](current_checkout_audit.md). This is not completion of Stage 1.
