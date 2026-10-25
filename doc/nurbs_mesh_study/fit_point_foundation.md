# Curve fit-point numerical foundation

This checkpoint builds reusable curve solving services. It does not activate
viewport fit handles, a new curve editing mode, or ordinary curved-source import.
The authoritative persistent source remains the existing rational NURBS payload.
No fit-handle identity or constraint metadata has been added to DNA in this slice.

## Implemented services

`evaluate_curve_basis` exposes rational span basis values using the same span
selection, Cox-de Boor recurrence, scaled rational weights and compensated sums
as the existing source evaluator. It supports the existing nonclamped domains
and repeated-knot/right-sided boundary rules. These are numerical evaluations,
not certified interval bounds.

`fit_freeform_curve` solves temporary position constraints at explicit curve
parameters. It retains the source identity, all weights/knots/degrees/domains,
and every coefficient outside the supplied internal editing range. Within that
range it minimizes Euclidean coefficient displacement for the computed linear
system. The scope is an internal geometry rule, not an exposed CV editing mode.
Constraint order is canonicalized; identical duplicate parameters are harmless
and conflicting targets reject. Anchors outside the editable basis support must
already meet their requested residual budget.

`interpolate_freeform_curve` constructs a new open clamped free-form curve using
caller-specified ordered parameters and degree, averaged interior knots, unit
weights and the same solver. It does not rebuild an existing source or choose a
parameterization implicitly. A standalone result has ID zero until its owner
allocates identity. Returned curves own their coefficient/knot buffers; failures
and result destruction never mutate/free the input source.

Blender's existing Eigen C API now has a double rectangular SVD solve. Geometry
construction and validation stay in BKE; generic matrix decomposition stays in
the existing numerical backend. The kernel does not pull Eigen types into its
public headers or duplicate a linear algebra implementation. The solve checks
full numerical rank and a caller-specified minimum singular-value ratio. It
rejects ill-conditioning instead of silently dropping singular values or using
regularization. It supports underdetermined minimum-norm and overdetermined
least-squares systems; curve acceptance still requires all point residuals to
meet the requested budget.

## Error and shape contract

Every solve re-evaluates all supplied parameters through the actual source
curve evaluator. The result reports the maximum measured point residual, matrix
conditioning/rank, maximum coefficient displacement and coefficient-addition
roundoff. A residual above the caller's absolute source-unit budget rejects;
so do non-finite data, unreachable constraints, conditioning failures and a
requested edit that cannot change stored coefficient positions. Error budgets
are not silently raised.

The point residual is not a bound on the entire curve, a proof of surface/trim
agreement, or a certificate of numerical solving/evaluation error. The addition
bound covers adding the computed coefficient displacement only. It excludes
basis/decomposition rounding, target construction and owner/world transforms.
The solver uses doubles, including coordinates too small for float storage, and
reports actual residuals rather than claiming exact real-arithmetic interpolation.

Positive weights are required for free-form fitting. Initial dense limits are
256 editable coefficients and 512 constraints; new interpolation allows 2-256
points. Factorization caching, sparse/local scaling and interactive performance
still need their own gates.

This explicitly free-form service is not an exact-circle/arc editing algorithm.
Moving coefficients in a rational circle's fixed basis can destroy circularity.
An editor must identify analytic intent and use the corresponding constrained
construction; it must never silently send a circle through the free-form route.
No ordinary circle/arc editing path has changed. No-op checks retain every source
coefficient, weight and knot on the existing rational arc and circle fixtures.

## Before viewport activation

Persistent handles must have stable identity and source parameters, with explicit
bindings to shared endpoint identities where applicable. An evaluated curve point
can supply the displayed handle position; a second conflicting geometry authority
must not be introduced. A handle's influence/anchor/analytic constraints and all
source geometry must commit together. Extending the existing owner requires
verified copy/remap, deletion, affine transforms, undo/read snapshots, save/read,
selection/history, picking/drawing and Python lifecycle behavior.

The following user-facing shape choices were asked in the conversation and are
still pending; no default was silently activated:

1. Dragging a free-form fit point: hold other fit points fixed, or move nearby
   points with a smooth falloff.
2. Dragging a point on an exact circle: hold its other defining fit points fixed
   while center/radius may change, or keep its center fixed and change radius.

Further integration must define local support/refinement, end/tangent/continuity
constraints, closed/periodic seams, analytic family preservation, shared surface
boundary propagation, unsupported-case reporting and gesture-independent selection.
Curve point insertion/shape handles must remain distinct from actual topology
cuts; Ctrl+R still needs real selectable edge/face splits. The numerical API here
does not settle that editor interaction by itself.

## Evidence and references

The new tests cover rational basis/evaluator agreement, nonclamped/repeated
knots, fixed anchors, local span isolation, minimum displacement, rational no-op
preservation, input-order/duplicate handling, strict residual/conditioning
rejection, exact lines, nonuniform spatial interpolation, large source origins,
sub-float coordinates, and malformed input. Generic backend tests cover multiple
right-hand sides, under/overdetermined solves, conditioning and failure atomicity.
The app/fifteen executable targets build; **269 C++ checks pass without skips**,
five CTest targets pass (39 NURBS Python cases), and the existing R/S viewport
workflow passes. Exact commands and corrected build failures are recorded in the
checkout audit.
No new fit-handle viewport behavior is claimed built/tested/visually confirmed.

Primary numerical references consulted for this implementation:

- [Eigen linear algebra and least-squares decompositions](https://eigen.tuxfamily.org/dox/group__TutorialLinearAlgebra.html).
- [Eigen SVD rank/solve threshold behavior](https://eigen.tuxfamily.org/dox/classEigen_1_1SVDBase.html).
- [McNeel NURBS geometry overview](https://developer.rhino3d.com/guides/opennurbs/nurbs-geometry-overview/).

These inform numerical/representation choices; their availability is not proof
that this fork implements a complete fit-point editing workflow.
