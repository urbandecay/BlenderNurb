# Mesh Edit menu crosswalk

This checks the fixed built-in entries shown by `VIEW3D_MT_edit_mesh` in
`scripts/startup/bl_ui/space_view3d.py` against the NURBS operator map. It
includes its fixed submenus and the edit-mode context used by the screenshot.
The table is source-based; runtime visibility still depends on mode, selection,
build options, and installed Geometry Nodes operator assets.

| Menu item | Fixed built-in actions reached from the menu | NURBS coverage and notes |
|---|---|---|
| Transform | Translate, Rotate, Scale, To Sphere, Shear, Bend, Push/Pull; To Circle, Flatten, Shrink/Fatten, Space Edge Loops Evenly, Skin Resize, Warp, Randomize | The transform-mode and mesh-operator rows are in the parity map. This review adds explicit rows for `TRANSFORM_OT_vertex_warp` and `TRANSFORM_OT_vertex_random`. Keep whole-support affine transforms exact; CV and BRep-vertex transforms are separate targets with different effects on the visible surface. |
| Mirror | Interactive Mirror and global/local X/Y/Z reflection | `transform.mirror` is inventoried. Whole-support reflection is exact; partial BRep reflection needs topology and weld semantics. |
| Snap | Selection to Grid; Selection to Cursor (two keep-offset variants); Selection to Active; Cursor to Selected, World Origin, Grid, or Active | Seven distinct `VIEW3D_OT_snap_*` IDs are now inventoried. Cursor-only actions leave geometry unchanged. Component snaps need stable anchors and the same selected-domain behavior as Blender. |
| Duplicate | Duplicate and move | `MESH_OT_duplicate_move` is mapped. Copy exact NURBS data and topology/component identity before applying the move. |
| Extrude | Face region, along normals, individual faces, manifold; edges; vertices; repeat; Spin | The mesh macros, direct mesh operators, and Python extrusion wrappers are mapped. Preserve Blender's separate sheet, region, individual, manifold, and finite-step behavior. |
| Merge | Merge modes and By Distance | `MESH_OT_merge` and `MESH_OT_remove_doubles` are mapped. Keep geometric coincidence separate from a topological weld and continuity claim. |
| Split | Selection split and edge-split modes | `MESH_OT_split` and `MESH_OT_edge_split` are mapped as topology duplication and trim/edge-reference updates. |
| Separate | Selection, loose parts, or material-based separation | `MESH_OT_separate` is mapped. Create separate NURBS/BRep objects or components while preserving exact support and explicit shared/weld references. |
| Bisect | Plane-defined cut, with the operator's clear/fill options | `MESH_OT_bisect` is mapped. Intersect supports, create p-curves and trim partitions; numerical cases use the declared tolerance and atomic failure policy. |
| Knife Project | Project outlines from other objects | `MESH_OT_knife_project` is mapped. Use the projection frame and split/trim affected NURBS faces. |
| Knife Topology Tool | Interactive knife cuts | `MESH_OT_knife_tool` is mapped. Preserve screen interaction while constructing curves and trim splits on authoritative supports. |
| Convex Hull | Convex hull of selected geometry; only shown when Bullet is enabled | `MESH_OT_convex_hull` is mapped. For selected BRep vertices, construct the exact degree-one polyhedral hull. A hull of whole curved geometry or selected CVs is a different input definition and needs an explicit exact or tolerance-managed policy. |
| Symmetrize | Mirror selected geometry and replace/clear the opposite side | `MESH_OT_symmetrize` is mapped. Reflect supports and topology, then apply the selected direction and seam-weld threshold. |
| Snap to Symmetry | Move corresponding vertex pairs to mirrored positions | `MESH_OT_symmetry_snap` is mapped. Establish geometric/topological correspondence and tolerance before reconciling supports. |
| Normals | Flip, recalculate inside/outside, set/rotate/point normals, merge/split/average, copy/paste/reset/smooth custom vectors, and face strength | The direct mesh registrations and transform normal mode are mapped. Store render/shading overrides separately from analytic NURBS derivatives and face orientation. |
| Shading | Smooth/flat faces and smooth/sharp edges or vertices; set sharpness by angle | The direct mesh registrations are mapped. Preserve display metadata without changing knots, degree, or geometric continuity. |
| Weights | Normalize All, Normalize, Mirror, Invert, Levels, Smooth, Clean, Quantize, Limit Total | The nine `OBJECT_OT_vertex_group_*` operations are now inventoried individually. Give weights explicit CV/BRep-vertex domains; never treat deform weights as rational NURBS basis weights. |
| Set Attribute | Set values of the active attribute on selected elements | `MESH_OT_attribute_set` is mapped. Define supported NURBS/BRep domains and propagation rules. |
| Sort Elements | Sort selected vertices/edges/faces by the selected method | `MESH_OT_sort_elements` is mapped. Define ordering over stable topology IDs; storage order must not alter geometry or component identity. |
| Show/Hide | Hide selected, hide unselected, reveal | `MESH_OT_hide` and `MESH_OT_reveal` are mapped. Keep temporary component visibility distinct from scene/object visibility. |
| Clean Up | Delete Loose, Decimate, Dissolve Degenerate, Dissolve Limited, Face Make Planar, Connect Non-Planar/Concave, Remove Doubles, Fill Holes | These direct mesh operators are present in the parity map. They need operation-specific BRep/NURBS behavior: exact removal only for true degeneracies; tolerance-managed simplification/fitting where shape changes; same-support trim partition where topology alone changes. |
| Delete | Delete by element type; dissolve vertex/edge/face; limited dissolve; edge collapse; delete edge loops | The direct mesh registrations are mapped. Preserve the distinction between deletion, topological dissolve, and geometric edge collapse. |

The Mesh menu also calls
`template_node_operator_asset_menu_items(catalog_path="Mesh")`. Those entries
are dynamic installed assets rather than a fixed built-in list, so this source
crosswalk cannot enumerate them. They need the same per-operation geometry
contract if NURBS-compatible node assets are added.

## Vertex context menu

The screenshot's `Vertex` menu is `VIEW3D_MT_edit_mesh_vertices` in the same
file, plus its `Vertex Groups` and `Hooks` submenus. It is a separate contextual
menu from the main `VIEW3D_MT_edit_mesh` menu above. Its fixed built-ins are
covered here; the final Geometry Nodes asset menu remains dynamic.

There is a key data-model distinction for this menu: Blender mesh vertices are
topological points joined by explicit edges and faces. A NURBS control vertex
is a handle in a curve or surface's control net and is not generally a point on
the visible shape. A CAD-style NURBS editor therefore needs both selectable
control vertices and, for trimmed BRep geometry, topological vertices. The
operator must declare which domain it edits; it must not silently use the
tessellated display mesh as the model.

