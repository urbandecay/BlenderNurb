# Subdivision-shaped native NURBS bevel

## Result and use

The new **Surface Shape: Subdivision** setting fits native NURBS geometry to the
Catmull–Clark limit surface of the bevel cage. It addresses the remaining difference
between the circular/affine bevel grid and the user's smoothed mesh reference.

Use Ctrl+B in native NURBS Edit Mode, confirm, then open F9 and select **Surface Shape
→ Subdivision**. Blender remembers the setting for subsequent invocations and uses it
during dragging. **Circular** remains the default because these are different surface
shapes: subdivision can also change the retained faces and the circular-radius interpretation.
Existing stored geometry does not regenerate automatically.

## Construction

1. The existing native bevel plans widths, topology, and mesh-guided corner vertices.
2. The resulting cage is evaluated with Blender's CPU OpenSubdiv limit-surface evaluator.
3. Every shared edge gets one authoritative polynomial NURBS curve, fitted to limit-surface
   samples. Endpoint tangent directions are coordinated across opposite edges at regular
   vertices. Other edge controls are refitted with those endpoint directions fixed.
4. Regular regions use bicubic surfaces. Irregular regions use degree-seven surfaces.
   Boundary coefficients are copied or degree-elevated from the shared curves.
5. Surface interior fitting uses both target positions and constraints on the shared tangent
   planes. A Woodbury reduction reuses the small Bernstein position matrix instead of rebuilding
   a large sparse least-squares system per face.
6. Triangular cage faces need special parameterization. Their three PTex charts are piecewise,
   so fitting those rectangular coordinates directly produced artificial waviness. A smooth
   planar Coons chart from the shared boundaries is inverted onto the subdivision surface;
   NURBS fitting uses that smooth chart.

The output contains native NURBS curves, surfaces, and topology. It stores no live mesh modifier.
The temporary analytic cage's unused curve and surface resources are removed. Source geometry
is immutable throughout candidate construction, so a failed fit rejects atomically.

## Validation

`SubdivisionBevelMatchesMeshLimitCurvesAndSmoothSharedPatches` independently builds Blender's
actual BMesh bevel and evaluates that mesh with OpenSubdiv. It covers regular, slanted, and
stretched/thin boxes, each at 3, 8, and 13 segments.

The checks include:

- Vertex positions against the mesh limit vertices.
- Whole-curve samples at parameters absent from the fitting sample set.
- Opposite edge tangent alignment at regular vertices.
- Surface normal agreement across sampled shared boundaries.
- Shared boundary evaluation, tessellation, complete display evaluation and defined normals.
- Source immutability.

Across these nine cases, curve discrepancies were about 0.0000007–0.0000051 model units,
sampled surface discrepancies were below 0.000019 units, and sampled surface join angles
were below 0.017 degrees. The source boxes are about two model units across and the tested
bevel width is 0.2. These are sampled measurements, not certified global distance or G1 bounds.

The final bevel regression run passed **24 tests**. Three optional saved-file fixture tests
were skipped because their external fixtures were not configured. The regression set includes
the exhaustive skewed and tapered box edge-subset tests for Circular shape.

Viewport workflows passed for the new shape at 8 and 13 segments, including cancel, confirm,
undo/redo, mode switching, moving, and duplicating. An additional operator execution workflow
checked both surface choices from the same source and confirmed their different geometry.
A redo-panel workflow also verified that changing Circular to Subdivision recalculates the
bevel and remembers Subdivision for the next Ctrl+B invocation.

Logs:

- `/tmp/nurbs-subdivision-final-tests.log`
- `/tmp/nurbs-subdivision-cleanup-results.log`
- `/tmp/nurbs-subdivision-retained8.log`
- `/tmp/nurbs-subdivision-retained13.log`
- `/tmp/nurbs-subdivision-exec-final-pass.log`
- `/tmp/nurbs-subdivision-redo-verified.log`
- `/tmp/nurbs-subdivision-final-build.log`

Visual comparison at the same camera:

- Circular grid: `/tmp/nurbs-move-optimization/subdiv-grid-before-close/close.png`
- Subdivision NURBS: `/tmp/nurbs-move-optimization/subdivision-final-close2/close.png`
- Mesh with Catmull–Clark: `/tmp/nurbs-move-optimization/subdiv-grid-smooth-target/smooth-mesh.png`

## Limits and cost

The new shape currently requires a **closed cage with triangular or quadrilateral faces**.
Unsupported cage types reject without changing the source; the Circular path remains available.
The underlying bevel kernel's limits on curved input, higher-valence rounded corners, and
intersection/collision handling still apply. This implementation does not establish universal
parity with every BMesh bevel dispatch.

The fit approximates a subdivision surface. Its sampled deviation is reported separately from
the coefficient roundoff budget, and a candidate exceeding one percent of bevel width in the
sampled comparison rejects. The sampled checks are not global approximation certificates.

The new shape costs additional CPU time during construction. Representative viewport traces
recorded about 65 ms for fitting an eight-segment slanted box and 129 ms for thirteen segments;
complete interactive candidate preparation in those runs was about 146 and 292 ms respectively.
Those workflows ran concurrently, so these figures are stage observations rather than a
controlled performance comparison or measured frame rate. Geometry movement continues through
the existing transform and display caches. Further fitting-cache work remains possible.

Rejected intermediate experiments are retained only under `/tmp`: projecting subdivision paths
onto the old analytic surface caused intersecting curves and was removed from the production
path. The new version fits the surfaces and their shared curves to the same target.
