# FreeCAD Boolean capability target

This is the acceptance scope, not a statement of completed compatibility.
The current Solid operator does not have FreeCAD Boolean parity.

## Required operation families

Union/fuse, subtraction/cut, intersection/common, section, general fuse and
the fragment/slice/XOR workflows derived from the shared partition. Include
multiple operands, repeated operations, compounds, closed solids and the
sheet/wire operations supported by FreeCAD Part.

## Required geometry and topology

- Keep surviving planes, cylinders, cones, spheres and other analytic surfaces
  analytic. Include toroidal, swept, revolved, offset, Bezier and BSpline forms.
- Represent general intersection curves with the required analytic or spline
  geometry and a measured approximation tolerance. FreeCAD/OCCT itself uses
  tolerance-controlled approximations for general surface intersections.
- Store per-face UV trim curves, periodic seams, singular/degenerate boundary
  occurrences and persistent vertex/edge/face tolerances.
- Share VV/VE/EE/VF/EF/FF intersection data across both operands; reconcile
  common vertices/edges and coincident surface regions before assembly.
- Rebuild wires, faces, shells and solids with orientation and cavity handling.
- Return Modified/Generated/Deleted ancestry for source selections and references.
- Preserve inherited face materials and UV maps, and define UVs for generated faces.
- Reject unsupported or invalid cases transactionally; tests must distinguish
  an intentional empty result from failure or a silently missing operand.

## Current implementation boundaries

The native body entry point has the planar Boolean path plus union, intersection,
subtraction and reverse subtraction with a supported curved body and a convex
planar operand. It uses joint plane partitions, closed analytic cells, planar
interface overlays and boundary assembly. General curved/curved pairs remain
unsupported there. Existing
analytic cylinder/prism special cases remain separate from that general body
path. Float/Exact/Manifold operate on mesh representations.

The experimental OCCT conversion bridge has been removed. The native solver
does not call or link OCCT. It includes one adapted edge-partition helper; see
`BOOLEAN_PORT.md` for its source and license. That helper does not provide a
general OCCT Boolean backend.

The native Solid model also has no per-face material index or UV-map fields.
Display regeneration keeps the object's material slots, but currently drops
per-face material assignments and UV maps for native Solid Booleans.

Native helper implementations now include bounded Plane/Cylinder face cuts,
periodic cylinder chart partitioning, geometry-preserving face sewing, analytic
point classification for Plane/Cylinder/conic-extrusion bodies, cubic and
rational-quadratic edge/plane intersections, and standalone rational BSpline
curve/surface evaluation. These helpers do not establish full operator support.
The standalone spline data is not yet a stored Solid geometry kind.

The only adapted OCCT source routine currently present is the edge interval
partition in `boolean_pave.cc`; see `BOOLEAN_PORT.md` for attribution. Other
native implementations follow the needed geometric stages but are independently
written. No OCCT Boolean solver is linked or run.

## Required comparisons and regression cases

Compare the same native geometry and set operations with FreeCAD: primitives,
nonparallel curved bodies, oblique and coplanar caps, nested cavities, thin
features, exact/near tangencies, overlapping domains, periodic seams/poles,
trimmed splines, sheets/wires, compounds and long operation chains. Check
topology, volume/material membership, analytic type retention, tolerance growth,
selection history, source immutability on error and persistence after reload.
Passing the current limited regression suites is not a parity claim.

## Primary reference sources

- [FreeCAD Boolean commands](https://github.com/FreeCAD/FreeCAD-documentation/blob/main/wiki/Part_Boolean.md)
- [FreeCAD split/general-fuse workflows](https://github.com/FreeCAD/FreeCAD/blob/main/src/Mod/Part/BOPTools/SplitAPI.py)
- [FreeCAD Part Fuse input scope](https://github.com/FreeCAD/FreeCAD-documentation/blob/main/wiki/Part_Fuse.md)
- [OCCT Boolean specification](https://dev.opencascade.org/doc/overview/html/specification__boolean_operations.html)
- [OCCT face intersection and approximation API](https://github.com/Open-Cascade-SAS/OCCT/blob/master/src/ModelingAlgorithms/TKBO/IntTools/IntTools_FaceFace.hxx)

OCCT is a study reference only, not an application dependency. Relevant
algorithm families for comparison include
BOPDS/BOPAlgo/BOPTools/IntTools and IntPatch/IntSurf/IntWalk/GeomInt/ApproxInt,
as well as BRepClass/BRepClass3d and the geometry/math routines they require.
