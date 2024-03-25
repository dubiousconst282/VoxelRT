#include "Renderer.h"
#include "BrickSlotAllocator.h"

static constexpr auto SectorSize = BrickIndexer::Size * VoxelIndexer::Size;
static constexpr auto ViewSize = glm::uvec2(4096, 1024) / glm::uvec2(SectorSize);
static constexpr uint32_t NumViewSectors = ViewSize.x * ViewSize.x * ViewSize.y;

static const std::vector<ogl::ShaderLoadParams::PrepDef> DefaultShaderDefs = {
    { "BRICK_SIZE", std::to_string(Brick::Size.x) },
    { "NUM_SECTORS_XZ", std::to_string(ViewSize.x) },
    { "NUM_SECTORS_Y", std::to_string(ViewSize.y) },
    { "TRAVERSAL_METRICS", "1" },
};

struct GpuVoxelStorage {
    std::unique_ptr<ogl::Buffer> StorageBuffer;
    std::unique_ptr<ogl::Buffer> OccupancyStorage;

    std::shared_ptr<ogl::Shader> BuildOccupancyShader;

    BrickSlotAllocator SlotAllocator = { ViewSize };
    glm::ivec3 ViewOffset; // world view offset in sector scale

    static_assert(std::endian::native == std::endian::little);
    struct GpuMeta {
        Material Palette[sizeof(VoxelMap::Palette) / sizeof(Material)];
        uint32_t BaseSlots[NumViewSectors];
        uint64_t AllocMasks[NumViewSectors];
        uint64_t SectorOccupancy[NumViewSectors / 64];  // Occupancy masks at sector level
        Brick Bricks[];
    };
    struct UpdateRequest {
        uint32_t Count;
        glm::uvec3 BrickLocs[];
    };

    GpuVoxelStorage(ogl::ShaderLib& shlib) {
        BuildOccupancyShader = shlib.LoadComp("UpdateOccupancy", DefaultShaderDefs);
    }

