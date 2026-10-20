# Adjacent Blender capabilities for NURBS modeling

This inventory broadens the mesh-editing comparison to the surrounding Blender
modeling stack in the checked-out Blender Fork. It is a source audit, not a
promise that every item is enabled in a particular build, mode, configuration,
or runtime. Operator registration, implementation files, type registries, and
UI definitions establish source presence only. Availability can still depend
on build flags, object type, mode, context, feature settings, and linked data.

The intended design space is native, editable NURBS geometry, including
degree-one polygon-equivalent curves and surfaces. A control net is not itself
a boundary representation: CVs, U/V grid adjacency, weights, knots, and trims
do not directly provide arbitrary mesh vertex/edge/face topology. Conversely,
the presence of a NURBS control net is not a reason to forbid topological
editing. Full BRep topology is a valid route: a CAD object may have vertices,
edges, faces, loops, shells, and persistent adjacency in addition to its exact
NURBS geometry. For degree-one geometry, mesh-like operations can often act on
the native control net/topology. For higher degree, each operation must state
whether it is an exact NURBS/BRep operation, a topology-changing operation with
valid exact representations, an approximate operation, or a conversion. Do
not silently replace the authoritative NURBS with a tessellation or a refit.

The prototype's current openNURBS integration supplies NURBS representation
and geometry operations; openNURBS should not be treated as a general CAD
modeling kernel with arbitrary BRep construction, topology healing, booleans,
constraints, and history. A full BRep route may require a separate modeling
kernel or substantial native topology/algorithm work. Kernel choice is an
architecture decision, not a mandate to add a dependency: document which exact
topology operations are implemented by classiCAD, the chosen kernel, or a
restricted operation-specific implementation.

Blender mesh editing intentionally permits intermediate open, wire, boundary,
non-manifold, and self-intersecting states. A closed-solid-only CAD validity
policy would reject many ordinary mesh editing steps (single-face extrude,
open shell, disconnected piece, temporary self-intersection). Preserve mesh UX
semantics where requested by representing these states as valid editable
topology, while separating “editable topology” from “closed/manifold solid”
validation and downstream manufacturing/export checks. Make command-specific
validity rules explicit; do not globally force every edit result to be a closed
solid.

## Reuse boundary

Blender's window-manager operators, modal event loop, keymaps, undo/redo and
redo-last properties, viewport navigation, gizmos, generic G/R/S transforms,
numeric constraints, transform orientations, pivots, cursor, object selection,
collections, visibility, Outliner, animation evaluation, and much of the
modifier stack UI are host-level concepts. They are useful interaction
references, but each needs a NURBS/BRep data adapter and explicit semantics.
For example, generic G/R/S can drive selected CAD components if the adapter
provides world-space positions and commit/undo behavior. Blender's transform
implementation uses float-oriented math structures in many paths; a CAD adapter
must preserve the model's double precision and avoid round-tripping stored
double CV coordinates through float display/evaluation data. Keep authoritative
coordinates, knots, and weights in the CAD model's precision.

Mesh Edit Mode remains topology-specific: vertex/edge/face selection,
manifold/pole tests, face sides, loop/ring traversal, bevel, inset, bridge,
knife, dissolve, remesh, and mesh attribute domains assume mesh topology.
Those operations can inspire analogous NURBS/BRep operations, but are not
generic host features. Blender already has a separate curve-edit family; consult
it for CV selection, curve rows, weights, radius, tilt, and spline operations
before treating mesh behavior as the only reference (`source/blender/editors/curve/curve_ops.cc`,
`editcurve_select.cc`, `editcurve.cc`).

## Core Edit Mode and topology semantics

The mesh operator roster is registered in
`source/blender/editors/mesh/mesh_ops.cc`. It includes selection, hide/reveal,
extrude, duplicate, split, delete, dissolve, bevel, inset, bridge, loop cut,
knife, intersect, fill, normals, attributes, UVs, and topology tools. This
roster is evidence of source registration, not runtime availability. The
relevant operation families and NURBS/BRep treatment are:

