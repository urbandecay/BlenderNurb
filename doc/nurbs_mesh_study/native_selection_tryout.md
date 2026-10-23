# Native NURBS selection tryout

Open the prepared sheet with this fork's executable:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup \
  /tmp/blender-nurbs-edit-ui/try-native-selection.blend
```

The file opens in Object Mode. Press **Tab** to enter Edit Mode. X-Ray is already
enabled. The sheet has four source vertices, four shared boundary edges and one
NURBS face; the bottom two vertices are selected.

Try these normal Blender controls:

* **Alt+A**: deselect everything.
* **Click**, then **Shift-click**: select individual corners.
* **B**, then drag: box select corners.
* **A**: select all corners.
* **Ctrl+Z / Ctrl+Shift+Z**: undo/redo selection.
* **Tab**: leave and re-enter Edit Mode with selection preserved.

The viewport statistics show source vertices, edges and faces. Display triangles
are separate. Changing the preview density from 8 to 13 is included in the
automated click/box test, followed by a saved-file check of actual source IDs.

This tryout covers surface-corner selection in X-Ray. Native geometry transforms,
extrusion, curve fit-point editing, linked selection and edge/face picking modes
are still being implemented. Use the supplied file for this native path; the
ordinary Add NURBS commands currently create the existing prototype.

For a Python check that the prepared object uses the new source, inspect
`bpy.context.object.data.is_native_nurbs`. It is read-only and should be `True`.
Native element bindings are separate work.

The reproducible viewport test is
`tests/python/ui_simulate/test_nurbs_native.py`. Build/test commands, failures,
corrections and evidence are recorded in
[the checkout audit](current_checkout_audit.md).
