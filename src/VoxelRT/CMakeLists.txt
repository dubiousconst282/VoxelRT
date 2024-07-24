add_executable(
    VoxelRT

    Main.cpp
    VoxelMap.cpp
    TerrainGenerator.cpp
    GenerateLOD.cpp
    Voxelize.cpp
    Brush.cpp
    
    Render/RendererXBrickMap.cpp
    Render/RendererCPU.cpp
    Render/RendererFlatDDA.cpp
    
    Render/BrickSlotAllocator.cpp
)

set(FASTNOISE2_NOISETOOL OFF CACHE BOOL "Build Noise Tool" FORCE)
add_subdirectory(../../deps/FastNoise2 ${CMAKE_BINARY_DIR}/deps/FastNoise2 EXCLUDE_FROM_ALL)

# Clang 17 crashes when compiling FastNoise with -march=native that is added by the top-level list.
# Don't care enough to find out why so as a workaround, just strip them all here. (Also silences annoying warnings)
set_target_properties(FastNoise PROPERTIES COMPILE_OPTIONS "")

add_subdirectory(../../deps/tracy ${CMAKE_BINARY_DIR}/deps/tracy EXCLUDE_FROM_ALL)
add_subdirectory(../../deps/fastgltf ${CMAKE_BINARY_DIR}/deps/fastgltf EXCLUDE_FROM_ALL)

target_link_libraries(VoxelRT PRIVATE
    glimpsw
    FastNoise
    fastgltf::fastgltf
    Tracy::TracyClient
)