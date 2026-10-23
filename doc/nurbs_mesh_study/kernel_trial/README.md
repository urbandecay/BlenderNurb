# Isolated OCCT ownership trial

This harness is research outside Blender's build. It does not add a dependency
or activate a NURBS editing path. It constructs known source coefficients
directly; it is not yet an adapter from Blender's `NurbsGeometry` storage.

Pinned source: OCCT `V8.0.1`,
`b8f597c677811d1f9f4d8a97f5ae2825c0353a42`.

```sh
git clone --depth 1 --branch V8.0.1 https://github.com/Open-Cascade-SAS/OCCT.git \
  /tmp/blender-nurbs-occt-trial/source
cmake -S /tmp/blender-nurbs-occt-trial/source \
  -B /tmp/blender-nurbs-occt-trial/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRARY_TYPE=Shared \
  -DBUILD_MODULE_FoundationClasses=OFF -DBUILD_MODULE_ModelingData=OFF \
  -DBUILD_MODULE_ModelingAlgorithms=OFF -DBUILD_MODULE_Visualization=OFF \
  -DBUILD_MODULE_ApplicationFramework=OFF -DBUILD_MODULE_DataExchange=OFF \
  -DBUILD_MODULE_Draw=OFF -DBUILD_ADDITIONAL_TOOLKITS=TKBRep \
  -DBUILD_GTEST=OFF -DUSE_FREETYPE=OFF -DUSE_OPENGL=OFF \
  -DUSE_TBB=OFF -DUSE_EIGEN=OFF \
  -DINSTALL_DIR=/tmp/blender-nurbs-occt-trial/install
nice -n 19 cmake --build /tmp/blender-nurbs-occt-trial/build \
  --target TKBRep --parallel 1
```

Clone, configure and the `TKBRep` dependency build passed, exit 0. Logs:
`/tmp/blender-nurbs-occt-trial-clone.log`,
`/tmp/blender-nurbs-occt-trial-configure.log`,
`/tmp/blender-nurbs-occt-trial-build.log`.

The harness was then configured, built and run from the Blender source root:

```sh
cmake -S doc/nurbs_mesh_study/kernel_trial \
  -B /tmp/blender-nurbs-occt-trial/harness \
  -DOCCT_BUILD_DIR=/tmp/blender-nurbs-occt-trial/build
cmake --build /tmp/blender-nurbs-occt-trial/harness --parallel 1
/tmp/blender-nurbs-occt-trial/harness/nurbs_kernel_trial
```

All three commands passed, exit 0. Logs:
`/tmp/blender-nurbs-occt-trial-harness-configure.log`,
`/tmp/blender-nurbs-occt-trial-harness-build.log`,
`/tmp/blender-nurbs-occt-trial-results.log`.

Observed results:

* The arc retained its stored weights and domain; maximum sampled radius error
  was `2.22045e-16` over 129 evaluations. This is a sampled error, not a proof
  of a universal bound.
* Explicit rational curve import retained the tested extreme near-equal weights;
  default import replaced them. Tiny subnormal, negative and zero weights were
  rejected. The surface constructor replaced the tested adjacent-to-one weight
  with a unit weight.
* Coincident independent wires retained four vertices; explicitly sharing one
  endpoint retained three. Input provenance, compaction UID lookup and deep-copy
  isolation passed. This first compaction has no deleted leading node, so it
  does not prove identity through an actual index change.
* Self-copying one of the two incident wire edges created two new vertices in
  this fixture, matching a disconnected Blender duplicate. This is narrower
  evidence than a guarantee for arbitrary shared face closures.

The harness checks stored rational arc weights/domain and sampled radius error;
explicit versus default rational import at extreme weight scales; reported
rejection of unsupported weights; surface import with adjacent-to-one weights;
separate identities for coincident wires;
declared shared vertex identity; input-shape provenance; persistent edge UID
through compaction; and deep-copy isolation. The pinned constructor restrictions
are expected observations, not claims that the full desired source domain is
supported. Compilation alone does not establish these runtime results.

Remaining trial work includes curved shared faces, holes, periodic seams,
non-manifold face incidence, mutation/copy maps, support splitting, numerical
operations, tolerance behavior and actual Blender source round trips. This
first harness does not select a production backend.

## Curved shared-face extension

`shared_boundary_trial.cc` constructs two rational tensor supports (a quarter
cylinder and conical patch) with different knot domains and different U weights.
One canonical 3D arc is shared by both faces. One face uses an affine p-curve;
the other needs a rational projective parameter map. The source supports are
not fitted to each other. Each face has a complete four-edge loop.

```sh
nice -n 19 cmake --build /tmp/blender-nurbs-occt-trial/harness --parallel 1
/tmp/blender-nurbs-occt-trial/harness/nurbs_kernel_trial
```

Build and run **passed**, exit 0. The first compile assumed the 2D spline
constructor had the 3D constructor's `CheckRational` switch; inspection showed
it does not. The call was corrected to the pinned API. Logs:
`/tmp/blender-nurbs-occt-trial-shared-build.log`,
`/tmp/blender-nurbs-occt-trial-shared-build-retry.log`,
`/tmp/blender-nurbs-occt-trial-shared-results.log`.

The graph retained **6 vertices, 7 edges and 8 face uses**, with exactly one
edge incident to both faces. All eight boundary uses matched their canonical
3D curves at 65 parameters each. Maximum measured difference was
`1.39449e-15`, both before and after a graph deep copy. Ingestion returned
status 0 (success). This is sampled correspondence on a known construction,
not a general geometric validity certificate or native Blender round trip.

The builder stored edge tolerance **1e-7** despite the fixture requesting zero.
That is an allowed error metadata value, distinct from the much smaller
measured sample differences. A Blender adapter must explicitly handle units,
working tolerances and source tolerances; silently adopting this default or
writing it back over the native source would violate the intended contract.

## Mutation and loose-vertex ownership

The subsequent identity/root extension also built and ran with exit 0 using
the same two commands above. Logs:
`/tmp/blender-nurbs-occt-trial-identity-build.log`,
`/tmp/blender-nurbs-occt-trial-identity-results.log`,
`/tmp/blender-nurbs-occt-trial-roots-build.log`,
`/tmp/blender-nurbs-occt-trial-roots-results.log`.

After removing the leading edge and compacting, the surviving edge's persistent
UID resolved from transient index **1 to 0**. Unlike the first no-removal smoke
check, this exercises an actual index change. The same compaction dropped the
two formerly incident vertices when nothing explicitly retained them: four
vertices became two, with two unmapped active definitions reported.

Repeating the removal with explicit roots for the vertices retained all four
vertices and their tested UID, with zero unmapped active definitions. A Blender
adapter must represent the intended ownership of loose points deliberately.
Default orphan cleanup must not silently replace a source operation that
preserves those points. This fixture establishes that a suitable root policy
works; it does not establish every remove/split/merge history map.
