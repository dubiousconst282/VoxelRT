#pragma once

#include "VoxelMap.h"

namespace cvox {


struct HitInfo {
    VFloat Dist;
    VoxelPack Voxels;
    
    VMask Mask;
    VMask SideMaskX, SideMaskY;
    // SideMaskZ = ~SideMaskX & ~SideMaskY

    VFloat3 GetNormal(VFloat3 rayDir) const {
        VMask sideMaskZ = ~SideMaskX & ~SideMaskY;

        return {
            csel(SideMaskX, (rayDir.x & -0.0f) ^ VFloat(-1.0f), 0),  // rayDir.x < 0 ? +1 : -1
            csel(SideMaskY, (rayDir.y & -0.0f) ^ VFloat(-1.0f), 0),
            csel(sideMaskZ, (rayDir.z & -0.0f) ^ VFloat(-1.0f), 0),
        };
    }
    VFloat2 GetUV(VFloat3 hitPos) const {
        VMask sideMaskZ = ~SideMaskX & ~SideMaskY;

        return {
            fract(csel(SideMaskX, hitPos.y, hitPos.x)),
            fract(csel(sideMaskZ, hitPos.y, hitPos.z)),
        };
    }
};

inline VFloat GetSideDist(VFloat x, VFloat sign) {
    x = fract(x);
    return csel(sign < 0.0f, x, 1.0f - x);
}

// https://medium.com/@calebleak/raymarching-voxel-rendering-58018201d9d6
// Plane ray-marching is less accurate than DDA, but apparently easier to accelerate with distance fields.
HitInfo RayMarch(const VoxelMap& map, VFloat3 origin, VFloat3 dir, VMask activeMask) {
    VFloat3 delta = { abs(rcp14(dir.x)), abs(rcp14(dir.y)), abs(rcp14(dir.z)) };
    VFloat3 sideDist;

    VoxelPack voxels;

    VFloat dist = 0.0f;

    for (uint32_t i = 0; i < 128; i++) {
        VFloat3 pos = origin + dir * dist;

        VInt vx = floor2i(pos.x), vy = floor2i(pos.y), vz = floor2i(pos.z);

        activeMask &= _mm512_cmplt_epu32_mask(vx | vy | vz, _mm512_set1_epi32(VoxelMap::Size));
        VoxelPack currVoxels = map.GetPack(vx, vy, vz, activeMask);

        set_if(activeMask, voxels.Packed, currVoxels.Packed);
        activeMask &= voxels.IsEmpty();

        if (!any(activeMask)) break;

        set_if(activeMask, sideDist.x, GetSideDist(pos.x, dir.x) * delta.x);
        set_if(activeMask, sideDist.y, GetSideDist(pos.y, dir.y) * delta.y);
        set_if(activeMask, sideDist.z, GetSideDist(pos.z, dir.z) * delta.z);
        
        VFloat nearestSideDist = min(min(sideDist.x, sideDist.y), sideDist.z);
        VFloat stepDist = max(voxels.GetDistToNearest() - 1.5f, nearestSideDist + 1.0f / 4096);

        dist += csel(activeMask, stepDist, 0.0f);
    }

    return {
        .Dist = dist,
        .Voxels = voxels,
        .Mask = (VMask)(~activeMask),
        .SideMaskX = sideDist.x < min(sideDist.y, sideDist.z),
        .SideMaskY = sideDist.y < min(sideDist.x, sideDist.z),
    };
}

void GetPrimaryRay(VFloat2 uv, const glm::mat4& invProjMat, VFloat3& rayPos, VFloat3& rayDir) {
    VFloat4 nearPos = TransformVector(invProjMat, { uv.x, uv.y, 0, 1 });
    VFloat4 farPos = nearPos + VFloat4(invProjMat[2]);
    rayPos = VFloat3(nearPos) * (1.0f / nearPos.w);
    rayDir = VFloat3(farPos) * (1.0f / farPos.w);

    rayDir = normalize(rayDir);
}

};