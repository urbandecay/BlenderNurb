# Native bevel: collision limits and rotated rounded corners

This records the earlier checkpoint. The [corner-blend follow-up](native_bevel_corner_blends.md)
supersedes its nonorthogonal three-edge restriction and adds private planar quad charts for
continued editing. Its generic corner blends certify G0, not general G1 fillet continuity.

This follow-up keeps Blender's existing native NURBS data owner, topology,
operators, transforms, evaluation and drawing. BMesh remains the interaction
and topology reference. SMLib's edge/corner construction stages were studied as
an algorithm reference; no external modeling kernel or SMLib code was added.

## Implemented behavior

Ctrl+B starts with **Clamp Overlap enabled**, as requested by the user. C toggles
it during the operation; the same option is exposed in the operator properties
and through `bpy.ops.curve.bevel(..., clamp_overlap=True)`.

Previously, a width beyond a neighboring boundary failed and held whichever
preview had last succeeded. Now the BKE service plans the corner motions per
unit offset, balances connected widths, and determines a limit from the planned
rails and retained boundaries before constructing surfaces. It scales the
whole selected network together. Oversized requests produce the same limiting
source regardless of the previous mouse positions or the requested magnitude.
The status displays the applied width and requested width when limited.
Confirmation accepts that applied geometry. Python execution reports the limit.

The local collision calculation follows BMesh's `geometry_collide_offset` /
`bevel_limit_offset` principle: find the first collapse or slide event and reduce
all related offsets together. It operates directly on source topology and
bounded native geometry arithmetic. It never constructs a polygon edit source.
`BevelOffsetInfo` returns the actual width and computed limit on success.

The limit keeps a positive clearance for coordinate and construction arithmetic.
At the exact event BMesh can retain coincident vertices and degenerate faces;
native regular supports stop just before that event. Very narrow retained faces
can therefore remain. Exact event cleanup and general intersection handling
are not claimed. With the clamp disabled, unsupported overlaps still reject
atomically and the editor labels any held previous preview.

All 4,095 nonempty cube-edge subsets were checked at both the ordinary chamfer
width and an oversized clamped width. The ordinary cases still compare with
actual BMesh topology/coordinates; the limiting fixtures compare BMesh's
clamped locations and maintain native shared incidence and orientation.

## Rotated spherical corners

A new regression reproduced the failure on a rotated box with every edge
selected: `Rounded three-edge NURBS corners currently require orthogonal source
directions`. The old test required a dot product and its arithmetic error to
both be exactly zero. A normal floating-point rotation does not preserve that
representation-level condition.

Eligibility now tests right angles against the recorded source-vertex and
arithmetic enclosure. A real shear is still rejected even with a generous
caller error budget. The new reusable `rational_spherical_octant_surface`
constructs the rational support without altering its supplied axes and bounds
the whole surface's radial deviation. For axes matrix A and radius r,
`||A^T A - r^2 I||_infinity / r` bounds the radial difference over the reference
octant. This is added to coefficient/weight construction roundoff and must fit
the explicit budget. It is not a sample-based acceptance or a hidden fit.

The regression checks the rotated output against the independently evaluated
rounded-box distance field at ordinary and limiting widths. It also checks
display normals, source preservation on rejected shear, invalid radii, and
reported deviation for a deliberately distorted octant.

At this checkpoint, general curved support fillets, real nonorthogonal or higher-valence rounded
junctions, nonplanar caps with more than four sides, vertex bevel, custom
profiles and other width types remain outside the implemented geometry scope.
SMLib's general surface intersections and trim insertion still require native
services before those cases can be supported.

## Builds and automated checks

Commands executed from the Blender source root; all final commands below
returned exit 0:

```sh
cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_topology_edit_test blenkernel_nurbs_transform_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_topology_edit_test --parallel 4
cmake --build /home/molotovgirl/Desktop/build_linux --target blenkernel_nurbs_tessellate_test --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Logs: `/tmp/nurbs-bevel-corners-build.log`,
`/tmp/nurbs-bevel-final-tests-build.log`,
`/tmp/nurbs-bevel-tessellation-build.log`,
`/tmp/nurbs-bevel-corners-install.log`.

```sh
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_transform_test --gtest_color=no
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_tessellate_test --gtest_color=no
env BLENDER_NURBS_TOPOLOGY_OUTPUT=/tmp/blender-nurbs-topology-ui BLENDER_NURBS_TOPOLOGY_UI_READY=1 BLENDER_NURBS_MERGE_REFERENCE_READY=1 BLENDER_NURBS_MERGE_UI_READY=1 BLENDER_NURBS_CUT_UI_READY=1 BLENDER_NURBS_APEX_OUTPUT=/tmp/blender-nurbs-apex-ui BLENDER_NURBS_LAYERED_OUTPUT=/tmp/blender-nurbs-layered-ui BLENDER_NURBS_COMPOSITION_OUTPUT=/tmp/blender-nurbs-composition BLENDER_NURBS_TRIANGLE_OUTPUT=/tmp/blender-nurbs-triangle-ui BLENDER_NURBS_BEVEL_OUTPUT=/tmp/blender-nurbs-bevel-ui BLENDER_NURBS_BEVEL_UI_READY=1 BLENDER_NURBS_BEVEL_MOUSE_UI_READY=1 BLENDER_NURBS_BEVEL_CLAMP_UI_READY=1 /home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_topology_edit_test --gtest_color=no
ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(nurbs_composition_fixture|script_nurbs_composition|nurbs_composition_saved_sources|script_nurbs_line|script_nurbs_legacy_compat)$'
```

Results: **30 transform + 12 tessellation + 54 topology = 96 passed, no skips**;
**5/5 CTest cases passed**. Logs:
`/tmp/nurbs-bevel-final-{transform,tessellation,topology}-tests.log` and
`/tmp/nurbs-bevel-final-ctest.log`.

## Viewport checks

Each function below ran in its own factory-startup Blender process using:

```sh
env -u DISPLAY BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup --python tests/python/ui_simulate/run_blender_setup.py -- --tests test_nurbs_native.bevel_clamped_mouse_workflow
```

Substitute the function after `test_nurbs_native.`. **20/20 returned exit 0**:

- New cases: `bevel_clamped_mouse_workflow`,
  `bevel_clamped_round_mouse_workflow`, `bevel_clamped_mesh_mouse_workflow`,
  `bevel_rotated_round_box_workflow`, `bevel_rotated_clamped_round_box_workflow`.
- Existing cases: `bevel_corner_workflow`, `bevel_box_workflow`,
  `bevel_round_workflow`, `bevel_round_box_workflow`, `bevel_vertex_workflow`,
  `bevel_mesh_workflow`, `bevel_connected_edges_workflow`,
  `bevel_connected_round_workflow`, `bevel_pyramid_workflow`,
  `bevel_mesh_connected_edges_workflow`, `bevel_mesh_pyramid_workflow`,
  `bevel_mouse_limits_workflow`, `bevel_round_mouse_limits_workflow`,
  `bevel_rotated_mouse_sweep_workflow`, `bevel_split_vertex_deformation_workflow`.

The clamp cases select an edge and invoke Ctrl+B, alternate ordinary and
oversized mouse positions, measure the visible width, toggle C, cancel,
confirm the limiting width, undo/redo, reenter Edit Mode and continue extruding.
The native and actual mesh flat-corner cases finish with the same logical
12 vertices / 18 edges / 7 faces after continued extrusion. The rounded native
case retains its rational source. Saved-source replay requires exact geometry,
identity-counter, selection/history/mode equality across cancel, confirm and
reentry, and matches direct BKE replay at an oversized width.

New logs: `/tmp/nurbs-bevel-clamped-{mouse,round-mouse,mesh-mouse}.log`,
`/tmp/nurbs-bevel-rotated-{round-box,clamped-round-box}.log`.
Existing-case logs: `/tmp/nurbs-bevel-validation-FUNCTION.log`, except the flat
unclamped mouse-limit case at `/tmp/nurbs-bevel-unclamped-mouse.log`.

The following screenshots were visually inspected:

- `/tmp/blender-nurbs-bevel-ui/bevel-native-corner-mouse-clamped-clamped.png`
- `/tmp/blender-nurbs-bevel-ui/bevel-native-corner-round-mouse-clamped-clamped.png`
- `/tmp/blender-nurbs-bevel-ui/bevel-native-connected-round-rotated-finished.png`
- `/tmp/blender-nurbs-bevel-ui/bevel-native-connected-round-rotated-clamped-finished.png`

This confirms those generated viewport cases, not arbitrary user scenes.

## Python operator check

The background operator check also returned exit 0:

```sh
/home/molotovgirl/Desktop/build_linux/bin/blender --background --factory-startup /tmp/blender-nurbs-bevel-ui/bevel-native-corner-before.blend --python-exit-code 1 --python-expr 'import bpy,math; obj=bpy.context.object; assert obj.mode=="EDIT"; assert bpy.ops.curve.bevel.get_rna_type().properties["clamp_overlap"].default; assert bpy.ops.curve.bevel(offset=100,segments=3,clamp_overlap=True)=={"FINISHED"}; assert obj.data.is_native_nurbs and len(obj.data.splines)==0; ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get()); mesh=ev.to_mesh(); assert mesh is not None and len(mesh.polygons)>0; deviation=max(abs(math.hypot(v.co.y,v.co.z-2)-2) for v in mesh.vertices); assert deviation<1e-6,deviation; print("NURBS_BEVEL_PYTHON_API_PASS",len(mesh.polygons),deviation); ev.to_mesh_clear()'
```

Log: `/tmp/nurbs-bevel-final-python-api.log`. The mesh is used only to measure
derived output; the edited source remains NURBS.

The Python check reported applied width `1.9999999999999962`, 392 derived mesh
polygons, and maximum sampled radial display error `6.647113703905916e-08`.
The installed binary reports build `2026-10-26 14:15:18`.

Clang-format 20.1.8 `--dry-run --Werror` passed on the six affected C++ files.
`python3 -m py_compile tests/python/ui_simulate/test_nurbs_native.py` and
`git diff --check` also passed.

Binary: `/home/molotovgirl/Desktop/build_linux/bin/blender`. Restart an existing
instance to load the rebuilt code. Work remains local, uncommitted and unpushed.
The user-owned handoff was preserved byte for byte.