| Mesh capability family | NURBS/BRep design consideration |
| --- | --- |
| Vertex/edge/face modes; box, circle, lasso, linked, shortest path, loop/ring, similar, random, by-attribute, more/less | Provide first-class BRep vertex/edge/face selection if full topology is modeled. CV selection remains a separate mode/domain. Degree-one tensor-product surfaces can expose U/V control-net adjacency; higher-degree surfaces need explicit control-net versus evaluated-topology mode. A CV is not generally a point on its represented curve/surface. |
| Extrude, inset, bevel, bridge, fill, knife, split, dissolve, merge, collapse, face creation | Often valid exact operations on BRep topology, with trims and support surfaces preserved or constructed as needed. A surface control net alone may not contain enough topology to define these. Degree-one geometry can map to simpler exact cases. Specify topology and continuity outcomes; do not assume tessellation is the only implementation. |
| Loop cut, edge ring, shortest path, edge slide, vertex slide, shrink/fatten | Require BRep adjacency and a defined metric/constraint. For control nets, define U/V row traversal and cyclic seams. For curved surfaces, specify whether the operation follows parameter lines, evaluated surface distance, or BRep edges. |
| Normals, sharpness, smooth/flat shading, normal editing | Surface derivatives and BRep face orientation provide geometric normals. Rendering crease/shading flags are distinct from changing degree, knot multiplicity, or geometric continuity. Preserve trim-loop orientation and face normal conventions. |
| Hide/reveal selected elements | Host object/collection visibility is separate from temporary per-component visibility in edit state. Define whether hidden topology is serialized, participates in selection/snapping, and affects dependent faces. |
| Selection history, active element, multi-object edit | Keep active object and active topological/CV component distinct; define selection history for pivot and “last selected” behavior. Multi-object edit must tolerate different degrees, knot vectors, frames, topology, and component counts. |

Degree-one does not by itself make every NURBS surface equivalent to a mesh.
An arbitrary four-corner degree-one patch is bilinear: its interior generally
differs from either triangulation of a non-planar quad in BMesh. A BMesh quad
also has explicit edge/face topology and a triangulation policy for display or
downstream operations. If a CAD BRep stores exact bilinear faces, preserve that
surface and define how mesh-style algorithms interpret it; do not silently
replace it with two planar triangles. Likewise, trims and shared-edge
relationships are topology beyond the tensor-product control net.

The associated mesh registration includes e.g. `MESH_OT_select_all`, linked,
random, hide/reveal, by-face-sides, by-pole-count, loose, mirror, shortest path,
select-by-attribute, loop/ring, select-similar, select-mode, and topology
operators (`mesh_ops.cc:26-40, 67-80, 87-123, 129-199`). Mesh macros combine
domain operators with generic transforms (`mesh_ops.cc:221-356`). The curve
family separately registers CV selection operations, including select all,
linked, row, next/previous, more/less, random, nth, similar, and shortest path
(`curve_ops.cc:96-110`).

## Object-level modeling and modifier roster

Object operators are separate from Edit Mode. This fork's
`source/blender/editors/object/object_ops.cc:29-71, 99-124, 138-200, 231-243`
registers object transforms and selection, duplicate/join/convert, modifier
stack operations (add/remove/reorder/apply/copy), constraint operations,
vertex groups, shape keys, data transfer, visibility, and voxel/Quadriflow
remesh commands. This is a mixed object/data editing surface, not a claim that
all operations apply to NURBS.

The actual modifier type registry is
`source/blender/modifiers/intern/MOD_util.cc:159-220`; the declared type list is
`source/blender/modifiers/MOD_modifiertypes.hh:17-69`. Below is the roster
installed by `modifier_type_init` in this fork, grouped by likely NURBS/BRep
policy. Grouping is an applicability assessment, not Blender's built-in
compatibility flag and not a runtime-enabled claim.

| Modifier policy category | Registered types in this fork | NURBS/BRep outlook |
| --- | --- | --- |
| Exact affine or parametric candidates | Curve, Lattice, Hook, Mirror, Array, Screw, Warp, SimpleDeform, Build | Often definable on CVs or exact geometry for affine operations, but apply after mapping through local/world frames. Array and screw may create separate exact components or require exact sweep constructions. Build can be component visibility/time evaluation. Preserve knots/weights and component identity. |
| Surface/solid construction or topology-changing | Subsurf, Bevel, Boolean, Solidify, Wireframe, Weld, Skin, Decimate, Triangulate, EdgeSplit, Remesh, Multires, MeshToVolume, VolumeToMesh | Some have exact BRep analogues; others are fundamentally tessellation/topology workflows. Be explicit about exact support surfaces, trimming, result topology, tolerance, and whether output remains NURBS/BRep or becomes a mesh/volume object. Boolean is a BRep capability, not inherently forbidden. |
| Deformation / smoothing / projection | Displace, Smooth, Cast, Wave, Shrinkwrap, MeshDeform, LaplacianSmooth, LaplacianDeform, CorrectiveSmooth, SurfaceDeform, NormalEdit, WeightedNormal | Can evaluate on exact surface samples, deform CVs, or create an approximated/refit result; these are different products. Prefer non-destructive evaluation where supported and state continuity/tolerance behavior. Normal-only modifiers affect display or downstream consumers, not necessarily the exact surface. |
| UV / attribute / weight transfer | UVProject, UVWarp, DataTransfer, WeightVGEdit, WeightVGMix, WeightVGProximity, Mask | Define domains explicitly (CV, BRep vertex/edge/face/loop, parameter-space sample, or evaluated point). NURBS UV parameters are not automatically mesh texture coordinates. |
| Simulation, cache, or generated geometry | Softbody, Cloth, Collision, ParticleSystem, ParticleInstance, Explode, Fluid, DynamicPaint, Ocean, MeshCache, MeshSequenceCache, Nodes, VolumeDisplace | Host evaluation can be reused as a stack concept; geometry support and data contracts vary. Keep evaluated output distinct from authoritative editable NURBS/BRep unless explicit apply/bake/conversion occurs. Geometry Nodes is an extensible system, not a single geometry algorithm. |
| Navigation/legacy/specialized | Surface, ShapeKey, None | Special-purpose or compatibility entries; inspect the type implementation and supported object/geometry contract before making a CAD analogue. Shape keys are discussed below. |

