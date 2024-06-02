#include "Renderer.h"
#include "BrickSlotAllocator.h"

// Keep in sync with VoxelMap.slang, or implement specialization constants
static constexpr auto SectorSize = MaskIndexer::Size * BrickIndexer::Size;
static constexpr auto ViewSize = glm::uvec2(4096, 2048) / glm::uvec2(SectorSize);
static constexpr uint32_t NumViewSectors = ViewSize.x * ViewSize.x * ViewSize.y;

static_assert(std::endian::native == std::endian::little);
struct GpuVoxelStorage {
    uint64_t Palette[256];
    uint32_t BaseSlots[NumViewSectors];
    uint64_t BrickMasks[NumViewSectors];
    uint64_t SectorMasks[NumViewSectors / 64];
    Brick BrickVoxelData[];
};
struct GpuVoxelMap {
    VkDeviceAddress Storage;         // MainStorageBlock*
    VkDeviceAddress VoxelOccupancy;  // uint64_t*
};

struct GpuVoxelStorageManager {
    havk::DeviceContext* Device;
    havk::BufferPtr StorageBuffer;
    havk::BufferPtr OccupancyStorage;

    havk::ComputePipelinePtr BuildOccupancyShader;

    BrickSlotAllocator SlotAllocator = { ViewSize };
    glm::ivec3 ViewOffset; // world view offset in sector scale
    uint64_t SectorOccupancy[NumViewSectors / 64] = {};  // Occupancy masks at sector level (host copy)

    GpuVoxelStorageManager(havk::DeviceContext* ctx) {
        Device = ctx;
        BuildOccupancyShader = ctx->PipeBuilder->CreateCompute("UpdateOccupancy.slang");
    }

