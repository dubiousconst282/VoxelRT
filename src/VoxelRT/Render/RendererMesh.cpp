#include "Renderer.h"
#include "BrickSlotAllocator.h"

#define BM_IMPLEMENTATION
#include "BinaryGreedyMesher.h"

#include <unordered_set>

#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/hash.hpp"

namespace {

void RepackChunkVoxels(VoxelMap& map, glm::ivec3 chunkPos, MeshData& mesh) {
    for (uint32_t y = 0; y < CS_P; y++) {
        for (uint32_t x = 0; x < CS_P; x++) {
            Brick* currBrick = nullptr;
            int32_t currBrickZ = INT_MIN;
            uint64_t opaqueMask = 0;

            for (uint32_t z = 0; z < CS_P; z++) {
                glm::ivec3 voxelPos = chunkPos * CS + glm::ivec3(x, y, z) - 1;
                glm::ivec3 brickPos = voxelPos >> BrickIndexer::Shift;

                // Cheap caching to minimize expansive GetBrick() calls
                if (currBrickZ != brickPos.z) {
                    currBrick = map.GetBrick(brickPos);
                    currBrickZ = brickPos.z;
                }
                uint8_t voxelId = currBrick ? currBrick->Data[BrickIndexer::GetIndex(voxelPos)].Data : 0;
                // if (voxelId != 0) voxelId = 1;
                mesh.voxels[z + (x * CS_P) + (y * CS_P2)] = voxelId;
                opaqueMask |= uint64_t(voxelId != 0) << z;
            }
            mesh.opaqueMask[y * CS_P + x] = opaqueMask;
        }
    }
}

struct GpuStorageData {
    uint64_t Palette[256];
    uint64_t QuadData[];
};

struct ChunkInfo {
    uint32_t BaseVertexOffset = 0;
    uint32_t NumFaceVertices[6] = { };

    uint32_t GetTotalVertexCount() const {
        uint32_t count = 0;
        for (uint32_t i = 0; i < 6; i++) {
            count += NumFaceVertices[i];
        }
        return count;
    }
};

struct RendererMesh : public Renderer {
    havk::GraphicsPipelinePtr DrawShader;
    havk::ImagePtr DepthBuffer;
    FreeList RangeAllocator = { 1024 * 1024 * 32 };  // max 32M quads

    havk::BufferPtr StorageBuffer;
    havk::BufferPtr IndexBuffer;
    bool EnableTriangleColoring = false;

    std::unordered_map<glm::ivec3, ChunkInfo> LoadedChunks;

    RendererMesh(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : Renderer(ctx, map) {
        DrawShader = ctx->PipeBuilder->CreateGraphics("Backends/Mesh/DrawGreedyMesh.slang", {
            .CullMode = VK_CULL_MODE_BACK_BIT,
            .FrontFace = VK_FRONT_FACE_CLOCKWISE,
            .EnableDepthTest = true,
            .EnableDepthWrite = true,
            .OutputFormats = { VK_FORMAT_R8G8B8A8_UNORM }, // TODO: render directly to swapchain ctx->Swapchain->SurfaceFormat.format
            .DepthFormat = VK_FORMAT_D32_SFLOAT,
        });
        
        _map->MarkAllDirty();
    }

    uint32_t _numDrawnQuads = 0;

    void RenderFrame(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        if (DepthBuffer == nullptr || DepthBuffer->Desc.Width != target->RenderSize.x || DepthBuffer->Desc.Height != target->RenderSize.y) {
            DepthBuffer = _ctx->CreateImage({
                .Format = VK_FORMAT_D32_SFLOAT,
                .Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                .Width = target->RenderSize.x,
                .Height = target->RenderSize.y,
            });
        }
        
        // Generate draw commands
        const uint32_t MaxDrawCommands = 1024 * 32;

        auto drawCmdBuffer = _ctx->CreateBuffer({
            .Size = sizeof(havk::DrawIndexedCommand) * MaxDrawCommands,
            .Usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        });
        auto drawCmds = (havk::DrawIndexedCommand*)drawCmdBuffer->MappedData;
        uint32_t numDrawCmds = 0;
        _numDrawnQuads = 0;

        for (auto& [pos, chunk] : LoadedChunks) {
            uint32_t vertexOffset = chunk.BaseVertexOffset;

            for (uint32_t face = 0; face < 6; face++) {
                // TODO: face culling
                if (chunk.NumFaceVertices[face] > 0) {
                    drawCmds[numDrawCmds++] = {
                        .NumIndices = chunk.NumFaceVertices[face] * 6,
                        .VertexOffset = int32_t(vertexOffset << 2),
                        .InstanceOffset = pos.x << 0 | pos.y << 8 | pos.z << 16 | face << 24,
                    };
                }
                vertexOffset += chunk.NumFaceVertices[face];
                _numDrawnQuads += chunk.NumFaceVertices[face];
            }

            if (numDrawCmds > MaxDrawCommands - 6) break;
        }
        drawCmdBuffer->Flush(0, numDrawCmds * sizeof(havk::DrawIndexedCommand));

        struct DispatchParams {
            VkDeviceAddress Storage;
            glm::ivec3 WorldOrigin;
            glm::mat4 ViewProjMat;
            uint32_t EnableTriangleColoring;
        };
        DispatchParams pc = {
            .Storage = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::GraphicsRead),
            .WorldOrigin = glm::floor(target->CurrentPos),
            .ViewProjMat = glm::translate(target->CurrentProj, glm::vec3(-glm::fract(target->CurrentPos))),
            .EnableTriangleColoring = EnableTriangleColoring ? 1u : 0,
        };

        cmds.BeginRendering({
            .Attachments = { {
                .Target = target->AlbedoTex.get(),
                .LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .ClearValue = { 0.77f, 0.90f, 0.96f, 1.0f } // #cceeff
            } },
            .DepthAttachment = {
                .Target = DepthBuffer.get(),
                .LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .StoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .ClearValue = { 1.0f },
            },
        }, true);
        cmds.BindIndexBuffer(*IndexBuffer, VK_INDEX_TYPE_UINT32);
        DrawShader->DrawIndexedIndirect(cmds, *drawCmdBuffer, 0, numDrawCmds, pc);
        cmds.EndRendering();

        target->DebugChannelView = GBuffer::DebugChannel::Albedo;
    }

