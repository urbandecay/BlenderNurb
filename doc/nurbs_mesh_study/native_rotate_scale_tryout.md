# Native Rotate and Scale checkpoint

Native NURBS now uses Blender's Rotate and Scale operators in Vertex, Edge and
Face modes. The same adapter also handles switching between G/R/S during a modal
operation. Endpoint selection updates shared incident geometry through kernel
services. Whole curves and surface supports retain their rational basis; supported
partial linear corners and ruled boundary rows use proven source correspondence.

Launch `/home/molotovgirl/Desktop/build_linux/bin/blender`, then **Add → Curve →
Nurbs Line**, **Tab**, **2**, **E, Y, 2, Enter**. Press **A** to select the whole
sheet. Try **R, Z, 90, Enter**, then **S, X, 2, Enter**. Choose **1 / 2 / 3** to
select endpoints, whole curves or faces. The right-click menu lists Move, Rotate
and Scale. Standard F9 fields, axis constraints, mouse input, Escape, undo and
redo use Blender's transform flow. Source-kind entry is described in
[the ordinary workflow checkpoint](component_mode_entry.md).

## Shared geometry and pivots

`transform_vertices` accepts an affine matrix for each source vertex identity.
Equal matrices form one group. All requests are prepared against the original
source before a complete candidate is returned. The shared correspondence planner
is also used by `translate_vertices`; Rotate/Scale do not have separate edge or
surface reconstruction rules. Geometry-resource owners must agree on coefficient
requests. Unsupported partial curved or trimmed edits reject explicitly.

Whole rational edges/supports receive affine transforms of their 3D coefficients.
Partial linear edits move exact endpoints/corners. Supported ruled edits transform
complete boundary rows while retaining rational parameter correspondence, including
the existing restricted interior-trim cases. Weights, knots, degrees, domains,
p-curves, topology identities, incidence and untouched resources remain intact.
No display triangles are used to reconstruct source data, and no fitting occurs.

The pivot service uses the current selection and actual topology:

- Median uses the selected endpoints. Active Element uses the existing source
  vertex/edge/face representative, with median fallback.
- Individual Origins follows BMesh precedence: selected edges connected by
  vertices in Vertex/Edge modes; selected faces connected by vertices in
  exclusive Face mode. Group centers average edge/face representatives. Loose
  selected vertices use their own origins. All seven combined modes are compared
  against actual BMesh grouping and center queries.
- Bounding Box uses Blender's bounds measured in world axes, including for a
  rotated owner. Cursor and explicit center overrides use Blender's supplied
  world center. These centers are converted to source space through the owner.

NURBS edge representatives are curve interval midpoints; face representatives
average unique boundary vertices. These are the existing NURBS-specific center
rules. Curved or holed source centers are not claims about a mesh polygon's area
centroid, a certified point on a surface, or a fit handle.

Every preview starts from the immutable original source, so mouse updates do not
accumulate coefficient rounding. Cancel restores that source and selection.
Switching transform modes restores the source before applying the next mode.
Saving and undo retain the actual source rather than float transform proxies.

## Precision and limits

Source coefficients and vertices are transformed as doubles. The kernel reports
an application roundoff bound and conservatively scales affected source
tolerances; no tolerance is widened to hide a failed fit. The supplied matrices
must be finite and affine. Shared-owner conflicts and exceeded arithmetic budgets
fail before mutation. Negative scales keep source orientation flags and loop
traversal; source derivatives determine the reflected geometric normals. Zero
scale remains a legal singular editing state.

Rotate/Scale currently use Blender's standard float operator angle/scale fields,
orientation matrices and mouse calculations. Cursor, world bounding-box and
explicit override centers are existing float UI inputs. Object matrix inversion
and composition happen in doubles, while the object matrix itself comes from
Blender's float owner system. The reported coefficient-application bound excludes
these input/construction boundaries and NURBS evaluation. Move retains its existing
precise decimal fields and double numeric path. R/S do not yet provide an
additional decimal parameter interface.

This checkpoint requires a single edit data owner. Normal/Gimbal source frames,
snapping, proportional deformation, Trackball, object Apply Transforms/Set Origin,
general partial curved/trimmed deformation and full component-data Python bindings
remain gated. General curve
fit-point editing and ordinary native curved-primitive creation/import are separate
stages. Request planning scans the source per distinct matrix group; very large
Individual Origins selections still need performance work. Structural validity
and a working preview do not certify tangent continuity, self-intersection freedom
or a closed solid.

## Verification artifacts

`/tmp/blender-nurbs-rotate-scale-ui/final.blend` contains the endpoint/edge-edited
surface; `fresh-extruded.blend` and `fresh-scaled.blend` continue it in another
process. The directory also contains the intermediate source states, the F9 screenshot, transformed face/edge
screenshots and the native context menu screenshot.

`/tmp/blender-nurbs-rotate-scale-parity-ui/` contains six NURBS results compared
against actual mesh operations with rotated, translated and nonuniformly scaled
owners. Cases cover Individual Origins in all three modes, plus bounding box,
active and cursor pivots, global rotation/scale and a Local axis constraint.
`mesh-reference.json` and `mesh-reference.txt` retain the actual mesh coordinates.
These are test comparisons, not a source import path.

The C++ saved-file regression compares complete authoritative records for the
lifecycle states against kernel results and compares parity vertices/coefficients
against the independently edited mesh within its float boundary. Exact commands,
final counts and results are recorded in [the checkout audit](current_checkout_audit.md).
Stage 1 and the full editing pipeline remain incomplete.


The checkpoint builds and passes **246 C++ checks without skips**, **25 viewport
cases** and **five CTest targets**, including **39 NURBS Python cases**. Twenty
saved lifecycle/continuation states match complete expected NURBS records; six
more match actual mesh edits within the float comparison boundary. The transformed
face/edge, F9, Individual Origins and context-menu screenshots were visually
inspected. Standard `bpy.ops.transform.rotate()` and `bpy.ops.transform.resize()`
reach this same native adapter in Edit Mode; full component-data bindings remain
later work.
