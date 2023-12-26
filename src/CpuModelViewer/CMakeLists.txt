add_executable(
    CpuModelViewer

    Main.cpp
    Rendering/DepthPyramid.cpp
)

target_link_libraries(CpuModelViewer PRIVATE
    imgui::imgui
    imguizmo::imguizmo
    glfw
    glad::glad
    assimp::assimp
    glm::glm
    libglimpsw
)