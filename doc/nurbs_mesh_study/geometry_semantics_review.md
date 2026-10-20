# Geometry semantics review for Blender-style NURBS editing

## Purpose and status

This is a supplementary, source-audited review of geometry-changing mesh edit
operations and proposed NURBS behavior. Blender source observations below are
identified as such; the proposed CAD semantics are recommendations, not claims
about Blender behavior and not a formal proof of representability. The review
does not modify code and did not run Blender or classiCAD runtime tests.

The existing `geometry_capability_matrix.md` correctly distinguishes
support-preserving topology edits, restricted exact constructions,
tolerance-managed operations, and representation-specific behavior. This
review expands coverage to the actual registered mesh-operation inventory and
states a default user-facing behavior for each operation where one can be
defined. It is not a request to ask the user to select a geometry policy for
every operation: choose a deterministic default; report when a tolerance fit
was used; reject only when neither the exact default nor its declared fallback
can meet the modeling tolerance.

## Evidence and default policy

**Blender source evidence.** `source/blender/editors/mesh/mesh_ops.cc` registers
the direct mesh operators and their macro workflows. The implementation
families are under `source/blender/editors/mesh/`; topology primitives are
under `source/blender/bmesh/operators/`. The inventory in
`doc/nurbs_mesh_study/operator_inventory.csv` records registration, likely
implementation, properties, and source notes. Registration is evidence that
an operator exists in the source, not that every context enables it or that a
particular menu exposes it. Macros and modal tools compose operators; their
interaction should be read with the implementing operator.

**Proposed default for NURBS.** Keep a persistent CAD topology graph and exact
edge/surface geometry authoritative. Apply the requested operator to selected
topological vertices, edges, faces, or regions, preserving the source support
where that gives the requested visible result. Use exact constructions where
available. Where Blender's effect implies a new geometry that is not generally
finite NURBS, run a tolerance-controlled fit with a deterministic default
tolerance derived from the document's modeling tolerance and report the
maximum deviation. If fitting fails or exceeds that tolerance, do not silently
flatten or bake: retain the last valid preview and explain the operation's
geometric limit. A user can explicitly request a polygon conversion/bake as a
separate, visible operation.

For exact operations, “same shape” means the represented 3D surface/curve is
unchanged, not merely that a tessellation looks similar. Degree-one NURBS can
represent polygonal edges/faces exactly, but mesh topology and NURBS CVs remain
different concepts. A topology vertex belongs to the shared graph; a NURBS CV
is a basis coefficient and usually is not a point on the surface.

## Complete operation-family review

