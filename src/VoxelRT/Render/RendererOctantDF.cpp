#include "Renderer.h"

#include <unordered_set>

#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/hash.hpp"

namespace {

// NOTE: keep in sync with shaders
constexpr int GRID_SIZE_XZ = 4096, GRID_SIZE_Y = 1024,
              TILE_SIZE = 256, FIELD_RES = 4, 
              FIELD_SIZE = TILE_SIZE / FIELD_RES,
              NUM_TILES = (GRID_SIZE_XZ / TILE_SIZE) * (GRID_SIZE_XZ / TILE_SIZE) * (GRID_SIZE_Y / TILE_SIZE);

struct FieldTile {
    uint8_t Dists[8][FIELD_SIZE * FIELD_SIZE * FIELD_SIZE];
    uint64_t OccMasks[(TILE_SIZE * TILE_SIZE * TILE_SIZE) / 64];
};
struct MapStorage {
    VkDeviceAddress Tiles[(GRID_SIZE_XZ / TILE_SIZE) * (GRID_SIZE_Y / TILE_SIZE) * (GRID_SIZE_XZ / TILE_SIZE)];
};
struct GpuVoxelMap {
    VkDeviceAddress Storage;
};

struct RendererOctantDF : public GpuRenderer {
    havk::BufferPtr StorageBuffer;
    havk::ComputePipelinePtr UpdateShader;

    RendererOctantDF(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : GpuRenderer(ctx, map, RendererId::OctantDF) {
        UpdateShader = ctx->PipeBuilder->CreateCompute("Backends/OctantDF/UpdateMap.slang");

        _map->MarkAllDirty();
    }

    void RenderFrame(havx::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap {
            .Storage = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeRead),
        });
    }

    bool SyncMap(havx::Camera& cam, havk::CommandList& cmds) override {
        if (StorageBuffer != nullptr && _map->DirtyLocs.size() == 0) return false;
        _map->DirtyLocs.clear();
        
        constexpr glm::ivec3 kSectorSize = BrickIndexer::Size * MaskIndexer::Size;
        static_assert(TILE_SIZE >= kSectorSize.x);

        std::unordered_set<glm::ivec3> tileLocs;

        for (auto [sectorIdx, dirtyMask] :  _map->Sectors) {
            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            glm::ivec3 tilePos = (sectorPos * Brick::Size * MaskIndexer::Size) / TILE_SIZE * TILE_SIZE;

            if (tilePos.x < GRID_SIZE_XZ && tilePos.y < GRID_SIZE_Y && tilePos.z < GRID_SIZE_XZ) {
                tileLocs.insert(tilePos);
            }
        }

        StorageBuffer = _ctx->CreateBuffer({
            .Size =  sizeof(MapStorage) + sizeof(FieldTile) * tileLocs.size(),
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        });

        auto tileGrid = (MapStorage*)StorageBuffer->MappedData;
        size_t tileStorageOffset = sizeof(MapStorage);

        memset(StorageBuffer->MappedData, 0, sizeof(MapStorage));

        for (glm::ivec3 pos : tileLocs) {
            uint64_t* occupancyData = (uint64_t*)((uint8_t*)StorageBuffer->MappedData + tileStorageOffset + offsetof(FieldTile, OccMasks));

            auto startPos = pos / 4;
            auto endPos = (pos + TILE_SIZE - 1) / 4;

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

            uint32_t tileGridIdx = GetLinearIndex(pos / TILE_SIZE, GRID_SIZE_XZ / TILE_SIZE, GRID_SIZE_Y / TILE_SIZE);
            tileGrid->Tiles[tileGridIdx] = StorageBuffer->DeviceAddress + tileStorageOffset;

            struct UpdateParams {
                uint32_t Axis;
                VkDeviceAddress Tile;
            };

            for (uint32_t i = 0; i < 3; i++) {
                UpdateShader->Dispatch(cmds, { FIELD_SIZE / 8, FIELD_SIZE / 8, 1 }, UpdateParams {
                    .Axis = i,
                    .Tile = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeReadWrite) + tileStorageOffset,
                });
            }
            tileStorageOffset += sizeof(FieldTile);
        }
        StorageBuffer->Flush();

        return true;
    }

    void DrawSettings(havx::SettingStore& settings) override {
        GpuRenderer::DrawSettings(settings);

        if (StorageBuffer != nullptr) {
            ImGui::Text("Storage: %.1fMB", StorageBuffer->Size / 1048576.0);
        }
    }
};

};  // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::OctantDF>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererOctantDF>(ctx, map);
}