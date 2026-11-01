# NURBS surface wire display

The Shape panel's Isocurve Density setting stores the number of interior U and V
curves per knot span on the curve data. It defaults to one, persists in blend files,
and updates the viewport independently of Preview/Render Resolution. Values from
one to 64 are supported. It affects display guides, not source topology.

Wireframe shading, the Wireframe overlay and object wire display show constant-U and
constant-V curves. By default, interior knot values and two subdivisions of each
visible knot span determine curve placement; shading tessellation resolution does not determine
the interior wire layout. A single-span plane displays one U/V cross. Source trim boundaries and loose source edges remain
visible through the existing display paths.

The evaluator extracts rational surface isocurves to establish their bases and knot
intervals. Display positions are evaluated from the identical surface parameters,
using the same arithmetic path as cached surface samples. Each visible interval is
sampled across knot spans and rendered as chords. Like every viewport curve, the
screen display is an approximation, not an editable source curve.

Intervals are clipped by parity against the preview's sampled UV trim loops, which
also define its shaded region. This clips holes and concave boundaries without
changing source trims, surfaces, edges, weights or knots. It does not certify exact
intersections with unsampled curved trim intervals.

Wire samples retain face IDs and UV parameters in the immutable preview. Partial
refresh remaps their derived indices, and rigid translations transport their
positions with the owning face. The evaluated Mesh carries a private point-link
attribute for the GPU batch; isocurves are not loose mesh edges and appear only
when surface wire display is enabled. Shading triangle diagonals are hidden.
