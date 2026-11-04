# Independent analytic solid workflow

This replacement implements a new kernel in `source/blender/solid`. It uses double precision
lines, conics and rational quadratic profiles, plane, extrusion, ruled and affine sphere
surfaces, vertices, edges, oriented boundary uses and faces. It has no dependency on Blender,
the NURBS library, or BMesh. Circles
and ellipses are stored as `center + u*cos(t) + v*sin(t)`, with angular edge intervals. Lines
are `origin + u*t`. Rounded junction profiles store three control points and weights directly, without knot
vectors, general surface grids, fitting, or conversion through the existing NURBS kernel.

A Mesh datablock hosts an independently serialized solid payload and a replaceable display
mesh. It is a viewport adapter, not the authoritative source. Display points and faces retain
source identities; sampling points and internal display edges have no editable source identity.
The host retains immutable source states and writes source bytes at file save, not each mouse
motion. Mesh edit undo also captures the independent source. Generic mesh modeling operators
are excluded from solid editing because they would only change the display proxy.

The existing drawing event code is shared. Line, Arc, Circle, Ellipse, Polygon and Rectangle
keep their construction modes, numeric input, constraints, markers, tool properties, navigation
and tangency solver. A neutral source query view and independent analytic constructors replace
NURBS geometry access for solid objects. Legacy NURBS objects retain their own backend. Freeform
spline/control-net authoring remains in the separate NURBS workflow.

## Use

1. In Object Mode, choose **Add > Solid > Sketch**, Line Sketch, Rectangle Sketch or Circle Sketch.
2. Draw and select one closed planar profile with the existing viewport drawing tools.
3. Press **F** to create a plane or **E** to extrude directly. Move the mouse or type a distance.
4. Confirm with Enter/left click, or restore the original source with Escape/right click.
   The outgoing cap is selected in Face mode. Press E again to extend that face outward.
5. In Vertex mode, select a vertex and press **E** to extend an edge from it. Select an open
   edge or chain and press **E** to sweep a surface. Use **X/Y/Z** to constrain the translation,
   type a distance, or move freely in the viewport plane. Ctrl snaps the translation.
   Confirming an extrusion while a sketch drawing tool is active returns to Select Box,
   including extrusion of the starter Line Sketch into a face.
6. Use **M** for merge at center/first/last/cursor, by distance, or collapse. The active vertex
   and vertex selection history supply the first/last targets. The cursor target is converted
   into the object's local coordinate system.
7. Use **Ctrl+B** for an edge bevel or **Ctrl+Shift+B** for a vertex chamfer.
   **Segments** in the redo panel (or the mouse wheel during bevel) uses 0 for a flat chamfer,
   1 for an exact curved fillet without extra source divisions, and higher values for divided
   curved source patches. Width is measured along the adjacent faces, as in the native NURBS
   bevel; it is not a constant-radius prescription on skewed geometry.
   The bevel redo panel offers **Corner Join: Sharp/Patch** for flat caps where three or more
   selected edge chamfers meet. Fillets use shared conic profiles, ruled strips and affine spherical corner surfaces.
   Wire corners with two straight edges and convex planar closed solids are supported. Width
   is the offset along adjoining faces for an edge chamfer. Multiple adjoining edge cuts
   intersect through the same half-space construction, including corners of higher valence.
8. Use **X/Delete** for source vertex, edge, face or only-face deletion. **F** can also connect
   two source vertices or close and fill an open planar boundary chain.
9. Use **Shift+D** to duplicate selected vertices, edges or faces and move the copy. The
   copied topology and analytic supports have independent identities. Shared boundaries within
   the copied selection remain connected. X/Y/Z constraints and numeric entry use the standard
   move tool. Escape cancels movement and leaves the duplicate at its original position.

Move, rotate and scale work on planar body components. Their straight edges and support planes
are reconstructed from edited source coordinates. A polygon that becomes nonplanar is split
into exact planar source triangles. This is an explicit source topology change; viewport
sampling remains derived data. These tools participate in source undo/redo and file storage.

The display of a single-edge cylinder uses two planar polygons and 64 wall strips at the
current display resolution. Its source has two plane faces and one analytic curved wall.
Changing display sampling does not increase source topology.

## Current boundaries

This is an analytic sketch and component-editing foundation, not a complete general CAD kernel.
It does not implement solid booleans, general intersections/trimming, feature history, constraints, or inference
of nested sketch loops. Planar face hole display is explicitly unsupported. Structural source
validation and oriented edge-incidence checks are not a general geometric self-intersection
proof. Collisions with unrelated source bodies are not certified.

