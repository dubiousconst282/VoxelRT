#include "Renderer.h"

namespace {

// NOTE: keep in sync with shaders
struct GpuMapStorage {
    static constexpr glm::uvec2 kGridSize = glm::uvec2(2048, 1024);
    static constexpr uint64_t kNumVoxels = uint64_t(kGridSize.x) * kGridSize.y * kGridSize.x;
    static constexpr uint32_t kBrickSize = 8, kNumBricks = kNumVoxels / (kBrickSize*kBrickSize*kBrickSize);

    uint64_t BrickMasks[kNumBricks / 64];
    uint64_t VoxelMasks[kNumVoxels / 64];
};
struct GpuVoxelMap {
    VkDeviceAddress Storage;         // GpuStorage*
};
struct GpuBrickUpdateRecord {
    glm::ivec3 BrickPos;
    uint64_t Data[Brick::NumVoxels / 64];
};

struct RendererFlatDDA : public GpuRenderer {
    havk::BufferPtr StorageBuffer;

    havk::ComputePipelinePtr UpdateShader;

    RendererFlatDDA(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map, RendererId type) : GpuRenderer(ctx, map, type) {
        UpdateShader = ctx->PipeBuilder->CreateCompute("Backends/FlatDDA/UpdateMap.slang");

        _map->MarkAllDirty();
    }

    void RenderFrame(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        // Sync buffers
        if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
            _map->MarkAllDirty();
        }
        SyncMap(cmds);

        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap {
            .Storage = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeRead),
        });
    }

    void SyncMap(havk::CommandList& cmds);
};

void RendererFlatDDA::SyncMap(havk::CommandList& cmds) {
    if (StorageBuffer == nullptr) {
        StorageBuffer = _ctx->CreateBuffer({
            .Size = sizeof(GpuMapStorage),
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        });
        cmds.Fill(*StorageBuffer, 0);
    }
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
            Brick::GetOccupancyMask(brick, record.Data);
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
    UpdateShader->Dispatch(cmds, { uint32_t(batch.size() + 63) / 64, 1, 1 }, DispatchParams {
        .NumRecords = uint32_t(batch.size()),
        .Records = cmds.GetDeviceAddress(*stagingBuffer, havk::UseBarrier::ComputeRead),
        .Map = {
            .Storage = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeReadWrite)
        }
    });
}

};  // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::PlainDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererFlatDDA>(ctx, map, RendererId::PlainDDA);
}
template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::MultiDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererFlatDDA>(ctx, map, RendererId::MultiDDA);
}