| Vertex menu item | Fixed built-in action(s) | NURBS coverage and notes |
|---|---|---|
| Extrude Vertices; Extrude to Cursor or Add | `MESH_OT_extrude_vertices_move`; `MESH_OT_dupli_extrude_cursor` | Keep Blender's modal placement, cursor, snap, constraints, preview, and undo. Control-net extrusion needs NURBS-aware knot/endpoint extension rules; BRep-vertex extrusion needs new edge/face topology. Knot insertion can add controls while exactly preserving the old shape, but arbitrary new control locations deform it. This is distinct from object-level curve extrusion into a ruled surface. |
| Bevel Vertices | `MESH_OT_bevel` with `affect='VERTICES'` | For BRep vertices, construct a fillet/chamfer from adjacent NURBS faces and trim the result; this is tolerance-managed and can fail at corners or tight radii. A control vertex has no incident model edges to bevel, so it needs an explicitly named control-net operation if desired. |
| New Edge/Face from Vertices | `MESH_OT_edge_face_add` | Ordered BRep endpoints can form an exact degree-one edge. A face can be an exact planar/ruled patch or a trim region on an existing support when the boundary fits; general boundaries require compatible parameterization or tolerance-controlled fitting. CVs alone do not define a face boundary. |
| Connect Vertex Path; Connect Vertex Pairs | `MESH_OT_vert_connect_path`; `MESH_OT_vert_connect` | On a trimmed NURBS face, make a path that lies on the support, then split its trim region while retaining the support. Across multiple faces, maintain p-curve and edge continuity. A path between CVs without BRep faces is a control-net edit or a separate curve-creation action, not a face split. |
| Rip Vertices; Rip Vertices and Fill; Rip Vertices and Extend | `MESH_OT_rip_move` (two menu property variants); `MESH_OT_rip_edge_move` | Rip duplicates/splits BRep topology while retaining the original supports, then moves the chosen side. Fill requires a valid patch: exact only in constrained cases, otherwise tolerance-managed. Extend splits an adjacent edge and moves the new vertex. These are not control-vertex duplication commands. |
| Slide Vertices | `TRANSFORM_OT_vert_slide` | Slide BRep vertices along incident NURBS edge curves and update adjacent trims exactly. Sliding a control vertex instead deforms the curve/surface and is a different transform mode. |
| Smooth Vertices; Smooth Vertices (Laplacian) | `MESH_OT_vertices_smooth`; `MESH_OT_vertices_smooth_laplacian` | Offer NURBS control-net fairing or BRep support reconstruction with an explicit tolerance and visible shape-change preview. Fixed knots and weights can be retained for CV fairing, but the represented shape changes; do not smooth the display tessellation as authoritative data. |
| Vertex Crease | `TRANSFORM_OT_vert_crease` | NURBS continuity depends on degree, knots, weights, and control-point layout. Define a NURBS crease action around those rules; a mesh crease scalar is not itself a knot value or rational weight. Knot refinement can preserve the current shape before a deliberate continuity change. |
| Blend from Shape; Propagate to Shapes | `MESH_OT_blend_from_shape`; `MESH_OT_shape_propagate_to_all` | Support named NURBS variants when they share compatible degree/order, knot arrays, parameter domains, rational structure, and control-net indexing. Blend corresponding control points; propagate edits through stable CV correspondence. Mismatched structures need remapping or must be rejected. |
| Vertex Groups submenu | `OBJECT_OT_vertex_group_assign_new`; `OBJECT_OT_vertex_group_assign`; `OBJECT_OT_vertex_group_remove_from`; `OBJECT_OT_vertex_group_set_active`; `OBJECT_OT_vertex_group_remove` | Store deform weights on stable NURBS CV IDs or BRep-vertex IDs and reuse group management semantics. Keep these scalar deform weights entirely separate from rational NURBS basis weights. Deformation modifiers only use them when explicitly wired to that domain. |
| Hooks submenu | `OBJECT_OT_hook_add_selob`; `OBJECT_OT_hook_assign`; `OBJECT_OT_hook_remove`; `OBJECT_OT_hook_select`; `OBJECT_OT_hook_reset`; `OBJECT_OT_hook_recenter` | Retain the object-target hook workflow as a non-destructive NURBS-aware modifier over stable CV IDs. Assign/select/reset/recenter/remove operate on that modifier's component set and transform. BRep-vertex hooks need support/trim reconciliation when the point belongs to several faces. Hook targets are ordinary objects; this feature does not depend on armatures or bone rigging. |

The current CSV already covered the 14 unique geometry/transform operator IDs
in this menu. This crosswalk adds the five vertex-group management operators
and six hook operators that the earlier inventory had missed. It also makes
explicit that several menu actions need different semantics for control
vertices and BRep topology vertices.

## Edge context menu

The screenshot's `Edge` menu is `VIEW3D_MT_edit_mesh_edges` in
`scripts/startup/bl_ui/space_view3d.py`. It contains 17 unique fixed operator
IDs; the clockwise/counter-clockwise, mark/clear, and vertex-derived sharpness
entries are property variants of shared operators. Freestyle marking is
conditional on Blender being built with Freestyle.

| Edge menu item | Fixed built-in action(s) | NURBS coverage and notes |
|---|---|---|
| Extrude Edges | `MESH_OT_extrude_edges_move` | Preserve the edge-selection and modal move workflow. For a wire NURBS curve, create an exact ruled surface where the source and displaced curves define one; for a BRep edge, create/update adjacent face boundaries and trims with explicit sheet/topology rules. This is different from extruding a whole face region. |
| Bevel Edges | `MESH_OT_bevel` with `affect='EDGES'` | Offset adjacent NURBS supports, intersect or blend them, and rebuild trims. Use exact conics/ruled patches in constrained cases; otherwise require a tolerance-managed result and handle corner joins atomically. |
| Bridge Edge Loops | `MESH_OT_bridge_edge_loops` | Bridge ordered boundary curves with a NURBS loft/patch, preserving them as trims. Exact construction depends on compatible parameterization and correspondence; provide a fit tolerance for general boundaries. |
| Screw | `MESH_OT_screw` | Keep Blender's finite turn and step controls. Build the requested discrete NURBS/BRep copies or connecting strips. An analytic continuous helical sweep is a separate CAD command. |
| Subdivide; Subdivide Edge-Ring | `MESH_OT_subdivide`; `MESH_OT_subdivide_edgering` | Split BRep face trims along paths on their existing supports and preserve the surface exactly. A loop cut adds topological edges; knot insertion refines a control net without adding a BRep edge, so they must remain distinct operations. |
| Un-Subdivide | `MESH_OT_unsubdivide` | Remove a reversible BRep partition pattern only when neighboring trims can be recombined on the same support. This is topology cleanup, not NURBS knot removal; knot removal needs its own exact-or-tolerance contract. |
| Rotate Edge CW/CCW | `MESH_OT_edge_rotate` with `use_ccw` variants | Repartition adjacent trims around the alternate shared edge while retaining the support and shape where the result remains representable. It needs valid adjacent topology and does not rotate a NURBS control-net row. |
| Edge Slide | `TRANSFORM_OT_edge_slide` | Slide shared trim edges along adjacent NURBS supports while preserving support geometry; update p-curves and shared topology. Sliding a control-net row is a separate shape-changing edit. |
| Loop Cut and Slide; Offset Edge Slide | `MESH_OT_loopcut_slide`; `MESH_OT_offset_edge_loops_slide` | Reuse Blender's hover, loop selection, modal slide, numeric input, and undo. Add NURBS trim partitions along valid surface paths; offset is exact for supported isoparametric cases and otherwise needs tolerance-managed curve construction. Do not silently substitute knot insertion for the visible topology cut. |
| Edge Bevel Weight | `TRANSFORM_OT_edge_bevelweight` | Store a separate BRep-edge bevel-weight value and define how a NURBS-aware bevel operation consumes it. The value alone does not alter the NURBS support and is not a rational basis weight. |
| Edge Crease | `TRANSFORM_OT_edge_crease` | NURBS edge continuity comes from degree, knots, weights, and control-point layout. Define an explicit continuity edit or display/subdivision metadata behavior; a mesh crease scalar is not itself a NURBS knot value. |
| Mark/Clear Seam | `MESH_OT_mark_seam` with `clear` variants | Store a texture-mapping seam hint on BRep edges. Keep it separate from geometric trim p-curves and from the inherent parameter seam of a periodic NURBS surface. |
| Mark/Clear Sharp; Mark/Clear Sharp from Vertices | `MESH_OT_mark_sharp` with `clear` and `use_verts` variants | Store display/shading sharpness on selected shared BRep edges; the vertex variants mark incident edges. This does not change analytic surface normals or geometric continuity. |
| Set Sharpness by Angle | `MESH_OT_set_sharpness_by_angle` | Evaluate adjacent support normals along shared edges and set display-sharp metadata using Blender's angle/extend choices. Keep this separate from changing NURBS continuity. |
| Mark/Clear Freestyle Edge | `MESH_OT_mark_freestyle_edge` with `clear` variants | Store a render feature-edge tag for NURBS/BRep edges. This menu item and operator exist only when Blender is built with Freestyle; the tag does not change geometry or texture seams. |

