#include "Renderer.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include <unordered_set>

namespace {

struct [[gnu::packed]] CompressedNode {
    uint32_t IsLeaf : 1 = 0;
    uint32_t IsAbsolutePtr : 1 = 0;     // For bookkeeping. Indicates ChildPtr is absolute within storage buffer and needs not be patched.
    uint32_t ChildPtr : 30 = 0;
    uint64_t PopMask = 0;
};

// Distribution of nodes per tree level (Bistro 4k):
// L1(4)   9 317 749
// L2(16)    457 323
// L3(64)     21 152
// L4(256)       967
// L5(1024)       38
// L6(4096)        2
static constexpr uint32_t kChunkScale = 6; // 64
static constexpr uint32_t kChunkSize = 1 << kChunkScale;

struct RenderClusterInfo {
    uint64_t PopMask = 0;
    VmaVirtualAllocation ChunkRanges[64] = {};
    CompressedNode ChunkRoots[64] = {};
};

// Encode to Z2-order curve (with interleaving in 2-bit chunks).
uint64_t GetClusterKey(glm::ivec3 pos) {
    // At most 20 bits per component, last 4 bits are unused for simplicity.
    assert(glm::all(glm::equal(pos, pos & 0xFFFFF)));
    glm::u64vec3 u = pos;

#if __BMI2__
    return _pdep_u64(u.x, 0x00C30C30C30C30C3ull) |  // 0b0_000...00_00_11
           _pdep_u64(u.z, 0x030C30C30C30C30Cull) |  // 0b0_000...00_11_00
           _pdep_u64(u.y, 0x0C30C30C30C30C30ull);   // 0b0_000...11_00_00
#endif
    // https://forceflow.be/2013/10/07/morton-encodingdecoding-through-bit-interleaving-implementations/
    u = (u | (u << 32ull)) & 0x000F00000000FFFFull;
    u = (u | (u << 16ull)) & 0x000F0000FF0000FFull;
    u = (u | (u << 8ull))  & 0x000F00F00F00F00Full;
    u = (u | (u << 4ull))  & 0x00C30C30C30C30C3ull;
    return u.x | (u.z << 2) | (u.y << 4);
}

// Left-pack bytes according to mask. Leading values are undefined.
void LeftPack(uint8_t data[64], uint64_t mask) {
#if SIMD_AVX512
    _mm512_storeu_epi8(data, _mm512_maskz_compress_epi8(mask, _mm512_loadu_epi8(data)));
    return;
#endif

    for (uint32_t i = 0, j = 0; mask != 0; i++) {
        data[j] = data[i];
        j += (mask & 1);
        mask >>= 1;
    }
}

// Recursive top-down grid builder
CompressedNode BuildChunkTree(VoxelMap& map, std::vector<CompressedNode>& nodePool, std::vector<uint8_t>& leafData, int32_t scale, glm::ivec3 pos) {
    CompressedNode node;

    // Create leaf
    if (scale == 2) {
        assert((pos.x | pos.y | pos.z) % 4 == 0);

        Brick* brick = map.GetBrick(pos >> BrickIndexer::ShiftXZ);
        if (brick == nullptr) return node;

        alignas(64) uint8_t temp[64];

        for (int32_t i = 0; i < 64; i += 4) {
            int32_t offset = BrickIndexer::GetIndex(pos.x, pos.y + (i >> 4 & 3), pos.z + (i >> 2 & 3));
            memcpy(&temp[i], &brick->Data[offset], 4);
        }
        node.PopMask = Brick::PackBits64(temp);
        node.IsLeaf = 1;

        LeftPack(temp, node.PopMask);

        node.ChildPtr = leafData.size() / 4;
        int size = (std::popcount(node.PopMask) + 3) & ~3;
        leafData.insert(leafData.end(), temp, temp + size);

        return node;
    }

    // Descend
    scale -= 2;

    std::vector<CompressedNode> children;

    for (int32_t i = 0; i < 64; i++) {
        glm::ivec3 childPos = i >> glm::ivec3(0, 4, 2) & 3;
        CompressedNode child = BuildChunkTree(map, nodePool, leafData, scale, pos + (childPos << scale));

        if (child.PopMask != 0) {
            node.PopMask |= 1ull << i;
            children.push_back(child);
        }
    }

    node.ChildPtr = nodePool.size();
    nodePool.insert(nodePool.end(), children.begin(), children.end());

    return node;
}

// Bottom-up agglomerating builder (similar to PLOC). Requires input nodes to be sorted in a 2-bit Z-curve.
CompressedNode BuildTLAS(std::map<uint64_t, RenderClusterInfo>& map, std::vector<CompressedNode>& nodePool, int32_t rootScale) {
    std::vector<std::pair<uint64_t, CompressedNode>> layerNodes;

    // Build initial input nodes
    for (auto& [pos, cluster] : map) {
        CompressedNode node = {
            .ChildPtr = (uint32_t)nodePool.size(),
            .PopMask = cluster.PopMask,
        };
        for (uint32_t i : BitIter(cluster.PopMask)) {
            nodePool.push_back(cluster.ChunkRoots[i]);
        }
        layerNodes.push_back({ pos << (kChunkScale * 3), node });
    }

    uint32_t scale = kChunkScale + 2;

    // Build tree layers until requested scale
    while (scale < rootScale) {
        uint32_t j = 0;

        for (uint32_t i = 0; i < layerNodes.size(); ) {
            CompressedNode parent = {
                .ChildPtr = (uint32_t)nodePool.size()
            };
            uint64_t parentMask = ~0ull << (scale * 3);
            uint64_t parentPos = layerNodes[i].first & parentMask;

            // Collect all nodes that are inside this parent.
            // Since we expect input to be sorted in a Z-curve,
            // we can break out of the loop as soon as we find a 
            // node that is outside the parent bounds.
            for (; i < layerNodes.size(); i++) {
                auto& [childPos, childNode] = layerNodes[i];
                if ((childPos ^ parentPos) & parentMask) break;

                parent.PopMask |= 1ull << (childPos >> ((scale - 2) * 3));
                nodePool.push_back(childNode);
            }
            // Write resulting node
            layerNodes[j++] = { parentPos, parent };
        }

        // Prepare for next layer
        layerNodes.resize(j);
        scale += 2;
    }

    return layerNodes[0].second;
}

struct GpuVoxelMap {
    uint32_t TreeScale;
    VkDeviceAddress TreeNodes;
};
struct RendererTree64 : public GpuRenderer {
    havk::BufferPtr StorageBuffer;
    uint32_t TreeScale = 4;