The registry has 58 entries including `None` (57 non-None types); this list is
source-level inventory only. `NavMesh` and `Fluidsim` appear in
`MOD_modifiertypes.hh` declarations but are not installed by the `INIT_TYPE`
calls in `MOD_util.cc`, so they are not in the registered roster above.
Conversely, `MeshDeform` is installed by `INIT_TYPE` and appears in the
deformation category. Some registered types may be legacy, conditionally
compiled elsewhere, unsupported by a given geometry type, or unavailable in a
particular runtime. The fork's removed armature-parenting feature relative to
upstream is out of scope and should not be restored as a prerequisite for CAD
modifier design. More generally, do not infer support from upstream Blender
documentation when auditing this fork.

### Per-type integration matrix

The table below accounts for all 58 installed registry entries, including the
`None` sentinel. “Path” describes a recommended CAD integration contract, not
the Blender modifier's implementation or the current classiCAD capability:

- **N**: exact NURBS/BRep-native evaluation or an exact topology-preserving
  construction is plausible; keep exact source data authoritative.
- **E**: evaluate over a derived display/evaluation mesh; do not write back to
  NURBS unless separately applied through an explicit conversion/refit.
- **C**: topology/representation conversion is inherent or likely; make the
  conversion explicit and retain source/undo history.
- **U**: no meaningful general NURBS analogue without a new domain contract.

