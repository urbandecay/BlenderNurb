# BlenderNurb architecture feature-pruning review

## Purpose and scope

This is a decision document for an architecture-focused Blender fork. For each
major Blender feature family it gives the strongest practical case for keeping
it and the strongest case for removing, hiding, or deferring it. It does not
choose for the user.

Working assumption: the product is for architectural modeling and design, with
precise curves/surfaces, building and site geometry, scene organization,
materials, views, and presentation. Whether it must also be a BIM authoring
tool, a drafting/documentation tool, or a full animation and rendering suite
has not been decided. Where that difference changes the argument, both cases
are stated.

Source reviewed: the local Blender 5.2.2 fork at `e694c2908bb` (the branch
contains several feature-removal commits after the 5.2.2 release). This is a
source review, not a build or runtime test. “Remove” can mean several different
things: hide a workflow from the default UI, stop shipping a specialist
editor/add-on, turn off an existing build option, or delete the data/runtime
implementation. Those choices have very different costs.

This covers Blender's major built-in application and source-module families.
The detailed mesh operation audit remains in
[`nurbs_mesh_editing_study.md`](nurbs_mesh_editing_study.md) and its linked
operator inventory. Blender add-ons, downloadable node assets, scripts, and
every possible RNA setting make a literal list of every user-installable
feature open-ended; they are called out as extension points rather than
pretended to be a finite built-in list.

## Features already removed on this fork

These are current branch facts, not new proposed cuts. The review still gives
both sides so the user can decide whether they belong in an architecture tool.

| Feature already removed | Current fork history | Strongest case to keep it | Strongest case to leave it removed |
|---|---|---|---|
| Grease Pencil | Commit `0126d791109`; user decision: leave removed | Viewport sketches, markups, review comments, and design annotations can be useful to architects. A smaller annotation layer could serve this without all 2D/3D drawing and animation tools. | Full Grease Pencil has its own data, tools, drawing, sculpting, animation, nodes, and file support. Precise CAD markup can be a later, narrower feature if sketching is not a core workflow. The user wants SketchUp-style sketchy edge rendering considered separately; it is not Grease Pencil. |
| Texture Paint | Commit `cc34cf4c71c`; user decision: leave removed | Architects use painted textures, masks, and touch-ups for materials and presentation. Keeping texture images and materials alone does not give users direct paint workflows. | Texture painting is a separate brush, image, cache, and undo stack. If users author materials elsewhere, the paint mode and its UI can stay out. Keep image/material data and UV mapping decisions separate from the paint tool decision. |
| Object parenting | Commit `c9b1755e202`; user decision: leave removed, using planned nested objects | Parent-child transforms are useful for assemblies: group a window's frame and sash, move a door assembly, place repeated furniture, or attach a camera/light rig. A building hierarchy can reflect floors, rooms, systems, and components. | Blender parenting semantics add transform inheritance, inverse matrices, cycles, parenting modes, file compatibility, and UI complexity. The user plans to nest objects inside objects, so the product will use that containment model instead of restoring Blender parenting. |
| Armatures and rigging | Commit `e694c2908bb`; user decision: leave removed | Armatures can animate people, shading devices, or other articulated assets in walkthroughs and presentations. | Character posing and bone deformation are not central to architectural geometry. Building components can be animated independently or through the planned nested-object structure. |
| Empty objects / generic locators | Commit `e694c2908bb`; user decision: leave removed | Locators can mark axes, reference positions, camera targets, survey points, or construction guides without adding visible geometry. Reference images and measurement/datum markers can help architectural modeling. | A generic invisible object adds another type to select, organize, serialize, and explain. The user prefers keeping empties removed; purpose-built datum/reference tools can be considered separately if needed. |

The removal commits are broad source changes, not just menu edits. For example,
the armature/rigging removal changes DNA/RNA, evaluation, modifiers, import and
export paths, UI, tests, and file handling. A future decision to restore any
one of these should first define the narrow user workflow to restore.

## Core application and shared editing

