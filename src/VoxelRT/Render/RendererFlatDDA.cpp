#include "Renderer.h"

// NOTE: keep in sync with shaders
struct GpuMapStorage {
    static constexpr auto kGridSize = glm::uvec2(2048, 1024);
    static constexpr auto kNumVoxels = uint64_t(kGridSize.x) * kGridSize.y * kGridSize.x;

    uint64_t VoxelMasks[kNumVoxels / 64];
};
struct GpuVoxelMap {
    VkDeviceAddress Storage;         // GpuStorage*
};
struct GpuBrickUpdateRecord {
    glm::ivec3 BrickPos;
    uint64_t Data[(8 * 8 * 8) / 64];
};

struct RendererFlatDDA : public GpuRenderer {
    havk::BufferPtr StorageBuffer;

    havk::ComputePipelinePtr RenderShader;
    havk::ComputePipelinePtr UpdateShader;

    RendererFlatDDA(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map, bool multiDDA) : GpuRenderer(ctx, map) {
        RenderShader = ctx->PipeBuilder->CreateCompute("Backends/Render.slang", { .PrepDefs = { { "BACKEND_ID", (multiDDA ? "3" : "2" )} } });
        UpdateShader = ctx->PipeBuilder->CreateCompute("Backends/FlatDDA/UpdateMap.slang");

        _map->MarkAllDirty();

        StorageBuffer = ctx->CreateBuffer({
            .Size = sizeof(GpuMapStorage),
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
        });
    }

    virtual void RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds);
    virtual void DrawSettings(glim::SettingStore& settings);

    void SyncMap(havk::CommandList& cmds);
};

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::PlainDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererFlatDDA>(ctx, map, false);
}
template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::MultiDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererFlatDDA>(ctx, map, true);
}

void RendererFlatDDA::SyncMap(havk::CommandList& cmds) {
    #ifdef NDEBUG
    const size_t MaxUpdateDataSize = 1024 * 1024 * 16;
    #else
    const size_t MaxUpdateDataSize = 1024 * 1024 * 0.5;
    #endif
    std::vector<GpuBrickUpdateRecord> batch;

    for (auto iter = _map->DirtyLocs.begin(); iter != _map->DirtyLocs.end();) {
        auto [sectorIdx, dirtyMask] = *iter;
        _map->DirtyLocs.erase(iter++);

        glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
        Sector* sector = nullptr;

        if (_map->Sectors.contains(sectorIdx)) {
            sector = &_map->Sectors[sectorIdx];
        } else {
            dirtyMask = ~0ull;
        }

        for (uint32_t brickIdx : BitIter(dirtyMask)) {
            Brick* brick = sector ? sector->GetBrick(brickIdx) : nullptr;
            auto& record = batch.emplace_back();
            record.BrickPos = sectorPos * 4 + MaskIndexer::GetPos(brickIdx);

            if (brick) {
                for (uint32_t i = 0; i < BrickIndexer::MaxArea; i += 64) {
                    uint64_t mask = 0;

                    for (uint32_t j = 0; j < 64; j++) {
                        mask |= uint64_t(brick->Data[i + j].Data != 0) << j;
                    }
                    record.Data[i / 64] = mask;
                }
            } else {
                memset(record.Data, 0, sizeof(record.Data));
            }
        }

        if (batch.size() * sizeof(GpuBrickUpdateRecord) >= MaxUpdateDataSize) break;
    }
    if (batch.size() == 0) return;

    auto stagingBuffer = _ctx->CreateBuffer({
        .Size = batch.size() * sizeof(GpuBrickUpdateRecord),
        .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    });
    stagingBuffer->Write(batch.data(), 0, stagingBuffer->Size);

    struct DispatchParams {
        uint NumRecords;
        VkDeviceAddress Records;
        GpuVoxelMap Map;
    };
    DispatchParams updatePars = {
        .NumRecords = uint32_t(batch.size()),
        .Records = cmds.GetDeviceAddress(*stagingBuffer, havk::UseBarrier::ComputeRead),
        .Map = {
            .Storage = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeReadWrite)
        }
    };
    UpdateShader->Dispatch(cmds, { (updatePars.NumRecords + 63) / 64, 1, 1 }, updatePars);
}

void RendererFlatDDA::RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds) {
    bool worldChanged = _map->DirtyLocs.size() > 0;

    // Sync buffers
    if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
        _map->MarkAllDirty();
    }
    SyncMap(cmds);

    glm::uvec2 renderSize = glm::round(glm::vec2(target->Desc.Width, target->Desc.Height) * _renderScale);
    _gbuffer->SetCamera(cmds, cam, renderSize, worldChanged);

    struct RenderParams {
        GpuVoxelMap Map;
        VkDeviceAddress GBuffer;  // GBufferUniforms*

        uint32_t MaxBounces;
        havk::ImageHandle StbnTexture;
        havk::ImageHandle SkyTexture;
    };
    RenderParams renderPars = {
        .Map = {
            .Storage = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeRead),
        },
        .GBuffer = cmds.GetDeviceAddress(*_gbuffer->UniformBuffer, havk::UseBarrier::ComputeRead),
        .MaxBounces = _numLightBounces,
        .StbnTexture = _blueNoiseTex->DescriptorHandle,
        .SkyTexture = _skyboxTex->DescriptorHandle,
    };
    uint32_t groupsX = (renderSize.x + 7) / 8, groupsY = (renderSize.y + 7) / 8;
    RenderShader->Dispatch(cmds, { groupsX, groupsY, 1 }, renderPars);

    _gbuffer->Resolve(target, cmds);
}
void RendererFlatDDA::DrawSettings(glim::SettingStore& settings) {
    GpuRenderer::DrawSettings(settings);
}