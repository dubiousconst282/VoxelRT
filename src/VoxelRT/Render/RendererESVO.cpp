#include "Renderer.h"

namespace {

// ESVO calls this "child descriptor"
//
// Some changes:
// - Pointers are backwards relative, so that tree can be generated in one pass without copies or relocations.
// - Far pointers are defined in range 0xFF00..0xFFFF instead of a dedicated bit.
//
struct RawNode {
    uint8_t NonLeafMask = 0;
    uint8_t ValidMask = 0;
    uint32_t ChildIdx = 0;  // 16-bit when encoded + 32-bit when too far
};

static RawNode GenerateTree(VoxelMap& map, std::vector<uint32_t>& data, uint32_t scale, glm::uvec3 pos = {}) {
    RawNode node;

    // Don't bother descending into empty sectors or bricks
    if (scale == BrickIndexer::ShiftXZ + MaskIndexer::ShiftXZ &&
        map.GetSector(pos / glm::uvec3(Brick::Size * MaskIndexer::Size)) == nullptr) {
        return RawNode();
    }
    if (scale == BrickIndexer::ShiftXZ && map.GetBrick(pos / glm::uvec3(Brick::Size)) == nullptr) {
        return RawNode();
    }

    // Create leaf
    if (scale == 1) {
        Brick* brick = map.GetBrick(pos / glm::uvec3(Brick::Size));

        uint8_t temp[8];
        memcpy(&temp[0], &brick->Data[BrickIndexer::GetIndex(pos.x, pos.y + 0, pos.z + 0)], 2);
        memcpy(&temp[2], &brick->Data[BrickIndexer::GetIndex(pos.x, pos.y + 1, pos.z + 0)], 2);
        memcpy(&temp[4], &brick->Data[BrickIndexer::GetIndex(pos.x, pos.y + 0, pos.z + 1)], 2);
        memcpy(&temp[6], &brick->Data[BrickIndexer::GetIndex(pos.x, pos.y + 1, pos.z + 1)], 2);

        node.NonLeafMask = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadl_epi64((__m128i*)temp), _mm_setzero_si128()));
        node.ValidMask = ~node.NonLeafMask;
        return node;
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

    // Encode children
    std::vector<uint32_t> farOffsets;

    for (uint32_t i = 0; i < 8; i++) {
        if (!(node.ValidMask >> i & 1)) continue;

        uint8_t popMask = child[i].ValidMask & child[i].NonLeafMask;
        uint32_t offset = popMask ? data.size() - child[i].ChildIdx : 0;

        // If offset is too far, encode it separately following children nodes
        if (offset >= 0xFF00) {
            uint32_t farSlotIdx = node.ChildIdx + uint32_t(std::popcount(node.ValidMask)) + farOffsets.size();
            farOffsets.push_back(offset);
            offset = 0xFF00 + (farSlotIdx - data.size());
        }
        data.push_back(uint16_t(child[i].NonLeafMask | child[i].ValidMask << 8) | offset << 16);
    }
    if (farOffsets.size() != 0) {
        data.insert(data.end(), farOffsets.begin(), farOffsets.end());
    }
    return node;
}

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
        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap{
            .RootNode = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeRead) + RootIndex * 4,
            .TreeScale = TreeScale,
        });
    }

    bool SyncMap(glim::Camera& cam, havk::CommandList& cmds) override {
        if (StorageBuffer != nullptr && _map->DirtyLocs.size() == 0) return false;
        _map->DirtyLocs.clear();

        TreeScale = 12;

    std::vector<uint32_t> data;
    RawNode root = GenerateTree(*_map, data, TreeScale, glm::uvec3(0));

    RootIndex = data.size();
    data.push_back(uint16_t(root.NonLeafMask | root.ValidMask << 8) | (data.size() - root.ChildIdx) << 16);

    StorageBuffer = _ctx->CreateBuffer({
        .Size = data.size() * sizeof(uint32_t),
        .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    });

        // TODO: make this copy staged to ensure device_local memory on non-UMA hardware
    StorageBuffer->Write(data.data(), 0, data.size() * sizeof(uint32_t));

        return true;
    }

    void DrawSettings(glim::SettingStore& settings) override {
        GpuRenderer::DrawSettings(settings);

        if (StorageBuffer != nullptr) {
            ImGui::Text("Storage: %.1fMB", StorageBuffer->Size / 1048576.0);
        }
    }
};

};  // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::ESVO>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererESVO>(ctx, map);
}
