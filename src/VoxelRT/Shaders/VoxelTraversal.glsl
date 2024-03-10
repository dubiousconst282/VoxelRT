#include "VoxelMap.glsl"

buffer ssbo_Metrics {
    uint TraversalIters;
} b_Metrics;

uniform bool u_UseAnisotropicLods;

uint shr64(uvec2 mask, uint count) {
    uint currMask = count < 32 ? mask.x : mask.y;
    return currMask >> (count & 31u);
}
bool isFineLod(uvec2 mask, uint idx) {
    return (shr64(mask, idx) & 1u) != 0;
}

int getIsotropicLod(uvec2 mask, uint idx) {
    if ((mask.x | mask.y) == 0) {
        return 4;
    }
    uint currMask = idx < 32 ? mask.x : mask.y;

    if ((currMask >> (idx & 0xAu) & 0x00330033u) == 0) {
        return 2;
    }
    return 1;
}
ivec3 alignToCellBoundaries(ivec3 pos, vec3 dir, int cellSize) {
    int cellMask = cellSize - 1;
    pos.x = dir.x < 0 ? (pos.x & ~cellMask) : (pos.x | cellMask);
    pos.y = dir.y < 0 ? (pos.y & ~cellMask) : (pos.y | cellMask);
    pos.z = dir.z < 0 ? (pos.z & ~cellMask) : (pos.z | cellMask);
    return pos;
}

ivec3 getAnisotropicStepPos(uvec2 mask, uint idx, ivec3 pos, vec3 dir, int scale) {
    if ((mask.x | mask.y) == 0) {
        return alignToCellBoundaries(pos, dir, 4 * scale);
    }
    uint currMask = idx < 32 ? mask.x : mask.y;

    // X
    uint maskX = currMask >> (idx & ~3u);
    int x = int(idx >> 0 & 3u), dx = dir.x < 0 ? -1 : +1;
    int sx = dx*3;
    if (uint(x + dx*3) >= 4 || (maskX >> (x + dx*3) & 1u) != 0) sx = 2*dx;
    if (uint(x + dx*2) >= 4 || (maskX >> (x + dx*2) & 1u) != 0) sx = 1*dx;
    if (uint(x + dx*1) >= 4 || (maskX >> (x + dx*1) & 1u) != 0) sx = 0*dx;

    // Z
    uint filterZ = (2u << max(x, x+sx)) - (1u << min(x, x+sx)); // mask where X row range must be all 0s

    uint maskZ = currMask >> (idx & ~15u);
    int z = int(idx >> 2 & 3u), dz = dir.z < 0 ? -1 : +1;
    int sz = dz*3;
    if (uint(z + dz*3) >= 4 || (maskZ >> ((z + dz*3)*4) & filterZ) != 0) sz = 2*dz;
    if (uint(z + dz*2) >= 4 || (maskZ >> ((z + dz*2)*4) & filterZ) != 0) sz = 1*dz;
    if (uint(z + dz*1) >= 4 || (maskZ >> ((z + dz*1)*4) & filterZ) != 0) sz = 0*dz;

    // Y
    uint filterY = filterZ * 0x1111u;
    filterY &= (0xFFFFu << min(z,z+sz)*4) & (0xFFFFu >> (3-max(z,z+sz))*4);

    int y = int(idx >> 4 & 3u), dy = dir.y < 0 ? -1 : +1;
    int sy = dy*3;
    if (uint(y + dy*3) >= 4 || (shr64(mask, (y + dy*3)*16) & filterY) != 0) sy = 2*dy;
    if (uint(y + dy*2) >= 4 || (shr64(mask, (y + dy*2)*16) & filterY) != 0) sy = 1*dy;
    if (uint(y + dy*1) >= 4 || (shr64(mask, (y + dy*1)*16) & filterY) != 0) sy = 0*dy;

    if (scale > 1) pos = alignToCellBoundaries(pos, dir, scale);
    return pos + ivec3(sx,sy,sz)*scale;
}

