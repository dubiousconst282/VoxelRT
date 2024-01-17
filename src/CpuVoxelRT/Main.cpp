
#include <chrono>
#include <execution>
#include <iostream>
#include <ranges>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <ImGuizmo.h>

#include <SwRast/Rasterizer.h>
#include <SwRast/Texture.h>
#include <Common/Camera.h>
#include <OGL/QuickGL.h>
#include <OGL/ShaderLib.h>
#include <Common/Scene.h>

#include "RayMarching.h"

class VoxelTracer {
    Camera _cam = {};

    std::unique_ptr<ogl::Texture2D> _tex;
    std::unique_ptr<swr::Framebuffer> _fb;
    swr::AlignedBuffer<uint32_t> _pixels;

    cvox::VoxelMap _map;

    uint32_t numAccumFrames = 0;

public:
    VoxelTracer() {
        ogl::EnableDebugCallback();

        _map.Palette[120] = cvox::Material::CreateDiffuse({ 1, 0.2, 0.2 }, 0.9f);
        _map.Palette[121] = cvox::Material::CreateDiffuse({ 0.2, 1, 0.2 }, 0.9f);
        _map.Palette[122] = cvox::Material::CreateDiffuse({ 0.2, 0.2, 1 }, 0.9f);
        _map.Palette[123] = cvox::Material::CreateDiffuse({ 1, 1, 1 }, 3.0f);

        auto model = scene::Model("assets/models/Sponza/Sponza.gltf");
        //auto model = scene::Model("logs/assets/models/ship_pinnace_4k/ship_pinnace_4k.gltf");
        //auto model = scene::Model("logs/assets/models/DamagedHelmet/DamagedHelmet.gltf");
        _map.VoxelizeModel(model);

        auto rng = swr::VRandom(1234);

        for (uint32_t x = 0; x < 512; x++) {
            for (uint32_t z = 0; z < 32; z++) {
                _map.At(x, 16, 240 + z) = cvox::Voxel::Create(123);
                _map.At(x, 150, 240 + z) = cvox::Voxel::Create(123);

                // uint32_t rand = rng.NextU32()[0];
                // if ((rand & 63) == 0) {
                //     _map.At(x, 20 + (rand >> 20) % 50, 240 + z) = cvox::Voxel::Create(120 + (rand >> 10) % 4);
                // }
            }
        }

        _map.UpdateDistanceField();

       // _cam.Position = glm::vec3(50.5, 50.5, 50.5);
        _cam.Position = glm::vec3(36, 9, 32);
        _cam.MoveSpeed = 10;
        _cam.Euler = glm::vec2(1.52, -0.5);
    }


