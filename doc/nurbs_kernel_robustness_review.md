# NURBS kernel robustness review

Reviewed November 2, 2026, at source revision `8a6aff8bb05`.

**Assessment: this is a carefully structured experimental NURBS modeling kernel, with strong safeguards in several supported operations. It is not yet a broadly robust general NURBS modeler.** Its numerical foundation and source ownership deserve confidence within their documented contracts. Its corner construction, curved intersection coverage, geometric validation, and approximation guarantees remain incomplete.

“Robust” has several meanings here: preserving source data when an edit fails; producing numerically controlled results; handling ordinary modeling configurations; and detecting invalid geometry. The implementation does substantially better at the first two than at general modeling coverage. An operation that safely refuses a valid shape demonstrates failure containment, but does not demonstrate that the shape is supported.

This review inspected the native kernel interfaces and selected implementations for storage, evaluation, topology, editing, refinement, fitting, beveling, preview generation, and logging, plus their tests. It is a source review with existing-binary test evidence, not an exhaustive audit of every branch, a sanitizer run, or a reproduction using the live object in your screenshots.

**Findings, in priority order**

| Priority | Finding | Evidence strength | Practical consequence |
| --- | --- | --- | --- |
| High | Rounded bevel rejects every affected vertex with more than three incident topology edges | Confirmed source behavior | Valid, bevelable configurations can be refused before construction starts |
| High | General curved bevel contact and end-cap intersection construction is missing from this path | Confirmed explicit rejection paths | A simple-looking edit can exceed the implemented geometry cases |
| High for precision-sensitive use | Subdivision bevel fitting depends on float geometry and sampled error checks | Confirmed implementation; effect on a particular model not reproduced | Double output storage and a small construction budget do not establish overall shape accuracy |
| Medium | Structural validity and successful preview do not establish geometric or solid validity | Confirmed documented contracts | Passing these gates cannot prove absence of gaps, intersections, or singularities everywhere |
| Medium | Diagnostics omit the offending topology and use a shared, truncated log file | Confirmed implementation | Failures are hard to localize, and evidence can be lost between processes |
| Medium | Test success includes expected rejection and skipped workflow checks | Confirmed tests and execution | A green suite does not mean all ordinary modeling cases work |

**1. The bevel error is an implementation restriction, not a proof that the shape cannot be beveled.**