bool getStepPos(inout ivec3 ipos, vec3 dir) {
    uvec3 pos = uvec3(ipos);
    uvec3 brickPos = pos / BRICK_SIZE;
    uint brickIdx = getLinearIndex(brickPos, 4, 4);
    Sector sector = b_VoxelData.Sectors[getLinearIndex(brickPos / 4, NUM_SECTORS_XZ, NUM_SECTORS_Y)];

    uvec2 occMask = uvec2(sector.AllocMask_0, sector.AllocMask_32);
    uint maskIdx = brickIdx;
    int scale = BRICK_SIZE;

    if (isFineLod(occMask, maskIdx)) {
        uint slotIdx = getBrickDataSlot(brickPos);
        uint cellOffset = getLinearIndex(pos / uvec3(4, 4, 4), BRICK_SIZE / 4, BRICK_SIZE / 4) * 2;
        occMask.x = b_Occupancy.Data[slotIdx * OCC_STRIDE + cellOffset + 0];
        occMask.y = b_Occupancy.Data[slotIdx * OCC_STRIDE + cellOffset + 1];
        
        maskIdx = getLinearIndex(pos, 4, 4);
        scale = 1;

        if (isFineLod(occMask, maskIdx)) return false;
    }

    if (u_UseAnisotropicLods) {
        ipos = getAnisotropicStepPos(occMask, maskIdx, ipos, dir, scale);
    } else {
        int lod = getIsotropicLod(occMask, maskIdx);
        ipos = alignToCellBoundaries(ipos, dir, lod * scale);
    }
    return true;
}

vec3 clipRayToAABB(vec3 origin, vec3 dir, vec3 bbMin, vec3 bbMax) {
    vec3 invDir = 1.0 / dir;
    vec3 t1 = (bbMin - origin) * invDir;
    vec3 t2 = (bbMax - origin) * invDir;
    vec3 temp = t1;
    t1 = min(temp, t2);
    t2 = max(temp, t2);

    float tmin = max(t1.x, max(t1.y, t1.z));
    float tmax = min(t2.x, min(t2.y, t2.z));

    return tmin > 0 && tmin < tmax ? origin + dir * tmin : origin;
}

struct HitInfo {
    float dist;
    vec3 pos;
    vec3 norm;
    vec2 uv;
    Material mat;
    uint iters;
};
#define TRAVERSAL_METRICS 1
bool rayCast(vec3 origin, vec3 dir, out HitInfo hit) {
    vec3 invDir = 1.0 / dir;
    vec3 tStart = (max(sign(dir), 0.0) - origin) * invDir;
    vec3 currPos = origin;
    vec3 sideDist = vec3(0);
    float tmin = 0;
    
    currPos = clipRayToAABB(currPos, dir, -u_WorldOrigin+1, vec3(GRID_SIZE_XZ, GRID_SIZE_Y,GRID_SIZE_XZ)-u_WorldOrigin-1);

    for (uint i = 0; i < 256; i++) {
        ivec3 pos = u_WorldOrigin + ivec3(floor(currPos));

        if (!isInBounds(pos)) {
            #if TRAVERSAL_METRICS
            atomicAdd(b_Metrics.TraversalIters, uint(i));
            #endif

            hit.iters = i;
            return false;
        }

        if (!getStepPos(pos, dir)) {
            #if TRAVERSAL_METRICS
            atomicAdd(b_Metrics.TraversalIters, uint(i));
            #endif

            uint voxelId = getVoxel(pos);
            hit.mat = b_VoxelData.Palette[voxelId];

            bvec3 sideMask = lessThanEqual(sideDist.xyz, min(sideDist.yzx, sideDist.zxy));
            hit.dist = tmin;
            hit.pos = currPos;
            hit.uv = fract(mix(currPos.xz, currPos.yy, sideMask.xz));
            hit.norm = mix(vec3(0), -sign(dir), sideMask);
            hit.iters = i;
            
            return true;
        }

        sideDist = tStart + (pos - u_WorldOrigin) * invDir;
        tmin = min(min(sideDist.x, sideDist.y), sideDist.z);

        // Bias tmin by the smallest amount that is representable by a float (BitIncrement),
        // to avoid ray from getting stuck. Constant found by trial and error.
        tmin = uintBitsToFloat(floatBitsToUint(tmin) + 4);

        currPos = origin + tmin * dir;
    }
    return false;
}