| Feature family | Strongest case to keep | Strongest case to remove or reduce | Main source areas / coupling |
|---|---|---|---|
| Application shell, window manager, screens, events, keymaps | It is the host that provides the viewport, modal tools, keyboard/mouse input, dialogs, workspaces, and Blender's editing feel. Keeping it avoids rebuilding the interaction framework. | If the product only needs a geometry library or batch converter, a full interactive application is unnecessary. For an interactive architecture tool, the practical reduction is a simpler default workspace and fewer exposed editors. | `source/blender/windowmanager/`, `editors/screen/`, `editors/space_api/`, `makesdna/`, `makesrna/` |
| Viewport navigation, selection, transforms, pivots, orientations, snapping, undo/redo | These are the shared foundation for every modeling workflow. Blender's G/R/S, numeric constraints, selection tools, pivots, and undo behavior are central to the requested mesh-like NURBS editing. | Rarely used navigation modes and domain-specific tools can be hidden or simplified. Removing the shared transform/selection/undo systems would remove the interaction foundation along with unwanted features. | `editors/space_view3d/`, `editors/transform/`, `editors/object/`, `editors/undo/`, `windowmanager/` |
| Dependency graph and evaluated data | It updates transforms, modifiers, constraints, animation, procedural geometry, and render data. A Blender-based architecture tool needs a reliable evaluation graph. | A simple static geometry editor could avoid some evaluation systems, but then it would no longer support Blender's non-destructive stack or much of its scene workflow. | `source/blender/depsgraph/`, `blenkernel/`, `geometry/`, `modifiers/` |
| Scene, objects, collections, and Outliner | Architecture needs scene organization for buildings, floors, rooms, components, context/site data, visibility, and reusable objects. Collections and an Outliner are immediately useful. | Blender's general data hierarchy supports many non-architecture types and can feel complex. A focused layer/component tree could be easier if it preserves visibility, selection, instancing, and organization. | `DNA_object_types.h`, `DNA_collection_types.h`, `editors/space_outliner/`, `asset_system/`, `blenkernel/` |
| Object parenting and assembly relationships | Parenting can group and place building assemblies and is familiar to Blender users. It can help with repeated components and animated assemblies. | Parent transforms can be confusing for CAD precision and are not the same as a constraint-based assembly. A lean product could use explicit assembly/group objects or CAD constraints instead. **Parenting is already removed on this branch.** | `DNA_object_types.h`, `blenkernel/intern/object.cc`, `depsgraph/`, transform/object editors, file versioning |
| Workspaces, Properties editor, menus and context panels | Users can adapt Blender for modeling, drafting, materials, and presentation; context panels expose many adjustable modeling settings. | The default UI includes many irrelevant workspaces and panels. Hiding or removing them from the shipped workspace set can greatly simplify the product without deleting the underlying data system. | `scripts/startup/bl_ui/`, `editors/space_buttons/`, `editors/space_topbar/`, workspace/screen data |
| Units, scene scale, measurement and annotation | Real units, precise placement, dimensions, area/length measurements, and scale are fundamental to architecture. | Blender's unit and measure tools may not provide a complete CAD dimensioning/drafting system. Keeping them as-is could imply capabilities they do not have; a dedicated dimension system might be clearer. | `DNA_scene_types.h`, `editors/space_view3d/`, `editors/transform/`, overlay and tool settings |
| File persistence, undo history, data IDs, and versioning | Projects need stable save/reopen, linked assets, recovery, undo, and years-long compatibility. New NURBS/BRep data depends on these foundations. | A throwaway prototype or single-purpose converter could use simpler storage. That is not a good fit for a production architectural model with many dependent components. | `source/blender/blenloader/`, `makesdna/`, `blenkernel/`, `editors/undo/` |

## Geometry, modeling, and data types

Decisions recorded during the one-by-one review:

- Keep lattice deformation.
- Keep point clouds and scan data for potential photogrammetry and site/building references.
- Keep volumes and volumetric objects.
- Keep Blender object constraints.
- Keep mesh sculpting and remeshing.
- Keep Blender's modifier system and all currently registered modifier types.
- Keep the Subdivision Surface modifier for mesh objects.
- Keep the Curve modifier.
- Keep the Build modifier.
- Keep the Mirror modifier.
- Keep all node systems, including Geometry Nodes, shader nodes, and compositor nodes.
- Keep generic attributes and vertex groups.
- Keep UV and texture mapping.
- Keep Cycles and advanced render support.
- Remove Weight Paint only; vertex/color painting and attribute painting remain undecided.
- Remove Speaker objects and their dedicated audio object workflow.

