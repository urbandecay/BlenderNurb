# Blender mesh operations on exact NURBS topology

## Scope and interpretation

This is a geometry capability study, not a requirement to preserve classiCAD's
current model or interaction. classiCAD's current NURBS curves, surfaces,
trimmed planar fills, extrusion solids, and constrained connected face editing
are proof-of-concept evidence only. Blender's edit-mode behavior is the UX and
operator baseline; a CAD topology model may need a substantial redesign.

The classifications below describe geometry feasibility, not implementation
effort:

- **Support-preserving:** topology changes while all surviving geometry stays
  on the same exact support curve/surface. A UV partition of a face is the
  canonical example.
- **Exact restricted construction:** exact for a stated geometry family or
  construction (for example planar polygons, degree-one patches, or rational
  circular arcs), but not generally exact for arbitrary NURBS input.
- **Tolerance / approximation policy:** the result needs numerical
  intersections, offsets, or fitting, with a declared tolerance and a way to
  report or reject excessive deviation.
- **Representation-specific:** the result depends on what a source face means.
  Triangulating a Blender nonplanar n-gon, for example, produces planar facets;
  partitioning a NURBS support in UV preserves the original curved surface.

These are engineering classifications, not formal proofs. No runtime tests or
formal geometry proofs were performed for this study.

## Topology and geometry are separate

A mesh topology vertex is an actual shared corner in the topology graph. A
NURBS control vertex is a coefficient in a basis expansion. Except for special
cases such as degree-one patches, a NURBS CV is generally not on the evaluated
surface and is not a topological corner. Moving a CV changes a supported region
of the surface; moving a topological vertex means changing an edge endpoint and
reconciling every incident edge, p-curve, trim loop, and face support.

A CAD-aware BMesh-like topology model can retain Blender's combinatorial
operator patterns. It should have stable shared IDs for vertices, edges, loops,
and faces; exact 3D edge curves; face support surfaces; and oriented UV trim
curves (p-curves) linked to 3D edges with parameter correspondence. Topology
algorithms can be shared where they only traverse incidence, choose adjacency,
or update combinatorial structure. Geometric predicates and construction must
be wrapped so they account for curve/surface domains, rational geometry,
trimmed regions, tolerances, and shell invariants.

Validation before commit should cover at least: edge endpoint agreement,
3D-edge/p-curve consistency, trim closure and orientation, non-self-intersecting
face regions where required, face and shell orientation, edge incidence for
closed shells, support continuity claims, and stable identity/selection maps
after splits and merges. G0 positional adjacency must be distinguished from
G1 tangent and G2 curvature continuity. A matching endpoint or shared
topological edge does not prove smoothness.

## Operation capability matrix

