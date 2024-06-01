cmake_minimum_required(VERSION 3.25)

set(VCPKG_MANIFEST_MODE ON)
set(VCPKG_BUILD_TYPE "release")
set(CMAKE_BINARY_DIR "${CMAKE_CURRENT_LIST_DIR}/../build")
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

if(CMAKE_HOST_LINUX)
    set(VCPKG_TARGET_TRIPLET "x64-linux-dynamic") # static link takes way too long
endif()

project("VoxelRT")

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
set(CMAKE_CXX_STANDARD 20)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	message(FATAL_ERROR "Build requires Clang or GCC")
endif()

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT MATCHES "MSVC") # clang-cl
	add_compile_options("/clang:-march=native" "/clang:-ffast-math" "-Wno-unused" "-Wsign-conversion")
else()
	# add_compile_options("-mavx2" "-mfma" "-mf16c" "-ffast-math" "-Wno-unused" "-Wsign-conversion")
	add_compile_options("-march=native" "-ffast-math" "-Wno-unused" "-Wsign-conversion")
endif()

add_subdirectory("LibGlimpsw/")
add_subdirectory("VoxelRT/")