    void SyncBuffers(VoxelMap& map, havk::CommandList& cmds) {
        std::vector<std::tuple<uint32_t, uint64_t>> updateBatch;

        uint32_t maxSlotId = SlotAllocator.Arena.NumAllocated;
        uint32_t dirtyBricksInBatch = 0;

        // Allocate slots for dirty bricks
        for (auto [sectorIdx, dirtyMask] : map.DirtyLocs) {
            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            auto sectorAlloc = SlotAllocator.GetSector(sectorPos);
            if (sectorAlloc == nullptr) continue;

            uint64_t freeMask;

            if (map.Sectors.contains(sectorIdx)) {
                Sector& sector = map.Sectors[sectorIdx];
                uint64_t allocMask = sector.GetAllocationMask();
                dirtyMask &= allocMask;
                freeMask = sectorAlloc->AllocMask & ~allocMask;
            } else {
                dirtyMask = 0;
                freeMask = ~0ull;
            }

            if (freeMask != 0) {
                dirtyMask |= SlotAllocator.Free(sectorAlloc, freeMask);
            }
            if (dirtyMask != 0) {
                dirtyMask |= SlotAllocator.Alloc(sectorAlloc, dirtyMask);
                maxSlotId = std::max(maxSlotId, sectorAlloc->BaseSlot + (uint32_t)std::popcount(sectorAlloc->AllocMask));
                dirtyBricksInBatch += (uint32_t)std::popcount(dirtyMask);
            }
            updateBatch.push_back({ sectorIdx, dirtyMask });
        }
        map.DirtyLocs.clear();
        
        // Initialize buffers
        uint32_t maxBricksInBuffer = std::bit_ceil(maxSlotId);
        size_t bufferSize = sizeof(GpuVoxelStorage) + maxBricksInBuffer * sizeof(Brick);

        if (StorageBuffer == nullptr || StorageBuffer->Size < bufferSize) {
            bool isResizing = StorageBuffer != nullptr;

            StorageBuffer = Device->CreateBuffer({
                .Size = bufferSize,
                .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .VmaFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            });
            OccupancyStorage = Device->CreateBuffer({
                .Size = maxBricksInBuffer * (BrickIndexer::MaxArea / 8),
                .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .VmaFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            });

            if (isResizing || maxSlotId < 1024) {
                map.MarkAllDirty();
                SlotAllocator = { ViewSize };
                return;
            }
        }
        auto mappedStorage = (GpuVoxelStorage*)StorageBuffer->MappedData;  // write only!

        // TODO: consider not updating palette every frame 
        for (uint32_t i = 0; i < 256; i++) {
            mappedStorage->Palette[i] = map.Palette[i].GetEncoded();
        }

        if (updateBatch.empty()) {
            StorageBuffer->Flush(offsetof(GpuVoxelStorage, Palette), sizeof(GpuVoxelStorage::Palette));
            return;
        }

        // Upload brick data
        auto updateBuffer = Device->CreateBuffer({
            .Size = dirtyBricksInBatch * sizeof(glm::uvec3),
            .Usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .VmaFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        });
        auto updateLocs = (glm::uvec3*)updateBuffer->MappedData;
        uint32_t updateLocIdx = 0;

        for (auto [sectorIdx, dirtyMask] : updateBatch) {
            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            auto sectorAlloc = SlotAllocator.GetSector(sectorPos);

            // Write bricks to GPU storage
            if (dirtyMask != 0) {
                Sector& sector = map.Sectors[sectorIdx];

                for (uint32_t brickIdx : BitIter(dirtyMask)) {
                    uint32_t slotIdx = sectorAlloc->GetSlot(brickIdx) - 1;
                    assert(slotIdx < maxBricksInBuffer);

                    Brick* brick = sector.GetBrick(brickIdx);
                    mappedStorage->BrickVoxelData[slotIdx] = *brick;

                    glm::uvec3 brickPos = sectorPos * MaskIndexer::Size + MaskIndexer::GetPos(brickIdx);
                    updateLocs[updateLocIdx++] = brickPos;
                }
            }

            // Sector-level occupancy mask
            uint32_t sectorMaskIdx = GetLinearIndex(sectorPos / 4, ViewSize.x / 4, ViewSize.y / 4);
            uint64_t& sectorOccMask = SectorOccupancy[sectorMaskIdx];
            uint32_t sectorOccIdx = GetLinearIndex(sectorPos, 4, 4);

            if (sectorAlloc->AllocMask != 0) {
                sectorOccMask |= (1ull << sectorOccIdx);
            } else {
                sectorOccMask &= ~(1ull << sectorOccIdx);
            }

            // Write headers to GPU storage
            uint32_t viewSectorIdx = sectorAlloc - SlotAllocator.Sectors.get();
            mappedStorage->BrickMasks[viewSectorIdx] = sectorAlloc->AllocMask;
            mappedStorage->BaseSlots[viewSectorIdx] = sectorAlloc->BaseSlot - 1;
            mappedStorage->SectorMasks[sectorMaskIdx] = sectorOccMask;
        }
        StorageBuffer->Flush();
        updateBuffer->Flush();

        struct OcmUpdateDispatchParams {
            uint32_t NumBricks;
            VkDeviceAddress BrickLocs;
            GpuVoxelMap Map;
        };
        OcmUpdateDispatchParams updatePars = {
            .NumBricks = updateLocIdx,
            .BrickLocs = updateBuffer->DeviceAddress,
            .Map = {
                .Storage = StorageBuffer->DeviceAddress,
                .VoxelOccupancy = OccupancyStorage->DeviceAddress
            },
        };
        cmds.MarkUse(updateBuffer);
        BuildOccupancyShader->Dispatch(cmds, { (updateLocIdx + 63) / 64, 1, 1 }, updatePars);
    }

    void ShiftView(glm::dvec3 cameraPos) {
        double dist = glm::distance(cameraPos / glm::dvec3(SectorSize), glm::dvec3(ViewOffset) + 0.5);
        if (dist < 2.0) return;

        glm::ivec3 newOffset = glm::floor(cameraPos);
        glm::ivec3 shift = ViewOffset - newOffset;
        glm::ivec3 disp = glm::min(glm::abs(shift), glm::ivec3(ViewSize.x, ViewSize.y, ViewSize.x));
        ViewOffset = newOffset;
        
        for (int32_t dy = 0; dy < disp.y; dy++) {
            for (int32_t dz = 0; dz < ViewSize.x; dz++) {
                for (int32_t dx = 0; dx < ViewSize.x; dx++) {
                    glm::ivec3 srcPos = glm::ivec3(dx, dy, dz);
                    auto srcSector = SlotAllocator.GetSector(srcPos);
                    auto dstSector = SlotAllocator.GetSector(srcPos + shift);

                    // TODO
                }
            }
        }
    }
};

