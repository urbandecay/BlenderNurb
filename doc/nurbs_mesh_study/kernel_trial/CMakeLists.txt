# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Standalone research harness. Not included by Blender's build.
cmake_minimum_required(VERSION 3.22)
project(nurbs_kernel_trial LANGUAGES CXX)

set(OCCT_BUILD_DIR "" CACHE PATH "Isolated OCCT 8.0.1 Release build")
set(OCCT_LIBRARY_DIR "${OCCT_BUILD_DIR}/lin64/gcc/lib" CACHE PATH "OCCT shared libraries")

add_executable(nurbs_kernel_trial ownership_trial.cc shared_boundary_trial.cc)
target_compile_features(nurbs_kernel_trial PRIVATE cxx_std_17)
target_include_directories(nurbs_kernel_trial SYSTEM PRIVATE "${OCCT_BUILD_DIR}/include/opencascade")
foreach(toolkit TKBRep TKGeomBase TKG3d TKG2d TKMath TKernel)
  find_library(${toolkit}_LIBRARY NAMES ${toolkit} PATHS "${OCCT_LIBRARY_DIR}" NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(nurbs_kernel_trial PRIVATE "${${toolkit}_LIBRARY}")
endforeach()
set_target_properties(nurbs_kernel_trial PROPERTIES BUILD_RPATH "${OCCT_LIBRARY_DIR}")
