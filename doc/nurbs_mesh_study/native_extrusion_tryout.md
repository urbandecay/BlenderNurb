# Native NURBS extrusion and deletion tryout

This checkpoint connects the shared NURBS construction/deletion services to
Blender's existing Curve/Surface operators. Editable source remains the native
double NURBS payload; the legacy splines collection stays empty. These prepared
files exercise that payload. The Add NURBS menu still creates the restricted
tagged prototype; general creation/migration and fit-point editing are later gates.

## Open a prepared source

Use the rebuilt executable:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup \
  /tmp/blender-nurbs-native-extrusion-ui/extrude-linked.blend
```

This file opens in Edit Mode with the top face's four source vertices selected.
It contains a six-face box. Press **E, Z, 1, Enter**: the selected face continues
into a connected second level with ten faces, without an internal cap. Press
**X, Enter** to delete its top vertices: the lower box remains with an open roof
(five faces), matching mesh vertex deletion. In front view with X-Ray, box-select
that roof's rim, then **E, Z, 1, Enter** to extend it again.

`/tmp/blender-nurbs-native-extrusion-ui/try-native-extrude-delete.blend` is the
saved result of that sequence, with the new rim selected: twelve shared vertices,
twenty shared edges, nine faces and an open top. Its viewport screenshot has been
visually inspected. A screenshot establishes appearance; the independent
saved-source checks establish topology, coefficients and selection.

Other generated native source files in the same directory:

| File | Exercise |
|---|---|
| `extrude-click.blend`, `extrude-box.blend`, `extrude-linked.blend` | Identical source geometry from three ways of selecting the same face. Active/history data preserves each gesture's meaning. |
| `extrude-line.blend`, `extrude-line-repeat.blend` | A source line becomes a connected sheet, then another connected sheet. |
| `extrude-two-face-region.blend` | Two adjacent source faces extruded as one region, with six boundary walls and no wall or rail on the internal selected seam. |
| `extrude-trimmed-arc.blend`, `extrude-trimmed-arc-repeat.blend` | An interior-trimmed rational arc swept twice; original rational weights, knots and trim ranges remain source data. |
| `extrude-circle.blend` | A periodic rational cylindrical face extruded with shared seam identities and no seam wall. |
| `extrude-hole.blend` | A holed face gets walls on both its outer and inner boundaries. |
| `duplicate-then-extrude.blend` | Shift+D makes an independent source copy; E extrudes that copy. |
| `extrude-cancel.blend`, `extrude-cancel-then-move.blend` | Esc cancels the Move part of E while retaining its new topology, as in mesh editing; a later G moves the cap. |
| `extrude-undo.blend`, `extrude-redo.blend` | Undo/redo restores topology, source coordinates and cap selection. |
| `delete-only-face.blend`, `delete-edges.blend`, `delete-faces.blend` | Native deletion modes through the existing Python operator. |

## Supported interaction and limits

- **E** reuses `curve.extrude_move`: the native editor adapter asks the kernel for
  a zero-displacement region candidate, remaps selection/history to the cap and
  commits once. The existing native **G** adapter then moves that source cap.
- Use **X/Y/Z** constraints or the supported Global/Local movement orientations.
  Automatic face-normal constraint is not implemented. Normal/Gimbal orientation,
  R/S, snapping, proportional editing and multi-object movement remain explicit
  gates. The editable zero sweep is allowed to be singular until moved.
- Native **X/Delete** offers Vertices, Edges, Faces and Only Faces using the same
  kernel incidence rules. Only Faces retains its boundary wires. Surviving IDs and
  the identity counter remain stable. Unreferenced coefficient resources are
  retained; resource garbage collection is not implemented.
- Delete preflights every native edit owner before changing any. E and Shift+D
  require a single edit owner because native modal Move still has that limit.
  Mixed legacy/native component edits reject before either source changes,
  whichever owner is active.
- Selection still uses source topology vertices and X-Ray picking. Independent
  edge/face picking and general fit-point handles are not enabled. These are not
  the old control-vertex arrays exposed as new handles.
- No fitting, tolerance-based welding or display-mesh edits occur. A measured
  pre-existing endpoint discrepancy covered by declared source tolerance is
  explicitly reported. Unsupported geometry or discrepancy beyond tolerance
  rejects the candidate. Movement retains its separate numerical error contract.

The Python operator hooks are the existing `curve.extrude_move`, `curve.extrude`
and `curve.delete`; this is not a public Python API for the full native component
model. Full arbitrary NURBS deformation and solid/trim validity certification
remain separate kernel work. Stage 1 is still incomplete.