// Based on https://www.youtube.com/watch?v=P2bGF6GPmfc
static void GenerateRayCellInteractionMaskLUT(uint64_t table[64 * 8]) {
    for (uint64_t dirOct = 0; dirOct < 8; dirOct++) {
        glm::ivec3 dir = (glm::ivec3(dirOct) >> glm::ivec3(0, 1, 2) & 1) * 2 - 1;

        for (uint64_t originIdx = 0; originIdx < 64; originIdx++) {
            uint64_t mask = 0;

            for (uint64_t j = 0; j < 64; j++) {
                glm::ivec3 pos = MaskIndexer::GetPos(originIdx) + MaskIndexer::GetPos(j) * dir;

                if (MaskIndexer::CheckInBounds(pos)) {
                    mask |= 1ull << MaskIndexer::GetIndex(pos);
                }
            }
            table[originIdx + dirOct * 64] = mask;
        }
    }
}

GpuRenderer::GpuRenderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    _ctx = ctx;
    _map = std::move(map);
    _storage = std::make_unique<GpuVoxelStorageManager>(ctx);

    _renderShader = ctx->PipeBuilder->CreateCompute("VoxelRender.slang");

    _gbuffer = std::make_unique<GBuffer>(ctx);
    _gbuffer->NumDenoiserPasses = 0;

    // _blueNoiseTex = ogl::Texture2D::Load("assets/bluenoise/stbn_vec2_2Dx1D_128x128x64_combined.png", 1, GL_RG8UI);
    // _renderShader->SetUniform("u_STBlueNoiseTex", *_blueNoiseTex);

    uint64_t interactionMaskLUT[64 * 8];
    GenerateRayCellInteractionMaskLUT(interactionMaskLUT);
    // _rayCellInteractionMaskLUT = std::make_unique<ogl::Buffer>(sizeof(interactionMaskLUT), 0, interactionMaskLUT);
    // _renderShader->SetUniform("ssbo_RayCellInteractionMaskLUT", *_rayCellInteractionMaskLUT);

    // auto panoToCubeShader = shlib.LoadComp("PanoramaToCube");
    // _skyTex = ogl::TextureCube::LoadPanorama("assets/skyboxes/evening_road_01_puresky_4k.hdr", *panoToCubeShader);
    // _renderShader->SetUniform("u_SkyTexture", *_skyTex);
}
GpuRenderer::~GpuRenderer() = default;

struct RenderDispatchParams {
    GpuVoxelMap Map;
    VkDeviceAddress GBuffer;  // GBufferUniforms*
    glm::ivec3 WorldOrigin;

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

    glm::uvec2 renderSize = glm::uvec2(target->Desc.Width, target->Desc.Height);
    _gbuffer->SetCamera(cam, renderSize, worldChanged);

    RenderDispatchParams renderPars = {
        .Map = {
            .Storage = _storage->StorageBuffer->DeviceAddress,
            .VoxelOccupancy = _storage->OccupancyStorage->DeviceAddress,
        },
        .GBuffer = _gbuffer->UniformBuffer->DeviceAddress,
        .WorldOrigin = glm::ivec3(glm::floor(_gbuffer->CurrentPos)),
        .MaxBounces = _numLightBounces,
    };
    uint32_t groupsX = (renderSize.x + 7) / 8, groupsY = (renderSize.y + 7) / 8;
    _renderShader->Dispatch(cmds, { groupsX, groupsY, 1 }, renderPars);
    cmds.MarkUse(_storage->StorageBuffer, _storage->OccupancyStorage, _gbuffer->UniformBuffer);

    _gbuffer->Resolve(target, cmds);
}
void GpuRenderer::DrawSettings(glim::SettingStore& settings) {
    ImGui::SeparatorText("Renderer##GPU");

    ImGui::PushItemWidth(150);
    settings.Combo("Debug Channel", &_gbuffer->DebugChannelView);
    settings.Slider("Light Bounces", &_numLightBounces, 1, 0u, 5u);
    settings.Slider("Denoiser Passes", &_gbuffer->NumDenoiserPasses, 1, 0u, 5u);
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
        ImGui::Text("Storage: %.1fMB (%zu free ranges)", _storage->StorageBuffer->Size / 1048576.0, _storage->SlotAllocator.Arena.FreeRanges.size());

        uint32_t v2 = 0;
        for (auto & sector : _map->Sectors) {
            v2 += (uint32_t)std::popcount(sector.second.GetAllocationMask());
        }
        ImGui::Text("Bricks: %.1fK (%.1fK on CPU)", _storage->SlotAllocator.Arena.NumAllocated / 1000.0, v2 / 1000.0);
    }
}