Sketch extrusion supports either normal direction. Extending an attached cap supports the
outward direction; inward cuts require intersections and trimming. Affine transforms preserve
complete analytic components. Straight sketch endpoints and planar solid components can move
independently. Partial conic/curved-surface deformation without an explicit construction rule
is refused. Merge can weld coincident conic endpoints but does not bend an arc to a new endpoint.
Solid bevels check the selected connected component, so a separate open sheet in the same
object no longer blocks beveling a closed body. The selected component still needs a closed,
convex planar boundary. Fillets support selected straight edges on convex planar bodies with
three incident faces at each affected corner. Face widths are balanced across the selection;
adjoining strips share one rational conic profile at a two-edge junction. There is no separate
setback wedge at that junction. Three-edge junctions use affine spherical octants. The source
construction is implemented in the independent solid kernel and follows the native NURBS
bevel's stations and shared profiles. Width follows face-offset semantics; skewed corner
transitions are not necessarily constant-radius cylinders or spheres.

The subdivision count divides the same analytic geometry, while viewport sampling remains
independent. Each strip segment has one selectable source face. Higher-valence rounded corners,
vertex-only fillets, concave blends and blends on existing curved faces need additional
constructions. Source storage version 5 adds rational quadratic curves and ruled supports,
and reads versions 1 through 4, including legacy cylinder and setback blend supports. Existing
fillets keep their saved geometry; undo and recreate an old fillet to use the new construction.
Merge by Distance welds nearby vertices but does not create a missing face
or remove an overlapping face. These limits are
reported instead of deforming a display mesh or falling back to NURBS.

## Validation

The standalone `solid_analytic_test` target links only the new kernel. It exercises analytic
box/cylinder construction, both extrusion directions, cap extension, body edge incidence,
display winding/volume, byte-stable source serialization and corrupt input refusal. Its small
kernel-only benchmark does not measure Blender viewport performance.

The replacement has been built and installed in `../build_linux/bin/blender`. Restart Blender
to use the new executable; an already running process continues using its previous build.

Validation of the component-editing update on November 3, 2026:

- Both standalone kernel CTests passed. The editing suite tests all 4,095 nonempty selections
  of a cube's 12 edges, vertex bevels on valence-four corners, closed-body incidence and volume,
  merge/collapse/weld, component movement and planar splitting, wire bevel, repeated open-edge
  extrusion and source deletion. Its AddressSanitizer/UndefinedBehaviorSanitizer build passed
  without diagnostics.
- All 15 host integration tests in `tests/python/bl_solid_sketch.py` passed, including source
  transforms, copy isolation, serialization/reopen, repeated extrusion, vertex/edge extrusion,
  planar body movement, merge, edge/vertex bevel and deletion.
- Twelve analytic viewport tests in `tests/python/ui_simulate/test_solid_sketch.py` cover
  the drawing construction modes, deferred numeric entry, arc input, tangent circles,
  snap/anchor reuse, Backspace, sketch creation, extrusion, axis/free movement, merge/delete
  menus, both bevel shortcuts, undo/redo and modal cancellation.
- Four native NURBS viewport regression cases passed: shape construction, deferred numeric
  input, arc workflow and line-to-sheet. The analytic shape/numeric/arc cases reuse the same
  interaction checks with the independent backend selected.

The final combined viewport regression run completed all 16 cases with zero failures on the
installed update, including free mouse extrusion, axis input, both bevel shortcuts and undo.

The subsequent Shift+D addition passed all 16 API tests, including vertex, edge, planar-face
and exact circular-face duplication followed by translation. Three additional viewport tests
passed for vertex, edge and face duplication, axis/numeric movement, undo/redo, and cancellation
of movement while retaining the copy. The installed Blender includes this addition.

The shared drawing code also fixes construction Backspace being consumed by inactive numeric
input, and keeps drawing input streams in the executable's C++ runtime to prevent a locale
destruction crash seen in numeric/foci input on both backends.

## Extrusion performance

`tests/python/solid_extrude_benchmark.py` ran unchanged against the saved previous NURBS-backed
Solid executable and the initial independent replacement on November 2, sequentially on the
same machine with factory defaults. These figures predate the component-editing update.
Each profile used 30 iterations, discarding five warmups. Timing includes extrusion operator
dispatch, source construction, display publication, dependency graph evaluation and retrieval
of the evaluated mesh. Sketch creation is excluded. This is a background integration benchmark,
not a measurement of interactive mouse-to-screen latency or a general CAD performance claim.

| Profile | Previous median | Independent solid median | Previous p95 | Independent solid p95 |
| --- | ---: | ---: | ---: | ---: |
| Rectangle | 2,964.41 ms | 0.61 ms | 3,018.61 ms | 0.97 ms |
| Circle | 2,925.81 ms | 0.79 ms | 2,948.09 ms | 1.66 ms |

| Profile | Previous display vertices / faces | Independent display vertices / faces |
| --- | ---: | ---: |
| Rectangle | 390,356 / 780,300 | 8 / 6 |
| Circle | 363,404 / 726,396 | 128 / 66 |

The previous path's default surface tessellation dominated these examples. The new path stores
analytic source geometry and builds a small display directly; the source cylinder still has
only three faces. The display counts describe each implementation's defaults, not matched
tessellation settings. Neither benchmark establishes robustness for unimplemented operations
such as fillets or booleans.
