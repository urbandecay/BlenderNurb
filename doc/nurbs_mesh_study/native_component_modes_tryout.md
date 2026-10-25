# Native Vertex, Edge and Face mode checkpoint

This checkpoint adds native component selection to the shared NURBS editing path.
Its mode-only checkpoint preceded F/connect/planar-fill. The subsequent
[Fill checkpoint](native_fill_tryout.md) implements that initial construction subset. Fit-point editing is later work. Ordinary Add NURBS creates the
restricted tagged prototype; the subsequent [source-entry checkpoint](component_mode_entry.md)
now makes these modes available on complete compatible tagged sources through the
header buttons and 1/2/3. Prepared files remain examples of broader native geometry.

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup \
  /tmp/blender-nurbs-component-ui/try-native-component-modes.blend
```

The file contains a six-face NURBS box in Edit Mode, with its top face selected.
The editable source is NURBS geometry with shared V/E/F identities; its legacy
splines collection is empty. The displayed triangles remain derived data.

- **1 / 2 / 3** switch Vertex / Edge / Face mode. Matching header buttons show
  the mode. **Shift+1/2/3** toggles combined modes; the last mode stays enabled.
- **Click** selects actual source vertices, edges or face interiors. Face picking
  respects the sampled trim holes. **Shift+click**, **A**, **Alt+A**, **B**,
  **L / Shift+L** and **Ctrl+L** act in the current component mode.
- **E, Z, 1, Enter** extrudes the selected top face through the existing shared
  region constructor. Mode and selection/history follow the cap. Native E still
  needs an explicit axis or supported Global/Local movement: automatic normal
  constraint is not implemented.
- G, Shift+D and Delete use the current component selection and shared owner
  commit. Active-edge centers use the curve parameter midpoint; active-face
  centers use the mean of unique boundary vertices. Cursor to Active and undo/
  redo are tested. Broader native transforms remain gated.
- With **X-Ray off**, picking and box selection check viewport scene depth,
  including other objects that obscure the native source. With X-Ray on, they
  can select through geometry. Box edges use fully enclosed edges first, then
  intersecting edges if none are enclosed. X-Ray face boxes use face markers;
  ordinary face boxes use visible face coverage, following the mesh distinction.

Selection mode is stored with the source selection. It survives undo, saving
and reopening. Entering a multi-object native edit session reconciles stored
modes to the active owner's mode; subsequent mode changes preflight every owner.
Blender's mesh selection settings remain unchanged. Python can read
`Curve.nurbs_selection_mode` and change it through
`bpy.ops.curve.select_mode(type='EDGE')` (or `'VERT'`, `'FACE'`), using the same
kernel service as the shortcuts. This is not a full component-data Python API.

Face markers use the source boundary-vertex mean when it lies in a preview
triangle, otherwise a nearby triangle centroid. This keeps a holed face's marker
out of its empty trim. Marker positions and sampled hit proxies are display
approximations; they are not new topology, fit points, support geometry or a
certified ray/trim intersection. Hits retain their source snapshot and IDs before
committing selection. Source coefficients are never reconstructed from a mesh.

This checkpoint has **211 passing C++ tests with no skips**, **14 successful
isolated viewport regressions**, and **five passing CTest targets**, including
37 unchanged NURBS Python cases. Sixteen saved component-mode states are checked
against full expected source geometry, modes, selection and history. The Face
mode header, tint and markers were visually inspected. Exact commands and the
corrected build errors are in [the checkout audit](current_checkout_audit.md).

Still gated at this historical checkpoint: F, native creation/migration, fit-point storage
and deformation, lasso/circle selection, complete near-plane clipping for picking,
certified curved-trim intersections, source normal frames, R/S, snapping,
proportional editing, multi-owner movement and full component bindings. Stage 1
remains incomplete. The user chose a smooth NURBS patch for four noncoplanar corners. General curved/nonplanar loops still need their own construction rule.


The source-entry checkpoint supersedes the prepared-file-only limitation. It
checks normal Add → header Edge → E → Face/Edge/Vertex picking and drawing,
Shift-click combined modes, source-kind undo/redo, and an existing rotated/scaled
six-face prototype. General untagged source migration and fit-point deformation
remain separate work. See the latest checkout audit for current test totals.
