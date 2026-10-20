# Mesh editing operator inventory scope

Static source inventory of Blender Fork mesh editing, used as a UX and capability reference for an exact NURBS design study.

## Coverage and counts

- `source/blender/editors/mesh/mesh_ops.cc`: 148 direct `MESH_OT_*` registrations and 16 macro registrations. Macros are choreography wrappers; child operators define geometry.
- Python definitions under `scripts/startup/bl_operators`: 3 `mesh.*` classes: `mesh.primitive_torus_add`, `mesh.select_next_item`, and `mesh.select_prev_item`. The latter two are BMesh ordered-selection helpers. The torus helper builds a polygon mesh.
- Additional Python viewport wrappers in the same tree: 4 `view3d.edit_mesh_extrude_*` classes (individual_move, move_normal, move_shrink_fatten, manifold_normal). They provide mesh extrusion UX and call shared transform/mesh operators.
- One adjacent Python helper, `view3d.transform_gizmo_set`, is included as a boundary row; it is not a mesh geometry operator. Thus there are 8 Python rows total: 3 `mesh.*`, 4 extrusion wrappers, and this helper.
- `source/blender/bmesh/intern/bmesh_opdefines.cc`: 83 `BMOpDefine` entries. CSV records each name, definition line, and executor location where the corresponding implementation was found; `solidify` maps to `bmo_solidify_face_region_exec`. BMesh operators are implementation primitives, not necessarily individually exposed UI actions.
- UV editor coverage: 49 direct registrations and 1 macro in `source/blender/editors/uvedit/uvedit_ops.cc`, included because UV editing is mesh-corner parameter data work.
- The transform mode table in `source/blender/editors/transform/transform_ops.cc` has 19 tmode implementations (translate, rotate, to-sphere, resize, skin resize, shear, bend, shrink/fatten, push/pull, tilt, trackball, mirror, edge slide, vertex slide, edge crease, vertex crease, edge bevel weight, sequence slide, rotate-normal). `transform_operatortypes()` independently registers every tmode opfunc in a loop at lines 1477–1478. The CSV includes all 19 plus the five explicit registrations after the loop: `TRANSFORM_OT_transform`, `select_orientation`, `create_orientation`, `delete_orientation`, and `from_gizmo`. The generic `TRANSFORM_OT_transform` also routes broader transform modes. Their modal interaction is shared across geometry domains; their targets differ. `seq_slide` is included as a source-boundary row even though it is specific to the sequence editor.
- The visible Mesh Edit menu (`scripts/startup/bl_ui/space_view3d.py`, `VIEW3D_MT_edit_mesh`) invokes shared operators registered elsewhere. Added its two edit-coordinate helpers (`TRANSFORM_OT_vertex_warp`, `TRANSFORM_OT_vertex_random`), all seven fixed View3D Snap operator IDs, and all nine vertex-group weight operators shown in its Edit Mode Weights submenu. These are included because parity covers the menu the user sees, not just `mesh_ops.cc` registrations.
- The separate Vertex context menu in the same UI file exposes five vertex-group management actions and six Hook modifier actions. These are included as well; the hook rows describe NURBS-aware alternatives, not existing Blender NURBS support.
- The Edge context menu exposes 17 unique operator IDs, all already covered by the inventory. Its Freestyle edge marker is conditional on `WITH_FREESTYLE`; its visible menu item is hidden when that build option is off.
- The Face context menu and its Face Data submenu expose 23 unique fixed operator IDs, all already covered by the inventory. Freestyle face metadata is also conditional on `WITH_FREESTYLE`; dynamic Geometry Nodes assets are outside the fixed list.
- The Mesh menu's `template_node_operator_asset_menu_items` entries are dynamic operator assets; their contents depend on installed assets and cannot be enumerated as fixed built-ins in this source inventory. The menu also conditionally shows Convex Hull when Bullet is enabled.

## Source boundaries

CSV rows distinguish registration or definition location from implementation location. An empty implementation field means the source scan did not find a direct name match; it does not mean the operator lacks an implementation. Transform-mode rows point to their definitions; the registration loop is documented above. The generic transform operator also routes mode enums, and macro rows list child calls. Properties and poll/context fields are static source observations, not a full RNA/runtime evaluation. Conditional guards report only obvious local guards and do not establish that an operator is enabled in a given runtime build.

The inventory is not a runtime-enabled operator census. It does not enumerate every add-on, extension, build configuration, context poll result, menu binding, or all non-mesh namespace operators. UI references can point to Python registrations or operators defined elsewhere. Direct registrations are not equivalent to visible menu entries.

## Semantic reading for NURBS planning

Selection, modal placement, snapping, numeric input, axis constraints, gizmos, and transform choreography are UX patterns that can be adapted to NURBS objects and control data. They do not imply that BMesh data structures or algorithms should be reused.

Topology operations based on edge/face incidence can share combinatorial patterns through a CAD-aware topology API; their geometric predicates and construction need a specified NURBS operation. A tessellated display proxy is not authoritative geometry. Exact constructions, support-preserving topology changes, tolerance-managed fitting, and explicit conversion are distinct policies. Other operations such as transforms, duplication, rotational constructions, bevel-like offsets, surface thickening, or selection by geometric criteria need an intentional NURBS definition; this inventory does not claim they are mathematically impossible.

## Spin, Screw, and triangulation notes

- `MESH_OT_spin` passes selected vertices/edges/faces to BMesh `spin`, with integer `steps`, total angle, axis, center, optional displacement vector, duplicate/extrude mode, normal flip, and conditional auto-merge. The kernel repeatedly duplicates or extrudes and rotates the mesh per step. This is a discrete mesh construction UX, not an analytic surface of revolution contract.
- `MESH_OT_screw` finds two selected endpoints of a selected edge chain, derives an axial displacement per step, then invokes the same discrete `spin` kernel for `turns * steps`. It has integer step/turn controls. This creates sampled mesh geometry, not an exact helical NURBS sweep.
- Quad-to-triangle conversion delegates to BMesh triangulation. Quad diagonal modes are beauty, fixed, alternate, short-edge, and long-edge; n-gons use beauty or ear clipping. The chosen diagonal partitions a polygonal face for mesh topology/display; it does not turn a nonplanar quad into a mathematically planar surface, and choice changes its piecewise-linear realization.
- Shared transform source uses floating-point `TransDataBasic.loc` at `source/blender/editors/transform/transform.hh:387` and `NumInput.val` at `source/blender/editors/include/ED_numinput.hh:36`; this is source-level evidence of Blender's current numeric representation, not a desired precision ceiling for classiCAD.

## Main source paths

- Mesh registrations/macros/keymap: `source/blender/editors/mesh/mesh_ops.cc`
- Mesh operator implementations: `source/blender/editors/mesh/`
- BMesh operators: `source/blender/bmesh/intern/bmesh_opdefines.cc`, `source/blender/bmesh/operators/`
- Python mesh and view3d wrappers: `scripts/startup/bl_operators/mesh.py`, `add_mesh_torus.py`, and `view3d.py`
- Mesh UI and contextual menus: `scripts/startup/bl_ui/space_view3d.py`, `space_toolsystem_toolbar.py`, `properties_data_mesh.py`
- UV operators: `source/blender/editors/uvedit/uvedit_ops.cc`
- Generic transforms: `source/blender/editors/transform/transform_ops.cc`
