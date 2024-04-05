#include <iostream>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <ImGuizmo.h>

#include <SwRast/Texture.h>

#include "Renderer.h"

#include "TerrainGenerator.h"

class Application {
    glim::Camera _cam = {};
    glim::SettingStore _settings;

    std::shared_ptr<VoxelMap> _map;

    std::unique_ptr<ogl::ShaderLib> _shaderLib;
    std::unique_ptr<Renderer> _renderer;
    std::unique_ptr<TerrainGenerator> _terrainGen;

public:
    Application() {
        ogl::EnableDebugCallback();

        _shaderLib = std::make_unique<ogl::ShaderLib>("src/VoxelRT/Shaders/", true);

        _map = std::make_shared<VoxelMap>();

        try {
            _map->Deserialize("logs/voxels_2k.dat");
        } catch (std::exception& ex) {
            std::cout << "Failed to load voxel map cache: " << ex.what() << std::endl;

            auto model = glim::Model("assets/models/Sponza/Sponza.gltf");
            // auto model = glim::Model("../SwRastCPP/logs/assets/models/Bistro_GLTF/BistroExterior.gltf");
            // auto model = glim::Model("logs/assets/models/ship_pinnace_4k/ship_pinnace_4k.gltf");
            // auto model = glim::Model("logs/assets/models/DamagedHelmet/DamagedHelmet.gltf");

            _map->VoxelizeModel(model, glm::uvec3(0), glm::uvec3(2048));

            _map->Serialize("logs/voxels_2k.dat");
        }

        _map->Palette[251] = Material::CreateDiffuse({ 0.3, 0.6, 0.25 }, 0.0f);
        _map->Palette[252] = Material::CreateDiffuse({ 1, 0.2, 0.2 }, 0.9f);
        _map->Palette[253] = Material::CreateDiffuse({ 0.2, 1, 0.2 }, 0.9f);
        _map->Palette[254] = Material::CreateDiffuse({ 0.2, 0.2, 1 }, 0.9f);
        _map->Palette[255] = Material::CreateDiffuse({ 1, 1, 1 }, 3.0f);

        _terrainGen = std::make_unique<TerrainGenerator>(_map);
        for (size_t y = 0; y < 7; y++) {
            for (size_t z = 0; z < 24; z++) {
                for (size_t x = 0; x < 24; x++) {
                    _terrainGen->RequestSector(glm::ivec3(x, y, z));
                }
            }
        }
        _terrainGen->RequestSector(glm::ivec3(1, 3, 1));

        _map->Set(glm::ivec3(3, 5, 3), Voxel::Create(255));
        _map->Set(glm::ivec3(4, 6, 3), Voxel::Create(254));
        _map->Set(glm::ivec3(10, 6, 3), Voxel::Create(254));

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

        while (true) {
            auto [sectorPos, sector] = _terrainGen->Poll();
            if (sector == nullptr) break;

            uint32_t sectorIdx = WorldSectorIndexer::GetIndex(sectorPos);
            _map->DirtyLocs[sectorIdx] = sector->GetAllocationMask();
            _map->Sectors[sectorIdx] = std::move(*sector);
        }

        ImGui::Begin("Settings");

        ImGui::SeparatorText("General");

        static bool useVSync = true;
        if (_settings.Checkbox("VSync", &useVSync)) {
            glfwSwapInterval(useVSync ? 1 : 0);
        }

        static bool useCpuRenderer = false;
        if (_settings.Checkbox("Use CPU Renderer", &useCpuRenderer) || _renderer == nullptr) {
            if (useCpuRenderer) {
                _renderer = std::make_unique<CpuRenderer>(*_shaderLib, _map);
            } else {
                _renderer = std::make_unique<GpuRenderer>(*_shaderLib, _map);
            }
        }

        _renderer->DrawSettings(_settings);

        ImGui::Text("Total Sectors: %zu (%d pending gen)", _map->Sectors.size(), _terrainGen->GetNumPendingRequests());

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
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || 
            (!erase && (hitDist > brushDist || _map->Get(hitPos).Data != voxel.Data))
        ) {
            brushDist = hitDist;
        }

        glm::ivec3 brushPos = glm::floor(_cam.ViewPosition + glm::dvec3(dir) * brushDist);
        int32_t radius = erase ? 160 : 30;

        _map->RegionDispatchSIMD(brushPos - radius, brushPos + radius, true, [&](VoxelDispatchInvocationPars& pars) {
            VInt dx = pars.X - brushPos.x, dy = pars.Y - brushPos.y, dz = pars.Z - brushPos.z;
            VMask mask = dx * dx + dy * dy + dz * dz <= radius * radius;

            simd::set_if(mask, pars.VoxelIds, voxel.Data);
            return simd::any(mask);
        });
    }
};

int main(int argc, char** args) {
    if (!glfwInit()) return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, 1);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "VoxelRT", NULL, NULL);
    //GLFWwindow* window = glfwCreateWindow(960, 540, "VoxelRT", NULL, NULL);
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