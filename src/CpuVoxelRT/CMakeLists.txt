add_executable(
    CpuVoxelRT

    Main.cpp
    VoxelMap.cpp
    Voxelize.cpp
    #CpuTracer.cpp
    GpuRenderer.cpp
    BrickSlotAllocator.cpp
)

target_link_libraries(CpuVoxelRT PRIVATE
    imgui::imgui
    imguizmo::imguizmo
    glfw
    glad::glad
    assimp::assimp
    glm::glm
    libglimpsw
)