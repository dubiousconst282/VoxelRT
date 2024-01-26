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

VInt GetMipCellSize(const OccupancyMap& occ, VInt x, VInt y, VInt z, VMask mask) {
    VInt opaqueLevel = 0;

    for (uint32_t k = 0; k < 5 && mask; k++) {
        // ivec3 mipPos = pos >> k;
        // uint column = texelFetch(u_OccupancyStorage, ivec3(mipPos.xy, mipPos.z >> 5), k).r;
        // uint opaque = (column >> (mipPos.z & 31u)) & 1u;
        // if (opaque != 0) break;
        x = x >> 1, y = y >> 1, z = z >> 1;
        uint32_t s = occ.CubeSizeLog2 - k;
        VInt offset = x + (y << s) + ((z >> 5) << (s * 2));
        VInt column = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), mask, offset, occ.Data.get() + occ.MipOffsets[k], 4);
        mask &= (column >> (z & 31) & 1) == 0;
        opaqueLevel = csel(mask, (int32_t)k, opaqueLevel);
    }
    return 1 << opaqueLevel;
}

// https://medium.com/@calebleak/raymarching-voxel-rendering-58018201d9d6
// Plane ray-marching is less accurate than DDA, but apparently easier to accelerate with distance fields.
HitInfo RayMarch(const VoxelMap& map, VFloat3 origin, VFloat3 dir, VMask activeMask, bool isPrimaryRay) {
    VFloat3 delta = { abs(rcp14(dir.x)), abs(rcp14(dir.y)), abs(rcp14(dir.z)) };
    VFloat3 sideDist;

    VoxelPack voxels;

    VFloat dist = 0.0f;
    VMask inboundMask = (VMask)~0u;

    for (uint32_t i = 0; i < (isPrimaryRay?128:20); i++) {
        VFloat3 pos = origin + dir * dist;

        VInt vx = floor2i(pos.x), vy = floor2i(pos.y), vz = floor2i(pos.z);

        inboundMask = VoxelMap::GetInboundMask(vx, vy, vz);
        activeMask &= inboundMask;
        VoxelPack currVoxels = map.GetPack(vx, vy, vz, activeMask);

        set_if(activeMask, voxels.Data, currVoxels.Data);
        activeMask &= voxels.IsEmpty();

        if (!any(activeMask)) break;

        VFloat cellSize = conv2f(GetMipCellSize(map.OccMap, vx, vy, vz, activeMask));
        if (!isPrimaryRay) cellSize *= 2;
        VFloat invCellSize = 1.0f / cellSize;

        set_if(activeMask, sideDist.x, GetSideDist(pos.x * invCellSize, dir.x) * (delta.x * cellSize));
        set_if(activeMask, sideDist.y, GetSideDist(pos.y * invCellSize, dir.y) * (delta.y * cellSize));
        set_if(activeMask, sideDist.z, GetSideDist(pos.z * invCellSize, dir.z) * (delta.z * cellSize));

        VFloat stepDist = min(min(sideDist.x, sideDist.y), sideDist.z) + 1.0f / 4096;
        dist += csel(activeMask, stepDist, 0.0f);
    }

    return {
        .Dist = dist,
        .Voxels = voxels,
        .Mask = (VMask)(~activeMask & inboundMask),
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