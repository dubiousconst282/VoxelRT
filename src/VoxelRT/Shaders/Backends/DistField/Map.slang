import "../BackendCommon";
import "../SparseRayMarch";
import Havk.Core;

namespace backend::distfield {

public static const int
    GRID_SIZE_XZ = 4096, GRID_SIZE_Y = 1024,
    TILE_SIZE = 256;

public struct VoxelMap<let Metric = 1> : IRayCastable, ITraversalStepper {
    public ImageHandle3D<uint> DistField;
    public uint64_t* OccupancyMap;
    
    public static const uint2 GridSize = uint2(GRID_SIZE_XZ, GRID_SIZE_Y);

    public uint GetVoxelId(int3 pos) {
        return kDefaultOpaqueMaterialId;
    }
    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        return SparseRayMarch(this, worldOrigin, rayPos, rayDir, coarse);
    }

    public bool GetStepPos(inout int3 pos, float3 dir, bool coarse) {
        int distToNearest = DistField.Instance[uint3(pos) / 4] * 4 - 3;

        if (distToNearest <= 0) {
            uint64_t mask = OccupancyMap[GetLinearIndex(uint3(pos) / 4, GRID_SIZE_XZ / 4, GRID_SIZE_Y / 4)];
            int maskIdx = GetLinearIndex(pos, 4, 4);
            
            if ((mask >> (maskIdx & 0b101010) & 0x00330033) == 0) {
                //pos = AlignToCellBoundaries(pos, dir, 2);
            }
            return (mask >> maskIdx & 1) == 0;
        }

        int3 stepSize;

        if (Metric == 1) {
            // Manhattan
            float mag = 1.0 / (abs(dir.x) + abs(dir.y) + abs(dir.z)); // manhattan space normalization factor
            stepSize = int3(ceil(distToNearest * mag * dir - step(0.0, dir)));
        } else {
            // Euclidean
            stepSize = int3(sqrt(distToNearest) * dir);
        }

        // Advance ray, clipping to tile boundaries
        int3 stepPos = pos + stepSize;
        bool crossesTile = any(uint3(pos ^ stepPos) >= uint(TILE_SIZE));
        pos = crossesTile ? AlignToCellBoundaries(pos, dir, TILE_SIZE) : stepPos;
        
        return true;
    }
}

};