| Registered type | Path | NURBS/BRep integration and topology lifecycle |
| --- | --- | --- |
| None | U | Sentinel only; no effect. |
| Subsurf | E/C | Blender subdivision changes mesh topology. Could later offer exact knot insertion/refinement or a separately named NURBS refinement where mathematically exact; do not claim Catmull–Clark output is the same operation. Applied result needs explicit mesh conversion or a distinct exact surface result. |
| Lattice | E | Deform evaluated samples by lattice field. Native CV deformation is only an approximation to deforming the entire surface and can alter shape between CVs; label any CV mode as such. Preserve source topology. |
| Curve | E | Blender curve-deform uses a guide curve and mesh coordinates. Evaluate NURBS samples through the same field or implement a NURBS-specific deformation; preserve source and frame. Exactness generally unavailable for arbitrary bend. |
| Build | N/E | Animate visibility/order of BRep components or evaluated mesh elements. For NURBS, define whether spans, CVs, components, trim loops, or tessellation samples reveal over time. Do not delete knots/CVs when merely animating reveal. |
| Mirror | N | Affine reflection maps Euclidean CVs and BRep vertices exactly; preserve positive weights and knot data. Welding/seam topology is a separate BRep operation with tolerance and continuity policy. |
| Decimate | E/C | Mesh simplification is evaluated-mesh-only; applying produces mesh. A NURBS simplifier/refitter would be a separate operation with tolerance, topology, trim, and error bounds. |
| Wave | E | Deform evaluated samples by time/position; retain source NURBS. CV displacement is not generally the same as continuous wave deformation. |
| Hook | E/N | Hook targets can transform chosen BRep/CV subsets. For CV hooks, bind stable IDs/weights and remap under topology edits; arbitrary surface deformation may need derived-mesh evaluation. |
| Softbody | E/C | Simulation acts on discretized state. Keep simulation cache/evaluated mesh separate; baking to NURBS requires explicit fit and error reporting. |
| Boolean | N/C | Valid BRep operation when supported by an appropriate exact/tolerant kernel; output can include NURBS faces and trims. If evaluated via tessellation, classify as mesh conversion with tolerance. Preserve operands and history where possible; reject or diagnose unsupported self-intersections/degeneracies instead of silently flattening. |
| Array | N | Duplicate exact curves/surfaces/BRep components with affine transforms; retain source component IDs and distinguish instances from committed copies. If array offset depends on evaluated mesh dimensions, use E for that placement calculation. |
| EdgeSplit | E/C | Mesh edge duplication/topology change. Equivalent BRep seam/edge duplication can be exact when a split curve and trim update are defined; otherwise explicit mesh conversion. |
| Displace | E | Displace evaluated samples along a chosen normal/vector field. A CV-only displacement is a distinct approximation. Store texture/field dependencies and evaluation time separately. |
| UVProject | E/N | Apply a texture mapping to the designated texture-coordinate domain, not automatically intrinsic `(u,v)`. Can be N for an authored procedural UV mapping; avoid altering surface knots or trims. |
| Smooth | E | Mesh Laplacian smoothing changes sample positions. A NURBS fairing/smoothing tool requires explicit basis-aware constraints and preservation/error policy; do not conflate with exact geometric smoothing. |
| Cast | E | Evaluate cast deformation on derived samples; native CV approximation must be a separately labeled mode. |
| MeshDeform | E | Cage deform binds mesh topology. Could bind stable NURBS CVs, but the result is not equivalent to deformation of all surface points; use E by default. Invalidate/rebind on CV or BRep topology changes. |
| ParticleSystem | E/U | Mesh emission/instancing and particle evaluation may consume a tessellated NURBS proxy; keep particles/cache separate. Native surface sampling is a distinct integration; object-type support must be checked. |
| ParticleInstance | E/U | Instance evaluated geometry through particle state; no direct NURBS topology edit. Retain source object/data references. |
| Explode | E/C | Particle-driven face separation is mesh topology-specific. Could create separate BRep/NURBS components only via explicit topology extraction; otherwise evaluated mesh conversion. |
| Cloth | E/C | Simulation uses discretized mesh state. Keep cache/evaluated result distinct; applying requires explicit fitting/conversion and topology/continuity validation. |
| Collision | E | Collision input can use tessellated NURBS/BRep proxy with documented resolution/tolerance. Exact analytic collision queries may be added independently. Never persist the proxy as source geometry. |
| Ocean | E/C | Generates procedural ocean geometry and simulation data, commonly as a mesh/texture product. Treat as a separate generated object or derived evaluation; a NURBS fitting of the output is an explicit conversion with approximation error. |
| Bevel | N/C | Exact BRep edge/vertex bevel is a core CAD operation and may construct analytic/NURBS support faces. Use edge topology, radius/angle, corner patches, tolerances, and failure diagnostics; mesh bevel output is E/C and may differ. |
| Shrinkwrap | E/N | Projection of evaluated points onto target is E. Exact surface-to-surface projection or curve projection can be N only with a supported solver and trim/domain handling; CV projection alone is not whole-surface shrinkwrap. |
| Mask | E/N | Mesh mask is a scalar field on a mesh domain. Store NURBS masks as named attributes on explicit domains (CV, parameter samples, BRep vertex/face) and interpolate for evaluation. Keep mask separate from weights and geometry. |
| SimpleDeform | N/E | Rigid/affine transforms can map CVs exactly. Bend/taper/twist depend on continuous spatial field; implement exact supported cases or evaluate on derived samples. State which modes preserve exactness. |
| Multires | E/C | Multires is mesh subdivision hierarchy and displacement data. A NURBS equivalent needs a separate hierarchical surface/displacement contract; otherwise explicit conversion to mesh. |
| Surface | U/E | Legacy/specialized surface modifier entry. Inspect its implementation and object poll/support before promising an analogue; if it consumes evaluated surface samples, keep it derived. |
| Fluid | E/C | Fluid simulation and caches use discretized fields/geometry. NURBS may be an input collision/emitter proxy; outputs are separate volumes/meshes unless explicitly fitted. |
| ShapeKey | N | Store compatible key blocks over stable CV/BRep topology, usually Euclidean CV positions; preserve degree, knots, weights and trims unless a compatible topology-key system is designed. Topology edits must remap keys or reject/clear with user-visible policy. |
| Solidify | N/C | Exact thickness offset of planar/analytic surfaces may be constructed as BRep; general NURBS offsets can self-intersect or fail. Define tolerance, joins, caps, and failure behavior. Mesh solidify is E/C. |
| Screw | N | Exact revolution of supported lines/conics/NURBS can produce rational NURBS surfaces; preserve parameterization and weights. Separate operation from approximate mesh screw; define axis, pitch, caps, and topology. |
| Warp | E/N | General nonlinear warp is evaluated over samples. Exact affine/supported parametric transforms can act on CVs; arbitrary warp requires E or explicit approximation/refit. |
| WeightVGEdit | N/E | Operates on vertex-group scalar data. For NURBS, target an explicitly named influence field/domain; do not confuse its scalar values with rational CV weights. |
| WeightVGMix | N/E | Mix named scalar influence fields on a declared domain. Define interpolation and topology transfer; rational weights remain geometric basis data and are not inputs by default. |
| WeightVGProximity | E/N | Generate a scalar proximity field from evaluated geometry or exact distance queries. Bind to a chosen CV/BRep/parameter domain and recompute or remap after topology changes. |
| DynamicPaint | E/C | Paint simulation depends on sampled surface topology and caches. Use a derived mesh or parameter-space field; persistence must include domain, resolution, and cache version. Applying paint geometry/material output is separate. |
| Remesh | C | Voxel remesh changes representation/topology to a mesh. Offer as explicit conversion/reconstruction with scale/tolerance, feature loss, source retention, and undo. It is not an in-place exact NURBS edit. |
| Skin | E/C | Skin modifier builds mesh surface from a graph-like mesh skeleton. Could be a separate exact sweep/BRep feature over curve networks; do not treat existing mesh skin output as NURBS-native. |
| LaplacianSmooth | E | Smooth derived mesh positions. A NURBS fairing operator is separate, with fixed/weighted boundaries and continuity/error contract. |
| Triangulate | E/C | A mesh topology conversion. BRep triangulation is a display/export tessellation derived from exact faces; retain original face and diagonal policy only in the evaluated mesh. |
| UVWarp | E/N | Transform authored texture UV attributes on their declared domain; do not modify intrinsic NURBS parameters implicitly. Can be exact as a UV mapping operation. |
| MeshCache | E/C | External mesh sequence cache. It can drive a display/evaluated mesh layer or explicit NURBS refit; preserve cache source and avoid replacing exact geometry. |
| LaplacianDeform | E | Mesh-bound deformation solve. A NURBS implementation requires control/sample domain and solver definition; default evaluate on derived mesh and invalidate bindings on topology changes. |
| Wireframe | E/C | Mesh edges to faces conversion. A BRep wire-to-thin-wall operation is distinct and requires thickness/joins; otherwise output a mesh conversion. |
| Weld | N/C | BRep vertex/edge merging can be exact under tolerance and topology checks. CV weld is not automatically BRep sewing; preserve parameterization and resolve trims/shared edges. Mesh weld is an evaluated topology operation. |
| DataTransfer | E/N | Transfer attributes between objects using spatial/topological mapping. Support declared NURBS/BRep domains and interpolation; topology edits may invalidate correspondence maps, which must be recomputed or reported. |
| NormalEdit | E/N | Surface normals derive from exact derivatives and face orientation. A display normal override is evaluation metadata; it must not change NURBS geometry or trim orientation. |
| CorrectiveSmooth | E | Mesh-bound corrective deformation. Requires stable evaluated topology and bind data; retain as derived evaluation or provide a distinct CV/BRep solver. |
| MeshSequenceCache | E/C | Cached mesh sequence is an evaluated result. It can be displayed/exported as mesh; conversion to NURBS is explicit and fitted per frame with tolerance. |
| SurfaceDeform | E | Bind/deform against target surface samples. Exact target distance queries may help, but arbitrary resulting deformations need E unless a parametric solver is defined. Invalidate bind on topology changes. |
| WeightedNormal | E | Rendering normal treatment only. Use exact surface derivatives and BRep face/edge shading metadata; never rewrite CVs or geometric normals to mimic a mesh normal modifier. |
| Nodes | E/N/C | Geometry Nodes is a graph host, not one geometry contract. Each node must declare supported geometry domains and whether it preserves exact NURBS/BRep, returns derived geometry, or converts. Keep graph output and baked/applied output distinct. |
| MeshToVolume | C/E | Convert evaluated mesh boundary to volume. NURBS can be tessellated for the conversion; retain exact source separately and record voxel resolution. |
| VolumeDisplace | E | Operates on volume data. NURBS input/output is not direct; evaluate a separate volume product or convert explicitly. |
| VolumeToMesh | C/E | Volume extraction outputs a mesh. It is a conversion product, not a NURBS edit; optional NURBS fitting is another explicit step. |

