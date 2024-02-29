#include <ranges>
#include <execution>

#include <SwRast/SIMD.h>

#include "VoxelMap.h"

using namespace swr::simd;

struct MaterialPack {
    VInt Data;

    VFloat3 GetColor() const {
        return {
            conv2f((Data >> 11) & 31) * (1.0f / 31),
            conv2f((Data >> 5) & 63) * (1.0f / 63),
            conv2f((Data >> 0) & 31) * (1.0f / 31),
        };
    }
    VFloat GetEmissionStrength() const { return conv2f((Data >> 16) & 15) * (7.0f / 15); }
};

struct HitInfo {
    VFloat Dist;

    VMask Mask;
    VMask SideMaskX, SideMaskY, SideMaskZ;

    VFloat3 GetNormal(VFloat3 rayDir) const {
        return {
            csel(SideMaskX, (rayDir.x & -0.0f) ^ VFloat(-1.0f), 0),  // rayDir.x < 0 ? +1 : -1
            csel(SideMaskY, (rayDir.y & -0.0f) ^ VFloat(-1.0f), 0),
            csel(SideMaskZ, (rayDir.z & -0.0f) ^ VFloat(-1.0f), 0),
        };
    }
    VFloat2 GetUV(VFloat3 hitPos) const {
        return {
            fract(csel(SideMaskX, hitPos.y, hitPos.x)),
            fract(csel(SideMaskZ, hitPos.y, hitPos.z)),
        };
    }
};

// Creates mask for voxel coords that are inside the brick map.
static VMask GetInboundMask(VInt x, VInt y, VInt z) {
    const uint32_t SizeXZ = BrickRenderMap::SegmentIndexer::SizeXZ * 2 * Brick::VoxelIndexer::SizeXZ;
    const uint32_t SizeY = BrickRenderMap::SegmentIndexer::SizeY * 2 * Brick::VoxelIndexer::SizeY;
    return _mm512_cmplt_epu32_mask(x | z, _mm512_set1_epi32(SizeXZ)) & _mm512_cmplt_epu32_mask(y, _mm512_set1_epi32(SizeY));
}

static VInt GetVoxels(const BrickRenderMap& map, VInt x, VInt y, VInt z, VMask mask) {
    VInt segIdx = csel(mask, BrickRenderMap::SegmentIndexer::GetIndex(x, y, z), 0);
    VInt extraInfo = _mm512_mask_i32gather_epi32(_mm512_undefined_epi32(), mask, segIdx, (uint8_t*)map.Segments + 4, 8);
    VInt baseSlot = _mm512_mask_i32gather_epi32(_mm512_undefined_epi32(), mask, segIdx, map.Segments, 8);

    VInt brickIdx = BrickRenderMap::SegmentBrickIndexer::GetIndex(x, y, z);
    baseSlot += _mm512_popcnt_epi32(extraInfo & ((1 << brickIdx) - 1));

    VInt storageOffset = baseSlot * sizeof(Brick) + Brick::VoxelIndexer::GetIndex(x, y, z);

    VInt voxelIds = _mm512_mask_i32gather_epi32(_mm512_set1_epi32(0), mask, storageOffset, BrickStorage.data(), 1);
    return voxelIds & 0xFF;
}
static MaterialPack GetMaterial(const VoxelMap& map, VInt voxelIds, VMask mask) {
    VInt mat = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), mask, voxelIds, map.Palette, 4);
    return { .Data = mat };
}

static VFloat GetSideDist(VFloat x, VFloat sign) {
    x = fract(x);
    return csel(sign < 0.0f, x, 1.0f - x);
}

static HitInfo RayMarch(const VoxelMap& map, VFloat3 origin, VFloat3 dir, VMask activeMask, bool isPrimaryRay) {
    VFloat3 delta = { abs(rcp14(dir.x)), abs(rcp14(dir.y)), abs(rcp14(dir.z)) };
    VFloat3 sideDist;

    VoxelPack voxels;

    VFloat dist = 0.0f;
    VMask inboundMask = (VMask)~0u;

    for (uint32_t i = 0; i < (isPrimaryRay ? 128 : 20); i++) {
        VFloat3 pos = origin + dir * dist;

        VInt vx = floor2i(pos.x), vy = floor2i(pos.y), vz = floor2i(pos.z);

        inboundMask = GetInboundMask(vx, vy, vz);
        activeMask &= inboundMask;
        VoxelPack currVoxels = map.GetPack(vx, vy, vz, activeMask);

        set_if(activeMask, voxels.Data, currVoxels.Data);
        activeMask &= voxels.IsEmpty();

        if (!any(activeMask)) break;

        set_if(activeMask, sideDist.x, GetSideDist(pos.x, dir.x) * delta.x);
        set_if(activeMask, sideDist.y, GetSideDist(pos.y, dir.y) * delta.y);
        set_if(activeMask, sideDist.z, GetSideDist(pos.z, dir.z) * delta.z);

        VFloat stepDist = min(min(sideDist.x, sideDist.y), sideDist.z) + 1.0f / 4096;
        dist += csel(activeMask, stepDist, 0.0f);
    }

    VMask sideMaskX = sideDist.x < min(sideDist.y, sideDist.z);
    VMask sideMaskY = sideDist.x < min(sideDist.y, sideDist.z);

    return {
        .Dist = dist,
        .Mask = (VMask)(~activeMask & inboundMask),
        .SideMaskX = sideMaskX,
        .SideMaskY = sideMaskY,
        .SizeMaskZ = (VMask)(~sideMaskX & ~sideMaskY),
    };
}

static void GetPrimaryRay(VFloat2 uv, const glm::mat4& invProjMat, VFloat3& rayPos, VFloat3& rayDir) {
    VFloat4 nearPos = TransformVector(invProjMat, { uv.x, uv.y, 0, 1 });
    VFloat4 farPos = nearPos + VFloat4(invProjMat[2]);
    rayPos = VFloat3(nearPos) * (1.0f / nearPos.w);
    rayDir = VFloat3(farPos) * (1.0f / farPos.w);

    rayDir = normalize(rayDir);
}