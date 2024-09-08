// Based on:
// https://www.shadertoy.com/view/lfyGRW  "Multilevel 3D DDA" by DapperCore
// https://www.shadertoy.com/view/4dX3zl  "Branchless Voxel Raycasting" by fb39ca4 and others

import "../BackendCommon";
#include "../PerfCounters.h.slang"

namespace backend::flat_dda {

public static const int 
    GRID_SIZE_XZ = 4096, GRID_SIZE_Y = 1024,
    BRICK_SIZE = 8,
    BRICK_STRIDE = BRICK_SIZE*BRICK_SIZE*BRICK_SIZE;
    
public static const uint64_t
    NUM_VOXELS = uint64_t(GRID_SIZE_XZ) * GRID_SIZE_Y * GRID_SIZE_XZ,
    NUM_BRICKS = NUM_VOXELS / BRICK_STRIDE;

static const int MAX_RAY_STEPS = 512;

public struct MapStorage {
    public uint64_t BrickMasks[NUM_BRICKS / 64];
    public uint64_t VoxelMasks[NUM_VOXELS / 64];
}

public struct VoxelMap : IRayCastable {
    public MapStorage* Storage;

    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        HitInfo hit;
        hit.MaterialId = 0;

        rayPos += worldOrigin;
        float3 invDir = 1.0 / rayDir;

        float3 startPos = floor(ClipRayToAABB(rayPos, rayDir, 0.001, float3(GRID_SIZE_XZ, GRID_SIZE_Y, GRID_SIZE_XZ) + 0.001));
        float3 sideDist = ((startPos - rayPos) + step(0.0, rayDir)) * invDir;
        int3 mapPos = int3(startPos);
        int3 raySign = sign(rayDir);

        bool3 stepMask = GetStepMask(sideDist);
        int i;
        
        for (i = 0; i < MAX_RAY_STEPS; i++) {
            if (!CheckBounds(mapPos, GRID_SIZE_XZ, GRID_SIZE_Y)) break;

            uint brickIdx = GetBrickIndex(mapPos / BRICK_SIZE);
            uint voxelIdx = GetLinearIndex(mapPos, BRICK_SIZE, BRICK_SIZE);

            uint maskIdx = brickIdx * (BRICK_STRIDE / 32) + (voxelIdx / 32);
            uint occMask = ((uint*)&Storage->VoxelMasks[0])[maskIdx];

            if ((occMask >> (voxelIdx & 31) & 1) != 0) {
                hit.MaterialId = kDefaultOpaqueMaterialId;
                break;
            }
            stepMask = GetStepMask(sideDist);
            mapPos += select(stepMask, raySign, 0);
            sideDist += select(stepMask, abs(invDir), 0);
        }
        PERF_STAT_INC(TraversalIters, i);
        
        if (hit.MaterialId != 0) {
            float3 mini = ((mapPos-rayPos) + 0.5 - 0.5*float3(raySign)) * invDir;
            float d = max(mini.x, max(mini.y, mini.z));
            
            hit.Dist = d; // this should consider worldOrigin but we only use it for primary ray depth, so it's fine
            hit.Pos = rayPos + rayDir * d - worldOrigin;
            
            hit.FaceUV = fract(select(stepMask.xz, hit.Pos.yy, hit.Pos.xz));
            hit.Normal = select(stepMask, -sign(rayDir), 0.0);
        }
        return hit;
    }

    public static uint GetBrickIndex(uint3 brickPos) {
        #if 0
        uint sectorIdx = GetLinearIndex(brickPos / 4, GRID_SIZE_XZ / (BRICK_SIZE * 4), GRID_SIZE_Y / (BRICK_SIZE * 4));
        uint tileIdx = GetLinearIndex(brickPos, 4, 4);
        return sectorIdx * 64 + tileIdx;
        #endif
        return GetLinearIndex(brickPos, GRID_SIZE_XZ / BRICK_SIZE, GRID_SIZE_Y / BRICK_SIZE);
    }
}

public struct VoxelMap_MultiDDA : VoxelMap {
    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        HitInfo hit;
        hit.MaterialId = 0;

        rayPos += worldOrigin;
        rayPos /= BRICK_SIZE;

        float3 invDir = 1.0 / rayDir;

        float3 startPos = floor(ClipRayToAABB(rayPos, rayDir, 0.001, float3(GRID_SIZE_XZ, GRID_SIZE_Y, GRID_SIZE_XZ)/BRICK_SIZE - 0.001));
        float3 sideDist = ((startPos - rayPos) + step(0.0, rayDir)) * invDir;
        int3 mapPos = int3(startPos);
        int3 raySign = sign(rayDir);

        bool3 stepMask = GetStepMask(sideDist);
        int i = 0;
        