| Feature family | Strongest case to keep | Strongest case to remove or reduce | Main source areas / coupling |
|---|---|---|---|
| Mesh objects, Edit Mode, BMesh, and mesh tools — user decision: keep | Useful for site/context models, faceted parts, scans, imported assets, visualization, and explicit polygon work. Mesh behavior is also the requested interaction baseline for NURBS. | A NURBS/BRep-first product may not want two equally authoritative modeling systems. The mesh tool catalog brings a large topology, attribute, modifier, and validation surface. Mesh can be secondary if CAD geometry is the main product. | `makesdna/DNA_mesh_types.h`, `bmesh/`, `editors/mesh/`, mesh operators and modifier stack |
| Legacy Curve and Surface objects, including NURBS — user decision: replace the current NURBS editing workflow | NURBS curves and surfaces remain essential geometry for architectural design. Existing evaluation and geometry routines may still be useful where they fit. | The current NURBS editing workflow is to be replaced by the mesh-like editing workflow, not retained as a second mode. Legacy curves combine old spline behavior and still do not provide a complete trimmed BRep/solid model. | `makesdna/DNA_curve_types.h`, `editors/curve/`, `blenkernel/intern/curve.cc`; replacing edit behavior does not by itself decide whether to retain, adapt, or replace data storage and evaluation code. |
| Modern CurvesGeometry — user decision: keep for Geometry Nodes | Useful for large curve sets, procedural path geometry, strand-like content, and Geometry Nodes interoperability. | It is designed for one-parameter curves, not tensor-product NURBS surfaces, shared trimmed faces, or solid topology. Keeping two overlapping curve systems can create duplicate behavior and data conversion work. | It is a separate data type that Geometry Nodes supports. `blenkernel/BKE_curves.hh`, `blenkernel/intern/curves_geometry.cc`, `editors/curves/`, `nodes/geometry/` |
| CAD surface and BRep topology (capability to add/strengthen) | Essential for exact trimmed surfaces, shared edges, face topology, solids, booleans, and mesh-like editing without making tessellation authoritative. | A general-purpose BRep kernel is a substantial project. It can be staged or limited to the exact architectural operations needed instead of delaying all editing for full CAD coverage. | Existing legacy surfaces in `DNA_curve_types.h`; geometry services in `source/blender/geometry/`; mesh topology in `bmesh/` is not itself a NURBS BRep. |
| Text and font objects — user decision: keep | Useful for room labels, signage, dimensions, title blocks, presentation, and model geometry such as house numbers. Text can remain editable and convert to geometry when needed. | Blender text is not a full annotation/dimension system. If all text is supplied through a dedicated drafting layer, a second text object model may be unnecessary. | `editors/curve/`, font data and `source/blender/blenfont/` |
| Metaballs / implicit surfaces | Can support quick organic massing, landscape forms, and conceptual studies. | They are a separate implicit geometry system with conversion and editing behavior unrelated to exact architectural surfaces. They can be deferred if the tool prioritizes precise explicit geometry. | `editors/metaball/`, `blenkernel/intern/mball*`, object type and evaluation |
| Lattice deformation — user decision: keep | A flexible cage can adjust furniture, landscape objects, or massing while preserving the source object. | It is a generic deformation cage, not an architectural constraint or parametric feature. | `editors/lattice/`, lattice modifier and object capability registration |
| Point clouds and scan data | Existing buildings and sites often arrive as scans. Point clouds can provide context and reconstruction references without immediately converting to a solid model. | Point clouds are not editable exact boundaries and add selection, storage, rendering, and export requirements. A read-only reference or external scan viewer could suffice. | `editors/pointcloud/`, `DNA_object_types.h`, geometry set, import/export |
| Volumes and volumetric objects | Can represent fog, smoke, environmental visualization, and some scanned/derived data. | Usually not core to building geometry. If no volumetric visualization or simulation workflow is intended, the object type, rendering, and volume I/O may be removed or made optional. | `editors/`, `blenkernel/`, `io/`, OpenVDB/NanoVDB, volume modifiers and render paths |
| Cameras, lights, light probes, speakers, and locators/empties | Cameras and lights support plans, elevations, perspective views, daylight/material studies, and presentations. Probes can help preview lighting. Speakers support walkthrough video/audio. Locators are useful for datum/reference positions. | A geometry-only tool could export to a renderer. Advanced light-probe, audio, stereo, or locator workflows can be reduced if they are not used. **Empties are already removed on this branch.** | `makesdna/DNA_object_types.h`, camera/light IDs, `render/`, `draw/`, object editors; empties removed in `e694c2908bb` |
| Armatures, bones, pose mode, and rigging | Could animate doors, blinds, movable structures, people, or transform studies. Rigging also enables deformation workflows for non-building assets. | Character rigging and pose tools are a large specialized workflow outside static architecture. If animation is out of scope, leaving it removed avoids a wide set of data, modifiers, editors, file, and interchange paths. **Already removed on this branch.** | `editors/armature/`, armature DNA/RNA, animation, modifiers, and interchange; removed in `e694c2908bb` |
| Object and CAD constraints | Blender object constraints can place/track objects and drive animation. CAD constraints could enforce dimensions, coincidence, tangency, alignment, and symmetry while designing. | Blender's existing constraints are not a sketch/BRep constraint solver. Retaining all object constraints adds UI and animation coupling; adding parametric CAD constraints is a separate solver project. | `editors/object/`, `blenkernel/`, `makesrna/`, depsgraph; CAD solver is not provided by generic mesh constraints |
| Mesh sculpting and remeshing | Useful for terrain, landscape, organic concept massing, and sculpted context assets. | Sculpt brushes, dynamic topology, remeshing, multires, masks, and caches are not precise NURBS edits. They can remain a mesh-only optional workflow or be removed from a precise CAD product. | `editors/sculpt_paint/mesh/`, `blenkernel/intern/sculpt*`, PBVH, remesh and undo |
| Paint modes: texture, vertex/color, weight, and attribute painting | Texture and color painting can create architectural materials and masks; vertex attributes can encode materials or other design data. | Brush systems are substantial and not needed by users who author materials externally. Weight painting is less relevant after armature removal, though it can still feed modifiers or procedural systems. Texture Paint is already removed on this branch; that does not by itself imply removing image, material, texture, or UV data. | `editors/sculpt_paint/`, `editors/space_image/`, brush/attribute APIs; Texture Paint removed in `cc34cf4c71c` |
| Grease Pencil and viewport annotation | Useful for sketch overlays, review notes, markup, and drawing alternatives over a model. | Full Grease Pencil brings drawing, sculpting, animation, node, and data-model complexity. A static architecture tool may use a smaller annotation/dimension feature instead. **Already removed on this branch.** | `editors/grease_pencil/`, drawing data, nodes, animation; removed in `0126d791109` |
| Modifiers (all 58 registered types in this checkout) | Non-destructive modeling is valuable for arrays, mirrors, bevels, booleans, thickness, repetition, and procedural design. The stack can preserve editable source geometry. | Many modifiers are mesh-specific, game/character-focused, or create derived meshes/volumes. A full menu implies broad support the NURBS model may not yet provide. Keep only explicitly supported types visible if scope is narrow. | `modifiers/intern/MOD_util.cc`, `MOD_modifiertypes.hh`, `makesrna/intern/rna_modifier.cc`; see the per-type roster in [`adjacent_mesh_capabilities.md`](nurbs_mesh_study/adjacent_mesh_capabilities.md) |
| Geometry Nodes | Strong for procedural façades, arrays, repetitive components, site layouts, and user-made tools. A node system is a powerful way to build architectural generators without a fixed operator for every task. | It is a major system with a large node catalog and mesh-oriented assumptions. Full exact NURBS/BRep node support is not automatic; if users only need direct modeling, it can be hidden or deferred. | `nodes/geometry/`, `editors/space_node/`, `blenkernel/BKE_geometry_set.hh`, `modifiers/MOD_nodes.*` |
| Generic attributes and vertex groups | Useful for material IDs, component metadata, schedules, procedural selection, and exchange data. Named attributes can bridge NURBS, meshes, and node workflows. | Mesh domains such as corners/loops do not automatically describe CAD faces, trims, control points, and edges. Keeping all mesh-specific tools before defining CAD domains risks misleading results. | `blenkernel/BKE_attribute.hh`, `makesdna/DNA_meshdata_types.h`, mesh/curve attributes, node fields |
| UV and texture mapping | Textures are important to architectural appearance; UV maps help export and material authoring. NURBS texture mapping can be built on top of surface parameters while preserving separate texture coordinates. | Mesh unwrap and painting are not the same as intrinsic NURBS `(u,v)` or trim curves. A dedicated UV editor is a lot of UI if the tool only uses procedural/material coordinates. | `editors/uvedit/`, `editors/space_image/`, mesh corner attributes; existing study explains NURBS parameter versus texture UV distinction |
| Materials and shader nodes | Materials support finish selection, design presentation, client review, and accurate render output. Shader nodes provide flexible custom materials. | A modeling-first tool can begin with a small material library and omit a general shader authoring system. Shader UI is a large domain if appearance is out of scope. | Material IDs, `nodes/shader/`, `editors/space_node/`, draw/render systems |