    std::map<uint64_t, RenderClusterInfo> Clusters;
    VmaVirtualBlock ChunkRangeAllocator;
    VmaVirtualAllocation TopRange = nullptr;

    RendererTree64(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : GpuRenderer(ctx, map, RendererId::Tree64) {
        _map->MarkAllDirty();
        
        StorageBuffer = _ctx->CreateBuffer({
            .Size = 1024 * 1024 * 1024, // 1GB
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        });
        VmaVirtualBlockCreateInfo allocatorCI = { .size = StorageBuffer->Size };
        VK_CHECK(vmaCreateVirtualBlock(&allocatorCI, &ChunkRangeAllocator));

        // Reserve first slot for root node
        VmaVirtualAllocationCreateInfo rangeCI = { .size = sizeof(CompressedNode) };
        VmaVirtualAllocation range;
        size_t offset = 0;
        VK_CHECK(vmaVirtualAllocate(ChunkRangeAllocator, &rangeCI, &range, &offset));
        assert(offset == 0);
    }
    ~RendererTree64() {
        vmaClearVirtualBlock(ChunkRangeAllocator);
        vmaDestroyVirtualBlock(ChunkRangeAllocator);
    }

    void RenderFrame(havx::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap {
            .TreeScale = TreeScale,
            .TreeNodes = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeRead),
        });
    }

    bool SyncMap(havx::Camera& cam, havk::CommandList& cmds) override {
        if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
            _ctx->WaitDeviceIdle();
            _map->MarkAllDirty();
            vmaClearVirtualBlock(ChunkRangeAllocator);

            Clusters.clear();
            TopRange = nullptr;
            cmds.Fill(*StorageBuffer, 0, 0, sizeof(CompressedNode));
            return false;
        }
        if (_map->DirtyLocs.size() == 0) return false;

        constexpr glm::ivec3 kSectorSize = Brick::Size * MaskIndexer::Size;
        constexpr int kMaxChunkUpdates = 90; // worst ~15ms

        std::unordered_set<glm::uvec3> chunkLocs;

        // Group dirty sectors (32^3) into chunks
        for (auto iter = _map->DirtyLocs.begin(); iter != _map->DirtyLocs.end();) {
            auto [sectorIdx, dirtyMask] = *iter;
            auto currIter = iter++;

            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            glm::ivec3 chunkPos = (sectorPos * kSectorSize) / (int)kChunkSize;

            bool inRange = glm::all(glm::greaterThanEqual(chunkPos, glm::ivec3(0)));

            if (chunkLocs.size() < kMaxChunkUpdates && inRange) {
                chunkLocs.insert(chunkPos);
                inRange = false; // opt: avoid extra lookup
            }
            // Only remove sectors that are going to be updated this time
            if (!inRange || chunkLocs.contains(chunkPos)) {
                _map->DirtyLocs.erase(currIter);
            }
        }

        if (chunkLocs.size() == 0) return false;

