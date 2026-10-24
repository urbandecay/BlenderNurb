# Native NURBS duplication tryout

Open the prepared native source using this fork's executable:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup \
  /tmp/blender-nurbs-duplicate-ui/try-native-duplicate.blend
```

It opens in Edit Mode with two independent NURBS faces, each with a hole. The
right face is selected. Statistics show 8/16 vertices, 8/16 edges and 1/2 faces
selected. These are source components; the legacy spline collection is empty.

Try **Shift+D**, **Z**, **1**, **Enter**. The selected face gets an independent
copy above it. **Escape** cancels Move while retaining the duplicate, as in mesh
editing. **Ctrl+Z / Ctrl+Shift+Z** undo/redo. **G** moves the selected copy. Native
F9 movement fields preserve binary64 input precision; the Duplicate popup uses
the same fields. The Select menu and L / Shift+L / Ctrl+L use the source selection
services described in [the selection tryout](native_selection_tryout.md).

Duplication works for source topology vertices, edges and complete selected
faces. Shared edges/vertices within a copied group are copied once; the result
has fresh identities independent of the originals. Rational coefficients,
weights, knots, trims and face orientation are copied unchanged. Movement then
uses the existing NURBS translation service and its explicit error reporting.

This native path currently requires a single edit object for Duplicate/Move and
vertex-mode source selection. Normal native creation/migration, extrusion,
edge/face picking, curve fit-point editing and broader transforms remain gates.
The ordinary Add NURBS commands still create the tagged prototype. Hole-linked
selection remains restricted when the pending connectivity choices disagree;
selecting all boundaries explicitly for duplication is unambiguous.

The UI regression and saved-source validator cover click/linked and box
selection, point/edge/face copies, cancellation, undo/redo, precise Python macro
execution, rational circles, holes and this tryout. Exact commands, counts and
limitations are in [the checkout audit](current_checkout_audit.md).