| Blender family / source evidence | Proposed Blender-like NURBS behavior | Geometry status and required rule |
|---|---|---|
| **Context and region extrusion** — `MESH_OT_extrude_region`, `MESH_OT_extrude_context`, `MESH_OT_extrude_repeat`; `editmesh_extrude.cc`, `bmo_extrude.cc`; macros in `mesh_ops.cc` include region move, manifold, shrink/fatten and repeat. | Use selected topological vertices/edges/faces. Duplicate the region boundary, attach side faces, and move the new cap with the same modal transform, normal, axis, numeric, snap, and repeat controls. Selected connected faces extrude as one region; internal selected edges do not get walls. Manifold mode updates adjacent faces and removes internal walls. | Exact for translated linear boundaries and supported ruled walls; planar polygon extrusion is exact degree-one NURBS. General supports require ruled-surface construction and exact edge/trim intersection. Apply exact route first, then tolerance-managed intersection/fit where needed. No implicit tessellation commit. |
| **Individual face/edge/vertex extrusion** — `MESH_OT_extrude_faces_indiv`, `...edges_indiv`, `...verts_indiv`. | Preserve Blender distinction: each face moves independently; each edge creates a separate strip; each vertex creates a point/edge result. Keep the existing modal movement and selection semantics. | Exact for linear curves, planes, and ruled output. Per-face normal and corner behavior is construction-specific. Faces with curved supports use exact translated copies and ruled connectors when valid; otherwise tolerance intersection/fit. |
| **Inset / outset / offset edge loops** — `MESH_OT_inset`, `MESH_OT_offset_edge_loops`; `editmesh_inset.cc`, `bmo_inset.cc`, `bmo_offset_edgeloops.cc`. | Offset the selected face region boundary on its support, generate the surrounding band, preserve boundary/individual inset modes and depth controls. On curved surfaces, define distance in 3D surface metric (geodesic offset) for consistent CAD behavior, rather than offsetting only UV coordinates. | Planar polygon lines: exact line offsets/intersections until topology changes. UV isoparametric offsets preserve support but are not constant 3D distance generally. General constant-distance NURBS offsets require tolerance-fit. Detect self-intersections/collapses, split/remove loops deterministically, preserve valid regions, and report fit deviation. |
| **Bevel** — `MESH_OT_bevel`, `editmesh_bevel.cc`, `bmo_bevel.cc`. | Retain width/segments/profile/affect modes and modal input. Create transition faces and update the neighboring trims. Default to exact planar chamfer when segment count is one; for circular profile, use exact rational arcs/surfaces when the boundary is analytic and compatible. | Straight chamfer is exact restricted construction. Circular fillet arcs are exact rational degree-2 NURBS, split into at most quarter-circle spans. General offset/rolling-ball surfaces need tolerance-fit. On failure beyond tolerance, reject the candidate rather than commit poor geometry. |
| **Subdivision / edge-ring subdivision / loop cut (Ctrl+R)** — `MESH_OT_subdivide`, `MESH_OT_subdivide_edgering`, `MESH_OT_loopcut`; `editmesh_loopcut.cc`, `bmo_subdivide*.cc`; loop-cut-slide macro. | Ctrl+R should retain edge hover, cut-count, preview, and slide UX. For NURBS faces, compute the loop as a path through shared face topology. On each face, create the corresponding support curve and split trim loops; split shared 3D edges once and share the new topology vertices. The new loop is an actual editable face boundary. | **Knot insertion is not a loop cut.** Knot insertion refines a tensor-product control net exactly without changing the surface or face topology; it is an optional implementation step when the cut aligns with a parameter isocurve. The topological cut is a UV trim split, can cross a face without changing its support, and needs valid p-curves. If path crosses unrelated supports, intersect them numerically and enforce tolerance. Blender subdivision's mesh smoothing/fractal options are separate shape-changing operations; do not conflate with exact knot refinement. |
| **Unsubdivide / decimate** — `MESH_OT_unsubdivide`, `MESH_OT_decimate`; `bmo_unsubdivide.cc`, dissolve/decimate kernels. | Remove redundant topology while retaining Blender's iteration/error controls. Coalesce same-support face partitions and remove degree-one collinear vertices where exact. | Exact only where the removed entity is redundant and support/trim union remains valid. General decimation is tolerance-driven; preserve boundaries/features by default, report geometric and normal deviation, and do not claim it is an exact inverse of subdivision. |
| **Triangulate / quad-to-triangle / beautify fill / tris-to-quads** — `MESH_OT_quads_convert_to_tris`, `MESH_OT_tris_convert_to_quads`, `MESH_OT_beautify_fill`; `bmo_triangulate.cc`, `bmo_join_triangles.cc`, `bmo_beautify.cc`. | Partition a selected NURBS face in UV into triangular/quad trim regions on the original support. Preserve the triangulation diagonal and beauty/edge-length choice UI. Tris-to-quads removes an internal edge when the merged trim is valid. | Same-support face partition is exact and support-preserving, including curved faces. This differs from Blender's nonplanar polygon face, where a diagonal changes the piecewise planar interpretation. On distinct supports, do not merge into a single face unless a valid exact common support exists; preserve separate faces or fit with tolerance. |
| **Poke** — `MESH_OT_poke`, `bmo_poke.cc`. | Add a topology vertex at the requested face point, construct spokes to the loop, and create subregions in UV on the same support. | Exact support-preserving partition if the UV point is interior and trims are valid. For Blender polygon faces, centroid/offset changes triangular facets; preserve that as a separate polygonal construction, not a NURBS CV edit. |
| **Dissolve vertices/edges/faces, limited dissolve, degenerate dissolve, delete edge loop** — `MESH_OT_dissolve_*`, `MESH_OT_delete_edgeloop`; `bmo_dissolve.cc`. | Retain selection-based dissolve, angle limits, boundary options, and loop deletion. Merge same-support adjacent faces by unioning UV trim regions; delete edge/vertex incidence only when resulting loops stay valid. | Same-support dissolve is exact. Coplanar planar faces can become one exact planar face. Different supports remain separate, with edge removed only if topology permits and the visible seam/continuity has explicit semantics; otherwise use tolerance surface fitting for a merged face. Limited/degenerate criteria should use exact normals/curvature where available, with tolerances documented for numeric comparisons. |
| **Merge / remove doubles / collapse** — `MESH_OT_merge`, `MESH_OT_remove_doubles`, `MESH_OT_edge_collapse`; `editmesh_tools.cc`, `bmo_removedoubles.cc`. | Provide Blender merge-at-center/first/cursor and threshold-based merge. Merge graph vertices/edges, reconcile edge curves and p-curves, and remap selected IDs. | Exact when endpoints and curves agree. Near merge uses modeling tolerance and must construct compatible shared geometry; positional coincidence alone does not imply smoothness. Edge collapse can be exact for redundant degree-one vertices, otherwise it is a topology simplification with a bounded deviation policy. |
| **Split / rip / edge split** — `MESH_OT_split`, `MESH_OT_rip`, `MESH_OT_rip_edge`, `MESH_OT_edge_split`; `bmo_split_edges.cc`. | Duplicate selected vertices/edges and reassign face loops as Blender does. Rip uses the cursor to choose a side; Extend Vertices uses the view ray to choose an adjacent edge. Their macros then translate the split result, so the gap comes from moving the duplicate rather than the split itself. | Exact and support-preserving while splitting. The later translation is an exact affine move of the duplicated topology; update its 3D edge curves and face p-curves consistently. |
| **Bridge edge loops / edge-net fill / edge-face add / fill and hole fill / grid fill** — `MESH_OT_bridge_edge_loops`, `MESH_OT_fill`, `MESH_OT_fill_holes`, `MESH_OT_fill_grid`, `MESH_OT_edge_face_add`; `bmo_bridge.cc`, `bmo_edgenet.cc`, `bmo_fill*.cc`. | Preserve boundary ordering, loop pairing, twist controls, and fill selection. Reuse one exact support when boundaries lie on it; otherwise construct bridge patches with explicit correspondence. | Existing-support UV fill is exact. Planar polygon fill is exact. Ruled bridge patches are exact restricted construction for compatible curves. General freeform bridge is underdetermined: use deterministic interpolation/fitting with tolerance and continuity targets. A hole can be a trim loop on its support or a newly constructed cap; select same-support trim fill automatically when possible, else build a cap by exact planar fill or fit. |
| **Knife / knife project / bisect / intersect / boolean intersect** — `MESH_OT_knife_tool`, `MESH_OT_knife_project`, `MESH_OT_bisect`, `MESH_OT_intersect`, `MESH_OT_intersect_boolean`; `editmesh_knife.cc`, `editmesh_bisect.cc`, `editmesh_intersect.cc`, `bmo_bisect_plane.cc`. | Keep modal screen projection, cut-through, clear-inner/outer, and boolean modes. Trace projected/user plane curves in world space; intersect against face supports, split crossed shared edges, make p-curves, classify regions, and update trims. | Exact for planar/analytic cases and exact user curves with known p-curves. General NURBS/NURBS intersection is numerical: solve to document tolerance, maintain parameter correspondence and intersection topology, then validate closure and sliver faces. Boolean classification is exact only after reliable intersection topology; reject ambiguous/failed regions instead of flattening. |
| **Transforms and deforms** — generic mesh transforms plus `TRANSFORM_OT_*`: translate, rotate, resize, shear, bend, shrink/fatten, push/pull, to-sphere, trackball, mirror, edge/vertex slide; `editmesh_transform.cc`, `transform` subsystem. | Retain modal, numeric, axis, orientation, pivot, proportional, snapping, and connected-selection UX, targeting topological vertices/edges/faces. | Affine transforms of all CVs and topology geometry are exact. Topological vertex movement requires a deformation rule. For selected surface vertices, default to moving support data through a local surface edit with continuity constraints on adjacent faces; if no exact compatible support exists, perform tolerance-fit. Edge slide on a face should slide along its support; loop slide should preserve neighboring spacing semantics. Bend, sphere projection, and shrink/fatten are point maps and generally transform CVs or topological vertices according to target mode; they are shape-changing and may need fitting, never mistaken for affine exactness. |
| **Extrude repeat / spin / screw / dupli-extrude cursor** — `MESH_OT_extrude_repeat`, `MESH_OT_spin`, `MESH_OT_screw`, `MESH_OT_dupli_extrude_cursor`; `editmesh_extrude_screw.cc`, `bmo_dupe.cc`; Python view3d wrappers. | Preserve discrete step count, turns, angle, axis, center, offset, duplicate/extrude toggle, and auto-merge. Build each discrete step as transformed exact geometry and connect step boundaries. | A finite sequence of affine-transformed NURBS objects is exact; planar polygon steps remain exact degree-one faces. Exact circular revolution can use rational NURBS. General continuous helix/screw is not generally a finite rational NURBS surface; Blender screw is discrete sampled mesh construction, so implement discrete steps exactly and offer continuous analytic/procedural or tolerance-fit mode separately. |
| **Symmetrize / symmetry snap / mirror** — `MESH_OT_symmetrize`, `MESH_OT_symmetry_snap`; mirror transform. | Mirror topology, weld at the symmetry plane under the configured threshold, and preserve direction/clear-side UX. | Reflection of all NURBS CVs/edge curves is exact; rational weights unchanged. Symmetry snap of topological vertices is exact only if incident supports and trims can also be reconciled; otherwise move connected support geometry or tolerance-fit. |
| **Circularize / to-sphere / flatten / make planar / evenly space loops** — `MESH_OT_circularize`, `TRANSFORM_OT_tosphere`, `MESH_OT_flatten`, `MESH_OT_face_make_planar`, `MESH_OT_space_edge_loops_evenly`. | Keep best-fit plane, center, radius, spacing, and modal options. Operate on selected topological boundary points or support curves, not indiscriminately on all CVs. | Circle construction can be exact rational NURBS when the result is circular. Projection of finite topology vertices to a plane is exact as a point operation but does not define incident surface supports; update neighboring faces with constrained fitting. Even spacing is exact for line/arc parameterization under the chosen metric; general curve spacing uses arc-length numerics within tolerance. |
| **Solidify / wireframe** — `MESH_OT_solidify`, `MESH_OT_wireframe`; `bmo_wireframe.cc` and solidify implementation. | Preserve thickness, boundary, rim, offset, and selected-region controls. Generate offset faces and joining walls, retaining source support where available. | Exact for planar polygon and restricted ruled cases. General surface offsets/thickness need tolerance-fit, self-intersection detection, and corner blending. Default to tolerance-fit within document tolerance, report maximum deviation and reject invalid shell. |
| **Convex hull** — conditional `MESH_OT_convex_hull`, `bmo_hull.cc`. | Build hull topology from selected topology vertices/CV samples according to explicit source mode. | Hull of finite topological points is exact polyhedral geometry. A smooth NURBS hull from a surface is a different optimization/fitting problem; use boundary/control-point samples only when user requested hull of those points, and label smooth fitted hull approximate. |
| **Create/connect/polybuild** — `MESH_OT_vert_connect*`, `MESH_OT_face_split_by_edges`, `MESH_OT_polybuild_*`, `MESH_OT_knife_project`; `bmo_connect*.cc`, `bmo_create.cc`, `editmesh_polybuild.cc`. | Preserve cursor-driven create/split/dissolve/transform/delete workflows. New edges become exact degree-one or user-drawn NURBS curves; new face loops reference support UV curves. | Exact for linework and planar faces. On curved faces, project cursor point to support and make p-curves; if creating a face across supports, require an exact/fitted bridge or cap construction. Do not infer face interior merely from loop boundary. |
| **Primitive creation** — plane, cube, circle, cylinder, cone, grid, monkey, UV sphere, ico sphere; Python torus. | Preserve parameters, edit-mode insertion, and object placement. | Use exact CAD variants automatically when the primitive names imply analytic geometry: rational circle, exact sphere, cylinder/cone, ruled plane/cube faces. A Blender UV/ico sphere or Monkey mesh is specifically a polygon model; retain exact degree-one polygon topology by default. Offer smooth fitted conversion explicitly, with tolerance. A polygon torus and exact torus are distinct representations. |
| **Vertex smoothing / Laplacian smoothing / shape blend / shape propagation** — `MESH_OT_vertices_smooth`, `MESH_OT_vertices_smooth_laplacian`, `MESH_OT_blend_from_shape`, `MESH_OT_shape_propagate_to_all`. | Preserve iteration, factor, and shape-key-like blend semantics over selected topological points or named shape states. | Affine blend of compatible CV arrays with identical topology/parameterization is exact as a NURBS data operation, though not necessarily shape-preserving. Laplacian movement of topological vertices requires support reconstruction and is generally approximate. Shape transfer is exact only with compatible topology and parameterization; otherwise correspondence/fitting tolerance is required. |
| **Normals, smooth/flat shading, sharpness, freestyle, UV/color/custom data, sorting** — `MESH_OT_normals_*`, shade ops, sharpness and custom-data registrations, UV operators. | Preserve as display/attribute operations on face/edge/corner topology, not geometry. UV corner data must remain distinct from NURBS face UV parameter coordinates. | Normal direction can be derived from exact surface derivatives; display smoothing/sharpness is metadata and does not change geometry. Recompute normals after topology operations. Mesh UV unwrap coordinates are texture attributes and are not p-curves. Keep these concepts separate; only operations that move geometry invoke geometry semantics. |