For a stack, each stage must expose its input/output representation, topology
change behavior, exactness/tolerance, and cache/bind invalidation rules. NURBS
native stages should preserve source degree/order, knot domains, positive
weights, trims, and stable BRep identities unless the operation explicitly
changes them. E/C stages must operate on a derived result and provide an
explicit apply/convert action to commit a different geometry type.

## Attribute and feature lifecycle matrix

These are adjacent data systems, rather than simply more Edit Mode topology
operators. The “path” labels use the modifier table's definitions. Any domain
that is indexed by CV, BRep component, parameter sample, or evaluated vertex
needs explicit remapping or invalidation when topology changes.

| System | Path | Data mapping, UX, and topology lifecycle |
| --- | --- | --- |
| Vertex groups / deform weights | N/E | Preserve Blender-like named groups, assignment, select/deselect, normalize, invert, smooth, clean, quantize, limit-total, mirror, and weight-paint workflows where useful. Do not conflate group scalar weight with NURBS rational basis weight: rational weight changes the geometric basis and is positive; deform weight is an independent influence value. Define group domains (CV, BRep vertex, face/edge, or sampled field), interpolation, and transfer on split/weld/subdivide. |
| Generic named attributes | N/E | Support typed named data with explicit domains (object, CV, BRep vertex/edge/face/loop, surface parameter, curve parameter, evaluated point). Follow Blender's field/attribute UX where meaningful. Topology operations must interpolate/copy/drop attributes deterministically and preserve original data for undo. |
| UV / texture coordinates | N/E | Keep intrinsic knot parameter `(u,v)` separate from one or more material texture-coordinate maps. Store face-corner UVs if BRep seams permit discontinuity; alternatively define a surface mapping/field. Unwrap/pack/project/stitch act on texture mappings and BRep trim charts, not on knot vectors. Remap seams and UV islands on trim/topology edits. |
| Shape keys / morph targets | N | Store basis and key blocks against stable compatible CV/BRep identities. Position keys can preserve fixed topology; topology-changing edits need stable-ID remap, key propagation, or an explicit compatibility failure. Do not silently apply keys to a tessellated proxy. |
| Sculpt brushes and masks | E/N | Blender-like brush stroke, radius/strength/falloff, symmetry, front-face/occlusion, mask and visibility workflows can target derived mesh samples initially. Native CV sculpt is useful but is sparse and changes the whole basis function; exact-surface displacement needs a displacement field or fit contract. Preserve strokes/cache independently and make bake/apply explicit. |
| Vertex/color/weight/image paint | N/E | Paint independent color/scalar attributes on declared domains or textures. CV paint requires basis interpolation; face/loop paint requires BRep topology. Image texture painting remains texture-space. Transfer/undo must handle changed topology and resolution. |
| Face sets, hidden geometry, sculpt visibility | N/E | Store face labels and temporary visibility over BRep faces or derived mesh faces. A NURBS patch without BRep faces has no native face set domain; use evaluated display regions only with explicit non-persistent/sample semantics. Hidden components must have documented effect on picking, snapping, sculpt, and export. |
| Materials and face assignment | N/E | Object-level material slots are host data. Per-face assignments map naturally to BRep faces; per-mesh-polygon material indices map to evaluated polygons and must not become authoritative NURBS face topology. Splits inherit assignment; merges define conflict policy. |
| Geometric normals and shading normals | N/E | NURBS derivatives and oriented BRep faces define geometric normals. Smooth/flat, sharp edges, weighted normals, and custom render normals are display/render attributes. Keep them distinct from changing surface degree, knots, continuity, or orientation. |
| Constraints and dimensions | N | Object constraints/animation are host-level evaluated transforms. CAD geometric constraints (coincident, tangent, distance, angle, symmetry) require a solver over BRep/CV/parameter entities and need persistent references that survive topology edits. Mesh snapping/constraints do not provide this solver. |
| Animation and simulation cache | E/N | Animate object transforms, modifier parameters, attributes, or compatible CV positions with stable IDs. Keep evaluated geometry and caches separate from source; topology edits invalidate or remap channels. State whether export writes source, evaluated frame, or baked animation. |
| Scene gravity and gravity-driven simulation | E/N | Reuse Blender's scene-wide gravity vector and Global Gravity toggle. Rigid Body, Cloth, Soft Body, Fluid, particle systems, and Dynamic Paint read the scene setting, with per-system weights or overrides. NURBS objects need a defined simulation adapter: evaluated collision/deformation meshes for existing solvers, or a NURBS-aware solver. Keep simulation caches derived; baking back to NURBS is an explicit fit/conversion with error reporting. |
| Geometry Nodes / procedural graph | N/E/C | Each node declares accepted and produced geometry types and exactness. Preserve exact NURBS/BRep through supported operations; derived mesh nodes remain evaluated; representation-changing nodes require explicit conversion. Graph edits and baked results remain separately undoable. |
| Display, picking, render, and export | N/E/C | Tessellation is disposable derived data for display, hit testing, raster render, and mesh-only render/export formats. Exact exchange writes NURBS/BRep where supported. Mesh export explicitly tessellates using documented tolerance, angle, trim, and seam settings. Picking/snap indexes are rebuilt from authoritative geometry or a versioned derived cache. |

