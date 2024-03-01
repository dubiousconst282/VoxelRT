
#include <chrono>
#include <iostream>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <ImGuizmo.h>

#include <SwRast/Rasterizer.h>
#include <SwRast/Texture.h>

#include "Renderer.h"


/*
class CpuRenderer: public Renderer {
    std::shared_ptr<ogl::Shader> _blitShader;
    std::unique_ptr<ogl::Buffer> _pbo;
    std::unique_ptr<swr::Framebuffer> _fb;

    std::unique_ptr<swr::HdrTexture2D> _skyBox;

    uint32_t _frameNo = 0;

    bool _enablePathTracer;
    glim::TimeStat _frameTime;

    glm::dvec3 _prevCameraPos;
    glm::quat _prevCameraRot;

public:
    CpuRenderer(ogl::ShaderLib& shlib) {
        _blitShader = shlib.LoadFrag("BlitTiledFramebuffer_4x4", { { "FORMAT_RGB10", "1" } });
        _skyBox = std::make_unique<swr::HdrTexture2D>(swr::texutil::LoadCubemapFromPanoramaHDR("assets/skyboxes/sunflowers_puresky_4k.hdr"));
    }

    virtual void RenderFrame(VoxelMap& map, glim::Camera& cam, glm::uvec2 viewSize) {
        viewSize &= ~3u; // round down to 4x4 steps

        #ifndef NDEBUG
        viewSize /= 4;
        #endif

        bool camChanged = false;
        if (glm::distance(cam.ViewPosition, _prevCameraPos) > 0.1f || glm::dot(cam.ViewRotation, _prevCameraRot) < 0.999999f) {
            camChanged = true;
        }
        _prevCameraPos = cam.ViewPosition;
        _prevCameraRot = cam.ViewRotation;

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

        _frameTime.Begin();

        auto rows = std::ranges::iota_view(0u, (height + 3) / 4);
        std::for_each(std::execution::par_unseq, rows.begin(), rows.end(), [&](uint32_t rowId) {
            swr::VRandom rng(rowId + _frameNo * 123456ull);
            uint32_t y = rowId * 4;
            swr::VFloat v = swr::simd::conv2f((int32_t)y + swr::FragPixelOffsetsY) + rng.NextUnsignedFloat() - 0.5f;

            for (uint32_t x = 0; x < width; x += 4) {
                swr::VFloat u = swr::simd::conv2f((int32_t)x + swr::FragPixelOffsetsX) + rng.NextUnsignedFloat() - 0.5f;

                swr::VFloat3 origin, dir;
                GetPrimaryRay({ u, v }, invProj, origin, dir);

                swr::VFloat3 attenuation = 1.0f;
                swr::VFloat3 incomingLight = 0.0f;
                swr::VMask mask = (swr::VMask)~0u;

                for (uint32_t i = 0; i < 3 && mask; i++) {
                    auto hit = RayMarch(map, origin, dir, mask, i==0);
                    auto mat = map.GetMaterial(hit.Voxels, mask);
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

                // float weight = 1.0f / (_frameNo + 1);
                float weight = camChanged ? 0.5f : 0.1f;
                auto finalColor = incomingLight * weight + prevColor * (1.0f - weight);
                auto color = swr::pixfmt::RGB10u::Pack(finalColor);

                color.store(tilePtr);
            }
        });

        _frameTime.End();
        _frameNo++;

        static_assert(offsetof(swr::Framebuffer, Height) == offsetof(swr::Framebuffer, Width) + 4);
        static_assert(offsetof(swr::Framebuffer, TileStride) == offsetof(swr::Framebuffer, Width) + 8);

        glNamedBufferSubData(_pbo->Handle, 0, 12, &_fb->Width);
        glNamedBufferSubData(_pbo->Handle, 12, _pbo->Size - 12, _fb->ColorBuffer.get());

        _blitShader->SetUniform("ssbo_FrameData", *_pbo);
        _blitShader->DispatchFullscreen();
    }
    virtual void DrawSettings(glim::SettingStore& settings) {
        ImGui::SeparatorText("Renderer##CPU");
        settings.Checkbox("Path Trace", &_enablePathTracer);
        _frameTime.Draw("Frame Time");
    }
};*/

class Application {
    glim::Camera _cam = {};
    glim::SettingStore _settings;

    std::shared_ptr<VoxelMap> _map;