## Knot refinement versus editable face topology

Knot insertion introduces control vertices/rows and modifies the basis while
preserving the exact curve/surface. It changes the representation of the
support, not the model's edge/face graph. It is useful internally to align
parameter intervals, expose handles, or subdivide a CV net. It must not be
reported as a Ctrl+R loop cut unless actual face loops, shared edge entities,
selection IDs, and trims are also created.

A face topology cut partitions the visible parameter region. On a single
support surface, define a UV path, split each intersected trim loop, create
shared topology edges and p-curves, and make adjacent face regions reference
the same support. The result can remain exactly unchanged. If the cut traverses
multiple supports, intersection curves define the path; robust numerical
intersection and tolerance checks are required. Selection and undo must track
the resulting face/edge IDs separately from any CV indices introduced by knot
refinement.

## Connected extrusion and connected editing

Model connectedness with shared topology IDs, not coordinate equality. A
selected region extrusion duplicates its boundary once, shares the translated
edges among all new walls, and suppresses walls on internal selected edges.
The new face supports should be exact ruled surfaces wherever the sweep
construction allows. Manifold extrusion additionally splits/rebuilds adjacent
face trims where the boundary crosses them, removes internal walls, checks
shell closure/orientation, and preserves unaffected supports. When an
intersection or resulting support cannot be constructed to tolerance, reject
the candidate atomically with the source unchanged.

