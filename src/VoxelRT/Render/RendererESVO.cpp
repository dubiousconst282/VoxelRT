#include "Renderer.h"

namespace {

struct GpuVoxelMap {
    VkDeviceAddress RootNode;
    uint32_t TreeScale;
};

struct RendererESVO : public GpuRenderer {
    havk::BufferPtr StorageBuffer;
    uint32_t TreeScale;
    uint32_t RootIndex;

    RendererESVO(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : GpuRenderer(ctx, map, RendererId::ESVO) {
        _map->MarkAllDirty();
    }

    void RenderFrame(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        // Sync buffers
        if (ImGui::IsKeyPressed(ImGuiKey_F9) || StorageBuffer == nullptr || _map->DirtyLocs.size()) {
            SyncMap(cmds);
            _map->DirtyLocs.clear();
        }

        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap {
            .RootNode = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeRead) + RootIndex * 4,
            .TreeScale = TreeScale,
        });
    }

    void SyncMap(havk::CommandList& cmds);
};

// ESVO calls this "child descriptor"
//
// Some changes:
// - Pointers are backwards relative, so that tree to be generated in a single pass and without relocations.
// - Far pointers are defined in range 0xFF00..0xFFFF instead of a dedicated bit.
//
struct RawNode {
    uint8_t NonLeafMask = 0;
    uint8_t ValidMask = 0;
    uint32_t ChildIdx = 0;
};

RawNode GenerateTree(VoxelMap& map, std::vector<uint32_t>& data, uint32_t scale, glm::uvec3 pos = {}) {
    RawNode node;

    // Don't bother recursing into empty sectors
    if (scale == BrickIndexer::ShiftXZ + MaskIndexer::ShiftXZ && !map.GetSector(pos / glm::uvec3(Brick::Size * MaskIndexer::Size))) {
        return RawNode();
    }

    if (scale <= BrickIndexer::ShiftXZ) {
        Brick* brick = map.GetBrick(pos / glm::uvec3(Brick::Size));
        if (!brick) return RawNode();

        // Create leaf
        if (scale == 1) {
            uint8_t temp[8];
            memcpy(&temp[0], &brick->Data[BrickIndexer::GetIndex(pos.x, pos.y + 0, pos.z + 0)], 2);
            memcpy(&temp[2], &brick->Data[BrickIndexer::GetIndex(pos.x, pos.y + 1, pos.z + 0)], 2);
            memcpy(&temp[4], &brick->Data[BrickIndexer::GetIndex(pos.x, pos.y + 0, pos.z + 1)], 2);
            memcpy(&temp[6], &brick->Data[BrickIndexer::GetIndex(pos.x, pos.y + 1, pos.z + 1)], 2);
            node.ValidMask = _mm_movemask_epi8(_mm_loadl_epi64((__m128i*)temp));
            node.NonLeafMask = ~node.ValidMask;
            return node;
        }
    }

    // Descend
    RawNode child[8];
    uint8_t childLeafMask = 0;

    scale--;

    for (uint32_t i = 0; i < 8; i++) {
        glm::uvec3 childPos = i >> glm::uvec3(0, 1, 2) & 1u;
        child[i] = GenerateTree(map, data, scale, pos + (childPos << scale));

        if (child[i].ValidMask != 0) {
            node.ValidMask |= 1u << i;
            childLeafMask |= uint8_t(child[i].ValidMask == 255 && child[i].NonLeafMask == 0) << i;
        }
    }

    // If all children are fully populated leafs, prune and make this node a full leaf too
    if (childLeafMask == 255) {
        node.ValidMask = 255;
        node.NonLeafMask = 0;
        return node;
    }

    node.ChildIdx = data.size();
    node.NonLeafMask = node.ValidMask;

    uint32_t childIdx = data.size();
    std::vector<uint32_t> extendedOffsets;

    // Encode children
    for (uint32_t i = 0, j = 0; i < 8; i++) {
        if (!(node.ValidMask >> i & 1)) continue;

        uint8_t popMask = child[i].ValidMask & child[i].NonLeafMask;
        uint32_t offset = popMask ? childIdx - child[i].ChildIdx : 0;

        if (offset >= 0xFF00) {
            extendedOffsets.push_back(offset);
            offset = 0xFF00 + childIdx - (node.ChildIdx + uint32_t(std::popcount(node.ValidMask)));
            assert(offset <= 0xFFFF);
        }
        data.push_back(uint16_t(child[i].NonLeafMask | child[i].ValidMask << 8) | offset << 16);
        childIdx++;
    }
    if (extendedOffsets.size() != 0) {
        data.insert(data.end(), extendedOffsets.begin(), extendedOffsets.end());
    }
    return node;
}

void RendererESVO::SyncMap(havk::CommandList& cmds) {
    TreeScale = 8;

    std::vector<uint32_t> data;
    RawNode root = GenerateTree(*_map, data, TreeScale, glm::uvec3(0));

    RootIndex = data.size();
    data.push_back(uint16_t(root.NonLeafMask | root.ValidMask << 8) | (data.size() - root.ChildIdx) << 16);

    printf("Nodes: %zu\n", data.size());

    StorageBuffer = _ctx->CreateBuffer({
        .Size = data.size() * sizeof(uint32_t),
        .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    });
    StorageBuffer->Write(data.data(), 0, data.size() * sizeof(uint32_t));
}

};  // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::ESVO>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererESVO>(ctx, map);
}
