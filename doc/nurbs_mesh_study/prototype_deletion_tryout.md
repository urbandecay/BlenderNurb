# Delete, then continue NURBS editing

Open this file in a fresh instance of the rebuilt fork:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup \
  /tmp/blender-nurbs-delete-ui/try-delete-and-extrude.blend
```

It contains the result of creating a cube, extruding its top again, deleting
the new top vertices and extruding the surviving open rim. It opens in Object
Mode. Press **Tab** to continue editing; the upper rim is selected.

The current tagged prototype uses vertex selection. **X → Vertices** removes
those vertices and their incident edges/faces, following mesh vertex deletion.
It preserves the remaining boundary and any unrelated loose edges or points.
To continue after deletion, select the surviving rim in X-Ray and use **E**.
Deleting vertices does not fill an open rim automatically.

The regression uses real X, box selection, E, axis/numeric input and undo/redo.
It also deletes opposite corners of a sheet, then draws, moves and extrudes the
two remaining loose points. These are persistent source vertices with real IDs.

For files already affected by the older deletion bug, the compatibility adapter
can recover a narrow known case: a collapsed patch record still names a shared
edge owned by an intact face, and its exact geometry and attributes agree with
that declared edge. It reports how many redundant remnants it recovered.
Coincidence alone is insufficient; ambiguous, periodic or differing source data
is rejected rather than welded or fitted. The strict geometry importer remains
strict. A pre-fix reproduction file is available at
`/tmp/blender-nurbs-delete-broken.blend`; continued extrusion was verified in a
fresh process, saving the recovered result separately at
`/tmp/blender-nurbs-delete-recovered.blend`.

This is the tagged point/line/patch compatibility path. Native source movement
and the full curve fit-point workflow remain under development. See the
[checkout audit](current_checkout_audit.md) for exact build/test evidence.