In [`nurbs_topology_bevel.cc`](../source/blender/blenkernel/intern/nurbs_topology_bevel.cc#L1568), the planar network path gathers endpoints of selected edges. It rejects endpoints with fewer than three incident edges, then rejects rounded bevels whenever the incident edge count is not exactly three. That second rejection emits your exact message at line 1584. It occurs before the offset and corner construction work.

The count comes from `TopologyIndex::edges_for_vertex`, which includes **all** topology edges attached to the vertex, including unselected and wire edges. [`nurbs_geometry.cc`](../source/blender/blenkernel/intern/nurbs_geometry.cc#L425) builds that index from edge endpoint identities; it does not infer the count from the silhouette or the visible surface fan. Thus, even a three-edge surface corner with an extra attached wire would fail this guard. This is a deduction from the code, not a claim that your model has that wire.

The public bevel contract explicitly documents the valence-three restriction in [`BKE_nurbs_topology_edit.hh`](../source/blender/blenkernel/BKE_nurbs_topology_edit.hh#L78). Removing the check alone would not implement the missing corner construction; the subsequent algorithm also assumes three-sided corner machinery.

The check also isn't a faithful test of “how many sides does this rounded corner have?” It measures total incident edges, while the later corner code makes decisions using selected edges and incident face stations. That mismatch is a concrete candidate for rejecting some otherwise supported corners. It merits a targeted regression case: take a valid three-edge manifold surface corner, attach an unselected wire edge at the same vertex, and compare the bevel with the same model before and after adding the wire. The current source predicts the rounded result will be rejected before the wire can be shown to affect the required corner construction. This predicted case is **not yet executed**.

**How mesh and NURBS beveling actually work in this fork**

It would be inaccurate to say the mesh corner algorithm has not been ported. [`bevel_mesh_reference()`](../source/blender/blenkernel/intern/nurbs_topology_bevel.cc#L611) creates a temporary BMesh from the NURBS topology, selects the requested edges, and calls Blender's real `bevel` operator at lines 661–679. It collects the resulting mesh vertices and faces. The NURBS corner builder then borrows their layout and positions: [`partition_rounded_corner_patches()`](../source/blender/blenkernel/intern/nurbs_topology_bevel.cc#L823) follows the same `mesh_vert_canon` index mapping, and lines 1056–1108 match the mesh corner faces and vertices to NURBS corner nodes.

This is substantive reuse of mesh behavior. BMesh decides a reference bevel cage, including corner layout. The NURBS path remains responsible for constructing native curves and support surfaces, edge p-curves, shared topology IDs, trims, and its own source error checks. It cannot simply store BMesh's answer as authoritative geometry: doing so would make the edited result a mesh, not an editable NURBS model.

The difference in implementation scope matters. BMesh bevel has general vertex-mesh (VMesh) machinery and separate handling for tri-corners, cube corners, pipes, and miter/profile cases. Its adjusted VMesh code handles variable boundary sizes (`bmesh_bevel.cc` around `adj_vmesh()`, lines 5082–5095), with additional special-case conditions. That does **not** mean Blender mesh bevel succeeds for every imaginable non-manifold or degenerate shape. It means the mesh code has a much broader set of corner layouts and fallback strategies than this NURBS reconstruction path currently maps to NURBS surfaces.

The conversion also has limits. The bevel reference refuses faces with multiple loops (such as a face with holes) at [`nurbs_topology_bevel.cc`](../source/blender/blenkernel/intern/nurbs_topology_bevel.cc#L616), and it converts source coordinates from double to float when creating BMesh vertices at lines 632–635. Thus it is a useful guide for certain inputs, not a universal, double-precision oracle for every trimmed NURBS model.

Mesh bevel can form polygonal corners without promising a particular smoothness between faces. NURBS must provide actual smooth patches and matching boundary curves. The native code builds conic rounded strips and, at some trivalent corners, a spherical or affine-octant patch. It rejects unsupported fans before that construction, and it can fail later if face directions, station counts, parameterization, or error budgets do not fit. This is engineering work in the mapping/reconstruction layer—not evidence of a fundamental impossibility and not just a matter of copying the BMesh function again.

The UI also has its own modal workflow. [`editnurbs_bevel.cc`](../source/blender/editors/curve/editnurbs_bevel.cc#L119) builds candidate geometry from the kernel, previews valid candidates, and preserves the last valid preview when a changed width or segment count fails (lines 283–322). That is careful interaction and failure handling; it cannot add a missing geometry construction.

**What the mesh comparisons establish**

The regression suite includes meaningful parity checks. A basic single-edge bevel checks analytic rational geometry and tangent continuity in [`RoundedBevelIsSharedTangentRationalGeometry`](../source/blender/blenkernel/intern/nurbs_topology_edit_test.cc#L496). Connected cases compare topology/geometry signatures directly with BMesh in [`MultipleSeparateBevelsShareFacesAndMatchBMesh`](../source/blender/blenkernel/intern/nurbs_topology_edit_test.cc#L954). Other tests cover box and tapered-box selections, skew and rotation, clamping, connected chains, and subdivision-shaped results.

Those comparisons are fixture-specific. The pyramid-subset test explicitly records eight expected unsupported edge selections in [`BevelPyramidSubsetsMatchBMeshOrReportUnsupportedCap`](../source/blender/blenkernel/intern/nurbs_topology_edit_test.cc#L2449). In that test, successful cases compare vertex/edge/face counts, but compare coordinates against mesh only for a single selected edge or the complete edge set (lines 2495–2505). The skipped cases don't establish mesh parity, and matching topology counts alone doesn't establish matching shape. Other saved-workflow tests skip until separate UI workflows write their source files. So the tests show both real reuse and real gaps; they do not show broad parity over arbitrary vertex fans.

**How far is parity?**

There is no useful “percentage complete” or time estimate from source inspection. The key architecture already exists: stable NURBS topology, operation-specific candidates, a BMesh bevel reference, and native NURBS construction for some common corner classes. The remaining distance is mainly expanding and validating the mapping across the mesh bevel's supported corner layouts and selection patterns, plus handling NURBS-only cases such as trims, holes, curved supports, and shared per-face parameter curves.

A practical parity study should define the target Blender version, then run the same parameterized cases on mesh and NURBS fixtures. At minimum, vary vertex valence/fan order; one, adjacent, loop, and all incident selected edges; open, closed, non-manifold, and wire-attached fans; triangle/quad/ngon faces; holes; convex, concave, flat, and skewed corners; curved/ruled supports; profiles and segment counts; widths near collision; scale, rotation, and large translations. For each case record: whether each tool accepts it; resulting topology counts and incidence; vertex/layout deviations; analytic error or sampled approximation error; tangent continuity; trim closure and self-intersection checks; and whether a failed edit leaves source and selection unchanged. This should classify intentional behavior differences instead of treating every mesh success as an automatic NURBS requirement.

For user-visible “as robust as mesh bevel” claims, parity needs both a documented supported-case matrix and an automated paired suite that exercises it. Passing all current NURBS tests, or visually matching a cube, is not sufficient evidence on its own.

There are tests for tapered boxes, including rounded bevels, in [`nurbs_topology_edit_test.cc`](../source/blender/blenkernel/intern/nurbs_topology_edit_test.cc#L1634). So tapering or raising the top is not, by itself, the demonstrated reason for rejection. To establish the exact reason in your object, diagnostics must record the selected edge IDs, rejected vertex ID, its coordinates, all incident edge IDs, and their face uses. The screenshots do not supply that information. My earlier suggestion that the post/base junction was responsible was a hypothesis, not a verified diagnosis.

**2. Curved modeling coverage relies on specific constructions.**

The curved bevel path requires compatible ruled supports, natural tensor boundaries, matching bases, and supported endpoint arrangements. It explicitly refuses non-ruled supports needing contact-curve tracing, rational transverse contact solving, and end caps needing surface intersection. See [`nurbs_topology_bevel.cc`](../source/blender/blenkernel/intern/nurbs_topology_bevel.cc#L3097), particularly lines 3126, 3166, and 3269.

Those are meaningful missing algorithms. A collection of exact constructions for supported cases does not replace a general intersection and trimming system. The current restrictions can be appropriate development safeguards, but users will encounter valid shapes that the tools cannot complete. The public API also states that general curved self-collision and circularity are not certified.

**3. Subdivision bevel has a precision boundary that needs explicit accounting.**

[`nurbs_topology_bevel_subdiv.cc`](../source/blender/blenkernel/intern/nurbs_topology_bevel_subdiv.cc#L327) converts source vertex positions from double to `float3` when creating its BMesh cage. Its subdivision target is then evaluated through float parameters and float geometry, with results promoted back to doubles at lines 416–430. Promoting the result does not restore discarded coordinate precision.

For scale intuition, the double coordinate `1000.00001` rounds to `1000.0` in binary32. A small feature at that object-local coordinate can therefore change before fitting begins. This illustrates the conversion risk; it is not a reproduced failure of a complete bevel.

The routine reports a coefficient roundoff allowance based on double epsilon at lines 584–593. It separately measures fitting deviation at finite holdout samples, including a 7-by-7 surface grid at lines 755–768. The caller rejects sampled deviation above one percent of the bevel width in [`nurbs_topology_bevel.cc`](../source/blender/blenkernel/intern/nurbs_topology_bevel.cc#L2964).

The code correctly distinguishes sampled fitting deviation from coefficient roundoff. However, these checks do not establish a global bound relative to the subdivision limit of the original double cage: the target itself has already passed through float storage, and unsampled regions remain unchecked. Precision claims should separately account for input quantization, target evaluation, fitting, and coefficient arithmetic. Recentered/scaled evaluation could reduce the quantization risk, subject to measured bounds and regression coverage.

**4. Validation is useful but deliberately incomplete.**

The structural validator checks identities, references, knot storage, finite coefficients, tolerances, and closed vertex chains. Its [public contract](../source/blender/blenkernel/BKE_nurbs_geometry.hh#L123) explicitly excludes geometric edge/support agreement, denominator regularity over the domain, trim intersections, continuity, orientation consistency, and solid validity. Checking that orientation is encoded as +1 or -1 is not a proof of globally consistent orientation.

Likewise, [`EditData::commit_impl`](../source/blender/blenkernel/intern/nurbs_edit.cc#L323) checks the candidate's structure and selection, then swaps it atomically after checking its source revision. Geometric acceptance remains the operation's responsibility.

Boundary evaluation provides whole-interval coefficient bounds for some supported representations and sampled agreement checks more generally. Preview tessellation checks sampled UV polygons and triangulator provenance, but explicitly offers no certified world-space approximation bound or exact trim-intersection proof. See [`BKE_nurbs_evaluate.hh`](../source/blender/blenkernel/BKE_nurbs_evaluate.hh) and [`BKE_nurbs_tessellate.hh`](../source/blender/blenkernel/BKE_nurbs_tessellate.hh#L103).

This separation is legitimate, especially because Blender editing permits open and non-manifold intermediate states. The weakness is that there is no universal geometric guarantee to infer from a successful commit or clean preview. Operations that need stronger properties must establish them explicitly. Uniform sampling can miss narrow features, curvature peaks, or crossings between samples.

**5. Logging is inadequate for the bevel diagnosis and fragile across sessions.**

The valence rejection records a fixed message without the vertex ID, valence, coordinates, selected edges, or incident faces. The caller adds timings and general context, but those do not identify the offending corner.

The macro in [`BKE_nurbs_debug.hh`](../source/blender/blenkernel/BKE_nurbs_debug.hh#L19) calls the logger unconditionally. [`nurbs_debug.cc`](../source/blender/blenkernel/intern/nurbs_debug.cc#L26) opens `/tmp/nurbs-debug.log`, or the environment override, using mode `"w"`. Each process's first log call truncates that path. Its mutex protects threads within one process; it does not coordinate separate Blender/test processes. Every line also performs a synchronous flush. The performance cost was not benchmarked in this review.

Use process/session-specific log paths, configurable logging, and structured failure details. Ideally the editor should also identify the rejected vertex visually. Correction to my earlier advice: this fork does not require a separate enable flag to activate this logging.

**What the code does well**

- **Source ownership and failure containment:** Independent geometry candidates, immutable retained snapshots, and revision-checked commits reduce the chance that a failed or stale operation damages the live object. The commit checks its expected state again under the lock before swapping it.
- **Explicit topology:** IDs distinguish components independently of coincident coordinates or display triangles. Edge curves, per-face parameter curves, and surface supports are separate resources. This is a sound foundation for trimmed surface editing.
- **Numerical care:** [`nurbs_math.hh`](../source/blender/blenkernel/intern/nurbs_math.hh) checks supported rounding/subnormal behavior and implements bounded arithmetic, outward rounding, FMA residual handling, and underflow allowances. The evaluator recenters coordinates and rescales weights before compensated summation in [`nurbs_geometry_evaluate.cc`](../source/blender/blenkernel/intern/nurbs_geometry_evaluate.cc#L152).
- **Honest error contracts:** Refinement distinguishes bounded construction error from sampling; curve fitting rejects ill-conditioned systems and reevaluates supplied constraints. The implementation generally makes its limitations explicit rather than silently claiming exactness.
- **Substantial regression coverage:** Tests exercise source preservation, ID exhaustion, rational curves, rotations, extreme scales, shared boundaries, cache invalidation, persistence, and comparisons with BMesh behavior.

These strengths justify extending the architecture. They do not establish that every operation has the same numerical guarantees.

**Test evidence and its limits**

I ran the 12 available native NURBS test executables under `../build_linux/bin/tests`. Final results, counting the corrected geometry-suite invocation, were:

| Executable suffix | Passed | Skipped |
| --- | ---: | ---: |
| differential | 11 | 0 |
| edit | 50 | 10 |
| fill | 19 | 0 |
| fit | 14 | 0 |
| geometry_legacy | 23 | 0 |
| geometry | 61 | 0 |
| mesh | 34 | 0 |
| object | 6 | 0 |
| selection | 43 | 0 |
| tessellate | 17 | 0 |
| transform | 43 | 0 |
| topology_edit | 75 | 10 |
| **Total** | **396** | **20** |

The initial geometry invocation failed because I omitted `--test-assets-dir`; rerunning with this checkout's `tests/files` path passed all 61 tests. The skips include checks requiring saved output from separate UI workflows. I did not run those workflows.

The binaries predate some current source files; for example, the topology test executable was dated November 1 and the bevel source November 2. I attempted a targeted refresh, but it required a broader Blender dependency rebuild and stopped it before completion. Consequently these results describe the available prebuilt binaries, not verified execution of the exact reviewed revision. No implementation changes were made.

Test totals also overstate modeling coverage if interpreted without reading expectations. For example, [`BevelPyramidSubsetsMatchBMeshOrReportUnsupportedCap`](../source/blender/blenkernel/intern/nurbs_topology_edit_test.cc#L2449) expects eight particular edge-selection cases to be rejected. That is valuable regression coverage for known limitations, not successful construction in those cases.

Execution logs and the initial machine-readable results are in `/tmp/nurbs-kernel-review-tests`; the corrected geometry run is `geometry-with-assets.log`. The first parallel test pass used the kernel's default shared logger and may have replaced prior contents of `/tmp/nurbs-debug.log`. Treat that shared file as test output now, not preserved evidence of the earlier interactive bevel session. The corrected geometry run used an isolated debug log.

**Recommended work sequence**

1. Add offending-component diagnostics to bevel rejection and preserve logs by session. Capture the actual failing object's topology before attributing the failure to a particular junction.
2. Extend rounded corner construction beyond valence three. Distinguish the relevant surface fan from unrelated wire incidence, and cover partial selections and repeated bevels. Retain the guard until the replacement construction is valid.
3. Add the curved contact/intersection and trim-reconciliation services required by the rejected cases, with explicit convergence and error contracts.
4. Account for float quantization in subdivision fitting, and distinguish measured deviation from certified approximation error in both APIs and UI.
5. Add operation-appropriate geometric validation and adaptive preview error controls while continuing to permit intentional open/non-manifold editing states.
6. Rebuild and run current-source tests, then expand end-to-end and randomized operation sequences: extrude, cut, move, bevel, merge, save/reload, and bevel again. Compare cached and fresh evaluation, selection order, scale, translation, and source preservation after rejection.

For the reported bevel, the immediate engineering task is to expose which actual vertex triggers the count and what edges it owns. The source already establishes a missing rounded-corner capability; it does not establish that your visible shape is geometrically impossible.
