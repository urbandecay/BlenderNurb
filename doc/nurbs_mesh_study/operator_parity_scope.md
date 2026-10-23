# Blender mesh-edit operator parity map

`operator_parity_map.csv` is the row-level NURBS parity analysis for every row in [`operator_inventory.csv`](operator_inventory.csv). It has 358 data rows: 148 direct `MESH_OT` operators, 16 mesh macros, 83 BMesh operator definitions, 49 UV operators, one UV macro, eight Python mesh/edit wrappers, 19 registered transform-mode operations, five additional transform/orientation/gizmo registrations, two mesh-edit transform helpers, seven View3D snap actions, nine vertex-group weight actions, five vertex-group management actions, and six hook actions. The [Mesh Edit menu crosswalk](edit_mesh_menu_crosswalk.md) checks the visible menu, its fixed built-in submenus, and the separate Vertex, Edge, and Face context menus against this inventory. The CSV includes mesh, UV, transform, snap, weight, and hook workflows rather than silently excluding contextual actions.

## How to read the map

The current-checkout review adds
[`operator_workflow_classes.csv`](operator_workflow_classes.csv), keyed by the
same 358 ordered operator IDs. It uses the handoff's four workflow classes:
1 = same interaction/topology, 2 = NURBS-specific exact construction,
3 = restricted or tolerance-managed geometry, 4 = representation-specific or
explicit conversion/separate alternative. Each row states conditions; these
are proposed contracts, not implementation or acceptance results. The original
five geometry categories below answer a different question and are retained as
research detail. Read both alongside
[`current_checkout_audit.md`](current_checkout_audit.md).

The new conditions correct several conflicting older recommendations: polygon
primitive segment counts are topology inputs, unsubdivide is not knot removal,
region extrusion is not automatic solid creation, varying-normal extrusion is
not uniform exact translation, and subdivision smoothing is a shape change.
Texture-map work is classified as data editing and never changes p-curves.
General partial movement of curved supports is not covered by the exactness of
a complete-support affine transform. Macro classification follows its geometry
children; a macro registration is not an additional algorithm.

The user's later clarification is **fit-point editing only**. Older row text
that mentions selectable CVs or an advanced control-net editing view does not
define the current product. Control coefficients and weights remain internal;
topology components and fit handles are distinct source domains. The current
workflow map also limits exact dissolve to proven common-support partitions;
cross-support reconstruction and tolerance-based cleanup need different gates.

Each row records the source behavior description where available, a proposed NURBS behavior, Blender interaction code that can be reused, and the main implementation dependencies. These are design recommendations for a Blender-like NURBS editor, not claims that Blender currently supports NURBS or that any existing prototype restriction is required. A mesh macro is assessed as a single UX/action sequence while its children remain separately inventoried. BMesh definitions are kernel items, not standalone user-facing operators; their rows identify whether their geometry work can be replaced or whether only the caller interaction can be reused. Generic transform rows are registered mode operations, and `TRANSFORM_OT_seq_slide` is intentionally identified as sequencer-only, outside spatial geometry.

The five `geometry_category` values mean:

| Category | Use in this document |
| --- | --- |
| `support-preserving exact` | The represented shape can be retained exactly while changing selection, placement, parameterization, orientation, or controls under defined semantics. |
| `exact construction` | There is a direct exact NURBS construction or an exact result for the stated representable cases. |
| `tolerance-managed` | Intersections, offsets, fitting, joins, or other numerical operations require an explicit tolerance and robust topology handling. |
| `mesh-specific with NURBS alternative` | Blender's exact mesh operation has no identical NURBS state, but a user-facing NURBS action can be designed around controls, spans, parameter curves, trims, or attributes. |
| `no meaningful counterpart` | The operation concerns polygon tessellation/storage or another non-spatial domain and should remain mesh-specific or be exposed as an explicit derived-mesh action. |

Categories describe a recommended NURBS interpretation, not a proof that an operation is impossible. Many mesh concepts can be translated into an alternate NURBS workflow; the row names the semantics and dependency instead of equating a tessellated display mesh with authoritative geometry. For example, same-support triangulation, quad merging, beauty repartition, poke, edge rotation, split/rip, and dissolve are exact trim/topology operations that leave the underlying support unchanged when their topology preconditions hold. Blender-style face strength and smooth/flat/custom normals can be represented as display metadata or derived shading behavior. `flip_quad_tessellation` is only a derived display triangulation hint, and spatial reordering is an acceleration/storage optimization; neither should mutate NURBS geometry. Blender's Spin and Screw operators should retain their requested finite step counts and produce discrete transformed geometry. Continuous surface-of-revolution or helical NURBS tools are separate CAD operations, with their own exactness or tolerance contracts. Mesh polygon primitives can be retained as exact degree-one BRep assets, while analytic rational circles, spheres, cylinders, or cones are distinct CAD variants rather than silent reinterpretations. UV editing is distinguished from geometry: Blender texture UV maps are separate data over NURBS faces and trims, and must never edit intrinsic surface parameter coordinates, knots, or geometric trim p-curves. UV selection, transforms, stitching, unwrap/projection, and packing map to that separate texture-coordinate layer.

## Coverage and limitations

This is a static source inventory and recommendation map, not a runtime registration dump. `operator_inventory.csv` supplies source locations, compile guards, macro children, discovered property/poll data, and coverage notes; use it with the parity table to trace each row. Conditional compilation, runtime context, build configuration, and poll outcomes can change what is enabled in a particular Blender build. The inventory does not claim to enumerate add-ons, user scripts, third-party extensions, every editor's non-mesh operations, dynamic Geometry Nodes operator assets, or runtime-generated operators beyond the explicit registrations listed.

Some lower-level rows share a family-level recommendation because they implement the same geometric concept; where source comments do not expose user-visible behavior, the `source_behavior` cell says so rather than inventing detail. These entries are marked with source references and should be read alongside the implementation. Recommendations involving trimmed surfaces, joins, intersection classification, continuity, attribute domains, and self-intersection policy depend on NURBS topology/BRep capabilities and explicit product semantics. They are not implied to exist in the current classiCAD geometry model.

The row count and scope breakdown were checked against the source inventory while preparing this artifact. No application code, build, or tests were changed or run for the map.