All 17 operator IDs in this Edge menu were already present in the row-level
inventory. This crosswalk makes their visible menu behavior explicit and
separates BRep edge operations, control-net edits, and edge metadata.

## Face context menu

The screenshot's `Face` menu is `VIEW3D_MT_edit_mesh_faces`; its `Face Data`
submenu is `VIEW3D_MT_edit_mesh_faces_data`, in
`scripts/startup/bl_ui/space_view3d.py`. The menu exposes 23 unique fixed
operator IDs, with multiple extrusion, shading, and attribute variants. Most
geometry actions operate on topological faces, not directly on a tensor-product
surface's control net. NURBS face parity therefore depends on trimmed BRep
faces, shared edges, and valid parameter-space trim curves. The displayed
surface tessellation is never authoritative.

| Face menu item | Fixed built-in action(s) | NURBS coverage and notes |
|---|---|---|
| Extrude Faces; Along Normals; Individual Faces | `view3d.edit_mesh_extrude_move_normal`; `view3d.edit_mesh_extrude_move_shrink_fatten`; `MESH_OT_extrude_faces_move` | Preserve the three distinct interaction modes. Under the proposed BlenderNurb solid contract, planar trimmed face sources can create a closed `NurbsSolid` with matching caps and ruled walls; curved base faces remain unsupported unless a separate contract is added. Individual selected faces remain separate solids. |
| Inset Faces | `MESH_OT_inset` | Keep planar inset behavior exact with curve offsets and trim updates. Curved supports need a defined support-space offset or a tolerance-managed spatial offset; do not silently substitute geodesic distance. |
| Poke Faces | `MESH_OT_poke` | Add an interior BRep vertex and spokes as trim curves on the unchanged support. Require valid p-curves and reject degenerate regions; do not edit the control net as a side effect. |
| Triangulate Faces; Triangles to Quads | `MESH_OT_quads_convert_to_tris`; `MESH_OT_tris_convert_to_quads` | Partition a face into triangular trims or merge compatible adjacent trim regions on the same support. Preserve the exact support and shared topology; keep separate faces where supports differ. |
| Solidify Faces | `MESH_OT_solidify` | For supported planar trimmed faces, create a closed solid with translated caps and ruled boundary walls. General NURBS offsets, self-intersections, and corner joins need explicit tolerance handling; the proposed face-solid contract rejects unsupported curved base faces. |
| Wireframe | `MESH_OT_wireframe` | Generate NURBS curves from chosen boundaries/isocurves or ruled strips from surface regions. Define which source lines are included; thickness and corner joins are separate sweep/offset work. |
| Fill; Grid Fill; Beautify Faces | `MESH_OT_fill`; `MESH_OT_fill_grid`; `MESH_OT_beautify_fill` | Fill closed loops with an exact planar/compatible patch or a tolerance-managed fit; grid fill needs ordered, compatible boundaries. Beautify can repartition trims on the same support without changing the represented shape. |
| Intersect (Knife) | `MESH_OT_intersect` | Blender's operator intersects selected faces with themselves or selected/unselected faces, then splits the result; it is not the Knife Project tool. Compute NURBS intersection curves, build p-curves, split trims, and honor separate/cut modes under a robust tolerance policy. |
| Intersect (Boolean) | `MESH_OT_intersect_boolean` | Provide NURBS/BRep Intersect, Union, and Difference for supported closed solids. Surface intersections, inside/outside classification, coincident cases, and trim reconstruction require a tolerance-managed Boolean kernel. |
| Weld Edges into Faces | `MESH_OT_face_split_by_edges` | Associate selected loose-edge networks with affected faces, splice their endpoints into the face boundaries, and split the faces into new regions. Exact when the curves lie on the supports; otherwise require explicit projection/intersection and tolerance rules. |
| Shade Smooth; Shade Flat | `MESH_OT_faces_shade_smooth`; `MESH_OT_faces_shade_flat` | Keep these as display/shading metadata over derived rendering. Analytic NURBS normals remain the geometric source of truth. |
| Face Data: Rotate/Reverse Colors | `MESH_OT_colors_rotate`; `MESH_OT_colors_reverse` | Operate on a declared NURBS face-boundary color-attribute domain and preserve interpolation through tessellation. This changes attribute data, not geometry. |
| Face Data: Rotate/Reverse UVs | `MESH_OT_uvs_rotate`; `MESH_OT_uvs_reverse` | Change a separate texture-UV map attached to the NURBS face. Never rewrite intrinsic surface U/V, knots, parameter domains, or geometric trim p-curves. |
| Face Data: Flip Quad Tessellation | `MESH_OT_flip_quad_tessellation` | No authoritative NURBS geometry counterpart. If retained, it is a derived display-tessellation hint only; it must not change the NURBS support or trims. |
| Face Data: Mark/Clear Freestyle Face | `MESH_OT_mark_freestyle_face` with `clear` variants | Store a render feature-detection exclusion tag on NURBS/BRep faces. The operator and menu item are conditional on Blender being built with Freestyle. |