## Presentation, simulation, and exchange

| Feature family | Strongest case to keep | Strongest case to remove or reduce | Main source areas / coupling |
|---|---|---|---|
| Viewport display and Workbench/Eevee | Needed for immediate geometry feedback, selection, shaded/material previews, and technical review. Workbench can provide a fast architectural presentation mode without the cost of a path tracer. | Advanced real-time effects can be reduced if this application is chiefly a geometry editor. The viewport itself and a useful display renderer are not good removal candidates. | `draw/`, GPU, Workbench/Eevee engines, `editors/space_view3d/` |
| Cycles and advanced render backends | Photoreal stills, interiors, shadows, indirect lighting, and client-facing images are valuable architecture workflows. | Cycles, OSL, path guiding, and multi-vendor GPU backends add large dependencies and build/support costs if rendering is external or unnecessary. | `intern/cycles/`, `WITH_CYCLES`, device flags in root `CMakeLists.txt` |
| Render output, cameras, image buffers, color management | A reliable still render, image output, and consistent color management are useful even if only one render engine is shipped. | Exotic stereo/panorama modes, specialty output formats, and deep render controls can be trimmed. Removing render-result and color infrastructure is much riskier than disabling one engine. | `render/`, `DNA_camera_types.h`, `imbuf/`, OpenColorIO, image editor |
| Compositor | Useful for exposure, denoising, glare, alpha, color adjustment, and final presentation without leaving Blender. | VFX/keying-heavy node families may not serve architecture. A smaller post-processing set is possible; node infrastructure is shared with Geometry Nodes and should not be deleted solely for compositor pruning. | `compositor/`, `nodes/composite/`, `editors/space_node/`, render CMake dependencies |
| Freestyle and non-photoreal line rendering | Architectural line drawings, silhouettes, contours, and illustrative presentation can benefit from line-art rendering. | Remove if the target is only shaded/photoreal visualization; it is an optional rendering feature rather than a geometry foundation. | `source/blender/freestyle/`, `WITH_FREESTYLE` |
| Animation and timeline editors | Walkthroughs, camera moves, sun/time studies, phasing, animated design alternatives, and moving components can be valuable. Generic transform animation can serve architectural work without character animation. | Static modeling does not require the Action, Graph, NLA, or motion-path workflow. The UI can be pruned, but deleting animation data/evaluation affects many systems. | `animrig/`, `editors/animation/`, `space_action/`, `space_graph/`, `space_nla/`, depsgraph |
| Physics, particles, fluid, cloth, smoke, and simulation | Could support environmental visualization, wind/cloth tests, particles, crowds, and procedural animation. | Most general-purpose simulation is not a building-modeling or structural-analysis system. If those workflows are not part of the product, remove or disable the specialist tools. Do not equate Blender physics with validated structural engineering. | `editors/physics/`, `simulation/`, physics modifiers, `WITH_BULLET`, `WITH_MOD_FLUID`, `WITH_OPENVDB` |
| Video Sequencer and audio | Walkthroughs, turntables, presentation reels, and narration can be assembled in the same application. | If deliverables are models, drawings, and still images, video/audio editing is peripheral and has separate timeline, codec, cache, and UI systems. | `sequencer/`, `editors/space_sequencer/`, `editors/sound/`, `WITH_CODEC_FFMPEG`, `WITH_AUDASPACE` |
| Movie Clip Editor, tracking, masks, and motion capture | Camera tracking can place models into site footage; masks can support compositing and review. | This is primarily a VFX workflow and could be omitted if architects do not use video reference or matchmoving. | `editors/space_clip/`, tracking, mask editors, `WITH_LIBMV` |
| Image editor | Useful for reference boards, texture and UV work, render review, and image output. | Some paint/scopes/sequence features can be removed while retaining basic viewing, loading, and saving of images. | `editors/space_image/`, `imbuf/`, UV tools; image editor CMake integration is broad |
| Import/export formats | Interchange is essential for architectural collaboration and downstream visualization. Preserve `.blend` and prioritize formats by actual users; mesh exchange and exact CAD exchange serve different purposes. | Niche formats with no workflow owner increase dependency and maintenance burden. Mesh-only formats can lose NURBS/BRep accuracy, so shipping many generic formats may not solve CAD exchange. | `source/blender/io/`; options include `WITH_ALEMBIC`, `WITH_USD`, `WITH_IO_*`; built-in formats do not provide a full IFC/STEP/IGES CAD model in this checkout |
| USD, Alembic, FBX, OBJ, PLY, STL, glTF and related interchange | USD can support composed scenes; Alembic supports caches; OBJ/PLY/STL support common mesh handoffs; FBX/glTF support visualization pipelines. Architecture may need IFC, STEP, IGES, or DXF through separate integration. | Remove individual formats with no intended users. Alembic and some animation formats are less useful if animation/cache workflows are removed. STL/OBJ/PLY are tessellated exchange, not exact NURBS exchange. | `io/usd/`, `io/alembic/`, `io/fbx/`, `io/wavefront_obj/`, `io/ply/`, `io/stl/`; glTF lives under core add-ons |
| Asset Browser, libraries, linking, overrides | Architectural scenes reuse furniture, doors, windows, fixtures, materials, façade modules, and details. Reusable local libraries can improve large-project workflows. | Full library linking/override and asset-publishing systems have complex identity and update rules. A simpler project-local component library may be enough initially. | `asset_system/`, `editors/asset/`, `editors/space_file/`, ID linking and overrides |
| Python, console, text editor, add-ons/extensions | Useful for automation, custom building tools, format importers, batch tasks, and user-created workflows. Embedded Python/RNA is also deeply tied to Blender UI and startup code. | The in-app Console/Text Editor and a broad extension catalog can be hidden or reduced. Untrusted scripts need control. Removing embedded Python itself would remove a large part of Blender's extensibility and script-driven UI. | `python/`, `makesrna/`, `scripts/startup/`, `scripts/addons_core/`; `WITH_PYTHON` is described as development-only to disable |
| XR and stereoscopic/immersive viewing | Can support VR walkthroughs and spatial design review. | Not needed for most modeling workflows and brings platform/device support. It can be an optional build/distribution choice. | `WITH_XR_OPENXR`, XR editor/window-manager code |
| Localization, accessibility, input devices, and platform support | A usable professional tool needs broad input/display support, language support, and accessibility. NDOF devices can be useful when navigating architectural scenes. | A private/single-user fork might ship fewer translations or device backends, but removing these has little geometry payoff and can make the product harder to use. | `locale/`, `WITH_INTERNATIONAL`, GHOST/windowing options, input configuration |

