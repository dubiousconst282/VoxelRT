cmake_minimum_required(VERSION 3.30)

project("VoxelRT")
set(CMAKE_CXX_STANDARD 23)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Build requires Clang or GCC")
endif()

# Compile options: AVX2+, fast math
add_library(common_compile_opts INTERFACE)
target_compile_options(common_compile_opts INTERFACE "-Wno-unused" "-Wsign-conversion" "-ffast-math")

if("$ENV{TARGET_CPU}" STREQUAL "AVX2")
    target_compile_options(common_compile_opts INTERFACE "-march=x86-64-v3")
elseif("$ENV{TARGET_CPU}" STREQUAL "AVX512")
    target_compile_options(common_compile_opts INTERFACE "-march=x86-64-v4")
else()
    message("Compiling for march=native, set env TARGET_CPU=AVX2|AVX512 to override.")
    target_compile_options(common_compile_opts INTERFACE "-march=native")
endif()

# Fixup options for clang-cl (prepend "/clang:")
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT MATCHES "MSVC")
    get_target_property(fixed_opts common_compile_opts INTERFACE_COMPILE_OPTIONS)
    list(TRANSFORM fixed_opts REPLACE "^-[^W].+" "\/clang:\\0")
    set_target_properties(common_compile_opts PROPERTIES INTERFACE_COMPILE_OPTIONS "${fixed_opts}")
endif()

# Setup CPM
include("CPM.cmake")

if(NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "${CMAKE_BINARY_DIR}/deps/")  # speeds up cmake configures
endif()

include("packages.cmake")

# Sub projects
add_subdirectory("LibHavk/")
add_subdirectory("VoxelRT/")