    void SyncBuffers(VoxelMap& map) {
        const uint32_t MaxBatchSize = 1024 * 1024 * 128 / (sizeof(Brick) * 64);
        const uint32_t MinBricksInBuffer = 1024 * 1024 * 512 / sizeof(Brick);

        std::vector<std::tuple<uint32_t, uint64_t>> batch;

        auto itr = map.DirtyLocs.begin();
        uint32_t maxSlotId = SlotAllocator.Arena.NumAllocated;
        uint32_t dirtyBricksInBatch = 0;

        // Allocate slots for dirty bricks
        while (itr != map.DirtyLocs.end() && batch.size() < MaxBatchSize) {
            auto [sectorIdx, dirtyMask] = *itr;

            glm::ivec3 sectorPos = SectorIndexer::GetPos(sectorIdx);
            auto sectorAlloc = SlotAllocator.GetSector(sectorPos);

            if (sectorAlloc != nullptr) {
                assert(map.Sectors.contains(sectorIdx));
                uint64_t emptyMask = map.Sectors[sectorIdx].DeleteEmptyBricks();

                if (emptyMask != 0) {
                    dirtyMask &= ~emptyMask;
                    dirtyMask |= SlotAllocator.Free(sectorAlloc, emptyMask);
                }
                if (dirtyMask != 0) {
                    dirtyMask |= SlotAllocator.Alloc(sectorAlloc, dirtyMask);
                    maxSlotId = std::max(maxSlotId, sectorAlloc->BaseSlot + (uint32_t)std::popcount(sectorAlloc->AllocMask));
                }
                batch.push_back({ sectorIdx, dirtyMask });
            }

            dirtyBricksInBatch += (uint32_t)std::popcount(dirtyMask);
            map.DirtyLocs.erase(itr++);
        }
        // Initialize buffers
        uint32_t maxBricksInBuffer = std::bit_ceil(maxSlotId);
        size_t bufferSize = sizeof(GpuMeta) + maxBricksInBuffer * sizeof(Brick);

        if (StorageBuffer == nullptr || StorageBuffer->Size < bufferSize) {
            StorageBuffer = std::make_unique<ogl::Buffer>(bufferSize, GL_MAP_WRITE_BIT);
            OccupancyStorage = std::make_unique<ogl::Buffer>(maxBricksInBuffer * (VoxelIndexer::MaxArea / 8), 0);

            map.MarkAllDirty();
        }
        auto mappedStorage = StorageBuffer->Map<GpuMeta>(GL_MAP_WRITE_BIT);
        std::memcpy(mappedStorage->Palette, map.Palette, sizeof(VoxelMap::Palette));

        if (dirtyBricksInBatch == 0) return;

        // Upload brick data
        auto updateBuffer = ogl::Buffer(dirtyBricksInBatch * sizeof(glm::uvec3) + sizeof(UpdateRequest), GL_MAP_WRITE_BIT);
        auto updateLocs = updateBuffer.Map<UpdateRequest>(GL_MAP_WRITE_BIT);
        uint32_t updateLocIdx = 0;

        for (auto [sectorIdx, dirtyMask] : batch) {
            assert(map.Sectors.contains(sectorIdx));
            Sector& actualSector = map.Sectors[sectorIdx];
            glm::ivec3 sectorPos = SectorIndexer::GetPos(sectorIdx);
            auto sectorAlloc = SlotAllocator.GetSector(sectorPos);

            for (uint32_t brickIdx : BitIter(dirtyMask)) {
                uint32_t slotIdx = sectorAlloc->GetSlot(brickIdx) - 1;

                assert(slotIdx < maxBricksInBuffer);

                Brick* brick = actualSector.GetBrick(brickIdx);
                assert(brick != nullptr);

                mappedStorage->Bricks[slotIdx] = *brick;

                glm::uvec3 brickPos = sectorPos * BrickIndexer::Size + BrickIndexer::GetPos(brickIdx);
                updateLocs->BrickLocs[updateLocIdx++] = brickPos;
            }

            // Also update sector metadata while we have it in hand.
            uint32_t viewSectorIdx = sectorAlloc - SlotAllocator.Sectors.get();
            mappedStorage->AllocMasks[viewSectorIdx] = sectorAlloc->AllocMask;
            mappedStorage->BaseSlots[viewSectorIdx] = sectorAlloc->BaseSlot - 1;

            // Sector-level occupancy mask
            uint64_t& sectorOccMask = mappedStorage->SectorOccupancy[GetLinearIndex(sectorPos / 4, ViewSize.x / 4, ViewSize.y / 4)];
            uint32_t sectorOccIdx = GetLinearIndex(sectorPos, 4, 4);
            if (sectorAlloc->AllocMask != 0) {
                sectorOccMask |= (1ull << sectorOccIdx);
            } else {
                sectorOccMask &= ~(1ull << sectorOccIdx);
            }

            // Erase from memory if empty
            if (actualSector.GetAllocationMask() == 0) {
                assert(sectorAlloc->AllocMask == 0);
                map.Sectors.erase(sectorIdx);
            }
        }

        updateLocs->Count = updateLocIdx;
        updateLocs.reset();
        BuildOccupancyShader->SetUniform("ssbo_UpdateLocs", updateBuffer);
        BuildOccupancyShader->SetUniform("ssbo_VoxelData", *StorageBuffer);
        BuildOccupancyShader->SetUniform("ssbo_VoxelOccupancy", *OccupancyStorage);
        BuildOccupancyShader->DispatchCompute(1, 1, (updateLocIdx + 63) / 64);
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

GpuRenderer::GpuRenderer(ogl::ShaderLib& shlib, std::shared_ptr<VoxelMap> map) {
    _map = std::move(map);
    _storage = std::make_unique<GpuVoxelStorage>(shlib);

    _renderShader = shlib.LoadComp("VoxelRender", DefaultShaderDefs);
    _svgfShader = shlib.LoadComp("DenoiseSVGF", DefaultShaderDefs);
    _blitShader = shlib.LoadFrag("GBufferBlit", DefaultShaderDefs);

    _blueNoiseScramblingTex = ogl::Texture2D::Load("assets/bluenoise/ScramblingTile_128x128x4_1spp.png", 1, GL_RGBA8UI);
    _blueNoiseSobolTex = ogl::Texture2D::Load("assets/bluenoise/Sobol_256x256.png", 1, GL_RGBA8UI);

    _renderShader->SetUniform("u_BlueNoiseScramblingTex", *_blueNoiseScramblingTex);
    _renderShader->SetUniform("u_BlueNoiseSobolTex", *_blueNoiseSobolTex);

    glCreateQueries(GL_TIME_ELAPSED, 1, &_frameQueryObj);
    _metricsBuffer = std::make_unique<ogl::Buffer>(64, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
}
GpuRenderer::~GpuRenderer() { glDeleteQueries(1, &_frameQueryObj); }

void GpuRenderer::RenderFrame(glim::Camera& cam, glm::uvec2 viewSize) {
    GLint64 frameElapsedNs;
    glGetQueryObjecti64v(_frameQueryObj, GL_QUERY_RESULT, &frameElapsedNs);
    _frameTime.AddSample(frameElapsedNs / 1000000.0);
    glBeginQuery(GL_TIME_ELAPSED, _frameQueryObj);


    glm::dvec3 camPos = cam.ViewPosition;
    glm::ivec3 worldOrigin = glm::ivec3(glm::floor(camPos));
    glm::vec3 originFrac = glm::fract(camPos);

    glm::mat4 projMat = cam.GetProjMatrix() * cam.GetViewMatrix(false);
    glm::mat4 invProj = glm::inverse(projMat);
    uint32_t groupsX = (viewSize.x + 7) / 8, groupsY = (viewSize.y + 7) / 8;

    bool worldChanged = _map->DirtyLocs.size() > 0;

    // Sync buffers
    if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
        _map->MarkAllDirty();
        _storage->SlotAllocator = BrickSlotAllocator(ViewSize);
    }
    if (_backTex == nullptr || _backTex->Width != viewSize.x || _backTex->Height != viewSize.y) {
        _backTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA32UI);
        _frontTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA32UI);
    }
    _storage->SyncBuffers(*_map);