        // We'll be writing directly to the mapped buffer storage, so range deletions must be
        // delayed by a few frames to avoid overwriting data that the GPU may still be using.
        // It would probably be better to use a staging buffer, but this is a bit simpler to manage.
        std::vector<VmaVirtualAllocation> deletionQueue;
        std::vector<CompressedNode> nodes;
        std::vector<uint8_t> leafData;

        // Rebuild dirty chunks
        for (auto chunkPos : chunkLocs) {
            uint32_t extentCoord = glm::max(glm::max(chunkPos.x, chunkPos.y), glm::max(chunkPos.z, 1u)) * kChunkSize;
            uint32_t extentScale = (uint32_t)(std::bit_width(extentCoord) + 1) & ~1u;  // ceil log4
            TreeScale = glm::max(TreeScale, extentScale);

            uint64_t clusterId = GetClusterKey(chunkPos / 4u);
            uint64_t chunkIdx = MaskIndexer::GetIndex(chunkPos);

            RenderClusterInfo& cluster = Clusters[clusterId];
            VmaVirtualAllocation& range = cluster.ChunkRanges[chunkIdx];

            if (range != nullptr) {
                deletionQueue.push_back(range);
                range = nullptr;
            }

            CompressedNode root = BuildChunkTree(*_map, nodes, leafData, kChunkScale, chunkPos * kChunkSize);

            // If chunk is empty, stop tracking it.
            if (nodes.size() == 0) {
                cluster.PopMask &= ~(1ull << chunkIdx);

                if (cluster.PopMask == 0) {
                    Clusters.erase(clusterId);
                }
                continue;
            }
            cluster.PopMask |= (1ull << chunkIdx);

            size_t offset = CopyNodesToStorage(nodes, leafData.size(), root, &range);
            cluster.ChunkRoots[chunkIdx] = root;

            if (offset == 0) {
                // Ran out of storage, just stop for now.
                break;
            }
            uint8_t* dest = (uint8_t*)StorageBuffer->MappedData + offset;
            memcpy(dest, leafData.data(), leafData.size());
            
            nodes.clear();
            leafData.clear();
        }

        // Rebuild top-level tree
        CompressedNode topRoot = BuildTLAS(Clusters, nodes, (int)TreeScale);

        if (TopRange != nullptr) {
            deletionQueue.push_back(TopRange);
            TopRange = nullptr;
        }
        size_t topOffset = CopyNodesToStorage(nodes, 0, topRoot, &TopRange);
        if (topOffset == 0) {
            throw std::runtime_error("Ran out of space for tree storage!");
        }
        // Update root node on device timeline for same reason as deletion queue
        cmds.UpdateBuffer(*StorageBuffer, 0, sizeof(CompressedNode), &topRoot);

        // TODO: ...actually flush deletion queue without stalling
        // _ctx->EnqueueDeletion(Resource *ptr);
        _ctx->WaitDeviceIdle();
        for (auto& range : deletionQueue) {
            vmaVirtualFree(ChunkRangeAllocator, range);
        }

        return true;
    }

    size_t CopyNodesToStorage(std::vector<CompressedNode>& nodes, size_t leafDataSize, CompressedNode& rootNode, VmaVirtualAllocation* outRange) {
        constexpr size_t align = sizeof(CompressedNode);
        VmaVirtualAllocationCreateInfo rangeCI = {
            .size = nodes.size() * sizeof(CompressedNode) + ((leafDataSize + align - 1) / align * align)
        };
        size_t offset = 0;

        if (vmaVirtualAllocate(ChunkRangeAllocator, &rangeCI, outRange, &offset) != VK_SUCCESS) {
            return 0;
        }

        assert(offset % sizeof(CompressedNode) == 0);

        // Patch offsets and copy to storage buffer.
        uint8_t* dest = (uint8_t*)StorageBuffer->MappedData + offset;
        size_t leafDataOffset = offset + nodes.size() * sizeof(CompressedNode);
        size_t nodeOffset = offset / sizeof(CompressedNode);

        rootNode.IsAbsolutePtr = 1;
        rootNode.ChildPtr += nodeOffset;

        for (CompressedNode node : nodes) {
            if (node.IsLeaf) {
                node.ChildPtr += leafDataOffset / 4;
            } else if (!node.IsAbsolutePtr) {
                node.ChildPtr += nodeOffset;
            }
            memcpy(dest, &node, sizeof(CompressedNode));
            dest += sizeof(CompressedNode);
        }
        return leafDataOffset;
    }

    void DrawSettings(havx::SettingStore& settings) override {
        GpuRenderer::DrawSettings(settings);

        if (StorageBuffer != nullptr) {
            VmaStatistics stats;
            vmaGetVirtualBlockStatistics(ChunkRangeAllocator, &stats);
            ImGui::Text("Storage: %.1fMB/%.1fMB", stats.allocationBytes / 1048576.0, StorageBuffer->Size / 1048576.0);
        }
    }
};

};  // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::Tree64>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererTree64>(ctx, map);
}