    void Render(uint32_t vpWidth, uint32_t vpHeight) {
        ImGui::ShowMetricsWindow();

        auto oldPos = _cam.Position;
        auto oldRot = _cam.Euler;
        _cam.Update();
        if (_cam.Position != oldPos || _cam.Euler != oldRot){
            numAccumFrames = 0;
        }

        ImGui::Begin("Info");

        glm::ivec3 gridPos = glm::floor(_cam.Position);

        //ImGui::Text("DistField: %.1f", _map.At(gridPos.x, gridPos.y, gridPos.z).GetDistToNearest());
        ImGui::Text("Cam: %.2f %.2f %.2f, %.2f %.2f", _cam.Position.x, _cam.Position.y, _cam.Position.z, _cam.Euler.x, _cam.Euler.y);

        if (_tex == nullptr || _tex->Width != vpWidth || _tex->Height != vpHeight) {
            _fb = std::make_unique<swr::Framebuffer>(vpWidth, vpHeight);
            _tex = std::make_unique<ogl::Texture2D>(_fb->Width, _fb->Height, 1, GL_RGB10);
            _pixels = swr::alloc_buffer<uint32_t>(_tex->Width * _tex->Height);
        }

        uint32_t width = _fb->Width;
        uint32_t height = _fb->Height;

        glm::mat4 invProj = glm::inverse(_cam.GetProjMatrix() * glm::scale(_cam.GetViewMatrix(), glm::vec3(0.125f)));
        // Bias matrix to take UVs in range [0..screen] rather than [-1..1]
        invProj = glm::translate(invProj, glm::vec3(-1.0f, -1.0f, 0.0f));
        invProj = glm::scale(invProj, glm::vec3(2.0f / _fb->Width, 2.0f / _fb->Height, 1.0f));

        auto rows = std::ranges::iota_view(0u, (height + 3) / 4);
        auto startTs = std::chrono::high_resolution_clock::now();

        std::for_each(std::execution::par_unseq, rows.begin(), rows.end(), [&](uint32_t rowId) {
            swr::VRandom rng(rowId + numAccumFrames * 123456ull);
            uint32_t y = rowId * 4;

            for (uint32_t x = 0; x < width; x += 4) {
                swr::VFloat u = swr::simd::conv2f((int32_t)x + swr::FragPixelOffsetsX) + rng.NextUnsignedFloat() - 0.5f;
                swr::VFloat v = swr::simd::conv2f((int32_t)y + swr::FragPixelOffsetsY) + rng.NextUnsignedFloat() - 0.5f;

                swr::VFloat3 origin, dir;
                cvox::GetPrimaryRay({ u, v }, invProj, origin, dir);

                swr::VFloat3 attenuation = 1.0f;
                swr::VFloat3 incomingLight = 0.0f;
                swr::VMask mask = (swr::VMask)~0u;

                for (uint32_t i = 0; i < 3; i++) {
                    auto hit = cvox::RayMarch(_map, origin, dir, mask);
                    auto mat = _map.GetMaterial(hit.Voxels);

                    attenuation *= mat.GetColor();
                    incomingLight += attenuation * mat.GetEmissionStrength();
                    mask &= hit.Mask;

                    origin = origin + dir * (hit.Dist - 1.0f / 128);
                    swr::VFloat3 normal = hit.GetNormal(dir);

                    dir = swr::simd::normalize(normal + rng.NextDirection()); // lambertian
                }

                uint32_t* tilePtr = &_fb->ColorBuffer[_fb->GetPixelOffset(x, y)];

                auto prevColor = swr::pixfmt::RGB10u::Unpack(swr::VInt::load(tilePtr));

                float weight = 1.0f / (numAccumFrames + 1);
                auto finalColor = incomingLight * weight + prevColor * (1.0f - weight);
                auto color = swr::pixfmt::RGB10u::Pack(finalColor);

                color.store(tilePtr);
            }
        });
        numAccumFrames++;

        auto endTs = std::chrono::high_resolution_clock::now();
        auto elapsed = (endTs - startTs).count() / 1000000.0;
        ImGui::Text("Time: %.1fms", elapsed);
        ImGui::Text("Frames: %d", numAccumFrames);

        _fb->GetPixels(_pixels.get(), _fb->Width);
        _tex->SetPixels(GL_RGBA, GL_UNSIGNED_INT_10_10_10_2, _pixels.get(), _fb->Width);
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        auto texId = (ImTextureID)(uintptr_t)_tex->Handle;
        drawList->AddImage(texId, drawList->GetClipRectMin(), drawList->GetClipRectMax(), ImVec2(0, 1), ImVec2(1, 0));
        
        ImGui::End();
    }
};

int main(int argc, char** args) {
    if (!glfwInit()) return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, 1);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "VoxelRT", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "logs/imgui.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    io.Fonts->AddFontFromFileTTF("assets/Roboto-Medium.ttf", 18.0f);

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();

    ogl::EnableDebugCallback();
    VoxelTracer game;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
            static int winRect[4];
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();

            if (!glfwGetWindowMonitor(window)) {
                glfwGetWindowPos(window, &winRect[0], &winRect[1]);
                glfwGetWindowSize(window, &winRect[2], &winRect[3]);

                const GLFWvidmode* mode = glfwGetVideoMode(monitor);
                glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, 0);
            } else {
                glfwSetWindowMonitor(window, nullptr, winRect[0], winRect[1], winRect[2], winRect[3], 0);
            }
            glfwSwapInterval(1);
        }

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        game.Render((uint32_t)display_w, (uint32_t)display_h);

        // Rendering
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }
    glfwTerminate();
    return 0;
}