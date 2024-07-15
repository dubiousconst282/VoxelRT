#include "Renderer.h"
#include "BrickSlotAllocator.h"

// NOTE: keep in sync with VoxelMap.slang
// TODO: implement this via specialization constants
static constexpr auto SectorSize = MaskIndexer::Size * BrickIndexer::Size;
static constexpr auto ViewSize = glm::uvec2(4096, 2048) / glm::uvec2(SectorSize);
static constexpr uint32_t NumViewSectors = ViewSize.x * ViewSize.x * ViewSize.y;

static_assert(std::endian::native == std::endian::little);
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

struct GpuVoxelStorageManager {
    havk::DeviceContext* Device;
    havk::BufferPtr StorageBuffer;
    havk::BufferPtr OccupancyStorage;

    havk::ComputePipelinePtr UpdateShader;

    BrickSlotAllocator SlotAllocator = { ViewSize };
    
    GpuVoxelStorageManager(havk::DeviceContext* ctx) {
        Device = ctx;
        UpdateShader = ctx->PipeBuilder->CreateCompute("UpdateMap.slang");

        size_t voxelStorageSize = 1024 * 1024 * 1024 * 2.0;  // 2GB to start with...

        StorageBuffer = Device->CreateBuffer({
            .Size = sizeof(GpuVoxelStorage) + voxelStorageSize,
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        });
        OccupancyStorage = Device->CreateBuffer({
            .Size = voxelStorageSize / 8,
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

            if (dirtyMask != 0) {
                assert(map.Sectors.contains(sectorIdx));
                Sector& sector = map.Sectors[sectorIdx];

                // TODO: support updates at brick level
                uint64_t updateMask = sector.GetAllocationMask();

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

GpuRenderer::GpuRenderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    _ctx = ctx;
    _map = std::move(map);
    _storage = std::make_unique<GpuVoxelStorageManager>(ctx);

    _renderShader = ctx->PipeBuilder->CreateCompute("VoxelRender.slang");

    _gbuffer = std::make_unique<GBuffer>(ctx);
    _gbuffer->NumDenoiserPasses = 0;

    _blueNoiseTex = havk::Image::LoadFile(ctx, "assets/bluenoise/stbn_vec2_2Dx1D_128x128x64_combined.png",
                                          VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_R8G8_UINT, 1);
    _skyboxTex = havk::Image::LoadFilePanoramaToCube(ctx, "assets/skyboxes/evening_road_01_puresky_4k.hdr");

    _map->MarkAllDirty();
}
GpuRenderer::~GpuRenderer() = default;

struct RenderDispatchParams {
    GpuVoxelMap Map;
    VkDeviceAddress GBuffer;  // GBufferUniforms*

    uint32_t MaxBounces;
    havk::ImageHandle StbnTexture;
    havk::ImageHandle SkyTexture;
};

void GpuRenderer::RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds) {
    bool worldChanged = _map->DirtyLocs.size() > 0;

    // Sync buffers
    if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
        _map->MarkAllDirty();
        _storage->SlotAllocator = BrickSlotAllocator(ViewSize);
    }
    _storage->SyncBuffers(*_map, cmds);

    glm::uvec2 renderSize = glm::round(glm::vec2(target->Desc.Width, target->Desc.Height) * _renderScale);
    _gbuffer->SetCamera(cmds, cam, renderSize, worldChanged);

    RenderDispatchParams renderPars = {
        .Map = {
            .Storage = cmds.GetDeviceAddress(*_storage->StorageBuffer, havk::UseBarrier::ComputeRead),
            .VoxelOccupancy = cmds.GetDeviceAddress(*_storage->OccupancyStorage, havk::UseBarrier::ComputeRead),
        },
        .GBuffer = cmds.GetDeviceAddress(*_gbuffer->UniformBuffer, havk::UseBarrier::ComputeRead),
        .MaxBounces = _numLightBounces,
        .StbnTexture = _blueNoiseTex->DescriptorHandle,
        .SkyTexture = _skyboxTex->DescriptorHandle,
    };
    uint32_t groupsX = (renderSize.x + 7) / 8, groupsY = (renderSize.y + 7) / 8;
    _renderShader->Dispatch(cmds, { groupsX, groupsY, 1 }, renderPars);

    _gbuffer->Resolve(target, cmds);
}
void GpuRenderer::DrawSettings(glim::SettingStore& settings) {
    ImGui::SeparatorText("Renderer##GPU");

    ImGui::PushItemWidth(150);
    settings.Combo("Debug Channel", &_gbuffer->DebugChannelView);
    settings.Slider("Light Bounces", &_numLightBounces, 1, 0u, 5u);
    settings.Slider("Denoiser Passes", &_gbuffer->NumDenoiserPasses, 1, 0u, 5u);

    char label[32];
    sprintf(label, "%.3gx (%dx%d)", _renderScale, _gbuffer->RenderSize.x, _gbuffer->RenderSize.y);
    settings.Slider("Render Scale", &_renderScale, 1, 0.25f, 4.0f, label);
    _renderScale = std::round(_renderScale / 0.25f) * 0.25f;

    ImGui::PopItemWidth();
    
    ImGui::Separator();
    _frameTime.Draw("Frame Time");

    auto totalIters = 1;
    if (_gbuffer->AlbedoTex != nullptr) {
        double frameMs, frameDevMs;
        _frameTime.GetElapsedMs(frameMs, frameDevMs);

        uint32_t numPixels = _gbuffer->RenderSize.x * _gbuffer->RenderSize.y;
        uint32_t raysPerPixel = (_numLightBounces + 1) * 2; // sun
        double raysPerSec = numPixels * raysPerPixel * (1000 / frameMs);

        ImGui::Text("Rays/sec: %.2fM | Steps: %.3fM", raysPerSec / 1000000.0, totalIters / 1000000.0);
    }

    if (_storage->StorageBuffer != nullptr) {
        ImGui::Text("Storage: %.1fMB/%.1fMB (%zu free ranges)",
                    (sizeof(GpuVoxelStorage) + _storage->SlotAllocator.Arena.NumAllocated * SectorAllocInfo::PageSize) / 1048576.0,
                    _storage->StorageBuffer->Size / 1048576.0,
                    _storage->SlotAllocator.Arena.FreeRanges.size());

        uint32_t numBricks = 0;
        for (auto & sector : _map->Sectors) {
            numBricks += (uint32_t)std::popcount(sector.second.GetAllocationMask());
        }
        ImGui::Text("Bricks: %.1fK in %.1fK sectors", numBricks / 1000.0, _map->Sectors.size() / 1000.0);
    }
}