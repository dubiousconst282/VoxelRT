add_library(
    glimpsw STATIC

    SwRast/ImageHelpers.cpp

    Common/Scene.cpp
    Common/SettingStore.cpp
    Common/BinaryIO.cpp

    Havk/Context.cpp
    Havk/Swapchain.cpp
    Havk/Resource.cpp
    Havk/CommandQueue.cpp
    Havk/Pipeline.cpp
    Havk/FileWatcher.cpp
)
target_include_directories(
    glimpsw PUBLIC

    ${CMAKE_CURRENT_SOURCE_DIR}
)


#set(CMAKE_FIND_DEBUG_MODE ON)
find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

find_package(assimp CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)

target_link_libraries(glimpsw PRIVATE
    assimp::assimp
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static> # wtf
    magic_enum::magic_enum
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/slang.lib"
    # "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libslang.so"
)
target_link_libraries(glimpsw PUBLIC
    ${Vulkan_LIBRARIES}
    GPUOpen::VulkanMemoryAllocator

    glfw
    glm::glm
    imgui::imgui
)
target_compile_definitions(glimpsw PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_LEFT_HANDED
)
target_include_directories(glimpsw PRIVATE ${Stb_INCLUDE_DIR})