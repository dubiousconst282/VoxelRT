#include <iostream>

#include <Havk/Havk.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include "Render/Renderer.h"
#include "TerrainGenerator.h"
#include "Brush.h"

class Application {
    glim::Camera _cam = {};
    glim::SettingStore _settings;

    std::shared_ptr<VoxelMap> _map;

    havk::DeviceContext* _ctx;
    std::unique_ptr<Renderer> _renderer;
    std::unique_ptr<TerrainGenerator> _terrainGen;
    std::unique_ptr<GBuffer> _gbuffer;

    BrushSession _brush;

public:
    Application(havk::DeviceContext* ctx) {
        _ctx = ctx;
        _map = std::make_shared<VoxelMap>();
        _gbuffer = std::make_unique<GBuffer>(ctx);

        try {
            _map->Deserialize("logs/voxels_2k_sponza.dat");
        } catch (std::exception& ex) {
            std::cout << "Failed to load voxel map cache: " << ex.what() << std::endl;

            auto model = "assets/models/Sponza/Sponza.gltf";
            // auto model = "../GraphicsAssets/Bistro.glb";
            // auto model = "logs/assets/models/ship_pinnace_4k/ship_pinnace_4k.gltf";

            _map->VoxelizeModel(model, glm::uvec3(0), glm::uvec3(4096));
            _map->Serialize("logs/voxels_2k_sponza.dat");
        }

        _map->Palette[241] = { .Color = { 0xA7, 0x51, 0x23 } };
        _map->Palette[245] = { .Color = { 70, 150, 64 } };
        _map->Palette[246] = { .Color = { 110, 150, 64 } };
        _map->Palette[247] = { .Color = { 138, 160, 72 } };
        _map->Palette[248] = { .Color = { 60, 130, 56 } };

        _map->Palette[252] = { .Color = { 255, 48, 48 }, .Emission = 0.8f };
        _map->Palette[253] = { .Color = { 48, 255, 48 }, .Emission = 0.8f };
        _map->Palette[254] = { .Color = { 48, 48, 255 }, .Emission = 0.8f };
        _map->Palette[255] = { .Color = { 255, 255, 255 }, .Emission = 10.0f };

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

    void Render(havk::Image* target, havk::CommandList& cmds) {
        ImGui::ShowMetricsWindow();

        _cam.Update();
        DrawBrushParams();

        while (true) {
            auto [sectorPos, sector] = _terrainGen->Poll();
            if (sector == nullptr) break;

            uint32_t sectorIdx = WorldSectorIndexer::GetIndex(sectorPos);
            _map->DirtyLocs[sectorIdx] = sector->GetAllocationMask();
            _map->Sectors[sectorIdx] = std::move(*sector);
        }

        ImGui::Begin("Settings");

        ImGui::SeparatorText("General");

        // static bool useVSync = true;
        // if (_settings.Checkbox("VSync", &useVSync)) {
        //     glfwSwapInterval(useVSync ? 1 : 0);
        // }

        static auto rendererId = RendererId::XBrickMap;
        ImGui::PushItemWidth(150);
        if (_settings.Combo("Renderer", &rendererId) || _renderer == nullptr) {
            _renderer = Renderer::Create(_ctx, _map, rendererId);
        }

        glm::uvec2 renderSize = _gbuffer->RenderSize;
        if (renderSize.x == 0 || renderSize.y == 0) {
            renderSize = glm::uvec2(target->Desc.Width, target->Desc.Height);
        }

        _settings.Combo("Debug Channel", &_gbuffer->DebugChannelView);

        char label[32];
        snprintf(label, sizeof(label), "%dx%d", renderSize.x, renderSize.y);
        if (ImGui::BeginCombo("Render Size", label)) {
            for (float scale = 0.25; scale <= 2.0; scale += 0.25) {
                uint32_t width = round(target->Desc.Width * scale);
                uint32_t height = round(target->Desc.Height * scale);
                bool isCurrent = renderSize.x == width && renderSize.y == height;

                snprintf(label, sizeof(label), "%dx%d", width, height);
                if (ImGui::Selectable(label, isCurrent)) {
                    renderSize = glm::uvec2(width, height);
                }
            }
            ImGui::EndCombo();
        }

        _settings.Slider("Light Bounces", &_gbuffer->NumLightBounces, 1, 0u, 5u);
        _settings.Slider("Denoiser Passes", &_gbuffer->NumDenoiserPasses, 1, 0u, 5u);
        _settings.Checkbox("Temporal AA", &_gbuffer->EnableTAA);

        ImGui::PushID(typeid(_renderer).hash_code());
        _renderer->DrawSettings(_settings);
        ImGui::PopID();

        if (auto gpur = dynamic_cast<GpuRenderer*>(_renderer.get())) {
            ImGui::SeparatorText("Stats");

            auto stats = gpur->LastFrameStats;
            
            double elapsedMs = (stats.Counters[FramePerfStats::Frame_EndTS] - stats.Counters[FramePerfStats::Frame_StartTS]) / 1000000.0;
            double raysPerSec = stats.Counters[FramePerfStats::RayCasts] * (1000.0 / elapsedMs);
            ImGui::Text("Render time: %.2fms (%.2fmrays/s)", elapsedMs, raysPerSec / 1000000.0);

            double avgItersPerRay = stats.Counters[FramePerfStats::TraversalIters] / (double)stats.Counters[FramePerfStats::RayCasts];
            double avgClocksPerIter = stats.Counters[FramePerfStats::ClocksPerRay] / (double)(stats.Counters[FramePerfStats::TraversalIters] + stats.Counters[FramePerfStats::RayCasts]);
            ImGui::Text("Traversal: %.2f iters/ray, %.2f clocks/iter", avgItersPerRay, avgClocksPerIter);

            std::vector<float> iterBins;
            bool hasHistogramData = false;
            for (uint32_t bin : stats.RayCastItersHistogram) {
                iterBins.push_back(bin / 1000.0);
                hasHistogramData |= bin > 0;
            }

            if (hasHistogramData) {
                ImGui::PlotHistogram("##TraversalItersHistogram", iterBins.data(), iterBins.size(), 0, nullptr, FLT_MAX, FLT_MAX,
                                     ImVec2(0, 80));
            }
        }

        ImGui::PopItemWidth();

        static uint32_t numBricks = 0;
        
        if (_map->DirtyLocs.size() > 0) {
            numBricks = 0;

            for (auto& [idx, sector] : _map->Sectors) {
                numBricks += (uint32_t)std::popcount(sector.GetAllocationMask());
            }
        }
        ImGui::Text("Bricks: %.1fK in %.1fK sectors (%d pending gen)", numBricks / 1000.0, _map->Sectors.size() / 1000.0, _terrainGen->GetNumPendingRequests());

        ImGui::SeparatorText("Camera");
        _settings.Input("Pos", &_cam.Position.x, 3, "%.1f");
        _settings.Drag("Rot", &_cam.Euler.x, 2, -3.141f, +3.141f, 0.1f, "%.1f");
        _settings.Drag("Speed", &_cam.MoveSpeed, 1, 0.5f, 1000.0f, 1.0f, "%.1f");
        _settings.Drag("FOV", &_cam.FieldOfView, 1, 10.0f, 120.0f, 0.5f, "%.1f deg");
        ImGui::End();

        _gbuffer->SetCamera(cmds, _cam, renderSize, _map->DirtyLocs.size() > 0);

        if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
            _map->MarkAllDirty();
        }
        _renderer->SyncMap(_cam, cmds);
        _renderer->RenderFrame(_cam, _gbuffer.get(), cmds);

        _gbuffer->Resolve(target, cmds);
    }

