add_executable(
    CpuVoxelRT

    Main.cpp
    VoxelMap.cpp
    Voxelize.cpp
    VoxelSerialize.cpp
)

find_package(zstd CONFIG REQUIRED)
target_link_libraries(CpuVoxelRT PRIVATE
    imgui::imgui
    imguizmo::imguizmo
    glfw
    glad::glad
    assimp::assimp
    glm::glm
    libglimpsw
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static> # wtf
)