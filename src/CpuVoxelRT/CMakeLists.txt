add_executable(
    CpuVoxelRT

    Main.cpp
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