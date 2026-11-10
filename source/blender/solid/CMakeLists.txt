# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Deliberately independent: no curve, surface, BMesh, NURBS or Blender kernel libraries.
add_library(bf_solid_analytic STATIC
  geometry.cc
  intersections.cc
  boolean.cc
  boolean_replay.cc
  boolean_pave.cc
  boolean_face.cc
  boolean_sew.cc
  boolean_periodic.cc
  boolean_clip.cc
  boolean_partition.cc
  boolean_convex.cc
  boolean_cells.cc
  boolean_unify.cc
  boolean_planar_clip.cc
  boolean_planar_region.cc
  boolean_cylinder_unify.cc
  classify.cc
  spline.cc
  editing.cc
  fillet.cc
  fillet_wire.cc
  fillet_vertices.cc
  fillet_trimmed.cc
  display.cc
  storage.cc
  SOL_geometry.hh
  SOL_intersections.hh
  SOL_boolean.hh
  SOL_boolean_replay.hh
  SOL_classify.hh
  SOL_clip.hh
  SOL_partition.hh
  SOL_cells.hh
  SOL_unify.hh
  SOL_planar_clip.hh
  SOL_planar_region.hh
  SOL_cylinder_unify.hh
  SOL_spline.hh
  boolean_pave.hh
  fillet_corner_grid.hh
  fillet_intern.hh
  id_index.hh
  extrusion.hh
)
target_include_directories(bf_solid_analytic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bf_solid_analytic PUBLIC cxx_std_20)

# Diagnostic request replay works without loading Blender or the user's scene.
add_executable(solid_boolean_replay tools/boolean_replay_main.cc)
target_link_libraries(solid_boolean_replay PRIVATE bf_solid_analytic)

if(WITH_GTESTS)
  add_executable(solid_boolean_replay_test tests/boolean_replay_test.cc)
  target_link_libraries(solid_boolean_replay_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_boolean_replay_test COMMAND solid_boolean_replay_test)
  add_executable(solid_boolean_contract_test tests/boolean_contract_test.cc)
  target_link_libraries(solid_boolean_contract_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_boolean_contract_test COMMAND solid_boolean_contract_test)
  add_executable(solid_planar_region_test tests/planar_region_test.cc)
  target_link_libraries(solid_planar_region_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_planar_region_test COMMAND solid_planar_region_test)
  add_executable(solid_cylinder_unify_test tests/cylinder_unify_test.cc)
  target_link_libraries(solid_cylinder_unify_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_cylinder_unify_test COMMAND solid_cylinder_unify_test)
  add_executable(solid_planar_clip_test tests/planar_clip_test.cc)
  target_link_libraries(solid_planar_clip_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_planar_clip_test COMMAND solid_planar_clip_test)
  add_executable(solid_unify_test tests/unify_test.cc)
  target_link_libraries(solid_unify_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_unify_test COMMAND solid_unify_test)
  add_executable(solid_cells_test tests/cells_test.cc)
  target_link_libraries(solid_cells_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_cells_test COMMAND solid_cells_test)
  add_executable(solid_clip_test tests/clip_test.cc)
  target_link_libraries(solid_clip_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_clip_test COMMAND solid_clip_test)
  add_executable(solid_classify_test tests/classify_test.cc)
  target_link_libraries(solid_classify_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_classify_test COMMAND solid_classify_test)
  add_executable(solid_spline_test tests/spline_test.cc)
  target_link_libraries(solid_spline_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_spline_test COMMAND solid_spline_test)
  add_executable(solid_boolean_face_test tests/boolean_face_test.cc)
  target_link_libraries(solid_boolean_face_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_boolean_face_test COMMAND solid_boolean_face_test)
  add_executable(solid_analytic_test tests/geometry_test.cc)
  target_link_libraries(solid_analytic_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_analytic_test COMMAND solid_analytic_test)
  add_executable(solid_editing_test tests/editing_test.cc)
  target_link_libraries(solid_editing_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_editing_test COMMAND solid_editing_test)
  add_executable(solid_intersection_test tests/intersections_test.cc)
  target_link_libraries(solid_intersection_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_intersection_test COMMAND solid_intersection_test)
  add_executable(solid_boolean_test tests/boolean_test.cc)
  target_link_libraries(solid_boolean_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_boolean_test COMMAND solid_boolean_test)
endif()
