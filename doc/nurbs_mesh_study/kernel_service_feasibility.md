# Geometry service feasibility review

Research status: primary documentation checked on 2026-10-22. A separate source
trial is being prepared under `/tmp/blender-nurbs-occt-trial`; it is not a
Blender dependency. No external kernel has been linked into Blender,
benchmarked or selected for production.
This review informs the architecture experiment; it is not a completed Stage 6.

## Candidate directions

| Direction | What it supplies | Work Blender still owns | Assessment |
|---|---|---|---|
| Native BKE services with a focused NURBS implementation | Direct integration and control over open editing states, source identity, exact constructions and reporting. | Evaluation, refinement and fit solving as well as robust general intersections, trimming, offsets, blends, validation and maintenance. | Appropriate for the native data/interaction contract and bounded exact services. Do not expand this into a complete numerical CAD kernel without comparing an existing one. |
| OCCT modeling services behind BKE | Documented geometry, BRep topology, numerical modeling and history APIs. Its topology supports mixed dimensions and non-manifold representation. | Native ID ownership, persistent selection/attribute maps, mesh-like modal behavior, editing-state policy, numeric boundaries, undo, draw/picking, evaluation and exact-versus-fit reporting. | Strong candidate for an isolated numerical-service trial. Representation support does not prove every algorithm accepts every open/degenerate/non-manifold edit. |
| openNURBS | NURBS representation, evaluation and 3DM interchange. | General modeling and numerical operations, in addition to Blender integration. | Not a replacement for the missing modeling kernel by itself. Useful interchange/evaluation code is a different capability from robust modeling operations. |