    void DrawBrushParams() {
        if (ImGui::Begin("Brush")) {
            _settings.Combo("Action", &_brush.Pars.Action);
            _settings.Drag("Radius", &_brush.Pars.Radius, 1, 1.0f, 200.0f);
            _settings.Drag("Probability", &_brush.Pars.Probability, 1, 0.0f, 1.0f, 0.005f);
            DrawPaletteEditor(_brush.Pars.Material.Data);
        }
        ImGui::End();

        static Voxel quickAccessPalette[2] = { 0, 255 };

        if (!ImGui::GetIO().WantCaptureKeyboard) {
            for (uint32_t i = 0; i < std::size(quickAccessPalette); i++) {
                if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i))) {
                    _brush.Pars.Material = quickAccessPalette[i];
                    break;
                }
            }
        }

        if (!ApplyBrush()) {
            _brush.Reset();
        }
    }

    void DrawPaletteEditor(uint8_t& selectedIdx) {
        ImGui::SeparatorText("Material Properties");
        {
            auto& material = _map->Palette[selectedIdx];
            auto color = material.GetColor();

            if (ImGui::ColorEdit3("Color", &color.x)) {
                material.SetColor(color);
            }
            ImGui::DragFloat("Emission", &material.Emission, 0.1f, 0.0f, 1000.0f);
            ImGui::DragScalar("Fuzziness", ImGuiDataType_U8, &material.MetalFuzziness);
        }

        int32_t cellSize = 32;
        int32_t numCols = std::max(ImGui::GetContentRegionAvail().x, 100.0f) / (cellSize + 1);
        int32_t numRows = (256 + numCols - 1) / numCols;

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));

        auto tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("##Palette", numCols, tableFlags)) {
            for (uint32_t i = 0; i < numCols; i++) {
                std::string text = std::to_string(i);
                ImGui::TableSetupColumn(text.data(), ImGuiTableColumnFlags_WidthFixed, cellSize);
            }

            for (int32_t row = 0; row < numRows; row++) {
                ImGui::TableNextRow();

                for (int32_t col = 0; col < numCols; col++) {
                    int32_t i = row * numCols + col;
                    if (i > 255) break;

                    ImGui::TableSetColumnIndex(col);
                    ImGui::PushID(i);

                    ImGui::SetItemTooltip("%d", i);

                    auto selFlags = 1 << 26;  // ImGuiSelectableFlags_NoPadWithHalfSpacing
                    if (ImGui::Selectable("", false, selFlags, ImVec2(32, 32))) {
                        selectedIdx = i;
                    }
                    auto drawList = ImGui::GetWindowDrawList();
                    ImVec2 bbMin = ImGui::GetItemRectMin() + ImVec2(1, 1);
                    ImVec2 bbMax = ImGui::GetItemRectMax() - ImVec2(1, 1);

                    if (selectedIdx == i) {
                        for (int32_t i = 0; i < cellSize; i += 8) {
                            drawList->AddLine(ImVec2(bbMin.x + i, bbMin.y), ImVec2(bbMin.x + i + 4, bbMin.y), 0xFFFFFFFF, 1.5f);
                            drawList->AddLine(ImVec2(bbMin.x + i, bbMax.y), ImVec2(bbMin.x + i + 4, bbMax.y), 0xFFFFFFFF, 1.5f);
                            drawList->AddLine(ImVec2(bbMin.x, bbMin.y + i), ImVec2(bbMin.x, bbMin.y + i + 4), 0xFFFFFFFF, 1.5f);
                            drawList->AddLine(ImVec2(bbMax.x, bbMin.y + i), ImVec2(bbMax.x, bbMin.y + i + 4), 0xFFFFFFFF, 1.5f);
                        }

                        static int32_t prevSelection = -1;
                        if (i != prevSelection) {
                            prevSelection = i;
                            ImGui::SetScrollHereY();
                        }
                    }
                    auto& material = _map->Palette[i];
                    drawList->AddRectFilled(bbMin + ImVec2(1, 1), bbMax - ImVec2(1, 1), ImU32(material.Color[0] << 0 | material.Color[1] << 8 | material.Color[2] << 16) | 0xFF000000u);

                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }

    bool ApplyBrush() {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        glm::vec2 mouseUV = glm::vec2(mousePos.x / displaySize.x, 1 - mousePos.y / displaySize.y) * 2.0f - 1.0f;

        glm::mat4 invProj = glm::inverse(_cam.GetProjMatrix() * _cam.GetViewMatrix(false));
        glm::vec4 nearPos = invProj * glm::vec4(mouseUV, 0, 1);
        glm::vec4 farPos = nearPos + glm::vec4(invProj[2]);
        glm::vec3 dir = glm::normalize(farPos * (1.0f / farPos.w));

        // Apply brush when LCtrl is pressed
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
            _brush.UpdatePosFromRay(*_map, _cam.ViewPosition, dir);
            _brush.Dispatch(*_map);

            return true;
        }
        // Update selected material to hit on double click
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse) {
            HitResult hit = _map->RayCast(_cam.ViewPosition, dir);
            Voxel newMat = hit.IsMiss() ? Voxel::CreateEmpty() : _map->Get(hit.VoxelPos);

            if (!newMat.IsEmpty()) {
                _brush.Pars.Material = newMat;
            }
        }
        return false;
    }
};

