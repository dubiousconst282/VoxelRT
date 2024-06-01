set(FASTNOISE2_NOISETOOL OFF CACHE BOOL "Build Noise Tool" FORCE)
add_subdirectory(../../deps/FastNoise2 ${CMAKE_BINARY_DIR}/deps EXCLUDE_FROM_ALL)

# Clang 17 crashes when compiling FastNoise with -march=native that is added by the top-level list.
# Don't care enough to find out why so as a workaround, just strip them all here. (Also silences annoying warnings)
set_target_properties(FastNoise PROPERTIES COMPILE_OPTIONS "")

add_executable(
    VoxelRT

    Main.cpp
    VoxelMap.cpp
    Voxelize.cpp
    GpuRenderer.cpp
    CpuRenderer.cpp
    BrickSlotAllocator.cpp
    TerrainGenerator.cpp
    Brush.cpp
)

target_link_libraries(VoxelRT PRIVATE
    glimpsw
    FastNoise
)