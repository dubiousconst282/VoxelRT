import "../BackendCommon";
import "../ParametricTraversal";
import Havk.Core;

namespace backend::distfield {

public static const int 
    GRID_SIZE_XZ = 1024, GRID_SIZE_Y = 1024,
    TILE_SIZE = 128,
    TILE_STRIDE = TILE_SIZE*TILE_SIZE*TILE_SIZE;

public struct VoxelMap<let Metric = 1> : IRayCastable, ITraversalStepper {
    public ImageHandle3D<uint> DistField;
    
    public static const uint2 GridSize = uint2(GRID_SIZE_XZ, GRID_SIZE_Y);

    public uint GetVoxelId(int3 pos) {
        return kDefaultOpaqueMaterialId;
    }
    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        return ParametricRayCast(this, worldOrigin, rayPos, rayDir, coarse);
    }

    public bool GetStepPos(inout int3 pos, float3 dir, bool coarse) {
        uint distToNearest = DistField.Instance[pos];

        if (distToNearest <= 0) {
            return false;
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