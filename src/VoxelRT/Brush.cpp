#include "Brush.h"

#include <chrono>

// https://iquilezles.org/articles/distfunctions/
static VFloat sdCapsule(VFloat3 p, glm::vec3 a, glm::vec3 b, float r) {
    VFloat3 pa = p - a, ba = b - a;
    VFloat h = simd::clamp(simd::dot(pa, ba) / simd::dot(ba, ba), 0.0, 1.0);
    return simd::length(pa - ba * h) - r;
}

void BrushSession::Dispatch(VoxelMap& map) {
    glm::ivec3 minPos = glm::min(Pars.PointA, Pars.PointB) - (int)(Pars.Radius + 0.5);
    glm::ivec3 maxPos = glm::max(Pars.PointA, Pars.PointB) + (int)(Pars.Radius + 0.5);
    bool isErasing = Pars.Material.IsEmpty();
    VRandom rng(Pars.RandomSeed);

    auto start = std::chrono::high_resolution_clock::now();
    uint32_t visits = 0;

    map.RegionDispatchSIMD(
        minPos, maxPos, !isErasing,
        [&](VInt3 pos, VInt& voxelIds) {
            VFloat3 samplePos = VFloat3(simd::conv2f(pos.x), simd::conv2f(pos.y), simd::conv2f(pos.z)) + 0.5f;
            VMask mask = sdCapsule(samplePos, Pars.PointA, Pars.PointB, Pars.Radius) < 0.0;

            if (Pars.Probability < 1.0f) {
                mask &= rng.NextUnsignedFloat() < Pars.Probability;
            }
            if (Pars.Action == BrushAction::Replace) {
                mask &= voxelIds != 0;
            }
            voxelIds.set_if(mask, Pars.Material.Data);
            visits++;
            return simd::any(mask);
        },
        // Generate filter mask for bricks that might be affected
        [&](glm::ivec3 sectorPos, Sector& sector) -> uint64_t {
            uint64_t mask = sector.GetAllocationMask();
            if (isErasing && mask == 0) return 0;

            const float kBrickMinDist = sqrt(3) * (Brick::Size.x / 2.0f); // dist threshold for point at cube center
            const float kSectorMinDist = sqrt(3) * (Brick::Size.x * 4.0f); // dist threshold for point anywhere in cube

            glm::vec3 offsetPos = sectorPos * MaskIndexer::Size * Brick::Size;
            offsetPos += glm::vec3(Brick::Size) / 2.0f;

            for (int32_t i = 0; i < 64; i += simd::VectorWidth) {
                VInt3 brickPos = MaskIndexer::GetPos<VInt3>(i + simd::LaneIdx);
                VFloat3 pos = VFloat3(simd::conv2f(brickPos.x), simd::conv2f(brickPos.y), simd::conv2f(brickPos.z));
                pos = offsetPos + pos * glm::vec3(Brick::Size);

                VFloat dist = sdCapsule(pos, Pars.PointA, Pars.PointB, Pars.Radius);
                if (i == 0 && simd::all(dist > kSectorMinDist)) return 0; // early bail if we know sector won't be affected

                mask |= uint64_t(simd::movemask(dist < kBrickMinDist)) << i;
            }
            return mask;
        });

    auto end = std::chrono::high_resolution_clock::now();
    double elapsedMs = (end - start).count() / 1000000.0;
    double numVoxelsMl = visits * (uint64_t)simd::VectorWidth / 1000000.0;
    printf("Brush: %.2fms  %.2fM voxels (%.2fM voxels/s)\n", elapsedMs, numVoxelsMl, numVoxelsMl * (1000.0 / elapsedMs));
    fflush(stdout);
}

static bool IsNearMaterial(VoxelMap& map, Voxel voxel, glm::ivec3 pos, int32_t radius) {
    for (int32_t dy = -radius; dy <= radius; dy++) {
        for (int32_t dz = -radius; dz <= radius; dz++) {
            for (int32_t dx = -radius; dx <= radius; dx++) {
                if (map.Get(pos + glm::ivec3(dx, dy, dz)).Data == voxel.Data) {
                    return true;
                }
            }
        }
    }
    return false;
}

void BrushSession::UpdatePosFromRay(VoxelMap& map, glm::dvec3 origin, glm::dvec3 dir) {
    HitResult hit = map.RayCast(origin, dir);
    double dist = std::max(hit.Distance, Pars.Radius + 5.0);

    if (Pars.Material.IsEmpty()) {
        // When erasing, prevent hit from going too far away and creating deep holes
        if (FrameNo != 0) {
            dist = hit.IsMiss() ? PrevHitDist : std::min(dist, PrevHitDist);
        }
    } else if (Pars.Action == BrushAction::Fill) {
        // When filling, prevent hit from getting too near camera
        if (FrameNo != 0 && IsNearMaterial(map, Pars.Material, hit.VoxelPos, 2)) {
            dist = std::max(dist, PrevHitDist);
        }
    }
    PrevHitDist = dist;
    UpdatePos(glm::floor(origin + dir * dist));
}