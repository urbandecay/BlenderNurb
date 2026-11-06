# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Deliberately independent: no curve, surface, BMesh, NURBS or Blender kernel libraries.
add_library(bf_solid_analytic STATIC
  geometry.cc
  editing.cc
  fillet.cc
  fillet_wire.cc
  fillet_vertices.cc
  fillet_trimmed.cc
  display.cc
  storage.cc
  SOL_geometry.hh
  fillet_intern.hh
  id_index.hh
  extrusion.hh
)
target_include_directories(bf_solid_analytic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bf_solid_analytic PUBLIC cxx_std_20)

if(WITH_GTESTS)
  add_executable(solid_analytic_test tests/geometry_test.cc)
  target_link_libraries(solid_analytic_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_analytic_test COMMAND solid_analytic_test)
  add_executable(solid_editing_test tests/editing_test.cc)
  target_link_libraries(solid_editing_test PRIVATE bf_solid_analytic)
  add_test(NAME solid_editing_test COMMAND solid_editing_test)
endif()
