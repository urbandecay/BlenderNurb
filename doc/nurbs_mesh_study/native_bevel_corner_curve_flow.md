# NURBS bevel grid: mesh stations, exact rails and interpolating rows

## Target and rejected experiment

The reference is Blender's regular mesh bevel, without a Subdivision Surface
modifier. Preserve its edit-vertex positions and grid connectivity, retain exact
straight rails and flat faces, and draw smooth NURBS curves through those points.

A full Catmull-Clark-to-NURBS experiment reproduced the subdivision limit closely
but bowed a rail that must stay straight. That shape rule was rejected and removed
from production. Its code and measurements are archived under
`/tmp/nurbs-subdivision-experiment/` and described in
[native_bevel_subdivision_experiment.md](native_bevel_subdivision_experiment.md).

## Defects found

The earlier latitude/longitude support had a collapsed pole. A stereographic
chart fixes that singularity while retaining an exact rational spherical support.
That improvement alone did not make the grid agree with the mesh.

Two further issues caused the visible mismatch:

1. The strip cuts used evenly spaced *NURBS parameters*. Those are not evenly
   spaced circular angles. For example, the first 8-segment cut at t=1/8 is about
   10.48 degrees, while the mesh station is at 11.25 degrees. Corner interior
   points were matched to BMesh, but their shared strip-boundary points were not.
   The previous regression checked only interior sphere points and missed this.
2. Independent automatic handles aligned curve tangents but did not match the
   curvature of an entire row. This could produce uneven bends between otherwise
   correct endpoints and tangent directions.

## Construction

### Match profile stations before partitioning

For a circular quadratic with normalized middle weight w, let h=acos(w). A point
at angle a has the natural rational parameter

    t = sin(a/2) / (sin(a/2) + sin(h-a/2)).

Power-of-two mesh profiles use equal-angle stations. BMesh resamples the corner
lattice for other segment counts, so its actual stations differ slightly from
ideal angle spacing. At 3 segments, for example, the measured mesh angle is about
29.9566 degrees, rather than exactly 30 degrees. The existing BMesh guide supplies
those actual angles where needed; they are converted back onto the authoritative
exact NURBS circle. There is no coordinate welding or post-construction rail move.

`detail::plan_loop_cut_parameters` transports the selected seed parameters through
the existing loop-cut planner. Shared edges and face parameter coordinates are
therefore planned consistently before `partition_components` constructs them.
The public uniform loop-cut operation retains its original behavior.

### Solve complete corner rows

The corner graph identifies opposite edges at regular four-way vertices. Trace
each complete row, parameterize its point sequence by physical chord lengths, and
solve a clamped C2 cubic interpolation system in the regular sphere chart. The
existing straight strip supplies the endpoint tangent direction. Closed rows use
a periodic system. Original boundary curves remain authoritative.

The resulting spans are composed with the stereographic rational sphere formula.
Each spatial span is a degree-six rational NURBS curve; each incident face shares
that same spatial edge. Cubic span coefficients are storage for the solved whole
spline, not independently guessed automatic handles. Regular interior joins have
matching tangents and geometric curvature. Sphere-to-cylinder joins retain their
analytic tangent behavior; G2 curvature continuity at those joins is not claimed.

The analytic surface shape remains exact. There is no Catmull-Clark rounding of
planar-face boundaries and no additional subdivision of the editable topology.
Skewed Coons supports retain their previous construction path.

## Validation

The rounded connected-box regression now compares **every** edit vertex against
Blender's actual bevel, including strip/corner boundary stations. At width 0.25:

| Segments | Maximum distance to mesh vertex |
| --- | ---: |
| 2 | 0.0000000385 |
| 3 | 0.0000000668 |
| 8 | 0.0000000520 |
| 13 | 0.0000001070 |

The mesh guide uses BMesh float coordinates; native curves retain exact analytic
circle/sphere geometry within the existing construction-roundoff budget.

`RoundedBevelKeepsFlatFacesAndStraightRails` samples the complete retained planar
face boundaries at all four segment counts and requires less than 1e-11 model
units of bow from their endpoint chord. The row regression checks whole paths,
monotonic progress, endpoint tangents, and geometric curvature across opposing
interior edges. Its largest measured curvature jump at 13 segments is about
6.7e-11; the acceptance threshold is 1e-5.

Actual orthographic captures use the same camera, cube, width 0.25 and 8 segments:

* `/tmp/nurbs-meshmatch8-front-wire.png` — corrected NURBS
* `/tmp/nurbs-raw-mesh8-front-wire.png` — regular mesh bevel, no subdivision modifier
* `/tmp/nurbs-meshmatch8-oblique-wire.png` — corrected corner from an oblique view

These replace the earlier comparison against a subdivided mesh, which was the
wrong target for the straight-boundary requirement.

## Workflow and caching checks

Real event-simulated workflows passed at 8 and 13 segments: repeated width changes,
cancellation, confirmation, undo/redo and edit-mode reentry. The tapered 3-segment
workflow also passed moving and duplicating native geometry. Topology counts
remain the regular mesh bevel counts (488/972/486 at 8 segments and
1176/2352/1178 at 13 segments).

All 29 mesh evaluation/cache tests and all 31 transform tests passed. Width refresh
is compared against a completely fresh evaluation at both 8 and 13 segments.
Stable power-of-two profile charts preserve reuse of straight strip layouts.
Non-power-of-two mesh guide stations can change at float precision with width,
so their changed parameter charts are revalidated/rebuilt rather than reused
without checking.

One functional UI run recorded 14 nonzero-width updates with median 71.572 ms
at 8 segments and 171.064 ms at 13 segments. Other regression processes were
running during part of this run; these are observations, not a controlled
performance comparison with the rejected subdivision experiment or the baseline.

The visible corrected scene is `/tmp/bevel-mesh-match.blend`, with its live
session diagnostic log at `/tmp/nurbs-meshmatch-visible-session.log`. The window
named `nurbs-subdivision-ready` is the earlier rejected experiment.
