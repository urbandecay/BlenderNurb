# Native vertex snapping and extrusion with snapping enabled

The user reported: add a NURBS line, extrude it into a face, duplicate the face,
snap one corner onto another with vertex snapping and an Active snap base, then
try to extrude. The selected face remained flat instead of moving.

## Reproduction and cause

A geometry-only duplicate/translation/extrusion test did not exercise this bug.
Its success was insufficient evidence about the reported Blender interaction.
That test is now named `DuplicateFaceCanTouchAdjacentCornersAndStillExtrude` to
state its actual scope.

`test_nurbs_native.snapping_and_extrusion` reproduces the failure using ordinary
Add, Edit Mode, component selection, Shift+D, the scene snapping settings and E
keypresses. Before the fix, with vertex snapping enabled and the source set to
Active, E followed by Z and 1 left all geometry at Z=0. Face count increased
from 2 to 7. The native extrusion operator had created the initial coincident
cap/walls; the transform adapter then cancelled the move merely because the
snapping toggle was on. Later keys were handled as viewport shortcuts after the
transform had already exited. No corner contact or topology weld was necessary
to trigger the failure.

The before-fix failure log is `/tmp/blender-nurbs-snap-before-fix.log`.
The saved baseline is `/tmp/blender-nurbs-snapping-ui/baseline-after-extrude.blend`;
its screenshot has the same prefix. These are reproduction artifacts, not the
user's original working file, which was not supplied.

## Implemented path

- The snapping toggle alone no longer cancels native translation. Numeric input
  and motion without a target continue through the existing source transform.
- Native vertex targets are topology endpoints queried from the evaluated source
  snapshot. Picking uses float proxies, while the result retains the double
  source position and persistent vertex ID. Native surface tessellation vertices
  are excluded from the polygon picking shortcut and native point queries.
- The snap context retains a double world point obtained from source local
  coordinates and the object's existing matrix. Native endpoints override the
  float local proxy with their authoritative coordinates. Mesh targets use their
  existing float source coordinates, promoted before the world transform.
- Active, Closest, Median and Center snap bases resolve source representatives.
  Active reuses the kernel selection-origin service. Closest maps Blender's
  chosen proxy back to a selected source representative, with a source-distance
  tie break when multiple representatives share a proxy. Center observes the
  pivot, cursor, explicit override and world-axis bounds.
- Axis and plane constraints project a snapped point displacement using Blender's
  constraint matrix, evaluated in double precision.
- The resulting displacement uses the existing `translate_vertices` kernel
  service and common edit commit. Incident curves/supports and selection keep
  the existing ownership/transaction rules. Snapping does not allocate or weld
  topology. Coordinates that coincide can still have separate vertex IDs.

This adds editor orchestration and source picking to the existing kernel transform
architecture. It does not introduce another extrusion implementation or write
an evaluated display mesh back into the source.

## Scope and limits

This checkpoint supports point/vertex snapping for native G and the Move portion
of Shift+D/E, with one native edit owner. It also handles an external mesh vertex
as the target. Native endpoints in Object Mode use the same source query.

General native edge/face nearest-point queries, surface projection, alignment to
normals, accumulated snap points, a custom modal snap base, geometric Rotate/Scale
snapping and multiple moving edit owners remain unsupported. Unsupported target
application reports an error; an enabled toggle alone is not a geometry error.
Native edge/face target queries do not substitute a tessellated native surface.
Visibility storage and a snap acceleration cache remain future work.

The coordinate calculations avoid a float target/source subtraction for native
vertices. Object matrices and picking still use Blender's existing float host
boundary. Kernel coefficient rounding is reported by the existing transform
adapter; that bound is not a certificate of complete world-space snap error.
No fitting, knot/weight changes, tolerance increases or automatic merges occur.

Native curve E still starts as free translation; an automatic face-normal
constraint remains a separate source-frame task. Mesh E follows its face normal.
The mesh reference in the test uses the sign of its wire-built face's normal;
the NURBS test uses an explicit global Z constraint for the same final movement.

## Validation

The before-fix viewport regression failed at Z=0 versus the requested Z=1. With
the fix, the same test passes. `vertex_snap_then_extrude` uses actual viewport
picking, Shift+D, G corner snapping, face selection and E for both Mesh and NURBS.
It checks source active centers and topology counts, snap and extrusion undo/redo,
and snap cancellation. Its screenshots are inspected in an oblique solid view.

`native_vertex_snap_sources` covers Active/Closest/Median/Center, the cursor pivot,
axis and plane constraints, a rotated/scaled edit owner, transformed Closest,
an external transformed mesh target and source precision. The precision case
places the original at Z=2^-35 and moves its duplicate by 1+2^-40 before snapping.
A float-only displacement leaves a detectable error; the source snap reaches
Z=2^-35. `native_single_vertex_snap` moves one corner of a copied surface and
checks that incident geometry still evaluates. `snapped_face_fresh_continuation`
loads the saved native source into a fresh process, cancels one extrusion's
movement, then extrudes again with snapping enabled. Source remains native and
the legacy splines collection remains empty.

Exact build commands used:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender --parallel 4
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender blenkernel_nurbs_geometry_test --parallel 4
/home/molotovgirl/Desktop/build_linux/bin/tests/blenkernel_nurbs_geometry_test --test-assets-dir='/home/molotovgirl/Desktop/Blender Fork/tests/files'
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

Each viewport function uses this command, substituting its full test ID for
`TEST_ID` (each function runs in a separate Blender/display process):

```sh
nice -n 10 env -u DISPLAY \
  BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
  WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
  WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
  WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
  python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
  --python tests/python/ui_simulate/run_blender_setup.py -- --tests TEST_ID
```

Test IDs, in dependency order for the saved continuation:

```text
test_nurbs_native.snapping_and_extrusion
test_nurbs_native.vertex_snap_then_extrude
test_nurbs_native.native_vertex_snap_sources
test_nurbs_native.native_single_vertex_snap
test_nurbs_native.snapped_face_fresh_continuation
test_nurbs_native.rotate_scale_and_lifecycle
```

Build and test results and final log paths are recorded in the latest entry of
[current checkout audit](current_checkout_audit.md). No user Blender instance is
closed or replaced by the isolated viewport tests.
