#include "Renderer.h"
#include "BrickSlotAllocator.h"

// Annoynmous namespace needed to avoid struct naming collisions with other CUs.
// These *do not* manifest at compile time but templates will only be instantiated
// once and lead to very confusing behavior like push constant sizes being wrong.
namespace {

// NOTE: keep in sync with VoxelMap.slang
// TODO: implement this via specialization constants
static constexpr auto SectorSize = MaskIndexer::Size * BrickIndexer::Size;
static constexpr auto ViewSize = glm::uvec2(4096, 2048) / glm::uvec2(SectorSize);
static constexpr uint32_t NumViewSectors = ViewSize.x * ViewSize.x * ViewSize.y;

struct GpuVoxelStorage {
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
    }

    bool SyncBuffers(VoxelMap& map, havk::CommandList& cmds) {
        if (StorageBuffer == nullptr) {
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
            cmds.Fill(*StorageBuffer, 0, 0, sizeof(GpuVoxelStorage));
        }
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

        if (batch.empty()) return false;

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
        UpdateShader->Dispatch(cmds, { uint32_t(batch.size() + 63) / 64, 1, 1 }, UpdateParams {
            .NumRecords = uint32_t(batch.size()),
            .Records = cmds.GetDeviceAddress(*stagingBuffer, havk::UseBarrier::ComputeRead),
            .Map = {
                .Storage = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeReadWrite),
                .VoxelOccupancy = cmds.GetDeviceAddress(*OccupancyStorage, havk::UseBarrier::ComputeReadWrite),
            },
        });
        return true;
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
};

struct RendererBrickMap : public GpuRenderer {
    std::unique_ptr<GpuStorageManager> Storage;

    RendererBrickMap(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : GpuRenderer(ctx, map, RendererId::XBrickMap) {
        Storage = std::make_unique<GpuStorageManager>(ctx);
        _map->MarkAllDirty();
    }

    void RenderFrame(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap {
            .Storage = cmds.GetDeviceAddress(*Storage->StorageBuffer, havk::UseBarrier::ComputeRead),
            .VoxelOccupancy = cmds.GetDeviceAddress(*Storage->OccupancyStorage, havk::UseBarrier::ComputeRead),
        });
    }

    bool SyncMap(glim::Camera& cam, havk::CommandList& cmds) override {
        if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
            Storage->SlotAllocator = BrickSlotAllocator(ViewSize);
        }
        Storage->SectorViewPos = glm::floor(cam.ViewPosition / glm::dvec3(SectorSize));
        return Storage->SyncBuffers(*_map, cmds);
    }
    
    void DrawSettings(glim::SettingStore& settings) override {
        GpuRenderer::DrawSettings(settings);

        if (Storage->StorageBuffer != nullptr) {
            ImGui::Text("Storage: %.1fMB/%.1fMB (%zu free ranges)",
                        (sizeof(GpuVoxelStorage) + Storage->SlotAllocator.Arena.NumAllocated * SectorAllocInfo::PageSize) / 1048576.0,
                        Storage->StorageBuffer->Size / 1048576.0, Storage->SlotAllocator.Arena.FreeRanges.size());
        }
    }
};

}; // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::XBrickMap>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererBrickMap>(ctx, map);
}