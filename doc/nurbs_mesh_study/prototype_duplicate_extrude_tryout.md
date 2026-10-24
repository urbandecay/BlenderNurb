# Duplicate and face extrusion tryout

Open the generated test file with the newly built executable:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup \
  /tmp/blender-nurbs-duplicate-region-ui/try-duplicate-extrude.blend
```

The file contains an original sheet at Z=0 and a separate copy that was moved
to Z=2, then extruded twice to Z=4. It opens in Object Mode. Press **Tab**; the
upper cap is selected. Try **E**, **Z**, **1**, **Enter** to extend it again.
Try **Shift+D**, **X**, **4**, **Enter** to make a disconnected copy of that cap.

To reproduce from scratch:

1. Add → Curve → Nurbs Line, then enter Edit Mode with **Tab**.
2. **E**, **Y**, **2**, **Enter** creates a connected NURBS sheet.
3. **A** selects the complete sheet.
4. **Shift+D**, **X**, **4**, **Enter** makes an independent copy.
5. **E**, **Z**, **2**, **Enter** creates a cap and side walls on the copy.
6. Repeat **E**, **Z**, **1**, **Enter**. The previous cap is replaced, so no
   internal cap remains at the join between extrusion steps.
7. **Ctrl+Z** / **Ctrl+Shift+Z** undo/redo these edits.

As with Blender meshes, **Escape** during E cancels the movement after extrusion
has created topology. Use **Ctrl+Z** to remove that extrusion step.

This exercises the existing tagged two-point/two-by-two prototype. Its NURBS
coefficients remain the source. Kernel services now supply fresh copy identities,
selection normalization and face-region boundary/replacement planning. Native
source geometry movement/extrusion and general curved fit-point editing remain
separate integration work; the native selection tryout still has those commands
blocked. Unsupported mixed selections report an error without changing the source.
Loose point duplication and extension are now supported by the deletion checkpoint;
see the [delete/continue tryout](prototype_deletion_tryout.md). This does not add general face editing to
older untagged higher-order NURBS surfaces.

Exact commands and results are in [the checkout audit](current_checkout_audit.md).
