add_library(
    glimpsw STATIC

    SwRast/ImageHelpers.cpp

    Common/Scene.cpp
    Common/SettingStore.cpp
    Common/BinaryIO.cpp

    OGL/ShaderLib.cpp
    OGL/QuickGL.cpp
)
target_include_directories(
    glimpsw PUBLIC

    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(glimpsw PRIVATE
    assimp::assimp
    glm::glm
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static> # wtf
    magic_enum::magic_enum
)
target_include_directories(glimpsw PRIVATE ${Stb_INCLUDE_DIR})