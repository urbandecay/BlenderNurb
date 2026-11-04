# Solid fillet comparison — November 4, 2026

The previous solid fillet constructed separate constant-radius cylinders and joined them
with setback patches. This produced the narrow corner wedge shown in the reported result.
That was a different construction from the native NURBS bevel, even when both produced a
closed solid. Closure and boundary tangency tests did not establish visual equivalence.

The native reference in `blenkernel/intern/nurbs_topology_bevel.cc` balances offsets measured
along adjacent faces, constructs one shared rational conic at a two-edge junction, and joins
the endpoint profiles with ruled strips. It does not insert that separate setback wedge.
Its three-edge junctions use affine spherical octants.

## Replacement

`solid/fillet_trimmed.cc` implements those stations and shared profiles in the independent
solid kernel. It solves the same width preferences and shared station ratios, stores each
junction as a three-point rational quadratic, and builds strips between those profiles.
Three-edge junctions retain affine sphere supports. The library still does not link Blender's
NURBS, mesh, or BMesh implementations. The NURBS dependency is confined to the comparison test.

Width now follows the reference's face-offset meaning. On angled geometry it is not a
constant-radius specification. Segments 0 still produces a chamfer; positive segment counts
partition curved source geometry, independently of viewport sampling. Display normals come
from the analytic surface derivatives, and each strip segment has one selectable source face.

Storage version 5 records the new profiles and ruled supports. Decoding retains support for
older cylinder and setback blend surfaces. Saved old fillets keep their geometry; recreating
the fillet is necessary to apply the new construction.

## Evidence

- `IndependentSolidFilletMatchesNativeSharedProfileReference` passed on 24 combinations:
  cap displacements 0.4 and 1.4, tapered/untapered boxes, top-perimeter/all-edge selections,
  and widths 0.1, 0.35, and 0.6. It compares shared profile samples and ruled surface samples
  against native NURBS geometry with a positional threshold of 2e-8 source units. It accounts
  for the two representations' different ruling parameters. These checks establish agreement
  on those samples, not a certified error bound for arbitrary solids.
- Both standalone kernel suites passed. The editing suite additionally checks isolated and
  parallel edges, angled face perimeters, source subdivisions, shared boundary incidence,
  display watertightness, affine reflection, duplication, serialization and extrusion of a
  duplicated rounded planar end face. Its existing 4,095-edge-subset coverage tests chamfers.
- All 16 Blender API integration tests passed.
- Six viewport tests passed: angled top-face wheel fillet, angled whole-body wheel fillet,
  clicking different parts of one strip, source-face selection, starter-line extrusion return
  to selection, and right-click return to selection. The wheel cases include undo/redo.
- An older saved 24-face fillet and the new saved 10-face example both reopened and remained
  selectable in Edit Mode.

An actual viewport comparison used identical rectangles from (-1,-1,0) to (1,1,0), extrusion
height 2, top displacement (1.4,0,0), width 0.35 and segments 1. Native and solid screenshots
show the same broad corner construction. Artifacts saved for local review:

- `/tmp/native_shared_profile_edit.png`
- `/tmp/solid_shared_profile_edit.png`
- `/tmp/solid_native_shared_profiles_comparison.png` — NURBS left, solid right.
- `/tmp/solid_native_shared_profiles_comparison.blend` — both editable source objects.

This addresses the reported convex angled-box case. It does not implement arbitrary curved
face fillets, concave bodies, or rounded endpoint fans with more than three faces. The native
reference itself reports uncertified tangency at its two-edge corner joins; matching it is
not a claim of universal G1 continuity across those shared profiles.

## External reference checked

The official [Open CASCADE fillet API](https://occt3d.com/dev/doc/refman/html/class_b_rep_fillet_a_p_i___make_fillet.html)
exposes radius prescriptions and separate continuity controls. That supports treating radius
and continuity as explicit construction choices. The specific shape target for this fix is
the local native NURBS implementation and the user's example, rather than an assumed CAD
fillet convention.

The replacement was built and installed at `../build_linux/bin/blender`. Restart that
executable and recreate an older fillet to use the replacement.
