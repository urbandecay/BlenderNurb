# Subdivision conversion experiment: rejected as the production bevel shape

The user found that the exact Catmull-Clark target bows a previously straight
planar-face rail. This experiment therefore does not meet the boundary shape
requirement, even though its subdivision comparison and continuity tests pass.
The production conversion was removed. Code and tests are preserved under
`/tmp/nurbs-subdivision-experiment/` for research and comparison. The measurements
below describe that experiment, not the current default bevel.

## Behavior and scope

A rounded bevel of all edges of a closed planar body now takes the same polygon
control cage as Blender's mesh bevel and constructs native polynomial NURBS
surfaces from its Catmull-Clark subdivision rules. The rounded corner rows follow
the subdivision surface instead of independently drawing auto-handled curves on
an exact spherical support. Final geometry is stored as native NURBS faces,
shared topology edges, spatial curves and parameter curves; the reference cage
is temporary.

This path currently applies to a fully selected closed straight/planar body.
Partial edge networks and curved-source bevels retain their existing construction.
`BevelOptions::subdivision_surface = false` explicitly selects the analytic
circular/spherical construction in the kernel. There is no new UI setting.
Subdivision-shaped bevels intentionally differ from exact constant-radius fillets.
Their highlighted boundary is derived from the subdivision cage, so it can move
relative to the previous analytic boundary.

## Construction

The implementation follows Jorg Peters, **Patching Catmull-Clark Meshes**,
SIGGRAPH 2000, sections 3 and 4:
https://www.cise.ufl.edu/research/SurfLab/papers/00pccm.pdf

* Regular all-quad neighborhoods are converted directly from a 4x4 B-spline
  control net to a clamped bicubic NURBS patch. This exactly represents the
  regular Catmull-Clark limit shape, apart from bounded arithmetic roundoff.
* Irregular neighborhoods use shared Catmull-Clark refinement lattices. A ghost
  row from adjacent faces supplies the same tensor spline data on each side.
* Clamping and double-knot insertion produce the PCCM coefficient layout.
* At extraordinary vertices, the paper's A and B matrices construct a common
  tangent plane and compatible first interior rows. The displacement of Q20 is
  also applied to Q21 and the adjacent Q12; omitting this step gives a visible
  tangent-plane mismatch despite position-continuous boundaries.
* Odd/even valences use their respective matrix formulas. Even valences above
  four additionally receive the alternating-sum correction.
* Non-quad faces are represented by their first-step PTex quads. In the ordinary
  box case, each odd-segment corner triangle becomes three NURBS patches; even
  segment counts keep the mesh bevel's original quad topology.
* Every shared edge has one spatial NURBS curve, referenced by both faces through
  their own exact linear parameter curves. Edges of unlike-sized PTex patches
  are split consistently instead of welded by coordinate proximity.

PCCM is a finite, tangent-continuous approximation near extraordinary vertices.
It is not claimed to represent the complete extraordinary Catmull-Clark limit
surface exactly. The reported construction error bound encloses arithmetic and
knot reduction relative to this PCCM construction; it is not the PCCM-to-limit
approximation error. The temporary control cage uses BMesh's float geometry.

## Efficiency

Regular patches bypass uniform refinement coefficient extraction. Their integer
conversion masks use extended precision with a forward roundoff enclosure.
Irregular patches remove redundant refinement knots by inverse knot insertion,
then reinsert each candidate knot and bound its full coefficient residual.
Positive B-spline bases turn that residual into a whole-surface error bound.
This is coefficient reduction, not fitting a few surface samples.

Boundary curves are copied directly from clamped surface rows/columns; only
split boundaries need interval restriction. Native width limiting happens once,
before BMesh runs, so a replay at the reported width has identical results.

## Validation

`SubdivisionBevelMatchesBlenderLimitAndHasSmoothSharedEdges` independently creates
Blender mesh bevel cages at 2, 3, 8 and 13 segments. It compares dense NURBS
surface samples against Blender/OpenSubdiv limit evaluation and checks positions
and tangent planes along the full shared boundaries. It also checks the integrated
bevel against the independently generated control cage conversion.

Measured maximum sampled position errors, with bevel width 0.25:

| Segments | Regular regions | Extraordinary corner region |
| --- | ---: | ---: |
| 2 | 0.000000570 | 0.000857559 |
| 3 | 0.000000568 | 0.000555669 |
| 8 | 0.000000568 | 0.000146041 |
| 13 | 0.000000664 | 0.000083407 |

The regular-region comparison includes OpenSubdiv float evaluation error. At
8 segments the corner difference is approximately 0.0584% of the bevel width.
Maximum shared-boundary normal-angle differences are below 0.000003 degrees.
The maximum sampled normal difference from the subdivision target at eight
segments is about 0.241 degrees in the approximate corner region.

The real event-simulated 8-segment modal workflow passed preview updates,
cancellation, confirmation, undo/redo and edit-mode reentry. Using the same 14
nonzero-width updates and identical width sequence, one sequential before/after
run measured:

| Version | Median complete modal update | Range |
| --- | ---: | ---: |
| Previous spherical auto-handle construction | 67.291 ms | 64.208-69.951 ms |
| Subdivision NURBS construction | 65.972 ms | 62.854-71.591 ms |

This is a single controlled run, not a general speedup claim. Full-quality
reconstruction after confirmation is more expensive than the interactive preview.

The 13-segment modal workflow also passed cancellation, confirmation, undo/redo
and edit-mode reentry. The tapered 3-segment workflow additionally passed moving
and duplicating the resulting native faces.

Actual viewport captures (identical camera, 8 segments and width 0.25):

* `/tmp/nurbs-subdivision-old8-wire.png`
* `/tmp/nurbs-subdivision-new8-wire.png`
* `/tmp/nurbs-subdivision-mesh8-wire.png`
* `/tmp/nurbs-subdivision-new8-shaded.png`
* `/tmp/nurbs-subdivision-mesh8-shaded.png`

The mesh reference uses a level-two Subdivision Surface modifier with its edit
cage on the evaluated surface. The images are from real test app instances in
private Weston displays. Their windows do not appear on the user's desktop.

Final regression coverage also includes the full topology-edit suite (59 applicable
cases, with ten saved-workflow artifact tests skipped), all 29 mesh evaluation/cache
cases, and all 31 transform cases. The cache comparison covers both subdivision
and analytic construction: the subdivision charts reuse the complete layout as
width changes, while analytic strips can be reused independently of changing trims.

The additional asymmetric hexagonal-prism check exercises even-valence corner
correction and non-quad PTex joins; shared-edge tangent planes remain aligned.
This coarse, unbeveled cage has a larger local approximation error than the
beveled box, as expected; the tighter box accuracy requirements remain unchanged.
A visible test scene is `/tmp/nurbs-subdivision-ready.blend`; its app session log
is `/tmp/nurbs-subdivision-visible-session.log`.