void InitImGui(havk::DeviceContext* ctx, GLFWwindow* window, VkDescriptorPool& descPool) {
    ImGui_ImplGlfw_InitForVulkan(window, true);

    VkDescriptorPoolSize descPoolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo descPoolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &descPoolSize,
    };
    vkCreateDescriptorPool(ctx->Device, &descPoolCI, nullptr, &descPool);

    ctx->Swapchain->Initialize();

    ImGui_ImplVulkan_InitInfo vkInfo = {
        .Instance = ctx->Instance,
        .PhysicalDevice = ctx->PhysicalDeviceInfo.Handle,
        .Device = ctx->Device,
        .QueueFamily = ctx->PhysicalDeviceInfo.MainQueueIdx,
        .Queue = ctx->MainQueue,
        .DescriptorPool = descPool,
        .MinImageCount = std::max(2u, ctx->Swapchain->SurfaceCaps.minImageCount),
        .ImageCount = ctx->Swapchain->GetImageCount(),
        .MSAASamples = VK_SAMPLE_COUNT_1_BIT,

        .PipelineCache = ctx->PipeBuilder->Cache,
        .UseDynamicRendering = true,
        .PipelineRenderingCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &ctx->Swapchain->SurfaceFormat.format,
        },
    };
    ImGui_ImplVulkan_Init(&vkInfo);
}

