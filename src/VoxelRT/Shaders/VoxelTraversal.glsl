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
    if (mask == uvec2(0)) {
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
ivec3 findAnyOccupiedPos(ivec3 pos, uvec2 mask) {
    uint currMask = mask.x;
    uint idx = 0;

    if (mask.x == 0) {
        idx += 32;
        currMask = mask.y;
    }
    idx += findLSB(currMask);

    ivec3 occPos = ivec3(idx) >> ivec3(0, 4, 2) & 3;
    return (pos & ~3) | occPos;
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

bool getStepPos(inout ivec3 ipos, vec3 dir, bool coarse) {
    uvec3 pos = uvec3(ipos);
    uvec3 brickPos = pos / BRICK_SIZE;
    uvec3 sectorPos = brickPos / 4;
    uvec2 occMask = b_VoxelData.AllocMasks[getLinearIndex(sectorPos, NUM_SECTORS_XZ, NUM_SECTORS_Y)];

    uint maskIdx = getLinearIndex(brickPos, 4, 4);
    int scale = BRICK_SIZE;

    if (isFineLod(occMask, maskIdx)) {
        uint slotIdx = getBrickDataSlot(brickPos);
        uint cellOffset = getLinearIndex(pos / 4, BRICK_SIZE / 4, BRICK_SIZE / 4);
        occMask = b_VoxelOccupancy.Data[slotIdx * OCC_STRIDE + cellOffset];
        
        maskIdx = getLinearIndex(pos, 4, 4);
        scale = 1;

        if (isFineLod(occMask, maskIdx)) return false;
    } else if (occMask == uvec2(0)) {
        occMask = b_VoxelData.SectorOccupancy[getLinearIndex(sectorPos / 4, NUM_SECTORS_XZ / 4, NUM_SECTORS_Y / 4)];
        maskIdx = getLinearIndex(sectorPos, 4, 4);
        scale = BRICK_SIZE * 4;
    }

    if (u_UseAnisotropicLods) {
        ipos = getAnisotropicStepPos(occMask, maskIdx, ipos, dir, scale);
        return true;
    }

    int lod = getIsotropicLod(occMask, maskIdx) * scale;

    if (coarse && lod < 4) {
        ipos = findAnyOccupiedPos(ipos, occMask);
        return false;
    }
    ipos = alignToCellBoundaries(ipos, dir, lod);
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

#if TRAVERSAL_METRICS
    #define TRAVERSAL_METRIC_ITER_ADD atomicAdd(b_Metrics.TraversalIters, uint(i))
#else
    #define TRAVERSAL_METRIC_ITER_ADD
#endif

struct HitInfo {
    float dist;
    vec3 pos;
    vec3 normal;
    vec2 uv;
    Material mat;
    uint iters;
};

bool rayTrace(vec3 origin, vec3 dir, out HitInfo hit) {
    vec3 invDir = 1.0 / dir;
    vec3 tStart = (step(0.0, dir) - origin) * invDir;

    vec3 startPos = clipRayToAABB(origin, dir, -u_WorldOrigin+1, vec3(GRID_SIZE_XZ, GRID_SIZE_Y,GRID_SIZE_XZ)-u_WorldOrigin-1);
    ivec3 voxelPos = u_WorldOrigin + ivec3(floor(startPos));

    for (uint i = 0; i < 256; i++) {
        vec3 sideDist = tStart + vec3(voxelPos - u_WorldOrigin) * invDir;
        float tmin = min(min(sideDist.x, sideDist.y), sideDist.z);
        tmin += tmin < 8 ? 0.00015 : tmin < 256 ? 0.001 : 0.1;
        vec3 currPos = origin + tmin * dir;

        voxelPos = u_WorldOrigin + ivec3(floor(currPos));
        
        if (!isInBounds(voxelPos)) {
            hit.iters = i;
            TRAVERSAL_METRIC_ITER_ADD;
            return false;
        }
        if (!getStepPos(voxelPos, dir, false)) {
            hit.mat = getVoxelMaterial(voxelPos);
            hit.dist = tmin;
            hit.pos = currPos;
            
            bvec3 sideMask = greaterThanEqual(vec3(tmin), sideDist);
            hit.uv = fract(mix(currPos.xz, currPos.yy, sideMask.xz));
            hit.normal = mix(vec3(0), -sign(dir), sideMask);
            hit.iters = i;
            
            TRAVERSAL_METRIC_ITER_ADD;
            return true;
        }
    }
    return false;
}

bool rayTraceCoarse(vec3 origin, vec3 dir, out HitInfo hit) {
    origin = clipRayToAABB(origin, dir, -u_WorldOrigin+1, vec3(GRID_SIZE_XZ, GRID_SIZE_Y,GRID_SIZE_XZ)-u_WorldOrigin-1);
    
    vec3 invDir = 1.0 / dir;
    vec3 tStart = (step(0.0, dir) - origin) * invDir;
    ivec3 voxelPos = u_WorldOrigin + ivec3(floor(origin));

    for (uint i = 0; i < 64; i++) {
        vec3 sideDist = tStart + (voxelPos - u_WorldOrigin) * invDir;
        float tmin = min(min(sideDist.x, sideDist.y), sideDist.z) + 0.001;
        vec3 currPos = origin + tmin * dir;
        
        bool coarse = distance(floor(currPos), origin) > 50;
        voxelPos = u_WorldOrigin + ivec3(floor(currPos));
        
        if (!isInBounds(voxelPos)) {
            hit.iters = i;

            TRAVERSAL_METRIC_ITER_ADD;
            return false;
        }

        if (!getStepPos(voxelPos, dir, coarse)) {
            // FIXME: normals / hit pos break after changing LOD
            bvec3 sideMask = greaterThanEqual(vec3(tmin), sideDist);
            hit.mat = getVoxelMaterial(voxelPos);
            hit.dist = tmin;
            hit.pos = currPos;
            hit.uv = fract(mix(currPos.xz, currPos.yy, sideMask.xz));
            hit.normal = mix(vec3(0), -sign(dir), sideMask);
            hit.iters = i;
            
            TRAVERSAL_METRIC_ITER_ADD;
            return true;
        }
    }
    return false;
}