    void DrawSettings(glim::SettingStore& settings) override {
        ImGui::Checkbox("Show triangles", &EnableTriangleColoring);
        ImGui::Text("Quads: %.2fK", _numDrawnQuads/1000.0);
    }

    bool SyncMap(glim::Camera& cam, havk::CommandList& cmds) override {
        if (StorageBuffer == nullptr) {
            StorageBuffer = _ctx->CreateBuffer({
                .Size = sizeof(uint64_t) * RangeAllocator.Capacity,
                .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            });
        }
        if (IndexBuffer == nullptr) {
            CreateIndexBuffer(cmds);
        }

        uint64_t encodedPalette[256];
        for (uint32_t i = 0; i < 256; i++) {
            encodedPalette[i] = _map->Palette[i].GetEncoded();
        }
        cmds.UpdateBuffer(*StorageBuffer, offsetof(GpuStorageData, Palette), sizeof(encodedPalette), encodedPalette);

        if (_map->DirtyLocs.size() == 0) return false;

        std::unordered_set<glm::uvec3> chunkLocs;
        auto mesh = std::make_unique<MeshData>();  // too big to be on stack

        for (auto iter = _map->DirtyLocs.begin(); iter != _map->DirtyLocs.end();) {
            auto [sectorIdx, dirtyMask] = *iter;
            auto currIter = iter++;

            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            glm::ivec3 chunkPos = (sectorPos * Brick::Size * MaskIndexer::Size) / CS_P;

            // Generate mesh if we're within sync budget
            if (chunkLocs.size() < 32 && chunkLocs.insert(chunkPos).second) {
                RepackChunkVoxels(*_map, chunkPos, *mesh);
                mesh->generate();

                ChunkInfo& chunk = LoadedChunks[chunkPos];
                uint32_t currAddr = chunk.BaseVertexOffset;
                uint32_t currAllocSize = chunk.GetTotalVertexCount();

                chunk.BaseVertexOffset = RangeAllocator.Realloc(chunk.BaseVertexOffset, chunk.GetTotalVertexCount(), mesh->vertices.size());

                StorageBuffer->Write(mesh->vertices.data(), offsetof(GpuStorageData, QuadData) +  chunk.BaseVertexOffset * sizeof(uint64_t), mesh->vertices.size()* sizeof(uint64_t));

                uint32_t runningOffset = 0;
                for (uint32_t face = 0; face < 6; face++) {
                    assert(mesh->faceVertexBegin[face] == runningOffset);

                    chunk.NumFaceVertices[face] = mesh->faceVertexLength[face];
                    runningOffset += mesh->faceVertexLength[face];
                }
            }

            // We have to iterate over entire queue to flush all sectors covering a chunk,
            // only remove sectors that will be actually updated this call.
            if (chunkLocs.contains(chunkPos)) {
                _map->DirtyLocs.erase(currIter);
            }
        }

        return chunkLocs.size() > 0;
    }

    void CreateIndexBuffer(havk::CommandList& cmds) {
        uint32_t maxQuads = (CS * CS * CS * 6) / 4;

        IndexBuffer = _ctx->CreateBuffer({
            .Size = maxQuads * 6 * sizeof(uint32_t),
            .Usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
        });
        auto stagingBuffer = _ctx->CreateBuffer({
            .Size = maxQuads * 6 * sizeof(uint32_t),
            .Usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        });
        auto indices = (uint32_t*)stagingBuffer->MappedData;

        for (uint32_t i = 0; i < maxQuads; i++) {
            indices[i * 6 + 0] = (i << 2) | 2;
            indices[i * 6 + 1] = (i << 2) | 0;
            indices[i * 6 + 2] = (i << 2) | 1;
            indices[i * 6 + 3] = (i << 2) | 1;
            indices[i * 6 + 4] = (i << 2) | 3;
            indices[i * 6 + 5] = (i << 2) | 2;
        }
        cmds.CopyBuffer(*stagingBuffer, *IndexBuffer);
        cmds.Barrier(*IndexBuffer, havk::UseBarrier::All);
    }
};

};  // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::Mesh>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererMesh>(ctx, map);
}
