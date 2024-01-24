add_executable(
    CpuVoxelRT

    Main.cpp
    VoxelMap.cpp
    Voxelize.cpp
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