## Architecture-specific capabilities Blender does not supply as a complete core

These are not features to “gut”; they are product decisions about what to add
or leave to extensions.

| Capability | Case to include | Case to defer or omit |
|---|---|---|
| Building/BIM objects and IFC semantics | Walls, slabs, doors, windows, levels, spaces, schedules, classifications, and IFC exchange make the application a BIM authoring tool rather than only a 3D modeler. | This is a large schema, interoperability, and project-management commitment. If the goal is architectural geometry and visualization, generic objects plus a separate IFC add-on/export path may be a smaller first product. |
| Drafting, dimensions, sheets, sections, and elevations | Architects need measured drawings and review documents, not only 3D views. Native dimensions and associative sections can keep documents tied to the model. | This is a second substantial product area. A first version can export geometry/views to a dedicated drafting/BIM tool if drawing production is not an initial goal. |
| CAD constraints and parametric history | Dimension-driven sketches and feature history can make edits predictable and preserve design intent. | A robust solver and persistent reference system are expensive. Blender modifiers/nodes do not automatically provide sketch constraints or a CAD feature tree. It may be staged after core geometry and editing are stable. |
| Native NURBS texture mapping and unwrap | Useful for applying materials to trimmed surfaces and exporting presentation geometry. Texture mapping can use surface parameters as inputs while storing its own texture-coordinate data. | It is not required for geometry correctness. A full unwrap editor and seam workflow can be deferred until surfaces, trims, and material output exist. Never repurpose geometric `(u,v)` parameters or trim p-curves as texture UVs. |
| Component schedules, quantities, and data reports | Area, length, counts, materials, and object metadata can support architectural decisions and documentation. | A reporting/scheduling system requires stable building semantics and project-specific data. A general attribute spreadsheet is not a substitute for a BIM schedule. |

