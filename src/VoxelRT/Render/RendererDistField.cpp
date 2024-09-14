#include "Renderer.h"

#include <unordered_set>

#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/hash.hpp"

namespace {

// NOTE: keep in sync with shaders
static constexpr auto kGridSize = glm::uvec2(4096, 1024);
static constexpr auto kNumVoxels = uint64_t(kGridSize.x) * kGridSize.y * kGridSize.x;
static constexpr auto kTileSize = 256;

struct GpuVoxelMap {
    havk::ImageHandle DistField;
    VkDeviceAddress OccupancyMap;
};
struct GpuTileUpdateRecord {
    glm::uvec3 MaskPos;
    uint64_t OccMask[(kTileSize * kTileSize * kTileSize) / 64];
};

struct RendererDistField : public GpuRenderer {
    havk::ImagePtr StorageImage;
    havk::BufferPtr OcmStorageBuffer;

    havk::ComputePipelinePtr UpdateShader;
    RendererId _type;

    RendererDistField(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map, RendererId type) : GpuRenderer(ctx, map, type) {
        _type = type;
        UpdateShader = ctx->PipeBuilder->CreateCompute("Backends/DistField/UpdateMap.slang", { .PrepDefs = { { "BACKEND_ID", std::to_string((int)type) } } });

        _map->MarkAllDirty();
    }

    void RenderFrame(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap {
            .DistField = cmds.GetDescriptorHandle(*StorageImage, havk::UseBarrier::ComputeRead, VK_IMAGE_LAYOUT_GENERAL),
            .OccupancyMap = cmds.GetDeviceAddress(*OcmStorageBuffer, havk::UseBarrier::ComputeRead),
        });
    }

    bool SyncMap(glim::Camera& cam, havk::CommandList& cmds) override {
        if (StorageImage == nullptr) {
            // An early prototype used sparse residency images but it was scrapped because:
            // - Binding pages grows proportionally slower to the number of already bound pages (seems to be a well known issue)
            // - Reading from unbound pages is very slow on Intel (maybe some page fault quirk from UMA?),
            //   this could likely be mitigated by splatting an aliased helper page filled with MAX_DIST.
            StorageImage = _ctx->CreateImage({
                .Type = VK_IMAGE_TYPE_3D,
                .Format = _type == RendererId::ManhattanDF ? VK_FORMAT_R8_UINT : VK_FORMAT_R16_UINT,
                .Usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .Width = kGridSize.x / 4,
                .Height = kGridSize.y / 4,
                .Depth = kGridSize.x / 4,
                .NumLevels = 1,
            });
            cmds.TransitionLayout(*StorageImage, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            cmds.Clear(*StorageImage, { ~0u });

            OcmStorageBuffer = _ctx->CreateBuffer({
                .Size = kNumVoxels / 64 * sizeof(uint64_t),
                .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            });
        }
        const size_t MaxTileUpdates = 16;

        constexpr glm::ivec3 kSectorSize = BrickIndexer::Size * MaskIndexer::Size;
        static_assert(kTileSize >= kSectorSize.x);

        std::unordered_set<glm::uvec3> batchLocs;

        for (auto iter = _map->DirtyLocs.begin(); iter != _map->DirtyLocs.end();) {
            auto [sectorIdx, dirtyMask] = *iter;
            auto currIter = iter++;

            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            glm::ivec3 tilePos = (sectorPos * Brick::Size * MaskIndexer::Size) / kTileSize * kTileSize;

            bool inRange = tilePos.x < kGridSize.x && tilePos.y < kGridSize.y && tilePos.z < kGridSize.x;

            if (batchLocs.size() < MaxTileUpdates && inRange) {
                batchLocs.insert(tilePos);
                inRange = false;
            }

            // We have to iterate over entire queue to flush all sectors covering a tile,
            // only remove sectors that will be actually updated this call.
            if (!inRange || batchLocs.contains(tilePos)) {
                _map->DirtyLocs.erase(currIter);
            }
        }

        if (batchLocs.size() == 0) return false;

        auto stagingBuffer = _ctx->CreateBuffer({
            .Size = batchLocs.size() * sizeof(GpuTileUpdateRecord),
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        });
        auto record = (GpuTileUpdateRecord*)stagingBuffer->MappedData;

        for (glm::uvec3 pos : batchLocs) {
            record->MaskPos = pos / 4u;

            auto startPos = glm::ivec3(pos) / 4;
            auto endPos = (glm::ivec3(pos) + kTileSize - 1) / 4;
            uint64_t* occupancyData = (uint64_t*)record->OccMask;

            for_yzx_inclusive(glm::ivec3, maskPos, startPos, endPos) {
                auto pos = maskPos * 4;
                Brick* brick = _map->GetBrick(pos / Brick::Size);

                if (brick == nullptr) {
                    *occupancyData++ = 0;
                    continue;
                }

                alignas(64) uint8_t temp[64];

                for (int32_t i = 0; i < 64; i += 4) {
                    int32_t offset = BrickIndexer::GetIndex(pos.x, pos.y + (i >> 4 & 3), pos.z + (i >> 2 & 3));
                    memcpy(&temp[i], &brick->Data[offset], 4);
                }
                *occupancyData++ = Brick::PackBits64(temp);
            }
            record++;
        }
        stagingBuffer->Flush();

        struct UpdateParams {
            GpuVoxelMap Map;
            uint16_t NumTiles;
            uint16_t Axis;
            VkDeviceAddress Tiles;
        };
        UpdateParams pc = {
            .Map = {
                .DistField = cmds.GetDescriptorHandle(*StorageImage, havk::UseBarrier::ComputeRead, VK_IMAGE_LAYOUT_GENERAL),
                .OccupancyMap = cmds.GetDeviceAddress(*OcmStorageBuffer, havk::UseBarrier::ComputeRead),
            },
            .NumTiles = uint16_t(batchLocs.size()),
            .Tiles = cmds.GetDeviceAddress(*stagingBuffer, havk::UseBarrier::ComputeRead),
        };

        for (uint32_t i = 0; i < 3; i++) {
            pc.Axis = i;
            UpdateShader->Dispatch(cmds, { kTileSize / 8 / 4, kTileSize / 8 / 4, pc.NumTiles }, pc);
            cmds.Barrier(*StorageImage, havk::UseBarrier::All, VK_IMAGE_LAYOUT_GENERAL);
        }
        return true;
    }
};

};  // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::ManhattanDF>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererDistField>(ctx, map, RendererId::ManhattanDF);
}
template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::EuclideanDF>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererDistField>(ctx, map, RendererId::EuclideanDF);
}