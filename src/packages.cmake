find_package(Vulkan REQUIRED)

CPMAddPackage("gh:glfw/glfw#3.4")
CPMAddPackage("gh:GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator#v3.1.0")

CPMAddPackage("gh:g-truc/glm#1.0.1")
CPMAddPackage("gh:nothings/stb#master")
CPMAddPackage("gh:spnda/fastgltf#v0.8.0")

CPMAddPackage("gh:Neargye/magic_enum#v0.9.6")

CPMAddPackage("gh:Auburn/FastNoise2#v0.10.0-alpha")
CPMAddPackage("gh:madmann91/bvh#master")

# ImGui
CPMAddPackage("gh:ocornut/imgui#v1.91.4")
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC 
    ${imgui_SOURCE_DIR} 
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan)

# Slang
if(CMAKE_HOST_WIN32)
    set(SLANG_BIN_PACKAGE_NAME "windows-x86_64.zip")
elseif(CMAKE_HOST_LINUX)
    set(SLANG_BIN_PACKAGE_NAME "linux-x86_64.zip")
else()
    message(FATAL_ERROR "Unsupported host")
endif()

# Slang
if (NOT slang_SOURCE_DIR) # allow user override by -Dslang_SOURCE_DIR=xxx
    CPMAddPackage(
        NAME slang
        URL "https://github.com/shader-slang/slang/releases/download/v2025.6.2/slang-2025.6.2-${SLANG_BIN_PACKAGE_NAME}"
        DOWNLOAD_ONLY true
    )
endif()

add_library(slang::slang SHARED IMPORTED)
add_library(slang::slang-glslang SHARED IMPORTED)

if(CMAKE_HOST_WIN32)
    set_target_properties(slang::slang PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES   "${slang_SOURCE_DIR}/include/"
        IMPORTED_IMPLIB                 "${slang_SOURCE_DIR}/lib/slang.lib"
        IMPORTED_LOCATION               "${slang_SOURCE_DIR}/bin/slang.dll"
    )
elseif(CMAKE_HOST_LINUX)
    set_target_properties(slang::slang PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES   "${slang_SOURCE_DIR}/include/"
        IMPORTED_LOCATION               "${slang_SOURCE_DIR}/lib/libslang.so"
    )
    target_link_libraries(slang::slang PUBLIC INTERFACE
        "${slang_SOURCE_DIR}/lib/libslang.so"
    )
endif()

# zstd
CPMAddPackage(
    NAME zstd 
    URL "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz"
    SOURCE_SUBDIR "build/cmake"
    OPTIONS
        "ZSTD_BUILD_STATIC ON"
        "ZSTD_BUILD_SHARED OFF"
        "ZSTD_LEGACY_SUPPORT OFF"
        "ZSTD_BUILD_PROGRAMS OFF"
        "ZSTD_MULTITHREAD_SUPPORT OFF"
)

