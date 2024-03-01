#include "Renderer.h"
#include "BrickSlotAllocator.h"

struct GpuVoxelStorage {
    static constexpr auto ViewSize = glm::uvec2(64, 32); // XZ, Y sectors (* 4x4x4 * 8x8x8 -> 2048 XZ, 1024 Y)

    std::unique_ptr<ogl::Buffer> StorageBuffer;
    std::unique_ptr<ogl::Texture3D> OccupancyStorage;

    std::shared_ptr<ogl::Shader> BuildOccupancyShader;

    BrickSlotAllocator SlotAllocator = { ViewSize };

    struct SectorInfo {
        uint32_t BaseSlot;
        uint32_t AllocMask_0;
        uint32_t AllocMask_1;
    };
    struct GpuMeta {
        Material Palette[sizeof(VoxelMap::Palette) / sizeof(Material)];
        SectorInfo Sectors[ViewSize.x * ViewSize.x * ViewSize.y];
        Brick Bricks[];
    };

    void SyncGpuBuffers(VoxelMap& map) {
        const uint32_t MaxBatchSize = 1024 * 1024 * 128 / (sizeof(Brick) * 64);
        std::vector<std::tuple<uint32_t, uint64_t>> batch;

        auto itr = map.DirtyLocs.begin();
        uint32_t maxSlotId = SlotAllocator.Arena.NumAllocated;
        uint32_t dirtyBricksInBatch = 0;

        while (itr != map.DirtyLocs.end() && batch.size() < MaxBatchSize) {
            auto [sectorIdx, dirtyMask] = *itr;

            glm::ivec3 sectorPos = SectorIndexer::GetPos(sectorIdx);
            auto sectorAlloc = SlotAllocator.GetSector(sectorPos);

            if (sectorAlloc != nullptr) {
                dirtyMask |= SlotAllocator.Alloc(sectorAlloc, dirtyMask);
                batch.push_back({ sectorIdx, dirtyMask });

                maxSlotId = std::max(maxSlotId, sectorAlloc->BaseSlot + (uint32_t)std::popcount(sectorAlloc->AllocMask));
            }

            dirtyBricksInBatch += (uint32_t)std::popcount(dirtyMask);
            map.DirtyLocs.erase(itr++);
        }
        // avg * dlocs
        uint32_t estimDirtyBricksLeft = batch.size() == 0 ? 0 : dirtyBricksInBatch * (uint64_t)map.DirtyLocs.size() / batch.size();
        uint32_t maxBricksInBuffer = std::bit_ceil(std::max(estimDirtyBricksLeft * 3 / 4, maxSlotId));
        size_t bufferSize = sizeof(GpuMeta) + maxBricksInBuffer * sizeof(Brick);

        if (StorageBuffer == nullptr || StorageBuffer->Size < bufferSize) {
            StorageBuffer = std::make_unique<ogl::Buffer>(bufferSize, GL_MAP_WRITE_BIT);

            map.MarkAllDirty();
        }
        auto mappedStorage = StorageBuffer->Map<GpuMeta>(GL_MAP_WRITE_BIT);
        std::memcpy(mappedStorage->Palette, map.Palette, sizeof(VoxelMap::Palette));

        for (auto [sectorIdx, dirtyMask] : batch) {
            Sector& actualSector = map.Sectors[sectorIdx];
            glm::ivec3 sectorPos = SectorIndexer::GetPos(sectorIdx);
            auto sectorAlloc = SlotAllocator.GetSector(sectorPos);

            for (uint64_t m = dirtyMask; m != 0; m &= m - 1) {
                uint32_t idx = (uint32_t)std::countr_zero(m);
                uint32_t slotIdx = sectorAlloc->GetSlot(idx) - 1;

                assert(slotIdx < maxBricksInBuffer);

                Brick* brick = actualSector.GetBrick(idx);
                mappedStorage->Bricks[slotIdx] = *brick;
            }

            // Also update sector metadata while we have it in hand.
            auto& gpuSector = mappedStorage->Sectors[sectorAlloc - SlotAllocator.Sectors.get()];
            gpuSector.BaseSlot = sectorAlloc->BaseSlot - 1;
            gpuSector.AllocMask_0 = (uint32_t)(sectorAlloc->AllocMask >> 0);
            gpuSector.AllocMask_1 = (uint32_t)(sectorAlloc->AllocMask >> 32);
        }
    }
};

GpuRenderer::GpuRenderer(ogl::ShaderLib& shlib, std::shared_ptr<VoxelMap> map) {
    _map = std::move(map);
    _storage = std::make_unique<GpuVoxelStorage>();

    _mainShader = shlib.LoadFrag("VoxelRender", {
        { "BRICK_SIZE", std::to_string(Brick::Size.x) },
        { "NUM_SECTORS_XZ", std::to_string(GpuVoxelStorage::ViewSize.x) },
        { "NUM_SECTORS_Y", std::to_string(GpuVoxelStorage::ViewSize.y) },
    });

    glCreateQueries(GL_TIME_ELAPSED, 1, &_frameQueryObj);
    _metricsBuffer = std::make_unique<ogl::Buffer>(64, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
}
GpuRenderer::~GpuRenderer() { glDeleteQueries(1, &_frameQueryObj); }

void GpuRenderer::RenderFrame(glim::Camera& cam, glm::uvec2 viewSize) {
    GLint64 frameElapsedNs;
    glGetQueryObjecti64v(_frameQueryObj, GL_QUERY_RESULT, &frameElapsedNs);
    _frameTime.AddSample(frameElapsedNs / 1000000.0);
    glBeginQuery(GL_TIME_ELAPSED, _frameQueryObj);

    if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
        _map->MarkAllDirty();
        _storage->SlotAllocator = BrickSlotAllocator(GpuVoxelStorage::ViewSize);
    }

    _storage->SyncGpuBuffers(*_map);

    _mainShader->SetUniform("ssbo_VoxelData", *_storage->StorageBuffer);
    // _mainShader->SetUniform("u_DistField", *_storage->OccupancyStorage);

    glm::dvec3 camPos = cam.ViewPosition;
    glm::ivec3 worldOrigin = glm::ivec3(glm::floor(camPos));

    glm::mat4 viewMat = glm::translate(cam.GetViewMatrix(false), glm::vec3(glm::floor(camPos) - camPos));
    glm::mat4 invProj = glm::inverse(cam.GetProjMatrix() * viewMat);

    _mainShader->SetUniform("u_InvProjMat", &invProj[0][0], 16);
    _mainShader->SetUniform("u_ShowTraversalHeatmap", _showHeatmap ? 1 : 0);
    _mainShader->SetUniform("u_WorldOrigin", &worldOrigin.x, 3);
    _mainShader->SetUniform("ssbo_Metrics", *_metricsBuffer);
    _mainShader->DispatchFullscreen();

    glEndQuery(GL_TIME_ELAPSED);
}
void GpuRenderer::DrawSettings(glim::SettingStore& settings) {
    ImGui::SeparatorText("Renderer##GPU");
    settings.Checkbox("Traversal Heatmap", &_showHeatmap);

    ImGui::Separator();
    _frameTime.Draw("Frame Time");

    auto totalIters = _metricsBuffer->Map<uint32_t>(GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
    ImGui::Text("Traversal Iters: %.3fM", *totalIters / 1000000.0);
    *totalIters = 0;

    if (_storage->StorageBuffer != nullptr) {
        ImGui::Text("Storage: %.1fMB (FreeListRanges: %zu)", _storage->StorageBuffer->Size / 1048576.0, _storage->SlotAllocator.Arena.FreeRanges.size());
    }
}