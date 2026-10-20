# Native NURBS topology integration exercise

This is the implementation contract for the architecture exercise required by
`nurbs_mesh_editing_study.md`. The bounded connected-topology exercise described
here has passed its current acceptance checks. It validates native surface and
trim ownership, shared-edge incidence, source-ID selection, edge splitting and
the supported complete-boundary translation case. It does not establish
general NURBS surface deformation or mesh-editing parity.

## Geometry and connectivity

Keep geometric supports separate from topological components. A surface support
stores its rational tensor-product CV net, independent reduced U/V knots and
parameter domains. A curve support stores its rational CVs, reduced knots and
domain. All Euclidean XYZ supports are in the Blender object's local frame.
Display meshes are derived from these supports and their trims.

The native data-block owns these records through Blender DNA and BKE lifecycle
code. References between records use stable IDs scoped to that data-block:

- A topological vertex has an ID and an object-local XYZ position. It is not
  interchangeable with a support control vertex.
- An edge has an ID, two endpoint vertex IDs, a 3D curve-support ID, and an
  increasing parameter interval within that support's domain. A closed edge
  may use the same vertex ID at both ends.
- A face has an ID, a surface-support ID and oriented boundary loops.
- A loop has an ID, an outer/hole role and an ordered array of edge uses.
- An edge use has an ID, an edge ID, a UV curve-support ID and traversal
  orientation. Opposite uses of a shared edge refer to the same edge ID.
- UV curve supports are exact rational curves in the face's parameter space.
  They are separate from object-local 3D curves and texture UV attributes.

General editing permits open wires, open sheets, nonmanifold edges and seam
edges used more than once by one face. Counting more than two uses does not by
itself make an editing graph invalid. Closed-solid validation must impose its
own shell and manifold requirements when solids are implemented. Every use
still needs a valid edge, UV support and continuous oriented loop.

CVs, supports, vertices, edges, faces, loops and edge uses draw their IDs from
the data-block's shared allocator. Duplication into another data-block may
retain its internal IDs; the owning data-block plus component ID identifies a
component. No persistent reference depends on an array address, sample index
or floating-point coordinate equality.

Keep owned nested arrays in DNA and centralize clone/free/read/write and
validation in BKE. Editor undo must use the same geometry ownership services.
Runtime maps accelerate ID lookup and incidence traversal; they do not replace
persisted references. Support data referenced by edges is not also emitted as
an independent editable wire. Unreferenced 3D curves remain loose wires until
their creation/editing entry points migrate to the edge representation.

## Parameter agreement

For the first integration exercise, a 3D edge and each incident UV support use
the same increasing edge parameter. Traversal orientation is independent of
parameterization: reversing a loop use reverses traversal, not the stored
domain. A UV support may cover more than an edge's current interval, just as a
3D support may.

Construction must establish agreement between `edge_curve(t)` and
`surface(uv_curve(t))`. Different faces may have different UV coordinates and
surface parameter domains while sharing this edge parameter. A future import
or fitting service must establish compatible parameterization under its stated
tolerance, or supply an explicit mapping service. It must not assume two
unrelated imported parameterizations agree merely because their endpoints do.
General numerical services and their kernel choice remain part of the broader
architecture evaluation.

Outer loops run counterclockwise and hole loops clockwise in the support's UV
space. Face reversal changes the oriented surface normal; it does not rewrite
that UV convention. Check UV endpoint continuity as well as topological vertex
continuity, since different parameters of a periodic surface can map to the
same XYZ point. Use a dimensionless parameter tolerance after normalizing U/V
by their respective surface-domain widths. The object-local length tolerance
governs 3D agreement. These are distinct quantities with different units.
Sampled winding and agreement diagnostics must be identified as measured
checks; they do not certify arbitrary trim topology between sample points.

## Exact edge split

An edge split changes connectivity. It does not require modifying its geometric
support. Given an interior edge parameter `t`, evaluate the new vertex from the
3D support and replace the edge with two fresh edge IDs over `[a, t]` and
`[t, b]`. Both children retain the original support ID. Replace every incident
edge use in loop order, preserving orientation and the exact UV supports. The
child uses traverse the corresponding subintervals. A reversed use lists the
right child before the left child.

Return a remap from the removed edge/use IDs to their children and the new
vertex ID. Apply it to active selection and selection history. Surface CVs,
knots, weights, hole boundaries and unaffected IDs remain unchanged. Physical
curve-support splitting is a separate operation using exact homogeneous knot
insertion; it must not be confused with this topological interval split.

Validate the complete replacement before publishing it. Failure leaves the
original geometry and selection intact. ID allocation and replacement data
belong to the same transaction. No failure may leave a partially updated
incident face or silently weld two unrelated edges.

## Required curved fixture

Use two exact quarter-cylinder patches stacked along their common axis. Patch
A runs from Z=0 to Z=1; patch B runs from Z=1 to Z=2. Each has a quadratic
rational U direction with circular weights `1, sqrt(0.5), 1` and a linear V
direction. The shared boundary is a curved quarter-circle at Z=1, stored as
one edge with two incident uses.

Give patch A U/V domains `[0, 1]`. Give patch B U domain `[2, 5]` and V domain
`[-3, 7]`. For common edge parameter `t` in `[0, 1]`, its two exact UV supports
are `(t, 1)` and `(2 + 3*t, -3)`. This exercises different surface domains and
opposite loop traversal without changing the common edge parameter.

Patch A has an interior rectangular UV hole, bounded by exact iso-parameter
curves. Its horizontal boundaries are rational circular arcs in 3D; its
vertical boundaries are straight generators. The hole loop traverses opposite
to the outer loop. Sampling can draw the trim, but the committed trim remains
an exact UV curve.

Split the shared edge at `t=0.5`. Check both child domains, shared vertex,
incident loop ordering, original support IDs and the selection remap. Copy,
undo and save/reopen must retain the same graph and exact support values.
Viewport picking must identify edges/faces through source IDs, including after
the split; tessellation triangle indices cannot become component identity.

Exercise native Edit Mode Grab on the shared seam and update both incident
supports and dependent boundary curves coherently. For this ruled fixture,
moving the seam changes the adjacent boundary CV rows while preserving the
opposite rows and UV trims. Reconcile the affected 3D trim curves with those
supports before commit. This fixture does not establish a general algorithm
for arbitrary curved face deformation; that remains an explicit later service.

## Acceptance evidence

Check finite positive rational supports, reduced-knot counts/order/domains,
unique IDs, reference integrity, closed oriented loops, edge incidence,
support/UV agreement, atomic failure and complete selection remapping. State
the tolerance and distinguish measured sampled agreement from a certified
geometric bound. The fixture's exact construction is part of the evidence.

The bounded exercise passed its BKE tests and native Blender selection, modal
Grab, cancel, undo/redo and save/reopen checks. The UI acceptance uses Blender's
editor undo/redo operators and keyboard cancel. Its boundary translation only
supports compatible complete clamped isoparametric boundaries with linear UV
trims. Arbitrary partial vertex/face deformation, general snapping and the full
mesh-editing capability matrix remain later work. The standalone surface
evaluator and exact curve splitter alone would not satisfy this gate.
