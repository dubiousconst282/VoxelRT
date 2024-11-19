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
    Render/RendererDistField.cpp
    Render/RendererOctantDF.cpp
    Render/RendererESVO.cpp
    Render/RendererTree64.cpp
    Render/RendererBVH.cpp
    Render/RendererMesh.cpp
    Render/Benchmark.cpp
    
    Render/BrickSlotAllocator.cpp
)

target_link_libraries(VoxelRT PRIVATE
    common_compile_opts
    havk
    FastNoise
    fastgltf::fastgltf
    bvh
)

if(CMAKE_HOST_WIN32)
    add_custom_command(TARGET VoxelRT POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:VoxelRT> $<TARGET_FILE_DIR:VoxelRT>
        COMMAND_EXPAND_LISTS
    )
endif()