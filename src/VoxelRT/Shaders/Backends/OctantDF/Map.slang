import "../BackendCommon";
import "../ParametricTraversal";
import Havk.Core;

#include "../PerfCounters.h.slang"

namespace backend::octdf {

public static const int
    GRID_SIZE_XZ = 4096, GRID_SIZE_Y = 1024,
    TILE_SIZE = 256, FIELD_RES = 4,
    FIELD_SIZE = TILE_SIZE / FIELD_RES;

public struct FieldTile {
    public uint8_t Dists[8][FIELD_SIZE * FIELD_SIZE * FIELD_SIZE];
    public uint64_t OccMasks[(TILE_SIZE * TILE_SIZE * TILE_SIZE) / 64];
}
public struct VoxelMap : IRayCastable, ITraversalStepper {
    public FieldTile** Tiles;

    public static const uint2 GridSize = uint2(GRID_SIZE_XZ, GRID_SIZE_Y);

    public uint GetVoxelId(int3 pos) {
        return kDefaultOpaqueMaterialId;
    }
    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        return ParametricRayCast(this, worldOrigin, rayPos, rayDir, coarse);
    }

    public bool GetStepPos(inout int3 pos, float3 dir, bool coarse) {
        int distToNearest = 1024;
        FieldTile* tile = Tiles[GetTileIndex(pos)];

        if (tile != nullptr) {
            uint oct = (dir.x < 0 ? 0 : 1) + (dir.y < 0 ? 0 : 2) + (dir.z < 0 ? 0 : 4);
            distToNearest = int(tile.Dists[oct][GetSampleIndex(uint3(pos) / FIELD_RES)])*4-3;

            if (distToNearest <= -3) {
                uint64_t mask = tile.OccMasks[GetMaskIndex(pos)];
                
                if (mask != 0) {
                    int maskIdx = GetLinearIndex(pos, 4, 4);
                    return (mask >> maskIdx & 1) == 0;
                }
                distToNearest = 1024; // not sure why we sometimes get zero distances at random places
            }
        }

        float mag = 1.0 / (abs(dir.x) + abs(dir.y) + abs(dir.z)); // manhattan space normalization factor
        int3 stepSize = int3(ceil(distToNearest * mag * dir - step(0.0, dir)));

        // Advance ray, clipping to tile boundaries
        int3 stepPos = pos + stepSize;
        bool crossesTile = any(uint3(pos ^ stepPos) >= uint(TILE_SIZE));
        pos = crossesTile ? AlignToCellBoundaries(pos, dir, TILE_SIZE) : stepPos;
        
        return true;
    }
}

public static int GetTileIndex(uint3 pos) {
    return GetLinearIndex(pos / TILE_SIZE, GRID_SIZE_XZ / TILE_SIZE, GRID_SIZE_Y / TILE_SIZE);
}
public static uint GetSampleIndex(uint3 pos) {
    return GetLinearIndex(pos, FIELD_SIZE, FIELD_SIZE);
}
public static uint GetMaskIndex(uint3 pos) {
    return GetLinearIndex(pos / 4, TILE_SIZE / 4, TILE_SIZE / 4);
}


};