cmake_minimum_required(VERSION 3.25)

set(VCPKG_MANIFEST_MODE ON)
set(VCPKG_BUILD_TYPE "release")
set(CMAKE_BINARY_DIR "${CMAKE_CURRENT_LIST_DIR}/../build")
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

project("VoxelRT")

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
set(CMAKE_CXX_STANDARD 20)

#set(CMAKE_FIND_DEBUG_MODE ON)
find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)


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