Rational NURBS weights are part of the representation and participate in basis
evaluation and exact geometry. Vertex-group/deform weights, color attributes,
masks, and shape-key values are separate data channels. UI labels and storage
must keep these concepts distinct; weight paint must not mutate rational CV
weights unless the user invokes a specifically named rational-weight editor.

## Sculpting, remeshing, and paint

Paint and sculpt are adjacent systems with separate sessions, brush settings,
stroke operators, undo, masks, visibility, symmetry, and geometry-domain
algorithms. Registration in `source/blender/editors/sculpt_paint/paint_ops.cc`
includes image painting, vertex painting/color operations, weight painting,
vertex/face selection and hiding, sculpt masking/visibility gestures, and UV
sculpt grab/relax/pinch (`paint_ops.cc:546-648`). Mesh sculpt implementation
lives under `source/blender/editors/sculpt_paint/mesh/`; brush families include
draw, grab, smooth, inflate, crease, pinch, mask, cloth, pose, boundary,
trim, filter, and face-set operations. Sculpt strokes, falloff, symmetry,
front-face/occlusion tests, masks, face sets, and dynamic topology are not the
same as CV transforms. A NURBS sculpt mode could operate on CV fields, exact
surface samples with a defined fitting policy, or a BRep tessellation while
retaining exact source geometry; the choice must be visible and undoable.

## Scene gravity and simulation coupling