    std::unique_ptr<ogl::ShaderLib> _shaderLib;
    std::unique_ptr<Renderer> _renderer;

public:
    Application() {
        ogl::EnableDebugCallback();

        _shaderLib = std::make_unique<ogl::ShaderLib>("src/CpuVoxelRT/Shaders/", true);

        _map = std::make_shared<VoxelMap>();
        _map->Palette[252] = Material::CreateDiffuse({ 1, 0.2, 0.2 }, 0.9f);
        _map->Palette[253] = Material::CreateDiffuse({ 0.2, 1, 0.2 }, 0.9f);
        _map->Palette[254] = Material::CreateDiffuse({ 0.2, 0.2, 1 }, 0.9f);
        _map->Palette[255] = Material::CreateDiffuse({ 1, 1, 1 }, 3.0f);

        try {
            _map->Deserialize("logs/voxels_2k.dat");
        } catch (std::exception& ex) {
            std::cout << "Failed to load voxel map cache: " << ex.what() << std::endl;

            auto model = glim::Model("assets/models/Sponza/Sponza.gltf");
            // auto model = glim::Model("logs/assets/models/ship_pinnace_4k/ship_pinnace_4k.gltf");
            // auto model = glim::Model("logs/assets/models/DamagedHelmet/DamagedHelmet.gltf");

            _map->VoxelizeModel(model, glm::uvec3(24), glm::uvec3(2024));

            _map->Serialize("logs/voxels_2k.dat");
        }

        _cam.Position = glm::vec3(512, 128, 512);
        _cam.MoveSpeed = 180;
        _cam.Euler = glm::vec2(1.52, -0.5);

        _settings.Load("logs/voxelrt_settings.dat", true);
    }

    void Render(uint32_t vpWidth, uint32_t vpHeight) {
        ImGui::ShowMetricsWindow();

        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
            MousePickVoxels();
        } else {
            _cam.Update();
        }

        ImGui::Begin("Settings");

        ImGui::SeparatorText("General");

        static bool useVSync = true;
        if (_settings.Checkbox("VSync", &useVSync)) {
            glfwSwapInterval(useVSync ? 1 : 0);
        }

        static bool useCpuRenderer = false;
        if (_renderer == nullptr || _settings.Checkbox("Use CPU Renderer", &useCpuRenderer)) {
            if (useCpuRenderer) {
                _renderer = std::make_unique<CpuRenderer>(*_shaderLib);
            } else {
                _renderer = std::make_unique<GpuRenderer>(*_shaderLib, _map);
            }
            auto fn = std::function(ImGui::InputScalarN);
        }

        _renderer->DrawSettings(_settings);

        ImGui::Text("Total Sectors: %zu", _map->Sectors.size());

        ImGui::SeparatorText("Camera");
        _settings.InputScalarN("Pos", &_cam.Position.x, 3, "%.1f");
        _settings.InputScalarN("Rot", &_cam.Euler.x, 2, "%.1f");
        _settings.Slider("Speed", &_cam.MoveSpeed, 1, 0.5f, 500.0f, "%.1f");
        _settings.Slider("FOV", &_cam.FieldOfView, 1, 10.0f, 120.0f, "%.1f deg");
        ImGui::End();

        _renderer->RenderFrame(_cam, glm::uvec2(vpWidth, vpHeight));

        _shaderLib->Refresh();
    }

    void MousePickVoxels() {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        glm::vec2 mouseUV = glm::vec2(mousePos.x / displaySize.x, 1 - mousePos.y / displaySize.y) * 2.0f - 1.0f;

        glm::mat4 invProj = glm::inverse(_cam.GetProjMatrix() * _cam.GetViewMatrix(false));
        glm::vec4 nearPos = invProj * glm::vec4(mouseUV, 0, 1);
        glm::vec4 farPos = nearPos + glm::vec4(invProj[2]);
        glm::vec3 dir = glm::normalize(farPos * (1.0f / farPos.w));

        static double brushDist = 30;
        bool erase = ImGui::IsKeyDown(ImGuiKey_ModAlt);
        auto voxel = erase ? Voxel::CreateEmpty() : Voxel::Create(255);

        double hitDist = _map->RayCast(_cam.ViewPosition, dir, 4096);
        if (hitDist < 30) hitDist = 30;

        glm::ivec3 hitPos = glm::floor(_cam.ViewPosition + glm::dvec3(dir) * hitDist);
        if (erase || ImGui::IsMouseClicked(ImGuiMouseButton_Left) || 
            (!erase && (hitDist > brushDist || _map->Get(hitPos).Data != voxel.Data))
        ) {
            brushDist = hitDist;
        }

        glm::ivec3 brushPos = glm::floor(_cam.ViewPosition + glm::dvec3(dir) * brushDist);
        int32_t radius = 90;

        using swr::VInt, swr::VMask;
        _map->RegionDispatchSIMD(brushPos - radius, brushPos + radius, true, [&](VoxelDispatchInvocationPars& pars) {
            VInt dx = pars.X - brushPos.x, dy = pars.Y - brushPos.y, dz = pars.Z - brushPos.z;
            VMask mask = dx * dx + dy * dy + dz * dz <= radius * radius;

            swr::simd::set_if(mask, pars.VoxelIds, voxel.Data);
            return mask != 0;
        });
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