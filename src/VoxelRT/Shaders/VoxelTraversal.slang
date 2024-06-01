import VoxelMap;

public float3 ClipRayToAABB(float3 origin, float3 dir, float3 bbMin, float3 bbMax) {
    float3 invDir = 1.0 / dir;
    float3 t1 = (bbMin - origin) * invDir;
    float3 t2 = (bbMax - origin) * invDir;
    float3 temp = t1;
    t1 = min(temp, t2);
    t2 = max(temp, t2);

    float tmin = max(t1.x, max(t1.y, t1.z));
    float tmax = min(t2.x, min(t2.y, t2.z));

    return tmin > 0 && tmin < tmax ? origin + dir * tmin : origin;
}

public struct HitInfo {
    public float Dist;
    public float3 Pos;
    public float3 Normal;
    public float2 FaceUV;
    public Material Material;
    public uint Iters;
};

public bool RayCast(VoxelMap map, int3 voxelOrigin, float3 origin, float3 dir, out HitInfo hit) {
    float3 invDir = 1.0 / dir;
    float3 tStart = (step(0.0, dir) - origin) * invDir;

    float3 startPos = ClipRayToAABB(origin, dir, -voxelOrigin + 1, float3(VoxelMap.GRID_SIZE_XZ, VoxelMap.GRID_SIZE_Y, VoxelMap.GRID_SIZE_XZ) - voxelOrigin - 1);
    int3 voxelPos = voxelOrigin + int3(floor(startPos));

    for (uint i = 0; i < 256; i++) {
        float3 sideDist = tStart + float3(voxelPos - voxelOrigin) * invDir;
        float tmin = min(min(sideDist.x, sideDist.y), sideDist.z);
        tmin = bit_cast<float>(bit_cast<uint>(tmin) + 5);
        float3 currPos = origin + tmin * dir;

        // Accurate DDA masks, helps in a few cases but hurts on others.
        //sideMask.x = sideDist.x < sideDist.y && sideDist.x < sideDist.z;
        //sideMask.y = !sideMask.x && sideDist.y < sideDist.z;
        //sideMask.z = !sideMask.x && !sideMask.y;

        // Accurate stepping, helps when using small bias but alone isn't enough to prevent ray from getting stuck.
        //voxelPos = voxelOrigin + ivec3(floor(mix(currPos, currPos + dir*0.01, sideMask)));
        voxelPos = voxelOrigin + int3(floor(currPos));

        if (!map.CheckBounds(voxelPos)) {
            hit.Iters = i;
            return false;
        }
        if (!GetStepPos(map, voxelPos, dir, false)) {
            hit.Material = map.GetVoxelMaterial(voxelPos);
            hit.Dist = tmin;
            hit.Pos = currPos;

            bool3 sideMask = float3(tmin) >= sideDist;
            hit.FaceUV = fract(select(sideMask.xz, currPos.yy, currPos.xz));
            hit.Normal = select(sideMask, -sign(dir), 0.0);
            hit.Iters = i;
            return true;
        }
    }
    hit = HitInfo();
    return false;
}

bool GetStepPos(VoxelMap map, inout int3 ipos, float3 dir, bool coarse) {
    uint3 pos = uint3(ipos);
    uint3 brickPos = pos / VoxelMap.BRICK_SIZE;
    uint3 sectorPos = pos / (VoxelMap.BRICK_SIZE * 4);
    uint64_t occMask = map.GetBrickMask(sectorPos);

    uint maskIdx = GetLinearIndex(brickPos, 4, 4);
    int scale = VoxelMap.BRICK_SIZE;

    if ((occMask >> maskIdx & 1) != 0) {
        occMask = map.GetVoxelMask(pos);
        maskIdx = GetLinearIndex(pos, 4, 4);
        scale = 1;

        if ((occMask >> maskIdx & 1) != 0) return false;
    } else if (occMask == 0) {
        occMask = map.GetSectorMask(sectorPos);
        maskIdx = GetLinearIndex(sectorPos, 4, 4);
        scale = VoxelMap.BRICK_SIZE * 4;
    }

    // if (u_UseAnisotropicLods) {
    //     // ipos = getAnisotropicStepPos(occMask, maskIdx, ipos, dir, scale);
    //     // return true;
    //     uint dirOctant = (dir.x < 0 ? 0 : 1) + (dir.y < 0 ? 0 : 2) + (dir.z < 0 ? 0 : 4);
    //     occMask &= RayCellInteractionMaskLUT[maskIdx + dirOctant * 64];
    // }

    int lod = GetIsotropicLod(occMask, maskIdx) * scale;

    if (coarse && lod < 4) {
        ipos = FindAnyOccupiedPos(ipos, occMask);
        return false;
    }
    ipos = AlignToCellBoundaries(ipos, dir, lod);
    return true;
}

int GetIsotropicLod(uint64_t mask, uint idx) {
    if (mask == 0) {
        return 4;
    }
    uint currMask = idx < 32 ? uint(mask) : uint(mask >> 32);

    if ((currMask >> (idx & 0xAu) & 0x00330033u) == 0) {
        return 2;
    }
    return 1;
}
int3 AlignToCellBoundaries(int3 pos, float3 dir, int cellSize) {
    int cellMask = cellSize - 1;
    pos.x = dir.x < 0 ? (pos.x & ~cellMask) : (pos.x | cellMask);
    pos.y = dir.y < 0 ? (pos.y & ~cellMask) : (pos.y | cellMask);
    pos.z = dir.z < 0 ? (pos.z & ~cellMask) : (pos.z | cellMask);
    return pos;
}
int3 FindAnyOccupiedPos(int3 pos, uint64_t mask) {
    uint currMask = uint(mask);
    uint idx = 0;

    if (mask.x == 0) {
        idx += 32;
        currMask = uint(mask >> 32);
    }
    idx += firstbitlow(currMask);

    int3 occPos = int3(idx) >> int3(0, 4, 2) & 3;
    return (pos & ~3) | occPos;
}