## How removal scope affects engineering cost

| Cut level | What it means | Relative risk |
|---|---|---|
| Default workspace/UI | Remove a workspace, menu entry, panel, or tool from the default product layout. | Lowest. The code remains available, but hidden commands or file data may still work. |
| Shipped workflow/extensions | Stop packaging an add-on, asset catalog, or specialized workflow. | Low to medium. Check startup scripts, assets, menu registration, and dependencies. |
| Existing CMake option | Build without a subsystem already guarded by a `WITH_*` flag, such as Cycles, OpenVDB, Fluid, Freestyle, USD, Alembic, or codecs. | Medium. Verify dependent options and runtime menus; a flag can have indirect dependencies. |
| Editor/operator implementation | Remove a space, operator family, keymaps, UI, RNA, and tests. | High. Editors register centrally, operators may be called from scripts or other editors, and shared APIs may remain. |
| Data type/runtime/file support | Remove IDs, DNA/RNA, dependency-graph evaluation, serialization, conversion, versioning, and import/export. | Highest. Existing projects can lose data and shared systems may rely on those types. The previous broad removal commits illustrate this scope. |

Blender provides real build switches for a number of optional systems, but not
for every editor or object type. Many editors are included in the core Blender
build. Disabling a UI path is therefore not equivalent to removing the
implementation or reducing the full source/build graph.

## Source references

- Object and geometry type capabilities: `source/blender/makesdna/DNA_object_types.h`
- ID registration and file lifecycle: `source/blender/blenkernel/intern/idtype.cc`, `source/blender/blenloader/`
- Central editor/operator registration: `source/blender/editors/space_api/spacetypes.cc`
- Build options and dependency rules: root `CMakeLists.txt`, `source/blender/*/CMakeLists.txt`
- Mesh operators: `source/blender/editors/mesh/mesh_ops.cc`, `source/blender/bmesh/operators/`
- Modifiers: `source/blender/modifiers/intern/MOD_util.cc`; detailed 58-type roster in [`adjacent_mesh_capabilities.md`](nurbs_mesh_study/adjacent_mesh_capabilities.md)
- Existing mesh/NURBS operator-level study: [`nurbs_mesh_editing_study.md`](nurbs_mesh_editing_study.md), [`edit_mesh_menu_crosswalk.md`](nurbs_mesh_study/edit_mesh_menu_crosswalk.md), and the linked CSV inventories

No code was changed for this review, and no features were removed or restored.