    // Trace
    _renderShader->SetUniform("ssbo_VoxelData", *_storage->StorageBuffer);
    _renderShader->SetUniform("ssbo_VoxelOccupancy", *_storage->OccupancyStorage);
    _renderShader->SetUniform("u_WorldOrigin", &worldOrigin.x, 3);

    _renderShader->SetUniform("ssbo_Metrics", *_metricsBuffer);

    _renderShader->SetUniform("u_UseAnisotropicLods", _useAnisotropicLods ? 1 : 0);
    _renderShader->SetUniform("u_FrameNo", (int)_frameNo);

    _renderShader->SetUniform("u_InvProjMat", &invProj[0][0], 16);
    _renderShader->SetUniform("u_ProjMat", &projMat[0][0], 16);
    _renderShader->SetUniform("u_OriginFrac", &originFrac.x, 3);
    _renderShader->SetUniform("u_BackBuffer", *_backTex);
    _renderShader->SetUniform("u_FrontBuffer", *_frontTex);
    _renderShader->DispatchCompute(groupsX, groupsY, 1);

    // Denoise, TAA
    glm::vec3 originDelta = glm::vec3(camPos - _prevOrigin) - originFrac;
    _svgfShader->SetUniform("u_InvProjMat", &invProj[0][0], 16);
    _svgfShader->SetUniform("u_OldProjMat", &_prevProj[0][0], 16);
    _svgfShader->SetUniform("u_OriginFrac", &originFrac.x, 3);
    _svgfShader->SetUniform("u_OriginDelta", &originDelta.x, 3);
    _svgfShader->SetUniform("u_BackBuffer", *_backTex);
    _svgfShader->SetUniform("u_FrontBuffer", *_frontTex);
    _svgfShader->SetUniform("u_DiscardAccumSamples", worldChanged ? 1 : 0);

    for (int32_t i = 0; i < 3; i++) {
        _svgfShader->SetUniform("u_PassNo", i);
        _svgfShader->DispatchCompute(groupsX, groupsY, 1);
    }

    // Blit to screen
    _blitShader->SetUniform("u_BackBuffer", *_backTex);
    _blitShader->DispatchFullscreen();

    std::swap(_backTex, _frontTex);
    _prevProj = projMat;
    _prevOrigin = camPos;

    glEndQuery(GL_TIME_ELAPSED);

    _frameNo++;
}
void GpuRenderer::DrawSettings(glim::SettingStore& settings) {
    ImGui::SeparatorText("Renderer##GPU");
    settings.Combo("Debug View", &_debugView);
    settings.Checkbox("Anisotropic LODs", &_useAnisotropicLods);

    ImGui::Separator();
    _frameTime.Draw("Frame Time");

    auto totalIters = _metricsBuffer->Map<uint32_t>(GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
    ImGui::Text("Traversal Iters: %.3fM", *totalIters / 1000000.0);
    *totalIters = 0;

    if (_storage->StorageBuffer != nullptr) {
        ImGui::Text("Storage: %.1fMB (%zu free ranges)", _storage->StorageBuffer->Size / 1048576.0, _storage->SlotAllocator.Arena.FreeRanges.size());

        uint32_t v2 = 0;
        for (auto & sector : _map->Sectors) {
            v2 += (uint32_t)std::popcount(sector.second.GetAllocationMask());
        }
        ImGui::Text("Bricks: %.1fK (%.1fK on CPU)", _storage->SlotAllocator.Arena.NumAllocated / 1000.0, v2 / 1000.0);
        // FIXME: figure whytf sectors don't get deleted properly
        //        seems like the brush dispatcher should check for empty sectors
    }
}