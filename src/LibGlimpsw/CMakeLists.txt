add_library(
    libglimpsw STATIC

    SwRast/Rasterizer.cpp
    SwRast/VertexReader.cpp
    SwRast/Clipper.cpp
    SwRast/ImageHelpers.cpp

    Common/Scene.cpp
    Common/SettingManager.cpp
    Common/BinaryIO.cpp

    OGL/ShaderLib.cpp
)
target_include_directories(
    libglimpsw PUBLIC

    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(libglimpsw PRIVATE
    assimp::assimp
    glm::glm
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static> # wtf
)
target_include_directories(libglimpsw PRIVATE ${Stb_INCLUDE_DIR})