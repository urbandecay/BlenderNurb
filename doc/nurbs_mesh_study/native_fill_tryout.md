# Native F / Make Edge-Face checkpoint

Native component modes now feed the shared Fill constructor. **F** and
`bpy.ops.curve.make_segment()` use the same kernel service. Right-click also
provides **Make Edge/Face** for native sources. Ordinary Add NURBS still creates
the restricted tagged prototype; use these prepared native files for this stage.

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --factory-startup \
  /tmp/blender-nurbs-fill-ui/fill-bent-extrude.blend
```

This file opens in Edit Mode, Face selection, with the upper face selected.
It contains a box whose lower and upper faces bend through four noncoplanar
corners. Try **E, Z, 1, Enter**, undo/redo, and **1 / 2 / 3**. The editable source
has shared NURBS vertices, edges and faces; its legacy splines collection is empty.

Other saved examples in `/tmp/blender-nurbs-fill-ui/`:

- `fill-planar-edge.blend`: planar four-edge Fill, still in Edge mode.
- `fill-bent-edge.blend`: the four-corner bending face before extrusion.
- `fill-connect.blend`: two disconnected vertices connected by F.
- `fill-triangle.blend`: three-sided planar trim on a NURBS support.
- `fill-merge.blend`: two coplanar faces merged by F, keeping outer identities.

## Implemented behavior

- Two selected vertices connect with a new NURBS line and shared endpoint IDs.
  An existing unique edge is reused. An existing single face is unchanged.
- One closed boundary fills; an open chain with at least three vertices closes.
  A chain and one independent vertex can form one boundary. Loose vertices can
  be ordered around a supported projection, without coordinate welding.
- Four straight sides form a bilinear NURBS support through their corners.
  Noncoplanar corners create the bending face requested by the user. A simple
  convex planar quad also uses this chart. This differs from a mesh quad's
  triangulated shape. It does not impose tangent continuity across neighbors.
- Other supported planar boundaries, including concave polygons, use one planar
  NURBS support with a real trim boundary. Source triangles are not substituted
  for it. A concave planar quad uses this trimmed construction.
- Supported closed planar rational profiles retain their existing 3D curves,
  knots, weights and IDs. P-curves preserve rational parameterization. The circle
  cap test shares the cylinder's original circular edge; no circle fitting occurs.
- F on a compatible coplanar face region replaces its faces with one trimmed
  face, removes unused interior topology, and retains outer edges/vertices.
  Face orientation and a common material index survive the merge.
- New components become selected in the current mode. Face-region Fill makes
  the new face active in exclusive Face mode. Undo, redo and saving use the
  existing source transaction and persistence services. Context Fill preflights
  participating owners; mixed native/legacy sessions reject before mutation.

`component_closure` is now a reusable read-only dependency query, also used by
copy/duplicate. `trace_edge_chains` follows actual selected source incidence,
including closed and parallel curved edges. Neither allocates source identities.
`fill_components` constructs a complete independent candidate, reports new and
removed identities, and leaves owner commits/selection orchestration to editors.
BMesh `contextual_create` is a test-only topology reference.

## Explicit limits

This is an initial exact-construction subset, not general CAD surface filling.
Unsupported cases return a diagnostic before source mutation:

- Branching edge nets, multiple/nested loops, ambiguous parallel-edge choices,
  and mixed face-region/independent-wire selections need an explicit layout.
- Nonplanar boundaries beyond four straight sides need a geometry policy.
  Nonplanar curved boundaries and curved-support face merging are not implemented.
- Existing boundary curves must cover their full clamped domain, have positive
  weights, be continuous, and match shared endpoint positions exactly. Curved
  planar profiles additionally need the implemented composite-Bezier monotonicity
  and simple convex control-polygon certificate. Arbitrary trimmed arcs/curves
  need a more general correspondence and simplicity service.
- Planar charts must pass the exact affine correspondence checks. Some oblique
  planes are rejected because the required representation cannot be established
  by this conservative check. No fitting, flattening or tolerance expansion occurs.
- Simplicity checking currently limits the control polygon to 1,400 vertices.
  Certificates here are restricted construction checks, not general trim,
  regularity, tangent-continuity, self-intersection or solid-validity proofs.
- Fresh faces currently use material index zero; merging requires matching input
  material indices. Attribute propagation needs the later shared attribute policy.
  Unused coefficient resources are retained under the existing deletion policy.
- General deformation of trimmed polygons, full transforms, native creation,
  fit-point curve editing and full native component Python bindings remain gates.
  A planar filled triangle/polygon can be swept with E; arbitrary subsequent
  corner bending is still subject to the current transform kernel's limits.

The application and thirteen targeted test executables build. **228 C++ tests
pass with no skips**, along with **15 isolated viewport regressions** and **five
CTest targets**. Ten saved Fill states match expected authoritative source data;
fresh-process continuation passes. The bent extrusion viewport was visually
inspected. Build/test commands, results and corrected test-harness failures are recorded in
[current checkout audit](current_checkout_audit.md). Stage 1 remains incomplete.
