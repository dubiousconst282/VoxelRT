
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

class Renderer {
public:
    virtual ~Renderer() { }

    virtual void RenderFrame(cvox::VoxelMap& map, Camera& cam, glm::uvec2 viewSize) = 0;
    virtual void RenderSettings() { }
};
class CpuRenderer: public Renderer {
    std::shared_ptr<ogl::Shader> _blitShader;
    std::unique_ptr<ogl::Buffer> _pbo;
    std::unique_ptr<swr::Framebuffer> _fb;

    std::unique_ptr<swr::HdrTexture2D> _skyBox;

    uint32_t _numAccumFrames = 0;
    bool _enablePathTracer = false;

public:
    CpuRenderer(ogl::ShaderLib& shlib) {
        _blitShader = shlib.LoadFrag("BlitTiledFramebuffer_4x4", { { "FORMAT_RGB10", "1" } });
        _skyBox = std::make_unique<swr::HdrTexture2D>(swr::texutil::LoadCubemapFromPanoramaHDR("assets/skyboxes/sunflowers_puresky_4k.hdr"));
    }

    virtual void RenderFrame(cvox::VoxelMap& map, Camera& cam, glm::uvec2 viewSize) {
        viewSize &= ~3u; // round down to 4x4 steps

        #ifndef NDEBUG
        viewSize /= 4;
        #endif

        if (_fb == nullptr || _fb->Width != viewSize.x || _fb->Height != viewSize.y) {
            _fb = std::make_unique<swr::Framebuffer>(viewSize.x, viewSize.y);
            _pbo = std::make_unique<ogl::Buffer>(viewSize.x * viewSize.y * 4 + 12, GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
        }

        uint32_t width = _fb->Width;
        uint32_t height = _fb->Height;

        glm::mat4 invProj = glm::inverse(cam.GetProjMatrix() * cam.GetViewMatrix());
        // Bias matrix to take UVs in range [0..screen] rather than [-1..1]
        invProj = glm::translate(invProj, glm::vec3(-1.0f, -1.0f, 0.0f));
        invProj = glm::scale(invProj, glm::vec3(2.0f / _fb->Width, 2.0f / _fb->Height, 1.0f));

        auto rows = std::ranges::iota_view(0u, (height + 3) / 4);
        auto startTs = std::chrono::high_resolution_clock::now();

        std::for_each(std::execution::par_unseq, rows.begin(), rows.end(), [&](uint32_t rowId) {
            swr::VRandom rng(rowId + _numAccumFrames * 123456ull);
            uint32_t y = rowId * 4;

            for (uint32_t x = 0; x < width; x += 4) {
                swr::VFloat u = swr::simd::conv2f((int32_t)x + swr::FragPixelOffsetsX) + rng.NextUnsignedFloat() - 0.5f;
                swr::VFloat v = swr::simd::conv2f((int32_t)y + swr::FragPixelOffsetsY) + rng.NextUnsignedFloat() - 0.5f;

                swr::VFloat3 origin, dir;
                cvox::GetPrimaryRay({ u, v }, invProj, origin, dir);

                swr::VFloat3 attenuation = 1.0f;
                swr::VFloat3 incomingLight = 0.0f;
                swr::VMask mask = (swr::VMask)~0u;

                for (uint32_t i = 0; i < 3 && mask; i++) {
                    auto hit = cvox::RayMarch(map, origin, dir, mask);
                    auto mat = map.GetMaterial(hit.Voxels);
                    swr::VFloat3 matColor = mat.GetColor();
                    swr::VFloat emissionStrength = mat.GetEmissionStrength();

                    swr::VMask missMask = mask & ~hit.Mask;
                    if (missMask) {
                        constexpr swr::SamplerDesc SD = { .MinFilter = swr::FilterMode::Nearest, .EnableMips = true };
                        swr::VFloat3 skyColor = _skyBox->SampleCube<SD>(dir);
                        matColor.x = swr::simd::csel(missMask, skyColor.x, matColor.x);
                        matColor.y = swr::simd::csel(missMask, skyColor.y, matColor.y);
                        matColor.z = swr::simd::csel(missMask, skyColor.z, matColor.z);
                        emissionStrength = swr::simd::csel(missMask, 1.0f, emissionStrength);
                    }

                    if (!_enablePathTracer) [[unlikely]] {
                        incomingLight = matColor;
                        break;
                    }

                    attenuation *= matColor;
                    incomingLight += attenuation * emissionStrength;
                    mask &= hit.Mask;

                    origin = origin + dir * (hit.Dist - 1.0f / 128);
                    swr::VFloat3 normal = hit.GetNormal(dir);

                    dir = swr::simd::normalize(normal + rng.NextDirection());  // lambertian
                }
                // incomingLight = incomingLight / (incomingLight + 0.155) * 1.019;

                uint32_t* tilePtr = &_fb->ColorBuffer[_fb->GetPixelOffset(x, y)];

                auto prevColor = swr::pixfmt::RGB10u::Unpack(swr::VInt::load(tilePtr));

                float weight = 1.0f / (_numAccumFrames + 1);
                auto finalColor = incomingLight * weight + prevColor * (1.0f - weight);
                auto color = swr::pixfmt::RGB10u::Pack(finalColor);

                color.store(tilePtr);
            }
        });
        //_numAccumFrames++;

        auto endTs = std::chrono::high_resolution_clock::now();
        auto elapsed = (endTs - startTs).count() / 1000000.0;
        ImGui::Text("Time: %.1fms", elapsed);
        ImGui::Text("Frames: %d", _numAccumFrames);

        static_assert(offsetof(swr::Framebuffer, Height) == offsetof(swr::Framebuffer, Width) + 4);
        static_assert(offsetof(swr::Framebuffer, TileStride) == offsetof(swr::Framebuffer, Width) + 8);

        glNamedBufferSubData(_pbo->Handle, 0, 12, &_fb->Width);
        glNamedBufferSubData(_pbo->Handle, 12, _pbo->Size - 12, _fb->ColorBuffer.get());

        _blitShader->SetUniform("ssbo_FrameData", *_pbo);
        _blitShader->DispatchFullscreen();
    }
};
class GpuRenderer : public Renderer {
    std::shared_ptr<ogl::Shader> _shader;

public:
    GpuRenderer(ogl::ShaderLib& shlib) {
        _shader = shlib.LoadFrag("VoxelRender");
    }

