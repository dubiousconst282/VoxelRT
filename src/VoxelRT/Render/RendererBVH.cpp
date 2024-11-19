#include "Renderer.h"

#include <bvh/v2/bvh.h>
#include <bvh/v2/default_builder.h>

using BNode = bvh::v2::Node<float, 3, 32>;
using BBox = bvh::v2::BBox<float, 3>;
using BVec = bvh::v2::Vec<float, 3>;
using BvhBuilder = bvh::v2::DefaultBuilder<BNode>;
using Bvh = bvh::v2::Bvh<BNode>;

namespace {

// TODO: Implement custom BVH builder to take advantage of our use case:
// - overlaps never happen
// - bvh_v2 does not compile with integer scalars, pointless templatization syndrome
// - could minimize number of boxes via greedy merging? might complicate leaf indexing
// also read this stuff:
// - https://meistdan.github.io/publications/bvh_star/paper.pdf
// - https://research.nvidia.com/sites/default/files/publications/ylitie2017hpg-paper.pdf

//
struct PackedNode {
    glm::i16vec3 Min, Max;
    uint32_t IsLeaf : 1;
    uint32_t ChildPtr : 31;
};
struct PackedBrick {
    uint64_t PopMask[Brick::NumVoxels / 64];
};

void RepackBrick(const Brick& brick, glm::ivec3 bounds[2], PackedBrick& packed) {
    Brick::GetOccupancyMask(&brick, packed.PopMask);

    // Bricks are indexed as `x + z*8 + y*64`
    // each u64 = one xz layer, each u8 = one x row
    static_assert(Brick::NumVoxels == 8 * 8 * 8);

    uint8_t combinedY = 0;
    uint64_t combinedXZ = 0;

    for (uint32_t i = 0; i < 8; i++) {
        combinedXZ |= packed.PopMask[i];
        combinedY |= uint32_t(packed.PopMask[i] != 0) << i;
    }

    // OR all bytes 
    uint64_t combinedX = combinedXZ;
    combinedX |= (combinedX >> 32);
    combinedX |= (combinedX >> 16);
    combinedX |= (combinedX >> 8);
    combinedX &= 255;

    bounds[0].x = std::countr_zero(combinedX);
    bounds[1].x = 64 - std::countl_zero(combinedX);

    bounds[0].z = std::countr_zero(combinedXZ) / 8;
    bounds[1].z = (64 - std::countl_zero(combinedXZ) + 7) / 8;

    bounds[0].y = std::countr_zero(combinedY);
    bounds[1].y = 8 - std::countl_zero(combinedY);
}

PackedNode RepackNode(Bvh& bvh, const BNode& rawNode, std::vector<PackedNode>& nodes) {
    auto bb = rawNode.get_bbox();

    PackedNode node = {
        .Min = glm::i16vec3(bb.min.values[0], bb.min.values[1], bb.min.values[2]),
        .Max = glm::i16vec3(bb.max.values[0], bb.max.values[1], bb.max.values[2]),
        .IsLeaf = rawNode.is_leaf(),
    };
    if (!rawNode.is_leaf()) {
        auto childA = RepackNode(bvh, bvh.nodes[rawNode.index.first_id() + 0], nodes);
        auto childB = RepackNode(bvh, bvh.nodes[rawNode.index.first_id() + 1], nodes);

        node.ChildPtr = nodes.size();
        nodes.push_back(childA);
        nodes.push_back(childB);
    } else {
        assert(rawNode.index.prim_count() == 1);
        node.ChildPtr = bvh.prim_ids[rawNode.index.first_id()];
    }
    return node;
}

Bvh GenerateBVH(VoxelMap& map, std::vector<PackedBrick>& leafs) {
    std::vector<BBox> boxes;
    std::vector<BVec> centers;

    for (auto& [idx, sector] : map.Sectors) {
        auto sectorPos = WorldSectorIndexer::GetPos(idx);

        for (uint32_t i : BitIter(sector.GetAllocationMask())) {
            Brick* brick = sector.GetBrick(i);
            auto brickPos = sectorPos * 32 + MaskIndexer::GetPos(i) * 8;

            glm::ivec3 bounds[2];
            RepackBrick(*brick, bounds, leafs.emplace_back());

            auto& bb = boxes.emplace_back(BBox(
                BVec(bounds[0].x + brickPos.x, bounds[0].y + brickPos.y, bounds[0].z + brickPos.z),
                BVec(bounds[1].x + brickPos.x, bounds[1].y + brickPos.y, bounds[1].z + brickPos.z)
            ));
            centers.push_back(bb.get_center());
        }
    }

    auto config = BvhBuilder::Config();
    // For simplicity we only support one brick per leaf.
    // This increases memory usage but doesn't seem to affect performance much
    config.max_leaf_size = 1;
    config.quality = BvhBuilder::Quality::High;

    // NOTE: Parallel build generates broken trees (finding this out was fun...)
    return BvhBuilder::build(boxes, centers, config);
}

struct GpuVoxelMap {
    VkDeviceAddress Nodes;
    VkDeviceAddress Bricks;
    uint32_t RootIdx;
};

struct RendererBVH : public GpuRenderer {
    havk::BufferPtr StorageBuffer;
    uint32_t RootIdx;
    size_t BrickDataOffset;

    RendererBVH(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : GpuRenderer(ctx, map, RendererId::BrickBVH) {
        _map->MarkAllDirty();
    }

    void RenderFrame(havx::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        GpuRenderer::DispatchRenderShader(cam, target, cmds, GpuVoxelMap {
            .Nodes = cmds.GetDeviceAddress(*StorageBuffer, havk::UseBarrier::ComputeRead),
            .Bricks = StorageBuffer->DeviceAddress + BrickDataOffset,
            .RootIdx = RootIdx,
        });
    }

    [[clang::optnone]]
    bool SyncMap(havx::Camera& cam, havk::CommandList& cmds) override {
        if (StorageBuffer != nullptr && _map->DirtyLocs.size() == 0) return false;
        _map->DirtyLocs.clear();

        std::vector<PackedNode> nodes;
        std::vector<PackedBrick> bricks;
        
        auto bvh = GenerateBVH(*_map, bricks);
        auto root = RepackNode(bvh, bvh.get_root(), nodes);
        RootIdx = nodes.size();
        nodes.push_back(root);

        StorageBuffer = _ctx->CreateBuffer({
            .Size = nodes.size() * sizeof(PackedNode) + bricks.size() * sizeof(PackedBrick),
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        });

        // TODO: make this copy staged to ensure device_local memory on non-UMA hardware
        StorageBuffer->Write(nodes.data(), 0, nodes.size() * sizeof(PackedNode));

        BrickDataOffset = nodes.size() * sizeof(PackedNode);
        StorageBuffer->Write(bricks.data(), BrickDataOffset, bricks.size() * sizeof(PackedBrick));

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
std::unique_ptr<Renderer> Renderer::Create<RendererId::BrickBVH>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererBVH>(ctx, map);
}