Connected vertex/edge/face movement should first apply the requested movement
to topology geometry and propagate through shared incidences. Preserve exact
supports when the move is a support-preserving parameter edit or affine
transform. For arbitrary vertex motion, solve incident support constraints;
where no exact solution exists, fit adjacent supports under a stated positional
and continuity tolerance. Preserve G0 by default; do not silently promise G1/G2.

## Invariants and result reporting

Every topology-changing operation should return an explicit old-to-new entity
map for selection, component identity, dimension/constraint references, and
undo. Validate edge endpoint positions, 3D curve to p-curve correspondence,
trim closure/orientation, edge incidence, face orientation, shell closure when
required, and claimed continuity. Use scale-aware tolerances and report
whether output was exact or fitted, along with maximum fit deviation when
applicable. Avoid index-only persistence across topology changes.

For failed numeric operations, do not partially modify source topology. Keep
the last accepted preview and show an actionable failure reason. This preserves
Blender's modal workflow while making the CAD geometry outcome inspectable.

## Source coverage and omissions check

The previous capability matrix covered the principal families, but this audit
adds or sharpens: context and repeated extrusion; per-face/edge/vertex
extrusion; Ctrl+R as trim/topology split distinct from knot insertion;
edge-loop offset; beauty fill; quad-to-triangle; face split/connect/polybuild;
edge rotate and collapse; circularize, evenly space, to-sphere, and face planar;
discrete screw vs continuous helix; symmetry; solidify; shape blend/propagate;
primitives versus analytic CAD variants; transforms/deform modes; and normals,
UV, color, and custom data as geometry-adjacent but generally non-geometric
operations.

Source evidence is based on the static registrations in
`source/blender/editors/mesh/mesh_ops.cc`, macro definitions later in that
file, BMesh operator implementations under `source/blender/bmesh/operators/`,
and the inventory in `doc/nurbs_mesh_study/operator_inventory.csv`. Operators
that only select, hide, mark attributes, or alter shading are not geometry
changes; they are noted to avoid conflating mesh corner UV attributes with CAD
surface p-curves. Conditional operators (for example convex hull) depend on
build configuration. The inventory is not a runtime poll/menu census.