    virtual void RenderFrame(cvox::VoxelMap& map, Camera& cam, glm::uvec2 viewSize) {
        map.SyncGpuBuffers();

        _shader->SetUniform("u_BrickStorage", *map.GpuBrickStorage);
        _shader->SetUniform("ssbo_VoxelMapData", *map.GpuMetaStorage);

        glm::mat4 invProj = glm::inverse(cam.GetProjMatrix() * cam.GetViewMatrix());
        _shader->SetUniform("u_InvProjMat", &invProj[0][0], 16);
        _shader->DispatchFullscreen();
    }
};

class Application {
    Camera _cam = {};

    cvox::VoxelMap _map;

    std::unique_ptr<ogl::ShaderLib> _shaderLib;
    std::unique_ptr<Renderer> _renderer;

public:
    Application() {
        ogl::EnableDebugCallback();

        _shaderLib = std::make_unique<ogl::ShaderLib>("src/CpuVoxelRT/Shaders/", true);

        _map.Palette[220] = cvox::Material::CreateDiffuse({ 1, 0.2, 0.2 }, 0.9f);
        _map.Palette[221] = cvox::Material::CreateDiffuse({ 0.2, 1, 0.2 }, 0.9f);
        _map.Palette[222] = cvox::Material::CreateDiffuse({ 0.2, 0.2, 1 }, 0.9f);
        _map.Palette[223] = cvox::Material::CreateDiffuse({ 1, 1, 1 }, 3.0f);

        _map.Set({ 4, 2, 4 }, cvox::Voxel::Create(220));
        _map.Set({ 4, 3, 4 }, cvox::Voxel::Create(221));
        _map.Set({ 4, 4, 4 }, cvox::Voxel::Create(222));
        _map.Set({ 4, 5, 4 }, cvox::Voxel::Create(223));

        try {
            _map.Deserialize("logs/voxels.dat");
        } catch (std::exception& ex) {
            std::cout << "Failed to load voxel map cache" << std::endl;

            auto model = scene::Model("assets/models/Sponza/Sponza.gltf");
            // auto model = scene::Model("logs/assets/models/ship_pinnace_4k/ship_pinnace_4k.gltf");
            // auto model = scene::Model("logs/assets/models/DamagedHelmet/DamagedHelmet.gltf");

            _map.VoxelizeModel(model, glm::uvec3(0), glm::uvec3(512));

            for (uint32_t x = 0; x < 512; x++) {
                for (uint32_t z = 0; z < 32; z++) {
                    _map.Set({ x, 16, 240 + z }, cvox::Voxel::Create(223));
                    _map.Set({ x, 180, 240 + z }, cvox::Voxel::Create(223));
                }
            }

            _map.Serialize("logs/voxels.dat");
        }

        _renderer = std::make_unique<GpuRenderer>(*_shaderLib);

        // _cam.Position = glm::vec3(50.5, 50.5, 50.5);
        _cam.Position = glm::vec3(288, 72, 256);
        // _cam.Position = glm::vec3(2, 4, 2);
        _cam.MoveSpeed = 80;
        _cam.Euler = glm::vec2(1.52, -0.5);
    }

    void Render(uint32_t vpWidth, uint32_t vpHeight) {
        ImGui::ShowMetricsWindow();

        _cam.Update();

        ImGui::Begin("Settings");

        ImGui::SeparatorText("Renderer");

        _renderer->RenderSettings();

        static bool s_RenderOnCpu = false;

        if (ImGui::Checkbox("Render on CPU", &s_RenderOnCpu)) {
            if (s_RenderOnCpu) _renderer = std::make_unique<CpuRenderer>(*_shaderLib);
            else _renderer = std::make_unique<GpuRenderer>(*_shaderLib);
        }

        ImGui::SeparatorText("Camera");
        ImGui::Text("Pos: %.2f %.2f %.2f, Rot: %.2f %.2f", _cam.Position.x, _cam.Position.y, _cam.Position.z, _cam.Euler.x, _cam.Euler.y);
        ImGui::SliderFloat("Speed", &_cam.MoveSpeed, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);

        ImGui::End();

        _renderer->RenderFrame(_map, _cam, glm::uvec2(vpWidth, vpHeight));

        _shaderLib->Refresh();
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
    glfwSwapInterval(0); // Enable vsync

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
    Application app;

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

        app.Render((uint32_t)display_w, (uint32_t)display_h);

        // Rendering
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }
    glfwTerminate();
    return 0;
}