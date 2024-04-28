#include "Brush.h"

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

    // TODO: skip empty sectors/bricks (sample DF at center to check)
    map.RegionDispatchSIMD(minPos, maxPos, !isErasing, [&](VoxelDispatchInvocationPars& invoc) {
        VFloat3 pos = VFloat3(simd::conv2f(invoc.X), simd::conv2f(invoc.Y), simd::conv2f(invoc.Z)) + 0.5f;
        VMask mask = sdCapsule(pos, Pars.PointA, Pars.PointB, Pars.Radius) < 0.0;

        if (Pars.Probability < 1.0f) {
            mask &= rng.NextUnsignedFloat() < Pars.Probability;
        }
        if (Pars.Action == BrushAction::Replace) {
            mask &= invoc.VoxelIds != 0;
        }
        invoc.VoxelIds.set_if(mask, Pars.Material.Data);

        return simd::any(mask);
    });
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