add_library(
    havk STATIC

    Havk/Context.cpp
    Havk/Swapchain.cpp
    Havk/Resource.cpp
    Havk/CommandQueue.cpp
    Havk/Pipeline.cpp
    Havk/FileWatcher.cpp
    Havk/ImageIO.cpp
    
    Havx/SettingStore.cpp
    Havx/BinaryIO.cpp

    SwRast/ImageHelpers.cpp
)
target_link_libraries(havk PRIVATE common_compile_opts)

target_include_directories(
    havk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(havk PRIVATE
    slang::slang
    libzstd
)

target_link_libraries(havk PUBLIC
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    
    glfw
    glm::glm
    imgui
    magic_enum::magic_enum
)
target_compile_definitions(havk PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
   # GLM_FORCE_LEFT_HANDED
    GLM_FORCE_XYZW_ONLY
)
target_include_directories(havk PRIVATE ${stb_SOURCE_DIR})
