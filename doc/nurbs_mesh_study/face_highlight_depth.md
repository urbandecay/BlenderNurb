# Selected-face depth correction

The user's screenshots showed an even mesh face highlight and a speckled NURBS
highlight. The previous small-patch color tests did not adequately check selected
face coverage. Reusing mesh shaders was insufficient because the compatibility
surface and its source overlay used different tessellations with no cage bias.

## Mesh reference and correction

`overlay_mesh.hh` already has two face paths: normal faces use zero NDC offset;
an edit cage distinct from the evaluated surface uses 0.5 times the viewport's
polygon-offset factor. Edges use 1.0; vertices and face dots use 1.5. These are
screen-space offsets scaled by Blender's existing zoom-dependent factor.

Those rules are now shared in `EditMeshDepthOffset` in `overlay_private.hh`.
Both mesh and NURBS use that definition. Mesh pass behavior is unchanged.
The exact tagged compatibility overlay is a separate cage over the older body
tessellation, so it uses the mesh cage rule. Native body and overlay share their
source preview tessellation and continue using the normal mesh face rule.
Shaders, theme flags and depth-tested framebuffers remain the mesh ones used by
the preceding overlay implementation. This changes drawing, not source geometry,
topology, source tolerances or selection.

## Reproduction and tests

`test_nurbs_native.selected_face_depth_workflow` creates actual mesh reference
geometry, then repeats Add line → E to face → A → E to box for native and
untouched tagged sources. It selects the top face through viewport events and
asserts the actual selection mode. For each view it captures the same selected
geometry with face tint disabled and enabled. It checks every pixel inside an
inset projected face polygon against the actual mesh's highlight delta, rather
than comparing a few average color patches. Face-dot pixels with no expected
tint signal are excluded. At least 99.5% of tested pixels must receive at least
80% of the mesh tint signal. The final run achieved 100% in every case.

Before the correction, the fresh tagged perspective case had **1,065 missing
highlight pixels out of 5,934**, coverage **82.0526%**. The native path passed its
six initial views. Logs:
`/tmp/blender-nurbs-face-depth-before.log` and
`/tmp/blender-nurbs-face-depth-native-before.log`, both exit 1. The saved
reproduction capture is
`/tmp/blender-nurbs-overlay-ui/face-depth-before-tagged.png`.

The final matrix covers three angles, two zoom levels and both perspective and
orthographic projection: 12 views each for native vertex mode, native face mode
and untouched tagged vertex mode, using the corresponding real mesh references.
The tagged source deliberately stays in its original editing storage. Switching
it to face mode enters native editing, covered by the native face cases.

An initial expanded test put the closest low-angle camera below the top face;
the mesh positive control rejected that invalid view with only 20 tint pixels.
The final test aims at the top face, keeping every camera above it. Pixel
thresholds were not relaxed. That test setup failure is recorded in
`/tmp/blender-nurbs-face-depth-after.log`, exit 1.

## Exact commands and results

Commands are run from the Blender checkout:

```sh
nice -n 10 cmake --build /home/molotovgirl/Desktop/build_linux --target blender --parallel 4
cmake --install /home/molotovgirl/Desktop/build_linux --prefix /home/molotovgirl/Desktop/build_linux/bin
```

Both pass, exit 0:
`/tmp/blender-nurbs-face-depth-build.log` and
`/tmp/blender-nurbs-face-depth-install.log`.

Each viewport test below is run in a separate process:

```sh
for TEST_ID in \
  test_nurbs_native.selected_face_depth_workflow \
  test_nurbs_native.mesh_overlay_parity_workflow \
  test_nurbs_native.mesh_overlay_custom_theme_workflow \
  test_nurbs_native.mesh_overlay_tagged_workflow
do
  nice -n 10 env -u DISPLAY \
    BLENDER_BIN=/home/molotovgirl/Desktop/build_linux/bin/blender \
    WESTON_BIN=/tmp/blender-nurbs-deps-storage/wayland_weston/bin/weston \
    WAYLAND_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland \
    WESTON_ROOT_DIR=/tmp/blender-nurbs-deps-storage/wayland_weston \
    BLENDER_NURBS_OVERLAY_OUTPUT=/tmp/blender-nurbs-overlay-ui \
    python3 tests/utils/blender_headless.py --enable-event-simulate --factory-startup \
    --python tests/python/ui_simulate/run_blender_setup.py -- --tests "$TEST_ID"
done
nice -n 10 ctest --test-dir /home/molotovgirl/Desktop/build_linux --output-on-failure \
  -R '^(script_nurbs_(line|legacy_compat)|script_pyapi_bpy_utils_units|blendfile_autosave|global_undo)$'
```

All four viewport workflows pass, exit 0. The new coverage test passes **36/36
selected-face checks with zero missing tested pixels**, logged in
`/tmp/blender-nurbs-face-depth-final.log`. The existing default/custom theme
V/E/F solid/X-Ray and tagged solid occlusion comparisons also pass; their logs
are `/tmp/blender-nurbs-face-depth-TEST_FUNCTION.log`. The selected compatibility,
API utilities, autosave and undo tests pass **5/5**, in
`/tmp/blender-nurbs-face-depth-compat.log`. Python compilation, clang-format
verification and `git diff --check` pass.

Visually inspected: the failing tagged capture, fixed close perspective tagged
capture and its corresponding actual mesh reference. The fixed surface has a
continuous selected-face tint. Example matching captures:

```text
/tmp/blender-nurbs-overlay-ui/face-depth-TAGGED_VERT-1-PERSP-True.png
/tmp/blender-nurbs-overlay-ui/face-depth-MESH_VERT-1-PERSP-True.png
```

The user instance was not closed, modified or used to claim interactive
confirmation. The rebuilt binary is
`/home/molotovgirl/Desktop/build_linux/bin/blender`. Changes remain local and
unpushed. General compatibility body X-Ray opacity and other limitations in
[the preceding record](merge_overlay_fixes.md) remain separate from this fix.
