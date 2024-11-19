#include "Renderer.h"

namespace {

struct [[gnu::packed]] RawNode {
    uint32_t IsLeaf : 1 = 0;
    uint32_t ChildPtr : 31 = 0;
    uint64_t PopMask = 0;
};

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

bool IsEmptyCluster(VoxelMap& map, glm::ivec3 pos, int scale) {
    const auto sectorShift = BrickIndexer::Shift + MaskIndexer::Shift;
    glm::ivec3 startPos = (pos >> sectorShift), endPos = (pos + scale - 1) >> sectorShift;

    for_yzx_inclusive(glm::ivec3, sectorPos, startPos, endPos) {
        if (map.GetSector(sectorPos) != nullptr) {
            return false;
        }
    }
    return true;
}

RawNode GenerateTree(VoxelMap& map, std::vector<RawNode>& data, std::vector<uint8_t>& leafData, int32_t scale, glm::ivec3 pos = {}) {
    RawNode node;

    // FIXME: Don't bother descending into empty regions
    if ((scale == 6 || scale == 4) && IsEmptyCluster(map, pos, 1 << scale)) {
        return node;
    }

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

        node.ChildPtr = leafData.size();
        leafData.insert(leafData.end(), temp, temp + std::popcount(node.PopMask));

        return node;
    }

    // Descend
    scale -= 2;

    std::vector<RawNode> children;

    for (int32_t i = 0; i < 64; i++) {
        glm::ivec3 childPos = i >> glm::ivec3(0, 4, 2) & 3;
        RawNode child = GenerateTree(map, data, leafData, scale, pos + (childPos << scale));

        if (child.PopMask != 0) {
            node.PopMask |= 1ull << i;
            children.push_back(child);
        }
    }

    node.ChildPtr = data.size();
    data.insert(data.end(), children.begin(), children.end());

    return node;
}

struct GpuVoxelMap {
    uint32_t TreeScale;
    VkDeviceAddress TreeNodes;
    VkDeviceAddress LeafData;
};

struct RendererTree64 : public GpuRenderer {
    havk::BufferPtr StorageBuffer;
    uint32_t TreeScale;
    size_t LeafDataOffset;

    RendererTree64(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : GpuRenderer(ctx, map, RendererId::Tree64) {
        _map->MarkAllDirty();
    }

    void RenderFrame(havx::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap {
            .TreeScale = TreeScale,
            .TreeNodes = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeRead),
            .LeafData = StorageBuffer->DeviceAddress + LeafDataOffset,
        });
    }

    bool SyncMap(havx::Camera& cam, havk::CommandList& cmds) override {
        if (StorageBuffer != nullptr && _map->DirtyLocs.size() == 0) return false;
        _map->DirtyLocs.clear();

        TreeScale = 14;

        std::vector<RawNode> nodes;
        std::vector<uint8_t> leafData;

        nodes.resize(1);

        nodes[0] = GenerateTree(*_map, nodes, leafData, int32_t(TreeScale), glm::ivec3(0));
        LeafDataOffset = nodes.size() * sizeof(RawNode);

        StorageBuffer = _ctx->CreateBuffer({
            .Size = nodes.size() * sizeof(RawNode) + leafData.size(),
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        });

        // TODO: make this copy staged to ensure device_local memory on non-UMA hardware
        StorageBuffer->Write(nodes.data(), 0, nodes.size() * sizeof(RawNode));
        StorageBuffer->Write(leafData.data(), LeafDataOffset, leafData.size());
        
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
std::unique_ptr<Renderer> Renderer::Create<RendererId::Tree64>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererTree64>(ctx, map);
}
