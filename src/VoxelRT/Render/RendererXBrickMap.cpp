#include "Renderer.h"
#include "BrickSlotAllocator.h"

// NOTE: keep in sync with VoxelMap.slang
// TODO: implement this via specialization constants
static constexpr auto SectorSize = MaskIndexer::Size * BrickIndexer::Size;
static constexpr auto ViewSize = glm::uvec2(4096, 2048) / glm::uvec2(SectorSize);
static constexpr uint32_t NumViewSectors = ViewSize.x * ViewSize.x * ViewSize.y;

struct GpuVoxelStorage {
    uint64_t Palette[256];
    uint32_t BaseSlots[NumViewSectors];
    uint64_t BrickMasks[NumViewSectors];
    uint64_t SectorMasks[NumViewSectors / 64];
    uint8_t BrickVoxelData[];
};
struct GpuVoxelMap {
    VkDeviceAddress Storage;         // MainStorageBlock*
    VkDeviceAddress VoxelOccupancy;  // uint64_t*
};

struct GpuSectorUpdateRecord {
    uint32_t SectorIdx;
    uint32_t AllocInfo;
    uint64_t AllocMask;
    uint64_t DirtyMask;
    VkDeviceAddress SourceData;
};

struct GpuStorageManager {
    havk::DeviceContext* Device;
    havk::BufferPtr StorageBuffer;
    havk::BufferPtr OccupancyStorage;

    havk::ComputePipelinePtr UpdateShader;

    BrickSlotAllocator SlotAllocator = { ViewSize };
    glm::ivec3 SectorViewPos;
    
    GpuStorageManager(havk::DeviceContext* ctx) {
        Device = ctx;
        UpdateShader = ctx->PipeBuilder->CreateCompute("Backends/XBrickMap/UpdateMap.slang");

        size_t storageSize = 1024 * 1024 * 1024 * 2.0;  // 2GB to start with...

        StorageBuffer = Device->CreateBuffer({
            .Size = sizeof(GpuVoxelStorage) + storageSize,
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        });
        OccupancyStorage = Device->CreateBuffer({
            .Size = storageSize / 8,
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        });
    }

    void SyncBuffers(VoxelMap& map, havk::CommandList& cmds) {
        const size_t MaxUpdateDataSize = 1024 * 1024 * 64;
        const size_t MaxUpdateSectors = 32768;

        std::vector<std::tuple<uint32_t, uint64_t>> batch;
        uint32_t updateSize = 0;
        uint64_t maxAddress = 0;

        // Allocate slots for dirty bricks
        for (auto iter = map.DirtyLocs.begin(); iter != map.DirtyLocs.end(); ) {
            auto [sectorIdx, dirtyMask] = *iter;
            map.DirtyLocs.erase(iter++);

            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            SectorAllocInfo* sectorAlloc = SlotAllocator.GetSector(sectorPos);
            if (sectorAlloc == nullptr) continue;

            if (map.Sectors.contains(sectorIdx)) {
                Sector& sector = map.Sectors[sectorIdx];
                uint64_t allocMask = sector.GetAllocationMask();
                uint32_t levelOfDetail = GetSectorLOD(sectorPos);

                SlotAllocator.Reserve(sectorAlloc, allocMask, levelOfDetail);
            } else {
                SlotAllocator.Reserve(sectorAlloc, 0, 0);
                dirtyMask = 0;
            }

            if (dirtyMask != 0) {
                uint32_t dataSize = sectorAlloc->GetDataSize();
                updateSize += dataSize;
                maxAddress = std::max(maxAddress, sectorAlloc->GetBaseAddress() + dataSize);
            }
            batch.push_back({ sectorIdx, dirtyMask });
            updateSize += sizeof(GpuSectorUpdateRecord);

            if (batch.size() >= MaxUpdateSectors || updateSize >= MaxUpdateDataSize) break;
        }

        SyncPalette(map, cmds);
        if (batch.empty()) return;

        assert(maxAddress < StorageBuffer->Size); // TODO

        // Initialize staging buffer
        auto stagingBuffer = Device->CreateBuffer({
            .Size = updateSize,
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        });
        auto updateRecords = (GpuSectorUpdateRecord*)stagingBuffer->MappedData;
        auto updateData = (uint8_t*)&updateRecords[batch.size()];
        assert(uintptr_t(updateData) % 16 == 0);  // must be aligned

        for (auto [sectorIdx, dirtyMask] : batch) {
            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            auto sectorAlloc = SlotAllocator.GetSector(sectorPos);

            *updateRecords++ = {
                .SectorIdx = GetLinearIndex(sectorPos, ViewSize.x, ViewSize.y),
                .AllocInfo = sectorAlloc->GetPackedHeader(),
                .AllocMask = sectorAlloc->AllocMask,
                .DirtyMask = dirtyMask,
                .SourceData = stagingBuffer->DeviceAddress + uint64_t(updateData - (uint8_t*)stagingBuffer->MappedData),
            };
            //printf("Update %d %d %d  %08x %d %d\n", sectorPos.x*32,sectorPos.y*32,sectorPos.z*32, sectorAlloc->GetBaseAddress(), sectorAlloc->GetDataSize(), std::popcount(dirtyMask));
            //fflush(stdout);

            if (sectorAlloc->AllocMask != 0) {
                Sector& sector = map.Sectors[sectorIdx];

                // TODO: support updates at brick level
                uint64_t updateMask = sectorAlloc->AllocMask;

                for (uint32_t brickIdx : BitIter(updateMask)) {
                    Brick* brick = sector.GetBrick(brickIdx);

                    brick->GenerateLOD((Voxel*)updateData, sectorAlloc->LevelOfDetail);
                    updateData += SectorAllocInfo::GetBrickStride(sectorAlloc->LevelOfDetail);
                }
            }
        }
        stagingBuffer->Flush();

        // Dispatch sync shader
        struct UpdateParams {
            uint32_t NumRecords;
            VkDeviceAddress Records;
            GpuVoxelMap Map;
        };
        UpdateParams updatePars = {
            .NumRecords = uint32_t(batch.size()),
            .Records = cmds.GetDeviceAddress(*stagingBuffer, havk::UseBarrier::ComputeRead),
            .Map = {
                .Storage = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeReadWrite),
                .VoxelOccupancy = cmds.GetDeviceAddress(*OccupancyStorage, havk::UseBarrier::ComputeReadWrite),
            },
        };
        UpdateShader->Dispatch(cmds, { (updatePars.NumRecords + 63) / 64, 1, 1 }, updatePars);
    }

    uint32_t GetSectorLOD(glm::ivec3 pos) {
        return 0;
        int32_t dist = 0;
        dist += abs(pos.x - SectorViewPos.x);
        dist += abs(pos.y - SectorViewPos.y);
        dist += abs(pos.z - SectorViewPos.z);
        dist /= 2;
        // log2
        dist = int32_t(std::bit_width(uint32_t(dist))) - 3;
        return uint32_t(glm::clamp(dist, 0, 3));
    }