| Blender operation family | Topology/combinatorial work | NURBS geometry capability and classification |
|---|---|---|
| Move/translate/rotate/scale/shear | Update selected topology vertices and/or face geometry while preserving incidence. | Applying an affine transform to every CV of a curve or surface, and transforming rational Euclidean CVs correctly, is **exact**. Moving a topological vertex while holding arbitrary incident face supports fixed is **underdetermined**: supports, edge curves, p-curves, and neighboring faces need an explicit update policy. |
| Extrude region | Duplicate region boundary, connect old/new loops, add walls; suppress internal walls for a selected connected region. | Linear extrusion of line segments, planar regions, and compatible NURBS boundaries is **exact restricted construction** using ruled surfaces. Arbitrary face regions require exact side construction and trim reconciliation; unsupported intersections need a **tolerance policy**. |
| Extrude individual faces | Duplicate each selected face boundary and add walls, with per-face or shared displacement. | **Exact restricted construction** for planar polygon faces and supported ruled surfaces. Shared boundaries can produce overlapping walls by design; manifold behavior is a separate topology policy. |
| Manifold extrusion | Update adjacent faces and remove internal/overlapping walls as the swept region joins the shell. | Exact for compatible planar/degree-one arrangements and some ruled cases. General NURBS face intersections and trim changes need exact construction or **tolerance/approximation policy**. |
| Inset/outset | Offset selected loops, connect old/new boundaries, resolve corners, collapsed edges, and self-intersections. | Straight planar polygon offsets are **exact restricted construction**. General NURBS offsets are usually not finite NURBS exactly; use **tolerance-fit**, report deviation, and resolve topology changes explicitly. |
| Bevel/chamfer | Replace edges/vertices by transition faces and update adjoining loops. | Straight planar chamfers are exact in restricted cases. Circular planar fillets can be exact rational degree-2 NURBS arcs with appropriate span splits and weights. General rolling-ball blends and arbitrary curved-face transitions usually require **tolerance-fit**. |
| Bridge edge loops | Establish loop correspondence, split/reorder as needed, add connecting faces, resolve twist. | Exact ruled patches are possible for compatible boundary curves; loop correspondence and surface shape are not unique in general. Classify as **exact restricted construction** with explicit correspondence and patch rules; otherwise approximate or reject. |
| Loop cut / edge ring (Ctrl+R) | Preserve Blender's pick/preview/slide workflow; traverse actual face incidence and create a selectable loop across adjacent faces. | On compatible NURBS supports, an exact isoparametric cut can split face trims and create shared topology while leaving the visible shape unchanged. Knot insertion adds shape-preserving CV rows but is not by itself a face-topology cut. Across unrelated supports, the path may require numerical surface intersections and a **tolerance policy**. 1D spline knot insertion is a separate control-refinement action, not a face loop. |
| Knife / bisect / surface intersection | Trace a cut, split crossed edges/faces, classify regions, optionally delete or cap. | Exact for restricted analytic/planar cases or user-specified curves with exact p-curves. General NURBS/NURBS intersections generally require numerical intersection curves and a **tolerance/approximation policy**. |
| Subdivide | Insert topology edges/vertices or apply a subdivision scheme. | Knot insertion and UV partitioning can be **support-preserving and exact**. Blender polygon subdivision is a shape rule over polygon topology and is **representation-specific**; it is not automatically equivalent to NURBS knot insertion or control-net refinement. |
| Unsubdivide / decimate | Remove vertices/edges/faces under topology and error criteria. | Exact inverse is generally not unique. Can be exact when removing redundant collinear degree-one vertices or merging same-support UV partitions. Shape-error-based removal is **tolerance policy** and needs error bounds. |
| Triangulate | Partition each face loop into triangles by adding edges and loops. | On a NURBS face, triangle regions can be UV trims on the same support: **support-preserving and exact**. For a nonplanar Blender n-gon, triangulation defines planar facets; this is **representation-specific** and not equivalent to a bilinear or higher-degree NURBS patch. |
| Tris-to-quads / join triangles | Remove a shared edge and merge face loops. | Exact and **support-preserving** when both faces lie on the same support and the merged UV loop is valid. Different supports cannot generally become one exact NURBS face; retaining two supports while removing a visible seam requires explicit topology/continuity semantics. |
| Fill / grid fill / hole fill | Add face loops or a structured patch layout across boundary edges. | Planar polygon fill is exact. A partition on an existing support can be exact. A new curved face is not uniquely determined by its boundary; exactness requires a specified construction, otherwise use **tolerance-fit**. Grid fill additionally needs a stable boundary correspondence and parameterization. |
| Merge / weld / remove doubles | Merge topology vertices or edges, update incidence, repair degenerate loops. | Exact if geometry agrees under a defined tolerance and p-curves/trim references can be reconciled. Coordinate coincidence does not establish G1/G2. Near-coincident merging is an explicit **tolerance policy**. |
| Split / rip / edge split | Duplicate topology entities and reassign incident loops to create boundaries. Rip uses cursor-side selection; Extend Vertices picks an adjacent edge from the view ray. Their move macros translate the selected duplicate after splitting. | **Support-preserving and exact** when both resulting faces retain their supports, trims are split consistently, and the shared 3D edge is duplicated with matching parameterization. The macro's translation is an exact affine move; update edge curves and p-curves. Whether pieces remain one object is an object-model decision. |
| Dissolve vertices/edges/faces | Remove selected graph elements and merge adjacent loops or regions. | Removing a UV seam between same-support faces is **support-preserving** if their union is a valid trim region. Coplanar planar faces can merge exactly. Joining different supports into one face is generally not exact; otherwise retain separate faces and define seam visibility/continuity separately. |
| Delete | Remove selected topology and update open/closed boundary state. | Surviving geometry can remain exact. Shell closure changes by design; dangling boundaries, dependent trims, and object validity require explicit policy. |
| Duplicate / separate / join | Copy graph entities, extract subgraphs, combine connected components and remap references. | Duplication and separation can preserve support geometry exactly. Joining objects should retain component faces, curves, and parameter domains unless a distinct exact surface-merge operation succeeds. |
| Edge rotate / edge collapse | Rotate a diagonal across a face pair; collapse an edge and merge endpoint neighborhoods. | Edge rotation is **support-preserving** for same-support UV regions with valid resulting loops; on distinct supports, keep separate faces and ensure shared-edge geometry remains valid. Collapse is exact for redundant degree-one vertices in restricted cases; general collapse moves/replaces topology vertices and needs a geometric policy plus often **tolerance/error bounds**. |
| Convex hull | Construct boundary topology around a set of points. | For finite points, a polyhedral hull is exact as planar degree-one faces. This does not produce a smooth NURBS hull. A smooth fitted hull is a separate **approximation policy**. |
| Polybuild / create topology | Add/move/delete vertices and edges, create faces, dissolve at cursor, and connect selections. | Exact for point/line/planar polygon construction; curved faces require support-surface placement and p-curve generation. A face interior is underdetermined by its boundary alone. |
| Primitives (cube, plane, circle, UV sphere, etc.) | Create predefined graph topology and selection. | Polygon primitives are exact planar degree-one topology. Circle and circular arc primitives are exact rational NURBS. A sphere is exactly rational NURBS with suitable patches. Some Blender polygon primitives approximate analytic shapes by design; retain that representation-specific intent or offer exact CAD variants. |
| Monkey/Suzanne and similar mesh primitives | Create a fixed polygon control topology. | Its polygon mesh is exact as that specified faceted object. Converting it to smooth NURBS changes representation and requires fitting; it is **representation-specific**, not an exact conversion. |
| Spin / discrete screw | Duplicate selected topology through finite steps, transform copies, connect successive copies. | A finite sequence of transformed polygon faces can be represented exactly as degree-one planar/ruled facets. Circular/revolution cases have exact rational NURBS constructions. A general continuous screw sweep creates helical geometry, which is generally **not a finite rational NURBS surface exactly**; approximation or a procedural surface type is needed. Keep discrete spin separate from continuous screw. |
| Poke | Add face-interior vertex and spokes to partition the face. | On a NURBS face, add UV trim edges and retain the same support: **support-preserving and exact**. On a nonplanar Blender n-gon, planar triangle output is representation-specific and changes the piecewise planar interpretation. |
| Wireframe / solidify | Offset selected boundaries, create walls/thickness, manage corners and intersections. | Exact for restricted planar polygon cases. General curved offsets are generally not exact NURBS; require **tolerance-fit**, self-intersection checks, and a thickness/corner policy. |
| Flatten / planarize | Move selected graph vertices or alter selected face geometry toward a plane. | Moving all NURBS CVs by an affine transform is exact as a shape transform. Moving topology vertices alone does not determine the new face supports. Planarization is **exact restricted construction** for polygon points; arbitrary NURBS faces need a reconstruction/fitting policy. |
| Smooth / Laplacian / relax | Move graph vertices from adjacency-weighted neighborhoods. | The combinatorial stencil can be shared, but applying it to topological vertices does not define new NURBS supports. Applying a stencil to CVs is a different, shape-changing operation. Surface fitting or a CAD-native fairing contract is needed; usually **approximation/shape-change policy**. |