The Scene Properties control shown in the Blender UI is registered as
`Scene.gravity` and `Scene.use_gravity` in
`source/blender/makesrna/intern/rna_scene.cc:8633-8643`. Gravity is a
scene-wide acceleration vector; the toggle controls whether global gravity is
used. Changing either property resets scene-object physics point caches and
tags the scene for evaluation through `rna_Physics_update`. The vector is
consumed by rigid body world updates
(`blenkernel/intern/rigidbody.cc:1605-1618`), cloth mass-spring forces
(`simulation/intern/SIM_mass_spring.cc:584-595`), soft-body forces
(`blenkernel/intern/softbody.cc:2036-2044`), fluid-domain gravity
(`blenkernel/intern/fluid.cc:483-505`), particle systems and hair
(`blenkernel/intern/particle_system.cc:2130-2134, 2286-2300`), and Dynamic
Paint (`blenkernel/intern/dynamicpaint.cc:5152-5160`). These systems may also
apply effector weights or a local gravity override.
The 3D View's Walk Navigation also reads the scene gravity toggle and its Z
component for falling/jumping behavior (`editors/space_view3d/view3d_navigate_walk.cc:610-616`).

This setting does not directly move an ordinary NURBS object. It is input to
the scene's simulations. NURBS integration therefore needs to preserve the
scene setting and connect simulated objects through an explicit evaluated
collision/deformation representation or a NURBS-aware solver. Rigid-body
collision, cloth and soft-body deformation, fluid boundaries, and particle
emission have different geometry needs; a single “supports gravity” flag is
not enough. Keep the simulation result/cache separate from exact source NURBS,
and make any bake to editable NURBS an explicit fit or conversion.

Object-level `OBJECT_OT_voxel_remesh` and `OBJECT_OT_quadriflow_remesh` are
registered in `object_ops.cc:235-243`. They are mesh-producing workflows.
Remesh can still be a useful explicit conversion/reconstruction feature for a
CAD BRep, provided the source, tolerance, feature loss, and resulting geometry
type are clear. It must not silently flatten an authoritative NURBS object.

Vertex paint and weight paint are data painting rather than geometric editing.
Mesh vertex colors, point/face attributes, face sets, masks, and weights need
domain-aware equivalents. Directly painting control vertices is possible but
can create visible discontinuities between sparse CV values; surface-sampled
paint requires interpolation and a storage contract.

## UV editor and parameter-space distinction

The UV editor is a full adjacent editing workspace. Its operator registration
`source/blender/editors/uvedit/uvedit_ops.cc:2925-2981` includes UV select
modes, linked/path/loop/ring/similar/overlap/winding/tile selection, move/snap,
align/arrange, rip/stitch/weld/remove-doubles, pin/hide/reveal, copy/paste,
unwrap, smart/cube/cylinder/sphere/view projection, pack islands, average
island scale, minimize stretch, reset, and seam tools.

For NURBS, distinguish at least three concepts:

1. The intrinsic parameter domain `(u,v)` used to evaluate a NURBS surface and
   its trims. This comes from the surface's U/V domains and knot vectors.
2. Texture coordinates (UV maps) used by materials. They may map parameter
   space, use a separate authored mapping, or be generated from another
   projection. They are not automatically identical to `(u,v)`.
3. BRep face-loop texture coordinates / seams, which may be discontinuous at
   shared edges even when the underlying surface geometry is continuous.

Decide whether the product needs a UV workspace and which of these it edits.
Unwrap, seams, islands, pinning, and stretch minimization require a mesh-like
parameterization or face-corner data model. A tensor-product surface already
has a natural parameter domain, but that does not imply low-distortion texture
coordinates, multiple UV sets, trim-aware island packing, or seam editing.

## Attributes, materials, vertex groups, and shape keys

Object-level data operations in `object_ops.cc:157-200` cover vertex group
CRUD, assignment, selection, normalize, invert, smooth, clean, quantize, limit,
mirror, ordering and weights; shape key add/copy/remove/clear/retime/mirror/move,
lock, basis and apply-to-basis; plus data-layout and data transfer at lines
231-233. Mesh data supports named attributes with different domains and types;
generic attribute APIs are broader than the old vertex-group model. Blender
mesh custom data also includes UV/color layers, skin radii, face sets, masks,
custom split normals, seams, creases, sharp flags, and other domain-specific
data (`mesh_ops.cc:60, 82-85, 124-150`).

NURBS/BRep should define attribute domains rather than force every layer onto
CVs. Candidate domains include object, CV/control-net point, curve parameter,
surface parameter, BRep vertex, edge, face, trim loop/edge, and evaluated
sample. A degree-one mesh-equivalent may map some domains directly; higher
degree attributes need basis-aware interpolation. Vertex groups and deform
weights can be CV weights, BRep vertex weights, or evaluated influence fields;
these differ in semantics. Shape keys are especially topology-sensitive: a
CV-position key can preserve fixed NURBS topology, while degree/knot/CV-count,
trim, or BRep topology changes need compatibility rules. Materials are
typically host/object or face assignments, not a property of CV count.