        for (i = 0; i < MAX_RAY_STEPS; i++) {
            if (!CheckBounds(mapPos, GRID_SIZE_XZ / BRICK_SIZE, GRID_SIZE_Y / BRICK_SIZE)) break;

            uint brickIdx = GetBrickIndex(mapPos);
            uint occMask = ((uint*)&Storage->BrickMasks[0])[brickIdx / 32];

            //int loopActiveLanes = WaveActiveCountBits(true);

            if ((occMask >> (brickIdx & 31) & 1) != 0) {
                // Wait until we have enough lanes to traverse brick
                //if (WaveActiveCountBits(true) < loopActiveLanes * 6/8) continue;

                float3 mini = ((mapPos-rayPos) + 0.5 - 0.5*float3(raySign)) * invDir;
                float d = max(mini.x, max(mini.y, mini.z));
                float3 intersect = rayPos + rayDir*d;
                float3 uv3d = intersect - mapPos;

                if (all(mapPos == floor(rayPos))) // Handle edge case where camera origin is inside of block
                    uv3d = rayPos - mapPos;
                    
                uint* brickData = (uint*)&Storage->VoxelMasks[brickIdx * (BRICK_STRIDE / 64)];
                int3 subp = TraceBrick(uv3d*8.0, rayDir, stepMask, brickData);

                if (subp.x >= 0) {
                    hit.MaterialId = kDefaultOpaqueMaterialId;

                    // TODO: this would ideally be outside the loop to avoid divergence, Mesa/NIR doesn't optimize but other compilers might
                    float3 finalPos = mapPos*8+subp;
                    float3 startPos = rayPos*8;

                    float3 mini = ((finalPos-startPos) + 0.5 - 0.5*float3(raySign)) * invDir;
                    float d = max(mini.x, max(mini.y, mini.z));
                    float3 intersect = startPos + rayDir*d;

                    hit.Dist = d; // this should consider worldOrigin but we only use it for primary ray depth, so it's fine
                    hit.Pos = intersect - worldOrigin;
                    
                    break;
                }
            }
            stepMask = GetStepMask(sideDist);
            mapPos += select(stepMask, raySign, 0);
            sideDist += select(stepMask, abs(invDir), 0);
        }
        PERF_STAT_INC(TraversalIters, i);
        #if PERF_TRACK_LEVEL >= PERF_LEVEL_HIGH
            InterlockedAdd(g_PerfTracker->RayCastItersHistogram[min(i, 32)], 1);
        #endif

        if (hit.MaterialId != 0) {
            hit.FaceUV = fract(select(stepMask.xz, hit.Pos.yy, hit.Pos.xz));
            hit.Normal = select(stepMask, -sign(rayDir), 0.0);
        }
        return hit;
    }

    static int3 TraceBrick(float3 rayPos, float3 rayDir, inout bool3 stepMask, uint* brickData) {
        rayPos = clamp(rayPos, float3(0.0001), float3(7.9999));

        float3 invDir = 1.0 / rayDir;
        float3 sideDist = ((floor(rayPos) - rayPos) + step(0.0, rayDir)) * invDir;
        int3 mapPos = int3(floor(rayPos));
        int3 raySign = sign(rayDir);
        
        do {
            #if PERF_PRECISE_ITER_HEATMAP
            g_CurrTraversalIters++;
            #endif
            uint index = (mapPos.x + mapPos.z * 8 + mapPos.y * 64);

            if ((brickData[index / 32u] >> (index & 31) & 1) != 0) {
                return mapPos;
            }
            stepMask = GetStepMask(sideDist);
            mapPos += select(stepMask, raySign, 0);
            sideDist += select(stepMask, abs(invDir), 0);
        } while (all(uint3(mapPos) < 8u));

        return -1;
    }
}

static bool3 GetStepMask(float3 sideDist) {
    bool3 mask;

    mask.x = sideDist.x < sideDist.y && sideDist.x < sideDist.z;
    mask.y = !mask.x && sideDist.y < sideDist.z;
    mask.z = !mask.x && !mask.y;

    return mask;
}

import "../ParametricTraversal";

public struct VoxelMap_ParamBrickStepping : VoxelMap, ITraversalStepper {
    public static const uint2 GridSize = uint2(GRID_SIZE_XZ, GRID_SIZE_Y);

    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        return ParametricRayCast(this, worldOrigin, rayPos, rayDir, coarse);
    }
    public bool GetStepPos(inout int3 pos, float3 dir, bool coarse) {
        
        uint brickIdx = GetBrickIndex(pos / BRICK_SIZE);
        uint occMask = ((uint*)&Storage->BrickMasks[0])[brickIdx / 32];

        if ((occMask >> (brickIdx & 31) & 1) != 0) {
            uint* brickData = (uint*)&Storage->VoxelMasks[brickIdx * (BRICK_STRIDE / 64)];
            uint index = GetLinearIndex(pos, BRICK_SIZE, BRICK_SIZE);
            return (brickData[index / 32u] >> (index & 31) & 1) == 0;
        }

        pos = AlignToCellBoundaries(pos, dir, BRICK_SIZE);
        return true;
    }
    public uint GetVoxelId(int3 pos) {
        return kDefaultOpaqueMaterialId;
    }
}

};