The Face Data submenu also exposes dynamic Geometry Nodes operator assets;
their installed contents cannot be enumerated as fixed built-ins. All 23
fixed operator IDs above are already in the row-level inventory. The crosswalk
clarifies texture UVs versus intrinsic surface parameters and separates
same-support trim edits from actual NURBS control-net refinement.

## Shared transform orientations

The screenshot's Transform Orientations selector is shared by Blender's
transform and gizmo system, rather than being a mesh-only menu. The six built-in
choices should remain available while editing NURBS. The official [Blender 5.2
orientation manual](https://docs.blender.org/manual/en/5.2/editors/3dview/controls/orientation.html)
describes the user-facing choices; the implementation is in
`source/blender/editors/transform/transform_orientations.cc`.

| Orientation | Blender behavior | NURBS behavior to preserve |
|---|---|---|
| Global | World XYZ axes. | Reuse directly. It changes the transform axes, not the NURBS parameter directions, knots, or weights. |
| Local | Active object's local axes. | Reuse the object's transform frame. For a selected support or object, keep the same active-object rule as Blender. |
| Normal | In Edit Mode, derive axes from the active or selected components; in Object Mode, Blender falls back to Local. | Use the NURBS/BRep selection adapter below. A topological face supplies its analytic surface normal; an edge supplies its curve tangent and incident-face context. A control vertex gets a control-net frame, not a claim that the CV lies on the surface. |
| Gimbal | Use the active object's Euler gimbal axes when available; Blender falls back to its Normal calculation. | Reuse the active object's gimbal frame. It is not a special NURBS surface frame and does not vary with the selected CVs. Preserve Blender's fallback behavior. |
| View | Viewport camera axes. | Reuse directly, including for transform constraints and gizmos. |
| Cursor | 3D Cursor rotation axes. | Reuse directly. The cursor position remains a separate pivot choice. |
| Custom (`+`) | Save the current selection-derived or object frame and expose it by name. | Support both NURBS control-net selections and BRep selections. Reuse the same create/select/delete lifecycle and matrix storage. |

There is already a legacy NURBS-adjacent path in Blender: `getTransformOrientation_ex`
handles `OB_CURVES_LEGACY` and `OB_SURF`. Active BPoints use
`BKE_nurb_bpoint_calc_normal` and `BKE_nurb_bpoint_calc_plane`, which derive a
frame from neighboring control points. This is a **control-net** frame, not an
evaluated surface normal. The current multi-selection branch only aggregates
BPoints when `pntsv == 1`, so selected CVs on a 2D NURBS surface do not yet get
the same full group-orientation treatment as selected mesh faces. Blender's
custom-orientation creator explicitly dispatches to mesh and legacy curve edit
objects; NURBS surface editing needs to be included deliberately as well.

For full CAD-style NURBS parity, `Normal` needs these explicit selection rules:

- A selected BRep face uses its oriented analytic normal from surface
  derivatives. Multiple selected faces use a stable aggregate; active-element
  pivot uses the active face, matching Blender's active-selection convention.
- A selected BRep edge uses the exact edge-curve tangent. Its other axis comes
  from the incident face normal(s), with a deterministic rule at seams,
  boundaries, and nonmanifold edges.
- A selected BRep vertex uses its selected incident topology and active face or
  edge to construct a frame. Ambiguous disconnected or multi-face cases need a
  deterministic fallback.
- A selected surface CV uses neighboring U/V control-net directions. It does
  not use the displayed tessellation normal or pretend that the CV itself is a
  point on the evaluated surface. A CV and a BRep vertex remain separate edit
  domains.
- Degenerate or empty selections fall back consistently to the same behavior
  Blender uses for that mode; they must not produce a zero or unstable matrix.

The orientation only defines the transform axes. Pivot mode still defines the
transform origin, and transform math still acts on the selected NURBS data or
BRep components under their own geometry rules. Preserve Blender's orientation
selector, numeric input, modal constraints, gizmo, and custom-orientation UI;
add the NURBS frame calculation behind that shared interface.

## Shared transform pivot points

The screenshot's Transform Pivot Point selector is also part of Blender's
shared transform system. The selected `Median Point` setting applies to NURBS
transforms. The [Blender 5.2 Pivot Point manual](https://docs.blender.org/manual/en/5.2/editors/3dview/controls/pivot_point/index.html)
covers both Object Mode and Edit Mode. Blender calculates median and bounding
centers from transform elements, and uses the last-selected edit component for
Active Element; see `source/blender/editors/transform/transform_generics.cc`
and `source/blender/editors/object/object_utils.cc`.

| Pivot | Blender behavior | NURBS behavior to preserve |
|---|---|---|
| Bounding Box Center | Center of the world-axis-aligned selection bounds in Edit Mode. In Object Mode, bounds are around object origins, not object geometry. | In Object Mode, keep Blender's object-origin rule. In CV editing, bound the selected CV positions. In BRep editing, use exact selected component bounds or a documented tolerance; never let display tessellation density move the pivot. |
| 3D Cursor | Use the 3D Cursor location. | Reuse directly for objects, CVs, and BRep components. |
| Individual Origins | Rotate/scale each selected element around its own center; connected selected edge/face regions remain together. | In BRep mode, keep connected selected edge/face regions together as Blender does. In CV-point mode, each CV is its own point origin, so rotation and scale leave its position unchanged. If the editor adds connected control-net edge/face selection, apply the same connected-region rule there. |
| Median Point | Average the selected transform-element centers. In Edit Mode Blender's mesh median is count-based, so dense selections can pull it toward the area with more selected elements. | For CV editing, average selected CV positions with equal weight, not by curve length or surface area. For BRep editing, average stable model-space component anchors with equal weight. Do not use tessellation vertices or a surface-area centroid as a silent substitute. |
| Active Element | Use the last-selected component's center; in Object Mode use the active object's origin. | Use the active CV position in CV mode. In BRep mode, use the active topological component's stable center, and retain Blender's fallback to Median Point if no active component exists. |

These choices determine the transform origin, independently of the orientation
axes described above. In particular, the `Cursor` orientation points the axes
along the 3D Cursor's rotation, while the `3D Cursor` pivot uses its position;
either can be used without requiring the other. The shared pivot selector,
keyboard shortcut, gizmo placement, and modal transform behavior can be reused,
but its center calculation must consume NURBS component data rather than the
display mesh.

## Shared transform snapping panel

The screenshot is Blender's transform-snapping popover, not a mesh-only menu.
Its settings apply to Object and Edit transforms. See the [Blender 5.2 Snapping
manual](https://docs.blender.org/manual/en/5.2/editors/3dview/controls/snapping.html)
and the panel in `scripts/startup/bl_ui/space_view3d.py` (`VIEW3D_PT_snapping`).
These settings are separate from CAD drawing OSnaps: they determine how a
transform's source is matched to scene geometry or a grid.

### Snap Base

The base is the point on the moved selection that Blender tries to place on a
target. The labels map to `rna_enum_snap_source_items` in
`source/blender/makesrna/intern/rna_scene.cc`.

| Choice | Blender behavior | NURBS rule |
|---|---|---|
| Closest | Use the selected bounding-box corner (Object Mode) or selected vertex (Edit Mode) closest to the target. | Use exact bounds for an object; in CV mode use selected CVs. For BRep components find the closest point on the selected exact geometry, not its display mesh. |
| Center | Use the current transform pivot. | Share the pivot modes above, including 3D Cursor. |
| Median | Use the average of selected object origins or edit vertices. | Average the selected object origins or selected CVs with Blender's equal-element weighting; define stable BRep component anchors for topology mode. |
| Active | Use the active object's origin or active edit component's center. | Use the active object origin, active CV, or active BRep component center according to the editing domain. |

### Snap Target

The target is the scene point or geometry onto which the base snaps. Blender
allows multiple target kinds to be active together.

| Target | Blender behavior | NURBS rule |
|---|---|---|
| Increment | Move by viewport-grid-sized steps relative to the source's starting position. | Reuse the transform increment and precision modifiers; do not quantize stored knots or weights. |
| Grid | Snap to the visible viewport grid point nearest the cursor. | Reuse Blender's active drawing/work-plane grid and preserve full model precision after placement. |
| Vertex | Snap to the vertex nearest the cursor. | Expose BRep vertices as topology targets; expose CVs only when the CV selection/display domain is active. Keep these target types distinguishable. |
| Edge | Snap to the edge nearest the cursor. | Refine the hit against the exact trimmed NURBS edge curve. |
| Face | Project onto the face under the cursor. | Intersect the view ray with the exact support surface, then reject hits outside that face's trim loops. |
| Volume | Snap to the depth centered inside the target object under the cursor; it is not a Blender Volume object. | Require a closed NURBS solid and robust ray/shell intersections. Open curves, sheets, and open shells have no volume center. |
| Edge Center | Snap to the center of the edge nearest the cursor. | Compute the center on the exact trimmed curve (arc-length midpoint is the stable CAD interpretation), not the midpoint of a tessellation segment. |
| Edge Perpendicular | Snap to the edge point where the line from the source's original position is perpendicular to the edge. | Solve for perpendicular feet on the exact trimmed curve and keep only valid points within the edge domain. |
| Face Center | Snap to the center of the face under the cursor. | Define a stable representative point on the trimmed face that lies inside its trim region; do not use a tessellation-dependent center. |

### Snap Target for Individual Elements

These modes snap each moved object (Object Mode) or each moved vertex (Edit
Mode) separately. For a NURBS control net, that means per-CV movement; for a
BRep it means per selected topology element under the declared editing domain.

| Choice | Blender behavior | NURBS rule |
|---|---|---|
| Face Project | Project each element along the current view direction onto a face. | Ray-project each source element to a trimmed NURBS face and move that element using its declared support-deformation rule. |
| Face Nearest | Move each element to the nearest point on a face, including occluded faces. | Use a closest-point solve on the exact trimmed surface. Preserve `Snap to Same Target` and `Face Nearest Steps`; steps control intermediate projection along the transform, not surface tessellation quality. |

### Other settings in the popover

| Setting | Blender behavior and visibility | NURBS rule |
|---|---|---|
| Absolute Increment Snap | When Increment is active, align to the absolute grid using the pivot rather than stepping from the starting position. | Use the selected pivot as the grid anchor and retain precision in the model coordinates. |
| Snap Peel Object | Shown with Volume snapping. Treat the target as one whole object and use its outer depth envelope rather than internal/disconnected surfaces. | For a closed solid, use the outer shell for depth snapping; retain separate-shell and cavity semantics explicitly. |
| Snap to Same Target | Shown with Face Nearest. Keep snapping to the object that was nearest before the transform. | Preserve a stable target-object ID through the modal operation. |
| Face Nearest Steps | Shown with Face Nearest in Edit Mode. Break the transform into repeated closest-face steps. | Apply the same staged solve to exact surface/trim geometry; do not silently replace it with tessellation steps. |
| Align Rotation to Target | Align the selection's local Z axis with the target normal. | For whole-object rigid transforms, use the analytic target-face normal. Do not independently rotate CVs unless the transform mode explicitly asks for per-element orientation. |
| Backface Culling | Ignore back-facing target geometry. | Use oriented NURBS face normals and the same front/back test as the viewport. |
| Target Selection | In Edit Mode, choose whether to include the active object, other edited objects, and non-edited objects; Exclude Non-Selectable filters unselectable objects. Object Mode shows the applicable selectable filter. | Preserve those object-level filters, then route eligible target hits to exact NURBS geometry. Respect hidden and non-selectable components according to the scene selection rules. |
| Affect: Move / Rotate / Scale | Choose which transform kinds use snapping. | Reuse these toggles. Translation is a rigid support move for whole objects; component transforms follow their own geometry rules. |
| Rotation Increment | Set the normal and precision angular steps; Blender uses the precision step while the Shift precision modifier is held. | Reuse Blender's angle increments independently of geometric snap targets. |

The user-visible panel and transform controls can be reused, but mesh snap
queries cannot simply be pointed at a NURBS display mesh. Exact vertices,
trimmed-curve closest points, ray/surface intersections, trimmed-face closest
points, and oriented normals need NURBS/BRep providers. In the Blender fork,
extend Blender's existing transform and viewport snapping infrastructure with
those providers so CAD-style drawing OSnaps and transform snapping share one
target model instead of adding a second snap overlay.

## Proportional Editing falloff and radius

The screenshot is the falloff selector in Blender's shared Proportional Editing
popover. `Smooth` is selected and the displayed influence radius is `1 m`.
Proportional Editing works in Object and Edit Mode; it changes how much nearby
unselected transform elements move along with the selected elements. The
[Blender 5.2 Proportional Editing manual](https://docs.blender.org/manual/en/5.2/editors/3dview/controls/proportional_editing.html)
documents the controls and falloff types. The options are defined in
`source/blender/makesrna/intern/rna_scene.cc` and converted to per-element
transform weights by `calculatePropRatio` in
`source/blender/editors/transform/transform_generics.cc`.

| Falloff | NURBS transform behavior |
|---|---|
| Smooth | Smoothly ease the influence from full strength at selected elements to zero at the radius. |
| Sphere | Use Blender's spherical falloff profile. |
| Root | Use the square-root falloff profile. |
| Inverse Square | Use Blender's named inverse-square falloff profile. |
| Sharp | Use a squared falloff profile, concentrated near the selection. |
| Linear | Reduce influence linearly with distance. |
| Constant | Apply full strength to all affected elements inside the radius. |
| Random | Randomize the per-element influence within the radius. |
| Proportional Size | Set the world-space influence radius; the screenshot shows `1 m`. Blender also lets users adjust it during a transform. |

The controls and falloff curves can be reused for NURBS. The geometry they
affect must be explicit:

- In NURBS control-point mode, apply Blender's falloff weight to selected and
  nearby unselected CVs. The NURBS curve or surface then changes through its
  basis functions. The CV is a control handle, so the visible surface's motion
  will not equal the falloff curve point-for-point; degree, weights, and knot
  spacing affect the result.
- In BRep mode, use the same selected/unselected weighting idea on actual
  topological vertices. A moved vertex still requires the BRep deformation and
  adjacent-support policy; the falloff weight alone does not update edges,
  faces, or trims.
- In Object Mode, apply the influence to whole NURBS objects according to
  their transform centers, as Blender does for whole objects.
- `Connected Only` and `Projected from View` are related Edit Mode controls in
  the full popover, though not shown in this falloff list. For CVs, Connected
  Only follows the curve chain or the surface's U/V control-net graph. For
  BRep, it follows topological adjacency. Projected from View measures the
  influence distance in screen space and deliberately ignores depth.
- Blender's legacy curve transform path calculates Connected Only distance
  across each NURBS object's flattened control-point sequence. The CAD surface
  adapter should use explicit U/V neighbor links instead of copying that
  limitation for a two-dimensional control net.

Do not calculate influence from display-mesh vertices. Use world-space CV
positions for CV editing, topology-aware path distance for Connected Only,
and projected source positions for Projected from View. Preserve Blender's
modal radius adjustment and falloff selection. Sparse NURBS control nets may
produce broader or less mesh-like shape changes; exact local surface editing
would require a separate surface-point deformation or control-net refinement
design.

## Object type Selectability & Visibility

The screenshot is Blender's shared, per-viewport Object Type Visibility
popover. Its pointer icon controls whether a type can be selected in this
3D Viewport; its eye controls whether the type is displayed there. The settings
apply in all modes and are separate from each object's Outliner restrictions.
The [Blender 5.2 Object Type Visibility
manual](https://docs.blender.org/manual/en/5.2/editors/3dview/display/visibility.html)
also notes that type filtering applies to evaluated geometry, and that an
unselectable object can still be selected from another viewport or the Outliner.
The UI is `VIEW3D_PT_object_type_visibility` in
`scripts/startup/bl_ui/space_view3d.py`; its type-to-object mapping is defined
in `source/blender/makesrna/intern/rna_space_api.cc`.

| Listed type | Blender object type | NURBS relevance |
|---|---|---|
| Mesh | `OB_MESH` | Applies to real mesh objects. A NURBS object's display proxy must not make the authoritative NURBS object behave as a mesh for visibility or selection filtering. |
| Curve | `OB_CURVES_LEGACY` | Directly applies to legacy Curve objects, which include NURBS curves as well as Bezier and poly splines. |
| Surface | `OB_SURF` | Directly applies to Blender's NURBS Surface objects. |
| Meta | `OB_MBALL` | A separate geometry type; not NURBS. |
| Text | `OB_FONT` | A separate object type; not NURBS. |
| Hair Curves | `OB_CURVES` | A separate modern curves object type, used for hair/curves geometry; it is not the legacy Curve object that contains NURBS splines. |
| Point Cloud | `OB_POINTCLOUD` | A separate geometry type; not NURBS. |
| Volume | `OB_VOLUME` | A separate geometry type; not a NURBS solid. |
| Lattice | `OB_LATTICE` | A separate deformation-control object; not a NURBS control net. |
| Light | `OB_LAMP` | Scene object filter only; it does not alter NURBS geometry. |
| Light Probe | `OB_LIGHTPROBE` | Scene object filter only; it does not alter NURBS geometry. |
| Camera | `OB_CAMERA` | Scene object filter only; it does not alter NURBS geometry. |
| Speaker | `OB_SPEAKER` | Scene object filter only; it does not alter NURBS geometry. |

For NURBS editing, keep selection and visibility filters attached to the
logical NURBS object type: `Curve` for NURBS curves and `Surface` for NURBS
surfaces. If a future fork introduces a separate unified NURBS object type,
these per-viewport toggles must include it or map each NURBS subtype
consistently. A tessellated mesh used only to draw or accelerate picking must
not silently inherit the Mesh filter. The viewport renderer, object picking,
component picking, and transform-snap target collection must all respect the
same per-viewport visibility and selectability flags.

## Viewport Overlays

The screenshot is Blender 5.2's Viewport Overlays popover. The official
[Viewport Overlays manual](https://docs.blender.org/manual/en/5.2/editors/3dview/display/overlays.html)
describes these as display aids; the panel is defined by
`VIEW3D_PT_overlay_*` classes in `scripts/startup/bl_ui/space_view3d.py`.
The settings are not all geometry operations: some are scene guides, some
draw annotations for selected objects, and some inspect evaluated geometry.

| Screenshot control | What it displays | NURBS relevance and implementation rule |
|---|---|---|
| Grid, Floor, X/Y/Z Axes, Scale, Subdivisions | View-space/world-space reference grid and principal axes. The panel enables grid/scale controls according to the current view. | Directly useful while drawing or editing NURBS, but independent of NURBS data. Reuse viewport settings and coordinates. |
| 3D Cursor | The shared scene cursor. | Directly useful as a transform pivot, snap target, and drawing reference. It is scene state, not a point on a NURBS object. |
| General Info | View name, active/selected object information, and contextual status text. | Generic. NURBS-specific status should identify curve/surface degree, CV or component selection, and dimensions only when relevant. |
| Statistics | Object and geometry counters, selected by mode and data type. | Useful, but NURBS counters must say what they count: control vertices, spline spans, surface patches, trims, and/or evaluated display triangles. Do not label CV counts as mesh vertices or confuse rational weights with vertex-group weights. |
| Performance | Viewport evaluation, synchronization, and draw timing. | Generic; useful for seeing tessellation and interaction costs. It does not depend on NURBS editing semantics. |
| Extras | Auxiliary object displays such as cameras/lights and other non-surface helpers; Blender also routes some helper displays through this flag. | Not a NURBS feature. Keep only for auxiliary objects the CAD scene actually supports. If an app omits those object types, this toggle may have nothing useful to control. |
| Motion Paths | A path annotation for objects with stored animation paths. | Applies to an animated NURBS object's object transform if animation paths are supported. It does not display the motion of the curve's CVs or surface itself unless component animation has its own path feature. |
| Light Colors | Tints light-object outlines by their emitted color. | Only relevant if the scene supports Blender-style light objects; it does not change NURBS shading or material color. |
| Origins / Origins (All) | Selected object origins, or all object origins, respectively. | Applies to NURBS objects because they have object transforms/origins. Keep object origins distinct from CVs, curve anchors, surface parameter origins, and the 3D Cursor. |
| Relationship Lines | Dashed parent/constraint relationship annotations between objects. | Object-system display only; it does not affect NURBS geometry. Retain only if the application retains those relationship types. |
| Outline Selected | Outline around selected objects. | Applies to selected NURBS objects. Component selection still needs its own CV/edge/face highlight and must not be mistaken for an object outline. |
| Wireframe and Opacity | A line overlay on displayed geometry. Blender's overlay draw path has dedicated legacy Curve and Surface wire batches as well as a mesh path. | Useful for seeing an evaluated NURBS curve/surface wire display. It is not automatically the NURBS control net: provide the CV cage and U/V adjacency as a separate Edit Mode overlay. Keep evaluated display lines derived from the exact NURBS source and map picks back to source IDs. The opacity slider only affects this wire overlay. |
| Face Orientation | Front/back colors for rendered surface geometry; Blender draws this from the object's surface batch. | Useful on NURBS surfaces and solid faces, including open sheets. Derive orientation from NURBS surface derivatives (and oriented face data for solids), and display colors on evaluated surface triangles. The display tessellation must not become authoritative topology. Curves without a surface have no face orientation. |
| Viewer Node: Color Opacity / Attribute Text | Color and labels for data produced by the active Geometry Nodes Viewer; controls are gated by viewer state, and text also depends on the general text overlay. | Conditional debugging UI, not a general NURBS overlay. It can apply only if NURBS data is passed through an equivalent node/evaluation viewer and its attributes have well-defined NURBS or topology domains. |
| Motion Tracking | Camera reconstruction/tracking annotations in the 3D View. | Independent of NURBS geometry. Useful only if the CAD application includes camera tracking or reconstruction. |

The screenshot's generally reusable set for NURBS work is **Grid/Floor/Axes,
3D Cursor, General Info/Statistics/Performance, Origins, Outline Selected,
Wireframe, and Face Orientation**. Extras, Motion Paths, Light Colors,
Relationship Lines, Viewer Node, and Motion Tracking are conditional on other
scene or evaluation systems. “Reusable” means the UI/display behavior can be
shared; it does not mean every item alters or understands NURBS geometry.

Source checks for the geometry-dependent cases: Blender's overlay wireframe
renderer routes `OB_CURVES_LEGACY` and `OB_SURF` to curve/surface wire batches
in `source/blender/draw/engines/overlay/overlay_wireframe.hh`; the legacy
curve edit overlay separately draws the control handles and points in
`overlay_curve.hh`. Face Orientation draws an object's evaluated surface
batch in `overlay_facing.hh`. This confirms that a NURBS-aware editor needs
both a display-wire layer and a distinct editable control-net layer.

## Mesh Edit Options: Transform, Mirror, Auto Merge, and UVs

This screenshot is the Edit Mode Tool Options popover. Blender documents the
mesh-specific settings in the
[Tool Settings manual](https://docs.blender.org/manual/en/5.2/modeling/meshes/tools/tool_settings.html).
The panel is `VIEW3D_PT_tools_meshedit_options_transform` and its UV child in
`scripts/startup/bl_ui/space_view3d_toolbar.py`. These settings are not a
single NURBS feature: they cover transform-time shading/texture attributes,
mirrored component editing, topology welding/splitting, and UV seam unwrap.

| Screenshot control | Blender mesh behavior | NURBS/CAD applicability |
|---|---|---|
| Correct Face Attributes | During supported transforms, corrects face-corner data such as UVs and color attributes to reduce visible distortion. The implementation works on mesh loops/faces. | Conditional. Moving NURBS CVs does not by itself require “face attribute” correction: the surface's geometric U/V parameterization remains its own data. For BRep faces with texture attributes, provide a separate preservation policy when supports or boundaries deform. Do not confuse texture UVs with NURBS surface parameters or trim p-curves. |
| Keep Connected | Only active with Correct Face Attributes; merges/corrects attribute values across loops connected to the same mesh vertex. | A seam-aware attribute rule is needed for BRep corners. Preserve intentional UV/material discontinuities at seams and across separate surface charts. It has no direct role in an isolated NURBS CV edit. |
| Mirror X / Y / Z | Mirrors mesh component transforms across enabled axes using spatially or topologically matched counterparts. | Useful for symmetric CV and BRep editing. NURBS curves/surfaces need stable counterpart maps; a surface map must respect its U/V control-net structure, weights, knots, and any mirrored trims. Define the reflection frame (object/local or world) and center-plane behavior explicitly. |
| Topology Mirror | Finds mirrored mesh vertices from their connectivity when coordinates alone do not identify the pair. Requires a mirror axis. | Useful only when a corresponding NURBS/BRep graph can be defined. For a control net, use indexed curve order or U/V adjacency and verify mirrored degrees, knot structure, and weights as appropriate. For BRep, use graph correspondence. Do not guess correspondence from a tessellated display mesh. |
| Auto Merge | After an interactive transform, welds moved mesh vertices to nearby vertices within the threshold. | Applies to actual BRep vertices with an explicit weld policy. For NURBS curves/surfaces, coincident CVs are not automatically topological vertices; merging CVs may change the shape. A CAD equivalent should join/weld compatible endpoints and edges only after checking geometric coincidence, support compatibility, trims, and requested continuity. |
| Split Edges & Faces | At transform completion, detects intersections of transformed mesh edges, creates vertices, and splits affected edges/faces; Blender's manual describes it alongside Auto Merge. | A future BRep version can split actual boundary curves and trimmed faces at computed intersections. It must intersect exact NURBS geometry (or use a declared tolerance-managed solver). Crossing control-polygon segments or tessellation edges alone is not evidence that the represented NURBS geometry intersects. |
| Threshold | Distance limit for Auto Merge and the mesh auto-merge/split interaction; the screenshot shows 0.001 m. | Use model units and explicit tolerances. Keep endpoint welding tolerance separate from intersection/trim solver tolerances where their numerical needs differ; never let a display tessellation setting silently determine topology. |
| UVs: Live Unwrap | In this Viewport Tool Options panel, recomputes mesh UV unwrap when an edge's seam property changes. This differs from the UV Editor's Live Unwrap option. Blender's standard unwrap path is mesh-only: `ED_uvedit_test` in `source/blender/editors/uvedit/uvedit_ops.cc` rejects non-mesh objects. | Not directly applicable to NURBS geometric U/V. A NURBS surface already has a parameter domain; texture-UV unwrap across surface charts is a separate BlenderNurb feature. Add this toggle only after persistent NURBS texture UVs and seam editing exist. See the UV mapping section of `nurbs_mesh_editing_study.md`. |

The transferable behaviors are **mirror transforms** (with NURBS-aware
counterpart mapping) and **merge/split** for real BRep topology. Attribute
correction and Live Unwrap are conditional texture-mapping features. None of
these settings should merge or split raw NURBS control points as if they were
mesh vertices. Blender 5.2's legacy Curve/Surface data stores NURBS patches but
does not provide a general shared BRep topology graph; the topology-dependent
entries therefore require a Blender-native topology layer or explicit
extension.

The source confirms the boundary: `transform_convert_mesh.cc` initializes
face-attribute correction for mesh loops; `EDBM_automerge` and
`EDBM_automerge_and_split` in `editmesh_automerge.cc` operate on BMesh vertices
and edges; and `use_edge_path_live_unwrap` is specifically described in RNA as
recalculating UV unwrap when edge seams change. Keep its behavior separate
from the UV Editor's pinned-island Live Unwrap described in the
[UV Editing manual](https://docs.blender.org/manual/en/5.2/modeling/meshes/uv/editing.html).

## Mesh Edit Mode Overlays

The screenshot is the Edit Mode section of Viewport Overlays. Blender's
[overlay manual](https://docs.blender.org/manual/en/5.2/editors/3dview/display/overlays.html)
states that this panel is mesh-edit-only and describes the display meaning of
each setting. Blender has a separate Curve Edit Mode overlay panel for
handles and curve normals; the mesh panel's labels do not all transfer
literally to NURBS.

| Screenshot control | Blender mesh meaning | NURBS/CAD behavior to provide |
|---|---|---|
| Bevel | Displays per-edge weights consumed by the Bevel Modifier. | On BRep edges, a corresponding display attribute could mark edges that feed a fillet/bevel operation. It is metadata/input to a CAD blend, not a fillet itself. NURBS control-net segments are not BRep edges. Keep this separate from rational CV weights. |
| Crease | Displays edge crease values for the Subdivision Surface Modifier. | Subdivision crease is not NURBS smoothness. NURBS shape/continuity is determined by degree, knots, weights, and CVs. A CAD editor could mark a desired continuity boundary, but must label it as a constraint/annotation and solve it explicitly rather than copy a mesh crease value into knots. |
| Seam | Displays mesh edges marked as UV unwrap seams. | Useful for texture-map chart boundaries on BRep edges if texture UV mapping exists. It is distinct from a NURBS surface's periodic parameter seam and from its trim curves; those need their own indicators. |
| Sharp | Displays edges marked sharp for mesh shading/Edge Split behavior. | A BRep edge may carry a shading discontinuity or separate oriented faces, so a marker can be useful. Keep it separate from actual G0/G1/G2 continuity. A single untrimmed NURBS surface has no internal BRep edge to mark. |
| Faces | Highlights selected mesh faces. | Highlight selected trimmed NURBS/BRep faces. A standalone NURBS surface can be selected as a whole; sub-face editing requires explicit trim/topology regions. |
| Center | Shows face-center points; Blender limits this to face selection mode and solid/wire display rules. | A trimmed face may show a computed surface-area centroid or another explicitly named representative point. Do not call the UV-domain midpoint a geometric centroid; it can lie outside a trimmed or concave face. |
| Indices | Displays indices of selected mesh vertices, edges, and faces. | Display stable CV, curve, trim, edge, and face IDs when available. Do not show tessellation-vertex or triangle indices as model IDs; those change with display resolution. |
| Retopology and Offset | Hides/offsets the edit mesh overlay so it can be seen against a sculpted reference. Offset is a display-depth aid. | Useful when a NURBS surface is a retopology/reference target or when edit overlays need depth separation. Apply offset in drawing only; never move the authoritative NURBS geometry. It is not a NURBS topology operation. |
| Vertex Group Weights | Colors mesh surface/vertices using the active vertex group's deformation weights. | Relevant only if NURBS CV deformation groups or equivalent attributes exist. A surface field may be interpolated from CV values for display. Rational NURBS weights are basis weights and must never be displayed or edited as deformation-group weights. |
| Mesh Analysis: Overhang | Colors mesh faces according to face-normal angle relative to a selected axis; used for printability checks. | Useful on NURBS surfaces using analytic normals from surface derivatives, sampled adaptively and evaluated in world space. This is a real NURBS diagnostic, not a mesh-only meaning. |
| Mesh Analysis: Thickness | Uses ray samples through mesh surfaces to find wall thickness. | Useful for closed NURBS solids with exact/tolerance-managed surface ray intersections. An open NURBS sheet has no defined wall thickness without a separate thickness/offset model. |
| Mesh Analysis: Intersect | Marks intersecting mesh geometry. | Useful for detecting self-intersections or overlapping NURBS/BRep faces, but requires a robust surface-intersection solver and a stated tolerance. Display tessellation may visualize results; it cannot define the authoritative intersection. |
| Mesh Analysis: Distortion | Flags non-planar mesh faces whose triangulation points in different directions. | This measures polygon face distortion and has no direct meaning on an exact NURBS support. Offer a separately named tessellation-error or surface-quality analysis instead of running this on proxy triangles. |
| Mesh Analysis: Sharp | Highlights mesh edges whose adjacent-face angle is within a range, for printability/thin-feature review. | Can be adapted to BRep dihedral angles. For a continuous NURBS patch, use curvature or minimum-feature analysis with a separate name and definition. |
| Measurement: Edge Length | Displays selected mesh-edge lengths. | Display exact line length where analytic, otherwise numerically integrate the NURBS boundary curve to a stated tolerance. Apply CAD units. A control-net segment length is not the represented curve length. |
| Measurement: Edge Angle | Displays the angle between the two faces adjacent to a selected mesh edge. | Applies to a shared BRep edge with two incident faces; calculate from the oriented surface normals along that edge. It has no meaning for an isolated curve or an edge without two incident faces. |
| Measurement: Face Area | Displays selected mesh-face area. | Compute area on the exact trimmed NURBS support, with numerical integration/error tolerance where needed. Do not report the area of the display triangles as exact CAD area. |
| Measurement: Face Angle | Displays the angle at selected mesh face corners. | For BRep topology, measure the angle between incident boundary tangents in 3D at a topological corner. Define behavior at smooth or multi-edge vertices. No generic face-corner angle exists on a standalone untrimmed surface. |
| Normals: Vertex / Face / Split | Displays mesh vertex normals, face normals, and per-corner split normals. | Show analytic NURBS surface normals at requested samples and oriented BRep face normals. Treat shading/split normals as display data separate from geometric normals. Curve normals require a curve-frame convention and belong with curve-specific overlays. |
| Freestyle: Edge Marks / Face Marks | Displays edge/face marks consumed by Blender's Freestyle Line Art renderer. | Applies only if the CAD app retains that renderer and can attach these marks to real BRep edges/faces. It is a render annotation, not geometry or topology. Otherwise omit it. |

The controls with direct NURBS editing value are **Faces, stable Indices,
Normals, Measurements, and the appropriate geometric Mesh Analysis modes**.
Bevel, seam, sharp, and weight displays require corresponding BRep or attribute
systems. Crease and Distortion keep mesh-specific meanings. Retopology is a
reference-display aid. This same screen area must distinguish NURBS control
points from topological vertices and all tessellation-derived display data.

The source implementation reflects those boundaries: the controls live in
`VIEW3D_PT_overlay_edit_mesh*` in `scripts/startup/bl_ui/space_view3d.py`;
mesh overlays and normal/weight/analysis batches are prepared in
`source/blender/draw/engines/overlay/overlay_mesh.hh`; and measurements are
generated from BMesh faces/edges in `source/blender/draw/intern/draw_manager_text.cc`.
The analysis modes in
`source/blender/draw/intern/mesh_extractors/extract_mesh_vbo_mesh_analysis.cc`
use mesh face/triangle data, so porting their visual colors alone would not
produce valid NURBS measurements.

## Audit correction

The original 329-row inventory covered the mesh registrations but did not
include every shared operator reached from this menu. The main-menu crosswalk
added 18 rows: two edit-coordinate transform helpers, seven View3D snapping
actions, and nine vertex-group weight actions. The Vertex context menu review
added five vertex-group management operators and six hook operators. The
updated inventory now has 358 rows, with matching IDs and order between the
inventory and parity map.