## BMesh reuse and boundaries

BMesh's combinatorial ideas are reusable: adjacency traversal, selected-region
classification, loop ordering, edge-ring propagation, region extrusion
bookkeeping, split/merge remapping, and operator transaction structure can all
be adapted to shared CAD topology. Use invariant-aware wrappers and exact
geometry services for construction and validation rather than hard-coding
triangle/plane assumptions into operators.

Directly adopting Blender's BMesh as the authoritative CAD model has important
limits. Its BMVert coordinates are floating-point mesh coordinates, not a
double-precision exact-curve representation. A BMesh face is a polygonal loop
on its mesh vertices; it has no native NURBS support surface plus UV trim
curves linked to shared 3D edges. Its face-loop model also does not provide the
general multiple outer/inner trim-loop semantics needed for CAD faces with
holes. A custom layer can still present a BMesh-like API while using CAD-grade
precision, stable IDs, multiple trim loops, edge curves, support surfaces,
and p-curves.

Thus “reuse BMesh algorithms” means reuse or adapt the combinatorial structure
and proven adjacency behavior. It does not imply that float predicates,
polygon face geometry, or mesh-only validity rules are sufficient for exact
NURBS editing. Preview tessellations may seed numerical operations, but a
committed operation must update exact topology and geometry and validate the
result under its declared exactness/tolerance category.

## classiCAD proof-of-concept references

The current implementation is not a mandatory representation or a ceiling on
the feature set. It demonstrates exact NURBS surfaces, UV trim loops, planar
fill, ruled curve extrusion, translational extrusion solids, and a restricted
connected rectangular-patch editor. Relevant files are:

- `src/core/document/shape.h`
- `src/core/geometry/nurbs_surface.h`
- `src/core/geometry/nurbs_solid.h`
- `src/core/geometry/nurbs_solid_face_extrusion.cpp`
- `src/core/geometry/extrusion_weld.cpp`
- `CONNECTED_SURFACE_VERTEX_EDITING.md`
- `SOLID_FACE_EXTRUSION.md`

These contracts explicitly limit current connected editing and do not define
a general BRep kernel. They are evidence about the prototype's present scope,
not proof that other topology architectures cannot support the operations
above.

Blender implementation paths reviewed include `source/blender/bmesh/operators`
files `bmo_extrude.cc`, `bmo_inset.cc`, `bmo_bevel.cc`, `bmo_bridge.cc`,
`bmo_subdivide.cc`, `bmo_subdivide_edgering.cc`, `bmo_unsubdivide.cc`,
`bmo_bisect_plane.cc`, `bmo_triangulate.cc`, `bmo_join_triangles.cc`,
`bmo_fill_holes.cc`, `bmo_fill_grid.cc`, `bmo_poke.cc`, `bmo_wireframe.cc`,
`bmo_dissolve.cc`, `bmo_dupe.cc`, `bmo_removedoubles.cc`,
`bmo_split_edges.cc`, `bmo_offset_edgeloops.cc`, `bmo_flatten.cc`,
`bmo_planar_faces.cc`, `bmo_rotate_edges.cc`, `bmo_hull.cc`, `bmo_primitive.cc`,
and `bmo_dupe.cc` (spin). This study did not run Blender or classiCAD runtime
tests.