OCCT's modeling-data guide documents mixed-dimensional/non-manifold topology
and both interpolation and approximation services. That supports investigating
it for geometry services; it does not establish Blender workflow compatibility.
[OCCT Modeling Data](https://occt3d.com/dev/doc/overview/html/occt_user_guides__modeling_data.html)

McNeel explicitly lists intersections, tessellation, interpolation, booleans,
closest-point and mass-property calculations among openNURBS' limitations.
Depending on openNURBS therefore would not supply those missing services.
[What is openNURBS?](https://developer.rhino3d.com/guides/opennurbs/what-is-opennurbs/)

## New topology evidence the older study did not cover

OCCT 8.0 documents a `BRepGraph` alternative to its linked TopoDS model, with
typed node IDs, traversal, mutation guards, copy/remapping, validation, caches
and history. This deserves investigation before expanding a native mutable
topology implementation. It is not evidence that Blender can adopt its IDs,
undo or serialization unchanged.
[OCCT upgrade guide](https://occt3d.com/dev/doc/overview/html/occt__upgrade.html)

The maintainer's description separates persistent metadata layers from derived
caches and describes ingestion of operation history. Those concepts align with
the required source/cache separation. Whether this provides a better runtime
backend than the small native incidence index requires a concrete trial.
[BRepGraph in OCCT 8.0](https://occt3d.com/blog/brepgraph-in-occt-8-0/)

OCCT also exposes generated/modified/removed shape relations through
`BRepTools_History`. These are useful input to Blender identity remapping.
API availability alone does not prove complete or unambiguous provenance for
every operation and element type; test the maps we actually consume.
[BRepTools_History](https://occt3d.com/dev/doc/refman/html/class_b_rep_tools___history.html)

## Ownership and dependency boundary

Keep the production authoritative representation singular. Two candidate
integration designs should be compared:

1. Native DNA source geometry with an external algorithm adapter. Construct a
   temporary kernel representation from exact source data and IDs, run the
   operation, validate and export a candidate with identity maps, then commit.
   Conversion must preserve rational coefficients, knots, domains, p-curves and
   incidence. The temporary representation is not another editable authority.
2. A native ID-owned external geometry payload with explicit serialization and
   Blender wrappers. This must prove native file versioning, library/copy/free,
   undo, access to exact source and portable persistent identities. An opaque
   pointer or unexplained file blob is insufficient evidence of those properties.

The current DNA/BKE experiment tests native ownership; it does not settle the
runtime numerical backend. BKE clients should depend on geometry/operation
contracts, with the host owner isolated to lifecycle and adapters.

OCCT's CMake source exposes module/toolkit selection. A trial should select
only required modeling dependencies and preserve Blender's own UI, renderer,
file ownership and undo. Exact flags and dependency closure must be checked
against a pinned source revision before adding any build dependency.
[OCCT CMake source](https://github.com/Open-Cascade-SAS/OCCT/blob/master/CMakeLists.txt)

The published OCCT license is LGPL 2.1 with an additional exception; the
distribution must follow the actual license shipped with the selected source.
This review does not constitute a completed license/distribution audit.
[OCCT license](https://github.com/Open-Cascade-SAS/OCCT/blob/master/dox/license.md)

## Required numerical-service trial

Before choosing the backend, measure and test:

* Exact round trips of a rational arc, nonuniform weights, non-default knots and
  intervals, and a spatial curve. No automatic fitting or domain normalization.
* A curved shared edge, two unrelated support parameterizations, a face with a
  hole, and a periodic seam using one edge twice with distinct p-curves.
* Wires, sheets, disconnected islands and three-face edge incidence. Keep
  editing validity separate from closed-solid eligibility.
* Support splitting, translated ruled walls, one general intersection and one
  tolerance-managed blend/offset case. Bevel implementation remains deferred.
* Complete source-to-result maps for vertices, edges, uses, loops and faces,
  including one-to-many splits, merges, unchanged elements and deletions.
* Cancellation/failure without source mutation; diagnostics for ambiguous or
  invalid results; no silent tolerance increase, sewing, simplification or fit.
* Large coordinates with small features, nearly coincident boundaries, poles,
  tangent contacts, negative/nonuniform transforms and units.
* Build size and dependency cost, per-edit import/export overhead, runtime
  memory, cache invalidation, thread behavior and native undo cost.

Numerical results must declare the achieved deviation and whether it is a
sampled measurement or a certified bound. A kernel reporting success does not
prove exact finite NURBS output or the user's intended topology. Inspect the
actual output supports, trims, p-curves, orientation and provenance.

The recommendation is to run this trial before implementing broad numerical
modeling ourselves. It does not require adopting another application's UI,
document model or solid-only editing workflow.

## Pinned source inspection for the trial

The isolated checkout uses OCCT `V8.0.1`, commit
`b8f597c677811d1f9f4d8a97f5ae2825c0353a42`. The annotated `V8_0_1` tag
resolves to the same commit. Initial configuration requests only `TKBRep` and
its dependency closure, with application, visualization, exchange and Draw
modules disabled. Nothing is installed into Blender's dependency tree.

Source inspection establishes two adapter constraints to test:

* `BRepGraph` distinguishes transient typed indices from persistent UIDs.
  Compaction can change indices; clearing starts a new UID generation.
  Blender selection must not serialize transient node indices. A runtime
  adapter needs explicit maps between native source IDs and the kernel's
  entities/occurrences, including copies and compaction.
  [Pinned BRepGraph interface](https://github.com/Open-Cascade-SAS/OCCT/blob/b8f597c677811d1f9f4d8a97f5ae2825c0353a42/src/ModelingData/TKBRep/BRepGraph/BRepGraph.hxx)
* `Geom_BSplineCurve` rejects weights at or below `gp::Resolution()` (`DBL_MIN`
  in this revision). Its rationality check compares absolute adjacent-weight
  differences against that same threshold, so nearly equal weights at extreme
  small scales can be classified as non-rational; ordinary near-equal weights
  around one are not collapsed by this threshold. Equal weights can also be
  replaced by unit weights. The explicit `CheckRational=false` constructor
  retains accepted input weights and must be tested.
  This differs from the experimental native evaluator's finite signed-weight
  domain. An adapter cannot promise coefficient-preserving general round
  trips merely because both sides support rational splines. Test ordinary,
  very small, negative and nearly equal weights explicitly. Unsupported
  cases require a reported restriction; source coefficients must remain
  authoritative and unchanged.
  [Pinned spline implementation](https://github.com/Open-Cascade-SAS/OCCT/blob/b8f597c677811d1f9f4d8a97f5ae2825c0353a42/src/ModelingData/TKG3d/Geom/Geom_BSplineCurve.cxx)
* The surface constructor has a different rationality check: adjacent weights
  are compared against `Epsilon(weight)`. A variation of one representable step
  near one can be classified as non-rational and replaced with unit weights.
  The inspected constructor has no equivalent `CheckRational` switch. The
  harness includes this case. A general coefficient-preserving surface adapter
  needs a proven route or a reported restriction; it cannot assume the curve
  constructor's opt-out also exists for surfaces.
  [Pinned surface implementation](https://github.com/Open-Cascade-SAS/OCCT/blob/b8f597c677811d1f9f4d8a97f5ae2825c0353a42/src/ModelingData/TKG3d/Geom/Geom_BSplineSurface.cxx)

The isolated library build and first harness now pass, exit 0. The harness
confirmed the tested curve/surface weight restrictions, ordinary rational arc
weights/domain, independent versus shared wire identity, and deep-copy isolation.
See [commands and measured results](kernel_trial/README.md). Its constructors
use known coefficients directly; this is not yet a Blender source adapter,
numerical modeling trial or production backend choice. The subsequent known
cylinder/cone construction also preserved one curved shared edge and its two
different parameter maps through graph ingestion and deep copy, with maximum
sampled boundary difference `1.39449e-15`. The builder raised the requested zero
edge tolerance to `1e-7` model units. A future adapter must account for this
working tolerance separately from source tolerance and measured deviation;
these results do not authorize silent tolerance changes or a general exactness
claim. Native Blender conversion and trimmed numerical operations remain
unproven.

The mutation extension confirms that a surviving edge UID resolves after its
index changes from 1 to 0. It also demonstrates an ownership difference to
handle: compaction drops newly orphaned vertices unless explicit roots retain
them. A tested explicit-root policy preserved all four vertices after edge
removal. This is a concrete adapter requirement for Blender's loose-point
editing states, not evidence that every graph operation has Blender semantics.