In the classiCAD prototype, component-editing state is distributed across the
viewport rather than owned solely by `SelectionModel`: `src/ui/viewport_widget.cpp`
defines Vertex/Edge/Face modes near line 142, per-mode/per-object selected
component maps and active indices near lines 1900-1945 and 8200-8217, and routes
selected component sources into extrusion around `beginPointExtrude` at line
2556 onward. Scale and Rotate tools also accept per-object CV index sets
(`src/tools/scale_tool.h:54`, `rotate_tool.h:51`). The Outliner builds container
rows with geometry-object children (`src/ui/panels/outliner_panel.cpp:89-177`)
and synchronizes the current row to selection (`:223-245`). Its tree uses
single-row selection (`:53-58`); component selection is maintained separately
in the viewport. This is a UI/state split, not evidence that the application
lacks component selection or multi-object component operations. Treat these
prototype facilities as evidence of a distributed implementation and a
possible starting point, not as normative requirements.

## Constraints, animation, Geometry Nodes, evaluation

Object constraints are registered in `object_ops.cc:138-155`; object animation
is represented by animation data/F-curves and edited in Action, Graph, and NLA
spaces. These are host features, distinct from geometric constraints such as
coincident, tangent, dimensional, or symmetry relations in a CAD sketch/BRep.
If object constraints or animation are supported, define how evaluated
transforms affect NURBS frames and how exact source geometry stays editable.
Point-level constraints need a separate solver and persistence contract.

Geometry Nodes is registered as a modifier (`MOD_util.cc:219`) and object-level
node tree operations are in `object_ops.cc:130-132, 221-226`. It is a general
procedural graph/evaluation framework. The fork includes NURBS-related
geometry nodes (for example `source/blender/nodes/geometry/nodes/node_geo_set_nurbs_order.cc`
and `node_geo_set_nurbs_weight.cc`), but that does not establish support for
every editor, modifier, or operation on every native NURBS/BRep object. For a
CAD system, node evaluation, cache/bake, and apply-to-source should be separate
from the authoritative native geometry unless an explicit operation commits
the result.

The depsgraph/evaluation split, modifier stack ordering, animation evaluation,
and render-facing evaluated geometry are useful host concepts. CAD should keep
the exact NURBS/BRep model authoritative while derived tessellation, render
meshes, picking acceleration, and caches remain disposable derived data. Export
must state whether it writes exact NURBS/BRep, an evaluated representation, or a
mesh conversion; format capabilities differ. A display or render proxy cannot
stand in for saved exact control points, weights, knots, trims, or topology.

## Scope boundaries and completeness

This file deliberately covers Blender's principal built-in adjacent families:
object operators; the registered modifier type roster; mesh sculpt/paint and
remesh; UV editing; attributes/materials/vertex groups/shape keys; constraints,
animation, Geometry Nodes, evaluation, and export implications. It does not
enumerate every operator in every editor, every individual brush property,
every Geometry Nodes node, every shader/material node, every render engine, or
third-party/add-on feature. Those catalogs are open-ended and version/config
dependent. The source paths are anchors for follow-up audits, not an exhaustive
API specification. No runtime enabled-state claim is made.

## Source anchors in the Blender Fork

- Edit Mode mesh tools and macros: `source/blender/editors/mesh/mesh_ops.cc`.
- Curve/NURBS edit operators: `source/blender/editors/curve/curve_ops.cc`,
  `editcurve_select.cc`, `editcurve.cc`.
- Generic transforms: `source/blender/editors/transform/transform_ops.cc`.
- Object, modifier, constraint, group, key, remesh operators:
  `source/blender/editors/object/object_ops.cc`.
- Modifier type declarations and installed roster:
  `source/blender/modifiers/MOD_modifiertypes.hh`,
  `source/blender/modifiers/intern/MOD_util.cc`.
- Paint/sculpt registration: `source/blender/editors/sculpt_paint/paint_ops.cc`;
  mesh brush and sculpt implementations under
  `source/blender/editors/sculpt_paint/mesh/`.
- Scene gravity and simulation consumers: `source/blender/makesrna/intern/rna_scene.cc`,
  `source/blender/blenkernel/intern/rigidbody.cc`, `softbody.cc`, `fluid.cc`,
  `particle_system.cc`, `dynamicpaint.cc`, and
  `source/blender/simulation/intern/SIM_mass_spring.cc`; Walk Navigation also
  reads the setting in `source/blender/editors/space_view3d/view3d_navigate_walk.cc`.
- UV tool registration and algorithms:
  `source/blender/editors/uvedit/uvedit_ops.cc`,
  `uvedit_unwrap_ops.cc`, and sibling `uvedit_*.cc` files.
- NURBS Geometry Nodes examples:
  `source/blender/nodes/geometry/nodes/node_geo_set_nurbs_order.cc`,
  `node_geo_set_nurbs_weight.cc`.