private:
    uint64_t _palette[256] = {};

    void SyncPalette(VoxelMap& map, havk::CommandList& cmds) {
        uint32_t changedMin = UINT_MAX, changedMax = 0;

        for (uint32_t i = 0; i < 256; i++) {
            uint64_t encoded = map.Palette[i].GetEncoded();

            if (_palette[i] != encoded) {
                _palette[i] = encoded;
                changedMin = std::min(changedMin, i);
                changedMax = i;
            }
        }
        if (changedMin == UINT_MAX) return;

        uint32_t numChanges = changedMax - changedMin + 1;
        cmds.UpdateBuffer(*StorageBuffer, offsetof(GpuVoxelStorage, Palette[changedMin]), sizeof(uint64_t) * numChanges, &_palette[changedMin]);
    }
};

struct RendererBrickMap : public GpuRenderer {
    std::unique_ptr<GpuStorageManager> Storage;
    havk::ComputePipelinePtr RenderShader;

    RendererBrickMap(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : GpuRenderer(ctx, map) {
        Storage = std::make_unique<GpuStorageManager>(ctx);
        RenderShader = ctx->PipeBuilder->CreateCompute("Backends/Render.slang", { .PrepDefs = { { "BACKEND_ID", "1" }}});

        _map->MarkAllDirty();
    }

    virtual void RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds);
    virtual void DrawSettings(glim::SettingStore& settings);
};

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::XBrickMap>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererBrickMap>(ctx, map);
}

struct RenderDispatchParams {
    GpuVoxelMap Map;
    VkDeviceAddress GBuffer;  // GBufferUniforms*

    uint32_t MaxBounces;
    havk::ImageHandle StbnTexture;
    havk::ImageHandle SkyTexture;
};

void RendererBrickMap::RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds) {
    bool worldChanged = _map->DirtyLocs.size() > 0;

    // Sync buffers
    if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
        _map->MarkAllDirty();
        Storage->SlotAllocator = BrickSlotAllocator(ViewSize);
    }
    Storage->SectorViewPos = glm::floor(cam.ViewPosition / glm::dvec3(SectorSize));
    Storage->SyncBuffers(*_map, cmds);

    glm::uvec2 renderSize = glm::round(glm::vec2(target->Desc.Width, target->Desc.Height) * _renderScale);
    _gbuffer->SetCamera(cmds, cam, renderSize, worldChanged);

    RenderDispatchParams renderPars = {
        .Map = {
            .Storage = cmds.GetDeviceAddress(*Storage->StorageBuffer, havk::UseBarrier::ComputeRead),
            .VoxelOccupancy = cmds.GetDeviceAddress(*Storage->OccupancyStorage, havk::UseBarrier::ComputeRead),
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
void RendererBrickMap::DrawSettings(glim::SettingStore& settings) {
    GpuRenderer::DrawSettings(settings);
    
    if (Storage->StorageBuffer != nullptr) {
        ImGui::Text("Storage: %.1fMB/%.1fMB (%zu free ranges)",
                    (sizeof(GpuVoxelStorage) + Storage->SlotAllocator.Arena.NumAllocated * SectorAllocInfo::PageSize) / 1048576.0,
                    Storage->StorageBuffer->Size / 1048576.0,
                    Storage->SlotAllocator.Arena.FreeRanges.size());
    }
}