int main(int argc, char** args) {
    #if _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
    #else
    setlinebuf(stdout);
    #endif
    
    if (!glfwInit()) return -1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "VoxelRT", NULL, NULL);

    if (!window) {
        glfwTerminate();
        return -1;
    }

    havk::DeviceContextPtr ctx = havk::Create({
        .Window = window,
        .ShaderBasePath = "src/VoxelRT/Shaders/",
        .EnableShaderHotReload = true,
        .EnableDebugLayers = true,
    });
    std::cout << "Device: " << ctx->PhysicalDeviceInfo.Props.deviceName << std::endl;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "logs/imgui.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    io.Fonts->AddFontFromFileTTF("assets/Roboto-Medium.ttf", 18.0f);

    ImGui::StyleColorsDark();

    {
        VkDescriptorPool igDescPool;
        InitImGui(ctx.get(), window, igDescPool);

        Application app(ctx.get());

        while (!glfwWindowShouldClose(window)) {
            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);

            if (display_w == 0 || display_h == 0) {
                glfwWaitEvents();
                continue;
            }

            auto [image, cmdList] = ctx->Swapchain->AcquireImage();

            // Poll events after AcquireImage() to minimize input lag, because
            // it may block for too long and make events stale
            glfwPollEvents();

            // Start the Dear ImGui frame
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // Rendering
            app.Render(image, cmdList);

            ImGui::Render();

            static bool showUI = true;
            showUI ^= ImGui::IsKeyPressed(ImGuiKey_F1);

            if (showUI) {
                cmdList.BeginRendering({ .Attachments = { { .Target = image, .LoadOp = VK_ATTACHMENT_LOAD_OP_LOAD } } }, true);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdList.Buffer);
                cmdList.EndRendering();
            }

            // Present
            ctx->Swapchain->Present();
            ctx->Tick();
        }
        ctx->WaitDeviceIdle();
        ImGui_ImplVulkan_Shutdown();
        vkDestroyDescriptorPool(ctx->Device, igDescPool, nullptr);
    }

    ctx.reset();
    glfwTerminate();

    return 0;
}